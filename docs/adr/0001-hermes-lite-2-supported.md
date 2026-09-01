# 0001 — Hermes-Lite 2 promoted from experimental to supported

**Status:** proposed

> **Gate progress (updated as the milestone lands):**
> - Plans 1–4 implemented on `feat/hl2-supported-groundwork` (verification
>   verbs; RTTY/DFM + authoritative mode list; panadapter trace averaging;
>   DSP-chain hardening). Plan 5.1 (resampling affordance) and 5.2 (HERMES.md
>   de-drift) done.
> - **Certifying unit gateware recorded: v7.4** (0x4A) — Metis discovery reply
>   from `100.117.237.246`, MAC `00:1C:C0:A2:02:01`, board `0x06`, idle,
>   2026-08-31. This is the reference gateware; the 2026-08-10 TX-meter
>   certification ran against the same version, so no gateware-dependent
>   re-check (drive-nibble decode, `0x0e` dual meaning, discovery `0x13`) is
>   triggered.
> - **Still open before Status → accepted:** the deviations in *Scope
>   deviations* below need the project lead's sign-off or implementation; a
>   `radiocert` run and the four-receiver soak on hardware (gate items 3–4).

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
   `docs/radio-certification.md`. **Note:** this milestone's changes are RX,
   display and verification only — Plan 4.2 established the T/R path and the
   channel lifecycle are untouched — so the **2026-08-10 TX-meter
   certification (gateware v74, `docs/radio-certification.md`) carries
   forward**; the keyed `tx`/`meters` phases are re-run only if the project
   lead wants fresh evidence. `tune` and `rx` (non-keying) are re-run for this
   milestone regardless;
4. a ten-minute four-receiver soak — no drops, no p99 growth, no crash;
5. this ADR accepted and the two costs below documented in it;
6. the certifying unit's gateware version recorded, with the
   gateware-dependent assumptions (drive-register nibble decode, register
   `0x0e` dual meaning, discovery receiver-count field) listed as re-check
   items for any other gateware.

The flip touches `ExperimentalRadioSupport.h`, the README and the ROADMAP, and
is the final PR of the milestone. Governance for this fork: this ADR plus the
project lead's sign-off; no separate RFC.

## Scope deviations found during implementation

Two items in the *Decision*'s panadapter-parity bullet were tracked separately.

1. **Peak / sample / average detector modes — BUILT.** The *Decision* asks for
   all three (issue #1 story 6, Testing Decisions "assert the peak detector
   holds peaks"). Delivered:
   - *sample* — `Hl2Spectrum::setAveraging(1, *)`, bit-for-bit the raw frame;
   - *average* — factor > 1, dB-domain EMA or boxcar (Plan 3);
   - *peak* — `Hl2Spectrum::setPeakHold(true)`, a per-bin max-hold; infinite
     until the span changes when the FFT AVG level is ≤ 1, else a sliding
     maximum over that many frames.
   The peak toggle is a new "Peak" button in the Display overlay's toggle row,
   shown only where the client shapes its own spectrum
   (`setRadioSideDspAvailable(false)` — HL2, not Flex). It routes
   `SpectrumOverlayMenu::fftPeakHoldChanged → RadioModel::requestPanPeakHold →
   IRadioBackend::setPanPeakHold → Hl2RxDsp::setSpectrumPeakHold`, held across
   a channel rebuild, and is drivable headlessly via `pan peakhold`.
   `hl2_spectrum_test` asserts the hold/age-out/reset behaviour.

2. **Span-following bin count — deferred (fast-follow).** The *Decision*'s "and
   a configurable bin count" and story 8 ("the configured bin count is
   honoured") are not built; the FFT stays at 1024 bins. Issue #1's own
   Implementation Decisions concede "the missing lever is a detector /
   averaging mode, not FFT size", and `HERMES.md §12.4` agrees — the detector
   work above *is* that lever. A runtime FFT-size change also means
   reallocating the FFTW plan on the DSP thread and reconciling the
   partial-frame buffer and the display-rate shaper, with artefact risk on
   every zoom.

**Disposition:** (1) is delivered. (2) is a named fast-follow alongside
FreeDV/RADE and the off-thread rebuild — accepted on the spec's own "not FFT
size" reasoning. The project lead's sign-off on deferring (2) is the remaining
governance step for this deviation (per the *Promotion gate* line).

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
