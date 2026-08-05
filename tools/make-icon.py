# Draw the application icon and write it as 16x16, 32x32 and 64x64 PNGs.
#
# The launcher reads /int/apps/<slug>/metadata.json and decodes the icon named
# there with pax_decode_png_fd, so these have to be real PNGs -- written here by
# hand rather than with a library, since each file is one uncompressed IDAT and
# that is less trouble than a dependency.
#
# The mark: a transmitting mast. Arcs sweep out from the antenna, blue on the
# left and green on the right, so the two networks the application speaks are
# the picture rather than a caption on it.
#
# 16x16 is drawn from its own geometry rather than scaled down. At that size the
# mast is one pixel wide and the third arc has nowhere to go, so a decimated 32
# turns to mush; the small mark drops an arc and the splayed legs and keeps what
# still reads.

import math, struct, zlib, sys, os

# Opaque, and the same near-black the application itself runs on. Transparent
# would leave the mark to sit on whatever the launcher happens to draw behind
# it, which is neither dark nor predictable.
BG = (0x0E, 0x0E, 0x14, 255)
MC = (0x4F, 0xA8, 0xFF, 255)   # MeshCore blue
MT = (0x5F, 0xD0, 0x7A, 255)   # Meshtastic green
FG = (0xE8, 0xE8, 0xE8, 255)   # mast and emitter


def render(S):
    """Draw the mark at size S and return the pixel grid."""
    k  = S / 32.0
    px = [[BG for _ in range(S)] for _ in range(S)]

    def put(x, y, c):
        xi, yi = int(round(x)), int(round(y))
        if 0 <= xi < S and 0 <= yi < S:
            px[yi][xi] = c

    cx, cy = S / 2.0 - 0.5, 9.0 * k

    # Arcs, drawn outward from the antenna, each a little wider than the last so
    # they read as expanding rather than as concentric rings. The small mark
    # keeps two: a third would land within a pixel of the second.
    arcs = ((5.0, 68), (8.4, 62), (11.8, 56)) if S >= 32 else ((4.8, 66), (8.6, 58))
    spread = [d * k for d in (0.0, 0.7)] if S >= 32 else [0.0]

    for radius, span in arcs:
        r     = radius * k
        steps = max(24, int(r * 40))
        for i in range(-steps, steps + 1):
            deg    = span * i / steps
            ang    = math.radians(-90 + deg)
            colour = MC if deg < 0 else MT
            for d in spread:
                put(cx + (r + d) * math.cos(ang), cy + (r + d) * math.sin(ang), colour)

    # Emitter at the mast head.
    er = 2.4 * k if S >= 32 else 1.6
    for y in range(S):
        for x in range(S):
            if (x - cx) ** 2 + (y - cy) ** 2 <= er ** 2:
                px[y][x] = FG

    # Mast: one pixel wide on the small mark, two scaled columns otherwise.
    half   = 1 if S < 32 else max(1, int(round(k)))
    mast_x = range(int(S / 2) - half, int(S / 2) + half) if S >= 32 else [int(S / 2) - 1, int(S / 2)]
    top    = int(round(10 * k))
    base   = int(round(29 * k))
    for y in range(top, base):
        for x in mast_x:
            put(x, y, FG)

    # Splayed legs, and the ground line they stand on. The legs are the first
    # thing to go at 16: two pixels of taper read as noise beside a mast that is
    # itself one pixel wide.
    if S >= 32:
        legs_from = int(round(19 * k))
        thickness = max(1, int(round(k)))
        for y in range(legs_from, base):
            # Integer stepping, not a float slope: rounding a half-pixel offset
            # lands on the same column twice and leaves gaps in between, which
            # shows up as a ragged leg.
            #
            # The slope does not scale with k. Twice the size means twice the
            # rows and twice the reach, so the ratio between them is unchanged;
            # scaling it as well splays the legs to double the angle.
            offset = (y - legs_from + 1) // 2
            for t in range(thickness):
                put(min(mast_x) - offset + t, y, FG)
                put(max(mast_x) + offset - t, y, FG)

    ground = 26 if S >= 32 else 22
    for x in range(int(round(6 * k)), int(round(ground * k))):
        put(x, base, FG)

    return px


def png(path, px):
    S   = len(px)
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


def preview(px):
    for row in px:
        print("".join("." if c == BG else ("B" if c == MC else "G" if c == MT else "#") for c in row))


if __name__ == "__main__":
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    for size in (16, 32, 64):
        px   = render(size)
        path = os.path.join(out_dir, f"icon{size}.png")
        png(path, px)
        print(f"wrote {path} ({os.path.getsize(path)} bytes)")
        if size <= 32:
            preview(px)
            print()
