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
#include "dedup.h"
#include "esp_log.h"
#include "esp_random.h"
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

static bool meshtastic_init(void) {
    dedup_reset(&seen);
    memset(pending, 0, sizeof(pending));
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

static bool meshtastic_handle(const lora_protocol_lora_packet_t* pkt, mesh_state_t* mesh) {
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
    if (dedup_check(&seen, key, sizeof(key))) {
        mesh->stats.duplicates++;
        // A repeat of something we sent is not noise: it is the only delivery
        // confirmation available for a broadcast.
        credit_repeat(mesh, packet.from, packet.id);
        detail(mesh);
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

#define MT_BROADCAST_ADDR 0xFFFFFFFFu
#define MT_DEFAULT_HOPS   3

static uint8_t meshtastic_encode(mesh_state_t* mesh, uint8_t channel, const identity_t* identity, const char* text,
                                 uint32_t msg_seq, uint8_t* out, size_t out_max) {
    if (channel >= mesh->channel_count) return 0;
    const channel_t* ch = &mesh->channels[channel];
    if (!ch->ready) return 0;

    uint8_t payload[MT_MAX_PAYLOAD_SIZE];
    size_t  payload_len = mt_data_encode(MT_PORTNUM_TEXT_MESSAGE, (const uint8_t*)text, strlen(text), payload,
                                         sizeof(payload));
    if (payload_len == 0) return 0;

    // The packet id is half the AES-CTR nonce. Reusing one under the same
    // channel key reuses keystream, and two messages XORed together are
    // recoverable plaintext -- so this must come from the hardware RNG and never
    // from a counter that restarts at boot.
    uint32_t id = esp_random();
    if (id == 0) id = 1;  // zero is reserved for "no id"

    mt_key_t key = {.length = ch->key_len};
    memcpy(key.bytes, ch->key, ch->key_len);
    if (!mt_encrypt(&key, identity->node_num, id, payload, payload_len)) return 0;

    uint8_t len = mt_packet_build(MT_BROADCAST_ADDR, identity->node_num, id, MT_DEFAULT_HOPS, ch->hash, payload,
                                  (uint8_t)payload_len, out, out_max);
    if (len == 0) return 0;

    track_transmission(identity->node_num, id, msg_seq);
    return len;
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
    .handle          = meshtastic_handle,
    .encode          = meshtastic_encode,
};
