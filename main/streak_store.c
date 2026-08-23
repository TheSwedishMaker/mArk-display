/**
 * Streak store — NVS with WAL pattern (ESP-IDF)
 *
 * Per-user NVS namespaces (max 15 chars each):
 *   Main : u0_stk .. u5_stk
 *   WAL  : u0_wal .. u5_wal
 *
 * Scoring model (v2):
 *   month_days — completed days in the current calendar month (the visible
 *                highscore, resets at month rollover)
 *   total_days — lifetime completed days, basis for levels, never resets
 *
 * Migration: namespaces written by the old consecutive-streak firmware have
 * a "streak" key but no "total" key. On first load the old streak seeds
 * total_days, and month_days is estimated if the last completion falls in
 * the current month. User 0 additionally falls back to the legacy global
 * "streak" namespace.
 */
#include "streak_store.h"
#include "user_store.h"

#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "STREAK";

/* Thresholds are lifetime completed days */
static const streak_level_t LEVELS[] = {
    { "Starter",      0   },
    { "Consistent",   5   },
    { "Dedicated",    15  },
    { "Unstoppable",  30  },
    { "Legend",       50  },
    { "Titan",        100 },
};
#define NUM_LEVELS (sizeof(LEVELS) / sizeof(LEVELS[0]))

streak_data_t streak_data = {0};

/* Tracks which user's data is currently loaded */
static int s_loaded_user = -1;

/* ── Namespace helpers ── */

static void ns_main(int idx, char *buf, size_t len) {
    snprintf(buf, len, "u%d_stk", idx);
}

static void ns_wal(int idx, char *buf, size_t len) {
    snprintf(buf, len, "u%d_wal", idx);
}

static void current_date(int *y, int *m, int *d) {
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    *y = t.tm_year + 1900;
    *m = t.tm_mon + 1;
    if (d) *d = t.tm_mday;
}

/* ── NVS read/write of a full record ── */

static void write_record(nvs_handle_t h, const streak_data_t *sd) {
    nvs_set_i32(h, "mdays", sd->month_days);
    nvs_set_i32(h, "mY",    sd->month_year);
    nvs_set_i32(h, "mMo",   sd->month_month);
    nvs_set_i32(h, "total", sd->total_days);
    nvs_set_i32(h, "lastY", sd->last_year);
    nvs_set_i32(h, "lastM", sd->last_month);
    nvs_set_i32(h, "lastD", sd->last_day);
}

/* Returns true if the namespace held data (v2 or legacy); false if empty.
 * Legacy (old streak firmware) data is migrated in place into *sd and
 * *out_migrated (optional) is set so the caller can persist the result. */
static bool read_record(nvs_handle_t h, streak_data_t *sd, bool *out_migrated) {
    if (out_migrated) *out_migrated = false;
    if (nvs_get_i32(h, "total", &sd->total_days) == ESP_OK) {
        nvs_get_i32(h, "mdays", &sd->month_days);
        nvs_get_i32(h, "mY",    &sd->month_year);
        nvs_get_i32(h, "mMo",   &sd->month_month);
        nvs_get_i32(h, "lastY", &sd->last_year);
        nvs_get_i32(h, "lastM", &sd->last_month);
        nvs_get_i32(h, "lastD", &sd->last_day);
        return true;
    }

    /* Legacy record: consecutive streak + last completed date */
    int32_t old_streak = 0;
    if (nvs_get_i32(h, "streak", &old_streak) != ESP_OK) return false;

    nvs_get_i32(h, "lastY", &sd->last_year);
    nvs_get_i32(h, "lastM", &sd->last_month);
    nvs_get_i32(h, "lastD", &sd->last_day);

    sd->total_days = old_streak;

    int cy, cm;
    current_date(&cy, &cm, NULL);
    if (sd->last_year == cy && sd->last_month == cm) {
        /* Best estimate: the streak can't have covered more of this month
         * than the day-of-month it ended on */
        sd->month_days  = old_streak < sd->last_day ? old_streak : sd->last_day;
        sd->month_year  = cy;
        sd->month_month = cm;
    }
    if (out_migrated) *out_migrated = true;
    ESP_LOGI(TAG, "Migrated legacy streak=%" PRId32 " -> total=%" PRId32
             " month=%" PRId32, old_streak, sd->total_days, sd->month_days);
    return true;
}

/* Persist a migrated record into the user's namespace so the migration runs
 * once and is deterministic — the month_days estimate in read_record depends
 * on the current date, so leaving it unpersisted would let the displayed
 * history change between boots. */
static void persist_record(int idx, const streak_data_t *sd) {
    char ns[16];
    ns_main(idx, ns, sizeof(ns));
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        write_record(h, sd);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ── Internal load / save / WAL ── */

static void load_for_user(int idx) {
    char ns[16];
    ns_main(idx, ns, sizeof(ns));

    nvs_handle_t h;
    bool loaded = false, migrated = false;

    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        loaded = read_record(h, &streak_data, &migrated);
        nvs_close(h);
    }

    /* User 0: fall back to legacy "streak" namespace on first run */
    if (!loaded && idx == 0) {
        if (nvs_open("streak", NVS_READONLY, &h) == ESP_OK) {
            if (read_record(h, &streak_data, NULL)) {
                migrated = true;
                ESP_LOGI(TAG, "User 0: migrated data from legacy namespace");
            }
            nvs_close(h);
        }
    }

    if (migrated)
        persist_record(idx, &streak_data);
}

static void save_with_wal_for_user(int idx) {
    char ns_m[16], ns_w[16];
    ns_main(idx, ns_m, sizeof(ns_m));
    ns_wal(idx,  ns_w, sizeof(ns_w));

    /* 1. Write WAL */
    nvs_handle_t wal;
    if (nvs_open(ns_w, NVS_READWRITE, &wal) == ESP_OK) {
        write_record(wal, &streak_data);
        nvs_set_u8(wal, "valid", 1);
        nvs_commit(wal);
        nvs_close(wal);
    }

    /* 2. Write main */
    nvs_handle_t main_h;
    if (nvs_open(ns_m, NVS_READWRITE, &main_h) == ESP_OK) {
        write_record(main_h, &streak_data);
        nvs_commit(main_h);
        nvs_close(main_h);
    }

    /* 3. Clear WAL */
    if (nvs_open(ns_w, NVS_READWRITE, &wal) == ESP_OK) {
        nvs_erase_all(wal);
        nvs_commit(wal);
        nvs_close(wal);
    }

    ESP_LOGI(TAG, "User %d saved (WAL-safe) month=%" PRId32 " total=%" PRId32,
             idx, streak_data.month_days, streak_data.total_days);
}

static void recover_wal_for_user(int idx) {
    char ns_m[16], ns_w[16];
    ns_main(idx, ns_m, sizeof(ns_m));
    ns_wal(idx,  ns_w, sizeof(ns_w));

    nvs_handle_t wal;
    bool recovered = false;

    if (nvs_open(ns_w, NVS_READONLY, &wal) == ESP_OK) {
        uint8_t valid = 0;
        nvs_get_u8(wal, "valid", &valid);
        if (valid) {
            ESP_LOGW(TAG, "User %d: WAL recovery in progress", idx);
            streak_data_t sd = {0};
            read_record(wal, &sd, NULL);
            nvs_close(wal);

            nvs_handle_t main_h;
            if (nvs_open(ns_m, NVS_READWRITE, &main_h) == ESP_OK) {
                write_record(main_h, &sd);
                nvs_commit(main_h);
                nvs_close(main_h);
            }
            if (nvs_open(ns_w, NVS_READWRITE, &wal) == ESP_OK) {
                nvs_erase_all(wal);
                nvs_commit(wal);
                nvs_close(wal);
            }
            ESP_LOGI(TAG, "User %d: WAL recovery complete", idx);
            recovered = true;
        } else {
            nvs_close(wal);
        }
    }

    /* User 0: replay a legacy "streak_wal" left by a crash on the old
     * firmware — read_record migrates its legacy keys to v2 on the way */
    if (!recovered && idx == 0) {
        if (nvs_open("streak_wal", NVS_READWRITE, &wal) == ESP_OK) {
            uint8_t valid = 0;
            nvs_get_u8(wal, "valid", &valid);
            if (valid) {
                ESP_LOGW(TAG, "User 0: legacy WAL recovery in progress");
                streak_data_t sd = {0};
                if (read_record(wal, &sd, NULL)) {
                    nvs_handle_t main_h;
                    if (nvs_open(ns_m, NVS_READWRITE, &main_h) == ESP_OK) {
                        write_record(main_h, &sd);
                        nvs_commit(main_h);
                        nvs_close(main_h);
                    }
                }
                nvs_erase_all(wal);
                nvs_commit(wal);
                ESP_LOGI(TAG, "User 0: legacy WAL recovery complete");
            }
            nvs_close(wal);
        }
    }
}

/* ── Seed defaults (new device only) ── */

static void seed_streaks_if_empty(void) {
    /* Hardcoded starting totals for the replacement display.
     * Only written if NVS has no data for that user slot yet. */
    static const struct { int32_t total; } seeds[] = {
        { 5 },  /* user 0 — Pierre */
        { 4 },  /* user 1 — Astrid */
        { 4 },  /* user 2 — Ingrid */
        { 4 },  /* user 3 — Julia  */
        { 4 },  /* user 4 — Stina  */
    };
    const int n = sizeof(seeds) / sizeof(seeds[0]);

    for (int i = 0; i < n; i++) {
        char ns[16];
        ns_main(i, ns, sizeof(ns));
        nvs_handle_t h;
        /* Only seed if namespace has no v2 or legacy value at all */
        bool has_data = false;
        if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
            int32_t v = 0;
            has_data = (nvs_get_i32(h, "total", &v) == ESP_OK) ||
                       (nvs_get_i32(h, "streak", &v) == ESP_OK);
            nvs_close(h);
        }
        /* User 0: real history may still live in the legacy global "streak"
         * namespace — seeding over it would shadow the migration path in
         * load_for_user forever */
        if (!has_data && i == 0) {
            if (nvs_open("streak", NVS_READONLY, &h) == ESP_OK) {
                int32_t v = 0;
                has_data = (nvs_get_i32(h, "streak", &v) == ESP_OK);
                nvs_close(h);
            }
        }
        if (!has_data) {
            if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
                streak_data_t sd = {0};
                sd.total_days = seeds[i].total;
                sd.last_year  = 2026;
                sd.last_month = 4;
                sd.last_day   = 13;
                write_record(h, &sd);
                nvs_commit(h);
                nvs_close(h);
                ESP_LOGI(TAG, "Seeded user %d total=%" PRId32, i, seeds[i].total);
            }
        }
    }
}

/* ── Public API ── */

void streak_store_init(void) {
    seed_streaks_if_empty();
    /* Load whichever user was active at last shutdown */
    streak_set_active_user(active_user);
}

void streak_set_active_user(int user_idx) {
    if (user_idx < 0 || user_idx >= user_count) user_idx = 0;

    /* Save the outgoing user's data before switching */
    if (s_loaded_user >= 0 && s_loaded_user != user_idx) {
        save_with_wal_for_user(s_loaded_user);
    }

    memset(&streak_data, 0, sizeof(streak_data));
    recover_wal_for_user(user_idx);
    load_for_user(user_idx);
    s_loaded_user = user_idx;

    ESP_LOGI(TAG, "Active user=%d (%s) month=%" PRId32 " total=%" PRId32,
             user_idx, users[user_idx].name,
             streak_data.month_days, streak_data.total_days);
}

void streak_mark_day_complete(int day_offset) {
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    t.tm_mday += day_offset;
    mktime(&t);

    int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;

    if (streak_data.last_year == y && streak_data.last_month == m && streak_data.last_day == d)
        return;  /* already marked today */

    /* Month rollover: start a fresh monthly count */
    if (streak_data.month_year != y || streak_data.month_month != m) {
        streak_data.month_days  = 0;
        streak_data.month_year  = y;
        streak_data.month_month = m;
    }

    streak_data.month_days++;
    streak_data.total_days++;

    streak_data.last_year  = y;
    streak_data.last_month = m;
    streak_data.last_day   = d;
    save_with_wal_for_user(s_loaded_user);
}

int32_t streak_month_days(const streak_data_t *sd) {
    int cy, cm;
    current_date(&cy, &cm, NULL);
    if (sd->month_year != cy || sd->month_month != cm) return 0;
    return sd->month_days;
}

/* Read any user's data from NVS without disturbing the active streak_data */
void streak_read_user(int user_idx, streak_data_t *out) {
    memset(out, 0, sizeof(*out));
    if (user_idx < 0 || user_idx >= MAX_USERS) return;

    /* If it's the active user, just copy the live data (may be unsaved) */
    if (user_idx == s_loaded_user) {
        *out = streak_data;
        return;
    }

    char ns[16];
    ns_main(user_idx, ns, sizeof(ns));
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        bool migrated = false;
        read_record(h, out, &migrated);
        nvs_close(h);
        if (migrated)
            persist_record(user_idx, out);
    }
}

/* Return the level name for a lifetime total */
const char *streak_level_for(int32_t total_days) {
    const streak_level_t *cur = &LEVELS[0];
    for (int i = 0; i < NUM_LEVELS; i++)
        if (total_days >= LEVELS[i].threshold) cur = &LEVELS[i];
    return cur->name;
}

const streak_level_t *streak_get_level(void) {
    const streak_level_t *cur = &LEVELS[0];
    for (int i = 0; i < NUM_LEVELS; i++)
        if (streak_data.total_days >= LEVELS[i].threshold) cur = &LEVELS[i];
    return cur;
}

const streak_level_t *streak_get_next_level(void) {
    for (int i = 0; i < NUM_LEVELS; i++)
        if (streak_data.total_days < LEVELS[i].threshold) return &LEVELS[i];
    return NULL;
}

int streak_get_progress_to_next(void) {
    const streak_level_t *cur  = streak_get_level();
    const streak_level_t *next = streak_get_next_level();
    if (!next) return 100;
    int range    = next->threshold - cur->threshold;
    int progress = streak_data.total_days - cur->threshold;
    return (progress * 100) / range;
}
