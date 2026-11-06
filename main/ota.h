#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

// Register POST /api/ota: the request body is the raw firmware .bin.
esp_err_t ota_register(httpd_handle_t server);

// Call once the app has proved itself (network up, web server running). Cancels the
// bootloader's rollback for a freshly updated image.
void ota_mark_running_app_valid(void);
