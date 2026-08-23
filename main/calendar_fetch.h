/**
 * Calendar fetcher — ESP-IDF version
 * Supports multiple Google Calendar + ICS sources.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_TASKS     20
#define MAX_TITLE_LEN 64
#define MAX_CAL_SOURCES 5
#define CAL_URL_MAX 512

/* ── Calendar source definition (shared across modules) ── */
typedef struct {
    int type;           /* 0=google, 1=ics */
    char name[32];
    char url[CAL_URL_MAX];
    bool enabled;
} cal_source_t;

/* Calendar source array (defined in ui_taskviewer.c) */
extern cal_source_t cal_sources[];
extern int cal_source_count;

/* ── Task definition ── */
typedef struct {
    char id[64];
    char title[MAX_TITLE_LEN];
    char time[8];       /* "HH:MM" or "All day" */
    bool completed;
} cal_task_t;

/* Current task list (populated by calendar_fetch) */
extern cal_task_t cal_tasks[];
extern int         cal_task_count;
extern int         cal_day_offset;

/* WiFi management */
bool wifi_init_and_connect(void);
bool wifi_is_connected(void);

/* Calendar operations */
bool calendar_fetch(void);
void calendar_apply_staged(void);   /* apply fetch results to cal_tasks[] — call under LVGL lock */
void calendar_set_offline_placeholder(void);
int  calendar_get_completed(void);
int  calendar_get_total(void);

/* Source persistence (NVS) — operate on the active user */
void calendar_sources_save(void);
void calendar_sources_load(void);

/* Per-user variants — use these when switching users */
void calendar_sources_save_user(int user_idx);
void calendar_sources_load_user(int user_idx);

/* Read/write a user's sources into/from an explicit buffer without
 * touching the live cal_sources[] array. Used by the settings editor
 * when configuring a non-active user's calendars. */
int  calendar_sources_read_user(int user_idx, cal_source_t *dest, int max_count);
void calendar_sources_write_user(int user_idx, const cal_source_t *src, int count);

/* Per-user task cache — restored instantly on user switch */
bool calendar_restore_cached_tasks(int user_idx);

/* Per-user completion state — call before switching active_user */
void calendar_save_completion_state(void);
void calendar_suppress_next_completion_save(void);
void calendar_sync_completion_cache(void);   /* call after toggling a task's completion */

/* Manual tasks (NVS-backed, per user, per date YYYYMMDD — editable from web UI) */
void manual_tasks_save_user(int user_idx, const char *date8, const cal_task_t *tasks, int count);
int  manual_tasks_load_user(int user_idx, const char *date8, cal_task_t *dest, int max_count);

/* Weekly recurring tasks (NVS-backed, per user, per weekday — Sunday=0..Saturday=6) */
void weekly_tasks_save_user(int user_idx, int wday, const cal_task_t *tasks, int count);
int  weekly_tasks_load_user(int user_idx, int wday, cal_task_t *dest, int max_count);

/* Deferred refresh (set flag, picked up by refresh task) */
void calendar_request_refresh(void);
bool calendar_refresh_pending(void);

/* Date helpers */
void calendar_get_greeting(char *buf, size_t len);
void calendar_get_date_str(char *buf, size_t len);
void calendar_get_day_label(char *buf, size_t len);
