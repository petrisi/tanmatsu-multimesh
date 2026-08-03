// SPDX-License-Identifier: MIT
//
// Implemented against the PSA Crypto API rather than the legacy mbedtls_aes_* /
// mbedtls_sha256_* calls: ESP-IDF 6.0 ships mbedtls 4.x, where those headers
// moved to mbedtls/private/ and PSA is the supported interface. (The upstream
// Tanmatsu MeshCore client still uses the legacy API, which is why it pins
// ESP-IDF 5.5.1.)

#include "meshcore_crypto.h"
#include <string.h>
#include "esp_log.h"
#include "psa/crypto.h"

static const char TAG[] = "mc_crypto";

const uint8_t MC_PUBLIC_CHANNEL_KEY[MC_CIPHER_KEY_SIZE] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72,
};

bool mc_crypto_init(void) {
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return false;
    }
    return true;
}

uint8_t mc_channel_hash(const uint8_t key[MC_CIPHER_KEY_SIZE]) {
    uint8_t      digest[32];
    size_t       digest_len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, key, MC_CIPHER_KEY_SIZE, digest, sizeof(digest),
                                           &digest_len);
    if (status != PSA_SUCCESS || digest_len == 0) {
        ESP_LOGE(TAG, "sha256 failed: %d", (int)status);
        return 0;
    }
    return digest[0];
}

// HMAC-SHA256 over `data` under `key`, into a caller-supplied 32-byte buffer.
static bool hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t out[32]) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, key_len * 8);

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    if (psa_import_key(&attributes, key, key_len, &key_id) != PSA_SUCCESS) return false;

    size_t       mac_len = 0;
    psa_status_t status  = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len, out, 32, &mac_len);
    psa_destroy_key(key_id);

    return status == PSA_SUCCESS && mac_len == 32;
}

// AES-128-ECB decrypt in place-ish: `in` -> `out`, length must be a block multiple.
static bool aes_ecb_decrypt(const uint8_t key[MC_CIPHER_KEY_SIZE], const uint8_t* in, size_t len, uint8_t* out,
                            size_t out_size) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, MC_CIPHER_KEY_SIZE * 8);

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    if (psa_import_key(&attributes, key, MC_CIPHER_KEY_SIZE, &key_id) != PSA_SUCCESS) return false;

    size_t       out_len = 0;
    psa_status_t status  = psa_cipher_decrypt(key_id, PSA_ALG_ECB_NO_PADDING, in, len, out, out_size, &out_len);
    psa_destroy_key(key_id);

    return status == PSA_SUCCESS && out_len == len;
}

bool mc_grp_decrypt(const mc_grp_txt_t* grp, const uint8_t key[MC_CIPHER_KEY_SIZE], mc_grp_msg_t* out) {
    if (grp == NULL || key == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

    // ECB decrypts whole blocks only; a truncated tail means a malformed frame.
    if (grp->cipher_length == 0 || (grp->cipher_length % MC_CIPHER_BLOCK) != 0) return false;

    uint8_t mac[32];
    if (!hmac_sha256(key, MC_CIPHER_KEY_SIZE, grp->cipher, grp->cipher_length, mac)) return false;
    if (memcmp(mac, grp->mac, MC_CIPHER_MAC_SIZE) != 0) return false;

    uint8_t plain[MC_MAX_PAYLOAD_SIZE];
    if (!aes_ecb_decrypt(key, grp->cipher, grp->cipher_length, plain, sizeof(plain))) return false;

    // timestamp(4) | text_type(1) | text(...)
    if (grp->cipher_length < 5) return false;
    memcpy(&out->timestamp, plain, 4);
    out->text_type = plain[4];

    size_t text_len = grp->cipher_length - 5;
    if (text_len >= sizeof(out->text)) text_len = sizeof(out->text) - 1;
    memcpy(out->text, &plain[5], text_len);
    out->text[text_len] = '\0';

    // Zero padding to the block size is already cut off by the NUL above; only
    // trailing whitespace still needs trimming so it does not render as blanks.
    size_t len = strlen(out->text);
    while (len > 0 && (out->text[len - 1] == ' ' || out->text[len - 1] == '\r' || out->text[len - 1] == '\n')) {
        out->text[--len] = '\0';
    }
    return true;
}
