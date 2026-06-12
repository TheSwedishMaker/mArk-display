/**
 * Task Viewer v4.0 — ESP-IDF for CrowPanel Advanced 10.1" (ESP32-P4)
 * 1024x600 MIPI-DSI IPS, GT911 touch, ESP32-C6 WiFi
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "bsp_illuminate.h"   /* display_init(), set_lcd_blight() */

#include "calendar_fetch.h"
#include "streak_store.h"
#include "challenge_store.h"
#include "ui_taskviewer.h"
#include "ws2812_led.h"
#include "power_button.h"
#include "rotary_encoder.h"
#include "sound_driver.h"
#include "web_config.h"
#include "user_store.h"

static const char *TAG = "MAIN";

/* ── LDO handles (required for CrowPanel P4 display power) ── */
static esp_ldo_channel_handle_t ldo3 = NULL;
static esp_ldo_channel_handle_t ldo4 = NULL;

static void init_fail(const char *module, esp_err_t err) {
    while (1) {
        ESP_LOGE(TAG, "[%s] init failed: %s", module, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void system_init(void) {
    esp_err_t err;

    /* 1. NVS (needed for WiFi + streak store) */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) init_fail("NVS", err);
    ESP_LOGI(TAG, "NVS ready");

    /* 2. LDO regulators (required for display) */
    ws2812_set_all(64, 0, 0); /* RED: acquiring LDOs */
    esp_ldo_channel_config_t ldo3_cfg = { .chan_id = 3, .voltage_mv = 2500 };
    err = esp_ldo_acquire_channel(&ldo3_cfg, &ldo3);
    if (err != ESP_OK) init_fail("LDO3", err);

    esp_ldo_channel_config_t ldo4_cfg = { .chan_id = 4, .voltage_mv = 3300 };
    err = esp_ldo_acquire_channel(&ldo4_cfg, &ldo4);
    if (err != ESP_OK) init_fail("LDO4", err);
    ESP_LOGI(TAG, "LDO3/LDO4 ready");
    vTaskDelay(pdMS_TO_TICKS(100)); /* allow display/touch hardware to power up */

    /* 3. I2C (for touch controller) */
    ws2812_set_all(64, 32, 0); /* ORANGE: I2C init */
    err = i2c_init();
    if (err != ESP_OK) init_fail("I2C", err);
    ESP_LOGI(TAG, "I2C ready");

    /* 4. Touch panel (non-fatal: display can work without touch) */
    ws2812_set_all(64, 64, 0); /* YELLOW: touch init */
    err = touch_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed (%s) — continuing without touch", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Touch ready");
    }

    /* 5. Display (MIPI-DSI + LVGL) */
    ws2812_set_all(0, 0, 64); /* BLUE: display init */
    err = display_init();
    if (err != ESP_OK) init_fail("Display", err);
    ESP_LOGI(TAG, "Display ready");

    /* 6. Backlight */
    ws2812_set_all(0, 64, 0); /* GREEN: backlight on */
    err = set_lcd_blight(100);
    if (err != ESP_OK) init_fail("Backlight", err);
    ESP_LOGI(TAG, "Backlight on");
    ws2812_set_all(0, 0, 0); /* off: normal operation */
}

/* ── Calendar refresh task (also handles deferred web config refreshes) ── */
static void calendar_refresh_task(void *arg) {
    static char last_date[9] = "";  /* YYYYMMDD of last fetch */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));  /* check frequently for pending refresh */

        /* Detect date change (midnight rollover) — force refresh for new day */
        if (wifi_is_connected()) {
            time_t now = time(NULL);
            struct tm t;
            localtime_r(&now, &t);
            char today[9];
            snprintf(today, sizeof(today), "%04d%02d%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
            if (last_date[0] != '\0' && strcmp(last_date, today) != 0) {
                ESP_LOGI(TAG, "Date changed → %s, refreshing calendar...", today);
                calendar_request_refresh();
            }
            strncpy(last_date, today, sizeof(last_date));
        }

        /* Deferred refresh from user switch, web config, or date change */
        if (wifi_is_connected() && calendar_refresh_pending()) {
            ESP_LOGI(TAG, "Deferred calendar refresh...");
            calendar_fetch();  /* network fetch — no lock, UI remains responsive */
            if (lvgl_port_lock(1000)) {
                calendar_apply_staged();
                ui_refresh_all();
                lvgl_port_unlock();
            }
            continue;
        }

        /* Periodic refresh every 5 minutes */
        static uint32_t last_periodic = 0;
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (last_periodic == 0) last_periodic = now_ms;
        if ((now_ms - last_periodic) >= (5 * 60 * 1000) && wifi_is_connected()) {
            ESP_LOGI(TAG, "Auto-refreshing calendar...");
            calendar_fetch();
            if (lvgl_port_lock(1000)) {
                calendar_apply_staged();
                ui_refresh_all();
                lvgl_port_unlock();
            }
            last_periodic = now_ms;
        }
    }
}

/* ── Encoder polling task ── */
static void encoder_task(void *arg) {
    bool btn_was_pressed = false;
    uint32_t heartbeat = 0;

    ESP_LOGI(TAG, "Encoder task started!");

    while (1) {
        /* Heartbeat every ~5s (2500 × 2ms) */
        if (++heartbeat % 2500 == 0) {
            uint8_t a = gpio_get_level(27);
            uint8_t b = gpio_get_level(28);
            uint8_t btn = gpio_get_level(5);
            ESP_LOGI(TAG, "ENC heartbeat: A=%d B=%d BTN=%d", a, b, btn);
        }

        /* Rotation: scroll tasks */
        int rot = encoder_poll();
        if (rot != 0 && !ui_is_complete_shown()) {
            if (lvgl_port_lock(100)) {
                if (rot > 0) ui_next_task();
                else ui_prev_task();
                lvgl_port_unlock();
            }
        }

        /* Push button: toggle complete on current task (works even on complete screen) */
        bool btn_pressed = encoder_button_pressed();
        if (btn_pressed && !btn_was_pressed) {
            static uint32_t btn_last_ms = 0;
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if ((now_ms - btn_last_ms) >= 400) {   /* 400ms debounce — kills contact bounce */
                bool handled = false;
                if (ui_is_complete_shown()) {
                    if (lvgl_port_lock(100)) {
                        ui_dismiss_complete();
                        lvgl_port_unlock();
                        handled = true;
                    }
                } else {
                    if (lvgl_port_lock(100)) {
                        ui_complete_current_task();
                        lvgl_port_unlock();
                        handled = true;
                    }
                }
                if (handled) btn_last_ms = now_ms;
            }
        }
        btn_was_pressed = btn_pressed;

        vTaskDelay(pdMS_TO_TICKS(2));  /* poll at 500 Hz for reliable rotation */
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=============================");
    ESP_LOGI(TAG, "  TASK VIEWER v4.0 (ESP32-P4)");
    ESP_LOGI(TAG, "=============================");

    /* Blank LEDs immediately (they show random colors on power-up) */
    ws2812_init();
    ESP_LOGI(TAG, "WS2812 blanked on startup");

    /* System hardware init */
    system_init();

    /* Peripheral drivers */
    encoder_init();
    ESP_LOGI(TAG, "Encoder ready (A=IO27, B=IO28, Push=IO5)");

    /* ws2812 already initialized above */

    power_button_init();
    ESP_LOGI(TAG, "Power button ready (IO3)");

    streak_store_init();
    ESP_LOGI(TAG, "Streak store ready");

    /* User profiles must be loaded before streak and calendar sources */
    user_store_init();
    ESP_LOGI(TAG, "User store ready (%d user(s), active=%d)", user_count, active_user);

    challenge_store_init();
    ESP_LOGI(TAG, "Challenge store ready");

    /* Load saved calendar sources for the active user */
    calendar_sources_load();

    sound_init();
    ESP_LOGI(TAG, "Sound ready");

    /* Show loading screen */
    if (lvgl_port_lock(0)) {
        lv_obj_t *loading = lv_label_create(lv_scr_act());
        lv_label_set_text(loading, "Ansluter...");
        lv_obj_set_style_text_color(loading, lv_color_hex(0x6E7080), 0);
        lv_obj_set_style_text_font(loading, &lv_font_montserrat_14, 0);
        lv_obj_center(loading);
        lvgl_port_unlock();
    }

    /* WiFi + NTP */
    bool online = wifi_init_and_connect();
    if (online) {
        ESP_LOGI(TAG, "WiFi connected");
        calendar_request_refresh();
        /* Start web config server */
        web_config_start();
        ESP_LOGI(TAG, "Web config at http://%s/", web_config_get_ip());
    } else {
        ESP_LOGW(TAG, "Offline — showing placeholder");
        calendar_set_offline_placeholder();
    }

    /* Build main UI */
    if (lvgl_port_lock(0)) {
        lv_obj_clean(lv_scr_act());
        ui_build();
        lvgl_port_unlock();
    }

    /* Update LED strip */
    ws2812_update_progress(calendar_get_completed(), calendar_get_total());

    /* Background tasks */
    xTaskCreate(calendar_refresh_task, "cal_refresh", 8192, NULL, 2, NULL);
    xTaskCreate(encoder_task, "encoder", 8192, NULL, 3, NULL);

    ESP_LOGI(TAG, "Setup complete!");
}
