#pragma once
#include <stdbool.h>
#include <stdint.h>

// Initialize SAM TTS engine (only needs to be called once)
void mm_tts_init(void);

// Generate audio for a string of text.
// Returns a dynamically allocated 16-bit signed stereo buffer at 22050 Hz.
// The caller is responsible for freeing the returned buffer.
// out_samples will be set to the number of stereo samples (number of L/R pairs).
int16_t* mm_tts_generate(const char* text, int* out_samples);
