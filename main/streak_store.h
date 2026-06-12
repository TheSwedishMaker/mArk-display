/**
 * Streak store — NVS persistence with WAL safety (ESP-IDF)
 * Supports up to MAX_USERS independent counters via per-user NVS namespaces.
 *
 * Scoring model:
 *   month_days — completed days in the current calendar month (the visible
 *                "highscore"; resets naturally at each month rollover)
 *   total_days — lifetime completed days; levels build on this and it
 *                never resets
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name;
    int threshold;
} streak_level_t;

typedef struct {
    int32_t month_days;   /* completed days in (month_year, month_month) */
    int32_t month_year;
    int32_t month_month;
    int32_t total_days;   /* lifetime completed days — basis for levels */
    int32_t last_year;    /* last completed date, for dedupe */
    int32_t last_month;
    int32_t last_day;
} streak_data_t;

/* Always holds the active user's data — read by UI and completion logic */
extern streak_data_t streak_data;

/**
 * Load the active user's data from NVS (with WAL recovery).
 * Call once at boot after user_store_init().
 */
void streak_store_init(void);

/**
 * Switch the active user: saves the current user's data, then loads
 * the new user's data into streak_data.
 * Call this whenever active_user changes.
 */
void streak_set_active_user(int user_idx);

/** Mark today complete for the current active user and persist. */
void streak_mark_day_complete(int day_offset);

/* Month count valid for the current month — returns 0 if the stored
 * count belongs to a previous month (i.e. nothing completed yet) */
int32_t streak_month_days(const streak_data_t *sd);

const streak_level_t *streak_get_level(void);
const streak_level_t *streak_get_next_level(void);
int streak_get_progress_to_next(void);

/* Read any user's data from NVS without affecting the active user */
void streak_read_user(int user_idx, streak_data_t *out);
const char *streak_level_for(int32_t total_days);

/** Reset a user's scores to zero and persist to NVS. */
void streak_reset_user(int user_idx);

/** Call after user_store_remove(idx) to keep NVS namespaces in sync. */
void streak_shift_down(int removed_idx, int new_count);
