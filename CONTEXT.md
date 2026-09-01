# AetherSDR — Domain Glossary

AetherSDR is a multi-radio SDR client. Radio-family logic lives behind one
vendor-neutral interface. The terms below are the ones that have been
load-bearing in real bugs, so they are worth stating precisely; this file is a
glossary, not a spec.

## Radio families and the seam

**Seam**:
The `IRadioBackend` interface — `RadioCapabilities` plus typed status and
command deltas — behind which all radio-family wire logic lives. Four backends
ride it: Flex, HL2, Icom, and the synthetic Sim.
_Avoid_: driver, adapter, HAL.

**Backend-authoritative**:
The rule that clients render against what a backend *reports* through
`RadioCapabilities`, never against a test of which family it is — no
`caps.family` branch, no `dynamic_cast` to a concrete backend. A capability
field left unset declares the feature *absent*, so every backend sets every
field explicitly.
_Avoid_: model impersonation (the anti-pattern this replaced).

**Radio family status** — one of:
- **experimental**: core receive and transmit work; some controls, meters and
  radio-specific features may be incomplete. Carries an in-app notice.
- **supported**: the family has met its promotion gate and is a first-class
  target. FlexRadio is the reference.
- **verified** (Icom only): a specific model has been checked against its own
  CI-V guide. An unverified model gets no scope and no transmit.

**Raw-IQ backend**:
A backend that ships raw IQ and nothing else, so the *client* owns an
engine-side WDSP chain (tune, decimate, demodulate, filter, AGC). HL2 is the
first.
_Contrast_: **cooked-audio backend** (Flex, Icom) — ships demodulated audio,
and for Flex a hardware spectrum.

**Level-triggered / edge-triggered producer**:
A status producer is *level-triggered* if it re-asserts its current state
periodically, *edge-triggered* if it only announces changes. Code that drops
an inbound update "because another will arrive" is correct only for the
level-triggered kind. The HL2 emits pan geometry once per tune (edge); Flex
re-echoes it continuously (level).

## HL2 backend

**DDC**:
A hardware digital down-converter in the HL2 gateware — one NCO plus a
decimation chain, producing one IQ stream. The HL2 runs up to N concurrently,
where N is read from discovery register `0x13` and capped by the link budget.

**The four index spaces**:
Four distinct integer identities that must never be derived from one another
arithmetically (PureSignal and diversity break the arithmetic):
- **DDC index** — the hardware down-converter slot;
- **WDSP channel index** — the demod channel; drawn from a shared 32-slot
  pool, so after a TX channel has come and gone, receiver 0 is routinely not
  channel 0;
- **analyzer id** — the spectrum/FFT instance;
- **UI receiver number** — what the operator sees ("RX1").

**Slice**:
An operator-facing tuned receiver — a frequency, mode, filter and AGC. On the
HL2 a slice sits at an offset *within* a DDC's passband; moving the slice does
not move the DDC NCO (which is the **pan centre**) unless the target would
leave the usable span.

**Flat memory** (vs per-receiver state):
A control whose live value is *per-receiver* but whose *remembered* value is a
single pair, seeded onto every receiver at the next connect to a new radio. The
AGC works this way — RX1 and RX2 can hold different modes and thresholds within
a session, but only one remembered pair survives a restart. Deliberate: an
operator's AGC is treated as a property of how they like to listen, like the TX
cut points.
_Avoid_: "there is only one AGC" — at runtime there is one per receiver.

**Silent fall-through**:
A mode the UI offers that the backend does not map, so it demodulates as
something else (usually USB) while the mode label still reads the original. The
HL2 mode list is backend-authoritative specifically to make this impossible.

**RQST/ACK**:
The HPSDR Protocol-1 request/acknowledge pattern — a single outstanding
request, no transaction id, matched by echo. Model it as a state machine, not
as RPC.

## Certification

**Certified by effect**:
A control or meter is certified only by an *observed* change in the world — a
tone lands on the expected pitch, forward power rises with drive, an unrelated
receiver hears the transmission. Reading back the value that was written proves
only that a model has a variable.
_Avoid_: readback, round-trip check.

**NEVER FED** (vs **IDLE**):
A meter with no reading is *IDLE* if it is transmit-only and correctly quiet
while receiving, *NEVER FED* if it is defined and no reading has ever arrived.
Both look like "no value" on a gauge; only one is a bug.
