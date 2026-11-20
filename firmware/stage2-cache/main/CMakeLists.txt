idf_component_register(
    SRCS "stage2_main.c" "audio_cache.c"
    INCLUDE_DIRS "."
    REQUIRES bsp opus esp_timer esp_partition esp_driver_i2s
)
