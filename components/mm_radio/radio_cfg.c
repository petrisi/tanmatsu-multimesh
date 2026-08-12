// SPDX-License-Identifier: MIT

#include "radio_cfg.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char TAG[] = "radio_cfg";

#define NVS_NAMESPACE "system"

// Small readers for the device-wide keys. Each opens the namespace itself
// rather than being folded into radio_cfg_load(), because these apply to both
// networks and so are read where the radio is configured, not where MeshCore's
// modem settings are assembled.
static bool system_get_u8(const char* key, uint8_t* out) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    bool ok = nvs_get_u8(handle, key, out) == ESP_OK;
    nvs_close(handle);
    return ok;
}

int32_t radio_cfg_frequency_offset(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
    int32_t offset = 0;
    if (nvs_get_i32(handle, "lora.offset", &offset) != ESP_OK) offset = 0;
    nvs_close(handle);
    return offset;
}

bool radio_cfg_automatic_correction(void) {
    uint8_t value = 0;
    // The launcher's default is on, so an absent key means on rather than off.
    return system_get_u8("lora.autooffset", &value) ? value != 0 : true;
}

bool radio_cfg_low_data_rate(void) {
    uint8_t value = 0;
    return system_get_u8("lora.ldro", &value) ? value != 0 : false;
}

void radio_cfg_load(lora_protocol_config_params_t* out, bool* out_from_nvs) {
    if (out == NULL) return;

    memset(out, 0, sizeof(*out));
    out->frequency                  = RADIO_DEFAULT_FREQUENCY;
    out->spreading_factor           = RADIO_DEFAULT_SF;
    out->bandwidth                  = RADIO_DEFAULT_BANDWIDTH;
    out->coding_rate                = RADIO_DEFAULT_CODINGRATE;
    out->power                      = RADIO_DEFAULT_POWER;
    out->preamble_length            = RADIO_DEFAULT_PREAMBLE;
    out->sync_word                  = RADIO_DEFAULT_SYNC_WORD;
    out->rx_boost                   = RADIO_DEFAULT_RX_BOOST;
    out->crc_enabled                = true;
    out->invert_iq                  = false;
    out->use_dcdc                   = true;
    out->use_automatic_correction   = radio_cfg_automatic_correction();
    out->low_data_rate_optimization = radio_cfg_low_data_rate();
    out->ramp_time                  = 200;

    if (out_from_nvs) *out_from_nvs = false;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "no '%s' NVS namespace, using EU/UK narrow defaults", NVS_NAMESPACE);
        return;
    }

    bool found = false;

    uint32_t u32;
    if (nvs_get_u32(handle, "lora.freq", &u32) == ESP_OK) {
        out->frequency = u32;
        found          = true;
    }
    uint16_t u16;
    if (nvs_get_u16(handle, "lora.bandwidth", &u16) == ESP_OK) {
        out->bandwidth = u16;
        found          = true;
    }
    if (nvs_get_u16(handle, "lora.preamble", &u16) == ESP_OK) {
        out->preamble_length = u16;
        found                = true;
    }
    uint8_t u8;
    if (nvs_get_u8(handle, "lora.sf", &u8) == ESP_OK) {
        out->spreading_factor = u8;
        found                 = true;
    }
    if (nvs_get_u8(handle, "lora.codingrate", &u8) == ESP_OK) {
        out->coding_rate = u8;
        found            = true;
    }
    if (nvs_get_u8(handle, "lora.power", &u8) == ESP_OK) {
        out->power = u8;
        found      = true;
    }
    if (nvs_get_u8(handle, "lora.sync", &u8) == ESP_OK) {
        out->sync_word = u8;
        found          = true;
    }
    if (nvs_get_u8(handle, "lora.rxboost", &u8) == ESP_OK) {
        out->rx_boost = (u8 != 0);
        found         = true;
    }

    nvs_close(handle);

    if (out_from_nvs) *out_from_nvs = found;

    ESP_LOGI(TAG, "%.3f MHz SF%u BW%u CR4/%u pwr%u sync0x%02X rxboost%d (%s)", out->frequency / 1000000.0,
             out->spreading_factor, out->bandwidth, out->coding_rate, out->power, out->sync_word, out->rx_boost,
             found ? "NVS" : "defaults");
}
