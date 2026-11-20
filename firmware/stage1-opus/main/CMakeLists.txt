idf_component_register(
    SRCS "stage1_main.c"
    INCLUDE_DIRS "."
    REQUIRES bsp opus esp_timer esp_partition esp_driver_i2s esp_wifi esp_netif esp_event nvs_flash
)
