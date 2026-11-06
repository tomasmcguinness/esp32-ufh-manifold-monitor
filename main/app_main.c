#include "camera.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ethernet.h"
#include "flow_reader.h"
#include "flows.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "ota.h"
#include "web_server.h"

static const char *TAG = "app";

#define MDNS_HOSTNAME "ufh-monitor"

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("UFH Manifold Monitor");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(flows_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Bring the camera up first: a wiring or ribbon problem shows in the log
    // immediately, without waiting on DHCP.
    if (camera_start() != ESP_OK) {
        ESP_LOGE(TAG, "Continuing without a camera; /capture.jpg will return 500");
    } else {
        flow_reader_start();
    }

    ESP_ERROR_CHECK(ethernet_start());
    start_mdns();
    ESP_ERROR_CHECK(web_server_start());

    while (!ethernet_wait_for_ip(30000)) {
        ESP_LOGW(TAG, "Still waiting for an IP address");
    }
    ESP_LOGI(TAG, "Open http://%s/ or http://%s.local/", ethernet_ip_string(), MDNS_HOSTNAME);

    // Reachable over the network with the web server up: that is the bar a new image has to
    // clear. If it crashes before this, the bootloader rolls back to the previous firmware.
    ota_mark_running_app_valid();
}
