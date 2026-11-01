# HL2 transmit LNA gain (register 0x0e) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the Hermes-Lite 2 switch its receive LNA to a separate gain while transmitting (register `0x0e`), and give the bench a raw receive-level reading to prove it. This is step 1 of the PureSignal design (backlog #22), plus the small instrument from its step 2.

**Architecture:** The 0x0e control bank is built by a new encoder in `MetisProtocol`, held live in `MetisClient` (replacing today's constant all-zero bank in the round robin), and set from `Hl2Backend` through the existing `hl2` extension namespace. An automation verb `txlnagain` drives it for the bench. A raw RX1 IQ peak per telemetry window rides `Hl2Telemetry` into Radio Health.

**Tech Stack:** C++20, Qt 6, the fork's plain `check()` test executables registered in `tests/tests.cmake`.

**Spec:** `docs/superpowers/specs/2026-10-31-hl2-puresignal-design.md` (step 1, and the instrument from step 2).

## Global Constraints

- Gateware semantics (softerhardware/Hermes-Lite2 `gateware/rtl/ad9866.v`, commit 7de0b57): on address `0x0e`, `cmd_data[15]` enables the transmit gain; with `cmd_data[14]` set, `cmd_data[13:8]` is the six-bit gain, the same native code as `0x0a`'s `cmd_data[5:0]`. Bytes: C1 = `cmd_data[31:24]`, C2 = `[23:16]`, C3 = `[15:8]`, C4 = `[7:0]`.
- Gain range and code: identical to `ccRxGain` — clamp to `[kLnaGainMinDb, kLnaGainMaxDb]` = `[-12, +48]`, code = dB − (−12).
- With no transmit gain set, the 0x0e bank is **byte-identical to today** (`{0x1C, 0, 0, 0, 0}`), so non-HL2 Protocol-1 devices (hpsdrsim) and the HL2 behave exactly as now. In the gateware that also keeps its existing behaviour of forcing minimum gain during CW.
- The transmit gain is session state only: not persisted. PureSignal (spec step 4) will own it later.
- Setting the gain never keys anything and is not TX-gated.
- Commits are SSH-signed (`git commit -S`) and end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Nothing is pushed.
- Bench work follows the spec's bench rules: operator present, RF and Tune power ≤ 80%, CW ID W5TSU at the end, unkey on every exit path, `AETHER_AUTOMATION_ALLOW_TX=1`, `AETHER_AUTOMATION_TX_MAX_POWER=80`, `AETHER_AUTOMATION_TX_MAX_MS=15000`. HL2 alone into the dummy load; no amplifier in this plan.

## Review Focus

1. **Reconnect after a gain was set** — the radio must get the gain back on the next session without the operator touching anything (round robin re-asserts it; `start()` seeds it from Params). Test in Task 2.
2. **Out-of-range dB** (e.g. 100, −50) — clamped to +48 / −12, never wraps into other bits. Test in Task 1.
3. **"off" after a value** — the bank goes back to exactly `{0x1C,0,0,0,0}`, not "bit 15 clear but gain bits left behind". Tests in Tasks 1 and 2.
4. **A window with no RX1 IQ** — the raw peak reading is absent, not 0 dBFS or −inf. Test in Task 3.
5. **The verb on a non-HL2 radio** — refused with a message, not an empty success. Covered by the extension-namespace check in Task 4 (same guard as `bandscope`).

---

### Task 1: The 0x0e encoder and the raw IQ peak window

**Files:**
- Modify: `src/core/backends/hl2/MetisProtocol.h` (around `ccAdcAssign`, ~line 407; `Hl2Telemetry` ~line 727; beside `ForwardPowerWindow`)
- Modify: `src/core/backends/hl2/MetisProtocol.cpp` (`ccAdcAssign`, ~line 175)
- Test: `tests/hl2_metis_protocol_test.cpp`

**Interfaces:**
- Produces: `Cc ccAdcAssignOrTxGain(std::optional<int> txGainDb) noexcept;` (replaces `ccAdcAssign()`; its one caller moves in Task 2)
- Produces: `struct RxIqPeakWindow { std::optional<float> peakMag; void observe(std::span<const std::complex<float>> iq) noexcept; void clear() noexcept; [[nodiscard]] std::optional<double> peakDbfs() const noexcept; };`
- Produces: `Hl2Telemetry::rx1IqPeakDbfs` (`std::optional<double>`)

- [ ] **Step 1: Write the failing tests** in `hl2_metis_protocol_test.cpp`, next to the existing `ccRxGain` checks:

```cpp
// 0x0e: generic ADC assignment when unset, HL2 transmit LNA gain when set.
check(ccAdcAssignOrTxGain(std::nullopt) == Cc{kC0AdcAssignOrTxGain, 0, 0, 0, 0},
      "0x0e unset: all zero, byte-identical to the old ccAdcAssign()");
check(ccAdcAssignOrTxGain(-12) == Cc{kC0AdcAssignOrTxGain, 0, 0, 0xC0, 0},
      "0x0e at -12 dB: C3 = enable | native | code 0");
check(ccAdcAssignOrTxGain(0) == Cc{kC0AdcAssignOrTxGain, 0, 0, 0xCC, 0},
      "0x0e at 0 dB: code 12");
check(ccAdcAssignOrTxGain(48) == Cc{kC0AdcAssignOrTxGain, 0, 0, 0xFC, 0},
      "0x0e at +48 dB: code 60");
check(ccAdcAssignOrTxGain(100) == ccAdcAssignOrTxGain(48)
          && ccAdcAssignOrTxGain(-50) == ccAdcAssignOrTxGain(-12),
      "0x0e: out-of-range dB clamps, never spills into C2 or C4");
// Same code as the receive gain, for the same dB.
check((ccAdcAssignOrTxGain(20)[3] & 0x3F) == (ccRxGain(20)[4] & 0x3F),
      "0x0e gain code matches 0x0a's for the same dB");

// Raw IQ peak window.
{
    RxIqPeakWindow w;
    check(!w.peakDbfs().has_value(), "iq peak: empty window has none");
    const std::vector<std::complex<float>> a{{0.1f, 0.0f}, {0.0f, -0.5f}, {0.3f, 0.4f}};
    w.observe(a);
    check(std::abs(*w.peakDbfs() - 20.0 * std::log10(0.5)) < 1e-6,
          "iq peak: largest |I+jQ| in dBFS (0.5 -> -6.02 dB)");
    w.clear();
    check(!w.peakDbfs().has_value(), "iq peak: clear() leaves none");
}
```

(Use the file's existing way of comparing `Cc` values; if `Cc` has no `operator==`, compare element by element as the neighbouring `ccRxGain` checks do.)

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --target hl2_metis_protocol_test 2>&1 | grep error: | head -3`
Expected: errors naming `ccAdcAssignOrTxGain` and `RxIqPeakWindow`.

- [ ] **Step 3: Implement**
  - `ccAdcAssignOrTxGain`: `nullopt` → `{kC0AdcAssignOrTxGain, 0, 0, 0, 0}`; a value → C3 = `0x80 | 0x40 | code`, code computed exactly as `ccRxGain` does (reuse its clamp helper). Delete `ccAdcAssign()`. Rewrite the header comment on `kC0AdcAssignOrTxGain` and the function: cite `ad9866.v` @ 7de0b57 for bits 15/14/13:8, and say the unset value keeps the generic all-zero assignment.
  - `RxIqPeakWindow` beside `ForwardPowerWindow`; `peakDbfs()` is `20·log10(peakMag)`; samples are already normalised to full scale by `ep6SamplesMulti`.
  - `Hl2Telemetry::rx1IqPeakDbfs` with a comment: raw RX1 IQ before any DSP, so it reads during transmit when the receive chain is fed zeros; filled by `MetisClient` at each emit.

- [ ] **Step 4: Run it to verify it passes**

Run: `cmake --build build --target hl2_metis_protocol_test && ./build/hl2_metis_protocol_test | tail -1`
Expected: `hl2_metis_protocol_test: all checks passed`

- [ ] **Step 5: Commit** — `feat(hl2): encode a transmit LNA gain on 0x0e and a raw IQ peak window. Principle VIII.`

---

### Task 2: MetisClient carries the 0x0e bank live

**Files:**
- Modify: `src/core/backends/hl2/MetisClient.h` (`Params` ~line 56; setters ~line 243; members near `m_ccTxDrive` ~line 862)
- Modify: `src/core/backends/hl2/MetisClient.cpp` (`start` ~line 116 and ~295; `buildNextControlPacket` ~line 1459; the telemetry emit ~line 1815; IQ decode ~line 1835; the `kRequestableAddresses` comment ~line 1274)
- Create: `tests/hl2_tx_lna_gain_frame_test.cpp`
- Modify: `tests/tests.cmake` (register beside `hl2_band_filter_frame_test`, same sources and link libraries)

**Interfaces:**
- Consumes: `ccAdcAssignOrTxGain`, `RxIqPeakWindow`, `Hl2Telemetry::rx1IqPeakDbfs` (Task 1)
- Produces: `MetisClient::Params::txLnaGainDb` (`std::optional<int>`, default `nullopt`)
- Produces: `Q_INVOKABLE void MetisClient::setTxLnaGainDb(int db);` and `Q_INVOKABLE void MetisClient::clearTxLnaGain();`

- [ ] **Step 1: Write the failing test** `hl2_tx_lna_gain_frame_test.cpp`, modelled on `hl2_band_filter_frame_test.cpp` (same `MetisClientTestAccess` friend, `bank()`, `addrOf()`, `payloadOf()` helpers). Drive `buildNextControlPacket()` for a full rotation and find the 0x0e bank. Checks:
  - default params, streaming: the 0x0e bank is `{0x1C,0,0,0,0}`;
  - `setTxLnaGainDb(-6)` while streaming: queues exactly one one-shot, the next frame carries 0x0e with C3 = `0xC6`, and the 0x0e slot of the next full rotation carries it too;
  - `clearTxLnaGain()`: one one-shot, then the rotation's 0x0e bank is all-zero again;
  - stopped (not streaming): `setTxLnaGainDb(10)` queues nothing; after `setStreaming`, the rotation carries C3 = `0xD6`;
  - `start()` with `Params::txLnaGainDb = 20` seeds the bank (C3 = `0xE0`) without any setter call.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --target hl2_tx_lna_gain_frame_test 2>&1 | grep error: | head -3`
Expected: errors naming `setTxLnaGainDb` / `txLnaGainDb`.

- [ ] **Step 3: Implement**
  - Member `Cc m_ccAdcAssignOrTxGain{}` set from `m_params.txLnaGainDb` wherever `m_ccGain` is set from `m_params.lnaGainDb` (both `start` sites); the round robin's `kCcAdc` slot uses it; delete the `static const Cc kCcAdc`.
  - The setters update `m_params.txLnaGainDb` and the member, then `queueOneShotIfRunning(m_ccAdcAssignOrTxGain)`, the same pattern as `setTxDriveLevel`. No TX gate.
  - Add an `RxIqPeakWindow m_rxIqPeak`: observe `m_blocks[0]` after each successful `ep6SamplesMulti`; at the telemetry emit set `m_telemetry.rx1IqPeakDbfs = m_rxIqPeak.peakDbfs()` and clear it, beside the forward-power window. Clear it wherever `m_fwdWindow.clear()` is called on stop.
  - Update the `kRequestableAddresses` comment's name to `ccAdcAssignOrTxGain`.

- [ ] **Step 4: Run it to verify it passes**

Run: `cmake --build build --target hl2_tx_lna_gain_frame_test hl2_band_filter_frame_test hl2_fwd_window_wiring_test && for t in hl2_tx_lna_gain_frame_test hl2_band_filter_frame_test hl2_fwd_window_wiring_test; do ./build/$t | tail -1; done`
Expected: each prints `... all checks passed`.

- [ ] **Step 5: Commit** — `feat(hl2): MetisClient sends a live 0x0e bank and measures raw RX1 IQ peak. Principle VIII.`

---

### Task 3: Hl2Backend: the extension verb and Radio Health rows

**Files:**
- Modify: `src/core/backends/hl2/Hl2Backend.h` (member beside `m_lnaGainDb`)
- Modify: `src/core/backends/hl2/Hl2Backend.cpp` (`invokeExtension` ~line 5335; connect-time `Params` fill near `mp.ocFilterByte`; `healthSnapshot` near the `adcPeakDbfs` row ~line 6475; telemetry handler near the forward-power block ~line 7745)
- Test: extend the existing test that drives `Hl2Backend::invokeExtension` for `options.*` verbs if there is one (`grep -l 'options.setAdcDither' tests/*.cpp`); otherwise add the checks to `tests/hl2_tx_lna_gain_frame_test.cpp` using the backend directly.

**Interfaces:**
- Consumes: `MetisClient::setTxLnaGainDb(int)`, `clearTxLnaGain()`, `Params::txLnaGainDb`, `Hl2Telemetry::rx1IqPeakDbfs`
- Produces: extension verb `hl2` / `txlnagain.set` — argument a `QVariant` holding an `int` dB, or an invalid `QVariant` for off; answers through `extensionResult` like `options.*`
- Produces: health rows `txLnaGainDb` (label "Receive gain while transmitting (dB)", invalid variant when off) and `rx1IqPeakDbfs` (label "RX1 raw IQ peak (dBFS, before DSP)", invalid variant when the last window had none)

- [ ] **Step 1: Write the failing test**: invoking `txlnagain.set` with `-6` makes `healthSnapshot().values["txLnaGainDb"]` read `-6`; with an invalid `QVariant` it reads invalid; the verb answers `extensionResult` with the request id.
- [ ] **Step 2: Run it to verify it fails** (build error or failed check naming `txLnaGainDb`).
- [ ] **Step 3: Implement**: `std::optional<int> m_txLnaGainDb` (session-only, reset on disconnect); the verb stores it, clamps to `[-12, 48]`, and queues `setTxLnaGainDb` / `clearTxLnaGain` on `m_metis` with `Qt::QueuedConnection`; connect copies it into `Params::txLnaGainDb`; the telemetry handler keeps the last `rx1IqPeakDbfs` for the health row; log one `qCInfo(lcHl2)` line when the gain changes.
- [ ] **Step 4: Run it to verify it passes**, then the whole HL2 group: `ctest --test-dir build -R hl2 -j$(nproc) | tail -3` → `100% tests passed`.
- [ ] **Step 5: Commit** — `feat(hl2): set the receive gain used while transmitting, and show raw RX1 IQ peak in Radio Health. Principle VIII.`

---

### Task 4: The `txlnagain` automation verb

**Files:**
- Modify: `src/core/AutomationServer.cpp` (register beside `bandscope`, ~line 3431; implement beside `doBandscope`, ~line 8262)
- Modify: `src/core/AutomationServer.h` (declaration beside `doBandscope`)
- Modify: the bridge docs, regenerated by `python3 tools/gen_bridge_docs.py`

**Interfaces:**
- Consumes: extension verb `hl2` / `txlnagain.set`; health rows `txLnaGainDb`, `rx1IqPeakDbfs`
- Produces: bridge verb `txlnagain [status|off|<dB>]` → `{"ok":true, "txlnagain":<action>, "gainDb":<int|null>, "rx1IqPeakDbfs":<double|null>}`

- [ ] **Step 1: Implement `QJsonObject doTxLnaGain(const QString& action)`** on the `doBandscope` pattern: refuse when `!backendDeclaresExtension("hl2")` ("txlnagain: this radio has no hardware-managed transmit gain — register 0x0e is a Hermes-Lite 2 feature"); `status` reports from the health snapshot; `off` sends an invalid `QVariant`; anything else must parse as an integer dB or is an error; report from the snapshot after the call, never by echoing the request. Not TX-gated.
- [ ] **Step 2: Regenerate and check the docs**: `python3 tools/gen_bridge_docs.py && python3 tools/gen_bridge_docs.py --check` → exit 0.
- [ ] **Step 3: Build and run the bridge-docs test**: `cmake --build build -j$(nproc) 2>&1 | grep error: ; python3 -m pytest -q tools/test_gen_bridge_docs.py` → no errors, tests pass.
- [ ] **Step 4: Commit** — `feat(bridge): txlnagain verb for the HL2's receive gain while transmitting. Principle VIII.`

---

### Task 5: Bench check on the HL2 (operator present)

**Files:**
- Modify: `docs/HERMES.md` — backlog row 22 and the §11.1 note on `0x0e`, with what was measured

- [ ] **Step 1: Confirm with the operator** that they are at the bench and the HL2 is into the dummy load with no amplifier, then launch the build with the bench environment (Global Constraints) plus `QT_LOGGING_RULES="aether.hl2.tx.debug=true"`, connect, and set RF Power 50, Tune Power 10.
- [ ] **Step 2: Measure.** For each of `txlnagain off`, `txlnagain -12`, `txlnagain 20`: key TUNE for 2 s, read `txlnagain status` (`rx1IqPeakDbfs`) once mid-carrier, unkey. Then unkeyed, read it once more with the gain still at 20.
  Expected: the keyed `-12` reading is about 32 dB below the keyed `20` reading (less if either clips near 0 dBFS); `off` reads at the normal receive LNA gain; the unkeyed reading is unaffected by the setting.
- [ ] **Step 3: Send the CW ID** (DE W5TSU), confirm `transmitting=false`, `txlnagain off`, quit the app by PID.
- [ ] **Step 4: Record the numbers** in `docs/HERMES.md` row 22 (mark it done, with the measured step) and the §11.1 note.
- [ ] **Step 5: Commit** — `docs(hl2): 0x0e transmit gain measured on the bench; backlog #22 done. Principle VIII.`
