// SPDX-License-Identifier: MIT
//
// Meshtastic channel crypto: AES-CTR under a channel PSK.
//
// Unlike MeshCore there is no MAC on the packet, so decryption cannot fail --
// a wrong key just produces plausible-length garbage. The protobuf parse in
// meshtastic_wire.c is what actually rejects foreign traffic.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT_MAX_KEY_SIZE 32

typedef struct {
    uint8_t bytes[MT_MAX_KEY_SIZE];
    size_t  length;  // 0 (unencrypted), 16 (AES-128) or 32 (AES-256)
} mt_key_t;

// Expand a configured PSK the way Meshtastic does. Every size means something
// different, and getting this wrong silently produces a channel nobody else can
// read:
//
//   0 bytes    no encryption. A legitimate configuration, not an error: the
//              channel is plaintext and anyone on the frequency can read it.
//   1 byte     an index into the "simple" key family, not a key. Index 0 also
//              means no encryption; index 1 is the well-known default PSK;
//              index n bumps its last byte by n-1. "AQ==" decodes to {0x01},
//              which is why the default channel's PSK looks so short.
//   2-15       a short AES-128 key, zero-padded to 16.
//   16         AES-128.
//   17-31      a short AES-256 key, zero-padded to 32.
//   32         AES-256.
//
// Returns false only for a length above 32, which cannot be any of these.
bool mt_key_expand(const uint8_t* psk, size_t psk_len, mt_key_t* out);

// Channel hash = xor of the channel name bytes, xored with the xor of the
// expanded key bytes. Carried in the clear in the header, so it is a cheap
// pre-filter before spending a decrypt.
uint8_t mt_channel_hash(const char* name, const mt_key_t* key);

// AES-CTR decrypt in place. The 16-byte counter block is packet id as a 64-bit
// little-endian value, then the sender node number as 32-bit little-endian,
// then four zero bytes.
bool mt_decrypt(const mt_key_t* key, uint32_t from_node, uint32_t packet_id, uint8_t* data, size_t length);

// CTR is its own inverse, so this is the same operation under a different name.
// It exists so the transmit path does not read as though it decrypts.
//
// `packet_id` MUST be unique per (key, node): it is half the counter block, and
// reusing one reuses keystream. Two messages XORed together are recoverable
// plaintext, so the id has to come from a hardware RNG rather than a counter
// that restarts at boot.
static inline bool mt_encrypt(const mt_key_t* key, uint32_t from_node, uint32_t packet_id, uint8_t* data,
                              size_t length) {
    return mt_decrypt(key, from_node, packet_id, data, length);
}

// --- PKI direct messages -------------------------------------------------
//
// A direct message between two nodes that have exchanged Curve25519 keys is
// encrypted end to end instead of under a channel key, so nobody else on the
// channel can read it. Unlike the channel cipher this one is authenticated:
// AES-256-CCM with an eight-byte tag, which means a wrong key *does* fail
// rather than yielding garbage.
//
// Nothing marks such a packet on the air. A receiver recognises it by the
// channel hash being zero on a packet addressed to it and nothing else, then
// tries the sender's key -- so the authentication tag is what decides.
//
// The key is SHA-256 over the raw X25519 output. The extra hash is upstream's
// and is what makes this incompatible with MeshCore's otherwise similar scheme.

// Ciphertext is this much longer than plaintext: an 8-byte tag and the 4-byte
// per-packet nonce extension, both carried after the ciphertext.
#define MT_PKI_OVERHEAD 12

// Derive the AES key shared with a peer: SHA-256(X25519(ours, theirs)). Costs a
// scalar multiplication, so cache it per contact rather than deriving per packet.
bool mt_pki_shared_key(uint8_t out[32], const uint8_t our_private_key[32], const uint8_t their_public_key[32]);

// Encrypt in place-ish: writes `length + MT_PKI_OVERHEAD` bytes to `out`.
// `extra_nonce` must be fresh per packet -- with the packet id it is the whole
// of the nonce, and repeating a pair under one key breaks CCM outright.
bool mt_pki_encrypt(const uint8_t shared_key[32], uint32_t from_node, uint32_t packet_id, uint32_t extra_nonce,
                    const uint8_t* plain, size_t length, uint8_t* out, size_t out_max);

// The inverse. `length` is the whole on-air payload including the overhead;
// `out_length` receives the plaintext length. False when the tag does not
// verify, which is the normal way a packet meant for someone else is rejected.
bool mt_pki_decrypt(const uint8_t shared_key[32], uint32_t from_node, uint32_t packet_id, const uint8_t* payload,
                    size_t length, uint8_t* out, size_t out_max, size_t* out_length);

// Round-trip the PKI path with two known key pairs and check that a tampered
// byte fails. This proves the plumbing -- key derivation, nonce assembly, the
// tag -- is self-consistent.
//
// It cannot prove interoperability: the nonce layout and the extra SHA-256 are
// transcribed from upstream and no published test vector exists for them, so
// only a real Meshtastic node on the other end settles that.
bool mt_pki_selftest(void);
