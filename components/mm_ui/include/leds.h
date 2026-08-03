// SPDX-License-Identifier: MIT
//
// The six RGB LEDs, used as an ambient status channel so the device says
// something useful face-down. Each has one job rather than all six showing the
// same thing:
//
//   LED 2  message   unread text message; blinks in the channel's colour until
//                    the user looks at the screen or touches the keyboard
//   LED 4  "A"       which mesh is active (MeshCore blue / Meshtastic green)
//   LED 5  "B"       any other traffic heard -- position, telemetry, a packet
//                    for a channel we hold no key for; a brief dim flicker
//
// Tying the message blink to the channel's own colour is what makes the colour
// scheme pay off: you learn which channel is busy without reading the screen.

#pragma once

#include <stdbool.h>
#include <stdint.h>

void leds_init(void);

// Which mesh is active. Shown steadily on LED "A".
void leds_set_mesh(uint32_t argb);

// An unread text message arrived. LED "message" blinks in `argb` until
// leds_clear_unread().
void leds_notify_message(uint32_t argb);

// The user is looking: stop the unread blink.
void leds_clear_unread(void);

// Any non-message traffic. Brief flicker on LED "B".
void leds_notify_activity(void);

// Drives blink and flicker timing. Call from the main loop.
void leds_tick(void);
