// SPDX-License-Identifier: MIT
//
// The parts of the Meshtastic stack that do not fit behind mesh_net_t.
//
// Chiefly the information exchange, which is a deliberate act rather than
// something the protocol does on its own.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "app_model.h"

// Ask one node to swap identities: our NodeInfo, addressed to them, with
// want_response set so they send theirs back.
//
// This is Meshtastic's key exchange and there is no other. Direct messages are
// encrypted to the recipient public key, current firmware refuses outright to
// send one to a node whose key it does not hold, and NodeInfo is deliberately
// excluded from that encryption so it can travel first. Until this has happened
// in both directions, two nodes cannot message each other privately at all.
//
// Returns the frame length, or 0.
uint8_t mt_encode_info_exchange(mesh_state_t* mesh, const identity_t* identity, const node_t* peer, uint8_t* out,
                                size_t out_max);
// The hop limit outgoing packets carry. Set from the configuration at start and
// whenever it changes, including the per-session override.
void mt_set_hop_limit(uint8_t hops);

// What we advertise ourselves as in NodeInfo.
void mt_set_role(uint8_t role);