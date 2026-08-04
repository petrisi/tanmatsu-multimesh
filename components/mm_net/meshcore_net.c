// SPDX-License-Identifier: MIT
//
// MeshCore stack adapter: channel receive and transmit, plus adverts.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "base64.h"
#include "crypto_jobs.h"
#include "dedup.h"
#include "ed25519.h"
#include "esp_log.h"
#include "mesh_net.h"
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

// --- acknowledgements ----------------------------------------------------
//
// Reading a direct message obliges us to acknowledge it, but receive runs on the
// event loop and transmitting blocks. So the frame is built here and parked for
// the loop to pick up and queue, the same way verification results travel.

#define MAX_PENDING_ACKS 4

typedef struct {
    bool    active;
    uint8_t frame[MC_MAX_PAYLOAD_SIZE + 8];
    uint8_t length;
} pending_ack_t;

static pending_ack_t acks[MAX_PENDING_ACKS];

static void queue_ack(const uint8_t hash[MC_ACK_HASH_SIZE]) {
    uint8_t payload[MC_ACK_HASH_SIZE];
    uint8_t payload_len = mc_ack_build(hash, payload, sizeof(payload));
    if (payload_len == 0) return;

    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (acks[i].active) continue;
        // Flooded rather than routed back along the path: we do not track return
        // paths yet, and an unacknowledged message reads as a delivery failure.
        acks[i].length = mc_packet_build(MC_PAYLOAD_ACK, MC_ROUTE_FLOOD, payload, payload_len, acks[i].frame,
                                         sizeof(acks[i].frame));
        acks[i].active = acks[i].length > 0;
        return;
    }
    ESP_LOGW(TAG, "no slot for an acknowledgement; the sender will retry");
}

bool mc_take_pending_ack(uint8_t* out, size_t out_max, uint8_t* out_len) {
    if (out == NULL || out_len == NULL) return false;
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!acks[i].active || acks[i].length > out_max) continue;
        memcpy(out, acks[i].frame, acks[i].length);
        *out_len       = acks[i].length;
        acks[i].active = false;
        return true;
    }
    return false;
}

static bool meshcore_init(void) {
    dedup_reset(&seen);
    memset(pending, 0, sizeof(pending));
    memset(acks, 0, sizeof(acks));
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

// Ask for the shared secret with a node, once. Cheap to call repeatedly: it
// does nothing when the secret is already known or already queued.
static void want_secret(node_t* node, const identity_t* identity) {
    if (node == NULL || node->has_secret || node->secret_pending) return;
    if (!identity->has_keypair || !node->has_public_key) return;

    if (crypto_queue_mc_secret(node->key, identity->private_key)) node->secret_pending = true;
}

// A direct message addressed to us. The one-byte sender hash narrows the field;
// the MAC is what actually identifies the sender, so every contact sharing that
// byte gets a try.
static bool handle_datagram(const mc_packet_t* packet, mesh_state_t* mesh, const identity_t* identity,
                            const lora_protocol_lora_packet_t* pkt) {
    if (!identity->has_keypair) return false;

    mc_datagram_t datagram;
    if (!mc_datagram_parse(packet->payload, packet->payload_length, &datagram)) {
        mesh->stats.packets_bad++;
        return false;
    }
    if (datagram.dest_hash != identity->public_key[0]) return false;  // not for us

    for (int i = 0; i < MAX_NODES; i++) {
        node_t* node = &mesh->nodes[i];
        if (!node->used || node->key[0] != datagram.src_hash) continue;

        if (!node->has_secret) {
            // We know of this node but have never agreed a key with it. Start
            // that now; this message is lost, but the sender will retry and the
            // next one will read.
            want_secret(node, identity);
            continue;
        }

        mc_dm_msg_t decoded;
        if (!mc_dm_decrypt(node->shared_secret, datagram.mac, datagram.cipher, datagram.cipher_length, &decoded)) {
            continue;
        }
        // Only plain text is displayable; the CLI and signed forms are other
        // features of upstream that we do not implement.
        if (decoded.text_type != 0) return false;

        node->last_heard = (uint32_t)time(NULL);

        char who[SENDER_MAX];
        model_node_label(node, MESH_MC, who, sizeof(who));

        message_t* msg = model_push(mesh, (uint8_t)mesh->input_channel, who, node->named, decoded.text, false);
        msg->dm        = true;
        snprintf(msg->peer, sizeof(msg->peer), "%s", who);
        msg->sender_timestamp = decoded.timestamp;
        msg->rssi_dbm         = -(int)pkt->stats.rssi_pkt_raw / 2;
        msg->snr_db_x4        = pkt->stats.snr_pkt_raw;
        msg->hops             = packet->hop_count;
        msg->path_len = packet->path_length > sizeof(msg->path) ? (uint8_t)sizeof(msg->path) : packet->path_length;
        memcpy(msg->path, packet->path, msg->path_len);

        // Acknowledge it. The hash is over the plaintext and the *sender's* key,
        // so returning it proves we read the message rather than merely heard
        // the frame -- which is the whole point of it.
        uint8_t plain[MC_MAX_PAYLOAD_SIZE];
        size_t  plain_len = mc_dm_frame_plaintext(decoded.timestamp, decoded.attempt, decoded.text, plain,
                                                  sizeof(plain), NULL);
        if (plain_len > 0) {
            uint8_t hash[MC_ACK_HASH_SIZE];
            if (mc_dm_ack_hash(hash, plain, decoded.signed_len, node->key)) queue_ack(hash);
        }

        mesh->stats.messages++;
        ESP_LOGI(TAG, "dm from %s: %s", who, decoded.text);
        return true;
    }

    // Addressed to us but from nobody we hold a key for.
    mesh->stats.not_our_channel++;
    return false;
}

// An acknowledgement for one of our direct messages. Matched on the hash the
// sender computed when it built the message.
static bool handle_ack(const mc_packet_t* packet, mesh_state_t* mesh) {
    uint8_t hash[MC_ACK_HASH_SIZE];
    if (!mc_ack_parse(packet->payload, packet->payload_length, hash)) return false;

    uint32_t value = (uint32_t)hash[0] | ((uint32_t)hash[1] << 8) | ((uint32_t)hash[2] << 16) |
                     ((uint32_t)hash[3] << 24);

    for (int i = 0; i < mesh->count; i++) {
        message_t* msg = (message_t*)model_message_at(mesh, i);
        if (msg == NULL || !msg->used || !msg->outgoing || !msg->dm) continue;
        if (msg->expected_ack != value || msg->acked) continue;

        msg->acked = true;
        msg->tx    = TX_CONFIRMED;
        ESP_LOGI(TAG, "dm to %s acknowledged", msg->peer);
        return true;
    }
    return false;
}

static bool meshcore_handle(const lora_protocol_lora_packet_t* pkt, mesh_state_t* mesh, const identity_t* identity) {
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
                    uint8_t region[MC_MAX_PAYLOAD_SIZE];
                    uint8_t region_len = mc_advert_signed_region(packet.payload, packet.payload_length, region,
                                                                 sizeof(region));
                    if (region_len > 0 &&
                        crypto_queue_mc_verify(advert.pub_key, advert.signature, region, region_len)) {
                        node->verified = NODE_VERIFY_PENDING;
                    }
                }

                // An advert is where a MeshCore key comes from, so it is also
                // the moment a conversation with this node becomes possible.
                want_secret(node, identity);

                ESP_LOGI(TAG, "advert %s (%s)", advert.has_name ? advert.name : "unnamed",
                         mc_role_name(advert.role));
            }
        }
        detail(mesh);
        return false;
    }

    if (packet.type == MC_PAYLOAD_TXT_MSG) {
        bool shown = handle_datagram(&packet, mesh, identity, pkt);
        detail(mesh);
        return shown;
    }

    if (packet.type == MC_PAYLOAD_ACK) {
        bool matched = handle_ack(&packet, mesh);
        detail(mesh);
        return matched;
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

static uint8_t meshcore_encode_dm(mesh_state_t* mesh, const identity_t* identity, const node_t* peer,
                                  const char* text, message_t* msg, uint8_t* out, size_t out_max) {
    (void)mesh;
    if (!identity->has_keypair || peer == NULL || !peer->has_secret) return 0;

    uint8_t plain[MC_MAX_PAYLOAD_SIZE];
    uint8_t unpadded = 0;
    // Attempt zero: resends are not implemented, and the counter exists only to
    // make a retry hash differently from the original.
    size_t padded = mc_dm_frame_plaintext((uint32_t)time(NULL), 0, text, plain, sizeof(plain), &unpadded);
    if (padded == 0) return 0;

    // What the recipient will send back if it reads this. Computed over our own
    // key, because that is the key it will hash on its side.
    uint8_t hash[MC_ACK_HASH_SIZE];
    if (!mc_dm_ack_hash(hash, plain, unpadded, identity->public_key)) return 0;

    uint8_t cipher[MC_MAX_PAYLOAD_SIZE];
    uint8_t mac[32];
    if (!mc_dm_encrypt(peer->shared_secret, plain, padded, cipher, mac)) return 0;

    uint8_t payload[MC_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = mc_datagram_build(peer->key[0], identity->public_key[0], mac, cipher, (uint8_t)padded,
                                            payload, sizeof(payload));
    if (payload_len == 0) return 0;

    uint8_t len = mc_packet_build(MC_PAYLOAD_TXT_MSG, MC_ROUTE_FLOOD, payload, payload_len, out, out_max);
    if (len == 0) return 0;

    if (msg) {
        msg->expected_ack = (uint32_t)hash[0] | ((uint32_t)hash[1] << 8) | ((uint32_t)hash[2] << 16) |
                            ((uint32_t)hash[3] << 24);
        track_transmission(payload, payload_len, msg->seq);
    }
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
    .encode_dm       = meshcore_encode_dm,
    .encode_advert   = meshcore_encode_advert,
};
