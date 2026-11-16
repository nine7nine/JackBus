# a 440 Hz stereo sine, 16 bit: mkwav.py <file> <rate> <seconds>
import math, struct, sys, wave
path, rate, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
w = wave.open(path, "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
n = int(rate * secs)
w.writeframes(b"".join(struct.pack("<hh", v, v) for v in (int(0.5 * 32767 * math.sin(2 * math.pi * 440 * i / rate)) for i in range(n))))
w.close()
