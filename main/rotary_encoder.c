/**
 * Rotary encoder — full quadrature driver (ESP-IDF)
 * A=IO27, B=IO28, Push=IO5
 * Threshold: 1 edge = 1 scroll, with time-based debounce.
 */
#include "rotary_encoder.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ENC";

#define ENC_A_GPIO    27
#define ENC_B_GPIO    28
#define ENC_BTN_GPIO  5

static const int8_t enc_table[4][4] = {
    {  0, -1, +1,  0 },
    { +1,  0,  0, -1 },
    { -1,  0,  0, +1 },
    {  0, +1, -1,  0 },
};

static uint8_t last_state = 0;
static int accumulator = 0;
static int64_t last_scroll_us = 0;
#define SCROLL_DEBOUNCE_US  150000  /* 150ms between scroll events */

void encoder_init(void) {
    /* A/B: pull-up (encoder outputs pull low on transitions) */
    gpio_config_t cfg_ab = {
        .pin_bit_mask = (1ULL << ENC_A_GPIO) | (1ULL << ENC_B_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg_ab);

    /* BTN: pull-down — button connects IO5 to 3.3V, so pressed = HIGH */
    gpio_config_t cfg_btn = {
        .pin_bit_mask = (1ULL << ENC_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg_btn);

    last_state = (gpio_get_level(ENC_A_GPIO) << 1) | gpio_get_level(ENC_B_GPIO);
    ESP_LOGI(TAG, "Init OK: A=IO%d B=IO%d BTN=IO%d (state=%d)",
             ENC_A_GPIO, ENC_B_GPIO, ENC_BTN_GPIO, last_state);
}

int encoder_poll(void) {
    uint8_t a = gpio_get_level(ENC_A_GPIO);
    uint8_t b = gpio_get_level(ENC_B_GPIO);
    uint8_t new_state = (a << 1) | b;

    if (new_state == last_state) return 0;

    int8_t delta = enc_table[last_state][new_state];
    last_state = new_state;
    accumulator += delta;

    /* Threshold 4: two full detents (4 quadrature edges) = 1 scroll */
    int result = 0;
    if (accumulator >= 4) {
        result = +1;
        accumulator = 0;
    } else if (accumulator <= -4) {
        result = -1;
        accumulator = 0;
    }

    if (result != 0) {
        int64_t now = esp_timer_get_time();
        if ((now - last_scroll_us) < SCROLL_DEBOUNCE_US) {
            return 0;  /* too soon, suppress */
        }
        last_scroll_us = now;
        ESP_LOGI(TAG, "SCROLL %+d", result);
    }

    return result;
}

bool encoder_button_pressed(void) {
    return gpio_get_level(ENC_BTN_GPIO) == 1;  /* active HIGH: button pulls IO5 to 3.3V */
}
