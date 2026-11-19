#!/usr/bin/env python3
"""level_check.py <file.wav> <peak>: a recorded tone has the level it was sent at.

Reads 16/24/32-bit integer and 32-bit float WAV (plain or extensible), takes
one second from 0.2 s after the tone starts and prints each channel's
peak and its difference from <peak> in dB. Exit 0 if every channel is within
0.5 dB of it."""
import math
import struct
import sys

import numpy as np

path, want = sys.argv[1], float(sys.argv[2])
data = open(path, "rb").read()
if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
    print("level_check: not a WAV file")
    sys.exit(2)
pos, fmt, body = 12, None, None
while pos + 8 <= len(data):
    cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
    chunk = data[pos + 8:pos + 8 + size]
    if cid == b"fmt ":
        fmt = chunk
    elif cid == b"data":
        body = chunk
    pos += 8 + size + (size & 1)
tag, ch, rate = struct.unpack("<HHI", fmt[:8])
bits = struct.unpack("<H", fmt[14:16])[0]
if tag == 0xFFFE:
    tag = struct.unpack("<H", fmt[24:26])[0]
if tag == 3 and bits == 32:
    x = np.frombuffer(body, dtype="<f4")
elif tag == 1 and bits == 16:
    x = np.frombuffer(body, dtype="<i2") / 32768.0
elif tag == 1 and bits == 32:
    x = np.frombuffer(body, dtype="<i4") / 2147483648.0
elif tag == 1 and bits == 24:
    b = np.frombuffer(body[:len(body) // 3 * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
    x = np.where(v >= 1 << 23, v - (1 << 24), v) / 8388608.0
else:
    print("level_check: format %d, %d bits not handled" % (tag, bits))
    sys.exit(2)
x = x[:len(x) // ch * ch].reshape(-1, ch)
# from 0.2 s after the tone starts (a resampler rings at a step from silence)
loud = np.nonzero(np.max(np.abs(x), axis=1) > 0.01)[0]
start = (int(loud[0]) if len(loud) else 0) + rate // 5
seg = x[start:start + rate]
if len(seg) < rate // 2:
    print("level_check: %d frames, too short" % len(x))
    sys.exit(1)
ok, parts = True, []
for c in range(ch):
    peak = float(np.max(np.abs(seg[:, c])))
    db = 20 * math.log10(max(peak, 1e-9) / want)
    ok = ok and abs(db) <= 0.5
    parts.append("ch%d %.4f (%+.2f dB)" % (c + 1, peak, db))
print("level_check: %d Hz %d ch, peak %s: %s" % (rate, ch, ", ".join(parts), "PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
