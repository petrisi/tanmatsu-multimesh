// SPDX-License-Identifier: MIT
//
// MeshCore stack adapter: channel receive and transmit, plus adverts.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "base64.h"
#include "dedup.h"
#include "ed25519.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mesh_net.h"
#include "mesh_verify.h"
#include "meshcore_crypto.h"
#include "meshcore_wire.h"
#include "radio_cfg.h"

static const char TAG[] = "net_mc";

static uint32_t adverts = 0;
static uint32_t grp_txt = 0;
static dedup_t  seen;

// Frames we transmitted, still listening for a repeater to echo them back.
// A channel broadcast has no destination to filter on, so without this our own
// flood would come back and be shown as though a stranger had sent it.
#define MAX_PENDING_TX 4

typedef struct {
    bool     active;
    uint8_t  key[DEDUP_KEY_LEN];
    uint32_t seq;  // the message this frame belongs to
} pending_tx_t;

static pending_tx_t pending[MAX_PENDING_TX];

static void track_transmission(const uint8_t* payload, uint8_t payload_len, uint32_t seq) {
    int slot = 0;
    for (int i = 0; i < MAX_PENDING_TX; i++) {
        if (!pending[i].active) {
            slot = i;
            break;
        }
        // Full: replace the oldest, which is the lowest sequence number.
        if (pending[i].seq < pending[slot].seq) slot = i;
    }

    memset(&pending[slot], 0, sizeof(pending[slot]));
    pending[slot].active = true;
    pending[slot].seq    = seq;
    size_t n = payload_len < DEDUP_KEY_LEN ? payload_len : DEDUP_KEY_LEN;
    memcpy(pending[slot].key, payload, n);

    // Also record it as seen, so the echo is suppressed as a duplicate rather
    // than decrypted and displayed a second time.
    dedup_remember(&seen, payload, payload_len);
}

// Credit a repeat to the message that produced it. Returns true if this frame
// was one of ours.
static bool credit_repeat(mesh_state_t* mesh, const uint8_t* payload, uint8_t payload_len) {
    uint8_t probe[DEDUP_KEY_LEN] = {0};
    size_t  n                    = payload_len < DEDUP_KEY_LEN ? payload_len : DEDUP_KEY_LEN;
    memcpy(probe, payload, n);

    for (int i = 0; i < MAX_PENDING_TX; i++) {
        if (!pending[i].active || memcmp(pending[i].key, probe, DEDUP_KEY_LEN) != 0) continue;

        for (int m = 0; m < mesh->count; m++) {
            message_t* msg = (message_t*)model_message_at(mesh, m);
            if (msg && msg->used && msg->seq == pending[i].seq) {
                if (msg->repeats < 255) msg->repeats++;
                return true;
            }
        }
        // The message aged out of the ring; stop tracking it.
        pending[i].active = false;
        return true;
    }
    return false;
}

// --- advert signature verification ---------------------------------------
//
// See mesh_verify.h for why this is not done inline.

#define VERIFY_QUEUE_DEPTH 4

typedef struct {
    uint8_t pub_key[MC_PUB_KEY_SIZE];
    uint8_t signature[MC_SIGNATURE_SIZE];
    uint8_t signed_len;
    uint8_t signed_bytes[MC_MAX_PAYLOAD_SIZE];
} verify_job_t;

static QueueHandle_t verify_jobs;
static QueueHandle_t verify_results;

// Queue an advert for checking. False when it was dropped -- a full queue means
// adverts are arriving faster than we can verify them, and receive must not
// block for a verdict. The caller leaves the node unchecked so the next advert
// from it tries again.
static bool queue_verification(const uint8_t* payload, uint8_t payload_len, const mc_advert_t* advert) {
    if (verify_jobs == NULL) return false;

    verify_job_t job;
    job.signed_len = mc_advert_signed_region(payload, payload_len, job.signed_bytes, sizeof(job.signed_bytes));
    if (job.signed_len == 0) return false;

    memcpy(job.pub_key, advert->pub_key, sizeof(job.pub_key));
    memcpy(job.signature, advert->signature, sizeof(job.signature));

    if (xQueueSend(verify_jobs, &job, 0) != pdTRUE) {
        ESP_LOGD(TAG, "verification backlog, advert unchecked");
        return false;
    }
    return true;
}

bool mc_verify_run_one(uint32_t wait_ms) {
    if (verify_jobs == NULL || verify_results == NULL) return false;

    verify_job_t job;
    if (xQueueReceive(verify_jobs, &job, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return false;

    mc_verify_result_t result;
    memcpy(result.pub_key, job.pub_key, sizeof(result.pub_key));
    result.valid = ed25519_verify(job.signature, job.signed_bytes, job.signed_len, job.pub_key);

    if (!result.valid) {
        ESP_LOGW(TAG, "advert signature failed for %02x%02x%02x%02x", job.pub_key[0], job.pub_key[1], job.pub_key[2],
                 job.pub_key[3]);
    }

    xQueueSend(verify_results, &result, portMAX_DELAY);
    return true;
}

bool mc_verify_take_result(mc_verify_result_t* out) {
    if (verify_results == NULL || out == NULL) return false;
    return xQueueReceive(verify_results, out, 0) == pdTRUE;
}

static bool meshcore_init(void) {
    dedup_reset(&seen);
    memset(pending, 0, sizeof(pending));

    if (verify_jobs == NULL) verify_jobs = xQueueCreate(VERIFY_QUEUE_DEPTH, sizeof(verify_job_t));
    if (verify_results == NULL) verify_results = xQueueCreate(VERIFY_QUEUE_DEPTH, sizeof(mc_verify_result_t));
    if (verify_jobs == NULL || verify_results == NULL) {
        ESP_LOGE(TAG, "verification queues could not be allocated");
        return false;
    }

    return mc_crypto_init();
}

static void meshcore_get_config(lora_protocol_config_params_t* out) {
    // Read from the shared `system` NVS namespace so we follow whatever region
    // preset the user configured with the MeshCore app, rather than hardcoding.
    bool from_nvs = false;
    radio_cfg_load(out, &from_nvs);
}

// MeshCore has three ways to arrive at a channel key, and which one applies is
// decided by what the user typed:
//
//   an explicit base64 PSK   a private channel, 16 or 32 bytes
//   a name starting with '#' a hashtag channel -- the key is SHA256 of the
//                            name, so anyone who knows the name can join. That
//                            is the point: they are topic rooms, not secrets.
//   neither                  the well-known public channel
//
// The PSK is base64, not hex: that is the format MeshCore clients exchange.
static void meshcore_prepare_channel(channel_t* channel) {
    channel->ready   = false;
    channel->key_len = 0;

    if (channel->secret[0] != '\0') {
        uint8_t raw[32];
        int     len = base64_decode(channel->secret, raw, sizeof(raw));
        if (len != 16 && len != 32) return;  // MeshCore accepts only these two
        memcpy(channel->key, raw, (size_t)len);
        channel->key_len = (uint8_t)len;
    } else if (channel->name[0] == '#') {
        if (!mc_derive_hashtag_key(channel->name, channel->key)) return;
        channel->key_len = MC_CIPHER_KEY_SIZE;
    } else {
        memcpy(channel->key, MC_PUBLIC_CHANNEL_KEY, MC_CIPHER_KEY_SIZE);
        channel->key_len = MC_CIPHER_KEY_SIZE;
    }

    channel->hash  = mc_channel_hash(channel->key, channel->key_len);
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
        // A repeat of something we sent is not noise: it is the only delivery
        // confirmation this network offers for a channel broadcast.
        credit_repeat(mesh, packet.payload, packet.payload_length);
        detail(mesh);
        return false;
    }

    // An advert is how a MeshCore node introduces itself: public key, role and
    // usually a name. It is the only way we learn nodes on this network, since
    // channel messages carry no identity beyond the name inside the text.
    if (packet.type == MC_PAYLOAD_ADVERT) {
        adverts++;

        mc_advert_t advert;
        if (mc_advert_parse(packet.payload, packet.payload_length, &advert)) {
            node_t* node = model_node_touch_mc(mesh, advert.pub_key);
            if (node) {
                node->role      = (uint8_t)advert.role;
                node->rssi_dbm  = -(int)pkt->stats.rssi_pkt_raw / 2;
                node->snr_db_x4 = pkt->stats.snr_pkt_raw;
                node->hops      = packet.hop_count;
                memcpy(node->public_key, advert.pub_key, NODE_KEY_LEN);
                node->has_public_key = true;
                if (advert.has_name) {
                    snprintf(node->long_name, sizeof(node->long_name), "%s", advert.name);
                    node->named = true;
                }

                // Check the signature once per node. Re-checking every advert
                // would keep a slow crypto task permanently busy for no gain:
                // the key is the identity, so a key that has signed correctly
                // once cannot later turn out to be someone else. A previous
                // failure is retried, since it may have been a corrupt frame.
                if (node->verified != NODE_VERIFY_VALID && node->verified != NODE_VERIFY_PENDING) {
                    if (queue_verification(packet.payload, packet.payload_length, &advert)) {
                        node->verified = NODE_VERIFY_PENDING;
                    }
                }

                ESP_LOGI(TAG, "advert %s (%s)", advert.has_name ? advert.name : "unnamed",
                         mc_role_name(advert.role));
            }
        }
        detail(mesh);
        return false;
    }

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
        if (!mc_grp_decrypt(&grp, ch->key, ch->key_len, &decoded)) continue;

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
        // Display and ordering use our own clock; the sender's is kept for the
        // detail view. A node with an unsynced clock would otherwise scatter
        // messages across the log and invent day separators.
        msg->sender_timestamp = decoded.timestamp;
        msg->rssi_dbm         = -(int)pkt->stats.rssi_pkt_raw / 2;
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

static uint8_t meshcore_encode(mesh_state_t* mesh, uint8_t channel, const identity_t* identity, const char* text,
                               uint32_t msg_seq, uint8_t* out, size_t out_max) {
    if (channel >= mesh->channel_count) return 0;
    const channel_t* ch = &mesh->channels[channel];
    if (!ch->ready) return 0;

    // MeshCore has no identity field on the wire: the sender's name is carried
    // inside the message text, by convention, as "Name: message".
    char prefixed[TEXT_MAX];
    snprintf(prefixed, sizeof(prefixed), "%s: %s", identity->name, text);

    uint8_t plain[MC_MAX_PAYLOAD_SIZE];
    size_t  padded = mc_grp_frame_plaintext((uint32_t)time(NULL), prefixed, plain, sizeof(plain));
    if (padded == 0) return 0;

    uint8_t cipher[MC_MAX_PAYLOAD_SIZE];
    uint8_t mac[32];
    if (!mc_grp_encrypt(ch->key, ch->key_len, plain, padded, cipher, mac)) return 0;

    uint8_t payload[MC_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = mc_grp_txt_build(ch->hash, mac, cipher, (uint8_t)padded, payload, sizeof(payload));
    if (payload_len == 0) return 0;

    uint8_t len = mc_packet_build(MC_PAYLOAD_GRP_TXT, MC_ROUTE_FLOOD, payload, payload_len, out, out_max);
    if (len == 0) return 0;

    track_transmission(payload, payload_len, msg_seq);
    return len;
}

// An advert is the whole of MeshCore's identity system: it binds a name and a
// role to a public key, and the signature is what makes that binding worth
// anything. It is not scoped to a channel -- everyone on the frequency can read
// it, whatever keys they hold -- so `channel` is unused here.
static uint8_t meshcore_encode_advert(mesh_state_t* mesh, uint8_t channel, const identity_t* identity, uint8_t* out,
                                      size_t out_max) {
    (void)mesh;
    (void)channel;
    if (!identity->has_keypair) return 0;

    mc_advert_t advert = {0};
    memcpy(advert.pub_key, identity->public_key, MC_PUB_KEY_SIZE);
    advert.timestamp = (uint32_t)time(NULL);
    advert.role      = MC_ROLE_CHAT_NODE;
    if (identity->name[0]) {
        snprintf(advert.name, sizeof(advert.name), "%s", identity->name);
        advert.has_name = true;
    }

    uint8_t payload[MC_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = mc_advert_build(&advert, payload, sizeof(payload));
    if (payload_len == 0) return 0;

    // Sign exactly the bytes a receiver will reconstruct: taken back off the
    // serialised payload rather than assembled a second time, so the two can
    // never disagree.
    uint8_t region[MC_MAX_PAYLOAD_SIZE];
    uint8_t region_len = mc_advert_signed_region(payload, payload_len, region, sizeof(region));
    if (region_len == 0) return 0;

    uint8_t signature[MC_SIGNATURE_SIZE];
    if (!ed25519_sign(signature, region, region_len, identity->public_key, identity->private_key)) {
        ESP_LOGE(TAG, "advert signing failed");
        return 0;
    }
    memcpy(&payload[MC_ADVERT_SIGNATURE_OFFSET], signature, sizeof(signature));

    uint8_t len = mc_packet_build(MC_PAYLOAD_ADVERT, MC_ROUTE_FLOOD, payload, payload_len, out, out_max);
    if (len == 0) return 0;

    // Repeaters will flood this back at us. Remember it so our own advert is not
    // decoded as a stranger's and entered in the node list as a second self.
    track_transmission(payload, payload_len, UINT32_MAX);
    return len;
}

static const char* meshcore_local_sender(const identity_t* identity) {
    // The name we actually put in the message text, so the echo matches what
    // everyone else will see.
    return identity->name;
}

const mesh_net_t mesh_net_meshcore = {
    .name            = "MeshCore",
    .tag             = "MC",
    .init            = meshcore_init,
    .get_config      = meshcore_get_config,
    .prepare_channel = meshcore_prepare_channel,
    .local_sender    = meshcore_local_sender,
    .handle          = meshcore_handle,
    .encode          = meshcore_encode,
    .encode_advert   = meshcore_encode_advert,
};
