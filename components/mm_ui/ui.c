// SPDX-License-Identifier: MIT

#include "session_log.h"
#include "ui.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bsp/display.h"
#include "esp_log.h"
#include "font_mono_fi.h"
#include "meshcore_wire.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"

static const char TAG[] = "ui";

// Monospace at exactly 2x the 7x9 cell, so glyph pixels stay square and the
// character advance is an integer -- which is what lets the column maths below
// be exact rather than measured.
#define FONT      pax_font_mono_fi
#define FONT_SIZE 18
#define CHAR_W    14
#define LINE_H    20

#define COL_BG      0xFF0E0E14
#define COL_TEXT    0xFFE8E8E8
#define COL_SENT    0xFFA8C8F0  // our own messages, echoed locally
#define COL_DIM     0xFF6E6E80
#define COL_FROM    0xFFCBD5E1  // a node we have a name for
#define COL_FROM_ID 0xFF8A7BA8  // ...and one we only have an id for
#define COL_SEP     0xFF2A2A38
#define COL_CHIP    0xFF0A0A10
#define COL_SEL     0xFF243044
#define COL_OVERLAY 0xD0000000
#define COL_OK      0xFF5FD07A
#define COL_WARN    0xFFE8D44C
#define COL_BAD     0xFFE24B4B
#define COL_DM      0xFFE0A0FF  // conversations, distinct from every channel colour
#define COL_RISK    0xFFF08030  // allowed, but unlikely to survive the trip
#define COL_MC_ARC  0xFF4FA8FF  // the two networks on the boot mast, bright enough
#define COL_MT_ARC  0xFF5FD07A  // to read as colour rather than tint
#define COL_LOGO_MC 0xFFCFE4FF  // the wordmark, tinted toward each network but
#define COL_LOGO_MT 0xFFCFF0DA  // pale enough to stay one piece of lettering

// Keycap colours, matched to the physical legends on the top row.
#define KEY_RED    0xFFE24B4B
#define KEY_ORANGE 0xFFE8913C
#define KEY_YELLOW 0xFFE8D44C
#define KEY_GREEN  0xFF5FD07A
#define KEY_BLUE   0xFF5AA9E8
#define KEY_VIOLET 0xFFB07BE8

typedef enum { CAP_CROSS, CAP_TRI, CAP_SQUARE, CAP_CIRCLE, CAP_CLOUD, CAP_DIAMOND } keycap_t;

#define STATUS_H   26
#define MSG_TOP    32
#define COMPOSER_H 26
#define HINT_H     20

static pax_buf_t fb           = {0};
static bool      have_display = false;

// Native panel geometry. The Tanmatsu panel is 480x800 portrait and the BSP
// asks for a 270-degree rotation, so these are NOT the dimensions to lay out
// against -- they exist only to size the buffer and the blit.
static size_t h_res = 0;
static size_t v_res = 0;

// Logical drawing surface after rotation: what every coordinate below uses.
static float ui_w = 0;
static float ui_h = 0;

static float composer_y = 0;
static float hint_y     = 0;
static float sep_y      = 0;
static float msg_bottom = 0;
static int   msg_rows   = 0;

static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, h_res, v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "blit failed: %d", res);
    }
}

int ui_visible_rows(void) {
    return msg_rows;
}

bool ui_init(void) {
    bsp_display_color_format_t color_format = 0;
    bsp_display_endianness_t   endianness   = 0;

    esp_err_t res = bsp_display_get_parameters(&h_res, &v_res, &color_format, &endianness);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "no display (%d)", res);
        return false;
    }

    pax_buf_type_t format;
    switch (color_format) {
        case BSP_DISPLAY_COLOR_FORMAT_16_565RGB: format = PAX_BUF_16_565RGB; break;
        case BSP_DISPLAY_COLOR_FORMAT_24_888RGB: format = PAX_BUF_24_888RGB; break;
        case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB: format = PAX_BUF_32_8888ARGB; break;
        default:
            ESP_LOGE(TAG, "unsupported color format %d", color_format);
            return false;
    }

    pax_orientation_t orientation;
    switch (bsp_display_get_default_rotation()) {
        case BSP_DISPLAY_ROTATION_90: orientation = PAX_O_ROT_CCW; break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW; break;
        default: orientation = PAX_O_UPRIGHT; break;
    }

    pax_buf_init(&fb, NULL, h_res, v_res, format);
    pax_buf_reversed(&fb, endianness == BSP_DISPLAY_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    // Query the buffer rather than reusing h_res/v_res: orientation swaps them.
    ui_w = (float)pax_buf_get_width(&fb);
    ui_h = (float)pax_buf_get_height(&fb);

    composer_y = ui_h - COMPOSER_H;
    hint_y     = composer_y - HINT_H;
    sep_y      = hint_y - 6;
    msg_bottom = sep_y - 4;
    msg_rows   = (int)((msg_bottom - MSG_TOP) / LINE_H);
    if (msg_rows < 1) msg_rows = 1;

    have_display = true;
    ESP_LOGI(TAG, "panel %ux%u -> logical %.0fx%.0f, %d cols x %d msg rows", (unsigned)h_res, (unsigned)v_res, ui_w,
             ui_h, (int)(ui_w / CHAR_W), msg_rows);
    return true;
}

// The boot screen: a transmitting mast over the wordmark. Pure ASCII, because
// the only monospace font here covers Latin-1 and nothing more -- no box drawing
// and no block glyphs, so the art is built from brackets and slashes.
//
// The arcs run blue on the left and green on the right, the two networks meeting
// at the antenna, which is the whole idea of the application in one picture.

// Each line is centred on the mast, so every one is an odd number of columns.
static const char* const BOOT_MAST[] = {
    "((  o  ))", "((     |     ))", "((        |        ))", "/|\\", "/ | \\", "_____/__|__\\_____",
};

// 5x5 letters, one blank column between them.
static const char* const GLYPH_MULTI[5] = {
    "#   # #   # #     ##### #####", "## ## #   # #       #     #  ", "# # # #   # #       #     #  ",
    "#   # #   # #       #     #  ", "#   #  ###  #####   #   #####",
};
static const char* const GLYPH_MESH[5] = {
    "#   # #####  #### #   #", "## ## #     #     #   #", "# # # ####   ###  #####",
    "#   # #         # #   #", "#   # ##### ####  #   #",
};

static void boot_centred(const char* text, float y, pax_col_t col) {
    float w = (float)strlen(text) * CHAR_W;
    pax_draw_text(&fb, col, FONT, FONT_SIZE, (ui_w - w) / 2, y, text);
}

// Draw one mast line with its left half blue and its right half green, split at
// the mast itself.
static void boot_mast_line(const char* text, float y) {
    size_t len   = strlen(text);
    float  x     = (ui_w - (float)len * CHAR_W) / 2;
    size_t middle = len / 2;

    for (size_t i = 0; i < len; i++) {
        if (text[i] == ' ') continue;
        char glyph[2] = {text[i], '\0'};
        pax_col_t col = i < middle ? COL_MC_ARC : (i > middle ? COL_MT_ARC : COL_TEXT);
        pax_draw_text(&fb, col, FONT, FONT_SIZE, x + (float)i * CHAR_W, y, glyph);
    }
}

void ui_boot_line(const char* fmt, ...) {
    char    line[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", line);
    if (!have_display) return;

    pax_background(&fb, COL_BG);

    float y = LINE_H;
    for (size_t i = 0; i < sizeof(BOOT_MAST) / sizeof(BOOT_MAST[0]); i++, y += LINE_H) {
        boot_mast_line(BOOT_MAST[i], y);
    }

    // The two words carry the two networks' tints, close enough to read as one
    // wordmark and far enough apart to read as two words.
    y += LINE_H;
    for (int i = 0; i < 5; i++, y += LINE_H) boot_centred(GLYPH_MULTI[i], y, COL_LOGO_MC);
    for (int i = 0; i < 5; i++, y += LINE_H) boot_centred(GLYPH_MESH[i], y, COL_LOGO_MT);

    y += LINE_H;
    boot_centred("MeshCore  +  Meshtastic", y, COL_DIM);

    y += LINE_H * 2;
    boot_centred(line, y, COL_SEP);
    blit();
}

// --- keycap glyphs -------------------------------------------------------
// Drawn as shapes rather than font glyphs so the on-screen hints look like the
// symbols actually printed on the keys.

// pax_draw_thick_line() dispatches its quad without applying the buffer
// orientation -- unlike pax_draw_line(), which does. On this rotated panel that
// puts the stroke at transposed coordinates, so fake the thickness with
// parallel thin lines instead.
static void stroke(pax_col_t color, float x0, float y0, float x1, float y1) {
    pax_draw_line(&fb, color, x0, y0, x1, y1);
    pax_draw_line(&fb, color, x0 + 1, y0, x1 + 1, y1);
    pax_draw_line(&fb, color, x0, y0 + 1, x1, y1 + 1);
}

static void draw_keycap(float x, float cy, keycap_t cap) {
    float r = 7;
    switch (cap) {
        case CAP_CROSS:
            stroke(KEY_RED, x - r + 1, cy - r + 1, x + r - 1, cy + r - 1);
            stroke(KEY_RED, x + r - 1, cy - r + 1, x - r + 1, cy + r - 1);
            break;
        case CAP_TRI:
            pax_outline_tri(&fb, KEY_ORANGE, x, cy - r, x + r, cy + r - 1, x - r, cy + r - 1);
            break;
        case CAP_SQUARE:
            pax_draw_rect(&fb, KEY_YELLOW, x - r + 1, cy - r + 1, (r - 1) * 2, (r - 1) * 2);
            break;
        case CAP_CIRCLE:
            pax_outline_circle(&fb, KEY_GREEN, x, cy, r - 1);
            break;
        case CAP_CLOUD:
            pax_draw_circle(&fb, KEY_BLUE, x - 3, cy + 1, 4);
            pax_draw_circle(&fb, KEY_BLUE, x + 3, cy + 1, 4);
            pax_draw_circle(&fb, KEY_BLUE, x, cy - 2, 5);
            break;
        case CAP_DIAMOND:
            pax_draw_tri(&fb, KEY_VIOLET, x, cy - r, x + r - 1, cy, x - r + 1, cy);
            pax_draw_tri(&fb, KEY_VIOLET, x, cy + r, x + r - 1, cy, x - r + 1, cy);
            break;
    }
}

static float hint(float x, float y, keycap_t cap, const char* label) {
    draw_keycap(x + 8, y + LINE_H / 2.0f, cap);
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, x + 19, y, label);
    return x + 19 + (float)strlen(label) * CHAR_W + 16;
}

static float hint_text(float x, float y, const char* label) {
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, x, y, label);
    return x + (float)strlen(label) * CHAR_W + 16;
}

// --- columns -------------------------------------------------------------

#define COL_TIME_CELLS 5
#define COL_FROM_CELLS 4
#define COL_META_CELLS 11

// The channel column is sized to the longest abbreviation actually configured
// rather than to CH_DISPLAY_MAX, so short names do not cost the message text any
// width. Clamped below so the column never collapses.
#define COL_CHAN_MIN 4

static int chan_cells(const app_model_t* model) {
    const mesh_state_t* mesh  = &model->mesh[model->active];
    int                 width = COL_CHAN_MIN;
    for (int i = 0; i < mesh->channel_count; i++) {
        int len = (int)strlen(mesh->channels[i].display);
        if (len > width) width = len;
    }
    return width > CH_DISPLAY_MAX ? CH_DISPLAY_MAX : width;
}

static float x_time(void) {
    return 6;
}
static float x_chan(void) {
    return x_time() + (COL_TIME_CELLS + 1) * CHAR_W;
}
static float x_from(const app_model_t* model) {
    return x_chan() + (chan_cells(model) + 1) * CHAR_W;
}
static float x_text(const app_model_t* model) {
    return x_from(model) + (COL_FROM_CELLS + 1) * CHAR_W;
}

static int text_room(const app_model_t* model) {
    float right = model->show_meta ? ui_w - 10 - (COL_META_CELLS + 1) * CHAR_W : ui_w - 10;
    int   room  = (int)((right - x_text(model)) / CHAR_W);
    return room > 4 ? room : 4;
}

// --- wrapping ------------------------------------------------------------

typedef struct {
    int16_t msg;        // logical message index
    int16_t start;      // byte offset into the message text
    int16_t len;
    bool    first;      // first line of the message: draw the columns
    bool    separator;  // a day rule rather than a message line
    uint8_t day_index;  // which message the separator precedes
} line_t;

#define MAX_LINES_PER_MSG 6
static line_t lines[MAX_MESSAGES * MAX_LINES_PER_MSG + MAX_MESSAGES];
static int    line_total;

static int utf8_adv(unsigned char c) {
    return (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
}

// Copy at most `cells` characters, never a fraction of one.
//
// The columns here are measured in character cells, but printf precision counts
// bytes, so "%.4s" on a name like "Häämä" keeps four bytes and leaves a dangling
// lead byte. That is not a shortened name on screen, it is a broken glyph -- and
// it only shows up on exactly the names this app exists to carry.
static void utf8_clip(char* dst, size_t dst_size, const char* src, int cells) {
    size_t at = 0;
    for (int n = 0; n < cells && src[at]; n++) {
        int width = utf8_adv((unsigned char)src[at]);
        if (at + width >= dst_size) break;
        memcpy(&dst[at], &src[at], width);
        at += width;
    }
    dst[at] = '\0';
}

// What to put in a narrow column for a sender: the short name if one has been
// set for them, otherwise their real name for the clipper to shorten. Resolved
// at draw time rather than stored on the message, so setting a short name
// relabels the conversation already on screen instead of only what arrives next.
static const char* short_label(const mesh_state_t* mesh, const char* name) {
    const char* shortened = model_short_name_for(mesh, name);
    return shortened ? shortened : name;
}

#define msg_at(mesh, logical) model_message_at((mesh), (logical))

static void wrap_message(int msg_index, const char* text, int room) {
    int pos   = 0;
    int count = 0;

    while (count < MAX_LINES_PER_MSG) {
        int scan = pos, cells = 0, last_space = -1;
        while (text[scan] && cells < room) {
            if (text[scan] == ' ') last_space = scan;
            scan += utf8_adv((unsigned char)text[scan]);
            cells++;
        }

        int  end  = scan;
        bool more = text[scan] != '\0';
        if (more && last_space > pos) end = last_space;

        if (line_total < (int)(sizeof(lines) / sizeof(lines[0]))) {
            lines[line_total++] = (line_t){.msg   = (int16_t)msg_index,
                                           .start = (int16_t)pos,
                                           .len   = (int16_t)(end - pos),
                                           .first = (count == 0)};
        }
        count++;

        if (!more) return;
        pos = end;
        while (text[pos] == ' ') pos++;
        if (!text[pos]) return;
    }
}

static int day_of(uint32_t timestamp) {
    if (timestamp < 1000000000u) return -1;
    time_t    t = (time_t)timestamp;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    return tm_buf.tm_year * 512 + tm_buf.tm_yday;
}

static void build_lines(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];
    int                 room = text_room(model);

    line_total   = 0;
    int last_day = -2;

    for (int i = 0; i < mesh->count; i++) {
        const message_t* msg = msg_at(mesh, i);
        if (!msg->used) continue;

        int day = day_of(msg->timestamp);
        if (day != last_day && day != -1) {
            if (line_total < (int)(sizeof(lines) / sizeof(lines[0]))) {
                lines[line_total++] = (line_t){.msg = (int16_t)i, .separator = true};
            }
            last_day = day;
        }
        wrap_message(i, msg->text, room);
    }
}

int ui_line_count(const app_model_t* model) {
    build_lines(model);
    return line_total;
}

// Translate the stored anchor into an index into `lines`. build_lines() must
// have run. Falls back to the bottom when the anchored message has aged out of
// the ring.
static int anchor_line_index(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];

    if (mesh->pinned) {
        int first = line_total - msg_rows;
        return first < 0 ? 0 : first;
    }

    int seen = 0;
    for (int i = 0; i < line_total; i++) {
        if (lines[i].separator) continue;
        const message_t* msg = msg_at(mesh, lines[i].msg);
        if (msg->seq == mesh->anchor_seq) {
            if (seen == mesh->anchor_line) {
                // Show the day rule with the message it introduces.
                if (i > 0 && lines[i - 1].separator) return i - 1;
                return i;
            }
            seen++;
        }
    }
    int first = line_total - msg_rows;
    return first < 0 ? 0 : first;
}

int ui_anchor_index(const app_model_t* model) {
    build_lines(model);
    return anchor_line_index(model);
}

int ui_line_of_seq(const app_model_t* model, uint32_t seq) {
    const mesh_state_t* mesh = &model->mesh[model->active];
    build_lines(model);
    for (int i = 0; i < line_total; i++) {
        if (lines[i].separator || !lines[i].first) continue;
        if (msg_at(mesh, lines[i].msg)->seq == seq) return i;
    }
    return -1;
}

void ui_set_anchor(app_model_t* model, int line_index) {
    mesh_state_t* mesh = &model->mesh[model->active];

    build_lines(model);
    if (line_index < 0) line_index = 0;
    if (line_index > line_total - msg_rows) line_index = line_total - msg_rows;
    if (line_index < 0) line_index = 0;

    if (line_index >= line_total - msg_rows) {
        mesh->pinned = true;
        mesh->unseen = 0;
        return;
    }

    // Skip forward off a separator: anchors name a message, not a rule.
    while (line_index < line_total && lines[line_index].separator) line_index++;
    if (line_index >= line_total) {
        mesh->pinned = true;
        return;
    }

    const message_t* msg = msg_at(mesh, lines[line_index].msg);
    int              off = 0;
    for (int i = line_index - 1; i >= 0 && !lines[i].separator && lines[i].msg == lines[line_index].msg; i--) off++;

    mesh->pinned      = false;
    mesh->anchor_seq  = msg->seq;
    mesh->anchor_line = off;
}

// --- status bar ----------------------------------------------------------

// Sits on the mesh accent, so the "healthy" colour has to hold its own against
// blue or green rather than being a neutral grey.
#define COL_BATT_OK   0xFF2FBF5A
#define COL_BATT_LOW  0xFFF0A030
#define COL_BATT_CRIT 0xFFE24B4B

static void draw_battery(float x, float y, int pct, bool charging) {
    pax_col_t col = charging ? COL_BATT_OK : pct < 15 ? COL_BATT_CRIT : pct <= 30 ? COL_BATT_LOW : COL_BATT_OK;
    pax_outline_rect(&fb, col, x, y + 4, 22, 12);
    pax_draw_rect(&fb, col, x + 22, y + 7, 2, 6);
    float fill = 20.0f * (float)pct / 100.0f;
    pax_draw_rect(&fb, col, x + 1, y + 5, fill, 10);
}

static void draw_status(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];
    const channel_t*    ch   = &mesh->channels[mesh->input_channel];

    pax_draw_rect(&fb, mesh->accent, 0, 0, ui_w, STATUS_H);
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, 8, 4, mesh->name);

    // Where the composer is aimed, on its own dark chip: neither a channel
    // colour nor the conversation colour is reliably legible directly against
    // the mesh accent.
    // Clipped, because the right of the bar is occupied and a long name would
    // otherwise run straight through the clock. Contacts are the ones that get
    // long -- a MeshCore name can be 32 bytes.
#define STATUS_CHIP_CELLS 16
    char      full[NODE_NAME_MAX + 4];
    char      label[NODE_NAME_MAX + 4];
    pax_col_t label_col = ch->color;
    if (mesh->target_contact) {
        const node_t* peer = model_target_node((mesh_state_t*)mesh, model->active);
        char          who[NODE_NAME_MAX + 1] = "(gone)";
        if (peer) model_node_label(peer, model->active, who, sizeof(who));
        snprintf(full, sizeof(full), ">%s", who);
        label_col = peer ? COL_DM : COL_BAD;
    } else {
        snprintf(full, sizeof(full), "%s", ch->name);
    }
    utf8_clip(label, sizeof(label), full, STATUS_CHIP_CELLS);
    float chip_w = (float)strlen(label) * CHAR_W + 12;
    float chip_x = 8 + 11 * CHAR_W;
    pax_draw_round_rect(&fb, COL_CHIP, chip_x, 3, chip_w, STATUS_H - 6, 4);
    pax_draw_text(&fb, label_col, FONT, FONT_SIZE, chip_x + 6, 4, label);

    // Right side: radio, battery, clock.
    float x = ui_w - 8 - 5 * CHAR_W;

    char clock[16] = "--:--";
    if (model->time_synced) {
        time_t    now = time(NULL);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(clock, sizeof(clock), "%H:%M", &tm_buf);
    }
    // An unsynced clock is dimmed rather than hidden: wrong time is worse than
    // obviously-absent time.
    pax_draw_text(&fb, model->time_synced ? COL_TEXT : COL_BAD, FONT, FONT_SIZE, x, 4, clock);

    x -= 34;
    draw_battery(x, 3, model->battery_pct, model->charging);

    // Recording is stated plainly. A diagnostic that runs unnoticed is one that
    // fills flash for a week and is never collected.
    if (session_log_active()) {
        x -= 6 + 4 * CHAR_W;
        pax_draw_text(&fb, COL_BAD, FONT, FONT_SIZE, x, 4, "REC");
    }

    // Relaying: MeshCore off-grid repeat, or a Meshtastic CLIENT forwarding for
    // others. Present only when it is on, so it never takes space it has no use
    // for. Dim while idle, bright for a moment each time a packet is relayed,
    // and carrying the count -- which is the part worth having, since repeating
    // for nobody looks exactly like repeating for everybody.
    bool relaying = model->active == MESH_MC ? model->settings.mc_repeater
                                             : model->settings.mt_role == MT_ROLE_CLIENT;
    if (relaying) {
        uint32_t count = model->repeat_count[model->active];
        char     badge[12];
        snprintf(badge, sizeof(badge), "RPT %lu", (unsigned long)(count > 999 ? 999 : count));
        x -= 6 + (float)strlen(badge) * CHAR_W;
        pax_draw_text(&fb, model->repeat_busy ? COL_OK : COL_DIM, FONT, FONT_SIZE, x, 4, badge);
    }

    x -= 6 + 3 * CHAR_W;
    const char* radio_label = model->radio == RADIO_TX ? "TX" : model->radio == RADIO_ERROR ? "!!" : "RX";
    pax_col_t   radio_col   = model->radio == RADIO_TX ? COL_WARN : model->radio == RADIO_ERROR ? COL_BAD : COL_OK;
    pax_draw_circle(&fb, radio_col, x + 5, STATUS_H / 2.0f, 4);
    pax_draw_text(&fb, radio_col, FONT, FONT_SIZE, x + 13, 4, radio_label);
}

// --- messages ------------------------------------------------------------

// The timestamp column doubles as the delivery indicator for our own messages:
// progress while it matters, then a clock whose colour carries the outcome.
static void draw_time_cell(const app_model_t* model, const message_t* msg, float y) {
    char      buf[8];
    pax_col_t col = COL_DIM;

    if (msg->outgoing && msg->tx != TX_NONE && msg->tx != TX_CONFIRMED && msg->tx != TX_FAILED) {
        switch (msg->tx) {
            case TX_QUEUED:
                snprintf(buf, sizeof(buf), "  .  ");
                col = COL_DIM;
                break;
            case TX_SENDING:
                snprintf(buf, sizeof(buf), " TX  ");
                col = COL_WARN;
                break;
            default:  // TX_AWAITING
                // A broadcast has only repeats to report. A direct message also
                // has attempts, and the attempt number is the part that says
                // something is going wrong -- so it extends the same notation
                // rather than replacing it: x means repeats on every row, a is
                // the attempt. "x1a2" is one repeat heard, second try.
                if (msg->dm) {
                    snprintf(buf, sizeof(buf), " x%ua%u", (unsigned)(msg->repeats > 9 ? 9 : msg->repeats),
                             (unsigned)(msg->dm_attempt > 8 ? 9 : msg->dm_attempt + 1));
                    col = msg->dm_attempt > 0 ? COL_RISK : (msg->repeats ? COL_OK : COL_WARN);
                } else {
                    snprintf(buf, sizeof(buf), " x%-2u ", (unsigned)msg->repeats);
                    col = msg->repeats ? COL_OK : COL_WARN;
                }
                break;
        }
        pax_draw_text(&fb, col, FONT, FONT_SIZE, x_time(), y, buf);
        return;
    }

    snprintf(buf, sizeof(buf), "--:--");
    if (msg->timestamp > 1000000000u) {
        time_t    t = (time_t)msg->timestamp;
        struct tm tm_buf;
        localtime_r(&t, &tm_buf);
        strftime(buf, sizeof(buf), "%H:%M", &tm_buf);
    }
    // A send that never saw a repeat keeps a red clock forever; the detail view
    // explains why.
    if (msg->tx == TX_FAILED) col = COL_BAD;
    else if (!model->time_synced) col = COL_BAD;
    pax_draw_text(&fb, col, FONT, FONT_SIZE, x_time(), y, buf);
}

static void draw_empty_state(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];
    char                line[64];
    snprintf(line, sizeof(line), "No messages yet on %s", mesh->name);
    float w = (float)strlen(line) * CHAR_W;
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, (ui_w - w) / 2, (MSG_TOP + msg_bottom) / 2 - LINE_H, line);

    if (mesh->target_contact) {
        const node_t* peer = model_target_node((mesh_state_t*)mesh, model->active);
        char          who[NODE_NAME_MAX + 1] = "a contact that is gone";
        if (peer) model_node_label(peer, model->active, who, sizeof(who));
        snprintf(line, sizeof(line), "type below to message %s", who);
    } else {
        snprintf(line, sizeof(line), "type below to send on %s", mesh->channels[mesh->input_channel].name);
    }
    w = (float)strlen(line) * CHAR_W;
    pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, (ui_w - w) / 2, (MSG_TOP + msg_bottom) / 2 + 4, line);
}

static void draw_messages(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];

    build_lines(model);
    if (line_total == 0) {
        draw_empty_state(model);
        return;
    }

    int first = anchor_line_index(model);

    // Selection highlights every line of the message, not just one row.
    float y = MSG_TOP;
    for (int i = first; i < line_total && y + LINE_H <= msg_bottom; i++, y += LINE_H) {
        const line_t*    line = &lines[i];
        const message_t* msg  = msg_at(mesh, line->msg);
        const channel_t* ch   = &mesh->channels[msg->channel];

        if (line->separator) {
            char      date[24] = "";
            struct tm tm_buf;
            time_t    t = (time_t)msg->timestamp;
            localtime_r(&t, &tm_buf);
            strftime(date, sizeof(date), "%a %d %b", &tm_buf);
            float w = (float)strlen(date) * CHAR_W;
            float cx = (ui_w - w) / 2;
            pax_draw_line(&fb, COL_SEP, 8, y + LINE_H / 2.0f, cx - 10, y + LINE_H / 2.0f);
            pax_draw_line(&fb, COL_SEP, cx + w + 10, y + LINE_H / 2.0f, ui_w - 12, y + LINE_H / 2.0f);
            pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, cx, y, date);
            continue;
        }

        bool selected = mesh->selected_seq >= 0 && (uint32_t)mesh->selected_seq == msg->seq;
        if (selected) {
            pax_draw_rect(&fb, COL_SEL, 0, y - 1, ui_w - 6, LINE_H);
            pax_draw_rect(&fb, ch->color, 0, y - 1, 3, LINE_H);
        }

        if (line->first) {
            draw_time_cell(model, msg, y);

            // A direct message takes over the channel column: which channel it
            // travelled on is an implementation detail, who it was with is not.
            // The arrow gives the direction, so the sender column can stay empty
            // rather than repeating the same name on every row.
            char chan[SENDER_MAX + 2];
            if (msg->dm) {
                // The direction arrow lives in the gutter between the timestamp
                // and this column, which is otherwise blank. It is a marker
                // about the message rather than part of anyone's name, and
                // putting it here buys the name back the cell it was eating --
                // which matters when four characters is all a name gets.
                pax_draw_text(&fb, COL_DM, FONT, FONT_SIZE, x_chan() - CHAR_W, y, msg->outgoing ? ">" : "<");

                utf8_clip(chan, sizeof(chan), short_label(mesh, msg->peer), chan_cells(model));
                pax_draw_text(&fb, COL_DM, FONT, FONT_SIZE, x_chan(), y, chan);
            } else {
                utf8_clip(chan, sizeof(chan), ch->display, chan_cells(model));
                pax_draw_text(&fb, ch->color, FONT, FONT_SIZE, x_chan(), y, chan);
            }

            if (!msg->dm) {
                char who[SENDER_MAX];
                utf8_clip(who, sizeof(who), short_label(mesh, msg->sender), COL_FROM_CELLS);
                // Colour alone distinguishes a NodeInfo short name from a raw
                // node id -- no prefix, so the column is the same width either
                // way.
                pax_draw_text(&fb, msg->sender_named ? COL_FROM : COL_FROM_ID, FONT, FONT_SIZE, x_from(model), y,
                              who);
            } else if (msg->outgoing && msg->acked) {
                // The one place either network offers a real delivery proof.
                pax_draw_text(&fb, COL_OK, FONT, FONT_SIZE, x_from(model), y, "ack");
            }

            if (model->show_meta) {
                // Signal-to-noise, not RSSI. The coprocessor reports a per-packet
                // RSSI of zero for every frame, so the dBm figure that used to be
                // here was never a measurement. SNR is real and is what
                // distinguishes a comfortable link from a marginal one anyway.
                char meta[24];
                snprintf(meta, sizeof(meta), "%3d.%1u dB %uh", msg->snr_db_x4 / 4,
                         (unsigned)((msg->snr_db_x4 < 0 ? -msg->snr_db_x4 : msg->snr_db_x4) % 4) * 25 / 10,
                         (unsigned)(msg->hops > 99 ? 99 : msg->hops));
                pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, ui_w - 10 - COL_META_CELLS * CHAR_W, y, meta);
            }
        }

        char body[TEXT_MAX];
        int  len = line->len < (int)sizeof(body) - 1 ? line->len : (int)sizeof(body) - 1;
        memcpy(body, &msg->text[line->start], len);
        body[len] = '\0';
        pax_draw_text(&fb, msg->outgoing ? COL_SENT : COL_TEXT, FONT, FONT_SIZE, x_text(model), y, body);
    }

    // Slim scrollbar rather than a text indicator: it costs no columns and
    // shows position as well as the fact that there is more above.
    if (line_total > msg_rows) {
        float track_h = msg_bottom - MSG_TOP;
        float thumb_h = track_h * (float)msg_rows / (float)line_total;
        if (thumb_h < 12) thumb_h = 12;
        float travel = track_h - thumb_h;
        float above  = (float)first / (float)(line_total - msg_rows);
        pax_draw_rect(&fb, COL_SEP, ui_w - 4, MSG_TOP, 3, track_h);
        pax_draw_rect(&fb, mesh->pinned ? COL_FROM_ID : KEY_YELLOW, ui_w - 4, MSG_TOP + travel * above, 3, thumb_h);
    }

    // Arrivals while scrolled away are announced rather than silently added.
    if (!mesh->pinned && mesh->unseen > 0) {
        char note[32];
        snprintf(note, sizeof(note), " %d new - fn+down ", mesh->unseen);
        float w = (float)strlen(note) * CHAR_W;
        pax_draw_round_rect(&fb, 0xFF2A3550, (ui_w - w) / 2, msg_bottom - LINE_H - 2, w, LINE_H, 4);
        pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, (ui_w - w) / 2, msg_bottom - LINE_H, note);
    }
}

// --- composer ------------------------------------------------------------

static int cells_in(const char* s, int bytes) {
    int cells = 0;
    for (int i = 0; i < bytes && s[i];) {
        i += utf8_adv((unsigned char)s[i]);
        cells++;
    }
    return cells;
}

static void draw_composer(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];
    const channel_t*    ch   = &mesh->channels[mesh->input_channel];

    pax_draw_line(&fb, COL_SEP, 6, sep_y, ui_w - 6, sep_y);

    // The prompt carries the target's colour: the composer should never be
    // ambiguous about where the text is going, and a private message going out
    // on a channel by mistake is the version of that which actually matters.
    pax_draw_text(&fb, mesh->target_contact ? COL_DM : ch->color, FONT, FONT_SIZE, 6, composer_y + 3, ">");

    float x    = 6 + 2 * CHAR_W;
    int   room = (int)((ui_w - 8 - 5 * CHAR_W - x) / CHAR_W);

    int cursor_cell = cells_in(model->composer, model->composer_cursor);

    // Scroll the view so the cursor stays visible.
    int skip = 0;
    if (cursor_cell > room) skip = cursor_cell - room;

    const char* shown = model->composer;
    for (int c = 0; c < skip && *shown;) {
        shown += utf8_adv((unsigned char)*shown);
        c++;
    }
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, x, composer_y + 3, shown);
    pax_draw_rect(&fb, COL_TEXT, x + (cursor_cell - skip) * CHAR_W, composer_y + 4, 2, LINE_H - 4);

    // Bytes, not characters: the protocol limit counts bytes and 'ä' is two of
    // them. Colour warns as the budget runs out, so no number is needed.
    //
    // On MeshCore the warning is about delivery rather than legality. A long
    // message is legal well past the point where it stops arriving: airtime
    // grows with length and every hop is another chance to lose the frame, so
    // the thresholds are set where messages were observed to start failing, not
    // where the protocol stops accepting them. Red still means "will be
    // refused"; amber now means "may not arrive".
    int limit   = model_byte_limit(model);
    int strain  = model->active == MESH_MC ? MC_STRAIN_BYTES : limit * 9 / 10;
    int comfort = model->active == MESH_MC ? MC_COMFORT_BYTES : limit * 9 / 10;

    pax_col_t col;
    if (model->composer_len > limit) {
        col = COL_BAD;  // will be refused outright
    } else if (model->composer_len > strain) {
        col = COL_RISK;  // legal, but observed to often not arrive
    } else if (model->composer_len > comfort) {
        col = COL_WARN;
    } else {
        col = COL_OK;
    }
    char count[8];
    snprintf(count, sizeof(count), "%4d", model->composer_len);
    pax_draw_text(&fb, model->composer_len ? col : COL_SEP, FONT, FONT_SIZE, ui_w - 8 - 4 * CHAR_W,
                  composer_y + 3, count);
}

static void draw_hints(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];
    float               x    = 6;

    if (mesh->selected_seq >= 0) {
        x = hint(x, hint_y + 1, CAP_CROSS, "exit sel");
        x = hint_text(x, hint_y + 1, "alt+up/dn move");
        hint_text(x, hint_y + 1, "enter details");
        return;
    }

    // The red cross escalates: clear, then leave the conversation, then leave
    // the app. Label it with whichever it will actually do next, so leaving a
    // private conversation is never a guess.
    const char* escape = "exit";
    if (model->composer_len > 0) {
        escape = "clear";
    } else if (mesh->target_contact) {
        escape = "leave DM";
    }
    x = hint(x, hint_y + 1, CAP_CROSS, escape);
    x = hint(x, hint_y + 1, CAP_TRI, model->active == MESH_MC ? "MT" : "MC");
    x = hint(x, hint_y + 1, CAP_SQUARE, model->show_meta ? "meta on" : "meta off");
    x = hint(x, hint_y + 1, CAP_CIRCLE, "nodes");
    x = hint(x, hint_y + 1, CAP_CLOUD, "ident");
    x = hint(x, hint_y + 1, CAP_DIAMOND, "channels");
    (void)x;
}

// --- overlays ------------------------------------------------------------

static void overlay_box(float bw, float bh, float* out_x, float* out_y, pax_col_t accent, const char* title) {
    pax_draw_rect(&fb, COL_OVERLAY, 0, 0, ui_w, ui_h);
    float bx = (ui_w - bw) / 2;
    float by = (ui_h - bh) / 2;
    pax_draw_round_rect(&fb, 0xFF161620, bx, by, bw, bh, 6);
    pax_outline_round_rect(&fb, accent, bx, by, bw, bh, 6);
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, bx + 12, by + 8, title);
    *out_x = bx;
    *out_y = by;
}

static bool node_is_target(const mesh_state_t* mesh, mesh_id_t id, const node_t* node) {
    if (!mesh->target_contact) return false;
    return id == MESH_MT ? node->node_num == mesh->target_num
                         : memcmp(node->key, mesh->target_key, NODE_KEY_LEN) == 0;
}

// The picker lists channels and then contacts as one column, because the
// composer aims at exactly one of them: a target, not two separate settings.
// Indices above the channel count are contacts, in the order the nodes view
// shows them.
static int ui_picker_contacts(const app_model_t* model, int* order, int max) {
    const mesh_state_t* mesh = &model->mesh[model->active];
    return model_nodes_by_recency(mesh, order, max);
}

int ui_picker_count(const app_model_t* model) {
    int order[MAX_NODES];
    return model->mesh[model->active].channel_count + ui_picker_contacts(model, order, MAX_NODES);
}

static void draw_picker(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];

    int order[MAX_NODES];
    int contacts = ui_picker_contacts(model, order, MAX_NODES);
    // Enough contacts to be useful without pushing the box off screen; the rest
    // are reachable from the nodes view.
    int contact_rows = contacts < 8 ? contacts : 8;

    float bw = 44 * CHAR_W;
    float bh = LINE_H * (mesh->channel_count + contact_rows + 6) + 12;
    float bx, by;
    overlay_box(bw, bh, &bx, &by, mesh->accent, "Send to");

    float y = by + 8 + LINE_H * 1.5f;
    for (int i = 0; i < mesh->channel_count; i++, y += LINE_H) {
        const channel_t* ch = &mesh->channels[i];
        if (i == model->picker_index) pax_draw_rect(&fb, COL_SEL, bx + 6, y - 2, bw - 12, LINE_H);
        pax_draw_rect(&fb, ch->color, bx + 14, y + 4, 10, 10);

        char label[CH_NAME_MAX + 2];
        snprintf(label, sizeof(label), "%s", ch->name);
        pax_draw_text(&fb, ch->color, FONT, FONT_SIZE, bx + 32, y, label);

        bool current = !mesh->target_contact && i == mesh->input_channel;
        char shown[16];
        snprintf(shown, sizeof(shown), "%-4s %s", ch->display, current ? "<" : " ");
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + bw - 12 - 6 * CHAR_W, y, shown);
    }

    pax_draw_line(&fb, COL_SEP, bx + 10, y, bx + bw - 10, y);
    pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, bx + 14, y + 2, "contacts");
    y += LINE_H + 2;

    if (contacts == 0) {
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "nobody heard yet");
        y += LINE_H;
    }

    // Scroll the contact window so the selection stays visible.
    int first = 0;
    int chosen_contact = model->picker_index - mesh->channel_count;
    if (chosen_contact >= contact_rows) first = chosen_contact - contact_rows + 1;

    for (int i = first; i < contacts && i < first + contact_rows; i++, y += LINE_H) {
        const node_t* node = &mesh->nodes[order[i]];
        if (mesh->channel_count + i == model->picker_index) {
            pax_draw_rect(&fb, COL_SEL, bx + 6, y - 2, bw - 12, LINE_H);
        }

        char label[NODE_NAME_MAX + 2];
        model_node_label(node, model->active, label, sizeof(label));
        pax_draw_text(&fb, node->named ? COL_FROM : COL_FROM_ID, FONT, FONT_SIZE, bx + 32, y, label);

        // Whether this contact can be messaged at all, and how privately.
        // Neither network can carry a conversation without a key, so a missing
        // one is not a weaker option -- it is no option.
        const char* state = "no key";
        pax_col_t   col   = COL_BAD;
        if (node->has_secret) {
            state = "e2e";
            col   = COL_OK;
        } else if (node->secret_pending) {
            state = "...";
            col   = COL_DIM;
        }
        pax_draw_text(&fb, col, FONT, FONT_SIZE, bx + bw - 12 - 8 * CHAR_W, y, state);

        bool current = mesh->target_contact && node_is_target(mesh, model->active, node);
        if (current) pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + bw - 12 - CHAR_W, y, "<");
    }

    y       += 4;
    float hx = bx + 12;
    hx       = hint(hx, y, CAP_CROSS, "close");
    hx       = hint(hx, y, CAP_TRI, "new");
    hx       = hint(hx, y, CAP_SQUARE, "edit");
    hint_text(hx, y, "enter select");
}

static void draw_editor(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];
    const editor_t*     ed   = &model->editor;

    float bw = 48 * CHAR_W;
    float bh = LINE_H * (FIELD_COUNT + 4) + 16;
    float bx, by;

    char title[48];
    snprintf(title, sizeof(title), "%s channel - %s", ed->creating ? "New" : "Edit", mesh->name);
    overlay_box(bw, bh, &bx, &by, mesh->accent, title);

    // Both networks carry the key as base64, so the label is the same for each.
    const char* labels[FIELD_COUNT] = {"Name", "Short", "PSK b64", "Colour"};
    const char* values[FIELD_COUNT] = {ed->name, ed->display, ed->secret, NULL};

    // An empty PSK is meaningful rather than missing, and what it means depends
    // on the network and the name. Saying so removes the guesswork about whether
    // a blank field is going to work.
    const char* secret_hint;
    if (model->active == MESH_MT) {
        secret_hint = "(unencrypted)";
    } else if (ed->name[0] == '#') {
        secret_hint = "(key derived from name)";
    } else {
        secret_hint = "(public channel key)";
    }

    float y = by + 8 + LINE_H * 1.5f;
    for (int f = 0; f < FIELD_COUNT; f++, y += LINE_H) {
        bool focused = (f == ed->field);
        if (focused) pax_draw_rect(&fb, COL_SEL, bx + 6, y - 2, bw - 12, LINE_H);
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, labels[f]);

        float vx = bx + 14 + 9 * CHAR_W;
        if (f == FIELD_COLOR) {
            for (int c = 0; c < CH_PALETTE_SIZE; c++) {
                float sx = vx + c * (CHAR_W + 8);
                pax_draw_rect(&fb, ch_palette[c], sx, y + 2, 14, 14);
                if (c == ed->color) pax_outline_rect(&fb, COL_TEXT, sx - 2, y, 18, 18);
            }
        } else {
            int         room  = (int)((bx + bw - 14 - vx) / CHAR_W) - 1;
            const char* shown = values[f];
            int         len   = (int)strlen(shown);
            if (len > room) shown += len - room;

            if (f == FIELD_SECRET && len == 0) {
                pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, vx, y, secret_hint);
            } else {
                pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, shown);
            }
            if (focused) {
                int cells = len > room ? room : len;
                pax_draw_rect(&fb, COL_TEXT, vx + cells * CHAR_W, y + 3, 2, LINE_H - 6);
            }
        }
    }

    y       += 4;
    float hx = bx + 12;
    hx       = hint(hx, y, CAP_CROSS, "cancel");
    if (!ed->creating) hx = hint(hx, y, CAP_SQUARE, "delete");
    hint_text(hx, y, "up/dn field  enter save");
}

static void draw_identity(const app_model_t* model) {
    const identity_t* id = &model->identity;

    float bw = 46 * CHAR_W;
    float bh = LINE_H * (ID_FIELD_COUNT + 7) + 16;
    float bx, by;
    overlay_box(bw, bh, &bx, &by, model->mesh[model->active].accent, "Identity");

    const char* labels[ID_FIELD_COUNT] = {"Name", "Short"};
    const char* values[ID_FIELD_COUNT] = {id->name, id->short_name};

    float y = by + 8 + LINE_H * 1.5f;
    for (int f = 0; f < ID_FIELD_COUNT; f++, y += LINE_H) {
        bool focused = (f == id->field);
        if (focused) pax_draw_rect(&fb, COL_SEL, bx + 6, y - 2, bw - 12, LINE_H);
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, labels[f]);
        float vx = bx + 14 + 9 * CHAR_W;
        pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, values[f]);
        if (focused) {
            pax_draw_rect(&fb, COL_TEXT, vx + (float)strlen(values[f]) * CHAR_W, y + 3, 2, LINE_H - 6);
        }
    }

    // The node id is derived from the MAC and must never change: it is what
    // other clients key their name lookups on.
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Node");
    pax_draw_text(&fb, COL_FROM_ID, FONT, FONT_SIZE, bx + 14 + 9 * CHAR_W, y, id->node_id);
    y += LINE_H;
    pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, bx + 14, y, "fixed - derived from the MAC");
    y += LINE_H;

    // On MeshCore the public key *is* the node id, so this is what contacts will
    // know us by. Only a prefix fits, which is what other clients show too.
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Key");
    if (id->has_keypair) {
        char hex[32];
        snprintf(hex, sizeof(hex), "%02x%02x%02x%02x %02x%02x%02x%02x...", id->public_key[0], id->public_key[1],
                 id->public_key[2], id->public_key[3], id->public_key[4], id->public_key[5], id->public_key[6],
                 id->public_key[7]);
        pax_draw_text(&fb, COL_FROM_ID, FONT, FONT_SIZE, bx + 14 + 9 * CHAR_W, y, hex);
        y += LINE_H;
        pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, bx + 14, y, "MeshCore identity - signs adverts");
    } else {
        pax_draw_text(&fb, COL_BAD, FONT, FONT_SIZE, bx + 14 + 9 * CHAR_W, y, "unavailable");
        y += LINE_H;
        pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, bx + 14, y, "MeshCore adverts cannot be sent");
    }

    y       += LINE_H + 4;
    float hx = bx + 12;
    hx       = hint(hx, y, CAP_CROSS, "cancel");
    hint_text(hx, y, "up/dn field  enter save");
}

static void draw_detail(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];

    const message_t* msg = NULL;
    for (int i = 0; i < mesh->count; i++) {
        const message_t* m = msg_at(mesh, i);
        if (m->used && (int32_t)m->seq == mesh->selected_seq) msg = m;
    }
    if (!msg) return;

    const channel_t* ch = &mesh->channels[msg->channel];

    float bw = 50 * CHAR_W;
    float bh = LINE_H * 13 + 16;
    float bx, by;
    overlay_box(bw, bh, &bx, &by, mesh->accent, "Message");

    float y  = by + 8 + LINE_H * 1.5f;
    float vx = bx + 14 + 9 * CHAR_W;

    char when[40] = "unknown";
    if (msg->timestamp > 1000000000u) {
        time_t    t = (time_t)msg->timestamp;
        struct tm tm_buf;
        localtime_r(&t, &tm_buf);
        strftime(when, sizeof(when), "%a %d %b %H:%M:%S", &tm_buf);
    }

    struct {
        const char* label;
        const char* value;
        pax_col_t   color;
    } rows[4];
    char chan_val[CH_NAME_MAX + CH_DISPLAY_MAX + 8];
    snprintf(chan_val, sizeof(chan_val), "%s (%s)", ch->name, ch->display);
    rows[0] = (typeof(rows[0])){"Channel", chan_val, ch->color};

    char from_val[SENDER_MAX + 24];
    snprintf(from_val, sizeof(from_val), "%s%s", msg->sender, msg->sender_named ? "" : "  (no NodeInfo yet)");
    rows[1] = (typeof(rows[0])){"From", from_val, msg->sender_named ? COL_FROM : COL_FROM_ID};
    rows[2] = (typeof(rows[0])){"Received", when, COL_TEXT};

    char radio_val[48];
    if (msg->outgoing) {
        const char* state = msg->tx == TX_CONFIRMED ? "delivered" : msg->tx == TX_FAILED ? "no repeat heard"
                            : msg->tx == TX_AWAITING ? "waiting"   : msg->tx == TX_SENDING ? "on air" : "queued";
        snprintf(radio_val, sizeof(radio_val), "%s, %u repeat(s)", state, (unsigned)msg->repeats);
    } else {
        snprintf(radio_val, sizeof(radio_val), "%d dBm  %d.%02d dB  %u hop(s)", msg->rssi_dbm, msg->snr_db_x4 / 4,
                 (msg->snr_db_x4 < 0 ? -msg->snr_db_x4 : msg->snr_db_x4) % 4 * 25, (unsigned)msg->hops);
    }
    rows[3] = (typeof(rows[0])){msg->outgoing ? "Delivery" : "Radio", radio_val,
                                msg->tx == TX_FAILED ? COL_BAD : COL_TEXT};

    for (int i = 0; i < 4; i++, y += LINE_H) {
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, rows[i].label);
        pax_draw_text(&fb, rows[i].color, FONT, FONT_SIZE, vx, y, rows[i].value);
    }

    // Where the protocol carries the sender's own clock, show it beside ours.
    // A large disagreement means that node's time is wrong, which is worth
    // seeing rather than hiding behind our own timestamp.
    if (msg->sender_timestamp > 0) {
        char      claimed[40] = "not set";
        struct tm tm_buf;
        if (msg->sender_timestamp > 1000000000u) {
            time_t t = (time_t)msg->sender_timestamp;
            localtime_r(&t, &tm_buf);
            strftime(claimed, sizeof(claimed), "%a %d %b %H:%M:%S", &tm_buf);
        }
        // Off by more than a few minutes and the remote clock is the problem.
        int32_t   skew  = (int32_t)msg->sender_timestamp - (int32_t)msg->timestamp;
        pax_col_t color = (skew > 300 || skew < -300) ? COL_BAD : COL_DIM;
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Sender");
        pax_draw_text(&fb, color, FONT, FONT_SIZE, vx, y, claimed);
        y += LINE_H;
    }

    // Routing differs by network: MeshCore records the path actually taken,
    // Meshtastic only a hop budget and the node that last relayed.
    char route_val[56];
    if (model->active == MESH_MC) {
        if (msg->path_len == 0) {
            snprintf(route_val, sizeof(route_val), "direct");
        } else {
            // One entry per hop, not per byte. A hop is one, two or three bytes
            // of a repeater's key depending on what the sender used, so stepping
            // a byte at a time would split every repeater on a two-byte mesh in
            // half and double the apparent distance.
            uint8_t width = msg->path_hash_size ? msg->path_hash_size : 1;
            int     n     = 0;
            for (int i = 0; i + width <= msg->path_len && n < (int)sizeof(route_val) - 12; i += width) {
                if (i) n += snprintf(route_val + n, sizeof(route_val) - n, ">");
                for (int b = 0; b < width; b++) {
                    n += snprintf(route_val + n, sizeof(route_val) - n, "%02x", msg->path[i + b]);
                }
            }
            // The hop count comes from the header, so it stays right even when
            // the route was too long to keep in full.
            snprintf(route_val + n, sizeof(route_val) - n, "%s (%u)", msg->path_truncated ? ">..." : "",
                     (unsigned)msg->hops);
        }
    } else {
        snprintf(route_val, sizeof(route_val), "%u of %u used", (unsigned)(msg->hop_start - msg->hop_limit),
                 (unsigned)msg->hop_start);
    }
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, model->active == MESH_MC ? "Path" : "Hops");
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, route_val);
    y += LINE_H;

    if (msg->relayed_by[0]) {
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Relay");
        pax_draw_text(&fb, COL_FROM_ID, FONT, FONT_SIZE, vx, y, msg->relayed_by);
        y += LINE_H;
    }

    char size_val[32];
    snprintf(size_val, sizeof(size_val), "%u bytes", (unsigned)strlen(msg->text));
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Size");
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, size_val);
    y += LINE_H;

    pax_draw_line(&fb, COL_SEP, bx + 12, y + LINE_H / 2, bx + bw - 12, y + LINE_H / 2);
    y += LINE_H;

    // The text again, wrapped to the box, so a long message can be read in full.
    int   room = (int)((bw - 28) / CHAR_W);
    int   pos  = 0;
    for (int l = 0; l < 3 && msg->text[pos]; l++, y += LINE_H) {
        int scan = pos, cells = 0, last_space = -1;
        while (msg->text[scan] && cells < room) {
            if (msg->text[scan] == ' ') last_space = scan;
            scan += utf8_adv((unsigned char)msg->text[scan]);
            cells++;
        }
        int end = scan;
        if (msg->text[scan] && last_space > pos) end = last_space;

        char seg[TEXT_MAX];
        int  len = end - pos;
        if (len > (int)sizeof(seg) - 1) len = sizeof(seg) - 1;
        memcpy(seg, &msg->text[pos], len);
        seg[len] = '\0';
        pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, bx + 14, y, seg);

        pos = end;
        while (msg->text[pos] == ' ') pos++;
    }

    hint(bx + 12, by + bh - LINE_H - 8, CAP_CROSS, "close");
}

static void draw_confirm(const app_model_t* model) {
    float bw = 44 * CHAR_W;
    float bh = LINE_H * 4 + 16;
    float bx, by;
    overlay_box(bw, bh, &bx, &by, COL_BAD, "Confirm");

    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, bx + 14, by + 8 + LINE_H * 1.5f, model->confirm_text);

    float hx = bx + 12;
    float y  = by + bh - LINE_H - 8;
    hx       = hint(hx, y, CAP_CROSS, "cancel");
    hint_text(hx, y, "enter delete");
}

// --- nodes ---------------------------------------------------------------

// "3m", "4h", "6d" -- an absolute time is useless for judging whether a node is
// still out there, and this fits a narrow column.
static void format_age(char* out, size_t out_size, uint32_t last_heard, uint32_t now) {
    if (last_heard == 0 || now < last_heard) {
        snprintf(out, out_size, "  -");
        return;
    }
    uint32_t age = now - last_heard;
    if (age < 60) {
        snprintf(out, out_size, "%2lus", (unsigned long)age);
    } else if (age < 3600) {
        snprintf(out, out_size, "%2lum", (unsigned long)(age / 60));
    } else if (age < 86400) {
        snprintf(out, out_size, "%2luh", (unsigned long)(age / 3600));
    } else {
        snprintf(out, out_size, "%2lud", (unsigned long)(age / 86400));
    }
}

// The label a node is known by, falling back through what we actually have.
static void node_label(const app_model_t* model, const node_t* node, char* out, size_t out_size) {
    if (node->long_name[0]) {
        snprintf(out, out_size, "%s", node->long_name);
    } else if (node->short_name[0]) {
        snprintf(out, out_size, "%s", node->short_name);
    } else if (model->active == MESH_MT) {
        snprintf(out, out_size, "!%08lx", (unsigned long)node->node_num);
    } else {
        // MeshCore identifies nodes by public key; a prefix is what other
        // clients show too.
        snprintf(out, out_size, "%02x%02x%02x%02x...", node->key[0], node->key[1], node->key[2], node->key[3]);
    }
}

static void draw_nodes(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];

    int order[MAX_NODES];
    int count = model_nodes_by_recency(mesh, order, MAX_NODES);

    float bw   = 50 * CHAR_W;
    int   rows = count < 12 ? count : 12;
    float bh   = LINE_H * (rows + 6) + 12;
    float bx, by;

    char title[40];
    snprintf(title, sizeof(title), "Nodes - %s", mesh->name);
    overlay_box(bw, bh, &bx, &by, mesh->accent, title);

    float y   = by + 8 + LINE_H * 1.5f;
    uint32_t now = (uint32_t)time(NULL);

    // This radio, always first: it is the entry the user is most likely to want
    // and it is not something we ever hear over the air.
    if (model->node_index < 0) pax_draw_rect(&fb, COL_SEL, bx + 6, y - 2, bw - 12, LINE_H);
    pax_draw_text(&fb, COL_OK, FONT, FONT_SIZE, bx + 14, y, "this radio");
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, bx + 14 + 12 * CHAR_W, y,
                  model->identity.name[0] ? model->identity.name : "(no name)");
    y += LINE_H;
    pax_draw_line(&fb, COL_SEP, bx + 10, y - 2, bx + bw - 10, y - 2);
    y += 2;

    if (count == 0) {
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "nothing heard yet");
        y += LINE_H;
    }

    // Scroll the window so the selection stays visible.
    int first = 0;
    if (model->node_index >= rows) first = model->node_index - rows + 1;

    for (int i = first; i < count && i < first + rows; i++, y += LINE_H) {
        const node_t* node = &mesh->nodes[order[i]];
        if (i == model->node_index) pax_draw_rect(&fb, COL_SEL, bx + 6, y - 2, bw - 12, LINE_H);

        char label[NODE_NAME_MAX + 8];
        node_label(model, node, label, sizeof(label));
        pax_draw_text(&fb, node->named ? COL_FROM : COL_FROM_ID, FONT, FONT_SIZE, bx + 14, label[0] ? y : y,
                      label);

        // One character between name and age for the signature verdict. Only a
        // proven name or a failed check is worth the ink; unchecked is the
        // normal state and says nothing.
        if (model->active == MESH_MC) {
            const char* mark = NULL;
            pax_col_t   col  = COL_OK;
            if (node->verified == NODE_VERIFY_VALID) {
                mark = "*";
            } else if (node->verified == NODE_VERIFY_BAD) {
                mark = "!";
                col  = COL_BAD;
            }
            if (mark) pax_draw_text(&fb, col, FONT, FONT_SIZE, bx + bw - 12 - 5 * CHAR_W, y, mark);
        }

        char age[8];
        format_age(age, sizeof(age), node->last_heard, now);
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + bw - 12 - 3 * CHAR_W, y, age);
    }

    y       += 6;
    float hx = bx + 12;
    hx       = hint(hx, y, CAP_CROSS, "close");
    hx       = hint(hx, y, CAP_TRI, "announce");
    hx       = hint(hx, y, CAP_SQUARE, "clear all");
    hint_text(hx, y, "enter details");
}

static void draw_node_detail(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];

    int order[MAX_NODES];
    int count = model_nodes_by_recency(mesh, order, MAX_NODES);
    if (model->node_index < 0 || model->node_index >= count) return;
    const node_t* node = &mesh->nodes[order[model->node_index]];

    float bw = 52 * CHAR_W;
    float bh = LINE_H * 13 + 16;
    float bx, by;
    overlay_box(bw, bh, &bx, &by, mesh->accent, "Node");

    float y  = by + 8 + LINE_H * 1.5f;
    float vx = bx + 14 + 10 * CHAR_W;

    char label[NODE_NAME_MAX + 8];
    node_label(model, node, label, sizeof(label));
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Name");
    pax_draw_text(&fb, node->named ? COL_FROM : COL_FROM_ID, FONT, FONT_SIZE, vx, y, label);
    y += LINE_H;

    if (model->active == MESH_MT) {
        char id[16];
        snprintf(id, sizeof(id), "!%08lx", (unsigned long)node->node_num);
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Node id");
        pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, id);
        y += LINE_H;
    } else {
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Role");
        pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, mc_role_name((mc_role_t)node->role));
        y += LINE_H;
    }

    // Whether a private message to this node is even possible. Meshtastic
    // encrypts them to the recipient's key and current firmware will not send
    // one without it, so a missing key is not a detail -- it is the difference
    // between the conversation working and being refused.
    if (model->active == MESH_MT) {
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Key");
        if (node->has_public_key) {
            char hex[24];
            snprintf(hex, sizeof(hex), "%02x%02x%02x%02x...", node->public_key[0], node->public_key[1],
                     node->public_key[2], node->public_key[3]);
            pax_draw_text(&fb, COL_OK, FONT, FONT_SIZE, vx, y, hex);
        } else {
            pax_draw_text(&fb, COL_WARN, FONT, FONT_SIZE, vx, y, "none - exchange info first");
        }
        y += LINE_H;
    }

    // What the message column will show for this node, and where it came from.
    // A Meshtastic node publishes its own; a MeshCore one has to be given one.
    char shown[NODE_SHORT_MAX + 1];
    if (node->short_name[0]) {
        snprintf(shown, sizeof(shown), "%s", node->short_name);
    } else {
        utf8_clip(shown, sizeof(shown), label, COL_FROM_CELLS);
    }
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Short");
    pax_draw_text(&fb, node->short_name[0] ? COL_TEXT : COL_DIM, FONT, FONT_SIZE, vx, y, shown);
    if (!node->short_name[0]) {
        pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, vx + 6 * CHAR_W, y, "(from name)");
    }
    y += LINE_H;

    // The key is the identity on MeshCore and the DM key on Meshtastic; either
    // way a prefix is enough to recognise it and the full thing will not fit.
    if (node->has_public_key || model->active == MESH_MC) {
        const uint8_t* key = model->active == MESH_MC ? node->key : node->public_key;
        char           hex[40];
        snprintf(hex, sizeof(hex), "%02x%02x%02x%02x %02x%02x%02x%02x ...", key[0], key[1], key[2], key[3], key[4],
                 key[5], key[6], key[7]);
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Key");
        pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, hex);
        y += LINE_H;
    }

    char when[40] = "never";
    if (node->last_heard > 1000000000u) {
        time_t    t = (time_t)node->last_heard;
        struct tm tm_buf;
        localtime_r(&t, &tm_buf);
        strftime(when, sizeof(when), "%a %d %b %H:%M:%S", &tm_buf);
    }
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Last heard");
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, when);
    y += LINE_H;

    // No RSSI: the coprocessor reports zero for it on every packet, so printing
    // it would be inventing a measurement.
    char radio[48];
    snprintf(radio, sizeof(radio), "%d.%02d dB SNR  %u hop(s)", node->snr_db_x4 / 4,
             (node->snr_db_x4 < 0 ? -node->snr_db_x4 : node->snr_db_x4) % 4 * 25, (unsigned)node->hops);
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Signal");
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, radio);
    y += LINE_H;

    // Whether we know a way to reach this node without flooding the mesh. Shown
    // because it is the difference between a message costing one packet and
    // costing every repeater in range a retransmission.
    if (model->active == MESH_MC) {
        char route[52];
        if (!node->has_out_path) {
            snprintf(route, sizeof(route), "flood");
        } else {
            uint8_t width = MC_PATH_HASH_SIZE(node->out_path_ctrl);
            uint8_t hops  = MC_PATH_COUNT(node->out_path_ctrl);
            int     n     = snprintf(route, sizeof(route), "%u hop%s  ", (unsigned)hops, hops == 1 ? "" : "s");
            for (int i = 0; i + width <= MC_PATH_BYTES(node->out_path_ctrl) && n < (int)sizeof(route) - 8;
                 i += width) {
                if (i) n += snprintf(route + n, sizeof(route) - n, ">");
                for (int b = 0; b < width; b++) {
                    n += snprintf(route + n, sizeof(route) - n, "%02x", node->out_path[i + b]);
                }
            }
        }
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Route");
        pax_draw_text(&fb, node->has_out_path ? COL_OK : COL_DIM, FONT, FONT_SIZE, vx, y, route);
        y += LINE_H;
    }

    // What the advert signature proved, if anything. Only MeshCore can answer:
    // a Meshtastic NodeInfo is unsigned, so there is nothing to report and
    // saying "unverified" there would imply a check that could have been made.
    if (model->active == MESH_MC) {
        const char* verdict = NULL;
        pax_col_t   col     = COL_DIM;
        switch ((node_verify_t)node->verified) {
            case NODE_VERIFY_VALID:
                verdict = "signature verified";
                col     = COL_OK;
                break;
            case NODE_VERIFY_BAD:
                // Deliberately not "forged". A single failed check does not
                // prove one: the frame's CRC is sixteen bits and not
                // cryptographic, so a corrupted advert reaches us looking
                // intact and fails the signature for an entirely innocent
                // reason. That is what happened the one time this fired, and
                // the next advert cleared it. Say what was observed and that
                // it will be tried again, rather than accusing anyone.
                verdict = "signature failed - rechecked on next advert";
                col     = COL_WARN;
                break;
            case NODE_VERIFY_PENDING: verdict = "checking signature..."; break;
            default: verdict = "signature not checked"; break;
        }
        pax_draw_text(&fb, col, FONT, FONT_SIZE, bx + 14, y, verdict);
        y += LINE_H;
    }

    float hx = bx + 12;
    float hy = by + bh - LINE_H - 8;
    hx       = hint(hx, hy, CAP_CROSS, "back");
    hx       = hint(hx, hy, CAP_TRI, "message");
    hx       = hint(hx, hy, CAP_CIRCLE, "short");
    hx       = hint(hx, hy, CAP_SQUARE, "remove");
    // Only offered where they mean something, both to keep the bar short and
    // because an action that does nothing is worse than an absent one.
    if (model->active == MESH_MC && node->has_out_path) {
        hint(hx, hy, CAP_CLOUD, "forget route");
    } else if (model->active == MESH_MT) {
        hint(hx, hy, CAP_DIAMOND, "exchange info");
    }
}

static void draw_node_short(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];

    int order[MAX_NODES];
    int count = model_nodes_by_recency(mesh, order, MAX_NODES);
    if (model->node_index < 0 || model->node_index >= count) return;
    const node_t* node = &mesh->nodes[order[model->node_index]];

    float bw = 46 * CHAR_W;
    float bh = LINE_H * 7 + 16;
    float bx, by;
    overlay_box(bw, bh, &bx, &by, mesh->accent, "Short name");

    float y = by + 8 + LINE_H * 1.5f;

    char full[NODE_NAME_MAX + 1];
    model_node_label(node, model->active, full, sizeof(full));
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Node");
    pax_draw_text(&fb, COL_FROM, FONT, FONT_SIZE, bx + 14 + 8 * CHAR_W, y, full);
    y += LINE_H * 1.5f;

    float vx = bx + 14 + 8 * CHAR_W;
    pax_draw_rect(&fb, COL_SEL, bx + 6, y - 2, bw - 12, LINE_H);
    pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, "Short");
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, vx, y, model->node_short_edit);
    pax_draw_rect(&fb, COL_TEXT, vx + (float)strlen(model->node_short_edit) * CHAR_W, y + 3, 2, LINE_H - 6);
    y += LINE_H;

    // Say what leaving it empty does, so clearing the field is an obvious way
    // back to the default rather than something to be avoided.
    char note[52];
    // Four cells, but up to four bytes each: the fallback is exactly what the
    // message column would show, Finnish letters and all.
    char preview[COL_FROM_CELLS * 4 + 1];
    utf8_clip(preview, sizeof(preview), full, COL_FROM_CELLS);
    snprintf(note, sizeof(note), "empty uses \"%s\"", preview);
    pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, bx + 14, y, note);

    y       += LINE_H + 4;
    float hx = bx + 12;
    hx       = hint(hx, y, CAP_CROSS, "cancel");
    hint_text(hx, y, "enter save");
}

static void setting_row(const app_model_t* model, setting_field_t field, char* label, size_t label_size, char* value,
                        size_t value_size, pax_col_t* col) {
    const settings_t* s = &model->settings;
    *col                = COL_TEXT;

    switch (field) {
        case SET_FIELD_LATITUDE:
            snprintf(label, label_size, "Latitude");
            snprintf(value, value_size, "%s", model->lat_text[0] ? model->lat_text : "not set");
            if (!model->lat_text[0]) *col = COL_DIM;
            break;
        case SET_FIELD_LONGITUDE:
            snprintf(label, label_size, "Longitude");
            snprintf(value, value_size, "%s", model->lon_text[0] ? model->lon_text : "not set");
            if (!model->lon_text[0]) *col = COL_DIM;
            break;
        case SET_FIELD_DISPLAY_OFF:
            snprintf(label, label_size, "Screen off");
            if (s->display_off_minutes == 0) {
                snprintf(value, value_size, "never");
                *col = COL_DIM;
            } else {
                snprintf(value, value_size, "%u min", (unsigned)s->display_off_minutes);
            }
            break;
        case SET_FIELD_MT_HOPS:
            snprintf(label, label_size, "Hop limit");
            // Both numbers, because they differ whenever a session has raised
            // it and only one of them survives a restart.
            if (model->mt_active_hops != s->mt_default_hops) {
                snprintf(value, value_size, "%u  (now %u)", (unsigned)s->mt_default_hops,
                         (unsigned)model->mt_active_hops);
            } else {
                snprintf(value, value_size, "%u", (unsigned)s->mt_default_hops);
            }
            break;
        case SET_FIELD_MT_ROLE:
            snprintf(label, label_size, "Role");
            snprintf(value, value_size, "%s", s->mt_role == MT_ROLE_CLIENT ? "CLIENT" : "CLIENT_MUTE");
            // Green for the role that carries traffic for others, matching the
            // two relay rows below it.
            *col = s->mt_role == MT_ROLE_CLIENT ? COL_OK : COL_DIM;
            break;
        case SET_FIELD_MT_ALWAYS_REPEAT:
            snprintf(label, label_size, "Always repeat");
            snprintf(value, value_size, "%s", s->mt_always_repeat ? "on" : "off");
            *col = s->mt_always_repeat ? COL_OK : COL_DIM;
            break;
        case SET_FIELD_MT_OPTIMIZE:
            snprintf(label, label_size, "Optimize text");
            snprintf(value, value_size, "%s", s->mt_optimize_text ? "on" : "off");
            *col = s->mt_optimize_text ? COL_OK : COL_DIM;
            break;
        case SET_FIELD_MC_REPEATER:
            snprintf(label, label_size, "Off-grid repeat");
            snprintf(value, value_size, "%s", s->mc_repeater ? "on" : "off");
            *col = s->mc_repeater ? COL_OK : COL_DIM;
            break;
        default: break;
    }
}

static void draw_settings(const app_model_t* model) {
    const mesh_state_t* mesh = &model->mesh[model->active];

    setting_field_t fields[SET_FIELD_COUNT];
    int             count = settings_visible_fields(model->active, &model->settings, fields, SET_FIELD_COUNT);

    float bw = 52 * CHAR_W;
    float bh = LINE_H * (count + 8) + 12;
    float bx, by;

    char title[40];
    snprintf(title, sizeof(title), "Settings - %s", mesh->name);
    overlay_box(bw, bh, &bx, &by, mesh->accent, title);

    float y = by + 8 + LINE_H * 1.5f;
    // Wide enough for the longest label with a gap after it. The values are all
    // short -- "CLIENT_MUTE" and a signed coordinate are the biggest -- so the
    // column can afford to fit the names rather than the names being cut to fit
    // the column.
    float vx = bx + 14 + 17 * CHAR_W;

    for (int i = 0; i < count; i++, y += LINE_H) {
        // The network's own settings are separated from the shared ones, so it
        // is never a guess which of the two a row belongs to.
        if (fields[i] == SET_FIELD_MT_HOPS || fields[i] == SET_FIELD_MC_REPEATER) {
            pax_draw_line(&fb, COL_SEP, bx + 10, y - 3, bx + bw - 10, y - 3);
        }
        if (i == model->setting_index) pax_draw_rect(&fb, COL_SEL, bx + 6, y - 2, bw - 12, LINE_H);

        char      label[24], value[40];
        pax_col_t col;
        setting_row(model, fields[i], label, sizeof(label), value, sizeof(value), &col);
        pax_draw_text(&fb, COL_DIM, FONT, FONT_SIZE, bx + 14, y, label);
        pax_draw_text(&fb, col, FONT, FONT_SIZE, vx, y, value);

        // A cursor on the coordinate rows, because those are typed rather than
        // stepped and otherwise look inert.
        if (i == model->setting_index &&
            (fields[i] == SET_FIELD_LATITUDE || fields[i] == SET_FIELD_LONGITUDE)) {
            const char* txt = fields[i] == SET_FIELD_LATITUDE ? model->lat_text : model->lon_text;
            pax_draw_rect(&fb, COL_TEXT, vx + (float)strlen(txt) * CHAR_W, y + 3, 2, LINE_H - 6);
        }
    }

    y += 4;
    // What the selected row actually does, since several of these are not
    // obvious and one of them transmits your location to strangers.
    const char* note = "";
    switch (fields[model->setting_index < count ? model->setting_index : 0]) {
        case SET_FIELD_LATITUDE:
        case SET_FIELD_LONGITUDE: note = "sent in every MeshCore advert once set"; break;
        case SET_FIELD_DISPLAY_OFF: note = "backlight only - radio keeps running"; break;
        case SET_FIELD_MT_HOPS: note = "resets here at start; fn+0..7 for now"; break;
        case SET_FIELD_MT_ROLE: note = "CLIENT forwards for others, MUTE listens only"; break;
        case SET_FIELD_MT_ALWAYS_REPEAT: note = "repeat last, even if another node already did"; break;
        case SET_FIELD_MT_OPTIMIZE: note = "carry text and acks only, and keep their hops"; break;
        case SET_FIELD_MC_REPEATER: note = "forward others' packets to extend the mesh"; break;
        default: break;
    }
    pax_draw_text(&fb, COL_SEP, FONT, FONT_SIZE, bx + 14, y, note);

    y       += LINE_H + 4;
    float hx = bx + 12;
    hx       = hint(hx, y, CAP_CROSS, "cancel");
    hint_text(hx, y, "up/dn  left/right  enter save");
}

static void draw_toast(const app_model_t* model) {
    if (model->toast[0] == '\0') return;

    float w = (float)strlen(model->toast) * CHAR_W + 20;
    float x = (ui_w - w) / 2;
    float y = msg_bottom - LINE_H - 10;

    pax_draw_round_rect(&fb, 0xFF262634, x, y, w, LINE_H + 6, 4);
    pax_draw_text(&fb, COL_TEXT, FONT, FONT_SIZE, x + 10, y + 3, model->toast);
}

void ui_render(const app_model_t* model) {
    if (!have_display) return;

    pax_background(&fb, COL_BG);
    draw_status(model);
    draw_messages(model);
    draw_toast(model);
    draw_hints(model);
    draw_composer(model);

    switch (model->overlay) {
        case OVERLAY_PICKER: draw_picker(model); break;
        case OVERLAY_EDITOR: draw_editor(model); break;
        case OVERLAY_IDENTITY: draw_identity(model); break;
        case OVERLAY_DETAIL: draw_detail(model); break;
        case OVERLAY_CONFIRM: draw_confirm(model); break;
        case OVERLAY_NODES: draw_nodes(model); break;
        case OVERLAY_NODE_DETAIL: draw_node_detail(model); break;
        case OVERLAY_NODE_SHORT: draw_node_short(model); break;
        case OVERLAY_SETTINGS: draw_settings(model); break;
        default: break;
    }
    blit();
}
