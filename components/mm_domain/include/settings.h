// SPDX-License-Identifier: MIT
//
// Persistence for everything the user configures: channels, identity and a few
// interface preferences.
//
// Stored in this app's own NVS namespace rather than the shared `system` one.
// The MeshCore client keeps its radio settings in `system`, and MultiMesh reads
// those, but it must never write there -- two apps fighting over one key set is
// how configuration mysteriously changes.
//
// Channel secrets are stored in the clear. That is what every mesh client does:
// the device is the trust boundary, and NVS is not encrypted unless flash
// encryption is enabled (it is not, on a device that ships with secure boot
// permanently disabled). Anyone with the hardware has the keys.

#pragma once

#include <stdbool.h>
#include "app_model.h"

// Open the namespace. False if NVS is unusable, in which case the app runs with
// defaults and nothing persists.
bool settings_init(void);

// Fill `model` from storage. Returns false when nothing was stored yet, leaving
// the model untouched so the caller can apply defaults.
bool settings_load(app_model_t* model);

// Channels for one network. Called after any create, edit, delete or reorder.
void settings_save_channels(const app_model_t* model, mesh_id_t mesh);

void settings_save_identity(const app_model_t* model);

// Active network, metadata column, and each network's selected input channel.
void settings_save_prefs(const app_model_t* model);

// The channels a first run should start with: the well-known public channel on
// MeshCore and EdgeFastLow on Meshtastic. Both use published keys -- neither is
// a secret, and both are what their networks expect a new node to join.
void settings_apply_default_channels(app_model_t* model);

// Derive the node number from the factory MAC and format it as Meshtastic's
// "!aabbccdd". Stable for the life of the device, which is what lets other
// clients attach a name to us later.
void settings_derive_node_id(identity_t* identity);

// Load the MeshCore identity seed, generating and storing one on first run.
// Only the 32-byte seed is persisted; the key pair is derived from it at every
// boot, so there is one thing to keep secret rather than three.
//
// The seed is stored in the clear, like the channel keys and for the same
// reason: on a device that ships with secure boot permanently disabled, anyone
// holding the hardware can read it whatever we do.
//
// `derive` performs the actual Ed25519 derivation. It is passed in so this
// component keeps no dependency on the crypto, which lives above it.
bool settings_load_identity_keypair(identity_t* identity,
                                    bool (*derive)(uint8_t pub[32], uint8_t priv[64], const uint8_t seed[32]));

// The same thing for Meshtastic, which needs a Curve25519 pair of its own for
// end-to-end direct messages. Stored as the private key rather than a seed,
// because that is what the curve generates and what upstream clients store.
//
// `generate` is used on first run, `derive` on every boot afterwards; both are
// passed in so this component keeps no dependency on the crypto.
bool settings_load_mt_keypair(identity_t* identity, bool (*generate)(uint8_t pub[32], uint8_t priv[32]),
                              bool (*derive)(uint8_t pub[32], const uint8_t priv[32]));
