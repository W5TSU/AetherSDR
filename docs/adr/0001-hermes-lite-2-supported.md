# 0001 — Hermes-Lite 2 promoted from experimental to supported

**Status:** proposed

## Context

The HL2 backend has been `experimental` since v26.7.4 and reached near-parity
with the Flex path by v26.8.3 — four receivers, the SSB/CW/digital-voice
chains, CW/RTTY decoding and the QSO recorder, band switching with hardware
filters and preamp, host-side memory channels, per-MAC operating-state restore,
and a transmit-meter surface certified against physical hardware. The ROADMAP
names an explicit bar for taking the label off — wider mode coverage,
panadapter/waterfall parity with the Flex path, and hardening the raw-IQ DSP
chain — but records that "the experimental → supported call itself is still
open."

## Decision

Promote the HL2 to a **supported** radio family once, and only once, a fixed
gate is met, and accept two known costs rather than block promotion on them.

**Scope of "supported" is the ROADMAP bar**, not the full `HERMES.md §13`
backlog:

- the published mode list is **backend-authoritative**, and every mode in it
  either works or is absent — no silent fall-through to USB. This adds real
  RTTY and DFM support and drops D-STAR, DRM, FreeDV and RADE from the HL2
  menu (they need decoders or an audio path this backend does not have);
- the HL2 spectrum engine gains peak/sample/average detector modes,
  configurable averaging and a configurable bin count, and the four-receiver
  display behaviours are driven to green against the multi-DDC test matrix;
- the T/R transition, the ADC-overload log rate, the per-slice dB reference,
  and the notch-filter latency are hardened (see *Accepted costs*).

**PureSignal, diversity receive, and the wideband bandscope are explicitly not
part of "supported."** They are new capabilities, framed by the ROADMAP as
differentiation, and remain separately roadmapped. `HERMES.md §13` Tier-3
(request/ack state machine, TX FIFO servo, discovery-telemetry poll, PA bias,
config EEPROM, hardware-managed T/R LNA gain) and the `§13` T1-4 pipeline reset
are also out.

**Promotion gate** — every item evidenced before the label-flip PR merges:

1. the offline test suite green;
2. the automation-bridge verification verbs and the multi-DDC parity script
   green at one and four receivers;
3. `radiocert` clean through tune → rx → tx → meters on physical hardware,
   transmitting into a dummy load under the written safety authorization in
   `docs/radio-certification.md`;
4. a ten-minute four-receiver soak — no drops, no p99 growth, no crash;
5. this ADR accepted and the two costs below documented in it;
6. the certifying unit's gateware version recorded, with the
   gateware-dependent assumptions (drive-register nibble decode, register
   `0x0e` dual meaning, discovery receiver-count field) listed as re-check
   items for any other gateware.

The flip touches `ExperimentalRadioSupport.h`, the README and the ROADMAP, and
is the final PR of the milestone. Governance for this fork: this ADR plus the
project lead's sign-off; no separate RFC.

## Considered options

- **Hold experimental until PureSignal / diversity / bandscope also land.**
  Rejected: those are multi-cycle efforts the ROADMAP treats as differentiation,
  not parity. "Supported" would never ship.
- **Fix both known costs before promoting.** Rejected: the off-GUI-thread
  receiver rebuild is entangled with the backend-teardown lifetime hazard and
  is its own design conversation. Blocking a finishable milestone on an
  optimisation is the wrong trade.
- **Promote with looser criteria** — RX and TX work on hardware, docs updated,
  skip the `radiocert` / conformance / soak ceremony. Rejected: this backend's
  history is a catalogue of bugs that passed every check that existed and
  failed one that did not; the project built `radiocert` for exactly this
  moment.

## Accepted costs

1. **Notch-filter latency is gated, not eliminated.** The receive bandpass FIR
   runs at the long (8192-tap, ~50 Hz notch floor) length only while a notch is
   placed; the RX path otherwise defaults to the short (~2048-tap) low-latency
   filter. The mechanism is a runtime filter-length change on the WDSP channel
   wrapper under the existing control-operation guard. If that runtime change
   proves unsafe in practice, the fallback is the unconditional +64 ms,
   documented as such. Either way, a deep 50 Hz notch and lowest-latency
   receive cannot both be had at once on this radio.
2. **The sample-rate-boundary stall stays.** Crossing a sample-rate boundary
   rebuilds every receiver on a blocking call — 0.6–1.1 s of frozen UI with
   four panes. This milestone ships a non-blocking "resampling…" affordance and
   this written note; moving the rebuild off the GUI thread is a scheduled
   fast-follow.

## Consequences

- FreeDV and RADE stay unavailable on HL2 until the bus-B RX-audio tap is
  built — a named fast-follow, not part of this promotion.
- The `experimental` notice machinery loses its HL2 descriptor; the Icom
  descriptor stays.
- `radio_capability_gating_test` gains the authoritative-mode-list assertions
  and, at promotion, the descriptor-removal assertion.
- The `HERMES.md §13` backlog table must be updated as milestone items land, so
  the canonical to-do list does not drift.
