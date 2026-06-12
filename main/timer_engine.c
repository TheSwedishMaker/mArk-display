#include "timer_engine.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "TIMER_ENG";

static timer_state_t    s_state  = {0};
static esp_timer_handle_t s_htimer = NULL;
static lv_async_cb_t    s_expiry_cb = NULL;

static void timer_tick_cb(void *arg) {
    (void)arg;
    if (!s_state.active || s_state.paused) return;
    s_state.elapsed_sec++;
    if (s_state.elapsed_sec >= s_state.duration_sec) {
        esp_timer_stop(s_htimer);
        s_state.active  = false;
        s_state.expired = true;
        ESP_LOGI(TAG, "Expired: task=%d user=%d", s_state.task_idx, s_state.user_idx);
        if (s_expiry_cb) lv_async_call(s_expiry_cb, NULL);
    }
}

void timer_engine_init(void) {
    memset(&s_state, 0, sizeof(s_state));
    const esp_timer_create_args_t args = {
        .callback = timer_tick_cb,
        .arg      = NULL,
        .name     = "task_timer",
    };
    esp_timer_create(&args, &s_htimer);
    ESP_LOGI(TAG, "Init OK");
}

bool timer_engine_start(int task_idx, int user_idx, uint32_t duration_sec) {
    if (s_state.active || s_state.paused) return false;
    esp_timer_stop(s_htimer);  /* no-op if not running */
    s_state = (timer_state_t){
        .active       = true,
        .task_idx     = task_idx,
        .user_idx     = user_idx,
        .duration_sec = duration_sec,
    };
    esp_timer_start_periodic(s_htimer, 1000000ULL);
    ESP_LOGI(TAG, "Started: task=%d user=%d dur=%ds", task_idx, user_idx, (int)duration_sec);
    return true;
}

void timer_engine_pause(void) {
    if (!s_state.active || s_state.paused) return;
    s_state.paused = true;
    esp_timer_stop(s_htimer);
    ESP_LOGI(TAG, "Paused at %ds elapsed", (int)s_state.elapsed_sec);
}

void timer_engine_resume(void) {
    if (!s_state.active || !s_state.paused) return;
    s_state.paused = false;
    esp_timer_start_periodic(s_htimer, 1000000ULL);
    ESP_LOGI(TAG, "Resumed");
}

void timer_engine_reset(void) {
    if (s_state.duration_sec == 0) return;
    esp_timer_stop(s_htimer);
    int ti = s_state.task_idx, ui = s_state.user_idx;
    uint32_t dur = s_state.duration_sec;
    s_state = (timer_state_t){
        .active       = true,
        .task_idx     = ti,
        .user_idx     = ui,
        .duration_sec = dur,
    };
    esp_timer_start_periodic(s_htimer, 1000000ULL);
    ESP_LOGI(TAG, "Reset");
}

void timer_engine_cancel(void) {
    esp_timer_stop(s_htimer);
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "Cancelled");
}

bool timer_engine_is_busy(void) {
    return s_state.active || s_state.paused;
}

const timer_state_t *timer_engine_get_state(void) { return &s_state; }

uint32_t timer_engine_remaining_sec(void) {
    if (s_state.elapsed_sec >= s_state.duration_sec) return 0;
    return s_state.duration_sec - s_state.elapsed_sec;
}

void timer_engine_set_expiry_cb(lv_async_cb_t cb) { s_expiry_cb = cb; }
