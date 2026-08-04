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
#include "esp_random.h"
#include "mesh_net.h"
#include "meshcore_crypto.h"
#include "meshcore_net.h"
#include "meshcore_wire.h"
#include "radio_cfg.h"
#include "session_log.h"

// A path as "0cbd>8dea", for the log. Static buffer: the log is single-threaded
// through the event loop and the result is consumed immediately.
static const char* path_text(uint8_t ctrl, const uint8_t* path) {
    static char buf[3 * MC_MAX_PATH_SIZE + 8];
    uint8_t     width = MC_PATH_HASH_SIZE(ctrl);
    uint8_t     bytes = MC_PATH_BYTES(ctrl);
    int         n     = 0;

    if (bytes == 0) return "-";
    for (int i = 0; i + width <= bytes && n < (int)sizeof(buf) - 8; i += width) {
        if (i) n += snprintf(buf + n, sizeof(buf) - n, ">");
        for (int b = 0; b < width; b++) n += snprintf(buf + n, sizeof(buf) - n, "%02x", path[i + b]);
    }
    return buf;
}

static const char TAG[] = "net_mc";

static uint32_t adverts = 0;
static uint32_t grp_txt = 0;
static dedup_t  seen;

// Direct messages need a second ring keyed on content rather than on the wire
// payload. A resend differs on the wire -- the attempt counter is inside the
// ciphertext -- so only what the message says identifies it.
static dedup_t  dm_seen;

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

static pending_ack_t* free_ack_slot(void) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!acks[i].active) return &acks[i];
    }
    ESP_LOGW(TAG, "no slot for an acknowledgement; the sender will retry");
    return NULL;
}

// A bare acknowledgement, sent when the sender already knows how to reach us.
static void queue_ack(const node_t* peer, const uint8_t hash[MC_ACK_HASH_SIZE], uint8_t extended_attempt) {
    pending_ack_t* slot = free_ack_slot();
    if (slot == NULL) return;

    uint8_t payload[MC_ACK_PAYLOAD_SIZE];
    uint8_t payload_len = mc_ack_build(hash, extended_attempt, (uint8_t)esp_random(), payload, sizeof(payload));
    if (payload_len == 0) return;

    // Answer the way the message came: along their route if we have one, else
    // flooded. An acknowledgement that floods the whole mesh to travel two hops
    // is most of the airtime a short conversation costs.
    bool direct = peer != NULL && peer->has_out_path;
    slot->length =
        mc_packet_build(MC_PAYLOAD_ACK, direct ? MC_ROUTE_DIRECT : MC_ROUTE_FLOOD,
                        direct ? peer->out_path_ctrl : MC_PATH_CTRL_NEW, direct ? peer->out_path : NULL, payload, payload_len,
                        slot->frame, sizeof(slot->frame));
    slot->active = slot->length > 0;
}

// The reply to a message that reached us by flood: the route it took, so the
// sender can stop flooding, with the acknowledgement carried inside it.
//
// This is the whole of MeshCore's path discovery. Nothing is learned passively
// -- a node that never returns a path is one every peer must flood to reach,
// forever, which on a duty-cycle limited band is a cost the whole mesh pays.
static void queue_path_return(const node_t* peer, const mc_packet_t* packet, const identity_t* identity,
                              const uint8_t hash[MC_ACK_HASH_SIZE], uint8_t extended_attempt) {
    pending_ack_t* slot = free_ack_slot();
    if (slot == NULL) return;

    uint8_t ack[MC_ACK_PAYLOAD_SIZE];
    uint8_t ack_len = mc_ack_build(hash, extended_attempt, (uint8_t)esp_random(), ack, sizeof(ack));
    if (ack_len == 0) return;

    // The path is returned exactly as it arrived. No reversing: a MeshCore path
    // is ordered sender to recipient and repeaters carry traffic both ways, so
    // the route that brought this to us is the route back to us.
    uint8_t path_ctrl = MC_PATH_CTRL(packet->hop_count, packet->bytes_per_hop);

    uint8_t plain[MC_MAX_PAYLOAD_SIZE];
    size_t  len = mc_path_frame(path_ctrl, packet->path, MC_PAYLOAD_ACK, ack, ack_len, esp_random(), plain,
                                sizeof(plain));
    if (len == 0) return;

    size_t padded = mc_pad_plaintext(plain, len, sizeof(plain));
    if (padded == 0) return;

    uint8_t cipher[MC_MAX_PAYLOAD_SIZE];
    uint8_t mac[32];
    if (!mc_dm_encrypt(peer->shared_secret, plain, padded, cipher, mac)) return;

    uint8_t payload[MC_MAX_PAYLOAD_SIZE];
    uint8_t payload_len = mc_datagram_build(peer->key[0], identity->public_key[0], mac, cipher, (uint8_t)padded,
                                            payload, sizeof(payload));
    if (payload_len == 0) return;

    // Flooded, necessarily: they have no route to us yet, which is why we are
    // sending this at all.
    slot->length = mc_packet_build(MC_PAYLOAD_PATH, MC_ROUTE_FLOOD, MC_PATH_CTRL_NEW, NULL, payload, payload_len, slot->frame,
                                   sizeof(slot->frame));
    slot->active = slot->length > 0;
}

// See mesh_net.h: built here, queued by the event loop.
static bool meshcore_take_pending_ack(uint8_t* out, size_t out_max, uint8_t* out_len) {
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
    dedup_reset(&dm_seen);
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

// Keep the route a packet took, along with how wide each hop is.
//
// A long path is truncated to whole hops rather than mid-hop: half an identifier
// is not a shorter route, it is a wrong one. The hop count is recorded
// separately, so a truncated path still reports the true distance.
static void store_path(message_t* msg, const mc_packet_t* packet) {
    msg->path_hash_size = packet->bytes_per_hop ? packet->bytes_per_hop : 1;

    uint8_t room = (uint8_t)(sizeof(msg->path) / msg->path_hash_size) * msg->path_hash_size;
    msg->path_len       = packet->path_length > room ? room : packet->path_length;
    msg->path_truncated = msg->path_len < packet->path_length;
    memcpy(msg->path, packet->path, msg->path_len);
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

        // Acknowledge before anything else, and acknowledge every copy. The
        // sender keeps resending until it hears one, so a retransmission of
        // something we have already shown still needs an answer -- staying quiet
        // because we recognise it is what makes the sender try again.
        //
        // Hashed over the bytes as they arrived rather than a re-framing of the
        // decoded fields: any difference produces an acknowledgement the sender
        // does not recognise, which looks exactly like a lost message.
        uint8_t hash[MC_ACK_HASH_SIZE];
        if (mc_dm_ack_hash(hash, decoded.plain, decoded.signed_len, node->key)) {
            // Upstream mixes in the byte after the text's terminator, which
            // carries the attempt number on a fourth or later try. Zero when the
            // frame is too short to hold one.
            uint8_t extended = decoded.signed_len + 1 < decoded.plain_len ? decoded.plain[decoded.signed_len + 1] : 0;

            // Arriving by flood means the sender does not know the way here.
            // Answering with the route it took teaches them, and costs one
            // packet rather than the several a repeated flood would.
            bool arrived_flooded = packet->route == MC_ROUTE_FLOOD || packet->route == MC_ROUTE_TRANSPORT_FLOOD;
            session_log("dm.rx from=%02x%02x attempt=%u flooded=%u hops=%u ack=%02x%02x%02x%02x", node->key[0],
                        node->key[1], (unsigned)decoded.attempt, arrived_flooded ? 1u : 0u,
                        (unsigned)packet->hop_count, hash[0], hash[1], hash[2], hash[3]);

            if (arrived_flooded) {
                queue_path_return(node, packet, identity, hash, extended);
                session_log("path.tx to=%02x%02x hops=%u path=%s", node->key[0], node->key[1],
                            (unsigned)packet->hop_count,
                            path_text(MC_PATH_CTRL(packet->hop_count, packet->bytes_per_hop), packet->path));
            } else {
                queue_ack(node, hash, extended);
                session_log("ack.tx to=%02x%02x direct=%u", node->key[0], node->key[1],
                            node->has_out_path ? 1u : 0u);
            }
        }

        // Every retry is a fresh ciphertext -- the attempt counter lives inside
        // the encrypted plaintext -- so payload dedup cannot catch it. Identify
        // the message by what it says instead, and show it once.
        uint8_t identity_key[MC_DM_IDENTITY_LEN];
        mc_dm_identity(identity_key, node->key, decoded.timestamp, decoded.text);
        if (dedup_check(&dm_seen, identity_key, sizeof(identity_key))) {
            mesh->stats.duplicates++;
            ESP_LOGD(TAG, "dm resend, acknowledged again");
            return false;
        }

        char who[SENDER_MAX];
        model_node_label(node, MESH_MC, who, sizeof(who));

        message_t* msg = model_push(mesh, (uint8_t)mesh->input_channel, who, node->named, decoded.text, false);
        msg->dm        = true;
        snprintf(msg->peer, sizeof(msg->peer), "%s", who);
        msg->sender_timestamp = decoded.timestamp;
        msg->rssi_dbm         = -(int)pkt->stats.rssi_pkt_raw / 2;
        msg->snr_db_x4        = pkt->stats.snr_pkt_raw;
        msg->hops = packet->hop_count;
        store_path(msg, packet);

        mesh->stats.messages++;
        ESP_LOGI(TAG, "dm from %s: %s", who, decoded.text);
        return true;
    }

    // Addressed to us but from nobody we hold a key for.
    mesh->stats.not_our_channel++;
    return false;
}

// Mark the outgoing direct message a four-byte acknowledgement hash belongs to.
static bool credit_ack(mesh_state_t* mesh, const uint8_t hash[MC_ACK_HASH_SIZE]) {
    uint32_t value = (uint32_t)hash[0] | ((uint32_t)hash[1] << 8) | ((uint32_t)hash[2] << 16) |
                     ((uint32_t)hash[3] << 24);

    for (int i = 0; i < mesh->count; i++) {
        message_t* msg = (message_t*)model_message_at(mesh, i);
        if (msg == NULL || !msg->used || !msg->outgoing || !msg->dm || msg->acked) continue;

        // Any attempt's hash will do. A reply to the first attempt that arrives
        // after the second went out still proves delivery.
        int matched = -1;
        for (int slot = 0; slot < msg->expected_ack_count; slot++) {
            if (msg->expected_ack[slot] == value) matched = slot;
        }
        if (matched < 0) continue;

        msg->acked = true;
        msg->tx    = TX_CONFIRMED;
        session_log("ack.match hash=%08lx seq=%lu for_attempt=%d latest=%u direct=%u", (unsigned long)value,
                    (unsigned long)msg->seq, matched, (unsigned)msg->dm_attempt, msg->dm_direct ? 1u : 0u);
        ESP_LOGI(TAG, "dm to %s acknowledged", msg->peer);
        return true;
    }

    // Nothing matched. Log what we were waiting for, because an acknowledgement
    // that arrives and is not credited looks identical on screen to one that
    // never arrived, and the two have completely different causes.
    if (session_log_active()) {
        char waiting[160];
        int  n = 0;
        for (int i = 0; i < mesh->count && n < (int)sizeof(waiting) - 12; i++) {
            const message_t* msg = model_message_at(mesh, i);
            if (msg == NULL || !msg->used || !msg->outgoing || !msg->dm || msg->acked) continue;
            for (int slot = 0; slot < msg->expected_ack_count && n < (int)sizeof(waiting) - 12; slot++) {
                n += snprintf(waiting + n, sizeof(waiting) - n, "%s%08lx", n ? "," : "",
                              (unsigned long)msg->expected_ack[slot]);
            }
        }
        session_log("ack.nomatch hash=%08lx waiting=%s", (unsigned long)value, n ? waiting : "-");
    }
    return false;
}

// A route handed back to us, with an acknowledgement usually inside it.
//
// This is what a real MeshCore client sends in reply to a flooded message --
// not a bare acknowledgement -- so without this our own direct messages would
// never be confirmed, however correct the acknowledgement handling is.
static bool handle_path_return(const mc_packet_t* packet, mesh_state_t* mesh, const identity_t* identity) {
    if (!identity->has_keypair) return false;

    mc_datagram_t datagram;
    if (!mc_datagram_parse(packet->payload, packet->payload_length, &datagram)) return false;
    if (datagram.dest_hash != identity->public_key[0]) {
        session_log("path.skip reason=notus dest=%02x us=%02x", datagram.dest_hash, identity->public_key[0]);
        return false;
    }

    int candidates = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        node_t* node = &mesh->nodes[i];
        if (!node->used || node->key[0] != datagram.src_hash) continue;
        candidates++;
        // A contact we have never agreed a key with cannot be the sender as far
        // as we are concerned, and saying so distinguishes "no key yet" from
        // "the wrong key" when a path return goes unread.
        if (!node->has_secret) {
            session_log("path.skip reason=nosecret src=%02x", datagram.src_hash);
            continue;
        }

        uint8_t plain[MC_MAX_PAYLOAD_SIZE];
        size_t  plain_len = 0;
        if (!mc_datagram_decrypt(node->shared_secret, datagram.mac, datagram.cipher, datagram.cipher_length, plain,
                                 sizeof(plain), &plain_len)) {
            session_log("path.skip reason=mac src=%02x", datagram.src_hash);
            continue;
        }

        mc_path_msg_t returned;
        if (!mc_path_parse(plain, plain_len, &returned)) {
            session_log("path.bad reason=parse len=%u", (unsigned)plain_len);
            return false;
        }

        session_log("path.rx from=%s hops=%u width=%u path=%s extra=%u len=%u",
                    node->long_name[0] ? node->long_name : "?", (unsigned)MC_PATH_COUNT(returned.path_ctrl),
                    (unsigned)MC_PATH_HASH_SIZE(returned.path_ctrl), path_text(returned.path_ctrl, returned.path),
                    (unsigned)returned.extra_type, (unsigned)returned.extra_len);

        // Replace whatever we had. A node handing back a route is describing the
        // one that just worked, and an older path is not evidence of anything.
        if (returned.path_bytes <= sizeof(node->out_path)) {
            memcpy(node->out_path, returned.path, returned.path_bytes);
            node->out_path_ctrl = returned.path_ctrl;
            node->has_out_path  = true;
            node->last_heard    = (uint32_t)time(NULL);
            ESP_LOGI(TAG, "learned a %u hop route to %s", (unsigned)MC_PATH_COUNT(returned.path_ctrl),
                     node->long_name[0] ? node->long_name : "a node");
        }

        // The acknowledgement rides inside the same packet.
        if (returned.extra_type == MC_PAYLOAD_ACK && returned.extra_len >= MC_ACK_HASH_SIZE) {
            credit_ack(mesh, returned.extra);
        }
        return true;
    }

    // Arrived for us but matched nobody: the sender is unknown, or the one-byte
    // hash pointed at contacts whose keys all failed.
    session_log("path.drop src=%02x candidates=%d", datagram.src_hash, candidates);
    return false;
}

// An acknowledgement for one of our direct messages. Matched on the hash the
// sender computed when it built the message.
static bool handle_ack(const mc_packet_t* packet, mesh_state_t* mesh) {
    uint8_t hash[MC_ACK_HASH_SIZE];
    if (!mc_ack_parse(packet->payload, packet->payload_length, hash)) return false;
    return credit_ack(mesh, hash);
}

static bool meshcore_handle(const lora_protocol_lora_packet_t* pkt, mesh_state_t* mesh, const identity_t* identity) {
    mesh->stats.packets_total++;

    if (session_log_active()) {
        // The three stats bytes exactly as the coprocessor reported them. The
        // documented conversions have not proved trustworthy -- rssi_pkt_raw
        // arrives as zero for every packet -- so the raw values go in the log
        // and the reading of them is left until they can be compared.
        char extra[64];
        snprintf(extra, sizeof(extra), "rssiraw=%u snrraw=%d sigraw=%u", (unsigned)pkt->stats.rssi_pkt_raw,
                 (int)pkt->stats.snr_pkt_raw, (unsigned)pkt->stats.signal_rssi_pkt_raw);
        session_log_frame("rx", "mc", extra, pkt->data, pkt->length);
    }

    mc_packet_t packet;
    if (!mc_packet_parse(pkt->data, pkt->length, &packet)) {
        mesh->stats.packets_bad++;
        session_log("rx.bad net=mc reason=parse");
        return false;
    }

    session_log("rx.mc type=%s route=%u hops=%u width=%u path=%s paylen=%u",
                mc_payload_type_name(packet.type), (unsigned)packet.route, (unsigned)packet.hop_count,
                (unsigned)packet.bytes_per_hop, path_text(MC_PATH_CTRL(packet.hop_count, packet.bytes_per_hop),
                                                          packet.path),
                (unsigned)packet.payload_length);

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

    if (packet.type == MC_PAYLOAD_PATH) {
        bool matched = handle_path_return(&packet, mesh, identity);
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
        if (colon) {
            size_t n = (size_t)(colon - decoded.text);
            // An over-long prefix is truncated rather than rejected. Refusing the
            // split leaves the name sitting in the message body with a "?" beside
            // it, which is worse than a shortened name -- and the limit is in
            // bytes, so it is the multi-byte names that hit it first.
            if (n >= sizeof(who)) n = sizeof(who) - 1;
            // Never cut a character in half: a trailing partial sequence is not
            // rendered as anything, it is rendered as damage.
            while (n > 0 && ((unsigned char)decoded.text[n] & 0xC0) == 0x80) n--;

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
        msg->hops = packet.hop_count;
        store_path(msg, &packet);

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

    uint8_t len = mc_packet_build(MC_PAYLOAD_GRP_TXT, MC_ROUTE_FLOOD, MC_PATH_CTRL_NEW, NULL, payload, payload_len, out, out_max);
    if (len == 0) return 0;

    track_transmission(payload, payload_len, msg_seq);
    return len;
}

// Build one attempt at a direct message.
//
// `attempt` goes into the plaintext, so every retry is a different ciphertext
// and a different expected acknowledgement. That is deliberate on MeshCore's
// part: an identical retry would be suppressed as a duplicate by the first
// repeater that saw the original, and never reach anyone.
//
// `use_path` sends along the route we have learned instead of flooding the mesh.
// Directed traffic costs a fraction of the airtime, but it is a claim that the
// route still works -- so the caller counts failures and stops making it.
uint8_t mc_encode_dm_attempt(const identity_t* identity, const node_t* peer, const char* text, uint8_t attempt,
                             bool use_path, message_t* msg, uint8_t* out, size_t out_max) {
    if (!identity->has_keypair || peer == NULL || !peer->has_secret) return 0;

    uint8_t plain[MC_MAX_PAYLOAD_SIZE];
    uint8_t unpadded = 0;
    size_t  padded = mc_dm_frame_plaintext((uint32_t)time(NULL), attempt, text, plain, sizeof(plain), &unpadded);
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

    bool    direct = use_path && peer->has_out_path;
    uint8_t len    = mc_packet_build(MC_PAYLOAD_TXT_MSG, direct ? MC_ROUTE_DIRECT : MC_ROUTE_FLOOD,
                                     direct ? peer->out_path_ctrl : MC_PATH_CTRL_NEW, direct ? peer->out_path : NULL, payload,
                                     payload_len, out, out_max);
    if (len == 0) return 0;

    if (msg) {
        uint32_t expect = (uint32_t)hash[0] | ((uint32_t)hash[1] << 8) | ((uint32_t)hash[2] << 16) |
                          ((uint32_t)hash[3] << 24);
        // Remembered alongside the earlier attempts, not instead of them.
        if (msg->expected_ack_count < MSG_ACK_SLOTS) {
            msg->expected_ack[msg->expected_ack_count++] = expect;
        }
        msg->dm_attempt = attempt;
        msg->dm_direct  = direct;
        memcpy(msg->dm_peer_key, peer->key, NODE_KEY_LEN);
        track_transmission(payload, payload_len, msg->seq);

        session_log("dm.tx seq=%lu to=%02x%02x attempt=%u direct=%u hops=%u path=%s exp=%08lx",
                    (unsigned long)msg->seq, peer->key[0], peer->key[1], (unsigned)attempt, direct ? 1u : 0u,
                    direct ? (unsigned)MC_PATH_COUNT(peer->out_path_ctrl) : 0u,
                    direct ? path_text(peer->out_path_ctrl, peer->out_path) : "-", (unsigned long)expect);
    }
    return len;
}

// How long to wait for an acknowledgement before trying again, following
// upstream's own formula so the timing matches what the other end expects.
uint32_t mc_ack_timeout_ms(const node_t* peer, bool direct) {
    // A short direct message at SF8 / BW62.5 / CR4/8 is roughly this long on
    // air. Close enough: the result is a patience budget, not a measurement.
    const uint32_t airtime = 750;

    if (!direct) return MC_SEND_TIMEOUT_BASE_MS + MC_FLOOD_TIMEOUT_FACTOR * airtime;

    uint8_t hops = peer && peer->has_out_path ? MC_PATH_COUNT(peer->out_path_ctrl) : 0;
    return MC_SEND_TIMEOUT_BASE_MS + (airtime * MC_DIRECT_PERHOP_FACTOR + MC_DIRECT_PERHOP_EXTRA_MS) * (hops + 1);
}

static uint8_t meshcore_encode_dm(mesh_state_t* mesh, const identity_t* identity, const node_t* peer,
                                  const char* text, message_t* msg, uint8_t* out, size_t out_max) {
    (void)mesh;
    // First attempt: use the route if we have one. Failures walk it back.
    return mc_encode_dm_attempt(identity, peer, text, 0, true, msg, out, out_max);
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

    uint8_t len = mc_packet_build(MC_PAYLOAD_ADVERT, MC_ROUTE_FLOOD, MC_PATH_CTRL_NEW, NULL, payload, payload_len, out, out_max);
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
    .handle           = meshcore_handle,
    .take_pending_ack = meshcore_take_pending_ack,
    .encode          = meshcore_encode,
    .encode_dm       = meshcore_encode_dm,
    .encode_advert   = meshcore_encode_advert,
};
