# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.10.8

### In flight

- **aetherd — vendor-neutral radio backend** — extracting an
  `IRadioBackend` seam (`RadioCapabilities` + typed status/command deltas)
  so radio-family logic lives behind a stable interface instead of being
  woven through `RadioModel`. FlexBackend owns the Flex wire objects
  and threads, and the Panadapter / Slice / Meter / Transmit / Amp / Tuner
  status+command paths decode behind the seam (RFC steps 2.1–2.4). The seam
  now carries **seven** backends — `FlexBackend`, `HL2Backend`, `IcomCIV`, the
  synthetic `SimBackend`, two RX-only additions from v26.9.4,
  **AnanBackend** (openHPSDR Protocol 2, ANAN-G2) and **RtlSdrBackend**
  (`librtlsdr` + `fftw3f`), and the RX+TX **HackRfBackend** (v26.9.10), all
  three carrying the `experimental` notice HL2 wore before its own promotion. v26.9.4 also landed **Stage 3** of the control
  protocol — a local control server plus wire codec — a further step toward
  the headless-engine/thin-client split the RFC targets, though not the full
  split: UI code still consumes models directly, and that remains correct
  until the versioned protocol (RFC step 3+, ongoing) finishes replacing it.
  v26.10.8 put more controls behind declared capability records (tuning
  step, NR/ANF, recording, VOX, monitor, speech processor, CW speed and
  break-in, among others), so a control a backend can't honour says why
  instead of silently doing nothing.
- **Icom networked radios — early** — `IcomCIV` speaks CI-V inside the RS-BA1
  UDP transport, brought up in v26.8.2 against a live **IC-705** (RX, scope,
  transmit, and FT8 both decoding and spotting on PSK Reporter) and an
  **IC-7300** (RX, scope and stability; transmit unverified). Only the IC-705
  and IC-7300MK2 are `verified` against their own CI-V guides; an unknown model
  gets no scope and no transmit rather than optimistic defaults. v26.8.3 gave
  the backend a **command plane**: every meter read, control write,
  reconciliation poll and PTT transition goes through one CI-V scheduler with
  explicit priorities, coalescing and stale-reply rejection — written because a
  delayed PTT-OFF reply arriving after a newer PTT-ON was cutting transmit
  audio. It also completed the **IC-7300MK2** control surface (18
  operator-visible defects), fixed the **RS-BA1 lease renewal** that froze the
  panadapter at the 255→256 sequence boundary, made **DATA mode** actually reach
  the radio for DIGU/DIGL and DFM, and replaced the hardcoded `0xA4` connect
  address with a broadcast `19 00` query. **WSPR** transmits (20 PSK Reporter
  reception reports on the air), **PC Audio** switches the model-specific DATA OFF
  modulation input, and the built-in CW decoder opens on normalized `CWU`.
  v26.10.8 fixed how squelch, COMP and the preamp/ATT pair read back.
  Remaining: transmit confirmation beyond the 705, the per-model SET-menu item
  numbers the MOD Input check needs, audio gain/mute/pan, VOX and CW break-in, an
  automation verb making the modulation sources assertable without parsing Radio
  Health text, and the once-a-second FT8 transmit dropout still under
  investigation.
- **Workspace canvas — experimental** — RFC #4887 landed complete in v26.8.3,
  all seven phases: pans and applets as freely placed, resizable, layered items
  on a canvas that can span several top-level windows, with named workspaces,
  full-recall switching and radio-profile bindings. It is **off by default**, and
  an install that never enables it never gains a settings key. Remaining before
  the experimental label can come off: live cross-window drag (deferred this
  cycle — a cross-top-level reparent is the #2495/#4617/#4319 crash lineage, so
  moves go through one deliberate menu path for now), and field time on real
  stations against the Classic shell.
- **AppSettings nested-JSON refactor** — ~460 flat call sites today;
  the new pattern is one nested-JSON value per feature (Principle V).
  The storage layer moved to SQLite and the scoped feature-document store,
  BandStack and memory-bank fold-ins, and the Settings Browser all shipped in
  v26.8.1 (RFC #4603, PRs 1–6). New radio-scoped configuration lands as
  versioned feature documents in `radio_settings`; the remaining work is
  migrating the legacy flat keys feature-by-feature.
- **TX DSP chain visual rebuild** — stage-per-applet chain with the
  visual `CHAIN` widget as the primary entry point.
- **Flathub submission** — the AppStream metainfo and manpage landed in
  v26.6.4; the actual Flathub PR + manifest is the remaining step.

### Queued (next cycle)

- **Windows: AetherSDR keeps running after exit**
  ([#99](https://github.com/W5TSU/AetherSDR/issues/99)) — reported on
  v26.10.7 with a Hermes-Lite 2 on Windows 11. After the window closes, an
  AetherSDR process stays running, and it interferes with the next launch.
  Not yet diagnosed or checked against v26.10.8.
- **Hermes-Lite 2 — on-air check of v26.10.8's two level changes** — receive
  audio 12.04 dB quieter, and CW at full scale like TUNE (about 6 dB more at
  the same RF Power). Both were ported from upstream and haven't yet been
  checked on this fork's HL2: the CW carrier against TUNE on a power meter
  into a dummy load, and receive audio level at AF 100.
- **Upstream bug-fix sync (batch 6)** — batch 5 shipped in v26.10.8
  ([#100](https://github.com/W5TSU/AetherSDR/pull/100)). What it deferred
  is tracked in [#98](https://github.com/W5TSU/AetherSDR/issues/98):
  - upstream's RTL-SDR receive work (M1, WFM, multi-RX, SSB/AM, live PPM),
    which builds on the typed receive-intent seam the fork doesn't have;
  - three Flex items that need live verification: nickname confirm,
    per-slice Auto squelch, and SmartSDR 4.3 per-band RCA suppression;
  - the rest of the HL2 session memory: receiver set, notches, FFT AVG and
    the auto-gain law;
  - the release-pipeline rewrite and Qt 6.12, which need a dry-run tag.

  **Open upstream HL2 bugs likely shared here:**
  - [#6052](https://github.com/aethersdr/AetherSDR/issues/6052): each over
    opens with 90–125 ms of keyed silence.
  - [#5198](https://github.com/aethersdr/AetherSDR/issues/5198): SSB transmit
    distortion.
- **HackRF — check transmit on HF** — all 14 transmit modes passed on the
  air at 915.5 MHz on a HackRF Pro (v26.10.6), judged on an RTL-SDR by
  `tools/hackrf_tx_ota_test.py` ([guide](docs/HACKRF-TX-TEST.md)). HF is
  still unchecked, because the RTL-SDR can't receive there. It needs another
  receiver, such as the Hermes-Lite 2. The one HF attempt (2026-10-13,
  28.400 MHz into a dummy load) heard nothing in any mode, but it ran before
  the fix for a Pro transmitting about 70 dB too weak
  ([#76](https://github.com/W5TSU/AetherSDR/pull/76)), which very
  likely explains it. MOX in USB/LSB/DSB with no microphone audio transmits
  nothing by design. Use TUNE or a tone for the check.
- **HackRF — multi-slice receive** — the one goal of the HackRF design not
  yet shipped: several independent slices across the HackRF's wide capture,
  each with its own DDC. The backend is single-slice today. Tracked in
  [#79](https://github.com/W5TSU/AetherSDR/issues/79). Design:
  [`docs/superpowers/specs/2026-09-16-hackrf-backend-design.md`](docs/superpowers/specs/2026-09-16-hackrf-backend-design.md).
- **HL2 span-following FFT bin count** — named fast-follow from the
  Hermes-Lite 2 supported-promotion (`docs/adr/0001-hermes-lite-2-supported.md`,
  shipped v26.9.2). The other accepted cost from that ADR — the span-change
  rebuild blocking the GUI thread — is resolved (v26.9.4 moved it to the I/O
  thread); this one is still open.
- **KiwiSDR follow-ups** — WebSDR / OpenWebRX support on top of the shipped
  public-receiver browser (per-receiver passwords, idle-release, and
  waterfall polish landed in v26.7.2; warm audio through TX and the
  resume-after-TX-delay option in v26.8.1).
- **Extended region band plans** — DXCC entities outside IARU R1/R2/R3.
- **macOS VirtualAudioBridge audit** ([#2940](https://github.com/aethersdr/AetherSDR/issues/2940))
  — focused security review of the macOS shared-memory audio bridge.
  (The RigctlPty side is resolved — RigctlPty was removed in #3380.)
- **PureSignal on HL2** — TX predistortion/linearization using an RX
  feedback path to correct amplifier nonlinearity. Explicitly out of scope
  for the HL2 "supported" milestone
  ([`docs/adr/0001-hermes-lite-2-supported.md`](docs/adr/0001-hermes-lite-2-supported.md))
  and not yet designed — needs its own RFC (feedback path, predistortion
  algorithm, calibration UI) before implementation starts.
- **Decouple RADE from DAX** — `RADEEngine::feedRxAudio`/`feedTxAudio`
  already take plain PCM (`QByteArray`), but the integration wiring in
  `MainWindow_DigitalModes.cpp` sources that PCM exclusively from Flex's
  DAX: RX via `PanadapterStream::daxAudioReady` filtered by DAX channel,
  TX via `ensureDaxTxStream()`. A guard (lines 298–338) refuses to enable
  RADE at all without DAX, with a user-facing message naming FlexRadio
  explicitly. This is a codec-version-independent prerequisite for RADE on
  any non-Flex backend (Hermes-Lite 2 included) — RADE V1 and V2 both just
  need PCM, wherever it comes from. **RTTY and AX.25/AetherModem packet
  already prove the pattern**: both decode off the backend-agnostic
  `PanadapterStream::audioDataReady` / generic `feedAudio()` path instead
  of DAX, which is why they already work on HL2. Scope: give RADE the same
  generic audio source (RX and TX) as an alternative to DAX, gated on
  backend capability rather than a hardcoded Flex check.
- **RADE V2 on the main backend** — the vendored codec
  (`third_party/radae`, pinned at upstream commit `4da110a`) is V1-only and
  65 commits / nearly six months behind
  [W5TSU/rade_c](https://github.com/W5TSU/rade_c); upstream's `main` has
  since grown a full V2 encoder/decoder, OFDM demod, AGC and BPF behind
  `RADE_MODE_V2`. Scope: bump the vendored snapshot, add a V2 mode to
  `RADEEngine` (`src/core/RADEEngine.{h,cpp}`) alongside V1 rather than
  replacing it, and expose the mode choice through `RadeApplet`. The two V1
  phase-drift fixes (`a2d77e8`/`5cb2436`) landed in v26.10.8 as local
  patches on the pinned snapshot; a re-vendor must move the pin past them or
  re-apply them
  ([`third_party/radae/LOCAL_PATCHES.md`](third_party/radae/LOCAL_PATCHES.md)).
  Land and prove out on the main backend before any Hermes-Lite 2 rollout — HL2
  doesn't offer FreeDV/RADE at all today, and, independent of the DAX
  decoupling above, debugging a new codec generation and a new backend's
  audio path at the same time gives no known-good reference point.

### Larger feature requests (community backlog)

Substantial features requested on the
[issue tracker](https://github.com/aethersdr/AetherSDR/issues?q=is%3Aopen+label%3A%22New+Feature%22)
— captured here for visibility, **not yet scheduled**. 👍 the issue to signal demand.

**Extensibility**

- **Plugin subsystem** — loadable decoder/DSP extensions, e.g. FT8/FT4/WSPR
  ([#3474](https://github.com/aethersdr/AetherSDR/issues/3474)).
- **TX-audio VST plugin host**
  ([#662](https://github.com/aethersdr/AetherSDR/issues/662)).

**Multi-radio & remote operation**

- **Single instance, two radios** — multi-radio operation; the `RadioSession`
  aggregate landed as the foundation
  ([#3445](https://github.com/aethersdr/AetherSDR/issues/3445)).
- **AetherLink** — integrated mobile remote server with low-bandwidth transport
  and an Android client
  ([#3128](https://github.com/aethersdr/AetherSDR/issues/3128)).

**Client-side DSP**

- **AM co-channel canceller** for MW/SW DX
  ([#578](https://github.com/aethersdr/AetherSDR/issues/578)).
- **Beat-cancel** — heterodyne/carrier interference canceller
  ([#529](https://github.com/aethersdr/AetherSDR/issues/529)).
- **CQUAM AM-stereo decoder**
  ([#176](https://github.com/aethersdr/AetherSDR/issues/176)).

**Operating modes & spotting**

- **Band-traffic / band-opening monitor**
  ([#3114](https://github.com/aethersdr/AetherSDR/issues/3114)).
- **Advanced spot colouring** — DXCC status, LoTW activity, per-callsign worked
  status ([#2809](https://github.com/aethersdr/AetherSDR/issues/2809)).
- **Contest-optimized high-contrast GUI**
  ([#2893](https://github.com/aethersdr/AetherSDR/issues/2893)).
- **Client-side digital voice keyer (DVK)** with local audio playback
  ([#957](https://github.com/aethersdr/AetherSDR/issues/957)).

**Packet / APRS / mapping** (building on the new map engine + AFSK demod)

- **APRS digipeater** tab (MVP: WIDE1-1 fill-in)
  ([#3571](https://github.com/aethersdr/AetherSDR/issues/3571)).
- **Live NEXRAD / weather-radar tile overlay** on the map
  ([#3574](https://github.com/aethersdr/AetherSDR/issues/3574)).
- **IQ-stream transmission over TCI** for CW/RTTY skimmers
  ([#999](https://github.com/aethersdr/AetherSDR/issues/999)).

**Amplifier & tuner integrations**

- **RF2K+ / RF2K-S** PA ([#1902](https://github.com/aethersdr/AetherSDR/issues/1902)),
  **Palstar HF-Auto** ([#97](https://github.com/aethersdr/AetherSDR/issues/97)),
  **LDG** USB-serial tuner ([#2092](https://github.com/aethersdr/AetherSDR/issues/2092)),
  and **Icom AH4** tuner protocol ([#542](https://github.com/aethersdr/AetherSDR/issues/542)).

### Recently shipped

Highlights from the last 30 days — full list in
[`CHANGELOG.md`](CHANGELOG.md):

- **HackRF One — receive and transmit (experimental)** — a new backend that
  went from source-only to usable in a week: in the AppImages and DMGs
  (v26.9.11); a connect that no longer freezes the app while DSP is prepared,
  receive processing on its own thread, working PTT and TUNE, TX drive from
  RF Power with the amp off on transmit, frequency calibration, and an LNA
  control (v26.10.1); and timestamp-accurate, click-free CW transmit with
  break-in (v26.10.2); then drag and Ctrl+wheel zoom from 62.5 kHz with the
  slice kept apart from the view, plus fixes for two HackRF Pro hardware
  quirks: stations folding in near the capture edge, and three sample rates
  its firmware mis-tunes (v26.10.4); and every mode in both directions
  (SSB, digital, AM, DSB, FM, CW; WFM receive-only), with receive sidebands
  corrected, CW at the pitch, cleaner FM and a remembered AGC (v26.10.5);
  then an S-meter and spectrum in dBm, the full transmit drive, and full
  transmit power on a HackRF Pro, with all 14 transmit modes checked on the
  air (v26.10.6); then Windows builds with the HackRF bundled, and a clean
  disconnect and reconnect when it is unplugged mid-session (v26.10.7).
  Driving goal: FM amateur-satellite work. Fork-only.
- **Upstream fix sync, batch 5** — 82 of the 100 upstream fixes
  reviewed in [#98](https://github.com/W5TSU/AetherSDR/issues/98)
  (v26.10.8); the rest are deferred or don't apply. On
  the Hermes-Lite 2: receive audio 12 dB quieter (the WDSP panel gain was
  hot), CW at the same level as TUNE, hardware PTT shown as transmit, TUNE
  power per band, RIT/XIT, and discovery on every interface. Also: TUNE
  restores only the outputs it turned off, controls that did nothing on
  non-Flex radios now work or say why, and Flex, TCI, Icom and waterfall
  fixes.
- **Upstream fix sync, 75 commits** — the WDSP/DSP seam (real channel
  start/stop, a process-wide FFTW planner lock and the `IRadioBackend`
  threading contract); on the Hermes-Lite 2, a dBm reference from the AD9866
  datasheet and a watchdog that asks a silent radio to stream again; RX
  meters that read WDSP's average instead of peak-hold, so the noise floor no
  longer reads 11–14 dB high; and ANAN, TGXL, PGXL and SPE fixes (v26.9.10).
- **No more Windows crash on connect** — v26.10.1 and v26.10.2 closed on
  Windows every time a Hermes-Lite 2, or another radio whose signal
  processing runs on the PC, connected. Memory the bundled FFT library had
  allocated was freed under a different C runtime (v26.10.3).
- **Complete, signed releases with their notes** — each release carries its
  changelog section as its release notes, published when the tag is pushed,
  and RTL-SDR is in the Linux and macOS downloads; a release build now fails
  rather than ship without it or HackRF (v26.9.11). The x86_64 AppImage and
  the GPG signatures that v26.10.6 shipped without are back (v26.10.7).
- **JS8Call spots in SpotHub** — `RX.SPOT` and `RX.DIRECTED` decodes from
  JS8Call's TCP API become spots, with their own SpotHub tab. Receive-only
  (v26.9.10).

For older highlights (the Hermes-Lite 2's promotion to supported, the ANAN
and RTL-SDR backends, the workspace canvas, networked Icom radios and the
CI-V command scheduler, four HL2 receivers, client settings on SQLite, demo
mode, KiwiSDR receive and the public-receiver browser, SmartMTR TX meters,
the agent automation bridge, the accessibility pass, CAT/rigctld parity, and
packaging work) see [`CHANGELOG.md`](CHANGELOG.md).

## How to influence the roadmap

- **Open an issue** with the feature-request template if you want
  something specific. The AetherClaude orchestrator triages it within
  minutes.
- **Open a PR** if you've already built it — see
  [`CONTRIBUTING.md`](CONTRIBUTING.md). Most cleanup-class work
  AetherClaude can do autonomously; novel features benefit from a
  design discussion in the issue first.
- **Sponsor a feature** — email the project lead at
  `kk7gwy@aethersdr.com`. Sponsored work jumps the queue while
  remaining open-source.

This roadmap is intentionally short. Long roadmaps don't ship.
