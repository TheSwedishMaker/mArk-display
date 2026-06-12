/**
 * Sound driver — I2S audio via ESP-IDF
 * CrowPanel Advanced has built-in audio amplifier on SPK-R/SPK-L
 *
 * Note: Exact I2S pins need verification from the CrowPanel P4 schematic.
 * The pins below are placeholders — check your board's documentation.
 */
#include "sound_driver.h"

#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "SND";

/* I2S pins from CrowPanel Advanced P4 schematic (9"/10.1") */
#define I2S_BCLK_PIN   22
#define I2S_WS_PIN     21   /* LRCLK */
#define I2S_DOUT_PIN   23   /* SDATA */
#define AMP_CTRL_PIN   30   /* NS4168 amplifier enable */

#define SAMPLE_RATE    16000

static i2s_chan_handle_t tx_chan = NULL;
static QueueHandle_t sound_queue = NULL;
static bool sound_ready = false;
static int  s_volume = 80;   /* 0–100, persisted in NVS */

typedef enum {
    SOUND_TASK_COMPLETE = 1,
    SOUND_DAY_COMPLETE  = 2,
    SOUND_TIMER_ALARM   = 3,
    SOUND_TIMER_STOP    = 4,
} sound_event_t;

static volatile bool s_alarm_active = false;

static void amp_on(void)  { gpio_set_level(AMP_CTRL_PIN, 0); }
static void amp_off(void) { gpio_set_level(AMP_CTRL_PIN, 1); }

void sound_set_volume(int vol) {
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "volume", vol);
        nvs_commit(h);
        nvs_close(h);
    }
}

int sound_get_volume(void) { return s_volume; }

static void play_tone(uint16_t freq, uint16_t duration_ms, uint8_t volume) {
    if (!sound_ready || !tx_chan) {
        ESP_LOGW(TAG, "play_tone skipped: ready=%d chan=%p", sound_ready, tx_chan);
        return;
    }
    ESP_LOGI(TAG, "Playing tone: %dHz %dms vol=%d", freq, duration_ms, volume);

    /* Enable amp + channel for this tone */
    amp_on();
    vTaskDelay(pdMS_TO_TICKS(2));
    i2s_channel_enable(tx_chan);

    int total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    int16_t buf[256];  /* stereo: L+R per sample = 2 int16 per sample, 64 samples max */
    float amplitude = (32767.0f * volume * s_volume) / (100.0f * 100.0f);

    int idx = 0;
    while (idx < total_samples) {
        int chunk = (64 < (total_samples - idx)) ? 64 : (total_samples - idx);
        for (int i = 0; i < chunk; i++) {
            float t = (float)(idx + i) / (float)SAMPLE_RATE;
            int16_t val = (int16_t)(amplitude * sinf(2.0f * M_PI * freq * t));

            /* Fade in/out to avoid clicks */
            int fade = (SAMPLE_RATE * 3) / 1000;
            int pos = idx + i;
            if (fade > 0 && pos < fade)
                val = (int16_t)(val * ((float)pos / (float)fade));
            else if (fade > 0 && pos > total_samples - fade)
                val = (int16_t)(val * ((float)(total_samples - pos) / (float)fade));

            buf[i * 2] = val;       /* Left channel */
            buf[i * 2 + 1] = val;   /* Right channel */
        }
        size_t written = 0;
        i2s_channel_write(tx_chan, buf, chunk * 4, &written, portMAX_DELAY);
        idx += chunk;
    }

    /* Flush silence to push out any remaining audio in DMA */
    memset(buf, 0, sizeof(buf));
    size_t w = 0;
    for (int i = 0; i < 8; i++) {
        i2s_channel_write(tx_chan, buf, sizeof(buf), &w, pdMS_TO_TICKS(50));
    }

    /* Disable channel — DMA stops completely, no looping */
    i2s_channel_disable(tx_chan);

    /* Turn off amplifier to kill hiss/noise */
    amp_off();
}

static void sound_worker(void *arg) {
    uint8_t evt;
    while (1) {
        if (xQueueReceive(sound_queue, &evt, portMAX_DELAY)) {
            switch (evt) {
            case SOUND_TASK_COMPLETE:
                ESP_LOGI(TAG, ">>> TASK COMPLETE sound");
                play_tone(880, 120, 70);
                vTaskDelay(pdMS_TO_TICKS(30));
                play_tone(1175, 180, 80);
                break;
            case SOUND_DAY_COMPLETE:
                ESP_LOGI(TAG, ">>> DAY COMPLETE sound");
                play_tone(523, 140, 70);
                vTaskDelay(pdMS_TO_TICKS(25));
                play_tone(659, 140, 75);
                vTaskDelay(pdMS_TO_TICKS(25));
                play_tone(784, 140, 80);
                vTaskDelay(pdMS_TO_TICKS(25));
                play_tone(1047, 250, 85);
                break;
            case SOUND_TIMER_ALARM:
                if (s_alarm_active) {
                    ESP_LOGI(TAG, ">>> TIMER ALARM beep");
                    play_tone(987,  110, 80);
                    vTaskDelay(pdMS_TO_TICKS(45));
                    play_tone(1175, 110, 80);
                    vTaskDelay(pdMS_TO_TICKS(45));
                    play_tone(1568, 260, 90);
                    vTaskDelay(pdMS_TO_TICKS(700));
                    /* Re-enqueue to repeat while flag is set */
                    if (s_alarm_active) {
                        uint8_t ev2 = SOUND_TIMER_ALARM;
                        xQueueSend(sound_queue, &ev2, 0);
                    }
                }
                break;
            case SOUND_TIMER_STOP:
                s_alarm_active = false;
                ESP_LOGI(TAG, ">>> TIMER ALARM stopped");
                break;
            }
        }
    }
}

void sound_init(void) {
    ESP_LOGW(TAG, "===== Sound init starting =====");

    /* Load persisted volume */
    nvs_handle_t nvs_h;
    if (nvs_open("settings", NVS_READONLY, &nvs_h) == ESP_OK) {
        int32_t vol = 80;
        nvs_get_i32(nvs_h, "volume", &vol);
        s_volume = (int)vol;
        nvs_close(nvs_h);
    }

    /* Enable amplifier (NS4168 CTRL pin) */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << AMP_CTRL_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t amp_err = gpio_config(&io_conf);
    if (amp_err != ESP_OK) {
        ESP_LOGE(TAG, "AMP GPIO config failed: %s", esp_err_to_name(amp_err));
        return;
    }
    gpio_set_level(AMP_CTRL_PIN, 1);  /* Start with amp OFF (active low) */
    ESP_LOGW(TAG, "Amplifier OFF at init (GPIO%d = HIGH)", AMP_CTRL_PIN);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 256;

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel create failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "I2S channel created OK");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws = (gpio_num_t)I2S_WS_PIN,
            .dout = (gpio_num_t)I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "I2S std mode init OK");

    /* Channel stays disabled — play_tone() enables/disables per tone */
    ESP_LOGW(TAG, "I2S channel ready (enabled on demand)");

    sound_queue = xQueueCreate(6, sizeof(uint8_t));
    xTaskCreate(sound_worker, "sound", 4096, NULL, 1, NULL);
    sound_ready = true;
    ESP_LOGW(TAG, "===== I2S sound ready (BCLK=%d, WS=%d, DOUT=%d) =====", I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN);
}

static void enqueue(uint8_t evt) {
    if (!sound_ready || !sound_queue) {
        ESP_LOGW(TAG, "enqueue skipped: ready=%d queue=%p", sound_ready, sound_queue);
        return;
    }
    ESP_LOGI(TAG, "Enqueuing sound event: %d", evt);
    xQueueSend(sound_queue, &evt, 0);
}

void sound_task_complete(void) { enqueue(SOUND_TASK_COMPLETE); }
void sound_day_complete(void)  { enqueue(SOUND_DAY_COMPLETE); }

void sound_timer_alarm(void) {
    s_alarm_active = true;
    enqueue(SOUND_TIMER_ALARM);
}

void sound_timer_stop(void) {
    s_alarm_active = false;
    enqueue(SOUND_TIMER_STOP);
}
