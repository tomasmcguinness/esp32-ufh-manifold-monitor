#include "flows.h"

#include <stdio.h>
#include <string.h>

#include "camera.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ethernet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "flows";
static const char *NVS_NS = "flows";

// The installed meters (GD brand) are printed 0-5 L/min, with 0 at the top: the red
// float moves down as flow increases.
#define DEFAULT_SCALE_MAX_LPM 5.0f

static flow_loop_t s_loops[FLOW_LOOP_COUNT];
static SemaphoreHandle_t s_lock;

esp_err_t flows_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    nvs_handle_t nvs;
    bool have_nvs = nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK;

    for (int i = 0; i < FLOW_LOOP_COUNT; i++) {
        flow_loop_t *loop = &s_loops[i];
        memset(loop, 0, sizeof(*loop));
        loop->scale_max_lpm = DEFAULT_SCALE_MAX_LPM;

        char key[8];
        snprintf(key, sizeof(key), "name%d", i);
        size_t len = sizeof(loop->name);
        if (!have_nvs || nvs_get_str(nvs, key, loop->name, &len) != ESP_OK) {
            snprintf(loop->name, sizeof(loop->name), "Loop %d", i + 1);
        }
    }

    if (have_nvs) {
        nvs_close(nvs);
    }
    return ESP_OK;
}

void flows_get(flow_loop_t out[FLOW_LOOP_COUNT])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_loops, sizeof(s_loops));
    xSemaphoreGive(s_lock);
}

esp_err_t flows_set_name(int index, const char *name)
{
    if (index < 0 || index >= FLOW_LOOP_COUNT || name == NULL || name[0] == '\0' ||
        strlen(name) >= FLOW_LOOP_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    snprintf(key, sizeof(key), "name%d", index);
    err = nvs_set_str(nvs, key, name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_loops[index].name, name, sizeof(s_loops[index].name));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Loop %d renamed to '%s'", index + 1, name);
    return ESP_OK;
}

void flows_set_reading(int index, float flow_lpm)
{
    if (index < 0 || index >= FLOW_LOOP_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_loops[index].valid = true;
    s_loops[index].flow_lpm = flow_lpm;
    s_loops[index].updated_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

// Append a JSON string literal, escaping the characters a loop name could contain.
static int json_str(char *buf, size_t len, const char *s)
{
    size_t n = 0;
    if (n < len) buf[n] = '"';
    n++;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            if (n + 1 < len) { buf[n] = '\\'; buf[n + 1] = c; }
            n += 2;
        } else if (c < 0x20) {
            if (n + 6 < len) snprintf(buf + n, len - n, "\\u%04x", c);
            n += 6;
        } else {
            if (n < len) buf[n] = c;
            n++;
        }
    }
    if (n < len) buf[n] = '"';
    n++;
    return (int)n;
}

int flows_build_json(char *buf, size_t len)
{
    flow_loop_t loops[FLOW_LOOP_COUNT];
    flows_get(loops);
    int64_t now = esp_timer_get_time();

    size_t n = 0;
#define APPEND(...)                                                                 \
    do {                                                                            \
        int w = snprintf(buf + n, n < len ? len - n : 0, __VA_ARGS__);              \
        if (w < 0) return -1;                                                       \
        n += (size_t)w;                                                             \
    } while (0)

    bool any_valid = false;
    float total = 0;

    APPEND("{\"loops\":[");
    for (int i = 0; i < FLOW_LOOP_COUNT; i++) {
        flow_loop_t *l = &loops[i];
        APPEND("%s{\"id\":%d,\"name\":", i ? "," : "", i + 1);
        n += (size_t)json_str(buf + n, n < len ? len - n : 0, l->name);
        APPEND(",\"scaleMaxLpm\":%.1f", l->scale_max_lpm);
        if (l->valid) {
            any_valid = true;
            total += l->flow_lpm;
            APPEND(",\"flowLpm\":%.2f,\"ageSeconds\":%lld}", l->flow_lpm, (now - l->updated_us) / 1000000);
        } else {
            APPEND(",\"flowLpm\":null,\"ageSeconds\":null}");
        }
    }
    APPEND("],");

    if (any_valid) {
        APPEND("\"totalLpm\":%.2f,\"measurement\":\"live\",", total);
    } else {
        APPEND("\"totalLpm\":null,\"measurement\":\"not_calibrated\",");
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *part = esp_ota_get_running_partition();
    // The ELF hash changes on every build; app->date/time only change when esp_app_desc.c recompiles.
    char sha[17];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    APPEND("\"firmware\":{\"version\":\"%s\",\"build\":\"%s\",\"built\":\"%s %s\",\"idf\":\"%s\",\"partition\":\"%s\"},",
           app->version, sha, app->date, app->time, app->idf_ver, part ? part->label : "?");

    APPEND("\"device\":{\"uptimeSeconds\":%lld,\"ip\":\"%s\",\"hostname\":\"ufh-monitor.local\","
           "\"cameraReady\":%s,\"cameraSensor\":\"%s\",\"freeHeap\":%u,\"freePsram\":%u}}",
           now / 1000000, ethernet_ip_string(), camera_is_ready() ? "true" : "false", camera_sensor_name(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#undef APPEND

    if (n >= len) {
        return -1;
    }
    return (int)n;
}
