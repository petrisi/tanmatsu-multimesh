// SPDX-License-Identifier: MIT
//
// MeshComms — UI prototype.
//
// No radio and no protocol: this build exists to settle layout, colour and key
// handling on real hardware before any of that is wired back in. The verified
// receive stacks from the previous milestone are still in the tree
// (meshcore_*.c, meshtastic_*.c) but are excluded from this build in
// main/CMakeLists.txt -- their adapters target the old UI API.
//
// Everything on screen is mock data. Sent messages walk through the real
// delivery states on a timer so the timestamp-column indicator can be judged.

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "app_model.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "leds.h"
#include "nvs_flash.h"
#include "ui.h"

static const char TAG[] = "main";

static app_model_t   model;
static QueueHandle_t input_event_queue = NULL;

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

static void composer_send(void) {
    if (model.composer_len == 0) return;
    if (model.composer_len > model_byte_limit(&model)) {
        toast("too long for this network");
        return;
    }

    mesh_state_t* mesh = model_active(&model);
    message_t*    msg  = model_push(mesh, (uint8_t)mesh->input_channel, model.identity.name, true, model.composer,
                                    true);
    msg->tx         = TX_QUEUED;
    msg->tx_tick_ms = now_ms();

    history_push(model.composer);
    composer_set("");

    mesh->pinned = true;  // jump back to live on send
    mesh->unseen = 0;
}

// --- delivery state machine (mock) ---------------------------------------
//
// Stands in for what the radio and the repeat/ack watcher will drive. A send
// spends a moment queued, half a second on air, then waits to hear itself
// repeated. Every third message hears nothing and settles red.
//
// The window has to outlast a multi-hop repeat: at SF8/BW62.5 one transmission
// is roughly half a second, plus each repeater's backoff. Too short and quiet
// moments look like failures.
#define TX_REPEAT_WINDOW_MS 60000

// Returns true when something changed and the screen needs repainting.
static bool tx_tick(void) {
    bool changed = false;
    for (int m = 0; m < MESH_COUNT; m++) {
        mesh_state_t* mesh = &model.mesh[m];
        for (int i = 0; i < MAX_MESSAGES; i++) {
            message_t* msg = &mesh->messages[i];
            if (!msg->used || !msg->outgoing) continue;

            uint32_t age = now_ms() - msg->tx_tick_ms;
            switch (msg->tx) {
                case TX_QUEUED:
                    if (age > 400) {
                        msg->tx         = TX_SENDING;
                        msg->tx_tick_ms = now_ms();
                        model.radio     = RADIO_TX;
                        changed         = true;
                    }
                    break;
                case TX_SENDING:
                    if (age > 600) {
                        msg->tx         = TX_AWAITING;
                        msg->tx_tick_ms = now_ms();
                        model.radio     = RADIO_RX;
                        changed         = true;
                    }
                    break;
                case TX_AWAITING:
                    // Repeats trickle in, then the window closes.
                    if (msg->seq % 3 != 2 && age > 1500 && msg->repeats < 1 + msg->seq % 3) {
                        msg->repeats++;
                        msg->tx_tick_ms = now_ms();
                        changed         = true;
                    } else if (age > TX_REPEAT_WINDOW_MS) {
                        msg->tx = msg->repeats ? TX_CONFIRMED : TX_FAILED;
                        if (msg->tx == TX_FAILED) toast("no repeat heard");
                        changed = true;
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return changed;
}

// --- mock incoming traffic ----------------------------------------------
//
// Stands in for the radio so the behaviours that only appear with live traffic
// can be judged: the viewport holding still while scrolled away, the unseen
// counter, and the message LED blinking until someone looks.

#define INCOMING_EVERY_MS 12000
#define ACTIVITY_EVERY_MS 4500

static uint32_t next_incoming_ms;
static uint32_t next_activity_ms;
static int      incoming_index;

static const struct {
    const char* sender;
    bool        named;
    const char* text;
} incoming[] = {
    {"OH6ABC", true, "kuuluuko siellä vielä?"},
    {"elk1", true, "sää selkenee illalla"},
    {"c3d4", false, "uusi solmu näkyvissä"},
    {"Vuores", true, "lähdössä ulos, testataan matkalla kuuluvuutta pidemmällä viestillä"},
    {"owl7", true, "kuittaan"},
};

static bool incoming_tick(void) {
    uint32_t t = now_ms();
    bool     changed = false;

    if (t >= next_activity_ms) {
        next_activity_ms = t + ACTIVITY_EVERY_MS;
        leds_notify_activity();  // position/telemetry/foreign channel: LED only
    }

    if (t < next_incoming_ms) return false;
    next_incoming_ms = t + INCOMING_EVERY_MS;

    mesh_state_t* mesh = model_active(&model);
    int           i    = incoming_index++ % (int)(sizeof(incoming) / sizeof(incoming[0]));
    uint8_t       ch   = (uint8_t)(incoming_index % mesh->channel_count);

    message_t* msg = model_push(mesh, ch, incoming[i].sender, incoming[i].named, incoming[i].text, false);
    msg->rssi_dbm  = -70 - (incoming_index * 7) % 30;
    msg->snr_db_x4 = 12 + (incoming_index * 5) % 32;
    msg->hops      = (uint8_t)(incoming_index % 3);
    msg->path_len  = msg->hops;
    for (int p = 0; p < msg->path_len; p++) msg->path[p] = (uint8_t)(0x3b + p * 0x29);
    msg->hop_start = 3;
    msg->hop_limit = (uint8_t)(3 - msg->hops);
    if (msg->hops) snprintf(msg->relayed_by, sizeof(msg->relayed_by), "owl7");

    // Scrolled away: the view must not move, but the arrival is counted.
    if (!mesh->pinned) mesh->unseen++;
    leds_notify_message(mesh->channels[ch].color);
    changed = true;
    return changed;
}

// --- navigation ----------------------------------------------------------

static void switch_mesh(void) {
    model.active = (model.active + 1) % MESH_COUNT;
    mesh_state_t* mesh = model_active(&model);
    leds_set_mesh(mesh->accent);
    toast("%s", mesh->name);
}

static void next_channel(int delta) {
    mesh_state_t* mesh  = model_active(&model);
    mesh->input_channel = (mesh->input_channel + delta + mesh->channel_count) % mesh->channel_count;
    toast("sending to #%s", mesh->channels[mesh->input_channel].name);
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

static const message_t* mesh_msg_at(const mesh_state_t* mesh, int logical) {
    int idx = (mesh->head - mesh->count + logical + MAX_MESSAGES * 2) % MAX_MESSAGES;
    return &mesh->messages[idx];
}

static int selected_logical(const mesh_state_t* mesh) {
    for (int i = 0; i < mesh->count; i++) {
        const message_t* m = mesh_msg_at(mesh, i);
        if (m->used && (int32_t)m->seq == mesh->selected_seq) return i;
    }
    return -1;
}

static void selection_move(int dir) {
    mesh_state_t* mesh = model_active(&model);
    if (mesh->count == 0) return;

    if (mesh->selected_seq < 0) {
        if (dir > 0) return;  // alt+down with nothing selected does nothing
        mesh->selected_seq = (int32_t)mesh_msg_at(mesh, mesh->count - 1)->seq;
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
    mesh->selected_seq = (int32_t)mesh_msg_at(mesh, next)->seq;

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

    model.overlay = OVERLAY_PICKER;
    toast("saved #%s", ch->name);
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
    toast("deleted #%s", gone);
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
                    snprintf(model.confirm_text, sizeof(model.confirm_text), "Delete #%s?",
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
            model.overlay = OVERLAY_NONE;
            toast("identity saved");
            break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
        case BSP_INPUT_NAVIGATION_KEY_F1: model.overlay = OVERLAY_NONE; break;
        default: break;
    }
}

static void handle_picker_key(bsp_input_navigation_key_t key) {
    mesh_state_t* mesh = model_active(&model);

    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            model.picker_index = (model.picker_index - 1 + mesh->channel_count) % mesh->channel_count;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            model.picker_index = (model.picker_index + 1) % mesh->channel_count;
            break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            mesh->input_channel = model.picker_index;
            model.overlay       = OVERLAY_NONE;
            toast("sending to #%s", mesh->channels[mesh->input_channel].name);
            break;
        case BSP_INPUT_NAVIGATION_KEY_F2: editor_open(true, -1); break;
        case BSP_INPUT_NAVIGATION_KEY_F3: editor_open(false, model.picker_index); break;
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
        case OVERLAY_DETAIL:
            if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_F1 ||
                key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                model.overlay = OVERLAY_NONE;
            }
            return;
        case OVERLAY_CONFIRM:
            if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                editor_delete_confirmed();
            } else if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_F1) {
                model.overlay = OVERLAY_EDITOR;
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
            if (model.composer_len > 0) {
                composer_set("");
                toast("cleared");
            } else {
                ESP_LOGI(TAG, "exit to launcher");
                bsp_device_restart_to_launcher();
            }
            break;

        case BSP_INPUT_NAVIGATION_KEY_F2: switch_mesh(); break;
        case BSP_INPUT_NAVIGATION_KEY_F3: model.show_meta = !model.show_meta; break;
        case BSP_INPUT_NAVIGATION_KEY_F4: toast("emoji picker: not in this build"); break;
        case BSP_INPUT_NAVIGATION_KEY_F5:
            model.identity.field = ID_FIELD_NAME;
            model.overlay        = OVERLAY_IDENTITY;
            break;
        case BSP_INPUT_NAVIGATION_KEY_F6:
            model.overlay      = OVERLAY_PICKER;
            model.picker_index = mesh->input_channel;
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

    mock_data_init(&model);
    leds_init();
    leds_set_mesh(model_active(&model)->accent);

    next_incoming_ms = now_ms() + INCOMING_EVERY_MS;
    next_activity_ms = now_ms() + ACTIVITY_EVERY_MS;

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

        if (tx_tick()) dirty = true;
        if (incoming_tick()) dirty = true;
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
