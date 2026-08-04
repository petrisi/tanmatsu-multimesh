// SPDX-License-Identifier: MIT
//
// Session logging, for diagnosing behaviour that only appears on a real mesh.
//
// Faults like "acknowledgements arrive from the node next door but not from the
// one three hops away" cannot be reproduced at a desk, and the screen shows only
// the conclusion, never the evidence. This records both: every frame in and out
// verbatim, and the decisions taken about them, so a session can be read back
// afterwards and the step that went wrong identified rather than guessed at.
//
// Off by default and never enabled on its own. It costs flash writes and
// airtime-proportional storage, neither of which anyone should pay for by
// accident.
//
// Written by a background task. The event loop only ever appends to a buffer in
// PSRAM, because a FAT write takes tens of milliseconds and the receive path
// runs where the interface is drawn.

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Prepare the buffer and writer task. Does not start logging. False if the
// buffer could not be allocated, in which case logging stays unavailable.
bool session_log_init(void);

bool session_log_active(void);

// Start or stop. Starting truncates the previous session: two sessions in one
// file is harder to read than one, and the interesting one is always the last.
// Returns the new state.
bool session_log_toggle(void);

// One line. A newline is appended; do not include one.
void session_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// A frame, verbatim, as hex. `dir` is "rx" or "tx"; extra is appended as-is
// before the bytes, and may be NULL.
void session_log_frame(const char* dir, const char* net, const char* extra, const uint8_t* data, size_t len);

// Bytes written this session, and lines lost because the buffer was full. A
// non-zero drop count means the log has holes and cannot be read as complete.
uint32_t session_log_bytes(void);
uint32_t session_log_dropped(void);

// Where the file lands, for the retrieval tooling and for saying so on screen.
#define SESSION_LOG_PATH "/locfd/multimesh/session.log"
