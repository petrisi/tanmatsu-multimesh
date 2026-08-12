#include "audio.h"
#include "bsp/audio.h"
#include "bsp/tanmatsu.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include <math.h>

static const char* TAG = "audio";

#include "mm_tts.h"

// We need to pass text to the task
static char* tts_text_to_play = NULL;

static void audio_task(void *pvParameters) {
    i2s_chan_handle_t i2s_handle;
    if (bsp_audio_get_i2s_handle(&i2s_handle) != ESP_OK) {
        if (tts_text_to_play) { free(tts_text_to_play); tts_text_to_play = NULL; }
        vTaskDelete(NULL);
        return;
    }

    // Enable amplifier
    bsp_audio_set_amplifier(true);
    bsp_audio_set_amplifier_force(true);
    
    // Check headphone detect
    tanmatsu_coprocessor_handle_t coprocessor_handle = NULL;
    if (bsp_tanmatsu_coprocessor_get_handle(&coprocessor_handle) == ESP_OK) {
        tanmatsu_coprocessor_inputs_t inputs = {0};
        tanmatsu_coprocessor_get_inputs(coprocessor_handle, &inputs);
        ESP_LOGI(TAG, "Headphone detect: %u", inputs.headphone_detect);
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // ---------------------------------------------------------
    // 1. Play Notification Sound (44100 Hz)
    // ---------------------------------------------------------
    i2s_channel_disable(i2s_handle);
    bsp_audio_set_rate(44100);
    i2s_channel_enable(i2s_handle);
    
    bsp_audio_set_volume(100.0f);

    size_t bytes_written;
    int sample_rate = 44100;
    int duration_ms = 1000;
    int samples = (sample_rate * duration_ms) / 1000;
    
    int16_t *buffer = malloc(1000 * sizeof(int16_t));
    if (buffer) {
        for (int i = 0; i < samples; i += 500) {
            int current_time_ms = (i * 1000) / sample_rate;
            
            // "DIIDII" pattern
            bool is_beep = false;
            if (current_time_ms < 250) is_beep = true;
            else if (current_time_ms >= 400 && current_time_ms < 650) is_beep = true;
            
            for (int j = 0; j < 500; j++) {
                if (is_beep) {
                    float t = (float)(i + j) / sample_rate;
                    int16_t val = (int16_t)(sin(2.0 * M_PI * 1000.0 * t) * 10000.0);
                    buffer[j * 2] = val;     // Left
                    buffer[j * 2 + 1] = val; // Right
                } else {
                    buffer[j * 2] = 0;
                    buffer[j * 2 + 1] = 0;
                }
            }
            i2s_channel_write(i2s_handle, buffer, 1000 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        }
        free(buffer);
    }

    // ---------------------------------------------------------
    // 2. Play TTS (22050 Hz) if provided
    // ---------------------------------------------------------
    if (tts_text_to_play) {
        // vTaskDelay(pdMS_TO_TICKS(200));  No need for delay here since our tone ends with 350ms of silence
        
        // Generate TTS audio (returns 16-bit stereo at 8000 Hz)
        int tts_samples = 0;
        int16_t* tts_buf = mm_tts_generate(tts_text_to_play, &tts_samples);
        
        if (tts_buf && tts_samples > 0) {
            i2s_channel_disable(i2s_handle);
            bsp_audio_set_rate(8000);
            i2s_channel_enable(i2s_handle);
            
            // Write in chunks to not block too long and allow watchdog to be happy
            int samples_written = 0;
            while (samples_written < tts_samples) {
                int chunk = (tts_samples - samples_written > 1000) ? 1000 : (tts_samples - samples_written);
                i2s_channel_write(i2s_handle, &tts_buf[samples_written * 2], chunk * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
                samples_written += chunk;
                vTaskDelay(pdMS_TO_TICKS(1)); // Yield
            }
            free(tts_buf);
            
            // Flush DMA with silence to prevent last-buffer repeating
            int16_t* silence_buf = calloc(1000, sizeof(int16_t));
            if (silence_buf) {
                for (int f = 0; f < 10; f++) {
                    i2s_channel_write(i2s_handle, silence_buf, 1000 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
                }
                free(silence_buf);
            }
        }
        free(tts_text_to_play);
        tts_text_to_play = NULL;
    }

    // Wait a tiny bit for I2S DMA to finish
    vTaskDelay(pdMS_TO_TICKS(100));

    bsp_audio_set_amplifier(false);
    vTaskDelete(NULL);
}

void audio_play_notification(void) {
    if (tts_text_to_play) return; // Already playing
    xTaskCreate(audio_task, "audio_task", 65536, NULL, 5, NULL);
}

void audio_play_tts(const char* text) {
    if (tts_text_to_play) return; // Already playing
    if (text) {
        tts_text_to_play = strdup(text);
    }
    xTaskCreate(audio_task, "audio_task", 65536, NULL, 5, NULL);
}
