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
    size_t  length;  // 16 or 32
} mt_key_t;

// Expand a configured PSK the way Meshtastic does.
//
// A single byte is an index into the "simple" key family rather than a key:
// index 1 is the well-known default PSK, and index n bumps its last byte by
// n-1. Index 0 disables encryption. Longer keys are zero-padded up to 16 or 32
// bytes. "AQ==" therefore decodes to {0x01}, which means the default key.
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
