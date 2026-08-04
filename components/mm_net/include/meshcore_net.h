// SPDX-License-Identifier: MIT
//
// The parts of the MeshCore stack that do not fit behind mesh_net_t, because
// they are things MeshCore does and Meshtastic does not.
//
// Chiefly: MeshCore learns routes, and a route is a claim that stops being true
// without warning. Sending along one has to be attempted, checked and given up
// on, which is a conversation between the stack and the event loop rather than
// something either can do alone.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_model.h"

// Upstream's send-timeout constants, so our patience matches what the other end
// expects of itself.
#define MC_SEND_TIMEOUT_BASE_MS    500
#define MC_FLOOD_TIMEOUT_FACTOR    16
#define MC_DIRECT_PERHOP_FACTOR    6
#define MC_DIRECT_PERHOP_EXTRA_MS  250

// Attempts along a learned route before concluding it no longer works. The
// original send plus this many retries; after that the route is discarded and
// the message floods instead.
#define MC_DIRECT_RETRIES 2

// Build one attempt at a direct message, filling in what the retry ladder needs
// to make the next one. `attempt` is carried inside the encrypted plaintext, so
// each retry is a distinct packet with a distinct expected acknowledgement --
// an identical one would be dropped as a duplicate before it arrived.
//
// Returns the frame length, or 0.
uint8_t mc_encode_dm_attempt(const identity_t* identity, const node_t* peer, const char* text, uint8_t attempt,
                             bool use_path, message_t* msg, uint8_t* out, size_t out_max);

// How long to wait for an acknowledgement, from upstream's formula: flooding
// scales with airtime alone, a directed send with the number of hops it must
// cross and come back over.
uint32_t mc_ack_timeout_ms(const node_t* peer, bool direct);
