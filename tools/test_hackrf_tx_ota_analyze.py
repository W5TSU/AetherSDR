#!/usr/bin/env python3
"""Offline tests for tools/hackrf_tx_ota_analyze.py; never touches a radio.

Every capture is synthesised (tools/hackrf_tx_ota_fixtures.py): the correct
signal for a mode and deliberately wrong ones, so each check is shown to pass
what it should and fail what it should before it ever judges a HackRF.
"""

import json
import os
import sys
import tempfile
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


# ── Task 3: per-mode checks ────────────────────────────────────────────────

def ref0(**kw):
    return A.Reference(carrier_offset_hz=0.0, **kw)


def checks(result):
    return {c.name: c for c in result.checks}


def test_tune_offset_and_ppm():
    iq = F.synth(F.carrier(), total_s=2.0, key_start_s=0.5, key_s=1.0,
                 carrier_offset_hz=9000.0)
    result, ref = A.measure_tune(iq, F.SPEC, freq_tol_ppm=25.0)
    close(ref.carrier_offset_hz, 9000.0, 5.0, "TUNE carrier offset")
    c = checks(result)
    check(c["frequency"].passed, f"9 kHz (9.8 ppm) failed 25 ppm: {c['frequency']}")
    check(c["snr"].measured > 40.0, f"snr {c['snr'].measured}")
    check(result.status == "pass", f"status {result.status} {result.checks}")
    iq = F.synth(F.carrier(), total_s=2.0, key_start_s=0.5, key_s=1.0,
                 carrier_offset_hz=30000.0)
    result, _ = A.measure_tune(iq, F.SPEC, freq_tol_ppm=25.0)
    check(not checks(result)["frequency"].passed, "30 kHz (32.8 ppm) passed 25 ppm")
    check(result.status == "fail", f"status {result.status}")


def test_usb_passes_lsb_fails_as_usb():
    iq = F.synth(F.tone(1000.0))
    usb = A.analyze_mode("USB", iq, F.SPEC, ref0())
    check(usb.status == "pass", f"USB status {usb.status}: {usb.checks} {usb.notes}")
    lsb = A.analyze_mode("LSB", iq, F.SPEC, ref0())
    c = checks(lsb)
    check(not c["opposite_sideband"].passed, f"USB signal passed as LSB: {c['opposite_sideband']}")
    check(lsb.status == "fail", f"LSB status {lsb.status}")
    check(any("other sideband" in n for n in lsb.notes), f"wrong sideband not named: {lsb.notes}")
    check(not any("no signal" in n or "spurious" in n for n in lsb.notes), f"notes {lsb.notes}")


def test_ssb_carrier_leak_fails():
    iq = F.synth(F.mix(F.tone(1000.0), F.carrier(10 ** (-10 / 20))))
    usb = A.analyze_mode("USB", iq, F.SPEC, ref0())
    c = checks(usb)
    check(not c["carrier_suppression"].passed, f"-10 dBc carrier passed: {c['carrier_suppression']}")
    close(c["carrier_suppression"].measured, 10.0, 1.5, "carrier suppression dB")


def test_ssb_finds_its_own_reference_after_drift():
    # The radios drift between the TUNE reference and this mode: 120 Hz here.
    iq = F.synth(F.tone(1000.0), carrier_offset_hz=120.0)
    usb = A.analyze_mode("USB", iq, F.SPEC, ref0())
    check(usb.status == "pass", f"drifted USB: {usb.checks} {usb.notes}")


def test_am_depth():
    iq = F.synth(F.am(0.7))
    am = A.analyze_mode("AM", iq, F.SPEC, ref0())
    c = checks(am)
    close(c["depth"].measured, 0.70, 0.05, "AM depth")
    check(am.status == "pass", f"AM status {am.status}: {am.checks}")
    over = A.analyze_mode("AM", F.synth(F.am(1.3, clip=True)), F.SPEC, ref0())
    check(not checks(over)["harmonics"].passed, f"over-modulated AM passed harmonics: {checks(over)['harmonics']}")


def test_dsb_symmetry_and_carrier():
    dsb = A.analyze_mode("DSB", F.synth(F.dsb()), F.SPEC, ref0())
    check(dsb.status == "pass", f"DSB status {dsb.status}: {dsb.checks}")
    leak = A.analyze_mode("DSB", F.synth(F.mix(F.dsb(), F.carrier(10 ** (-6 / 20)))), F.SPEC, ref0())
    check(not checks(leak)["carrier_suppression"].passed, "DSB with a -6 dB carrier passed")


def test_fm_deviation():
    iq4 = F.synth(F.fm(4000.0))
    fm = A.analyze_mode("FM", iq4, F.SPEC, ref0())
    c = checks(fm)
    close(c["deviation"].measured, 4000.0, 200.0, "FM deviation Hz")
    close(c["tone_frequency"].measured, 1000.0, 5.0, "FM tone Hz")
    check(fm.status == "pass", f"FM 4 kHz status {fm.status}: {fm.checks}")
    fmn = A.analyze_mode("FMN", iq4, F.SPEC, ref0())
    check(not checks(fmn)["deviation"].passed, "4 kHz passed FMN's 2.5 kHz limit")
    iq2 = F.synth(F.fm(2000.0))
    fmn2 = A.analyze_mode("FMN", iq2, F.SPEC, ref0(fm_peak_dev_hz=4000.0))
    c = checks(fmn2)
    check(c["deviation"].passed, f"2 kHz failed FMN: {c['deviation']}")
    close(c["fmn_ratio"].measured, 0.5, 0.05, "FMN/FM ratio")
    check(c["fmn_ratio"].passed, "ratio 0.5 failed")


def test_data_mode_without_audio_fails():
    # What a data mode transmits when no TCI audio reaches it: a bare carrier.
    digu = A.analyze_mode("DIGU", F.synth(F.carrier(10 ** (-30 / 20))), F.SPEC, ref0(), tone_hz=1500.0)
    check(digu.status == "fail", f"bare carrier passed DIGU: {digu.checks}")
    dfm = A.analyze_mode("DFM", F.synth(F.carrier()), F.SPEC, ref0())
    check(not checks(dfm)["modulated"].passed, "unmodulated DFM passed")


CW_TEXT = "VVV DE W5TSU"


def _cw_capture(edge_s, jitter_s=0.005):
    elements = A.morse_elements(CW_TEXT, 15.0)
    sig = F.keyed(elements, edge_s=edge_s, jitter_s=jitter_s)
    return F.synth(sig, total_s=sig.duration_s + 1.6, key_start_s=0.8, key_s=sig.duration_s + 0.2)


def test_morse_elements_paris_timing():
    el = A.morse_elements("E E", 12.0)   # dit, word gap, dit
    check([d for d, _ in el] == [True, False, True], f"elements {el}")
    close(el[0][1], 0.1, 1e-9, "dit at 12 WPM")
    close(el[1][1], 0.7, 1e-9, "word gap = 7 units")


def test_cw_decode_and_timing():
    cw = A.analyze_mode("CW", _cw_capture(0.010), F.SPEC, ref0(), cw_text=CW_TEXT, cw_wpm=15.0)
    c = checks(cw)
    check(c["decode"].passed, f"decode: {c['decode']} {cw.notes}")
    close(c["dit_length"].measured, 80.0, 12.0, "dit ms")
    check(c["carrier_offset"].passed, f"carrier: {c['carrier_offset']}")
    check(cw.status == "pass", f"CW status {cw.status}: {cw.checks}")


def test_cw_key_clicks_fail_width():
    cw = A.analyze_mode("CW", _cw_capture(0.0), F.SPEC, ref0(), cw_text=CW_TEXT, cw_wpm=15.0)
    check(not checks(cw)["width"].passed, f"hard keying passed width: {checks(cw)['width']}")


def test_interferer_in_keyed_window_reports_not_fails():
    def interferer(t):
        return 10 ** (-10 / 20) * np.exp(1j * (2 * np.pi * 120e3 * t + 3 * np.sin(2 * np.pi * 1000 * t)))
    usb = A.analyze_mode("USB", F.synth(F.tone(1000.0), extra=interferer), F.SPEC, ref0())
    c = checks(usb)
    check(usb.status == "pass", f"interferer failed USB: {usb.checks}")
    check(c["spurious"].passed, "spurious is report-only")
    check(any("spurious" in n for n in usb.notes), f"no spurious note: {usb.notes}")


def test_rtty_two_tones():
    iq = F.synth(F.fsk_halves(2125.0, 2295.0), key_s=2.0, total_s=3.0)
    rtty = A.analyze_mode("RTTY", iq, F.SPEC, ref0())
    c = checks(rtty)
    close(c["shift"].measured, 170.0, 5.0, "RTTY shift")
    check(rtty.status == "pass", f"RTTY status {rtty.status}: {rtty.checks}")
    wide = A.analyze_mode("RTTY", F.synth(F.fsk_halves(2125.0, 2325.0), key_s=2.0, total_s=3.0),
                          F.SPEC, ref0())
    check(not checks(wide)["shift"].passed, "200 Hz shift passed")


def _dominant_hz(audio, fs):
    spec = np.abs(np.fft.rfft(audio * np.hanning(audio.size)))
    return float(np.fft.rfftfreq(audio.size, 1.0 / fs)[np.argmax(spec[1:]) + 1])


def test_demod_audio_recovers_the_tone():
    cases = [
        ("USB", F.tone(1000.0), 1000.0), ("LSB", F.tone(-1000.0), 1000.0),
        ("DIGU", F.tone(1500.0), 1500.0), ("AM", F.am(0.8), 1000.0),
        ("DSB", F.dsb(), 1000.0), ("FM", F.fm(5000.0), 1000.0),
        ("FMN", F.fm(2500.0), 1000.0), ("CW", F.carrier(), 700.0),
    ]
    for mode, sig, want in cases:
        iq = F.synth(sig, key_s=1.0, total_s=1.5, key_start_s=0.25)
        audio = A.demod_audio(mode, A.to_baseband(iq, F.SPEC), F.FS)
        check(audio.dtype == np.float32, f"{mode}: dtype {audio.dtype}")
        close(audio.size / 48000.0, 1.5, 0.01, f"{mode} audio length (s)")
        check(np.max(np.abs(audio)) <= 1.0, f"{mode}: audio clips")
        close(_dominant_hz(audio[int(0.3 * 48000):int(1.2 * 48000)], 48000.0),
              want, 5.0, f"{mode} demodulated tone")
    # USB demodulation rejects a lower-sideband signal.
    usb = A.demod_audio("USB", A.to_baseband(F.synth(F.tone(-1000.0)), F.SPEC), F.FS)
    lsb = A.demod_audio("LSB", A.to_baseband(F.synth(F.tone(-1000.0)), F.SPEC), F.FS)
    check(np.std(usb) < 0.1 * np.std(lsb), "USB demod passed an LSB tone")


# ── Task 4: reports, artifacts, re-analysis ────────────────────────────────

def _spec_dict(spec):
    return {"sample_rate_hz": spec.sample_rate_hz, "rtl_center_hz": spec.rtl_center_hz,
            "tx_freq_hz": spec.tx_freq_hz}


def test_reports_round_trip():
    ok = A.ModeResult("USB", "pass", [A.CheckResult("signal", True, 60.0, ">= 20 dB", "dB")])
    bad = A.ModeResult("LSB", "fail", [
        A.CheckResult("signal", True, 58.0, ">= 20 dB", "dB"),
        A.CheckResult("opposite_sideband", False, 12.3, ">= 25 dB below the tone", "dB"),
    ], ["tone on the wrong side"])
    meta = {"freq_mhz": 915.5, "ppm": -1.4, "git_commit": "abc1234"}
    with tempfile.TemporaryDirectory() as d:
        A.write_reports(d, meta, [ok, bad])
        with open(os.path.join(d, "report.json")) as f:
            rep = json.load(f)
        check([r["status"] for r in rep["results"]] == ["pass", "fail"], f"statuses {rep}")
        check(rep["exit_code"] == 1, f"exit code {rep['exit_code']}")
        check(A.exit_code([A.result_from_dict(r) for r in rep["results"]]) == 1, "round trip")
        with open(os.path.join(d, "report.md")) as f:
            md = f.read()
        check("| USB | pass |" in md, md)
        check("opposite_sideband" in md and "12.3" in md, md)
        check("915.5" in md and "abc1234" in md, md)
    check(A.exit_code([ok, A.ModeResult("FM", "skip")]) == 0, "skip is not a failure")
    check(A.exit_code([bad, A.ModeResult("FM", "abort")]) == 2, "abort wins")


def test_reanalyze_from_run_json():
    with tempfile.TemporaryDirectory() as d:
        modes = []
        for mode, sig in (("TUNE", F.carrier()), ("USB", F.tone(1000.0))):
            os.makedirs(os.path.join(d, mode))
            rel = os.path.join(mode, "capture.cu8")
            F.write_cu8(os.path.join(d, rel), F.synth(sig, carrier_offset_hz=-1300.0))
            modes.append({"mode": mode, "capture": rel, "spec": _spec_dict(F.SPEC),
                          "nominal_s": [0.5, 1.5], "power": 30, "rtl_gain_db": 20.7})
        modes.append({"mode": "FM", "status": "skip", "notes": ["not run"]})
        run = {"config": {"freq_mhz": 915.5, "freq_tol_ppm": 25, "git_commit": "abc1234"},
               "modes": modes}
        with open(os.path.join(d, "run.json"), "w") as f:
            json.dump(run, f)
        results = A.reanalyze(d)
        check([r.status for r in results] == ["pass", "pass", "skip"],
              f"statuses {[(r.mode, r.status, r.checks) for r in results]}")
        check(os.path.exists(os.path.join(d, "report.md")), "no report.md")
        check(os.path.exists(os.path.join(d, "USB", "audio.wav")), "no USB audio.wav")
        with open(os.path.join(d, "report.md")) as f:
            md = f.read()
        check("-1.4" in md, f"ppm missing from report: {md}")


def test_artifacts_without_matplotlib():
    saved = {k: v for k, v in sys.modules.items() if k == "matplotlib" or k.startswith("matplotlib.")}
    for k in saved:
        del sys.modules[k]
    sys.modules["matplotlib"] = None
    try:
        iq = F.synth(F.tone(1000.0))
        result = A.ModeResult("USB", "pass")
        with tempfile.TemporaryDirectory() as d:
            A.write_mode_artifacts(d, "USB", A.to_baseband(iq, F.SPEC), F.FS, result)
            check(os.path.exists(os.path.join(d, "result.json")), "no result.json")
            check(os.path.exists(os.path.join(d, "audio.wav")), "no audio.wav")
            check(not os.path.exists(os.path.join(d, "spectrum.png")), "spectrum.png written")
            with open(os.path.join(d, "result.json")) as f:
                notes = json.load(f)["notes"]
            check(any("matplotlib" in n for n in notes), f"no note: {notes}")
    finally:
        del sys.modules["matplotlib"]
        sys.modules.update(saved)


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
