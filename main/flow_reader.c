#include "flow_reader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "flows.h"
#include "history.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "rom/tjpgd.h"

static const char *TAG = "flowread";

#define READ_INTERVAL_MS  60000
#define MIN_RING_STRENGTH 10.0f   // mean redness of the ring row; prototype frames were 29-48

// All tube strips live in this band of the full-resolution frame. Decoding stops below it.
#define BAND_Y0 80
#define BAND_Y1 420
#define BAND_ROWS (BAND_Y1 - BAND_Y0)

// The cap is found first, anywhere in CAP_SEARCH; the ring is then searched for in a window
// hanging above it (cap - RING_ABOVE_FAR .. cap - RING_ABOVE_NEAR), so a camera that shifts by
// a few tens of pixels still reads correctly. Fixed windows broke on 2026-11-06 when the
// camera moved ~15 px and the cap's top edge fell inside the ring search.
#define CAP_SEARCH_Y0   250
#define CAP_SEARCH_Y1   420
#define RING_ABOVE_FAR  170
#define RING_ABOVE_NEAR 8
#define RING_WINDOW     (RING_ABOVE_FAR - RING_ABOVE_NEAR)

typedef struct {
    uint16_t x0, x1;              // strip around the rod, full-resolution pixels
    float zero_px;                // ring height above the cap at 0 L/min
    float px_per_lpm;             // ring travel per L/min (the ring moves DOWN with flow)
} tube_cal_t;

// Defaults measured by tools/flow_prototype.py on the 2026-11-06 frames: valve shut, and
// all three loops at ~1.1 L/min read by eye.
static tube_cal_t s_cal[FLOW_LOOP_COUNT] = {
    {1040, 1090, 122.1f, 21.2f},
    {1307, 1357, 129.8f, 27.0f},
    {1571, 1621, 128.4f, 25.3f},
};

typedef struct {
    bool valid;
    float height_px;
    float strength;
} tube_raw_t;

static tube_raw_t s_last[FLOW_LOOP_COUNT];
static SemaphoreHandle_t s_cal_lock;
static TaskHandle_t s_task;

// Per tube, per band row: sum of max(0, R - max(G, B)) across the strip.
static float s_rowsum[FLOW_LOOP_COUNT][BAND_ROWS];

/* ---------- calibration storage ---------- */

static void cal_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open("flowcal", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    tube_cal_t stored[FLOW_LOOP_COUNT];
    size_t len = sizeof(stored);
    if (nvs_get_blob(nvs, "cal", stored, &len) == ESP_OK && len == sizeof(stored)) {
        memcpy(s_cal, stored, sizeof(s_cal));
        ESP_LOGI(TAG, "Calibration loaded from NVS");
    }
    nvs_close(nvs);
}

static esp_err_t cal_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("flowcal", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, "cal", s_cal, sizeof(s_cal));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* ---------- JPEG band decode ---------- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} jpeg_src_t;

static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT n)
{
    jpeg_src_t *src = (jpeg_src_t *)jd->device;
    size_t left = src->len - src->pos;
    if (n > left) {
        n = left;
    }
    if (buf) {
        memcpy(buf, src->data + src->pos, n);
    }
    src->pos += n;
    return n;
}

// Called per MCU block, left to right then top to bottom. Returning 0 stops the decode,
// which we do once the blocks are below the band.
static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    if (rect->top >= BAND_Y1) {
        return 0;
    }
    if (rect->bottom < BAND_Y0) {
        return 1;
    }
    const uint8_t *px = (const uint8_t *)bitmap;
    int w = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; y++) {
        if (y < BAND_Y0 || y >= BAND_Y1) {
            px += w * 3;
            continue;
        }
        for (int x = rect->left; x <= rect->right; x++, px += 3) {
            for (int t = 0; t < FLOW_LOOP_COUNT; t++) {
                if (x >= s_cal[t].x0 && x < s_cal[t].x1) {
                    int red = (int)px[0] - (px[1] > px[2] ? px[1] : px[2]);
                    if (red > 0) {
                        s_rowsum[t][y - BAND_Y0] += red;
                    }
                }
            }
        }
    }
    return 1;
}

static esp_err_t decode_band(const camera_fb_t *fb)
{
    memset(s_rowsum, 0, sizeof(s_rowsum));

    void *pool = malloc(4096);
    if (!pool) {
        return ESP_ERR_NO_MEM;
    }
    jpeg_src_t src = {.data = fb->buf, .len = fb->len, .pos = 0};
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, jpeg_in, pool, 4096, &src);
    if (rc == JDR_OK) {
        if (jd.width != 2560 || jd.height != 1920) {
            ESP_LOGW(TAG, "Unexpected frame size %ux%u", jd.width, jd.height);
            rc = JDR_PAR;
        } else {
            rc = jd_decomp(&jd, jpeg_out, 0);
            if (rc == JDR_INTR) {
                rc = JDR_OK; // we stopped it on purpose below the band
            }
        }
    }
    free(pool);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "JPEG decode failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------- ring and cap location (mirrors tools/flow_prototype.py) ---------- */

static float row_mean(int t, int y)
{
    return s_rowsum[t][y - BAND_Y0] / (float)(s_cal[t].x1 - s_cal[t].x0);
}

static void ring_row(int t, int y0, float *row, float *strength)
{
    float profile[RING_WINDOW];
    const int n = RING_WINDOW;
    for (int i = 0; i < n; i++) {
        profile[i] = row_mean(t, y0 + i);
    }
    // 5-row box smoothing, then the peak.
    int peak = 0;
    float best = -1;
    for (int i = 0; i < n; i++) {
        // Matches np.convolve(mode="same") with a 5-wide box: missing edge rows count as 0.
        float s = 0;
        for (int k = i - 2; k <= i + 2; k++) {
            if (k >= 0 && k < n) {
                s += profile[k];
            }
        }
        s /= 5.0f;
        if (s > best) {
            best = s;
            peak = i;
        }
    }
    // Sub-pixel centroid over peak-4 .. peak+4.
    int lo = peak - 4 < 0 ? 0 : peak - 4;
    int hi = peak + 5 > n ? n : peak + 5;
    float wsum = 0, msum = 0;
    for (int i = lo; i < hi; i++) {
        wsum += profile[i];
        msum += profile[i] * (i - lo);
    }
    *row = y0 + lo + (wsum > 1e-6f ? msum / wsum : 0);
    *strength = best;
}

static int cap_top(int t)
{
    float maxv = 0;
    for (int y = CAP_SEARCH_Y0; y < CAP_SEARCH_Y1; y++) {
        float v = row_mean(t, y);
        if (v > maxv) {
            maxv = v;
        }
    }
    float level = 0.5f * maxv;
    for (int y = CAP_SEARCH_Y0; y < CAP_SEARCH_Y1; y++) {
        if (row_mean(t, y) > level) {
            return y;
        }
    }
    return CAP_SEARCH_Y0;
}

static void read_once(void)
{
    int64_t t0 = esp_timer_get_time();
    camera_fb_t *fb = camera_capture_full_res();
    if (!fb) {
        ESP_LOGW(TAG, "No full-resolution frame");
        return;
    }
    int64_t t1 = esp_timer_get_time();
    esp_err_t err = decode_band(fb);
    camera_release_full_res(fb);
    int64_t t2 = esp_timer_get_time();
    if (err != ESP_OK) {
        return;
    }

    float flows_now[FLOW_LOOP_COUNT];
    bool valid_now[FLOW_LOOP_COUNT];
    xSemaphoreTake(s_cal_lock, portMAX_DELAY);
    for (int t = 0; t < FLOW_LOOP_COUNT; t++) {
        float row, strength;
        int cap = cap_top(t);
        ring_row(t, cap - RING_ABOVE_FAR, &row, &strength);
        float height = (float)cap - row;
        bool ok = strength >= MIN_RING_STRENGTH;
        s_last[t] = (tube_raw_t){.valid = ok, .height_px = height, .strength = strength};

        float flow = (s_cal[t].zero_px - height) / s_cal[t].px_per_lpm;
        if (flow < 0) {
            flow = 0; // the ring can sit a pixel or two above its zero
        }
        flows_set_measurement(t, ok, flow, height, strength);
        flows_now[t] = flow;
        valid_now[t] = ok;
        ESP_LOGI(TAG, "Loop %d: height %.1f px, strength %.1f -> %.2f L/min%s", t + 1, height,
                 strength, flow, ok ? "" : " (ring not found)");
    }
    xSemaphoreGive(s_cal_lock);
    history_add(flows_now, valid_now);
    ESP_LOGI(TAG, "Read in %lld ms (capture %lld, decode %lld)", (t2 - t0) / 1000, (t1 - t0) / 1000,
             (t2 - t1) / 1000);
}

static void reader_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000)); // let the sensor's auto-exposure settle after boot
    while (true) {
        read_once();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

esp_err_t flow_reader_start(void)
{
    if (!camera_is_ready()) {
        ESP_LOGW(TAG, "Camera not ready; flow reader not started");
        return ESP_ERR_INVALID_STATE;
    }
    s_cal_lock = xSemaphoreCreateMutex();
    cal_load();
    // The JPEG decoder (ROM TJpgDec) needs a few KB of stack on top of our own frames.
    if (xTaskCreate(reader_task, "flow_reader", 8192, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void flow_reader_trigger(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

esp_err_t flow_reader_calibrate_zero(int loop)
{
    if (!s_cal_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_cal_lock, portMAX_DELAY);
    for (int t = 0; t < FLOW_LOOP_COUNT; t++) {
        if (loop >= 0 && t != loop) {
            continue;
        }
        if (!s_last[t].valid) {
            err = ESP_ERR_INVALID_STATE;
            continue;
        }
        s_cal[t].zero_px = s_last[t].height_px;
        ESP_LOGI(TAG, "Loop %d zero set to %.1f px", t + 1, s_cal[t].zero_px);
    }
    esp_err_t serr = cal_save();
    xSemaphoreGive(s_cal_lock);
    return err != ESP_OK ? err : serr;
}

esp_err_t flow_reader_calibrate_flow(int loop, float lpm)
{
    if (!s_cal_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!(lpm > 0.2f && lpm <= 10.0f)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_cal_lock, portMAX_DELAY);
    for (int t = 0; t < FLOW_LOOP_COUNT; t++) {
        if (loop >= 0 && t != loop) {
            continue;
        }
        float travel = s_cal[t].zero_px - s_last[t].height_px;
        if (!s_last[t].valid || travel < 5.0f) {
            err = ESP_ERR_INVALID_STATE; // ring has not moved far enough from zero to scale
            continue;
        }
        s_cal[t].px_per_lpm = travel / lpm;
        ESP_LOGI(TAG, "Loop %d scale set to %.1f px per L/min", t + 1, s_cal[t].px_per_lpm);
    }
    esp_err_t serr = cal_save();
    xSemaphoreGive(s_cal_lock);
    return err != ESP_OK ? err : serr;
}
