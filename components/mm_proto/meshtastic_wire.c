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

static size_t write_varint(uint8_t* out, size_t out_max, uint64_t value) {
    size_t n = 0;
    do {
        if (n >= out_max) return 0;
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value) byte |= 0x80;
        out[n++] = byte;
    } while (value);
    return n;
}

size_t mt_data_encode(uint32_t portnum, const uint8_t* payload, size_t payload_len, uint8_t* out, size_t out_max) {
    if (out == NULL || payload == NULL) return 0;

    size_t pos = 0;

    // Field 1, wire type 0: portnum.
    size_t n = write_varint(&out[pos], out_max - pos, (1 << 3) | 0);
    if (n == 0) return 0;
    pos += n;
    n = write_varint(&out[pos], out_max - pos, portnum);
    if (n == 0) return 0;
    pos += n;

    // Field 2, wire type 2: the message bytes.
    n = write_varint(&out[pos], out_max - pos, (2 << 3) | 2);
    if (n == 0) return 0;
    pos += n;
    n = write_varint(&out[pos], out_max - pos, payload_len);
    if (n == 0) return 0;
    pos += n;

    if (pos + payload_len > out_max) return 0;
    memcpy(&out[pos], payload, payload_len);
    return pos + payload_len;
}

uint8_t mt_packet_build(uint32_t to, uint32_t from, uint32_t id, uint8_t hop_limit, uint8_t channel_hash,
                        const uint8_t* payload, uint8_t payload_len, uint8_t* out, size_t out_max) {
    if (out == NULL || payload == NULL) return 0;

    size_t total = MT_HEADER_SIZE + payload_len;
    if (total > out_max || payload_len > MT_MAX_PAYLOAD_SIZE) return 0;

    // All multi-byte header fields are little endian on the wire.
    out[0] = (uint8_t)to;
    out[1] = (uint8_t)(to >> 8);
    out[2] = (uint8_t)(to >> 16);
    out[3] = (uint8_t)(to >> 24);
    out[4] = (uint8_t)from;
    out[5] = (uint8_t)(from >> 8);
    out[6] = (uint8_t)(from >> 16);
    out[7] = (uint8_t)(from >> 24);
    out[8]  = (uint8_t)id;
    out[9]  = (uint8_t)(id >> 8);
    out[10] = (uint8_t)(id >> 16);
    out[11] = (uint8_t)(id >> 24);

    out[12] = (uint8_t)((hop_limit & MT_FLAGS_HOP_LIMIT_MASK) |
                        ((hop_limit << MT_FLAGS_HOP_START_SHIFT) & MT_FLAGS_HOP_START_MASK));
    out[13] = channel_hash;
    out[14] = 0;  // next_hop: unset, this is a broadcast
    out[15] = 0;  // relay_node: filled in by whoever relays us

    memcpy(&out[MT_HEADER_SIZE], payload, payload_len);
    return (uint8_t)total;
}

// Copy a length-delimited protobuf string into a fixed buffer, always
// terminated. Over-long values are truncated rather than rejected: a name we
// cannot fully display is still better than no node at all.
static void copy_string(char* dst, size_t dst_size, const uint8_t* src, size_t len) {
    if (len > dst_size - 1) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

bool mt_user_parse(const uint8_t* buf, size_t len, mt_user_t* out) {
    if (buf == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

    size_t pos       = 0;
    bool   saw_field = false;

    while (pos < len) {
        uint64_t key;
        if (!read_varint(buf, len, &pos, &key)) return false;

        uint32_t field     = (uint32_t)(key >> 3);
        uint8_t  wire_type = (uint8_t)(key & 0x07);
        if (field == 0) return false;

        switch (wire_type) {
            case 0: {  // varint
                uint64_t value;
                if (!read_varint(buf, len, &pos, &value)) return false;
                if (field == 5) out->hw_model = (uint8_t)value;
                if (field == 7) out->role = (uint8_t)value;
                saw_field = true;
                break;
            }
            case 1:
                if (len - pos < 8) return false;
                pos += 8;
                break;
            case 2: {  // length-delimited
                uint64_t length;
                if (!read_varint(buf, len, &pos, &length)) return false;
                if (length > len - pos) return false;

                switch (field) {
                    case 1: copy_string(out->id, sizeof(out->id), &buf[pos], (size_t)length); break;
                    case 2: copy_string(out->long_name, sizeof(out->long_name), &buf[pos], (size_t)length); break;
                    case 3: copy_string(out->short_name, sizeof(out->short_name), &buf[pos], (size_t)length); break;
                    case 8:
                        if (length == MT_PUBLIC_KEY_LEN) {
                            memcpy(out->public_key, &buf[pos], MT_PUBLIC_KEY_LEN);
                            out->has_public_key = true;
                        }
                        break;
                    default: break;
                }
                pos       += (size_t)length;
                saw_field  = true;
                break;
            }
            case 5:
                if (len - pos < 4) return false;
                pos += 4;
                break;
            default:
                return false;
        }
    }
    return saw_field;
}

size_t mt_user_encode(const mt_user_t* user, uint8_t* out, size_t out_max) {
    if (user == NULL || out == NULL) return 0;

    size_t pos = 0;

    // Only the fields that identify us. Others are optional and adding them
    // would cost airtime on a channel that is already duty-cycle limited.
    const struct {
        uint32_t    field;
        const char* value;
    } strings[] = {{1, user->id}, {2, user->long_name}, {3, user->short_name}};

    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        size_t len = strlen(strings[i].value);
        if (len == 0) continue;

        size_t n = write_varint(&out[pos], out_max - pos, (strings[i].field << 3) | 2);
        if (n == 0) return 0;
        pos += n;
        n    = write_varint(&out[pos], out_max - pos, len);
        if (n == 0) return 0;
        pos += n;
        if (pos + len > out_max) return 0;
        memcpy(&out[pos], strings[i].value, len);
        pos += len;
    }

    if (user->hw_model) {
        size_t n = write_varint(&out[pos], out_max - pos, (5 << 3) | 0);
        if (n == 0) return 0;
        pos += n;
        n    = write_varint(&out[pos], out_max - pos, user->hw_model);
        if (n == 0) return 0;
        pos += n;
    }

    return pos;
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
