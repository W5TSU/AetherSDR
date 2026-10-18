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

from dataclasses import dataclass, field

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
