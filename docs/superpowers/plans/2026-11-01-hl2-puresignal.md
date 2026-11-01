# HL2 PureSignal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A one-button PureSignal on the Hermes-Lite 2 that measurably lowers intermodulation through the XPA125B and can be watched while it works.

**Architecture:** WDSP's `calcc` measures and its `iqc` corrects, but the HL2's modulator (`Hl2TxDsp`) is not a WDSP TXA chain. So a stopped TXA channel exists only to own `calcc` and `iqc`, and a new WDSP call runs `iqc` on the IQ `MetisClient` is about to send. A pure controller (`Hl2PsController`) decides everything; `Hl2PureSignal` is the only code that touches WDSP.

**Tech Stack:** C++20, Qt6, vendored WDSP 2.10, openHPSDR Protocol 1.

**Spec:** `docs/superpowers/specs/2026-10-31-hl2-puresignal-design.md`. This plan supersedes its step 3 where the spec assumed WDSP's own transmit chain, and it adds the `ps` bridge verb.

## Global Constraints

- PRs only to `W5TSU/AetherSDR`, and only when the operator asks. Commits are SSH-signed.
- WDSP changes are numbered patches recorded in `third_party/wdsp/AETHERSDR-PATCHES.md`.
- `Hl2Receivers.h`: never derive one receiver index from another.
- PS never keys the transmitter.
- If feedback is absent, clipped or implausible, correction stops and the signal goes out uncorrected.
- Corrected IQ magnitude never exceeds 1.0 (wire full scale). `IQC_OUT_MAX` is already 0.999; the corrector asserts it again.
- With PS off, the wire bytes are exactly today's.
- Live transmit: operator at the bench, RF and Tune power at most 80%, CW ID W5TSU, unkey on every exit path, automation launched with `AETHER_AUTOMATION_ALLOW_TX=1`, `AETHER_AUTOMATION_TX_MAX_POWER=80`, `AETHER_AUTOMATION_TX_MAX_MS=15000`.
- The HackRF is never wired to the amplifier output. On hermes-pc, never toggle COM1 or COM2.

## Facts this plan rests on (read from source, 2026-11-01)

| Fact | Source |
|---|---|
| `0x0a` bit 22 enables PureSignal (C2 bit 6) | `Hermes-Lite2/gateware/rtl/radio_openhpsdr1/radio.v:280` |
| During `tx_on & pure_signal`, the second mixer takes the DAC instead of the ADC. It feeds DDC 1 and DDC 3 | `radio.v:510-524` |
| DDC 2 stays on the ADC (feedback). DDC 3 is the TX reference. 4 receivers needed | `radio.v:479-485`; `deskhpsdr/src/old_protocol.c:1344-1395,1527-1542` |
| Both feedback NCOs sit on the TX frequency | `old_protocol.c:1465` |
| Protocol 1 feedback rate is the receiver sample rate | `deskhpsdr/src/radio.c:1325` |
| HL2 HW peak measured 0.2386, set 0.2400 | `deskhpsdr/src/transmitter.c:1370-1372` |
| Delays: TX 150 ns, MOX 0.2 s, loop 0 | `transmitter.c:1186-1190` |
| Auto level: target 152.293; act outside 150..155; >275 or <25 step 15 dB; else `20*log10(level/152.293)`; reset then resume after a change | `deskhpsdr/src/ps_menu.c:582-625` |
| Reset takes effect only while samples reach `pscc` | `transmitter.c:2486-2530` |
| `SetTXAiqcStart/End` spin until `xiqc` runs, so `xiqc` must keep running | `third_party/wdsp/upstream/iqc.c:253-295` |
| `info`: 0-3 fit flags, 4 feedback level, 5 successes, 6 sanity flags, 7 attempts, 14 correcting, 15 state | `third_party/wdsp/upstream/calcc.c` |

## Review Focus

1. Sampler cable pulled while correcting: correction ends within one calibration cycle, no burst. Test in Task 4.
2. Operator turns PS on with three slices open, or at a rate where 4 receivers cannot be delivered: refused with a reason, no slice lost. Test in Task 5.
3. Link drops or the app quits while correction is ramping: no hang on the correction thread's spin. Test in Task 3.
4. Mode or rate change, reconnect, band change while PS is on: the layout, the feedback rate and the stored correction all follow. Test in Tasks 5 and 6.
5. CW and single-carrier TUNE: uncorrected, and not fed to calibration. Test in Task 3.

---

### Task 1: WDSP runs the correction on caller-supplied IQ (patch 23)

**Files:**
- Modify: `third_party/wdsp/upstream/iqc.c`, `iqc.h`
- Modify: `third_party/wdsp/AETHERSDR-PATCHES.md`
- Test: `tests/wdsp_ps_apply_test.cpp`, `tests/tests.cmake`

**Interfaces:**
- Produces: `PORT void PSApplyCorrection(int channel, int size, double* in, double* out)`. Under `ch[channel].csDSP`, saves `in`, `out`, `size` of `txa[channel].iqc.p`, runs `xiqc`, restores them. With the stage not running, `out` equals `in`.
- Produces: `PORT int PSCorrectionBusy(int channel)`, the stage's `busy` bit.

- [ ] **Step 1:** Write failing test `wdsp_ps_apply_test`. Open a stopped TX channel at 48 kHz. Assert: with no correction loaded, `PSApplyCorrection` copies 1024 samples unchanged, both with `in == out` and with separate buffers; the stored buffers and size are restored afterwards, shown by a second call with a different size.
- [ ] **Step 2:** Run `ctest -R wdsp_ps_apply_test`. Expected: link failure on `PSApplyCorrection`.
- [ ] **Step 3:** Implement both calls in `iqc.c` with an `// AetherSDR patch 23:` marker. Record it in the patch ledger and raise its count.
- [ ] **Step 4:** Run the test and `wdsp_calcc_teardown_test`. Expected: both pass.
- [ ] **Step 5:** Commit.

### Task 2: The wire layout for PureSignal

**Files:**
- Modify: `src/core/backends/hl2/MetisProtocol.h/.cpp`, `MetisClient.h/.cpp`
- Test: `tests/hl2_ps_wire_test.cpp`, `tests/tests.cmake`

**Interfaces:**
- Produces: `Cc ccRxGain(int db, bool pureSignal = false) noexcept`, setting C2 bit 6.
- Produces: `MetisClient::setPureSignalEnabled(bool)` (Q_INVOKABLE), queued as a one-shot and re-asserted in the round robin; `Params::pureSignal`.
- Produces: `MetisClient::setTxIqCorrector(ITxIqCorrector*)`, with `struct ITxIqCorrector { virtual void correct(std::span<std::complex<float>> block, bool applyToOutput) noexcept = 0; };` declared in `MetisClient.h`.
- Produces: `MetisClient::setTxTwoTone(double f1Hz, double f2Hz, double amplitudeEach)` (Q_INVOKABLE). Two tones through the existing test-tone branch, phases carried across packets. Cleared by `setTxTestTone(0, 0)` and `flushTxIq()`.

- [ ] **Step 1:** Write failing tests:
  - `ccRxGain(10, true)` has C2 `0x40`; `ccRxGain(10)` equals today's bytes.
  - Default rotation carries no bit 22; `setPureSignalEnabled(true)` queues one one-shot and the rotation carries it.
  - The corrector is called on every built EP2 block, with `applyToOutput` true for queued IQ and two-tone, false for silence and the single test tone; its output reaches the wire only when true.
  - Two-tone at 700 and 1900 Hz with 0.45 each: an FFT of the wire samples shows both tones within 0.5 dB of each other, nothing else above -60 dBc, and peak magnitude at most 0.9.
- [ ] **Step 2:** Run. Expected: compile failure.
- [ ] **Step 3:** Implement. The corrector call sits just before `ep2WriteTxIq` in all three branches of `buildNextControlPacket`.
- [ ] **Step 4:** Run the new test plus `ctest -R hl2`. Expected: pass.
- [ ] **Step 5:** Commit.

### Task 3: `Hl2PureSignal`, the WDSP boundary

**Files:**
- Create: `src/core/backends/hl2/Hl2PureSignal.h/.cpp`
- Modify: the `aethercore` source list
- Test: `tests/hl2_puresignal_loop_test.cpp`, `tests/tests.cmake`

**Interfaces:**
- Consumes: Task 1's calls, Task 2's `ITxIqCorrector`.
- Produces: `class Hl2PureSignal : public ITxIqCorrector`, used only on the I/O thread:
  - `bool open(int feedbackRateHz, std::string* error)`, `void close()`. Reserves a channel the way `WdspChannel` does and opens it stopped at 48 kHz. Sets HW peak 0.2400, TX delay 150 ns, MOX delay 0.2 s, loop delay 0. `close()` resets, then runs `PSApplyCorrection` on silence until `PSCorrectionBusy` clears (2 s limit) before closing.
  - `void setMox(bool modulated)`, `SetPSMox`. True only for voice, data and two-tone keying.
  - `void feed(std::span<const std::complex<float>> feedback, std::span<const std::complex<float>> reference)`. Buffers to 1024 pairs, then calls `pscc`. Does nothing unless `setMox(true)` or a reset is draining.
  - `void start()` (automode), `void stop()`, `void setFeedbackRate(int)`.
  - `Hl2PsSnapshot snapshot() const`: `info[16]`, `maxTx`, `hwPeak`, `feedbackPeakDbfs`, `referencePeakDbfs`.
  - `void saveCorrection(const QString&)`, `void restoreCorrection(const QString&)`.

- [ ] **Step 1:** Write failing test `hl2_puresignal_loop_test`, the plan's main proof with no radio. It builds a software amplifier, `out = tanh(1.5*|x|)/1.5 * x/|x|` plus 5° of AM-PM at peak. The reference is `corrected * 0.24`, the feedback is the amplifier output scaled to the working range, and the source is a 700/1900 Hz two-tone. Assert:
  - Before correction, IMD3 is measured as the baseline.
  - After `start()`, `info[14]` becomes 1 and `info[5]` grows, within a 30 s wall limit.
  - IMD3 with correction is at least 15 dB below baseline, and no IMD3 or IMD5 product rises.
  - Peak corrected magnitude never exceeds 1.0.
  - Feedback set to zero mid-run with `stop()`: output equals input within the ramp, with no sample above the previous peak.
  - `setMox(false)` with feeds continuing: `info[5]` stops growing and output is still valid.
  - `close()` called during a ramp returns within 2 s.
  - Save, reset, restore: `info[14]` returns to 1 without any new calibration, and IMD3 still beats the baseline by 15 dB.
- [ ] **Step 2:** Run. Expected: compile failure.
- [ ] **Step 3:** Implement. Float to double conversion uses preallocated buffers, with no allocation in `correct()` or `feed()`.
- [ ] **Step 4:** Run. If the correction falls short of 15 dB, stop and report the measured number; do not lower the threshold silently.
- [ ] **Step 5:** Run under TSan if available (`dax_iq_rate_limits_test` is a known unrelated timeout, #104).
- [ ] **Step 6:** Commit.

### Task 4: `Hl2PsController`, every decision in one testable place

**Files:**
- Create: `src/core/backends/hl2/Hl2PsController.h/.cpp`
- Test: `tests/hl2_ps_controller_test.cpp`, `tests/tests.cmake`

**Interfaces:**
- Consumes: `Hl2PsSnapshot` (Task 3).
- Produces: `enum class Hl2PsState { Off, Idle, Calibrating, Correcting, FeedbackWeak, FeedbackStrong }`; `QString hl2PsStateName(Hl2PsState)` giving `off`, `idle`, `calibrating`, `correcting`, `feedback too weak`, `feedback too strong`.
- Produces: `struct Hl2PsActions { std::optional<int> newGainDb; bool reset = false; bool resume = false; };` and `Hl2PsActions Hl2PsController::tick(const Hl2PsSnapshot&, bool keyedModulated)`, called every 100 ms. Plus `setEnabled(bool)`, `setGainDb(int)`, `state()`, `gainDb()`.

Values: gain range -12..+48 dB, starting at -12 so an unknown PURE level cannot hit the converter hard. Level target 152.293, dead band 150..155. Gain moves opposite to the attenuation in `ps_menu.c`: `newGain = gain - 20*log10(level/152.293)`, or ∓15 when above 275 or below 25, only after a new calibration (`info[5]` or `info[7]` advanced).

- [ ] **Step 1:** Write failing tests:
  - Level 200 at gain 0 lowers gain by `round(20*log10(200/152.293))` and returns reset, then resume on the next tick.
  - Level 152 changes nothing.
  - Level 300 steps -15; level 10 steps +15; both clamp at the limits; a clamped gain returns no reset.
  - No gain change without a new calibration.
  - Correcting, then level below 25 or fit flags set, or `feedbackPeakDbfs` 40 dB or more below `referencePeakDbfs`: reset immediately, state `feedback too weak`.
  - `feedbackPeakDbfs` above -1 dBFS: gain steps -15 and reset; at minimum gain, state `feedback too strong` and reset.
  - Twenty attempts without success while keyed: state follows the level (weak or strong), correction stays off.
  - Unkeyed tick changes nothing and returns `Idle` or holds `Correcting`.
  - Fault clears on the next keying.
  - `setEnabled(false)` from any state returns reset and reaches `Off`.
- [ ] **Step 2:** Run. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run. Expected: pass.
- [ ] **Step 5:** Commit.

### Task 5: The backend: layout, slices, two-tone, `ps` verb

**Files:**
- Modify: `src/core/backends/hl2/Hl2Backend.h/.cpp`, `Hl2MiscOptionsSettings.h/.cpp`
- Modify: `src/core/backends/RadioCapabilities.h`, other backends set the new field explicitly
- Modify: `src/core/AutomationServer.h/.cpp`, regenerate `docs/automation-bridge.md`
- Test: `tests/hl2_ps_backend_test.cpp`, `tests/automation_ps_verb_test.cpp`, `tests/tests.cmake`

**Interfaces:**
- Consumes: Tasks 2, 3 and 4.
- Produces: `RadioCapabilities::hasPureSignal` (true on HL2 with transmit allowed).
- Produces: extension verbs `hl2/ps.set` (bool) and `hl2/ps.status`, and health rows `psState`, `psFeedbackLevel`, `psCalibrations`, `psAttempts`, `psLastFailure`, `psMaxTx`, `psHwPeak`, `psFeedbackPeakDbfs`, `psReferencePeakDbfs`, `psGainDb`.
- Produces: bridge verb `ps [status|on|off]`, answering from a backend snapshot, never echoing the request.
- Produces: `int Hl2Backend::wireReceiverCount() const`: 4 with PS on, else `m_rx.size()`. Every `setReceiverCount` call site uses it.
- Produces: `signal IRadioBackend::pureSignalStatusChanged(QString state, int feedbackLevel)`, or the existing seam if one fits.
- Two-tone: `twoToneGenerator = {"hl2/twotone"}`. `transmit set tune_mode=two_tone` selects it; `setTune(true, …)` then calls `setTxTwoTone(700, 1900, 0.45)` at tune power, through the same gates as TUNE.

Behaviour:
- PS on: refused with a stated reason if more than 2 slices are open, if keyed, or if `maxReceiversAtRate(rate, board) < 4`. `maxSlices` becomes 2.
- PS off while keyed: correction stops now; the layout returns to normal at unkey.
- DDC 2 and 3 follow every TX frequency change. DDC 1 with no slice stays on the last tuned frequency, delivered nowhere.
- `iqBlocksReady` taps DDC 2 and 3 into `feed()`; the `m_ioDsps` fan-out is untouched and already clamps.
- The TX gain from Task 4 goes through the existing `setTxLnaGainDb`; `txlnagain` refuses to set while PS is on.

- [ ] **Step 1:** Write failing tests:
  - Socket-free backend: `ps.set true` with 3 slices errors and leaves 3 slices; at 384 kHz errors with a rate message; capability `maxSlices` is 2 after success, and returns afterward.
  - `wireReceiverCount` is 4 with one or two slices open.
  - Status reads back from the backend.
  - `hasPureSignal` false on every other backend.
  - Two-tone: `txtest twotone` no longer refuses on HL2 and is still gated without TX permission.
  - `ps on`, `ps off`, `ps status`, unknown action is an error, no-backend is refused.
  - With PS off, the rotation bytes equal a captured baseline from before this change.
- [ ] **Step 2:** Run. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run with `ctest -R "hl2|automation_ps"`. Regenerate the bridge docs and run the doc tool's check. Expected: pass, verb count 79.
- [ ] **Step 5:** Commit.

### Task 6: Persistence

**Files:**
- Modify: `src/core/backends/hl2/Hl2Backend.cpp`, `Hl2MiscOptionsSettings.h/.cpp`
- Test: extend `tests/hl2_ps_backend_test.cpp`

**Interfaces:**
- Produces: setting `pureSignalEnabled` (global); per-band files `<radio settings dir>/ps/<band>.corr` and a per-band starting gain.

- [ ] **Step 1:** Write failing tests: on/off survives a reconstruct under `TestSettingsProfile`; a band change while correcting saves the old band and restores the new one if present, or resets if not; a restored band starts from its stored gain; a corrupt or truncated file gives uncorrected output and a logged warning. PS on at startup never keys.
- [ ] **Step 2:** Run. Expected: fail.
- [ ] **Step 3:** Implement, saving also on disconnect.
- [ ] **Step 4:** Run. Expected: pass.
- [ ] **Step 5:** Commit.

### Task 7: The PS button and indicator

**Files:**
- Modify: `src/gui/TxApplet.h/.cpp`, `src/models/RadioModel.h/.cpp`
- Test: `tests/tx_applet_ps_button_test.cpp`, `tests/tests.cmake`

**Interfaces:**
- Consumes: `hasPureSignal`, `hl2/ps.set`, the status signal.
- Produces: `m_psBtn` in the TUNE/MOX row, checkable, styled through `ThemeManager::applyStyleSheet`, with accessible name "PureSignal". It is not marked `markTxKeying`, because it does not key. Beneath it, a one-line status label: state name and feedback level, tinted green for 128-181, amber for 90-128 and above 181, red otherwise.

- [ ] **Step 1:** Write failing tests: hidden until the capability is true, and hidden again on disconnect; a click sends `ps.set`; a refusal unchecks the button and shows the reason; the checked state reflects the backend's reported state, not the click; the label tracks each state; a click never changes keying state.
- [ ] **Step 2:** Run. Expected: fail.
- [ ] **Step 3:** Implement. Run the setStyleSheet ratchet and the touchpoint manifest checks that caught #105.
- [ ] **Step 4:** Run. Expected: pass.
- [ ] **Step 5:** Commit, then update `docs/HERMES.md`, `ROADMAP.md` and `CHANGELOG.md`'s unreleased section, and the spec's status.

### Task 8: Bench verification

Stop before the first transmission of each stage and confirm the wiring and that the operator is present. Scripts live in `tools/bench/ps/`; every one unkeys in a `finally`, ends with a CW ID, and records to `bench-results/` (untracked).

- [ ] **Stage A, HL2 alone into the dummy load, no amplifier.**
  - Run the outstanding `0x0e` check: TUNE at low power with `txlnagain off`, `-12`, `+20`. Pass: `rx1IqPeakDbfs` differs by about 32 dB between -12 and +20.
  - Two-tone, `ps status` with PS on but no sampler. Record `psMaxTx` (expected near 0.24) and the feedback level. This shows whether internal leakage gives any feedback; a `feedback too weak` result is also a pass here.
- [ ] **Stage B, sampler on PURE, amplifier at lowest output into a rated dummy load.**
  - Read `psFeedbackPeakDbfs` at -12 dB gain, confirm it is far below clipping, then step power while watching it. This answers the undocumented PURE attenuator; record the number in `docs/HERMES.md`.
- [ ] **Stage C, two-tone through the amplifier, moderate power.** The HackRF Pro on this machine records by pickup near the dummy load (`hackrf_transfer`, receive only), first at lowest power to check its level. Compare PS off, then on. Pass: third- and fifth-order products lower, none higher, and PS reaches `correcting`.
- [ ] **Stage D, recorded speech,** the same way.
- [ ] **Stage E, pull the sampler cable while correcting.** Pass: `feedback too weak` within one calibration cycle, HackRF shows no burst, signal continues uncorrected.
- [ ] **Stage F, hermes-pc.** Run the Windows installer build from the branch with PS on. Pass: the button appears, the steps above marked A and C work, exit is clean. Do not touch COM1 or COM2; use `AETHER_SETTINGS_DIR` isolation and check no TCI client is connected before keying.
- [ ] **Step:** Record the measured numbers in `docs/HERMES.md` and the spec, then commit.
