#!/usr/bin/env python3
"""Pull the HackRF out from under a running AetherSDR and check it recovers.

#42's hardware checklist item "unplug-mid-session recovery": with the HackRF
connected and receiving, the device disappears from USB. AetherSDR must notice
(HackRfWorker::checkStreamHealth), report it and disconnect cleanly rather
than freeze or crash, and connect again once the device is back.

By default the unplug is simulated with libhackrf's own reset request (USB
vendor request 30, what hackrf_reset() sends): the board reboots, drops off
the bus and re-enumerates, exactly as an unplug and replug look to the host.
It is sent without claiming the interface, so it works while AetherSDR holds
the device. --physical waits for you to pull the cable instead.

Receive only: TX is never keyed, and the app is launched without the bridge's
TX permission. Needs pyusb (python3-usb) and read/write access to the HackRF's
USB node (the hackrf udev rule).

Usage: python3 tools/hackrf_unplug_test.py [--physical] [--serial S]
"""

import argparse
import contextlib
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hackrf_tx_ota_test import connect_hackrf, quit_app, wait_for_bridge  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GSG_VENDOR = 0x1D50
HACKRF_PRODUCTS = (0x6089, 0x604B, 0xCC15)   # One/Pro, Jawbreaker, rad1o
VENDOR_REQUEST_RESET = 30


def find_hackrf(serial=""):
    import usb.core
    for dev in usb.core.find(find_all=True, idVendor=GSG_VENDOR):
        if dev.idProduct not in HACKRF_PRODUCTS:
            continue
        if serial:
            with contextlib.suppress(Exception):
                if not (dev.serial_number or "").endswith(serial.lstrip("0")):
                    continue
        return dev
    return None


def reset_hackrf(dev):
    """The reset hackrf_reset() sends. The board reboots before the status
    stage, so an error from the transfer itself is expected."""
    with contextlib.suppress(Exception):
        dev.ctrl_transfer(0x40, VENDOR_REQUEST_RESET, 0, 0, None, timeout=1000)


def wait_until(pred, seconds, step=0.2):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def launch(app, out_dir, headless):
    name = f"hackrf-unplug-{os.getpid()}-{time.time_ns()}"
    profile = os.path.join(out_dir, "profile")
    os.makedirs(profile, exist_ok=True)
    base = dict(os.environ, AETHER_SETTINGS_DIR=profile)
    subprocess.run([app, "--config", "set", "AutoConnectToLastRadio", "False"],
                   env=base, capture_output=True, text=True, timeout=60, check=True)
    env = dict(base, AETHER_AUTOMATION="1", AETHER_AUTOMATION_SOCKET=name)
    env.pop("AETHER_AUTOMATION_ALLOW_TX", None)        # receive only
    if headless:
        env["QT_QPA_PLATFORM"] = "offscreen"
    log = open(os.path.join(out_dir, "aethersdr.log"), "w")
    proc = subprocess.Popen([app], env=env, stdout=log, stderr=subprocess.STDOUT)
    return proc, os.path.join(tempfile.gettempdir(), name)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog="Guide: docs/HACKRF.md, 'Testing transmit' and below")
    ap.add_argument("--app", default=os.path.join(REPO, "build", "AetherSDR"))
    ap.add_argument("--serial", default="", help="pick this HackRF (default: the first)")
    ap.add_argument("--physical", action="store_true",
                    help="wait for you to unplug and replug instead of resetting the board")
    ap.add_argument("--out-dir", default="", help="log and profile (default: a temp dir)")
    ap.add_argument("--headless", action="store_true", help="QT_QPA_PLATFORM=offscreen")
    args = ap.parse_args(argv)

    try:
        import usb.core  # noqa: F401
    except ImportError:
        print("needs pyusb: sudo apt install python3-usb (or pip install pyusb)")
        return 2
    if find_hackrf(args.serial) is None:
        print("no HackRF on USB" + (f" with serial {args.serial}" if args.serial else ""))
        return 2

    out_dir = args.out_dir or tempfile.mkdtemp(prefix="hackrf-unplug-")
    os.makedirs(out_dir, exist_ok=True)
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        print(f"[{'PASS' if ok else 'FAIL'}] {what}", flush=True)
        return ok

    def radio(b):
        return b.request({"cmd": "get", "model": "radio"}, 5.0).get("radio", {})

    def pan_fps(b):
        return float(b.request({"cmd": "perf"}, 5.0).get("panFps", 0) or 0)

    proc, sock = launch(args.app, out_dir, args.headless)
    bridge = None
    try:
        bridge = wait_for_bridge(sock, 60, proc)
        connect_hackrf(bridge, args.serial, print)
        check(wait_until(lambda: pan_fps(bridge) > 0, 15, 0.5), "receiving before the unplug (panFps > 0)")

        if args.physical:
            print(">>> Unplug the HackRF now.", flush=True)
            gone = wait_until(lambda: find_hackrf(args.serial) is None, 60)
        else:
            print("resetting the HackRF (it drops off USB and comes back)", flush=True)
            reset_hackrf(find_hackrf(args.serial))
            gone = True
        check(gone, "the HackRF left the bus")

        t0 = time.monotonic()
        dropped = wait_until(lambda: not radio(bridge).get("connected", True), 10)
        check(dropped, f"AetherSDR disconnected on its own ({time.monotonic() - t0:.1f} s)")
        check(proc.poll() is None, "AetherSDR is still running")

        if args.physical:
            print(">>> Plug the HackRF back in.", flush=True)
        check(wait_until(lambda: find_hackrf(args.serial) is not None, 60, 0.5),
              "the HackRF is back on USB")
        time.sleep(2.0)                            # let discovery see it again
        connect_hackrf(bridge, args.serial, print)
        check(radio(bridge).get("connected"), "it connects again")
        check(wait_until(lambda: pan_fps(bridge) > 0, 15, 0.5), "receiving again (panFps > 0)")
        events = bridge.request({"cmd": "log", "action": "tail", "value": "2000"}, 10.0).get("events", [])
        lost = [e.get("msg", "") for e in events if "stream stopped unexpectedly" in e.get("msg", "")]
        check(lost, "the log names the lost stream" + (f": {lost[0]}" if lost else ""))
    except Exception as e:                         # a setup step failed: report, still clean up
        check(False, f"{type(e).__name__}: {e}")
    finally:
        if bridge is not None:
            with contextlib.suppress(Exception):
                bridge.request({"cmd": "disconnect"}, 10.0)
        quit_app(proc, sock)

    check(proc.returncode == 0, f"AetherSDR quit cleanly (exit {proc.returncode})")

    failed = [w for ok, w in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed; app output: "
          f"{os.path.join(out_dir, 'aethersdr.log')}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
