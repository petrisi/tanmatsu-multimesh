// SPDX-License-Identifier: MIT
//
// UI prototype state.
//
// This is a mock-up: no radio, no protocol. It exists to settle layout, colour
// and key handling before any of that is wired in. The shape of the model is
// deliberately the shape the real app will need, so the drawing code written
// against it survives.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pax_types.h"

#define MAX_CHANNELS  6
#define MAX_MESSAGES  64
#define COMPOSER_MAX  256
// Long enough for a full MeshCore name plus its terminator. MeshCore allows 32
// bytes, and bytes are what matters: "Härmälänranta" is thirteen characters but
// sixteen bytes, and a limit set by character count silently rejects Finnish
// names that a Finnish user considers unremarkable.
#define SENDER_MAX    33
#define TEXT_MAX      192
#define HISTORY_MAX   12

// The column abbreviation. Longer than it looks it needs to be: MeshCore
// hashtag channels read badly cut to four characters, and the message list sizes
// its channel column to the longest one actually configured rather than always
// reserving the maximum.
#define CH_DISPLAY_MAX 10
#define CH_NAME_MAX    23
#define CH_SECRET_MAX  47

#define ID_NAME_MAX  23
#define ID_SHORT_MAX 4

// Outstanding acknowledgements one message can be waiting on: MeshCore's attempt
// counter is two bits, so four is every attempt it can distinguish.
#define MSG_ACK_SLOTS 4

// A public key, which on MeshCore is also the node's identity. Declared here
// rather than beside the other node constants because a message has to remember
// which key it was sent to, and messages are declared first.
#define NODE_KEY_LEN 32

// MeshCore's MAX_PATH_SIZE. Every legal route fits: 63 one-byte hops, 32
// two-byte or 21 three-byte, and the protocol rejects anything longer.
#define NODE_PATH_MAX 64

// Channel colours are chosen from a fixed palette so every channel is visually
// distinct and the editor can offer them as a row of swatches.
#define CH_PALETTE_SIZE 6
extern const pax_col_t ch_palette[CH_PALETTE_SIZE];

typedef enum {
    MESH_MC = 0,
    MESH_MT = 1,
    MESH_COUNT
} mesh_id_t;

typedef struct {
    char      name[CH_NAME_MAX + 1];        // as configured: "EdgeFastLow"
    char      display[CH_DISPLAY_MAX + 1];  // what fits a column: "EFL"
    char      secret[CH_SECRET_MAX + 1];    // MC: key hex. MT: PSK base64.
    pax_col_t color;

    // Derived from `secret` by the owning network stack, never edited directly:
    // the two networks expand and hash their keys differently, and that is
    // protocol knowledge the domain has no business holding.
    uint8_t key[32];
    uint8_t key_len;
    uint8_t hash;   // channel hash byte carried in the clear on the wire
    bool    ready;  // false when the secret failed to parse
} channel_t;

// Outgoing messages occupy the timestamp column with their progress, and only
// settle into a clock once nothing further is expected. A message that never
// saw a repeat keeps its timestamp in red.
typedef enum {
    TX_NONE = 0,   // received, not ours
    TX_QUEUED,     // waiting for the radio
    TX_SENDING,    // on air
    TX_AWAITING,   // sent; watching for repeats / acks
    TX_CONFIRMED,  // heard repeated, or acked
    TX_FAILED,     // window closed with nothing heard
} tx_state_t;

typedef struct {
    bool     used;
    uint32_t seq;      // monotonic; scroll and selection anchor on this
    uint8_t  channel;  // index into the owning mesh's channel list

    // Always our receive clock, on both networks. It is the only clock we
    // control, so it is the only one that gives a stable ordering and cannot
    // put a message in the wrong week because a sender's clock is adrift.
    uint32_t timestamp;

    // What the sender claimed, where the protocol carries one -- MeshCore does,
    // Meshtastic does not. Kept rather than discarded: it is the evidence when
    // a remote clock is wrong, and it is shown in the message detail.
    uint32_t sender_timestamp;
    // Meshtastic: the node's 4-char short name, or the low 16 bits of the node
    // number in hex until a NodeInfo arrives to name it -- the two are told
    // apart by colour, not by a prefix, because columns are expensive.
    // MeshCore: the name the sender put in the message.
    char sender[SENDER_MAX];
    bool sender_named;
    char text[TEXT_MAX];
    int  rssi_dbm;
    int  snr_db_x4;
    uint8_t hops;

    // Routing, shown in the detail view. The two networks expose different
    // things: MeshCore carries the actual path taken as a list of node-key
    // prefixes, Meshtastic only a hop budget plus the last relay.
    //
    // A MeshCore hop is one, two or three bytes of the repeater's public key,
    // chosen per packet -- a mesh moving to two-byte hops identifies its
    // repeaters far less ambiguously. So the width has to travel with the bytes:
    // without it a four-hop two-byte path reads as eight one-byte hops, which is
    // both the wrong route and the wrong distance.
    uint8_t path[24];        // enough for 24 one-byte, 12 two-byte or 8 three-byte hops
    uint8_t path_len;        // bytes stored, which may be fewer than arrived
    uint8_t path_hash_size;  // bytes per hop; 0 on rows that predate this
    bool    path_truncated;  // the route was longer than we kept
    uint8_t hop_start;       // Meshtastic: hop limit as sent
    uint8_t hop_limit;       // Meshtastic: hops remaining on arrival
    char    relayed_by[8];  // short name or id of the last relay, "" if unknown

    bool       outgoing;
    tx_state_t tx;
    uint8_t    repeats;    // repeaters heard repeating this message
    uint32_t   tx_tick_ms;  // when the current tx state was entered

    // Direct messages. `peer` is the other end whichever way it went, so the
    // column reads the same for both halves of a conversation.
    bool     dm;
    char     peer[SENDER_MAX];
    bool acked;  // the recipient proved it decrypted this

    // What would prove it. One entry per attempt, because a MeshCore retry
    // carries a different attempt counter inside the encrypted plaintext and so
    // expects a *different* acknowledgement hash. Keeping only the newest means
    // an acknowledgement for an earlier attempt -- which is the normal outcome
    // when the reply is merely slow rather than lost -- cannot be recognised,
    // and a message that was delivered is reported as failed.
    uint32_t expected_ack[MSG_ACK_SLOTS];
    uint8_t  expected_ack_count;

    // What a resend needs. MeshCore direct messages are retried along a stored
    // route before giving up on it, and a retry has to be re-encrypted to the
    // same key -- so the recipient is held by identity rather than by a node
    // index that pruning would invalidate.
    uint8_t dm_peer_key[NODE_KEY_LEN];
    uint8_t dm_attempt;  // bumped per retry; it is inside the signed plaintext
    bool    dm_direct;   // this attempt used a stored route rather than flooding
} message_t;

// Receive counters, kept per network. `detail` is a short free-form breakdown
// each stack fills with whatever is diagnostic for it (payload types, portnums).
typedef struct {
    uint32_t packets_total;    // frames handed up by the radio
    uint32_t packets_bad;      // failed to parse as this network's framing
    uint32_t duplicates;       // flood retransmits of something already seen
    uint32_t not_our_channel;  // parsed, but wrong channel or failed the MAC
    uint32_t messages;         // decoded, displayable messages
    char     detail[72];
} rx_stats_t;

#define NODE_NAME_MAX  39
#define NODE_SHORT_MAX 7
#define MAX_NODES      48

// Whether the name attached to a node is actually proven to belong to it.
//
// Only MeshCore answers this: its adverts are Ed25519-signed by the key that is
// the node's identity, so a good signature means the name really came from the
// holder of that key. Meshtastic NodeInfo is unsigned -- any node can claim any
// name -- so Meshtastic entries stay UNKNOWN and the UI says nothing.
typedef enum {
    NODE_VERIFY_UNKNOWN = 0,  // nothing checked, or nothing checkable
    NODE_VERIFY_PENDING,      // queued; the check takes about a second
    NODE_VERIFY_VALID,
    NODE_VERIFY_BAD,  // the signature did not match: someone is spoofing
} node_verify_t;

// One heard node, on either network. The two disagree about what identifies a
// node -- Meshtastic uses a 32-bit number, MeshCore uses the public key itself
// -- so both fields exist and each network fills the one it uses.
typedef struct {
    bool     used;
    uint32_t last_heard;  // our clock, never the sender's
    bool     named;       // a NodeInfo or a named advert has been seen
    uint8_t  verified;    // node_verify_t

    uint32_t node_num;  // Meshtastic

    // The short label the message list shows. Meshtastic nodes publish one in
    // their NodeInfo; MeshCore has no such field on the wire, so there it is
    // whatever the user typed. Either way an empty one falls back to the first
    // characters of the long name, so this only needs setting when that reads
    // badly -- which for a Finnish place name it usually does.
    //
    // Field order here is the on-disk order of the node table. Do not reorder
    // without bumping NODEFILE_VERSION.
    char    short_name[NODE_SHORT_MAX + 1];
    uint8_t hw_model;  // Meshtastic

    uint8_t key[NODE_KEY_LEN];  // MeshCore: the identity. Meshtastic: unused.
    uint8_t role;               // MeshCore

    char    long_name[NODE_NAME_MAX + 1];
    uint8_t public_key[NODE_KEY_LEN];  // Meshtastic: Curve25519 from NodeInfo
    bool    has_public_key;

    // The key direct messages with this node are encrypted under. Derived once
    // -- it costs a scalar multiplication -- and kept, because it depends only
    // on two permanent identities and so never changes.
    uint8_t shared_secret[NODE_KEY_LEN];
    bool    has_secret;
    bool    secret_pending;  // queued for derivation; not persisted as such

    // MeshCore: the route to this node, learned when it hands one back. Sending
    // along it costs a fraction of the airtime of flooding the whole mesh.
    //
    // Kept because it is a claim about the world that stops being true without
    // warning -- a repeater moves or dies and the route silently stops working.
    // So it is discarded on repeated failure, and that discarding is written to
    // disk too: rediscovering by flood is cheap, while retrying a dead route
    // every time is not.
    uint8_t out_path[NODE_PATH_MAX];
    uint8_t out_path_ctrl;  // packed hop count and width; 0 when there is none
    bool    has_out_path;

    int     rssi_dbm;  // of the last packet heard from it
    int     snr_db_x4;
    uint8_t hops;
} node_t;

typedef struct {
    const char* name;
    pax_col_t   accent;  // status bar background for this mesh
    rx_stats_t  stats;

    node_t nodes[MAX_NODES];
    int    node_count;

    channel_t channels[MAX_CHANNELS];
    int       channel_count;
    int       input_channel;  // where the composer sends when not in a conversation

    // A conversation, when one is selected. Identified by who rather than by
    // index: the node table is pruned and reordered, and a stale index would
    // quietly retarget a message at whoever moved into the slot.
    bool     target_contact;
    uint32_t target_num;               // Meshtastic
    uint8_t  target_key[NODE_KEY_LEN];  // MeshCore

    message_t messages[MAX_MESSAGES];
    int       head;
    int       count;
    uint32_t  next_seq;

    // Scrolling anchors on a message rather than a distance from the end, so an
    // arriving message does not drag the viewport.
    bool     pinned;       // following live traffic
    uint32_t anchor_seq;   // topmost message when not pinned
    int      anchor_line;  // wrapped line within that message
    int      unseen;       // arrived while scrolled away

    // Selection mode. -1 when inactive; otherwise a message seq.
    int32_t selected_seq;
} mesh_state_t;

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_PICKER,
    OVERLAY_EDITOR,
    OVERLAY_DETAIL,
    OVERLAY_IDENTITY,
    OVERLAY_CONFIRM,
    OVERLAY_NODES,
    OVERLAY_NODE_DETAIL,
    OVERLAY_NODE_SHORT,  // typing a short name for one node
} overlay_t;

// What a pending confirmation will do if accepted.
typedef enum {
    CONFIRM_NONE = 0,
    CONFIRM_DELETE_CHANNEL,
    CONFIRM_CLEAR_NODES,
} confirm_action_t;

typedef enum {
    FIELD_NAME = 0,
    FIELD_DISPLAY,
    FIELD_SECRET,
    FIELD_COLOR,
    FIELD_COUNT,
} editor_field_t;

typedef struct {
    bool creating;
    int  index;
    int  field;
    char name[CH_NAME_MAX + 1];
    char display[CH_DISPLAY_MAX + 1];
    char secret[CH_SECRET_MAX + 1];
    int  color;
} editor_t;

typedef enum {
    ID_FIELD_NAME = 0,
    ID_FIELD_SHORT,
    ID_FIELD_COUNT,
} identity_field_t;

typedef struct {
    char     name[ID_NAME_MAX + 1];  // MeshCore sender name / Meshtastic long name
    char     short_name[ID_SHORT_MAX + 1];
    char     node_id[12];  // "!aabbccdd", derived from the MAC, never edited
    uint32_t node_num;     // the same value the wire carries
    int      field;

    // The MeshCore identity. On that network the public key *is* the node id,
    // so this is what other nodes will know us by -- generated once and then
    // never changed, or every contact stops recognising us.
    uint8_t public_key[32];
    uint8_t private_key[64];  // derived from the stored seed, never persisted
    bool    has_keypair;

    // Meshtastic's end-to-end key is a *different* key pair on a different
    // curve, published in NodeInfo. It cannot be shared with the MeshCore one:
    // that is a signing key, this is an agreement key, and the two networks
    // derive their secrets differently even before that.
    uint8_t mt_public_key[32];
    uint8_t mt_private_key[32];
    bool    has_mt_keypair;
} identity_t;

// Transmitting without an identity would put an anonymous message on a public
// network: MeshCore carries the sender name inside the message text and has no
// other identity field, so an empty name is not merely unfriendly, it is
// unattributable. TX is refused until this is true.
static inline bool identity_is_set(const identity_t* identity) {
    return identity->name[0] != '\0';
}

typedef enum {
    RADIO_RX = 0,
    RADIO_TX,
    RADIO_ERROR,
} radio_state_t;

typedef struct {
    mesh_state_t mesh[MESH_COUNT];
    mesh_id_t    active;

    char composer[COMPOSER_MAX];
    int  composer_len;     // bytes, which is what the protocol limit counts
    int  composer_cursor;  // byte offset

    // Previously sent messages, recalled with ctrl+up / ctrl+down.
    char history[HISTORY_MAX][COMPOSER_MAX];
    int  history_count;
    int  history_pos;  // -1 = not browsing

    bool show_meta;

    overlay_t overlay;
    int       picker_index;
    editor_t   editor;
    identity_t identity;
    char       confirm_text[80];
    confirm_action_t confirm_action;

    // Nodes view. -1 on the "this radio" row that is pinned above the list.
    int      node_index;
    uint32_t last_advert_ms;  // manual announce cooldown
    char     node_short_edit[NODE_SHORT_MAX + 1];  // the short name being typed

    radio_state_t radio;
    int           battery_pct;
    bool          charging;
    bool          time_synced;

    char     toast[64];
    uint32_t toast_until_ms;
} app_model_t;

// Longest message each network can carry, in bytes of text.
//
// MeshCore's own limit is ten cipher blocks. Meshtastic's is the packet payload
// less protobuf overhead.
#define LIMIT_MC_BYTES 160
#define LIMIT_MT_BYTES 220

// Where a MeshCore message stops being reliably deliverable, which is well short
// of where it stops being legal. Airtime grows with length and every hop is
// another chance to lose the frame, so a long message across several hops fails
// far more often than a short one -- observed, not theorised. These colour the
// byte counter; they do not restrict anything.
#define MC_COMFORT_BYTES 48
#define MC_STRAIN_BYTES  96

static inline int model_byte_limit(const app_model_t* model) {
    if (model->active == MESH_MT) return LIMIT_MT_BYTES;

    // A MeshCore channel message carries the sender's name in the same payload,
    // as "Name: text", because the protocol has nowhere else to put it. That
    // prefix comes out of the same budget: without subtracting it, a full-length
    // message from anyone with a long name overflows the cipher buffer and is
    // refused at the point of sending with nothing to explain why.
    const mesh_state_t* mesh = &model->mesh[MESH_MC];
    if (mesh->target_contact) return LIMIT_MC_BYTES;  // a direct message has no prefix

    int prefix = 0;
    while (model->identity.name[prefix] != '\0') prefix++;
    prefix += 2;  // the ": " separator

    int room = LIMIT_MC_BYTES - prefix;
    return room > 0 ? room : 0;
}

// Zero the model and set the per-network constants. Channels and identity come
// from settings_load(), or from settings_apply_default_channels() on first run.
void model_init(app_model_t* model);

// Append to a network's ring. Increments `unseen` when a received message
// arrives while the user is scrolled away, so the viewport can stay put.
message_t* model_push(mesh_state_t* mesh, uint8_t channel, const char* sender, bool sender_named, const char* text,
                      bool outgoing);

// Ring access by position (0 = oldest held) or by sequence number. Both return
// NULL rather than a stale slot when the message has aged out.
const message_t* model_message_at(const mesh_state_t* mesh, int logical);
const message_t* model_message_by_seq(const mesh_state_t* mesh, int32_t seq);

// --- nodes ---------------------------------------------------------------

// Find or create the entry for a node, and stamp it as heard now. Both return
// NULL only when the table is full of entries newer than this one.
node_t* model_node_touch_mt(mesh_state_t* mesh, uint32_t node_num);
node_t* model_node_touch_mc(mesh_state_t* mesh, const uint8_t key[NODE_KEY_LEN]);

// Look up without creating or stamping. NULL when the node is not known.
node_t* model_node_find_mt(mesh_state_t* mesh, uint32_t node_num);
node_t* model_node_find_mc(mesh_state_t* mesh, const uint8_t key[NODE_KEY_LEN]);

// The contact the composer is aimed at, or NULL when it is aimed at a channel
// or the contact has since been pruned. Callers must handle NULL: a
// conversation can outlive the node entry behind it.
node_t* model_target_node(mesh_state_t* mesh, mesh_id_t id);

// Point the composer at a contact, or back at a channel.
void model_target_set_contact(mesh_state_t* mesh, mesh_id_t id, const node_t* node);
void model_target_set_channel(mesh_state_t* mesh, int channel);

// How the target should be labelled in the status bar and the message column.
// Falls back through name, short name and key or number, like the nodes list.
void model_node_label(const node_t* node, mesh_id_t id, char* out, size_t out_size);

// The short name set for whoever sent under `name`, or NULL if there is none.
//
// MeshCore channel messages carry no identity at all -- the sender's name is a
// string inside the text -- so the only way back to a node record is to match on
// that name. Two nodes calling themselves the same thing are indistinguishable
// here, which is a property of the network rather than of this lookup.
const char* model_short_name_for(const mesh_state_t* mesh, const char* name);

// Drop entries we have not heard from in a long time. Nodes that never told us
// their name expire far sooner: an unnamed entry is little more than evidence
// that something transmitted once, while a named one is a contact.
#define NODE_EXPIRY_UNNAMED_S (7 * 24 * 60 * 60)
#define NODE_EXPIRY_NAMED_S   (30 * 24 * 60 * 60)
int model_nodes_prune(mesh_state_t* mesh, uint32_t now);

void model_nodes_clear(mesh_state_t* mesh);
void model_node_remove(mesh_state_t* mesh, int index);

// Forget routes recorded at a narrower hop width than we now send with.
//
// A stored route carries its own width and works whatever we originate with, so
// a narrow one is not invalid -- only ambiguous, since a hop is that many bytes
// of a repeater's public key and fewer bytes means more repeaters answer to it.
// Narrower than we now use is worth discarding because flooding re-learns a
// wider one at the cost of a single message; wider than we now use is better
// than anything we could re-learn, and is kept.
//
// Returns how many were dropped.
int model_drop_narrow_routes(mesh_state_t* mesh, uint8_t min_hash_size);

// Indices into `nodes`, most recently heard first. Returns how many were
// written. Used by the list view, which never shows the raw array order.
int model_nodes_by_recency(const mesh_state_t* mesh, int* out, int max);

static inline mesh_state_t* model_active(app_model_t* model) {
    return &model->mesh[model->active];
}

static inline const channel_t* model_input_channel(app_model_t* model) {
    mesh_state_t* mesh = model_active(model);
    return &mesh->channels[mesh->input_channel];
}
