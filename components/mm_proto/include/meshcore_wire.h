// SPDX-License-Identifier: MIT
//
// MeshCore on-air framing: the outer packet header and the GRP_TXT (public
// channel) payload. Parse only -- the PoC listens, it does not transmit.
//
// Wire format follows the upstream MeshCore library by Scott Powell /
// rippleradios.com; this is an independent implementation of the same layout,
// cross-checked against the MIT-licensed mirror in Nicolai Electronics'
// tanmatsu MeshCore client.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_MAX_PATH_SIZE    64
#define MC_MAX_PAYLOAD_SIZE 184
#define MC_CIPHER_MAC_SIZE  2
#define MC_CIPHER_KEY_SIZE  16
#define MC_CIPHER_BLOCK     16

// Payload type, header bits 2..5.
typedef enum {
    MC_PAYLOAD_REQ        = 0x0,
    MC_PAYLOAD_RESPONSE   = 0x1,
    MC_PAYLOAD_TXT_MSG    = 0x2,
    MC_PAYLOAD_ACK        = 0x3,
    MC_PAYLOAD_ADVERT     = 0x4,
    MC_PAYLOAD_GRP_TXT    = 0x5,
    MC_PAYLOAD_GRP_DATA   = 0x6,
    MC_PAYLOAD_ANON_REQ   = 0x7,
    MC_PAYLOAD_PATH       = 0x8,
    MC_PAYLOAD_TRACE      = 0x9,
    MC_PAYLOAD_MULTIPART  = 0xA,
    MC_PAYLOAD_RAW_CUSTOM = 0xF,
} mc_payload_type_t;

// Route type, header bits 0..1.
typedef enum {
    MC_ROUTE_TRANSPORT_FLOOD  = 0x0,
    MC_ROUTE_FLOOD            = 0x1,
    MC_ROUTE_DIRECT           = 0x2,
    MC_ROUTE_TRANSPORT_DIRECT = 0x3,
} mc_route_type_t;

typedef struct {
    mc_payload_type_t type;
    mc_route_type_t   route;
    uint8_t           version;
    uint16_t          transport_codes[2];  // only present on the TRANSPORT_* routes
    uint8_t           hop_count;
    uint8_t           bytes_per_hop;  // 1..3
    uint8_t           path_length;    // hop_count * bytes_per_hop
    uint8_t           path[MC_MAX_PATH_SIZE];
    uint8_t           payload_length;
    uint8_t           payload[MC_MAX_PAYLOAD_SIZE];
} mc_packet_t;

// Public-channel payload: channel_hash[1] | mac[2] | ciphertext[...]
typedef struct {
    uint8_t channel_hash;
    uint8_t mac[MC_CIPHER_MAC_SIZE];
    uint8_t cipher_length;
    uint8_t cipher[MC_MAX_PAYLOAD_SIZE];
} mc_grp_txt_t;

// Both return true on success. A malformed or over-long frame returns false and
// leaves the output zeroed rather than partially filled.
bool mc_packet_parse(const uint8_t* data, uint8_t size, mc_packet_t* out);
bool mc_grp_txt_parse(const uint8_t* payload, uint8_t size, mc_grp_txt_t* out);

// Human-readable payload type, for the traffic counters on screen.
const char* mc_payload_type_name(mc_payload_type_t type);
