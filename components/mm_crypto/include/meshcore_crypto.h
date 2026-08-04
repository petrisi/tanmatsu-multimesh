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

// --- direct messages -----------------------------------------------------
//
// A DM is encrypted under the X25519 shared secret between the two identities
// rather than a channel key. The construction is otherwise the channel one with
// two differences that matter, both upstream's choices:
//
//   - the cipher takes the first 16 bytes of the secret, but the MAC is keyed
//     with all 32, so the two lengths are not the same number;
//   - the secret is the raw agreement output, with no KDF over it.

#define MC_SHARED_SECRET_LEN 32

// The shared secret with a peer, from our Ed25519 identity and theirs. Slow
// enough (a scalar multiplication) that callers should cache the result rather
// than derive it per packet.
bool mc_shared_secret(uint8_t out[MC_SHARED_SECRET_LEN], const uint8_t our_private_key[64],
                      const uint8_t their_public_key[MC_PUB_KEY_SIZE]);

typedef struct {
    uint32_t timestamp;   // the sender's clock
    uint8_t  text_type;   // 0 is a plain message; others are CLI and signed forms
    uint8_t  attempt;     // resend counter, low two bits of the flags byte
    char     text[MC_MAX_PAYLOAD_SIZE];

    // The decrypted bytes exactly as they arrived, padding included. The
    // acknowledgement is hashed over a prefix of these rather than over a
    // re-framing of the fields above: reconstructing the plaintext would have to
    // reproduce every choice the sender made, and any difference produces an
    // acknowledgement the sender does not recognise -- which looks like a lost
    // message rather than a hashing bug.
    uint8_t plain[MC_MAX_PAYLOAD_SIZE];
    uint8_t plain_len;   // padded length, a multiple of the cipher block
    uint8_t signed_len;  // 5 + strlen(text): the prefix the acknowledgement covers
} mc_dm_msg_t;

// Frame a direct message: timestamp[4] | flags[1] | text[...] NUL-terminated,
// zero-padded to a cipher block. `attempt` occupies the low two bits of the
// flags byte and exists to make a resend hash differently from the original.
// Returns the padded length, or 0 if the text does not fit.
size_t mc_dm_frame_plaintext(uint32_t timestamp, uint8_t attempt, const char* text, uint8_t* out, size_t out_max,
                             uint8_t* out_unpadded_len);

bool mc_dm_encrypt(const uint8_t secret[MC_SHARED_SECRET_LEN], const uint8_t* plain, size_t padded_len,
                   uint8_t* out_cipher, uint8_t out_mac[32]);

// Verify the MAC, decrypt, and split. False without writing anything when the
// MAC does not match -- which is also how the right contact is picked out, since
// the one-byte sender hash on the wire is not unique.
bool mc_dm_decrypt(const uint8_t secret[MC_SHARED_SECRET_LEN], const uint8_t mac[MC_CIPHER_MAC_SIZE],
                   const uint8_t* cipher, size_t cipher_len, mc_dm_msg_t* out);

// The acknowledgement a message expects: SHA256(plaintext || sender_public_key)
// truncated to four bytes, over the *unpadded* plaintext. Both ends compute it
// over the sender's key, so a receiver proves it read the message rather than
// merely heard the frame.
bool mc_dm_ack_hash(uint8_t out[4], const uint8_t* plain, size_t unpadded_len,
                    const uint8_t sender_public_key[MC_PUB_KEY_SIZE]);

// An identity for a received direct message that ignores the resend counter, so
// a retransmission of a message already shown is recognised as the same one.
//
// The wire payload cannot do this job: the attempt counter sits inside the
// encrypted plaintext, so every retry is a different ciphertext and a different
// payload. Only the decrypted content identifies the message.
#define MC_DM_IDENTITY_LEN 16
void mc_dm_identity(uint8_t out[MC_DM_IDENTITY_LEN], const uint8_t sender_public_key[MC_PUB_KEY_SIZE],
                    uint32_t timestamp, const char* text);
