// SPDX-License-Identifier: MIT

#include "dedup.h"
#include <string.h>

// Keys shorter than DEDUP_KEY_LEN are zero-padded, so a short payload cannot
// alias a longer one that happens to share a prefix.
static void make_key(uint8_t out[DEDUP_KEY_LEN], const uint8_t* key, size_t len) {
    memset(out, 0, DEDUP_KEY_LEN);
    if (key == NULL) return;
    if (len > DEDUP_KEY_LEN) len = DEDUP_KEY_LEN;
    memcpy(out, key, len);
}

void dedup_reset(dedup_t* state) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
}

void dedup_remember(dedup_t* state, const uint8_t* key, size_t len) {
    if (state == NULL) return;

    make_key(state->keys[state->head], key, len);
    state->head = (uint8_t)((state->head + 1) % DEDUP_ENTRIES);
    if (state->count < DEDUP_ENTRIES) state->count++;
}

bool dedup_check(dedup_t* state, const uint8_t* key, size_t len) {
    if (state == NULL) return false;

    uint8_t probe[DEDUP_KEY_LEN];
    make_key(probe, key, len);

    for (int i = 0; i < state->count; i++) {
        if (memcmp(state->keys[i], probe, DEDUP_KEY_LEN) == 0) return true;
    }

    dedup_remember(state, key, len);
    return false;
}
