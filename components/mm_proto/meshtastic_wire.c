// SPDX-License-Identifier: MIT

#include "meshtastic_wire.h"
#include <string.h>

bool mt_packet_parse(const uint8_t* data, uint8_t size, mt_packet_t* out) {
    if (data == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (size < MT_HEADER_SIZE) return false;

    // All multi-byte header fields are little endian on the wire.
    out->to   = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    out->from = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    out->id   = (uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);

    out->flags        = data[12];
    out->channel_hash = data[13];
    out->next_hop     = data[14];
    out->relay_node   = data[15];

    out->hop_limit = out->flags & MT_FLAGS_HOP_LIMIT_MASK;
    out->hop_start = (out->flags & MT_FLAGS_HOP_START_MASK) >> MT_FLAGS_HOP_START_SHIFT;

    out->payload_length = size - MT_HEADER_SIZE;
    if (out->payload_length > MT_MAX_PAYLOAD_SIZE) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    memcpy(out->payload, &data[MT_HEADER_SIZE], out->payload_length);
    return true;
}

uint8_t mt_hops_taken(const mt_packet_t* pkt) {
    if (pkt == NULL || pkt->hop_start == 0) return 0;
    if (pkt->hop_limit > pkt->hop_start) return 0;  // malformed; do not underflow
    return pkt->hop_start - pkt->hop_limit;
}

// Read a base-128 varint. Returns false on truncation or an implausibly long
// encoding (>10 bytes cannot fit in 64 bits).
static bool read_varint(const uint8_t* buf, size_t len, size_t* pos, uint64_t* out) {
    uint64_t value = 0;
    int      shift = 0;
    while (*pos < len) {
        uint8_t byte = buf[(*pos)++];
        if (shift < 64) value |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            *out = value;
            return true;
        }
        if (shift > 70) return false;
    }
    return false;
}

bool mt_data_parse(const uint8_t* buf, size_t len, mt_data_t* out) {
    if (buf == NULL || out == NULL || len == 0) return false;
    memset(out, 0, sizeof(*out));

    bool   saw_portnum = false;
    size_t pos         = 0;

    while (pos < len) {
        uint64_t key;
        if (!read_varint(buf, len, &pos, &key)) return false;

        uint32_t field     = (uint32_t)(key >> 3);
        uint8_t  wire_type = (uint8_t)(key & 0x07);

        // Field 0 is never valid; hitting it means we are reading noise, which
        // is the common case for a packet from a channel we do not hold.
        if (field == 0) return false;

        switch (wire_type) {
            case 0: {  // varint
                uint64_t value;
                if (!read_varint(buf, len, &pos, &value)) return false;
                if (field == 1) {
                    out->portnum = (uint32_t)value;
                    saw_portnum  = true;
                }
                break;
            }
            case 1:  // 64-bit
                if (len - pos < 8) return false;
                pos += 8;
                break;
            case 2: {  // length-delimited
                uint64_t length;
                if (!read_varint(buf, len, &pos, &length)) return false;
                if (length > len - pos) return false;
                if (field == 2) {
                    if (length > sizeof(out->payload)) return false;
                    memcpy(out->payload, &buf[pos], (size_t)length);
                    out->payload_length = (size_t)length;
                }
                pos += (size_t)length;
                break;
            }
            case 5:  // 32-bit
                if (len - pos < 4) return false;
                pos += 4;
                break;
            default:
                // Group wire types (3, 4) are not used by Meshtastic; seeing one
                // means this is not a valid Data message.
                return false;
        }
    }

    // CTR is a stream cipher, so the plaintext is exactly as long as the
    // ciphertext -- no padding to tolerate here. A buffer that consumed cleanly
    // to the end and carried a portnum is a Data message; anything else is a
    // packet from a channel whose key we do not hold.
    return saw_portnum;
}

const char* mt_portnum_name(uint32_t portnum) {
    switch (portnum) {
        case MT_PORTNUM_TEXT_MESSAGE: return "text";
        case MT_PORTNUM_POSITION: return "pos";
        case MT_PORTNUM_NODEINFO: return "nodeinfo";
        case MT_PORTNUM_TELEMETRY: return "telem";
        default: return "other";
    }
}
