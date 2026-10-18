#!/usr/bin/env python3
"""TX audio over TCI for the HackRF over-the-air TX test's data modes.

A non-Flex backend takes data-mode audio (DIGU, DIGL, RTTY, DFM) only from
TCI; the client test tone never reaches it. This client keys with
`trx:0,true,tci;` and answers every TX_CHRONO frame the server sends with a
TX_AUDIO frame of float32 stereo at 48 kHz, so the transmitter carries a tone
the analyser can measure.

Standard library and numpy only (a WebSocket client in ~100 lines), so the
test needs no extra packages. AetherSDR drops TCI PTT when this client's
connection closes, so a killed harness cannot leave a data mode keyed.
"""

import base64
import os
import socket
import struct
import threading

import numpy as np

AUDIO_RATE = 48000
_TX_AUDIO, _TX_CHRONO = 2, 3
_FLOAT32 = 3


class TciError(Exception):
    pass


def tone_source(freq_hz, level=0.5):
    """Mono samples of a phase-continuous tone, `n` at a time."""
    state = {"phase": 0.0}

    def source(n):
        step = 2 * np.pi * freq_hz / AUDIO_RATE
        out = level * np.sin(state["phase"] + step * np.arange(n))
        state["phase"] = (state["phase"] + step * n) % (2 * np.pi)
        return out
    return source


def fsk_source(mark_hz, space_hz, half_s, level=0.5):
    """RTTY test pattern: mark for half_s, then space, phase-continuous —
    both tones at equal power, so the analyser's two-tone check can run."""
    state = {"phase": 0.0, "sent": 0}
    switch = int(round(half_s * AUDIO_RATE))

    def source(n):
        idx = state["sent"] + np.arange(n)
        f = np.where(idx < switch, mark_hz, space_hz)
        phase = state["phase"] + np.cumsum(2 * np.pi * f / AUDIO_RATE)
        out = level * np.sin(phase - 2 * np.pi * f / AUDIO_RATE)
        state["phase"] = phase[-1] % (2 * np.pi) if n else state["phase"]
        state["sent"] += n
        return out
    return source


def _frame(opcode, payload):
    """One masked client frame (RFC 6455 requires clients to mask)."""
    mask = os.urandom(4)
    n = len(payload)
    if n < 126:
        head = bytes([0x80 | opcode, 0x80 | n])
    elif n < 65536:
        head = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack(">H", n)
    else:
        head = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack(">Q", n)
    data = np.frombuffer(payload, dtype=np.uint8)
    masked = data ^ np.resize(np.frombuffer(mask, dtype=np.uint8), n)
    return head + mask + masked.tobytes()


def audio_frame(samples, receiver=0):
    """TX_AUDIO: the 64-byte TCI header, then float32 L=R stereo."""
    stereo = np.repeat(np.asarray(samples, dtype="<f4"), 2)
    head = struct.pack("<16I", receiver, AUDIO_RATE, _FLOAT32, 0, 0, stereo.size,
                       _TX_AUDIO, 2, *([0] * 8))
    return head + stereo.tobytes()


class TciClient:
    """One connection to AetherSDR's TCI server for the whole run.

    A reader thread answers pings and TX_CHRONO frames; `start_tx(source)`
    keys and feeds `source(n)` samples, `stop_tx()` unkeys.
    """

    def __init__(self, host="127.0.0.1", port=50001, timeout=5.0):
        self.host, self.port, self.timeout = host, port, timeout
        self._sock = None
        self._buf = b""
        self._send_lock = threading.Lock()
        self._source = None
        self._thread = None
        self._closed = threading.Event()
        self.chronos = 0
        self.error = None

    # ── connection ──────────────────────────────────────────────────────────

    def connect(self):
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f"GET / HTTP/1.1\r\nHost: {self.host}:{self.port}\r\n"
                   "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                raise TciError("TCI server closed the connection during the handshake")
            buf += chunk
        head, self._buf = buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise TciError(f"TCI handshake refused: {head.splitlines()[0]!r}")
        self._sock = s
        while True:                       # the state burst ends with "ready;"
            op, payload = self._read_frame()
            if op == 1 and b"ready;" in payload:
                break
        self._send(1, b"audio_samplerate:48000;")
        self._sock.settimeout(None)
        self._thread = threading.Thread(target=self._reader, name="tci-reader", daemon=True)
        self._thread.start()
        return self

    def close(self):
        if self._sock is None:
            return
        try:
            self.stop_tx()
            self._send(8, b"")
        except OSError:
            pass
        self._closed.set()
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        self._sock = None

    # ── keying ──────────────────────────────────────────────────────────────

    def start_tx(self, source):
        if self._sock is None:
            raise TciError("TCI client is not connected")
        if self.error is not None:
            raise TciError(f"TCI reader failed: {self.error}")
        self._source = source
        self._send(1, b"trx:0,true,tci;")

    def stop_tx(self, lock_timeout=1.0):
        """Unkey. Never blocks longer than lock_timeout: a signal handler may
        call this while the main thread is mid-send."""
        self._source = None
        if self._sock is not None:
            self._send(1, b"trx:0,false;", lock_timeout)

    # ── plumbing ────────────────────────────────────────────────────────────

    def _send(self, opcode, payload, lock_timeout=-1):
        if not self._send_lock.acquire(timeout=lock_timeout):
            return
        try:
            self._sock.sendall(_frame(opcode, payload))
        finally:
            self._send_lock.release()

    def _need(self, n):
        while len(self._buf) < n:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise EOFError("TCI server closed the connection")
            self._buf += chunk

    def _read_frame(self):
        self._need(2)
        b0, b1 = self._buf[0], self._buf[1]
        n, off = b1 & 0x7F, 2
        if n == 126:
            self._need(4)
            n, off = struct.unpack(">H", self._buf[2:4])[0], 4
        elif n == 127:
            self._need(10)
            n, off = struct.unpack(">Q", self._buf[2:10])[0], 10
        self._need(off + n)
        payload, self._buf = self._buf[off:off + n], self._buf[off + n:]
        return b0 & 0x0F, payload

    def _reader(self):
        try:
            while not self._closed.is_set():
                op, payload = self._read_frame()
                if op == 9:
                    self._send(10, payload)
                elif op == 8:
                    break
                elif op == 2 and len(payload) >= 64:
                    head = struct.unpack("<16I", payload[:64])
                    source = self._source
                    if head[6] != _TX_CHRONO or source is None:
                        continue
                    self.chronos += 1
                    n_floats = head[5] or 2048      # the server says how many it wants
                    self._send(2, audio_frame(source(n_floats // 2), receiver=head[0]))
        except (OSError, EOFError) as e:
            if not self._closed.is_set():
                self.error = e
