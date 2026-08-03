// SPDX-License-Identifier: MIT
//
// A monospace font that can spell Finnish.
//
// PAX ships exactly one fixed-pitch font, `sky_mono` (7x9), and it is also the
// only PAX font without Latin-1 -- every font that has ä/ö/å is proportional.
// A chat log wants aligned columns, so rather than give that up we reuse
// sky_mono's own glyph data and bolt on the six characters it lacks.
//
// No fork of PAX is needed: a font is just a struct pointing at ranges, and
// PAX's glyph array has external linkage.

#pragma once

#include "pax_types.h"

// Drop-in replacement for pax_font_sky_mono. Identical for U+0000..U+007F,
// and additionally covers Ä Å Ö ä å ö.
extern pax_font_t const pax_font_mono_fi_raw;
#define pax_font_mono_fi (&pax_font_mono_fi_raw)
