# Windows Code Signing

AetherSDR's Windows release artifacts are **Authenticode-signed in CI** so that
Windows SmartScreen stops blocking the install with *"Windows protected your
PC — unknown publisher"*.

This is separate from:

- the **GPG** detached signatures (`.asc`) produced by `sign-release.yml` —
  those authenticate the download, they are not Authenticode and do nothing for
  SmartScreen;
- the **Microsoft Store** package — Partner Center signs the `.msixupload`, so
  it stays built with `-SkipSign` and is not covered here.

## What gets signed

`.github/workflows/windows-installer.yml`, on a `v*` tag build:

| Artifact | Step | Notes |
|---|---|---|
| `deploy\AetherSDR.exe`, `deploy\aether-dv-waveform.exe` | *Sign application binaries* | Signed in the payload before the ZIP / installer / MSIX consume it, so all three carry a signed exe. Qt and other third-party DLLs are already vendor-signed and are left alone. |
| `AetherSDR-*-setup.exe` (Inno) | *Sign installer* | The artifact most users download and double-click; its signature is what SmartScreen checks at install time. |
| `sideload-msix\AetherSDR-*.msix` | *Create signed sideload MSIX* | A second MSIX built **without** the Partner Center identity, signed so an end user can install it directly. `continue-on-error` — see the caveat below. |

All signing runs through `packaging/windows/sign-windows-artifacts.ps1`
(`signtool sign /fd SHA256 /tr <timestamp> /td SHA256 /f <pfx>`), which decodes
the certificate to a temp file and shreds it in a `finally` block.

## One-time setup

You need a code-signing certificate as a password-protected `.pfx` / `.p12`
file. Then, on the repository:

```bash
# base64-encode the PFX (no line wrapping)
base64 -w0 my-codesign.pfx > my-codesign.pfx.b64          # Linux/WSL
#   PowerShell: [Convert]::ToBase64String([IO.File]::ReadAllBytes('my-codesign.pfx')) > my-codesign.pfx.b64

gh secret set WINDOWS_CODESIGN_PFX_BASE64  < my-codesign.pfx.b64
gh secret set WINDOWS_CODESIGN_PFX_PASSWORD                # paste the PFX password
gh variable set WINDOWS_CODESIGN_TIMESTAMP_URL --body 'http://timestamp.digicert.com'
```

| Name | Kind | Purpose |
|---|---|---|
| `WINDOWS_CODESIGN_PFX_BASE64` | secret | base64 of the `.pfx` |
| `WINDOWS_CODESIGN_PFX_PASSWORD` | secret | the PFX export password (may be empty) |
| `WINDOWS_CODESIGN_TIMESTAMP_URL` | variable | RFC-3161 timestamp URL; the script defaults to DigiCert's if unset. Use your CA's if it publishes one. |

**Without `WINDOWS_CODESIGN_PFX_BASE64`** every signing step no-ops (logs a
warning, exits 0) and the installers ship unsigned — so forks and secret-less
`workflow_dispatch` runs still build.

The timestamp is not optional in practice: it lets signatures stay valid after
the certificate expires. The steps always pass `/tr`.

## Certificate type and the SmartScreen "Install anyway" prompt

Signing removes *"unknown publisher"*. Whether the **"Install anyway"** button
disappears depends on the certificate class:

- **OV** (Organization Validation) — the common `.pfx` case. SmartScreen keeps
  warning until the signed binary accumulates **reputation** (downloads over
  time under a *stable* signing identity), typically days to weeks. Rotating to
  a new certificate resets that reputation.
- **EV** (Extended Validation) — historically trusted by SmartScreen
  immediately, but EV keys must live on a hardware token / cloud HSM and cannot
  be used as a plain `.pfx` on a GitHub-hosted runner.
- **Azure Trusted Signing** — Microsoft's managed service; OV-class reputation
  behaviour, no hardware token. A possible future migration; not wired here.

So expect the prompt to fade as downloads accumulate, not vanish on the first
signed release.

## Signing locally

```powershell
$env:WINDOWS_CODESIGN_PFX_BASE64   = [Convert]::ToBase64String([IO.File]::ReadAllBytes('my-codesign.pfx'))
$env:WINDOWS_CODESIGN_PFX_PASSWORD = 'the-password'
pwsh packaging/windows/sign-windows-artifacts.ps1 -Path deploy\AetherSDR.exe, AetherSDR-1.2.3-setup.exe
```

`-DryRun` prints the `signtool` command (password redacted) without running it.
`-ShowPublisher` prints the certificate subject (what the sideload MSIX uses for
`Identity/@Publisher`).

## Rotating the certificate

1. `gh secret set WINDOWS_CODESIGN_PFX_BASE64 < new.pfx.b64`
2. `gh secret set WINDOWS_CODESIGN_PFX_PASSWORD`
3. If the subject DN changed, SmartScreen reputation restarts from zero.

## Caveats

- **Sideload MSIX publisher match.** `signtool` requires the MSIX manifest's
  `Identity/@Publisher` to exactly equal the certificate subject. The workflow
  sets it from `-ShowPublisher`, but distinguished-name string ordering can
  differ between `X509Certificate2.Subject` and what `signtool` expects. If the
  *Create signed sideload MSIX* step fails with a publisher-mismatch error,
  pass the exact string `signtool` reports as `AETHERSDR_MSIX_PUBLISHER`. The
  step is `continue-on-error`, so this never blocks the signed exe / installer.
- **`tests/windows_codesign_test.ps1`** covers the helper's argument
  construction, the no-secret no-op, and cleanup — offline, no `signtool`. It
  runs on the `windows-latest` lane (it mints a throwaway self-signed PFX via
  the Windows PKI cmdlets).
