#!/usr/bin/env python3
"""Measure HackRF transmit captures recorded by an RTL-SDR (`rtl_sdr` .cu8).

The analysis half of the HackRF over-the-air TX test
(docs/HACKRF-TX-TEST.md). Pure measurement: IQ arrays in, results out. The
orchestrator, tools/hackrf_tx_ota_test.py, keys the radio and records; this
module only judges what was recorded, so a saved run can be judged again
(`--reanalyze <dir>`) after a threshold changes.

Every level is relative dB. An RTL-SDR is not a calibrated receiver, so
nothing here claims watts or dBm.
"""

import argparse
import json
import os
import sys
from dataclasses import asdict, dataclass, field
from fractions import Fraction

import numpy as np
from scipy import signal as sps


@dataclass
class CaptureSpec:
    sample_rate_hz: float
    rtl_center_hz: float
    tx_freq_hz: float


@dataclass
class CheckResult:
    name: str
    passed: bool
    measured: float | None
    limit: str
    unit: str


@dataclass
class ModeResult:
    mode: str
    status: str                      # "pass" | "fail" | "skip" | "abort"
    checks: list[CheckResult] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)


# ── Capture handling ───────────────────────────────────────────────────────

def load_cu8(path):
    """Read an `rtl_sdr` file: interleaved unsigned 8-bit I/Q.

    Returns (complex64 IQ scaled to +/-1, clip fraction). The clip fraction is
    the share of bytes sitting on a rail (0 or 255): an overloaded RTL-SDR
    folds its own distortion products into every measurement.
    """
    raw = np.fromfile(path, dtype=np.uint8)
    raw = raw[: raw.size - (raw.size % 2)]
    clip = float(np.count_nonzero((raw == 0) | (raw == 255))) / max(1, raw.size)
    v = (raw.astype(np.float32) - 127.5) / 127.5
    return (v[0::2] + 1j * v[1::2]).astype(np.complex64), clip


def to_baseband(iq, spec, offset_hz=0.0):
    """Mix so that tx_freq_hz + offset_hz sits at 0 Hz."""
    shift = spec.tx_freq_hz + offset_hz - spec.rtl_center_hz
    n = np.arange(iq.size, dtype=np.float64)
    # Phase in cycles, wrapped before the multiply by 2*pi, so a 20 M-sample
    # capture keeps full float64 resolution to its last sample.
    cycles = np.mod(n * (shift / spec.sample_rate_hz), 1.0)
    return (iq * np.exp(-2j * np.pi * cycles)).astype(np.complex64)


def _lowpass_decimate(bb, fs, cutoff_hz, out_fs):
    """Band-limit to +/-cutoff_hz and decimate to about out_fs."""
    down = max(1, int(fs // out_fs))
    if down > 1:
        bb = sps.resample_poly(bb, 1, down)
        fs = fs / down
    sos = sps.butter(8, cutoff_hz, fs=fs, output="sos")
    return sps.sosfilt(sos, bb), fs


def keyed_window(bb, fs, trim_s=0.05, max_gap_s=1.0):
    """[start, end) sample indices of the keyed transmission, or None.

    The envelope within +/-20 kHz of TX, smoothed over 10 ms, must sit 10 dB
    above the lead-in's median (the first 0.3 s, recorded before the key).
    Runs closer than max_gap_s merge — CW keys on and off inside one
    transmission. The longest run must last 200 ms; trim_s comes off each end
    so the measurement sees neither edge.
    """
    env_bb, efs = _lowpass_decimate(bb, fs, 20e3, 64e3)
    smooth = max(1, int(round(0.010 * efs)))
    power = np.convolve(np.abs(env_bb) ** 2, np.ones(smooth) / smooth, mode="same")
    lead = power[: max(1, int(0.3 * efs))]
    threshold = np.median(lead) * 10.0           # +10 dB
    above = power > threshold
    edges = np.flatnonzero(np.diff(above.astype(np.int8)))
    starts = list(edges[~above[edges]] + 1) if edges.size else []
    ends = list(edges[above[edges]] + 1) if edges.size else []
    if above[0]:
        starts.insert(0, 0)
    if above[-1]:
        ends.append(above.size)
    runs = []
    for s, e in zip(starts, ends):
        if runs and (s - runs[-1][1]) < max_gap_s * efs:
            runs[-1][1] = e
        else:
            runs.append([s, e])
    if not runs:
        return None
    s, e = max(runs, key=lambda r: r[1] - r[0])
    if (e - s) < 0.2 * efs:
        return None
    scale = fs / efs
    trim = int(round(trim_s * fs))
    start = int(s * scale) + trim
    end = int(e * scale) - trim
    return (start, end) if end > start else None


# ── Spectrum measurements ──────────────────────────────────────────────────

def spectrum_db(seg, fs, rbw_hz=10.0):
    """Averaged Hann spectrum: (baseband Hz ascending, dB re full scale).

    Power-spectrum scaling, so a tone reads its own power whatever the bin
    width; noise reads its power in one bin (Hann ENBW is 1.5 bins).
    """
    nperseg = int(round(fs / rbw_hz))
    nperseg = min(nperseg, seg.size)
    freqs, pxx = sps.welch(seg, fs=fs, window="hann", nperseg=nperseg,
                           return_onesided=False, scaling="spectrum",
                           detrend=False)
    freqs = np.fft.fftshift(freqs)
    pxx = np.fft.fftshift(pxx)
    return freqs, 10.0 * np.log10(np.maximum(pxx, 1e-30))


def peak_near(freqs, db, offset_hz, search_hz=50.0):
    """(frequency, level) of the strongest bin within +/-search_hz of offset.

    The frequency is refined by a parabola through the peak and its two
    neighbours, so it is good to a fraction of a bin.
    """
    sel = np.flatnonzero(np.abs(freqs - offset_hz) <= search_hz)
    if sel.size == 0:
        return float(offset_hz), float("-inf")
    i = int(sel[np.argmax(db[sel])])
    f, level = float(freqs[i]), float(db[i])
    if 0 < i < db.size - 1:
        a, b, c = db[i - 1], db[i], db[i + 1]
        denom = a - 2 * b + c
        if denom < 0:
            p = 0.5 * (a - c) / denom
            f += p * (freqs[1] - freqs[0])
            level = float(b - 0.25 * (a - c) * p)
    return f, level


def noise_floor_db(freqs, db, exclude):
    """Median level of the bins within +/-100 kHz, outside the excluded
    (lo, hi) ranges. The median ignores the few bins any signal occupies."""
    keep = np.abs(freqs) <= 100e3
    for lo, hi in exclude:
        keep &= ~((freqs >= lo) & (freqs <= hi))
    if not np.any(keep):
        return float("nan")
    return float(np.median(db[keep]))


def channel_busy(bb, fs, half_width_hz=30e3, threshold_db=15.0):
    """True when something within +/-half_width_hz of TX stands threshold_db
    above the noise — another station on the channel we are about to use."""
    freqs, db = spectrum_db(bb, fs, rbw_hz=1000.0)
    floor = noise_floor_db(freqs, db, exclude=[])
    near = np.abs(freqs) <= half_width_hz
    return bool(np.max(db[near]) - floor >= threshold_db)


# ── Per-mode checks ────────────────────────────────────────────────────────

# Every numeric limit the checks use, in one place. The first over-the-air run
# set them (docs/HACKRF-TX-TEST.md, "Tuning the thresholds"); a change here is
# judged again on saved captures with `--reanalyze`.
THRESHOLDS = {
    "tune_snr_db": 55.0,       # ramp target: carrier over noise, 10 Hz bin (~20 dB in 25 kHz)
    "min_snr_db": 20.0,        # below this nothing was received worth measuring
    "ref_search_hz": 300.0,    # how far a mode's own reference may sit from TUNE's (drift)
    "ssb_opposite_db": 25.0,
    "ssb_carrier_db": 20.0,
    "rtty_shift_hz": 170.0,
    "rtty_shift_tol_hz": 10.0,
    "rtty_balance_db": 6.0,
    "cw_offset_hz": 50.0,
    "cw_dit_tol": 0.15,
    "cw_width_hz": 400.0,
    "am_symmetry_db": 2.0,
    "am_depth_min": 0.30,
    "am_depth_max": 1.00,
    "am_harmonic_db": 25.0,
    "dsb_symmetry_db": 2.0,
    "dsb_carrier_db": 20.0,
    "fm_tone_tol": 0.02,
    "fm_dev_hz": 5000.0,
    "fmn_dev_hz": 2500.0,
    "dev_margin": 0.10,
    "fmn_ratio_min": 0.4,
    "fmn_ratio_max": 0.6,
    "spur_db": 40.0,           # report-only
}

UPPER_SSB = ("USB", "DIGU")
LOWER_SSB = ("LSB", "DIGL")
FM_MODES = ("FM", "FMN", "DFM")

MORSE = {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.",
    "G": "--.", "H": "....", "I": "..", "J": ".---", "K": "-.-", "L": ".-..",
    "M": "--", "N": "-.", "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.",
    "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-",
    "Y": "-.--", "Z": "--..", "0": "-----", "1": ".----", "2": "..---",
    "3": "...--", "4": "....-", "5": ".....", "6": "-....", "7": "--...",
    "8": "---..", "9": "----.", "/": "-..-.",
}
_MORSE_BACK = {v: k for k, v in MORSE.items()}


@dataclass
class Reference:
    carrier_offset_hz: float          # TUNE carrier minus nominal TX
    fm_peak_dev_hz: float | None = None


def _db_to_lin(db):
    return 10.0 ** (np.asarray(db) / 10.0)


def _lin_mean_db(*dbs):
    return float(10.0 * np.log10(np.mean(_db_to_lin(dbs))))


def _status(checks):
    return "pass" if all(c.passed for c in checks) else "fail"


def _segment(bb, fs, nominal_s, trim_s=0.05):
    window = keyed_window(bb, fs, trim_s=trim_s)
    if window is None and nominal_s is not None:
        a = int((nominal_s[0] + trim_s) * fs)
        b = int((nominal_s[1] - trim_s) * fs)
        window = (a, b) if b > a else None
    return window


def _no_signal(mode, why="no signal received"):
    return ModeResult(mode, "fail",
                      [CheckResult("signal", False, None, "a keyed transmission", "")],
                      [why])


def _spurious(freqs, db, main_hz, main_db, exclude_hz):
    """Strongest bin within +/-200 kHz, away from the wanted signal. Report-only:
    a busy 900 MHz band puts other stations here, and they are not ours."""
    sel = (np.abs(freqs) <= 200e3) & (np.abs(freqs - main_hz) > exclude_hz)
    if not np.any(sel):
        return CheckResult("spurious", True, None, "report-only", "dBc"), None
    i = np.flatnonzero(sel)[np.argmax(db[sel])]
    rel = float(db[i] - main_db)
    note = None
    if rel > -THRESHOLDS["spur_db"]:
        note = (f"spurious: {rel:+.1f} dBc at {freqs[i] / 1e3:+.1f} kHz "
                f"(another station, or ours: check spectrum.png)")
    return CheckResult("spurious", True, round(rel, 1),
                       f"report-only (flagged above -{THRESHOLDS['spur_db']:g} dBc)", "dBc"), note


def measure_tune(iq, spec, freq_tol_ppm, nominal_s=None):
    """TUNE: one carrier. Returns the result and the Reference every other mode
    starts from (its frequency is the two radios' combined crystal error)."""
    fs = spec.sample_rate_hz
    search = 2.0 * freq_tol_ppm * 1e-6 * spec.tx_freq_hz
    # Find the carrier first, anywhere the crystals could put it, so the keyed
    # window (+/-20 kHz) is looked for around it rather than around nominal.
    cf, cdb = spectrum_db(to_baseband(iq, spec), fs, rbw_hz=100.0)
    coarse, _ = peak_near(cf, cdb, 0.0, search_hz=search)
    bb = to_baseband(iq, spec, offset_hz=coarse)
    window = _segment(bb, fs, nominal_s)
    if window is None:
        return _no_signal("TUNE"), Reference(0.0)
    freqs, db = spectrum_db(bb[window[0]:window[1]], fs)
    # freqs are relative to the coarse carrier; f0 is relative to TX, as the
    # ppm, the Reference and the spur note's offset are.
    f_rel, level = peak_near(freqs, db, 0.0, search_hz=300.0)
    f0 = f_rel + coarse
    floor = noise_floor_db(freqs, db, exclude=[(f_rel - 10e3, f_rel + 10e3)])
    snr = level - floor
    if snr < THRESHOLDS["min_snr_db"]:
        return _no_signal("TUNE"), Reference(0.0)
    ppm = f0 / spec.tx_freq_hz * 1e6
    checks = [
        CheckResult("snr", True, round(snr, 1),
                    f">= {THRESHOLDS['min_snr_db']:g} dB (target {THRESHOLDS['tune_snr_db']:g})", "dB"),
        CheckResult("frequency", abs(ppm) <= freq_tol_ppm, round(ppm, 3),
                    f"within +/-{freq_tol_ppm:g} ppm", "ppm"),
    ]
    notes = [f"carrier {f0:+.1f} Hz from {spec.tx_freq_hz / 1e6:.6f} MHz (both radios' crystal error)"]
    if snr < THRESHOLDS["tune_snr_db"]:
        notes.append(f"weak signal: {snr:.1f} dB, under the {THRESHOLDS['tune_snr_db']:g} dB target")
    spur, note = _spurious(freqs + coarse, db, f0, level, 5e3)
    checks.append(spur)
    if note:
        notes.append(note)
    return ModeResult("TUNE", _status(checks), checks, notes), Reference(f0)


def _check_line_snr(freqs, db, f, level, exclude):
    floor = noise_floor_db(freqs, db, exclude=exclude)
    snr = level - floor
    return CheckResult("signal", snr >= THRESHOLDS["min_snr_db"], round(snr, 1),
                       f">= {THRESHOLDS['min_snr_db']:g} dB over the noise", "dB")


def _ssb(mode, freqs, db, tone_hz, upper):
    rs = THRESHOLDS["ref_search_hz"]
    sign = 1.0 if upper else -1.0
    fw, lw = peak_near(freqs, db, sign * tone_hz, rs)      # wanted sideband
    fo, lo = peak_near(freqs, db, -sign * tone_hz, rs)     # the other one
    carrier_hz = fw - sign * tone_hz
    _, lc = peak_near(freqs, db, carrier_hz, 30.0)
    checks = [
        _check_line_snr(freqs, db, fw, lw, [(-tone_hz - rs - 2e3, tone_hz + rs + 2e3)]),
        CheckResult("opposite_sideband", lw - lo >= THRESHOLDS["ssb_opposite_db"], round(lw - lo, 1),
                    f">= {THRESHOLDS['ssb_opposite_db']:g} dB below the tone", "dB"),
        CheckResult("carrier_suppression", lw - lc >= THRESHOLDS["ssb_carrier_db"], round(lw - lc, 1),
                    f">= {THRESHOLDS['ssb_carrier_db']:g} dB below the tone", "dB"),
    ]
    return checks, fw, lw


def _rtty(freqs, db, mark_hz=2125.0):
    rs = THRESHOLDS["ref_search_hz"]
    shift = THRESHOLDS["rtty_shift_hz"]
    fm_, lm = peak_near(freqs, db, mark_hz, rs)
    fs_, ls = peak_near(freqs, db, fm_ + shift, 60.0)
    _, lo = peak_near(freqs, db, -mark_hz - shift / 2, rs + shift)
    carrier_hz = fm_ - mark_hz
    _, lc = peak_near(freqs, db, carrier_hz, 30.0)
    top = max(lm, ls)
    checks = [
        _check_line_snr(freqs, db, fm_, lm, [(-mark_hz - 2e3, mark_hz + 2e3)]),
        CheckResult("shift", abs((fs_ - fm_) - shift) <= THRESHOLDS["rtty_shift_tol_hz"],
                    round(fs_ - fm_, 1),
                    f"{shift:g} +/- {THRESHOLDS['rtty_shift_tol_hz']:g} Hz", "Hz"),
        CheckResult("balance", abs(lm - ls) <= THRESHOLDS["rtty_balance_db"], round(lm - ls, 1),
                    f"mark and space within {THRESHOLDS['rtty_balance_db']:g} dB", "dB"),
        CheckResult("opposite_sideband", top - lo >= THRESHOLDS["ssb_opposite_db"], round(top - lo, 1),
                    f">= {THRESHOLDS['ssb_opposite_db']:g} dB below the tones", "dB"),
        CheckResult("carrier_suppression", top - lc >= THRESHOLDS["ssb_carrier_db"], round(top - lc, 1),
                    f">= {THRESHOLDS['ssb_carrier_db']:g} dB below the tones", "dB"),
    ]
    return checks, fm_, top


def _am(freqs, db, tone_hz):
    fc, lc = peak_near(freqs, db, 0.0, THRESHOLDS["ref_search_hz"])
    _, lu = peak_near(freqs, db, fc + tone_hz, 30.0)
    _, ll = peak_near(freqs, db, fc - tone_hz, 30.0)
    _, hu = peak_near(freqs, db, fc + 2 * tone_hz, 30.0)
    _, hl = peak_near(freqs, db, fc - 2 * tone_hz, 30.0)
    side = _lin_mean_db(lu, ll)
    depth = 2.0 * 10.0 ** ((side - lc) / 20.0)
    harm = side - _lin_mean_db(hu, hl)
    checks = [
        _check_line_snr(freqs, db, fc, lc, [(fc - 3 * tone_hz, fc + 3 * tone_hz)]),
        CheckResult("symmetry", abs(lu - ll) <= THRESHOLDS["am_symmetry_db"], round(lu - ll, 1),
                    f"sidebands within {THRESHOLDS['am_symmetry_db']:g} dB", "dB"),
        CheckResult("depth", THRESHOLDS["am_depth_min"] <= depth <= THRESHOLDS["am_depth_max"],
                    round(depth, 3),
                    f"{THRESHOLDS['am_depth_min']:g}..{THRESHOLDS['am_depth_max']:g}", ""),
        CheckResult("harmonics", harm >= THRESHOLDS["am_harmonic_db"], round(harm, 1),
                    f"2nd-harmonic sidebands >= {THRESHOLDS['am_harmonic_db']:g} dB down", "dB"),
    ]
    return checks, fc, lc


def _dsb(freqs, db, tone_hz):
    rs = THRESHOLDS["ref_search_hz"]
    fu, lu = peak_near(freqs, db, tone_hz, rs)
    fl, ll = peak_near(freqs, db, -tone_hz, rs)
    carrier_hz = 0.5 * (fu + fl)
    _, lc = peak_near(freqs, db, carrier_hz, 30.0)
    side = _lin_mean_db(lu, ll)
    checks = [
        _check_line_snr(freqs, db, fu, lu, [(-tone_hz - rs - 2e3, tone_hz + rs + 2e3)]),
        CheckResult("symmetry", abs(lu - ll) <= THRESHOLDS["dsb_symmetry_db"], round(lu - ll, 1),
                    f"sidebands within {THRESHOLDS['dsb_symmetry_db']:g} dB", "dB"),
        CheckResult("carrier_suppression", side - lc >= THRESHOLDS["dsb_carrier_db"], round(side - lc, 1),
                    f">= {THRESHOLDS['dsb_carrier_db']:g} dB below the sidebands", "dB"),
    ]
    return checks, fu, max(lu, ll)


def _line_powers(freqs, db, center_hz, spacing_hz, k_max, half_bw=25.0):
    """Linear power summed within +/-half_bw of center + k*spacing."""
    lin = _db_to_lin(db)
    df = freqs[1] - freqs[0]
    out = np.empty(2 * k_max + 1)
    for j, k in enumerate(range(-k_max, k_max + 1)):
        f = center_hz + k * spacing_hz
        a = np.searchsorted(freqs, f - half_bw)
        b = np.searchsorted(freqs, f + half_bw + df)
        out[j] = lin[a:b].sum()
    return out


def fit_fm(freqs, db, tone_hz, k_max=12):
    """(center Hz, measured tone Hz, beta, fit residual) for a tone-modulated FM
    signal, from its Bessel lines. The comb of lines is located first (its
    centre drifts with the crystals, its spacing is the audio tone); beta then
    fits the lines' relative powers to J_k(beta)^2."""
    from scipy.special import jv
    rs = THRESHOLDS["ref_search_hz"]
    best = (-1.0, 0.0, tone_hz)
    for spacing in np.arange(tone_hz * 0.97, tone_hz * 1.03 + 0.1, max(1.0, tone_hz / 500)):
        for c in np.arange(-rs, rs + 1.0, 4.0):
            p = _line_powers(freqs, db, c, spacing, k_max, half_bw=12.0).sum()
            if p > best[0]:
                best = (p, c, spacing)
    _, center, spacing = best
    for c in np.arange(center - 4.0, center + 4.5, 1.0):     # refine the centre
        p = _line_powers(freqs, db, c, spacing, k_max, half_bw=12.0).sum()
        if p > best[0]:
            best = (p, c, spacing)
    _, center, spacing = best
    p = _line_powers(freqs, db, center, spacing, k_max)
    p = p / p.sum()
    ks = np.arange(-k_max, k_max + 1)
    betas = np.arange(0.0, 10.0, 0.005)
    model = jv(ks[None, :], betas[:, None]) ** 2
    err = ((model - p[None, :]) ** 2).sum(axis=1)
    i = int(np.argmin(err))
    return float(center), float(spacing), float(betas[i]), float(err[i])


def _fm(mode, freqs, db, tone_hz, ref):
    center, spacing, beta, resid = fit_fm(freqs, db, tone_hz)
    dev = beta * spacing
    limit = THRESHOLDS["fmn_dev_hz"] if mode == "FMN" else THRESHOLDS["fm_dev_hz"]
    _, lc = peak_near(freqs, db, center, 30.0)
    total = 10.0 * np.log10(_line_powers(freqs, db, center, spacing, 12).sum())
    checks = [
        _check_line_snr(freqs, db, center, total, [(center - 15e3, center + 15e3)]),
        CheckResult("modulated", beta >= 0.1, round(beta, 3), "beta >= 0.1 (audio reached the modulator)", ""),
        CheckResult("tone_frequency", abs(spacing - tone_hz) / tone_hz <= THRESHOLDS["fm_tone_tol"],
                    round(spacing, 1), f"{tone_hz:g} Hz +/- {THRESHOLDS['fm_tone_tol'] * 100:g} %", "Hz"),
        CheckResult("deviation", dev <= limit * (1 + THRESHOLDS["dev_margin"]), round(dev, 1),
                    f"<= {limit:g} Hz + {THRESHOLDS['dev_margin'] * 100:g} %", "Hz"),
    ]
    if mode == "FMN" and ref.fm_peak_dev_hz:
        ratio = dev / ref.fm_peak_dev_hz
        checks.append(CheckResult(
            "fmn_ratio", THRESHOLDS["fmn_ratio_min"] <= ratio <= THRESHOLDS["fmn_ratio_max"],
            round(ratio, 3), f"{THRESHOLDS['fmn_ratio_min']:g}..{THRESHOLDS['fmn_ratio_max']:g} of FM", ""))
    notes = [f"Bessel fit: beta {beta:.3f}, residual {resid:.2e}"]
    return checks, center, total, notes, dev


def morse_elements(text, wpm):
    """(key down, seconds) for each element and gap of `text`, PARIS timing."""
    unit = 1.2 / wpm
    out = []
    words = [w for w in text.upper().split() if w]
    for wi, word in enumerate(words):
        if wi:
            out.append((False, 7 * unit))
        for li, ch in enumerate(word):
            if li:
                out.append((False, 3 * unit))
            code = MORSE.get(ch)
            if code is None:
                raise ValueError(f"no Morse for {ch!r}")
            for si, sym in enumerate(code):
                if si:
                    out.append((False, unit))
                out.append((True, unit if sym == "." else 3 * unit))
    return out


def _runs(mask):
    """[(value, start, length)] of consecutive equal values."""
    edges = np.flatnonzero(np.diff(mask.astype(np.int8))) + 1
    starts = np.concatenate(([0], edges))
    ends = np.concatenate((edges, [mask.size]))
    return [(bool(mask[s]), int(s), int(e - s)) for s, e in zip(starts, ends)]


def decode_cw(bb, fs):
    """(text, median dit seconds) from the key-down envelope near the carrier."""
    x, efs = _lowpass_decimate(bb, fs, 600.0, 8000.0)
    env = np.abs(x)
    smooth = max(1, int(0.002 * efs))
    env = np.convolve(env, np.ones(smooth) / smooth, mode="same")
    hi, lo = np.percentile(env, 95), np.percentile(env, 5)
    on = env > np.sqrt(max(hi, 1e-12) * max(lo, 1e-12))
    runs = [(v, s, n) for v, s, n in _runs(on) if n >= 0.008 * efs]
    # merge neighbours left equal after dropping glitches
    merged = []
    for v, s, n in runs:
        if merged and merged[-1][0] == v:
            merged[-1] = (v, merged[-1][1], merged[-1][2] + n)
        else:
            merged.append((v, s, n))
    while merged and not merged[0][0]:
        merged.pop(0)
    while merged and not merged[-1][0]:
        merged.pop()
    downs = np.array([n for v, _, n in merged if v], dtype=float)
    if downs.size == 0:
        return "", 0.0
    lower = np.sort(downs)[: max(1, downs.size // 2)]
    dit = float(np.median(lower))
    out, letter = [], ""
    for v, _, n in merged:
        if v:
            letter += "." if n < 2.0 * dit else "-"
        else:
            if n >= 5.0 * dit:
                out.append(_MORSE_BACK.get(letter, "?"))
                out.append(" ")
                letter = ""
            elif n >= 2.0 * dit:
                out.append(_MORSE_BACK.get(letter, "?"))
                letter = ""
    if letter:
        out.append(_MORSE_BACK.get(letter, "?"))
    dits = downs[downs < 2.0 * dit]
    return "".join(out), float(np.median(dits) / efs)


def _cw(mode, bb, fs, window, freqs, db, cw_text, cw_wpm):
    fc, lc = peak_near(freqs, db, 0.0, THRESHOLDS["ref_search_hz"])
    a = max(0, window[0] - int(0.2 * fs))
    b = min(bb.size, window[1] + int(0.2 * fs))
    seg = bb[a:b] * np.exp(-2j * np.pi * fc * np.arange(b - a) / fs)
    text, dit_s = decode_cw(seg, fs)
    sel = np.flatnonzero(db >= lc - 40.0)
    sel = sel[np.abs(freqs[sel] - fc) <= 5e3]
    width = float(freqs[sel].max() - freqs[sel].min()) if sel.size else 0.0
    want_dit = 1.2 / cw_wpm
    checks = [
        _check_line_snr(freqs, db, fc, lc, [(fc - 6e3, fc + 6e3)]),
        CheckResult("carrier_offset", abs(fc) <= THRESHOLDS["cw_offset_hz"], round(fc, 1),
                    f"within {THRESHOLDS['cw_offset_hz']:g} Hz of the TUNE carrier (the dial)", "Hz"),
        CheckResult("dit_length", abs(dit_s - want_dit) / want_dit <= THRESHOLDS["cw_dit_tol"],
                    round(dit_s * 1000, 1),
                    f"{want_dit * 1000:.0f} ms +/- {THRESHOLDS['cw_dit_tol'] * 100:g} %", "ms"),
        CheckResult("width", width <= THRESHOLDS["cw_width_hz"], round(width, 1),
                    f"<= {THRESHOLDS['cw_width_hz']:g} Hz at -40 dB (key clicks)", "Hz"),
    ]
    if cw_text is not None:
        sent = " ".join(cw_text.upper().split())
        checks.insert(1, CheckResult("decode", text.strip() == sent, None,
                                     f"decodes to {sent!r}", ""))
    return checks, fc, lc, [f"decoded {text.strip()!r}"]


def analyze_mode(mode, iq, spec, ref, tone_hz=1000.0, cw_text=None, cw_wpm=15.0,
                 nominal_s=None):
    """One mode's checks, measured on its keyed segment relative to its own
    strongest line (the TUNE Reference is only where to look: the crystals drift
    ~0.1 ppm in minutes)."""
    mode = mode.upper()
    fs = spec.sample_rate_hz
    bb = to_baseband(iq, spec, offset_hz=ref.carrier_offset_hz)
    is_cw = mode in ("CW", "CWR")
    window = _segment(bb, fs, nominal_s, trim_s=0.0 if is_cw else 0.05)
    if window is None:
        return _no_signal(mode)
    freqs, db = spectrum_db(bb[window[0]:window[1]], fs)
    notes = []
    if mode in UPPER_SSB or mode in LOWER_SSB:
        checks, main_f, main_db = _ssb(mode, freqs, db, tone_hz, upper=mode in UPPER_SSB)
        excl = 5e3
    elif mode == "RTTY":
        checks, main_f, main_db = _rtty(freqs, db)
        excl = 5e3
    elif mode in ("AM", "SAM"):
        checks, main_f, main_db = _am(freqs, db, tone_hz)
        excl = 5e3
    elif mode == "DSB":
        checks, main_f, main_db = _dsb(freqs, db, tone_hz)
        excl = 5e3
    elif mode in FM_MODES:
        checks, main_f, main_db, notes, _ = _fm(mode, freqs, db, tone_hz, ref)
        excl = 15e3
    elif is_cw:
        checks, main_f, main_db, notes = _cw(mode, bb, fs, window, freqs, db, cw_text, cw_wpm)
        excl = 5e3
    else:
        raise ValueError(f"no checks for mode {mode}")
    spur, note = _spurious(freqs, db, main_f, main_db, excl)
    checks.append(spur)
    if not checks[0].passed:
        # Spur levels relative to the noise floor mean nothing; say what is wrong.
        opposite = {c.name: c for c in checks}.get("opposite_sideband")
        if (opposite is not None and opposite.measured is not None
                and opposite.measured <= -THRESHOLDS["ssb_opposite_db"]):
            got = "LSB" if mode in UPPER_SSB or mode == "RTTY" else "USB"
            notes.insert(0, f"the tone is on the other sideband: the radio sent {got}, "
                            f"{-opposite.measured:.1f} dB above the wanted side")
        else:
            notes.insert(0, "no signal received above the noise")
    elif note:
        notes.append(note)
    return ModeResult(mode, _status(checks), checks, notes)


def fm_peak_deviation(result):
    """The deviation an FM ModeResult measured, for FMN's ratio check."""
    for c in result.checks:
        if c.name == "deviation":
            return c.measured
    return None


def fm_demod(bb, fs):
    """Instantaneous frequency in Hz, low-passed to 15 kHz."""
    x, efs = _lowpass_decimate(bb, fs, 15e3, 48e3)
    d = np.angle(x[1:] * np.conj(x[:-1])) * efs / (2 * np.pi)
    sos = sps.butter(6, 15e3, fs=efs, output="sos")
    return sps.sosfilt(sos, d)


AUDIO_FS = 48000.0
CW_BFO_HZ = 700.0


def _to_audio_rate(bb, fs):
    """Resample complex baseband to exactly AUDIO_FS (resample_poly filters)."""
    r = Fraction(AUDIO_FS / fs).limit_denominator(1000)
    return sps.resample_poly(bb, r.numerator, r.denominator)


def _complex_lowpass(x, fs, cutoff_hz, order=8):
    return sps.sosfilt(sps.butter(order, cutoff_hz, fs=fs, output="sos"), x)


def demod_audio(mode, bb, fs):
    """48 kHz float32 audio for the WAV: what a receiver in `mode` would play,
    with bb's 0 Hz as the dial frequency (carrier for AM/FM/CW).

    Fixed gain, not normalised: a capture with no signal plays as quiet noise,
    and a wrong-sideband transmission plays as near silence. FM is scaled so
    the mode's nominal deviation is full scale; CW beats against a 700 Hz BFO.
    """
    mode = mode.upper()
    x = _to_audio_rate(bb, fs)
    t = np.arange(x.size) / AUDIO_FS
    if mode in UPPER_SSB or mode in LOWER_SSB or mode == "RTTY":
        # Shift the 0..3 kHz passband (or -3..0 for lower) to be centred on
        # 0 Hz, low-pass to +/-1.5 kHz, shift back: only that sideband is left.
        sign = -1.0 if mode in LOWER_SSB else 1.0
        rot = np.exp(2j * np.pi * sign * 1500.0 * t)
        audio = np.real(_complex_lowpass(x * np.conj(rot), AUDIO_FS, 1500.0) * rot)
    elif mode in ("AM", "SAM"):
        env = np.abs(_complex_lowpass(x, AUDIO_FS, 5e3))
        audio = env - np.mean(env)
    elif mode == "DSB":
        # Squaring loop: x^2 holds a line at twice the carrier phase.
        y = _complex_lowpass(x, AUDIO_FS, 5e3)
        p = _complex_lowpass(y * y, AUDIO_FS, 20.0, order=2)
        theta = np.unwrap(np.angle(p)) / 2.0
        audio = np.real(y * np.exp(-1j * theta))
    elif mode in FM_MODES:
        nominal = THRESHOLDS["fmn_dev_hz"] if mode == "FMN" else THRESHOLDS["fm_dev_hz"]
        d = fm_demod(x, AUDIO_FS)
        d = np.concatenate([d[:1], d])          # fm_demod returns one sample fewer
        audio = (d - np.median(d)) / nominal
    elif mode in ("CW", "CWR"):
        y = _complex_lowpass(x, AUDIO_FS, 300.0)
        audio = np.real(y * np.exp(2j * np.pi * CW_BFO_HZ * t))
    else:
        raise ValueError(f"no demodulator for mode {mode}")
    return np.clip(audio, -1.0, 1.0).astype(np.float32)


# ── Reports, artifacts, re-analysis ────────────────────────────────────────

SPECTRUM_SPAN_HZ = 25e3


def exit_code(results):
    """0 every mode passed (skips allowed), 1 a check failed, 2 an abort."""
    statuses = {r.status for r in results}
    if "abort" in statuses:
        return 2
    if "fail" in statuses:
        return 1
    return 0


def result_from_dict(d):
    return ModeResult(d["mode"], d["status"],
                      [CheckResult(**c) for c in d.get("checks", [])],
                      list(d.get("notes", [])))


def _json_default(o):
    if isinstance(o, np.generic):        # np.bool_ / np.float64 from the checks
        return o.item()
    raise TypeError(f"{type(o).__name__} is not JSON serialisable")


def _write_json(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f, indent=2, default=_json_default)
        f.write("\n")


def _plot_spectrum(path, mode, freqs, db, result):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    sel = np.abs(freqs) <= SPECTRUM_SPAN_HZ
    fig, ax = plt.subplots(figsize=(10, 4))
    ax.plot(freqs[sel] / 1e3, db[sel], linewidth=0.6)
    ax.set_xlabel("kHz from the dial frequency")
    ax.set_ylabel("dB (relative, 10 Hz RBW)")
    ax.set_title(f"{mode}: {result.status}")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=100)
    plt.close(fig)


def write_mode_artifacts(mode_dir, mode, bb, fs, result):
    """result.json, audio.wav (int16, 48 kHz) and, when matplotlib imports,
    spectrum.png (+/-25 kHz, the keyed segment) into mode_dir. bb has the
    mode's reference frequency at 0 Hz."""
    from scipy.io import wavfile
    os.makedirs(mode_dir, exist_ok=True)
    window = keyed_window(bb, fs) or (0, bb.size)
    seg = bb[window[0]:window[1]]
    demod_mode = "CW" if mode.upper() == "TUNE" else mode
    audio = demod_audio(demod_mode, bb, fs)
    wavfile.write(os.path.join(mode_dir, "audio.wav"), int(AUDIO_FS),
                  np.round(audio * 32767.0).astype(np.int16))
    try:
        freqs, db = spectrum_db(seg, fs)
        _plot_spectrum(os.path.join(mode_dir, "spectrum.png"), mode, freqs, db, result)
    except ImportError:
        result.notes.append("spectrum.png not written: matplotlib is not installed")
    _write_json(os.path.join(mode_dir, "result.json"), asdict(result))


def _fmt_measured(c):
    if c.measured is None:
        return "nothing measured"
    return f"{c.measured:g} {c.unit}".rstrip()


META_LABELS = [
    ("freq_mhz", "Frequency (MHz)"),
    ("ppm", "Combined crystal error (ppm)"),
    ("power", "Drive (%)"),
    ("rtl_gain_db", "RTL-SDR gain (dB)"),
    ("callsign", "Callsign"),
    ("git_commit", "AetherSDR commit"),
]


def _fmt_meta_value(v):
    if isinstance(v, dict):
        return ", ".join(f"{k} {v[k]:g}" if isinstance(v[k], (int, float)) else f"{k} {v[k]}"
                         for k in v)
    return str(v)


def write_reports(out_dir, run_meta, results):
    """report.json and report.md: one table row per mode, then the run meta."""
    code = exit_code(results)
    _write_json(os.path.join(out_dir, "report.json"),
                {"meta": run_meta, "results": [asdict(r) for r in results], "exit_code": code})
    lines = ["# HackRF over-the-air TX test", "",
             "| Mode | Status | Failed checks (measured vs limit) |",
             "|---|---|---|"]
    for r in results:
        failed = [f"{c.name}: {_fmt_measured(c)} (limit {c.limit})"
                  for c in r.checks if not c.passed]
        lines.append(f"| {r.mode} | {r.status} | {'; '.join(failed) or '—'} |")
    noted = [r for r in results if r.notes]
    if noted:
        lines += ["", "## Notes", ""]
        for r in noted:
            lines += [f"- **{r.mode}:** {n}" for n in r.notes]
    lines += ["", "## Run", ""]
    known = {k for k, _ in META_LABELS}
    for key, label in META_LABELS:
        if key in run_meta:
            lines.append(f"- {label}: {_fmt_meta_value(run_meta[key])}")
    for key in run_meta:
        if key not in known:
            lines.append(f"- {key}: {_fmt_meta_value(run_meta[key])}")
    lines += ["", f"Exit code: {code}", ""]
    with open(os.path.join(out_dir, "report.md"), "w") as f:
        f.write("\n".join(lines))


def _check_measured(result, name):
    for c in result.checks:
        if c.name == name:
            return c.measured
    return None


def build_meta(config, entries, results):
    """The report's run meta from run.json's config and mode entries: the
    TUNE-measured ppm, and the drive and RTL-SDR gain each mode ran at."""
    meta = {k: v for k, v in config.items() if k not in ("tone_hz", "cw_text", "cw_wpm")}
    tune = next((r for r in results if r.mode == "TUNE"), None)
    if tune is not None and _check_measured(tune, "frequency") is not None:
        meta["ppm"] = _check_measured(tune, "frequency")
    for key in ("power", "rtl_gain_db"):
        per_mode = {e["mode"]: e[key] for e in entries if key in e}
        if per_mode:
            meta[key] = per_mode
    return meta


def reanalyze(out_dir):
    """Judge a saved run again from its run.json and .cu8 files, rewriting
    every mode's artifacts and the reports. Modes without a capture (skipped,
    aborted) keep the status run.json recorded."""
    with open(os.path.join(out_dir, "run.json")) as f:
        run = json.load(f)
    config = run.get("config", {})
    tol = float(config.get("freq_tol_ppm", 25.0))
    entries = run["modes"]
    # TUNE first (it is every mode's reference), FM before FMN (the ratio).
    order = sorted(range(len(entries)),
                   key=lambda i: {"TUNE": 0, "FM": 1}.get(entries[i]["mode"].upper(), 2))
    results = [None] * len(entries)
    ref = Reference(0.0)
    for i in order:
        e = entries[i]
        mode = e["mode"].upper()
        if not e.get("capture"):
            results[i] = ModeResult(mode, e.get("status", "skip"), [], list(e.get("notes", [])))
            continue
        spec = CaptureSpec(**e["spec"])
        iq, clip = load_cu8(os.path.join(out_dir, e["capture"]))
        nominal = tuple(e["nominal_s"]) if e.get("nominal_s") else None
        if mode == "TUNE":
            result, ref = measure_tune(iq, spec, tol, nominal_s=nominal)
            offset = ref.carrier_offset_hz
        else:
            result = analyze_mode(mode, iq, spec, ref,
                                  tone_hz=float(e.get("tone_hz", config.get("tone_hz", 1000.0))),
                                  cw_text=e.get("cw_text", config.get("cw_text")),
                                  cw_wpm=float(e.get("cw_wpm", config.get("cw_wpm", 15.0))),
                                  nominal_s=nominal)
            offset = ref.carrier_offset_hz
            if mode == "FM":
                ref.fm_peak_dev_hz = fm_peak_deviation(result)
        if clip > 0.001:
            result.notes.append(f"RTL-SDR clipped {clip * 100:.2f} % of samples")
        result.notes = list(e.get("notes", [])) + result.notes
        write_mode_artifacts(os.path.join(out_dir, os.path.dirname(e["capture"]) or mode),
                             mode, to_baseband(iq, spec, offset_hz=offset),
                             spec.sample_rate_hz, result)
        results[i] = result
    write_reports(out_dir, build_meta(config, entries, results), results)
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="hackrf_tx_ota_analyze.py",
        description="Judge a saved HackRF over-the-air TX test run again, without\n"
                    "transmitting. To run the test itself, use:\n\n"
                    "  python3 tools/hackrf_tx_ota_test.py --callsign <YOUR CALL>",
        epilog="Guide: docs/HACKRF-TX-TEST.md",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reanalyze", metavar="DIR",
                    help="a results folder holding run.json and the .cu8 captures")
    args, unknown = ap.parse_known_args(sys.argv[1:] if argv is None else argv)
    if unknown:
        ap.error(f"unrecognized arguments: {' '.join(unknown)}\n"
                 "Test options such as --callsign and --out-dir belong to the test itself:\n"
                 "  python3 tools/hackrf_tx_ota_test.py --callsign <YOUR CALL> --out-dir <DIR>")
    if not args.reanalyze:
        ap.print_help(sys.stderr)
        return 2
    try:
        results = reanalyze(args.reanalyze)
    except (OSError, KeyError, ValueError) as e:
        print(f"reanalyze: {e}", file=sys.stderr)
        return 2
    for r in results:
        print(f"{r.mode:6s} {r.status}")
    print(f"report: {os.path.join(args.reanalyze, 'report.md')}")
    return exit_code(results)


if __name__ == "__main__":
    sys.exit(main())
