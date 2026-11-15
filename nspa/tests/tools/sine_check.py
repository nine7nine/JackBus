#!/usr/bin/env python3
"""sine_check.py <file.wav> <freq>: a recorded sine is what it should be.

Checks the frequency (from zero crossings) and that no sample jumps more than
a sine of that frequency and the recording's peak amplitude can between two
samples: a phase discontinuity (a cycle out of place) shows up as a jump.
Prints one line; exit 0 if it holds."""
import math
import struct
import sys
import wave

path, freq = sys.argv[1], float(sys.argv[2])
w = wave.open(path, "rb")
rate, width, n, ch = w.getframerate(), w.getsampwidth(), w.getnframes(), w.getnchannels()
raw = w.readframes(n)
if width == 2:
    samples = [s / 32768.0 for s in struct.unpack("<%dh" % (n * ch), raw)][::ch]
elif width == 4:
    samples = [s / 2147483648.0 for s in struct.unpack("<%di" % (n * ch), raw)][::ch]
else:
    print("sine_check: %d-byte samples not handled" % width)
    sys.exit(2)
# skip the start (the recorder and the tone may begin mid-cycle) and a silent tail
start = next((i for i, s in enumerate(samples) if abs(s) > 0.01), len(samples)) + rate // 10
body = samples[start:start + rate]
if len(body) < rate // 2:
    print("sine_check: not enough signal (%d samples)" % len(body))
    sys.exit(1)
peak = max(abs(s) for s in body)
crossings = sum(1 for a, b in zip(body, body[1:]) if a < 0 <= b)
measured = crossings * rate / len(body)
max_step = max(abs(b - a) for a, b in zip(body, body[1:]))
limit = 2 * math.pi * freq / rate * peak * 1.05 + 2.0 / 32768
ok = abs(measured - freq) < 2.0 and max_step <= limit
print("sine_check: %.1f Hz (expected %.1f), peak %.3f, largest step %.5f (limit %.5f, from the sine itself): %s"
      % (measured, freq, peak, max_step, limit, "PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
