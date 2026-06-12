/**
 * Timer engine — single countdown timer with pause/resume/reset/cancel.
 * Driven by esp_timer (1 s tick). Calls lv_async_call on expiry so the
 * registered handler runs safely in the LVGL task context.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct {
    bool     active;        /* counting down (not paused, not expired) */
    bool     paused;        /* mid-count pause */
    bool     expired;       /* reached zero — alarm should ring */
    int      task_idx;      /* cal_tasks[] index this timer belongs to */
    int      user_idx;      /* which user */
    uint32_t duration_sec;
    uint32_t elapsed_sec;
} timer_state_t;

void     timer_engine_init(void);

/* Start a new timer. Returns false if a timer is already active/paused. */
bool     timer_engine_start(int task_idx, int user_idx, uint32_t duration_sec);

void     timer_engine_pause(void);
void     timer_engine_resume(void);
void     timer_engine_reset(void);   /* restart from zero, sets active=true */
void     timer_engine_cancel(void);  /* stop and clear all state */

/* True if a timer is active or paused (something is "in flight") */
bool     timer_engine_is_busy(void);

const timer_state_t *timer_engine_get_state(void);
uint32_t             timer_engine_remaining_sec(void);

/* Register callback to invoke (via lv_async_call) when timer expires. */
void timer_engine_set_expiry_cb(lv_async_cb_t cb);
