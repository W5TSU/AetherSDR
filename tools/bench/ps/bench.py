#!/usr/bin/env python3
"""PureSignal bench helpers: bridge client, keying that always unkeys, CW ID.

Keys a real transmitter. Run only with the operator at the bench.
Needs AETHER_BENCH_TOKEN_FILE and AETHER_BENCH_PID for a running app.
"""
import json
import os
import socket
import tempfile
import time

CALLSIGN = "W5TSU"
MORSE = {"W": ".--", "5": ".....", "T": "-", "S": "...", "U": "..-"}
DIT_S = 0.1  # 12 WPM
MAX_KEY_S = 14.0  # under AETHER_AUTOMATION_TX_MAX_MS


class Bridge:
    def __init__(self):
        self.token = open(os.environ["AETHER_BENCH_TOKEN_FILE"]).read().strip()
        pid = os.environ["AETHER_BENCH_PID"]
        info = json.load(open(os.path.join(tempfile.gettempdir(), "aethersdr-automation",
                                           pid + ".json")))
        self.path = info["socket"]

    def call(self, timeout=20, **req):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        s.connect(self.path)
        req["token"] = self.token
        s.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            d = s.recv(1 << 20)
            if not d:
                break
            buf += d
        s.close()
        return json.loads(buf)

    def health(self):
        return {r["key"]: r["value"] for r in self.call(cmd="health")["rows"]}

    def keyed(self):
        return bool(self.health().get("keyed"))


def unkey(b):
    """Every way the bridge can release the key, then check."""
    for req in ({"cmd": "txtest", "action": "off"}, {"cmd": "key", "action": "ptt", "value": "off"}):
        try:
            b.call(**req)
        except Exception as e:  # keep going: the next route may still work
            print("unkey:", req, e)
    time.sleep(0.2)
    if b.keyed():
        raise SystemExit("STILL KEYED after unkey. Turn the radio off.")


def twotone(b, seconds, sample=None, every=0.5):
    """Two-tone for `seconds`, unkeying on any exit. `sample(i)` runs while keyed."""
    seconds = min(seconds, MAX_KEY_S)
    try:
        r = b.call(cmd="txtest", action="twotone")
        if not r.get("ok"):
            raise SystemExit("twotone refused: %s" % r)
        t0 = time.time()
        i = 0
        while time.time() - t0 < seconds:
            if sample:
                sample(i)
            i += 1
            time.sleep(every)
    finally:
        unkey(b)


def carrier(b, seconds, sample=None, every=0.25):
    seconds = min(seconds, MAX_KEY_S)
    try:
        b.call(cmd="shortcut", target="tune_toggle")
        t0 = time.time()
        i = 0
        while time.time() - t0 < seconds:
            if sample:
                sample(i)
            i += 1
            time.sleep(every)
    finally:
        h = b.health()
        if h.get("tuning"):
            b.call(cmd="shortcut", target="tune_toggle")
        unkey(b)


def cw_id(b):
    """The callsign on the TUNE carrier. The bridge's CW verbs have no HL2 binding."""
    try:
        for letter in CALLSIGN:
            for sym in MORSE[letter]:
                b.call(cmd="shortcut", target="tune_toggle")
                time.sleep(DIT_S * (1 if sym == "." else 3))
                b.call(cmd="shortcut", target="tune_toggle")
                time.sleep(DIT_S)
            time.sleep(DIT_S * 2)
    finally:
        if b.health().get("tuning"):
            b.call(cmd="shortcut", target="tune_toggle")
        unkey(b)
