// SPDX-License-Identifier: MIT

#include "settings.h"
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
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
static void load_config(app_model_t* model);

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

// The launcher's namespace, read and never written -- the same rule the radio
// settings follow. Two apps writing one key set is how configuration
// mysteriously changes.
#define SYSTEM_NAMESPACE "system"
#define KEY_TZ           "tz"
#define TZ_STRING_MAX    64  // matches the launcher's TIMEZONE_TZ_LEN

bool settings_apply_timezone(void) {
    char tz[TZ_STRING_MAX] = {0};
    bool found             = false;

    // Not gated on `available`: that flag reports our own namespace, and this
    // one belongs to the launcher and exists independently of it.
    nvs_handle_t handle;
    if (nvs_open(SYSTEM_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(tz);
        if (nvs_get_str(handle, KEY_TZ, tz, &size) == ESP_OK && tz[0] != '\0') found = true;
        nvs_close(handle);
    }

    // "UTC0" is the POSIX form, and the string the launcher's own Etc/UTC entry
    // carries -- so an unset device agrees with it exactly.
    setenv("TZ", found ? tz : "UTC0", 1);
    tzset();

    ESP_LOGI(TAG, "timezone %s (%s)", found ? tz : "UTC0", found ? "device setting" : "no device setting");
    return found;
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

// As read_blob, but accepts a record written by an older build that had fewer
// fields: the leading bytes are read and the rest is left at whatever the
// caller pre-filled, which is the caller's defaults.
//
// Only safe for a record that grows by appending, and only because the version
// byte still guards a change of meaning. Adding a field used to resize the
// blob, fail the exact-size check and silently discard everything -- which is
// how location, hop limit and role reset three times.
static bool read_blob_growable(const char* key, void* out, size_t full_size, size_t* out_read) {
    if (!available) return false;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t    size = full_size;
    esp_err_t res  = nvs_get_blob(handle, key, out, &size);
    nvs_close(handle);

    if (res != ESP_OK) return false;
    if (size > full_size) {
        // Written by a newer build. The tail means nothing to us and the
        // version byte cannot vouch for it, so do not guess.
        ESP_LOGW(TAG, "%s is %u bytes, newer than this build understands -- ignoring", key, (unsigned)size);
        return false;
    }
    if (out_read) *out_read = size;
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

    load_config(model);

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

#define KEY_CONFIG "cfg"

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t has_location;
    int32_t latitude;
    int32_t longitude;
    uint8_t display_off_minutes;
    uint8_t kbd_off_minutes;
    uint8_t mt_default_hops;
    uint8_t mt_role;
    uint8_t mc_repeater;
    uint8_t mt_always_repeat;
    uint8_t mt_optimize_text;
    // Appended after the growable read was introduced. A record written before
    // these existed is short, and the fields below keep their defaults rather
    // than the whole record being thrown away. Append only; never reorder.
    uint8_t  mt_profile;
    uint8_t  mt_power;
    uint32_t mt_custom_freq;
    uint8_t  mt_custom_sf;
    uint8_t  mt_custom_cr;
    uint16_t mt_custom_bw;
} stored_config_t;

// Offset of the first field a short record may be missing. Anything read at
// least this long has every field up to it.
#define CONFIG_V1_SIZE offsetof(stored_config_t, mt_profile)

void settings_save_config(const app_model_t* model) {
    const settings_t* s      = &model->settings;
    stored_config_t   stored = {
          .version             = STORED_VERSION,
          .has_location        = s->has_location ? 1 : 0,
          .latitude            = s->latitude,
          .longitude           = s->longitude,
          .display_off_minutes = s->display_off_minutes,
          .kbd_off_minutes     = s->kbd_off_minutes,
          .mt_default_hops     = s->mt_default_hops,
          .mt_role             = s->mt_role,
          .mc_repeater         = s->mc_repeater ? 1 : 0,
          .mt_always_repeat    = s->mt_always_repeat ? 1 : 0,
          .mt_optimize_text    = s->mt_optimize_text ? 1 : 0,
          .mt_profile          = s->mt_profile,
          .mt_power            = s->mt_power,
          .mt_custom_freq      = s->mt_custom.freq_hz,
          .mt_custom_sf        = s->mt_custom.sf,
          .mt_custom_cr        = s->mt_custom.cr,
          .mt_custom_bw        = s->mt_custom.bw,
    };
    write_blob(KEY_CONFIG, &stored, sizeof(stored));
}

static void load_config(app_model_t* model) {
    // Pre-filled with the defaults model_init() set, so anything a short record
    // does not reach keeps them.
    stored_config_t stored = {0};
    size_t          read   = 0;
    if (!read_blob_growable(KEY_CONFIG, &stored, sizeof(stored), &read)) return;
    if (read < CONFIG_V1_SIZE || stored.version != STORED_VERSION) {
        ESP_LOGW(TAG, "configuration record unusable -- keeping defaults");
        return;
    }

    settings_t* s = &model->settings;
    s->has_location = stored.has_location != 0;
    s->latitude     = stored.latitude;
    s->longitude    = stored.longitude;
    s->mc_repeater  = stored.mc_repeater != 0;

    s->display_off_minutes = stored.display_off_minutes;
    s->kbd_off_minutes     = stored.kbd_off_minutes;
    // Clamped rather than trusted: a record written by a build with different
    // limits must not put an illegal hop count on the air.
    s->mt_default_hops = stored.mt_default_hops > SET_HOPS_MAX_STORED ? SET_HOPS_MAX_STORED : stored.mt_default_hops;
    s->mt_role         = stored.mt_role > MT_ROLE_CLIENT_MUTE ? MT_ROLE_CLIENT_MUTE : stored.mt_role;

    // Only meaningful at CLIENT, but stored as written rather than forced off
    // here: a user who switches to CLIENT_MUTE and back should find their relay
    // settings as they left them.
    s->mt_always_repeat = stored.mt_always_repeat != 0;
    s->mt_optimize_text = stored.mt_optimize_text != 0;

    // Only if the record is long enough to carry them; an older one leaves the
    // defaults model_init() put there.
    if (read >= sizeof(stored_config_t)) {
        // Clamped, because a stored profile index this build does not have
        // would otherwise resolve to nothing and transmit on garbage.
        s->mt_profile = stored.mt_profile < MT_PROFILE_COUNT ? stored.mt_profile : MT_PROFILE_EFL_EU;
        s->mt_power   = stored.mt_power >= MT_POWER_MIN && stored.mt_power <= MT_POWER_MAX ? stored.mt_power
                                                                                           : MT_POWER_DEFAULT;

        mt_radio_t c = {.freq_hz = stored.mt_custom_freq,
                        .sf      = stored.mt_custom_sf,
                        .cr      = stored.mt_custom_cr,
                        .bw      = stored.mt_custom_bw};
        bool usable  = c.freq_hz >= MT_FREQ_MIN_HZ && c.freq_hz <= MT_FREQ_MAX_HZ && c.sf >= MT_SF_MIN &&
                      c.sf <= MT_SF_MAX && c.cr >= MT_CR_MIN && c.cr <= MT_CR_MAX && mt_bw_label(c.bw)[0] != '?';
        if (usable) {
            s->mt_custom = c;
        } else {
            ESP_LOGW(TAG, "stored custom radio settings unusable -- seeding from the default profile");
            if (s->mt_profile == MT_PROFILE_CUSTOM) s->mt_profile = MT_PROFILE_EFL_EU;
            mt_radio_resolve(s, &s->mt_custom);
        }
    }

    // The active limit always starts from the stored default; a session that
    // raised it does not get to make that permanent by outliving the reboot.
    model->mt_active_hops = s->mt_default_hops;
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
        // There is no fallback to lose: current firmware refuses to send a
        // keyless direct message and discards one that arrives, so without this
        // key Meshtastic direct messages are simply unavailable.
        ESP_LOGE(TAG, "no Meshtastic encryption key; direct messages unavailable");
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
