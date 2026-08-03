// SPDX-License-Identifier: MIT
//
// The seam that makes multi-network work.
//
// Every network is an ordinary consumer of one dumb SX1262: they differ only in
// modem settings and in how they interpret the bytes. Expressing that as a small
// interface is what reduces "switch network" to swapping a pointer and pushing a
// new modem config -- no reboot, and no reflashing the C6.
//
// Note what this header does NOT include: anything from the UI. A stack decodes
// into the domain model and stops there; drawing is somebody else's problem.
// The earlier version called the UI directly, and that coupling is exactly what
// forced these files out of the build when the UI changed shape.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_model.h"
#include "lora.h"

typedef struct mesh_net_s {
    const char* name;  // "MeshCore"
    const char* tag;   // "MC"

    // One-time setup (key expansion, channel hashes). False aborts boot.
    bool (*init)(void);

    // Modem settings for this network. Called on every switch.
    void (*get_config)(lora_protocol_config_params_t* out);

    // Expand `secret` into the binary key and channel hash. Called whenever a
    // channel is created, edited or loaded, because each network derives these
    // differently -- MeshCore hashes the key, Meshtastic xors name and key.
    void (*prepare_channel)(channel_t* channel);

    // Decode one frame into `mesh`, updating its counters. Returns true when a
    // displayable message was added, so the caller can notify.
    bool (*handle)(const lora_protocol_lora_packet_t* pkt, mesh_state_t* mesh);
} mesh_net_t;

extern const mesh_net_t mesh_net_meshcore;
extern const mesh_net_t mesh_net_meshtastic;
