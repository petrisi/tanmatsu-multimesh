// SPDX-License-Identifier: MIT
//
// Fake traffic for the UI prototype. Finnish text throughout, deliberately:
// the whole point of the custom font is that ä/ö/å render in a monospace cell.
// Several messages are long enough to wrap, so the hanging indent can be judged.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_model.h"

// Channel palette. Chosen to stay legible on the dark background and to remain
// distinguishable from each other at a glance.
#define CH_CYAN   0xFF7FD4FF
#define CH_AMBER  0xFFFFD166
#define CH_GREEN  0xFF9BE564
#define CH_PINK   0xFFFF8FA3
#define CH_VIOLET 0xFFC59BFF
#define CH_ORANGE 0xFFFFAE73

const pax_col_t ch_palette[CH_PALETTE_SIZE] = {CH_CYAN, CH_AMBER, CH_GREEN, CH_PINK, CH_VIOLET, CH_ORANGE};

#define MESH_MC_ACCENT 0xFF14508C  // MeshCore blue
#define MESH_MT_ACCENT 0xFF1B7A46  // Meshtastic green

message_t* model_push(mesh_state_t* mesh, uint8_t channel, const char* sender, bool sender_named, const char* text,
                      bool outgoing) {
    message_t* msg = &mesh->messages[mesh->head];
    memset(msg, 0, sizeof(*msg));

    msg->used         = true;
    msg->seq          = mesh->next_seq++;
    msg->channel      = channel;
    msg->outgoing     = outgoing;
    msg->sender_named = sender_named;
    msg->timestamp    = (uint32_t)time(NULL);
    msg->tx           = outgoing ? TX_QUEUED : TX_NONE;
    snprintf(msg->sender, sizeof(msg->sender), "%s", sender);
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    msg->rssi_dbm  = -70;
    msg->snr_db_x4 = 24;
    msg->hops      = 0;

    mesh->head = (mesh->head + 1) % MAX_MESSAGES;
    if (mesh->count < MAX_MESSAGES) mesh->count++;
    return msg;
}

// Push with explicit radio metadata and a back-dated timestamp, so the seeded
// log looks like it accumulated over time rather than all at once.
static void seed(mesh_state_t* mesh, uint8_t channel, const char* sender, bool named, const char* text, int rssi,
                 int snr_x4, uint8_t hops, int seconds_ago) {
    message_t* msg = model_push(mesh, channel, sender, named, text, false);
    msg->rssi_dbm  = rssi;
    msg->snr_db_x4 = snr_x4;
    msg->hops      = hops;
    msg->timestamp = (uint32_t)(time(NULL) - seconds_ago);

    // Plausible routing so the detail view has something to show. Both sets of
    // fields are filled; the view picks whichever the active network uses.
    for (int i = 0; i < hops && i < (int)sizeof(msg->path); i++) {
        msg->path[i] = (uint8_t)(0x2a + i * 0x37 + hops * 0x11);
    }
    msg->path_len  = hops;
    msg->hop_start = 3;
    msg->hop_limit = (uint8_t)(hops > 3 ? 0 : 3 - hops);
    if (hops > 0) snprintf(msg->relayed_by, sizeof(msg->relayed_by), "%s", hops > 1 ? "owl7" : "c3d4");
}

static void add_channel(mesh_state_t* mesh, const char* name, const char* display, const char* secret,
                        pax_col_t color) {
    if (mesh->channel_count >= MAX_CHANNELS) return;
    channel_t* ch = &mesh->channels[mesh->channel_count++];
    snprintf(ch->name, sizeof(ch->name), "%s", name);
    snprintf(ch->display, sizeof(ch->display), "%s", display);
    snprintf(ch->secret, sizeof(ch->secret), "%s", secret);
    ch->color = color;
}

// A previously sent message, already resolved, so the settled states can be
// judged next to live ones.
static void seed_sent(mesh_state_t* mesh, uint8_t channel, const char* text, tx_state_t state, uint8_t repeats,
                      int seconds_ago) {
    message_t* msg = model_push(mesh, channel, "Petri", true, text, true);
    msg->tx        = state;
    msg->repeats   = repeats;
    msg->timestamp = (uint32_t)(time(NULL) - seconds_ago);
}

void mock_data_init(app_model_t* model) {
    memset(model, 0, sizeof(*model));

    model->radio        = RADIO_RX;
    model->battery_pct  = 87;
    model->charging     = false;
    model->time_synced  = true;
    model->history_pos  = -1;
    snprintf(model->identity.name, sizeof(model->identity.name), "Petri");
    snprintf(model->identity.short_name, sizeof(model->identity.short_name), "PSim");
    snprintf(model->identity.node_id, sizeof(model->identity.node_id), "!30eda0e2");

    mesh_state_t* mc = &model->mesh[MESH_MC];
    mc->name         = "MeshCore";
    mc->accent       = MESH_MC_ACCENT;
    add_channel(mc, "Public", "publ", "8b3387e9c5cdea6ac9e5edbaa115cd72", CH_CYAN);
    add_channel(mc, "OH6-alue", "OH6", "3f10aa77c1de40928b6631ee0d55ac14", CH_AMBER);
    add_channel(mc, "Labra", "labi", "0011223344556677889900aabbccddee", CH_PINK);

    mesh_state_t* mt = &model->mesh[MESH_MT];
    mt->name         = "Meshtastic";
    mt->accent       = MESH_MT_ACCENT;
    add_channel(mt, "EdgeFastLow", "EFL", "AQ==", CH_GREEN);
    add_channel(mt, "Yksityinen", "priv", "1PG7OiApB1nwvP+rz05pAQ==", CH_VIOLET);

    // MeshCore: senders name themselves inside the message, so every sender is
    // "named" -- there is no id fallback on this network.
    seed(mc, 0, "OH6ABC", true, "moikka kaikille, testataan kuuluvuutta", -92, 20, 1, 3400);
    seed(mc, 1, "Vuores", true, "kuittaan täältä", -78, 36, 0, 3300);
    seed(mc, 0, "MeshRelay-1", true, "toistin pystyssä Pyynikillä", -101, 8, 2, 3100);
    seed(mc, 2, "Petri", true, "labrassa uusi ääkkösfontti käytössä", -55, 44, 0, 2900);
    seed(mc, 1, "OH6XYZ", true, "näkyy hyvin, kiitos", -88, 28, 1, 2700);
    seed(mc, 0, "Vuores", true,
         "täällä hyvin kuuluu, säätä riittää vaikka muille jakaa. Antenni on nyt katolla ja ero edelliseen "
         "paikkaan on todella iso, ehkä kymmenen desibeliä koko matkalla",
         -78, 36, 0, 2500);
    seed(mc, 0, "Hervanta", true, "kuka on ollut yöllä liikkeellä?", -94, 16, 2, 2300);
    seed(mc, 2, "Petri", true, "ÄÖÅ isoina, äöå pieninä", -55, 44, 0, 2100);
    seed(mc, 1, "OH6ABC", true, "pakkanen kiristyy, akut kärsii", -90, 22, 1, 1900);
    seed(mc, 0, "Jyväskylä", true,
         "yöllä oli pitkä yhteys, jopa neljä hyppyä ja silti viesti tuli perille alle minuutissa", -97, 12, 3, 1700);
    seed(mc, 2, "Nokia", true, "testiviesti labrasta", -60, 40, 0, 1500);
    seed(mc, 0, "MeshRelay-1", true, "toistin käynnistyi uudelleen", -101, 6, 2, 1300);
    seed(mc, 1, "Vuores", true, "kuuluuko vielä?", -80, 32, 0, 1100);
    seed(mc, 0, "OH6XYZ", true,
         "pitkä testi siitä miten rivitys toimii kun viesti on todella pitkä ja sisältää myös ääkkösiä kuten "
         "höyrylaiva, sähköinen ja määrä",
         -86, 26, 1, 900);
    seed(mc, 2, "Petri", true, "kanavan värit näyttävät hyvältä", -55, 44, 0, 700);
    seed_sent(mc, 0, "kuuluuko minua?", TX_CONFIRMED, 3, 620);
    seed(mc, 0, "Hervanta", true, "samaa mieltä", -94, 18, 2, 500);
    seed_sent(mc, 2, "tämä ei mennyt perille", TX_FAILED, 0, 420);
    seed(mc, 1, "OH6ABC", true, "hyvää yötä kaikille", -91, 20, 1, 300);
    seed(mc, 0, "Vuores", true, "öitä", -78, 36, 0, 120);

    // Meshtastic: short names come from NodeInfo. Nodes we have not heard a
    // NodeInfo from are shown by node id, told apart by colour alone.
    seed(mt, 0, "elk1", true, "EFL testi, kuuluuko?", -84, 24, 1, 3200);
    seed(mt, 0, "c3d4", false, "kuuluu Tampereelta asti", -95, 14, 2, 3000);
    seed(mt, 1, "zeb0", true, "yksityinen kanava toimii myös", -70, 40, 0, 2800);
    seed(mt, 0, "elk1", true,
         "sää on kylmä ja akku kestää silti yllättävän hyvin. Mittasin yön yli noin kaksitoista prosenttia "
         "kulutusta, mikä riittää helposti koko viikonlopun retkelle",
         -90, 18, 1, 2600);
    seed(mt, 0, "7faa", false, "kuka kuulee tämän?", -95, 16, 2, 2400);
    seed(mt, 0, "owl7", true, "minä kuulen, signaali heikko", -99, 8, 3, 2200);
    seed(mt, 1, "zeb0", true, "vaihdoin antennin, testataan", -66, 44, 0, 2000);
    seed(mt, 0, "elk1", true, "paljon parempi nyt", -72, 38, 0, 1800);
    seed(mt, 0, "b201", false, "uusi solmu verkossa, ei vielä nimeä", -88, 22, 1, 1600);
    seed(mt, 0, "owl7", true,
         "toistimen sijainti on nyt vahvistettu ja kuuluvuus kattaa käytännössä koko kaupungin keskustan sekä "
         "pohjoiset lähiöt",
         -93, 14, 2, 1400);
    seed(mt, 1, "zeb0", true, "hyvä kuulla", -68, 42, 0, 1200);
    seed(mt, 0, "c3d4", false, "täällä sataa lunta", -95, 12, 2, 1000);
    seed(mt, 0, "elk1", true, "täällä myös, ajokeli on kehno", -74, 36, 0, 800);
    seed(mt, 0, "owl7", true, "yöpakkanen tulossa", -97, 10, 3, 600);
    seed_sent(mt, 0, "kiitos tiedosta", TX_CONFIRMED, 2, 500);
    seed(mt, 0, "elk1", true, "hyvää yötä äiti", -80, 30, 1, 200);

    // Both meshes start pinned to live traffic with nothing selected.
    for (int i = 0; i < MESH_COUNT; i++) {
        model->mesh[i].pinned       = true;
        model->mesh[i].selected_seq = -1;
    }
}
