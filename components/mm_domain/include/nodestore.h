// SPDX-License-Identifier: MIT
//
// Node tables live on the internal FAT partition, not in NVS.
//
// NVS is 16 KB and shared with the launcher, WiFi credentials and every other
// installed app. Two tables of node records would take a third of it, and when
// NVS fills it is the *launcher* that stops being able to save -- a failure with
// no visible connection to us. The `locfd` partition is 3968 KB and ours to use.
//
// Two consequences worth knowing:
//
//   The partition is never formatted, even if it fails to mount. The launcher
//   keeps its icons there, so reformatting after a hiccup would destroy data we
//   do not own. If it will not mount we keep nodes in RAM and say so.
//
//   Writes are debounced. `last_heard` changes on every packet, and writing the
//   table each time would hammer the flash for no benefit.

#pragma once

#include <stdbool.h>
#include "app_model.h"

// Mount the partition. False means no persistence this session; the app still
// works, it just forgets nodes on reboot.
bool nodestore_init(void);

bool nodestore_ready(void);

// Load both networks' tables. Missing or unreadable files are not an error --
// a first run has none.
void nodestore_load(app_model_t* model);

// Note that something changed. Cheap; the actual write happens in the flush.
void nodestore_mark_dirty(mesh_id_t mesh);

// Write any dirty table if enough time has passed, or immediately when `force`
// is set (on exit). Returns true if anything was written.
bool nodestore_flush(const app_model_t* model, bool force);
