#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "flow_reader.h"
#include "flows.h"
#include "ota.h"

static const char *TAG = "web";

// main/www/index.html, embedded by EMBED_TXTFILES (which appends a NUL).
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = camera_capture();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_release(fb);
    return err;
}

static esp_err_t framesize_handler(httpd_req_t *req)
{
    char query[32] = {0};
    char size[8] = {0}; // longest is "qsxga"
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "size", size, sizeof(size)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size required");
        return ESP_FAIL;
    }

    framesize_t fs;
    if (strcmp(size, "svga") == 0) fs = FRAMESIZE_SVGA;
    else if (strcmp(size, "xga") == 0) fs = FRAMESIZE_XGA;
    else if (strcmp(size, "sxga") == 0) fs = FRAMESIZE_SXGA;
    else if (strcmp(size, "uxga") == 0) fs = FRAMESIZE_UXGA;
    else if (strcmp(size, "qxga") == 0) fs = FRAMESIZE_QXGA;
    else if (strcmp(size, "qsxga") == 0) fs = FRAMESIZE_QSXGA;
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown size");
        return ESP_FAIL;
    }

    if (camera_set_framesize(fs) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_framesize failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Frame size -> %s", size);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t flows_handler(httpd_req_t *req)
{
    char json[1536];
    int n = flows_build_json(json, sizeof(json));
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON too large");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

// Decode %XX and '+' in place (loop names come URL-encoded from the page).
static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *out++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

// POST /api/loop?id=1&name=Kitchen
static esp_err_t loop_handler(httpd_req_t *req)
{
    char query[160] = {0};
    char id[4] = {0};
    char name[96] = {0}; // URL-encoded; decoded length is checked by flows_set_name
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id and name required");
        return ESP_FAIL;
    }
    url_decode(name);

    esp_err_t err = flows_set_name(atoi(id) - 1, name);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid id or name (1-23 characters)");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/read: take a reading now.
static esp_err_t read_handler(httpd_req_t *req)
{
    flow_reader_trigger();
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/calibrate?zero=1[&loop=N]   all (or loop N) are at 0 L/min now
// POST /api/calibrate?lpm=1.1[&loop=N]  all (or loop N) read 1.1 L/min now
// Uses the most recent reading; POST /api/read and wait a few seconds first.
static esp_err_t calibrate_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char val[12] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zero=1 or lpm=<value> required");
        return ESP_FAIL;
    }
    int loop = -1;
    if (httpd_query_key_value(query, "loop", val, sizeof(val)) == ESP_OK) {
        loop = atoi(val) - 1;
        if (loop < 0 || loop >= FLOW_LOOP_COUNT) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "loop must be 1-3");
            return ESP_FAIL;
        }
    }

    esp_err_t err;
    if (httpd_query_key_value(query, "zero", val, sizeof(val)) == ESP_OK) {
        err = flow_reader_calibrate_zero(loop);
    } else if (httpd_query_key_value(query, "lpm", val, sizeof(val)) == ESP_OK) {
        err = flow_reader_calibrate_flow(loop, strtof(val, NULL));
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zero=1 or lpm=<value> required");
        return ESP_FAIL;
    }

    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lpm must be between 0.2 and 10");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "no usable reading: ring not found, or not far enough from zero");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 12;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/capture.jpg", .method = HTTP_GET, .handler = capture_handler},
        {.uri = "/api/flows", .method = HTTP_GET, .handler = flows_handler},
        {.uri = "/api/loop", .method = HTTP_POST, .handler = loop_handler},
        {.uri = "/api/framesize", .method = HTTP_POST, .handler = framesize_handler},
        {.uri = "/api/read", .method = HTTP_POST, .handler = read_handler},
        {.uri = "/api/calibrate", .method = HTTP_POST, .handler = calibrate_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ota_register(server);

    ESP_LOGI(TAG, "Web server on port %d", config.server_port);
    return ESP_OK;
}
