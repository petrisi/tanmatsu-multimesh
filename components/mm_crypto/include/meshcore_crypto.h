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
//
// `key_len` is 16 or 32: MeshCore hashes whichever length the key actually is,
// so passing the wrong one produces a hash that matches nothing.
uint8_t mc_channel_hash(const uint8_t* key, size_t key_len);

// Hashtag channels are public-by-name: anyone who knows the name can derive the
// key, which is the point -- they are topic-based rooms rather than secrets.
// The key is the first 16 bytes of SHA256 over the name *including* the '#'.
//
//   #test -> 9cd8fcf22a47333b591d96a2b848b73f
//   #mesh -> 5b664cde0b08b220612113db980650f3
bool mc_derive_hashtag_key(const char* name, uint8_t out[MC_CIPHER_KEY_SIZE]);

typedef struct {
    uint32_t timestamp;  // Unix seconds, as stamped by the sender
    uint8_t  text_type;
    char     text[MC_MAX_PAYLOAD_SIZE];  // NUL-terminated, may be "Sender: body"
} mc_grp_msg_t;

// Verify HMAC-SHA256(key)[0:2] over the ciphertext, then AES-128-ECB decrypt and
// split the plaintext into timestamp[4] | text_type[1] | text[...].
// Returns false without writing plaintext when the MAC does not match.
// `key_len` is 16 or 32. MeshCore accepts both, so the cipher width follows the
// key rather than being assumed.
bool mc_grp_decrypt(const mc_grp_txt_t* grp, const uint8_t* key, size_t key_len, mc_grp_msg_t* out);

// The inverse. `plain` must already be padded to a 16-byte multiple; ECB has no
// notion of a partial block. Writes `padded_len` bytes of ciphertext and the
// full 32-byte HMAC, of which the wire keeps the first MC_CIPHER_MAC_SIZE.
bool mc_grp_encrypt(const uint8_t* key, size_t key_len, const uint8_t* plain, size_t padded_len, uint8_t* out_cipher,
                    uint8_t out_mac[32]);

// Frame a channel message into the padded plaintext the wire expects:
//   timestamp[4] | text_type[1] | text[...]  zero-padded to a 16-byte multiple.
// Returns the padded length, or 0 if the text does not fit.
size_t mc_grp_frame_plaintext(uint32_t timestamp, const char* text, uint8_t* out, size_t out_max);
