// SPDX-License-Identifier: MIT
//
// MultiMesh: application entry point and event loop.
//
// The loop owns the model and is the only thing that mutates it. Everything that
// can block -- transmitting, above all -- reports in through a queue rather than
// reaching into state, so a send in flight cannot race a repaint.

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "app_model.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "bsp/rtc.h"
#include "crypto_jobs.h"
#include "driver/gpio.h"
#include "ed25519.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "leds.h"
#include "mesh_net.h"
#include "meshtastic_crypto.h"
#include "meshtastic_wire.h"
#include "nodestore.h"
#include "nvs_flash.h"
#include "radio.h"
#include "settings.h"
#include "ui.h"
#include "x25519.h"

static const char TAG[] = "main";

static app_model_t   model;
static QueueHandle_t input_event_queue = NULL;

// The networks, in the order the switch key cycles them.
static const mesh_net_t* const nets[MESH_COUNT] = {
    [MESH_MC] = &mesh_net_meshcore,
    [MESH_MT] = &mesh_net_meshtastic,
};

static uint32_t now_ms(void);
static void     toast(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// --- transmit ------------------------------------------------------------
//
// radio_send() blocks for the full airtime of the packet, so it runs on its own
// task. The model stays single-threaded: the worker never touches it, it only
// reports back through a queue that the event loop drains.

#define TX_QUEUE_DEPTH 4

typedef struct {
    mesh_id_t mesh;
    uint32_t  seq;  // the message this frame belongs to
    uint8_t   frame[256];
    uint8_t   length;
} tx_request_t;

typedef enum { TX_EVENT_SENDING, TX_EVENT_SENT, TX_EVENT_FAILED } tx_event_kind_t;

typedef struct {
    mesh_id_t       mesh;
    uint32_t        seq;
    tx_event_kind_t kind;
} tx_event_t;

static QueueHandle_t tx_requests;
static QueueHandle_t tx_events;

static void tx_worker(void* arg) {
    (void)arg;
    tx_request_t request;

    while (xQueueReceive(tx_requests, &request, portMAX_DELAY) == pdTRUE) {
        tx_event_t event = {.mesh = request.mesh, .seq = request.seq, .kind = TX_EVENT_SENDING};
        xQueueSend(tx_events, &event, 0);

        bool ok    = radio_send(request.frame, request.length);
        event.kind = ok ? TX_EVENT_SENT : TX_EVENT_FAILED;
        xQueueSend(tx_events, &event, portMAX_DELAY);
    }
}

// --- public-key work -----------------------------------------------------
//
// Verifying an advert or agreeing a conversation key costs about a second of
// arithmetic. It runs here, below the transmit worker, so neither drawing nor a
// queued send ever waits on it. Like the transmit worker it touches nothing but
// its own queues; results are applied to the model by the event loop.

static void crypto_worker(void* arg) {
    (void)arg;
    while (1) {
        crypto_run_one(1000);
    }
}

// Apply finished results. Returns true if anything changed on screen.
static bool drain_crypto(void) {
    bool            changed = false;
    crypto_result_t result;

    while (crypto_take_result(&result)) {
        mesh_id_t     id   = result.kind == CRYPTO_JOB_MT_SECRET ? MESH_MT : MESH_MC;
        mesh_state_t* mesh = &model.mesh[id];

        node_t* node = id == MESH_MT ? model_node_find_mt(mesh, result.node_num)
                                     : model_node_find_mc(mesh, result.pub_key);
        // The node may have been pruned or cleared while the job was queued.
        if (node == NULL) continue;

        switch (result.kind) {
            case CRYPTO_JOB_MC_VERIFY:
                node->verified = result.ok ? NODE_VERIFY_VALID : NODE_VERIFY_BAD;
                break;

            case CRYPTO_JOB_MC_SECRET:
            case CRYPTO_JOB_MT_SECRET:
                node->secret_pending = false;
                node->has_secret     = result.ok;
                if (result.ok) memcpy(node->shared_secret, result.secret, NODE_KEY_LEN);
                break;

            default: continue;
        }

        nodestore_mark_dirty(id);
        changed = true;
    }
    return changed;
}

// Put any acknowledgement a receive path built onto the transmit queue. Built
// there because only the stack knows what proves delivery on its network; queued
// here because only this loop owns the transmitter.
//
// Only the active network's, because there is one radio and it is tuned to one
// network. An acknowledgement for the other would go out on the wrong frequency
// -- interference rather than delivery. The inactive network's queue is drained
// and discarded instead: it can only hold something queued moments before a
// switch, and an acknowledgement that arrives after a detour through another
// band is too late to stop the sender retrying anyway.
static void forward_acks(void) {
    for (int i = 0; i < MESH_COUNT; i++) {
        if (nets[i]->take_pending_ack == NULL) continue;

        tx_request_t request = {.mesh = (mesh_id_t)i, .seq = UINT32_MAX};
        while (nets[i]->take_pending_ack(request.frame, sizeof(request.frame), &request.length)) {
            if ((mesh_id_t)i != model.active) continue;  // drained, not sent

            if (xQueueSend(tx_requests, &request, 0) != pdTRUE) {
                ESP_LOGW(TAG, "transmit queue full; acknowledgement dropped");
                return;
            }
        }
    }
}

// Locate a message by sequence number so a worker report can be applied to it.
static message_t* find_message(mesh_id_t id, uint32_t seq) {
    mesh_state_t* mesh = &model.mesh[id];
    for (int i = 0; i < mesh->count; i++) {
        message_t* msg = (message_t*)model_message_at(mesh, i);
        if (msg && msg->used && msg->seq == seq) return msg;
    }
    return NULL;
}

static bool drain_tx_events(void) {
    bool       changed = false;
    tx_event_t event;

    while (xQueueReceive(tx_events, &event, 0) == pdTRUE) {
        message_t* msg = find_message(event.mesh, event.seq);
        if (msg == NULL) continue;

        switch (event.kind) {
            case TX_EVENT_SENDING:
                msg->tx     = TX_SENDING;
                model.radio = RADIO_TX;
                break;
            case TX_EVENT_SENT:
                // On the air. Now wait to hear a repeater echo it back, which is
                // the only delivery signal a broadcast gets.
                msg->tx         = TX_AWAITING;
                msg->tx_tick_ms = now_ms();
                model.radio     = RADIO_RX;
                break;
            case TX_EVENT_FAILED:
                msg->tx     = TX_FAILED;
                model.radio = RADIO_RX;
                toast("send failed");
                break;
        }
        changed = true;
    }
    return changed;
}

// The window has to outlast a multi-hop repeat: one transmission is roughly half
// a second at SF8/BW62.5, plus each repeater's backoff. Too short and a quiet
// moment looks like a failure.
#define TX_REPEAT_WINDOW_MS 60000

static bool tx_settle(void) {
    bool changed = false;

    for (int m = 0; m < MESH_COUNT; m++) {
        mesh_state_t* mesh = &model.mesh[m];
        for (int i = 0; i < MAX_MESSAGES; i++) {
            message_t* msg = &mesh->messages[i];
            if (!msg->used || msg->tx != TX_AWAITING) continue;
            if (now_ms() - msg->tx_tick_ms < TX_REPEAT_WINDOW_MS) continue;

            // Heard repeated at least once means it reached the mesh. Silence
            // means nobody relayed it -- which is not proof it was unheard, only
            // that nothing confirmed it.
            msg->tx = msg->repeats > 0 ? TX_CONFIRMED : TX_FAILED;
            changed = true;
        }
    }
    return changed;
}

#define TOAST_MS 1800

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void toast(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(model.toast, sizeof(model.toast), fmt, args);
    va_end(args);
    model.toast_until_ms = now_ms() + TOAST_MS;
}

static int utf8_adv(unsigned char c) {
    return (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
}

// --- composer ------------------------------------------------------------

static void composer_set(const char* text) {
    snprintf(model.composer, sizeof(model.composer), "%s", text);
    model.composer_len    = (int)strlen(model.composer);
    model.composer_cursor = model.composer_len;
}

static void composer_append(const char* utf8) {
    int add = (int)strlen(utf8);
    if (add == 0) return;
    if (model.composer_len + add >= COMPOSER_MAX - 1) {
        toast("message full");
        return;
    }
    // Insert at the cursor rather than appending: left/right move within the
    // line, so the cursor is not always at the end.
    memmove(&model.composer[model.composer_cursor + add], &model.composer[model.composer_cursor],
            model.composer_len - model.composer_cursor + 1);
    memcpy(&model.composer[model.composer_cursor], utf8, add);
    model.composer_len    += add;
    model.composer_cursor += add;
}

static void composer_backspace(void) {
    if (model.composer_cursor == 0) return;
    // Step back over a whole UTF-8 sequence: ä is two bytes and must not be
    // half-deleted.
    int i = model.composer_cursor - 1;
    while (i > 0 && ((unsigned char)model.composer[i] & 0xC0) == 0x80) i--;
    int removed = model.composer_cursor - i;
    memmove(&model.composer[i], &model.composer[model.composer_cursor],
            model.composer_len - model.composer_cursor + 1);
    model.composer_len    -= removed;
    model.composer_cursor  = i;
}

static void composer_move(int dir) {
    if (dir < 0) {
        if (model.composer_cursor == 0) return;
        int i = model.composer_cursor - 1;
        while (i > 0 && ((unsigned char)model.composer[i] & 0xC0) == 0x80) i--;
        model.composer_cursor = i;
    } else {
        if (model.composer_cursor >= model.composer_len) return;
        model.composer_cursor += utf8_adv((unsigned char)model.composer[model.composer_cursor]);
        if (model.composer_cursor > model.composer_len) model.composer_cursor = model.composer_len;
    }
}

static void history_push(const char* text) {
    if (model.history_count == HISTORY_MAX) {
        memmove(&model.history[0], &model.history[1], sizeof(model.history[0]) * (HISTORY_MAX - 1));
        model.history_count--;
    }
    snprintf(model.history[model.history_count++], COMPOSER_MAX, "%s", text);
    model.history_pos = -1;
}

static void history_browse(int dir) {
    if (model.history_count == 0) return;

    if (model.history_pos < 0) {
        model.history_pos = dir < 0 ? model.history_count - 1 : -1;
    } else {
        model.history_pos += dir < 0 ? -1 : 1;
    }

    if (model.history_pos < 0) {
        model.history_pos = 0;
    } else if (model.history_pos >= model.history_count) {
        model.history_pos = -1;
        composer_set("");
        return;
    }
    composer_set(model.history[model.history_pos]);
}

// Recompute a channel's binary key and hash after its secret or name changed.
// Which is protocol knowledge, so the owning network does it.
static void prepare_channels(mesh_id_t id) {
    mesh_state_t* mesh = &model.mesh[id];
    for (int i = 0; i < mesh->channel_count; i++) {
        nets[id]->prepare_channel(&mesh->channels[i]);
        if (!mesh->channels[i].ready) {
            ESP_LOGW(TAG, "%s channel '%s' has an unusable key", nets[id]->name, mesh->channels[i].name);
        }
    }
}

static void open_identity(void) {
    model.identity.field = ID_FIELD_NAME;
    model.overlay        = OVERLAY_IDENTITY;
}

static void composer_send(void) {
    if (model.composer_len == 0) return;

    // Refuse to transmit anonymously. MeshCore carries the sender name inside
    // the message text and has no other identity field, so an empty name is not
    // merely unfriendly -- the message is unattributable on the network.
    if (!identity_is_set(&model.identity)) {
        toast("set a name before sending");
        open_identity();
        return;
    }
    if (model.composer_len > model_byte_limit(&model)) {
        toast("too long for this network");
        return;
    }

    if (!radio_is_ready()) {
        toast("radio unavailable");
        return;
    }

    mesh_state_t* mesh = model_active(&model);
    if (mesh->channel_count == 0) {
        toast("no channel to send on");
        return;
    }

    // A conversation, if one is selected. Checked before anything is pushed so a
    // contact that has since been pruned fails the send rather than quietly
    // broadcasting a private message to a channel.
    node_t* peer = NULL;
    if (mesh->target_contact) {
        peer = model_target_node(mesh, model.active);
        if (peer == NULL) {
            toast("that contact is gone - pick another target");
            return;
        }
        if (!peer->has_secret && model.active == MESH_MC) {
            toast(peer->secret_pending ? "still agreeing a key - try again shortly" : "no key for this contact yet");
            return;
        }
    }

    // The local echo goes in first so the message has a sequence number for the
    // encoder to attach repeats to, and so the user sees it immediately rather
    // than after the radio has finished.
    // Attribute the echo the way this network will: MeshCore shows the full
    // name it embeds in the text, Meshtastic the short name other nodes use.
    const char* sender = nets[model.active]->local_sender(&model.identity);
    message_t*  msg    = model_push(mesh, (uint8_t)mesh->input_channel, sender, true, model.composer, true);
    msg->tx        = TX_QUEUED;
    msg->tx_tick_ms = now_ms();
    if (peer) {
        msg->dm = true;
        model_node_label(peer, model.active, msg->peer, sizeof(msg->peer));
    }

    tx_request_t request = {.mesh = model.active, .seq = msg->seq};
    if (peer) {
        request.length = nets[model.active]->encode_dm(mesh, &model.identity, peer, model.composer, msg,
                                                       request.frame, sizeof(request.frame));
    } else {
        request.length = nets[model.active]->encode(mesh, (uint8_t)mesh->input_channel, &model.identity,
                                                    model.composer, msg->seq, request.frame, sizeof(request.frame));
    }
    if (request.length == 0) {
        msg->tx = TX_FAILED;
        toast("could not encode message");
        return;
    }

    if (xQueueSend(tx_requests, &request, 0) != pdTRUE) {
        msg->tx = TX_FAILED;
        toast("transmit queue full");
        return;
    }

    history_push(model.composer);
    composer_set("");

    mesh->pinned = true;  // jump back to live on send
    mesh->unseen = 0;
}

// --- power ---------------------------------------------------------------

// Reading the gauge is an I2C round trip to the coprocessor, so it is polled on
// an interval rather than every frame. Ten seconds is far finer than a battery
// moves.
#define POWER_POLL_MS 10000

static bool poll_power(void) {
    static uint32_t next_poll_ms = 0;
    if (now_ms() < next_poll_ms) return false;
    next_poll_ms = now_ms() + POWER_POLL_MS;

    bsp_power_battery_information_t info;
    if (bsp_power_get_battery_information(&info) != ESP_OK) return false;

    int  percent  = (int)(info.remaining_percentage + 0.5);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    bool changed = (percent != model.battery_pct) || (info.battery_charging != model.charging);
    model.battery_pct = percent;
    model.charging    = info.battery_charging;
    return changed;
}

// Announcing ourselves costs airtime on a duty-cycle limited band, so a manual
// send is rate limited. The automatic one is far rarer.
#define ADVERT_COOLDOWN_MS (5 * 60 * 1000)
#define ADVERT_INTERVAL_MS (24u * 60 * 60 * 1000)

static uint32_t next_auto_advert_ms;
static bool     send_announcement(bool manual);

// --- housekeeping --------------------------------------------------------

// Expire stale nodes, write the table if it changed, and announce ourselves on
// the slow timer. All rate-limited internally, so calling this every loop is
// cheap.
static bool housekeeping(void) {
    static uint32_t next_prune_ms = 0;
    bool            changed       = false;

    uint32_t now = (uint32_t)time(NULL);
    if (now_ms() >= next_prune_ms) {
        next_prune_ms = now_ms() + 60000;
        // Only prune once the clock is real: with an unset clock every node
        // would look ancient and be dropped.
        if (model.time_synced && now > 1000000000u) {
            for (int i = 0; i < MESH_COUNT; i++) {
                if (model_nodes_prune(&model.mesh[i], now) > 0) {
                    nodestore_mark_dirty((mesh_id_t)i);
                    changed = true;
                }
            }
        }
    }

    if (next_auto_advert_ms != 0 && now_ms() >= next_auto_advert_ms) {
        next_auto_advert_ms = now_ms() + ADVERT_INTERVAL_MS;
        send_announcement(false);
    }

    nodestore_flush(&model, false);
    return changed;
}

// --- navigation ----------------------------------------------------------

// Retune the radio to whichever network is now active. Anything already queued
// was received under the previous modem settings and belongs to the network we
// just left, so it is dropped rather than decoded against the wrong stack.
static void apply_active_net(void) {
    if (!radio_is_ready()) return;

    lora_protocol_config_params_t config;
    nets[model.active]->get_config(&config);

    radio_drain();
    if (radio_apply_config(&config)) {
        model.radio = RADIO_RX;
    } else {
        model.radio = RADIO_ERROR;
        toast("radio config failed");
    }
}

static void switch_mesh(void) {
    model.active = (model.active + 1) % MESH_COUNT;
    mesh_state_t* mesh = model_active(&model);
    apply_active_net();
    leds_set_mesh(mesh->accent);
    settings_save_prefs(&model);
    toast("%s", mesh->name);
}

// Drain whatever the radio has queued and hand each frame to the active
// network's decoder. Bounded per call so a burst cannot starve the UI.
#define RX_PER_ITERATION 8

static bool radio_poll(void) {
    bool changed = false;

    for (int i = 0; i < RX_PER_ITERATION; i++) {
        lora_protocol_lora_packet_t packet;
        if (!radio_receive(&packet, 0)) break;

        mesh_state_t* mesh = model_active(&model);
        bool          message = nets[model.active]->handle(&packet, mesh, &model.identity);
        // Any received packet updates a node's last-heard stamp.
        nodestore_mark_dirty(model.active);
        if (message) {
            // Blink the LED in the colour of the channel it arrived on.
            const message_t* newest = model_message_at(mesh, mesh->count - 1);
            if (newest && newest->channel < mesh->channel_count) {
                leds_notify_message(mesh->channels[newest->channel].color);
            }
        } else {
            // Heard something, but not a message for us: position, telemetry,
            // an advert, or a channel we hold no key for.
            leds_notify_activity();
        }
        changed = true;
    }
    return changed;
}

static void next_channel(int delta) {
    mesh_state_t* mesh = model_active(&model);
    if (mesh->channel_count == 0) return;

    // Cycling channels while aimed at a contact used to move `input_channel`
    // underneath an unchanged status chip -- so the next message still went to
    // the contact, on a channel the user had silently changed. Stepping the
    // channel means the channel is now the target.
    if (mesh->target_contact) {
        mesh->target_contact = false;
    } else {
        mesh->input_channel = (mesh->input_channel + delta + mesh->channel_count) % mesh->channel_count;
    }
    settings_save_prefs(&model);
    toast("sending to %s", mesh->channels[mesh->input_channel].name);
}

// Leave a conversation and aim at the channel again. Returns false when there
// was no conversation to leave, so the caller can fall through to whatever it
// would otherwise have done.
static bool leave_conversation(void) {
    mesh_state_t* mesh = model_active(&model);
    if (!mesh->target_contact) return false;

    mesh->target_contact = false;
    settings_save_prefs(&model);
    toast("sending to %s", mesh->channels[mesh->input_channel].name);
    return true;
}

static void scroll_by(int lines) {
    ui_set_anchor(&model, ui_anchor_index(&model) + lines);
}

static void jump_to_latest(void) {
    mesh_state_t* mesh = model_active(&model);
    mesh->pinned       = true;
    mesh->unseen       = 0;
}

// --- selection -----------------------------------------------------------

static int selected_logical(const mesh_state_t* mesh) {
    for (int i = 0; i < mesh->count; i++) {
        const message_t* m = model_message_at(mesh, i);
        if (m && m->used && (int32_t)m->seq == mesh->selected_seq) return i;
    }
    return -1;
}

static void selection_move(int dir) {
    mesh_state_t* mesh = model_active(&model);
    if (mesh->count == 0) return;

    if (mesh->selected_seq < 0) {
        if (dir > 0) return;  // alt+down with nothing selected does nothing
        mesh->selected_seq = (int32_t)model_message_at(mesh,mesh->count - 1)->seq;
        return;
    }

    int cur = selected_logical(mesh);
    if (cur < 0) {
        mesh->selected_seq = -1;
        return;
    }

    int next = cur + (dir > 0 ? 1 : -1);
    if (next < 0) return;              // already at the oldest
    if (next >= mesh->count) {
        mesh->selected_seq = -1;       // alt+down past the newest leaves the mode
        return;
    }
    mesh->selected_seq = (int32_t)model_message_at(mesh,next)->seq;

    // Scroll only as far as needed to bring the selection back into view.
    int line = ui_line_of_seq(&model, (uint32_t)mesh->selected_seq);
    if (line < 0) return;
    int anchor = ui_anchor_index(&model);
    int rows   = ui_visible_rows();
    if (line < anchor) {
        ui_set_anchor(&model, line);
    } else if (line >= anchor + rows) {
        ui_set_anchor(&model, line - rows + 1);
    }
}

// --- channel editor ------------------------------------------------------

static void editor_open(bool creating, int index) {
    mesh_state_t* mesh = model_active(&model);
    editor_t*     ed   = &model.editor;

    memset(ed, 0, sizeof(*ed));
    ed->creating = creating;
    ed->index    = index;
    ed->field    = FIELD_NAME;

    if (creating) {
        // Default to the first palette colour not already in use, so a new
        // channel does not silently collide with an existing one.
        for (int c = 0; c < CH_PALETTE_SIZE; c++) {
            bool taken = false;
            for (int i = 0; i < mesh->channel_count; i++) {
                if (mesh->channels[i].color == ch_palette[c]) taken = true;
            }
            if (!taken) {
                ed->color = c;
                break;
            }
        }
    } else {
        const channel_t* ch = &mesh->channels[index];
        snprintf(ed->name, sizeof(ed->name), "%s", ch->name);
        snprintf(ed->display, sizeof(ed->display), "%s", ch->display);
        snprintf(ed->secret, sizeof(ed->secret), "%s", ch->secret);
        for (int c = 0; c < CH_PALETTE_SIZE; c++) {
            if (ch_palette[c] == ch->color) ed->color = c;
        }
    }
    model.overlay = OVERLAY_EDITOR;
}

static void editor_save(void) {
    mesh_state_t* mesh = model_active(&model);
    editor_t*     ed   = &model.editor;

    if (ed->name[0] == '\0') {
        toast("name required");
        return;
    }

    int index = ed->index;
    if (ed->creating) {
        if (mesh->channel_count >= MAX_CHANNELS) {
            toast("channel list full");
            return;
        }
        index = mesh->channel_count++;
    }

    channel_t* ch = &mesh->channels[index];
    snprintf(ch->name, sizeof(ch->name), "%s", ed->name);
    // Fall back to the leading characters of the name when no abbreviation was
    // given, rather than leaving the column blank. The truncation is deliberate.
    snprintf(ch->display, sizeof(ch->display), "%.*s", CH_DISPLAY_MAX, ed->display[0] ? ed->display : ed->name);
    snprintf(ch->secret, sizeof(ch->secret), "%s", ed->secret);
    ch->color = ch_palette[ed->color];

    // The key and hash are derived, so they must be recomputed here: on
    // Meshtastic the hash mixes the channel name, meaning a rename alone changes
    // which traffic this channel matches.
    nets[model.active]->prepare_channel(ch);
    settings_save_channels(&model, model.active);

    model.overlay = OVERLAY_PICKER;
    if (!ch->ready) {
        toast("saved, but the key is unusable");
    } else {
        toast("saved %s", ch->name);
    }
}

static void editor_delete_confirmed(void) {
    mesh_state_t* mesh = model_active(&model);
    int           idx  = model.editor.index;

    char gone[CH_NAME_MAX + 1];
    snprintf(gone, sizeof(gone), "%s", mesh->channels[idx].name);

    for (int i = idx; i < mesh->channel_count - 1; i++) {
        mesh->channels[i] = mesh->channels[i + 1];
    }
    mesh->channel_count--;

    // Messages carry a channel index, so they have to follow the shift or they
    // would render under the wrong name and colour.
    for (int i = 0; i < MAX_MESSAGES; i++) {
        message_t* msg = &mesh->messages[i];
        if (!msg->used) continue;
        if (msg->channel == idx) {
            msg->channel = 0;
        } else if (msg->channel > idx) {
            msg->channel--;
        }
    }

    if (mesh->input_channel >= mesh->channel_count) mesh->input_channel = mesh->channel_count - 1;
    model.picker_index = 0;
    model.overlay      = OVERLAY_PICKER;
    settings_save_channels(&model, model.active);
    settings_save_prefs(&model);
    toast("deleted %s", gone);
}

static char* editor_focused_field(int* out_max) {
    editor_t* ed = &model.editor;
    switch (ed->field) {
        case FIELD_NAME: *out_max = CH_NAME_MAX; return ed->name;
        case FIELD_DISPLAY: *out_max = CH_DISPLAY_MAX; return ed->display;
        case FIELD_SECRET: *out_max = CH_SECRET_MAX; return ed->secret;
        default: *out_max = 0; return NULL;
    }
}

static char* identity_focused_field(int* out_max) {
    switch (model.identity.field) {
        case ID_FIELD_NAME: *out_max = ID_NAME_MAX; return model.identity.name;
        case ID_FIELD_SHORT: *out_max = ID_SHORT_MAX; return model.identity.short_name;
        default: *out_max = 0; return NULL;
    }
}

static void field_append(char* field, int max, const char* utf8) {
    if (!field) return;
    int len = (int)strlen(field);
    int add = (int)strlen(utf8);
    if (len + add > max) {
        toast("field full");
        return;
    }
    memcpy(&field[len], utf8, add + 1);
}

static void field_backspace(char* field) {
    if (!field) return;
    int i = (int)strlen(field);
    if (i == 0) return;
    i--;
    while (i > 0 && ((unsigned char)field[i] & 0xC0) == 0x80) i--;
    field[i] = '\0';
}

// --- overlay key handling ------------------------------------------------

static void handle_editor_key(bsp_input_navigation_key_t key) {
    editor_t* ed  = &model.editor;
    int       max = 0;

    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_UP: ed->field = (ed->field - 1 + FIELD_COUNT) % FIELD_COUNT; break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
        case BSP_INPUT_NAVIGATION_KEY_TAB: ed->field = (ed->field + 1) % FIELD_COUNT; break;
        case BSP_INPUT_NAVIGATION_KEY_LEFT:
            if (ed->field == FIELD_COLOR) ed->color = (ed->color - 1 + CH_PALETTE_SIZE) % CH_PALETTE_SIZE;
            break;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT:
            if (ed->field == FIELD_COLOR) ed->color = (ed->color + 1) % CH_PALETTE_SIZE;
            break;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: field_backspace(editor_focused_field(&max)); break;
        case BSP_INPUT_NAVIGATION_KEY_SPACE_L:
        case BSP_INPUT_NAVIGATION_KEY_SPACE_M:
        case BSP_INPUT_NAVIGATION_KEY_SPACE_R: field_append(editor_focused_field(&max), max, " "); break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN: editor_save(); break;
        case BSP_INPUT_NAVIGATION_KEY_F3:
            if (!ed->creating) {
                mesh_state_t* mesh = model_active(&model);
                if (mesh->channel_count <= 1) {
                    toast("cannot delete the last channel");
                } else {
                    model.confirm_action = CONFIRM_DELETE_CHANNEL;
                    snprintf(model.confirm_text, sizeof(model.confirm_text), "Delete %s?",
                             mesh->channels[ed->index].name);
                    model.overlay = OVERLAY_CONFIRM;
                }
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
        case BSP_INPUT_NAVIGATION_KEY_F1: model.overlay = OVERLAY_PICKER; break;
        default: break;
    }
}

static void handle_identity_key(bsp_input_navigation_key_t key) {
    int max = 0;
    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            model.identity.field = (model.identity.field - 1 + ID_FIELD_COUNT) % ID_FIELD_COUNT;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
        case BSP_INPUT_NAVIGATION_KEY_TAB:
            model.identity.field = (model.identity.field + 1) % ID_FIELD_COUNT;
            break;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: field_backspace(identity_focused_field(&max)); break;
        case BSP_INPUT_NAVIGATION_KEY_SPACE_L:
        case BSP_INPUT_NAVIGATION_KEY_SPACE_M:
        case BSP_INPUT_NAVIGATION_KEY_SPACE_R: field_append(identity_focused_field(&max), max, " "); break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            if (!identity_is_set(&model.identity)) {
                toast("a name is required");
                break;
            }
            // A blank short name is filled from the long one rather than
            // rejected: Meshtastic needs four characters and the user should not
            // have to invent them twice.
            if (model.identity.short_name[0] == '\0') {
                snprintf(model.identity.short_name, sizeof(model.identity.short_name), "%.*s", ID_SHORT_MAX,
                         model.identity.name);
            }
            settings_save_identity(&model);
            model.overlay = OVERLAY_NONE;
            toast("identity saved");
            break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
        case BSP_INPUT_NAVIGATION_KEY_F1:
            // Escaping an unset identity would leave the app unable to send with
            // no obvious reason why, so reload whatever was stored and let the
            // user discover it again from the composer.
            model.overlay = OVERLAY_NONE;
            break;
        default: break;
    }
}

// --- nodes ---------------------------------------------------------------

// Build and queue our own announcement for the active network: a NodeInfo on
// Meshtastic, a signed advert on MeshCore. Each stack knows how to construct
// its own; all that happens here is the queueing.
static bool send_announcement(bool manual) {
    if (!radio_is_ready()) {
        if (manual) toast("radio unavailable");
        return false;
    }
    if (!identity_is_set(&model.identity)) {
        if (manual) toast("set a name first");
        return false;
    }
    if (model.active == MESH_MC && !model.identity.has_keypair) {
        if (manual) toast("no signing key - cannot advertise");
        return false;
    }

    mesh_state_t* mesh = model_active(&model);
    if (mesh->channel_count == 0) return false;
    const channel_t* ch = &mesh->channels[mesh->input_channel];
    if (!ch->ready) return false;

    tx_request_t request = {.mesh = model.active, .seq = UINT32_MAX};  // no message row to track
    request.length       = nets[model.active]->encode_advert(mesh, (uint8_t)mesh->input_channel, &model.identity,
                                                             request.frame, sizeof(request.frame));
    if (request.length == 0) {
        if (manual) toast("could not build announcement");
        return false;
    }

    if (xQueueSend(tx_requests, &request, 0) != pdTRUE) {
        if (manual) toast("transmit queue full");
        return false;
    }

    model.last_advert_ms = now_ms();
    if (manual) toast("announced");
    return true;
}

static void handle_nodes_key(bsp_input_navigation_key_t key) {
    mesh_state_t* mesh = model_active(&model);
    int           order[MAX_NODES];
    int           count = model_nodes_by_recency(mesh, order, MAX_NODES);

    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            if (model.node_index > -1) model.node_index--;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (model.node_index < count - 1) model.node_index++;
            break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            // The pinned "this radio" row has no node record behind it.
            if (model.node_index >= 0 && model.node_index < count) model.overlay = OVERLAY_NODE_DETAIL;
            break;
        case BSP_INPUT_NAVIGATION_KEY_F2:  // announce
            if (now_ms() - model.last_advert_ms < ADVERT_COOLDOWN_MS && model.last_advert_ms != 0) {
                uint32_t left = (ADVERT_COOLDOWN_MS - (now_ms() - model.last_advert_ms)) / 1000;
                toast("wait %lus before announcing again", (unsigned long)left);
            } else {
                send_announcement(true);
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_F3:  // clear all
            if (count == 0) {
                toast("no nodes to clear");
            } else {
                snprintf(model.confirm_text, sizeof(model.confirm_text), "Forget all %d %s nodes?", count,
                         mesh->name);
                model.confirm_action = CONFIRM_CLEAR_NODES;
                model.overlay        = OVERLAY_CONFIRM;
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
        case BSP_INPUT_NAVIGATION_KEY_F1:
        case BSP_INPUT_NAVIGATION_KEY_F4: model.overlay = OVERLAY_NONE; break;
        default: break;
    }
}

static void handle_node_detail_key(bsp_input_navigation_key_t key) {
    mesh_state_t* mesh = model_active(&model);
    int           order[MAX_NODES];
    int           count = model_nodes_by_recency(mesh, order, MAX_NODES);

    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_F2:  // start a conversation with this node
            if (model.node_index >= 0 && model.node_index < count) {
                node_t* node = &mesh->nodes[order[model.node_index]];
                model_target_set_contact(mesh, model.active, node);
                model.overlay = OVERLAY_NONE;

                char label[NODE_NAME_MAX + 1];
                model_node_label(node, model.active, label, sizeof(label));
                if (node->has_secret) {
                    toast("messaging %s", label);
                } else if (model.active == MESH_MT) {
                    toast("%s: no key, channel-encrypted only", label);
                } else {
                    toast("%s: agreeing a key...", label);
                }
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_F3:  // remove this node
            if (model.node_index >= 0 && model.node_index < count) {
                model_node_remove(mesh, order[model.node_index]);
                nodestore_mark_dirty(model.active);
                if (model.node_index >= count - 1) model.node_index = count - 2;
                model.overlay = OVERLAY_NODES;
                toast("node removed");
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
        case BSP_INPUT_NAVIGATION_KEY_F1:
        case BSP_INPUT_NAVIGATION_KEY_RETURN: model.overlay = OVERLAY_NODES; break;
        default: break;
    }
}

// The picker is one list: channels first, then contacts. An index past the
// channel count is a contact, which is what makes "send to" a single choice
// rather than two settings that can disagree.
static void picker_choose(void) {
    mesh_state_t* mesh = model_active(&model);

    if (model.picker_index < mesh->channel_count) {
        model_target_set_channel(mesh, model.picker_index);
        model.overlay = OVERLAY_NONE;
        settings_save_prefs(&model);
        toast("sending to %s", mesh->channels[mesh->input_channel].name);
        return;
    }

    int order[MAX_NODES];
    int contacts = model_nodes_by_recency(mesh, order, MAX_NODES);
    int index    = model.picker_index - mesh->channel_count;
    if (index < 0 || index >= contacts) return;

    node_t* node = &mesh->nodes[order[index]];
    model_target_set_contact(mesh, model.active, node);
    model.overlay = OVERLAY_NONE;
    settings_save_prefs(&model);

    char label[NODE_NAME_MAX + 1];
    model_node_label(node, model.active, label, sizeof(label));
    if (node->has_secret) {
        toast("messaging %s", label);
    } else if (model.active == MESH_MT) {
        // Worth saying out loud: the message will go out under the channel key,
        // so everyone on the channel can read it.
        toast("%s: no key, channel-encrypted only", label);
    } else {
        toast("%s: agreeing a key...", label);
    }
}

static void handle_picker_key(bsp_input_navigation_key_t key) {
    mesh_state_t* mesh  = model_active(&model);
    int           count = ui_picker_count(&model);
    if (count < 1) count = 1;

    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            model.picker_index = (model.picker_index - 1 + count) % count;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            model.picker_index = (model.picker_index + 1) % count;
            break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN: picker_choose(); break;
        case BSP_INPUT_NAVIGATION_KEY_F2: editor_open(true, -1); break;
        case BSP_INPUT_NAVIGATION_KEY_F3:
            // Only channels are editable; a contact is something we heard, not
            // something configured.
            if (model.picker_index < mesh->channel_count) {
                editor_open(false, model.picker_index);
            } else {
                toast("contacts are edited from the nodes view");
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
        case BSP_INPUT_NAVIGATION_KEY_F1:
        case BSP_INPUT_NAVIGATION_KEY_F6: model.overlay = OVERLAY_NONE; break;
        default: break;
    }
}

static void handle_navigation(bsp_input_navigation_key_t key, uint32_t modifiers) {
    bool ctrl = (modifiers & BSP_INPUT_MODIFIER_CTRL) != 0;
    bool alt  = (modifiers & BSP_INPUT_MODIFIER_ALT) != 0;
    bool fn   = (modifiers & BSP_INPUT_MODIFIER_FUNCTION) != 0;

    switch (model.overlay) {
        case OVERLAY_EDITOR: handle_editor_key(key); return;
        case OVERLAY_IDENTITY: handle_identity_key(key); return;
        case OVERLAY_PICKER: handle_picker_key(key); return;
        case OVERLAY_NODES: handle_nodes_key(key); return;
        case OVERLAY_NODE_DETAIL: handle_node_detail_key(key); return;
        case OVERLAY_DETAIL:
            if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_F1 ||
                key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                model.overlay = OVERLAY_NONE;
            }
            return;
        case OVERLAY_CONFIRM:
            // Where cancelling returns to depends on what asked for the
            // confirmation, so the action carries that too.
            if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                if (model.confirm_action == CONFIRM_CLEAR_NODES) {
                    model_nodes_clear(model_active(&model));
                    nodestore_mark_dirty(model.active);
                    model.node_index = -1;
                    model.overlay    = OVERLAY_NODES;
                    toast("nodes cleared");
                } else {
                    editor_delete_confirmed();
                }
                model.confirm_action = CONFIRM_NONE;
            } else if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_F1) {
                model.overlay = model.confirm_action == CONFIRM_CLEAR_NODES ? OVERLAY_NODES : OVERLAY_EDITOR;
                model.confirm_action = CONFIRM_NONE;
            }
            return;
        default: break;
    }

    mesh_state_t* mesh = model_active(&model);

    // Selection mode owns the arrow keys it uses, but leaves the rest alone.
    if (alt && key == BSP_INPUT_NAVIGATION_KEY_UP) {
        selection_move(-1);
        return;
    }
    if (alt && key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        selection_move(1);
        return;
    }
    if (mesh->selected_seq >= 0) {
        if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
            model.overlay = OVERLAY_DETAIL;
            return;
        }
        if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_F1) {
            mesh->selected_seq = -1;
            return;
        }
    }

    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_F1:  // red cross
            // Escalating escape: clear what you typed, then leave the
            // conversation, then leave the app. Each step is the smallest thing
            // the key could plausibly mean at that moment, and the shortcut bar
            // relabels itself so it is never a guess.
            if (model.composer_len > 0) {
                composer_set("");
                toast("cleared");
            } else if (leave_conversation()) {
                // done
            } else {
                ESP_LOGI(TAG, "exit to launcher");
                bsp_device_restart_to_launcher();
            }
            break;

        case BSP_INPUT_NAVIGATION_KEY_F2: switch_mesh(); break;
        case BSP_INPUT_NAVIGATION_KEY_F3:
            model.show_meta = !model.show_meta;
            settings_save_prefs(&model);
            break;
        case BSP_INPUT_NAVIGATION_KEY_F4:
            model.overlay    = OVERLAY_NODES;
            model.node_index = -1;
            break;
        case BSP_INPUT_NAVIGATION_KEY_F5: open_identity(); break;
        case BSP_INPUT_NAVIGATION_KEY_F6:
            model.overlay = OVERLAY_PICKER;
            // Open on whatever is currently selected, contact rows included, so
            // the list opens where the user left it rather than at the top.
            model.picker_index = mesh->input_channel;
            if (mesh->target_contact) {
                int order[MAX_NODES];
                int contacts = model_nodes_by_recency(mesh, order, MAX_NODES);
                for (int i = 0; i < contacts; i++) {
                    node_t* node = &mesh->nodes[order[i]];
                    bool    same = model.active == MESH_MT ? node->node_num == mesh->target_num
                                                           : memcmp(node->key, mesh->target_key, NODE_KEY_LEN) == 0;
                    if (same) {
                        model.picker_index = mesh->channel_count + i;
                        break;
                    }
                }
            }
            break;

        case BSP_INPUT_NAVIGATION_KEY_TAB: next_channel(1); break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN: composer_send(); break;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: composer_backspace(); break;
        case BSP_INPUT_NAVIGATION_KEY_ESC: composer_set(""); break;

        case BSP_INPUT_NAVIGATION_KEY_LEFT: composer_move(-1); break;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT: composer_move(1); break;

        case BSP_INPUT_NAVIGATION_KEY_UP:
            if (ctrl) {
                history_browse(-1);
            } else if (fn) {
                scroll_by(-ui_visible_rows());
            } else {
                scroll_by(-1);
            }
            break;

        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (ctrl) {
                history_browse(1);
            } else if (fn) {
                jump_to_latest();
            } else {
                scroll_by(1);
            }
            break;

        case BSP_INPUT_NAVIGATION_KEY_SPACE_L:
        case BSP_INPUT_NAVIGATION_KEY_SPACE_M:
        case BSP_INPUT_NAVIGATION_KEY_SPACE_R: composer_append(" "); break;

        default: break;
    }
}

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", res);
        return;
    }

    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %d", res);
        return;
    }

    if (!ui_init()) {
        ESP_LOGE(TAG, "no display, nothing to prototype");
        return;
    }
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();

    // No radio in this build, so no coprocessor RTC read. Seed a plausible wall
    // clock so the timestamp column shows something meaningful.
    struct timeval tv = {.tv_sec = 1785000000, .tv_usec = 0};
    settimeofday(&tv, NULL);

    model_init(&model);
    settings_derive_node_id(&model.identity);

    settings_init();
    settings_load(&model);
    // Defaults fill only what storage did not: the well-known public channel on
    // MeshCore and EdgeFastLow on Meshtastic, so a fresh device is on the air
    // without configuring anything. A user who deletes them keeps them deleted,
    // because their own channels will already have been stored.
    settings_apply_default_channels(&model);

    if (!crypto_jobs_init()) ESP_LOGE(TAG, "public-key work will not run");

    for (int i = 0; i < MESH_COUNT; i++) {
        if (!nets[i]->init()) ESP_LOGE(TAG, "%s stack init failed", nets[i]->name);
        prepare_channels((mesh_id_t)i);
    }

    // Prove the signature code before anything relies on it. A wrong Ed25519
    // produces signatures that look fine locally and are rejected by every
    // peer, which is close to undiagnosable over the air.
    // Key agreement is checked first because it is cheap; the signature test
    // below is the slow one.
    if (x25519_selftest() && mt_pki_selftest()) {
        ESP_LOGI(TAG, "key agreement self-test passed");
    } else {
        ESP_LOGE(TAG, "KEY AGREEMENT SELF-TEST FAILED - direct messages disabled");
        ui_boot_line("key agreement self-test FAILED");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    // A malformed NodeInfo transmits perfectly well and simply never appears on
    // the other node, so this is checked here rather than discovered on air.
    if (!mt_wire_selftest()) {
        ESP_LOGE(TAG, "NODEINFO ENCODING SELF-TEST FAILED");
        ui_boot_line("nodeinfo encoding self-test FAILED");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ui_boot_line("Checking signatures...");
    if (ed25519_selftest()) {
        ESP_LOGI(TAG, "ed25519 self-test passed");

        // Only now is it safe to make a key. A MeshCore identity is permanent --
        // contacts remember the public key, not the name -- so deriving one from
        // arithmetic we have not proved would burn it.
        ui_boot_line("Loading identity...");
        if (!settings_load_identity_keypair(&model.identity, ed25519_keypair)) {
            ESP_LOGE(TAG, "no MeshCore identity; adverts disabled");
        }
        // Meshtastic's end-to-end key is a separate pair on a separate curve.
        // Cheap by comparison, and independent: failing to load one does not
        // stop the other network working.
        settings_load_mt_keypair(&model.identity, x25519_keypair, x25519_public_from_private);
    } else {
        ESP_LOGE(TAG, "ed25519 SELF-TEST FAILED - signing disabled");
        ui_boot_line("ed25519 self-test FAILED");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    if (nodestore_init()) {
        nodestore_load(&model);
    } else {
        ESP_LOGW(TAG, "nodes will not persist this session");
    }

    leds_init();
    leds_set_mesh(model_active(&model)->accent);

    tx_requests = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_request_t));
    tx_events   = xQueueCreate(TX_QUEUE_DEPTH * 3, sizeof(tx_event_t));
    if (tx_requests == NULL || tx_events == NULL) {
        ESP_LOGE(TAG, "transmit queues could not be allocated");
        return;
    }

    ui_boot_line("Starting radio...");
    if (radio_start()) {
        // The P4 has no RTC; the clock lives on the C6 and the link has to be up
        // before it can be read. Without this every timestamp renders as 1970.
        model.time_synced = (bsp_rtc_update_time() == ESP_OK);
        if (!model.time_synced) ESP_LOGW(TAG, "clock not available from the coprocessor");
        apply_active_net();

        // Priority below the UI loop: a blocked transmit must never delay input
        // handling or drawing.
        next_auto_advert_ms = now_ms() + 60000;
        if (xTaskCreate(tx_worker, "mm_tx", 4096, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "transmit worker could not be started");
        }
        // Lower still, and a generous stack: the big-integer arithmetic behind
        // Ed25519 is not frugal with it.
        if (xTaskCreate(crypto_worker, "mm_crypto", 8192, NULL, 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "verification worker could not be started");
        }
    } else {
        // Still usable: channels and identity can be configured with no radio.
        model.radio = RADIO_ERROR;
        ESP_LOGE(TAG, "radio unavailable");
    }

    // Nothing can be sent without a name, so ask for one immediately rather than
    // letting the user discover it when their first message is refused.
    if (!identity_is_set(&model.identity)) {
        open_identity();
        toast("welcome - set a name to start");
    } else if (model.radio == RADIO_ERROR) {
        toast("radio unavailable");
    }

    ui_render(&model);

    while (1) {
        bsp_input_event_t event;
        bool              dirty = false;

        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(120)) == pdTRUE) {
            // Any interaction means the user is looking, so the unread blink
            // has done its job.
            leds_clear_unread();

            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION:
                    if (event.args_navigation.state) {
                        handle_navigation(event.args_navigation.key, event.args_navigation.modifiers);
                        dirty = true;
                    }
                    break;

                case INPUT_EVENT_TYPE_KEYBOARD:
                    // utf8 carries the AltGr layer, which is where ä å ö live.
                    if (event.args_keyboard.utf8[0] && (unsigned char)event.args_keyboard.utf8[0] >= 0x20) {
                        int max = 0;
                        switch (model.overlay) {
                            case OVERLAY_EDITOR:
                                field_append(editor_focused_field(&max), max, event.args_keyboard.utf8);
                                break;
                            case OVERLAY_IDENTITY:
                                field_append(identity_focused_field(&max), max, event.args_keyboard.utf8);
                                break;
                            case OVERLAY_NONE:
                                if (model_active(&model)->selected_seq < 0) {
                                    composer_append(event.args_keyboard.utf8);
                                }
                                break;
                            default: break;
                        }
                        dirty = true;
                    }
                    break;

                default: break;
            }
        }

        if (radio_poll()) dirty = true;
        if (drain_tx_events()) dirty = true;
        if (drain_crypto()) dirty = true;
        forward_acks();
        if (poll_power()) dirty = true;
        if (housekeeping()) dirty = true;
        if (tx_settle()) dirty = true;
        leds_tick();

        if (model.toast[0] && (int32_t)(now_ms() - model.toast_until_ms) >= 0) {
            model.toast[0] = '\0';
            dirty          = true;
        }

        // The clock in the status bar needs a repaint even when idle.
        static uint32_t last_paint = 0;
        if (dirty || now_ms() - last_paint > 1000) {
            last_paint = now_ms();
            ui_render(&model);
        }
    }
}
