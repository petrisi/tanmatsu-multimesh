// SPDX-License-Identifier: MIT
//
// Meshtastic stack adapter, configured for the Finnish EdgeFastLow (EFL)
// network: a custom modem preset used in areas where 868 MHz interference makes
// the standard LongFast profile unreliable.
//
//   channel   EdgeFastLow, PSK "AQ==" (= {0x01}, the default key)
//   modem     use_preset false, bandwidth 62 (=62.5 kHz), SF8, CR 4/8
//   slot      channel_num 1 in EU_868 (869.4-869.65 MHz)
//             -> 869.4 + 62.5/2 kHz = 869.43125 MHz
//
// EFL and LongFast are mutually unreadable: different frequency and different
// modem settings.

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "mesh_net.h"
#include "meshtastic_crypto.h"
#include "meshtastic_wire.h"

static const char TAG[] = "net_mt";

#define EFL_FREQUENCY   869431250  // Hz; EU_868 band start 869.4 MHz + bandwidth/2
#define EFL_SF          8
#define EFL_BANDWIDTH   62  // nominal label for 62.5 kHz
#define EFL_CODING_RATE 8   // 4/8
#define EFL_SYNC_WORD   0x2B
#define EFL_PREAMBLE    16
#define EFL_POWER       22  // module maximum; EU_868 permits 27

static uint32_t text_msgs   = 0;
static uint32_t other_ports = 0;

static bool meshtastic_init(void) {
    return true;
}

static void meshtastic_get_config(lora_protocol_config_params_t* out) {
    memset(out, 0, sizeof(*out));
    out->frequency                  = EFL_FREQUENCY;
    out->spreading_factor           = EFL_SF;
    out->bandwidth                  = EFL_BANDWIDTH;
    out->coding_rate                = EFL_CODING_RATE;
    out->power                      = EFL_POWER;
    out->preamble_length            = EFL_PREAMBLE;
    out->sync_word                  = EFL_SYNC_WORD;
    out->rx_boost                   = true;
    out->crc_enabled                = true;
    out->invert_iq                  = false;
    out->use_dcdc                   = true;
    out->use_automatic_correction   = true;
    out->low_data_rate_optimization = false;
    out->ramp_time                  = 200;
}

// Decode base64 into `out`. Meshtastic PSKs are carried as base64 in every
// client, so that is what the editor accepts.
static int base64_decode(const char* in, uint8_t* out, int out_max) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t           acc = 0;
    int                bits = 0, len = 0;

    for (const char* p = in; *p; p++) {
        if (*p == '=' || *p == '\n' || *p == '\r') continue;
        const char* pos = strchr(alphabet, *p);
        if (!pos) return -1;
        acc   = (acc << 6) | (uint32_t)(pos - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (len >= out_max) return -1;
            out[len++] = (uint8_t)(acc >> bits);
        }
    }
    return len;
}

static void meshtastic_prepare_channel(channel_t* channel) {
    channel->ready   = false;
    channel->key_len = 0;

    uint8_t raw[MT_MAX_KEY_SIZE];
    int     raw_len = base64_decode(channel->secret, raw, sizeof(raw));
    if (raw_len <= 0) return;

    mt_key_t expanded;
    if (!mt_key_expand(raw, (size_t)raw_len, &expanded)) return;

    memcpy(channel->key, expanded.bytes, expanded.length);
    channel->key_len = (uint8_t)expanded.length;
    // The hash mixes the channel NAME as well as the key, so renaming a channel
    // changes which traffic it matches. That is upstream behaviour, not a bug.
    channel->hash  = mt_channel_hash(channel->name, &expanded);
    channel->ready = true;
}

static void detail(mesh_state_t* mesh) {
    snprintf(mesh->stats.detail, sizeof(mesh->stats.detail), "text:%lu other:%lu", (unsigned long)text_msgs,
             (unsigned long)other_ports);
}

static bool meshtastic_handle(const lora_protocol_lora_packet_t* pkt, mesh_state_t* mesh) {
    mesh->stats.packets_total++;

    mt_packet_t packet;
    if (!mt_packet_parse(pkt->data, pkt->length, &packet)) {
        mesh->stats.packets_bad++;
        return false;
    }

    for (int i = 0; i < mesh->channel_count; i++) {
        const channel_t* ch = &mesh->channels[i];
        if (!ch->ready || packet.channel_hash != ch->hash) continue;

        mt_key_t key = {.length = ch->key_len};
        memcpy(key.bytes, ch->key, ch->key_len);

        // Decrypting mutates the payload, so work on a copy: a hash collision
        // between two configured channels must not destroy the ciphertext for
        // the next candidate.
        mt_packet_t attempt = packet;
        if (!mt_decrypt(&key, attempt.from, attempt.id, attempt.payload, attempt.payload_length)) continue;

        // CTR cannot report a wrong key, so the protobuf parse is the real gate.
        mt_data_t data;
        if (!mt_data_parse(attempt.payload, attempt.payload_length, &data)) continue;

        if (data.portnum != MT_PORTNUM_TEXT_MESSAGE) {
            other_ports++;
            detail(mesh);
            return false;
        }

        char body[TEXT_MAX];
        size_t len = data.payload_length < sizeof(body) - 1 ? data.payload_length : sizeof(body) - 1;
        memcpy(body, data.payload, len);
        body[len] = '\0';

        // Until a NodeInfo arrives the sender is the low 16 bits of the node
        // number; colour, not a prefix, marks it as an id rather than a name.
        char who[SENDER_MAX];
        snprintf(who, sizeof(who), "%04lx", (unsigned long)(attempt.from & 0xFFFF));

        message_t* msg = model_push(mesh, (uint8_t)i, who, false, body, false);
        // Meshtastic stamps no time in the payload, so this is our receive clock.
        msg->rssi_dbm  = -(int)pkt->stats.rssi_pkt_raw / 2;
        msg->snr_db_x4 = pkt->stats.snr_pkt_raw;
        msg->hops      = mt_hops_taken(&attempt);
        msg->hop_start = attempt.hop_start;
        msg->hop_limit = attempt.hop_limit;
        if (attempt.relay_node) snprintf(msg->relayed_by, sizeof(msg->relayed_by), "%02x", attempt.relay_node);

        text_msgs++;
        mesh->stats.messages++;
        detail(mesh);
        ESP_LOGI(TAG, "msg [%d dBm] %s: %s", msg->rssi_dbm, who, body);
        return true;
    }

    mesh->stats.not_our_channel++;
    detail(mesh);
    return false;
}

const mesh_net_t mesh_net_meshtastic = {
    .name            = "Meshtastic",
    .tag             = "MT",
    .init            = meshtastic_init,
    .get_config      = meshtastic_get_config,
    .prepare_channel = meshtastic_prepare_channel,
    .handle          = meshtastic_handle,
};
