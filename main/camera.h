#pragma once

#include "esp_camera.h"
#include "esp_err.h"

#include <stdbool.h>

esp_err_t camera_start(void);

// True once esp_camera_init() has succeeded.
bool camera_is_ready(void);

// Sensor model reported by the driver, e.g. "OV5640"; "none" before init succeeds.
const char *camera_sensor_name(void);

// Grab the most recent JPEG frame. Release it with camera_release().
// Returns NULL if the sensor did not deliver a frame.
camera_fb_t *camera_capture(void);
void camera_release(camera_fb_t *fb);

// Grab one full-resolution (QSXGA) frame for analysis, switching the sensor up and
// discarding stale frames first. Holds the camera until camera_release_full_res(), which
// restores the live-view resolution.
camera_fb_t *camera_capture_full_res(void);
void camera_release_full_res(camera_fb_t *fb);

// Change the capture resolution at runtime (FRAMESIZE_SVGA up to FRAMESIZE_QSXGA).
esp_err_t camera_set_framesize(framesize_t size);
