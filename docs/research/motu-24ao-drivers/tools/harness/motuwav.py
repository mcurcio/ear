"""Shared pieces of the MOTU 24Ao loopback harness: the tone plan, WAV writing and WAV reading.

The plan (written as JSON beside the tone file) is the one source of truth for what was played:
gen-tones writes it, analyze and analyze-selftest read it. Nothing here touches audio hardware.

Signal layout of a tone file (all times in samples at the plan's rate, from the file's start):
  lead      silence
  id slots  slot k (k = 0..23): only channel k plays its tone f_k, with short fades
  gap       silence
  steady    every channel k plays f_k continuously (fade in / fade out at the edges)
  tail      silence
Every tone is A*sin(2*pi*f_k*n/fs) with n the absolute sample index, so phase is predictable.
"""
import json, math, os, struct
import numpy as np

FS = 48000
CHANNELS = 24
LEVEL_DBFS = -12.0
F_LO, F_HI = 250.0, 14000.0     # all tones sit inside this band ("well inside 200 Hz-15 kHz")
GRID_HZ = 4.0                   # 250 ms windows at 48 kHz have 4 Hz bins: tones sit on bins
NEAR_HZ = 24.0                  # "near": closer than 6 bins
HARMONIC_ORDER = 6              # m*fa vs n*fb collisions checked for m, n <= this

LEAD_S, SLOT_S, SLOT_FADE_S, GAP_S, STEADY_FADE_S, TAIL_S = 0.5, 0.1, 0.005, 0.5, 0.01, 0.25


def choose_frequencies(n=CHANNELS):
    """Deterministic: geometric targets from 300 Hz to 12 kHz, each moved to the nearest 4 Hz grid
    point that is not near a harmonic of (or harmonic-collision with) any tone already chosen."""
    lo_t, hi_t = 300.0, 12000.0
    r = (hi_t / lo_t) ** (1.0 / (n - 1))
    chosen = []
    for k in range(n):
        target = round(lo_t * r ** k / GRID_HZ) * GRID_HZ
        for step in range(0, 400):
            for cand in ((target + step * GRID_HZ, target - step * GRID_HZ) if step else (target,)):
                if F_LO <= cand <= F_HI and _compatible(cand, chosen):
                    chosen.append(cand)
                    break
            else:
                continue
            break
        else:
            raise RuntimeError("no frequency found for channel %d" % k)
    return chosen


def _compatible(f, chosen):
    for g in chosen:
        if abs(f - g) < 4 * NEAR_HZ:
            return False
        # f near any harmonic of g or g near any harmonic of f (up to Nyquist)
        for a, b in ((f, g), (g, f)):
            for h in range(2, int((FS / 2) // b) + 1):
                if abs(a - h * b) < NEAR_HZ:
                    return False
        # low-order harmonic collisions: m*f near n*g
        for m in range(1, HARMONIC_ORDER + 1):
            for nn in range(1, HARMONIC_ORDER + 1):
                if (m, nn) != (1, 1) and abs(m * f - nn * g) < NEAR_HZ:
                    return False
    return True


def make_plan(steady_s, fs=FS, channels=CHANNELS):
    freqs = choose_frequencies(channels)
    s = lambda sec: int(round(sec * fs))
    lead, slot, gap, tail = s(LEAD_S), s(SLOT_S), s(GAP_S), s(TAIL_S)
    slots_start = lead
    steady_start = lead + channels * slot + gap
    steady_end = steady_start + s(steady_s)
    total = steady_end + tail
    return {
        "version": 1, "rate": fs, "channels": channels, "level_dbfs": LEVEL_DBFS,
        "amplitude": 10 ** (LEVEL_DBFS / 20.0), "freqs_hz": freqs,
        "slots_start": slots_start, "slot_len": slot, "slot_fade": s(SLOT_FADE_S),
        "steady_start": steady_start, "steady_end": steady_end, "steady_fade": s(STEADY_FADE_S),
        "total_frames": total,
        "note": "channel k (0-based) carries freqs_hz[k]; sample n of channel k = A*sin(2*pi*f_k*n/rate)*envelope",
    }


def _fade(n, fade):
    """Raised-cosine envelope of length n with fade samples at each end."""
    env = np.ones(n)
    if fade > 0:
        ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(fade) / fade)
        env[:fade] = ramp
        env[n - fade:] = ramp[::-1]
    return env


def render(plan, start, stop):
    """Float64 frames [start, stop) x channels of the plan's signal."""
    fs, A, ch = plan["rate"], plan["amplitude"], plan["channels"]
    n = np.arange(start, stop)
    out = np.zeros((stop - start, ch))
    # id slots
    for k in range(ch):
        a = plan["slots_start"] + k * plan["slot_len"]
        b = a + plan["slot_len"]
        lo, hi = max(a, start), min(b, stop)
        if lo < hi:
            env = _fade(plan["slot_len"], plan["slot_fade"])[lo - a:hi - a]
            out[lo - start:hi - start, k] = A * env * np.sin(2 * np.pi * plan["freqs_hz"][k] * n[lo - start:hi - start] / fs)
    a, b = plan["steady_start"], plan["steady_end"]
    lo, hi = max(a, start), min(b, stop)
    if lo < hi:
        fade = plan["steady_fade"]
        env = np.ones(hi - lo)
        idx = np.arange(lo, hi)
        m = idx < a + fade
        env[m] = 0.5 - 0.5 * np.cos(np.pi * (idx[m] - a) / fade)
        m = idx >= b - fade
        env[m] = 0.5 - 0.5 * np.cos(np.pi * (b - idx[m]) / fade)
        nn = n[lo - start:hi - start]
        for k in range(ch):
            out[lo - start:hi - start, k] = A * env * np.sin(2 * np.pi * plan["freqs_hz"][k] * nn / fs)
    return out


# ---------------------------------------------------------------- WAV writing

def to_s24_bytes(x):
    """float frames -> S24_3LE interleaved bytes (clipped, rounded)."""
    q = np.clip(np.round(x * 8388608.0), -8388608, 8388607).astype("<i4")
    b = q.reshape(-1).view(np.uint8).reshape(-1, 4)[:, :3]
    return b.tobytes()


class WavWriter:
    """Minimal RIFF/WAVE writer: fmt 1 (PCM, 24-bit packed) or 3 (IEEE float 32). Header fixed on close."""
    def __init__(self, path, channels, rate, kind):
        self.f = open(path, "wb")
        self.ch, self.rate, self.kind = channels, rate, kind
        self.bps = 3 if kind == "s24" else 4
        self.frames = 0
        self._header(0)

    def _header(self, data_bytes):
        tag = 1 if self.kind == "s24" else 3
        block = self.ch * self.bps
        fmt = struct.pack("<HHIIHH", tag, self.ch, self.rate, self.rate * block, block, self.bps * 8)
        if tag == 3:
            fmt += struct.pack("<H", 0)   # non-PCM formats carry cbSize
        hdr = b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + data_bytes) + b"WAVE"
        hdr += b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", data_bytes)
        self.f.seek(0)
        self.f.write(hdr)

    def write(self, x):
        if self.kind == "s24":
            self.f.write(to_s24_bytes(x))
        else:
            self.f.write(np.asarray(x, dtype="<f4").tobytes())
        self.frames += x.shape[0]

    def close(self):
        n = self.frames * self.ch * self.bps
        if n > 0xFFFFFFFF - 64:
            raise RuntimeError("WAV over 4 GiB")
        self._header(n)
        self.f.close()


# ---------------------------------------------------------------- WAV reading

class WavReader:
    """Reads PCM 16/24/32-bit integer and 32-bit float WAV (fmt 1, 3 or 0xFFFE) as float64 frames.
    A data chunk size of 0 or past the end of the file (a recorder killed before fixing the
    header) is replaced by what the file holds."""
    def __init__(self, path):
        self.path = path
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            riff = f.read(12)
            if riff[:4] != b"RIFF" or riff[8:12] != b"WAVE":
                raise ValueError("%s: not RIFF/WAVE" % path)
            fmt = None
            while True:
                h = f.read(8)
                if len(h) < 8:
                    raise ValueError("%s: no data chunk" % path)
                cid, clen = h[:4], struct.unpack("<I", h[4:])[0]
                if cid == b"fmt ":
                    fmt = f.read(clen)
                    if clen % 2:
                        f.read(1)
                elif cid == b"data":
                    self.data_off = f.tell()
                    avail = size - self.data_off
                    self.data_len = clen if 0 < clen <= avail else avail
                    break
                else:
                    f.seek(clen + (clen % 2), 1)
        tag, ch, rate, _, block, bits = struct.unpack("<HHIIHH", fmt[:16])
        if tag == 0xFFFE:
            tag = struct.unpack("<H", fmt[24:26])[0]
        self.tag, self.channels, self.rate, self.block, self.bits = tag, ch, rate, block, bits
        self.frames = self.data_len // block
        self.kind = {(1, 16): "s16", (1, 24): "s24", (1, 32): "s32", (3, 32): "f32"}.get((tag, bits))
        if self.kind is None or block != ch * bits // 8:
            raise ValueError("%s: unsupported format tag=%d bits=%d block=%d" % (path, tag, bits, block))
        self.mm = np.memmap(path, dtype=np.uint8, mode="r", offset=self.data_off, shape=(self.frames * block,))

    def read(self, start, stop):
        start, stop = max(0, start), min(self.frames, stop)
        if stop <= start:
            return np.zeros((0, self.channels))
        raw = np.asarray(self.mm[start * self.block:stop * self.block])
        if self.kind == "s24":
            b = raw.reshape(-1, 3).astype(np.int32)
            v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
            v = np.where(v & 0x800000, v - 0x1000000, v)
            x = v / 8388608.0
        elif self.kind == "s16":
            x = raw.view("<i2") / 32768.0
        elif self.kind == "s32":
            x = raw.view("<i4") / 2147483648.0
        else:
            x = raw.view("<f4").astype(np.float64)
        return x.reshape(-1, self.channels)


def load_plan(path):
    with open(path) as f:
        return json.load(f)
