// SPDX-License-Identifier: MIT
//
// Implemented against the PSA Crypto API rather than the legacy mbedtls_aes_* /
// mbedtls_sha256_* calls: ESP-IDF 6.0 ships mbedtls 4.x, where those headers
// moved to mbedtls/private/ and PSA is the supported interface. (The upstream
// Tanmatsu MeshCore client still uses the legacy API, which is why it pins
// ESP-IDF 5.5.1.)

#include "meshcore_crypto.h"
#include <string.h>
#include "ed25519.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include "x25519.h"

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

static bool sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    size_t       digest_len = 0;
    psa_status_t status     = psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &digest_len);
    if (status != PSA_SUCCESS || digest_len != 32) {
        ESP_LOGE(TAG, "sha256 failed: %d", (int)status);
        return false;
    }
    return true;
}

uint8_t mc_channel_hash(const uint8_t* key, size_t key_len) {
    uint8_t digest[32];
    if (key == NULL || !sha256(key, key_len, digest)) return 0;
    return digest[0];
}

bool mc_derive_hashtag_key(const char* name, uint8_t out[MC_CIPHER_KEY_SIZE]) {
    if (name == NULL || out == NULL || name[0] == '\0') return false;

    uint8_t digest[32];
    if (!sha256((const uint8_t*)name, strlen(name), digest)) return false;
    memcpy(out, digest, MC_CIPHER_KEY_SIZE);
    return true;
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

static bool aes_ecb_encrypt(const uint8_t* key, size_t key_len, const uint8_t* in, size_t len, uint8_t* out,
                            size_t out_size) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, key_len * 8);

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    if (psa_import_key(&attributes, key, key_len, &key_id) != PSA_SUCCESS) return false;

    size_t       out_len = 0;
    psa_status_t status  = psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING, in, len, out, out_size, &out_len);
    psa_destroy_key(key_id);

    // ECB carries no IV, so the output must be exactly as long as the input.
    // Anything else means the mode is not doing what this code assumes.
    return status == PSA_SUCCESS && out_len == len;
}

// AES-128-ECB decrypt in place-ish: `in` -> `out`, length must be a block multiple.
static bool aes_ecb_decrypt(const uint8_t* key, size_t key_len, const uint8_t* in, size_t len, uint8_t* out,
                            size_t out_size) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, key_len * 8);

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    if (psa_import_key(&attributes, key, key_len, &key_id) != PSA_SUCCESS) return false;

    size_t       out_len = 0;
    psa_status_t status  = psa_cipher_decrypt(key_id, PSA_ALG_ECB_NO_PADDING, in, len, out, out_size, &out_len);
    psa_destroy_key(key_id);

    return status == PSA_SUCCESS && out_len == len;
}

size_t mc_grp_frame_plaintext(uint32_t timestamp, const char* text, uint8_t* out, size_t out_max) {
    if (text == NULL || out == NULL) return 0;

    size_t text_len  = strlen(text);
    size_t plain_len = 4 + 1 + text_len;
    // Round up to a whole cipher block; the tail is left zeroed, which the
    // receiver's NUL-terminated read of the text treats as the end of it.
    size_t padded = ((plain_len + MC_CIPHER_BLOCK - 1) / MC_CIPHER_BLOCK) * MC_CIPHER_BLOCK;
    if (padded > out_max || padded > MC_MAX_PAYLOAD_SIZE) return 0;

    memset(out, 0, padded);
    memcpy(out, &timestamp, 4);
    out[4] = 0;  // text_type: a normal message
    memcpy(&out[5], text, text_len);
    return padded;
}

// MeshCore accepts 16- and 32-byte channel keys, so both the cipher width and
// the HMAC key length follow the key rather than being fixed at 128 bits.
static bool valid_key_len(size_t key_len) {
    return key_len == 16 || key_len == 32;
}

bool mc_grp_encrypt(const uint8_t* key, size_t key_len, const uint8_t* plain, size_t padded_len, uint8_t* out_cipher,
                    uint8_t out_mac[32]) {
    if (key == NULL || plain == NULL || out_cipher == NULL || out_mac == NULL) return false;
    if (!valid_key_len(key_len)) return false;
    if (padded_len == 0 || (padded_len % MC_CIPHER_BLOCK) != 0) return false;

    if (!aes_ecb_encrypt(key, key_len, plain, padded_len, out_cipher, padded_len)) return false;
    // The MAC covers the ciphertext, not the plaintext: the receiver checks it
    // before attempting to decrypt, which is what makes a wrong channel key a
    // cheap rejection rather than a garbage message.
    return hmac_sha256(key, key_len, out_cipher, padded_len, out_mac);
}

bool mc_grp_decrypt(const mc_grp_txt_t* grp, const uint8_t* key, size_t key_len, mc_grp_msg_t* out) {
    if (grp == NULL || key == NULL || out == NULL) return false;
    if (!valid_key_len(key_len)) return false;
    memset(out, 0, sizeof(*out));

    // ECB decrypts whole blocks only; a truncated tail means a malformed frame.
    if (grp->cipher_length == 0 || (grp->cipher_length % MC_CIPHER_BLOCK) != 0) return false;

    uint8_t mac[32];
    if (!hmac_sha256(key, key_len, grp->cipher, grp->cipher_length, mac)) return false;
    if (memcmp(mac, grp->mac, MC_CIPHER_MAC_SIZE) != 0) return false;

    uint8_t plain[MC_MAX_PAYLOAD_SIZE];
    if (!aes_ecb_decrypt(key, key_len, grp->cipher, grp->cipher_length, plain, sizeof(plain))) return false;

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

// --- direct messages -----------------------------------------------------

// The cipher takes 16 bytes of the shared secret and the MAC all 32. That
// asymmetry is upstream's, and getting it wrong produces frames that decrypt
// perfectly here and are dropped by every peer.
#define DM_CIPHER_KEY_LEN MC_CIPHER_KEY_SIZE

bool mc_shared_secret(uint8_t out[MC_SHARED_SECRET_LEN], const uint8_t our_private_key[64],
                      const uint8_t their_public_key[MC_PUB_KEY_SIZE]) {
    if (out == NULL || our_private_key == NULL || their_public_key == NULL) return false;

    // MeshCore identifies nodes by their Ed25519 signing key, so the peer's key
    // has to be moved onto the Montgomery curve before it can be agreed with.
    uint8_t peer_u[32];
    if (!ed25519_pub_to_x25519(peer_u, their_public_key)) return false;

    uint8_t scalar[32];
    ed25519_priv_to_x25519(scalar, our_private_key);

    bool ok = x25519_agree(out, scalar, peer_u);
    memset(scalar, 0, sizeof(scalar));
    if (!ok) memset(out, 0, MC_SHARED_SECRET_LEN);
    return ok;
}

size_t mc_dm_frame_plaintext(uint32_t timestamp, uint8_t attempt, const char* text, uint8_t* out, size_t out_max,
                             uint8_t* out_unpadded_len) {
    if (text == NULL || out == NULL) return 0;

    size_t text_len = strlen(text);
    // The NUL is carried: the receiver reads the text as a C string, and without
    // it the zero padding would be the only thing ending it -- which fails
    // exactly when the text happens to fill the last block.
    size_t plain_len = 4 + 1 + text_len + 1;
    size_t padded    = ((plain_len + MC_CIPHER_BLOCK - 1) / MC_CIPHER_BLOCK) * MC_CIPHER_BLOCK;
    if (padded > out_max || padded > MC_MAX_PAYLOAD_SIZE) return 0;

    memset(out, 0, padded);
    memcpy(out, &timestamp, 4);
    // Upstream reads the message type as flags >> 2, so a plain message is a
    // zero type with the attempt counter in the low two bits.
    out[4] = (uint8_t)(attempt & 0x03);
    memcpy(&out[5], text, text_len);

    // The acknowledgement covers the plaintext up to but not including the NUL,
    // so that is what the caller has to hash -- not the padded length.
    if (out_unpadded_len) *out_unpadded_len = (uint8_t)(5 + text_len);
    return padded;
}

bool mc_dm_encrypt(const uint8_t secret[MC_SHARED_SECRET_LEN], const uint8_t* plain, size_t padded_len,
                   uint8_t* out_cipher, uint8_t out_mac[32]) {
    if (secret == NULL || plain == NULL || out_cipher == NULL || out_mac == NULL) return false;
    if (padded_len == 0 || (padded_len % MC_CIPHER_BLOCK) != 0) return false;

    if (!aes_ecb_encrypt(secret, DM_CIPHER_KEY_LEN, plain, padded_len, out_cipher, padded_len)) return false;
    return hmac_sha256(secret, MC_SHARED_SECRET_LEN, out_cipher, padded_len, out_mac);
}

bool mc_dm_decrypt(const uint8_t secret[MC_SHARED_SECRET_LEN], const uint8_t mac[MC_CIPHER_MAC_SIZE],
                   const uint8_t* cipher, size_t cipher_len, mc_dm_msg_t* out) {
    if (secret == NULL || mac == NULL || cipher == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (cipher_len == 0 || (cipher_len % MC_CIPHER_BLOCK) != 0) return false;
    if (cipher_len > MC_MAX_PAYLOAD_SIZE) return false;

    uint8_t computed[32];
    if (!hmac_sha256(secret, MC_SHARED_SECRET_LEN, cipher, cipher_len, computed)) return false;
    if (memcmp(computed, mac, MC_CIPHER_MAC_SIZE) != 0) return false;

    uint8_t plain[MC_MAX_PAYLOAD_SIZE];
    if (!aes_ecb_decrypt(secret, DM_CIPHER_KEY_LEN, cipher, cipher_len, plain, sizeof(plain))) return false;
    if (cipher_len < 6) return false;  // timestamp, flags and at least one text byte

    memcpy(&out->timestamp, plain, 4);
    out->text_type = (uint8_t)(plain[4] >> 2);
    out->attempt   = (uint8_t)(plain[4] & 0x03);

    // The text is NUL-terminated inside the padding. Bound the copy by the block
    // length so a frame whose padding was tampered with cannot run off the end.
    size_t max_text = cipher_len - 5;
    if (max_text >= sizeof(out->text)) max_text = sizeof(out->text) - 1;
    memcpy(out->text, &plain[5], max_text);
    out->text[max_text] = '\0';

    out->signed_len = (uint8_t)(5 + strlen(out->text));
    return true;
}

bool mc_dm_ack_hash(uint8_t out[4], const uint8_t* plain, size_t unpadded_len,
                    const uint8_t sender_public_key[MC_PUB_KEY_SIZE]) {
    if (out == NULL || plain == NULL || sender_public_key == NULL) return false;

    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    uint8_t              digest[32];
    size_t               digest_len = 0;

    bool ok = psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS &&
              psa_hash_update(&operation, plain, unpadded_len) == PSA_SUCCESS &&
              psa_hash_update(&operation, sender_public_key, MC_PUB_KEY_SIZE) == PSA_SUCCESS &&
              psa_hash_finish(&operation, digest, sizeof(digest), &digest_len) == PSA_SUCCESS && digest_len == 32;

    if (!ok) {
        psa_hash_abort(&operation);
        return false;
    }
    memcpy(out, digest, 4);
    return true;
}
