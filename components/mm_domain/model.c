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

// --- nodes ---------------------------------------------------------------

// Claim a slot: an unused one if there is any, otherwise the least recently
// heard entry. A full table should shed the stalest node rather than refuse to
// learn about a new one.
static node_t* claim_slot(mesh_state_t* mesh) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (!mesh->nodes[i].used) {
            if (i >= mesh->node_count) mesh->node_count = i + 1;
            return &mesh->nodes[i];
        }
    }

    node_t* oldest = &mesh->nodes[0];
    for (int i = 1; i < MAX_NODES; i++) {
        if (mesh->nodes[i].last_heard < oldest->last_heard) oldest = &mesh->nodes[i];
    }
    return oldest;
}

static node_t* touch(mesh_state_t* mesh, node_t* node, bool fresh) {
    if (fresh) memset(node, 0, sizeof(*node));
    node->used       = true;
    node->last_heard = (uint32_t)time(NULL);
    return node;
}

node_t* model_node_touch_mt(mesh_state_t* mesh, uint32_t node_num) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (mesh->nodes[i].used && mesh->nodes[i].node_num == node_num) return touch(mesh, &mesh->nodes[i], false);
    }

    node_t* node = touch(mesh, claim_slot(mesh), true);
    node->node_num = node_num;
    return node;
}

node_t* model_node_touch_mc(mesh_state_t* mesh, const uint8_t key[NODE_KEY_LEN]) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (mesh->nodes[i].used && memcmp(mesh->nodes[i].key, key, NODE_KEY_LEN) == 0) {
            return touch(mesh, &mesh->nodes[i], false);
        }
    }

    node_t* node = touch(mesh, claim_slot(mesh), true);
    memcpy(node->key, key, NODE_KEY_LEN);
    return node;
}

node_t* model_node_find_mt(mesh_state_t* mesh, uint32_t node_num) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (mesh->nodes[i].used && mesh->nodes[i].node_num == node_num) return &mesh->nodes[i];
    }
    return NULL;
}

node_t* model_node_find_mc(mesh_state_t* mesh, const uint8_t key[NODE_KEY_LEN]) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (mesh->nodes[i].used && memcmp(mesh->nodes[i].key, key, NODE_KEY_LEN) == 0) return &mesh->nodes[i];
    }
    return NULL;
}

node_t* model_target_node(mesh_state_t* mesh, mesh_id_t id) {
    if (!mesh->target_contact) return NULL;
    return id == MESH_MT ? model_node_find_mt(mesh, mesh->target_num) : model_node_find_mc(mesh, mesh->target_key);
}

void model_target_set_contact(mesh_state_t* mesh, mesh_id_t id, const node_t* node) {
    if (node == NULL) return;
    mesh->target_contact = true;
    if (id == MESH_MT) {
        mesh->target_num = node->node_num;
        memset(mesh->target_key, 0, NODE_KEY_LEN);
    } else {
        memcpy(mesh->target_key, node->key, NODE_KEY_LEN);
        mesh->target_num = 0;
    }
}

void model_target_set_channel(mesh_state_t* mesh, int channel) {
    mesh->target_contact = false;
    if (channel >= 0 && channel < mesh->channel_count) mesh->input_channel = channel;
}

void model_node_label(const node_t* node, mesh_id_t id, char* out, size_t out_size) {
    if (node == NULL || out == NULL || out_size == 0) return;

    if (node->long_name[0]) {
        snprintf(out, out_size, "%s", node->long_name);
    } else if (node->short_name[0]) {
        snprintf(out, out_size, "%s", node->short_name);
    } else if (id == MESH_MT) {
        snprintf(out, out_size, "!%08lx", (unsigned long)node->node_num);
    } else {
        // MeshCore identifies nodes by public key; a prefix is what other
        // clients show too.
        snprintf(out, out_size, "%02x%02x%02x%02x", node->key[0], node->key[1], node->key[2], node->key[3]);
    }
}

int model_nodes_prune(mesh_state_t* mesh, uint32_t now) {
    int removed = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        node_t* node = &mesh->nodes[i];
        if (!node->used) continue;

        // A clock that has not been set yet would expire everything at once.
        if (now < node->last_heard) continue;

        uint32_t age   = now - node->last_heard;
        uint32_t limit = node->named ? NODE_EXPIRY_NAMED_S : NODE_EXPIRY_UNNAMED_S;
        if (age > limit) {
            memset(node, 0, sizeof(*node));
            removed++;
        }
    }
    return removed;
}

void model_nodes_clear(mesh_state_t* mesh) {
    memset(mesh->nodes, 0, sizeof(mesh->nodes));
    mesh->node_count = 0;
}

void model_node_remove(mesh_state_t* mesh, int index) {
    if (index < 0 || index >= MAX_NODES) return;
    memset(&mesh->nodes[index], 0, sizeof(mesh->nodes[index]));
}

int model_nodes_by_recency(const mesh_state_t* mesh, int* out, int max) {
    int count = 0;
    for (int i = 0; i < MAX_NODES && count < max; i++) {
        if (mesh->nodes[i].used) out[count++] = i;
    }

    // Insertion sort: the table is small and this runs only when the list is
    // drawn, so the simplest correct thing is the right thing.
    for (int i = 1; i < count; i++) {
        int key = out[i];
        int j   = i - 1;
        while (j >= 0 && mesh->nodes[out[j]].last_heard < mesh->nodes[key].last_heard) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return count;
}

const message_t* model_message_by_seq(const mesh_state_t* mesh, int32_t seq) {
    if (seq < 0) return NULL;
    for (int i = 0; i < mesh->count; i++) {
        const message_t* msg = model_message_at(mesh, i);
        if (msg && msg->used && (int32_t)msg->seq == seq) return msg;
    }
    return NULL;
}
