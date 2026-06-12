/**
 * Calendar fetcher — ESP-IDF
 * Iterates through all enabled cal_sources (Google + ICS).
 */
#include "calendar_fetch.h"
#include "user_store.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdlib.h>

static const char *TAG = "CAL";

/* ── Configuration ── */
#include "secrets.h"
#include "lang.h"

#define NTP_SERVER     "pool.ntp.org"
#define NVS_NAMESPACE  "cal_cfg"

/* ── State ── */
cal_task_t cal_tasks[MAX_TASKS];
int        cal_task_count = 0;
int        cal_day_offset = 0;

/* ── Staging buffer — fetch writes here, applied to cal_tasks[] under lock ── */
static cal_task_t s_stage[MAX_TASKS];
static int        s_stage_count = 0;
static bool       s_last_fetch_succeeded = false;

static bool s_wifi_connected = false;
static bool s_time_synced = false;
static volatile bool s_refresh_pending = false;

/* ── Per-user task cache — restores instantly on user switch ── */
static cal_task_t s_task_cache[MAX_USERS][MAX_TASKS];
static int        s_task_cache_count[MAX_USERS];
static bool       s_task_cache_valid[MAX_USERS];
static char       s_task_cache_date[MAX_USERS][9];  /* YYYYMMDD of cached day */

/* ── Completion persistence across refreshes — per-user ── */
#define MAX_COMPLETED_KEYS 20
#define COMP_KEY_LEN 128

typedef struct {
    char key[COMP_KEY_LEN];
} comp_key_t;

/* Per-user completion state: each user's completed task keys stored separately */
static comp_key_t s_completed_keys[MAX_USERS][MAX_COMPLETED_KEYS];
static int        s_completed_count[MAX_USERS];
static char       s_completed_date[MAX_USERS][9];  /* YYYYMMDD the keys were saved for */

/* When set, the next calendar_fetch() skips the internal save (used on user switch) */
static bool s_suppress_completion_save = false;

/* Snapshot of active_user taken at the start of calendar_fetch() — prevents
 * a concurrent user switch from corrupting completion state mid-fetch */
static int s_fetch_user = 0;

/* ── Manual tasks (NVS-backed, editable from web UI) — per user ── */
#define MANUAL_TASK_MAX 20

static void manual_ns_for_user(int idx, char *buf, size_t len) {
    snprintf(buf, len, "u%d_tsk", idx);
}

/* Shared save/load core — `key` is either a date "YYYYMMDD" or a weekday "w0".."w6" */
static void manual_tasks_save_key(int user_idx, const char *key, const cal_task_t *tasks, int count) {
    char ns[16];
    manual_ns_for_user(user_idx, ns, sizeof(ns));
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    if (count > MANUAL_TASK_MAX) count = MANUAL_TASK_MAX;

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "t", tasks[i].title);
        cJSON_AddStringToObject(obj, "m", tasks[i].time);
        cJSON_AddItemToArray(arr, obj);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (json) {
        nvs_set_str(h, key, json);
        free(json);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %d manual tasks for user %d key %s", count, user_idx, key);
}

/* Loads tasks but leaves dest[].id empty — callers stamp their own id format */
static int manual_tasks_load_key(int user_idx, const char *key, cal_task_t *dest, int max_count) {
    char ns[16];
    manual_ns_for_user(user_idx, ns, sizeof(ns));
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return 0;

    size_t json_len = 0;
    if (nvs_get_str(h, key, NULL, &json_len) != ESP_OK || json_len == 0) {
        nvs_close(h);
        return 0;
    }
    char *json = malloc(json_len);
    if (!json) { nvs_close(h); return 0; }
    nvs_get_str(h, key, json, &json_len);
    nvs_close(h);

    cJSON *arr = cJSON_Parse(json);
    free(json);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return 0; }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (count >= max_count) break;
        cJSON *jt = cJSON_GetObjectItem(item, "t");
        cJSON *jm = cJSON_GetObjectItem(item, "m");
        strncpy(dest[count].title, cJSON_IsString(jt) ? jt->valuestring : "", MAX_TITLE_LEN - 1);
        dest[count].title[MAX_TITLE_LEN - 1] = '\0';
        strncpy(dest[count].time, cJSON_IsString(jm) ? jm->valuestring : "", sizeof(dest[count].time) - 1);
        dest[count].time[sizeof(dest[count].time) - 1] = '\0';
        dest[count].id[0] = '\0';
        dest[count].completed = false;  /* completion applied later via was_completed() */
        count++;
    }
    cJSON_Delete(arr);
    return count;
}

/* Manual and weekly tasks keep an empty id: completion tracking then uses the
 * content-based "title__time" composite key in save_completion_state() /
 * was_completed(), which stays stable when the list is edited or reordered
 * (a positional index id would re-key every task after a deletion). */
void manual_tasks_save_user(int user_idx, const char *date8, const cal_task_t *tasks, int count) {
    manual_tasks_save_key(user_idx, date8, tasks, count);
}

int manual_tasks_load_user(int user_idx, const char *date8, cal_task_t *dest, int max_count) {
    return manual_tasks_load_key(user_idx, date8, dest, max_count);
}

void weekly_tasks_save_user(int user_idx, int wday, const cal_task_t *tasks, int count) {
    if (wday < 0 || wday > 6) return;
    char key[4];
    snprintf(key, sizeof(key), "w%d", wday);
    manual_tasks_save_key(user_idx, key, tasks, count);
}

int weekly_tasks_load_user(int user_idx, int wday, cal_task_t *dest, int max_count) {
    if (wday < 0 || wday > 6) return 0;
    char key[4];
    snprintf(key, sizeof(key), "w%d", wday);
    return manual_tasks_load_key(user_idx, key, dest, max_count);
}

static void save_completion_state(void) {
    int u = s_fetch_user;
    s_completed_count[u] = 0;

    /* Tag the saved state with today's date (adjusted for day offset) */
    time_t now = time(NULL);
    now += cal_day_offset * 86400;
    struct tm td;
    localtime_r(&now, &td);
    snprintf(s_completed_date[u], sizeof(s_completed_date[u]), "%04d%02d%02d",
             td.tm_year + 1900, td.tm_mon + 1, td.tm_mday);

    for (int i = 0; i < cal_task_count && s_completed_count[u] < MAX_COMPLETED_KEYS; i++) {
        if (!cal_tasks[i].completed) continue;
        if (cal_tasks[i].id[0] != '\0') {
            strncpy(s_completed_keys[u][s_completed_count[u]].key, cal_tasks[i].id, COMP_KEY_LEN - 1);
            s_completed_keys[u][s_completed_count[u]].key[COMP_KEY_LEN - 1] = '\0';
            s_completed_count[u]++;
        } else if (s_completed_count[u] < MAX_COMPLETED_KEYS) {
            snprintf(s_completed_keys[u][s_completed_count[u]].key, COMP_KEY_LEN,
                     "%s__%s", cal_tasks[i].title, cal_tasks[i].time);
            s_completed_count[u]++;
        }
    }
}

static bool was_completed(const cal_task_t *task) {
    int u = s_fetch_user;
    if (s_completed_count[u] == 0) return false;

    /* Reject keys saved for a different day — prevents yesterday's completions
     * bleeding into today after midnight rollover */
    time_t now = time(NULL);
    now += cal_day_offset * 86400;
    struct tm td;
    localtime_r(&now, &td);
    char fetch_date[9];
    snprintf(fetch_date, sizeof(fetch_date), "%04d%02d%02d",
             td.tm_year + 1900, td.tm_mon + 1, td.tm_mday);
    if (s_completed_date[u][0] != '\0' && strcmp(s_completed_date[u], fetch_date) != 0) {
        return false;  /* stale — saved for a different date */
    }

    for (int i = 0; i < s_completed_count[u]; i++) {
        if (strcmp(s_completed_keys[u][i].key, task->id) == 0) return true;
        char composite[COMP_KEY_LEN];
        snprintf(composite, COMP_KEY_LEN, "%s__%s", task->title, task->time);
        if (strcmp(s_completed_keys[u][i].key, composite) == 0) return true;
    }
    return false;
}

/* Public: save the current user's completion state (call before switching active_user) */
void calendar_save_completion_state(void) {
    s_fetch_user = (active_user >= 0 && active_user < MAX_USERS) ? active_user : 0;
    save_completion_state();
}

/* Public: sync live cal_tasks[] completion flags into the task cache — call after toggling a task */
void calendar_sync_completion_cache(void) {
    int u = (active_user >= 0 && active_user < MAX_USERS) ? active_user : 0;
    if (!s_task_cache_valid[u]) return;
    int n = cal_task_count < MAX_TASKS ? cal_task_count : MAX_TASKS;
    for (int i = 0; i < n; i++) {
        s_task_cache[u][i].completed = cal_tasks[i].completed;
    }
    s_task_cache_count[u] = n;
}

/* Public: tell the next calendar_fetch() not to overwrite the new user's completion slot */
void calendar_suppress_next_completion_save(void) {
    s_suppress_completion_save = true;
}

/* ── WiFi event handler ── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

bool wifi_init_and_connect(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    for (int i = 0; i < 40 && !s_wifi_connected; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (s_wifi_connected) {
        setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, (const char *)NTP_SERVER);
        esp_sntp_init();

        for (int i = 0; i < 20; i++) {
            time_t now = 0;
            time(&now);
            if (now > 1700000000) {
                s_time_synced = true;
                struct tm t;
                localtime_r(&now, &t);
                ESP_LOGI(TAG, "NTP: %04d-%02d-%02d %02d:%02d:%02d",
                         t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                         t.tm_hour, t.tm_min, t.tm_sec);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    return s_wifi_connected;
}

bool wifi_is_connected(void) { return s_wifi_connected; }

/* ── HTTP response buffer (for Google Calendar JSON) ── */
#define HTTP_BUF_SIZE 16384
static char http_buf[HTTP_BUF_SIZE];
static int  http_buf_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (http_buf_len + evt->data_len < HTTP_BUF_SIZE - 1) {
            memcpy(http_buf + http_buf_len, evt->data, evt->data_len);
            http_buf_len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ── Parse and strip [Name:N] challenge tag from task title ── */
static void parse_challenge_tag(cal_task_t *ct) {
    ct->challenge_series[0] = '\0';
    ct->challenge_target = 0;

    /* Find [Name:N] anywhere in the title */
    char *p = strchr(ct->title, '[');
    if (!p) return;

    char *close = strchr(p + 1, ']');
    if (!close) return;

    char *colon = NULL;
    for (char *c = p + 1; c < close; c++) {
        if (*c == ':') { colon = c; break; }
    }
    if (!colon) return;

    int name_len = (int)(colon - (p + 1));
    int num_len  = (int)(close - colon - 1);
    if (name_len <= 0 || name_len >= 24 || num_len <= 0 || num_len >= 8) return;

    char num_buf[8];
    memcpy(num_buf, colon + 1, num_len);
    num_buf[num_len] = '\0';
    int n = atoi(num_buf);
    if (n <= 0) return;

    memcpy(ct->challenge_series, p + 1, name_len);
    ct->challenge_series[name_len] = '\0';
    ct->challenge_target = (int16_t)n;

    /* Strip tag and any surrounding spaces from display title */
    char *tag_start = p;
    while (tag_start > ct->title && *(tag_start - 1) == ' ') tag_start--;
    char *rest = close + 1;
    while (*rest == ' ') rest++;
    memmove(tag_start, rest, strlen(rest) + 1);
}

/* ── Parse ISO8601 time → "HH:MM" ── */
static void parse_time(const char *iso, char *out, size_t len) {
    const char *t = strchr(iso, 'T');
    if (t && strlen(t) >= 6) {
        snprintf(out, len, "%.5s", t + 1);
    } else {
        snprintf(out, len, TR_ALL_DAY);
    }
}

typedef struct {
    char uid[64];
    char summary[MAX_TITLE_LEN];
    char time[16];
    bool all_day;
    bool has_start;
    bool has_end;
    time_t start_ts;
    time_t end_ts;
} ics_event_t;

static void ics_reset_event(ics_event_t *event) {
    memset(event, 0, sizeof(*event));
}

static bool ics_parse_datetime(const char *value, bool *all_day, time_t *out_ts, char *out_time, size_t out_time_len) {
    if (!value || !*value) return false;

    int year = 0, month = 0, day = 0;
    if (sscanf(value, "%4d%2d%2d", &year, &month, &day) != 3) {
        return false;
    }

    struct tm tm_val = {0};
    tm_val.tm_year = year - 1900;
    tm_val.tm_mon = month - 1;
    tm_val.tm_mday = day;

    const char *tpos = strchr(value, 'T');
    if (!tpos) {
        *all_day = true;
        if (out_time && out_time_len > 0) {
            snprintf(out_time, out_time_len, TR_ALL_DAY);
        }
        tm_val.tm_hour = 0;
        tm_val.tm_min = 0;
        tm_val.tm_sec = 0;
        *out_ts = mktime(&tm_val);
        return true;
    }

    int hour = 0, minute = 0, second = 0;
    if (sscanf(tpos + 1, "%2d%2d%2d", &hour, &minute, &second) < 2) {
        return false;
    }

    /* Check if time is UTC (ends with 'Z') */
    bool is_utc = false;
    const char *end = value + strlen(value) - 1;
    if (*end == 'Z') is_utc = true;

    *all_day = false;
    tm_val.tm_hour = hour;
    tm_val.tm_min = minute;
    tm_val.tm_sec = second;

    if (is_utc) {
        /* Convert UTC struct tm to time_t using timegm equivalent */
        /* Save/restore TZ to get UTC mktime */
        char *old_tz = getenv("TZ");
        char saved_tz[64] = {0};
        if (old_tz) {
            strncpy(saved_tz, old_tz, sizeof(saved_tz) - 1);
            saved_tz[sizeof(saved_tz) - 1] = '\0';
        }
        setenv("TZ", "UTC0", 1);
        tzset();
        *out_ts = mktime(&tm_val);
        if (old_tz) setenv("TZ", saved_tz, 1);
        else unsetenv("TZ");
        tzset();
        /* Convert UTC hour/minute to local for display */
        struct tm local_tm;
        localtime_r(out_ts, &local_tm);
        hour = local_tm.tm_hour;
        minute = local_tm.tm_min;
    } else {
        *out_ts = mktime(&tm_val);
    }

    if (out_time && out_time_len > 0) {
        snprintf(out_time, out_time_len, "%02d:%02d", hour, minute);
    }

    return true;
}

static bool ics_event_matches_day(const ics_event_t *event, time_t day_start, time_t day_end) {
    if (!event->has_start) return false;

    time_t event_end = event->has_end ? event->end_ts : event->start_ts + 60;
    if (event->all_day && event->has_end && event_end > event->start_ts) {
        return event->start_ts < day_end && event_end > day_start;
    }

    return event->start_ts < day_end && event_end > day_start;
}

static void ics_commit_event(const ics_event_t *event, time_t day_start, time_t day_end) {
    if (s_stage_count >= MAX_TASKS) return;
    if (!ics_event_matches_day(event, day_start, day_end)) return;
    if (event->summary[0] == '\0') return;

    cal_task_t *ct = &s_stage[s_stage_count];
    strncpy(ct->id, event->uid[0] ? event->uid : "ics-event", sizeof(ct->id) - 1);
    ct->id[sizeof(ct->id) - 1] = '\0';
    strncpy(ct->title, event->summary, MAX_TITLE_LEN - 1);
    ct->title[MAX_TITLE_LEN - 1] = '\0';
    strncpy(ct->time, event->time[0] ? event->time : TR_ALL_DAY, sizeof(ct->time) - 1);
    ct->time[sizeof(ct->time) - 1] = '\0';
    parse_challenge_tag(ct);
    ct->completed = was_completed(ct);
    s_stage_count++;
}

/* ── Fetch Google Calendar events for a specific calendar ID ── */
static bool fetch_google(const char *calendar_id, const char *time_min, const char *time_max) {
    /* URL-encode the calendar ID (replace @ with %40) */
    char encoded_id[128];
    int ei = 0;
    for (int i = 0; calendar_id[i] && ei < (int)sizeof(encoded_id) - 4; i++) {
        if (calendar_id[i] == '@') {
            encoded_id[ei++] = '%';
            encoded_id[ei++] = '4';
            encoded_id[ei++] = '0';
        } else {
            encoded_id[ei++] = calendar_id[i];
        }
    }
    encoded_id[ei] = '\0';

    char url[512];
    snprintf(url, sizeof(url),
             "https://www.googleapis.com/calendar/v3/calendars/%s/events"
             "?key=%s&timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime",
             encoded_id, GCAL_API_KEY, time_min, time_max);

    ESP_LOGI(TAG, "Google fetch: %s", url);

    http_buf_len = 0;
    memset(http_buf, 0, sizeof(http_buf));

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Google HTTP error: %s (status %d)", esp_err_to_name(err), status);
        return false;
    }

    http_buf[http_buf_len] = '\0';

    cJSON *doc = cJSON_Parse(http_buf);
    if (!doc) {
        ESP_LOGE(TAG, "Google JSON parse error");
        return false;
    }

    cJSON *items = cJSON_GetObjectItem(doc, "items");
    if (cJSON_IsArray(items)) {
        cJSON *item;
        cJSON_ArrayForEach(item, items) {
            if (s_stage_count >= MAX_TASKS) break;

            cal_task_t *ct = &s_stage[s_stage_count];

            cJSON *id = cJSON_GetObjectItem(item, "id");
            cJSON *summary = cJSON_GetObjectItem(item, "summary");
            cJSON *start = cJSON_GetObjectItem(item, "start");

            strncpy(ct->id, cJSON_IsString(id) ? id->valuestring : "unknown",
                    sizeof(ct->id) - 1);
            ct->id[sizeof(ct->id) - 1] = '\0';
            strncpy(ct->title, cJSON_IsString(summary) ? summary->valuestring : "Untitled",
                    MAX_TITLE_LEN - 1);
            ct->title[MAX_TITLE_LEN - 1] = '\0';
            parse_challenge_tag(ct);

            if (start) {
                cJSON *dt = cJSON_GetObjectItem(start, "dateTime");
                if (cJSON_IsString(dt)) {
                    parse_time(dt->valuestring, ct->time, sizeof(ct->time));
                } else {
                    snprintf(ct->time, sizeof(ct->time), TR_ALL_DAY);
                }
            }

            ct->completed = was_completed(ct);
            s_stage_count++;
        }
    }

    cJSON_Delete(doc);
    return true;
}

/* ── Streaming ICS parser state ── */
#define ICS_LINE_MAX 512

typedef struct {
    /* Line accumulation (handles folded lines) */
    char line_buf[ICS_LINE_MAX];
    int  line_len;
    bool prev_cr;          /* last char was \r */

    /* Event state */
    bool in_event;
    bool skip_event;       /* true if DTSTART is outside ±2 day window */
    ics_event_t current_event;

    /* Target day */
    time_t day_start;
    time_t day_end;
    int    added;
} ics_stream_t;

static ics_stream_t *s_ics_ctx = NULL;

static void ics_process_line(ics_stream_t *ctx) {
    char *line = ctx->line_buf;
    /* Remove trailing \r */
    int len = ctx->line_len;
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n'))
        line[--len] = '\0';
    ctx->line_len = len;

    if (len == 0) return;

    if (strncmp(line, "BEGIN:VEVENT", 12) == 0) {
        ctx->in_event = true;
        ctx->skip_event = false;
        ics_reset_event(&ctx->current_event);
    } else if (strncmp(line, "END:VEVENT", 10) == 0) {
        if (ctx->in_event && !ctx->skip_event && s_stage_count < MAX_TASKS) {
            ics_commit_event(&ctx->current_event, ctx->day_start, ctx->day_end);
            ctx->added++;
        }
        ctx->in_event = false;
        ctx->skip_event = false;
    } else if (ctx->in_event && !ctx->skip_event) {
        char *val = strchr(line, ':');
        if (val) {
            *val++ = '\0';

            if (strncmp(line, "DTSTART", 7) == 0) {
                ctx->current_event.has_start = ics_parse_datetime(
                    val,
                    &ctx->current_event.all_day,
                    &ctx->current_event.start_ts,
                    ctx->current_event.time,
                    sizeof(ctx->current_event.time)
                );
                /* Skip events that ended more than 1 day before target window */
                if (ctx->current_event.has_start &&
                    ctx->current_event.start_ts < (ctx->day_start - 86400)) {
                    ctx->skip_event = true;
                }
            } else if (!ctx->skip_event && strncmp(line, "SUMMARY", 7) == 0 && (line[7] == '\0' || line[7] == ';')) {
                strncpy(ctx->current_event.summary, val, MAX_TITLE_LEN - 1);
                ctx->current_event.summary[MAX_TITLE_LEN - 1] = '\0';
            } else if (!ctx->skip_event && strncmp(line, "UID", 3) == 0 && (line[3] == '\0' || line[3] == ';')) {
                strncpy(ctx->current_event.uid, val, sizeof(ctx->current_event.uid) - 1);
                ctx->current_event.uid[sizeof(ctx->current_event.uid) - 1] = '\0';
            } else if (!ctx->skip_event && strncmp(line, "DTEND", 5) == 0) {
                bool end_all_day = false;
                ctx->current_event.has_end = ics_parse_datetime(
                    val,
                    &end_all_day,
                    &ctx->current_event.end_ts,
                    NULL,
                    0
                );
                if (end_all_day) {
                    ctx->current_event.all_day = true;
                }
            }
        }
    }
}

static esp_err_t ics_stream_handler(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || !s_ics_ctx) return ESP_OK;

    ics_stream_t *ctx = s_ics_ctx;
    const char *data = (const char *)evt->data;
    int data_len = evt->data_len;

    for (int i = 0; i < data_len; i++) {
        char c = data[i];

        if (c == '\n') {
            /* End of raw line — but check next char for folding */
            ctx->line_buf[ctx->line_len] = '\0';
            ctx->prev_cr = false;
            /* We need to peek at next char for line folding.
               Store completed line, mark pending. */
            /* For streaming we can't peek ahead easily,
               so we use a flag: if next char is space/tab, it's a continuation */
            /* Actually, set a "line_complete" state and check on next char */
            /* Simplified: just process the line. If next chunk starts with
               space/tab, we'll handle it by appending. We mark line as ready. */
            ctx->line_buf[ctx->line_len] = '\0';
            /* Process only if not followed by continuation (handled below) */
            /* We'll process eagerly and re-append if continuation found */
            ics_process_line(ctx);
            ctx->line_len = 0;
            continue;
        }

        if (c == '\r') {
            ctx->prev_cr = true;
            continue;
        }

        /* Line folding: if at start of line and char is space/tab, append to previous */
        if (ctx->line_len == 0 && (c == ' ' || c == '\t')) {
            /* This is a continuation — unfortunately we already processed the line.
               For ICS feeds, continuation lines are rare for SUMMARY/DTSTART.
               Skip the folding whitespace and start accumulating into line_buf
               so the NEXT line-end will process it. This may lose the previous
               line's data, but for our use case (short fields) it's acceptable. */
            continue;
        }

        if (ctx->line_len < ICS_LINE_MAX - 1) {
            ctx->line_buf[ctx->line_len++] = c;
        }
    }

    return ESP_OK;
}

/* ── Fetch & parse ICS feed for target date (streaming) ── */
static bool fetch_ics(const char *ics_url, int year, int month, int day) {
    char url[512];
    if (strncmp(ics_url, "webcal://", 9) == 0) {
        snprintf(url, sizeof(url), "https://%s", ics_url + 9);
    } else if (strncmp(ics_url, "http://", 7) == 0) {
        snprintf(url, sizeof(url), "https://%s", ics_url + 7);
    } else {
        strncpy(url, ics_url, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    }

    /* Strip trailing whitespace */
    int len = strlen(url);
    while (len > 0 && (url[len-1] == ' ' || url[len-1] == '\n' || url[len-1] == '\r'))
        url[--len] = '\0';

    ESP_LOGI(TAG, "ICS fetch URL: [%s] (len=%d)", url, len);

    /* Set up streaming parser context */
    static ics_stream_t ics_ctx;
    memset(&ics_ctx, 0, sizeof(ics_ctx));

    struct tm day_tm = {0};
    day_tm.tm_year = year - 1900;
    day_tm.tm_mon = month - 1;
    day_tm.tm_mday = day;
    ics_ctx.day_start = mktime(&day_tm);
    ics_ctx.day_end = ics_ctx.day_start + 24 * 60 * 60;

    s_ics_ctx = &ics_ctx;
    int added_before = s_stage_count;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = ics_stream_handler,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Accept", "text/calendar");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");  /* prevent gzip — ESP can't decompress */
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    s_ics_ctx = NULL;

    /* Process any remaining line in buffer */
    if (ics_ctx.line_len > 0) {
        ics_ctx.line_buf[ics_ctx.line_len] = '\0';
        ics_process_line(&ics_ctx);
    }

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "ICS HTTP error: %s (status %d)", esp_err_to_name(err), status);
        return false;
    }

    ESP_LOGI(TAG, "ICS streaming parse done, added %d events for %04d%02d%02d",
             s_stage_count - added_before, year, month, day);
    return true;
}

/* ── Sort tasks by time ── */
static int task_time_cmp(const void *a, const void *b) {
    const cal_task_t *ta = (const cal_task_t *)a;
    const cal_task_t *tb = (const cal_task_t *)b;
    /* TR_ALL_DAY sorts first */
    bool a_allday = (strcmp(ta->time, TR_ALL_DAY) == 0 || ta->time[0] == '\0');
    bool b_allday = (strcmp(tb->time, TR_ALL_DAY) == 0 || tb->time[0] == '\0');
    if (a_allday && !b_allday) return -1;
    if (!a_allday && b_allday) return 1;
    return strcmp(ta->time, tb->time);
}

bool calendar_fetch(void) {
    /* Snapshot active_user once — prevents a concurrent user switch from
     * corrupting save_completion_state() / was_completed() mid-fetch */
    s_fetch_user = (active_user >= 0 && active_user < MAX_USERS) ? active_user : 0;

    if (!s_suppress_completion_save) {
        save_completion_state();
    }
    s_suppress_completion_save = false;

    if (!s_wifi_connected || !s_time_synced) { s_last_fetch_succeeded = false; return false; }

    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    t.tm_mday += cal_day_offset;
    mktime(&t);

    char time_min[64], time_max[64];
    snprintf(time_min, sizeof(time_min), "%04d-%02d-%02dT00:00:00Z",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    snprintf(time_max, sizeof(time_max), "%04d-%02d-%02dT23:59:59Z",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

    s_stage_count = 0;

    /* Merge NVS-backed manual + weekly tasks first, so user-authored tasks
     * are never crowded out of the MAX_TASKS cap by a full calendar day.
     * One buffer serves both loads — it is fully copied into s_stage before
     * being reused, and a second array would double this frame's stack cost
     * under the TLS fetches below. */
    {
        char date8[9];
        snprintf(date8, sizeof(date8), "%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        cal_task_t buf[MANUAL_TASK_MAX];
        int n = manual_tasks_load_user(s_fetch_user, date8, buf, MANUAL_TASK_MAX);
        for (int i = 0; i < n && s_stage_count < MAX_TASKS; i++) {
            buf[i].completed = was_completed(&buf[i]);
            s_stage[s_stage_count++] = buf[i];
        }

        /* Weekly recurring tasks for this weekday (tm_wday set by mktime above) */
        n = weekly_tasks_load_user(s_fetch_user, t.tm_wday, buf, MANUAL_TASK_MAX);
        for (int i = 0; i < n && s_stage_count < MAX_TASKS; i++) {
            buf[i].completed = was_completed(&buf[i]);
            s_stage[s_stage_count++] = buf[i];
        }
    }

    /* Iterate all enabled sources */
    for (int s = 0; s < cal_source_count; s++) {
        if (!cal_sources[s].enabled) continue;
        if (cal_sources[s].url[0] == '\0') continue;

        if (cal_sources[s].type == 0) {
            fetch_google(cal_sources[s].url, time_min, time_max);
        } else if (cal_sources[s].type == 1) {
            fetch_ics(cal_sources[s].url, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        }
    }

    if (s_stage_count >= MAX_TASKS)
        ESP_LOGW(TAG, "Task list full (%d) — some calendar events were dropped", MAX_TASKS);

    /* Sort by time */
    if (s_stage_count > 1) {
        qsort(s_stage, s_stage_count, sizeof(cal_task_t), task_time_cmp);
    }

    ESP_LOGI(TAG, "Fetch done: %d event(s) from %d source(s)", s_stage_count, cal_source_count);
    s_last_fetch_succeeded = true;
    return true;
}

/* ── Apply staged fetch results to live cal_tasks[] — call under LVGL lock ── */
void calendar_apply_staged(void) {
    int n = s_stage_count < MAX_TASKS ? s_stage_count : MAX_TASKS;

    /* Cache for instant user-switch restore — only results from a successful
     * fetch (a failed fetch must not overwrite a valid cache), and under the
     * user the fetch was actually FOR (s_fetch_user), not whoever is active
     * now. Tag with today's date so stale caches from a previous day are
     * rejected on restore. */
    if (s_last_fetch_succeeded && s_fetch_user >= 0 && s_fetch_user < MAX_USERS) {
        memcpy(s_task_cache[s_fetch_user], s_stage, n * sizeof(cal_task_t));
        s_task_cache_count[s_fetch_user] = n;
        s_task_cache_valid[s_fetch_user] = true;
        time_t now = time(NULL);
        now += cal_day_offset * 86400;
        struct tm ct;
        localtime_r(&now, &ct);
        snprintf(s_task_cache_date[s_fetch_user], sizeof(s_task_cache_date[0]),
                 "%04d%02d%02d", ct.tm_year + 1900, ct.tm_mon + 1, ct.tm_mday);

    }

    /* Only touch the live view if the fetched user is still active — a user
     * switch mid-fetch must not put another user's tasks on screen */
    if (s_fetch_user != active_user) {
        ESP_LOGW(TAG, "Fetch for user %d finished after switch to user %d — cached only",
                 s_fetch_user, active_user);
        return;
    }

    memcpy(cal_tasks, s_stage, n * sizeof(cal_task_t));
    cal_task_count = n;
}

/* Restore this user's cached tasks — returns false if no cache or cache is from a previous day */
bool calendar_restore_cached_tasks(int user_idx) {
    if (user_idx < 0 || user_idx >= MAX_USERS || !s_task_cache_valid[user_idx])
        return false;

    /* Reject cache if it was built for a different day */
    time_t now = time(NULL);
    now += cal_day_offset * 86400;
    struct tm ct;
    localtime_r(&now, &ct);
    char today[9];
    snprintf(today, sizeof(today), "%04d%02d%02d", ct.tm_year + 1900, ct.tm_mon + 1, ct.tm_mday);
    if (strcmp(s_task_cache_date[user_idx], today) != 0) {
        s_task_cache_valid[user_idx] = false;   /* stale — force fresh fetch */
        return false;
    }

    int n = s_task_cache_count[user_idx];
    memcpy(cal_tasks, s_task_cache[user_idx], n * sizeof(cal_task_t));
    cal_task_count = n;
    return true;
}

void calendar_set_offline_placeholder(void) {
    cal_task_count = 0;
}

int calendar_get_completed(void) {
    int n = 0;
    for (int i = 0; i < cal_task_count; i++)
        if (cal_tasks[i].completed) n++;
    return n;
}

int calendar_get_total(void) { return cal_task_count; }

void calendar_get_greeting(char *buf, size_t len) {
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_hour < 12) snprintf(buf, len, TR_GOOD_MORNING);
    else if (t.tm_hour < 17) snprintf(buf, len, TR_GOOD_AFTERNOON);
    else snprintf(buf, len, TR_GOOD_EVENING);
}

void calendar_get_date_str(char *buf, size_t len) {
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    const char *days[] = TR_DAY_NAMES;
    const char *months[] = TR_MONTH_NAMES;
    snprintf(buf, len, "%s, %s %d", days[t.tm_wday], months[t.tm_mon], t.tm_mday);
}

void calendar_get_day_label(char *buf, size_t len) {
    if (cal_day_offset == 0) snprintf(buf, len, TR_TODAY);
    else if (cal_day_offset == 1) snprintf(buf, len, TR_TOMORROW);
    else if (cal_day_offset == -1) snprintf(buf, len, TR_YESTERDAY);
    else {
        time_t now;
        time(&now);
        struct tm t;
        localtime_r(&now, &t);
        t.tm_mday += cal_day_offset;
        mktime(&t);
        strftime(buf, len, "%b %d", &t);
    }
}

/* ── NVS source persistence ── */

/* Build the NVS namespace for a user's calendar sources: "u0_cal".."u5_cal" */
static void cal_ns_for_user(int idx, char *buf, size_t len) {
    snprintf(buf, len, "u%d_cal", idx);
}

static void sources_save_to_ns(const char *ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_i32(h, "src_count", cal_source_count);
    for (int i = 0; i < cal_source_count; i++) {
        char ktype[24], kname[24], kurl[24], kenabled[24];
        snprintf(ktype,    sizeof(ktype),    "s%d_type", i);
        snprintf(kname,    sizeof(kname),    "s%d_name", i);
        snprintf(kurl,     sizeof(kurl),     "s%d_url",  i);
        snprintf(kenabled, sizeof(kenabled), "s%d_en",   i);

        nvs_set_i32(h, ktype,    cal_sources[i].type);
        nvs_set_str(h, kname,    cal_sources[i].name);
        nvs_set_str(h, kurl,     cal_sources[i].url);
        nvs_set_i32(h, kenabled, cal_sources[i].enabled ? 1 : 0);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %d sources to '%s'", cal_source_count, ns);
}

/* Returns true if sources were loaded, false if namespace was empty/missing */
static bool sources_load_from_ns(const char *ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;

    int32_t count = 0;
    if (nvs_get_i32(h, "src_count", &count) != ESP_OK || count <= 0) {
        nvs_close(h);
        return false;
    }
    if (count > MAX_CAL_SOURCES) count = MAX_CAL_SOURCES;

    for (int i = 0; i < count; i++) {
        char ktype[24], kname[24], kurl[24], kenabled[24];
        snprintf(ktype,    sizeof(ktype),    "s%d_type", i);
        snprintf(kname,    sizeof(kname),    "s%d_name", i);
        snprintf(kurl,     sizeof(kurl),     "s%d_url",  i);
        snprintf(kenabled, sizeof(kenabled), "s%d_en",   i);

        int32_t type_val = 0, en_val = 1;
        nvs_get_i32(h, ktype, &type_val);
        cal_sources[i].type = type_val;

        size_t nlen = sizeof(cal_sources[i].name);
        if (nvs_get_str(h, kname, cal_sources[i].name, &nlen) != ESP_OK)
            snprintf(cal_sources[i].name, sizeof(cal_sources[i].name), "Calendar");

        size_t ulen = CAL_URL_MAX;
        if (nvs_get_str(h, kurl, cal_sources[i].url, &ulen) != ESP_OK)
            cal_sources[i].url[0] = '\0';

        nvs_get_i32(h, kenabled, &en_val);
        cal_sources[i].enabled = (en_val != 0);
    }
    cal_source_count = count;
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d sources from '%s'", cal_source_count, ns);
    return true;
}

void calendar_sources_save_user(int user_idx) {
    char ns[16];
    cal_ns_for_user(user_idx, ns, sizeof(ns));
    sources_save_to_ns(ns);
}

void calendar_sources_load_user(int user_idx) {
    char ns[16];
    cal_ns_for_user(user_idx, ns, sizeof(ns));

    /* Clear first so a missing namespace yields 0 sources, not the previous user's */
    cal_source_count = 0;

    if (!sources_load_from_ns(ns)) {
        /* User 0: fall back to legacy "cal_cfg" namespace on first run */
        if (user_idx == 0 && sources_load_from_ns(NVS_NAMESPACE)) {
            ESP_LOGI(TAG, "User 0: migrated sources from legacy namespace");
        } else {
            ESP_LOGI(TAG, "User %d: no saved sources, using defaults", user_idx);
        }
    }
}

/* Active-user wrappers — used everywhere else in the codebase */
void calendar_sources_save(void) {
    calendar_sources_save_user(active_user);
}

void calendar_sources_load(void) {
    calendar_sources_load_user(active_user);
}

int calendar_sources_read_user(int user_idx, cal_source_t *dest, int max_count) {
    /* Swap globals, load, copy out, restore — safe under LVGL lock */
    cal_source_t saved[MAX_CAL_SOURCES];
    int saved_count = cal_source_count;
    memcpy(saved, cal_sources, sizeof(cal_source_t) * cal_source_count);

    /* Reset before loading so a missing namespace yields 0 sources, not the previous user's */
    cal_source_count = 0;
    calendar_sources_load_user(user_idx);
    int result = cal_source_count < max_count ? cal_source_count : max_count;
    memcpy(dest, cal_sources, sizeof(cal_source_t) * result);

    cal_source_count = saved_count;
    memcpy(cal_sources, saved, sizeof(cal_source_t) * saved_count);
    return result;
}

void calendar_sources_write_user(int user_idx, const cal_source_t *src, int count) {
    /* Swap globals, save, restore — safe under LVGL lock */
    cal_source_t saved[MAX_CAL_SOURCES];
    int saved_count = cal_source_count;
    memcpy(saved, cal_sources, sizeof(cal_source_t) * cal_source_count);

    cal_source_count = count;
    memcpy(cal_sources, src, sizeof(cal_source_t) * count);
    calendar_sources_save_user(user_idx);

    cal_source_count = saved_count;
    memcpy(cal_sources, saved, sizeof(cal_source_t) * saved_count);
}

/* ── Deferred refresh flag ── */

void calendar_request_refresh(void) {
    s_refresh_pending = true;
}

bool calendar_refresh_pending(void) {
    if (s_refresh_pending) {
        s_refresh_pending = false;
        return true;
    }
    return false;
}
