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
