/**
 * Soft power button — momentary push on IO3
 */
#pragma once

void power_button_init(void);
/* Turn on backlight and sync internal display_on state — safe to call from any context. */
void power_button_force_wake(void);
