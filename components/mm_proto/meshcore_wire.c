// SPDX-License-Identifier: MIT

#include "meshcore_wire.h"
#include <string.h>

#define HDR_ROUTE_SHIFT 0
#define HDR_ROUTE_MASK  0x03
#define HDR_TYPE_SHIFT  2
#define HDR_TYPE_MASK   0x0F
#define HDR_VER_SHIFT   6
#define HDR_VER_MASK    0x03

bool mc_packet_parse(const uint8_t* data, uint8_t size, mc_packet_t* out) {
    if (data == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (size < 1) return false;

    uint8_t pos = 0;
    uint8_t header = data[pos++];

    out->route   = (header >> HDR_ROUTE_SHIFT) & HDR_ROUTE_MASK;
    out->type    = (header >> HDR_TYPE_SHIFT) & HDR_TYPE_MASK;
    out->version = (header >> HDR_VER_SHIFT) & HDR_VER_MASK;

    // The two transport codes are on the wire only for the scoped routes.
    if (out->route == MC_ROUTE_TRANSPORT_FLOOD || out->route == MC_ROUTE_TRANSPORT_DIRECT) {
        if (size - pos < (int)sizeof(out->transport_codes)) return false;
        memcpy(out->transport_codes, &data[pos], sizeof(out->transport_codes));
        pos += sizeof(out->transport_codes);
    }

    if (size - pos < 1) return false;
    // One byte carries both dimensions of the path: the upper two bits are the
    // per-hop size (00->1B, 01->2B, 10->3B), the lower six the hop count.
    uint8_t path_ctrl   = data[pos++];
    out->hop_count      = path_ctrl & 0x3F;
    out->bytes_per_hop  = ((path_ctrl >> 6) & 0x03) + 1;
    out->path_length    = out->hop_count * out->bytes_per_hop;

    if (out->bytes_per_hop > 3) goto fail;
    if (out->path_length > MC_MAX_PATH_SIZE) goto fail;
    if (size - pos < out->path_length) goto fail;
    memcpy(out->path, &data[pos], out->path_length);
    pos += out->path_length;

    out->payload_length = size - pos;
    if (out->payload_length > MC_MAX_PAYLOAD_SIZE) goto fail;
    memcpy(out->payload, &data[pos], out->payload_length);
    return true;

fail:
    memset(out, 0, sizeof(*out));
    return false;
}

bool mc_grp_txt_parse(const uint8_t* payload, uint8_t size, mc_grp_txt_t* out) {
    if (payload == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (size < 1 + MC_CIPHER_MAC_SIZE) return false;

    uint8_t pos = 0;
    out->channel_hash = payload[pos++];
    memcpy(out->mac, &payload[pos], MC_CIPHER_MAC_SIZE);
    pos += MC_CIPHER_MAC_SIZE;

    out->cipher_length = size - pos;
    if (out->cipher_length > sizeof(out->cipher)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    memcpy(out->cipher, &payload[pos], out->cipher_length);
    return true;
}

uint8_t mc_grp_txt_build(uint8_t channel_hash, const uint8_t mac[MC_CIPHER_MAC_SIZE], const uint8_t* cipher,
                         uint8_t cipher_len, uint8_t* out, size_t out_max) {
    if (out == NULL || mac == NULL || cipher == NULL) return 0;

    size_t total = 1 + MC_CIPHER_MAC_SIZE + cipher_len;
    if (total > out_max || total > MC_MAX_PAYLOAD_SIZE) return 0;

    out[0] = channel_hash;
    memcpy(&out[1], mac, MC_CIPHER_MAC_SIZE);
    memcpy(&out[1 + MC_CIPHER_MAC_SIZE], cipher, cipher_len);
    return (uint8_t)total;
}

uint8_t mc_packet_build(mc_payload_type_t type, mc_route_type_t route, const uint8_t* payload, uint8_t payload_len,
                        uint8_t* out, size_t out_max) {
    if (out == NULL || payload == NULL) return 0;
    if (payload_len > MC_MAX_PAYLOAD_SIZE) return 0;

    // header + path control byte + payload. A packet we originate has no path,
    // so no transport codes and no hop bytes.
    size_t total = 1 + 1 + payload_len;
    if (total > out_max) return 0;

    out[0] = (uint8_t)(((route & HDR_ROUTE_MASK) << HDR_ROUTE_SHIFT) | ((type & HDR_TYPE_MASK) << HDR_TYPE_SHIFT));
    // Upper two bits are bytes-per-hop minus one, lower six the hop count: one
    // byte per hop, zero hops so far.
    out[1] = 0x00;
    memcpy(&out[2], payload, payload_len);
    return (uint8_t)total;
}

const char* mc_payload_type_name(mc_payload_type_t type) {
    switch (type) {
        case MC_PAYLOAD_REQ: return "REQ";
        case MC_PAYLOAD_RESPONSE: return "RESP";
        case MC_PAYLOAD_TXT_MSG: return "DM";
        case MC_PAYLOAD_ACK: return "ACK";
        case MC_PAYLOAD_ADVERT: return "ADVERT";
        case MC_PAYLOAD_GRP_TXT: return "GRP_TXT";
        case MC_PAYLOAD_GRP_DATA: return "GRP_DATA";
        case MC_PAYLOAD_ANON_REQ: return "ANON_REQ";
        case MC_PAYLOAD_PATH: return "PATH";
        case MC_PAYLOAD_TRACE: return "TRACE";
        case MC_PAYLOAD_MULTIPART: return "MULTIPART";
        case MC_PAYLOAD_RAW_CUSTOM: return "RAW";
        default: return "?";
    }
}
