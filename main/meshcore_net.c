// SPDX-License-Identifier: MIT
//
// MeshCore stack adapter: public-channel receive.

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "mesh_net.h"
#include "meshcore_crypto.h"
#include "meshcore_wire.h"
#include "radio_cfg.h"

static const char TAG[] = "net_mc";

static uint8_t  our_channel_hash = 0;
static uint32_t adverts          = 0;
static uint32_t grp_txt          = 0;

static bool meshcore_init(void) {
    if (!mc_crypto_init()) return false;
    our_channel_hash = mc_channel_hash(MC_PUBLIC_CHANNEL_KEY);
    ESP_LOGI(TAG, "public channel hash 0x%02X", our_channel_hash);
    return true;
}

static void meshcore_get_config(lora_protocol_config_params_t* out) {
    // Read from the shared `system` NVS namespace so we follow whatever region
    // preset the user configured with the MeshCore app, rather than hardcoding.
    bool from_nvs = false;
    radio_cfg_load(out, &from_nvs);
}

static void meshcore_handle(const lora_protocol_lora_packet_t* pkt, ui_stats_t* stats) {
    stats->packets_total++;

    mc_packet_t packet;
    if (!mc_packet_parse(pkt->data, pkt->length, &packet)) {
        stats->packets_bad++;
        return;
    }

    if (packet.type == MC_PAYLOAD_ADVERT) adverts++;
    if (packet.type != MC_PAYLOAD_GRP_TXT) {
        snprintf(stats->detail, sizeof(stats->detail), "advert:%lu grp:%lu", (unsigned long)adverts,
                 (unsigned long)grp_txt);
        return;
    }
    grp_txt++;

    mc_grp_txt_t grp;
    if (!mc_grp_txt_parse(packet.payload, packet.payload_length, &grp)) {
        stats->packets_bad++;
        return;
    }

    // Cheap reject before spending an HMAC on a channel we hold no key for.
    if (grp.channel_hash != our_channel_hash) {
        stats->not_our_channel++;
        return;
    }

    mc_grp_msg_t msg;
    if (!mc_grp_decrypt(&grp, MC_PUBLIC_CHANNEL_KEY, &msg)) {
        // Hash collision, or a private channel that shares the leading byte.
        stats->not_our_channel++;
        return;
    }

    stats->messages++;
    int rssi_dbm = -(int)pkt->stats.rssi_pkt_raw / 2;
    ui_add_message(mesh_net_meshcore.tag, msg.timestamp, msg.text, rssi_dbm, pkt->stats.snr_pkt_raw,
                   packet.hop_count);
    snprintf(stats->detail, sizeof(stats->detail), "advert:%lu grp:%lu", (unsigned long)adverts,
             (unsigned long)grp_txt);
    ESP_LOGI(TAG, "msg [%d dBm %u hops] %s", rssi_dbm, packet.hop_count, msg.text);
}

const mesh_net_t mesh_net_meshcore = {
    .name       = "MeshCore",
    .tag        = "MC",
    .init       = meshcore_init,
    .get_config = meshcore_get_config,
    .handle     = meshcore_handle,
};
