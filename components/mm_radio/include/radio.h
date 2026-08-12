// SPDX-License-Identifier: MIT
//
// The LoRa transport: the only place that talks to the radio.
//
// The SX1262 lives on the ESP32-C6, not on the P4 that runs this app, and the
// stock C6 firmware exposes it as a dumb modem over an ESP-HOSTED RPC. So every
// call here is a round trip across the SDIO link, and lora_send_packet() does
// not return until the packet has actually finished transmitting -- roughly half
// a second at SF8/BW62.5. Nothing on the UI thread may call the transmit side.
//
// Receive is different: the component behind this API already fills a queue from
// its own callback, so radio_receive() with a zero timeout is a cheap poll and
// needs no task of its own.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lora.h"

// Bring up the P4<->C6 link and open the radio. False leaves the app usable but
// unable to send or receive, which is worth surfacing rather than hiding: the
// user can still configure channels and identity.
bool radio_start(void);

bool radio_is_ready(void);

// Push modem settings and return to receive. Called at boot and on every
// network switch.
bool radio_apply_config(const lora_protocol_config_params_t* config);

// Discard anything already queued. Used after a config change, because those
// packets were received under the previous modem settings and belong to the
// network we just left.
void radio_drain(void);

// Poll for one received frame. `timeout_ms` of 0 returns immediately.
bool radio_receive(lora_protocol_lora_packet_t* out, uint32_t timeout_ms);

// Spectrum scanner helpers
void radio_scan_start(void);
float radio_scan_measure(uint32_t freq_hz);
void radio_scan_stop(void);

// Transmit one frame. BLOCKS until the packet has left the antenna -- around
// half a second for a short message at SF8/BW62.5 -- so this must only ever be
// called from a worker task, never from the loop that draws the screen.
//
// Performs a best-effort listen-before-talk first; see radio.c for why "best
// effort" is the honest description.
bool radio_send(const uint8_t* data, uint8_t length);

// Radio firmware version reported by the C6, for diagnostics. Empty until
// radio_start() has succeeded.
const char* radio_firmware_version(void);
