"""Block-streaming WAV I/O on the stdlib `wave` module plus numpy.

WHY BLOCKS
----------
The target is a 20-60 minute multi-channel recording. A 60-minute 4-channel
44.1 kHz 16-bit WAV is ~1.3 GB; as float64 in RAM it would be 5 GB. So nothing
here ever materialises the whole file. `WavReader.blocks()` yields fixed-size
float32 windows and the callers carry only small filter/accumulator state, which
keeps the whole analysis inside a few tens of MB regardless of file length.

WHY A FALLBACK PARSER
---------------------
`wave` handles PCM only. Real USB interfaces and DAWs happily write
WAVE_FORMAT_IEEE_FLOAT (format tag 3) and WAVE_FORMAT_EXTENSIBLE (0xFFFE), and
`wave` raises `wave.Error: unknown format` on both. Refusing to read the user's
recording because of a container detail would be a silly reason for the
instrument to fail, so there is a ~60-line RIFF chunk walker behind it. stdlib
only -- no soundfile, no scipy.
"""

from __future__ import annotations

import struct
import wave
from dataclasses import dataclass
from typing import Iterator

import numpy as np

_WAVE_FORMAT_PCM = 1
_WAVE_FORMAT_IEEE_FLOAT = 3
_WAVE_FORMAT_EXTENSIBLE = 0xFFFE


@dataclass
class _Fmt:
    audio_format: int
    n_channels: int
    sample_rate: int
    bits: int
    data_offset: int
    data_size: int


def _probe_riff(path: str) -> _Fmt:
    """Minimal RIFF walker for the formats `wave` refuses."""
    with open(path, "rb") as fh:
        riff = fh.read(16)
        if len(riff) < 12:
            raise ValueError(f"{path}: file is too short to be a WAV")
        # Name the 64-bit container formats explicitly. A 60-minute, 6-channel,
        # 24-bit, 48 kHz take is 3.1 GB and a 90-minute one is 4.7 GB -- over the
        # 4 GB RIFF ceiling -- at which point interfaces either write RF64/Wave64
        # or silently split the take. "not a RIFF/WAVE file" after a 90-minute
        # session is the worst possible moment to learn that.
        if riff[0:4] == b"RF64":
            raise ValueError(
                f"{path}: this is an RF64 file (a >4 GB recording). avmeasure "
                f"reads plain RIFF/WAVE only. Re-export as 16-bit (which halves "
                f"the size), or split the session, or convert with "
                f"`ffmpeg -i in.wav -c copy -f wav out.wav`.")
        if riff[0:4] == b"riff":
            raise ValueError(
                f"{path}: this is a Sony Wave64 file (a >4 GB recording). "
                f"avmeasure reads plain RIFF/WAVE only -- re-export as 16-bit, "
                f"split the session, or convert it.")
        if riff[0:4] != b"RIFF" or riff[8:12] != b"WAVE":
            raise ValueError(
                f"{path}: not a RIFF/WAVE file (magic {riff[0:4]!r}). If the "
                f"recording is over 4 GB your interface may have written "
                f"RF64 or Wave64, which RIFF cannot represent.")
        fh.seek(12)
        fmt = None
        while True:
            hdr = fh.read(8)
            if len(hdr) < 8:
                break
            cid, csize = struct.unpack("<4sI", hdr)
            body_at = fh.tell()
            if cid == b"fmt ":
                raw = fh.read(min(csize, 40))
                (afmt, nch, rate, _byterate, _align, bits) = struct.unpack(
                    "<HHIIHH", raw[:16])
                if afmt == _WAVE_FORMAT_EXTENSIBLE and len(raw) >= 26:
                    # The real format lives in the SubFormat GUID's first 2 bytes.
                    afmt = struct.unpack("<H", raw[24:26])[0]
                fmt = _Fmt(afmt, nch, rate, bits, 0, 0)
            elif cid == b"data":
                if fmt is None:
                    raise ValueError(f"{path}: data chunk before fmt chunk")
                fmt.data_offset = body_at
                # A streaming writer may leave csize == 0 or 0xFFFFFFFF.
                fh.seek(0, 2)
                remaining = fh.tell() - body_at
                fmt.data_size = min(csize, remaining) if csize else remaining
                return fmt
            fh.seek(body_at + csize + (csize & 1))
    raise ValueError(f"{path}: no data chunk found")


class WavReader:
    """Read a WAV in blocks as float32 in [-1, 1]."""

    def __init__(self, path: str):
        self.path = path
        self._wave = None
        try:
            w = wave.open(path, "rb")
            self.n_channels = w.getnchannels()
            self.sample_rate = w.getframerate()
            self.sampwidth = w.getsampwidth()
            self.n_frames = w.getnframes()
            self.is_float = False
            self._wave = w
        except Exception:
            f = _probe_riff(path)
            if f.audio_format not in (_WAVE_FORMAT_PCM, _WAVE_FORMAT_IEEE_FLOAT):
                raise ValueError(
                    f"{path}: unsupported WAV format tag {f.audio_format}; "
                    f"re-export as PCM or 32-bit float")
            self.n_channels = f.n_channels
            self.sample_rate = f.sample_rate
            self.sampwidth = f.bits // 8
            self.is_float = f.audio_format == _WAVE_FORMAT_IEEE_FLOAT
            self._raw = f
            frame_bytes = self.sampwidth * self.n_channels
            self.n_frames = f.data_size // max(1, frame_bytes)
        if self.n_channels < 1 or self.sample_rate < 1:
            raise ValueError(f"{path}: degenerate WAV header")

    @property
    def duration_s(self) -> float:
        return self.n_frames / self.sample_rate

    @property
    def frame_bytes(self) -> int:
        return self.sampwidth * self.n_channels

    def close(self):
        if self._wave is not None:
            self._wave.close()
            self._wave = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- decoding ---------------------------------------------------------

    def _decode(self, raw: bytes) -> np.ndarray:
        n = len(raw) // self.frame_bytes
        raw = raw[: n * self.frame_bytes]
        if self.is_float:
            if self.sampwidth == 4:
                a = np.frombuffer(raw, dtype="<f4").astype(np.float32)
            elif self.sampwidth == 8:
                a = np.frombuffer(raw, dtype="<f8").astype(np.float32)
            else:
                raise ValueError(f"float WAV with {self.sampwidth * 8} bits")
        elif self.sampwidth == 1:
            # 8-bit WAV is UNSIGNED with a 128 offset -- getting this wrong puts
            # a huge DC step into the light envelope and wrecks edge detection.
            a = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
        elif self.sampwidth == 2:
            a = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
        elif self.sampwidth == 3:
            b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
            v = (b[:, 0].astype(np.int32)
                 | (b[:, 1].astype(np.int32) << 8)
                 | (b[:, 2].astype(np.int32) << 16))
            v = np.where(v & 0x800000, v - 0x1000000, v)
            a = v.astype(np.float32) / 8388608.0
        elif self.sampwidth == 4:
            a = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
        else:
            raise ValueError(f"unsupported sample width {self.sampwidth}")
        return a.reshape(n, self.n_channels)

    def blocks(self, block_frames: int = 1 << 15) -> Iterator[tuple[int, np.ndarray]]:
        """Yield (start_frame, float32[frames, channels])."""
        pos = 0
        if self._wave is not None:
            self._wave.rewind()
            while pos < self.n_frames:
                want = min(block_frames, self.n_frames - pos)
                raw = self._wave.readframes(want)
                if not raw:
                    break
                arr = self._decode(raw)
                if arr.shape[0] == 0:
                    break
                yield pos, arr
                pos += arr.shape[0]
        else:
            with open(self.path, "rb") as fh:
                fh.seek(self._raw.data_offset)
                while pos < self.n_frames:
                    want = min(block_frames, self.n_frames - pos)
                    raw = fh.read(want * self.frame_bytes)
                    if not raw:
                        break
                    arr = self._decode(raw)
                    if arr.shape[0] == 0:
                        break
                    yield pos, arr
                    pos += arr.shape[0]


class WavWriter:
    """Streaming 16-bit PCM writer, used by synth.py.

    int16 rather than float32 because every tool on every platform can open it,
    and 96 dB of dynamic range is far more than a photodiode front-end or a
    10%-depth AM measurement needs.
    """

    def __init__(self, path: str, n_channels: int, sample_rate: int):
        self.w = wave.open(path, "wb")
        self.w.setnchannels(n_channels)
        self.w.setsampwidth(2)
        self.w.setframerate(sample_rate)
        self.n_channels = n_channels
        self.clipped = 0

    def write(self, block: np.ndarray):
        """block: float array [frames, channels] in [-1, 1]."""
        if block.ndim == 1:
            block = block[:, None]
        if block.shape[1] != self.n_channels:
            raise ValueError("channel count mismatch")
        # Count clipping rather than silently wrapping: a clipped synthetic
        # recording would make the observation front-end measure harmonics and
        # the test suite would chase a phantom.
        self.clipped += int(np.count_nonzero(np.abs(block) > 1.0))
        q = np.clip(block, -1.0, 1.0) * 32767.0
        self.w.writeframes(np.rint(q).astype("<i2").tobytes())

    def close(self):
        self.w.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
