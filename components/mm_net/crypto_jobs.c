// SPDX-License-Identifier: MIT

#include "crypto_jobs.h"
#include <string.h>
#include "ed25519.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "meshcore_crypto.h"
#include "meshcore_wire.h"
#include "meshtastic_crypto.h"

static const char TAG[] = "crypto_jobs";

// Deep enough to absorb a burst of adverts, shallow enough that a backlog is
// dropped rather than queued for minutes behind work nobody is waiting for.
#define QUEUE_DEPTH 6

typedef struct {
    crypto_job_kind_t kind;

    uint8_t  pub_key[NODE_KEY_LEN];
    uint32_t node_num;

    // MC verify
    uint8_t signature[MC_SIGNATURE_SIZE];
    uint8_t signed_bytes[MC_MAX_PAYLOAD_SIZE];
    uint8_t signed_len;

    // Secret derivation. A copy, so the worker never reads the live identity.
    uint8_t our_private[64];
} crypto_job_t;

static QueueHandle_t jobs;
static QueueHandle_t results;

bool crypto_jobs_init(void) {
    if (jobs == NULL) jobs = xQueueCreate(QUEUE_DEPTH, sizeof(crypto_job_t));
    if (results == NULL) results = xQueueCreate(QUEUE_DEPTH, sizeof(crypto_result_t));
    if (jobs == NULL || results == NULL) {
        ESP_LOGE(TAG, "queues could not be allocated");
        return false;
    }
    return true;
}

static bool submit(const crypto_job_t* job) {
    if (jobs == NULL) return false;
    if (xQueueSend(jobs, job, 0) != pdTRUE) {
        ESP_LOGD(TAG, "backlog; job %d dropped", (int)job->kind);
        return false;
    }
    return true;
}

bool crypto_queue_mc_verify(const uint8_t pub_key[NODE_KEY_LEN], const uint8_t signature[64],
                            const uint8_t* signed_bytes, uint8_t signed_len) {
    if (pub_key == NULL || signature == NULL || signed_bytes == NULL || signed_len == 0) return false;
    if (signed_len > sizeof(((crypto_job_t*)0)->signed_bytes)) return false;

    crypto_job_t job = {.kind = CRYPTO_JOB_MC_VERIFY, .signed_len = signed_len};
    memcpy(job.pub_key, pub_key, NODE_KEY_LEN);
    memcpy(job.signature, signature, MC_SIGNATURE_SIZE);
    memcpy(job.signed_bytes, signed_bytes, signed_len);
    return submit(&job);
}

bool crypto_queue_mc_secret(const uint8_t pub_key[NODE_KEY_LEN], const uint8_t our_private_key[64]) {
    if (pub_key == NULL || our_private_key == NULL) return false;

    crypto_job_t job = {.kind = CRYPTO_JOB_MC_SECRET};
    memcpy(job.pub_key, pub_key, NODE_KEY_LEN);
    memcpy(job.our_private, our_private_key, 64);
    return submit(&job);
}

bool crypto_queue_mt_secret(uint32_t node_num, const uint8_t their_public_key[NODE_KEY_LEN],
                            const uint8_t our_private_key[32]) {
    if (their_public_key == NULL || our_private_key == NULL) return false;

    crypto_job_t job = {.kind = CRYPTO_JOB_MT_SECRET, .node_num = node_num};
    memcpy(job.pub_key, their_public_key, NODE_KEY_LEN);
    memcpy(job.our_private, our_private_key, 32);
    return submit(&job);
}

bool crypto_run_one(uint32_t wait_ms) {
    if (jobs == NULL || results == NULL) return false;

    crypto_job_t job;
    if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return false;

    crypto_result_t result = {.kind = job.kind, .node_num = job.node_num};
    memcpy(result.pub_key, job.pub_key, NODE_KEY_LEN);

    switch (job.kind) {
        case CRYPTO_JOB_MC_VERIFY:
            result.ok = ed25519_verify(job.signature, job.signed_bytes, job.signed_len, job.pub_key);
            if (!result.ok) {
                ESP_LOGW(TAG, "advert signature failed for %02x%02x%02x%02x", job.pub_key[0], job.pub_key[1],
                         job.pub_key[2], job.pub_key[3]);
            }
            break;

        case CRYPTO_JOB_MC_SECRET: result.ok = mc_shared_secret(result.secret, job.our_private, job.pub_key); break;

        case CRYPTO_JOB_MT_SECRET: result.ok = mt_pki_shared_key(result.secret, job.our_private, job.pub_key); break;

        default: result.ok = false; break;
    }

    // The job held a copy of a private key; do not leave it on the stack.
    memset(job.our_private, 0, sizeof(job.our_private));

    xQueueSend(results, &result, portMAX_DELAY);
    return true;
}

bool crypto_take_result(crypto_result_t* out) {
    if (results == NULL || out == NULL) return false;
    return xQueueReceive(results, out, 0) == pdTRUE;
}
