<#
.SYNOPSIS
    Authenticode-sign Windows build artifacts with a PFX code-signing
    certificate, or read the certificate's publisher subject.

.DESCRIPTION
    This is the PFX branch of the `./.github/actions/sign-windows` composite
    action (the other branch is Azure Trusted Signing). It is used for a
    self-signed certificate in local / pipeline testing, or a legacy file-based
    certificate. Public releases should prefer Trusted Signing — see
    docs/WINDOWS-CODE-SIGNING.md.

    The certificate is supplied as a base64-encoded PFX plus its password,
    through environment variables that map to GitHub Actions secrets:

        WINDOWS_CODESIGN_PFX_BASE64      base64 of the .pfx / .p12 file
        WINDOWS_CODESIGN_PFX_PASSWORD    the PFX export password (may be empty)
        WINDOWS_CODESIGN_TIMESTAMP_URL   RFC-3161 timestamp URL (optional;
                                         defaults to DigiCert's)

    When WINDOWS_CODESIGN_PFX_BASE64 is empty the -Path mode is a no-op that
    warns and returns a marker object with exit 0. -ShowPublisher requires the
    secret and throws without it.

    The decoded PFX is written to a fresh temp directory and shredded in a
    finally block; it never lands in the workspace.

.PARAMETER Path
    One or more files (globs allowed) to sign. Signing mode.

.PARAMETER ShowPublisher
    Print the certificate Subject distinguished name to stdout and return.
    The sideload MSIX build needs it for Identity/@Publisher.

.PARAMETER DryRun
    Resolve inputs and emit the signtool command that would run (password
    redacted) without invoking signtool. Returns a summary object.
#>
[CmdletBinding(DefaultParameterSetName = 'Sign')]
param(
    [Parameter(ParameterSetName = 'Sign', Mandatory = $true, Position = 0)]
    [string[]]$Path,

    [Parameter(ParameterSetName = 'ShowPublisher', Mandatory = $true)]
    [switch]$ShowPublisher,

    [Parameter(ParameterSetName = 'Sign')]
    [switch]$DryRun,

    [string]$PfxBase64 = $env:WINDOWS_CODESIGN_PFX_BASE64,
    [string]$PfxPassword = $env:WINDOWS_CODESIGN_PFX_PASSWORD,
    [string]$TimestampUrl = $(if ($env:WINDOWS_CODESIGN_TIMESTAMP_URL) { $env:WINDOWS_CODESIGN_TIMESTAMP_URL } else { 'http://timestamp.digicert.com' })
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Find-SignTool {
    $cmd = Get-Command 'signtool.exe' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $binRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path -LiteralPath $binRoot) {
        $found = Get-ChildItem -LiteralPath $binRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
            Where-Object { Test-Path -LiteralPath $_ } |
            Select-Object -First 1
        if ($found) { return $found }
    }
    throw 'signtool.exe not found. Install the Windows SDK or add it to PATH.'
}

if ([string]::IsNullOrWhiteSpace($PfxBase64)) {
    if ($ShowPublisher) {
        throw 'WINDOWS_CODESIGN_PFX_BASE64 is required for -ShowPublisher.'
    }
    Write-Warning 'WINDOWS_CODESIGN_PFX_BASE64 is not set - skipping Authenticode signing. Artifacts will be unsigned.'
    return [pscustomobject]@{ Skipped = $true; Reason = 'no-certificate'; Signed = @() }
}

$workDir = Join-Path ([IO.Path]::GetTempPath()) ('aether-codesign-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $workDir | Out-Null
$pfxPath = Join-Path $workDir 'codesign.pfx'

try {
    [IO.File]::WriteAllBytes($pfxPath, [Convert]::FromBase64String($PfxBase64.Trim()))

    if ($ShowPublisher) {
        $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
            $pfxPath, $PfxPassword,
            [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
        Write-Output $cert.Subject
        return
    }

    $files = @()
    foreach ($pattern in $Path) {
        $resolved = @(Resolve-Path -Path $pattern -ErrorAction SilentlyContinue | ForEach-Object { $_.Path })
        if (-not $resolved) { throw "No file matched '$pattern'." }
        $files += $resolved
    }
    $files = @($files | Sort-Object -Unique)

    $signArgs = @('sign', '/fd', 'SHA256', '/f', $pfxPath)
    if (-not [string]::IsNullOrWhiteSpace($PfxPassword)) { $signArgs += @('/p', $PfxPassword) }
    if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) { $signArgs += @('/tr', $TimestampUrl, '/td', 'SHA256') }
    $signArgs += $files

    $redacted = ($signArgs | ForEach-Object {
        if ($PfxPassword -and $_ -eq $PfxPassword) { '***' } else { $_ }
    }) -join ' '
    Write-Host "signtool $redacted"

    if ($DryRun) {
        return [pscustomobject]@{
            Skipped         = $false
            DryRun          = $true
            SignArgs        = $signArgs
            RedactedCommand = "signtool $redacted"
            Files           = $files
            TimestampUrl    = $TimestampUrl
        }
    }

    $signtool = Find-SignTool
    & $signtool @signArgs
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed (exit $LASTEXITCODE)." }

    foreach ($file in $files) {
        & $signtool verify /pa /v $file
        if ($LASTEXITCODE -ne 0) { throw "signtool verify failed for $file (exit $LASTEXITCODE)." }
    }

    return [pscustomobject]@{ Skipped = $false; DryRun = $false; Signed = $files }
}
finally {
    Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue
}
