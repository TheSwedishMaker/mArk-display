/**
 * User store — NVS persistence for family member profiles.
 *
 * NVS namespace: "users"
 *   count  (u8)  — number of users
 *   active (u8)  — index of the active user
 *   n0..n5 (str) — user names
 */
#include "user_store.h"

#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG        = "USER_STORE";
static const char *NVS_NS     = "users";
static const char *KEY_COUNT  = "count";
static const char *KEY_ACTIVE = "active";

user_profile_t users[MAX_USERS] = {0};
int            user_count  = 1;
int            active_user = 0;

/* Build the NVS key for user N's name: "n0".."n5" */
static void name_key(int idx, char *buf, size_t len) {
    snprintf(buf, len, "n%d", idx);
}

void user_store_init(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);

    if (err != ESP_OK) {
        /* No namespace yet — first boot. Seed family members. */
        ESP_LOGI(TAG, "No user data found, seeding family users");
        user_count  = 5;
        active_user = 0;
        strncpy(users[0].name, "Pierre", MAX_USER_NAME_LEN - 1);
        strncpy(users[1].name, "Astrid", MAX_USER_NAME_LEN - 1);
        strncpy(users[2].name, "Ingrid", MAX_USER_NAME_LEN - 1);
        strncpy(users[3].name, "Julia",  MAX_USER_NAME_LEN - 1);
        strncpy(users[4].name, "Stina",  MAX_USER_NAME_LEN - 1);
        for (int i = 0; i < user_count; i++)
            users[i].name[MAX_USER_NAME_LEN - 1] = '\0';
        user_store_save();
        return;
    }

    uint8_t count = 1, active = 0;
    nvs_get_u8(h, KEY_COUNT,  &count);
    nvs_get_u8(h, KEY_ACTIVE, &active);

    if (count == 0 || count > MAX_USERS) count = 1;
    if (active >= count)                 active = 0;

    user_count  = count;
    active_user = active;

    char key[4];
    for (int i = 0; i < user_count; i++) {
        name_key(i, key, sizeof(key));
        size_t req = MAX_USER_NAME_LEN;
        if (nvs_get_str(h, key, users[i].name, &req) != ESP_OK) {
            snprintf(users[i].name, MAX_USER_NAME_LEN, "Person %d", i + 1);
        }
    }
    nvs_close(h);

    ESP_LOGI(TAG, "Loaded %d user(s), active=%d (%s)",
             user_count, active_user, users[active_user].name);
}

void user_store_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write");
        return;
    }

    nvs_set_u8(h, KEY_COUNT,  (uint8_t)user_count);
    nvs_set_u8(h, KEY_ACTIVE, (uint8_t)active_user);

    char key[4];
    for (int i = 0; i < user_count; i++) {
        name_key(i, key, sizeof(key));
        nvs_set_str(h, key, users[i].name);
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %d user(s), active=%d", user_count, active_user);
}

int user_store_add(const char *name) {
    if (user_count >= MAX_USERS) return -1;

    int idx = user_count;
    strncpy(users[idx].name, name, MAX_USER_NAME_LEN - 1);
    users[idx].name[MAX_USER_NAME_LEN - 1] = '\0';
    user_count++;

    ESP_LOGI(TAG, "Added user %d: %s", idx, users[idx].name);
    return idx;
}

void user_store_remove(int idx) {
    if (idx < 0 || idx >= user_count) return;

    /* Shift remaining users down */
    for (int i = idx; i < user_count - 1; i++) {
        users[i] = users[i + 1];
    }
    memset(&users[user_count - 1], 0, sizeof(user_profile_t));
    user_count--;

    if (user_count == 0) {
        /* Should not happen, but guard anyway */
        strncpy(users[0].name, "Person 1", MAX_USER_NAME_LEN - 1);
        user_count  = 1;
        active_user = 0;
        return;
    }

    if (active_user >= user_count) active_user = 0;
    ESP_LOGI(TAG, "Removed user %d, now %d user(s)", idx, user_count);
}

void user_store_rename(int idx, const char *name) {
    if (idx < 0 || idx >= user_count) return;
    strncpy(users[idx].name, name, MAX_USER_NAME_LEN - 1);
    users[idx].name[MAX_USER_NAME_LEN - 1] = '\0';
    ESP_LOGI(TAG, "Renamed user %d to %s", idx, users[idx].name);
}
