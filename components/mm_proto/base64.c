// SPDX-License-Identifier: MIT

#include "base64.h"
#include <string.h>

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_decode(const char* in, uint8_t* out, size_t out_max) {
    if (in == NULL || out == NULL) return -1;

    uint32_t accumulator = 0;
    int      bits        = 0;
    int      length      = 0;

    for (const char* p = in; *p; p++) {
        if (*p == '=' || *p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') continue;

        const char* found = memchr(ALPHABET, *p, sizeof(ALPHABET) - 1);
        if (found == NULL) return -1;

        accumulator = (accumulator << 6) | (uint32_t)(found - ALPHABET);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if ((size_t)length >= out_max) return -1;
            out[length++] = (uint8_t)(accumulator >> bits);
        }
    }
    return length;
}
