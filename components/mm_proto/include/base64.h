// SPDX-License-Identifier: MIT
//
// Both networks carry channel keys as base64: Meshtastic in its PSK field,
// MeshCore in the psk_base64 its clients exchange. Shared here so there is one
// implementation to get right rather than one per stack.

#pragma once

#include <stddef.h>
#include <stdint.h>

// Decode `in` into `out`. Returns the number of bytes written, or -1 if the
// input contains a character outside the alphabet. Padding and whitespace are
// skipped. An empty input decodes to zero bytes, which is a legitimate answer
// -- both networks treat an empty key as "no encryption" rather than an error.
int base64_decode(const char* in, uint8_t* out, size_t out_max);
