// SPDX-License-Identifier: MIT
//
// The parts of the MeshCore stack that do not fit behind mesh_net_t, because
// they are things MeshCore does and Meshtastic does not.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Collect an acknowledgement the receive path built for a direct message.
//
// Reading a direct message obliges us to acknowledge it, and the acknowledgement
// proves we decrypted it rather than merely heard the frame. But receive runs on
// the event loop and transmitting blocks, so the frame is parked here for the
// loop to pick up and put on its own transmit queue.
//
// Returns false when there is nothing waiting. Call from the thread that owns
// the transmit queue.
bool mc_take_pending_ack(uint8_t* out, size_t out_max, uint8_t* out_len);
