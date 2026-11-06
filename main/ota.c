#include "ota.h"

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "ota";

#define OTA_BUF_SIZE 4096
#define OTA_MAX_TIMEOUTS 5

// The app description sits right after the image header and the first segment header.
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HEADER_BYTES    (APP_DESC_OFFSET + sizeof(esp_app_desc_t))

static bool s_in_progress;

static void restart_cb(void *arg)
{
    esp_restart();
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGE(TAG, "Update rejected: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

// Read exactly len bytes (or the rest of the body if shorter). Returns bytes read, -1 on error.
static int recv_full(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= OTA_MAX_TIMEOUTS) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return (int)got;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (s_in_progress) {
        return send_error(req, "409 Conflict", "An update is already in progress");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return send_error(req, "500 Internal Server Error", "No OTA partition available");
    }
    if (req->content_len < HEADER_BYTES) {
        return send_error(req, "400 Bad Request", "Upload is too small to be firmware");
    }
    if (req->content_len > target->size) {
        return send_error(req, "400 Bad Request", "Firmware is larger than the OTA partition");
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (buf == NULL) {
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }

    s_in_progress = true;
    esp_ota_handle_t handle = 0;
    esp_err_t result = ESP_FAIL;
    size_t remaining = req->content_len;

    // First chunk: check it is an ESP image *for this project* before touching flash.
    int n = recv_full(req, buf, MIN(OTA_BUF_SIZE, remaining));
    if (n < (int)HEADER_BYTES) {
        send_error(req, "400 Bad Request", "Upload interrupted");
        goto out;
    }
    const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + APP_DESC_OFFSET);
    const esp_app_desc_t *running = esp_app_get_description();
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        send_error(req, "400 Bad Request", "Not an ESP32 firmware image (use build/ufh-manifold-monitor.bin)");
        goto out;
    }
    if (hdr->chip_id != ESP_CHIP_ID_ESP32S3) {
        send_error(req, "400 Bad Request", "Firmware was built for a different chip");
        goto out;
    }
    if (strncmp(desc->project_name, running->project_name, sizeof(desc->project_name)) != 0) {
        send_error(req, "400 Bad Request", "Firmware is for a different project");
        goto out;
    }

    ESP_LOGI(TAG, "Updating %s -> %s (%u bytes) into %s", running->version, desc->version,
             (unsigned)req->content_len, target->label);

    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        send_error(req, "500 Internal Server Error", esp_err_to_name(err));
        goto out;
    }

    while (true) {
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            send_error(req, "500 Internal Server Error", esp_err_to_name(err));
            esp_ota_abort(handle);
            goto out;
        }
        remaining -= (size_t)n;
        if (remaining == 0) {
            break;
        }
        n = recv_full(req, buf, MIN(OTA_BUF_SIZE, remaining));
        if (n <= 0) {
            ESP_LOGE(TAG, "Upload interrupted with %u bytes to go", (unsigned)remaining);
            esp_ota_abort(handle);
            goto out; // the socket is gone; no response to send
        }
    }

    err = esp_ota_end(handle); // verifies the image checksum (and signature, if enabled)
    if (err != ESP_OK) {
        send_error(req, "400 Bad Request",
                   err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image failed verification" : esp_err_to_name(err));
        goto out;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        send_error(req, "500 Internal Server Error", esp_err_to_name(err));
        goto out;
    }

    ESP_LOGI(TAG, "Update written; rebooting into %s", target->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"rebooting\":true}");

    // Reboot shortly, after the response has gone out.
    const esp_timer_create_args_t args = {.callback = restart_cb, .name = "ota_restart"};
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 1000 * 1000);
    }
    result = ESP_OK;

out:
    free(buf);
    if (result != ESP_OK) {
        s_in_progress = false;
    }
    return ESP_OK; // a response (or a dead socket) has been handled either way
}

esp_err_t ota_register(httpd_handle_t server)
{
    const httpd_uri_t uri = {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler};
    return httpd_register_uri_handler(server, &uri);
}

void ota_mark_running_app_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "New firmware in %s confirmed; rollback cancelled", running->label);
        }
    }
}
