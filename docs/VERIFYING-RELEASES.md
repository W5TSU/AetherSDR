# Verifying W5TSU AetherSDR Releases

> This is the **W5TSU fork**. Releases here are signed with W5TSU's own key,
> **not** the upstream `aethersdr/AetherSDR` release key. The fingerprint below
> is the one to trust for downloads from `github.com/W5TSU/AetherSDR`.

## Signing Overview

| Platform | Signing Method |
|----------|---------------|
| Linux AppImage | GPG detached signature (`.asc`) |
| Windows .exe | GPG detached signature (`.asc`) |
| Windows .zip | GPG detached signature (`.asc`) |
| Source archive | GPG detached signature (`.asc`) |
| macOS DMG / .pkg | not currently produced by this fork |

Each release also includes a GPG-signed `SHA256SUMS.txt` covering all
Linux, Windows and source artifacts.

## GPG Key Fingerprint

    D179 B7F0 75A9 EAA0 27B4  8E30 B05C EE12 778F BEC5

    Mark Grennan (W5TSU) <mark@w5tsu.net>  —  rsa4096, key id B05CEE12778FBEC5

## Import the Public Key

From the repository:

```bash
curl -sSL https://raw.githubusercontent.com/W5TSU/AetherSDR/main/docs/RELEASE-SIGNING-KEY.pub.asc | gpg --import
```

Or from keys.openpgp.org:

```bash
gpg --keyserver keys.openpgp.org --recv-keys B05CEE12778FBEC5
```

## Verify a Linux Download

### Option 1: Verify the artifact directly

```bash
gpg --verify AetherSDR-v1.0.0-x86_64.AppImage.asc AetherSDR-v1.0.0-x86_64.AppImage
```

### Option 2: Verify checksums first, then check the file

```bash
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
sha256sum -c SHA256SUMS.txt
```

Expected output should show
**`Good signature from "Mark Grennan (W5TSU) (Used for AetherSDR releases) <mark@w5tsu.net>"`**.

GPG is typically pre-installed on Linux. If not:

```bash
# Arch
sudo pacman -S gnupg

# Debian/Ubuntu
sudo apt install gnupg
```

## macOS Users

This fork does not currently publish macOS DMG or `.pkg` artifacts. Build
from source, or use the upstream `aethersdr/AetherSDR` macOS release.

## Windows Users

The Windows `.exe` installer and `.zip` portable build are GPG-signed.
Windows SmartScreen may still show a warning because the binaries are
not Authenticode-signed. This is expected for open-source projects
without an EV code signing certificate. To verify the download:

```powershell
# Install Gpg4win from https://gpg4win.org/
gpg --import RELEASE-SIGNING-KEY.pub.asc
gpg --verify AetherSDR-Setup-vX.Y.Z.exe.asc AetherSDR-Setup-vX.Y.Z.exe
```

## Commit Signing

All commits on `main` must be GPG-signed by their author. GitHub displays
a green "Verified" badge on signed commits. Contributors should set up
commit signing with their personal GPG key — see
[DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md#commit-signing) for setup instructions.
