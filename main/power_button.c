/**
 * Soft power button — momentary push on IO3
 * Short press: toggle backlight (sleep/wake)
 */
#include "power_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp_illuminate.h"
#include "ws2812_led.h"
#include "ui_taskviewer.h"

static const char *TAG = "PWR";

#define POWER_BTN_GPIO  3
#define DEBOUNCE_MS     30

static bool display_on = true;

static void power_button_task(void *arg) {
    int64_t press_start = 0;
    bool was_pressed = false;

    while (1) {
        bool pressed = (gpio_get_level(POWER_BTN_GPIO) == 0);  /* active low */

        if (pressed && !was_pressed) {
            /* Button just pressed */
            press_start = esp_timer_get_time();
        } else if (!pressed && was_pressed) {
            /* Button released */
            int64_t duration_ms = (esp_timer_get_time() - press_start) / 1000;

            if (duration_ms >= DEBOUNCE_MS) {
                /* Short press → toggle display */
                display_on = !display_on;
                set_lcd_blight(display_on ? 100 : 0);
                ui_set_display_sleeping(!display_on);
                if (display_on) {
                    ui_led_refresh();
                }
                ESP_LOGI(TAG, "Display %s", display_on ? "ON" : "OFF");
            }
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void power_button_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << POWER_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&cfg);

    xTaskCreate(power_button_task, "pwr_btn", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "Init OK: IO%d (short press = display on/off)", POWER_BTN_GPIO);
}
