// SPDX-License-Identifier: MIT

#include "session_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char TAG[] = "session_log";

// Large enough to absorb a burst of traffic while the writer is blocked on a
// FAT write, small enough to be nothing next to 32 MB of PSRAM.
#define RING_BYTES 65536

// Stop before filling a partition that also holds the launcher's data. A log
// that bricks the device it was diagnosing would not be a good trade.
#define MAX_FILE_BYTES (768 * 1024)

#define LOG_LINE_MAX 640

static char*             ring;
static size_t            head;  // next write
static size_t            tail;  // next read
static SemaphoreHandle_t lock;
static volatile bool     active;
static volatile uint32_t dropped;
static volatile uint32_t written;
static bool              full_notice;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static size_t ring_used(void) {
    return (head + RING_BYTES - tail) % RING_BYTES;
}

// Append, or drop the whole line. A half-written line would corrupt the one
// after it, and a log with an obvious gap is worth more than one with a subtle
// splice.
static void ring_put(const char* text, size_t len) {
    if (ring == NULL || len == 0) return;

    xSemaphoreTake(lock, portMAX_DELAY);
    if (RING_BYTES - 1 - ring_used() < len) {
        dropped++;
    } else {
        for (size_t i = 0; i < len; i++) {
            ring[head] = text[i];
            head       = (head + 1) % RING_BYTES;
        }
    }
    xSemaphoreGive(lock);
}

static void writer_task(void* arg) {
    (void)arg;
    FILE*  file = NULL;
    size_t open_for_session = 0;

    while (1) {
        if (!active) {
            if (file) {
                fclose(file);
                file = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (file == NULL) {
            // "w" truncates: a session starts clean.
            file = fopen(SESSION_LOG_PATH, open_for_session == 0 ? "w" : "a");
            if (file == NULL) {
                ESP_LOGE(TAG, "cannot open %s", SESSION_LOG_PATH);
                active = false;
                continue;
            }
        }

        char   chunk[LOG_LINE_MAX];
        size_t take = 0;

        xSemaphoreTake(lock, portMAX_DELAY);
        while (take < sizeof(chunk) && tail != head) {
            chunk[take++] = ring[tail];
            tail          = (tail + 1) % RING_BYTES;
        }
        xSemaphoreGive(lock);

        if (take == 0) {
            // Nothing waiting: flush what is there so a session that is stopped
            // abruptly still has everything up to the last quiet moment.
            if (file) fflush(file);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (fwrite(chunk, 1, take, file) != take) {
            ESP_LOGE(TAG, "write failed; logging stopped");
            active = false;
            continue;
        }
        written          += take;
        open_for_session += take;

        if (written >= MAX_FILE_BYTES) {
            if (!full_notice) {
                const char* note = "# log full, stopping\n";
                fwrite(note, 1, strlen(note), file);
                full_notice = true;
            }
            fflush(file);
            active = false;
        }
    }
}

bool session_log_init(void) {
    if (ring != NULL) return true;

    // PSRAM: 64 KB of internal memory is worth more than this is.
    ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
    if (ring == NULL) ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_DEFAULT);
    if (ring == NULL) {
        ESP_LOGE(TAG, "no memory for the log buffer");
        return false;
    }

    lock = xSemaphoreCreateMutex();
    if (lock == NULL) {
        heap_caps_free(ring);
        ring = NULL;
        return false;
    }

    // Below everything that touches the radio or the screen. A diagnostic must
    // not change the behaviour it is diagnosing.
    if (xTaskCreate(writer_task, "mm_log", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "writer task could not be started");
        return false;
    }
    return true;
}

bool session_log_active(void) {
    return active;
}

bool session_log_toggle(void) {
    if (ring == NULL) return false;

    if (active) {
        session_log("# session ended");
        // Let the writer drain before the file is closed.
        vTaskDelay(pdMS_TO_TICKS(400));
        active = false;
        return false;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    head = tail = 0;
    xSemaphoreGive(lock);
    dropped     = 0;
    written     = 0;
    full_notice = false;
    active      = true;

    time_t    now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);

    session_log("# multimesh session log v1");
    session_log("# started %s  uptime %lums", stamp, (unsigned long)now_ms());
    session_log("# fields are key=value; ms is milliseconds since boot");
    return true;
}

void session_log(const char* fmt, ...) {
    if (!active) return;

    char    line[LOG_LINE_MAX];
    int     n = snprintf(line, sizeof(line), "%lu ", (unsigned long)now_ms());
    va_list args;
    va_start(args, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    va_end(args);

    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    ring_put(line, (size_t)n);
}

void session_log_frame(const char* dir, const char* net, const char* extra, const uint8_t* data, size_t len) {
    if (!active || data == NULL) return;

    static const char hex[] = "0123456789abcdef";

    char line[LOG_LINE_MAX];
    int  n = snprintf(line, sizeof(line), "%lu %s net=%s len=%u %s raw=", (unsigned long)now_ms(), dir, net,
                      (unsigned)len, extra ? extra : "");
    if (n < 0 || n >= (int)sizeof(line)) return;

    // Truncate rather than drop: the header of an over-long frame is still
    // worth having, and the length field says what was cut.
    for (size_t i = 0; i < len && n + 3 < (int)sizeof(line); i++) {
        line[n++] = hex[data[i] >> 4];
        line[n++] = hex[data[i] & 0x0F];
    }
    line[n++] = '\n';
    ring_put(line, (size_t)n);
}

uint32_t session_log_bytes(void) {
    return written;
}

uint32_t session_log_dropped(void) {
    return dropped;
}
