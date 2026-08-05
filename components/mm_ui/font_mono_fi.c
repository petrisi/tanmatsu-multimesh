// SPDX-License-Identifier: MIT
//
// The accented glyphs below are derived from PAX's 7x9 bitmap font, which is
// MIT licensed and carries the notice reproduced here as that licence requires:
//
//   Copyright (c) 2022 Julian Scheffers
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the "Software"),
//   to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense,
//   and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// Glyph format, verified against pax_text.c: 9 bytes per glyph, one byte per
// row, bit 0 = leftmost pixel, 7 of 8 bits used. Range `end` is inclusive and
// glyphs are indexed as (codepoint - range.start).
//
// The accented forms are derived from sky_mono's own 'a', 'o', 'A' and 'O'
// rather than drawn by hand, so they match the base letterforms exactly.
// Lowercase keeps its letterform untouched and puts the diacritic in the empty
// rows above it. Capitals are squeezed by a single row to make that room.
//
// The ring on Å/å is only two rows tall; its bottom is closed by the letter's
// own apex, which is also how the character actually looks.
//
// Regenerate with tools/gen_fi_glyphs.py if the base font ever changes.

#include "font_mono_fi.h"
#include <stdint.h>
#include "pax_fonts.h"

// sky_mono's ASCII glyphs. Declared here because PAX does not export it in a
// header, but it does have external linkage.
extern unsigned char const font_bitmap_raw_7x9[];

// 0xC4 Ä, 0xC5 Å
static uint8_t const glyphs_c4_c5[] = {
    0x14, 0x00, 0x1C, 0x22, 0x22, 0x3E, 0x22, 0x22, 0x00,
    0x1C, 0x14, 0x1C, 0x22, 0x22, 0x3E, 0x22, 0x22, 0x00,
};

// 0xD6 Ö
static uint8_t const glyphs_d6[] = {
    0x14, 0x00, 0x1C, 0x22, 0x22, 0x22, 0x22, 0x1C, 0x00,
};

// 0xE4 ä, 0xE5 å
static uint8_t const glyphs_e4_e5[] = {
    0x00, 0x14, 0x00, 0x1C, 0x20, 0x3C, 0x22, 0x3C, 0x00,
    0x00, 0x1C, 0x14, 0x1C, 0x20, 0x3C, 0x22, 0x3C, 0x00,
};

// 0xF6 ö
static uint8_t const glyphs_f6[] = {
    0x00, 0x14, 0x00, 0x1C, 0x22, 0x22, 0x22, 0x1C, 0x00,
};

#define MONO_RANGE(first, last, data)                     \
    {                                                     \
        .type        = PAX_FONT_TYPE_BITMAP_MONO,         \
        .start       = (first),                           \
        .end         = (last),                            \
        .bitmap_mono = {                                  \
            .glyphs = (data),                             \
            .width  = 7,                                  \
            .height = 9,                                  \
            .bpp    = 1,                                  \
        },                                                \
    }

static pax_font_range_t const mono_fi_ranges[] = {
    // ASCII, straight from sky_mono. 0x7F rather than 0x80: the upstream array
    // holds 128 glyphs, so 0x80 would read past its end.
    MONO_RANGE(0x00, 0x7F, font_bitmap_raw_7x9),
    MONO_RANGE(0xC4, 0xC5, glyphs_c4_c5),
    MONO_RANGE(0xD6, 0xD6, glyphs_d6),
    MONO_RANGE(0xE4, 0xE5, glyphs_e4_e5),
    MONO_RANGE(0xF6, 0xF6, glyphs_f6),
};

pax_font_t const pax_font_mono_fi_raw = {
    .name         = "Mono FI",
    .n_ranges     = sizeof(mono_fi_ranges) / sizeof(mono_fi_ranges[0]),
    .ranges       = mono_fi_ranges,
    .default_size = 9,
    .recommend_aa = false,
};
