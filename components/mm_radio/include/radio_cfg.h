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

// Crystal correction in hertz, from `lora.offset`.
//
// Not a preference but a per-device measurement: the launcher's LoRa
// information screen measures the frequency error against received traffic and
// stores it there. It applies to the hardware rather than to a network, so it
// belongs to whichever profile happens to be tuned -- both of ours.
//
// At 62.5 kHz bandwidth a few kilohertz of crystal error is a real fraction of
// the channel, which is exactly the kind of fault that works for everyone
// except the person whose radio is off.
int32_t radio_cfg_frequency_offset(void);

// Whether the radio's own automatic frequency correction is enabled, from
// `lora.autooffset`. Also a hardware behaviour rather than a network setting.
bool radio_cfg_automatic_correction(void);

// Low data rate optimisation, from `lora.ldro`. Required when a symbol lasts
// longer than 16 ms, which narrow bandwidths at high spreading factors reach.
bool radio_cfg_low_data_rate(void);
