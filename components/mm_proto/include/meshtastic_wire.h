// SPDX-License-Identifier: MIT
//
// Meshtastic on-air framing: the 16-byte header, and just enough protobuf to
// pull a text message out of the decrypted Data submessage.
//
// Wire layout derived from the Meshtastic firmware (GPL-3.0) -- this is an
// independent implementation of the format, not a copy of its code, so this
// file stays MIT like the rest of the project.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT_HEADER_SIZE      16
#define MT_MAX_PAYLOAD_SIZE 240

// Header flag byte.
#define MT_FLAGS_HOP_LIMIT_MASK  0x07
#define MT_FLAGS_WANT_ACK_MASK   0x08
#define MT_FLAGS_VIA_MQTT_MASK   0x10
#define MT_FLAGS_HOP_START_MASK  0xE0
#define MT_FLAGS_HOP_START_SHIFT 5

// Port numbers we care about; everything else is counted and dropped.
#define MT_PORTNUM_TEXT_MESSAGE 1
#define MT_PORTNUM_POSITION     3
#define MT_PORTNUM_NODEINFO     4
#define MT_PORTNUM_ROUTING      5
#define MT_PORTNUM_TELEMETRY    67

typedef struct {
    uint32_t to;
    uint32_t from;
    uint32_t id;
    uint8_t  flags;
    uint8_t  channel_hash;
    uint8_t  next_hop;
    uint8_t  relay_node;

    uint8_t  hop_limit;
    uint8_t  hop_start;
    bool     want_ack;  // the sender is waiting for a routing acknowledgement

    uint8_t  payload_length;
    uint8_t  payload[MT_MAX_PAYLOAD_SIZE];  // still encrypted at this point
} mt_packet_t;

// Split a received frame into header + ciphertext. False if it is too short to
// be a Meshtastic packet at all.
bool mt_packet_parse(const uint8_t* data, uint8_t size, mt_packet_t* out);

// Hops actually taken = hop_start - hop_limit, or 0 when hop_start is unset
// (older senders leave it zero).
uint8_t mt_hops_taken(const mt_packet_t* pkt);

typedef struct {
    uint32_t portnum;
    uint8_t  payload[MT_MAX_PAYLOAD_SIZE];
    size_t   payload_length;

    // Which earlier packet this one answers. On a ROUTING_APP reply it is the id
    // of the message being acknowledged, which is the only thing tying an
    // acknowledgement back to what it acknowledges.
    uint32_t request_id;
    bool     has_request_id;
} mt_data_t;

// Minimal protobuf reader for the Data submessage: field 1 varint portnum,
// field 2 bytes payload, field 6 fixed32 request_id. Everything else is skipped
// by wire type.
//
// This doubles as the "is the key right" test. AES-CTR never fails loudly, so a
// wrong channel key yields random bytes; strict parsing is what rejects them.
bool mt_data_parse(const uint8_t* buf, size_t len, mt_data_t* out);

// Encode a Data submessage. `request_id` of zero is omitted, which is what
// proto3 does with a default value and what every other client expects.
// Returns the encoded length, or 0 if it will not fit.
size_t mt_data_encode(uint32_t portnum, const uint8_t* payload, size_t payload_len, uint32_t request_id, uint8_t* out,
                      size_t out_max);

// The Routing submessage that acknowledges a message: variant 3, error_reason,
// set to NONE. Two bytes, but they have to be exactly these two -- an empty
// payload is a different thing and other clients ignore it.
#define MT_ROUTING_ACK_LEN 2
size_t mt_routing_ack_encode(uint8_t* out, size_t out_max);

// True when a decoded Routing payload says the message succeeded rather than
// carrying a routing error.
bool mt_routing_is_ack(const uint8_t* payload, size_t len);

// Assemble a complete frame: the 16-byte header followed by already-encrypted
// payload. Returns the frame length, or 0 if it will not fit.
//
// `hop_limit` is written to both the limit and the start field: a receiver
// computes hops taken as start minus limit, so leaving start at zero makes the
// hop count unreadable for everyone downstream.
//
// `want_ack` asks the recipient to reply with a routing acknowledgement. Only
// meaningful on a packet addressed to one node: nobody acknowledges a broadcast.
uint8_t mt_packet_build(uint32_t to, uint32_t from, uint32_t id, uint8_t hop_limit, uint8_t channel_hash,
                        bool want_ack, const uint8_t* payload, uint8_t payload_len, uint8_t* out, size_t out_max);

// The User submessage carried on NODEINFO_APP: how a node announces its names
// and key. Note the node *number* is not in here -- it comes from the packet
// header's `from` field, and `id` is only a rendering of it.
#define MT_USER_NAME_MAX  39
#define MT_USER_SHORT_MAX 7
#define MT_PUBLIC_KEY_LEN 32

// HardwareModel.PRIVATE_HW: the value upstream reserves for hardware that is not
// one of its own board types. More honest than leaving the field at UNSET, which
// clients render as an empty hardware column.
#define MT_HW_PRIVATE 255

typedef struct {
    char    id[16];  // "!aabbccdd"
    char    long_name[MT_USER_NAME_MAX + 1];
    char    short_name[MT_USER_SHORT_MAX + 1];
    uint8_t hw_model;
    uint8_t role;
    uint8_t public_key[MT_PUBLIC_KEY_LEN];  // Curve25519, for PKI direct messages
    bool    has_public_key;
} mt_user_t;

// Fields we do not use are skipped by wire type rather than rejected, so a
// newer sender with extra fields still parses.
bool   mt_user_parse(const uint8_t* buf, size_t len, mt_user_t* out);
size_t mt_user_encode(const mt_user_t* user, uint8_t* out, size_t out_max);

// Check the NodeInfo encoding against bytes computed from the protobuf spec
// rather than by this code.
//
// Worth a boot-time check because the failure is invisible from here: a missing
// or malformed field encodes cleanly, transmits cleanly, and simply never
// appears on the other node -- which is how the public key went out empty for
// several releases while everything looked fine locally.
bool mt_wire_selftest(void);

const char* mt_portnum_name(uint32_t portnum);
