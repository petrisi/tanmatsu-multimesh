// SPDX-License-Identifier: MIT
//
// MeshComms PoC: listen on MeshCore or Meshtastic, switch with one key.
//
// Architecture note, because it is the whole point of this project: the SX1262
// is wired to the ESP32-C6, not to the P4 we run on. The stock C6 firmware
// exposes it as a dumb radio over the ESP-HOSTED link, so both protocol stacks
// -- framing and crypto alike -- run here in application code. That is what
// makes switching networks a config push rather than a C6 reflash, and it is
// why the C6 keeps its stock firmware (and therefore WiFi and BLE) either way.

#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "bsp/rtc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lora.h"
#include "mesh_net.h"
#include "nvs_flash.h"
#include "ui.h"
#include "wifi_connection.h"

static const char TAG[] = "main";

static lora_handle_t lora_handle       = {0};
static QueueHandle_t input_event_queue = NULL;

// Both stacks stay resident; only one owns the radio at a time because there is
// only one SX1262. Their counters are kept apart so the numbers stay meaningful
// across switches.
static const mesh_net_t* nets[] = {&mesh_net_meshcore, &mesh_net_meshtastic};
static ui_stats_t        stats[sizeof(nets) / sizeof(nets[0])];
static int               active = 0;

#define NET_COUNT (int)(sizeof(nets) / sizeof(nets[0]))

// Push the active network's modem settings to the radio and return to RX.
// Standby first: changing modulation parameters underneath an active receive is
// not something the SX1262 is required to cope with.
static esp_err_t apply_active_net(void) {
    const mesh_net_t*             net = nets[active];
    lora_protocol_config_params_t cfg;
    net->get_config(&cfg);

    esp_err_t res = lora_set_mode(&lora_handle, LORA_PROTOCOL_MODE_STANDBY_XOSC);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "standby failed: %d", res);
        return res;
    }

    res = lora_set_config(&lora_handle, &cfg);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %d", res);
        return res;
    }

    res = lora_set_mode(&lora_handle, LORA_PROTOCOL_MODE_RX);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "rx mode failed: %d", res);
        return res;
    }

    ui_set_header("%s  %.4f MHz SF%u BW%u CR4/%u sync0x%02X", net->name, cfg.frequency / 1000000.0,
                  cfg.spreading_factor, cfg.bandwidth, cfg.coding_rate, cfg.sync_word);
    ESP_LOGI(TAG, "active: %s @ %.4f MHz SF%u BW%u", net->name, cfg.frequency / 1000000.0, cfg.spreading_factor,
             cfg.bandwidth);
    return ESP_OK;
}

static void switch_net(void) {
    active = (active + 1) % NET_COUNT;

    // Anything already queued was received under the previous modem settings.
    lora_protocol_lora_packet_t stale;
    while (lora_receive_packet(&lora_handle, &stale, 0) == ESP_OK) {
    }

    char notice[80];
    if (apply_active_net() == ESP_OK) {
        snprintf(notice, sizeof(notice), "switched to %s", nets[active]->name);
    } else {
        snprintf(notice, sizeof(notice), "switch to %s FAILED", nets[active]->name);
    }
    ui_add_notice(notice);
}

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %d", res);
        return;
    }

    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %d", res);
        return;
    }

    ui_init();
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    for (int i = 0; i < NET_COUNT; i++) {
        memset(&stats[i], 0, sizeof(stats[i]));
        if (!nets[i]->init()) {
            ui_boot_line("%s init FAILED", nets[i]->name);
            vTaskDelay(pdMS_TO_TICKS(4000));
            return;
        }
    }

    // This is what actually brings up the P4<->C6 SDIO RPC pipeline that
    // tanmatsu-lora rides on. We want the transport, not a WiFi association --
    // no scan/associate/DHCP, so no airtime or battery spent on it.
    ui_boot_line("Starting radio link...");
    res = wifi_connection_init_stack();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "radio link init failed: %d", res);
        ui_boot_line("Radio link FAILED (%d)", res);
        vTaskDelay(pdMS_TO_TICKS(4000));
        return;
    }

    // No RTC on the P4; the C6 coprocessor holds the clock the launcher synced.
    // Without this, received MeshCore messages render with a 1970 timestamp.
    if (bsp_rtc_update_time() != ESP_OK) {
        ESP_LOGW(TAG, "RTC sync from coprocessor failed; timestamps may be wrong");
    }

    ui_boot_line("Opening LoRa radio...");
    res = lora_init_remote(&lora_handle, 16);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "lora_init_remote failed: %d", res);
        ui_boot_line("LoRa init FAILED (%d)", res);
        vTaskDelay(pdMS_TO_TICKS(4000));
        return;
    }

    if (apply_active_net() != ESP_OK) {
        ui_boot_line("LoRa config FAILED");
        vTaskDelay(pdMS_TO_TICKS(4000));
        return;
    }
    ui_render(&stats[active]);

    // RX loop. lora_receive_packet blocks on the driver's queue; the timeout is
    // only there to give the UI a periodic repaint and to poll the keyboard.
    while (1) {
        lora_protocol_lora_packet_t pkt = {0};
        if (lora_receive_packet(&lora_handle, &pkt, pdMS_TO_TICKS(250)) == ESP_OK) {
            nets[active]->handle(&pkt, &stats[active]);
        }

        ui_render(&stats[active]);

        bsp_input_event_t event;
        while (xQueueReceive(input_event_queue, &event, 0) == pdTRUE) {
            if (event.type != INPUT_EVENT_TYPE_NAVIGATION || !event.args_navigation.state) continue;

            switch (event.args_navigation.key) {
                case BSP_INPUT_NAVIGATION_KEY_F1:
                    ESP_LOGI(TAG, "F1: returning to launcher");
                    bsp_device_restart_to_launcher();
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F2:
                    switch_net();
                    break;
                default:
                    break;
            }
        }
    }
}
