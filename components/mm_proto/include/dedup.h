// SPDX-License-Identifier: MIT
//
// Duplicate suppression for flood-routed traffic.
//
// Both networks flood: a message reaches you once directly and again through
// every repeater that heard it. Without this the same text appears three or four
// times and the log becomes unusable within minutes.
//
// What identifies a repeat differs by network, so the key is supplied by the
// caller rather than computed here:
//
//   Meshtastic  the (from, id) pair from the header. Canonical, and what every
//               other client uses.
//   MeshCore    the leading bytes of the payload. There is no packet id, and
//               the header and transport codes change between retransmits --
//               but the payload (MAC plus ciphertext) is identical, so it is
//               the only stable thing to fingerprint.
//
// Pure C with no dependencies, so it is testable on the host.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEDUP_ENTRIES 32
#define DEDUP_KEY_LEN 16

typedef struct {
    uint8_t keys[DEDUP_ENTRIES][DEDUP_KEY_LEN];
    uint8_t head;
    uint8_t count;
} dedup_t;

void dedup_reset(dedup_t* state);

// Record a key without testing it. Used for our own transmissions, so that our
// message coming back off a repeater is recognised rather than shown twice.
void dedup_remember(dedup_t* state, const uint8_t* key, size_t len);

// True when this key has been seen recently. A new key is remembered, so the
// caller does not have to.
//
// The ring holds the last DEDUP_ENTRIES keys with no time limit: on a busy mesh
// a late repeat can fall out of the window and show as a duplicate message.
// Tracking time would not fix that, only trade one bound for another.
bool dedup_check(dedup_t* state, const uint8_t* key, size_t len);
