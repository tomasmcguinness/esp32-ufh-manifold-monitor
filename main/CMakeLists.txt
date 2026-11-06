idf_component_register(
    SRCS "app_main.c" "ethernet.c" "camera.c" "camera_diag.c" "flow_reader.c" "flows.c" "ota.c" "web_server.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html"
    PRIV_REQUIRES app_update esp_app_format bootloader_support esp_eth esp_netif esp_event esp_http_server esp_timer nvs_flash driver esp_rom
)
