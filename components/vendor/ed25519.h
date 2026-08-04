// SPDX-License-Identifier: MIT
//
// Ed25519 signing and verification (RFC 8032), for MeshCore adverts.
//
// Why this exists: ESP-IDF 6.0's mbedtls declares PSA_ALG_PURE_EDDSA but ships
// no implementation, and there is no Ed25519 anywhere else in the toolchain.
//
// Correctness over speed. All field and group arithmetic goes through
// mbedtls_mpi -- the same well-tested bignum layer the rest of the crypto uses,
// and one the ESP32-P4 accelerates in hardware -- rather than a hand-written
// carry chain. A hand-optimised ref10 port is perhaps twenty times faster and
// vastly easier to get subtly wrong: the reference Tanmatsu MeshCore client
// shipped one that produced valid-looking signatures every upstream verifier
// rejected. Adverts are rare enough that the speed does not matter.
//
// SHA-512 comes from PSA, because mbedtls 4.x made mbedtls/sha512.h private.
//
// This is NOT constant-time. It handles a long-term identity key, so the
// relevant question is whether an attacker can measure our timing: over a LoRa
// link that requires physical access to the device, at which point the key is
// readable from flash anyway. Do not reuse this where an attacker can time it.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ED25519_SEED_LEN       32
#define ED25519_PUBLIC_LEN     32
#define ED25519_PRIVATE_LEN    64
#define ED25519_SIGNATURE_LEN  64

// Derive a key pair from a 32-byte seed. The private key is SHA-512(seed) with
// the standard clamping applied to its first half.
bool ed25519_keypair(uint8_t public_key[ED25519_PUBLIC_LEN], uint8_t private_key[ED25519_PRIVATE_LEN],
                     const uint8_t seed[ED25519_SEED_LEN]);

bool ed25519_sign(uint8_t signature[ED25519_SIGNATURE_LEN], const uint8_t* message, size_t message_len,
                  const uint8_t public_key[ED25519_PUBLIC_LEN], const uint8_t private_key[ED25519_PRIVATE_LEN]);

// False for a bad signature, a malformed public key, or a non-canonical S.
bool ed25519_verify(const uint8_t signature[ED25519_SIGNATURE_LEN], const uint8_t* message, size_t message_len,
                    const uint8_t public_key[ED25519_PUBLIC_LEN]);

// Run the RFC 8032 test vectors. Call once at boot: a wrong implementation
// produces signatures that look fine locally and are rejected by every peer,
// which is close to undiagnosable from the air.
bool ed25519_selftest(void);
