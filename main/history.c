#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "history";

#define HISTORY_CAPACITY (7 * 24 * 60)   // a week of one-a-minute readings
#define MAX_POINTS       720
#define NO_VALUE         INT16_MIN

typedef struct __attribute__((packed)) {
    uint32_t t;                          // seconds since boot
    int16_t centi_lpm[FLOW_LOOP_COUNT];  // L/min x 100, or NO_VALUE
} sample_t;

static sample_t *s_buf;
static size_t s_head;   // next slot to write
static size_t s_count;
static SemaphoreHandle_t s_lock;

static uint32_t now_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

esp_err_t history_init(void)
{
    s_buf = heap_caps_calloc(HISTORY_CAPACITY, sizeof(sample_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "No PSRAM for history");
        return ESP_ERR_NO_MEM;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "History: %d samples (%u KB)", HISTORY_CAPACITY,
             (unsigned)(HISTORY_CAPACITY * sizeof(sample_t) / 1024));
    return ESP_OK;
}

void history_add(const float flow_lpm[FLOW_LOOP_COUNT], const bool valid[FLOW_LOOP_COUNT])
{
    if (!s_buf) {
        return;
    }
    sample_t s = {.t = now_seconds()};
    for (int i = 0; i < FLOW_LOOP_COUNT; i++) {
        s.centi_lpm[i] = valid[i] ? (int16_t)(flow_lpm[i] * 100.0f + 0.5f) : NO_VALUE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_buf[s_head] = s;
    s_head = (s_head + 1) % HISTORY_CAPACITY;
    if (s_count < HISTORY_CAPACITY) {
        s_count++;
    }
    xSemaphoreGive(s_lock);
}

char *history_build_json(int hours, size_t *out_len)
{
    if (!s_buf) {
        return NULL;
    }
    if (hours < 1) hours = 1;
    if (hours > 168) hours = 168;

    const uint32_t now = now_seconds();
    const uint32_t span = (uint32_t)hours * 3600;
    uint32_t bucket = (span + MAX_POINTS - 1) / MAX_POINTS;
    if (bucket < 60) bucket = 60;
    const int nbuckets = (int)((span + bucket - 1) / bucket);

    // Accumulate per bucket (index 0 = oldest).
    int32_t *sum = calloc((size_t)nbuckets * FLOW_LOOP_COUNT, sizeof(int32_t));
    uint16_t *cnt = calloc((size_t)nbuckets * FLOW_LOOP_COUNT, sizeof(uint16_t));
    // ~32 bytes per point is plenty: "[123456,1.23,1.23,1.23],"
    size_t cap = 96 + (size_t)nbuckets * 40;
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!sum || !cnt || !out) {
        free(sum);
        free(cnt);
        free(out);
        return NULL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t k = 0; k < s_count; k++) {
        const sample_t *s = &s_buf[(s_head + HISTORY_CAPACITY - 1 - k) % HISTORY_CAPACITY];
        uint32_t age = now - s->t;
        if (age >= span) {
            break; // newest first, so everything further back is older still
        }
        int b = nbuckets - 1 - (int)(age / bucket);
        for (int i = 0; i < FLOW_LOOP_COUNT; i++) {
            if (s->centi_lpm[i] != NO_VALUE) {
                sum[b * FLOW_LOOP_COUNT + i] += s->centi_lpm[i];
                cnt[b * FLOW_LOOP_COUNT + i]++;
            }
        }
    }
    xSemaphoreGive(s_lock);

    size_t n = (size_t)snprintf(out, cap, "{\"nowSeconds\":%lu,\"bucketSeconds\":%lu,\"points\":[",
                                (unsigned long)now, (unsigned long)bucket);
    bool first = true;
    for (int b = 0; b < nbuckets; b++) {
        bool any = false;
        for (int i = 0; i < FLOW_LOOP_COUNT; i++) {
            any |= cnt[b * FLOW_LOOP_COUNT + i] > 0;
        }
        if (!any) {
            continue; // no samples at all: leave the gap out entirely
        }
        // Age of the bucket's midpoint.
        uint32_t age = (uint32_t)(nbuckets - 1 - b) * bucket + bucket / 2;
        n += (size_t)snprintf(out + n, cap - n, "%s[%lu", first ? "" : ",", (unsigned long)age);
        for (int i = 0; i < FLOW_LOOP_COUNT; i++) {
            uint16_t c = cnt[b * FLOW_LOOP_COUNT + i];
            if (c) {
                n += (size_t)snprintf(out + n, cap - n, ",%.2f",
                                      sum[b * FLOW_LOOP_COUNT + i] / (100.0f * c));
            } else {
                n += (size_t)snprintf(out + n, cap - n, ",null");
            }
        }
        n += (size_t)snprintf(out + n, cap - n, "]");
        first = false;
    }
    n += (size_t)snprintf(out + n, cap - n, "]}");
    free(sum);
    free(cnt);

    if (n >= cap) {
        free(out);
        return NULL;
    }
    *out_len = n;
    return out;
}
