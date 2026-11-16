# a format-0 MIDI file: a 2-beat rest at 120 bpm (1 s), then 8 notes, 60 to 67,
# each an eighth of a beat long, a quarter beat apart
import struct, sys
ev = b""
for i in range(8):
    note = 60 + i
    delta = b"\x87\x40" if i == 0 else b"\x78"    # 960 ticks, then 120
    ev += delta + bytes([0x90, note, 100]) + b"\x3c" + bytes([0x80, note, 0])
ev += b"\x00\xff\x2f\x00"
trk = b"MTrk" + struct.pack(">I", len(ev)) + ev
open(sys.argv[1], "wb").write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + trk)
