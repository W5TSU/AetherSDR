# Windows Code Signing

AetherSDR's Windows release artifacts are **Authenticode-signed in CI** so that
Windows SmartScreen stops blocking the install with *"Windows protected your
PC — unknown publisher"*.

Two methods, selected automatically by `./.github/actions/sign-windows`:

1. **Azure Trusted Signing** — Microsoft's managed signing service. Chosen when
   `AZURE_TRUSTED_SIGNING_ACCOUNT` is set. This is the path for public
   releases: no key material to hold, no hardware token.
2. **PFX certificate** — a `.pfx` / `.p12` file + password, supplied as base64
   secrets. Chosen when `WINDOWS_CODESIGN_PFX_BASE64` is set and Trusted
   Signing is not. Public CAs no longer issue file-based code-signing certs, so
   in practice this means a **self-signed** cert (local / pipeline testing) or
   a **legacy** file cert issued before June 2023.

When neither is configured (forks, secret-less runs) the signing steps no-op
and the installers ship unsigned.

This is separate from:

- the **GPG** detached signatures (`.asc`) from `sign-release.yml` — those
  authenticate the download, not the publisher, and do nothing for SmartScreen;
- the **Microsoft Store** package — Partner Center signs the `.msixupload`, so
  it stays built with `-SkipSign`.

## What gets signed

`.github/workflows/windows-installer.yml`, on a `v*` tag build:

| Artifact | Step |
|---|---|
| `deploy\AetherSDR.exe`, `deploy\aether-dv-waveform.exe` | *Sign application binaries* — before the ZIP / installer / MSIX consume the payload |
| `AetherSDR-*-setup.exe` (Inno) | *Sign installer* — the artifact most users download and double-click |
| `sideload-msix\AetherSDR-*.msix` | *Build sideload MSIX (unsigned)* then *Sign sideload MSIX* — a directly-installable package, `continue-on-error` |

Qt and other third-party DLLs are already vendor-signed and are left alone.

---

## Method 1 — Azure Trusted Signing (recommended)

### One-time Azure setup

1. In the Azure portal, create a **Trusted Signing account** (pick a region;
   its endpoint is `https://<region>.codesigning.azure.net/`).
2. Complete an **identity validation** (a verified legal entity, or an
   individual — individual validation has a waiting period). This is what the
   certificate's subject name comes from.
3. Create a **certificate profile** (type *Public Trust*). Note its **name**
   and its **subject distinguished name** (shown on the profile page).
4. Create an **Entra app registration** + a **client secret**.
5. On the Trusted Signing account, assign that app the
   **Trusted Signing Certificate Profile Signer** role.

### Repository configuration

```bash
gh secret set AZURE_TENANT_ID     -R W5TSU/AetherSDR
gh secret set AZURE_CLIENT_ID     -R W5TSU/AetherSDR
gh secret set AZURE_CLIENT_SECRET -R W5TSU/AetherSDR

gh variable set AZURE_TRUSTED_SIGNING_ENDPOINT -R W5TSU/AetherSDR --body 'https://wus2.codesigning.azure.net/'
gh variable set AZURE_TRUSTED_SIGNING_ACCOUNT  -R W5TSU/AetherSDR --body '<account name>'
gh variable set AZURE_TRUSTED_SIGNING_PROFILE  -R W5TSU/AetherSDR --body '<certificate profile name>'
gh variable set AETHERSDR_SIDELOAD_MSIX_PUBLISHER -R W5TSU/AetherSDR --body 'CN=<exact subject DN from the certificate profile>'
```

| Name | Kind | |
|---|---|---|
| `AZURE_TENANT_ID` / `AZURE_CLIENT_ID` / `AZURE_CLIENT_SECRET` | secrets | the signing service principal (kept distinct from the `AZURE_AD_*` Store-submission SP) |
| `AZURE_TRUSTED_SIGNING_ENDPOINT` / `_ACCOUNT` / `_PROFILE` | variables | from the Trusted Signing resource |
| `AETHERSDR_SIDELOAD_MSIX_PUBLISHER` | variable | the certificate profile's exact subject DN — the sideload MSIX manifest's `Identity/@Publisher` must equal it |

Trusted Signing timestamps against `http://timestamp.acs.microsoft.com`
(hard-coded in the composite action).

### SmartScreen

Trusted Signing certificates are OV-class: they remove *"unknown publisher"*
immediately, but SmartScreen still builds **reputation** from downloads over
time under the stable signing identity. Expect the *"Install anyway"* prompt to
fade over days/weeks, not vanish on the first signed release.

---

## Method 2 — PFX certificate (testing / legacy)

### Repository configuration

```bash
base64 -w0 my-codesign.pfx | gh secret set WINDOWS_CODESIGN_PFX_BASE64 -R W5TSU/AetherSDR
#   PowerShell: [Convert]::ToBase64String([IO.File]::ReadAllBytes('my-codesign.pfx')) | gh secret set WINDOWS_CODESIGN_PFX_BASE64 ...
gh secret   set WINDOWS_CODESIGN_PFX_PASSWORD -R W5TSU/AetherSDR
gh variable set WINDOWS_CODESIGN_TIMESTAMP_URL -R W5TSU/AetherSDR --body 'http://timestamp.digicert.com'
```

| Name | Kind | |
|---|---|---|
| `WINDOWS_CODESIGN_PFX_BASE64` | secret | base64 of the `.pfx` |
| `WINDOWS_CODESIGN_PFX_PASSWORD` | secret | export password (may be empty) |
| `WINDOWS_CODESIGN_TIMESTAMP_URL` | variable | RFC-3161 URL; `packaging/windows/sign-windows-artifacts.ps1` defaults to DigiCert's |

The helper decodes the PFX to a temp file and shreds it in a `finally` block.
For the sideload MSIX it reads the manifest publisher straight off the cert
(`-ShowPublisher`), so `AETHERSDR_SIDELOAD_MSIX_PUBLISHER` is not needed on this
path.

### Making a self-signed PFX for pipeline testing

```powershell
$c = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Mark Grennan" `
     -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(3)
Export-PfxCertificate -Cert $c -FilePath aethersdr-codesign.pfx `
     -Password (Read-Host -AsSecureString "PFX password")
```

A self-signed cert removes *"unknown publisher"* only on machines where it has
been installed into **Trusted Root** + **Trusted Publishers**. It does nothing
for the public — use it to exercise the pipeline, not to ship.

### Signing locally

```powershell
$env:WINDOWS_CODESIGN_PFX_BASE64   = [Convert]::ToBase64String([IO.File]::ReadAllBytes('my-codesign.pfx'))
$env:WINDOWS_CODESIGN_PFX_PASSWORD = 'the-password'
pwsh packaging/windows/sign-windows-artifacts.ps1 -Path deploy\AetherSDR.exe, AetherSDR-1.2.3-setup.exe
```

`-DryRun` prints the `signtool` command (password redacted) without running it.

---

## Caveats

- **Sideload MSIX publisher match.** `signtool` requires the MSIX manifest's
  `Identity/@Publisher` to exactly equal the certificate subject. Trusted
  Signing: set `AETHERSDR_SIDELOAD_MSIX_PUBLISHER` to the profile's subject DN. PFX: read
  off the cert automatically. Distinguished-name string ordering can still
  differ from what `signtool` expects; the sign step is `continue-on-error`, so
  a mismatch never blocks the signed exe / installer.
- **`tests/windows_codesign_test.ps1`** covers the PFX helper's argument
  construction, the no-secret no-op, `-ShowPublisher`, and cleanup — offline, no
  `signtool`, on the `windows-latest` lane. The Trusted Signing path is the
  vendored `Azure/trusted-signing-action` and is exercised only by a real run.
- **Rotation.** Re-set the secret(s); if the certificate subject changes,
  SmartScreen reputation restarts from zero and `AETHERSDR_SIDELOAD_MSIX_PUBLISHER`
  must be updated to match.
