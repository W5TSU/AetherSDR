# HackRF Backend — Design

**Status:** proposed
**Date:** 2026-09-16
**Author:** Mark Grennan, with Claude Sonnet 5

## Motivation

Add HackRF One (and compatible clones) support to AetherSDR via `libhackrf`,
behind the existing `IRadioBackend` seam. The driving goal is amateur
satellite work — primarily FM satellites (SO-50, AO-91, ISS APRS/voice)
operated PTT-style, the way a repeater is worked. Marked **highly
experimental**: a single HackRF is TX-capable but has none of a purpose-built
transceiver's output filtering, so spurious emissions are a real risk without
external bandpass filtering the operator provides themselves.

This is fork-only work (per this fork's general policy of not upstreaming to
`aethersdr/AetherSDR`), and belongs in the W5TSU fork's own roadmap.

## Scope

**In scope for v1:**
- Full RX+TX over a single HackRF.
- TX modes: FM and CW only.
- Multi-slice receive within HackRF's wide (up to 20 MHz) capture — several
  simultaneous slices tuned within one wideband span, matching Flex/HL2's
  multi-receiver model rather than RtlSdr's single-slice shape.
- Experimental gating: a visible "experimental" label in the UI, matching how
  Anan and RtlSdr shipped (`v26.9.4`). No extra acknowledgment dialog, no
  software-enforced power ceiling — the operator is trusted to handle
  filtering and legal compliance themselves, same as any other radio.

**Explicitly out of scope for v1** (future projects, not part of this spec):
- **SSB TX.** Linear modulation is a materially larger DSP/ALC surface on
  8-bit DAC hardware; FM+CW covers the stated satellite goal.
- **Full-duplex / split operation for linear-transponder satellites**
  (AO-7, FO-29, and most cubesat U/V transponders). Operating those normally
  requires hearing your own signal through the transponder in real time for
  Doppler/tuning correction — which needs simultaneous RX+TX. A single
  HackRF is physically half-duplex (one antenna port, one RF pipeline,
  `hackrf_start_rx`/`hackrf_start_tx` are mutually exclusive on the USB
  pipe) and cannot do this alone. AetherSDR also has no existing concept of
  paired split operation across two backend instances (RadioModel.h's only
  "Doppler" reference is to an *external* tool, SatPC32, driving frequency
  over CAT — there's no built-in equivalent). If split operation is wanted
  later, it's its own design pass spanning the radio-model layer, not just
  a backend.
- A software-enforced TX power/gain ceiling, or a first-TX acknowledgment
  flow. Considered and declined for v1 — matches how Anan/RtlSdr's
  `experimental` label works today, nothing HackRF-specific added.

## Precedents this design reuses

- **RtlSdrBackend** (`src/core/backends/rtl/`) — the closest prior art for a
  local, host-demodulated USB SDR peripheral: worker-thread ownership of the
  vendor device handle, async-callback read loop (`rtlsdr_read_async`, which
  `hackrf_start_rx`/`hackrf_start_tx` mirror), a DDC per tuned slice.
  RtlSdr is RX-only and single-slice; this backend extends both dimensions.
- **Hl2Backend** (`src/core/backends/hl2/`) — the closest prior art for
  *transmit*: it already runs a WDSP-based modulator on the host that turns
  mic/CW audio into IQ samples, then ships that IQ to hardware with no
  onboard DSP (`Hl2Backend.cpp:452-456`, `m_metis->queueTxIq(iq)`). It also
  already sets the exact capability pair this backend needs:
  `hostModulates=true` + `takesTxAudioOverSeam=true`
  (`RadioCapabilities.h:248-254`, `:538-556` — see that comment block for why
  these two flags are separate and both required, learned the hard way when
  conflating them broke Icom transmit).
- **`ENABLE_RTL`** (`CMakeLists.txt:142`) — the template for build-flag
  gating, pkg-config discovery with a `find_library` fallback, and graceful
  disable-with-message when the library isn't present.

## Architecture

```
                    ┌─────────────────────┐
   submitTxAudio →  │   HackRfBackend      │  ← IRadioBackend
   (mic/CW keyer)   │  (I/O thread owner)  │
                    │                      │
                    │  WDSP TXA channel ───┼──→ modulated IQ ──┐
                    │  (FM / CW only)      │                    │
                    │                      │                    ▼
                    │  N × WDSP RXA        │         ┌──────────────────┐
                    │  channels (1 per     │ ◄────── │  RX/TX arbitration│
                    │  active slice)       │  audio  │  state machine     │
                    │      ▲               │         └─────────┬─────────┘
                    └──────┼───────────────┘                   │
                           │ IQ (per-slice tuned)               │
                    ┌──────┴───────────────┐                    │
                    │   N × HackRfDdc       │                    │
                    │  (tune/decimate)      │                    │
                    └──────▲────────────────┘                    │
                           │ raw wideband IQ                     │
                    ┌──────┴────────────────────────────────────▼──┐
                    │              HackRfWorker (I/O thread)         │
                    │  owns hackrf_device*; hackrf_start_rx/_tx      │
                    │  callbacks; 8-bit interleaved IQ ↔ float IQ    │
                    └─────────────────────────────────────────────┘
```

### Components (`src/core/backends/hackrf/`)

- **`HackRfBackend`** — the `IRadioBackend` implementor. Owns one WDSP RXA
  channel per active slice (same channel-pool pattern `Hl2Backend` uses) and
  one WDSP TXA channel for the FM/CW modulator. Orchestrates the RX↔TX
  arbitration state machine and publishes Panadapter/Slice/Transmit deltas
  through the seam exactly like every other backend.
- **`HackRfWorker`** — the only thread permitted to touch the
  `hackrf_device*` handle, mirroring `RtlSdrWorker`'s ownership rule for
  `rtlsdr_dev`. Runs the `hackrf_start_rx`/`hackrf_start_tx` C callbacks and
  converts HackRF's native 8-bit unsigned interleaved IQ to/from WDSP's
  float IQ format in both directions.
- **`HackRfDdc`** (one per active slice) — digital downconverter that tunes
  a slice's center frequency/bandwidth out of the wide raw capture. Extends
  `RtlSdrDdc`'s shape to run multiple concurrent instances off one raw feed.
  This — multi-instance DDC against one shared wideband source — is the one
  RX-side piece with no direct precedent; RtlSdr only ever needed one.

## Data flow

**RX:** `hackrf_rx` callback (I/O thread) → raw bytes → `HackRfWorker`
converts to float IQ → each active slice's `HackRfDdc` tunes/decimates →
feeds that slice's WDSP RXA channel → demodulated audio + spectrum publish
through the seam as Panadapter/Slice deltas.

**TX:** `IRadioBackend::submitTxAudio` → WDSP TXA channel (FM or CW-keyer
path only) → modulated IQ → arbitration state machine gates the transport →
`HackRfWorker`'s `hackrf_tx` callback pulls queued IQ once RX teardown is
confirmed.

## RX↔TX arbitration

The one piece of this design with no precedent to copy. HL2's hardware
streams RX and TX IQ concurrently over Ethernet — MOX there gates the PA and
antenna relay, not the ADC/DAC pipeline — so `Hl2Backend` never had to solve
transport-level RX/TX exclusivity. libhackrf genuinely cannot run
`hackrf_start_rx` and `hackrf_start_tx` at once on the same USB pipe; RX must
be explicitly torn down before TX starts, and rebuilt after.

State machine: `Idle → RxStreaming → TxPending → TxStreaming → RxPending →
RxStreaming`. A PTT/keying request from `TxStreaming`-eligible state first
requests RX teardown, waits for the worker's confirmation the USB transfer
queue has drained, then starts TX; release reverses the sequence. Explicit
sequencing and coalescing here is deliberate from day one — it's the same
class of bug the Icom command-plane work fixed (recent commit `#28`: a
delayed PTT-OFF reply arriving after a newer PTT-ON was cutting transmit
audio). The state machine must not wedge if libhackrf never confirms a mode
change (device error, unplug mid-transition) — a timeout path back to a safe
state is required, not optional.

## Capabilities (`RadioCapabilities`)

- `canTransmit = true`
- `hostModulates = true`
- `takesTxAudioOverSeam = true`
- `ownsRxAudio = true` (host demod, no VITA-49 stream)
- **Open question, not resolved by this spec:** HackRF exposes three
  independent gain controls (LNA 0–40dB, VGA 0–62dB, front-end AMP on/off)
  against the existing single `RfGain` capability (`RadioCapabilities.h:322`,
  documented elsewhere as "three preamp detents"). Whether this needs a
  small capability extension or the three controls fold into one is a
  first-PR investigation — every new backend so far has reshaped this shared
  file somewhat (the aetherd roadmap entry calls bringing up a third vendor
  "the seam's best audit to date"), and guessing the shape here risks
  designing around a wrong assumption.

## Build & vendoring

Follow `ENABLE_RTL`'s exact template (`CMakeLists.txt:142`):
- New `option(ENABLE_HACKRF "Enable the experimental HackRF backend" ON)`.
- `pkg_check_modules` for `libhackrf`, with a `find_library`/`find_path`
  fallback, gracefully disabled with a status message if not found —
  matching the `RTLSDR_FOUND` pattern (`CMakeLists.txt:2435-2478`).
- Linux/macOS use the system package (`apt install libhackrf-dev` /
  `brew install hackrf`).
- Windows needs a new `scripts/setup/setup-hackrf.ps1` fetching a prebuilt
  DLL, matching `setup-fftw.ps1`/`setup-opus.ps1`.
- Licensing: `libhackrf` is GPL-2.0; AetherSDR is GPLv3. No new concern —
  RtlSdr already established a GPL SDR driver library links cleanly here.

## Error handling

- Device unplugged mid-session: USB errors surfaced from the
  `hackrf_rx`/`hackrf_tx` callbacks must be caught on the worker thread and
  turned into a clean disconnect, not a crash.
- RX/TX switchover timeout: the arbitration state machine must have a
  bounded wait with a defined failure path (see above).
- No onboard SWR/ALC protection exists on HackRF hardware: the modulator
  must not be able to key with a null/invalid channel target — this is a
  software-only safety backstop, not a substitute for the operator's own
  filtering.

## Testing

- Unit tests around the arbitration state machine (transitions, timeout
  handling) that don't require real hardware.
- A loopback/simulation harness for RX/TX round-tripping, reusing
  `SimBackend`'s pattern if applicable.
- A real-hardware verification checklist (RX slice tuning across the wide
  span, FM/CW TX keying, unplug-mid-session recovery) before the
  `experimental` label comes off. Full detail belongs in the implementation
  plan, not this spec.

## Non-goals recap

SSB TX and full-duplex split operation for linear-transponder satellites are
deliberately out of scope for this spec — see "Explicitly out of scope"
above. Both are legitimate future projects; neither is assumed or
half-built here.
