// SPDX-License-Identifier: MIT

#include "meshtastic_crypto.h"
#include <string.h>
#include "esp_log.h"
#include "psa/crypto.h"
#include "x25519.h"

static const char TAG[] = "mt_crypto";

// The public default channel PSK all Meshtastic devices ship with.
static const uint8_t MT_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

bool mt_key_expand(const uint8_t* psk, size_t psk_len, mt_key_t* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (psk_len > MT_MAX_KEY_SIZE) return false;

    // No PSK at all: the channel is unencrypted. length 0 is a valid key here,
    // and the cipher becomes a pass-through.
    if (psk == NULL || psk_len == 0) return true;

    if (psk_len == 1) {
        uint8_t index = psk[0];
        if (index == 0) return true;  // encryption explicitly disabled
        memcpy(out->bytes, MT_DEFAULT_PSK, sizeof(MT_DEFAULT_PSK));
        out->length = sizeof(MT_DEFAULT_PSK);
        // Index 1 means the default key unchanged; each further index bumps the
        // last byte by one.
        out->bytes[out->length - 1] = (uint8_t)(out->bytes[out->length - 1] + index - 1);
        return true;
    }

    memcpy(out->bytes, psk, psk_len);
    // Short keys are zero-padded up to the next real AES size.
    out->length = (psk_len <= 16) ? 16 : 32;
    return true;
}

uint8_t mt_channel_hash(const char* name, const mt_key_t* key) {
    if (name == NULL || key == NULL) return 0;

    uint8_t hash = 0;
    for (const char* p = name; *p != '\0'; p++) {
        hash ^= (uint8_t)*p;
    }
    for (size_t i = 0; i < key->length; i++) {
        hash ^= key->bytes[i];
    }
    return hash;
}

bool mt_decrypt(const mt_key_t* key, uint32_t from_node, uint32_t packet_id, uint8_t* data, size_t length) {
    if (key == NULL || data == NULL || length == 0) return false;

    // An unencrypted channel: the payload is already plaintext, so succeeding
    // without touching it is the correct behaviour, not a silent failure.
    if (key->length == 0) return true;

    if (key->length != 16 && key->length != 32) return false;

    uint8_t nonce[16] = {0};
    // Packet id occupies a full 64-bit little-endian slot; the high word is
    // always zero for the 32-bit ids that actually go over the air.
    nonce[0] = (uint8_t)(packet_id & 0xFF);
    nonce[1] = (uint8_t)((packet_id >> 8) & 0xFF);
    nonce[2] = (uint8_t)((packet_id >> 16) & 0xFF);
    nonce[3] = (uint8_t)((packet_id >> 24) & 0xFF);
    nonce[8]  = (uint8_t)(from_node & 0xFF);
    nonce[9]  = (uint8_t)((from_node >> 8) & 0xFF);
    nonce[10] = (uint8_t)((from_node >> 16) & 0xFF);
    nonce[11] = (uint8_t)((from_node >> 24) & 0xFF);

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, key->length * 8);

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    if (psa_import_key(&attributes, key->bytes, key->length, &key_id) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key failed");
        return false;
    }

    // Multi-part API because the one-shot psa_cipher_decrypt() expects the IV
    // prepended to the ciphertext, and ours is derived rather than transmitted.
    psa_cipher_operation_t op     = PSA_CIPHER_OPERATION_INIT;
    bool                   ok     = false;
    uint8_t                out[MT_MAX_KEY_SIZE > 240 ? MT_MAX_KEY_SIZE : 240];
    size_t                 out_len = 0;
    size_t                 total   = 0;

    if (length > sizeof(out)) goto done;
    if (psa_cipher_decrypt_setup(&op, key_id, PSA_ALG_CTR) != PSA_SUCCESS) goto done;
    if (psa_cipher_set_iv(&op, nonce, sizeof(nonce)) != PSA_SUCCESS) goto done;
    if (psa_cipher_update(&op, data, length, out, sizeof(out), &out_len) != PSA_SUCCESS) goto done;
    total = out_len;
    if (psa_cipher_finish(&op, out + total, sizeof(out) - total, &out_len) != PSA_SUCCESS) goto done;
    total += out_len;

    if (total != length) goto done;
    memcpy(data, out, length);
    ok = true;

done:
    if (!ok) psa_cipher_abort(&op);
    psa_destroy_key(key_id);
    return ok;
}

// --- PKI direct messages -------------------------------------------------

#define PKI_TAG_LEN   8
#define PKI_NONCE_LEN 13
#define PKI_ALG       PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, PKI_TAG_LEN)

bool mt_pki_shared_key(uint8_t out[32], const uint8_t our_private_key[32], const uint8_t their_public_key[32]) {
    if (out == NULL || our_private_key == NULL || their_public_key == NULL) return false;

    uint8_t raw[X25519_KEY_LEN];
    if (!x25519_agree(raw, our_private_key, their_public_key)) return false;

    // Upstream hashes the agreement output before using it as an AES key. That
    // extra step is the only thing separating this from MeshCore's scheme, and
    // omitting it produces a key nobody else derives.
    size_t       digest_len = 0;
    psa_status_t status     = psa_hash_compute(PSA_ALG_SHA_256, raw, sizeof(raw), out, 32, &digest_len);
    memset(raw, 0, sizeof(raw));

    if (status != PSA_SUCCESS || digest_len != 32) {
        memset(out, 0, 32);
        return false;
    }
    return true;
}

// The nonce is the packet id, a per-packet random extension and the sender, in
// that order. Note the overlap: upstream writes the extension at offset 4, on
// top of the high half of a 64-bit packet id that is always zero for the 32-bit
// ids actually used. Reproduced rather than tidied -- both ends must build the
// same 13 bytes.
static void pki_nonce(uint8_t nonce[PKI_NONCE_LEN], uint32_t from_node, uint32_t packet_id, uint32_t extra_nonce) {
    memset(nonce, 0, PKI_NONCE_LEN);
    nonce[0]  = (uint8_t)(packet_id);
    nonce[1]  = (uint8_t)(packet_id >> 8);
    nonce[2]  = (uint8_t)(packet_id >> 16);
    nonce[3]  = (uint8_t)(packet_id >> 24);
    nonce[4]  = (uint8_t)(extra_nonce);
    nonce[5]  = (uint8_t)(extra_nonce >> 8);
    nonce[6]  = (uint8_t)(extra_nonce >> 16);
    nonce[7]  = (uint8_t)(extra_nonce >> 24);
    nonce[8]  = (uint8_t)(from_node);
    nonce[9]  = (uint8_t)(from_node >> 8);
    nonce[10] = (uint8_t)(from_node >> 16);
    nonce[11] = (uint8_t)(from_node >> 24);
}

static bool import_aead_key(const uint8_t key[32], psa_key_usage_t usage, psa_key_id_t* out_id) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PKI_ALG);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);

    *out_id = PSA_KEY_ID_NULL;
    return psa_import_key(&attributes, key, 32, out_id) == PSA_SUCCESS;
}

bool mt_pki_encrypt(const uint8_t shared_key[32], uint32_t from_node, uint32_t packet_id, uint32_t extra_nonce,
                    const uint8_t* plain, size_t length, uint8_t* out, size_t out_max) {
    if (shared_key == NULL || plain == NULL || out == NULL) return false;
    if (length == 0 || length + MT_PKI_OVERHEAD > out_max) return false;

    uint8_t nonce[PKI_NONCE_LEN];
    pki_nonce(nonce, from_node, packet_id, extra_nonce);

    psa_key_id_t key_id;
    if (!import_aead_key(shared_key, PSA_KEY_USAGE_ENCRYPT, &key_id)) return false;

    // PSA writes ciphertext and tag as one run, which is exactly the layout the
    // wire wants; the nonce extension is appended after them.
    size_t       written = 0;
    psa_status_t status  = psa_aead_encrypt(key_id, PKI_ALG, nonce, sizeof(nonce), NULL, 0, plain, length, out,
                                            out_max, &written);
    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS || written != length + PKI_TAG_LEN) {
        ESP_LOGE(TAG, "PKI encrypt failed: %d", (int)status);
        return false;
    }

    out[written]     = (uint8_t)(extra_nonce);
    out[written + 1] = (uint8_t)(extra_nonce >> 8);
    out[written + 2] = (uint8_t)(extra_nonce >> 16);
    out[written + 3] = (uint8_t)(extra_nonce >> 24);
    return true;
}

bool mt_pki_decrypt(const uint8_t shared_key[32], uint32_t from_node, uint32_t packet_id, const uint8_t* payload,
                    size_t length, uint8_t* out, size_t out_max, size_t* out_length) {
    if (shared_key == NULL || payload == NULL || out == NULL || out_length == NULL) return false;
    if (length <= MT_PKI_OVERHEAD) return false;

    // The last four bytes are the nonce extension, in the clear. Everything
    // before them -- ciphertext and tag together -- is what CCM verifies.
    size_t   sealed      = length - 4;
    uint32_t extra_nonce = (uint32_t)payload[length - 4] | ((uint32_t)payload[length - 3] << 8) |
                           ((uint32_t)payload[length - 2] << 16) | ((uint32_t)payload[length - 1] << 24);

    uint8_t nonce[PKI_NONCE_LEN];
    pki_nonce(nonce, from_node, packet_id, extra_nonce);

    psa_key_id_t key_id;
    if (!import_aead_key(shared_key, PSA_KEY_USAGE_DECRYPT, &key_id)) return false;

    size_t       written = 0;
    psa_status_t status  = psa_aead_decrypt(key_id, PKI_ALG, nonce, sizeof(nonce), NULL, 0, payload, sealed, out,
                                            out_max, &written);
    psa_destroy_key(key_id);

    // A failed tag is the ordinary outcome for a packet addressed to someone
    // else, so this is not logged as an error.
    if (status != PSA_SUCCESS) return false;

    *out_length = written;
    return true;
}

bool mt_pki_selftest(void) {
    // The RFC 7748 pairs, reused here: what matters is that two distinct keys
    // agree on the same secret, not which keys they are.
    static const uint8_t alice_private[32] = {0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1,
                                              0x72, 0x51, 0xb2, 0x66, 0x45, 0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0,
                                              0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a};
    static const uint8_t alice_public[32]  = {0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54, 0x74, 0x8b, 0x7d,
                                              0xdc, 0xb4, 0x3e, 0xf7, 0x5a, 0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38,
                                              0x1a, 0xf4, 0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a};
    static const uint8_t bob_private[32]   = {0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f,
                                              0x8b, 0x83, 0x80, 0x0e, 0xe6, 0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18,
                                              0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
    static const uint8_t bob_public[32]    = {0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61,
                                              0xc2, 0xec, 0xe4, 0x35, 0x37, 0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78,
                                              0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f};

    uint8_t sender_key[32], receiver_key[32];
    if (!mt_pki_shared_key(sender_key, alice_private, bob_public)) return false;
    if (!mt_pki_shared_key(receiver_key, bob_private, alice_public)) return false;
    if (memcmp(sender_key, receiver_key, 32) != 0) return false;

    const uint8_t plain[] = {'d', 'i', 'r', 'e', 'c', 't'};
    const uint32_t from = 0x12345678, id = 0x9abcdef0, extra = 0x0f1e2d3c;

    uint8_t sealed[sizeof(plain) + MT_PKI_OVERHEAD];
    if (!mt_pki_encrypt(sender_key, from, id, extra, plain, sizeof(plain), sealed, sizeof(sealed))) return false;

    uint8_t recovered[sizeof(plain) + 16];
    size_t  recovered_len = 0;
    if (!mt_pki_decrypt(receiver_key, from, id, sealed, sizeof(sealed), recovered, sizeof(recovered), &recovered_len))
        return false;
    if (recovered_len != sizeof(plain) || memcmp(recovered, plain, sizeof(plain)) != 0) return false;

    // A flipped ciphertext bit must fail, or the tag is not being checked.
    sealed[0] ^= 0x01;
    if (mt_pki_decrypt(receiver_key, from, id, sealed, sizeof(sealed), recovered, sizeof(recovered), &recovered_len))
        return false;

    return true;
}
