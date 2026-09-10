# External Control & Interop Review

**Status:** analysis (not an ADR). **Feeds:** the next external-integration PR.

## 1. What AetherSDR already exposes

| Surface | Mechanism | File(s) | Notes |
|---|---|---|---|
| Rig control (server) | rigctld emulation | `src/core/RigctlProtocol.*`, `src/core/CatPort.*` | TCP + PTY; split VFO-B create-on-demand; `dump_state` matched to Hamlib 4.6.5 |
| Rig control (server) | Kenwood TS-2000 + FlexRadio ZZ | `src/core/SmartCatProtocol.*`, `SmartCatSession.*` | AI async push of FA/MD |
| Full radio + audio + IQ + spots | TCI v2 WebSocket server, port 50001 | `src/core/TciServer.*`, `TciProtocol.*` | `#ifdef HAVE_WEBSOCKETS` |
| Passband audio to apps | DAX virtual audio | `PipeWireAudioBridge.*` (Linux), `VirtualAudioBridge.*` + `hal-plugin/` (macOS), SmartSDR DAX (Windows) | RADIO_RATE 24k / PIPE_RATE 48k |
| Raw IQ to apps | DAX-IQ, 4 ch 24-192 kHz | `src/gui/DaxIqApplet.*` | |
| Spot ingest | WSJT-X UDP, N1MM+ UDP XML, DXLab SpotCollector UDP, DX Cluster / RBN TCP, POTA, EiBi, FreeDV | `WsjtxClient.*`, `N1MMSpotClient.*`, `SpotCollectorClient.*`, `DxClusterClient.*` | all receive-only |
| State publish | MQTT `aethersdr/radio/state` `{slice,freq,mode,tx}` | `src/gui/MainWindow.cpp` (~3313-3340), `MqttClient.*` | `#ifdef HAVE_MQTT` |
| Agent automation | MCP bridge over `QLocalServer` | `src/core/AutomationServer.*`, `tools/aether_mcp.py` | opt-in, off in production |

## 2. Per-program analysis

Columns: transport & port · direction · data · already covered · gap · effort · priority.

### WSJT-X
- **Transport/port:** CAT via Hamlib NET rigctl (TCP, dflt 4532) or TS-2000; soundcard audio; outbound status/decodes UDP multicast 224.0.0.1:2237.
- **Direction:** CAT both; audio both; UDP WSJT-X→AetherSDR.
- **Data:** freq, mode, PTT, split/VFO-B; passband audio; Decode lines, Status (dial freq, mode), DX call.
- **Covered:** rigctld with split VFO-B create-on-demand; DAX audio; `WsjtxClient` ingests Decode + Status.
- **Gap:** no *outbound* to WSJT-X (Reply / free-text / Halt Tx / Configure messages).
- **Effort:** medium (UDP encoder, `QDataStream` schema). **Priority:** medium — read path already works.

### JS8Call
- **Transport/port:** same CAT + audio model as WSJT-X, **plus** its own line-delimited TCP JSON API, dflt port **2442**.
- **Direction:** CAT both; audio both; TCP API both (RX.DIRECTED / RX.SPOT / RX.ACTIVITY out; TX.SEND_MESSAGE / RIG.SET_FREQ / STATION.GET_* in).
- **Data:** freq/mode/PTT; audio; directed messages, spots (SNR, grid, callsign), station status.
- **Covered:** rigctld + DAX (CAT + audio work today).
- **Gap:** **no JS8Call TCP JSON API client** — cannot ingest JS8 spots/messages into SpotHub or send messages.
- **Effort:** medium — one client class modeled on `N1MMSpotClient`. **Priority:** **HIGH** — spots→SpotHub matches existing architecture.

### fldigi
- **Transport/port:** CAT via rigctld or fldigi XML-RPC (dflt 7362); soundcard audio; fldigi XML-RPC server for text/macros.
- **Direction:** rig both; audio both; XML-RPC both.
- **Covered:** rigctld + DAX; `RigctlProtocol` already special-cases fldigi (`dump_state`, `RIG_TRN_OFF`).
- **Gap:** no fldigi XML-RPC client (text/modem control) — low value; CAT + audio suffice for most users.
- **Effort:** medium. **Priority:** low.

### GridTracker
- **Transport:** consumes WSJT-X's UDP multicast; can re-forward; no direct rig link.
- **Covered:** nothing GridTracker-specific — it reads the same WSJT-X UDP stream, so with WSJT-X running it already works alongside AetherSDR.
- **Gap:** meaningful only if AetherSDR gains native FT8 decode and wants GridTracker to see it as the decode source. Out of scope.
- **Effort:** high (needs decoder) / low (passthrough, no value). **Priority:** low — no realistic gap.

### N1MM+
- **Transport/port:** N1MM as CAT client via Hamlib/OmniRig; N1MM bandmap spots out via UDP XML (ingested); N1MM "RadioInfo" UDP broadcast out.
- **Direction:** CAT N1MM→radio; spots N1MM→AetherSDR (covered); RadioInfo N1MM→others.
- **Covered:** rigctld (N1MM "Hamlib" radio); `N1MMSpotClient` UDP XML ingest.
- **Gap:** AetherSDR could *emit* N1MM-compatible RadioInfo UDP so tools expecting an N1MM radio source follow the VFO with no CAT polling.
- **Effort:** low-medium (UDP XML emitter). **Priority:** low-medium.

### Log4OM
- **Transport:** CAT via OmniRig/Hamlib, **or native TCI client** (Log4OM v2), or inbound UDP for QSO/callsign.
- **Covered:** **TCI server** — Log4OM connects as a TCI client today.
- **Gap:** no ADIF/QSO push from AetherSDR (it is not a logger).
- **Effort:** n/a. **Priority:** low — TCI already bridges it.

### Ham Radio Deluxe (HRD)
- **Transport:** Rig Control via CAT (serial/rigctld); Logbook via TCP.
- **Covered:** TS-2000 / FlexCAT / rigctld — HRD uses the Kenwood TS-2000 profile against a CatPort.
- **Gap:** none material (rotator/sat out of scope).
- **Effort:** n/a. **Priority:** low — verify TS-2000 compatibility, document.

### DXLab (Commander / DXKeeper / SpotCollector)
- **Transport:** Commander is the CAT hub (serial/rigctld); SpotCollector UDP spot push (ingested, dflt 9999); DXKeeper logs.
- **Covered:** rigctld / TS-2000 for Commander; `SpotCollectorClient` ingest.
- **Gap:** none material. **Priority:** low.

### Cloudlog / Wavelog
- **Transport:** HTTP REST API — external clients POST radio freq/mode and QSOs.
- **Direction:** AetherSDR→Cloudlog would be outbound HTTP.
- **Covered:** nothing.
- **Gap:** no outbound HTTP to push current freq/mode to a Cloudlog/Wavelog instance.
- **Effort:** low (HTTP POST on tune + API-key setting). **Priority:** low-medium — arguably a logger's job. Falls out for free given a generic HTTP state endpoint + a small external script.

### HamClock / OpenHamClock
- **Transport:** polls a Hamlib rigctld (`-r`) or flrig for current frequency; also its own REST control API.
- **Direction:** HamClock→rigctld (reads freq) — one-way read is all it needs.
- **Covered:** **fully, today** — point HamClock at a CatPort in Rigctld dialect.
- **Gap:** none. **Effort:** doc only. **Priority:** doc — verify + write a how-to.

### MacLoggerDX
- **Transport:** CAT via Hamlib/rigctld or native; AppleScript.
- **Covered:** rigctld — `RigctlProtocol` already special-cases MacLoggerDX (`RIG_TRN_OFF`).
- **Gap:** none material. **Priority:** low — verify, document.

### Gpredict / SatPC32 (satellite Doppler)
- **Transport:** controls a Hamlib rigctld with rapid `set_freq` for Doppler, often dual-VFO uplink/downlink; SatPC32 via DDE / Hamlib.
- **Direction:** tracker→rigctld (fast freq writes), some reads.
- **Data:** continuously-updated freq, mode; sometimes two slices (TX/RX).
- **Covered:** rigctld `set_freq` / `set_split_freq`; VFO-B create-on-demand.
- **Gap:** sustained high-rate `set_freq` performance; full-duplex split handling for sat work; RIT vs real QSY. Needs a validation pass more than new code.
- **Effort:** low (validate) + medium if split-sat is broken. **Priority:** medium — real constituency, good CAT stress case.

### Generic Hamlib / rigctld / flrig consumers
- **Transport:** anything speaking NET rigctl to a CatPort (Rigctld dialect) or Kenwood TS-2000 over serial/PTY/TCP. "flrig consumers" = apps speaking flrig's XML-RPC (port 12345) — **not implemented**.
- **Covered:** rigctld + TS-2000 broadly.
- **Gap:** **no FlRig XML-RPC dialect.** Some apps (WSJT-X "FlRig", JTDX, some loggers) prefer flrig.
- **Effort:** medium — XML-RPC over HTTP, ~30 methods mapped to existing CatPort state; a natural 4th `CatDialect`.
- **Priority:** medium — one addition covers several "and others".

## 3. Ranked recommendations (next-integration candidates)

1. **JS8Call TCP JSON API client** — fits the spot-ingest pattern; clear high-value gap. ~1 client class.
2. **Read-only HTTP/JSON state endpoint** (`GET /state` → `{freq,mode,tx,slices}`) — smallest; cross-platform; no per-app coupling; unlocks HamClock-style dashboards, Cloudlog/Wavelog push via a small script, home automation. Ages best.
3. **FlRig XML-RPC dialect for CatPort** — one addition covers several "and others".
4. **Outbound WSJT-X UDP** — click-to-reply / Halt Tx from the panadapter; read path already exists.
5. **N1MM RadioInfo UDP emitter** — low effort, modest value.
6. **Satellite / Doppler CAT validation pass** — mostly testing.

**Not recommended:** GridTracker (no gap without native FT8), fldigi XML-RPC (CAT + audio suffice), HRD / DXLab / MacLoggerDX (covered — doc only), Log4OM (TCI already bridges).

Front-runners #1 and #2 should be treated as co-equal; pick when that PR is planned.

## 4. Documentation follow-ups (separate work)

- "Feeding HamClock / OpenHamClock from AetherSDR" (rigctld dialect CatPort).
- "Using AetherSDR with WSJT-X / JS8Call" — CAT (rigctld) + DAX audio walkthrough, per-OS audio-routing notes.
- "Using AetherSDR as a Hamlib radio in a logger (N1MM+, DXLab, HRD, MacLoggerDX)".
