idf_component_register(
    SRCS
        "esp32-network.c"
        "proto.c"
        "wifi_mgr.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        driver
)
