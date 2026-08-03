// SPDX-License-Identifier: MIT
//
// TEMPORARY -- development scaffolding, not part of the product.
//
// Stands in for the radio so the behaviours that only appear with live traffic
// stay reviewable while the transmit path is being built: the viewport holding
// still while scrolled away, the unseen counter, the message LED.
//
// Delete this file, its .c, and the MM_DEMO_TRAFFIC guard once the real RX task
// feeds mesh_net_t.handle(). Nothing else should come to depend on it.

#pragma once

#include <stdbool.h>
#include "app_model.h"

// The single switch. Set to 0 for a build that only ever shows real traffic.
#define MM_DEMO_TRAFFIC 1

#if MM_DEMO_TRAFFIC

// What happened this tick. Reported rather than acted on: the domain layer has
// no business calling the LEDs, which live above it.
typedef struct {
    bool     message;        // a message arrived
    uint32_t message_color;  // its channel's colour, for the notification LED
    bool     activity;       // other traffic heard
} demo_event_t;

// Backfill a plausible log so scrolling, wrapping and day separators have
// something to act on.
void demo_traffic_seed(app_model_t* model);

// Inject an occasional message and background activity. Returns true when the
// screen needs repainting.
bool demo_traffic_tick(app_model_t* model, demo_event_t* out);

#endif
