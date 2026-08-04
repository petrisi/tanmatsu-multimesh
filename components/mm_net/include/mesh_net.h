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

    // How our own messages should be attributed in the local echo. The networks
    // disagree: MeshCore carries the full name inside the message text, while
    // Meshtastic identifies nodes by their four-character short name.
    const char* (*local_sender)(const identity_t* identity);

    // Decode one frame into `mesh`, updating its counters. Returns true when a
    // displayable message was added, so the caller can notify.
    //
    // Also recognises our own transmission arriving back off a repeater and
    // credits it to the originating message, which is what the delivery
    // indicator counts. That match has to happen here because only the stack
    // knows what identifies one of its frames.
    // `identity` is needed to recognise traffic addressed to us: both networks
    // now carry direct messages, and only our own keys can pick them out.
    bool (*handle)(const lora_protocol_lora_packet_t* pkt, mesh_state_t* mesh, const identity_t* identity);

    // Build a transmittable frame for `text` on `channel`, attributed to
    // `identity`. `msg_seq` is the message this frame belongs to, so repeats
    // heard later can be credited to it.
    //
    // Returns the frame length, or 0 when the text will not fit or the channel
    // has no usable key.
    uint8_t (*encode)(mesh_state_t* mesh, uint8_t channel, const identity_t* identity, const char* text,
                      uint32_t msg_seq, uint8_t* out, size_t out_max);

    // Build a direct message to one contact. `msg` is the row it belongs to, so
    // the stack can record what would acknowledge it -- the two networks prove
    // delivery differently and only the stack knows how.
    //
    // Returns the frame length, or 0 when the contact has no usable key.
    uint8_t (*encode_dm)(mesh_state_t* mesh, const identity_t* identity, const node_t* peer, const char* text,
                         message_t* msg, uint8_t* out, size_t out_max);

    // Build the frame that announces us to the network. Both networks have the
    // concept and neither builds it the same way -- Meshtastic sends an
    // unsigned NodeInfo on a channel, MeshCore an Ed25519-signed advert that
    // belongs to no channel -- so the whole construction lives behind this hook
    // rather than being assembled by the caller.
    //
    // `channel` is where the announcement goes on networks that scope it to one;
    // MeshCore ignores it.
    //
    // Returns the frame length, or 0 when we have nothing to announce with.
    uint8_t (*encode_advert)(mesh_state_t* mesh, uint8_t channel, const identity_t* identity, uint8_t* out,
                             size_t out_max);
} mesh_net_t;

extern const mesh_net_t mesh_net_meshcore;
extern const mesh_net_t mesh_net_meshtastic;
