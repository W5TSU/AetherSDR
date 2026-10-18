# HackRF — Over-the-Air Transmit Test

`tools/hackrf_tx_ota_test.py` keys every transmit mode of a HackRF through
AetherSDR, a few seconds each at low power. An RTL-SDR dongle a short distance
away records each one. The script then measures every recording and says, mode
by mode, whether what went out is right: the correct sideband, a suppressed
carrier, sensible AM depth and FM deviation, and CW that decodes back to the
text sent.

One command runs it all, and it needs no help once it starts.

## Quick start

From the top of the AetherSDR checkout, with both radios plugged in and
AetherSDR built:

```sh
python3 tools/hackrf_tx_ota_test.py --callsign <YOUR CALL> --out-dir my-run
```

It transmits for about 3 minutes, so stay at the station. When it finishes,
open `my-run/report.md`. Leave out `--out-dir` and the results go in
`./hackrf-tx-ota-<date>-<time>`.

Three scripts have similar names. Only the first one transmits:

| Command | What it does |
|---|---|
| `python3 tools/hackrf_tx_ota_test.py --callsign <CALL>` | **runs the test**: keys the HackRF, records, judges, reports |
| `python3 tools/hackrf_tx_ota_analyze.py --reanalyze <run folder>` | judges a saved run again, without transmitting |
| `python3 tools/test_hackrf_tx_ota_analyze.py`, `python3 tools/test_hackrf_tx_ota_test.py` | the test's own offline tests, no radio needed |

`--callsign` and `--out-dir` belong to `hackrf_tx_ota_test.py`. The analyser
only takes `--reanalyze`.

## What it tests, and what it doesn't

It tests the 14 HackRF transmit modes: TUNE, USB, LSB, DIGU, DIGL, RTTY, CW,
CWR, AM, SAM, DSB, FM, FMN and DFM. Each mode gets a clear pass or fail, and
every failed check gives the number it measured and the limit it missed. The
evidence for each mode stays in the results folder, so someone else can check
it: the raw recording, a spectrum plot, a WAV to listen to, and the numbers
behind each check.

It doesn't measure power. An RTL-SDR isn't a calibrated receiver, so every
level is relative dB, and nothing claims watts or dBm. It doesn't cover HF:
the RTL-SDR tunes from about 24 MHz up. Its spurious-emission numbers are
diagnostics, not a compliance measurement. WFM is receive-only on a HackRF and
isn't tested.

## What you need

- A HackRF One or HackRF Pro, with an antenna.
- An RTL-SDR with an R820T or R820T2 tuner (a NESDR SMArt, an RTL-SDR Blog
  V3/V4, ...), with an antenna.
- The `rtl-sdr` command-line tools (`rtl_test`, `rtl_sdr`; `rtl_fm` for
  listening live). On Debian or Ubuntu, install the `rtl-sdr` package.
- Python 3 with numpy and scipy. matplotlib is optional: without it there are
  no spectrum plots.
- A built AetherSDR. The script runs `build/AetherSDR` unless you pass
  `--app`.

## Licence and identification

This transmits, so you need a licence for the frequency you use.

The default, 915.5 MHz, is in the 33 cm amateur band (902–928 MHz). That band
is amateur only in ITU Region 2, the Americas. In the US it falls under
Part 97, which has local restrictions on 902–928 MHz (§97.303), so check
yours. Part 97 also wants your callsign at least every 10 minutes and at the
end (§97.119). The script sends `DE <CALL>` in CW every 9 minutes and when it
finishes.

Outside Region 2, choose a frequency in one of your own bands with
`--freq-mhz`, for example in 70 cm. It must be between 24 and 1700 MHz,
where the RTL-SDR can receive.

The HackRF has no output filter, so its harmonics and spurs go out too. Keep
the drive low and the antennas close. The script does both by default.

## Setup

1. Put the two antennas a short distance apart, about a metre. The script
   finds the drive level the RTL-SDR needs, so the exact distance isn't
   critical.
2. Close every other SDR program. AetherSDR has to open the HackRF, and
   `rtl_sdr` has to open the dongle.
3. Check that both radios are there:

   ```sh
   rtl_test -t      # lists the dongle and its tuner; Ctrl+C if it keeps running
   hackrf_info      # lists the HackRF and its firmware
   ```

   Update the HackRF firmware first if it's old (see
   [HACKRF.md](HACKRF.md#check-your-firmware-first)).

## Running it

```sh
python3 tools/hackrf_tx_ota_test.py --callsign W1AW
```

The script:

1. checks the RTL-SDR, then launches AetherSDR on its own settings profile
   (your normal settings aren't touched) and connects the HackRF;
2. tunes to 915.5 MHz and checks the channel is clear;
3. steps the TUNE drive up from 10 %, in 1.5 s bursts, until the dongle sees
   the carrier 55 dB above its noise, and then uses that drive for every mode;
4. tests each mode in turn: checks the channel, starts a recording, keys for
   2.5 s, unkeys, confirms the radio stopped, and analyses the recording;
5. sends your callsign in CW, quits AetherSDR and writes the report.

A full run takes about 3 minutes. The AetherSDR window is visible so you can
watch it work. **Don't type into the AetherSDR window while the test runs.**
The test profile turns keyboard shortcuts on, and F9 keys the transmitter.

Press **Ctrl+C** at any time. The script unkeys everything, confirms the
radio stopped, writes the report so far and quits AetherSDR. It doesn't send
the ID after an abort, so send it by hand if the run had transmitted.

### How each mode is keyed

| Mode | Keyed by | Audio |
|---|---|---|
| TUNE | AetherSDR's TUNE (in USB) | none: a plain carrier |
| USB, LSB, AM, SAM, DSB, FM, FMN | PTT | AetherSDR's test tone, 1 kHz at −6 dB |
| DIGU, DIGL | TCI | 1500 Hz tone |
| RTTY | TCI | 2125 Hz (mark) for half the burst, then 2295 Hz (space) |
| DFM | TCI | 1 kHz tone |
| CW, CWR | PTT and the straight key (bound to F9) | sends `TEST` at 15 WPM |

Data modes take their audio from TCI, the same path WSJT-X and fldigi use. On
a HackRF, the test tone only reaches the voice modes. The script turns
AetherSDR's TCI server on in its profile (port 50001) and is the TCI client.

### Options

| Option | Default | |
|---|---|---|
| `--callsign` | (required) | sent in CW every 9 minutes and at the end |
| `--freq-mhz` | 915.5 | TX frequency, 24–1700 MHz |
| `--max-power` | 80 | drive cap in %, at most 80 (see Safety) |
| `--start-power` | 10 | the power ramp's first step, in % |
| `--modes` | all 14 | comma-separated, e.g. `USB,LSB`; TUNE always runs first |
| `--burst-s` | 2.5 | key-down per mode, 0.5–3 s |
| `--tone-hz`, `--tone-db` | 1000, −6 | the voice modes' test tone |
| `--data-level-db` | −6 | the data modes' TCI audio level, dBFS |
| `--cw-text`, `--cw-wpm` | TEST, 15 | what the CW test sends; the ID uses the same speed |
| `--rtl-gain` | 20.7 | RTL-SDR gain in dB; lowered a step when it clips |
| `--rtl-rate` | 2048000 | RTL-SDR sample rate |
| `--rtl-offset-hz` | −400000 | where the dongle tunes relative to TX, off its DC spike |
| `--freq-tol-ppm` | 25 | absolute frequency tolerance, both crystals together |
| `--out-dir` | `./hackrf-tx-ota-<date>-<time>` | the results folder |
| `--app` | `build/AetherSDR` | the AetherSDR binary |
| `--headless` | off | launch AetherSDR with no window (`QT_QPA_PLATFORM=offscreen`) |
| `--serial` | the first HackRF | which HackRF, by serial |
| `--tci-port` | 50001 | AetherSDR's TCI port |
| `--attach` | off | use an AetherSDR that's already running (its automation socket name) |

`--attach` needs an AetherSDR started with `AETHER_AUTOMATION=1` and
`AETHER_AUTOMATION_ALLOW_TX=1`. For CW and the data modes, its profile also
needs keyboard shortcuts on, the CW key bound to F9 and TCI enabled. Without
TCI the data modes are skipped. Without the key binding the CW tests fail.
Letting the script launch AetherSDR is simpler.

## Safety

- **The drive never goes above 80 %.** AetherSDR's HackRF backend switches the
  HackRF's RF amp on at 82 % and above. With the amp off, the output stays
  well under 50 mW (the HackRF's published maximum, amp included, is about
  15 dBm).
  `--max-power` can lower the cap but not raise it. AetherSDR is launched
  with `AETHER_AUTOMATION_TX_MAX_POWER` at the same cap. The script reads
  both RF Power and Tune Power back before every key and refuses to key if
  either is higher.
- **Every key is short and is always undone.** A mode keys for 2.5 s, and the
  CW ID for about 5 s. After every key the script unkeys everything and waits
  up to 1 s for AetherSDR to report the radio not transmitting. If it doesn't,
  the run stops. Ctrl+C, a `kill` and the script's own exit also unkey. As a
  last resort, AetherSDR's bridge drops a stuck PTT after 15 s
  (`AETHER_AUTOMATION_TX_MAX_MS=15000`), and AetherSDR drops TCI PTT when the
  script's TCI connection closes.
- **Stay at the station.** A HackRF has no power meter, so nothing measures
  what actually goes out. `docs/automation/TX_TEST_PROMPT.md` says not to run
  automated transmit unattended without a physical limit on power. Here, the
  amp-off cap is that limit, but you're still the control operator.

## Reading the results

```text
hackrf-tx-ota-20261018-153000/
  report.md          the summary: one row per mode
  report.json        the same, for scripts
  run.json           the settings and every capture (what --reanalyze reads)
  run.log            the console output
  aethersdr.log      AetherSDR's log
  profile/           the private settings profile the run used
  ramp/10/ ...       the power ramp's TUNE bursts
  TUNE/ USB/ ...     one folder per mode:
    capture.cu8        the raw recording (rtl_sdr format)
    result.json        every check, measured value and limit
    audio.wav          the recording demodulated as that mode
    spectrum.png       ±25 kHz around the dial frequency
```

The recordings take about 17 MB per mode, roughly 250 MB per run.

`report.md` has a table of `Mode | Status | Failed checks (measured vs limit)`,
then notes per mode, then the run itself: frequency, the measured crystal
error in ppm, how far it drifted during the run, the drive, the RTL-SDR gain
and the AetherSDR commit.

| Status | Meaning |
|---|---|
| pass | every check passed |
| fail | at least one check failed; the row says which, with the numbers |
| skip | not tested: the channel stayed busy, or no TCI for a data mode |
| abort | the run stopped here (Ctrl+C, a failed unkey, a lost radio, ...) |

The script exits 0 if every mode passed (skips are allowed), 1 if a check
failed, and 2 on an abort.

### The checks

Everything is measured relative to the mode's own signal. TUNE's carrier
gives the two radios' combined crystal error, which is the absolute
`frequency` check. After that the reference follows the crystals as they
drift. Each mode that passes measures where the dial is from its own signal
(the SSB tone minus 1 kHz, the AM carrier, the FM Bessel lines' centre, ...),
and the next mode starts from there, looking within 300 Hz. A mode that fails
(a wrong sideband, say) doesn't move the reference.

This matters most for CW, the only mode judged on an absolute position: its
carrier must sit on the dial. Cold radios drift fast as they warm. The
reference bench measured 74 Hz in under 2 minutes after half an hour idle.
Against TUNE's reading from a minute earlier, a correct CW carrier looked
50 Hz off. Against the previous mode's reading it's within 5 Hz. The report's
"Drift over the run" line shows how far the dial moved.

| Mode | Check | Passes when |
|---|---|---|
| all but TUNE | `signal` | the wanted line stands at least 20 dB over the noise |
| all | `spurious` | report-only: the strongest other line within ±200 kHz, flagged above −40 dBc (another station may be the cause) |
| TUNE | `snr` | report-only: carrier over noise in a 10 Hz bin; under 55 dB is noted as a weak signal |
| TUNE | `frequency` | within `--freq-tol-ppm` of the dial frequency |
| USB, DIGU | `opposite_sideband` | the image at −tone is at least 25 dB below the tone at +tone |
| LSB, DIGL | `opposite_sideband` | the same, mirrored |
| USB, LSB, DIGU, DIGL, RTTY | `carrier_suppression` | the carrier is at least 20 dB below the tone |
| RTTY | `shift` | mark and space are 170 ± 10 Hz apart, on the upper sideband |
| RTTY | `balance` | mark and space within 6 dB of each other |
| AM, SAM | `symmetry` | the two sidebands within 2 dB |
| AM, SAM | `depth` | modulation depth from the sideband-to-carrier ratio, 30–100 % |
| AM, SAM | `harmonics` | the 2 kHz sidebands at least 25 dB below the 1 kHz ones |
| DSB | `symmetry` | the two sidebands within 2 dB |
| DSB | `carrier_suppression` | the carrier at least 20 dB below the sidebands |
| FM, FMN, DFM | `modulated` | the audio reached the modulator (β ≥ 0.1) |
| FM, FMN, DFM | `tone_frequency` | the Bessel lines are spaced at the tone, within 2 % |
| FM, DFM | `deviation` | peak deviation at most 5 kHz + 10 % |
| FMN | `deviation` | at most 2.5 kHz + 10 % |
| FMN | `fmn_ratio` | 0.4–0.6 of FM's deviation (FM must run in the same pass) |
| CW, CWR | `decode` | the envelope decodes back to the text sent |
| CW, CWR | `carrier_offset` | the carrier within 50 Hz of the dial, as the previous passing mode measured it |
| CW, CWR | `dit_length` | within 15 % of the set WPM |
| CW, CWR | `width` | at most 400 Hz wide at −40 dB (key clicks) |

FM deviation comes from the spectrum: a 1 kHz tone puts lines every 1 kHz,
and their levels follow Bessel functions of β = deviation / tone. The fit's
β is in the mode's notes.

If an SSB mode has no tone where it should be but a strong one on the other
side, the note says "the tone is on the other sideband". That's the
wrong-sideband mistake this test exists to catch.

### Listening

Each `audio.wav` is the recording demodulated the way a receiver in that
mode would play it (CW and TUNE beat at 700 Hz). The levels are fixed, not
normalised: a mode that sent nothing sounds like quiet noise, and a
wrong-sideband transmission sounds nearly silent.

```sh
aplay hackrf-tx-ota-*/USB/audio.wav
```

### Judging a run again

The thresholds live in one table, `THRESHOLDS`, at the top of the per-mode
checks in `tools/hackrf_tx_ota_analyze.py`. Re-judge a saved run without
transmitting:

```sh
python3 tools/hackrf_tx_ota_analyze.py --reanalyze hackrf-tx-ota-20261018-153000
```

It rewrites every `result.json`, `audio.wav`, `spectrum.png` and the reports,
and exits 0/1/2 like the test.

## Results on the reference bench

2026-10-18: a HackRF Pro (firmware 2026.01.3) and a NESDR SMArt v5, antennas
about a metre apart, at 915.5 MHz. The power ramp stopped at its first step:
10 % drive gave a 72.6 dB carrier. Every mode ran at 10 %, with the RTL-SDR
at 20.7 dB gain. Every run, with warm radios or cold, gives the same result:

| Mode | Status | Failed checks (measured vs limit) |
|---|---|---|
| TUNE | pass | — |
| USB | pass | — |
| LSB | pass | — |
| DIGU | pass | — |
| DIGL | pass | — |
| RTTY | pass | — |
| CW | pass | — |
| CWR | pass | — |
| AM | pass | — |
| SAM | pass | — |
| DSB | pass | — |
| FM | pass | — |
| FMN | pass | — |
| DFM | pass | — |

What it measured, so you can compare your own run:

| | Measured | Limit |
|---|---|---|
| Combined crystal error | −1.41 ppm (−1.3 kHz) | ±25 ppm |
| SSB and data opposite sideband | 43.6–45.1 dB down | 25 dB |
| Carrier suppression, USB and LSB (test tone) | 33.1–33.7 dB | 20 dB |
| Carrier suppression, DIGU, DIGL, RTTY (TCI audio) | 24.9–27.4 dB | 20 dB |
| RTTY shift, balance | 166 Hz, 1.3 dB | 170 ± 10 Hz, 6 dB |
| AM and SAM depth, harmonics | 49–50 %, 43 dB down | 30–100 %, 25 dB |
| DSB carrier suppression | 27.5 dB | 20 dB |
| FM, FMN, DFM deviation (1 kHz tone at −6 dB) | 2.52, 1.25, 1.26 kHz | 5.5, 2.75, 5.5 kHz |
| FMN / FM | 0.497 | 0.4–0.6 |
| CW dit at 15 WPM, width at −40 dB | 84.8 ms (+6 %), 290 Hz | ±15 %, 400 Hz |
| CW carrier from the dial | −3 to +5 Hz | 50 Hz |
| Drift over the run | 18 Hz warm; 74 Hz after half an hour idle | report-only |
| Strongest other line within ±200 kHz | −49 dBc or lower | report-only |

The data modes' carrier suppression is lower than USB's because their TCI
tone is 4–6 dB weaker than the test tone. The carrier itself sits within
about 2 dB of the same level.

## How this test was verified

**Offline**, on synthetic recordings, in ctest as `hackrf_tx_ota_analyze` and
`hackrf_tx_ota_safety`:

- `tools/test_hackrf_tx_ota_analyze.py` (25 tests) builds each mode's signal
  correctly and deliberately wrong. Each check has to pass the right one and
  fail the wrong one: a wrong sideband, carrier leak, over-modulated AM,
  over-deviated FM, CW key clicks, an interfering station and crystal drift.
  It also covers the reports, the WAVs and `--reanalyze`.
- `tools/test_hackrf_tx_ota_test.py` (19 tests) runs the whole sequence
  against a fake radio, a fake RTL-SDR and a fake clock. It checks the safety
  guarantees: the 80 % cap, refusing to key above it, aborting when the radio
  doesn't confirm unkey, Ctrl+C in mid-burst, a missing dongle, and that
  nothing is keyed after an abort. It also covers the power ramp, the 9-minute
  ID, a busy channel, restoring the radio, the TCI audio client and
  following drift.

**On the air**, 2026-10-18, HackRF Pro and NESDR SMArt v5 at 915.5 MHz:

1. Building the test found an AetherSDR bug: a HackRF Pro transmitted about
   70 dB too weak, in bursts. Fixed in 1a35baff (see the ROADMAP).
2. The first run stopped before testing any mode. `rtl_sdr` never stops
   when the recording fills its buffers exactly. The script now avoids those
   lengths.
3. Two full runs with warm radios: all 14 modes passed both times.
   `--reanalyze` reproduced the report exactly.
4. Two runs by the operator, W5TSU, after the radios had sat idle for half
   an hour. Every mode passed except CW and CWR, which decoded correctly but
   measured 50–54 Hz from TUNE's carrier. W5TSU confirmed by ear that CW
   works. The cause was drift: the dial moved 74 Hz during the run, steadily
   from mode to mode. The reference now follows the drift. The second run
   (both used the same folder) was re-judged with `--reanalyze`, with no new
   transmission. It passes all 14 modes, with CW 4–5 Hz from the dial.

## Listening live

To hear a mode yourself, key it from AetherSDR's own controls and listen on
the dongle (not while the test runs: only one program can open the dongle):

```sh
rtl_fm -f 915.5e6 -M usb -s 48k - | aplay -r 48000 -f S16_LE
```

Use `-M lsb`, `-M am` or `-M fm` for the other modes. Add `-p <ppm>` to
correct the dongle's crystal: the test's report gives the combined error.

## Troubleshooting

| Symptom | What to do |
|---|---|
| `RTL-SDR: usb_claim_interface error -6` | Another program has the dongle (`rtl_fm`, `rtl_tcp`, a running SDR app, or the kernel DVB driver). Close it. For the DVB driver, blacklist `dvb_usb_rtl28xxu`. |
| `no HackRF in AetherSDR's connect list` | Run `hackrf_info`. If it doesn't list the HackRF, unplug it and plug it back in, or press its RESET button. Close other programs that hold it. |
| A mode is `skip` with "channel busy" | Something else was within 30 kHz of the TX frequency for 10 s. Run again later, or pick another `--freq-mhz`. |
| `weak signal` in TUNE's notes | Even at the cap the carrier was under 55 dB over the noise. Move the antennas closer, or raise `--rtl-gain`. The checks still run. |
| Every mode fails with "no signal received" | Nothing was heard even at the cap. Check both antennas, the frequency (inside both radios' range), and that `hackrf_info` sees the HackRF. Listen live with `rtl_fm` while you press TUNE in AetherSDR. |
| "RTL-SDR clipped" in a note | The dongle was overloaded. The script already lowered the gain and repeated the mode. If it keeps happening, move the antennas apart or lower `--rtl-gain`. |
| CW or CWR fails `carrier_offset` | If the mode before it failed, CW was judged against an older reading of the dial. Fix that mode first. Otherwise check "Drift over the run": let cold radios warm up for a few minutes. |
| TUNE `frequency` fails | The two crystals together are more than `--freq-tol-ppm` off. Check the HackRF's frequency calibration in Radio Setup and the dongle's (`rtl_test -p`). |
| `the radio still reports transmitting 1 s after unkey` | The run stopped on purpose. Check AetherSDR shows not transmitting, then look at `aethersdr.log`. |

## Tuning the thresholds

The limits started from the design and were checked on the first real run
(see the plan, `docs/superpowers/plans/2026-10-18-hackrf-tx-ota-test.md`).
To change one, edit `THRESHOLDS` in `tools/hackrf_tx_ota_analyze.py`, run the
offline tests (`python3 tools/test_hackrf_tx_ota_analyze.py`), then
`--reanalyze` a saved run to see the effect without transmitting again.
