// SPDX-License-Identifier: MIT
//
// Background signature verification for MeshCore adverts.
//
// A MeshCore advert is Ed25519-signed by the key that *is* the sender's
// identity, so checking it is the only thing that turns a claimed name into a
// proven one. It is also expensive: one verification is two scalar
// multiplications, which on this hardware takes on the order of a second. Doing
// that in the receive path would stall drawing and input for as long as adverts
// keep arriving.
//
// So the receive path queues the work and returns, a background task grinds
// through it, and the event loop -- which owns the model -- collects verdicts
// and applies them. Nothing here touches the model.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_model.h"

// Verify at most one queued advert, waiting up to `wait_ms` for work to appear.
// Returns true if a verdict was produced. Call from a task of its own: this
// blocks, and when it is working it is working for about a second.
bool mc_verify_run_one(uint32_t wait_ms);

typedef struct {
    uint8_t pub_key[NODE_KEY_LEN];  // which node the verdict is about
    bool    valid;
} mc_verify_result_t;

// Collect one finished verdict, if any. Returns false when the queue is empty.
// Call from the thread that owns the model.
bool mc_verify_take_result(mc_verify_result_t* out);
