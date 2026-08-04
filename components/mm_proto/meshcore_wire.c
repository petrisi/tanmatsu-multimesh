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

// Advert flag byte: the low nibble is the role, the high bits say which optional
// sections follow.
#define ADVERT_FLAG_POSITION 0x10
#define ADVERT_FLAG_FEAT1    0x20
#define ADVERT_FLAG_FEAT2    0x40
#define ADVERT_FLAG_NAME     0x80

#define ADVERT_FIXED_LEN (MC_PUB_KEY_SIZE + 4 + MC_SIGNATURE_SIZE)

bool mc_advert_parse(const uint8_t* payload, uint8_t size, mc_advert_t* out) {
    if (payload == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (size < ADVERT_FIXED_LEN) return false;

    // Ignore app data past the cap, exactly as upstream does before it verifies.
    // Reading further would mean parsing bytes the signature does not cover.
    if (size - ADVERT_FIXED_LEN > MC_ADVERT_APP_DATA_MAX) size = ADVERT_FIXED_LEN + MC_ADVERT_APP_DATA_MAX;

    uint8_t pos = 0;
    memcpy(out->pub_key, &payload[pos], MC_PUB_KEY_SIZE);
    pos += MC_PUB_KEY_SIZE;
    memcpy(&out->timestamp, &payload[pos], 4);
    pos += 4;
    memcpy(out->signature, &payload[pos], MC_SIGNATURE_SIZE);
    pos += MC_SIGNATURE_SIZE;

    // An advert with no app data is still a valid advert: we know the node
    // exists and its key, just not its name.
    if (pos >= size) return true;

    uint8_t flags = payload[pos++];
    out->role     = (mc_role_t)(flags & 0x0F);

    if (flags & ADVERT_FLAG_POSITION) {
        if (size - pos < 8) return false;
        memcpy(&out->latitude, &payload[pos], 4);
        pos += 4;
        memcpy(&out->longitude, &payload[pos], 4);
        pos += 4;
        out->has_position = true;
    }
    // The two feature words are skipped rather than stored: nothing here uses
    // them yet, but their length has to be honoured to find the name.
    if (flags & ADVERT_FLAG_FEAT1) {
        if (size - pos < 2) return false;
        pos += 2;
    }
    if (flags & ADVERT_FLAG_FEAT2) {
        if (size - pos < 2) return false;
        pos += 2;
    }

    if (flags & ADVERT_FLAG_NAME) {
        uint8_t name_len = (uint8_t)(size - pos);
        if (name_len > MC_NAME_MAX) name_len = MC_NAME_MAX;
        memcpy(out->name, &payload[pos], name_len);
        out->name[name_len] = '\0';
        out->has_name       = name_len > 0;
    }
    return true;
}

uint8_t mc_advert_signed_region(const uint8_t* payload, uint8_t size, uint8_t* out, size_t out_max) {
    if (payload == NULL || out == NULL) return 0;
    if (size < ADVERT_FIXED_LEN) return 0;

    // Public key and timestamp, then everything after the signature -- capped
    // where upstream caps it, or we would verify over bytes the sender's peers
    // discard and disagree with all of them.
    size_t head = MC_PUB_KEY_SIZE + 4;
    size_t tail = (size_t)size - ADVERT_FIXED_LEN;
    if (tail > MC_ADVERT_APP_DATA_MAX) tail = MC_ADVERT_APP_DATA_MAX;
    if (head + tail > out_max) return 0;

    memcpy(out, payload, head);
    if (tail) memcpy(out + head, payload + ADVERT_FIXED_LEN, tail);
    return (uint8_t)(head + tail);
}

uint8_t mc_advert_build(const mc_advert_t* advert, uint8_t* out, size_t out_max) {
    if (advert == NULL || out == NULL) return 0;

    // Everything after the signature is "app data", and receivers clamp it to
    // MC_ADVERT_APP_DATA_MAX before checking the signature. Exceed that and the
    // signature covers bytes the far end has thrown away, so it always fails.
    size_t app_data = 1;  // the flag byte
    if (advert->has_position) app_data += 8;
    size_t name_len = advert->has_name ? strlen(advert->name) : 0;
    if (app_data + name_len > MC_ADVERT_APP_DATA_MAX) return 0;
    app_data += name_len;

    size_t need = ADVERT_FIXED_LEN + app_data;
    if (need > out_max || need > MC_MAX_PAYLOAD_SIZE) return 0;

    uint8_t pos = 0;
    memcpy(&out[pos], advert->pub_key, MC_PUB_KEY_SIZE);
    pos += MC_PUB_KEY_SIZE;
    memcpy(&out[pos], &advert->timestamp, 4);
    pos += 4;

    // Left zeroed; the caller signs and patches it in.
    memset(&out[pos], 0, MC_SIGNATURE_SIZE);
    pos += MC_SIGNATURE_SIZE;

    uint8_t flags = (uint8_t)(advert->role & 0x0F);
    if (advert->has_position) flags |= ADVERT_FLAG_POSITION;
    if (name_len) flags |= ADVERT_FLAG_NAME;
    out[pos++] = flags;

    if (advert->has_position) {
        memcpy(&out[pos], &advert->latitude, 4);
        pos += 4;
        memcpy(&out[pos], &advert->longitude, 4);
        pos += 4;
    }
    if (name_len) {
        memcpy(&out[pos], advert->name, name_len);
        pos += (uint8_t)name_len;
    }
    return pos;
}

const char* mc_role_name(mc_role_t role) {
    switch (role) {
        case MC_ROLE_CHAT_NODE: return "chat";
        case MC_ROLE_REPEATER: return "repeater";
        case MC_ROLE_ROOM_SERVER: return "room";
        case MC_ROLE_SENSOR: return "sensor";
        default: return "unknown";
    }
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
