/**
 * Sound driver — I2S audio for CrowPanel Advanced (ESP-IDF)
 * Uses SPK-R and SPK-L ports with built-in amplifier
 */
#pragma once

void sound_init(void);
void sound_task_complete(void);
void sound_day_complete(void);
void sound_timer_alarm(void);   /* start repeating alarm — call sound_timer_stop() to end */
void sound_timer_stop(void);    /* stop the repeating alarm */
void sound_set_volume(int vol); /* 0 = mute, 100 = max */
int  sound_get_volume(void);
