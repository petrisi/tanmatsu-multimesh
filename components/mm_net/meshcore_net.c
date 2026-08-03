// SPDX-License-Identifier: MIT
//
// MeshCore stack adapter: channel receive.

#include <stdio.h>
#include <string.h>
#include "dedup.h"
#include "esp_log.h"
#include "mesh_net.h"
#include "meshcore_crypto.h"
#include "meshcore_wire.h"
#include "radio_cfg.h"

static const char TAG[] = "net_mc";

static uint32_t adverts = 0;
static uint32_t grp_txt = 0;
static dedup_t  seen;

static bool meshcore_init(void) {
    dedup_reset(&seen);
    return mc_crypto_init();
}

static void meshcore_get_config(lora_protocol_config_params_t* out) {
    // Read from the shared `system` NVS namespace so we follow whatever region
    // preset the user configured with the MeshCore app, rather than hardcoding.
    bool from_nvs = false;
    radio_cfg_load(out, &from_nvs);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void meshcore_prepare_channel(channel_t* channel) {
    channel->ready   = false;
    channel->key_len = 0;

    // An empty secret means the well-known public channel rather than an error:
    // it is what a new install should join by default.
    if (channel->secret[0] == '\0') {
        memcpy(channel->key, MC_PUBLIC_CHANNEL_KEY, MC_CIPHER_KEY_SIZE);
        channel->key_len = MC_CIPHER_KEY_SIZE;
    } else {
        if (strlen(channel->secret) != MC_CIPHER_KEY_SIZE * 2) return;
        for (int i = 0; i < MC_CIPHER_KEY_SIZE; i++) {
            int hi = hex_nibble(channel->secret[i * 2]);
            int lo = hex_nibble(channel->secret[i * 2 + 1]);
            if (hi < 0 || lo < 0) return;
            channel->key[i] = (uint8_t)((hi << 4) | lo);
        }
        channel->key_len = MC_CIPHER_KEY_SIZE;
    }

    channel->hash  = mc_channel_hash(channel->key);
    channel->ready = true;
}

static void detail(mesh_state_t* mesh) {
    snprintf(mesh->stats.detail, sizeof(mesh->stats.detail), "advert:%lu grp:%lu dup:%lu", (unsigned long)adverts,
             (unsigned long)grp_txt, (unsigned long)mesh->stats.duplicates);
}

static bool meshcore_handle(const lora_protocol_lora_packet_t* pkt, mesh_state_t* mesh) {
    mesh->stats.packets_total++;

    mc_packet_t packet;
    if (!mc_packet_parse(pkt->data, pkt->length, &packet)) {
        mesh->stats.packets_bad++;
        return false;
    }

    // MeshCore floods, and the same message arrives once directly and again via
    // every repeater. There is no packet id to key on, but the header and
    // transport codes are the only parts that differ between retransmits -- the
    // payload is byte-identical, so fingerprint that.
    if (dedup_check(&seen, packet.payload, packet.payload_length)) {
        mesh->stats.duplicates++;
        detail(mesh);
        return false;
    }

    if (packet.type == MC_PAYLOAD_ADVERT) adverts++;
    if (packet.type != MC_PAYLOAD_GRP_TXT) {
        detail(mesh);
        return false;
    }
    grp_txt++;

    mc_grp_txt_t grp;
    if (!mc_grp_txt_parse(packet.payload, packet.payload_length, &grp)) {
        mesh->stats.packets_bad++;
        detail(mesh);
        return false;
    }

    // Try every configured channel: the one-byte hash is a cheap pre-filter,
    // and the MAC is what actually proves membership.
    for (int i = 0; i < mesh->channel_count; i++) {
        const channel_t* ch = &mesh->channels[i];
        if (!ch->ready || grp.channel_hash != ch->hash) continue;

        mc_grp_msg_t decoded;
        if (!mc_grp_decrypt(&grp, ch->key, &decoded)) continue;

        // MeshCore has no identity field: senders put their name in the text as
        // "Name: message". Split it so the sender column has something to show.
        const char* body = decoded.text;
        char        who[SENDER_MAX];
        snprintf(who, sizeof(who), "?");
        const char* colon = strstr(decoded.text, ": ");
        if (colon && (size_t)(colon - decoded.text) < sizeof(who)) {
            size_t n = (size_t)(colon - decoded.text);
            memcpy(who, decoded.text, n);
            who[n] = '\0';
            body   = colon + 2;
        }

        message_t* msg = model_push(mesh, (uint8_t)i, who, true, body, false);
        msg->timestamp = decoded.timestamp;  // MeshCore carries the sender's clock
        msg->rssi_dbm  = -(int)pkt->stats.rssi_pkt_raw / 2;
        msg->snr_db_x4 = pkt->stats.snr_pkt_raw;
        msg->hops      = packet.hop_count;
        msg->path_len  = packet.path_length > sizeof(msg->path) ? (uint8_t)sizeof(msg->path) : packet.path_length;
        memcpy(msg->path, packet.path, msg->path_len);

        mesh->stats.messages++;
        detail(mesh);
        ESP_LOGI(TAG, "msg [%d dBm %u hops] %s", msg->rssi_dbm, packet.hop_count, body);
        return true;
    }

    // The hash matched nothing, or every MAC failed: a channel we hold no key for.
    mesh->stats.not_our_channel++;
    detail(mesh);
    return false;
}

const mesh_net_t mesh_net_meshcore = {
    .name            = "MeshCore",
    .tag             = "MC",
    .init            = meshcore_init,
    .get_config      = meshcore_get_config,
    .prepare_channel = meshcore_prepare_channel,
    .handle          = meshcore_handle,
};
