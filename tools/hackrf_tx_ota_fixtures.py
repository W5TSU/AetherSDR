#!/usr/bin/env python3
"""Synthetic RTL-SDR captures for the HackRF over-the-air TX test's own tests.

Test-only code: tools/test_hackrf_tx_ota_analyze.py and
tools/test_hackrf_tx_ota_test.py build their captures here, so both judge the
analyser against the same signals. Nothing in the live harness imports this.

A capture is what `rtl_sdr` would hand back: complex IQ at the RTL-SDR's
centre, with the transmitter's signal sitting at (TX - centre + carrier
offset), keyed for part of the recording, in complex white noise.
"""

import numpy as np

from hackrf_tx_ota_analyze import CaptureSpec

# A lower rate than the live 2.048 MS/s keeps the tests fast; every analysis
# function takes the rate from the CaptureSpec, so nothing depends on it.
FS = 512_000.0
TX = 915.5e6
SPEC = CaptureSpec(sample_rate_hz=FS, rtl_center_hz=TX - 100e3, tx_freq_hz=TX)

HANN_ENBW_BINS = 1.5


def tone(freq_hz, amp=1.0):
    """A complex tone at freq_hz from the carrier (positive = above)."""
    def fn(t):
        return amp * np.exp(2j * np.pi * freq_hz * t)
    return fn


def carrier(amp=1.0):
    return tone(0.0, amp)


def mix(*fns):
    def fn(t):
        out = np.zeros(t.shape, dtype=np.complex128)
        for f in fns:
            out += f(t)
        return out
    return fn


def am(depth, tone_hz=1000.0, amp=1.0):
    def fn(t):
        return amp * (1.0 + depth * np.cos(2 * np.pi * tone_hz * t))
    return fn


def dsb(tone_hz=1000.0, amp=1.0):
    def fn(t):
        return amp * np.cos(2 * np.pi * tone_hz * t)
    return fn


def fm(dev_hz, tone_hz=1000.0, amp=1.0):
    def fn(t):
        phase = (dev_hz / tone_hz) * np.sin(2 * np.pi * tone_hz * t)
        return amp * np.exp(1j * phase)
    return fn


def keyed(elements, edge_s=0.010, jitter_s=0.0, amp=1.0, seed=7):
    """CW: `elements` is morse_elements() output — (down, seconds) pairs.

    Edges are raised-cosine of length edge_s (0 = hard keying, which is what
    the key-click check must catch). jitter_s adds uniform +/- timing error to
    every element, as the bridge's per-edge latency would.
    """
    rng = np.random.default_rng(seed)
    times = []
    t = 0.0
    for down, seconds in elements:
        seconds = max(0.002, seconds + rng.uniform(-jitter_s, jitter_s))
        if down:
            times.append((t, t + seconds))
        t += seconds

    def fn(tt):
        env = np.zeros(tt.shape)
        for start, end in times:
            if edge_s <= 0:
                env[(tt >= start) & (tt < end)] = 1.0
                continue
            rise = (tt >= start) & (tt < start + edge_s)
            env[rise] = 0.5 - 0.5 * np.cos(np.pi * (tt[rise] - start) / edge_s)
            on = (tt >= start + edge_s) & (tt < end - edge_s)
            env[on] = 1.0
            fall = (tt >= end - edge_s) & (tt < end)
            env[fall] = 0.5 + 0.5 * np.cos(np.pi * (tt[fall] - (end - edge_s)) / edge_s)
        return amp * env
    fn.duration_s = t
    return fn


def synth(signal, *, spec=SPEC, total_s=2.0, key_start_s=0.5, key_s=1.0,
          snr_db=50.0, rbw_hz=10.0, carrier_offset_hz=0.0, extra=None,
          seed=1, peak=0.7):
    """Complex IQ as the RTL-SDR records it.

    `signal(t)` is the transmitter's complex baseband (t from key-down), on
    only for [key_start_s, key_start_s + key_s). snr_db is a unit carrier's
    level above the noise in one rbw_hz Hann bin. `extra(t)` (t from the start
    of the capture, relative to TX) is added for the whole recording — an
    interferer. The result is scaled so its peak magnitude is `peak`;
    peak=None leaves it unscaled (a unit carrier at exactly 0 dBFS).
    """
    fs = spec.sample_rate_hz
    n = int(round(total_s * fs))
    t = np.arange(n) / fs
    rng = np.random.default_rng(seed)
    noise_var = 1.0 / (10 ** (snr_db / 10.0) * HANN_ENBW_BINS * rbw_hz / fs)
    x = (rng.standard_normal(n) + 1j * rng.standard_normal(n)) * np.sqrt(noise_var / 2.0)
    on = (t >= key_start_s) & (t < key_start_s + key_s)
    bb = np.zeros(n, dtype=np.complex128)
    bb[on] = signal(t[on] - key_start_s)
    if extra is not None:
        bb += extra(t)
    shift = spec.tx_freq_hz - spec.rtl_center_hz + carrier_offset_hz
    x += bb * np.exp(2j * np.pi * shift * t)
    if peak is not None:
        x *= peak / np.max(np.abs(x))
    return x.astype(np.complex64)


def to_cu8(iq):
    """Interleaved unsigned 8-bit I/Q, the `rtl_sdr` file format."""
    iq = np.asarray(iq)
    out = np.empty(iq.size * 2, dtype=np.float64)
    out[0::2] = iq.real
    out[1::2] = iq.imag
    return np.clip(np.round(out * 127.5 + 127.5), 0, 255).astype(np.uint8).tobytes()


def write_cu8(path, iq):
    with open(path, "wb") as f:
        f.write(to_cu8(iq))
