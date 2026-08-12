// SPDX-License-Identifier: MIT
//
// Meshtastic modem profiles. Pure data and one formatter, so both the UI and
// the network stack can read them without either depending on the other.

#include <stdio.h>
#include <string.h>
#include "app_model.h"

// Frequencies are stored, not derived. See app_model.h for why.
//
// EdgeFastLow is a community profile rather than an upstream preset, so its
// slot is a description of the arithmetic that produced the frequency, not a
// slot upstream would compute: EU_868 carries no 62.5 kHz preset at all, and
// the narrow presets live in a different region with 10.4 kHz of padding.
//
// LongFast EU is exactly upstream's: EU_868 spans 869.4-869.65 MHz with no
// padding, and 250 kHz bandwidth divides that into a single slot, so every
// LongFast node in the region lands on the same frequency whatever its channel
// is called.
static const mt_profile_t PROFILES[] = {
    [MT_PROFILE_EFL_EU]      = {.name     = "EdgeFastLow EU",
                                .radio    = {.freq_hz = 869431250, .sf = 8, .cr = 8, .bw = 62},
                                .bw_label = "62.5",
                                .slot     = 1,
                                .slots    = 4},
    [MT_PROFILE_LONGFAST_EU] = {.name     = "LongFast EU",
                                .radio    = {.freq_hz = 869525000, .sf = 11, .cr = 5, .bw = 250},
                                .bw_label = "250",
                                .slot     = 1,
                                .slots    = 1},
};

const uint16_t mt_bw_values[MT_BW_COUNT] = {62, 125, 250, 500};

const char* mt_bw_label(uint16_t bw) {
    // 62 is the driver's label for 62.5 kHz, so printing the number would say
    // something that is not true of the air.
    switch (bw) {
        case 62: return "62.5";
        case 125: return "125";
        case 250: return "250";
        case 500: return "500";
        default: return "?";
    }
}

const mt_profile_t* mt_profile_at(int id) {
    if (id < 0 || id >= (int)(sizeof(PROFILES) / sizeof(PROFILES[0]))) return NULL;
    if (PROFILES[id].name == NULL) return NULL;  // MT_PROFILE_CUSTOM has no entry
    return &PROFILES[id];
}

const char* mt_profile_name(int id) {
    const mt_profile_t* p = mt_profile_at(id);
    if (p) return p->name;
    return id == MT_PROFILE_CUSTOM ? "Custom" : "?";
}

void mt_radio_resolve(const settings_t* settings, mt_radio_t* out) {
    if (out == NULL) return;

    const mt_profile_t* p = settings ? mt_profile_at(settings->mt_profile) : NULL;
    if (p) {
        *out = p->radio;
        return;
    }
    if (settings && settings->mt_profile == MT_PROFILE_CUSTOM) {
        *out = settings->mt_custom;
        return;
    }
    // Unreadable stored value: fall back to the default profile rather than
    // transmitting on whatever the bytes happened to say.
    *out = PROFILES[MT_PROFILE_EFL_EU].radio;
}

void mt_radio_describe(const settings_t* settings, char* out, size_t out_size) {
    if (out == NULL || out_size == 0) return;

    mt_radio_t radio;
    mt_radio_resolve(settings, &radio);

    const mt_profile_t* p     = settings ? mt_profile_at(settings->mt_profile) : NULL;
    const char*         bw    = p ? p->bw_label : mt_bw_label(radio.bw);

    // Trailing zeros trimmed, so 869.431250 reads as 869.43125 and 869.525000
    // as 869.525 -- the form people actually quote at each other.
    char mhz[16];
    snprintf(mhz, sizeof(mhz), "%lu.%06lu", (unsigned long)(radio.freq_hz / 1000000u),
             (unsigned long)(radio.freq_hz % 1000000u));
    for (int i = (int)strlen(mhz) - 1; i > 0 && mhz[i] == '0'; i--) mhz[i] = '\0';
    size_t len = strlen(mhz);
    if (len && mhz[len - 1] == '.') mhz[len - 1] = '\0';

    char where[16];
    if (p) {
        snprintf(where, sizeof(where), "slot %u/%u", (unsigned)p->slot, (unsigned)p->slots);
    } else {
        snprintf(where, sizeof(where), "custom");
    }

    snprintf(out, out_size, "%s MHz SF%u BW%s CR4:%u %s", mhz, (unsigned)radio.sf, bw, (unsigned)radio.cr, where);
}
