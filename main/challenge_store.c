/**
 * Challenge store — NVS persistence for series medal tracking (ESP-IDF)
 *
 * Per-user NVS namespaces: u0_chall .. u5_chall (max 15 chars each)
 *
 * Keys per namespace:
 *   "total"        int16  — total medals across all series
 *   "{hash8}_e"    int16  — extra completions toward next medal for that series
 *   "{hash8}_m"    int16  — medals earned for that series
 *
 * Medal logic (per series):
 *   - On complete: extra++; if extra >= target → medals++, extra = 0
 *   - On uncomplete: extra-- (floor at 0; earned medals are permanent)
 */
#include "challenge_store.h"
#include "user_store.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "CHALL";

static int s_loaded_user = -1;
static int16_t s_cached_total = 0;   /* active user's total medals, kept in sync */

/* ── Helpers ── */

static void chall_ns(int idx, char *buf, size_t len) {
    snprintf(buf, len, "u%d_chall", idx);
}

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static void make_keys(const char *series, char *k_extra, char *k_medals) {
    uint32_t h = fnv1a(series);
    /* 8 hex + "_e"/"_m" = 10 chars — fits NVS 15-char limit */
    snprintf(k_extra,  12, "%08" PRIx32 "_e", h);
    snprintf(k_medals, 12, "%08" PRIx32 "_m", h);
}

/* ── Public API ── */

void challenge_store_init(void) {
    challenge_set_active_user(active_user);
}

void challenge_set_active_user(int user_idx) {
    if (user_idx < 0 || user_idx >= MAX_USERS) user_idx = 0;
    s_loaded_user = user_idx;
    /* Read directly from NVS — calling challenge_total_medals() here would hit
       the s_loaded_user == user_idx early-return and return the stale cache. */
    char ns[16];
    chall_ns(user_idx, ns, sizeof(ns));
    s_cached_total = 0;
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i16(h, "total", &s_cached_total);
        nvs_close(h);
    }
}

bool challenge_complete(const char *series, int16_t target) {
    if (!series || series[0] == '\0' || target <= 0) return false;

    char ns[16], k_e[12], k_m[12];
    chall_ns(s_loaded_user, ns, sizeof(ns));
    make_keys(series, k_e, k_m);

    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;

    int16_t extra = 0, medals = 0;
    nvs_get_i16(h, k_e, &extra);
    nvs_get_i16(h, k_m, &medals);

    extra++;
    bool new_medal = false;
    if (extra >= target) {
        medals++;
        extra = 0;
        new_medal = true;
        s_cached_total++;
        nvs_set_i16(h, "total", s_cached_total);
        ESP_LOGI(TAG, "User %d: medal #%d earned for series '%s'",
                 s_loaded_user, medals, series);
    }

    nvs_set_i16(h, k_e, extra);
    nvs_set_i16(h, k_m, medals);
    nvs_commit(h);
    nvs_close(h);
    return new_medal;
}

void challenge_uncomplete(const char *series) {
    if (!series || series[0] == '\0') return;

    char ns[16], k_e[12], k_m[12];
    chall_ns(s_loaded_user, ns, sizeof(ns));
    make_keys(series, k_e, k_m);

    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;

    int16_t extra = 0;
    nvs_get_i16(h, k_e, &extra);
    if (extra > 0) {
        extra--;
        nvs_set_i16(h, k_e, extra);
        nvs_commit(h);
    }
    nvs_close(h);
}

void challenge_get_progress(const char *series, int16_t *extra_out, int16_t *medals_out) {
    if (extra_out)  *extra_out  = 0;
    if (medals_out) *medals_out = 0;
    if (!series || series[0] == '\0') return;

    char ns[16], k_e[12], k_m[12];
    chall_ns(s_loaded_user, ns, sizeof(ns));
    make_keys(series, k_e, k_m);

    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return;
    if (extra_out)  nvs_get_i16(h, k_e,  extra_out);
    if (medals_out) nvs_get_i16(h, k_m, medals_out);
    nvs_close(h);
}

void challenge_shift_down(int removed_idx, int new_count) {
    for (int i = removed_idx; i < new_count; i++) {
        char src_ns[16], dst_ns[16];
        chall_ns(i + 1, src_ns, sizeof(src_ns));
        chall_ns(i,     dst_ns, sizeof(dst_ns));

        /* Read all keys from slot i+1 */
        nvs_handle_t src;
        nvs_iterator_t it = NULL;
        if (nvs_open(src_ns, NVS_READONLY, &src) == ESP_OK) {
            /* Copy "total" */
            int16_t total = 0;
            nvs_get_i16(src, "total", &total);
            nvs_close(src);

            /* Erase dst, write total */
            nvs_handle_t dst;
            if (nvs_open(dst_ns, NVS_READWRITE, &dst) == ESP_OK) {
                nvs_erase_all(dst);
                nvs_set_i16(dst, "total", total);
                nvs_commit(dst);
                nvs_close(dst);
            }
        }
        /* Per-series keys use hashed names — copy entire namespace via iterator */
        if (nvs_open(src_ns, NVS_READONLY, &src) == ESP_OK) {
            nvs_handle_t dst;
            if (nvs_open(dst_ns, NVS_READWRITE, &dst) == ESP_OK) {
                esp_err_t err = nvs_entry_find_in_handle(src, NVS_TYPE_I16, &it);
                while (err == ESP_OK && it != NULL) {
                    nvs_entry_info_t info;
                    nvs_entry_info(it, &info);
                    int16_t val = 0;
                    nvs_get_i16(src, info.key, &val);
                    nvs_set_i16(dst, info.key, val);
                    err = nvs_entry_next(&it);
                }
                if (it) nvs_release_iterator(it);
                nvs_commit(dst);
                nvs_close(dst);
            }
            nvs_close(src);
        }

        /* Erase the now-orphaned source slot */
        nvs_handle_t old;
        if (nvs_open(src_ns, NVS_READWRITE, &old) == ESP_OK) {
            nvs_erase_all(old);
            nvs_commit(old);
            nvs_close(old);
        }
    }

    /* Erase the last orphaned slot */
    char last_ns[16];
    chall_ns(new_count, last_ns, sizeof(last_ns));
    nvs_handle_t last;
    if (nvs_open(last_ns, NVS_READWRITE, &last) == ESP_OK) {
        nvs_erase_all(last);
        nvs_commit(last);
        nvs_close(last);
    }

    if (s_loaded_user > removed_idx) {
        s_loaded_user--;
    } else if (s_loaded_user == removed_idx) {
        s_loaded_user = -1;
    }

    ESP_LOGI(TAG, "challenge_shift_down: removed=%d new_count=%d", removed_idx, new_count);
}

void challenge_reset_user(int user_idx) {
    if (user_idx < 0 || user_idx >= MAX_USERS) return;
    char ns[16];
    chall_ns(user_idx, ns, sizeof(ns));
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    if (user_idx == s_loaded_user) s_cached_total = 0;
    ESP_LOGI(TAG, "User %d challenge data reset", user_idx);
}

int16_t challenge_total_medals(int user_idx) {
    if (user_idx < 0 || user_idx >= MAX_USERS) return 0;
    /* Return cached value for active user to avoid NVS round-trip */
    if (user_idx == s_loaded_user) return s_cached_total;

    char ns[16];
    chall_ns(user_idx, ns, sizeof(ns));
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return 0;
    int16_t total = 0;
    nvs_get_i16(h, "total", &total);
    nvs_close(h);
    return total;
}
