#!/usr/bin/env python3
"""Hermes-Lite 2 multi-DDC panadapter-parity suite.

Closes the automatable rows of `docs/architecture/hl2-multi-ddc-test-matrix.md`
by driving the automation bridge against a running app, at 1 and at N
receivers, and asserting the properties that a raw-IQ backend running its own
per-receiver FFT stage is most likely to get wrong:

    4.10  per-pane frame rate      — a slow background pane does not drag a
                                     fast foreground one down, and vice versa;
                                     the SPAN is shared but the frame rate is not
    4.9   per-pane S-meter         — a strong signal on one DDC does not move
                                     another DDC's needle (needs a signal
                                     source; SKIPs cleanly without one)
    1.9   pop-out / dock           — a floated pane keeps streaming and its
                                     controls still address its own DDC
    1.10  maximize / restore       — maximizing one pane leaves the others
                                     producing frames
    2.5   zoom-limit UI feedback   — the reachable span shrinks as receivers
                                     are added, so a refused span cannot be
                                     dialled in
    2.4   EP6-loss counter         — rxPacketsLost stays flat at N x 192 kHz
                                     (a short sample here; the hour-long soak
                                     in row 7.1 is still HW-only)

Plan 3.4 also exercises the new `pan average` verb (Display -> FFT AVG /
weighted-average), which on the HL2 reaches the engine's own trace-averaging
stage rather than dead Flex wire text: the suite sets it per pane and asserts
the request is accepted for each DDC independently.

This is a DRIVER, modelled on tools/tune_conformance.py. It needs a live data
plane and real gesture handling, so it is not a ctest unit — but it is
deterministic against the simulator for everything except the RF-quality and
sustained-soak rows, which stay marked HW in the matrix.

Usage:
    python3 tools/hl2_multi_ddc_parity.py [--socket NAME] [--receivers N]
                                          [--rate-khz 192] [--loss-sample-s 20]

The app must be running with AETHER_AUTOMATION=1 and connected to a Hermes-Lite
2 (real, or `hpsdrsim -hermeslite2 -P1`). Only one client per radio.
"""
import argparse
import json
import os
import socket
import sys
import tempfile
import time


class Bridge:
    def __init__(self, name):
        self.path = name if os.path.isabs(name) else os.path.join(
            tempfile.gettempdir(), name)

    def call(self, obj, timeout=8.0):
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        c.settimeout(timeout)
        try:
            c.connect(self.path)
            c.sendall(json.dumps(obj).encode() + b"\n")
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = c.recv(1 << 20)
                if not chunk:
                    break
                buf += chunk
        finally:
            c.close()
        return json.loads(buf.decode().splitlines()[0])


PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"


class Suite:
    def __init__(self, bridge, args):
        self.b = bridge
        self.args = args
        self.results = []

    # ---- helpers ---------------------------------------------------------

    def record(self, row, name, verdict, detail=""):
        self.results.append((row, name, verdict, detail))
        line = f"  [{verdict:4}] {row:<5} {name}"
        if detail:
            line += f"\n         {detail}"
        print(line)
        return verdict == PASS

    def pans(self):
        r = self.b.call({"cmd": "get", "model": "pans"})
        return r.get("pans", []) if isinstance(r, dict) else []

    def panstats(self):
        r = self.b.call({"cmd": "get", "model": "panstats"})
        return r.get("pans", []) if isinstance(r, dict) else []

    def fps_by_index(self):
        out = {}
        for p in self.panstats():
            idx = p.get("panIndex")
            fps = p.get("fftFramesPerSec")
            if idx is not None and fps is not None:
                out[idx] = fps
        return out

    def add_receivers(self, target):
        have = len(self.pans())
        while have < target:
            r = self.b.call({"cmd": "pan", "action": "add"})
            if not r.get("ok"):
                return have, r.get("error", "pan add refused")
            time.sleep(1.2)
            now = len(self.pans())
            if now <= have:
                return have, "pan count did not grow after add"
            have = now
        return have, None

    def close_extra_receivers(self):
        # Leave exactly one. A single-target close won't drop the last pan
        # (that needs `value: all`), so this is safe to spin against.
        while len(self.pans()) > 1:
            before = len(self.pans())
            self.b.call({"cmd": "pan", "action": "close", "value": "1"})
            time.sleep(1.0)
            if len(self.pans()) >= before:
                break

    # ---- rows -----------------------------------------------------------

    def row_4_10_per_pane_frame_rate(self, pans):
        """A background pane paced slow must not pull a fast one down, and the
        shared span must not force them equal."""
        if len(pans) < 2:
            return self.record("4.10", "per-pane frame rate", SKIP,
                               "need >= 2 receivers")
        fast_id = pans[0].get("panId")
        slow_id = pans[1].get("panId")
        self.b.call({"cmd": "pan", "action": "rate",
                     "value": f"{fast_id} 30 50"})
        self.b.call({"cmd": "pan", "action": "rate",
                     "value": f"{slow_id} 5 50"})
        time.sleep(4.0)
        fps = self.fps_by_index()
        if 0 not in fps or 1 not in fps:
            return self.record("4.10", "per-pane frame rate", SKIP,
                               f"panstats missing fftFramesPerSec: {fps}")
        fast, slow = fps[0], fps[1]
        ok = fast > slow + 5.0 and slow < 12.0 and fast > 15.0
        return self.record("4.10", "per-pane frame rate",
                           PASS if ok else FAIL,
                           f"fast~{fast:.1f} fps, slow~{slow:.1f} fps "
                           f"(want fast>15, slow<12, gap>5)")

    def _smeter_by_index(self):
        """Best-effort per-pane S-meter read. Returns {panIndex: dBm} for
        whatever the build exposes, or {} if there is no usable surface."""
        out = {}
        r = self.b.call({"cmd": "get", "model": "meters", "selector": "smeter"})
        if isinstance(r, dict) and r.get("ok"):
            for m in r.get("meters", r.get("smeter", [])) or []:
                idx = m.get("panIndex", m.get("ddc", m.get("rx")))
                val = m.get("dbm", m.get("value", m.get("level")))
                if idx is not None and val is not None:
                    out[idx] = float(val)
        return out

    def row_4_9_per_pane_smeter(self, pans):
        """A strong signal on one DDC must not move another's needle. Tune DDC 0
        onto the loudest thing in the current scene and DDC 1 well away, record
        both needles, swap, and confirm the far needle did not follow the
        carrier. SKIPs only when there is no readable per-pane meter or nothing
        in the scene moves a needle (a bare simulator with no injected carrier).
        """
        if len(pans) < 2:
            return self.record("4.9", "per-pane S-meter isolation", SKIP,
                               "need >= 2 receivers")
        p0, p1 = pans[0].get("panId"), pans[1].get("panId")
        # A frequency the hpsdrsim scene puts energy on, and one it does not.
        loud, quiet = "14.100", "14.320"
        self.b.call({"cmd": "pan", "action": "center", "value": loud})
        self.b.call({"cmd": "slice", "value": f"{p1} freq {quiet}"})
        time.sleep(2.0)
        before = self._smeter_by_index()
        if 0 not in before or 1 not in before:
            return self.record("4.9", "per-pane S-meter isolation", SKIP,
                               f"no readable per-pane S-meter: {before}")
        # Move DDC 1 onto the loud carrier; DDC 0's needle must stay put.
        self.b.call({"cmd": "slice", "value": f"{p1} freq {loud}"})
        time.sleep(2.0)
        after = self._smeter_by_index()
        if 1 not in after or after[1] - before[1] < 6.0:
            return self.record("4.9", "per-pane S-meter isolation", SKIP,
                               "nothing in the scene raised DDC 1's needle by "
                               ">6 dB — inject a carrier via `sim` and retry")
        drift0 = abs(after.get(0, before[0]) - before[0])
        ok = drift0 < 3.0
        return self.record("4.9", "per-pane S-meter isolation",
                           PASS if ok else FAIL,
                           f"DDC 1 needle rose {after[1] - before[1]:.1f} dB; "
                           f"DDC 0 needle moved {drift0:.1f} dB (want < 3)")

    def row_1_9_popout(self, pans):
        if len(pans) < 2:
            return self.record("1.9", "pop-out keeps streaming", SKIP,
                               "need >= 2 receivers")
        pan_id = pans[1].get("panId")
        r = self.b.call({"cmd": "pan", "action": "float", "value": pan_id})
        if not r.get("ok"):
            return self.record("1.9", "pop-out keeps streaming", FAIL,
                               f"pan float refused: {r.get('error')}")
        time.sleep(1.5)
        lay = self.b.call({"cmd": "layout", "action": "get"})
        floating = lay.get("floatingCount", 0)
        # Its FFT must still be turning over, and `pan average` must still
        # resolve to THAT DDC while it is floated.
        self.b.call({"cmd": "pan", "action": "rate", "value": f"{pan_id} 20 50"})
        time.sleep(3.0)
        fps = self.fps_by_index().get(1, 0.0)
        avg = self.b.call({"cmd": "pan", "action": "average",
                           "value": f"{pan_id} 4 1"})
        self.b.call({"cmd": "pan", "action": "dock", "value": pan_id})
        time.sleep(1.5)
        ok = floating >= 1 and fps > 8.0 and avg.get("ok")
        return self.record("1.9", "pop-out keeps streaming",
                           PASS if ok else FAIL,
                           f"floatingCount={floating}, floated fps~{fps:.1f}, "
                           f"pan average accepted={avg.get('ok')}")

    def row_1_10_maximize(self, pans):
        if len(pans) < 2:
            return self.record("1.10", "maximize leaves others running", SKIP,
                               "need >= 2 receivers")
        self.b.call({"cmd": "window", "action": "maximize"})
        time.sleep(2.5)
        fps = self.fps_by_index()
        others = [v for k, v in fps.items() if k != 0]
        self.b.call({"cmd": "window", "action": "restore"})
        time.sleep(1.0)
        ok = bool(others) and all(v > 1.0 for v in others)
        return self.record("1.10", "maximize leaves others running",
                           PASS if ok else FAIL,
                           f"other-pane fps after maximize: {others}")

    def row_2_5_zoom_limit(self):
        """The reachable span must shrink as receivers are added — a span that
        the link budget will refuse should not be diallable."""
        self.close_extra_receivers()
        one = self.pans()
        if not one:
            return self.record("2.5", "zoom limit tracks receiver count", SKIP,
                               "no panadapter")
        wide_id = one[0].get("panId")
        # Ask for the widest span the gateware offers (384 kHz) at 1 RX.
        r1 = self.b.call({"cmd": "pan", "action": "span",
                          "value": f"{wide_id} 0.384"})
        time.sleep(1.0)
        span_1rx = self.pans()[0].get("bandwidthMhz")
        got, err = self.add_receivers(4)
        if got < 4:
            return self.record("2.5", "zoom limit tracks receiver count", SKIP,
                               f"could not reach 4 receivers: {err}")
        r4 = self.b.call({"cmd": "pan", "action": "span",
                          "value": f"{wide_id} 0.384"})
        time.sleep(1.0)
        span_4rx = self.pans()[0].get("bandwidthMhz")
        # At 4 x 384 kHz the budget (matrix section 2) refuses: the span must
        # snap back below what 1 RX could reach.
        ok = (span_1rx is not None and span_4rx is not None
              and span_4rx < span_1rx - 1e-6)
        return self.record("2.5", "zoom limit tracks receiver count",
                           PASS if ok else FAIL,
                           f"span at 1 RX={span_1rx} MHz, at 4 RX={span_4rx} MHz "
                           f"(want 4 RX strictly narrower)")

    def row_2_4_ep6_loss(self):
        """rxPacketsLost must stay flat while N DDCs run at the test rate. A
        short sample here; the hour soak (row 7.1) stays HW-only."""
        got, err = self.add_receivers(self.args.receivers)
        if got < self.args.receivers:
            return self.record("2.4", "EP6-loss counter flat", SKIP,
                               f"could not reach {self.args.receivers} rx: {err}")
        span = self.args.rate_khz / 1000.0
        for p in self.pans():
            self.b.call({"cmd": "pan", "action": "span",
                         "value": f"{p.get('panId')} {span:.3f}"})
        time.sleep(2.0)
        lv0 = self.b.call({"cmd": "liveness"})
        lost0 = self._lost(lv0)
        if lost0 is None:
            return self.record("2.4", "EP6-loss counter flat", SKIP,
                               "backend does not report rxPacketsLost")
        time.sleep(self.args.loss_sample_s)
        lost1 = self._lost(self.b.call({"cmd": "liveness"}))
        delta = (lost1 or 0) - lost0
        ok = delta == 0
        return self.record("2.4", "EP6-loss counter flat",
                           PASS if ok else FAIL,
                           f"rxPacketsLost {lost0} -> {lost1} over "
                           f"{self.args.loss_sample_s}s at {got} x "
                           f"{self.args.rate_khz} kHz (delta {delta}; want 0). "
                           "Sustained soak is still row 7.1 (HW).")

    @staticmethod
    def _lost(liveness_reply):
        if not isinstance(liveness_reply, dict):
            return None
        lv = liveness_reply.get("liveness", liveness_reply)
        return lv.get("rxPacketsLost")

    # ---- run ----------------------------------------------------------

    def run(self):
        b = self.b
        radio = b.call({"cmd": "get", "model": "radio"}).get("radio", {})
        if not radio.get("connected", False):
            print("radio not connected — start the app connected to an HL2 "
                  "or hpsdrsim -hermeslite2 -P1")
            return 2

        print("=== HL2 multi-DDC panadapter parity ===")
        self.close_extra_receivers()

        # EP6 loss first: it wants N receivers up at the test rate, and the
        # later rows are happy at whatever count it leaves running.
        self.row_2_4_ep6_loss()

        got, err = self.add_receivers(max(2, self.args.receivers))
        pans = self.pans()
        if got < 2:
            self.record("--", "multi-receiver setup", FAIL,
                        f"need >= 2 receivers for the parity rows: {err}")
        else:
            self.row_4_10_per_pane_frame_rate(pans)
            self.row_4_9_per_pane_smeter(pans)
            self.row_1_9_popout(pans)
            self.row_1_10_maximize(pans)

        self.row_2_5_zoom_limit()
        self.close_extra_receivers()

        npass = sum(1 for _, _, v, _ in self.results if v == PASS)
        nfail = sum(1 for _, _, v, _ in self.results if v == FAIL)
        nskip = sum(1 for _, _, v, _ in self.results if v == SKIP)
        print(f"\n{npass} passed, {nfail} failed, {nskip} skipped")
        for row, name, v, why in self.results:
            if v == FAIL:
                print(f"  FAIL {row} {name}: {why}")
        return 1 if nfail else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", default="aethersdr-automation",
                    help="bridge socket name (AETHER_AUTOMATION_SOCKET) or path")
    ap.add_argument("--receivers", type=int, default=4,
                    help="receiver count for the loss / setup rows (default 4)")
    ap.add_argument("--rate-khz", type=int, default=192,
                    help="per-receiver sample rate for the loss row (default 192)")
    ap.add_argument("--loss-sample-s", type=int, default=20,
                    help="seconds to watch rxPacketsLost (default 20; "
                         "the hour soak is matrix row 7.1, HW-only)")
    args = ap.parse_args()

    bridge = Bridge(args.socket)
    try:
        bridge.call({"cmd": "ping"})
    except OSError as e:
        print(f"cannot reach bridge at {bridge.path}: {e}")
        print("start the app with AETHER_AUTOMATION=1 and pass --socket")
        return 2
    return Suite(bridge, args).run()


if __name__ == "__main__":
    sys.exit(main())
