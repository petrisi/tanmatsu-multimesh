// SPDX-License-Identifier: MIT
//
// The seam that makes dual-stack work.
//
// Both networks are ordinary consumers of one dumb SX1262: they differ only in
// modem settings and in how they interpret the bytes. Expressing that as a small
// interface is what reduces "switch network" to swapping a pointer and pushing a
// new modem config -- no reboot, and no reflashing the C6.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lora.h"
#include "ui.h"

typedef struct mesh_net_s {
    const char* name;   // "MeshCore"
    const char* tag;    // "MC" -- prefixed to messages in the shared list

    // One-time setup (key expansion, channel hash). Returns false to abort boot.
    bool (*init)(void);

    // Modem settings for this network. Called on every switch.
    void (*get_config)(lora_protocol_config_params_t* out);

    // Decode one frame, update stats, and append any decoded message to the UI.
    void (*handle)(const lora_protocol_lora_packet_t* pkt, ui_stats_t* stats);
} mesh_net_t;

extern const mesh_net_t mesh_net_meshcore;
extern const mesh_net_t mesh_net_meshtastic;
