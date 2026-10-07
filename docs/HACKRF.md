# HackRF — Operator Notes

AetherSDR drives a HackRF One or HackRF Pro over USB through `libhackrf`:
one panadapter (zoom 62.5 kHz – 20 MHz with Ctrl+wheel; 16 MHz on a Pro), one slice
demodulated on the host (WDSP), and transmit (SSB, CW, TUNE) with TX drive
and frequency calibration in Radio Setup.

## Check your firmware first

Most "it receives badly" reports on a HackRF come down to firmware. With
AetherSDR closed (only one program can open the HackRF at a time), run:

```sh
hackrf_info
```

Look at two lines:

```text
libhackrf version: 2026.01.3 (0.9.2)
Firmware Version: git-1925e590 (API:1.09)
```

The firmware should be a **release** (`2026.01.3`), matching the release of
your `libhackrf` / `hackrf-tools`. A `git-…` firmware is a development build.
Update it as shown below.

If `hackrf_info` prints `hackrf_board_id_read() failed` or garbage in the
serial number, the HackRF has stopped responding over USB. Unplug it and plug
it back in, or press RESET, before anything else.

## Updating the firmware

This follows Great Scott Gadgets' own guide,
[Updating Firmware](https://hackrf.readthedocs.io/en/latest/updating_firmware.html).
Their guide is authoritative if the two ever disagree.

1. **Install the HackRF tools**, so you have `hackrf_info` and
   `hackrf_spiflash`:
   - Debian/Ubuntu: `sudo apt install hackrf`
   - macOS (Homebrew): `brew install hackrf`
   - Windows: the tools ship with [radioconda](https://github.com/ryanvolz/radioconda)

2. **Download the latest release package** from
   <https://github.com/greatscottgadgets/hackrf/releases/latest>
   (for example `hackrf-2026.01.3.tar.xz` or `.zip`) and unpack it.

3. **Pick the `.bin` file for your board** from the package's `firmware-bin/`
   folder. `hackrf_info` prints the board on its `Board ID Number` line.

   | `hackrf_info` says | File |
   |---|---|
   | `HackRF Pro` | `firmware-bin/hackrf_pro_usb.bin` |
   | `HackRF One` | `firmware-bin/hackrf_one_usb.bin` |

   Write the `.bin` file, never the `.dfu`. The `.dfu` is only for recovery
   (see below).

4. **Close AetherSDR and any other SDR program**, then flash:

   ```sh
   hackrf_spiflash -w hackrf-2026.01.3/firmware-bin/hackrf_pro_usb.bin
   ```

   Don't unplug the HackRF while this runs.

5. **Reset the HackRF**: press RESET, or unplug it and plug it back in.

6. **Confirm**: `hackrf_info` should now show `Firmware Version: 2026.01.3`.

If `hackrf_spiflash` says `HACKRF_ERROR_NOT_FOUND`, it's usually USB
permissions. On Linux, the `hackrf` package installs the udev rule; unplug
and replug after installing it.

### Only if the update fails: recovery over DFU

If the HackRF no longer starts after a failed flash:

1. Hold the **DFU** button, press and release **RESET**, then release DFU.
   On a HackRF Pro all LEDs stay off; on a HackRF One the 3V3 LED lights.
2. Load the firmware into RAM. This uses the `.dfu` file:

   ```sh
   dfu-util --device 1fc9:000c --alt 0 --download hackrf-2026.01.3/firmware-bin/hackrf_pro_usb.dfu
   ```

   (`sudo apt install dfu-util` on Debian/Ubuntu.)
3. Repeat steps 4–6 above to write the `.bin` file to flash.

## Known limitation: stations from outside the capture

The HackRF digitizes a block of spectrum as wide as its sample rate (the
"capture"). Its analog filter is supposed to stop stations outside that block
from folding into it. On a HackRF Pro running development firmware
`git-1925e590`, we measured almost no rejection:

- The 96.1 MHz FM station showed up on 108.6 MHz only 4.5 dB weaker with a
  12.5 MS/s capture centred on 103 MHz.
- Changing the filter setting (even to its narrowest, 1.75 MHz) made no
  measurable difference.

What you hear is a weak signal that **fades as you drag the panadapter or
zoom**, though the tuned frequency never changes. A folded-in station raises
the noise around your signal and the AGC turns it down.

AetherSDR works around it in two ways:

- **The slice stays near the middle of the capture**, within 25 % of the
  sample rate, where rejection measured best. Drag or zoom past that, and the
  capture recentres on your slice. Your frequency doesn't change.
- **Spans narrower than 8 MHz are shown from an 8 MS/s capture**, filtered
  and decimated in software, instead of running the hardware at 2 or 4 MS/s,
  which measured worst.

Updating to release firmware is the first thing to try if you still hear it.
On a HackRF Pro it does not change this: the Pro's firmware sets the analog
filter from the sample rate by itself and ignores the requested bandwidth.

## Known limitation: HackRF Pro sample rates

On a HackRF Pro (firmware 2026.01.3) we measured the whole spectrum landing
**1.75–2 MHz off the tuned frequency at 10, 12.5 and 20 MS/s**. The direction
depends on the frequency. The display still shows the frequency you tuned,
but the station you hear is somewhere else, so the radio sounds de-tuned
when you zoom out past 8 MHz. At 8 and 16 MS/s every FM station sits exactly
on its channel.

The Pro tunes its hardware off the requested frequency and shifts the
spectrum back in its FPGA. At those three rates the two evidently don't
cancel.

So on a HackRF Pro, AetherSDR only uses the **8 and 16 MS/s** hardware rates:
- zoom spans from 62.5 kHz to 8 MHz come from the 8 MS/s capture;
- the widest view is 16 MHz, not 20.

A HackRF One keeps every rate up to 20 MS/s.
