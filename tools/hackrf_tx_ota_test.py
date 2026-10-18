#!/usr/bin/env python3
"""Over-the-air transmit test for AetherSDR's HackRF backend, received on an
RTL-SDR. Guide: docs/HACKRF-TX-TEST.md.

It launches AetherSDR with a private settings profile, connects the HackRF,
and keys every transmit mode in turn for a few seconds at low drive while
`rtl_sdr` records. tools/hackrf_tx_ota_analyze.py judges each recording
(sideband, carrier suppression, AM depth, FM deviation, CW timing, ...), and
the run folder ends up with a report, a WAV and a spectrum per mode.

    python3 tools/hackrf_tx_ota_test.py --callsign W5TSU

SAFETY. This transmits. You need a licence for the frequency (the default,
915.5 MHz, is in the US 33 cm amateur band) and must stay at the station.
  * Drive never exceeds --max-power (at most 80 %): the HackRF backend switches
    the RF amp on at 82 %, so the output stays near 50 mW or less.
  * Every key is a few seconds long and sits inside try/finally: unkey
    everything, then confirm the radio reports not transmitting within 1 s, or
    abort the run. SIGINT, SIGTERM and exit unkey too, and the bridge's own
    watchdog (AETHER_AUTOMATION_TX_MAX_MS=15000) drops a stuck PTT.
  * The callsign goes out in CW every 9 minutes and at the end.
"""

import argparse
import atexit
import contextlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import traceback
from dataclasses import asdict, dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hackrf_tx_ota_analyze as A  # noqa: E402
import hackrf_tx_ota_tci as T  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ALL_MODES = ["TUNE", "USB", "LSB", "DIGU", "DIGL", "RTTY", "CW", "CWR",
             "AM", "SAM", "DSB", "FM", "FMN", "DFM"]
DATA_MODES = ("DIGU", "DIGL", "RTTY", "DFM")       # audio over TCI
CW_MODES = ("CW", "CWR")
POWER_CAP = 80            # the HackRF backend switches the RF amp on at 82 %
TUNE_SLICE_MODE = "USB"   # the slice mode TUNE's carrier is keyed in
LEAD_S = 1.0              # recorded before the key: the capture's own noise floor
TAIL_S = 0.7
RAMP_BURST_S = 1.5
PTT_SETTLE_S = 0.2        # CW: PTT up before the first element, down after the last
MAX_BURST_S = 3.0
ID_INTERVAL_S = 540.0     # 9 minutes
BUSY_CHECK_S = 0.5
BUSY_RECHECKS = 5
BUSY_WAIT_S = 2.0
CLIP_LIMIT = 0.001
UNKEY_CONFIRM_S = 1.0
DATA_TONE_HZ = 1500.0
RTTY_MARK_HZ, RTTY_SPACE_HZ = 2125.0, 2295.0
# rtl_sdr -g accepts these R820T steps (rtl_test lists them).
R820T_GAINS = [0.0, 0.9, 1.4, 2.7, 3.7, 7.7, 8.7, 12.5, 14.4, 15.7, 16.6, 19.7, 20.7,
               22.9, 25.4, 28.0, 29.7, 32.8, 33.8, 36.4, 37.2, 38.6, 40.2, 42.1, 43.4,
               43.9, 44.5, 48.0, 49.6]
RTL_ERRORS = ("usb_claim_interface", "No supported devices", "Failed to open")


class PowerCapError(Exception):
    pass


class RtlError(Exception):
    pass


class BridgeError(Exception):
    pass


class AbortRun(Exception):
    pass


class SetupError(Exception):
    pass


# ── Configuration ──────────────────────────────────────────────────────────

@dataclass
class Config:
    callsign: str
    freq_hz: float = 915.5e6
    max_power: int = POWER_CAP
    start_power: int = 10
    tone_hz: float = 1000.0
    tone_db: float = -6.0
    data_level_db: float = -6.0
    cw_text: str = "TEST"
    cw_wpm: float = 15.0
    rtl_gain: float = 20.7
    rtl_rate: float = 2.048e6
    rtl_offset_hz: float = -400e3
    freq_tol_ppm: float = 25.0
    burst_s: float = 2.5
    out_dir: str = ""
    app_path: str = "build/AetherSDR"
    attach: str = ""
    headless: bool = False
    serial: str = ""
    tci_port: int = 50001
    modes: list = field(default_factory=lambda: list(ALL_MODES))


def parse_args(argv):
    ap = argparse.ArgumentParser(
        prog="hackrf_tx_ota_test.py",
        description="Key every AetherSDR HackRF transmit mode and judge it on an RTL-SDR.",
        epilog="Guide: docs/HACKRF-TX-TEST.md")
    ap.add_argument("--callsign", required=True, help="sent in CW every 9 minutes and at the end")
    ap.add_argument("--freq-mhz", type=float, default=915.5, help="TX frequency (default 915.5)")
    ap.add_argument("--max-power", type=int, default=POWER_CAP,
                    help="drive cap in %%, at most 80 (default 80)")
    ap.add_argument("--start-power", type=int, default=10, help="first power-ramp step in %% (default 10)")
    ap.add_argument("--modes", default=",".join(ALL_MODES),
                    help="comma-separated; TUNE always runs first (default: all 14)")
    ap.add_argument("--tone-hz", type=float, default=1000.0, help="voice-mode test tone (default 1000)")
    ap.add_argument("--tone-db", type=float, default=-6.0, help="test tone level in dB (default -6)")
    ap.add_argument("--data-level-db", type=float, default=-6.0,
                    help="TCI data-mode audio level in dBFS (default -6)")
    ap.add_argument("--cw-text", default="TEST", help="sent in the CW and CWR tests (default TEST)")
    ap.add_argument("--cw-wpm", type=float, default=15.0, help="CW speed (default 15)")
    ap.add_argument("--burst-s", type=float, default=2.5, help="key-down per mode, at most 3 s (default 2.5)")
    ap.add_argument("--rtl-gain", type=float, default=20.7, help="RTL-SDR gain in dB (default 20.7)")
    ap.add_argument("--rtl-rate", type=float, default=2.048e6, help="RTL-SDR sample rate (default 2048000)")
    ap.add_argument("--rtl-offset-hz", type=float, default=-400e3,
                    help="RTL-SDR centre minus TX, off its DC spike (default -400000)")
    ap.add_argument("--freq-tol-ppm", type=float, default=25.0,
                    help="absolute frequency tolerance, both radios' crystals (default 25)")
    ap.add_argument("--out-dir", default="", help="results folder (default ./hackrf-tx-ota-<time>)")
    ap.add_argument("--app", dest="app_path", default="build/AetherSDR", help="AetherSDR binary")
    ap.add_argument("--attach", default="",
                    help="use a running AetherSDR's automation socket instead of launching one")
    ap.add_argument("--headless", action="store_true", help="launch with QT_QPA_PLATFORM=offscreen")
    ap.add_argument("--serial", default="", help="HackRF serial (default: the first HackRF)")
    ap.add_argument("--tci-port", type=int, default=50001, help="AetherSDR TCI port (default 50001)")
    a = ap.parse_args(argv)

    if a.max_power > POWER_CAP:
        ap.error("--max-power above 80 would switch on the HackRF RF amp")
    if not 1 <= a.start_power <= a.max_power:
        ap.error("--start-power must be between 1 and --max-power")
    if not 0.5 <= a.burst_s <= MAX_BURST_S:
        ap.error("--burst-s must be between 0.5 and 3")
    if not 24.0 <= a.freq_mhz <= 1700.0:
        ap.error("--freq-mhz must be inside the RTL-SDR's range, 24-1700 MHz")
    callsign = a.callsign.strip().upper()
    for text, what in ((callsign, "--callsign"), (a.cw_text.upper(), "--cw-text")):
        if not text or any(ch not in A.MORSE and ch != " " for ch in text):
            ap.error(f"{what} must be letters, digits, '/' and spaces")
    wanted = {m.strip().upper() for m in a.modes.split(",") if m.strip()}
    unknown = wanted - set(ALL_MODES)
    if unknown:
        ap.error(f"unknown or receive-only mode(s): {', '.join(sorted(unknown))}")
    modes = [m for m in ALL_MODES if m in wanted | {"TUNE"}]
    out_dir = a.out_dir or os.path.join(os.getcwd(), time.strftime("hackrf-tx-ota-%Y%m%d-%H%M%S"))
    return Config(callsign=callsign, freq_hz=a.freq_mhz * 1e6, max_power=a.max_power,
                  start_power=a.start_power, tone_hz=a.tone_hz, tone_db=a.tone_db,
                  data_level_db=a.data_level_db, cw_text=a.cw_text.upper(), cw_wpm=a.cw_wpm,
                  rtl_gain=a.rtl_gain, rtl_rate=a.rtl_rate, rtl_offset_hz=a.rtl_offset_hz,
                  freq_tol_ppm=a.freq_tol_ppm, burst_s=a.burst_s, out_dir=os.path.abspath(out_dir),
                  app_path=a.app_path, attach=a.attach, headless=a.headless, serial=a.serial,
                  tci_port=a.tci_port, modes=modes)


# ── The radio, through the automation bridge ───────────────────────────────

class Radio:
    """AetherSDR's bridge, one verb per method. Every keying method returns
    only after the bridge said ok. `connect` makes a new Bridge; all_stop uses
    its own connection, because it may run from a signal handler while the
    main connection is in the middle of a request."""

    def __init__(self, connect, max_power, tci=None):
        self._connect = connect
        self._bridge = None
        self.max_power = max_power
        self.tci = tci

    def _req(self, obj, timeout=10.0):
        if self._bridge is None:
            self._bridge = self._connect()
        try:
            r = self._bridge.request(obj, timeout)
        except BaseException:
            self._drop()          # a half-read reply would desynchronise the next request
            raise
        if not r.get("ok"):
            raise BridgeError(f"{json.dumps(obj)}: {r.get('error', r)}")
        return r

    def _drop(self):
        try:
            if self._bridge is not None:
                self._bridge.close()
        except Exception:  # noqa: BLE001
            pass
        self._bridge = None

    def get(self, model):
        return self._req({"cmd": "get", "model": model})[model]

    # setup
    def set_mode(self, mode):
        self._req({"cmd": "slice", "action": "mode", "value": mode})
        got = self.get("slice").get("mode")
        if str(got).upper() != mode:
            raise BridgeError(f"slice mode is {got} after asking for {mode}")

    def set_freq_hz(self, hz):
        self._req({"cmd": "tune", "value": f"{hz / 1e6:.6f}"})
        got = self.get("slice").get("frequency")
        if got is None or abs(float(got) * 1e6 - hz) > 1.0:
            raise BridgeError(f"slice is at {got} MHz after tuning to {hz / 1e6:.6f}")

    def set_power(self, rf, tune):
        if rf > self.max_power or tune > self.max_power:
            raise PowerCapError(f"RF {rf} % / Tune {tune} %: above the {self.max_power} % cap")
        self._req({"cmd": "transmit", "action": "rfpower", "value": str(int(rf))})
        self._req({"cmd": "transmit", "action": "tunepower", "value": str(int(tune))})
        self.check_before_key()

    def check_before_key(self):
        """Refuse to key unless the radio is connected and both drive settings
        read back at or under the cap (a fresh profile starts at RF 100 %)."""
        if not self.get("radio").get("connected"):
            raise BridgeError("the radio is not connected")
        t = self.get("transmit")
        rf, tune = t.get("rfPower"), t.get("tunePower")
        if rf is None or tune is None or rf > self.max_power or tune > self.max_power:
            raise PowerCapError(f"RF Power {rf} % / Tune Power {tune} %: above the "
                                f"{self.max_power} % cap or unreadable; refusing to key")

    def has_data_audio(self):
        return self.tci is not None and self.tci.error is None

    def testtone(self, on, hz=1000.0, db=-6.0):
        if on:
            self._req({"cmd": "testtone", "action": "on", "value": f"{hz:g} {db:g}"})
        else:
            self._req({"cmd": "testtone", "action": "off"})

    # keying
    def ptt(self, on):
        self._req({"cmd": "key", "action": "ptt", "value": "on" if on else "off"})

    def tune_carrier(self, on):
        """TUNE is a toggle (`shortcut tune_toggle`): read `tuning` first."""
        if bool(self.get("transmit").get("tuning")) == on:
            return
        self._req({"cmd": "shortcut", "target": "tune_toggle"})
        if on:
            end = time.monotonic() + 1.0
            while not self.get("transmit").get("tuning"):
                if time.monotonic() > end:
                    raise BridgeError("TUNE did not start")
                time.sleep(0.02)

    def cw_key(self, down):
        """The straight key: the profile binds `cwkey` (KeyboardShortcutsEnabled)."""
        self._req({"cmd": "keyevent", "action": "press" if down else "release", "value": "cwkey"})

    def data_tx(self, source):
        """Key over TCI and feed `source` as TX audio; None unkeys."""
        if self.tci is None:
            raise BridgeError("no TCI connection for data-mode audio")
        if source is None:
            self.tci.stop_tx()
        else:
            self.tci.start_tx(source)

    # state
    def transmitting(self):
        r, t = self.get("radio"), self.get("transmit")
        return bool(r.get("transmitting") or t.get("transmitting") or t.get("tuning") or t.get("mox"))

    def snapshot(self):
        s, t = self.get("slice"), self.get("transmit")
        return {"mode": s.get("mode"), "freq_mhz": s.get("frequency"),
                "rf_power": t.get("rfPower"), "tune_power": t.get("tunePower")}

    def restore(self, snap):
        """Put back what snapshot() found. Never raises; returns what failed."""
        steps = [("mode", {"cmd": "slice", "action": "mode", "value": snap.get("mode")}),
                 ("frequency", {"cmd": "tune", "value": None if snap.get("freq_mhz") is None
                                else f"{float(snap['freq_mhz']):.6f}"}),
                 ("RF Power", {"cmd": "transmit", "action": "rfpower",
                               "value": None if snap.get("rf_power") is None else str(snap["rf_power"])}),
                 ("Tune Power", {"cmd": "transmit", "action": "tunepower",
                                 "value": None if snap.get("tune_power") is None else str(snap["tune_power"])})]
        notes = []
        for what, obj in steps:
            if obj["value"] is None:
                continue
            try:
                self._req(obj)
            except Exception as e:  # noqa: BLE001 — restore is best effort
                notes.append(f"could not restore {what}: {e}")
        return notes

    def all_stop(self):
        """Unkey everything, each step on its own; never raises."""
        if self.tci is not None:
            with contextlib.suppress(Exception):
                self.tci.stop_tx(lock_timeout=1.0)
        try:
            b = self._connect()
        except Exception:  # noqa: BLE001
            return
        for obj in ({"cmd": "keyevent", "action": "release", "value": "cwkey"},
                    {"cmd": "key", "action": "ptt", "value": "off"},
                    {"cmd": "testtone", "action": "off"}):
            with contextlib.suppress(Exception):
                b.request(obj, 5.0)
        with contextlib.suppress(Exception):
            if b.request({"cmd": "get", "model": "transmit"}, 5.0).get("transmit", {}).get("tuning"):
                b.request({"cmd": "shortcut", "target": "tune_toggle"}, 5.0)
        with contextlib.suppress(Exception):
            b.close()


# ── The RTL-SDR ────────────────────────────────────────────────────────────

class RtlCapture:
    def __init__(self, device=0):
        self.device = str(device)

    def preflight(self):
        """The dongle's name, or RtlError if it is missing or busy."""
        for tool in ("rtl_sdr", "rtl_test"):
            if shutil.which(tool) is None:
                raise RtlError(f"{tool} not found: install the rtl-sdr tools")
        try:
            r = subprocess.run(["rtl_test", "-t", "-d", self.device], capture_output=True,
                               text=True, timeout=20)
        except subprocess.TimeoutExpired as e:
            raise RtlError("rtl_test did not finish") from e
        out = r.stdout + r.stderr
        bad = next((ln for ln in out.splitlines() if any(m in ln for m in RTL_ERRORS)), None)
        if bad or "Found 0 device" in out:
            raise RtlError(bad or "no RTL-SDR found")
        for line in out.splitlines():
            line = line.strip()
            if line.startswith(f"{self.device}:"):
                return line.split(":", 1)[1].strip()
        return "RTL-SDR"

    def record(self, path, center_hz, rate, gain, seconds):
        """Start `rtl_sdr` and return once samples are reaching the file (it
        takes 0.4-0.6 s to start), so the caller's lead-in is really recorded."""
        log_path = path + ".log"
        with open(log_path, "w") as err:
            proc = subprocess.Popen(
                ["rtl_sdr", "-d", self.device, "-f", str(int(round(center_hz))),
                 "-s", str(int(round(rate))), "-g", f"{gain:g}",
                 "-n", str(int(round(seconds * rate))), path],
                stdout=subprocess.DEVNULL, stderr=err)
        proc.rtl_log = log_path
        end = time.monotonic() + 5.0
        while time.monotonic() < end and proc.poll() is None:
            if os.path.exists(path) and os.path.getsize(path) > 0:
                return proc
            time.sleep(0.01)
        with contextlib.suppress(Exception):
            proc.kill()
            proc.wait(2)
        raise RtlError(self._why(log_path) or "rtl_sdr wrote nothing within 5 s")

    def wait(self, proc, timeout=None):
        try:
            proc.wait(timeout)
        except subprocess.TimeoutExpired as e:
            proc.kill()
            proc.wait()
            raise RtlError("rtl_sdr did not finish") from e
        why = self._why(proc.rtl_log)
        if proc.returncode != 0 or why:
            raise RtlError(why or f"rtl_sdr exited {proc.returncode}")

    @staticmethod
    def _why(log_path):
        with contextlib.suppress(OSError), open(log_path) as f:
            for line in f:
                if any(m in line for m in RTL_ERRORS):
                    return line.strip()
        return None


def lower_gain(gain):
    lower = [g for g in R820T_GAINS if g < gain - 0.05]
    return lower[-1] if lower else None


# ── Launching AetherSDR ────────────────────────────────────────────────────

def profile_seed(cfg):
    """The private profile: no auto-connect, the straight key bound to F9
    (and keyboard shortcuts on, which the bridge's keyevent needs), and TCI
    on, which carries the data modes' audio."""
    return [("AutoConnectToLastRadio", "False"),
            ("KeyboardShortcutsEnabled", "True"),
            ("Shortcut_cwkey", "F9"),
            ("TciServer", json.dumps({"enabled": True, "port": cfg.tci_port}))]


def _app_binary(cfg):
    path = cfg.app_path
    if not os.path.isabs(path) and not os.path.exists(path):
        path = os.path.join(REPO, path)
    return path


def launch_app(cfg, popen=subprocess.Popen, runner=subprocess.run):
    """Start AetherSDR on a fresh private profile with TX allowed and capped.
    Returns the process and its automation socket path."""
    name = f"hackrf-tx-ota-{os.getpid()}-{time.time_ns()}"
    settings = os.path.join(cfg.out_dir, "profile")
    os.makedirs(settings, exist_ok=True)
    app = _app_binary(cfg)
    base = dict(os.environ, AETHER_SETTINGS_DIR=settings)
    for key, value in profile_seed(cfg):
        r = runner([app, "--config", "set", key, value], env=base, capture_output=True,
                   text=True, timeout=60)
        if r.returncode != 0:
            raise SetupError(f"could not seed {key} in the test profile: {r.stderr.strip()}")
    env = dict(base, AETHER_AUTOMATION="1", AETHER_AUTOMATION_ALLOW_TX="1",
               AETHER_AUTOMATION_TX_MAX_POWER=str(cfg.max_power),
               AETHER_AUTOMATION_TX_MAX_MS="15000", AETHER_AUTOMATION_SOCKET=name)
    if cfg.headless:
        env["QT_QPA_PLATFORM"] = "offscreen"
    with open(os.path.join(cfg.out_dir, "aethersdr.log"), "w") as log:
        proc = popen([app], env=env, stdout=log, stderr=subprocess.STDOUT)
    return proc, os.path.join(tempfile.gettempdir(), name)


def _bridge_class():
    from automation_probe import Bridge
    return Bridge


def wait_for_bridge(sock, seconds, proc=None):
    Bridge = _bridge_class()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if proc is not None and proc.poll() is not None:
            raise SetupError(f"AetherSDR exited ({proc.returncode}) before its bridge came up")
        if os.path.exists(sock):
            try:
                b = Bridge(sock)
                if b.request({"cmd": "whoami"}, 5.0).get("ok"):
                    return b
                b.close()
            except OSError:
                pass
        time.sleep(0.25)
    raise SetupError(f"no automation bridge at {sock} after {seconds:g} s")


def connect_hackrf(bridge, serial, log):
    """`connect list` → the HackRF (by serial if given) → connect and wait."""
    end = time.monotonic() + 15.0
    while True:
        radios = bridge.request({"cmd": "connect", "action": "list"}, 15.0).get("radios", [])
        hackrfs = [r for r in radios if str(r.get("family", "")).lower() == "hackrf"
                   and (not serial or r.get("serial") == serial)]
        if hackrfs or time.monotonic() > end:
            break
        time.sleep(1.0)
    if not hackrfs:
        raise SetupError("no HackRF in AetherSDR's connect list" + (f" with serial {serial}" if serial else ""))
    pick = hackrfs[0]
    log(f"connecting {pick.get('model', 'HackRF')} serial {pick.get('serial')}")
    r = bridge.request({"cmd": "connect", "action": "local", "value": f"serial {pick['serial']}"}, 15.0)
    if not r.get("ok"):
        raise SetupError(f"connect refused: {r.get('error')}")
    r = bridge.request({"cmd": "connect", "action": "wait", "value": "30000"}, 40.0)
    if not r.get("connected"):
        raise SetupError(f"the HackRF did not connect: {r.get('error', r)}")
    return pick


def quit_app(proc, sock):
    with contextlib.suppress(Exception):
        _bridge_class()(sock).request({"cmd": "invoke", "target": "Quit", "action": "trigger"}, 5.0)
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()


# ── The run ────────────────────────────────────────────────────────────────

@dataclass
class Capture:
    path: str
    spec: A.CaptureSpec
    nominal: tuple
    clip: float
    gain: float
    iq: object


def git_commit():
    try:
        r = subprocess.run(["git", "-C", REPO, "describe", "--always", "--dirty", "--abbrev=8"],
                           capture_output=True, text=True, timeout=10)
        return r.stdout.strip() or "unknown"
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"


def _measured(result, name):
    return next((c.measured for c in result.checks if c.name == name), None)


def _no_signal(mode, power):
    return A.ModeResult(mode, "fail", [A.CheckResult("signal", False, None, "a keyed transmission", "")],
                        [f"no signal received, even at the {power} % drive cap"])


class _Run:
    def __init__(self, cfg, radio, rtl, clock, log):
        self.cfg, self.radio, self.rtl, self.clock, self.log = cfg, radio, rtl, clock, log
        self.gain = cfg.rtl_gain
        self.power = None
        self.ref = A.Reference(0.0)
        self.last_id = None
        self.results = {m: None for m in cfg.modes}
        self.entries = {}
        self.extra = []           # results that are not modes (an aborted ID)
        self.current = "preflight"
        self.ramp = []

    # ── keying ──────────────────────────────────────────────────────────────

    @contextlib.contextmanager
    def keyed(self):
        """Every key: drive checked first; afterwards unkey everything and
        confirm, or abort the run."""
        self.radio.check_before_key()
        if self.last_id is None:
            self.last_id = self.clock.monotonic()     # the ID clock starts at the first key
        try:
            yield
        finally:
            self.radio.all_stop()
            self.confirm_unkeyed()

    def confirm_unkeyed(self):
        end = self.clock.monotonic() + UNKEY_CONFIRM_S
        while self.radio.transmitting():
            if self.clock.monotonic() >= end:
                self.radio.all_stop()
                raise AbortRun("the radio still reports transmitting 1 s after unkey")
            self.clock.sleep(0.1)

    def _wait_until(self, t):
        dt = t - self.clock.monotonic()
        if dt > 0:
            self.clock.sleep(dt)

    def send_morse(self, text):
        """Each edge at its own monotonic target, so bridge latency does not
        accumulate through the message."""
        t = self.clock.monotonic()
        for down, seconds in A.morse_elements(text, self.cfg.cw_wpm):
            self._wait_until(t)
            self.radio.cw_key(down)
            t += seconds
        self._wait_until(t)
        self.radio.cw_key(False)

    def key_tune(self, hold_s):
        def keyer():
            self.radio.tune_carrier(True)
            t_on = self.clock.monotonic()
            self.clock.sleep(hold_s)
            self.radio.tune_carrier(False)
            return t_on, self.clock.monotonic()
        return keyer, hold_s

    def key_voice(self):
        def keyer():
            self.radio.testtone(True, self.cfg.tone_hz, self.cfg.tone_db)
            self.radio.ptt(True)
            t_on = self.clock.monotonic()
            self.clock.sleep(self.cfg.burst_s)
            self.radio.ptt(False)
            t_off = self.clock.monotonic()
            self.radio.testtone(False)
            return t_on, t_off
        return keyer, self.cfg.burst_s

    def key_data(self, mode):
        level = 10 ** (self.cfg.data_level_db / 20.0)

        def keyer():
            if mode == "RTTY":
                source = T.fsk_source(RTTY_MARK_HZ, RTTY_SPACE_HZ, self.cfg.burst_s / 2, level)
            else:
                source = T.tone_source(self.tone_for(mode), level)
            self.radio.data_tx(source)
            t_on = self.clock.monotonic()
            self.clock.sleep(self.cfg.burst_s)
            self.radio.data_tx(None)
            return t_on, self.clock.monotonic()
        return keyer, self.cfg.burst_s

    def key_cw(self, text):
        length = sum(s for _, s in A.morse_elements(text, self.cfg.cw_wpm))

        def keyer():
            self.radio.ptt(True)
            self.clock.sleep(PTT_SETTLE_S)
            t_on = self.clock.monotonic()
            self.send_morse(text)
            t_off = self.clock.monotonic()
            self.clock.sleep(PTT_SETTLE_S)
            self.radio.ptt(False)
            return t_on, t_off
        return keyer, length + 2 * PTT_SETTLE_S

    def keyer_for(self, mode):
        if mode == "TUNE":
            return self.key_tune(self.cfg.burst_s)
        if mode in DATA_MODES:
            return self.key_data(mode)
        if mode in CW_MODES:
            return self.key_cw(self.cfg.cw_text)
        return self.key_voice()

    def tone_for(self, mode):
        if mode in ("DIGU", "DIGL"):
            return DATA_TONE_HZ
        if mode == "RTTY":
            return RTTY_MARK_HZ
        return self.cfg.tone_hz

    def send_id(self):
        label, self.current = self.current, "ID"
        self.log(f"ID: DE {self.cfg.callsign}")
        self.radio.set_mode("CW")
        keyer, _ = self.key_cw(f"DE {self.cfg.callsign}")
        with self.keyed():
            keyer()
        self.last_id = self.clock.monotonic()
        self.current = label

    def maybe_id(self):
        if self.last_id is not None and self.clock.monotonic() - self.last_id >= ID_INTERVAL_S:
            self.send_id()

    # ── recording ───────────────────────────────────────────────────────────

    def _spec(self):
        cfg = self.cfg
        return A.CaptureSpec(cfg.rtl_rate, cfg.freq_hz + cfg.rtl_offset_hz, cfg.freq_hz)

    def capture(self, rel, keyer, key_s):
        spec = self._spec()
        folder = os.path.join(self.cfg.out_dir, rel)
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "capture.cu8")
        seconds = LEAD_S + key_s + TAIL_S
        gain = self.gain
        proc = self.rtl.record(path, spec.rtl_center_hz, spec.sample_rate_hz, gain, seconds)
        t0 = self.clock.monotonic()
        try:
            self.clock.sleep(LEAD_S)
            with self.keyed():
                t_on, t_off = keyer()
        except BaseException:
            with contextlib.suppress(Exception):
                self.rtl.wait(proc, timeout=seconds + 10)
            raise
        self.rtl.wait(proc, timeout=seconds + 10)
        iq, clip = A.load_cu8(path)
        return Capture(path, spec, (t_on - t0, t_off - t0), clip, gain, iq)

    def channel_clear(self, folder):
        """A short unkeyed recording; anything 15 dB up within +/-30 kHz is
        another station. Re-check every 2 s, up to 5 times."""
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "busy-check.cu8")
        spec = self._spec()
        for attempt in range(1 + BUSY_RECHECKS):
            if attempt:
                self.clock.sleep(BUSY_WAIT_S)
            proc = self.rtl.record(path, spec.rtl_center_hz, spec.sample_rate_hz, self.gain, BUSY_CHECK_S)
            self.rtl.wait(proc, timeout=10)
            iq, _ = A.load_cu8(path)
            if not A.channel_busy(A.to_baseband(iq, spec), spec.sample_rate_hz):
                with contextlib.suppress(OSError):
                    os.remove(path)
                    os.remove(path + ".log")
                return True
            self.log(f"  channel busy (check {attempt + 1} of {1 + BUSY_RECHECKS})")
        return False

    # ── the sequence ────────────────────────────────────────────────────────

    def power_ramp(self):
        """TUNE bursts from start_power up in 10 % steps to the cap; stop at the
        first that reaches the SNR target. False: nothing heard at the cap."""
        cfg = self.cfg
        levels = sorted(set(range(cfg.start_power, cfg.max_power, 10)) | {cfg.max_power})
        self.radio.set_mode(TUNE_SLICE_MODE)
        target = A.THRESHOLDS["tune_snr_db"]
        snr = None
        for p in levels:
            self.radio.set_power(p, p)
            self.power = p
            keyer, key_s = self.key_tune(RAMP_BURST_S)
            cap = self.capture(os.path.join("ramp", f"{p:02d}"), keyer, key_s)
            if cap.clip > CLIP_LIMIT:
                lower = lower_gain(self.gain)
                self.ramp.append(f"{p} %: clipped")
                self.log(f"ramp {p} %: the RTL-SDR clipped; gain {self.gain:g} -> {lower} dB")
                if lower is not None:
                    self.gain = lower
                return True
            result, _ = A.measure_tune(cap.iq, cap.spec, cfg.freq_tol_ppm, nominal_s=cap.nominal)
            snr = _measured(result, "snr")
            self.ramp.append(f"{p} %: " + ("nothing heard" if snr is None else f"{snr:.1f} dB"))
            self.log(f"ramp {p} %: " + ("nothing heard" if snr is None else f"SNR {snr:.1f} dB"))
            if snr is not None and snr >= target:
                return True
        return snr is not None

    def test_mode(self, mode):
        cfg = self.cfg
        self.radio.set_mode(TUNE_SLICE_MODE if mode == "TUNE" else mode)
        if mode in DATA_MODES and not self.radio.has_data_audio():
            return A.ModeResult(mode, "skip", [], [
                "no TCI connection: data modes take their audio from AetherSDR's TCI server"])
        folder = os.path.join(cfg.out_dir, mode)
        if not self.channel_clear(folder):
            return A.ModeResult(mode, "skip", [], [
                f"channel busy: another signal within 30 kHz after {BUSY_RECHECKS} rechecks"])
        keyer, key_s = self.keyer_for(mode)
        notes = []
        cap = self.capture(mode, keyer, key_s)
        lower = lower_gain(self.gain) if cap.clip > CLIP_LIMIT else None
        if lower is not None:
            notes.append(f"RTL-SDR clipped at {self.gain:g} dB gain; repeated at {lower:g} dB")
            self.log(f"  clipped; repeating at {lower:g} dB")
            self.gain = lower
            cap = self.capture(mode, keyer, key_s)
        if mode == "TUNE":
            result, self.ref = A.measure_tune(cap.iq, cap.spec, cfg.freq_tol_ppm, nominal_s=cap.nominal)
        else:
            result = A.analyze_mode(mode, cap.iq, cap.spec, self.ref, tone_hz=self.tone_for(mode),
                                    cw_text=cfg.cw_text if mode in CW_MODES else None,
                                    cw_wpm=cfg.cw_wpm, nominal_s=cap.nominal)
            if mode == "FM":
                self.ref.fm_peak_dev_hz = A.fm_peak_deviation(result)
        # The same note order as --reanalyze builds, so both reports match.
        if cap.clip > CLIP_LIMIT:
            result.notes.append(f"RTL-SDR clipped {cap.clip * 100:.2f} % of samples")
        result.notes = notes + result.notes
        A.write_mode_artifacts(folder, mode,
                               A.to_baseband(cap.iq, cap.spec, offset_hz=self.ref.carrier_offset_hz),
                               cap.spec.sample_rate_hz, result)
        entry = {"mode": mode, "capture": os.path.relpath(cap.path, cfg.out_dir),
                 "spec": asdict(cap.spec), "nominal_s": [round(x, 4) for x in cap.nominal],
                 "tone_hz": self.tone_for(mode), "power": self.power, "rtl_gain_db": cap.gain,
                 "notes": notes}
        if mode in CW_MODES:
            entry.update(cw_text=cfg.cw_text, cw_wpm=cfg.cw_wpm)
        self.entries[mode] = entry
        return result

    def execute(self):
        cfg = self.cfg
        snap = None
        aborted = None
        try:
            try:
                self.log(f"RTL-SDR: {self.rtl.preflight()}")
            except RtlError as e:
                raise AbortRun(f"RTL-SDR: {e}") from e
            snap = self.radio.snapshot()
            self.radio.set_freq_hz(cfg.freq_hz)
            self.current = "TUNE"
            self.radio.set_mode(TUNE_SLICE_MODE)
            if not self.channel_clear(os.path.join(cfg.out_dir, "ramp")):
                for mode in cfg.modes:
                    self.results[mode] = A.ModeResult(mode, "skip", [], ["channel busy before the run"])
            elif not self.power_ramp():
                for mode in cfg.modes:
                    self.results[mode] = _no_signal(mode, cfg.max_power)
            else:
                self.log(f"drive {self.power} %, RTL-SDR gain {self.gain:g} dB")
                for mode in cfg.modes:
                    self.current = mode
                    if mode != "TUNE":
                        self.maybe_id()
                    self.log(f"{mode} ...")
                    r = self.results[mode] = self.test_mode(mode)
                    failed = [c.name for c in r.checks if not c.passed]
                    self.log(f"{mode}: {r.status}" + (f" ({', '.join(failed)})" if failed else ""))
            self.current = None
            if self.last_id is not None:
                self.send_id()
        except (Exception, KeyboardInterrupt) as e:  # noqa: BLE001 — any failure is an abort
            aborted = "interrupted" if isinstance(e, KeyboardInterrupt) else f"{type(e).__name__}: {e}"
            self.log(f"ABORT: {aborted}")
            if not isinstance(e, (AbortRun, BridgeError, PowerCapError, RtlError, SetupError,
                                  T.TciError, OSError, KeyboardInterrupt)):
                self.log(traceback.format_exc())
            self.radio.all_stop()
        finally:
            notes = self.radio.restore(snap) if snap is not None else []
            self.radio.all_stop()
            for n in notes:
                self.log(f"warning: {n}")
        if aborted:
            self._mark_abort(aborted)
        return self.finish()

    def _mark_abort(self, why):
        if self.current == "preflight":
            for mode in self.cfg.modes:
                self.results[mode] = A.ModeResult(mode, "abort", [], [why])
            return
        if self.current in self.results and self.results[self.current] is None:
            self.results[self.current] = A.ModeResult(self.current, "abort", [], [why])
        else:
            self.extra.append(A.ModeResult(self.current or "ID", "abort", [], [why]))
        for mode, r in self.results.items():
            if r is None:
                self.results[mode] = A.ModeResult(mode, "skip", [], ["not run: the run aborted"])

    def finish(self):
        cfg = self.cfg
        results = [self.results[m] for m in cfg.modes] + self.extra
        config = {"freq_mhz": cfg.freq_hz / 1e6, "freq_tol_ppm": cfg.freq_tol_ppm,
                  "callsign": cfg.callsign, "git_commit": git_commit(),
                  "max_power": cfg.max_power, "power_ramp": "; ".join(self.ramp) or "not run",
                  "tone_hz": cfg.tone_hz, "cw_text": cfg.cw_text, "cw_wpm": cfg.cw_wpm}
        modes = [self.entries.get(r.mode) or {"mode": r.mode, "status": r.status, "notes": r.notes}
                 for r in results]
        with open(os.path.join(cfg.out_dir, "run.json"), "w") as f:
            json.dump({"config": config, "modes": modes}, f, indent=2)
            f.write("\n")
        A.write_reports(cfg.out_dir, A.build_meta(config, list(self.entries.values()), results), results)
        code = A.exit_code(results)
        self.log("")
        for r in results:
            self.log(f"  {r.mode:5s} {r.status}")
        self.log(f"report: {os.path.join(cfg.out_dir, 'report.md')}  (exit {code})")
        return code


def run(cfg, radio, rtl, clock=time, log=print):
    """The whole test: preflight, power ramp, TUNE, every mode, the ID,
    restore, reports. Returns the exit code: 0 pass, 1 a check failed, 2 abort."""
    os.makedirs(cfg.out_dir, exist_ok=True)
    return _Run(cfg, radio, rtl, clock, log).execute()


# ── main ───────────────────────────────────────────────────────────────────

class _Log:
    def __init__(self, path):
        self._f = open(path, "a")

    def __call__(self, msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}" if msg else ""
        print(line, flush=True)
        self._f.write(line + "\n")
        self._f.flush()


def main(argv=None):
    cfg = parse_args(sys.argv[1:] if argv is None else argv)
    os.makedirs(cfg.out_dir, exist_ok=True)
    log = _Log(os.path.join(cfg.out_dir, "run.log"))
    state = {"radio": None}

    def stop(*_):
        if state["radio"] is not None:
            state["radio"].all_stop()

    def on_signal(signum, frame):
        stop()
        raise KeyboardInterrupt

    atexit.register(stop)
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    app, sock, tci = None, None, None
    try:
        rtl = RtlCapture()
        log(f"RTL-SDR: {rtl.preflight()}")
        if cfg.attach:
            sock = cfg.attach if os.sep in cfg.attach else os.path.join(tempfile.gettempdir(), cfg.attach)
        else:
            app, sock = launch_app(cfg)
            log(f"AetherSDR launched (pid {app.pid}), profile {os.path.join(cfg.out_dir, 'profile')}")
        bridge = wait_for_bridge(sock, 60.0, app)
        who = bridge.request({"cmd": "whoami"}, 10.0)
        if not who.get("txAllowed"):
            raise SetupError("AetherSDR does not allow automation TX (AETHER_AUTOMATION_ALLOW_TX=1)")
        radio_state = bridge.request({"cmd": "get", "model": "radio"}, 10.0).get("radio", {})
        if not (cfg.attach and radio_state.get("connected")
                and "hackrf" in str(radio_state.get("model", "")).lower()):
            connect_hackrf(bridge, cfg.serial, log)
        bridge.close()
        try:
            tci = T.TciClient(port=cfg.tci_port).connect()
        except (OSError, T.TciError) as e:
            log(f"warning: no TCI on port {cfg.tci_port} ({e}); data modes will be skipped")
        Bridge = _bridge_class()
        state["radio"] = radio = Radio(lambda: Bridge(sock), cfg.max_power, tci=tci)
        return run(cfg, radio, rtl, log=log)
    except KeyboardInterrupt:
        log("interrupted before the run started")
        return 2
    except (SetupError, RtlError, BridgeError, OSError) as e:
        log(f"setup failed: {e}")
        return 2
    finally:
        stop()
        if tci is not None:
            tci.close()
        if app is not None:
            quit_app(app, sock)
            log("AetherSDR closed")


if __name__ == "__main__":
    sys.exit(main())
