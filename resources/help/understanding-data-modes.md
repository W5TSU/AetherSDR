# Configuring Data Modes

## Why Digital Operation Feels More Complicated

Voice operation can often be understood as "microphone in, speaker out." Digital operation adds more moving parts:

- a radio-control path (CAT or TCI)
- a receive-audio path (DAX)
- a transmit-audio path (DAX)
- sometimes an IQ path
- sometimes a single integrated protocol such as TCI that carries everything

The goal is not to memorize every acronym. The goal is to keep each path visible
and predictable so you can wire things up once and get back to operating.

---

## The Digital Signal Paths Inside AetherSDR

### Control path

This is how outside software reads frequency, mode, PTT, and other radio state.
AetherSDR offers three options:

| Method | Transport | Protocol | Best for |
|---|---|---|---|
| CAT over TCP | TCP socket | Hamlib rigctld | WSJT-X, fldigi, JS8Call |
| CAT over TTY/PTY | Virtual serial port | Hamlib rigctld | Older apps that require a COM/serial port |
| TCI | WebSocket | TCI v2.0 | WSJT-X 3.0 (TCI mode), apps with native TCI support |

### Receive audio path (DAX RX)

This is how digital software hears the radio. AetherSDR creates virtual audio
devices that appear as microphone inputs to other applications.

### Transmit audio path (DAX TX)

This is how digital software sends audio back into the radio. AetherSDR creates
a virtual audio output device that captures audio from your digital program.

### IQ path (DAX IQ)

For applications that need raw baseband IQ instead of demodulated audio.
AetherSDR supports four DAX IQ channels with selectable sample rates
(24k, 48k, 96k, 192k).

---

## Where the External-Control Settings Live

You **configure** CAT, TCI, DAX and DAX-IQ in one place:

> **Radio Setup ▸ EXTERNAL CONTROL**

Open it from the menu bar with **Settings ▸ Radio Setup…** and pick the
**EXTERNAL CONTROL** category, or use the shortcut **Settings ▸ CAT & TCI…**
which jumps straight there. It has four pages:

| Page | What you set |
|---|---|
| **CAT** | *Enable CAT server*, plus a list of listeners — each with its own **Port**, protocol **Dialect** (Rigctld / TS-2000 / Flex) and **VFO A / VFO B** slice. "Add CAT port" / "Remove selected" manage the list (up to 8). |
| **TCI** | *Enable TCI server* and the WebSocket **Port** (default `50001`). |
| **DAX** | *Enable DAX* (the virtual-audio bridge). |
| **DAX-IQ** | per-channel sample rate (24k/48k/96k/192k) and enable. |

Each *Enable …* control both starts the service now and brings it back on the
next radio connection — there is no separate "autostart".

> **Older builds:** earlier versions put these in the applet drawer ("Enable
> TCP" / "Enable TTY" buttons) and in three **Settings ▸ Autostart …** menu
> items. Those are gone; everything is on the EXTERNAL CONTROL pages now.

### The drawer status tiles

The **CAT**, **TCI**, **DAX** and **IQ** buttons in the applet-panel drawer
still open small tiles, but these are now **status readouts**, not
configuration:

| Tile | Shows | Keeps |
|---|---|---|
| **CAT Control** | enabled indicator; one line per running listener (`port · dialect · client count`) | a **CAT settings…** link to the page |
| **TCI Server** | `(stopped)` or `:port (n clients)` | the RX1-4 / TX **gain meters + sliders**, a **TCI settings…** link |
| **DAX Audio** | slice ↔ channel labels | the RX/TX **gain sliders**, a **DAX settings…** link |
| **DAX IQ** | per-channel level meters + rate | — |

Keep the CAT/TCI tiles visible during setup — they answer "what is AetherSDR
actually serving right now?" Adjust gain from the TCI/DAX tiles; everything
else is on the settings pages.

---

## How CAT Works — TCP and Serial Port Emulation

### Adding a CAT listener

Each row on the **EXTERNAL CONTROL ▸ CAT** page is one **listener** — a TCP
socket (and, on Linux/macOS, a matching virtual serial device) speaking one
protocol dialect, bound to a VFO-A / VFO-B slice.

1. Open **Settings ▸ CAT & TCI…**.
2. Tick **Enable CAT server**.
3. On the default row (or click **Add CAT port**), set:
   - **Port** — default `4532`; any free port ≥ 1024.
   - **Dialect** — **Rigctld** for WSJT-X / JS8Call / fldigi / HamClock / most
     loggers; **TS-2000** for software that expects a Kenwood; **Flex** for
     SmartSDR-CAT clients.
   - **VFO A** — the slice this listener reports and tunes (slice index `0` =
     slice A). **VFO B** — `none`, unless the client does real split.
4. Close the dialog. The listener starts, and the **CAT Control** drawer tile
   lists it as `<port>  <dialect>  <n> clients`.

CAT over TCP works identically on **Linux, macOS, and Windows**. Point your
digital program's rig-control setting at `localhost:<port>` as
**Hamlib NET rigctl** / **rigctld**.

### The virtual serial device (Linux / macOS)

On Linux and macOS every enabled listener also exposes a PTY (pseudo-terminal)
for software that wants a serial port instead of a socket. The path is a
per-user symlink:

- **Linux (systemd):** `$XDG_RUNTIME_DIR/aethersdr/cat-<n>` (typically
  `/run/user/<uid>/aethersdr/cat-0`)
- **Linux (fallback):** `~/.cache/aethersdr/cat-<n>`
- **macOS:** `~/Library/Caches/AetherSDR/cat-<n>`

`<n>` is the listener's index in the list (0 for the first). The **CAT
Control** tile shows the resolved path — copy it from there. On Windows there
is no PTY; use TCP.

### What commands does CAT support?

AetherSDR's CAT interface implements the Hamlib rigctld command set, including:

- **Frequency:** get/set VFO frequency
- **Mode:** get/set operating mode and passband
- **PTT:** get/set push-to-talk
- **VFO:** VFO selection and split operation
- **CW:** CW keying via the `b` (send Morse) command

Mode mapping between AetherSDR and Hamlib:

| AetherSDR mode | Hamlib mode |
|---|---|
| USB | USB |
| LSB | LSB |
| DIGU | PKTUSB |
| DIGL | PKTLSB |
| CW | CW |
| AM | AM |
| FM | FM |
| RTTY | RTTY |

---

## How DAX Audio Devices Work

DAX (Digital Audio Exchange) creates virtual audio devices on your system so
that digital programs can receive audio from the radio and send audio back.

### How it works by platform

#### Linux (PipeWire / PulseAudio)

AetherSDR uses PulseAudio pipe modules (compatible with both PulseAudio and
PipeWire via `pipewire-pulse`) to create virtual audio devices:

- **RX devices** appear as audio *sources* (microphone inputs):
  `AetherSDR DAX 1` through `AetherSDR DAX 4`
- **TX device** appears as an audio *sink* (speaker output):
  `AetherSDR TX`

Audio format: **24 kHz, mono, 16-bit integer**.

**Requirements:** PipeWire with `pipewire-pulse`, or PulseAudio.

**Finding the devices:**

```bash
# List all audio sources (look for AetherSDR DAX)
pactl list sources short | grep -i aether

# List all audio sinks (look for AetherSDR TX)
pactl list sinks short | grep -i aether
```

In most applications, DAX devices will appear in the audio input/output
dropdown menus once DAX is enabled and the radio is connected.

#### macOS (Core Audio HAL plugin)

AetherSDR uses a Core Audio HAL plugin with shared memory to create virtual
audio devices:

- **RX devices** appear as audio inputs in System Preferences and app settings
- **TX device** appears as an audio output

Audio format: **24 kHz, stereo, 32-bit float** (shared memory ring buffer).

**Finding the devices:**

1. Open **System Settings > Sound** (or **System Preferences > Sound** on older macOS).
2. Look for devices named `AetherSDR DAX 1` through `AetherSDR DAX 4` under Input.
3. Look for `AetherSDR TX` under Output.
4. In your digital program's audio settings, select these devices.

#### Windows

AetherSDR does **not** ship its own DAX audio driver on Windows — the built-in
DAX bridge (**Enable DAX** on the EXTERNAL CONTROL ▸ DAX page) runs on
**macOS or Linux with PipeWire** only, and that page is hidden on Windows.
You still have two working paths for digital-mode audio:

- **TCI** (recommended) — carries both control and audio over a single WebSocket
  connection, with no virtual audio devices to install. See the TCI setup below.
- **FlexRadio's SmartSDR DAX** — install FlexRadio's own DAX drivers (from the
  SmartSDR installer). AetherSDR runs alongside them, and your digital program
  (including JTDX, which has no TCI support) uses the DAX audio devices those
  drivers create.

### DAX gain staging

The DAX Audio drawer tile keeps gain sliders for each DAX channel:

- **RX gain (DAX 1-4):** Controls the level of audio sent from the radio to
  your digital program. Start at 50% and adjust if decodes are poor or the
  program shows clipping.
- **TX gain:** Controls the level of audio sent from your digital program back
  to the radio. Keep this moderate to avoid an overdriven transmit signal.

> **Tip:** Digital software is much less tolerant of overdriven audio than
> voice. If decode quality is poor or your transmitted signal looks dirty,
> reduce DAX levels before changing anything else.

---

## Enabling the Services

Each service has a single **Enable …** control on its **Radio Setup ▸
EXTERNAL CONTROL** page:

- **Enable CAT server** — starts every configured listener.
- **Enable TCI server** — starts the WebSocket server on the configured port.
- **Enable DAX** — starts the virtual-audio bridge *(Linux with PipeWire, or
  macOS only)*.
- Per-channel **Enable** on the DAX-IQ page.

Ticking the box starts the service immediately, and the setting persists: on
the next launch, once you connect to a radio, AetherSDR re-applies it
automatically. There is nothing else to arm — the old three "Autostart …"
menu items are gone.

**On connect, with a service enabled:**

- **CAT:** each listener's TCP port (and, on Linux/macOS, its PTY symlink at
  `<runtime>/aethersdr/cat-<n>`) begins accepting connections. The CAT Control
  tile lists the running listeners.
- **TCI:** the WebSocket server starts on the configured port (default `50001`).
- **DAX:** the virtual audio devices are created after a short delay (about
  3 seconds, so the radio can finish session setup).

---

## Application Walkthroughs

### WSJT-X 3.0

WSJT-X is the most popular application for FT8, FT4, JT65, and other weak-signal
digital modes. WSJT-X 3.0 supports both traditional CAT+DAX and native TCI.

#### Option 1: CAT over TCP + DAX audio

This method works on **Linux and macOS**.

**Step 1 — Prepare AetherSDR**

1. Connect to the radio. **Do this before you open WSJT-X** — a CAT listener
   with no slice to report returns an error (see *Common Mistakes*).
2. Select or create a slice and set the mode to **DIGU** (for FT8/FT4) or the
   appropriate digital mode.
3. Confirm the slice owns transmit (TX indicator visible on the slice).
4. Assign the slice to **DAX channel 1** (DAX channel selector in the slice
   bar or the panadapter DAX overlay).
5. Open **Settings ▸ CAT & TCI…**. On the **CAT** page, tick **Enable CAT
   server**; on the default listener set **Port** `4532`, **Dialect**
   `Rigctld`, **VFO A** = your slice.
6. On the **DAX** page, tick **Enable DAX**. Close the dialog.

**Step 2 — Configure WSJT-X**

1. Open WSJT-X and go to **File > Settings** (or **Preferences** on macOS).
2. Go to the **Radio** tab:
   - **Rig:** select `Hamlib NET rigctl`
   - **Network Server:** `localhost:4532` (or whatever port you set)
   - **PTT Method:** `CAT`
   - **Mode:** `None` or `Data/Pkt`
   - **Split Operation:** `Fake It` (simplest; use `Rig` only if you also set
     VFO B to a second slice)
   - Click **Test CAT** — the button should turn green.
   - Click **Test PTT** — the radio should briefly key up.
3. Go to the **Audio** tab:
   - **Input (Soundcard):** select `AetherSDR DAX 1`
   - **Output (Soundcard):** select `AetherSDR TX`
4. Click **OK** to save settings.

**Step 3 — Verify**

1. You should see the WSJT-X waterfall filling with signals.
2. The **CAT Control** drawer tile should show `4532  Rigctld  1 client`.
3. The DAX 1 level meter (DAX Audio tile) should show activity.
4. To test transmit: click **Tune** in WSJT-X. The radio should key up and
   the TX meter should show level. Keep the DAX TX gain moderate.

#### Option 2: TCI (control + audio over one connection)

This method works on **Linux, macOS, and Windows** — no virtual audio devices
needed.

**Step 1 — Prepare AetherSDR**

1. Connect to the radio.
2. Select or create a slice and set the mode to **DIGU**.
3. Confirm the slice owns transmit.
4. Open **Settings ▸ CAT & TCI…**, go to the **TCI** page, tick **Enable TCI
   server**, note the **Port** (default `50001`).
5. You do **not** need to enable DAX — TCI carries audio internally.

**Step 2 — Configure WSJT-X 3.0**

1. Open WSJT-X and go to **File > Settings**.
2. Go to the **Radio** tab:
   - **Rig:** select `TCI Server`
   - **Network Server:** `localhost:50001`
   - **PTT Method:** `CAT`
   - Click **Test CAT** — should turn green.
3. Go to the **Audio** tab:
   - **Input:** select `TCI Audio` (or leave at default — TCI handles routing)
   - **Output:** select `TCI Audio`
4. Click **OK**.

**Step 3 — Verify**

1. The TCI Server tile should show `:50001 (1 client)`.
2. The WSJT-X waterfall should fill with signals.
3. Test transmit with **Tune** as above.

> **Windows users:** AetherSDR ships no DAX audio driver on Windows, so TCI is
> the recommended method — WSJT-X 3.0's TCI support gives you full control and
> audio through a single connection with nothing to install. If you need
> soundcard-style DAX (for example JTDX, which has no TCI support), install
> FlexRadio's SmartSDR DAX drivers and select those devices instead.

---

### JS8Call

JS8Call uses the same CAT + audio model as WSJT-X. It has **no TCI support**,
so the path is CAT over rigctld plus DAX audio (Linux/macOS), or the SmartSDR
DAX drivers on Windows.

**Step 1 — Prepare AetherSDR**

1. Connect to the radio **first**.
2. Select or create a slice, set it to **DIGU**, confirm it owns transmit,
   and assign it to **DAX channel 1**.
3. **Settings ▸ CAT & TCI…** → **CAT** page: **Enable CAT server**; default
   listener at **Port** `4532`, **Dialect** `Rigctld`, **VFO A** = your slice.
4. **DAX** page: **Enable DAX**.
5. Put the slice on a JS8 dial frequency (e.g. 7.078, 10.130, 14.078 MHz)
   with a ~2.7–3 kHz filter.

**Step 2 — Configure JS8Call**

1. **File ▸ Settings ▸ Radio**:
   - **Rig:** `Hamlib NET rigctl`
   - **Network Server:** `localhost:4532`
   - **PTT Method:** `CAT`
   - **Mode:** `Data/Pkt` (or `None` and set DIGU manually)
   - **Split Operation:** `Fake It`
   - Click **Test CAT** and **Test PTT** — both should go green.
2. **File ▸ Settings ▸ Audio**:
   - **Input:** `AetherSDR DAX 1`
   - **Output:** `AetherSDR TX`

**Step 3 — Verify**

1. JS8Call's waterfall fills; its RX level meter sits mid-scale (green) —
   trim with the **DAX RX gain** slider on the DAX tile, not in JS8Call.
2. The **CAT Control** tile shows `4532  Rigctld  1 client`.
3. Tuning in JS8Call moves the AetherSDR slice.

> JS8Call also has its own TCP JSON API (default port 2442) for messages and
> spot automation. That is unrelated to the radio connection above and is not
> needed to operate.

---

### Winlink with VARA

Winlink uses VARA (or VARA FM) as the modem, and VARA needs both CAT control
and audio routing. This walkthrough covers the CAT+DAX method.

> **Platform note:** VARA is a Windows application. On Linux, VARA can be run
> under Wine. On macOS, use a Windows VM. The CAT+DAX configuration is the same
> regardless of how VARA is running.

**Step 1 — Prepare AetherSDR**

1. Connect to the radio first.
2. Select or create a slice and set the mode to **DIGU** (for VARA HF) or
   **USB** depending on your VARA configuration.
3. Confirm the slice owns transmit.
4. Assign the slice to **DAX channel 1**.
5. **Settings ▸ CAT & TCI…** → **CAT** page: **Enable CAT server**; default
   listener at **Port** `4532`, **Dialect** `Rigctld`, **VFO A** = your slice.
6. **DAX** page: **Enable DAX**.

**Step 2 — Configure VARA**

1. Open VARA HF (or VARA FM).
2. Go to **Settings > Soundcard**:
   - **Input:** select `AetherSDR DAX 1`
   - **Output:** select `AetherSDR TX`
   - On Linux under Wine: you may need to configure the Wine audio to use
     PulseAudio and the devices will appear with their PulseAudio names.
3. Go to **Settings > PTT** (or **CAT Control** depending on VARA version):
   - VARA itself may not need CAT — Winlink handles rig control.
   - If VARA asks for PTT, set it to **VOX** or configure it through Winlink's
     CAT connection.

**Step 3 — Configure Winlink Express**

1. Open Winlink Express.
2. Select **VARA HF Winlink** as the session type.
3. Go to **Settings** (gear icon):
   - Under **Radio Setup / Rig Control**:
     - **Rig type:** `Hamlib NET rigctl`. (If Winlink offers only
       `Kenwood TS-2000`, set that listener's **Dialect** to **TS-2000** on
       the CAT page.)
     - If using TCP: **Host:** `localhost`, **Port:** `4532`
     - If using serial: **Port:** copy the listener's path shown in the
       CAT Control tile (e.g. `/run/user/1000/aethersdr/cat-0` on Linux)
   - Verify that Winlink can read the frequency from the radio.
4. Click **Start** to begin a Winlink session.

**Step 4 — Verify**

1. The CAT Control tile should show `1 client` on your listener.
2. When VARA transmits, the TX level meter in the DAX Audio tile should show
   activity.
3. When receiving, the DAX 1 meter should show activity and VARA's waterfall
   should display signals.

---

### fldigi with Hamlib

fldigi supports a wide range of digital modes (PSK31, RTTY, Olivia, etc.)
and integrates with Hamlib for rig control.

**Step 1 — Prepare AetherSDR**

1. Connect to the radio first.
2. Select or create a slice and set the mode to **DIGU** (for most digital
   modes) or **DIGL** / **RTTY** as appropriate.
3. Confirm the slice owns transmit.
4. Assign the slice to **DAX channel 1**.
5. **Settings ▸ CAT & TCI…** → **CAT** page: **Enable CAT server**; default
   listener at **Port** `4532`, **Dialect** `Rigctld`, **VFO A** = your slice.
6. **DAX** page: **Enable DAX**.

**Step 2 — Configure fldigi**

1. Open fldigi. If this is the first launch, the configuration wizard will run.
2. Go to **Configure > Config Dialog** (or the wizard will guide you).
3. Under **Rig Control > Hamlib**:
   - Check **Use Hamlib**.
   - **Rig:** select `NET rigctl` (Hamlib model 2, or search for "rigctl").
   - **Device:** `localhost:4532`
     - On some fldigi versions, you set the host and port separately:
       - **Address:** `localhost`
       - **Port:** `4532`
   - Click **Initialize** or **Connect**. The frequency display in fldigi
     should sync with the radio.

   **Alternative — using a serial port (Linux/macOS):**
   - Under **Rig Control > Hamlib**:
     - **Rig:** select `NET rigctl`
     - Or under **Rig Control > RigCAT** or **Rig Control > Hardware PTT**:
       - **Device:** copy the listener's path from the CAT Control tile
         (e.g. `/run/user/1000/aethersdr/cat-0` on Linux,
         `~/Library/Caches/AetherSDR/cat-0` on macOS)
       - **Baud rate:** does not matter for virtual serial ports, but
         set it to `9600` if the field is required.

4. Under **Audio > Devices** (or **Soundcard**):
   - **Capture (Input):** select `AetherSDR DAX 1`
     - On Linux with PulseAudio/PipeWire: select **PulseAudio** as the audio
       system, then choose `AetherSDR DAX 1` from the capture device list.
     - On macOS: select `AetherSDR DAX 1` from the PortAudio device list.
   - **Playback (Output):** select `AetherSDR TX`
5. Click **Save** and **Close**.

**Step 3 — Verify**

1. fldigi's waterfall should show signals from the radio.
2. The frequency display in fldigi should match the radio's frequency.
3. Changing frequency in fldigi should move the radio, and vice versa.
4. The CAT Control tile should show a connected CAT client.
5. To test transmit: type some text in the transmit pane and press the TX
   button (or Ctrl+T). The radio should key up and the DAX Audio tile TX meter
   should show level.

---

### HamClock / OpenHamClock

HamClock only needs to **read** the current frequency, to show band and
propagation context. It polls a Hamlib `rigctld`, so a plain CAT listener is
all AetherSDR has to provide — no audio, no DAX.

**AetherSDR:**

1. Connect to the radio.
2. **Settings ▸ CAT & TCI…** → **CAT** page: **Enable CAT server**; a listener
   at **Port** `4532`, **Dialect** `Rigctld`, **VFO A** = the slice you want
   HamClock to follow.

**HamClock:**

- Start it with the rigctld option pointed at AetherSDR:
  `hamclock -r localhost:4532` (or set the rig host/port in HamClock's setup
  pages). HamClock then reads AetherSDR's frequency and updates as you tune.

No transmit path is involved, so nothing else is required.

---

### Using AetherSDR as a Hamlib radio in a logger

N1MM+, DXLab Commander, Ham Radio Deluxe, MacLoggerDX, Log4OM, CQRLOG and
similar all treat AetherSDR as a **Hamlib "NET rigctl" radio** (some also
accept a **Kenwood TS-2000**). Frequency/mode tracking and click-to-tune from
the logger's bandmap work over one CAT listener; you only add DAX audio if the
logger also runs a digital-mode engine.

**AetherSDR:**

1. Connect to the radio.
2. **Settings ▸ CAT & TCI…** → **CAT** page: **Enable CAT server**; a listener
   at **Port** `4532`, **VFO A** = your operating slice. Set **Dialect** to
   **Rigctld** for a Hamlib logger, or **TS-2000** for one that only speaks
   Kenwood.

**Logger:**

- Rig / radio type: **Hamlib NET rigctl** (or **Kenwood TS-2000**), host
  `localhost`, port `4532`. On Linux/macOS a logger that wants a serial port
  can use the listener's PTY path from the CAT Control tile instead.
- Log4OM v2 can alternatively connect as a **TCI client** — enable the TCI
  server instead and point Log4OM at `localhost:50001`.

Spot ingestion (DX Cluster, RBN, WSJT-X, N1MM bandmap) is a separate feature —
see **Settings ▸ SpotHub…**.

---

## Important Terms

### DIGU and DIGL

Digital sideband modes. They give digital software a predictable transmit and
receive environment without the voice-processing assumptions of SSB phone modes.
Use **DIGU** for most digital modes (FT8, PSK31, etc.). Use **DIGL** for
modes that conventionally use lower sideband.

### DAX

Digital Audio Exchange — the software patch cable between the radio session and
your digital program. Creates virtual audio devices on your system.

### CAT

Computer Aided Transceiver — rig control. A digital program uses CAT to read
and set frequency, mode, and PTT.

### TTY / PTY

The virtual serial port version of CAT. Some older or more rigid software
expects a serial port instead of a TCP socket. AetherSDR creates Unix
pseudo-terminals that behave like real serial ports.

### TCI

Transceiver Control Interface — a more integrated network protocol that carries
control, audio, IQ, CW, and spot data through one WebSocket connection. When
the client application supports TCI, it eliminates the need for separate CAT
and audio routing.

### DAX IQ

Raw IQ streaming for applications that need baseband data rather than
demodulated audio (e.g. SDR receivers, digital mode research tools).

---

## How AetherSDR Maps Channels

CAT listeners are **not** fixed to a slice letter — each one carries its own
**Port**, **Dialect** and **VFO A / VFO B** assignment, so you decide the
routing. DAX audio channels 1-4 still line up with the slice workflow:

| Slice | CAT listener | DAX audio |
|---|---|---|
| A | your first listener (default `4532`, `Rigctld`), **VFO A** = slice A | DAX 1 |
| B | add a listener on `4533`, **VFO A** = slice B | DAX 2 |
| C | add a listener on `4534`, **VFO A** = slice C | DAX 3 |
| D | add a listener on `4535`, **VFO A** = slice D | DAX 4 |

The virtual serial device for listener *n* is
`<runtime>/aethersdr/cat-<n>` (index `0` for the first) — see *The virtual
serial device* above for how `<runtime>` resolves per platform. Keep your
port numbering consistent with your slice usage so you don't rediscover the
routing every session.

---

## First Digital Setup Checklist

Use this sequence the first time you integrate a new digital application:

1. Connect to the radio. **(Do this before you start the other app.)**
2. Create or select the slice you want for digital work.
3. Set the slice to the correct digital mode (usually **DIGU** or **DIGL**).
4. Confirm that the correct slice owns transmit.
5. Assign the slice to a DAX channel (1-4).
6. Open **Settings ▸ CAT & TCI…** (or Radio Setup ▸ EXTERNAL CONTROL).
7. **Enable CAT server** (with a listener on your slice) *or* **Enable TCI
   server**, depending on what your application expects.
8. **Enable DAX** if your workflow needs virtual audio devices.
9. Configure your digital application to point at the correct CAT endpoint
   and audio devices.
10. **Test receive first** — verify decodes or waterfall before touching transmit.
11. **Test transmit** only after receive, frequency tracking, and slice
    assignment all make sense.

---

## Gain Staging

Bad gain staging creates many fake "protocol" problems. If decode quality is
poor or your transmitted signal looks dirty:

1. Reduce the problem to one slice and one digital application.
2. Set DAX RX and TX gain sliders to 50% as a starting point.
3. Avoid clipping in the digital program's audio meter.
4. Avoid excessive transmit processing in the voice path.
5. Test with a short transmission and check your signal on a monitor.

---

## Keep the Window Readable During Digital Sessions

A good digital layout keeps these items on screen:

- the active slice and its mode
- the data mode tiles (CAT Control, DAX Audio, TCI Server, DAX IQ as needed)
- the TX applet
- the status bar network and TX indicators
- at least one visible panadapter

If you collapse too much into Minimal Mode too early, you may hide the clues
you need to diagnose a routing problem.

---

## Common Mistakes

### "Rig failure" / the program can't read the frequency

**Cause:** AetherSDR is **not connected to a radio**. A CAT listener with no
receiver slice has nothing to report, so `get_freq` returns Hamlib
`RPRT -8` — which JS8Call / WSJT-X render as a scary "Rig failure" dump.

**Fix:** Connect AetherSDR to the radio first, confirm a slice with a real
frequency, then start (or re-run "Test CAT" in) the external program. Also
check the listener's **VFO A** points at an existing slice (index `0` =
slice A).

### The software connects, but the wrong slice moves

**Cause:** The listener's **VFO A** is set to a different slice than the one
you are watching.

**Fix:** On **Settings ▸ CAT & TCI… ▸ CAT**, set that listener's **VFO A** to
the slice you want it to control, or connect the program to the listener whose
VFO A already matches.

### Receive works, but transmit goes nowhere

**Cause:** Wrong transmit slice, wrong DAX transmit path, or wrong audio device
selected in the external program.

**Fix:** Confirm TX ownership first (check which slice has the TX indicator),
then confirm the application's transmit audio is routed to `AetherSDR TX`.

### CAT works, but there is no decode audio

**Cause:** The radio-control path is correct but the DAX receive audio path
is not.

**Fix:** Leave CAT alone. Check that **Enable DAX** is ticked on the
EXTERNAL CONTROL ▸ DAX page and that the application's audio input is set to
the correct `AetherSDR DAX` channel.

### Audio is present, but the application does not tune the radio

**Cause:** DAX is right, but CAT or TCI is wrong or not connected.

**Fix:** Leave audio alone. Check the CAT Control tile for a connected CAT client.
Verify the port number and host in your application's rig control settings.

### No DAX devices appear in the audio settings

**Cause:** DAX is not enabled, the radio is not connected, or (on Linux) the
PulseAudio/PipeWire service is not running.

**Fix:**
- Verify the radio is connected.
- Tick **Enable DAX** on the EXTERNAL CONTROL ▸ DAX page.
- On Linux: run `pactl list sources short` to check if the devices exist.
- On macOS: check **System Settings > Sound > Input** for AetherSDR devices.
- On Windows: AetherSDR ships no DAX driver — use TCI, or FlexRadio's SmartSDR
  DAX drivers, and check those devices instead.

### Everything works locally, but remote digital operation is choppy

**Cause:** WAN latency, audio compression, or too much visual and streaming
load.

**Fix:** Simplify the session, reduce extra panadapters, and check network
quality before changing digital settings.

---

## A Safe Troubleshooting Order

When a digital setup fails, isolate one layer at a time:

1. Verify the slice and mode.
2. Verify CAT, TTY, or TCI control (can the app read the frequency?).
3. Verify receive audio (does the app's waterfall show signals?).
4. Verify transmit audio (does the radio key up and show TX power?).
5. Verify transmit ownership (is the correct slice assigned TX?).
6. Verify IQ only if the workflow actually needs it.

This order is faster than changing ports, channels, modes, and audio devices
all at once.
