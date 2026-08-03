// SPDX-License-Identifier: MIT
//
// TEMPORARY -- see demo_traffic.h. Delete once the radio feeds the real stacks.

#include "demo_traffic.h"

#if MM_DEMO_TRAFFIC

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define INCOMING_EVERY_MS 12000
#define ACTIVITY_EVERY_MS 4500

static uint32_t next_incoming_ms;
static uint32_t next_activity_ms;
static int      incoming_index;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// Routing that looks like something a real mesh would produce, so the detail
// view has both a MeshCore path and a Meshtastic hop budget to render.
static void fake_routing(message_t* msg, uint8_t hops) {
    msg->hops     = hops;
    msg->path_len = hops > sizeof(msg->path) ? (uint8_t)sizeof(msg->path) : hops;
    for (int i = 0; i < msg->path_len; i++) {
        msg->path[i] = (uint8_t)(0x2a + i * 0x37 + hops * 0x11);
    }
    msg->hop_start = 3;
    msg->hop_limit = (uint8_t)(hops > 3 ? 0 : 3 - hops);
    if (hops > 0) snprintf(msg->relayed_by, sizeof(msg->relayed_by), "%s", hops > 1 ? "owl7" : "c3d4");
}

static void seed(mesh_state_t* mesh, uint8_t channel, const char* sender, bool named, const char* text, int rssi,
                 int snr_x4, uint8_t hops, int seconds_ago) {
    if (channel >= mesh->channel_count) channel = 0;
    message_t* msg = model_push(mesh, channel, sender, named, text, false);
    msg->rssi_dbm  = rssi;
    msg->snr_db_x4 = snr_x4;
    msg->timestamp = (uint32_t)(time(NULL) - seconds_ago);
    fake_routing(msg, hops);
}

static void seed_sent(mesh_state_t* mesh, uint8_t channel, const char* text, tx_state_t state, uint8_t repeats,
                      int seconds_ago) {
    if (channel >= mesh->channel_count) channel = 0;
    message_t* msg = model_push(mesh, channel, "you", true, text, true);
    msg->tx        = state;
    msg->repeats   = repeats;
    msg->timestamp = (uint32_t)(time(NULL) - seconds_ago);
}

void demo_traffic_seed(app_model_t* model) {
    mesh_state_t* mc = &model->mesh[MESH_MC];
    seed(mc, 0, "OH6ABC", true, "moikka kaikille, testataan kuuluvuutta", -92, 20, 1, 3400);
    seed(mc, 1, "Vuores", true, "kuittaan täältä", -78, 36, 0, 3300);
    seed(mc, 0, "MeshRelay-1", true, "toistin pystyssä Pyynikillä", -101, 8, 2, 3100);
    seed(mc, 2, "Tampere", true, "labrassa uusi ääkkösfontti käytössä", -55, 44, 0, 2900);
    seed(mc, 1, "OH6XYZ", true, "näkyy hyvin, kiitos", -88, 28, 1, 2700);
    seed(mc, 0, "Vuores", true,
         "täällä hyvin kuuluu, säätä riittää vaikka muille jakaa. Antenni on nyt katolla ja ero edelliseen "
         "paikkaan on todella iso, ehkä kymmenen desibeliä koko matkalla",
         -78, 36, 0, 2500);
    seed(mc, 0, "Hervanta", true, "kuka on ollut yöllä liikkeellä?", -94, 16, 2, 2300);
    seed(mc, 2, "Tampere", true, "ÄÖÅ isoina, äöå pieninä", -55, 44, 0, 2100);
    seed(mc, 0, "Jyväskylä", true,
         "yöllä oli pitkä yhteys, jopa neljä hyppyä ja silti viesti tuli perille alle minuutissa", -97, 12, 3, 1700);
    seed(mc, 0, "MeshRelay-1", true, "toistin käynnistyi uudelleen", -101, 6, 2, 1300);
    seed(mc, 0, "OH6XYZ", true,
         "pitkä testi siitä miten rivitys toimii kun viesti on todella pitkä ja sisältää myös ääkkösiä kuten "
         "höyrylaiva, sähköinen ja määrä",
         -86, 26, 1, 900);
    seed_sent(mc, 0, "kuuluuko minua?", TX_CONFIRMED, 3, 620);
    seed(mc, 0, "Hervanta", true, "samaa mieltä", -94, 18, 2, 500);
    seed_sent(mc, 0, "tämä ei mennyt perille", TX_FAILED, 0, 420);
    seed(mc, 0, "Vuores", true, "öitä", -78, 36, 0, 120);

    mesh_state_t* mt = &model->mesh[MESH_MT];
    seed(mt, 0, "elk1", true, "EFL testi, kuuluuko?", -84, 24, 1, 3200);
    seed(mt, 0, "c3d4", false, "kuuluu Tampereelta asti", -95, 14, 2, 3000);
    seed(mt, 0, "elk1", true,
         "sää on kylmä ja akku kestää silti yllättävän hyvin. Mittasin yön yli noin kaksitoista prosenttia "
         "kulutusta, mikä riittää helposti koko viikonlopun retkelle",
         -90, 18, 1, 2600);
    seed(mt, 0, "7faa", false, "kuka kuulee tämän?", -95, 16, 2, 2400);
    seed(mt, 0, "owl7", true, "minä kuulen, signaali heikko", -99, 8, 3, 2200);
    seed(mt, 0, "elk1", true, "paljon parempi nyt", -72, 38, 0, 1800);
    seed(mt, 0, "b201", false, "uusi solmu verkossa, ei vielä nimeä", -88, 22, 1, 1600);
    seed(mt, 0, "owl7", true,
         "toistimen sijainti on nyt vahvistettu ja kuuluvuus kattaa käytännössä koko kaupungin keskustan sekä "
         "pohjoiset lähiöt",
         -93, 14, 2, 1400);
    seed(mt, 0, "c3d4", false, "täällä sataa lunta", -95, 12, 2, 1000);
    seed_sent(mt, 0, "kiitos tiedosta", TX_CONFIRMED, 2, 500);
    seed(mt, 0, "elk1", true, "hyvää yötä äiti", -80, 30, 1, 200);

    next_incoming_ms = now_ms() + INCOMING_EVERY_MS;
    next_activity_ms = now_ms() + ACTIVITY_EVERY_MS;
}

static const struct {
    const char* sender;
    bool        named;
    const char* text;
} incoming[] = {
    {"OH6ABC", true, "kuuluuko siellä vielä?"},
    {"elk1", true, "sää selkenee illalla"},
    {"c3d4", false, "uusi solmu näkyvissä"},
    {"Vuores", true, "lähdössä ulos, testataan matkalla kuuluvuutta pidemmällä viestillä"},
    {"owl7", true, "kuittaan"},
};

bool demo_traffic_tick(app_model_t* model, demo_event_t* out) {
    demo_event_t event = {0};
    uint32_t     t     = now_ms();

    if (t >= next_activity_ms) {
        next_activity_ms = t + ACTIVITY_EVERY_MS;
        event.activity   = true;  // position/telemetry/foreign channel
    }

    mesh_state_t* mesh = model_active(model);
    if (t >= next_incoming_ms && mesh->channel_count > 0) {
        next_incoming_ms = t + INCOMING_EVERY_MS;

        int     i  = incoming_index++ % (int)(sizeof(incoming) / sizeof(incoming[0]));
        uint8_t ch = (uint8_t)(incoming_index % mesh->channel_count);

        message_t* msg = model_push(mesh, ch, incoming[i].sender, incoming[i].named, incoming[i].text, false);
        msg->rssi_dbm  = -70 - (incoming_index * 7) % 30;
        msg->snr_db_x4 = 12 + (incoming_index * 5) % 32;
        fake_routing(msg, (uint8_t)(incoming_index % 3));

        event.message       = true;
        event.message_color = mesh->channels[ch].color;
    }

    if (out) *out = event;
    return event.message;
}

#endif  // MM_DEMO_TRAFFIC

