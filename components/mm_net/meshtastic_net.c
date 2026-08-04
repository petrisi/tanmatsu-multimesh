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
#include "base64.h"
#include "crypto_jobs.h"
#include "dedup.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mesh_net.h"
#include "meshtastic_crypto.h"
#include "meshtastic_wire.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "session_log.h"

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
static dedup_t  seen;

// Frames we transmitted, still listening for a repeater to echo them back. A
// broadcast has no destination to filter on, so without this our own message
// would come back and be shown as though a stranger had sent it.
#define MAX_PENDING_TX 4

typedef struct {
    bool     active;
    uint32_t from;
    uint32_t id;
    uint32_t seq;  // the message this frame belongs to
} pending_tx_t;

static pending_tx_t pending[MAX_PENDING_TX];

// The dedup key for a Meshtastic frame: the (from, id) header pair.
static void tx_key(uint32_t from, uint32_t id, uint8_t out[8]) {
    out[0] = (uint8_t)from;
    out[1] = (uint8_t)(from >> 8);
    out[2] = (uint8_t)(from >> 16);
    out[3] = (uint8_t)(from >> 24);
    out[4] = (uint8_t)id;
    out[5] = (uint8_t)(id >> 8);
    out[6] = (uint8_t)(id >> 16);
    out[7] = (uint8_t)(id >> 24);
}

static void track_transmission(uint32_t from, uint32_t id, uint32_t seq) {
    int slot = 0;
    for (int i = 0; i < MAX_PENDING_TX; i++) {
        if (!pending[i].active) {
            slot = i;
            break;
        }
        if (pending[i].seq < pending[slot].seq) slot = i;  // replace the oldest
    }

    pending[slot] = (pending_tx_t){.active = true, .from = from, .id = id, .seq = seq};

    // Record it as seen, so the echo is suppressed rather than displayed twice.
    uint8_t key[8];
    tx_key(from, id, key);
    dedup_remember(&seen, key, sizeof(key));
}

// Credit a repeat to the message that produced it. Returns true if this frame
// was one of ours.
static bool credit_repeat(mesh_state_t* mesh, uint32_t from, uint32_t id) {
    for (int i = 0; i < MAX_PENDING_TX; i++) {
        if (!pending[i].active || pending[i].from != from || pending[i].id != id) continue;

        for (int m = 0; m < mesh->count; m++) {
            message_t* msg = (message_t*)model_message_at(mesh, m);
            if (msg && msg->used && msg->seq == pending[i].seq) {
                if (msg->repeats < 255) msg->repeats++;
                return true;
            }
        }
        pending[i].active = false;  // the message aged out of the ring
        return true;
    }
    return false;
}

static uint8_t encode_to_node(mesh_state_t* mesh, const identity_t* identity, const node_t* peer, uint32_t portnum,
                              const uint8_t* body, size_t body_len, uint32_t request_id, bool want_ack,
                              bool want_response, uint32_t msg_seq, uint8_t* out, size_t out_max);
static uint8_t encode_nodeinfo(mesh_state_t* mesh, const identity_t* identity, const node_t* peer, bool ask,
                               uint8_t* out, size_t out_max);

// --- acknowledgements ----------------------------------------------------
//
// Same arrangement as MeshCore: receive builds the frame, the event loop queues
// it, because receive runs on the event loop and transmitting blocks.

#define MAX_PENDING_ACKS 4

typedef struct {
    bool    active;
    uint8_t frame[MT_HEADER_SIZE + MT_MAX_PAYLOAD_SIZE];
    uint8_t length;
} pending_ack_t;

static pending_ack_t acks[MAX_PENDING_ACKS];

// --- information request throttling --------------------------------------
//
// A NodeInfo request is answered once per day per asker. Upstream does the same
// -- a shorter window there -- because the request is cheap to send and the
// reply is not: on a busy mesh a node that asks everyone, or asks repeatedly,
// would otherwise have the whole band answering it.
//
// Held in memory rather than on the node record, as upstream holds it: a reboot
// forgetting who has been answered costs one extra reply, while putting it on
// disk would change the stored layout and discard every node we know.
//
// Measured against uptime, not the clock. Upstream is explicit about why: a
// wall-clock correction, or a replayed packet carrying a stale timestamp, would
// otherwise move the window under us.
#define INFO_REPLY_INTERVAL_MS (24u * 60 * 60 * 1000)
#define INFO_REPLY_TRACKED     12

typedef struct {
    uint32_t node_num;
    uint32_t at_ms;
    bool     used;
} info_request_t;

static info_request_t info_requests[INFO_REPLY_TRACKED];

static uint32_t uptime_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Record a request and say whether it should be answered. Stamped on every
// request whether or not we reply, which is upstream's behaviour and makes the
// window slide: a node asking more often than once a day never gets an answer
// until it pauses for one.
static bool info_reply_due(uint32_t node_num) {
    uint32_t now   = uptime_ms();
    int      slot  = -1;
    int      oldest = 0;

    for (int i = 0; i < INFO_REPLY_TRACKED; i++) {
        if (info_requests[i].used && info_requests[i].node_num == node_num) {
            slot = i;
            break;
        }
        if (!info_requests[i].used) {
            slot = i;
            break;
        }
        if (info_requests[i].at_ms < info_requests[oldest].at_ms) oldest = i;
    }
    if (slot < 0) slot = oldest;  // full: the least recently heard from makes way

    bool known = info_requests[slot].used && info_requests[slot].node_num == node_num;
    bool due   = !known || (now - info_requests[slot].at_ms) >= INFO_REPLY_INTERVAL_MS;

    info_requests[slot].used     = true;
    info_requests[slot].node_num = node_num;
    info_requests[slot].at_ms    = now;
    return due;
}

// The slots are just deferred outgoing frames: acknowledgements, and replies to
// an information request. Anything the receive path decides to send but cannot,
// because it runs on the loop that must not block.
static pending_ack_t* free_ack_slot(void) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!acks[i].active) return &acks[i];
    }
    ESP_LOGW(TAG, "no free slot for a deferred reply");
    return NULL;
}

static void queue_ack(mesh_state_t* mesh, const identity_t* identity, const node_t* peer, uint32_t request_id) {
    uint8_t routing[MT_ROUTING_ACK_LEN];
    size_t  routing_len = mt_routing_ack_encode(routing, sizeof(routing));
    if (routing_len == 0) return;

    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (acks[i].active) continue;

        // The reply is encrypted the same way the message was: end to end when
        // we hold their key, channel otherwise. It carries no want_ack of its
        // own -- acknowledging an acknowledgement never terminates.
        acks[i].length = encode_to_node(mesh, identity, peer, MT_PORTNUM_ROUTING, routing, routing_len, request_id,
                                        false, false, UINT32_MAX, acks[i].frame, sizeof(acks[i].frame));
        acks[i].active = acks[i].length > 0;
        return;
    }
    ESP_LOGW(TAG, "no slot for an acknowledgement; the sender will retry");
}

// See mesh_net.h: built here, queued by the event loop.
static bool meshtastic_take_pending_ack(uint8_t* out, size_t out_max, uint8_t* out_len) {
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

static bool meshtastic_init(void) {
    dedup_reset(&seen);
    memset(pending, 0, sizeof(pending));
    memset(acks, 0, sizeof(acks));
    memset(info_requests, 0, sizeof(info_requests));
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

static void meshtastic_prepare_channel(channel_t* channel) {
    channel->ready   = false;
    channel->key_len = 0;

    // An empty PSK is valid and means an unencrypted channel, so only a
    // malformed one (negative) is a failure.
    uint8_t raw[MT_MAX_KEY_SIZE];
    int     raw_len = base64_decode(channel->secret, raw, sizeof(raw));
    if (raw_len < 0) return;

    mt_key_t expanded;
    if (!mt_key_expand(raw, (size_t)raw_len, &expanded)) return;

    if (expanded.length > 0) memcpy(channel->key, expanded.bytes, expanded.length);
    channel->key_len = (uint8_t)expanded.length;
    // The hash mixes the channel NAME as well as the key, so renaming a channel
    // changes which traffic it matches. That is upstream behaviour, not a bug.
    channel->hash  = mt_channel_hash(channel->name, &expanded);
    channel->ready = true;
}

static void detail(mesh_state_t* mesh) {
    snprintf(mesh->stats.detail, sizeof(mesh->stats.detail), "text:%lu other:%lu dup:%lu", (unsigned long)text_msgs,
             (unsigned long)other_ports, (unsigned long)mesh->stats.duplicates);
}

// Ask for the end-to-end key with a node, once. Only possible after a NodeInfo
// has told us their public key.
static void want_secret(node_t* node, const identity_t* identity) {
    if (node == NULL || node->has_secret || node->secret_pending) return;
    if (!identity->has_mt_keypair || !node->has_public_key) return;

    if (crypto_queue_mt_secret(node->node_num, node->public_key, identity->mt_private_key)) {
        node->secret_pending = true;
    }
}

// Nothing on the air says "this is end-to-end encrypted". A PKI direct message
// is recognised by a zero channel hash on a packet addressed to us and nothing
// else -- the authentication tag is what decides, which is the one advantage
// this has over the channel cipher.
static bool try_pki(mt_packet_t* packet, mesh_state_t* mesh, const identity_t* identity, node_t* node,
                    mt_data_t* out) {
    if (!identity->has_mt_keypair || packet->channel_hash != 0) return false;
    if (packet->to != identity->node_num) return false;
    if (node == NULL || !node->has_secret) return false;
    if (packet->payload_length <= MT_PKI_OVERHEAD) return false;

    uint8_t plain[MT_MAX_PAYLOAD_SIZE];
    size_t  plain_len = 0;
    if (!mt_pki_decrypt(node->shared_secret, packet->from, packet->id, packet->payload, packet->payload_length, plain,
                        sizeof(plain), &plain_len)) {
        return false;
    }
    return mt_data_parse(plain, plain_len, out);
}

// What to do with a decoded payload, whichever cipher got us here. `channel` is
// the row's channel column; `dm` says the packet was addressed to us rather than
// broadcast, which is true of both an end-to-end message and one merely
// addressed to us under a channel key.
// A routing reply naming one of our messages. Meshtastic has no separate ack
// packet: an acknowledgement is a ROUTING_APP payload carrying a success code,
// tied to what it acknowledges only by request_id.
static bool handle_routing(const mt_data_t* data, mesh_state_t* mesh) {
    if (!data->has_request_id) return false;
    if (!mt_routing_is_ack(data->payload, data->payload_length)) return false;

    for (int i = 0; i < mesh->count; i++) {
        message_t* msg = (message_t*)model_message_at(mesh, i);
        if (msg == NULL || !msg->used || !msg->outgoing || !msg->dm) continue;
        if (msg->acked || msg->expected_ack_count == 0 || msg->expected_ack[0] != data->request_id) continue;

        msg->acked = true;
        msg->tx    = TX_CONFIRMED;
        ESP_LOGI(TAG, "dm to %s acknowledged", msg->peer);
        return true;
    }
    return false;
}

// `duplicate` means we have seen this packet before. It is still decoded and
// still acknowledged -- only the display is suppressed.
static bool deliver(const mt_data_t* data, const mt_packet_t* packet, mesh_state_t* mesh, node_t* node,
                    const lora_protocol_lora_packet_t* pkt, int channel, bool dm, bool duplicate,
                    const identity_t* identity) {
    // NodeInfo is what turns a bare node number into a name, and where the key
    // for an end-to-end conversation comes from. Handled before the text path
    // because it is not a message and must not be shown as one.
    if (data->portnum == MT_PORTNUM_NODEINFO) {
        mt_user_t user;
        if (node && mt_user_parse(data->payload, data->payload_length, &user)) {
            snprintf(node->long_name, sizeof(node->long_name), "%s", user.long_name);
            snprintf(node->short_name, sizeof(node->short_name), "%s", user.short_name);
            node->hw_model = user.hw_model;
            node->role     = user.role;
            if (user.has_public_key) {
                // A node that changes its key is a different node as far as
                // encryption goes, so the cached secret has to go with it.
                if (!node->has_public_key || memcmp(node->public_key, user.public_key, NODE_KEY_LEN) != 0) {
                    memcpy(node->public_key, user.public_key, NODE_KEY_LEN);
                    node->has_public_key = true;
                    node->has_secret     = false;
                    node->secret_pending = false;
                }
                want_secret(node, identity);
            }
            node->named = user.long_name[0] != '\0' || user.short_name[0] != '\0';
            ESP_LOGI(TAG, "nodeinfo %08lx = %s (%s)", (unsigned long)packet->from, user.long_name, user.short_name);
            session_log("nodeinfo.rx from=%08lx name=\"%s\" short=\"%s\" haskey=%u wantresp=%u",
                        (unsigned long)packet->from, user.long_name, user.short_name, user.has_public_key ? 1u : 0u,
                        data->want_response ? 1u : 0u);

            // Answer a request with our own, which is the other half of the
            // exchange: without it the asker never learns our key and can never
            // send us anything private. Only when addressed to us -- a broadcast
            // asking everyone would set the whole mesh talking at once.
            if (data->want_response && packet->to == identity->node_num) {
                if (!info_reply_due(packet->from)) {
                    session_log("nodeinfo.hold to=%08lx reason=answered_within_24h", (unsigned long)packet->from);
                } else {
                    pending_ack_t* slot = free_ack_slot();
                    if (slot) {
                        slot->length = encode_nodeinfo(mesh, identity, node, false, slot->frame, sizeof(slot->frame));
                        slot->active = slot->length > 0;
                        session_log("nodeinfo.tx to=%08lx reason=request", (unsigned long)packet->from);
                    }
                }
            }
        }
        other_ports++;
        return false;
    }

    if (data->portnum == MT_PORTNUM_ROUTING) {
        bool matched = handle_routing(data, mesh);
        other_ports++;
        return matched;
    }

    if (data->portnum != MT_PORTNUM_TEXT_MESSAGE) {
        other_ports++;
        return false;
    }

    char   body[TEXT_MAX];
    size_t len = data->payload_length < sizeof(body) - 1 ? data->payload_length : sizeof(body) - 1;
    memcpy(body, data->payload, len);
    body[len] = '\0';

    // Prefer the short name we learned from a NodeInfo. Until one arrives the
    // sender is the low 16 bits of the node number, and colour rather than a
    // prefix marks it as an id.
    char who[SENDER_MAX];
    bool named = node && node->named && node->short_name[0];
    if (named) {
        snprintf(who, sizeof(who), "%s", node->short_name);
    } else {
        snprintf(who, sizeof(who), "%04lx", (unsigned long)(packet->from & 0xFFFF));
    }

    // Acknowledge before displaying, and acknowledge every copy: the sender
    // retransmits until it hears one, so staying quiet about a repeat we already
    // recognise is what makes it try again. Dedup drops the display, not the
    // reply.
    if (dm && packet->want_ack && node != NULL) queue_ack(mesh, identity, node, packet->id);
    if (duplicate) return false;

    message_t* msg = model_push(mesh, (uint8_t)channel, who, named, body, false);
    msg->dm        = dm;
    if (dm) snprintf(msg->peer, sizeof(msg->peer), "%s", who);
    // Meshtastic stamps no time in the payload, so this is our receive clock.
    msg->rssi_dbm  = -(int)pkt->stats.rssi_pkt_raw / 2;
    msg->snr_db_x4 = pkt->stats.snr_pkt_raw;
    msg->hops      = mt_hops_taken(packet);
    msg->hop_start = packet->hop_start;
    msg->hop_limit = packet->hop_limit;
    if (packet->relay_node) snprintf(msg->relayed_by, sizeof(msg->relayed_by), "%02x", packet->relay_node);

    text_msgs++;
    mesh->stats.messages++;
    ESP_LOGI(TAG, "%s [%d dBm] %s: %s", dm ? "dm" : "msg", msg->rssi_dbm, who, body);
    return true;
}

static bool meshtastic_handle(const lora_protocol_lora_packet_t* pkt, mesh_state_t* mesh,
                              const identity_t* identity) {
    mesh->stats.packets_total++;

    mt_packet_t packet;
    if (!mt_packet_parse(pkt->data, pkt->length, &packet)) {
        mesh->stats.packets_bad++;
        return false;
    }

    // Meshtastic floods, so the same message arrives directly and again from
    // every repeater. (from, id) identifies it: the payload is unchanged by a
    // relay but the flags and relay_node byte are not, so keying on the header
    // pair is both cheaper and more accurate than fingerprinting bytes.
    uint8_t key[8];
    tx_key(packet.from, packet.id, key);
    bool duplicate = dedup_check(&seen, key, sizeof(key));
    if (duplicate) {
        mesh->stats.duplicates++;
        // A repeat of something we sent is not noise: it is the only delivery
        // confirmation available for a broadcast.
        credit_repeat(mesh, packet.from, packet.id);

        // A retransmission addressed to us still has to be answered. Meshtastic
        // reuses the packet id when it retries, so this is exactly the traffic
        // dedup would otherwise swallow -- and the sender only stops retrying
        // once it hears an acknowledgement. Upstream skips its own seen-check
        // for the same reason. Anything else can be dropped here.
        if (!(packet.want_ack && packet.to == identity->node_num)) {
            detail(mesh);
            return false;
        }
    }

    // Every packet tells us a node exists, whatever it turns out to contain and
    // whether or not we hold its channel key. Names arrive later on NodeInfo.
    node_t* node = model_node_touch_mt(mesh, packet.from);
    if (node) {
        node->rssi_dbm  = -(int)pkt->stats.rssi_pkt_raw / 2;
        node->snr_db_x4 = pkt->stats.snr_pkt_raw;
        node->hops      = mt_hops_taken(&packet);
    }

    // End-to-end first. It is tried before the channels because a PKI packet
    // carries a zero channel hash, which an unencrypted channel could otherwise
    // match by coincidence and then fail to parse.
    mt_data_t pki_data;
    if (try_pki(&packet, mesh, identity, node, &pki_data)) {
        bool shown = deliver(&pki_data, &packet, mesh, node, pkt, (uint8_t)mesh->input_channel, true, duplicate, identity);
        detail(mesh);
        return shown;
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

        // NodeInfo is what turns a bare node number into a name. Handled before
        // the text-message path because it is not a message and must not be
        // shown as one.
        bool shown = deliver(&data, &attempt, mesh, node, pkt, i, attempt.to == identity->node_num, duplicate, identity);
        detail(mesh);
        return shown;
    }

    mesh->stats.not_our_channel++;
    detail(mesh);
    return false;
}

#define MT_BROADCAST_ADDR 0xFFFFFFFFu
#define MT_DEFAULT_HOPS   3

// The hop limit in force. Pushed in from the configuration rather than read
// from the model, so the stack stays a consumer of settings rather than a
// reader of application state.
static uint8_t active_hops = MT_DEFAULT_HOPS;

void mt_set_hop_limit(uint8_t hops) {
    active_hops = hops > SET_HOPS_MAX_SESSION ? SET_HOPS_MAX_SESSION : hops;
}

static uint8_t advertised_role = MT_ROLE_CLIENT_MUTE;

void mt_set_role(uint8_t role) {
    advertised_role = role;
}

// Everything we transmit goes through here: wrap an application payload in a
// Data submessage, encrypt it, and build the broadcast header. `msg_seq` of
// UINT32_MAX means there is no message row to credit repeats to.
static uint8_t encode_frame(mesh_state_t* mesh, uint8_t channel, const identity_t* identity, uint32_t portnum,
                            const uint8_t* body, size_t body_len, uint32_t msg_seq, uint8_t* out, size_t out_max) {
    if (channel >= mesh->channel_count) return 0;
    const channel_t* ch = &mesh->channels[channel];
    if (!ch->ready) return 0;

    uint8_t payload[MT_MAX_PAYLOAD_SIZE];
    size_t  payload_len = mt_data_encode(portnum, body, body_len, 0, false, payload, sizeof(payload));
    if (payload_len == 0) return 0;

    // The packet id is half the AES-CTR nonce. Reusing one under the same
    // channel key reuses keystream, and two messages XORed together are
    // recoverable plaintext -- so this must come from the hardware RNG and never
    // from a counter that restarts at boot.
    uint32_t id = esp_random();
    if (id == 0) id = 1;  // zero is reserved for "no id"

    mt_key_t key = {.length = ch->key_len};
    if (ch->key_len) memcpy(key.bytes, ch->key, ch->key_len);
    if (!mt_encrypt(&key, identity->node_num, id, payload, payload_len)) return 0;

    // Nobody acknowledges a broadcast, so want_ack is never set here.
    uint8_t len = mt_packet_build(MT_BROADCAST_ADDR, identity->node_num, id, active_hops, ch->hash, false,
                                  payload, (uint8_t)payload_len, out, out_max);
    if (len == 0) return 0;

    track_transmission(identity->node_num, id, msg_seq);
    return len;
}

static uint8_t meshtastic_encode(mesh_state_t* mesh, uint8_t channel, const identity_t* identity, const char* text,
                                 uint32_t msg_seq, uint8_t* out, size_t out_max) {
    return encode_frame(mesh, channel, identity, MT_PORTNUM_TEXT_MESSAGE, (const uint8_t*)text, strlen(text),
                        msg_seq, out, out_max);
}

// Everything addressed to a single node goes through here: a text message, or
// the routing reply that acknowledges one.
//
// End-to-end encrypted when the recipient has published a key and we have agreed
// one with them; otherwise addressed to them but encrypted under the channel key,
// which every node understands and everyone on the channel can read. The UI says
// which happened -- silently sending something weaker than the user expects
// would be the worse failure.
static uint8_t encode_to_node(mesh_state_t* mesh, const identity_t* identity, const node_t* peer, uint32_t portnum,
                              const uint8_t* body, size_t body_len, uint32_t request_id, bool want_ack, bool want_response,
                              uint32_t msg_seq, uint8_t* out, size_t out_max) {
    if (peer == NULL || peer->node_num == 0) return 0;

    uint8_t payload[MT_MAX_PAYLOAD_SIZE];
    size_t  payload_len = mt_data_encode(portnum, body, body_len, request_id, want_response, payload, sizeof(payload));
    if (payload_len == 0) return 0;

    uint32_t id = esp_random();
    if (id == 0) id = 1;

    if (identity->has_mt_keypair && peer->has_secret) {
        uint8_t sealed[MT_MAX_PAYLOAD_SIZE];
        if (payload_len + MT_PKI_OVERHEAD > sizeof(sealed)) return 0;

        // The nonce extension must be fresh per packet: with the id it is the
        // whole nonce, and a repeat under one key breaks CCM outright.
        uint32_t extra_nonce = esp_random();
        if (!mt_pki_encrypt(peer->shared_secret, identity->node_num, id, extra_nonce, payload, payload_len, sealed,
                            sizeof(sealed))) {
            return 0;
        }

        // Channel hash zero is how the far end knows to try its own key.
        uint8_t len = mt_packet_build(peer->node_num, identity->node_num, id, active_hops, 0, want_ack, sealed,
                                      (uint8_t)(payload_len + MT_PKI_OVERHEAD), out, out_max);
        if (len == 0) return 0;

        if (msg_seq != UINT32_MAX) track_transmission(identity->node_num, id, msg_seq);
        return len;
    }

    // No key for them: fall back to the channel cipher, addressed to one node.
    if (mesh->input_channel >= mesh->channel_count) return 0;
    const channel_t* ch = &mesh->channels[mesh->input_channel];
    if (!ch->ready) return 0;

    mt_key_t key = {.length = ch->key_len};
    if (ch->key_len) memcpy(key.bytes, ch->key, ch->key_len);
    if (!mt_encrypt(&key, identity->node_num, id, payload, payload_len)) return 0;

    uint8_t len = mt_packet_build(peer->node_num, identity->node_num, id, active_hops, ch->hash, want_ack,
                                  payload, (uint8_t)payload_len, out, out_max);
    if (len == 0) return 0;

    if (msg_seq != UINT32_MAX) track_transmission(identity->node_num, id, msg_seq);
    return len;
}

// Build our NodeInfo addressed to one node. `ask` sets want_response, which is
// what makes this an exchange rather than an announcement.
//
// This is Meshtastic's key exchange, and the whole of it. NodeInfo is one of the
// port numbers upstream excludes from end-to-end encryption precisely so it can
// travel before any key is known: it goes out under the channel key, carries our
// public key, and asks for theirs back. Everything private between two nodes
// depends on this having happened first.
static uint8_t encode_nodeinfo(mesh_state_t* mesh, const identity_t* identity, const node_t* peer, bool ask,
                               uint8_t* out, size_t out_max) {
    mt_user_t user = {0};
    snprintf(user.id, sizeof(user.id), "%s", identity->node_id);
    snprintf(user.long_name, sizeof(user.long_name), "%s", identity->name);
    snprintf(user.short_name, sizeof(user.short_name), "%s", identity->short_name);
    user.hw_model = MT_HW_PRIVATE;
    user.role     = advertised_role;
    if (identity->has_mt_keypair) {
        memcpy(user.public_key, identity->mt_public_key, MT_PUBLIC_KEY_LEN);
        user.has_public_key = true;
    }

    uint8_t body[MT_MAX_PAYLOAD_SIZE];
    size_t  body_len = mt_user_encode(&user, body, sizeof(body));
    if (body_len == 0) return 0;

    return encode_to_node(mesh, identity, peer, MT_PORTNUM_NODEINFO, body, body_len, 0, false, ask, UINT32_MAX, out,
                          out_max);
}

uint8_t mt_encode_info_exchange(mesh_state_t* mesh, const identity_t* identity, const node_t* peer, uint8_t* out,
                                size_t out_max) {
    return encode_nodeinfo(mesh, identity, peer, true, out, out_max);
}

static uint8_t meshtastic_encode_dm(mesh_state_t* mesh, const identity_t* identity, const node_t* peer,
                                    const char* text, message_t* msg, uint8_t* out, size_t out_max) {
    // No key, no message. Current firmware refuses to send a text message to a
    // node whose public key it lacks, and refuses one that arrives encrypted
    // under a channel key -- it decrypts it, parses it, recognises it as a
    // direct message and discards it with "Rejecting legacy DM".
    //
    // So the channel fallback that used to be here did not degrade gracefully:
    // it produced a message that looked sent, was never shown to anyone, and
    // could not even be acknowledged, because the acknowledgement is generated
    // after a successful decode. Refusing is the honest outcome, and the node
    // detail offers the exchange that fixes it.
    //
    // The check is here rather than in encode_to_node because acknowledgements
    // and NodeInfo still travel under the channel key by design -- NodeInfo
    // especially, since that is what carries the key that lifts this.
    if (!identity->has_mt_keypair || peer == NULL || !peer->has_secret) return 0;

    // want_ack is what asks the recipient for a routing reply. Without it a
    // Meshtastic node stays silent and there is nothing to confirm delivery
    // with, which is why our direct messages never showed as acknowledged.
    uint8_t len = encode_to_node(mesh, identity, peer, MT_PORTNUM_TEXT_MESSAGE, (const uint8_t*)text, strlen(text), 0,
                                 true, false, msg ? msg->seq : UINT32_MAX, out, out_max);
    if (len == 0) return 0;

    if (msg) {
        msg->acked = false;
        // The recipient names the packet it is answering, so remember which one
        // this was. Recovered from the frame rather than threaded back out of
        // the encoder, because the header is where the id actually ended up.
        msg->expected_ack[0]    = (uint32_t)out[8] | ((uint32_t)out[9] << 8) | ((uint32_t)out[10] << 16) |
                               ((uint32_t)out[11] << 24);
        msg->expected_ack_count = 1;
    }
    return len;
}

// NodeInfo: how a Meshtastic node tells the mesh what to call it, and where it
// publishes the key others need to message it end to end. Unsigned and
// unauthenticated -- any node may claim any name -- which is why nothing here
// records a verification verdict.
static uint8_t meshtastic_encode_advert(mesh_state_t* mesh, uint8_t channel, const identity_t* identity,
                                        uint8_t* out, size_t out_max) {
    mt_user_t user = {0};
    snprintf(user.id, sizeof(user.id), "%s", identity->node_id);
    snprintf(user.long_name, sizeof(user.long_name), "%s", identity->name);
    snprintf(user.short_name, sizeof(user.short_name), "%s", identity->short_name);
    user.hw_model = MT_HW_PRIVATE;  // a Tanmatsu is not one of upstream's boards
    user.role     = advertised_role;
    if (identity->has_mt_keypair) {
        memcpy(user.public_key, identity->mt_public_key, MT_PUBLIC_KEY_LEN);
        user.has_public_key = true;
    }

    uint8_t body[MT_MAX_PAYLOAD_SIZE];
    size_t  body_len = mt_user_encode(&user, body, sizeof(body));
    if (body_len == 0) return 0;

    return encode_frame(mesh, channel, identity, MT_PORTNUM_NODEINFO, body, body_len, UINT32_MAX, out, out_max);
}

static const char* meshtastic_local_sender(const identity_t* identity) {
    // Meshtastic nodes are shown by short name; other clients will label us that
    // way once a NodeInfo goes out, so the echo should match rather than showing
    // a truncated long name.
    return identity->short_name[0] ? identity->short_name : identity->node_id;
}

const mesh_net_t mesh_net_meshtastic = {
    .name            = "Meshtastic",
    .tag             = "MT",
    .init            = meshtastic_init,
    .get_config      = meshtastic_get_config,
    .prepare_channel = meshtastic_prepare_channel,
    .local_sender    = meshtastic_local_sender,
    .handle           = meshtastic_handle,
    .take_pending_ack = meshtastic_take_pending_ack,
    .encode          = meshtastic_encode,
    .encode_dm       = meshtastic_encode_dm,
    .encode_advert   = meshtastic_encode_advert,
};
