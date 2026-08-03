// SPDX-License-Identifier: MIT
//
// LoRa modem settings.
//
// These live in the shared `system` NVS namespace, not an app-private one --
// the MeshCore client on this device writes them there, so reading rather than
// hardcoding keeps us tuned to whatever region preset the user has selected.
// Defaults below are the MeshCore EU/UK narrow preset.

#pragma once

#include <stdbool.h>
#include "lora.h"

// MeshCore EU/UK narrow. bandwidth is the *nominal* kHz label the sx126x driver
// maps to an enum: 62 means 62.5 kHz. Passing 63 or a rounded 62.5 would fall
// through to the 125 kHz default and hear nothing.
#define RADIO_DEFAULT_FREQUENCY  869618000
#define RADIO_DEFAULT_SF         8
#define RADIO_DEFAULT_BANDWIDTH  62
#define RADIO_DEFAULT_CODINGRATE 8
#define RADIO_DEFAULT_POWER      22
#define RADIO_DEFAULT_PREAMBLE   8
#define RADIO_DEFAULT_SYNC_WORD  0x12  // MeshCore; Meshtastic uses 0x2B
#define RADIO_DEFAULT_RX_BOOST   true

// Fill `out` from NVS, falling back to the constants above per key.
// `out_from_nvs` reports whether any key was actually present, so the UI can
// say whether we adopted the user's settings or fell back to defaults.
void radio_cfg_load(lora_protocol_config_params_t* out, bool* out_from_nvs);
