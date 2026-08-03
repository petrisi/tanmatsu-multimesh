// SPDX-License-Identifier: MIT

#include "ui.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bsp/display.h"
#include "esp_log.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"

static const char TAG[] = "ui";

#define COL_BG     0xFF101018
#define COL_TEXT   0xFFE8E8E8
#define COL_DIM    0xFF7A7A8A
#define COL_ACCENT 0xFF66D9A0
#define COL_WARN   0xFFE0C060
#define COL_NOTICE 0xFF80B0F0

#define FONT_SIZE  16
#define LINE_H     18
#define MAX_MSGS   64
#define TEXT_MAX   192

typedef struct {
    bool     used;
    bool     notice;  // local status line rather than received traffic
    char     tag[4];
    uint32_t timestamp;
    char     text[TEXT_MAX];
    int      rssi_dbm;
    int      snr_db_x4;
    uint8_t  hops;
} ui_msg_t;

static pax_buf_t fb           = {0};
static size_t    h_res        = 0;
static size_t    v_res        = 0;
static bool      have_display = false;

// Ring buffer; `head` is the next slot to write.
static ui_msg_t msgs[MAX_MSGS];
static int      head  = 0;
static int      count = 0;

static char header[160] = "";

static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, h_res, v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "blit failed: %d", res);
    }
}

bool ui_init(void) {
    bsp_display_color_format_t color_format = 0;
    bsp_display_endianness_t   endianness   = 0;

    esp_err_t res = bsp_display_get_parameters(&h_res, &v_res, &color_format, &endianness);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "no display (%d)", res);
        return false;
    }

    // The BSP is asked for 16-bit 565RGB in main.c, so this mapping is the only
    // one exercised; anything else is a configuration mistake worth shouting about.
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

    have_display = true;
    ESP_LOGI(TAG, "display %ux%u fmt=%d", (unsigned)h_res, (unsigned)v_res, format);
    return true;
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
    pax_draw_text(&fb, COL_TEXT, pax_font_sky_mono, FONT_SIZE, 8, 8, "MeshComms PoC");
    pax_draw_text(&fb, COL_ACCENT, pax_font_sky_mono, FONT_SIZE, 8, 8 + LINE_H * 2, line);
    blit();
}

void ui_set_header(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(header, sizeof(header), fmt, args);
    va_end(args);
}

static ui_msg_t* next_slot(void) {
    ui_msg_t* slot = &msgs[head];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    head       = (head + 1) % MAX_MSGS;
    if (count < MAX_MSGS) count++;
    return slot;
}

void ui_add_message(const char* tag, uint32_t timestamp, const char* text, int rssi_dbm, int snr_db_x4, uint8_t hops) {
    ui_msg_t* slot = next_slot();
    snprintf(slot->tag, sizeof(slot->tag), "%s", tag ? tag : "??");
    slot->timestamp = timestamp;
    slot->rssi_dbm  = rssi_dbm;
    slot->snr_db_x4 = snr_db_x4;
    slot->hops      = hops;
    snprintf(slot->text, sizeof(slot->text), "%s", text ? text : "");
}

void ui_add_notice(const char* text) {
    ui_msg_t* slot = next_slot();
    slot->notice   = true;
    snprintf(slot->text, sizeof(slot->text), "%s", text ? text : "");
    ESP_LOGI(TAG, "%s", slot->text);
}

void ui_render(const ui_stats_t* stats) {
    if (!have_display) return;

    pax_background(&fb, COL_BG);

    int y = 6;
    pax_draw_text(&fb, COL_ACCENT, pax_font_sky_mono, FONT_SIZE, 8, y, header);
    y += LINE_H;

    char line[256];
    snprintf(line, sizeof(line), "rx:%lu bad:%lu other-ch:%lu msgs:%lu  %.72s", (unsigned long)stats->packets_total,
             (unsigned long)stats->packets_bad, (unsigned long)stats->not_our_channel,
             (unsigned long)stats->messages, stats->detail);
    pax_draw_text(&fb, COL_DIM, pax_font_sky_mono, FONT_SIZE, 8, y, line);
    y += LINE_H;

    pax_draw_text(&fb, COL_DIM, pax_font_sky_mono, FONT_SIZE, 8, y, "F1 launcher   F2 switch network");
    y += LINE_H + 4;

    pax_draw_line(&fb, COL_DIM, 8, y, (float)h_res - 8, y);
    y += 6;

    // How many rows fit below the header, and therefore how far back to start.
    int rows = ((int)v_res - y - 6) / LINE_H;
    if (rows < 1) rows = 1;
    int show = count < rows ? count : rows;

    for (int i = 0; i < show; i++) {
        // Oldest of the visible window first, so new messages appear at the bottom.
        int       idx = (head - show + i + MAX_MSGS * 2) % MAX_MSGS;
        ui_msg_t* m   = &msgs[idx];
        if (!m->used) continue;

        if (m->notice) {
            snprintf(line, sizeof(line), "-- %.180s", m->text);
            pax_draw_text(&fb, COL_NOTICE, pax_font_sky_mono, FONT_SIZE, 8, y, line);
            y += LINE_H;
            continue;
        }

        char stamp[16] = "--:--:--";
        if (m->timestamp > 1000000000u) {
            time_t    t = (time_t)m->timestamp;
            struct tm tm_buf;
            localtime_r(&t, &tm_buf);
            strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_buf);
        }

        // Integer arithmetic on purpose: a %f here makes the width unbounded as
        // far as the compiler is concerned, which trips -Werror=format-truncation.
        int snr_whole = m->snr_db_x4 / 4;
        int snr_frac  = (m->snr_db_x4 < 0 ? -m->snr_db_x4 : m->snr_db_x4) % 4 * 25;

        snprintf(line, sizeof(line), "%.2s %s [%4ddBm %d.%02ddB %uh] %.150s", m->tag, stamp, m->rssi_dbm, snr_whole,
                 snr_frac, (unsigned)m->hops, m->text);
        pax_draw_text(&fb, COL_TEXT, pax_font_sky_mono, FONT_SIZE, 8, y, line);
        y += LINE_H;
    }

    if (count == 0) {
        pax_draw_text(&fb, COL_WARN, pax_font_sky_mono, FONT_SIZE, 8, y, "Listening. Nothing decoded yet.");
    }

    blit();
}
