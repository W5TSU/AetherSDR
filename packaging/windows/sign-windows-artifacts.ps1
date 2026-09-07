<#
.SYNOPSIS
    Authenticode-sign Windows build artifacts with the project's code-signing
    certificate — or read the certificate's publisher subject / export it to a
    file for another tool to consume.

.DESCRIPTION
    windows-installer.yml calls this after the deploy payload is assembled and
    again after the Inno installer is built, so the downloaded AetherSDR.exe
    and the -setup.exe carry a trusted signature and Windows SmartScreen stops
    blocking the install with "unknown publisher".

    The certificate is supplied as a base64-encoded PFX plus its password,
    through environment variables that map to GitHub Actions secrets:

        WINDOWS_CODESIGN_PFX_BASE64      base64 of the .pfx / .p12 file
        WINDOWS_CODESIGN_PFX_PASSWORD    the PFX export password (may be empty)
        WINDOWS_CODESIGN_TIMESTAMP_URL   RFC-3161 timestamp URL (optional;
                                         defaults to DigiCert's)

    When WINDOWS_CODESIGN_PFX_BASE64 is empty the -Path mode is a no-op that
    warns and returns a marker object with exit 0 — a fork build or a
    secret-less run still produces (unsigned) installers instead of failing.
    The -ShowPublisher / -ExportPfx modes require the secret and throw without
    it.

    The decoded PFX is written to a fresh temp directory and shredded in a
    finally block; it never lands in the workspace. (-ExportPfx is the one
    exception: it deliberately writes a copy the caller owns and must delete.)

.PARAMETER Path
    One or more files (globs allowed) to sign. Signing mode.

.PARAMETER ShowPublisher
    Print the certificate Subject distinguished name to stdout and return.
    The signed sideload MSIX build needs it for Identity/@Publisher.

.PARAMETER ExportPfx
    Directory to write the decoded PFX into (as aether-codesign.pfx). Prints
    the full path. Used to hand the cert to create-msix.ps1's -CertificateFile.
    The caller is responsible for deleting the file.

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

    [Parameter(ParameterSetName = 'ExportPfx', Mandatory = $true)]
    [string]$ExportPfx,

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
    if ($ShowPublisher -or $ExportPfx) {
        throw 'WINDOWS_CODESIGN_PFX_BASE64 is required for -ShowPublisher / -ExportPfx.'
    }
    Write-Warning 'WINDOWS_CODESIGN_PFX_BASE64 is not set - skipping Authenticode signing. Artifacts will be unsigned.'
    return [pscustomobject]@{ Skipped = $true; Reason = 'no-certificate'; Signed = @() }
}

$workDir = Join-Path ([IO.Path]::GetTempPath()) ('aether-codesign-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $workDir | Out-Null
$pfxPath = Join-Path $workDir 'codesign.pfx'

try {
    [IO.File]::WriteAllBytes($pfxPath, [Convert]::FromBase64String($PfxBase64.Trim()))

    if ($ExportPfx) {
        New-Item -ItemType Directory -Force -Path $ExportPfx | Out-Null
        $dest = Join-Path (Resolve-Path -LiteralPath $ExportPfx).Path 'aether-codesign.pfx'
        Copy-Item -LiteralPath $pfxPath -Destination $dest -Force
        Write-Output $dest
        return
    }

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
