# Draw the application icon and write it as a 32x32 PNG.
#
# The launcher reads /int/apps/<slug>/metadata.json and decodes the icon named
# there with pax_decode_png_fd, so this has to be a real PNG -- written here by
# hand rather than with a library, since the whole file is one uncompressed
# IDAT and that is less trouble than a dependency.
#
# The mark: a transmitting mast. Arcs sweep out from the antenna, blue on the
# left and green on the right, so the two networks the application speaks are
# the picture rather than a caption on it.

import math, struct, zlib, sys, os

S = 32
# Opaque, and the same near-black the application itself runs on. Transparent
# would leave the mark to sit on whatever the launcher happens to draw behind
# it, which is neither dark nor predictable.
BG = (0x0E, 0x0E, 0x14, 255)
MC = (0x4F, 0xA8, 0xFF, 255)   # MeshCore blue
MT = (0x5F, 0xD0, 0x7A, 255)   # Meshtastic green
FG = (0xE8, 0xE8, 0xE8, 255)   # mast and emitter

px = [[BG for _ in range(S)] for _ in range(S)]


def put(x, y, c):
    xi, yi = int(round(x)), int(round(y))
    if 0 <= xi < S and 0 <= yi < S:
        px[yi][xi] = c


cx, cy = 15.5, 9.0

# Arcs, drawn outward from the antenna. Each is a little wider than the last so
# they read as expanding rather than as concentric rings.
for radius, span in ((5.0, 68), (8.4, 62), (11.8, 56)):
    steps = int(radius * 40)
    for i in range(-steps, steps + 1):
        deg = span * i / steps
        ang = math.radians(-90 + deg)
        colour = MC if deg < 0 else MT
        for d in (0.0, 0.7):
            put(cx + (radius + d) * math.cos(ang), cy + (radius + d) * math.sin(ang), colour)

# Emitter at the mast head.
for y in range(S):
    for x in range(S):
        if (x - cx) ** 2 + (y - cy) ** 2 <= 2.4 ** 2:
            px[y][x] = FG

# Mast, splayed legs, ground line.
for y in range(10, 29):
    put(15, y, FG)
    put(16, y, FG)
for i, y in enumerate(range(19, 29)):
    put(15 - (i + 1) // 2, y, FG)
    put(16 + (i + 1) // 2, y, FG)
for x in range(6, 26):
    put(x, 29, FG)


def png(path):
    raw = b"".join(
        b"\x00" + b"".join(struct.pack("BBBB", *px[y][x]) for x in range(S)) for y in range(S)
    )

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0)))  # 8-bit RGBA
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "icon32.png"
    png(out)
    print(f"wrote {out} ({os.path.getsize(out)} bytes)")
    for y in range(S):
        print("".join("." if px[y][x] == BG else ("B" if px[y][x] == MC else "G" if px[y][x] == MT else "#") for x in range(S)))
