#include "camera.h"
#include "camera_diag.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "camera";

// Waveshare ESP32-S3-ETH DVP connector (https://www.waveshare.com/wiki/ESP32-S3-ETH).
// esp32-camera names the data lines Y2..Y9, which are D0..D7.
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  3
#define CAM_PIN_SIOD  48
#define CAM_PIN_SIOC  47
#define CAM_PIN_D0    41
#define CAM_PIN_D1    45
#define CAM_PIN_D2    46
#define CAM_PIN_D3    42
#define CAM_PIN_D4    40
#define CAM_PIN_D5    38
#define CAM_PIN_D6    15
#define CAM_PIN_D7    18
#define CAM_PIN_VSYNC 1
#define CAM_PIN_HREF  2
#define CAM_PIN_PCLK  39

// Camera power enable, active low. Not on the wiki's pin table; both Waveshare demos
// (ESP32-S3-ETH-Demo.zip, cam_web.c and ETH_Web_CAM.ino) drive it low before init.
// Without it nothing answers on SCCB and init fails with "Detected camera not supported".
#define CAM_PIN_ENABLE 8

// The driver is not safe for concurrent fb_get calls (web page + future analysis task).
static SemaphoreHandle_t s_lock;
static bool s_ready;
static const char *s_sensor_name = "none";

bool camera_is_ready(void)
{
    return s_ready;
}

const char *camera_sensor_name(void)
{
    return s_sensor_name;
}

esp_err_t camera_start(void)
{
    s_lock = xSemaphoreCreateMutex();

    gpio_config_t enable = {.pin_bit_mask = 1ULL << CAM_PIN_ENABLE, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&enable));
    ESP_ERROR_CHECK(gpio_set_level(CAM_PIN_ENABLE, 0));
    vTaskDelay(pdMS_TO_TICKS(20)); // let the module's regulators settle

    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        // JPEG for the web page. The flow analysis will decode a region of
        // interest from this rather than switching the sensor to raw mode.
        .pixel_format = PIXFORMAT_JPEG,
        // The driver sizes the frame buffers from this, so start at QSXGA (2560x1920),
        // which is esp32-camera's maximum for the OV5640 (it rejects FRAMESIZE_5MP);
        // switching up later from a smaller init overflows the buffers. The live view
        // drops to SVGA straight after init.
        .frame_size = FRAMESIZE_QSXGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        camera_diagnose();
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    s_sensor_name = info ? info->name : "unknown";
    ESP_LOGI(TAG, "Sensor %s (PID 0x%04x) started", s_sensor_name, s->id.PID);

    if (s->id.PID == OV5640_PID) {
        // The OV5640 module on this board delivers a left-right mirrored image by default
        // (confirmed 2026-11-06: tape labels on the manifold read backwards).
        s->set_hmirror(s, 1);
    }
    s->set_framesize(s, FRAMESIZE_SVGA);
    s_ready = true;
    return ESP_OK;
}

camera_fb_t *camera_capture(void)
{
    if (!s_ready) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "No frame");
    }
    return fb;
}

void camera_release(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_lock);
    }
}

esp_err_t camera_set_framesize(framesize_t size)
{
    sensor_t *s = s_ready ? esp_camera_sensor_get() : NULL;
    if (!s) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = s->set_framesize(s, size);
    xSemaphoreGive(s_lock);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
