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

#define EFL_CHANNEL_NAME "EdgeFastLow"
#define EFL_FREQUENCY    869431250  // Hz; EU_868 band start 869.4 MHz + bandwidth/2
#define EFL_SF           8
#define EFL_BANDWIDTH    62  // nominal label for 62.5 kHz
#define EFL_CODING_RATE  8   // 4/8
#define EFL_SYNC_WORD    0x2B
#define EFL_PREAMBLE     16
#define EFL_POWER        22  // module maximum; EU_868 permits 27

// "AQ==" base64-decoded. A one-byte PSK is a key index, not a key.
static const uint8_t EFL_PSK[1] = {0x01};

static mt_key_t key              = {0};
static uint8_t  our_channel_hash = 0;
static uint32_t text_msgs        = 0;
static uint32_t other_ports      = 0;

static bool meshtastic_init(void) {
    if (!mt_key_expand(EFL_PSK, sizeof(EFL_PSK), &key)) {
        ESP_LOGE(TAG, "PSK expansion failed");
        return false;
    }
    our_channel_hash = mt_channel_hash(EFL_CHANNEL_NAME, &key);
    ESP_LOGI(TAG, "channel '%s' hash 0x%02X, key %u bytes", EFL_CHANNEL_NAME, our_channel_hash,
             (unsigned)key.length);
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

static void meshtastic_handle(const lora_protocol_lora_packet_t* pkt, ui_stats_t* stats) {
    stats->packets_total++;

    mt_packet_t packet;
    if (!mt_packet_parse(pkt->data, pkt->length, &packet)) {
        stats->packets_bad++;
        return;
    }

    // The header carries the channel hash in the clear, so foreign channels cost
    // us nothing. A zero hash means an unencrypted or PKI packet: not ours.
    if (packet.channel_hash != our_channel_hash) {
        stats->not_our_channel++;
        return;
    }

    if (!mt_decrypt(&key, packet.from, packet.id, packet.payload, packet.payload_length)) {
        stats->packets_bad++;
        return;
    }

    // CTR cannot report a bad key, so the protobuf parse is the real gate.
    mt_data_t data;
    if (!mt_data_parse(packet.payload, packet.payload_length, &data)) {
        stats->not_our_channel++;
        return;
    }

    if (data.portnum != MT_PORTNUM_TEXT_MESSAGE) {
        other_ports++;
        snprintf(stats->detail, sizeof(stats->detail), "text:%lu other:%lu", (unsigned long)text_msgs,
                 (unsigned long)other_ports);
        return;
    }

    char text[MT_MAX_PAYLOAD_SIZE + 16];
    size_t len = data.payload_length;
    if (len > MT_MAX_PAYLOAD_SIZE) len = MT_MAX_PAYLOAD_SIZE;

    // Meshtastic text is raw UTF-8 with no sender name; show the node id so
    // messages are attributable without a nodeinfo database.
    char body[MT_MAX_PAYLOAD_SIZE + 1];
    memcpy(body, data.payload, len);
    body[len] = '\0';
    snprintf(text, sizeof(text), "!%08lx: %s", (unsigned long)packet.from, body);

    text_msgs++;
    stats->messages++;

    int rssi_dbm = -(int)pkt->stats.rssi_pkt_raw / 2;
    // Meshtastic stamps no timestamp in the Data submessage; 0 renders as "--:--:--".
    ui_add_message(mesh_net_meshtastic.tag, 0, text, rssi_dbm, pkt->stats.snr_pkt_raw, mt_hops_taken(&packet));
    snprintf(stats->detail, sizeof(stats->detail), "text:%lu other:%lu", (unsigned long)text_msgs,
             (unsigned long)other_ports);
    ESP_LOGI(TAG, "msg [%d dBm] %s", rssi_dbm, text);
}

const mesh_net_t mesh_net_meshtastic = {
    .name       = "Meshtastic EFL",
    .tag        = "MT",
    .init       = meshtastic_init,
    .get_config = meshtastic_get_config,
    .handle     = meshtastic_handle,
};
