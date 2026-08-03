// SPDX-License-Identifier: MIT
//
// Screen output. The PoC deliberately renders everything on-device rather than
// relying on printf: the USB port is either in debug mode or BadgeLink mode,
// never both, so a serial monitor is not available while deploying.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Counters are per-network; `detail` is a short free-form breakdown each stack
// fills in with whatever is diagnostic for it (payload types, portnums, ...).
typedef struct {
    uint32_t packets_total;    // frames handed up by the radio
    uint32_t packets_bad;      // failed to parse as this network's framing
    uint32_t not_our_channel;  // parsed, but wrong channel hash or failed MAC
    uint32_t messages;         // decoded, displayable messages
    char     detail[72];
} ui_stats_t;

// Bring up the framebuffer against whatever geometry the BSP reports.
// Returns false if this board has no display.
bool ui_init(void);

// Full-screen status line, used during boot before the message list exists.
void ui_boot_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Header text shown above the message list (active network + radio settings).
void ui_set_header(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Append a message to the shared scrollback. `tag` identifies the network so
// both stacks' traffic can share one list.
void ui_add_message(const char* tag, uint32_t timestamp, const char* text, int rssi_dbm, int snr_db_x4, uint8_t hops);

// Append a local notice (network switched, radio error) to the same list.
void ui_add_notice(const char* text);

// Repaint the message list plus the traffic counters.
void ui_render(const ui_stats_t* stats);
