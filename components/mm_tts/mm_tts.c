#include "mm_tts.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_log.h"
#include "flite.h"

static const char* TAG = "mm_tts";

extern cst_voice *register_cmu_us_kal(const char *voxdir);
static cst_voice *flite_voice = NULL;

void mm_tts_init(void) {
    flite_init();
    flite_voice = register_cmu_us_kal(NULL);
    if (!flite_voice) {
        ESP_LOGE(TAG, "Failed to load Flite voice");
    }
}

static char* apply_finglish(const char* text) {
    if (!text) return NULL;
    char* out = malloc(strlen(text) + 1); // No expansion needed anymore
    if (!out) return NULL;
    char* p = out;
    
    for (int i = 0; text[i] != '\0'; ) {
        unsigned char c = (unsigned char)text[i];
        
        // Handle UTF-8 ä, ö, Ä, Ö, å, Å
        if (c == 0xC3 && text[i+1] != '\0') {
            unsigned char c2 = (unsigned char)text[i+1];
            if (c2 == 0xA4 || c2 == 0x84) { // ä, Ä
                *p++ = 'a';
                i += 2;
                continue;
            } else if (c2 == 0xB6 || c2 == 0x96) { // ö, Ö
                *p++ = 'o';
                i += 2;
                continue;
            } else if (c2 == 0xA5 || c2 == 0x85) { // å, Å
                *p++ = 'o';
                i += 2;
                continue;
            }
        }
        
        // Pass everything else through (Flite expects mostly ASCII)
        *p++ = text[i];
        i++;
    }
    *p = '\0';
    return out;
}

int16_t* mm_tts_generate(const char* text, int* out_samples) {
    if (!text || !out_samples || !flite_voice) return NULL;
    
    char* finglish = apply_finglish(text);
    ESP_LOGI(TAG, "Generating TTS for: %s (Finglish: %s)", text, finglish ? finglish : "NULL");
    
    cst_wave *w = flite_text_to_wave(finglish ? finglish : text, flite_voice);
    if (finglish) free(finglish);
    
    if (!w) {
        ESP_LOGE(TAG, "Flite synthesis failed");
        return NULL;
    }
    
    int16_t* out = malloc(w->num_samples * 2 * sizeof(int16_t));
    if (!out) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        delete_wave(w);
        return NULL;
    }
    
    // Convert mono to stereo
    for (int i = 0; i < w->num_samples; i++) {
        out[i*2] = w->samples[i];
        out[i*2 + 1] = w->samples[i];
    }
    
    *out_samples = w->num_samples;
    delete_wave(w);
    return out;
}
