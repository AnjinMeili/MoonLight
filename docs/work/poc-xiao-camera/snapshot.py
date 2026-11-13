"""One preview frame from a MoonLight device as a PNG: snapshot.py <host> <cols> <rows> <out.png> [scale]."""
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "moondeck" / "scenario"))   # the project's /wsp client
from _preview_ws import PreviewSocket  # noqa: E402

host, cols, rows, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
scale = int(sys.argv[5]) if len(sys.argv) > 5 else 20

ws = PreviewSocket(host)
deadline = time.time() + 10
rgb = None
while time.time() < deadline:
    _, p = ws.read_frame(deadline)
    if p and p[0] == 0x02:
        count, stride = struct.unpack_from("<IH", p, 1)
        rgb = p[9:9 + count * 3]
        break
ws.close()
if rgb is None:
    sys.exit("no RGB frame within 10 s")

w, h = cols * scale, rows * scale
raw = bytearray()
for y in range(h):
    raw.append(0)
    for x in range(w):
        i = (y // scale) * cols + (x // scale)
        raw += rgb[i * 3:i * 3 + 3] if i * 3 + 3 <= len(rgb) else b"\0\0\0"

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
print(f"{out}: {count} lights, {w}x{h}, mean RGB {[sum(rgb[c::3]) // max(1, count) for c in range(3)]}")
