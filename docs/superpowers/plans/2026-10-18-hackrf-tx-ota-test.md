# HackRF Over-the-Air TX Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One command keys AetherSDR's HackRF in every transmit mode at
≤ 80 % drive (amp off) on 915.5 MHz. It records each burst with an RTL-SDR
through `rtl_sdr`, and reports a pass/fail table from the IQ. An operator
guide is included.

**Architecture:** Two Python modules in `tools/`:

- **`hackrf_tx_ota_analyze.py`**: pure numpy/scipy measurement. It takes IQ
  arrays in and gives results out, with no I/O beyond the file helpers, and
  can re-analyse a saved results folder.
- **`hackrf_tx_ota_test.py`**: the orchestrator. It does the preflight,
  launches the app and talks to the bridge through `automation_probe.Bridge`.
  It runs `rtl_sdr` as a subprocess, sequences each mode, enforces safety and
  writes the reports.

Offline tests drive both modules with synthetic IQ and a fake bridge.

**Tech Stack:** Python 3 (stdlib, numpy ≥ 1.24, scipy ≥ 1.10, optional
matplotlib); `rtl-sdr` CLI (`rtl_test`, `rtl_sdr`); AetherSDR automation
bridge plus its in-process TCI client.

**Spec:** `docs/superpowers/specs/2026-10-18-hackrf-tx-ota-test-design.md`

## Global Constraints

- TX frequency default **915.500 MHz**; `--freq` overrides.
- RF and Tune Power are **never above 80 %** (the amp engages at ≥ 82 %:
  `txDriveForPowerPercent`, `kHackRfTxAmpGainDb = 11`). The script refuses
  `--max-power` > 80, and every value it sends passes the same check. The app
  is launched with `AETHER_AUTOMATION_TX_MAX_POWER=<max-power>`.
- The power ramp starts at **10 %**.
- `--callsign` is required. The CW test text is `VVV DE <CALL>`. Every run
  ends with `DE <CALL>`, and the run identifies again whenever 9 minutes have
  passed since the last ID.
- Non-CW keyed windows are **3 s**. CW is the text's length plus 0.5 s. The app
  is launched with `AETHER_AUTOMATION_TX_MAX_MS=15000`.
- After every unkey, the radio must report not transmitting **within 1 s**, or
  the run aborts with exit code 2.
- Unkey runs from a `finally` block, a SIGINT/SIGTERM handler and `atexit`.
- Exit codes: `0` all passed (skips allowed), `1` any check failed, `2` abort.
- RTL-SDR: **2.048 MS/s**, centre = TX − **400 kHz**, start gain **20.7 dB**.
  Clipping means more than 0.1 % of samples at 0 or 255; the response is one
  gain step down and one repeat of the mode.
- Busy channel: anything within ±30 kHz of TX that is ≥ 15 dB above the noise.
  Wait 2 s and recheck, up to 5 times, then mark the mode `skip`.
- Test tone: **1000 Hz**. The level is settled in Task 1; the default is
  `--tone-db -6`.
- Measurements are relative to the TUNE carrier. The absolute frequency check
  allows `--freq-tol-ppm` 25.
- The repo's Python tests are plain scripts using `check()` and `main()`, run
  as `python3 tools/test_*.py`. Follow `tools/test_tx_meter_test.py`, not
  pytest.
- Commits are SSH-signed (already configured) and end with the
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` trailer.

## Review Focus

1. **A busy 900 MHz channel** (smart-meter hops) during the keyed window, not
   just before it, must not read as a sideband or spurious failure. The
   analyser only uses the keyed segment, and the spurious check is report-only.
   *Test:* `test_interferer_in_keyed_window_reports_not_fails` in Task 3.
2. **No carrier found** (the HackRF transmitted nothing, as on 2026-10-13) must
   end with a clear "no signal received" failure for every mode. It must not
   produce a crash or a NaN-filled report. *Test:*
   `test_silence_reports_no_signal` in Task 2, and
   `test_run_with_no_signal_fails_cleanly` in Task 5.
3. **Ctrl+C mid-burst** must unkey (PTT off, `tune:0,false`, `keyer:0,false`,
   test tone off) before exiting. *Test:* `test_sigint_during_burst_unkeys` in
   Task 5.
4. **`rtl_sdr` failing to open the dongle** (busy or unplugged) must abort
   before any keying. It must not key with nothing recording. *Test:*
   `test_rtl_open_failure_aborts_before_keying` in Task 5.
5. **An app already running on the default socket** (`--attach` not given)
   must not be driven by mistake. The script launches its own instance on a
   unique socket and talks only to that one. *Test:*
   `test_launch_uses_unique_socket` in Task 5.

---

### Task 1: Live feasibility check (spike, no repo code)

This settles the design's six unknowns on hardware, with Mark at the bench,
before any harness is written. Write the findings into this plan under
"Task 1 findings". If an AetherSDR defect blocks a mode, stop and report it to
Mark. Don't fix it inside this work.

**Files:**
- Modify: this plan (the findings section only)

- [ ] **Step 1: Rebuild the app from the branch**

Run: `cmake --build build --parallel`
Expected: the build ends with no errors, and `build/AetherSDR` is newer than
HEAD.

- [ ] **Step 2: Launch with the bridge and TX enabled**

```bash
AETHER_AUTOMATION=1 AETHER_AUTOMATION_ALLOW_TX=1 AETHER_AUTOMATION_TX_MAX_POWER=80 \
AETHER_AUTOMATION_TX_MAX_MS=15000 AETHER_AUTOMATION_SOCKET=hackrf-tx-ota-spike \
./build/AetherSDR &
```

Then run `python3 tools/automation_probe.py --socket <path> whoami`.

Expected: `txAllowed: true`. Record the socket path the explicit name
resolves to, for Task 5's launcher.

- [ ] **Step 3: Connect to the HackRF**

Run `connect list`, then `connect local serial <hackrf serial>`, then
`connect wait 30000`. Record the `connect list` entry shape: the field that
identifies a HackRF and its serial.

- [ ] **Step 4: Confirm the NESDR is still free**

With the app connected, run
`rtl_sdr -f 915.1e6 -s 2.048e6 -g 20.7 -n 2048000 /tmp/spike.cu8`.
Expected: it writes 4 096 000 bytes with no `usb_claim_interface` error.

- [ ] **Step 5: TUNE through TCI at 10 %, with recording**

1. Set up: `tune 915.5`, `slice mode USB`, `transmit tunepower 10`,
   `invoke tciEnable click` (note its prior state), `tci start`.
2. Start `rtl_sdr` for 5 s in the background.
3. After 1 s, run `tci send tune:0,true;`. Hold 3 s, then run
   `tci send tune:0,false;`.
4. Check `get radio`: `transmitting:false`.
5. Look at the capture's FFT. Expected: one carrier near +400 kHz from the RTL
   centre.
6. Record its level above the noise in a 10 Hz bin.
7. Repeat at 20/40/60 % until the carrier is ≥ 55 dB above the 10 Hz-bin
   noise, or 80 % is reached. Record the level that reached it.

If nothing shows at 80 %, stop: this is the 2026-10-13 symptom and a finding
for Mark.

- [ ] **Step 6: Test tone through PTT**

1. Set up: `slice mode USB`, `transmit rfpower <level>`,
   `testtone on 1000 -6`.
2. Start the capture, then `key ptt on`. Hold 3 s, then `key ptt off`, then
   `testtone off`.
3. Expected: a tone at carrier + 1 kHz.

If there's no RF, the test tone isn't reaching the HackRF without a mic. That
is a finding.

Also try −12 dB and 0 dB. Record which level gives an undistorted tone (the
2 kHz harmonic ≥ 30 dB down) and an AM depth between 30 and 100 %. That
becomes `--tone-db`.

- [ ] **Step 7: CW through PTT plus the TCI keyer**

1. Set up: `slice mode CW`, `key ptt on`.
2. Send five 100 ms `tci send keyer:0,true;` / `keyer:0,false;` pairs, 100 ms
   apart.
3. Then `key ptt off`.
4. Expected: five carrier pulses on the dial frequency (CW transmits at the
   dial; `cwBfoHz` only shifts receive).
5. Record the measured pulse lengths. Jitter tells us whether 15 WPM (80 ms
   dit) timing holds to ±15 %.

- [ ] **Step 8: FM deviation sanity check**

Run `slice mode FM` with the tone on for 3 s of PTT, then FMN. Record the peak
deviation in each and their ratio. Expected: ≤ 5 kHz and ≤ 2.5 kHz, with a
ratio of about 0.5.

- [ ] **Step 9: Restore and quit**

Restore the TCI enable state and the original mode, frequency and powers.
Then `quit` the app. Run `hackrf_info`. Expected: the HackRF is still listed.

- [ ] **Step 10: Record the findings**

Fill in the "Task 1 findings" section below:

- the socket path rule;
- the `connect list` shape;
- the NESDR concurrency result;
- the working TUNE level;
- the tone level;
- the CW jitter;
- the FM deviations;
- `get radio`, `get slice` and `get transmit` field names for: transmitting,
  frequency, mode, `rfPower`, `tunePower`;
- the TCI enable control's state read-back.

### Task 1 findings

Run 2026-10-18 13:11–13:30 on a HackRF Pro (firmware 2026.01.3) and a NESDR
SMArt v5, over the air across the desk, at 915.5 MHz.

- **Socket:** `AETHER_AUTOMATION_SOCKET=<name>` listens on `/tmp/<name>`
  (the temp dir plus the name). Each instance also writes
  `/tmp/aethersdr-automation/<pid>.json`.
- **Connect:** `connect list` returns `radios[]`, where the HackRF entry has
  `family: "hackrf"`, `model: "HackRF Pro"` and `serial`. The NESDR shows as
  `family: "rtl"` but is not opened. Then `connect local serial <serial>`, and
  `connect wait 30000` replies `connected: true` in under 0.1 s.
- **NESDR concurrency:** `rtl_sdr` opens the NESDR fine while the app holds
  the HackRF. Its recording starts 0.4–0.6 s after launch, so the harness must
  wait for data before keying, not just sleep.
- **TUNE:** there's no bridge TUNE verb. `shortcut tune_toggle` (TX-gated)
  starts and stops TUNE on the HackRF. The log's `no command plane … dropping
  transmit tune 1` lines are harmless: the Flex text form is dropped, and
  `tuneCommandIssued` keys the backend.
- **Drive:** `transmit tunepower|rfpower N` reaches the HackRF at keying:
  VGA 6/12/23/35/46 dB at 10/20/40/60/80 %, amp off throughout.
- **CW:** `cwx send` refuses (the HackRF has no radio-side keyer), and the TCI
  server is off by default. The working path is a private settings profile
  (`AETHER_SETTINGS_DIR`), seeded before launch with
  `AetherSDR --config set Shortcut_cwkey <key>`. Then `keyevent press|release
  cwkey` keys the straight key (TX-gated) inside a `key ptt on`. **Not verified
  live:** blocked by the defect below.
- **Test tone:** `testtone on 1000 -6` reaches the HackRF with no microphone.
  The USB tone lands at carrier + 1 kHz.
- **Tone level:** at −6 dB, AM modulation depth is about 98 % (amCarrierLevel
  48). At −12 dB it is about 58 %. So AM and SAM use −12 dB, and the other
  modes −6 dB.
- **FM deviation:** a Bessel-line fit gave about 4.5 kHz for FM and about
  2.25 kHz for FMN at −6 dB, a ratio of about 0.5. Measured on the defective
  signal below.
- **915.5 MHz:** no TX inhibit.
- **Field names:**
  - `get slice`: `frequency` (MHz), `mode`.
  - `get transmit`: `rfPower`, `tunePower`, `transmitting`, `tuning`,
    `mox`, `cwPitch`, `cwBreakIn`, `amCarrierLevel`.
  - `get radio`: `transmitting`, `connected`, `connectState`.
- **Quit:** `invoke Quit trigger`.
- **DEFECT, which blocks every mode: AetherSDR's HackRF transmit output is about
  42 dB+ too weak, and it comes in bursts.**
  - At the same frequency, rate (8 MS/s), TX VGA (46 dB) and amp setting (off),
    `hackrf_transfer -c 100` reaches the NESDR at 0 dBFS (and clips it):
    73 dB above the noise in a 10 Hz bin, 20 Hz wide at −20 dB, with a steady
    envelope.
  - AetherSDR's TUNE (a full-scale DC carrier, about 2 % silence padding by
    the worker's own counters) reaches it at −41.7 dBFS: 34 dB above the
    noise, 2.3 kHz wide at −20 dB.
  - Its envelope is mostly about 44 dB below its peaks. It comes in about
    2.5 ms bursts and 0.1 ms spikes, about 3.5 ms apart.
  - USB, AM, DSB, FM and FMN show the same pedestal.
  - `packTxIq` and the TUNE feed look correct. The cause lies in streaming or
    hardware control during TX. It is not yet diagnosed, and is reported to
    Mark rather than fixed here. It very likely explains the 2026-10-13
    "no signal".
- **Minor:** the HackRF backend persists `txSetpoints.tunePower` only when TUNE
  is keyed. A changed Tune Power setting that is never keyed reverts on
  reconnect.

---

### Task 2: Capture handling and the core measurements

**Files:**
- Create: `tools/hackrf_tx_ota_analyze.py`
- Test: `tools/test_hackrf_tx_ota_analyze.py`

**Interfaces:**
- Produces:
  - `@dataclass CaptureSpec(sample_rate_hz: float, rtl_center_hz: float, tx_freq_hz: float)`
  - `load_cu8(path: str) -> tuple[np.ndarray, float]`: returns complex64 IQ
    scaled to ±1 (`(b - 127.5) / 127.5`) and the clip fraction (share of bytes
    equal to 0 or 255).
  - `to_baseband(iq: np.ndarray, spec: CaptureSpec, offset_hz: float = 0.0) -> np.ndarray`:
    mixes so `tx_freq_hz + offset_hz` sits at 0 Hz.
  - `keyed_window(bb: np.ndarray, fs: float) -> tuple[int, int] | None`:
    [start, end) sample indices where the 10 ms-smoothed envelope within
    ±20 kHz exceeds (lead-in median + 10 dB). It trims 50 ms at both ends. It
    returns None if no window is longer than 200 ms.
  - `spectrum_db(seg: np.ndarray, fs: float, rbw_hz: float = 10.0) -> tuple[np.ndarray, np.ndarray]`:
    averaged Hann FFT. Frequencies are baseband Hz, ascending; levels are dB
    relative to full scale.
  - `peak_near(freqs, db, offset_hz: float, search_hz: float = 50.0) -> tuple[float, float]`:
    `(freq_hz, level_db)` of the strongest bin within ±search.
  - `noise_floor_db(freqs, db, exclude: list[tuple[float, float]]) -> float`:
    the median over bins outside the excluded (lo, hi) ranges, within ±100 kHz.
  - `channel_busy(bb: np.ndarray, fs: float, half_width_hz: float = 30e3, threshold_db: float = 15.0) -> bool`
  - `@dataclass CheckResult(name: str, passed: bool, measured: float | None, limit: str, unit: str)`
  - `@dataclass ModeResult(mode: str, status: str, checks: list[CheckResult], notes: list[str])`:
    `status` is one of `"pass"`, `"fail"`, `"skip"`, `"abort"`.
  - `THRESHOLDS: dict[str, float]`: one module-level table holding every
    numeric limit in the spec's checks table, keyed as in Task 3.

- [ ] **Step 1: Write the failing tests.** Synthetic IQ at 2.048 MS/s,
      centre TX − 400 kHz, quantised to cu8 through a helper
      `to_cu8(iq) -> bytes`.
  - `test_load_cu8_scale_and_clip`: bytes `[0, 255, 127, 128]` load as
    `[-1+1j, -0.0039+0.0039j]` (±1e-3), with clip fraction 0.5. A capture
    with 1 % of its bytes at 0 or 255 reports `clip ≈ 0.01`.
  - `test_to_baseband_puts_tx_at_zero`: a tone at TX + 1000 Hz peaks at
    1000 ± 10 Hz after `to_baseband`.
  - `test_keyed_window_finds_burst`: 1 s noise, 3 s carrier, 1 s noise. The
    window starts within 1.0–1.1 s and ends within 3.9–4.0 s.
  - `test_silence_reports_no_signal`: noise only, so `keyed_window` is None.
  - `test_channel_busy`: noise plus an FM signal at +10 kHz, 25 dB up, is
    True. The same signal at +60 kHz is False.
  - `test_spectrum_rbw_and_floor`: a carrier 60 dB over noise gives
    `peak_near` within 2 dB of the injected level. `noise_floor_db` is within
    3 dB of the injected noise density for a 10 Hz RBW.

- [ ] **Step 2: Run them and see them fail**

Run: `python3 tools/test_hackrf_tx_ota_analyze.py`
Expected: `ModuleNotFoundError: hackrf_tx_ota_analyze`.

- [ ] **Step 3: Implement the Task 2 interfaces** in
      `tools/hackrf_tx_ota_analyze.py`. Use `scipy.signal.welch` (Hann,
      `nperseg = fs / rbw`) for `spectrum_db`, with `fftshift` to signed
      frequencies.

- [ ] **Step 4: Run the tests and see them pass**

Run: `python3 tools/test_hackrf_tx_ota_analyze.py`
Expected: `OK: 6 tests`.

- [ ] **Step 5: Commit**

```bash
git add tools/hackrf_tx_ota_analyze.py tools/test_hackrf_tx_ota_analyze.py
git commit -m "feat(tools): HackRF OTA TX test — cu8 capture loading and core spectrum measurements"
```

---

### Task 3: Per-mode checks

**Files:**
- Modify: `tools/hackrf_tx_ota_analyze.py`
- Test: `tools/test_hackrf_tx_ota_analyze.py`

**Interfaces:**
- Consumes: everything Task 2 produces.
- Produces:
  - `@dataclass Reference(carrier_offset_hz: float, fm_peak_dev_hz: float | None = None)`.
    `carrier_offset_hz` is the TUNE carrier minus nominal TX, in Hz.
  - `measure_tune(iq, spec, freq_tol_ppm: float) -> tuple[ModeResult, Reference]`
  - `analyze_mode(mode: str, iq: np.ndarray, spec: CaptureSpec, ref: Reference, tone_hz: float = 1000.0, cw_text: str | None = None, cw_wpm: float = 15.0) -> ModeResult`
  - `fm_demod(bb: np.ndarray, fs: float) -> np.ndarray`: instantaneous
    frequency in Hz, low-passed to 15 kHz.
  - `MORSE: dict[str, str]`: A–Z, 0–9 and `/`, as `.-` strings.
  - `morse_elements(text: str, wpm: float) -> list[tuple[bool, float]]`:
    (key-down, seconds) per element and gap, PARIS timing (dit = 1.2 / wpm).
  - `decode_cw(bb: np.ndarray, fs: float) -> tuple[str, float]`: the decoded
    text and the median dit length in seconds.
  - `demod_audio(mode: str, bb: np.ndarray, fs: float) -> np.ndarray`:
    48 kHz float audio for the WAV.

**`THRESHOLDS` keys and starting values** (copied from the spec's table):

| Key | Value |
|---|---|
| `tune_snr_db` | 55 (in a 10 Hz bin, ≈ 20 dB in 25 kHz) |
| `ssb_opposite_db` | 25 |
| `ssb_carrier_db` | 20 |
| `cw_offset_hz` | 50 |
| `cw_dit_tol` | 0.15 |
| `cw_width_hz` | 400 |
| `am_symmetry_db` | 2 |
| `am_depth_min` | 0.30 |
| `am_depth_max` | 1.00 |
| `am_harmonic_db` | 25 |
| `dsb_symmetry_db` | 2 |
| `dsb_carrier_db` | 20 |
| `fm_tone_tol` | 0.02 |
| `fm_dev_hz` | 5000 |
| `fmn_dev_hz` | 2500 |
| `dev_margin` | 0.10 |
| `fmn_ratio_min` | 0.4 |
| `fmn_ratio_max` | 0.6 |
| `spur_db` | 40 (report-only) |

**Mode groups** (all measured on the keyed segment, relative to
`ref.carrier_offset_hz`):

- USB, DIGU and RTTY use the USB checks.
- LSB and DIGL use the LSB checks.
- CW and CWR use the CW checks.
- AM and SAM use the AM checks.
- DSB uses the DSB checks.
- FM and DFM use the FM checks.
- FMN uses the FM checks with the FMN limit, plus the ratio to
  `ref.fm_peak_dev_hz` when it is set.

Every mode adds the report-only `spurious` check.

- [ ] **Step 1: Write the failing tests.** Each synthesises the correct signal
      and one deliberately wrong one at 50 dB SNR, both through `to_cu8`.
  - `test_tune_offset_and_ppm`: a carrier at TX + 9 kHz gives a Reference
    offset of 9000 ± 5 Hz and passes at 25 ppm. At TX + 30 kHz it fails the
    frequency check.
  - `test_usb_passes_lsb_fails_as_usb`: a tone at +1 kHz passes USB. The same
    capture analysed as `LSB` fails `opposite_sideband`.
  - `test_ssb_carrier_leak_fails`: +1 kHz tone plus a carrier 10 dB below it
    fails `carrier_suppression`.
  - `test_am_depth`: m = 0.7 measures between 0.65 and 0.75 and passes.
    m = 1.3 (over-modulated) fails `harmonics`.
  - `test_dsb_symmetry_and_carrier`: a clean DSB signal passes. Adding a
    carrier at −6 dB fails.
  - `test_fm_deviation`: 4 kHz deviation passes FM and fails FMN. 2 kHz passes
    FMN, and with `fm_peak_dev_hz = 4000` the ratio of 0.5 passes.
  - `test_cw_decode_and_timing`: `VVV DE W5TSU` keyed at 15 WPM, with 10 ms
    raised-cosine edges and ±5 ms random jitter, decodes exactly. The dit is
    within 15 % of 80 ms.
  - `test_cw_key_clicks_fail_width`: the same text with hard (0 ms) edges
    fails `width`.
  - `test_interferer_in_keyed_window_reports_not_fails`: a USB tone plus an
    FM interferer at +120 kHz, 10 dB below. USB passes, and the `spurious`
    check carries a note but `passed=True`.

- [ ] **Step 2: Run them and see them fail**

Run: `python3 tools/test_hackrf_tx_ota_analyze.py`
Expected: `AttributeError` / `NameError` on `measure_tune`.

- [ ] **Step 3: Implement the Task 3 interfaces.** Approaches the signatures
      leave open:
  - **AM depth:** `m = 2·10^((P_sb_avg − P_c)/20)`.
  - **FM peak deviation:** the 99.5th percentile of |`fm_demod` − its mean|.
  - **CW decoding:** threshold the envelope at the midpoint of the on and off
    levels, then classify each run. Key-down runs shorter than 2 × the median
    short run are dits. Key-up runs over 5 dits are word gaps, and those over
    2 dits are letter gaps.
  - **CW width:** −40 dB points around the carrier in a 10 Hz-RBW spectrum.

- [ ] **Step 4: Run the tests and see them pass**

Run: `python3 tools/test_hackrf_tx_ota_analyze.py`
Expected: `OK: 15 tests`.

- [ ] **Step 5: Commit**

```bash
git add tools/hackrf_tx_ota_analyze.py tools/test_hackrf_tx_ota_analyze.py
git commit -m "feat(tools): HackRF OTA TX test — per-mode sideband, AM, FM and CW checks"
```

---

### Task 4: Reports, plots, WAVs and re-analysis

**Files:**
- Modify: `tools/hackrf_tx_ota_analyze.py`
- Test: `tools/test_hackrf_tx_ota_analyze.py`

**Interfaces:**
- Consumes: Tasks 2 and 3.
- Produces:
  - `write_mode_artifacts(mode_dir: str, mode: str, bb: np.ndarray, fs: float, result: ModeResult) -> None`.
    Writes `result.json`, `audio.wav` (int16, 48 kHz, via
    `scipy.io.wavfile`) and `spectrum.png` (±25 kHz around TX; written only
    when matplotlib imports, otherwise a note goes into `result.json`).
  - `write_reports(out_dir: str, run_meta: dict, results: list[ModeResult]) -> None`.
    Writes `report.json`, and `report.md` with a table of
    `Mode | Status | Failed checks (measured vs limit)`. Below it go the run
    meta: frequency, ppm, power, RTL gain and the git commit.
  - `reanalyze(out_dir: str) -> list[ModeResult]`. Reads `run.json` (written
    by Task 5: `{"config": {...}, "modes": [{"mode", "capture", "spec"}, ...]}`),
    re-runs `measure_tune` and then `analyze_mode` per mode from the saved
    `.cu8` files, and rewrites the reports.
  - CLI: `python3 tools/hackrf_tx_ota_analyze.py --reanalyze <dir>` exits
    0, 1 or 2, the same as the orchestrator.

- [ ] **Step 1: Write the failing tests**
  - `test_reports_round_trip`: two synthetic ModeResults (one pass, one fail
    with a CheckResult) give a `report.json` that loads back to the same
    statuses. `report.md` contains `| USB | pass |` and the failed check's
    measured value.
  - `test_reanalyze_from_run_json`: a temp dir holds `run.json` plus TUNE and
    USB `.cu8` files synthesised as in Task 3. `reanalyze` returns
    `["pass", "pass"]` statuses and writes `report.md`.
  - `test_artifacts_without_matplotlib`: with `sys.modules["matplotlib"] = None`
    patched in, `write_mode_artifacts` writes `result.json` and `audio.wav`,
    not `spectrum.png`, and records a note.

- [ ] **Step 2: Run them and see them fail**

Run: `python3 tools/test_hackrf_tx_ota_analyze.py`
Expected: `NameError: write_reports`.

- [ ] **Step 3: Implement the Task 4 interfaces.**

- [ ] **Step 4: Run the tests and see them pass**

Run: `python3 tools/test_hackrf_tx_ota_analyze.py`
Expected: `OK: 18 tests`.

- [ ] **Step 5: Commit**

```bash
git add tools/hackrf_tx_ota_analyze.py tools/test_hackrf_tx_ota_analyze.py
git commit -m "feat(tools): HackRF OTA TX test — reports, WAVs, spectra and --reanalyze"
```

---

### Task 5: The orchestrator and its safety

**Files:**
- Create: `tools/hackrf_tx_ota_test.py`
- Test: `tools/test_hackrf_tx_ota_test.py`

**Interfaces:**
- Consumes:
  - `automation_probe.Bridge(sock_path).request(obj, timeout_seconds) -> dict`;
  - the analysis module's `CaptureSpec`, `load_cu8`, `to_baseband`,
    `channel_busy`, `measure_tune`, `analyze_mode`, `morse_elements`,
    `write_mode_artifacts`, `write_reports`;
  - the Task 1 findings: the socket rule and the field names.
- Produces:
  - **`parse_args(argv) -> Config`**, a dataclass holding `callsign`
    (required, uppercased), `freq_hz=915.5e6`, `max_power=80`,
    `start_power=10`, `tone_hz=1000`, `tone_db=<Task 1>`, `cw_wpm=15`,
    `rtl_gain=20.7`, `rtl_rate=2.048e6`, `rtl_offset_hz=-400e3`,
    `freq_tol_ppm=25`, `out_dir`, `app_path="build/AetherSDR"`, `attach=False`,
    `headless=False`, `modes` (default: all 14). Its errors:
    - `--max-power` above 80 is `SystemExit(2)`, with the message
      `--max-power above 80 would switch on the HackRF RF amp`;
    - a missing `--callsign` is argparse's own error.
  - **`class Radio`**, the bridge wrapper. Every keying method returns only
    after the bridge says `ok`.
    - Keying: `ptt(on: bool)`, `tune_carrier(on: bool)` (TCI
      `tune:0,<bool>;`), `cw_key(down: bool)` (TCI `keyer:0,<bool>;`).
    - Setup: `set_mode(mode)`, `set_freq_hz(hz)`, `set_power(rf: int, tune: int)`,
      `testtone(on: bool, hz=1000, db=-6)`.
    - State: `transmitting() -> bool`, `snapshot() -> dict`,
      `restore(snap: dict)`.
    - `all_stop()`: PTT off, `tune:0,false`, `keyer:0,false`, test tone off.
      It never raises.
    - `set_power` raises `PowerCapError` above `max_power`.
  - **`class RtlCapture`**: `preflight() -> str` (the device name, or raises
    `RtlError`) and `record(path, center_hz, rate, gain, seconds) -> Popen`.
    It uses `rtl_sdr -f <c> -s <r> -g <g> -n <N> <path>` and treats stderr
    containing `usb_claim_interface` or `No supported devices` as `RtlError`.
  - **`launch_app(cfg) -> tuple[Popen, str]`**: the process and its socket.
    The socket name is unique: `hackrf-tx-ota-<pid>-<time>`. The env sets
    `AETHER_AUTOMATION`, `ALLOW_TX`, `TX_MAX_POWER=<max_power>` and
    `TX_MAX_MS=15000`, plus `QT_QPA_PLATFORM=offscreen` when `--headless`.
  - **`run(cfg, radio, rtl, clock=time) -> int`**: the whole sequence; it
    returns the exit code.
  - **`main(argv) -> int`**.

**The sequence `run()` follows** (the spec's "How one mode is tested"):

1. Preflight the RTL-SDR.
2. Snapshot the radio, enable TCI and start its client, and set the frequency.
3. **Power ramp.** Use TUNE bursts of 1.5 s at 10, 20, 30, 40, 50, 60, 70 and
   80 % (capped), each one recorded and measured. Stop at the first level where
   the `snr` CheckResult from `measure_tune` reads ≥
   `THRESHOLDS["tune_snr_db"]`. If none reaches it, mark
   the run `weak signal` in the notes and use the cap.
   - If no keyed window is found even at the cap, every mode is reported
     `fail` with `no signal received`. Skip to step 6.
4. Run TUNE at the chosen level, which gives the Reference.
5. **Each remaining mode in order:**
   - USB, LSB, DIGU, DIGL, RTTY, CW, CWR, AM, SAM, DSB, FM, FMN, DFM.
   - FM must run before FMN (`ref.fm_peak_dev_hz`).
   - Steps: ID check → busy check → record → key → unkey → confirm → analyse.
   - On clipping, lower the gain one step and repeat that mode once.
6. CW ID.
7. Restore, then `all_stop()`. `main()` then quits the app if it launched it
   (bridge `quit`; terminate after 10 s).
8. Write `run.json` and the reports, and print the table.

Any bridge error or lost connection during keying is an abort (exit 2), after
`all_stop()`.

Every key, from step 3 onward, happens inside
`try/finally: radio.all_stop(); confirm_unkeyed()`.

- [ ] **Step 1: Write the failing tests.** Use a `FakeRadio` that records
      calls and returns scripted `transmitting()` values, a `FakeRtl` that
      writes synthetic `.cu8` files through the Task 3 synthesisers, and a fake
      clock.
  - `test_max_power_above_80_refused`: `--max-power 81` gives `SystemExit(2)`
    with the amp message. `set_power(81, 10)` with cap 80 raises
    `PowerCapError`.
  - `test_callsign_required`: no `--callsign` gives `SystemExit`.
  - `test_unkey_confirm_failure_aborts`: if `FakeRadio.transmitting()` stays
    True after an unkey, `run` returns 2. `all_stop` was called at least
    twice, and no later mode was keyed.
  - `test_sigint_during_burst_unkeys`: a `KeyboardInterrupt` raised from
    `FakeRtl.record` during the USB burst gives `all_stop` called. `run`
    returns 2 and the reports are still written.
  - `test_rtl_open_failure_aborts_before_keying`: if `FakeRtl.preflight`
    raises `RtlError`, `run` returns 2 with zero keying calls.
  - `test_power_ramp_stops_at_target`: synthetic SNR rises 10 dB per step
    from 25 dB. The ramp stops at 40 % and all later `set_power` calls use 40.
  - `test_run_with_no_signal_fails_cleanly`: `FakeRtl` writes noise only.
    `run` returns 1, every mode is `fail` with the note `no signal received`,
    and `report.json` is valid JSON.
  - `test_busy_channel_skips_after_five_rechecks`: if the busy check is always
    true, the mode is `skip` after 5 rechecks and is never keyed.
  - `test_id_sent_at_end_and_every_nine_minutes`: with a fake clock advancing
    2 minutes per mode, the CW ID is sent at least once mid-run and once at
    the end. `cw_key` edges follow `morse_elements("DE W5TSU", 15)`.
  - `test_launch_uses_unique_socket`: two `launch_app` env builds (Popen
    mocked) give different `AETHER_AUTOMATION_SOCKET` values, with
    `TX_MAX_POWER=80` and `TX_MAX_MS=15000`.
  - `test_restore_after_run`: the final calls restore the snapshot's mode,
    frequency, RF and Tune Power and TCI state, then `all_stop`.

- [ ] **Step 2: Run them and see them fail**

Run: `python3 tools/test_hackrf_tx_ota_test.py`
Expected: `ModuleNotFoundError: hackrf_tx_ota_test`.

- [ ] **Step 3: Implement the Task 5 interfaces.** Notes:
  - CW keying schedules each edge against `clock.monotonic()` targets, so
    bridge latency doesn't accumulate.
  - Signal handlers call `all_stop()` and then raise `KeyboardInterrupt`.
  - `atexit` registers `all_stop` too.

- [ ] **Step 4: Run the tests and see them pass**

Run: `python3 tools/test_hackrf_tx_ota_test.py`
Expected: `OK: 11 tests`.

- [ ] **Step 5: Commit**

```bash
git add tools/hackrf_tx_ota_test.py tools/test_hackrf_tx_ota_test.py
git commit -m "feat(tools): HackRF OTA TX test — orchestrator with power cap, unkey guarantees and CW ID"
```

---

### Task 6: Register the tests, write the guide, update the roadmap

**Files:**
- Modify: `tests/tests.cmake` (next to `tx_meter_safety`, around line 3529)
- Modify: both test scripts (the skip exit code)
- Create: `docs/HACKRF-TX-TEST.md`
- Modify: `docs/HACKRF.md` (a "Testing transmit" pointer section)
- Modify: `ROADMAP.md` (the "HackRF — on-air check of the new transmit modes"
  item)

- [ ] **Step 1: Add skip handling.** Both test scripts exit **77** with
      `SKIP: numpy/scipy not installed` when either import fails. Register the
      tests:

```cmake
add_test(NAME hackrf_tx_ota_analyze
         COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_hackrf_tx_ota_analyze.py)
set_tests_properties(hackrf_tx_ota_analyze PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME hackrf_tx_ota_safety
         COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_hackrf_tx_ota_test.py)
set_tests_properties(hackrf_tx_ota_safety PROPERTIES SKIP_RETURN_CODE 77)
```

- [ ] **Step 2: Verify through ctest**

Run: `cmake -S . -B build >/dev/null && ctest --test-dir build -R 'hackrf_tx_ota|tx_meter_safety|check_test_registration' --output-on-failure`
Expected: all pass (`tools/check_test_registration.py` must accept the new
entries).

- [ ] **Step 3: Write `docs/HACKRF-TX-TEST.md`.** The sections, in this order:
  1. What it tests and what it doesn't (the spec's goals and non-goals, in
     plain words).
  2. What you need: a HackRF One or Pro, any R820T/R820T2 RTL-SDR, antennas,
     `rtl-sdr`, Python 3 with numpy and scipy (matplotlib optional), and a
     built AetherSDR.
  3. Licence and identification: 33 cm amateur band, Region 2. Give the US
     Part 97 ID rule, and say that outside Region 2 you pick your own
     frequency.
  4. Setup: antennas a short distance apart, close other SDR programs, then
     run `rtl_test -t` and `hackrf_info`.
  5. Running it: the command, every option and the defaults, what you'll see,
     and Ctrl+C.
  6. Safety: the 80 % cap and why it keeps the amp off. There is no power
     meter, so stay at the bench (`docs/automation/TX_TEST_PROMPT.md`).
  7. Reading the results: the folder layout, `report.md`, the statuses, each
     check in a table, listening to `audio.wav`, and `--reanalyze`.
  8. Listening live: `rtl_fm -f 915.5e6 -M usb -s 48k - | aplay -r 48000 -f S16_LE`
     (and `-M fm`, `-M am`) while running modes by hand.
  9. Troubleshooting: dongle busy, channel busy (skips), weak signal, no
     signal, clipping, PPM out of tolerance, and the HackRF disappearing
     (`hackrf_info`, RESET).
  10. Tuning the thresholds: where `THRESHOLDS` lives, and running
      `--reanalyze` after changing it.

- [ ] **Step 4: Link and update.** In `docs/HACKRF.md`, add a section
      `## Testing transmit` with one paragraph linking
      `HACKRF-TX-TEST.md`. In `ROADMAP.md`, rewrite the on-air check item to
      name the test and keep the 10-13 history. The result goes in after
      Task 7.

- [ ] **Step 5: Commit**

```bash
git add tests/tests.cmake tools/test_hackrf_tx_ota_analyze.py tools/test_hackrf_tx_ota_test.py docs/HACKRF-TX-TEST.md docs/HACKRF.md ROADMAP.md
git commit -m "docs(hackrf): over-the-air TX test guide; register its offline tests"
```

---

### Task 7: Full live run and calibration

**Files:**
- Modify: `tools/hackrf_tx_ota_analyze.py` (`THRESHOLDS` only, if needed)
- Modify: `docs/HACKRF-TX-TEST.md` (an example report section),
  `ROADMAP.md` (the result)

- [ ] **Step 1: Run it**

Run: `python3 tools/hackrf_tx_ota_test.py --callsign W5TSU`
Expected: a full run with Mark at the bench, exit code 0 or 1,
`report.md` written, and `hackrf_info` working afterwards.

- [ ] **Step 2: Triage each failure.** For each one, decide which it is:
  - **A measurement artefact:** the threshold is unrealistic for an RTL-SDR
    over the air. Adjust `THRESHOLDS`, rerun `--reanalyze`, and note why in
    a comment next to the value.
  - **A real transmit defect:** leave the threshold and record it as a
    finding for Mark.

  Never loosen a threshold so far that the deliberately wrong synthetic cases
  in Task 3 would pass. Rerun `python3 tools/test_hackrf_tx_ota_analyze.py`
  after any change.

- [ ] **Step 3: Run it again** to confirm the result repeats. Expected: the
      same statuses.

- [ ] **Step 4: Record the results.** Paste the second run's `report.md`
      table into the guide's example section. Update the ROADMAP item with the
      date, frequency, power level used and the per-mode outcome.

- [ ] **Step 5: Commit**

```bash
git add tools/hackrf_tx_ota_analyze.py docs/HACKRF-TX-TEST.md ROADMAP.md
git commit -m "feat(hackrf): first over-the-air TX test run — calibrated thresholds and results"
```
