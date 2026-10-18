# HackRF transmit test over the air with an RTL-SDR: design

Date: 2026-10-18 · Branch: `feat/hackrf-tx-ota-test`

## Intent

AetherSDR's HackRF transmit modes were checked only in software (the
sideband, carrier and image measurements in `hackrf_tx_modes_test`). The one
on-air attempt (2026-10-13, 10 m) heard nothing, and the cause was never
pinned down. This test answers a simple question for every transmit mode:
**does the signal that actually leaves the HackRF look right to an independent
receiver?**

The receiver is an RTL-SDR (Mark's is a Nooelec NESDR SMArt v5, R820T tuner)
plugged into the same computer, read only through the standard `rtl-sdr`
command-line tools. The test is for anyone with a HackRF and an RTL-SDR: it
goes in the repo with an operator guide.

## What the operator does

They run one command and watch:

```sh
python3 tools/hackrf_tx_ota_test.py --callsign W5TSU
```

That command does everything else:

- checks the hardware;
- starts AetherSDR;
- connects to the HackRF;
- runs every mode;
- sends a CW ID;
- restores the radio's settings and quits.

It prints a pass/fail table and leaves the evidence in a results folder.
Nothing has to be clicked while it runs. Ctrl+C unkeys and stops cleanly.

## Goals

- One unattended run covers every HackRF transmit mode: TUNE, USB, LSB, DIGU,
  DIGL, RTTY, CW, CWR, AM, SAM, DSB, FM, FMN and DFM. WFM is receive-only and
  is excluded.
- A clear pass or fail for each mode, measured from the RTL-SDR's IQ. Every
  failed check states the number it measured and the limit it missed.
- Evidence another person can inspect, kept per mode in the results folder:
  - the raw capture (`.cu8`);
  - a spectrum plot;
  - a demodulated audio file to listen to;
  - the numbers behind each check.

  The run also writes a summary report in both Markdown and JSON.
- Offline unit tests for the analysis code, using synthetic IQ, so the checks
  themselves are trustworthy before they ever judge a radio.
- An operator guide covering setup, running, reading results and
  troubleshooting.

## Non-goals

- Absolute power measurement. An RTL-SDR is not calibrated. Levels are
  reported as relative dB only.
- The HF bands. The R820T tunes from about 24 MHz up, and the HackRF's HF output
  path is a separate question from this test.
- Running in CI. This is hardware-in-the-loop. Only its offline unit tests join
  `ctest`.
- FreeDV, DRM, D-STAR and WFM. AetherSDR has no HackRF transmit for these.
- Regulatory emissions compliance. The spurious levels this test reports are
  diagnostics, not certification.

## Anti-goals

These would make it a failure even if every mode passed.

- Leaving the transmitter keyed for any reason: a crash, a Ctrl+C, a bridge
  timeout or a lost connection.
- Turning on the HackRF's RF amp, or transmitting at more than the operator's
  power ceiling.
- Transmitting on the air without station identification.
- Being so flaky that people stop trusting it. Signals from other 900 MHz
  devices must not turn into false failures.
- Needing the operator mid-run.

## Constraints and decisions

### The RF path: over the air

Both radios connect to the same computer by USB and each has its own antenna,
a short distance apart. No coax runs between them. *(Mark said both are on
this computer. Separate antennas, no cable, is my reading of that.)*

### Frequency: the 33 cm band

The default is **915.500 MHz**, in the 33 cm amateur band (902–928 MHz in
ITU Region 2), as Mark asked.

The exact frequency is my call. 915.5 MHz sits in the gap between the US915
LoRa uplink channels (902.3–914.9 MHz) and its downlink channels
(923.3–927.5 MHz), which cuts down collisions with nearby devices. Smart meters
still hop across the whole band, so a run also checks the channel before each
transmission (see "Interference" below).

`--freq` overrides the default. The guide tells operators outside Region 2 to
pick a frequency in their own allocation.

### Power: minimum

Mark set the ceiling at about 50 mW, and lower is better.

AetherSDR maps the RF Power slider onto the HackRF's TX VGA (0–47 dB). The RF
amp (+11 dB) switches on only above 81 %.

- **The test never goes above 80 %, so the amp never comes on.** With the amp
  off, a HackRF's output at 900 MHz is a few mW at full VGA, well under 50 mW.
  This hardware limit is the real interlock: the HackRF has no
  forward-power meter to watch.
- **A second layer:** the app is launched with
  `AETHER_AUTOMATION_TX_MAX_POWER` set to the same cap. The script refuses any
  `--max-power` above 80.
- **It starts low.** RF and Tune Power begin at 10 %. During the first (TUNE)
  burst, the script steps power up until the RTL-SDR sees the carrier at least
  35 dB above the noise, then uses that level for every mode. If the cap is
  reached first, the run continues and the summary marks it "weak signal".

### Station identification

`--callsign` is required. The CW test sends `VVV DE <CALL>`. Every run ends with
`DE <CALL>` in CW. A run that lasts longer than 10 minutes also identifies
every 10 minutes. This follows the US Part 97 rule (ID at least every 10
minutes and at the end).

### Being there

The operator stays at the bench. `docs/automation/TX_TEST_PROMPT.md` says not
to run unattended TX on a backend with no power meter unless a physical
interlock limits power. Here the amp-off ceiling plays that role, and the guide
says so.

### Receive tools: `rtl-sdr` only

`rtl_test` confirms the dongle is there. `rtl_sdr` records raw IQ for each
mode. The analysis is Python with numpy and scipy, both present here.

Each mode gets one `rtl_sdr` recording, demodulated in Python, rather than a
second keyed burst through `rtl_fm`. That halves the time on the air. The
guide shows how to listen live with `rtl_fm` instead. *(My call.)*

### Driving the app

The script talks to AetherSDR's automation bridge directly, through the `Bridge`
class in `tools/automation_probe.py` (as `tools/tx_meter_test.py` does), not
through MCP, so others can run it with plain Python.

By default it launches the app itself, so the safety environment is always set:

```text
AETHER_AUTOMATION=1
AETHER_AUTOMATION_ALLOW_TX=1
AETHER_AUTOMATION_TX_MAX_POWER=<cap>
AETHER_AUTOMATION_SOCKET=<unique name>
```

It finds the HackRF with `connect list` and connects to it by serial.

- `--attach` uses an AetherSDR that is already running, provided its
  `whoami` reports `txAllowed`.
- `--headless` launches with `QT_QPA_PLATFORM=offscreen`.
- By default the window is shown, so the operator can watch.

## How one mode is tested

1. **Set up.**
   - Set the slice mode.
   - Use the TX frequency already set: the script tunes once, at the start.
   - Turn on the test tone where the mode needs audio: `testtone on 1000 0`
     (1 kHz, 0 dBFS). The guide notes that the mic chain's gain still applies.
2. **Check the channel.** A 0.5 s `rtl_sdr` recording with nothing keyed. If
   anything within ±30 kHz of the TX frequency is 15 dB or more above the
   noise, wait 2 s and look again, up to 5 times. After that, record the mode
   as "skipped (channel busy)", not as a failure.
3. **Record and key.**
   - Start a 5 s `rtl_sdr` recording in the background.
   - The RTL-SDR is tuned 250 kHz below the TX frequency, at 1.024 MS/s, which
     keeps the signal off the dongle's DC spike.
   - After 1 s, key for 3 s. Each mode keys its own way: TUNE starts the tune
     carrier, voice and data modes use PTT, CW and CWR send text through CWX.
   - Then unkey, and turn the test tone off.
4. **Confirm the radio stopped.** The radio and the model must both report not
   transmitting within 1 s. If not, abort the whole run: send unkey again, stop
   any tune, and exit non-zero.
5. **Analyse.**
   - Mix the recording to baseband at the TX frequency.
   - The power envelope locates the keyed section. The unkeyed lead-in is the
     noise floor, so each recording carries its own baseline.
   - Run that mode's checks.
   - If the recording clipped (`.cu8` samples at 0 or 255 in more than 0.1 % of
     samples), lower the RTL-SDR gain one step and repeat the mode once.

### Frequency reference

TUNE runs first. Its measured carrier offset is the HackRF and RTL-SDR
frequency errors combined, and the run reports it in ppm. A HackRF One has a
20 ppm crystal, about ±18 kHz at 915 MHz.

Every other mode is measured **relative to that carrier**, so crystal error
can't cause a sideband or deviation failure. The absolute frequency check
passes within `--freq-tol-ppm`, default 25.

### Checks per mode

The thresholds below are starting values, kept in one table at the top of the
analysis module. The first real run calibrates them. The checks themselves
don't change.

| Mode | What must be true |
|---|---|
| TUNE | A single carrier, at least 35 dB above the noise (else "weak signal"). Absolute frequency within tolerance. |
| USB, DIGU | A tone at carrier +1 kHz. Opposite sideband (−1 kHz) ≥ 25 dB down. Carrier ≥ 20 dB below the tone. |
| LSB, DIGL | The mirror of USB: tone at −1 kHz, upper sideband ≥ 25 dB down, carrier ≥ 20 dB down. |
| RTTY | AFSK on USB. With the test tone: the same checks as USB. If the bridge can send RTTY text, it also checks for two tones 170 Hz apart on the upper side. |
| CW, CWR | Carrier at the expected offset from the dial frequency, within 50 Hz. The envelope decodes back to the text sent. The dit length matches the set WPM within 15 %. Width at −40 dB within 400 Hz (key-click check). |
| AM, SAM | Carrier plus sidebands at ±1 kHz, within 2 dB of each other. Modulation depth worked out from the sideband-to-carrier ratio, between 30 % and 100 %. Second-harmonic sidebands at ±2 kHz ≥ 25 dB below the first. |
| DSB | Sidebands at ±1 kHz, within 2 dB of each other. Carrier ≥ 20 dB below them. |
| FM, DFM | Demodulates to a 1 kHz tone (within 2 %). Peak deviation no more than 5 kHz + 10 %. |
| FMN | As FM, with peak deviation no more than 2.5 kHz + 10 %, and about half of FM's deviation at the same tone (ratio 0.4–0.6). |
| All | No other signal within ±200 kHz stronger than 40 dB below the main one. Reported, not failed, on the first run. |

## Hard stops

The run aborts, unkeys and exits non-zero on:

- a failed unkey confirmation;
- losing the bridge or the radio;
- a `--max-power` above 80;
- a missing NESDR or HackRF;
- the app reporting the amp enabled while keyed (if it can be read).

Unkey also runs from a `finally` block, a SIGINT/SIGTERM handler and
`atexit`. Every burst is at most 3 s, under the bridge's force-unkey watchdog.

## At the end of a run

- Send the CW ID.
- Put back the mode, frequency, RF Power, Tune Power and CW settings found at
  the start.
- Turn the test tone off.
- Quit AetherSDR if the script launched it.

Exit codes: `0` if every mode passed (skips are allowed), `1` if any check
failed, `2` on an abort.

## What goes in the repo

- `tools/hackrf_tx_ota_test.py`: the orchestrator and CLI (preflight, launch,
  per-mode sequence, safety, report).
- `tools/hackrf_tx_ota_analyze.py`: pure analysis on numpy arrays, with no I/O
  or bridge. It can also re-analyse a saved results folder:
  `--reanalyze <dir>`.
- `tools/test_hackrf_tx_ota_analyze.py`: offline tests. It synthesises each
  mode's IQ, both correct and deliberately wrong (the wrong sideband, carrier
  leak, over-deviation, clipping, a busy channel), and checks that the analyser
  passes and fails the right ones. It is registered in `tests/tests.cmake` and
  skips cleanly (ctest `SKIP_RETURN_CODE`) when numpy or scipy is missing.
- `docs/HACKRF-TX-TEST.md`: the operator guide:
  - the hardware and antenna setup;
  - installing `rtl-sdr`, numpy and scipy;
  - licensing and ID;
  - the command and its options;
  - reading the report and listening to the WAVs;
  - troubleshooting (dongle busy, channel busy, weak signal, clipping);
  - how to tune the thresholds.

  `docs/HACKRF.md` links to it.
- A ROADMAP entry replacing the "on-air check" item with what was found.

## Unknowns the first live step must settle

The plan starts with a short live check of each of these before building the
full harness:

1. AetherSDR doesn't hold the NESDR open. Its RTL-SDR discovery might, which
   would block `rtl_sdr`.
2. The client test tone reaches the HackRF transmit path with no microphone
   configured.
3. Nothing blocks transmit at 915.5 MHz (band plan or TX inhibit).
4. The bridge's CWX send keys the HackRF in CW mode, and which verb starts
   TUNE.
5. How RTTY transmit audio is generated, and whether the bridge can send text.
6. The RTL-SDR hears the amp-off HackRF across the desk at a low drive.

Anything that turns out broken in AetherSDR itself is reported to Mark as a
finding. It is not fixed silently inside this work.

## Left to the builder

- Module structure and the CLI flag names beyond those named here.
- Plot styling, if matplotlib is present. If not, the plots are skipped with a
  notice.
- The RTL-SDR gain stepping. The start is 20 dB, adjusted down on clipping.
- How much time each burst gets beyond the 3 s keyed window.
