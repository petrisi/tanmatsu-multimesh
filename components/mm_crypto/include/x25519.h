// SPDX-License-Identifier: MIT
//
// X25519 key agreement, used by both networks for direct messages.
//
// The scalar multiplication comes from mbedtls via PSA rather than from the
// vendored Ed25519 code: Curve25519 ECDH is implemented, reviewed and rather
// faster than the big-integer arithmetic next door. Only the Edwards-to-
// Montgomery conversion has to be ours, because MeshCore identifies nodes by
// their *signing* key and expects the agreement to run on the equivalent point.
//
// What each network agrees on:
//
//   MeshCore    our Ed25519 identity against the peer's, converted. The raw
//               32-byte output is the secret -- there is no KDF, which is
//               upstream's choice, not ours.
//   Meshtastic  a separate Curve25519 key pair, exchanged in NodeInfo. Native
//               Montgomery keys, so no conversion.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define X25519_KEY_LEN 32

// Raw X25519. `scalar` is clamped by the implementation, so passing an already
// clamped Ed25519 scalar is fine. False on a degenerate peer key (an all-zero
// result, which means a low-order point and no shared secret worth having).
bool x25519_agree(uint8_t out[X25519_KEY_LEN], const uint8_t scalar[X25519_KEY_LEN],
                  const uint8_t peer_u[X25519_KEY_LEN]);

// Generate a Curve25519 key pair for Meshtastic PKI. `private_key` is the raw
// scalar; `public_key` the u-coordinate of scalar*G.
bool x25519_keypair(uint8_t public_key[X25519_KEY_LEN], uint8_t private_key[X25519_KEY_LEN]);

// Derive the public key for a private key we already hold, so the pair can be
// re-derived at boot from the one stored secret.
bool x25519_public_from_private(uint8_t public_key[X25519_KEY_LEN], const uint8_t private_key[X25519_KEY_LEN]);

// RFC 7748 section 6.1 test vectors. Same reasoning as the Ed25519 self-test: a
// wrong byte order here produces a secret nobody else can reproduce, and the
// only symptom is that direct messages silently never decrypt.
bool x25519_selftest(void);
