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

// Assemble a GRP_TXT payload: channel_hash[1] | mac[2] | ciphertext[...].
// Returns the payload length, or 0 if it will not fit.
uint8_t mc_grp_txt_build(uint8_t channel_hash, const uint8_t mac[MC_CIPHER_MAC_SIZE], const uint8_t* cipher,
                         uint8_t cipher_len, uint8_t* out, size_t out_max);

// Assemble a complete frame for transmission.
//
// `path_ctrl` is the packed hop control byte -- count in bits 0-5, bytes per hop
// minus one in bits 6-7 -- and `path` the hops themselves. Pass 0 and NULL to
// originate a flooded packet, which starts with no path and collects one as
// repeaters append themselves. A directed packet carries the route it should
// take, and each repeater strips itself off the front as it forwards.
//
// Returns the frame length, or 0 if it will not fit.
uint8_t mc_packet_build(mc_payload_type_t type, mc_route_type_t route, uint8_t path_ctrl, const uint8_t* path,
                        const uint8_t* payload, uint8_t payload_len, uint8_t* out, size_t out_max);

// Hops and bytes-per-hop packed into the one control byte the wire uses.
#define MC_PATH_CTRL(count, hash_size) ((uint8_t)((((hash_size) - 1) << 6) | ((count) & 0x3F)))
#define MC_PATH_COUNT(ctrl)            ((uint8_t)((ctrl) & 0x3F))
#define MC_PATH_HASH_SIZE(ctrl)        ((uint8_t)(((ctrl) >> 6) + 1))
#define MC_PATH_BYTES(ctrl)            ((uint8_t)(MC_PATH_COUNT(ctrl) * MC_PATH_HASH_SIZE(ctrl)))

// True when a control byte describes a path that can actually exist: hash size 4
// is reserved, and the hops have to fit MC_MAX_PATH_SIZE.
bool mc_path_ctrl_valid(uint8_t ctrl);

// How wide a hop is in the packets we originate.
//
// The originator decides: repeaters append however many bytes the control byte
// declares, and the recipient hands the whole thing back, so this alone fixes
// the width of every route anyone learns to us.
//
// Two, not the protocol default of one. A hop is a prefix of a repeater's public
// key, and one byte of it is 256 possibilities -- on a mesh of any size two
// repeaters sharing a leading byte is ordinary rather than unlucky. A directed
// packet is forwarded by whichever node matches the leading hop, so an ambiguous
// hop means the wrong repeater forwards it, or several do, and a route that
// looks perfectly good silently fails to deliver. Two bytes costs one byte per
// hop and makes a collision remote.
#define MC_PATH_HASH_SIZE_OURS 2

// The control byte for a packet we are originating: our width, no hops yet.
#define MC_PATH_CTRL_NEW MC_PATH_CTRL(0, MC_PATH_HASH_SIZE_OURS)

// ADVERT: how a MeshCore node announces itself. There is no separate node id on
// this network -- the Ed25519 public key *is* the identity.
#define MC_PUB_KEY_SIZE   32
#define MC_SIGNATURE_SIZE 64
#define MC_NAME_MAX       32

// Everything after the signature -- flags, optional position, name -- is one
// blob that upstream calls the app data, and it clamps that blob to this before
// verifying. Anything longer is simply not covered by the signature the far end
// checks, so building one is a silent way to be rejected by every peer.
#define MC_ADVERT_APP_DATA_MAX 32

typedef enum {
    MC_ROLE_UNKNOWN     = 0,
    MC_ROLE_CHAT_NODE   = 1,
    MC_ROLE_REPEATER    = 2,
    MC_ROLE_ROOM_SERVER = 3,
    MC_ROLE_SENSOR      = 4,
} mc_role_t;

typedef struct {
    uint8_t   pub_key[MC_PUB_KEY_SIZE];
    uint32_t  timestamp;  // the sender's clock, which may be wrong
    uint8_t   signature[MC_SIGNATURE_SIZE];
    mc_role_t role;
    bool      has_name;
    char      name[MC_NAME_MAX + 1];
    bool      has_position;
    int32_t   latitude;   // in units of 1e-6 degrees
    int32_t   longitude;
} mc_advert_t;

bool mc_advert_parse(const uint8_t* payload, uint8_t size, mc_advert_t* out);

// The bytes an advert signature covers, taken from the raw payload: everything
// except the 64-byte signature itself.
//
// Deliberately works on raw bytes rather than the parsed struct. Re-serialising
// from mc_advert_t would silently drop any optional field this parser does not
// understand, and the signature would then fail against a sender that includes
// one -- a bug that would look like bad crypto rather than a lossy round trip.
//
// Returns the length written, or 0 if the payload is too short or will not fit.
uint8_t mc_advert_signed_region(const uint8_t* payload, uint8_t size, uint8_t* out, size_t out_max);

// Serialise an advert with its signature field left zeroed. Sign the region
// reported by mc_advert_signed_region() over the result, then write the 64-byte
// signature at MC_ADVERT_SIGNATURE_OFFSET.
#define MC_ADVERT_SIGNATURE_OFFSET (MC_PUB_KEY_SIZE + 4)
uint8_t mc_advert_build(const mc_advert_t* advert, uint8_t* out, size_t out_max);

// --- direct messages -----------------------------------------------------
//
// A datagram payload: dest_hash[1] | src_hash[1] | mac[2] | ciphertext[...].
//
// Both "hashes" are simply the first byte of the respective public key. One byte
// is not close to unique, and it is not meant to be: it narrows the candidate
// contacts, and the MAC is what actually decides. So a receiver must be prepared
// to try every contact whose key starts with that byte.
#define MC_DEST_HASH_SIZE 1

typedef struct {
    uint8_t dest_hash;
    uint8_t src_hash;
    uint8_t mac[MC_CIPHER_MAC_SIZE];
    uint8_t cipher_length;
    uint8_t cipher[MC_MAX_PAYLOAD_SIZE];
} mc_datagram_t;

bool mc_datagram_parse(const uint8_t* payload, uint8_t size, mc_datagram_t* out);

uint8_t mc_datagram_build(uint8_t dest_hash, uint8_t src_hash, const uint8_t mac[MC_CIPHER_MAC_SIZE],
                          const uint8_t* cipher, uint8_t cipher_len, uint8_t* out, size_t out_max);

// An acknowledgement carries the four-byte hash in the clear. It proves the
// recipient decrypted the message, not merely that a frame arrived, because the
// hash is computed over the plaintext.
//
// Six bytes go on the wire although only four are the hash. The fifth is the
// sender's extended attempt byte and the sixth is random, and both exist for the
// same reason: repeaters and receivers suppress packets they have seen before,
// keyed on content. Two acknowledgements for the same message would otherwise be
// byte-identical, and the second would be dropped by the mesh rather than
// delivered -- so the padding is what makes a repeated acknowledgement
// deliverable at all.
#define MC_ACK_HASH_SIZE    4
#define MC_ACK_PAYLOAD_SIZE 6

// `nonce` is the random sixth byte. Supplied by the caller rather than drawn
// here, because this file stays free of ESP-IDF so its codecs remain testable
// on the host.
uint8_t mc_ack_build(const uint8_t hash[MC_ACK_HASH_SIZE], uint8_t extended_attempt, uint8_t nonce, uint8_t* out,
                     size_t out_max);
bool    mc_ack_parse(const uint8_t* payload, uint8_t size, uint8_t out_hash[MC_ACK_HASH_SIZE]);

// --- returned paths ------------------------------------------------------
//
// How MeshCore discovers routes. Nothing is learned passively: a node that
// receives a flooded message hands the route back explicitly, in a PATH packet
// addressed and encrypted exactly like a direct message. The path it returns is
// the one the flood accumulated on the way in -- repeaters append themselves --
// and it needs no reversing, because a path is ordered sender to recipient and
// repeaters carry traffic both ways.
//
// An acknowledgement usually rides along in the `extra` field, so a single
// packet both confirms the message and teaches the route.
//
// Decrypted plaintext:  path_ctrl[1] | path[...] | extra_type[1] | extra[...]

typedef struct {
    uint8_t path_ctrl;
    uint8_t path[MC_MAX_PATH_SIZE];
    uint8_t path_bytes;
    uint8_t extra_type;
    uint8_t extra[MC_MAX_PAYLOAD_SIZE];
    uint8_t extra_len;
} mc_path_msg_t;

bool mc_path_parse(const uint8_t* plain, size_t len, mc_path_msg_t* out);

// Frame the plaintext of a PATH packet. `extra` may be NULL, in which case a
// dummy type and four random bytes are written instead -- upstream does this so
// two otherwise identical returns hash differently and neither is suppressed in
// transit. `nonce` supplies those bytes, since this file draws no randomness.
//
// Returns the length before padding, or 0 if it will not fit.
size_t mc_path_frame(uint8_t path_ctrl, const uint8_t* path, uint8_t extra_type, const uint8_t* extra,
                     size_t extra_len, uint32_t nonce, uint8_t* out, size_t out_max);

const char* mc_role_name(mc_role_t role);

// Human-readable payload type, for the traffic counters on screen.
const char* mc_payload_type_name(mc_payload_type_t type);
