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
#define SENDER_MAX    16
#define TEXT_MAX      192
#define HISTORY_MAX   12

#define CH_DISPLAY_MAX 4  // characters, not counting the terminator
#define CH_NAME_MAX    23
#define CH_SECRET_MAX  47

#define ID_NAME_MAX  23
#define ID_SHORT_MAX 4

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
    uint32_t seq;        // monotonic; scroll and selection anchor on this
    uint8_t  channel;    // index into the owning mesh's channel list
    uint32_t timestamp;  // MC: sender's clock. MT: our receive clock.
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
    uint8_t path[8];      // MeshCore: one byte per hop
    uint8_t path_len;
    uint8_t hop_start;    // Meshtastic: hop limit as sent
    uint8_t hop_limit;    // Meshtastic: hops remaining on arrival
    char    relayed_by[8];  // short name or id of the last relay, "" if unknown

    bool       outgoing;
    tx_state_t tx;
    uint8_t    repeats;    // repeaters heard repeating this message
    bool       acked;      // explicit ack (DMs; not in this phase)
    uint32_t   tx_tick_ms;  // when the current tx state was entered
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

typedef struct {
    const char* name;
    pax_col_t   accent;  // status bar background for this mesh
    rx_stats_t  stats;

    channel_t channels[MAX_CHANNELS];
    int       channel_count;
    int       input_channel;  // where the composer sends

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
} overlay_t;

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
    editor_t  editor;
    identity_t identity;
    char      confirm_text[80];

    radio_state_t radio;
    int           battery_pct;
    bool          charging;
    bool          time_synced;

    char     toast[64];
    uint32_t toast_until_ms;
} app_model_t;

// Longest message each network can carry, in bytes of text. Approximate: the
// real figures depend on the sender name and header overhead.
#define LIMIT_MC_BYTES 160
#define LIMIT_MT_BYTES 220

static inline int model_byte_limit(const app_model_t* model) {
    return model->active == MESH_MT ? LIMIT_MT_BYTES : LIMIT_MC_BYTES;
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

static inline mesh_state_t* model_active(app_model_t* model) {
    return &model->mesh[model->active];
}

static inline const channel_t* model_input_channel(app_model_t* model) {
    mesh_state_t* mesh = model_active(model);
    return &mesh->channels[mesh->input_channel];
}
