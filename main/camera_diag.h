#pragma once

// Scan the camera's SCCB bus and log what answers. Call only after esp_camera_init() fails.
void camera_diagnose(void);
