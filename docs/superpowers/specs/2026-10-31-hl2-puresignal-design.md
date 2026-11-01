# PureSignal on the Hermes-Lite 2 — design

Date: 2026-10-31. Status: draft for the operator's approval.

## Why

The operator (W5TSU) runs a Hermes-Lite 2 into a Xiegu XPA125B amplifier. The
amplifier is where the distortion comes from: the HL2 itself is clean at 5 W,
but an amplifier driven toward its rating spreads intermodulation products
either side of the signal. PureSignal measures what actually leaves the
amplifier, compares it with what the HL2 meant to send, and predistorts the
HL2's output so the amplifier's output comes out clean. The HL2 wiki's 100 W
example went from −30 dBc to −45 dBc unwanted products with PureSignal on.

The goal is a cleaner signal on the air through the amplifier, operated with
**one button**, the way Thetis and piHPSDR do it.

## The station (what the design can rely on)

- HL2 with the N2ADR I/O board fitted. The rear panel has **PURE** (the I/O
  board's Pure Signal input, J10), **ALT RX** (J9), **RF1**, **ACC** (DB9) and
  **ANT**.
- Amplifier: Xiegu XPA125B (up to about 125 W).
- Feedback sampler: Hasidh Projects FRS-2K, −55 dB coupling, on the amplifier
  output. Not yet wired; it goes to **PURE**.
- Expected sample level at PURE: about −5 dBm at 100 W, −12 dBm at 20 W.

The I/O board's register 11 (`REG_RF_INPUTS`, from
[jimahlstrom/HL2IOBoard](https://github.com/jimahlstrom/HL2IOBoard),
`i2c_registers.h` and `n2adr_lib/i2c_slave_handler.c`) decides how J9 and J10
are used:

| Mode | Receive input | PURE (J10) |
|---|---|---|
| 0 (power-on default) | normal ANT port; J9 unused | mixed into the receive signal at all times |
| 1 | J9 | not available |
| 2 | J9 | replaces the receive signal during transmit |

AetherSDR has never written register 11, so this station is in **mode 0**, and
mode 0 is what it should stay in: the antenna stays on ANT, and PURE is heard
during transmit. On receive PURE also mixes in a −55 dB copy of the antenna
signal, which is negligible. **No port switching is part of this design.**

The PURE input passes through an attenuator on the I/O board whose value, and
whose maximum safe input, are not documented. That is why the first powered
step is a low-power level check (see Verification).

## What the operator sees

- A **PS** button beside TUNE and MOX in the transmit panel, shown only when
  the connected radio can do PureSignal (an HL2 with transmit allowed).
- Turned on, PS needs nothing else. While the operator transmits normally, it:
  - sets the HL2's receive gain during transmit (register `0x0e`, the "TX ATT"
    in other programs) so the feedback sits in its working range, and adjusts
    it as power or band change;
  - calibrates against the amplifier and applies the correction.
- A small status indicator next to the button: off / calibrating / correcting /
  feedback too weak / feedback too strong, plus the feedback level, the way
  Thetis shows its PS feedback meter.
- On/off is remembered across restarts. The learned correction is saved per
  band and restored when the band is used again.
- While PS is on, the HL2 uses two of its four receivers for feedback, so at
  most **two slices** are available. The operator accepted this.

## Goals

1. Lower intermodulation through the XPA125B with PS on than with PS off, on a
   two-tone signal and on voice, measured outside AetherSDR (HackRF Pro into
   the dummy-load path).
2. One button; nothing else for the operator to set in normal use.
3. Feedback level visible while transmitting.

## Non-goals

- Port switching on the I/O board (modes 1 and 2). Mode 0 serves this station.
- PureSignal on any radio other than the HL2. The control should be written
  against a backend capability, not "is this an HL2", so another backend can
  add it later, but nothing beyond the HL2 is built now.
- Manual calibration screens, correction-curve displays, delay tuning UIs.
  WDSP's defaults and automode first; expose more only if the bench shows a
  need.
- Diversity receive, the wideband bandscope, the TX FIFO servo (other backlog
  items).

## Anti-goals (a failure even if it "works")

- **PS must never make the transmitted signal worse or louder than PS off.**
  If feedback is absent, clipped or implausible, correction stops and the
  signal goes out uncorrected. The predistorted output must not exceed the
  drive the operator's RF Power setting allows (Principle VI): WDSP's
  correction can expand peaks, so the hardware-peak setting (`SetPSHWPeak`)
  and the output amplitude are bounded deliberately, not left at defaults.
- **PS never keys the transmitter.** Calibration happens on transmissions the
  operator makes (or the explicit two-tone test the operator starts). No
  background keying.
- **No silent receiver loss.** If turning PS on would take receivers the
  operator is using, the app says so instead of dropping slices without a word.
- **Nothing changes for an HL2 without PS on.** With PS off, the wire bytes
  are what they are today, except the `0x0e` fix in step 1, which is inert
  until a non-zero value is written.

## Constraints

- Fork rules: work on a branch, PRs only to `W5TSU/AetherSDR`, user-initiated.
- Transmit testing follows the bench rules: operator present, RF and Tune
  power at or under 80%, a CW ID (W5TSU) at the end, unkey on every exit path,
  automation launched with `AETHER_AUTOMATION_ALLOW_TX=1`,
  `AETHER_AUTOMATION_TX_MAX_POWER=80`, `AETHER_AUTOMATION_TX_MAX_MS=15000`.
- The amplifier adds a constraint the HL2 alone did not have: amplifier tests
  go into a dummy load rated for the amplifier, starting at its lowest output.
- WDSP is vendored (`third_party/wdsp`, PureSignal in `upstream/calcc.c`, with
  the fork's patches 18 and 22 on its thread). Changes to it are patches,
  recorded in `third_party/wdsp/AETHERSDR-PATCHES.md`.
- `Hl2Receivers.h` keeps `{ ddcIndex, dspChannel, analyzerId, uiNumber }` per
  receiver and forbids deriving one from another. PureSignal is the case that
  rule was written for; build on it.

## The parts, in build order

Each is its own branch and PR, and each leaves the radio working.

### 1. `0x0e` carries a transmit gain on the HL2 (backlog #22)

- `MetisClient`'s round robin writes `kC0AdcAssignOrTxGain` (address `0x0e`)
  as all zeros. On the HL2, `0x0e[15]` enables hardware-managed LNA gain
  during transmit and `0x0e[13:8]` is that gain; `0x0e[14]` is a mode bit
  the protocol page defers to its LNA section. The generic openHPSDR meaning
  (per-receiver ADC assignment) must keep working for non-HL2 Protocol-1
  devices such as hpsdrsim.
- Add a setter for the transmit gain, encoded like the `0x0a` LNA field, and
  write it only for an HL2. Leave bit 15 clear (today's behaviour) until a
  value is set.
- Bench check, HL2 alone into the dummy load, no amplifier: with bit 15 set
  and a low transmit gain, the receive level of the HL2's own carrier during
  TUNE drops by the expected amount and returns on unkey.

### 2. Feedback level, visible

- With the sampler wired to PURE and mode 0, show the feedback level while
  transmitting: a reading in the transmit panel or Radio Health of the
  feedback receiver's peak, relative to the converter's full scale.
- This is the instrument for the first amplifier test and for setting the
  automatic range in step 4.

### 3. PureSignal receivers

- Enable PureSignal in the gateware (`0x0a` bit 22) and run the receiver
  layout PureSignal needs: four DDCs, two of them feedback (the transmit
  reference and the coupled sample), mapped through `Hl2Receivers`, with at
  most two slices left for the operator.
- Hand the two feedback streams to WDSP's `pscc()` for the TX channel while
  transmitting.
- **To verify, not assume:** which DDC carries which stream under PureSignal on
  current HL2 gateware (2019 list posts say the DAC reference on RX4 and the
  feedback on RX3), the feedback sample rate, and how `SetPSFeedbackRate`
  must be set. Check against the gateware source and piHPSDR's
  `old_protocol.c`, not memory.

### 4. The PS button

- Drive WDSP's calcc through its control calls (`SetPSControl`,
  `SetPSAutomode`, `SetPSMox`, `SetPSRunCal`, `GetPSInfo`) from a small state
  machine in the HL2 backend: off → calibrating → correcting, with the
  fault states above.
- Automatic feedback level: adjust the `0x0e` transmit gain from `GetPSInfo`'s
  feedback level, the way piHPSDR's auto-attenuate does, within limits found
  in step 2.
- Save and restore the correction per band (`PSSaveCorr` / `PSRestoreCorr`),
  in the HL2's radio-scoped settings.
- Bound the output: `SetPSHWPeak` set from the HL2's real full scale, and a
  check that the corrected output never exceeds the uncorrected drive.
- The UI: the button and status indicator; a capability flag on the backend
  so the button shows only where it works; persistence of on/off.

### 5. A two-tone signal on the HL2

- The HL2 has no two-tone generator (`twoToneGenerator` is unset), so the
  bridge refuses `txtest twotone`. Add one, alongside the existing single test
  tone, so PS can be calibrated on a clean, repeatable signal and the
  improvement can be measured. Gated exactly like TUNE.

## Verification

- Unit tests for each step that has logic: the `0x0e` encoding and its HL2
  gating; the receiver mapping under PureSignal; the PS state machine's
  transitions and fail-safe exits; the output bound.
- Bench, in order, each step only after the one before is clean:
  1. HL2 alone, dummy load: `0x0e` changes the receive level of its own
     carrier during TUNE.
  2. Sampler on PURE, amplifier at its lowest output into a dummy load: read
     the feedback level, confirm it is far below clipping, then step power up
     while watching it. This answers the undocumented PURE attenuator.
  3. Two-tone through the amplifier at moderate power: HackRF Pro records the
     output spectrum with PS off, then on. Pass: third- and fifth-order
     products lower with PS on, and no product higher.
  4. Voice (or recorded speech) the same way.
  5. Pull the sampler cable while correcting: PS must drop to uncorrected
     within one calibration cycle, without a burst.
- Each transmission follows the bench rules above.
- The HackRF Pro is **never wired to the amplifier output**. It listens by
  pickup near the dummy load, as in the 2026-10-31 TUNE test, or through enough
  fixed attenuation that full amplifier power lands well below its input
  limit. Check its level at the lowest amplifier power first.

## Left to the builder

- Where the PS state machine lives (backend vs. a small helper beside it) and
  its exact states.
- The indicator's look, within "small, beside the button, says which state".
- The automatic level loop's step size and limits, from step 2's numbers.
- Whether step 5 comes before step 4; it gives step 4 its test signal.

## Calls made without the operator (check these)

- Mode 0 on the I/O board, no register 11 writes.
- The button lives in the transmit panel beside TUNE/MOX.
- Correction saved per band; on/off saved globally.
- The two-tone generator is in scope.
- Build order 1 → 5 as separate PRs.
