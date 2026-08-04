// SPDX-License-Identifier: MIT

#include "nodestore.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char TAG[] = "nodestore";

#define PARTITION  "locfd"
#define MOUNT      "/locfd"
#define DIRECTORY  MOUNT "/multimesh"
#define FLUSH_EVERY_MS 60000

// Bump when the record layout changes; an unrecognised file is discarded rather
// than reinterpreted.
// v2 added the MeshCore signature verdict to node_t.
#define NODEFILE_MAGIC   0x4D4D4E44u  // "MMND"
#define NODEFILE_VERSION 2

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
} nodefile_header_t;

static bool        mounted     = false;
static wl_handle_t wl_handle   = WL_INVALID_HANDLE;
static bool        dirty[MESH_COUNT];
static uint32_t    next_flush_ms;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static const char* path_for(mesh_id_t mesh) {
    return mesh == MESH_MT ? DIRECTORY "/nodes-mt.bin" : DIRECTORY "/nodes-mc.bin";
}

static const char* temp_path_for(mesh_id_t mesh) {
    return mesh == MESH_MT ? DIRECTORY "/nodes-mt.tmp" : DIRECTORY "/nodes-mc.tmp";
}

bool nodestore_init(void) {
    if (mounted) return true;

    // Deliberately NOT format_if_mount_failed: the launcher keeps its icons on
    // this partition, and reformatting because of a transient failure would
    // destroy data that is not ours.
    const esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 4096,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT, PARTITION, &config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "internal store unavailable (%s); nodes will not persist", esp_err_to_name(err));
        return false;
    }

    mkdir(DIRECTORY, 0777);  // already exists on every run but the first
    mounted       = true;
    next_flush_ms = now_ms() + FLUSH_EVERY_MS;
    ESP_LOGI(TAG, "node store ready at %s", DIRECTORY);
    return true;
}

bool nodestore_ready(void) {
    return mounted;
}

static void load_one(app_model_t* model, mesh_id_t id) {
    FILE* file = fopen(path_for(id), "rb");
    if (file == NULL) return;  // first run

    nodefile_header_t header;
    if (fread(&header, sizeof(header), 1, file) != 1 || header.magic != NODEFILE_MAGIC ||
        header.version != NODEFILE_VERSION || header.count > MAX_NODES) {
        ESP_LOGW(TAG, "%s unusable, ignoring", path_for(id));
        fclose(file);
        return;
    }

    mesh_state_t* mesh = &model->mesh[id];
    size_t        read = fread(mesh->nodes, sizeof(node_t), header.count, file);
    fclose(file);

    mesh->node_count = (int)read;

    // A verdict that was still queued when we powered off is not a verdict. Drop
    // it back to unknown so the next advert re-runs the check.
    for (int i = 0; i < mesh->node_count; i++) {
        if (mesh->nodes[i].verified == NODE_VERIFY_PENDING) mesh->nodes[i].verified = NODE_VERIFY_UNKNOWN;
    }

    ESP_LOGI(TAG, "%s: %u nodes", model->mesh[id].name, (unsigned)read);
}

void nodestore_load(app_model_t* model) {
    if (!mounted) return;
    for (int i = 0; i < MESH_COUNT; i++) {
        load_one(model, (mesh_id_t)i);
    }
}

void nodestore_mark_dirty(mesh_id_t mesh) {
    if (mesh < MESH_COUNT) dirty[mesh] = true;
}

static bool write_one(const app_model_t* model, mesh_id_t id) {
    const mesh_state_t* mesh = &model->mesh[id];

    // Count what is actually live, so a table full of holes does not write
    // records that load would then have to filter.
    uint16_t live = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (mesh->nodes[i].used) live++;
    }

    // Write to a temporary file and rename over the old one. A power cut then
    // leaves the previous table intact rather than a half-written one.
    const char* temp = temp_path_for(id);
    FILE*       file = fopen(temp, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "cannot open %s", temp);
        return false;
    }

    nodefile_header_t header = {.magic = NODEFILE_MAGIC, .version = NODEFILE_VERSION, .count = live};
    bool              ok     = fwrite(&header, sizeof(header), 1, file) == 1;

    for (int i = 0; ok && i < MAX_NODES; i++) {
        if (!mesh->nodes[i].used) continue;
        ok = fwrite(&mesh->nodes[i], sizeof(node_t), 1, file) == 1;
    }

    if (fclose(file) != 0) ok = false;
    if (!ok) {
        ESP_LOGE(TAG, "write of %s failed", temp);
        remove(temp);
        return false;
    }

    remove(path_for(id));  // FAT rename will not overwrite
    if (rename(temp, path_for(id)) != 0) {
        ESP_LOGE(TAG, "rename to %s failed", path_for(id));
        return false;
    }
    return true;
}

bool nodestore_flush(const app_model_t* model, bool force) {
    if (!mounted) return false;
    if (!force && now_ms() < next_flush_ms) return false;
    next_flush_ms = now_ms() + FLUSH_EVERY_MS;

    bool wrote = false;
    for (int i = 0; i < MESH_COUNT; i++) {
        if (!dirty[i]) continue;
        if (write_one(model, (mesh_id_t)i)) {
            dirty[i] = false;
            wrote    = true;
        }
    }
    return wrote;
}
