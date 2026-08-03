// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_model.h"

// Channel palette. Chosen to stay legible on the dark background and to remain
// distinguishable from each other at a glance.
#define CH_CYAN   0xFF7FD4FF
#define CH_AMBER  0xFFFFD166
#define CH_GREEN  0xFF9BE564
#define CH_PINK   0xFFFF8FA3
#define CH_VIOLET 0xFFC59BFF
#define CH_ORANGE 0xFFFFAE73

const pax_col_t ch_palette[CH_PALETTE_SIZE] = {CH_CYAN, CH_AMBER, CH_GREEN, CH_PINK, CH_VIOLET, CH_ORANGE};

// Status bar backgrounds, one per network.
#define MESH_MC_ACCENT 0xFF14508C  // MeshCore blue
#define MESH_MT_ACCENT 0xFF1B7A46  // Meshtastic green

void model_init(app_model_t* model) {
    memset(model, 0, sizeof(*model));

    model->mesh[MESH_MC].name   = "MeshCore";
    model->mesh[MESH_MC].accent = MESH_MC_ACCENT;
    model->mesh[MESH_MT].name   = "Meshtastic";
    model->mesh[MESH_MT].accent = MESH_MT_ACCENT;

    for (int i = 0; i < MESH_COUNT; i++) {
        model->mesh[i].pinned       = true;
        model->mesh[i].selected_seq = -1;
    }

    model->radio       = RADIO_RX;
    model->history_pos = -1;
    model->battery_pct = 100;
    model->time_synced = false;
}

message_t* model_push(mesh_state_t* mesh, uint8_t channel, const char* sender, bool sender_named, const char* text,
                      bool outgoing) {
    message_t* msg = &mesh->messages[mesh->head];
    memset(msg, 0, sizeof(*msg));

    msg->used         = true;
    msg->seq          = mesh->next_seq++;
    msg->channel      = channel;
    msg->outgoing     = outgoing;
    msg->sender_named = sender_named;
    msg->timestamp    = (uint32_t)time(NULL);
    msg->tx           = outgoing ? TX_QUEUED : TX_NONE;
    snprintf(msg->sender, sizeof(msg->sender), "%s", sender);
    snprintf(msg->text, sizeof(msg->text), "%s", text);

    mesh->head = (mesh->head + 1) % MAX_MESSAGES;
    if (mesh->count < MAX_MESSAGES) mesh->count++;

    // A message arriving while the user is reading further up must not drag the
    // viewport; it is counted instead, and the UI offers a jump to the end.
    if (!outgoing && !mesh->pinned) mesh->unseen++;

    return msg;
}

const message_t* model_message_at(const mesh_state_t* mesh, int logical) {
    if (logical < 0 || logical >= mesh->count) return NULL;
    int idx = (mesh->head - mesh->count + logical + MAX_MESSAGES * 2) % MAX_MESSAGES;
    return &mesh->messages[idx];
}

const message_t* model_message_by_seq(const mesh_state_t* mesh, int32_t seq) {
    if (seq < 0) return NULL;
    for (int i = 0; i < mesh->count; i++) {
        const message_t* msg = model_message_at(mesh, i);
        if (msg && msg->used && (int32_t)msg->seq == seq) return msg;
    }
    return NULL;
}
