idf_component_register(
    SRCS "main.c"
         "app_ble.c"
         "app_error.c"
         "app_config.c"
         "app_net.c"
         "app_store.c"
         "app_ogg.c"
         "app_record.c"
         "app_upload.c"
         "app_prov.c"
         "app_ui.c"
         "octobara_frames.S"
         "audio_cache.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "lv_font_ui_14.c"
         "lv_font_ui_20.c"
    INCLUDE_DIRS "."
    REQUIRES
        bsp
        bt
        opus
        esp_timer
        esp_partition
        esp_driver_i2s
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_http_server
        esp-tls
        lvgl__lvgl
)

set_source_files_properties(octobara_frames.S PROPERTIES
    COMPILE_OPTIONS "-Wa,-I${CMAKE_CURRENT_LIST_DIR}"
    OBJECT_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/octobara.rgb565"
)