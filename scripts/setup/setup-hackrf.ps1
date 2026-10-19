<#
.SYNOPSIS
    Build libhackrf for Windows x64 (the HackRF backend, #42).

.DESCRIPTION
    libhackrf ships no Windows binaries. Great Scott Gadgets' own Windows
    recipe (docs/source/installing_hackrf_software.rst in the release) builds
    it with MSVC against vcpkg's libusb and pthreads; this does the same, at
    the release the macOS DMG builds (scripts/setup/setup-macos-deps.sh), and
    stages the result in third_party/hackrf/ for CMake:

        include/libhackrf/hackrf.h
        lib/hackrf.lib                     import library
        bin/hackrf.dll, libusb-1.0.dll, pthreadVC3.dll

    The three DLLs ship next to AetherSDR.exe (CMake copies them there).

    Needs vcpkg (GitHub's Windows runners have it at VCPKG_INSTALLATION_ROOT;
    elsewhere set VCPKG_ROOT) and an MSVC environment: a Developer prompt, or
    after ilammy/msvc-dev-cmd in CI.

    No driver install is needed for the HackRF itself: its firmware advertises
    a WinUSB compatible ID, so Windows binds WinUSB on first plug-in.

.EXAMPLE
    .\scripts\setup\setup-hackrf.ps1
#>

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_verify_sha256.ps1"

$HackrfVersion = "2026.01.3"
# The release .zip: Windows' tar cannot be relied on for .tar.xz. Its
# host/libhackrf tree is identical to the .tar.xz setup-macos-deps.sh pins.
$HackrfUrl     = "https://github.com/greatscottgadgets/hackrf/releases/download/v${HackrfVersion}/hackrf-${HackrfVersion}.zip"
$HackrfSha256  = "601599e14d03cc1900cf9a1c832957a997dc0d488f8299581bd73b40718481a5"
$Triplet       = "x64-windows"
$OutDir        = "third_party\hackrf"
$ZipFile       = "third_party\hackrf-${HackrfVersion}.zip"
$Stamp         = "hackrf=$HackrfVersion triplet=$Triplet"

# -- Check if already set up ----------------------------------------------
if ((Test-Path "$OutDir\lib\hackrf.lib") -and (Test-Path "$OutDir\.stamp") -and
    ((Get-Content -Raw "$OutDir\.stamp").Trim() -eq $Stamp)) {
    Write-Host "libhackrf $HackrfVersion already set up in $OutDir" -ForegroundColor Green
    exit 0
}

# -- libusb and pthreads from vcpkg ---------------------------------------
$vcpkgRoot = if ($env:VCPKG_INSTALLATION_ROOT) { $env:VCPKG_INSTALLATION_ROOT } else { $env:VCPKG_ROOT }
if (-not $vcpkgRoot -or -not (Test-Path (Join-Path $vcpkgRoot "vcpkg.exe"))) {
    throw "vcpkg not found: set VCPKG_ROOT (GitHub runners set VCPKG_INSTALLATION_ROOT)"
}
Write-Host "Installing libusb and pthreads ($Triplet) with vcpkg..." -ForegroundColor Cyan
& (Join-Path $vcpkgRoot "vcpkg.exe") install "libusb:$Triplet" "pthreads:$Triplet"
if ($LASTEXITCODE -ne 0) { throw "vcpkg install libusb pthreads failed ($LASTEXITCODE)" }
$installed = Join-Path $vcpkgRoot "installed\$Triplet"

# -- Download and extract the source --------------------------------------
New-Item -ItemType Directory -Force -Path "third_party" | Out-Null
if (-not (Test-Path $ZipFile)) {
    Write-Host "Downloading hackrf $HackrfVersion source..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $HackrfUrl -OutFile $ZipFile
}
Confirm-Sha256 -Path $ZipFile -Expected $HackrfSha256
$tempDir = "third_party\hackrf-temp"
if (Test-Path $tempDir) { Remove-Item -Recurse -Force $tempDir }
Expand-Archive -Path $ZipFile -DestinationPath $tempDir
$srcDir = (Resolve-Path "$tempDir\hackrf-$HackrfVersion\host\libhackrf").Path

# -- Build the DLL with MSVC ----------------------------------------------
# libusb is passed by path, as setup-macos-deps.sh does: FindLIBUSB's
# find_path() has no libusb-1.0/ suffix, so it cannot find vcpkg's header on
# its own. PThreads4W comes from the vcpkg toolchain (share/PThreads4W).
# Build-tree artifacts are copied directly rather than `cmake --install`ed:
# libhackrf's Windows install rule copies a $<CONFIGURATION>/ subdirectory
# that a single-config Ninja build never creates.
# -D values containing a dot or a path are quoted (PowerShell's native-argument
# tokenizer splits an unquoted -Dkey=a.b at the dot; see setup-hidapi.ps1).
Write-Host "Building libhackrf $HackrfVersion with MSVC..." -ForegroundColor Cyan
$buildDir = "$tempDir\build"
cmake -B $buildDir -S $srcDir -G "Ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    -DENABLE_STATIC_LIB=OFF `
    -DINSTALL_UDEV_RULES=OFF `
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
    "-DVCPKG_TARGET_TRIPLET=$Triplet" `
    "-DLIBUSB_INCLUDE_DIR=$installed\include\libusb-1.0" `
    "-DLIBUSB_LIBRARIES=$installed\lib\libusb-1.0.lib"
if ($LASTEXITCODE -ne 0) { throw "libhackrf configure failed ($LASTEXITCODE)" }
cmake --build $buildDir --target hackrf
if ($LASTEXITCODE -ne 0) { throw "libhackrf build failed ($LASTEXITCODE)" }

# -- Stage ----------------------------------------------------------------
if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path "$OutDir\include\libhackrf", "$OutDir\lib", "$OutDir\bin" | Out-Null
Copy-Item "$srcDir\src\hackrf.h" "$OutDir\include\libhackrf\"
foreach ($name in @("hackrf.dll", "hackrf.lib")) {
    $file = Get-ChildItem -Path $buildDir -Recurse -Filter $name | Select-Object -First 1
    if (-not $file) { throw "libhackrf build produced no $name" }
    $dest = if ($name.EndsWith(".dll")) { "$OutDir\bin" } else { "$OutDir\lib" }
    Copy-Item $file.FullName $dest
}
$runtime = @(Get-ChildItem "$installed\bin\libusb-1.0.dll") + @(Get-ChildItem "$installed\bin\pthreadVC*.dll")
if ($runtime.Count -lt 2) { throw "vcpkg's libusb-1.0.dll or pthreadVC*.dll is missing from $installed\bin" }
$runtime | ForEach-Object { Copy-Item $_.FullName "$OutDir\bin\" }
Set-Content -Path "$OutDir\.stamp" -Value $Stamp -NoNewline
Remove-Item -Recurse -Force $tempDir

Write-Host "libhackrf $HackrfVersion staged in ${OutDir}:" -ForegroundColor Green
Get-ChildItem -Recurse -File $OutDir | ForEach-Object { Write-Host "  $($_.FullName)" }
