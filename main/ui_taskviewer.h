/**
 * Task Viewer LVGL UI — 1024x600 Dieter Rams aesthetic
 */
#pragma once

#include <stdbool.h>

void ui_build(void);
void ui_refresh_all(void);
void ui_refresh_clock(void);
void ui_next_task(void);
void ui_prev_task(void);
bool ui_is_complete_shown(void);
bool ui_is_sleeping(void);
void ui_complete_current_task(void);
void ui_dismiss_complete(void);
/* Re-apply current progress to LEDs — safe to call from any task (no LVGL) */
void ui_led_refresh(void);
/* Sync physical power button sleep state so LEDs stay off when display is off */
void ui_set_display_sleeping(bool sleeping);
/* Wake the display: deletes sleep overlay (if any), restores LEDs, requests refresh.
 * Acquires the LVGL lock internally — safe to call from any task. */
void ui_wake_display(void);
