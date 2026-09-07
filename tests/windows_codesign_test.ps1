# Socket-free, signtool-free tests for the Authenticode signing helper
# (packaging/windows/sign-windows-artifacts.ps1). Mirrors
# windows_store_policy_test.ps1: self-contained, no network, no SDK tools.
# Runs on the windows-latest lane only — uses the Windows PKI cmdlets to mint
# a throwaway self-signed PFX so the decode / X509 / export paths are real.
[CmdletBinding()]
param([string]$RepositoryRoot = (Join-Path $PSScriptRoot '..'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$signScript = Join-Path $RepositoryRoot 'packaging/windows/sign-windows-artifacts.ps1'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('aether-codesign-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null

$script:assertions = 0
function Assert-True([bool]$Value, [string]$Message) {
    if (-not $Value) { throw "FAIL: $Message" }
    $script:assertions++
}
function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
    $caught = $null
    try { & $Action | Out-Null } catch { $caught = $_ }
    Assert-True ($null -ne $caught) "Expected failure matching $Pattern"
    Assert-True ($caught.ToString() -match $Pattern) "Wrong failure: $caught"
}

# Throwaway self-signed code-signing cert -> PFX -> base64. Never signs anything
# here; only the decode / subject-read / export code paths are exercised.
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=AetherSDR Test Signer' `
    -CertStoreLocation 'Cert:\CurrentUser\My' -KeyExportPolicy Exportable -NotAfter (Get-Date).AddDays(1)
$pfxPassword = 'test-pw'
$pfxFile = Join-Path $scratch 'signer.pfx'
Export-PfxCertificate -Cert $cert -FilePath $pfxFile `
    -Password (ConvertTo-SecureString $pfxPassword -AsPlainText -Force) | Out-Null
$pfxBase64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($pfxFile))
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force

$saved = @{
    B64 = $env:WINDOWS_CODESIGN_PFX_BASE64
    Pw  = $env:WINDOWS_CODESIGN_PFX_PASSWORD
    Ts  = $env:WINDOWS_CODESIGN_TIMESTAMP_URL
}
try {
    $fileA = Join-Path $scratch 'AetherSDR.exe'
    $fileB = Join-Path $scratch 'aether-dv-waveform.exe'
    Set-Content -LiteralPath $fileA -Value 'stub'
    Set-Content -LiteralPath $fileB -Value 'stub'

    # 1. No certificate configured -> -Path mode is a no-op, not a failure.
    $env:WINDOWS_CODESIGN_PFX_BASE64 = ''
    $env:WINDOWS_CODESIGN_PFX_PASSWORD = ''
    $r = & $signScript -Path $fileA -DryRun -WarningAction SilentlyContinue
    Assert-True ($r.Skipped -and $r.Reason -eq 'no-certificate') 'Empty secret is a no-op in -Path mode'

    # ...but -ShowPublisher / -ExportPfx demand the cert.
    Assert-Throws { & $signScript -ShowPublisher } 'required for -ShowPublisher'
    Assert-Throws { & $signScript -ExportPfx $scratch } 'required for -ShowPublisher'

    # 2. Dry run builds the expected signtool command.
    $env:WINDOWS_CODESIGN_PFX_BASE64 = $pfxBase64
    $env:WINDOWS_CODESIGN_PFX_PASSWORD = $pfxPassword
    $env:WINDOWS_CODESIGN_TIMESTAMP_URL = ''
    $r = & $signScript -Path $fileA, $fileB -DryRun
    Assert-True ($r.DryRun -and -not $r.Skipped) 'Dry run reports itself'
    Assert-True ($r.SignArgs[0] -eq 'sign') 'First signtool arg is sign'
    Assert-True ($r.SignArgs[$r.SignArgs.IndexOf('/fd') + 1] -eq 'SHA256') 'SHA-256 file digest'
    Assert-True ($r.SignArgs -contains '/tr') 'Timestamp requested'
    Assert-True ($r.SignArgs[$r.SignArgs.IndexOf('/tr') + 1] -eq 'http://timestamp.digicert.com') 'Default timestamp URL'
    Assert-True ($r.SignArgs[$r.SignArgs.IndexOf('/td') + 1] -eq 'SHA256') 'SHA-256 timestamp digest'
    Assert-True ($r.Files.Count -eq 2) 'Both target files resolved'
    Assert-True ($r.SignArgs[-1] -eq $r.Files[-1]) 'Target files are the trailing args'
    Assert-True (($r.SignArgs -join ' ') -notmatch [regex]::Escape($pfxPassword)) 'Password is redacted from the echoed command'

    # 3. Timestamp URL override is honoured.
    $env:WINDOWS_CODESIGN_TIMESTAMP_URL = 'http://timestamp.acme.example/rfc3161'
    $r = & $signScript -Path $fileA -DryRun
    Assert-True ($r.SignArgs[$r.SignArgs.IndexOf('/tr') + 1] -eq 'http://timestamp.acme.example/rfc3161') 'Timestamp URL override'

    # 4. An unmatched path is a hard error, not a silent skip.
    Assert-Throws { & $signScript -Path (Join-Path $scratch 'no-such-*.exe') -DryRun } 'No file matched'

    # 5. -ShowPublisher returns the certificate subject for the MSIX manifest.
    Assert-True ((& $signScript -ShowPublisher).Trim() -eq 'CN=AetherSDR Test Signer') '-ShowPublisher emits the cert subject'

    # 6. -ExportPfx writes a .pfx the caller can hand to create-msix.ps1.
    $exported = (& $signScript -ExportPfx (Join-Path $scratch 'export')).Trim()
    Assert-True ((Test-Path -LiteralPath $exported) -and ([IO.Path]::GetExtension($exported) -eq '.pfx')) '-ExportPfx writes a .pfx file'

    # 7. No decoded PFX temp dir is left behind by any mode.
    $leaked = @(Get-ChildItem ([IO.Path]::GetTempPath()) -Directory -Filter 'aether-codesign-*' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike 'aether-codesign-test-*' })
    Assert-True ($leaked.Count -eq 0) 'Decoded-PFX temp directory is shredded'

    Write-Host "PASS: $script:assertions code-signing helper assertions; no signtool, no network."
}
finally {
    $env:WINDOWS_CODESIGN_PFX_BASE64 = $saved.B64
    $env:WINDOWS_CODESIGN_PFX_PASSWORD = $saved.Pw
    $env:WINDOWS_CODESIGN_TIMESTAMP_URL = $saved.Ts
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
