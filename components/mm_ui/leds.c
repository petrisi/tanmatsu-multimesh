// SPDX-License-Identifier: MIT

#include "leds.h"
#include <string.h>
#include "bsp/led.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char TAG[] = "leds";

#define LED_COUNT   6
#define LED_MESSAGE 2
#define LED_MESH    4  // silkscreened "A"
#define LED_ACTIVITY 5  // silkscreened "B"

#define BLINK_MS    600  // unread message, on/off period
#define ACTIVITY_MS 150  // traffic flicker

// The mesh indicator is a steady background presence, so it is dimmer than a
// notification that wants attention.
#define MESH_DIVISOR     6
#define MESSAGE_DIVISOR  2
#define ACTIVITY_DIVISOR 8

static uint32_t pixels[LED_COUNT];
static uint32_t shown[LED_COUNT];

static uint32_t mesh_color    = 0;
static uint32_t unread_color  = 0;
static bool     unread        = false;
static uint32_t activity_until = 0;
static uint32_t alert_until    = 0;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t dim(uint32_t argb, int divisor) {
    uint32_t r = ((argb >> 16) & 0xFF) / divisor;
    uint32_t g = ((argb >> 8) & 0xFF) / divisor;
    uint32_t b = (argb & 0xFF) / divisor;
    return (r << 16) | (g << 8) | b;
}

// Only talk to the coprocessor when something actually changed: every send is
// an I2C transaction.
static void flush(void) {
    if (memcmp(pixels, shown, sizeof(pixels)) == 0) return;
    for (int i = 0; i < LED_COUNT; i++) {
        bsp_led_set_pixel(i, pixels[i]);
    }
    esp_err_t res = bsp_led_send();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "led send failed: %d", res);
        return;
    }
    memcpy(shown, pixels, sizeof(pixels));
}

void leds_init(void) {
    // Take the LEDs away from the launcher's automatic mode, or our writes get
    // overwritten.
    bsp_led_set_mode(false);
    memset(pixels, 0, sizeof(pixels));
    memset(shown, 0xFF, sizeof(shown));  // force the first flush
    flush();
}

void leds_set_mesh(uint32_t argb) {
    mesh_color            = dim(argb, MESH_DIVISOR);
    pixels[LED_MESH]      = mesh_color;
    flush();
}

void leds_notify_message(uint32_t argb) {
    unread_color = dim(0x00FF0000, MESSAGE_DIVISOR); // Always red for messages now
    unread       = true;
    alert_until  = now_ms() + 5000;
}

void leds_clear_unread(void) {
    if (!unread) return;
    unread              = false;
    alert_until         = 0;
    pixels[LED_MESSAGE] = 0;
    flush();
}

void leds_notify_activity(void) {
    activity_until = now_ms() + ACTIVITY_MS;
}

void leds_tick(void) {
    uint32_t t = now_ms();

    if (alert_until && t < alert_until) {
        // Fast alternating red blink every 100ms
        bool phase = (t / 100) & 1;
        pixels[LED_MESSAGE]  = phase ? dim(0x00FF0000, MESSAGE_DIVISOR) : 0;
        pixels[LED_ACTIVITY] = !phase ? dim(0x00FF0000, MESSAGE_DIVISOR) : 0;
        pixels[LED_MESH]     = mesh_color;
    } else {
        if (alert_until && t >= alert_until) alert_until = 0;
        
        pixels[LED_MESSAGE]  = (unread && ((t / BLINK_MS) & 1)) ? unread_color : 0;
        pixels[LED_MESH]     = mesh_color;
        pixels[LED_ACTIVITY] = (activity_until && t < activity_until) ? dim(0xFFFFFFFF, ACTIVITY_DIVISOR) : 0;
        if (activity_until && t >= activity_until) activity_until = 0;
    }

    flush();
}
