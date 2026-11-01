#!/usr/bin/env python3
"""PS off then PS on, two-tone, HackRF recording both. usage: run_ps.py <tag> <tunePowerPct> [settle runs]"""
import json, sys, time
import bench, hrf
tag = sys.argv[1]; pct = sys.argv[2]; settle = int(sys.argv[3]) if len(sys.argv) > 3 else 3
F = 14215000.0
b = bench.Bridge()
b.call(cmd="tune", value="14.215")
print("tunePower", b.call(cmd="transmit", action="tunepower", value=pct).get("tunePower"))
lv = []
for i in range(15):
    m = b.call(cmd="get", model="meters")["meters"]["all"]
    lv += [x["value"] for x in m if x["name"] == "LEVEL"]
    time.sleep(0.2)
print("listen: max %.1f dBm" % max(lv))
if max(lv) > -95:
    raise SystemExit("frequency busy, not keying")
last = [None]
def s(i):
    r = b.call(cmd="ps"); h = b.health()
    cur = (r["state"], r["feedbackLevel"], r["calibrations"], r["attempts"], r["gainDb"], r["lastFailure"])
    if cur != last[0]:
        print(" ", cur, "fb", r["feedbackPeakDbfs"], "ref", r["referencePeakDbfs"], "maxTx", r["maxTx"],
              "fwd %.2fW swr %s" % (h.get("forwardPowerW") or 0, h.get("swr") and round(h["swr"], 2)), flush=True)
        last[0] = cur
def rec(name, secs, sample=s):
    p = hrf.record(f"/tmp/psbench/{tag}-{name}.iq", secs + 1.5)
    time.sleep(0.8)
    try:
        bench.twotone(b, secs, sample)
    finally:
        p.wait(timeout=30)
    print(name, json.dumps({k: round(v, 3) for k, v in hrf.products(f"/tmp/psbench/{tag}-{name}.iq", F).items()}), flush=True)
was = b.call(cmd="ps").get("state")
if was != "off":
    b.call(cmd="ps", action="off"); time.sleep(1.5)
rec("off", 5.0)
time.sleep(1.5)
r = b.call(cmd="ps", action="on"); print("ps on:", r.get("state"), r.get("error", "")); time.sleep(2)
for n in range(settle):
    bench.twotone(b, 12.0, s); print("after run", n, b.call(cmd="ps").get("state"), flush=True); time.sleep(2)
rec("on", 5.0)
time.sleep(1)
b.call(cmd="transmit", action="tunepower", value="5")
bench.cw_id(b)
h = b.health()
print("final", b.call(cmd="ps").get("state"), "keyed", h.get("keyed"), "dropped", h.get("droppedPackets"), "wave", h.get("tuneWaveform"), "gain", h.get("txLnaGainDb"))
