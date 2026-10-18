#!/usr/bin/env python3
"""Offline tests for tools/hackrf_tx_ota_analyze.py; never touches a radio.

Every capture is synthesised (tools/hackrf_tx_ota_fixtures.py): the correct
signal for a mode and deliberately wrong ones, so each check is shown to pass
what it should and fail what it should before it ever judges a HackRF.
"""

import os
import sys
import tempfile
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402

import hackrf_tx_ota_analyze as A  # noqa: E402
import hackrf_tx_ota_fixtures as F  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def close(value, expected, tol, what):
    check(value is not None and abs(value - expected) <= tol,
          f"{what}: {value} not within {tol} of {expected}")


# ── Task 2: capture handling and core measurements ─────────────────────────

def test_load_cu8_scale_and_clip():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "x.cu8")
        with open(path, "wb") as f:
            f.write(bytes([0, 255, 127, 128]))
        iq, clip = A.load_cu8(path)
        check(iq.dtype == np.complex64, f"dtype {iq.dtype}")
        check(len(iq) == 2, f"len {len(iq)}")
        close(iq[0].real, -1.0, 1e-3, "I0")
        close(iq[0].imag, 1.0, 1e-3, "Q0")
        close(iq[1].real, -0.0039, 1e-3, "I1")
        close(iq[1].imag, 0.0039, 1e-3, "Q1")
        close(clip, 0.5, 1e-9, "clip of [0,255,127,128]")

        raw = np.full(10_000, 128, dtype=np.uint8)
        raw[:50] = 0
        raw[50:100] = 255
        with open(path, "wb") as f:
            f.write(raw.tobytes())
        _, clip = A.load_cu8(path)
        close(clip, 0.01, 1e-6, "clip of 1 % rails")


def test_to_baseband_puts_tx_at_zero():
    iq = F.synth(F.tone(1000.0), total_s=1.0, key_start_s=0.0, key_s=1.0)
    bb = A.to_baseband(iq, F.SPEC)
    freqs, db = A.spectrum_db(bb, F.FS, rbw_hz=10.0)
    f, _ = A.peak_near(freqs, db, 1000.0, search_hz=500.0)
    close(f, 1000.0, 10.0, "tone after to_baseband")
    # The offset argument moves the zero: TX+1000 Hz lands on 0 Hz.
    bb = A.to_baseband(iq, F.SPEC, offset_hz=1000.0)
    freqs, db = A.spectrum_db(bb, F.FS, rbw_hz=10.0)
    f, _ = A.peak_near(freqs, db, 0.0, search_hz=500.0)
    close(f, 0.0, 10.0, "tone after to_baseband(offset=1000)")


def test_keyed_window_finds_burst():
    iq = F.synth(F.carrier(), total_s=5.0, key_start_s=1.0, key_s=3.0)
    bb = A.to_baseband(iq, F.SPEC)
    window = A.keyed_window(bb, F.FS)
    check(window is not None, "no window found")
    start_s, end_s = window[0] / F.FS, window[1] / F.FS
    check(1.0 <= start_s <= 1.1, f"start {start_s:.3f} s")
    check(3.9 <= end_s <= 4.0, f"end {end_s:.3f} s")


def test_silence_reports_no_signal():
    iq = F.synth(F.carrier(0.0), total_s=3.0, key_start_s=1.0, key_s=1.0, peak=0.3)
    bb = A.to_baseband(iq, F.SPEC)
    check(A.keyed_window(bb, F.FS) is None, "a window found in pure noise")


def _fm_interferer(offset_hz, amp):
    def fn(t):
        return amp * np.exp(1j * (2 * np.pi * offset_hz * t
                                  + 3.0 * np.sin(2 * np.pi * 1000.0 * t)))
    return fn


def test_channel_busy():
    # snr_db=50 in 10 Hz bins puts the noise at -30 dB in 1 kHz bins; an FM
    # interferer of amplitude 1.15 (beta 3) has lines about 25 dB above that.
    quiet = F.carrier(0.0)
    near = F.synth(quiet, total_s=0.5, key_start_s=0.0, key_s=0.5,
                   extra=_fm_interferer(10e3, 1.15))
    check(A.channel_busy(A.to_baseband(near, F.SPEC), F.FS),
          "interferer at +10 kHz not seen as busy")
    far = F.synth(quiet, total_s=0.5, key_start_s=0.0, key_s=0.5,
                  extra=_fm_interferer(60e3, 1.15))
    check(not A.channel_busy(A.to_baseband(far, F.SPEC), F.FS),
          "interferer at +60 kHz counted as busy")
    clear = F.synth(quiet, total_s=0.5, key_start_s=0.0, key_s=0.5, peak=0.3)
    check(not A.channel_busy(A.to_baseband(clear, F.SPEC), F.FS),
          "pure noise counted as busy")


def test_spectrum_rbw_and_floor():
    # peak=None: unscaled, so the carrier is exactly 0 dBFS and the noise is
    # exactly 60 dB below it in a 10 Hz bin.
    iq = F.synth(F.carrier(), total_s=2.0, key_start_s=0.0, key_s=2.0,
                 snr_db=60.0, peak=None)
    bb = A.to_baseband(iq, F.SPEC)
    freqs, db = A.spectrum_db(bb, F.FS, rbw_hz=10.0)
    check(abs((freqs[1] - freqs[0]) - 10.0) < 0.5, f"bin spacing {freqs[1] - freqs[0]}")
    _, level = A.peak_near(freqs, db, 0.0)
    close(level, 0.0, 2.0, "carrier level dBFS")
    floor = A.noise_floor_db(freqs, db, exclude=[(-50.0, 50.0)])
    close(floor, -60.0, 3.0, "noise floor dB in 10 Hz")


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
