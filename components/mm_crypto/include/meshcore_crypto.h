// SPDX-License-Identifier: MIT
//
// MeshCore public-channel symmetric crypto.
//
// The channel is a shared-secret room: everyone holding the key can read and
// write. Membership is therefore established by the MAC check alone -- a packet
// encrypted under a different channel key simply fails to verify, which is also
// how we tell "not our channel" apart from "corrupt frame".

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "meshcore_wire.h"

// Upstream MeshCore PUBLIC_GROUP_PSK, base64 "izOH6cXN6mrJ5e26oRXNcg==".
extern const uint8_t MC_PUBLIC_CHANNEL_KEY[MC_CIPHER_KEY_SIZE];

// Must be called once before any other function here (brings up PSA Crypto).
bool mc_crypto_init(void);

// Channel hash = SHA256(key)[0]. Cheap pre-filter: incoming GRP_TXT frames
// carry it in the clear, so a mismatch lets us skip the HMAC entirely.
uint8_t mc_channel_hash(const uint8_t key[MC_CIPHER_KEY_SIZE]);

typedef struct {
    uint32_t timestamp;  // Unix seconds, as stamped by the sender
    uint8_t  text_type;
    char     text[MC_MAX_PAYLOAD_SIZE];  // NUL-terminated, may be "Sender: body"
} mc_grp_msg_t;

// Verify HMAC-SHA256(key)[0:2] over the ciphertext, then AES-128-ECB decrypt and
// split the plaintext into timestamp[4] | text_type[1] | text[...].
// Returns false without writing plaintext when the MAC does not match.
bool mc_grp_decrypt(const mc_grp_txt_t* grp, const uint8_t key[MC_CIPHER_KEY_SIZE], mc_grp_msg_t* out);
