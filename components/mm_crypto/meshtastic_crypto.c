// SPDX-License-Identifier: MIT

#include "meshtastic_crypto.h"
#include <string.h>
#include "esp_log.h"
#include "psa/crypto.h"

static const char TAG[] = "mt_crypto";

// The public default channel PSK all Meshtastic devices ship with.
static const uint8_t MT_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

bool mt_key_expand(const uint8_t* psk, size_t psk_len, mt_key_t* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (psk == NULL || psk_len == 0) return false;

    if (psk_len == 1) {
        uint8_t index = psk[0];
        if (index == 0) return false;  // encryption explicitly disabled
        memcpy(out->bytes, MT_DEFAULT_PSK, sizeof(MT_DEFAULT_PSK));
        out->length = sizeof(MT_DEFAULT_PSK);
        // Index 1 means the default key unchanged; each further index bumps the
        // last byte by one.
        out->bytes[out->length - 1] = (uint8_t)(out->bytes[out->length - 1] + index - 1);
        return true;
    }

    if (psk_len > MT_MAX_KEY_SIZE) return false;
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
