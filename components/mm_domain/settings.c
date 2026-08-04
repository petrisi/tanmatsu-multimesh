// SPDX-License-Identifier: MIT

#include "settings.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char TAG[] = "settings";

#define NVS_NAMESPACE "multimesh"

#define KEY_CHANNELS_MC "ch.mc"
#define KEY_CHANNELS_MT "ch.mt"
#define KEY_IDENTITY    "identity"
#define KEY_PREFS       "prefs"

// Bump when a stored layout changes meaning. Records with an unknown version are
// discarded rather than reinterpreted: losing settings is recoverable, silently
// misreading a channel key is not.
// v2 widened the channel abbreviation from 4 to 10 characters, which changed the
// stored record layout.
#define STORED_VERSION 2

typedef struct __attribute__((packed)) {
    char     name[CH_NAME_MAX + 1];
    char     display[CH_DISPLAY_MAX + 1];
    char     secret[CH_SECRET_MAX + 1];
    uint32_t color;
} stored_channel_t;

typedef struct __attribute__((packed)) {
    uint8_t          version;
    uint8_t          count;
    stored_channel_t channels[MAX_CHANNELS];
} stored_channels_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    char    name[ID_NAME_MAX + 1];
    char    short_name[ID_SHORT_MAX + 1];
} stored_identity_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t active;
    uint8_t show_meta;
    uint8_t input_channel[MESH_COUNT];
} stored_prefs_t;

static bool available = false;

bool settings_init(void) {
    nvs_handle_t handle;
    esp_err_t    res = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(res));
        available = false;
        return false;
    }
    nvs_close(handle);
    available = true;
    return true;
}

static const char* channels_key(mesh_id_t mesh) {
    return mesh == MESH_MT ? KEY_CHANNELS_MT : KEY_CHANNELS_MC;
}

static bool read_blob(const char* key, void* out, size_t expected) {
    if (!available) return false;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t    size = expected;
    esp_err_t res  = nvs_get_blob(handle, key, out, &size);
    nvs_close(handle);

    if (res != ESP_OK) return false;
    if (size != expected) {
        ESP_LOGW(TAG, "%s is %u bytes, expected %u -- discarding", key, (unsigned)size, (unsigned)expected);
        return false;
    }
    return true;
}

static void write_blob(const char* key, const void* data, size_t size) {
    if (!available) return;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;

    esp_err_t res = nvs_set_blob(handle, key, data, size);
    if (res == ESP_OK) res = nvs_commit(handle);
    if (res != ESP_OK) ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(res));
    nvs_close(handle);
}

static bool load_channels(app_model_t* model, mesh_id_t id) {
    stored_channels_t stored;
    if (!read_blob(channels_key(id), &stored, sizeof(stored))) return false;
    if (stored.version != STORED_VERSION || stored.count > MAX_CHANNELS) {
        ESP_LOGW(TAG, "channel record for mesh %d unusable -- using defaults", id);
        return false;
    }

    mesh_state_t* mesh   = &model->mesh[id];
    mesh->channel_count  = stored.count;
    for (int i = 0; i < stored.count; i++) {
        channel_t* ch = &mesh->channels[i];
        memset(ch, 0, sizeof(*ch));
        // Copy with an explicit terminator: a corrupt record must not produce an
        // unterminated string that later reads off the end of the struct.
        snprintf(ch->name, sizeof(ch->name), "%.*s", CH_NAME_MAX, stored.channels[i].name);
        snprintf(ch->display, sizeof(ch->display), "%.*s", CH_DISPLAY_MAX, stored.channels[i].display);
        snprintf(ch->secret, sizeof(ch->secret), "%.*s", CH_SECRET_MAX, stored.channels[i].secret);
        ch->color = stored.channels[i].color;
    }
    return stored.count > 0;
}

void settings_save_channels(const app_model_t* model, mesh_id_t id) {
    const mesh_state_t* mesh = &model->mesh[id];

    stored_channels_t stored = {.version = STORED_VERSION, .count = (uint8_t)mesh->channel_count};
    for (int i = 0; i < mesh->channel_count; i++) {
        snprintf(stored.channels[i].name, sizeof(stored.channels[i].name), "%s", mesh->channels[i].name);
        snprintf(stored.channels[i].display, sizeof(stored.channels[i].display), "%s", mesh->channels[i].display);
        snprintf(stored.channels[i].secret, sizeof(stored.channels[i].secret), "%s", mesh->channels[i].secret);
        stored.channels[i].color = mesh->channels[i].color;
    }
    write_blob(channels_key(id), &stored, sizeof(stored));
}

void settings_save_identity(const app_model_t* model) {
    stored_identity_t stored = {.version = STORED_VERSION};
    snprintf(stored.name, sizeof(stored.name), "%s", model->identity.name);
    snprintf(stored.short_name, sizeof(stored.short_name), "%s", model->identity.short_name);
    write_blob(KEY_IDENTITY, &stored, sizeof(stored));
}

void settings_save_prefs(const app_model_t* model) {
    stored_prefs_t stored = {
        .version   = STORED_VERSION,
        .active    = (uint8_t)model->active,
        .show_meta = model->show_meta ? 1 : 0,
    };
    for (int i = 0; i < MESH_COUNT; i++) {
        stored.input_channel[i] = (uint8_t)model->mesh[i].input_channel;
    }
    write_blob(KEY_PREFS, &stored, sizeof(stored));
}

bool settings_load(app_model_t* model) {
    bool any = false;

    for (int i = 0; i < MESH_COUNT; i++) {
        if (load_channels(model, (mesh_id_t)i)) any = true;
    }

    stored_identity_t identity;
    if (read_blob(KEY_IDENTITY, &identity, sizeof(identity)) && identity.version == STORED_VERSION) {
        snprintf(model->identity.name, sizeof(model->identity.name), "%.*s", ID_NAME_MAX, identity.name);
        snprintf(model->identity.short_name, sizeof(model->identity.short_name), "%.*s", ID_SHORT_MAX,
                 identity.short_name);
        any = true;
    }

    stored_prefs_t prefs;
    if (read_blob(KEY_PREFS, &prefs, sizeof(prefs)) && prefs.version == STORED_VERSION) {
        if (prefs.active < MESH_COUNT) model->active = (mesh_id_t)prefs.active;
        model->show_meta = prefs.show_meta != 0;
        for (int i = 0; i < MESH_COUNT; i++) {
            // Clamp: the stored index may point past a channel deleted by a
            // build that crashed before saving the channel list.
            int count = model->mesh[i].channel_count;
            int idx   = prefs.input_channel[i];
            model->mesh[i].input_channel = (count > 0 && idx < count) ? idx : 0;
        }
        any = true;
    }

    return any;
}

static void add_default(mesh_state_t* mesh, const char* name, const char* display, const char* secret,
                        pax_col_t color) {
    if (mesh->channel_count >= MAX_CHANNELS) return;
    channel_t* ch = &mesh->channels[mesh->channel_count++];
    memset(ch, 0, sizeof(*ch));
    snprintf(ch->name, sizeof(ch->name), "%s", name);
    snprintf(ch->display, sizeof(ch->display), "%s", display);
    snprintf(ch->secret, sizeof(ch->secret), "%s", secret);
    ch->color = color;
}

void settings_apply_default_channels(app_model_t* model) {
    mesh_state_t* mc = &model->mesh[MESH_MC];
    if (mc->channel_count == 0) {
        // An empty secret selects MeshCore's well-known public key, so the key
        // itself never has to appear here.
        add_default(mc, "Public", "publ", "", ch_palette[0]);
        mc->input_channel = 0;
    }

    mesh_state_t* mt = &model->mesh[MESH_MT];
    if (mt->channel_count == 0) {
        // "AQ==" is a key *index*, not a key: index 1 means Meshtastic's public
        // default PSK. Published in every client; nothing secret about it.
        add_default(mt, "EdgeFastLow", "EFL", "AQ==", ch_palette[2]);
        mt->input_channel = 0;
    }
}

#define KEY_MC_SEED "mc.seed"

bool settings_load_identity_keypair(identity_t* identity,
                                    bool (*derive)(uint8_t pub[32], uint8_t priv[64], const uint8_t seed[32])) {
    if (identity == NULL || derive == NULL) return false;

    uint8_t seed[32];
    if (!read_blob(KEY_MC_SEED, seed, sizeof(seed))) {
        // First run. A fresh identity means every MeshCore node sees a new
        // contact, which is why this is generated once and then left alone.
        esp_fill_random(seed, sizeof(seed));
        write_blob(KEY_MC_SEED, seed, sizeof(seed));
        ESP_LOGI(TAG, "generated a new MeshCore identity");
    }

    identity->has_keypair = derive(identity->public_key, identity->private_key, seed);
    if (!identity->has_keypair) ESP_LOGE(TAG, "key derivation failed; MeshCore adverts unavailable");

    // The seed is the secret; do not leave a copy on the stack.
    memset(seed, 0, sizeof(seed));
    return identity->has_keypair;
}

#define KEY_MT_PRIVATE "mt.key"

bool settings_load_mt_keypair(identity_t* identity, bool (*generate)(uint8_t pub[32], uint8_t priv[32]),
                              bool (*derive)(uint8_t pub[32], const uint8_t priv[32])) {
    if (identity == NULL || generate == NULL || derive == NULL) return false;

    if (read_blob(KEY_MT_PRIVATE, identity->mt_private_key, sizeof(identity->mt_private_key))) {
        identity->has_mt_keypair = derive(identity->mt_public_key, identity->mt_private_key);
    } else {
        // First run. Generated by the curve implementation rather than from raw
        // random bytes, so clamping and weak-key rejection are its problem.
        identity->has_mt_keypair = generate(identity->mt_public_key, identity->mt_private_key);
        if (identity->has_mt_keypair) {
            write_blob(KEY_MT_PRIVATE, identity->mt_private_key, sizeof(identity->mt_private_key));
            ESP_LOGI(TAG, "generated a new Meshtastic encryption key");
        }
    }

    if (!identity->has_mt_keypair) {
        memset(identity->mt_private_key, 0, sizeof(identity->mt_private_key));
        memset(identity->mt_public_key, 0, sizeof(identity->mt_public_key));
        ESP_LOGE(TAG, "no Meshtastic encryption key; direct messages fall back to the channel");
    }
    return identity->has_mt_keypair;
}

void settings_derive_node_id(identity_t* identity) {
    uint8_t   mac[6] = {0};
    esp_err_t res    = esp_read_mac(mac, ESP_MAC_BASE);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed: %s", esp_err_to_name(res));
    }
    // Meshtastic's convention: the node number is the low four bytes of the MAC.
    // It must never change -- other clients key their name lookups on it.
    identity->node_num = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    snprintf(identity->node_id, sizeof(identity->node_id), "!%08lx", (unsigned long)identity->node_num);
}
