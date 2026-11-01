#!/usr/bin/env python3
"""HackRF, receive only, by pickup: record while keyed, report the two-tone products."""
import subprocess
import numpy as np

RATE = 2_000_000
CENTER = 14_250_000.0  # off the signal, away from the HackRF's DC spike


def record(path, seconds, lna=16, vga=20):
    return subprocess.Popen(
        ["hackrf_transfer", "-r", path, "-f", str(int(CENTER)), "-s", str(RATE), "-a", "0",
         "-l", str(lna), "-g", str(vga), "-n", str(int(RATE * seconds))],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def products(path, carrier_hz, f1=700.0, f2=1903.0, skip_s=1.0, use_s=2.0):
    """Levels relative to the stronger tone, on one long FFT so the bins are ~0.5 Hz."""
    raw = np.fromfile(path, dtype=np.int8)
    iq = (raw[0::2].astype(np.float64) + 1j * raw[1::2].astype(np.float64)) / 128.0
    iq = iq[int(skip_s * RATE):int((skip_s + use_s) * RATE)]
    if len(iq) < RATE // 2:
        raise SystemExit("short capture: %d samples" % len(iq))
    clip = float(np.mean(np.abs(raw) >= 127))
    spec = np.abs(np.fft.fftshift(np.fft.fft(iq * np.hanning(len(iq))))) / len(iq)
    freqs = np.fft.fftshift(np.fft.fftfreq(len(iq), 1.0 / RATE)) + CENTER
    db = 20 * np.log10(spec + 1e-12)

    def at(hz, tol=25.0):
        m = np.abs(freqs - hz) < tol
        return float(db[m].max())

    tones = max(at(carrier_hz + f1), at(carrier_hz + f2))
    out = {"toneDb": tones, "clipFrac": clip,
           "floorDb": float(np.median(db[np.abs(freqs - carrier_hz - 1300) < 5000]))}
    for name, off in (("imd3Lo", 2 * f1 - f2), ("imd3Hi", 2 * f2 - f1),
                      ("imd5Lo", 3 * f1 - 2 * f2), ("imd5Hi", 3 * f2 - 2 * f1)):
        out[name] = at(carrier_hz + off) - tones
    return out
