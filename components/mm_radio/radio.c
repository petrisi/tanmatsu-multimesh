// SPDX-License-Identifier: MIT

#include "radio.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_connection.h"

static const char TAG[] = "radio";

// Deep enough to ride out a burst while the UI is busy repainting. Each entry is
// a full 256-byte packet, so this is not free.
#define RX_QUEUE_DEPTH 16

static lora_handle_t handle;
static bool          ready = false;
static char          firmware_version[24] = "";

bool radio_is_ready(void) {
    return ready;
}

const char* radio_firmware_version(void) {
    return firmware_version;
}

bool radio_start(void) {
    if (ready) return true;

    // This is what actually brings up the P4<->C6 SDIO RPC pipeline that the
    // LoRa component rides on. We want the transport, not a WiFi association --
    // no scan, associate or DHCP, so no airtime or battery is spent on it.
    esp_err_t res = wifi_connection_init_stack();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "P4<->C6 link init failed: %s", esp_err_to_name(res));
        return false;
    }

    res = lora_init_remote(&handle, RX_QUEUE_DEPTH);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "lora_init_remote failed: %s", esp_err_to_name(res));
        return false;
    }

    lora_protocol_status_params_t status = {0};
    if (lora_get_status(&handle, &status) == ESP_OK) {
        // The version string is fixed-length and not guaranteed to be
        // terminated, so bound the copy by the field rather than trusting it.
        int len = (int)sizeof(status.version_string);
        if (len > (int)sizeof(firmware_version) - 1) len = sizeof(firmware_version) - 1;
        memcpy(firmware_version, status.version_string, len);
        firmware_version[len] = '\0';
        ESP_LOGI(TAG, "radio ready: %s", firmware_version);
    } else {
        ESP_LOGW(TAG, "radio opened but did not report a version");
    }

    ready = true;
    return true;
}

bool radio_apply_config(const lora_protocol_config_params_t* config) {
    if (!ready || config == NULL) return false;

    // Standby first: changing modulation parameters underneath an active
    // receive is not something the SX1262 is required to cope with.
    esp_err_t res = lora_set_mode(&handle, LORA_PROTOCOL_MODE_STANDBY_XOSC);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "standby failed: %s", esp_err_to_name(res));
        return false;
    }

    res = lora_set_config(&handle, config);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(res));
        return false;
    }

    res = lora_set_mode(&handle, LORA_PROTOCOL_MODE_RX);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "rx mode failed: %s", esp_err_to_name(res));
        return false;
    }

    ESP_LOGI(TAG, "%.4f MHz SF%u BW%u CR4/%u sync0x%02X", config->frequency / 1000000.0,
             config->spreading_factor, config->bandwidth, config->coding_rate, config->sync_word);
    return true;
}

void radio_drain(void) {
    if (!ready) return;
    lora_protocol_lora_packet_t stale;
    while (lora_receive_packet(&handle, &stale, 0) == ESP_OK) {
    }
}

bool radio_receive(lora_protocol_lora_packet_t* out, uint32_t timeout_ms) {
    if (!ready || out == NULL) return false;
    return lora_receive_packet(&handle, out, pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
}
