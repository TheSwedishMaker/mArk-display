idf_component_register(
    SRCS
        "main.c"
        "calendar_fetch.c"
        "streak_store.c"
        "ui_taskviewer.c"
        "ws2812_led.c"
        "led_strip_encoder.c"
        "power_button.c"
        "rotary_encoder.c"
        "sound_driver.c"
        "web_config.c"
        "user_store.c"
        "lv_font_ui_14.c"
        "lv_font_ui_24.c"
        "lv_font_ui_48.c"
        "img_gold_medal.c"
        "challenge_store.c"
    INCLUDE_DIRS "."
    REQUIRES
        lvgl
        esp_http_client
        esp_http_server
        esp_wifi
        esp_wifi_remote
        nvs_flash
        json
        driver
        esp_timer
        esp_lcd
    PRIV_REQUIRES
        bsp_illuminate
        mbedtls
        app_update
)

target_compile_options(__idf_main PRIVATE -Wno-format-truncation)
