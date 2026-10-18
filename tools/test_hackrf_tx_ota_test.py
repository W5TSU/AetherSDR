#!/usr/bin/env python3
"""Offline tests for the HackRF over-the-air TX test's orchestrator
(tools/hackrf_tx_ota_test.py) and its TCI audio client
(tools/hackrf_tx_ota_tci.py). Nothing here touches a radio, an RTL-SDR or
AetherSDR: the radio, the dongle, the bridge, the TCI server and the clock are
all fakes, and the captures are synthesised from what the fake radio was told
to do and when.

These are mostly the safety guarantees: the power cap, unkey confirmation,
Ctrl-C mid-burst, the station ID, and what happens when nothing is received.
"""

import base64
import hashlib
import json
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import numpy as np
    import scipy  # noqa: F401
except ImportError:
    print("SKIP: numpy/scipy not installed")
    sys.exit(77)

import hackrf_tx_ota_analyze as A  # noqa: E402
import hackrf_tx_ota_fixtures as F  # noqa: E402
import hackrf_tx_ota_tci as T  # noqa: E402
import hackrf_tx_ota_test as H  # noqa: E402

# Small, fast captures: every analysis takes the rate from the CaptureSpec.
FAST = ["--rtl-rate", "512000", "--rtl-offset-hz", "-100000", "--burst-s", "1.0"]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


# ── Fakes ──────────────────────────────────────────────────────────────────

class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def monotonic(self):
        return self.t

    def sleep(self, seconds):
        self.t += max(0.0, seconds)


KEY_CALLS = ("ptt", "tune_carrier", "cw_key", "data_tx")


class FakeRadio:
    """Records every call with the fake clock's time. transmitting() follows
    the keying calls; stuck=True makes it stay True from the first unkey on."""

    def __init__(self, clock, max_power=80, stuck=False, mode_advance_s=0.0):
        self.clock, self.max_power = clock, max_power
        self.stuck, self.mode_advance_s = stuck, mode_advance_s
        self.calls = []
        self.mode, self.freq_hz, self.rf, self.tune = "WFM", 100e6, 100, 50
        self.keyed = set()
        self._stuck_now = False

    def _log(self, name, *args):
        self.calls.append((name, args, self.clock.t))

    def set_mode(self, mode):
        self._log("set_mode", mode)
        self.mode = mode
        self.clock.t += self.mode_advance_s

    def set_freq_hz(self, hz):
        self._log("set_freq_hz", hz)
        self.freq_hz = hz

    def set_power(self, rf, tune):
        if rf > self.max_power or tune > self.max_power:
            raise H.PowerCapError(f"{rf}/{tune} above {self.max_power}")
        self._log("set_power", rf, tune)
        self.rf, self.tune = rf, tune

    def check_before_key(self):
        self._log("check_before_key")
        if self.rf > self.max_power or self.tune > self.max_power:
            raise H.PowerCapError("above the cap")

    def has_data_audio(self):
        return True

    def testtone(self, on, hz=1000.0, db=-6.0):
        self._log("testtone", on, hz, db)

    def _key(self, name, on):
        self._log(name, on)
        if on:
            self.keyed.add(name)
        else:
            self.keyed.discard(name)
            if self.stuck:
                self._stuck_now = True

    def ptt(self, on):
        self._key("ptt", on)

    def tune_carrier(self, on):
        self._key("tune_carrier", on)

    def cw_key(self, down):
        self._key("cw_key", down)

    def data_tx(self, source):
        self._key("data_tx", source is not None)

    def transmitting(self):
        return self._stuck_now or bool(self.keyed)

    def snapshot(self):
        self._log("snapshot")
        return {"mode": "USB", "freq_mhz": 14.2407, "rf_power": 50, "tune_power": 1}

    def restore(self, snap):
        self._log("restore", snap)
        return []

    def all_stop(self):
        self._log("all_stop")
        self.keyed.clear()

    # helpers for assertions
    def key_ons(self):
        return [c for c in self.calls if c[0] in KEY_CALLS and c[1][0]]

    def count(self, name):
        return sum(1 for c in self.calls if c[0] == name)


class FakeProc:
    def __init__(self, on_wait):
        self._on_wait = on_wait
        self.returncode = None

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        if self.returncode is None:
            self._on_wait()
            self.returncode = 0
        return 0


def _mode_signal(mode):
    return {
        "TUNE": F.carrier(), "USB": F.tone(1000.0), "LSB": F.tone(-1000.0),
        "DIGU": F.tone(1500.0), "DIGL": F.tone(-1500.0),
        "AM": F.am(0.5), "SAM": F.am(0.5), "DSB": F.dsb(),
        "FM": F.fm(4500.0), "DFM": F.fm(4500.0), "FMN": F.fm(2250.0),
    }.get(mode)


class FakeRtl:
    """Writes the capture when the recording is waited for, built from the
    fake radio's call log: the signal is on exactly while the radio was keyed.

    snr(power) gives the carrier's level over the noise in a 10 Hz bin;
    signal=False writes noise only; busy_modes holds an interferer on the
    channel while the radio is in one of those modes.
    """

    def __init__(self, radio, clock, snr=lambda power: 60.0, signal=True,
                 busy_modes=(), fail_preflight=False, interrupt_mode=None):
        self.radio, self.clock = radio, clock
        self.snr, self.signal = snr, signal
        self.busy_modes, self.fail_preflight = set(busy_modes), fail_preflight
        self.interrupt_mode = interrupt_mode
        self.records = []

    def preflight(self):
        if self.fail_preflight:
            raise H.RtlError("usb_claim_interface error -6")
        return "Fake NESDR"

    def record(self, path, center_hz, rate, gain, seconds):
        mode = self.radio.mode
        self.records.append((path, mode, seconds))
        if self.interrupt_mode and mode == self.interrupt_mode and seconds >= 1.0:
            raise KeyboardInterrupt
        start = self.clock.t
        start_calls = len(self.radio.calls)
        spec = A.CaptureSpec(rate, center_hz, self.radio.freq_hz)

        def write():
            iq = self._synth(spec, seconds, start, self.radio.calls[start_calls:], mode)
            F.write_cu8(path, iq)
        return FakeProc(write)

    def wait(self, proc, timeout=None):
        proc.wait(timeout)

    def _synth(self, spec, seconds, start, calls, mode):
        busy = mode in self.busy_modes and seconds < 1.0
        intervals, cw = [], []
        on_at = None
        for name, args, t in calls:
            if name in ("ptt", "tune_carrier", "data_tx"):
                if args[0] and on_at is None:
                    on_at = t
                elif not args[0] and on_at is not None:
                    intervals.append((on_at - start, t - start))
                    on_at = None
            elif name == "cw_key":
                cw.append((args[0], t - start))
        sig = _mode_signal("TUNE" if any(c[0] == "tune_carrier" for c in calls) else mode)
        if cw:
            sig = F.carrier()
            # keying edges straight from the cw_key calls
            downs, d0 = [], None
            for down, t in cw:
                if down and d0 is None:
                    d0 = t
                elif not down and d0 is not None:
                    downs.append((d0, t))
                    d0 = None
            intervals = downs

        def signal(t):
            env = np.zeros(t.shape)
            if self.signal and sig is not None:
                for a, b in intervals:
                    env[(t >= a) & (t < b)] = 1.0
            return (sig(t) if sig is not None else 0.0) * env

        extra = (lambda t: 10 ** (-20 / 20) * np.exp(2j * np.pi * 8e3 * t)) if busy else None
        return F.synth(signal, spec=spec, total_s=seconds, key_start_s=0.0, key_s=seconds,
                       snr_db=self.snr(self.radio.rf), extra=extra)


def make_run(tmp, extra_args=(), modes="TUNE,USB", **radio_kw):
    clock = FakeClock()
    cfg = H.parse_args(["--callsign", "w5tsu", "--out-dir", tmp, "--modes", modes,
                        *FAST, *extra_args])
    radio = FakeRadio(clock, **radio_kw)
    return cfg, radio, clock


def read_report(tmp):
    with open(os.path.join(tmp, "report.json")) as f:
        return json.load(f)


# ── Orchestrator: arguments and safety ────────────────────────────────────

def test_max_power_above_80_refused():
    import contextlib
    import io
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            H.parse_args(["--callsign", "W5TSU", "--max-power", "81"])
        except SystemExit as e:
            check(e.code == 2, f"exit code {e.code}")
        else:
            raise AssertionError("--max-power 81 accepted")
    bridge = FakeBridge()
    radio = H.Radio(lambda: bridge, max_power=80)
    try:
        radio.set_power(81, 10)
    except H.PowerCapError:
        pass
    else:
        raise AssertionError("set_power(81, 10) accepted with cap 80")
    check(not any(r.get("cmd") == "transmit" for r in bridge.sent), f"sent {bridge.sent}")


def test_max_power_message():
    import contextlib
    import io
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        try:
            H.parse_args(["--callsign", "W5TSU", "--max-power", "81"])
        except SystemExit:
            pass
    check("--max-power above 80 would switch on the HackRF RF amp" in err.getvalue(),
          f"stderr {err.getvalue()!r}")


def test_callsign_required():
    import contextlib
    import io
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            H.parse_args([])
        except SystemExit as e:
            check(e.code == 2, f"exit {e.code}")
            return
    raise AssertionError("no --callsign accepted")


def test_unkey_confirm_failure_aborts():
    with tempfile.TemporaryDirectory() as d:
        cfg, radio, clock = make_run(d, modes="TUNE,USB,LSB", stuck=True)
        code = H.run(cfg, radio, FakeRtl(radio, clock), clock=clock)
        check(code == 2, f"exit {code}")
        check(radio.count("all_stop") >= 2, f"all_stop x{radio.count('all_stop')}")
        first_off = next(i for i, c in enumerate(radio.calls)
                         if c[0] in KEY_CALLS and not c[1][0])
        later = [c for c in radio.calls[first_off:] if c[0] in KEY_CALLS and c[1][0]]
        check(not later, f"keyed after a failed unkey: {later}")
        rep = read_report(d)
        check(rep["exit_code"] == 2, rep)


def test_sigint_during_burst_unkeys():
    with tempfile.TemporaryDirectory() as d:
        # TUNE keys in USB slice mode, so interrupt the LSB burst instead.
        cfg, radio, clock = make_run(d, modes="TUNE,LSB,AM")
        rtl = FakeRtl(radio, clock, interrupt_mode="LSB")
        code = H.run(cfg, radio, rtl, clock=clock)
        check(code == 2, f"exit {code}")
        check(radio.count("all_stop") >= 1, "no all_stop")
        rep = read_report(d)
        statuses = {r["mode"]: r["status"] for r in rep["results"]}
        check(statuses == {"TUNE": "pass", "LSB": "abort", "AM": "skip"}, statuses)
        check(not any(c[0] == "set_mode" and c[1][0] == "AM" for c in radio.calls), "AM ran")


def test_rtl_open_failure_aborts_before_keying():
    with tempfile.TemporaryDirectory() as d:
        cfg, radio, clock = make_run(d)
        code = H.run(cfg, radio, FakeRtl(radio, clock, fail_preflight=True), clock=clock)
        check(code == 2, f"exit {code}")
        check(not radio.key_ons(), f"keyed: {radio.key_ons()}")
        check(not any(c[0] == "set_power" for c in radio.calls), "power changed")
        check(read_report(d)["exit_code"] == 2, "report")


def test_power_ramp_stops_at_target():
    with tempfile.TemporaryDirectory() as d:
        cfg, radio, clock = make_run(d)
        rtl = FakeRtl(radio, clock, snr=lambda p: 17.0 + p)   # 27, 37, 47, 57 dB
        code = H.run(cfg, radio, rtl, clock=clock)
        powers = [c[1] for c in radio.calls if c[0] == "set_power"]
        check(powers[:4] == [(10, 10), (20, 20), (30, 30), (40, 40)], f"ramp {powers}")
        check(all(p == (40, 40) for p in powers[3:]), f"after the ramp {powers}")
        check(code == 0, f"exit {code}: {read_report(d)}")


def test_run_with_no_signal_fails_cleanly():
    with tempfile.TemporaryDirectory() as d:
        cfg, radio, clock = make_run(d, modes="TUNE,USB,CW")
        code = H.run(cfg, radio, FakeRtl(radio, clock, signal=False), clock=clock)
        check(code == 1, f"exit {code}")
        rep = read_report(d)
        check([r["status"] for r in rep["results"]] == ["fail", "fail", "fail"], rep["results"])
        for r in rep["results"]:
            check(any("no signal received" in n for n in r["notes"]), r)
        powers = [c[1] for c in radio.calls if c[0] == "set_power"]
        check(max(p[0] for p in powers) == 80, f"ramp never reached the cap: {powers}")


def test_busy_channel_skips_after_five_rechecks():
    with tempfile.TemporaryDirectory() as d:
        cfg, radio, clock = make_run(d, modes="TUNE,USB,LSB")
        rtl = FakeRtl(radio, clock, busy_modes={"LSB"})
        code = H.run(cfg, radio, rtl, clock=clock)
        checks_in_lsb = [r for r in rtl.records if r[1] == "LSB" and r[2] < 1.0]
        check(len(checks_in_lsb) == 6, f"{len(checks_in_lsb)} busy checks (want 1 + 5 rechecks)")
        lsb_at = next(i for i, c in enumerate(radio.calls) if c[0] == "set_mode" and c[1][0] == "LSB")
        nxt = next((i for i, c in enumerate(radio.calls[lsb_at + 1:], lsb_at + 1)
                    if c[0] == "set_mode"), len(radio.calls))
        keyed_in_lsb = [c for c in radio.calls[lsb_at:nxt] if c[0] in KEY_CALLS and c[1][0]]
        check(not keyed_in_lsb, f"keyed while busy: {keyed_in_lsb}")
        statuses = {r["mode"]: r["status"] for r in read_report(d)["results"]}
        check(statuses == {"TUNE": "pass", "USB": "pass", "LSB": "skip"}, statuses)
        check(code == 0, f"exit {code}")


def _id_runs(radio):
    """Each CW ID as the list of (down, time) cw_key edges, split on PTT."""
    runs, cur, in_id = [], None, False
    for name, args, t in radio.calls:
        if name == "set_mode":
            in_id = args[0] == "CW"
        if not in_id:
            continue
        if name == "ptt" and args[0]:
            cur = []
        elif name == "cw_key" and cur is not None:
            cur.append((args[0], t))
        elif name == "ptt" and not args[0] and cur is not None:
            runs.append(cur)
            cur = None
    return runs


def test_id_sent_at_end_and_every_nine_minutes():
    with tempfile.TemporaryDirectory() as d:
        cfg, radio, clock = make_run(d, modes="TUNE,USB,LSB,AM,DSB,FM", mode_advance_s=120.0)
        code = H.run(cfg, radio, FakeRtl(radio, clock), clock=clock)
        check(code == 0, f"exit {code}: {read_report(d)}")
        ids = _id_runs(radio)
        check(len(ids) >= 2, f"{len(ids)} IDs")
        want = A.morse_elements("DE W5TSU", 15)
        for edges in ids:
            check([e[0] for e in edges] == [w[0] for w in want] + [False],
                  f"edges {[e[0] for e in edges]}")
            gaps = np.diff([e[1] for e in edges])
            check(np.allclose(gaps, [w[1] for w in want], atol=1e-6), f"timing {gaps}")
        fm_at = next(i for i, c in enumerate(radio.calls) if c[0] == "set_mode" and c[1][0] == "FM")
        fm_key = next(c[2] for c in radio.calls[fm_at:] if c[0] == "ptt" and c[1][0])
        check(ids[0][0][1] < fm_key, "no ID before the last mode")
        check(ids[-1][0][1] > fm_key, "no ID at the end")


def test_launch_uses_unique_socket():
    with tempfile.TemporaryDirectory() as d:
        cfg = H.parse_args(["--callsign", "W5TSU", "--out-dir", d, "--headless"])
        envs, seeds = [], []

        def popen(argv, env=None, **kw):
            envs.append(env)
            return FakeProc(lambda: None)

        def runner(argv, env=None, **kw):
            seeds.append((argv, env))

            class Done:
                returncode = 0
                stdout = stderr = ""
            return Done()

        _, sock1 = H.launch_app(cfg, popen=popen, runner=runner)
        _, sock2 = H.launch_app(cfg, popen=popen, runner=runner)
        check(sock1 != sock2, f"same socket {sock1}")
        a, b = envs
        check(a["AETHER_AUTOMATION_SOCKET"] != b["AETHER_AUTOMATION_SOCKET"], "same socket name")
        check(sock1.endswith(a["AETHER_AUTOMATION_SOCKET"]), f"{sock1} vs {a['AETHER_AUTOMATION_SOCKET']}")
        for env in envs:
            check(env["AETHER_AUTOMATION"] == "1" and env["AETHER_AUTOMATION_ALLOW_TX"] == "1", env)
            check(env["AETHER_AUTOMATION_TX_MAX_POWER"] == "80", env["AETHER_AUTOMATION_TX_MAX_POWER"])
            check(env["AETHER_AUTOMATION_TX_MAX_MS"] == "15000", env["AETHER_AUTOMATION_TX_MAX_MS"])
            check(env["QT_QPA_PLATFORM"] == "offscreen", "headless")
            check(env["AETHER_SETTINGS_DIR"].startswith(d), env["AETHER_SETTINGS_DIR"])
        seeded = {argv[argv.index("set") + 1]: argv[argv.index("set") + 2] for argv, _ in seeds}
        check(seeded.get("KeyboardShortcutsEnabled") == "True", seeded)
        check(seeded.get("AutoConnectToLastRadio") == "False", seeded)
        check(seeded.get("Shortcut_cwkey") == "F9", seeded)
        check(json.loads(seeded["TciServer"])["enabled"] is True, seeded)


def test_restore_after_run():
    with tempfile.TemporaryDirectory() as d:
        cfg, radio, clock = make_run(d)
        H.run(cfg, radio, FakeRtl(radio, clock), clock=clock)
        names = [c[0] for c in radio.calls]
        check(names[-2:] == ["restore", "all_stop"], f"last calls {names[-4:]}")
        check(radio.calls[-2][1][0]["mode"] == "USB", radio.calls[-2])


def test_rtl_sample_count_avoids_whole_buffers():
    # rtl_sdr only stops when fewer bytes are left than one 262144-byte
    # buffer: an exact multiple never stops (3.2 s at 2.048 MS/s is 50).
    for seconds in (0.5, 3.2, 4.2, 6.4, 12.8):
        n = H.rtl_sample_count(seconds, 2.048e6)
        check((2 * n) % H.RTL_BUFFER_BYTES != 0, f"{seconds} s -> {n} samples, whole buffers")
        check(abs(n - seconds * 2.048e6) <= 1, f"{seconds} s -> {n} samples")


# ── Radio: the bridge wrapper ──────────────────────────────────────────────

class FakeBridge:
    def __init__(self, transmit=None, radio=None, fail=False):
        self.sent = []
        self.transmit = transmit or {"rfPower": 10, "tunePower": 10, "transmitting": False,
                                     "tuning": False, "mox": False}
        self.radio = radio or {"transmitting": False, "connected": True}
        self.slice = {"mode": "USB", "frequency": 915.5}
        self.fail = fail

    def request(self, obj, timeout=None):
        self.sent.append(obj)
        if self.fail:
            raise ConnectionError("bridge gone")
        if obj.get("cmd") == "get":
            model = obj["model"]
            return {"ok": True, model: {"transmit": self.transmit, "radio": self.radio,
                                        "slice": self.slice}[model]}
        if obj.get("cmd") == "transmit":
            key = {"rfpower": "rfPower", "tunepower": "tunePower"}[obj["action"]]
            self.transmit[key] = int(obj["value"])
        return {"ok": True}

    def close(self):
        pass


def test_radio_restore_and_all_stop():
    bridge = FakeBridge()
    radio = H.Radio(lambda: bridge, max_power=80)
    notes = radio.restore({"mode": "LSB", "freq_mhz": 14.2407, "rf_power": 50, "tune_power": 1})
    check(notes == [], notes)
    want = [{"cmd": "slice", "action": "mode", "value": "LSB"},
            {"cmd": "tune", "value": "14.240700"},
            {"cmd": "transmit", "action": "rfpower", "value": "50"},
            {"cmd": "transmit", "action": "tunepower", "value": "1"}]
    check(bridge.sent == want, f"sent {bridge.sent}")
    dead = H.Radio(lambda: FakeBridge(fail=True), max_power=80)
    dead.all_stop()                                    # must not raise
    check(not H.Radio(lambda: bridge, max_power=80).transmitting(), "transmitting")
    bridge.transmit["tuning"] = True
    check(H.Radio(lambda: bridge, max_power=80).transmitting(), "TUNE not seen as transmitting")


def test_radio_refuses_to_key_above_cap():
    bridge = FakeBridge(transmit={"rfPower": 100, "tunePower": 10, "transmitting": False,
                                  "tuning": False, "mox": False})
    radio = H.Radio(lambda: bridge, max_power=80)
    try:
        radio.check_before_key()
    except H.PowerCapError:
        pass
    else:
        raise AssertionError("RF Power 100 % accepted for keying")


# ── TCI client ─────────────────────────────────────────────────────────────

class FakeTciServer:
    """A one-client WebSocket server speaking just enough TCI: the state
    burst ending in ready;, TX_CHRONO frames while keyed, and a log of what
    the client sent."""

    def __init__(self, chronos=6, floats=2048):
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.texts, self.audio, self.headers = [], [], []
        self.chronos, self.floats = chronos, floats
        self.done = threading.Event()
        threading.Thread(target=self._serve, daemon=True).start()

    def _send(self, conn, opcode, payload):
        n = len(payload)
        head = bytes([0x80 | opcode]) + (bytes([n]) if n < 126 else
                                         bytes([126]) + struct.pack(">H", n))
        conn.sendall(head + payload)

    def _recv_frame(self, conn, buf):
        def need(k):
            nonlocal buf
            while len(buf) < k:
                chunk = conn.recv(65536)
                if not chunk:
                    raise EOFError
                buf += chunk
        need(2)
        op, n, off = buf[0] & 0x0F, buf[1] & 0x7F, 2
        if n == 126:
            need(4)
            n, off = struct.unpack(">H", buf[2:4])[0], 4
        elif n == 127:
            need(10)
            n, off = struct.unpack(">Q", buf[2:10])[0], 10
        need(off + 4 + n)
        mask = buf[off:off + 4]
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(buf[off + 4:off + 4 + n]))
        return op, data, buf[off + 4 + n:]

    def _serve(self):
        conn, _ = self.srv.accept()
        req = b""
        while b"\r\n\r\n" not in req:
            req += conn.recv(4096)
        key = [ln.split(b":", 1)[1].strip() for ln in req.split(b"\r\n")
               if ln.lower().startswith(b"sec-websocket-key")][0]
        accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
        conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                     b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        self._send(conn, 1, b"protocol:ExpertSDR3,1.9;device:AetherSDR;")
        self._send(conn, 1, b"ready;")
        buf = b""
        try:
            while True:
                op, data, buf = self._recv_frame(conn, buf)
                if op == 1:
                    self.texts.append(data.decode())
                    if data == b"trx:0,true,tci;":
                        for _ in range(self.chronos):
                            head = struct.pack("<16I", 0, 48000, 3, 0, 0, self.floats, 3, 2, *([0] * 8))
                            self._send(conn, 2, head)
                    if data == b"trx:0,false;":
                        self.done.set()
                elif op == 2:
                    self.headers.append(struct.unpack("<16I", data[:64]))
                    self.audio.append(np.frombuffer(data[64:], dtype="<f4"))
                elif op == 8:
                    break
        except (EOFError, OSError):
            pass
        conn.close()


def _wait(pred, seconds=5.0):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(0.01)
    return False


def _dominant(samples):
    spec = np.abs(np.fft.rfft(samples * np.hanning(samples.size)))
    return np.fft.rfftfreq(samples.size, 1 / 48000.0)[np.argmax(spec)]


def test_tci_feeds_tone_in_answer_to_tx_chrono():
    srv = FakeTciServer(chronos=6)
    c = T.TciClient(port=srv.port).connect()
    try:
        c.start_tx(T.tone_source(1500.0, level=0.5))
        check(_wait(lambda: len(srv.audio) == 6), f"{len(srv.audio)} audio frames")
        c.stop_tx()
        check(_wait(srv.done.is_set), "no trx:0,false;")
    finally:
        c.close()
    check(srv.texts[:2] == ["audio_samplerate:48000;", "trx:0,true,tci;"], srv.texts)
    h = srv.headers[0]
    check(h[1] == 48000 and h[2] == 3 and h[5] == 2048 and h[6] == 2 and h[7] == 2, f"header {h}")
    stereo = np.concatenate(srv.audio)
    left, right = stereo[0::2], stereo[1::2]
    check(np.array_equal(left, right), "L != R")
    check(abs(_dominant(left) - 1500.0) < 30.0, f"tone at {_dominant(left)}")
    check(abs(np.max(np.abs(left)) - 0.5) < 0.01, f"level {np.max(np.abs(left))}")
    # phase-continuous across frames: no step bigger than one sample's advance
    check(np.max(np.abs(np.diff(left))) < 0.5 * 2 * np.pi * 1500 / 48000 * 1.01, "phase jump")


def test_tci_close_unkeys():
    srv = FakeTciServer(chronos=1)
    c = T.TciClient(port=srv.port).connect()
    c.start_tx(T.tone_source(1000.0))
    c.close()
    check(_wait(srv.done.is_set), f"close did not send trx:0,false; ({srv.texts})")


def test_fsk_source_halves():
    src = T.fsk_source(2125.0, 2295.0, half_s=0.5)
    x = np.concatenate([src(1024) for _ in range(int(48000 / 1024) + 1)])[:48000]
    check(abs(_dominant(x[:24000]) - 2125.0) < 5.0, f"mark at {_dominant(x[:24000])}")
    check(abs(_dominant(x[24000:]) - 2295.0) < 5.0, f"space at {_dominant(x[24000:])}")
    check(np.max(np.abs(np.diff(x))) < 0.5 * 2 * np.pi * 2295 / 48000 * 1.01, "phase jump")


def main():
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
        except Exception:  # noqa: BLE001 — report every failure, keep going
            failed += 1
            print(f"FAIL: {name}")
            traceback.print_exc()
    if failed:
        print(f"FAILED: {failed} of {len(tests)} tests")
        return 1
    print(f"OK: {len(tests)} tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
