// SPDX-License-Identifier: MIT
//
// Public-key work that is too slow for the receive path.
//
// Two things here cost a scalar multiplication -- verifying a MeshCore advert
// signature, and deriving the shared secret for a conversation -- and one of
// those takes on the order of a second on this hardware. Doing either where
// packets arrive would stall drawing and input for as long as traffic keeps
// coming.
//
// So the stacks queue work and return, a background task grinds through it, and
// the event loop -- which owns the model -- collects results and applies them.
// Nothing in here touches the model, and no key material outlives a job: each
// carries the copy of the private key it needs, so the worker holds no state
// that has to be kept in step with the identity.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_model.h"

typedef enum {
    CRYPTO_JOB_MC_VERIFY = 0,  // is this advert signature genuine?
    CRYPTO_JOB_MC_SECRET,      // agree with a MeshCore identity
    CRYPTO_JOB_MT_SECRET,      // agree with a Meshtastic contact
} crypto_job_kind_t;

typedef struct {
    crypto_job_kind_t kind;
    bool              ok;

    // Which node the result belongs to. Each network uses the field it
    // identifies nodes by, and the other is left zero.
    uint8_t  pub_key[NODE_KEY_LEN];  // MeshCore
    uint32_t node_num;               // Meshtastic

    uint8_t secret[NODE_KEY_LEN];  // the two _SECRET kinds only
} crypto_result_t;

// Queue an advert for signature checking. False when the queue is full, so the
// caller can leave the node unchecked and try again on its next advert rather
// than waiting forever on a job that was never accepted.
bool crypto_queue_mc_verify(const uint8_t pub_key[NODE_KEY_LEN], const uint8_t signature[64],
                            const uint8_t* signed_bytes, uint8_t signed_len);

// Queue a shared-secret derivation. The private key is copied into the job.
bool crypto_queue_mc_secret(const uint8_t pub_key[NODE_KEY_LEN], const uint8_t our_private_key[64]);
bool crypto_queue_mt_secret(uint32_t node_num, const uint8_t their_public_key[NODE_KEY_LEN],
                            const uint8_t our_private_key[32]);

// Do one queued job, waiting up to `wait_ms` for one to appear. Returns true if
// something was done. Call from a task of its own: it blocks, and when it is
// working it is working for about a second.
bool crypto_run_one(uint32_t wait_ms);

// Collect one finished result. False when there is none. Call from the thread
// that owns the model.
bool crypto_take_result(crypto_result_t* out);

// Allocate the queues. Called once at startup, before either stack runs.
bool crypto_jobs_init(void);
