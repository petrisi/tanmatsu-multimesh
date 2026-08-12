"""Generate the Finnish glyphs for pax_font_mono_fi from PAX's own 7x9 base font.

Derived rather than hand-drawn so the accented forms match the base letterforms
exactly. Format, verified against pax_text.c: 9 bytes per glyph, one byte per
row, bit 0 = leftmost pixel, 7 of 8 bits used.
"""
import os
import re
import sys
from pathlib import Path

W, H = 7, 9

# Found relative to this file so a fresh clone works wherever it sits. The base
# font arrives with the pax-gfx dependency, so it only exists after a build has
# resolved managed components.
ROOT = Path(__file__).resolve().parent.parent
SRC = Path(
    sys.argv[1]
    if len(sys.argv) > 1
    else os.environ.get(
        "PAX_FONT_SOURCE",
        ROOT / "managed_components" / "robotman2412__pax-gfx" / "core" / "src" / "fonts" / "font_bitmap_7x9.c",
    )
)

if not SRC.is_file():
    sys.exit(
        f"Base font not found at {SRC}\n"
        "It comes from the pax-gfx dependency, so build once to fetch managed components,\n"
        "or pass the path as an argument / set PAX_FONT_SOURCE."
    )

body = SRC.read_text(encoding="utf-8", errors="replace").split("font_bitmap_raw_7x9[] = {", 1)[1]
data = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)]


def glyph(cp):
    return data[cp * H:(cp + 1) * H]


def art(rows):
    return ["".join("#" if (b >> x) & 1 else "." for x in range(W)) for b in rows]


def show(name, rows):
    print(f"\n  {name}")
    for line in art(rows):
        print(f"    {line}")


BLANK = 0x00
DOTS = 0b0010100   # ..#.#..  diaeresis
RING = 0b0011100   # ..###..  ring top; its bottom is closed by the letter apex

# --- lowercase: base letterform is preserved untouched in rows 3..7 ---
a_rows = glyph(ord("a"))
o_rows = glyph(ord("o"))

a_umlaut = [BLANK, DOTS, BLANK] + a_rows[3:8] + [BLANK]
o_umlaut = [BLANK, DOTS, BLANK] + o_rows[3:8] + [BLANK]
# The ring sits directly on the letter: rows 1-2 plus the letter's own apex at
# row 3 close a 3x3 circle.
a_ring = [BLANK, RING, DOTS] + a_rows[3:8] + [BLANK]

# --- capitals: squeezed by one row (2..7) so the diacritic has room at 0..1 ---
def squeeze_capital(cp):
    rows = glyph(cp)[1:8]          # 7 rows of letterform
    # Drop one row from below the crossbar/middle to keep the shape balanced.
    return rows[:4] + rows[5:]      # -> 6 rows


A6 = squeeze_capital(ord("A"))
O6 = squeeze_capital(ord("O"))

A_umlaut = [DOTS, BLANK] + A6 + [BLANK]
O_umlaut = [DOTS, BLANK] + O6 + [BLANK]
A_ring = [RING, DOTS] + A6 + [BLANK]

for nm, g in [("A_umlaut (0xC4)", A_umlaut), ("A_ring (0xC5)", A_ring), ("O_umlaut (0xD6)", O_umlaut),
              ("a_umlaut (0xE4)", a_umlaut), ("a_ring (0xE5)", a_ring), ("o_umlaut (0xF6)", o_umlaut)]:
    assert len(g) == H, (nm, len(g))
    show(nm, g)


def carr(rows):
    return ", ".join(f"0x{b:02X}" for b in rows)


print("\n\n----- C -----\n")
print(f"// 0xC4 A-diaeresis, 0xC5 A-ring\nstatic const uint8_t glyphs_c4_c5[] = {{\n"
      f"    {carr(A_umlaut)},\n    {carr(A_ring)},\n}};")
print(f"\n// 0xD6 O-diaeresis\nstatic const uint8_t glyphs_d6[] = {{\n    {carr(O_umlaut)},\n}};")
print(f"\n// 0xE4 a-diaeresis, 0xE5 a-ring\nstatic const uint8_t glyphs_e4_e5[] = {{\n"
      f"    {carr(a_umlaut)},\n    {carr(a_ring)},\n}};")
print(f"\n// 0xF6 o-diaeresis\nstatic const uint8_t glyphs_f6[] = {{\n    {carr(o_umlaut)},\n}};")
