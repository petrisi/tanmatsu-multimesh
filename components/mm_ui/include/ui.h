// SPDX-License-Identifier: MIT
//
// All drawing for the UI prototype. Pure presentation: it reads the model and
// never mutates it.

#pragma once

#include <stdbool.h>
#include "app_model.h"

// Bring up the framebuffer. False if this board has no display.
bool ui_init(void);

// Full-screen status line, used before the main UI exists.
void ui_boot_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Repaint everything.
void ui_render(const app_model_t* model);

// Rows of message list currently visible; the event loop needs it to clamp
// scrolling.
int ui_visible_rows(void);

// Total wrapped display lines for the active mesh. Scrolling counts display
// lines, not messages, because one message can occupy several.
int ui_line_count(const app_model_t* model);

// Scrolling is expressed as a line index into the wrapped view; the model
// stores it as a message anchor so arriving traffic cannot drag the viewport.
int  ui_anchor_index(const app_model_t* model);
void ui_set_anchor(app_model_t* model, int line_index);

// Line index of a message's first row, or -1 if it is not in the ring. Used to
// keep the selection on screen.
int ui_line_of_seq(const app_model_t* model, uint32_t seq);

// Rows in the send-to picker: channels followed by contacts. The event loop
// needs it to wrap the selection, and the split point is the channel count.
int ui_picker_count(const app_model_t* model);
