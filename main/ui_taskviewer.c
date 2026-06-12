/**
 * Task Viewer LVGL UI — 1024×600, Dieter Rams aesthetic
 * Muted palette with orange accents. Matches web preview.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "ui_taskviewer.h"
#include "calendar_fetch.h"
#include "streak_store.h"
#include "lang.h"
#include "challenge_store.h"
#include "img_gold_medal.h"
#include "sound_driver.h"
#include "ws2812_led.h"
#include "lvgl.h"
#include "esp_log.h"
#include "web_config.h"
#include "user_store.h"
#include "bsp_illuminate.h"
#include "timer_engine.h"


static const char *TAG = "UI";

/* Full UI fonts with Swedish characters */
LV_FONT_DECLARE(lv_font_ui_14);
LV_FONT_DECLARE(lv_font_ui_24);
LV_FONT_DECLARE(lv_font_ui_48);

/* Use LVGL built-in fonts for symbols/icons to avoid square glyphs */
static inline const lv_font_t *icon_font_sm(void) { return &lv_font_montserrat_14; }
static inline const lv_font_t *icon_font_md(void) { return &lv_font_montserrat_28; }
static inline const lv_font_t *icon_font_lg(void) { return &lv_font_montserrat_48; }

/* Forward declarations */
static void show_settings(void);
static void rebuild_user_bar(void);
static void settings_ensure_overlay(void);
static void settings_populate_user_list(void);
static void settings_populate_source_editor(int user_idx);
static void hide_settings(void);

/* ── Colors: Dieter Rams / warm neutral palette ── */
#define C_BG         lv_color_hex(0xF5F0E8)   /* warm off-white */
#define C_FG         lv_color_hex(0x2C2C2C)   /* near-black */
#define C_MUTED      lv_color_hex(0x8A8A7A)   /* warm gray */
#define C_ACCENT     lv_color_hex(0xE8742A)   /* Rams orange */
#define C_SIDEBAR    lv_color_hex(0xCECBC4)   /* cooler gray panel — darkened for contrast */
#define C_CARD       lv_color_hex(0xBCB9B2)   /* cooler gray card — darkened for contrast */
#define C_BORDER     lv_color_hex(0xA8A49C)   /* neutral border */
#define C_DARK_BG    lv_color_hex(0x1E1E1E)
#define C_DARK_TEXT  lv_color_hex(0xE8E0D8)
#define C_DARK_MUTED lv_color_hex(0x9CA3AF)
#define C_DARK_SIDEBAR lv_color_hex(0x262626)
#define C_DARK_CARD  lv_color_hex(0x333333)
#define C_DARK_BORDER lv_color_hex(0x444444)
#define C_WHITE      lv_color_hex(0xFFFFFF)
#define C_TRACK      lv_color_hex(0xD5CFC5)
#define C_DARK_TRACK lv_color_hex(0x444444)

/* ── Layout ── */
#define SCREEN_W   1024
#define SCREEN_H   600
#define SIDEBAR_W  320
#define MAIN_W     (SCREEN_W - SIDEBAR_W)

/* ── Bezel overlay — matches physical case opening ── */
#define BEZEL_BW      32   /* border-width = top coverage px */
#define BEZEL_LEFT    25   /* ~5mm left side */
#define BEZEL_RIGHT   16   /* 3.5mm × (1024/222) */
#define BEZEL_BOTTOM   8   /* thin bottom strip for rounded corners */
#define BEZEL_INNER_R  5   /* inner corner radius px */

/* Visible panel dimensions after bezels */
#define PANEL_H  (SCREEN_H - BEZEL_BW - BEZEL_BOTTOM)
#define PANEL_W  (SCREEN_W - BEZEL_LEFT - BEZEL_RIGHT)
/* Inner padding used by overlay views (keyboard + settings) */
#define ST_PAD   16

/* ── UI state (must be before inline helpers) ── */
static int ui_current = 0;
static int ui_completed = 0;
static bool ui_dark_mode = false;
static bool ui_showing_complete = false;
static bool ui_leds_suppressed = false;  /* LEDs off after day complete */
static bool ui_showing_keyboard = false;
static bool ui_showing_settings = false;

/* ── User bar — one button per user across the top of the right panel ── */
static lv_obj_t *user_bar_btns[MAX_USERS]       = {0};
static lv_obj_t *user_bar_medal_imgs[MAX_USERS] = {0};
static lv_obj_t *user_bar_medal_lbls[MAX_USERS] = {0};

/* ── Settings state ── */
static lv_obj_t *settings_overlay = NULL;
static lv_obj_t *settings_panel   = NULL;
static int settings_viewing_user = -1;  /* -1=user list, >=0=source editor for that user */
static int settings_renaming_user = -1;

/* ── Theme-aware color helpers ── */
static inline lv_color_t th_bg(void)      { return ui_dark_mode ? C_DARK_BG : C_BG; }
static inline lv_color_t th_fg(void)      { return ui_dark_mode ? C_DARK_TEXT : C_FG; }
static inline lv_color_t th_muted(void)   { return ui_dark_mode ? C_DARK_MUTED : C_MUTED; }
static inline lv_color_t th_sidebar(void) { return ui_dark_mode ? C_DARK_SIDEBAR : C_SIDEBAR; }
static inline lv_color_t th_card(void)    { return ui_dark_mode ? C_DARK_CARD : C_CARD; }
static inline lv_color_t th_border(void)  { return ui_dark_mode ? C_DARK_BORDER : C_BORDER; }
static inline lv_color_t th_track(void)   { return ui_dark_mode ? C_DARK_TRACK : C_TRACK; }

/* ── Multi-calendar source list (type defined in calendar_fetch.h) ── */
cal_source_t cal_sources[MAX_CAL_SOURCES] = {
    { .type = 0, .name = "Google Calendar", .url = "", .enabled = true },
};
int cal_source_count = 1;

/* ── On-screen keyboard state ── */
#define KB_MAX_INPUT 512
static char kb_input[KB_MAX_INPUT];
static int  kb_input_len = 0;
static bool kb_shift = true;
static int  kb_mode = 0;  /* 0=lower, 1=upper, 2=numeric */
static lv_obj_t *kb_overlay = NULL;
static lv_obj_t *kb_lbl_input = NULL;

/* Keyboard submit callback — NULL means "add local task" (default) */
typedef void (*kb_submit_fn)(const char *text);
static kb_submit_fn kb_on_submit = NULL;

/* ── UI elements ── */
static lv_obj_t *lbl_greeting, *lbl_date;
static lv_obj_t *lbl_streak_num, *lbl_streak_label;
static lv_obj_t *lbl_level_name, *lbl_level_days, *lbl_level_next;
static lv_obj_t *arc_progress, *lbl_arc_num, *lbl_arc_total;
static lv_obj_t *bar_xp;
static lv_obj_t *lbl_task_counter, *lbl_task_time, *lbl_task_title;
static lv_obj_t *badge_completed;
static lv_obj_t *img_challenge_medal_sm = NULL;   /* 32×32 medal icon on task card */
static lv_obj_t *lbl_challenge_progress = NULL;   /* "3/8" or "×2 5/8" */
static lv_obj_t *btn_complete, *lbl_btn_complete;
static lv_obj_t *dots_container;
static lv_obj_t *lbl_volume_icon = NULL;
static lv_obj_t *s_mp = NULL;
static lv_obj_t *lbl_clock = NULL;
static lv_obj_t *lbl_dark_btn = NULL;
static lv_obj_t *btn_refresh_obj = NULL;
static lv_obj_t *complete_screen = NULL;
static lv_obj_t *leaderboard_overlay = NULL;
static lv_obj_t *sleep_overlay = NULL;
static bool      display_sleeping = false;

/* ── Timer button (on task card) ── */
static lv_obj_t *btn_timer     = NULL;
static lv_obj_t *lbl_btn_timer = NULL;

/* ── Timer popup state ── */
static lv_obj_t   *s_timer_popup           = NULL;   /* full-screen overlay */
static lv_obj_t   *s_timer_popup_arc       = NULL;
static lv_obj_t   *s_timer_popup_lbl       = NULL;   /* mm:ss or "Stopp" */
static lv_obj_t   *s_timer_popup_pause_lbl = NULL;   /* pause/play button label */
static lv_timer_t *s_timer_popup_lv_timer  = NULL;   /* 250 ms update tick */

/* ── Forward declarations ── */
static void refresh_task(void);
static void refresh_progress(void);
static void refresh_dots(void);
static void refresh_sidebar(void);
static void show_complete_screen(void);
static void user_bar_refresh(void);
static void show_leaderboard(void);
static void hide_complete_screen(void);
static void cb_clock_tick(lv_timer_t *t);
static void close_timer_popup(void);
static void show_timer_popup(void);
static void on_timer_expired(void *arg);

/* ── Callbacks for completion screen (C doesn't have lambdas) ── */
static void cb_back_to_tasks(lv_event_t *e) {
    (void)e;
    hide_complete_screen();
}

static void cb_auto_dismiss(lv_timer_t *t) {
    hide_complete_screen();
    lv_timer_del(t);
}

/* ── Callbacks ── */
static void cb_complete(lv_event_t *e) {
    if (cal_task_count <= 0) return;
    bool was = cal_tasks[ui_current].completed;
    cal_tasks[ui_current].completed = !was;
    calendar_sync_completion_cache();

    /* Challenge series tracking */
    cal_task_t *t = &cal_tasks[ui_current];
    if (t->challenge_target > 0) {
        if (!was) {
            challenge_complete(t->challenge_series, t->challenge_target);
        } else {
            challenge_uncomplete(t->challenge_series);
        }
    }

    ui_completed = calendar_get_completed();

    if (ui_completed == cal_task_count && cal_task_count > 0) {
        /* Show full progress on LEDs, then suppress after day complete dismisses */
        ws2812_update_progress(ui_completed, cal_task_count);
        streak_mark_day_complete(cal_day_offset);
        sound_day_complete();
        ui_leds_suppressed = true;
        show_complete_screen();
    } else {
        /* Reset LED suppression when not all complete */
        ui_leds_suppressed = false;
        ws2812_update_progress(ui_completed, cal_task_count);
        if (!was) {
            sound_task_complete();
        }
    }

    refresh_task();
    refresh_progress();
    refresh_dots();
    refresh_sidebar();
    if (t->challenge_target > 0) user_bar_refresh();
}

static void cb_next(lv_event_t *e) {
    if (cal_task_count <= 0) return;
    ui_current = (ui_current + 1) % cal_task_count;
    refresh_task();
    refresh_dots();
}

static void cb_prev(lv_event_t *e) {
    if (cal_task_count <= 0) return;
    ui_current = (ui_current - 1 + cal_task_count) % cal_task_count;
    refresh_task();
    refresh_dots();
}

static void cb_gesture(lv_event_t *e) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT) cb_next(e);
    else if (dir == LV_DIR_RIGHT) cb_prev(e);
}

/* Dark mode toggle — rebuild entire UI with new theme */
static void show_leaderboard(void);
static void hide_leaderboard(void);

static void cb_toggle_dark(lv_event_t *e) {
    (void)e;
    ui_dark_mode = !ui_dark_mode;
    /* Clean and rebuild to apply all themed colors */
    lv_obj_clean(lv_scr_act());
    complete_screen = NULL;
    kb_overlay = NULL;
    kb_lbl_input = NULL;
    settings_overlay = NULL;
    leaderboard_overlay = NULL;
    sleep_overlay = NULL;
    for (int i = 0; i < MAX_USERS; i++) user_bar_btns[i] = NULL;
    ui_build();
}

/* ── Display sleep / wake ── */
static void cb_wake_display(lv_event_t *e) {
    (void)e;
    if (!display_sleeping) return;
    display_sleeping = false;
    set_lcd_blight(100);
    if (sleep_overlay) {
        lv_obj_del(sleep_overlay);
        sleep_overlay = NULL;
    }
    /* Restore LEDs to match current progress */
    if (!ui_leds_suppressed) {
        ws2812_update_progress(ui_completed, cal_task_count);
    }
    /* Trigger a refresh on wake — catches midnight rollover if overnight fetch failed */
    calendar_request_refresh();
}

/* ── Volume popup ── */
static void cb_volume_slider(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int val = (int)lv_slider_get_value(slider);
    sound_set_volume(val);
    if (lbl_volume_icon) {
        const char *sym = (val == 0)  ? LV_SYMBOL_MUTE :
                          (val < 50)  ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX;
        lv_label_set_text(lbl_volume_icon, sym);
    }
}

static void cb_volume_overlay_close(lv_event_t *e) {
    lv_obj_t *overlay = lv_event_get_target(e);
    lv_obj_del(overlay);
}

static void cb_volume_btn(lv_event_t *e) {
    (void)e;
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_add_event_cb(overlay, cb_volume_overlay_close, LV_EVENT_CLICKED, NULL);

    /* Modal card — stays clickable so touches on it don't reach the overlay */
    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 340, 110);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, th_card(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, TR_VOLUME);
    lv_obj_set_style_text_color(title, th_fg(), 0);
    lv_obj_set_style_text_font(title, &lv_font_ui_14, 0);
    lv_obj_set_pos(title, 24, 16);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_size(slider, 292, 8);
    lv_obj_set_pos(slider, 24, 56);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, sound_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, th_track(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, C_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 12, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, cb_volume_slider, LV_EVENT_VALUE_CHANGED, NULL);
}

static void cb_sleep_display(lv_event_t *e) {
    (void)e;
    if (display_sleeping) return;
    display_sleeping = true;
    /* Full-screen overlay absorbs all touches until woken */
    sleep_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sleep_overlay);
    lv_obj_set_size(sleep_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(sleep_overlay, 0, 0);
    lv_obj_set_style_bg_opa(sleep_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(sleep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sleep_overlay, cb_wake_display, LV_EVENT_CLICKED, NULL);
    set_lcd_blight(0);
    ws2812_off();
}

/* ── On-screen keyboard ── */
static void show_keyboard(void);
static void hide_keyboard(void);

static const char *kb_rows_lower[] = {
    "q","w","e","r","t","y","u","i","o","p","\xc3\xa5",NULL,
    "a","s","d","f","g","h","j","k","l","\xc3\xa4","\xc3\xb6",NULL,
    "\x01","z","x","c","v","b","n","m","\x02",NULL,  /* \x01=shift, \x02=backspace */
    "\x03"," ","\x04",NULL,  /* \x03=123, \x04=submit */
};
static const char *kb_rows_upper[] = {
    "Q","W","E","R","T","Y","U","I","O","P","\xc3\x85",NULL,
    "A","S","D","F","G","H","J","K","L","\xc3\x84","\xc3\x96",NULL,
    "\x01","Z","X","C","V","B","N","M","\x02",NULL,
    "\x03"," ","\x04",NULL,
};
static const char *kb_rows_num[] = {
    "1","2","3","4","5","6","7","8","9","0",NULL,
    "-","/",":",";","(",")","&","@","\"",NULL,
    ".","?","!","'",",","\x02",NULL,
    "\x05"," ","\x04",NULL,  /* \x05=abc */
};

static void kb_update_display(void) {
    if (!kb_lbl_input) return;
    if (kb_input_len == 0) {
        lv_label_set_text(kb_lbl_input, TR_TASK_NAME_PLACEHOLDER);
        lv_obj_set_style_text_color(kb_lbl_input, C_MUTED, 0);
    } else {
        lv_label_set_text(kb_lbl_input, kb_input);
        lv_obj_set_style_text_color(kb_lbl_input, C_FG, 0);
    }
}

static void kb_add_local_task(void) {
    if (kb_input_len == 0) return;

    /* Persist to NVS so the task survives a reboot */
    time_t now = time(NULL);
    struct tm tinfo;
    localtime_r(&now, &tinfo);
    char date8[9];
    snprintf(date8, sizeof(date8), "%04d%02d%02d",
             tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday);

    cal_task_t manual[20];
    int mc = manual_tasks_load_user(active_user, date8, manual, 20);
    if (mc < 20) {
        strncpy(manual[mc].title, kb_input, MAX_TITLE_LEN - 1);
        manual[mc].title[MAX_TITLE_LEN - 1] = '\0';
        manual[mc].time[0] = '\0';
        manual[mc].completed = false;
        mc++;
    }
    manual_tasks_save_user(active_user, date8, manual, mc);

    /* Add to live task list for immediate display — empty id, same as
     * manual_tasks_load_user, so completion tracking uses the stable
     * title__time composite key and survives the next fetch */
    if (cal_task_count < MAX_TASKS) {
        cal_task_t *ct = &cal_tasks[cal_task_count];
        ct->id[0] = '\0';
        strncpy(ct->title, kb_input, MAX_TITLE_LEN - 1);
        ct->title[MAX_TITLE_LEN - 1] = '\0';
        ct->time[0] = '\0';
        ct->completed = false;
        cal_task_count++;

        /* If we were showing "Inga uppgifter" placeholder, remove it */
        if (cal_task_count == 2 && strcmp(cal_tasks[0].title, "Inga uppgifter") == 0) {
            cal_tasks[0] = cal_tasks[1];
            cal_task_count = 1;
        }

        ui_refresh_all();
    }
    hide_keyboard();
}

static void cb_kb_key(lv_event_t *e) {
    const char *key = (const char *)lv_event_get_user_data(e);

    if (key[0] == '\x01') {
        /* Shift toggle */
        kb_shift = !kb_shift;
        if (kb_mode != 2) kb_mode = kb_shift ? 1 : 0;
        /* Rebuild keyboard */
        hide_keyboard();
        show_keyboard();
    } else if (key[0] == '\x02') {
        /* Backspace — handle multi-byte UTF-8 */
        if (kb_input_len > 0) {
            /* Walk back over continuation bytes (10xxxxxx) */
            do { kb_input_len--; } while (kb_input_len > 0 && (kb_input[kb_input_len] & 0xC0) == 0x80);
            kb_input[kb_input_len] = '\0';
            kb_update_display();
        }
    } else if (key[0] == '\x03') {
        /* 123 mode */
        kb_mode = 2;
        hide_keyboard();
        show_keyboard();
    } else if (key[0] == '\x04') {
        /* Submit */
        if (kb_on_submit) {
            kb_on_submit(kb_input);
            hide_keyboard();
        } else {
            kb_add_local_task();
        }
    } else if (key[0] == '\x05') {
        /* abc mode */
        kb_mode = kb_shift ? 1 : 0;
        hide_keyboard();
        show_keyboard();
    } else if (key[0] == ' ') {
        if (kb_input_len < KB_MAX_INPUT - 1) {
            kb_input[kb_input_len++] = ' ';
            kb_input[kb_input_len] = '\0';
            kb_update_display();
        }
    } else {
        /* Regular character (may be multi-byte UTF-8, e.g. å ä ö) */
        int klen = strlen(key);
        if (kb_input_len + klen < KB_MAX_INPUT - 1) {
            memcpy(kb_input + kb_input_len, key, klen);
            kb_input_len += klen;
            kb_input[kb_input_len] = '\0';
            if (kb_shift && kb_mode != 2) {
                kb_shift = false;
                kb_mode = 0;
            }
            kb_update_display();
        }
    }
}

static void cb_kb_close(lv_event_t *e) {
    (void)e;
    hide_keyboard();
    /* If settings was open when keyboard appeared, restore the overlay */
    if (ui_showing_settings) {
        settings_ensure_overlay();
        if (settings_viewing_user >= 0) {
            settings_populate_source_editor(settings_viewing_user);
        } else {
            settings_populate_user_list();
        }
    }
}

static void cb_add_task(lv_event_t *e) {
    (void)e;
    kb_input[0] = '\0';
    kb_input_len = 0;
    kb_shift = true;
    kb_mode = 1;  /* Start uppercase */
    kb_on_submit = NULL;  /* default: add local task */
    show_keyboard();
}

static void show_keyboard(void) {
    if (kb_overlay) {
        lv_obj_del(kb_overlay);
        kb_overlay = NULL;
    }
    ui_showing_keyboard = true;

    lv_obj_t *scr = lv_scr_act();
    kb_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(kb_overlay);
    lv_obj_set_size(kb_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(kb_overlay, 0, 0);
    lv_obj_set_style_bg_color(kb_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(kb_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(kb_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Inner content panel — same bezel geometry as the main view */
    lv_obj_t *kb_panel = lv_obj_create(kb_overlay);
    lv_obj_remove_style_all(kb_panel);
    lv_obj_set_size(kb_panel, PANEL_W, PANEL_H);
    lv_obj_set_pos(kb_panel, BEZEL_LEFT, BEZEL_BW);
    lv_obj_set_style_bg_color(kb_panel, th_bg(), 0);
    lv_obj_set_style_bg_opa(kb_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(kb_panel, BEZEL_INNER_R, 0);
    lv_obj_clear_flag(kb_panel, LV_OBJ_FLAG_SCROLLABLE);

    #define KB_PAD   16   /* inner padding within panel */
    #define KB_GAP    5
    #define KB_TOP_H 88   /* reserved height for close btn + input (pad + 60px + gap) */

    /* Close button — top-left of inner panel, fully visible below bezel */
    lv_obj_t *btn_close = lv_btn_create(kb_panel);
    lv_obj_set_size(btn_close, 52, 60);
    lv_obj_set_pos(btn_close, KB_PAD, KB_PAD);
    lv_obj_set_style_bg_color(btn_close, th_card(), 0);
    lv_obj_set_style_radius(btn_close, 8, 0);
    lv_obj_set_style_shadow_width(btn_close, 0, 0);
    lv_obj_add_event_cb(btn_close, cb_kb_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(btn_close);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, th_muted(), 0);
    lv_obj_set_style_text_font(xl, icon_font_sm(), 0);
    lv_obj_center(xl);

    /* Input display — tall prominent text box */
    lv_obj_t *input_box = lv_obj_create(kb_panel);
    lv_obj_remove_style_all(input_box);
    lv_obj_set_size(input_box, PANEL_W - KB_PAD * 2 - 60, 60);
    lv_obj_set_pos(input_box, KB_PAD + 60, KB_PAD);
    lv_obj_set_style_bg_color(input_box, th_card(), 0);
    lv_obj_set_style_bg_opa(input_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(input_box, 10, 0);
    lv_obj_set_style_border_width(input_box, 2, 0);
    lv_obj_set_style_border_color(input_box, th_border(), 0);
    lv_obj_clear_flag(input_box, LV_OBJ_FLAG_SCROLLABLE);

    kb_lbl_input = lv_label_create(input_box);
    lv_obj_set_width(kb_lbl_input, PANEL_W - KB_PAD * 2 - 60 - 32);
    lv_label_set_long_mode(kb_lbl_input, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(kb_lbl_input, &lv_font_ui_24, 0);
    lv_obj_align(kb_lbl_input, LV_ALIGN_LEFT_MID, 16, 0);
    kb_update_display();

    /* Keyboard rows */
    const char **rows;
    if (kb_mode == 2) rows = kb_rows_num;
    else if (kb_mode == 1) rows = kb_rows_upper;
    else rows = kb_rows_lower;

    int content_w = PANEL_W - KB_PAD * 2;
    int row_idx = 0;
    int ki = 0;
    int key_h = (PANEL_H - KB_TOP_H - KB_PAD) / 4 - KB_GAP;
    if (key_h > 70) key_h = 70;

    /* Vertically center the key grid in the space below the input area */
    int grid_h = key_h * 4 + KB_GAP * 3;
    int avail_h = PANEL_H - KB_TOP_H - KB_PAD;
    int y = KB_TOP_H + (avail_h - grid_h) / 2;

    while (row_idx < 4) {
        int row_start = ki;
        int row_len = 0;
        while (rows[ki] != NULL) { ki++; row_len++; }
        ki++;  /* skip NULL sentinel */

        int total_gap = KB_GAP * (row_len - 1);

        int x = KB_PAD;
        for (int k = 0; k < row_len; k++) {
            const char *key = rows[row_start + k];
            int key_w;

            if (key[0] == ' ') {
                key_w = (content_w * 3) / (row_len + 3);
            } else if (key[0] == '\x01' || key[0] == '\x02' || key[0] == '\x03' ||
                       key[0] == '\x04' || key[0] == '\x05') {
                key_w = (content_w * 3) / (2 * (row_len + 3));
            } else {
                key_w = (content_w - total_gap) / row_len;
            }

            if (x + key_w > PANEL_W - KB_PAD) {
                key_w = PANEL_W - KB_PAD - x;
            }

            lv_obj_t *btn = lv_btn_create(kb_panel);
            lv_obj_set_size(btn, key_w, key_h);
            lv_obj_set_pos(btn, x, y);
            lv_obj_set_style_radius(btn, 8, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);

            if (key[0] == '\x04') {
                lv_obj_set_style_bg_color(btn, C_ACCENT, 0);
            } else if (key[0] == '\x01' || key[0] == '\x02') {
                lv_obj_set_style_bg_color(btn, th_border(), 0);
            } else {
                lv_obj_set_style_bg_color(btn, th_card(), 0);
            }

            lv_obj_add_event_cb(btn, cb_kb_key, LV_EVENT_CLICKED, (void *)key);

            lv_obj_t *lbl = lv_label_create(btn);
            if (key[0] == '\x01') lv_label_set_text(lbl, LV_SYMBOL_UP);
            else if (key[0] == '\x02') lv_label_set_text(lbl, LV_SYMBOL_BACKSPACE);
            else if (key[0] == '\x03') lv_label_set_text(lbl, "123");
            else if (key[0] == '\x04') lv_label_set_text(lbl, LV_SYMBOL_OK);
            else if (key[0] == '\x05') lv_label_set_text(lbl, "abc");
            else if (key[0] == ' ') lv_label_set_text(lbl, "_____");
            else lv_label_set_text(lbl, key);

            lv_obj_set_style_text_color(lbl, key[0] == '\x04' ? C_WHITE : th_fg(), 0);
            lv_obj_set_style_text_font(
                lbl,
                (key[0] == '\x01' || key[0] == '\x02' || key[0] == '\x04') ? icon_font_sm() : &lv_font_ui_14,
                0
            );
            lv_obj_center(lbl);

            x += key_w + KB_GAP;
        }

        y += key_h + KB_GAP;
        row_idx++;
    }
}

static void hide_keyboard(void) {
    if (!kb_overlay) return;
    lv_obj_del(kb_overlay);
    kb_overlay = NULL;
    kb_lbl_input = NULL;
    ui_showing_keyboard = false;
}

/* ── Settings overlay — two-level: User List → Source Editor ── */
static cal_source_t settings_temp_sources[MAX_CAL_SOURCES];
static int settings_temp_count = 0;
static lv_obj_t *settings_list_container = NULL;

/* Forward declarations for two-level navigation */
static void settings_ensure_overlay(void);
static void settings_populate_user_list(void);
static void settings_populate_source_editor(int user_idx);
static void settings_rebuild_source_list(void);

static void hide_settings(void) {
    if (!settings_overlay) return;
    lv_obj_del(settings_overlay);
    settings_overlay = NULL;
    settings_panel = NULL;
    settings_list_container = NULL;
    ui_showing_settings = false;
    rebuild_user_bar();  /* reflect any user additions/removals */
}

static void cb_settings_close(lv_event_t *e) {
    (void)e;
    hide_settings();
}

static void cb_settings_back_to_users(lv_event_t *e) {
    (void)e;
    if (user_count <= 1) {
        hide_settings();
    } else {
        settings_populate_user_list();
    }
}

static void cb_settings_open_user(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    settings_populate_source_editor(idx);
}

/* ── User management callbacks ── */

static void cb_settings_remove_user(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (user_count <= 1) return;

    /* If removing the active user, switch to another user first */
    if (idx == active_user) {
        calendar_save_completion_state();
        active_user = (idx == 0) ? 1 : 0;
        calendar_sources_load_user(active_user);
        streak_set_active_user(active_user);
        challenge_set_active_user(active_user);
        calendar_suppress_next_completion_save();
        calendar_fetch();
        ui_refresh_all();
    }
    user_store_remove(idx);
    streak_shift_down(idx, user_count);      /* keep streak NVS indices in sync */
    challenge_shift_down(idx, user_count);   /* keep challenge NVS indices in sync */
    streak_set_active_user(active_user);     /* re-sync in-memory state to new indices */
    challenge_set_active_user(active_user);
    user_store_save();
    settings_populate_user_list();
}

static void cb_settings_kb_user_name_done(const char *text) {
    ui_showing_settings = true;
    settings_ensure_overlay();
    if (text[0] != '\0') {
        int new_idx = user_store_add(text);
        user_store_save();
        if (new_idx >= 0) {
            settings_populate_source_editor(new_idx);
            return;
        }
    }
    settings_populate_user_list();
}

static void cb_settings_add_user(lv_event_t *e) {
    (void)e;
    if (user_count >= MAX_USERS) return;

    /* Hide overlay before opening keyboard */
    if (settings_overlay) {
        lv_obj_del(settings_overlay);
        settings_overlay = NULL;
        settings_panel = NULL;
        settings_list_container = NULL;
    }
    kb_input[0] = '\0';
    kb_input_len = 0;
    kb_shift = true;
    kb_mode = 1;  /* start uppercase — names begin with a capital */
    kb_on_submit = cb_settings_kb_user_name_done;
    show_keyboard();
}

static void cb_settings_kb_rename_done(const char *text) {
    ui_showing_settings = true;
    settings_ensure_overlay();
    if (text[0] != '\0' && settings_renaming_user >= 0) {
        user_store_rename(settings_renaming_user, text);
        user_store_save();
    }
    settings_renaming_user = -1;
    settings_populate_user_list();
}

static void cb_settings_rename_user(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    settings_renaming_user = idx;
    if (settings_overlay) {
        lv_obj_del(settings_overlay);
        settings_overlay = NULL;
        settings_panel = NULL;
        settings_list_container = NULL;
    }
    const char *cur = users[idx].name;
    int len = (int)strlen(cur);
    if (len >= KB_MAX_INPUT) len = KB_MAX_INPUT - 1;
    memcpy(kb_input, cur, len);
    kb_input[len] = '\0';
    kb_input_len = len;
    kb_shift = false;
    kb_mode = 0;
    kb_on_submit = cb_settings_kb_rename_done;
    show_keyboard();
}

/* ── Source management callbacks (Level 2) ── */

static void cb_settings_toggle_source(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < settings_temp_count) {
        settings_temp_sources[idx].enabled = !settings_temp_sources[idx].enabled;
        settings_rebuild_source_list();
    }
}

static void cb_settings_remove_source(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < settings_temp_count) {
        for (int i = idx; i < settings_temp_count - 1; i++) {
            settings_temp_sources[i] = settings_temp_sources[i + 1];
        }
        settings_temp_count--;
        settings_rebuild_source_list();
    }
}


static void cb_settings_add_google(lv_event_t *e) {
    (void)e;
    if (settings_temp_count >= MAX_CAL_SOURCES) return;
    cal_source_t *s = &settings_temp_sources[settings_temp_count];
    s->type = 0;
    snprintf(s->name, sizeof(s->name), "Google Calendar");
    s->url[0] = '\0';
    s->enabled = true;
    settings_temp_count++;
    settings_rebuild_source_list();
}

static void cb_settings_add_ics(lv_event_t *e) {
    (void)e;
    if (settings_temp_count >= MAX_CAL_SOURCES) return;
    cal_source_t *s = &settings_temp_sources[settings_temp_count];
    s->type = 1;
    snprintf(s->name, sizeof(s->name), "ICS Feed");
    s->url[0] = '\0';
    s->enabled = true;
    settings_temp_count++;
    settings_rebuild_source_list();
}

static void cb_settings_save_sources(lv_event_t *e) {
    (void)e;
    if (settings_viewing_user == active_user) {
        /* Update live sources and re-fetch */
        cal_source_count = settings_temp_count;
        for (int i = 0; i < settings_temp_count; i++) cal_sources[i] = settings_temp_sources[i];
        calendar_sources_save();
        calendar_fetch();
        calendar_apply_staged();
        ui_completed = calendar_get_completed();
        if (ui_current >= cal_task_count) ui_current = 0;
        ui_refresh_all();
    } else {
        /* Save to NVS for this user without touching live data */
        calendar_sources_write_user(settings_viewing_user,
                                    settings_temp_sources, settings_temp_count);
    }
    settings_populate_user_list();
}

static void cb_settings_cancel_sources(lv_event_t *e) {
    (void)e;
    settings_populate_user_list();
}

static void cb_open_settings(lv_event_t *e) {
    (void)e;
    show_settings();
}

/* ── Scrollable source list (shared by settings_populate_source_editor) ── */
static void settings_rebuild_source_list(void) {
    if (!settings_list_container) return;
    lv_obj_clean(settings_list_container);

    for (int i = 0; i < settings_temp_count; i++) {
        cal_source_t *src = &settings_temp_sources[i];

        lv_obj_t *row = lv_obj_create(settings_list_container);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 72);
        lv_obj_set_style_bg_color(row, th_bg(), 0);
        lv_obj_set_style_bg_opa(row, src->enabled ? LV_OPA_10 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, src->enabled ? C_ACCENT : th_border(), 0);
        lv_obj_set_style_pad_all(row, 16, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *chk = lv_obj_create(row);
        lv_obj_remove_style_all(chk);
        lv_obj_set_size(chk, 22, 22);
        lv_obj_set_pos(chk, 0, 12);
        lv_obj_set_style_radius(chk, 4, 0);
        lv_obj_set_style_border_width(chk, 2, 0);
        lv_obj_set_style_border_color(chk, src->enabled ? C_ACCENT : th_muted(), 0);
        if (src->enabled) {
            lv_obj_set_style_bg_color(chk, C_ACCENT, 0);
            lv_obj_set_style_bg_opa(chk, LV_OPA_COVER, 0);
            lv_obj_t *tick = lv_label_create(chk);
            lv_label_set_text(tick, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(tick, C_WHITE, 0);
            lv_obj_set_style_text_font(tick, &lv_font_montserrat_14, 0);
            lv_obj_center(tick);
        }
        lv_obj_add_flag(chk, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chk, cb_settings_toggle_source, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, src->name);
        lv_obj_set_style_text_color(name_lbl, th_fg(), 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_ui_14, 0);
        lv_obj_set_pos(name_lbl, 34, 4);

        lv_obj_t *sub_lbl = lv_label_create(row);
        char sub_text[48];
        const char *type_str = src->type == 0 ? "Google" : "ICS";
        if (src->url[0]) {
            snprintf(sub_text, sizeof(sub_text), "%s - %.38s", type_str, src->url);
        } else {
            snprintf(sub_text, sizeof(sub_text), TR_ADD_URL_VIA_PHONE_FMT, type_str);
        }
        lv_label_set_text(sub_lbl, sub_text);
        lv_obj_set_style_text_color(sub_lbl, th_muted(), 0);
        lv_obj_set_style_text_font(sub_lbl, &lv_font_ui_14, 0);
        lv_obj_set_pos(sub_lbl, 34, 24);
        lv_obj_set_width(sub_lbl, PANEL_W - ST_PAD * 2 - 80 - 34);
        lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);

        lv_obj_t *btn_rm = lv_btn_create(row);
        lv_obj_set_size(btn_rm, 28, 28);
        lv_obj_align(btn_rm, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_opa(btn_rm, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn_rm, 0, 0);
        lv_obj_add_event_cb(btn_rm, cb_settings_remove_source, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t *rm_icon = lv_label_create(btn_rm);
        lv_label_set_text(rm_icon, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(rm_icon, th_muted(), 0);
        lv_obj_set_style_text_font(rm_icon, &lv_font_montserrat_14, 0);
        lv_obj_center(rm_icon);
    }

    /* Add-source buttons row */
    lv_obj_t *add_row = lv_obj_create(settings_list_container);
    lv_obj_remove_style_all(add_row);
    lv_obj_set_size(add_row, lv_pct(100), 52);
    lv_obj_set_style_pad_column(add_row, 8, 0);
    lv_obj_set_flex_flow(add_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(add_row, LV_OBJ_FLAG_SCROLLABLE);

    int add_btn_w = (PANEL_W - ST_PAD * 2 - 8) / 2;

    lv_obj_t *btn_add_g = lv_btn_create(add_row);
    lv_obj_set_size(btn_add_g, add_btn_w, 44);
    lv_obj_set_style_bg_opa(btn_add_g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_add_g, 2, 0);
    lv_obj_set_style_border_color(btn_add_g, th_border(), 0);
    lv_obj_set_style_radius(btn_add_g, 12, 0);
    lv_obj_set_style_shadow_width(btn_add_g, 0, 0);
    if (settings_temp_count >= MAX_CAL_SOURCES) {
        lv_obj_add_state(btn_add_g, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn_add_g, LV_OPA_40, 0);
    }
    lv_obj_add_event_cb(btn_add_g, cb_settings_add_google, LV_EVENT_CLICKED, NULL);
    lv_obj_t *g_lbl = lv_label_create(btn_add_g);
    lv_label_set_text(g_lbl, "+ Google Calendar");
    lv_obj_set_style_text_color(g_lbl, th_muted(), 0);
    lv_obj_set_style_text_font(g_lbl, &lv_font_ui_14, 0);
    lv_obj_center(g_lbl);

    lv_obj_t *btn_add_i = lv_btn_create(add_row);
    lv_obj_set_size(btn_add_i, add_btn_w, 44);
    lv_obj_set_style_bg_opa(btn_add_i, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_add_i, 2, 0);
    lv_obj_set_style_border_color(btn_add_i, th_border(), 0);
    lv_obj_set_style_radius(btn_add_i, 12, 0);
    lv_obj_set_style_shadow_width(btn_add_i, 0, 0);
    if (settings_temp_count >= MAX_CAL_SOURCES) {
        lv_obj_add_state(btn_add_i, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn_add_i, LV_OPA_40, 0);
    }
    lv_obj_add_event_cb(btn_add_i, cb_settings_add_ics, LV_EVENT_CLICKED, NULL);
    lv_obj_t *i_lbl = lv_label_create(btn_add_i);
    lv_label_set_text(i_lbl, "+ ICS / iCal Feed");
    lv_obj_set_style_text_color(i_lbl, th_muted(), 0);
    lv_obj_set_style_text_font(i_lbl, &lv_font_ui_14, 0);
    lv_obj_center(i_lbl);

    /* Web config hint */
    lv_obj_t *hint = lv_label_create(settings_list_container);
    const char *ip = web_config_get_ip();
    char hint_buf[160];
    if (ip[0]) {
        snprintf(hint_buf, sizeof(hint_buf), TR_EDIT_ON_PHONE_FMT, ip);
    } else {
        snprintf(hint_buf, sizeof(hint_buf), TR_CONNECT_WIFI);
    }
    lv_label_set_text(hint, hint_buf);
    lv_obj_set_style_text_color(hint, C_ACCENT, 0);
    lv_obj_set_style_text_font(hint, &lv_font_ui_14, 0);
    lv_obj_set_width(hint, PANEL_W - ST_PAD * 2);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

/* ── Overlay shell (idempotent — keyboard flow deletes and recreates it) ── */
static void settings_ensure_overlay(void) {
    if (settings_overlay) return;
    lv_obj_t *scr = lv_scr_act();
    settings_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(settings_overlay);
    lv_obj_set_size(settings_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(settings_overlay, 0, 0);
    lv_obj_set_style_bg_color(settings_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(settings_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(settings_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Inner content panel — same bezel geometry as the main view */
    settings_panel = lv_obj_create(settings_overlay);
    lv_obj_remove_style_all(settings_panel);
    lv_obj_set_size(settings_panel, PANEL_W, PANEL_H);
    lv_obj_set_pos(settings_panel, BEZEL_LEFT, BEZEL_BW);
    lv_obj_set_style_bg_color(settings_panel, th_bg(), 0);
    lv_obj_set_style_bg_opa(settings_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(settings_panel, BEZEL_INNER_R, 0);
    lv_obj_clear_flag(settings_panel, LV_OBJ_FLAG_SCROLLABLE);
}

/* ── Level 1: User List ── */
static void settings_populate_user_list(void) {
    settings_ensure_overlay();
    lv_obj_clean(settings_panel);
    settings_list_container = NULL;
    settings_viewing_user = -1;

    /* Header */
    lv_obj_t *btn_close = lv_btn_create(settings_panel);
    lv_obj_set_size(btn_close, 40, 40);
    lv_obj_set_pos(btn_close, ST_PAD, ST_PAD);
    lv_obj_set_style_bg_color(btn_close, th_card(), 0);
    lv_obj_set_style_radius(btn_close, 8, 0);
    lv_obj_set_style_shadow_width(btn_close, 0, 0);
    lv_obj_add_event_cb(btn_close, cb_settings_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(btn_close);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, th_muted(), 0);
    lv_obj_set_style_text_font(xl, icon_font_sm(), 0);
    lv_obj_center(xl);

    lv_obj_t *title = lv_label_create(settings_panel);
    lv_label_set_text(title, TR_USERS_TITLE);
    lv_obj_set_style_text_color(title, th_fg(), 0);
    lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
    lv_obj_set_pos(title, ST_PAD + 52, ST_PAD + 8);

    /* Scrollable list */
    lv_obj_t *list = lv_obj_create(settings_panel);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, PANEL_W - ST_PAD * 2, PANEL_H - 72);
    lv_obj_set_pos(list, ST_PAD, 72);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);

    for (int i = 0; i < user_count; i++) {
        bool is_active = (i == active_user);

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 68);
        lv_obj_set_style_bg_color(row, th_card(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, is_active ? C_ACCENT : th_border(), 0);
        lv_obj_set_style_pad_all(row, 16, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb_settings_open_user, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        /* Name */
        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, users[i].name);
        lv_obj_set_style_text_color(name_lbl, is_active ? C_ACCENT : th_fg(), 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_ui_14, 0);
        lv_obj_set_pos(name_lbl, 0, 6);

        /* Subtitle */
        lv_obj_t *sub_lbl = lv_label_create(row);
        lv_label_set_text(sub_lbl, is_active ? TR_ACTIVE_TAP : TR_TAP_EDIT_CAL);
        lv_obj_set_style_text_color(sub_lbl, is_active ? C_ACCENT : th_muted(), 0);
        lv_obj_set_style_text_font(sub_lbl, &lv_font_ui_14, 0);
        lv_obj_set_pos(sub_lbl, 0, 28);

        /* Arrow */
        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(arrow, th_muted(), 0);
        lv_obj_set_style_text_font(arrow, icon_font_sm(), 0);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -8, 0);

        /* Rename button — always shown */
        lv_obj_t *btn_rn = lv_btn_create(row);
        lv_obj_set_size(btn_rn, 28, 28);
        lv_obj_align(btn_rn, LV_ALIGN_RIGHT_MID, user_count > 1 ? -68 : -36, 0);
        lv_obj_set_style_bg_opa(btn_rn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn_rn, 0, 0);
        lv_obj_add_event_cb(btn_rn, cb_settings_rename_user, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t *rn_icon = lv_label_create(btn_rn);
        lv_label_set_text(rn_icon, LV_SYMBOL_EDIT);
        lv_obj_set_style_text_color(rn_icon, th_muted(), 0);
        lv_obj_set_style_text_font(rn_icon, &lv_font_montserrat_14, 0);
        lv_obj_center(rn_icon);

        /* Remove button — only shown when >1 user */
        if (user_count > 1) {
            lv_obj_t *btn_rm = lv_btn_create(row);
            lv_obj_set_size(btn_rm, 28, 28);
            lv_obj_align(btn_rm, LV_ALIGN_RIGHT_MID, -36, 0);
            lv_obj_set_style_bg_opa(btn_rm, LV_OPA_TRANSP, 0);
            lv_obj_set_style_shadow_width(btn_rm, 0, 0);
            lv_obj_add_event_cb(btn_rm, cb_settings_remove_user, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            lv_obj_t *rm_icon = lv_label_create(btn_rm);
            lv_label_set_text(rm_icon, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(rm_icon, th_muted(), 0);
            lv_obj_set_style_text_font(rm_icon, &lv_font_montserrat_14, 0);
            lv_obj_center(rm_icon);
        }
    }

    /* "+ Add User" button */
    lv_obj_t *btn_add = lv_btn_create(list);
    lv_obj_set_size(btn_add, lv_pct(100), 52);
    lv_obj_set_style_bg_opa(btn_add, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_add, 2, 0);
    lv_obj_set_style_border_color(btn_add, th_border(), 0);
    lv_obj_set_style_radius(btn_add, 12, 0);
    lv_obj_set_style_shadow_width(btn_add, 0, 0);
    if (user_count >= MAX_USERS) {
        lv_obj_add_state(btn_add, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn_add, LV_OPA_40, 0);
    }
    lv_obj_add_event_cb(btn_add, cb_settings_add_user, LV_EVENT_CLICKED, NULL);
    lv_obj_t *add_lbl = lv_label_create(btn_add);
    lv_label_set_text(add_lbl, TR_ADD_BTN);
    lv_obj_set_style_text_color(add_lbl, th_muted(), 0);
    lv_obj_set_style_text_font(add_lbl, &lv_font_ui_14, 0);
    lv_obj_center(add_lbl);
}

/* ── Level 2: Source Editor ── */
static void settings_populate_source_editor(int user_idx) {
    settings_ensure_overlay();
    lv_obj_clean(settings_panel);
    settings_list_container = NULL;
    settings_viewing_user = user_idx;

    /* Load this user's sources into temp without touching live cal_sources[] */
    if (user_idx == active_user) {
        settings_temp_count = cal_source_count;
        for (int i = 0; i < cal_source_count; i++) settings_temp_sources[i] = cal_sources[i];
    } else {
        settings_temp_count = calendar_sources_read_user(user_idx,
                                    settings_temp_sources, MAX_CAL_SOURCES);
    }

    /* Header: back button + "[Name]'s Calendars" */
    lv_obj_t *btn_back = lv_btn_create(settings_panel);
    lv_obj_set_size(btn_back, 40, 40);
    lv_obj_set_pos(btn_back, ST_PAD, ST_PAD);
    lv_obj_set_style_bg_color(btn_back, th_card(), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, cb_settings_back_to_users, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn_back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, th_muted(), 0);
    lv_obj_set_style_text_font(bl, icon_font_sm(), 0);
    lv_obj_center(bl);

    char title_buf[48];
    snprintf(title_buf, sizeof(title_buf), "%s's kalender", users[user_idx].name);
    lv_obj_t *title = lv_label_create(settings_panel);
    lv_label_set_text(title, title_buf);
    lv_obj_set_style_text_color(title, th_fg(), 0);
    lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
    lv_obj_set_pos(title, ST_PAD + 52, ST_PAD + 8);

    /* Scrollable source list */
    settings_list_container = lv_obj_create(settings_panel);
    lv_obj_remove_style_all(settings_list_container);
    lv_obj_set_size(settings_list_container, PANEL_W - ST_PAD * 2, PANEL_H - 72 - 64);
    lv_obj_set_pos(settings_list_container, ST_PAD, 72);
    lv_obj_set_flex_flow(settings_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(settings_list_container, 8, 0);
    lv_obj_add_flag(settings_list_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(settings_list_container, LV_OPA_TRANSP, 0);

    settings_rebuild_source_list();

    /* Footer: Cancel + Save */
    int footer_y = PANEL_H - 60;
    int btn_w = (PANEL_W - ST_PAD * 2 - 12) / 2;

    lv_obj_t *btn_cancel = lv_btn_create(settings_panel);
    lv_obj_set_size(btn_cancel, btn_w, 48);
    lv_obj_set_pos(btn_cancel, ST_PAD, footer_y);
    lv_obj_set_style_bg_color(btn_cancel, th_card(), 0);
    lv_obj_set_style_radius(btn_cancel, 12, 0);
    lv_obj_set_style_border_width(btn_cancel, 1, 0);
    lv_obj_set_style_border_color(btn_cancel, th_border(), 0);
    lv_obj_set_style_shadow_width(btn_cancel, 0, 0);
    lv_obj_add_event_cb(btn_cancel, cb_settings_cancel_sources, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(btn_cancel);
    lv_label_set_text(cl, TR_CLOSE_UPPER);
    lv_obj_set_style_text_color(cl, th_fg(), 0);
    lv_obj_set_style_text_font(cl, &lv_font_ui_14, 0);
    lv_obj_center(cl);

    lv_obj_t *btn_save = lv_btn_create(settings_panel);
    lv_obj_set_size(btn_save, btn_w, 48);
    lv_obj_set_pos(btn_save, ST_PAD + btn_w + 12, footer_y);
    lv_obj_set_style_bg_color(btn_save, C_ACCENT, 0);
    lv_obj_set_style_radius(btn_save, 12, 0);
    lv_obj_set_style_shadow_width(btn_save, 0, 0);
    lv_obj_add_event_cb(btn_save, cb_settings_save_sources, LV_EVENT_CLICKED, NULL);
    lv_obj_t *svl = lv_label_create(btn_save);
    lv_label_set_text(svl, TR_SAVE_UPPER);
    lv_obj_set_style_text_color(svl, C_WHITE, 0);
    lv_obj_set_style_text_font(svl, &lv_font_ui_14, 0);
    lv_obj_center(svl);
}

static void show_settings(void) {
    if (ui_showing_settings) return;
    ui_showing_settings = true;
    settings_ensure_overlay();
    /* Skip user list when there is only one user — go straight to source editor */
    if (user_count <= 1) {
        settings_populate_source_editor(active_user);
    } else {
        settings_populate_user_list();
    }
}

static void cb_refresh(lv_event_t *e) {
    (void)e;
    /* Flash button to accent color to acknowledge the tap */
    if (btn_refresh_obj) {
        lv_obj_set_style_bg_color(btn_refresh_obj, C_ACCENT, 0);
    }
    /* Request async refresh — the calendar_refresh_task handles the network
     * fetch without holding the LVGL lock, preventing UI freeze. */
    calendar_request_refresh();
}

/* ── Timer feature ── */

static void close_timer_popup(void) {
    if (s_timer_popup_lv_timer) {
        lv_timer_del(s_timer_popup_lv_timer);
        s_timer_popup_lv_timer = NULL;
    }
    if (s_timer_popup) {
        lv_obj_del(s_timer_popup);
        s_timer_popup           = NULL;
        s_timer_popup_arc       = NULL;
        s_timer_popup_lbl       = NULL;
        s_timer_popup_pause_lbl = NULL;
    }
}

/* Tick callback at 250 ms — updates arc and label while popup is visible */
static void cb_timer_popup_tick(lv_timer_t *t) {
    (void)t;
    if (!s_timer_popup) return;
    const timer_state_t *ts = timer_engine_get_state();
    uint32_t rem = timer_engine_remaining_sec();

    if (s_timer_popup_arc) {
        lv_arc_set_value(s_timer_popup_arc, (int32_t)rem);
    }
    if (s_timer_popup_lbl) {
        if (ts->expired) {
            lv_label_set_text(s_timer_popup_lbl, "Stopp");
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(rem / 60), (unsigned)(rem % 60));
            lv_label_set_text(s_timer_popup_lbl, buf);
        }
    }
}

/* 1 s tick — keeps timer button countdown fresh without a full refresh */
static void cb_timer_btn_tick(lv_timer_t *t) {
    (void)t;
    if (!btn_timer || !lbl_btn_timer || cal_task_count <= 0) return;
    const timer_state_t *ts = timer_engine_get_state();
    if (!ts->active && !ts->paused) return;
    if (ts->task_idx != ui_current || ts->user_idx != active_user) return;
    uint32_t rem = timer_engine_remaining_sec();
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(rem / 60), (unsigned)(rem % 60));
    lv_label_set_text(lbl_btn_timer, buf);
}

/* Close timer popup — cancel if expired, just hide if running */
static void cb_timer_popup_close(lv_event_t *e) {
    (void)e;
    const timer_state_t *ts = timer_engine_get_state();
    if (ts->expired) {
        sound_timer_stop();
        timer_engine_cancel();
    }
    close_timer_popup();
    refresh_task();
}

/* Card click in expired state cancels the timer */
static void cb_timer_card_click(lv_event_t *e) {
    (void)e;
    const timer_state_t *ts = timer_engine_get_state();
    if (ts->expired) {
        sound_timer_stop();
        timer_engine_cancel();
        close_timer_popup();
        refresh_task();
    }
}

static void cb_timer_pause(lv_event_t *e) {
    (void)e;
    const timer_state_t *ts = timer_engine_get_state();
    if (ts->paused) {
        timer_engine_resume();
    } else {
        timer_engine_pause();
    }
    if (s_timer_popup_pause_lbl) {
        const timer_state_t *nts = timer_engine_get_state();
        lv_label_set_text(s_timer_popup_pause_lbl,
                          nts->paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }
}

static void cb_timer_restart(lv_event_t *e) {
    (void)e;
    sound_timer_stop();
    timer_engine_reset();
    close_timer_popup();
    show_timer_popup();
}

static void show_timer_popup(void) {
    close_timer_popup();  /* close any existing popup first */

    const timer_state_t *ts = timer_engine_get_state();
    bool expired = ts->expired;
    uint32_t rem = timer_engine_remaining_sec();

    /* Full-screen semi-transparent overlay — clicks outside card close popup */
    s_timer_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_timer_popup);
    lv_obj_set_size(s_timer_popup, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_timer_popup, 0, 0);
    lv_obj_set_style_bg_color(s_timer_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_timer_popup, LV_OPA_50, 0);
    lv_obj_add_flag(s_timer_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_timer_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_timer_popup, cb_timer_popup_close, LV_EVENT_CLICKED, NULL);

    /* Modal card */
    lv_obj_t *card = lv_obj_create(s_timer_popup);
    lv_obj_set_size(card, 280, 320);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, th_bg(), 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, cb_timer_card_click, LV_EVENT_CLICKED, NULL);

    /* Depleting arc (full circle, orange indicator, gray background) */
    lv_obj_t *arc = lv_arc_create(card);
    lv_obj_set_size(arc, 200, 200);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 24);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, (int32_t)ts->duration_sec);
    lv_arc_set_value(arc, (int32_t)rem);
    lv_obj_set_style_arc_color(arc, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, th_track(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    s_timer_popup_arc = arc;

    /* Centre label overlaid on arc: "mm:ss" while running, "Stopp" when expired */
    lv_obj_t *lbl = lv_label_create(card);
    if (expired) {
        lv_label_set_text(lbl, "Stopp");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(rem / 60), (unsigned)(rem % 60));
        lv_label_set_text(lbl, buf);
    }
    lv_obj_set_style_text_font(lbl, &lv_font_ui_48, 0);
    lv_obj_set_style_text_color(lbl, th_fg(), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 100);
    s_timer_popup_lbl = lbl;

    /* Button row */
    int btn_row_y = 258;
    if (!expired) {
        /* Pause / play button */
        lv_obj_t *btn_pause = lv_btn_create(card);
        lv_obj_set_size(btn_pause, 52, 52);
        lv_obj_set_pos(btn_pause, 70, btn_row_y);
        lv_obj_set_style_bg_color(btn_pause, th_fg(), 0);
        lv_obj_set_style_radius(btn_pause, 26, 0);
        lv_obj_set_style_shadow_width(btn_pause, 0, 0);
        lv_obj_add_event_cb(btn_pause, cb_timer_pause, LV_EVENT_CLICKED, NULL);
        lv_obj_t *pl = lv_label_create(btn_pause);
        lv_label_set_text(pl, ts->paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(pl, th_bg(), 0);
        lv_obj_set_style_text_font(pl, icon_font_md(), 0);
        lv_obj_center(pl);
        s_timer_popup_pause_lbl = pl;
    }

    /* Restart button */
    int restart_x = expired ? 114 : 158;
    lv_obj_t *btn_restart = lv_btn_create(card);
    lv_obj_set_size(btn_restart, 52, 52);
    lv_obj_set_pos(btn_restart, restart_x, btn_row_y);
    lv_obj_set_style_bg_color(btn_restart, th_fg(), 0);
    lv_obj_set_style_radius(btn_restart, 26, 0);
    lv_obj_set_style_shadow_width(btn_restart, 0, 0);
    lv_obj_add_event_cb(btn_restart, cb_timer_restart, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(btn_restart);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(rl, th_bg(), 0);
    lv_obj_set_style_text_font(rl, icon_font_md(), 0);
    lv_obj_center(rl);

    /* 250 ms tick to keep arc and label updated */
    s_timer_popup_lv_timer = lv_timer_create(cb_timer_popup_tick, 250, NULL);
}

/* Show "already running" blocking notice */
static void cb_busy_popup_autodismiss(lv_timer_t *t) {
    lv_obj_t *overlay = (lv_obj_t *)lv_timer_get_user_data(t);
    lv_timer_del(t);
    if (overlay) lv_obj_del(overlay);
}

static void cb_busy_popup_close(lv_event_t *e) {
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
    if (overlay) lv_obj_del(overlay);
}

static void show_timer_busy_popup(void) {
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(overlay, cb_busy_popup_close, LV_EVENT_CLICKED, overlay);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, 320, 110);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, th_bg(), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, "En timer k\xC3\xB6rs\nredan i bakgrunden");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 280);
    lv_obj_set_style_text_font(lbl, &lv_font_ui_14, 0);
    lv_obj_set_style_text_color(lbl, th_fg(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    lv_timer_t *t = lv_timer_create(cb_busy_popup_autodismiss, 3000, overlay);
    lv_timer_set_repeat_count(t, 1);
}

static void cb_timer_btn(lv_event_t *e) {
    (void)e;
    if (cal_task_count <= 0 || ui_current >= cal_task_count) return;

    const timer_state_t *ts = timer_engine_get_state();
    bool same_task = (ts->task_idx == ui_current && ts->user_idx == active_user);

    if (timer_engine_is_busy() || ts->expired) {
        if (!same_task) {
            show_timer_busy_popup();
            return;
        }
        /* Same task — reopen the popup (may have been closed while timer runs) */
        show_timer_popup();
        return;
    }

    /* Start a fresh timer */
    cal_task_t *ct = &cal_tasks[ui_current];
    timer_engine_start(ui_current, active_user, ct->timer_duration_sec);
    show_timer_popup();
}

/* Called via lv_async_call when the esp_timer fires expiry — runs in LVGL context */
static void on_timer_expired(void *arg) {
    (void)arg;
    const timer_state_t *ts = timer_engine_get_state();

    /* Switch user if timer was running for a different user */
    if (ts->user_idx != active_user && ts->user_idx < user_count) {
        calendar_save_completion_state();
        calendar_sources_save_user(active_user);
        active_user = ts->user_idx;
        user_store_save();
        calendar_sources_load_user(active_user);
        streak_set_active_user(active_user);
        challenge_set_active_user(active_user);
        if (!calendar_restore_cached_tasks(active_user)) {
            calendar_set_offline_placeholder();
        }
        calendar_save_completion_state();
        calendar_suppress_next_completion_save();
        calendar_request_refresh();
    }

    /* Navigate to the expired task */
    if (ts->task_idx >= 0 && ts->task_idx < cal_task_count) {
        ui_current = ts->task_idx;
    }

    ui_completed = calendar_get_completed();
    ui_refresh_all();

    /* Alarm and popup */
    sound_timer_alarm();
    show_timer_popup();  /* shows in expired state since ts->expired == true */
}

/* ── Refresh functions ── */
static void refresh_task(void) {
    /* Handle empty task list */
    if (cal_task_count <= 0) {
        lv_label_set_text(lbl_task_counter, TR_NO_TASKS_UPPER);
        lv_label_set_text(lbl_task_time, "");
        lv_label_set_text(lbl_task_title, TR_NO_TASKS_TODAY);
        lv_obj_set_style_text_color(lbl_task_title, th_muted(), 0);
        lv_obj_set_style_text_decor(lbl_task_title, LV_TEXT_DECOR_NONE, 0);
        lv_label_set_text(lbl_btn_complete, TR_BTN_COMPLETE);
        lv_obj_set_style_bg_color(btn_complete, th_muted(), 0);
        lv_obj_add_flag(badge_completed, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(img_challenge_medal_sm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_challenge_progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_timer, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (ui_current >= cal_task_count) ui_current = 0;
    cal_task_t *t = &cal_tasks[ui_current];

    char ctr[24];
    snprintf(ctr, sizeof(ctr), TR_TASK_N_OF_M_FMT, ui_current + 1, cal_task_count);
    lv_label_set_text(lbl_task_counter, ctr);
    lv_label_set_text(lbl_task_time, t->time);
    lv_label_set_text(lbl_task_title, t->title);

    if (t->completed) {
        lv_obj_set_style_text_color(lbl_task_title, th_muted(), 0);
        lv_obj_set_style_text_decor(lbl_task_title, LV_TEXT_DECOR_STRIKETHROUGH, 0);
        lv_label_set_text(lbl_btn_complete, LV_SYMBOL_OK " " TR_BTN_COMPLETED);
        lv_obj_set_style_bg_color(btn_complete, th_muted(), 0);
        lv_obj_clear_flag(badge_completed, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_color(lbl_task_title, th_fg(), 0);
        lv_obj_set_style_text_decor(lbl_task_title, LV_TEXT_DECOR_NONE, 0);
        lv_label_set_text(lbl_btn_complete, TR_BTN_COMPLETE);
        lv_obj_set_style_bg_color(btn_complete, C_ACCENT, 0);
        lv_obj_add_flag(badge_completed, LV_OBJ_FLAG_HIDDEN);
    }

    /* Challenge indicator — shown whenever this task belongs to a series */
    if (t->challenge_target > 0) {
        int16_t extra = 0, medals = 0;
        challenge_get_progress(t->challenge_series, &extra, &medals);

        char prog_buf[24];
        if (medals > 0) {
            snprintf(prog_buf, sizeof(prog_buf), "x%d  %d/%d",
                     (int)medals, (int)extra, (int)t->challenge_target);
        } else {
            snprintf(prog_buf, sizeof(prog_buf), "%d/%d",
                     (int)extra, (int)t->challenge_target);
        }
        lv_label_set_text(lbl_challenge_progress, prog_buf);
        lv_obj_clear_flag(img_challenge_medal_sm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_challenge_progress, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(img_challenge_medal_sm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_challenge_progress, LV_OBJ_FLAG_HIDDEN);
    }

    /* Timer button */
    if (t->timer_duration_sec > 0) {
        lv_obj_clear_flag(btn_timer, LV_OBJ_FLAG_HIDDEN);
        const timer_state_t *ts = timer_engine_get_state();
        bool timer_here = (ts->active || ts->paused || ts->expired) &&
                          ts->task_idx == ui_current && ts->user_idx == active_user;
        if (timer_here) {
            uint32_t rem = timer_engine_remaining_sec();
            char buf[8];
            snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(rem / 60), (unsigned)(rem % 60));
            lv_label_set_text(lbl_btn_timer, buf);
            lv_obj_set_style_bg_color(btn_timer,
                                      ts->paused ? th_muted() : C_ACCENT, 0);
        } else {
            uint32_t min = (t->timer_duration_sec + 59) / 60;
            char buf[12];
            snprintf(buf, sizeof(buf), "%d min", (int)min);
            lv_label_set_text(lbl_btn_timer, buf);
            lv_obj_set_style_bg_color(btn_timer, C_ACCENT, 0);
        }
    } else {
        lv_obj_add_flag(btn_timer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_progress(void) {
    int pct = cal_task_count > 0 ? (ui_completed * 100) / cal_task_count : 0;
    lv_arc_set_value(arc_progress, pct);

    char n[4], t[8];
    snprintf(n, sizeof(n), "%d", ui_completed);
    snprintf(t, sizeof(t), TR_OF_N_FMT, cal_task_count);
    lv_label_set_text(lbl_arc_num, n);
    lv_label_set_text(lbl_arc_total, t);
}

static void refresh_dots(void) {
    lv_obj_clean(dots_container);
    for (int i = 0; i < cal_task_count; i++) {
        lv_obj_t *d = lv_obj_create(dots_container);
        lv_obj_remove_style_all(d);
        lv_obj_set_height(d, 10);
        lv_obj_set_style_radius(d, 5, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        if (i == ui_current) {
            lv_obj_set_width(d, 28);
            lv_obj_set_style_bg_color(d, C_ACCENT, 0);
        } else if (cal_tasks[i].completed) {
            lv_obj_set_width(d, 10);
            lv_obj_set_style_bg_color(d, th_muted(), 0);
        } else {
            lv_obj_set_width(d, 10);
            lv_obj_set_style_bg_color(d, th_track(), 0);
        }
    }
}

static void refresh_sidebar(void) {
    char s[8];
    snprintf(s, sizeof(s), "%" PRId32, streak_month_days(&streak_data));
    lv_label_set_text(lbl_streak_num, s);

    const streak_level_t *lvl = streak_get_level();
    lv_label_set_text(lbl_level_name, lvl->name);

    const streak_level_t *next = streak_get_next_level();
    char days_buf[24];
    if (next) {
        int32_t days_left = next->threshold - streak_data.total_days;
        snprintf(days_buf, sizeof(days_buf), TR_DAYS_TO_NEXT_FMT, days_left);
        lv_label_set_text(lbl_level_days, days_buf);
        lv_label_set_text(lbl_level_next, next->name);
    } else {
        int32_t days_at = streak_data.total_days - lvl->threshold;
        snprintf(days_buf, sizeof(days_buf), TR_DAYS_AT_LVL_FMT, days_at);
        lv_label_set_text(lbl_level_days, days_buf);
        lv_label_set_text(lbl_level_next, lvl->name);
    }

    lv_bar_set_value(bar_xp, streak_get_progress_to_next(), LV_ANIM_OFF);

    char greet[48], date[32];
    calendar_get_greeting(greet, sizeof(greet));
    if (user_count > 1) {
        /* Append the active user's name: "GOOD MORNING, ANNA" */
        char greet_with_name[48];
        snprintf(greet_with_name, sizeof(greet_with_name), "%s, %s",
                 greet, users[active_user].name);
        lv_label_set_text(lbl_greeting, greet_with_name);
    } else {
        lv_label_set_text(lbl_greeting, greet);
    }
    calendar_get_date_str(date, sizeof(date));
    lv_label_set_text(lbl_date, date);
}

/* ── Completion screen (full 1024×600 overlay) ── */
static void show_complete_screen(void) {
    if (complete_screen) return;
    ui_showing_complete = true;

    lv_obj_t *scr = lv_scr_act();
    complete_screen = lv_obj_create(scr);
    lv_obj_remove_style_all(complete_screen);
    lv_obj_set_size(complete_screen, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(complete_screen, 0, 0);
    lv_obj_set_style_bg_color(complete_screen, C_BG, 0);
    lv_obj_set_style_bg_opa(complete_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(complete_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Checkmark circle */
    lv_obj_t *circle = lv_obj_create(complete_screen);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 100, 100);
    lv_obj_align(circle, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(circle, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(circle, 50, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *check = lv_label_create(circle);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(check, C_WHITE, 0);
    lv_obj_set_style_text_font(check, icon_font_lg(), 0);
    lv_obj_center(check);

    /* Title */
    lv_obj_t *title = lv_label_create(complete_screen);
    lv_label_set_text(title, TR_DAY_COMPLETE);
    lv_obj_set_style_text_color(title, C_FG, 0);
    lv_obj_set_style_text_font(title, &lv_font_ui_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 180);

    /* Subtitle */
    lv_obj_t *sub = lv_label_create(complete_screen);
    char buf[32];
    snprintf(buf, sizeof(buf), TR_ALL_TASKS_DONE_FMT, cal_task_count);
    lv_label_set_text(sub, buf);
    lv_obj_set_style_text_color(sub, C_MUTED, 0);
    lv_obj_set_style_text_font(sub, &lv_font_ui_14, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 240);

    /* Monthly highscore info */
    char streak_buf[64];
    snprintf(streak_buf, sizeof(streak_buf), TR_MONTH_LEVEL_FMT,
             streak_month_days(&streak_data), streak_get_level()->name);
    lv_obj_t *streak_lbl = lv_label_create(complete_screen);
    lv_label_set_text(streak_lbl, streak_buf);
    lv_obj_set_style_text_color(streak_lbl, C_ACCENT, 0);
    lv_obj_set_style_text_font(streak_lbl, &lv_font_ui_24, 0);
    lv_obj_align(streak_lbl, LV_ALIGN_TOP_MID, 0, 300);

    /* Back button */
    lv_obj_t *btn = lv_btn_create(complete_screen);
    lv_obj_set_size(btn, 200, 52);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 400);
    lv_obj_set_style_bg_color(btn, C_ACCENT, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, cb_back_to_tasks, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " " TR_BACK);
    lv_obj_set_style_text_color(bl, C_WHITE, 0);
    lv_obj_set_style_text_font(bl, icon_font_sm(), 0);
    lv_obj_center(bl);

    /* Auto-dismiss after 5s */
    lv_timer_create(cb_auto_dismiss, 5000, NULL);

    ESP_LOGI(TAG, "Completion screen shown");
}

static void hide_complete_screen(void) {
    if (!complete_screen) return;
    lv_obj_del(complete_screen);
    complete_screen = NULL;
    ui_showing_complete = false;

    /* Turn off LEDs when completion screen is dismissed */
    ws2812_off();
    ESP_LOGI(TAG, "Completion screen hidden, LEDs off");
}

/* ── User bar constants ── */
#define USER_BAR_Y   72
#define USER_BAR_H   44
#define USER_BAR_X   40
#define USER_BAR_GAP  8

/* Restyle all user bar buttons to reflect the current active_user */
static void user_bar_refresh(void) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (!user_bar_btns[i]) continue;
        bool is_active = (i == active_user);
        lv_obj_set_style_bg_color(user_bar_btns[i], is_active ? C_ACCENT : th_card(), 0);
        lv_obj_set_style_border_width(user_bar_btns[i], is_active ? 0 : 1, 0);
        lv_obj_t *lbl = lv_obj_get_child(user_bar_btns[i], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, is_active ? C_WHITE : th_fg(), 0);

        /* Medal indicator */
        if (!user_bar_medal_imgs[i]) continue;
        int16_t medals = challenge_total_medals(i);
        if (medals > 0) {
            char mbuf[8];
            snprintf(mbuf, sizeof(mbuf), "x%d", (int)medals);
            lv_label_set_text(user_bar_medal_lbls[i], mbuf);
            lv_obj_set_style_text_color(user_bar_medal_lbls[i],
                is_active ? C_WHITE : lv_color_hex(0xB8860B), 0);
            lv_obj_clear_flag(user_bar_medal_imgs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(user_bar_medal_lbls[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(user_bar_medal_imgs[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(user_bar_medal_lbls[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── Leaderboard overlay ── */

/* ── Leaderboard tap-reset gestures ──
 *   5 taps  in 2 s → reset streak
 *  10 taps  in 2 s → reset all medals (parent override)
 * The counter persists across overlay rebuilds (static arrays), so the user
 * can streak-reset at 5 and then continue tapping to 10 for medal reset.
 */
#define LB_STREAK_RESET_TAPS  5
#define LB_MEDAL_RESET_TAPS  10
#define LB_RESET_WINDOW_MS   2000

static uint8_t  s_lb_tap_cnt[MAX_USERS];
static uint32_t s_lb_tap_t[MAX_USERS];

static void cb_lb_tap_reset(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_stop_bubbling(e);  /* don't propagate to overlay's dismiss handler */

    uint32_t now = lv_tick_get();
    if (s_lb_tap_cnt[idx] == 0 || (now - s_lb_tap_t[idx]) > LB_RESET_WINDOW_MS) {
        s_lb_tap_cnt[idx] = 1;
        s_lb_tap_t[idx]   = now;
    } else {
        s_lb_tap_cnt[idx]++;
    }

    if (s_lb_tap_cnt[idx] == LB_STREAK_RESET_TAPS) {
        /* Streak reset — keep counter going toward medal reset */
        streak_reset_user(idx);
        lv_obj_del(leaderboard_overlay);
        leaderboard_overlay = NULL;
        show_leaderboard();
        if (idx == active_user) refresh_sidebar();
        /* s_lb_tap_cnt[idx] stays at 5; tap window timestamp preserved */
    } else if (s_lb_tap_cnt[idx] >= LB_MEDAL_RESET_TAPS) {
        /* Medal reset */
        s_lb_tap_cnt[idx] = 0;
        challenge_reset_user(idx);
        if (idx == active_user) challenge_set_active_user(active_user);
        lv_obj_del(leaderboard_overlay);
        leaderboard_overlay = NULL;
        show_leaderboard();
        user_bar_refresh();
    }
}

static void cb_dismiss_leaderboard(lv_event_t *e) {
    hide_leaderboard();
}

static void hide_leaderboard(void) {
    if (!leaderboard_overlay) return;
    lv_obj_del(leaderboard_overlay);
    leaderboard_overlay = NULL;
}

static void show_leaderboard(void) {
    if (leaderboard_overlay) {
        hide_leaderboard();
        return;
    }

    /* Collect and sort user streaks (simple insertion sort, max 6 users) */
    typedef struct { int idx; streak_data_t sd; } entry_t;
    entry_t entries[MAX_USERS];
    int n = 0;
    for (int i = 0; i < user_count && i < MAX_USERS; i++) {
        entries[n].idx = i;
        streak_read_user(i, &entries[n].sd);
        n++;
    }
    /* Sort descending by days completed this month */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (streak_month_days(&entries[j].sd) > streak_month_days(&entries[i].sd)) {
                entry_t tmp = entries[i]; entries[i] = entries[j]; entries[j] = tmp;
            }
        }
    }

    /* Full-screen dim overlay */
    leaderboard_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(leaderboard_overlay);
    lv_obj_set_size(leaderboard_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(leaderboard_overlay, 0, 0);
    lv_obj_set_style_bg_color(leaderboard_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(leaderboard_overlay, LV_OPA_60, 0);
    lv_obj_clear_flag(leaderboard_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(leaderboard_overlay, cb_dismiss_leaderboard, LV_EVENT_CLICKED, NULL);

    /* Card */
    int card_w = 520;
    int row_h  = 72;
    int card_h = 64 + n * row_h + 24;
    lv_obj_t *card = lv_obj_create(leaderboard_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, ui_dark_mode ? lv_color_hex(0x262626) : lv_color_hex(0xF5F0E8), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE); /* clicks pass through to dismiss overlay */

    /* Title */
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, TR_THIS_MONTH);
    lv_obj_set_style_text_color(title, C_ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_ui_14, 0);
    lv_obj_set_pos(title, 28, 20);

    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, TR_TAP_TO_CLOSE);
    lv_obj_set_style_text_color(sub, ui_dark_mode ? lv_color_hex(0x9CA3AF) : lv_color_hex(0x8A8A7A), 0);
    lv_obj_set_style_text_font(sub, &lv_font_ui_14, 0);
    lv_obj_set_pos(sub, 28, 40);

    /* Rows */
    for (int i = 0; i < n; i++) {
        int ry = 64 + i * row_h;
        bool is_active = (entries[i].idx == active_user);

        /* Row background */
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, card_w - 32, row_h - 8);
        lv_obj_set_pos(row, 16, ry);
        lv_obj_set_style_bg_color(row,
            is_active ? C_ACCENT :
            (ui_dark_mode ? lv_color_hex(0x333333) : lv_color_hex(0xE8E6E2)), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, cb_lb_tap_reset, LV_EVENT_CLICKED,
                            (void *)(intptr_t)entries[i].idx);

        /* Rank number */
        char rank_buf[4];
        snprintf(rank_buf, sizeof(rank_buf), "%d", i + 1);
        lv_obj_t *rank = lv_label_create(row);
        lv_label_set_text(rank, rank_buf);
        lv_obj_set_style_text_color(rank, is_active ? C_WHITE : th_muted(), 0);
        lv_obj_set_style_text_font(rank, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(rank, 14, (row_h - 8 - 14) / 2);

        /* User name */
        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, users[entries[i].idx].name);
        lv_obj_set_style_text_color(name_lbl, is_active ? C_WHITE : th_fg(), 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_ui_14, 0);
        lv_obj_set_pos(name_lbl, 44, 10);

        /* Level name */
        lv_obj_t *lvl_lbl = lv_label_create(row);
        lv_label_set_text(lvl_lbl, streak_level_for(entries[i].sd.total_days));
        lv_obj_set_style_text_color(lvl_lbl, is_active ? lv_color_hex(0xFFDDB8) : th_muted(), 0);
        lv_obj_set_style_text_font(lvl_lbl, &lv_font_ui_14, 0);
        lv_obj_set_pos(lvl_lbl, 44, 32);

        /* Medal count (shown between name and the day count) */
        int16_t user_medals = challenge_total_medals(entries[i].idx);
        if (user_medals > 0) {
            lv_obj_t *medal_img = lv_image_create(row);
            lv_image_set_src(medal_img, &img_gold_medal);
            lv_image_set_scale(medal_img, 80);   /* ~20×20 from 64×64 */
            lv_obj_set_pos(medal_img, 200, 18);
            lv_obj_clear_flag(medal_img, LV_OBJ_FLAG_CLICKABLE);

            char medal_buf[8];
            snprintf(medal_buf, sizeof(medal_buf), "x%d", (int)user_medals);
            lv_obj_t *medal_lbl = lv_label_create(row);
            lv_label_set_text(medal_lbl, medal_buf);
            lv_obj_set_style_text_color(medal_lbl,
                is_active ? C_WHITE : lv_color_hex(0xB8860B), 0);
            lv_obj_set_style_text_font(medal_lbl, &lv_font_ui_14, 0);
            lv_obj_set_pos(medal_lbl, 226, 24);
        }

        /* Days-this-month number (right-aligned) */
        char streak_buf[16];
        snprintf(streak_buf, sizeof(streak_buf), "%" PRId32, streak_month_days(&entries[i].sd));
        lv_obj_t *streak_num = lv_label_create(row);
        lv_label_set_text(streak_num, streak_buf);
        lv_obj_set_style_text_color(streak_num, is_active ? C_WHITE : th_fg(), 0);
        lv_obj_set_style_text_font(streak_num, &lv_font_montserrat_28, 0);
        lv_obj_set_pos(streak_num, card_w - 32 - 80, 10);

        lv_obj_t *day_lbl = lv_label_create(row);
        lv_label_set_text(day_lbl, TR_DAYS);
        lv_obj_set_style_text_color(day_lbl, is_active ? lv_color_hex(0xFFDDB8) : th_muted(), 0);
        lv_obj_set_style_text_font(day_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(day_lbl, card_w - 32 - 80, 38);
    }
}

static void cb_open_leaderboard(lv_event_t *e) {
    (void)e;
    show_leaderboard();
}

static void cb_switch_user(lv_event_t *e) {
    int new_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (new_idx == active_user || new_idx >= user_count) return;

    /* Save outgoing user's completion state and calendar sources before switching */
    calendar_save_completion_state();
    calendar_sources_save_user(active_user);

    /* Switch active user and persist */
    active_user = new_idx;
    user_store_save();

    /* Load the new user's data */
    calendar_sources_load_user(active_user);
    streak_set_active_user(active_user);
    challenge_set_active_user(active_user);

    /* Restore cached tasks instantly — falls back to placeholder if never fetched before */
    if (!calendar_restore_cached_tasks(active_user)) {
        calendar_set_offline_placeholder();
    }
    /* Sync s_completed_keys for new user from whatever cal_tasks[] now holds,
     * so the background network fetch's was_completed() calls use correct state. */
    calendar_save_completion_state();

    ui_completed = calendar_get_completed();
    ui_current = 0;
    ui_refresh_all();

    /* Refresh from network in background — suppress save to preserve completion slot */
    calendar_suppress_next_completion_save();
    calendar_request_refresh();
}

/* Build the horizontal user-selection strip in the right panel.
 * Hidden automatically when only one user exists (single-user mode). */
static void build_user_bar(lv_obj_t *parent) {
    for (int i = 0; i < MAX_USERS; i++) {
        user_bar_btns[i]       = NULL;
        user_bar_medal_imgs[i] = NULL;
        user_bar_medal_lbls[i] = NULL;
    }

    if (user_count <= 1) return;  /* single-user: no bar, layout unchanged */

    int total_w = MAIN_W - USER_BAR_X * 2;
    int btn_w   = (total_w - (user_count - 1) * USER_BAR_GAP) / user_count;

    for (int i = 0; i < user_count; i++) {
        int x = USER_BAR_X + i * (btn_w + USER_BAR_GAP);
        bool is_active = (i == active_user);

        /* Plain obj so no button theme overrides the text color */
        lv_obj_t *btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(btn, btn_w, USER_BAR_H);
        lv_obj_set_pos(btn, x, USER_BAR_Y);
        lv_obj_set_style_bg_color(btn, is_active ? C_ACCENT : th_card(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_border_width(btn, is_active ? 0 : 1, 0);
        lv_obj_set_style_border_color(btn, th_border(), 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, cb_switch_user, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, users[i].name);
        lv_obj_set_style_text_color(lbl, is_active ? C_WHITE : th_fg(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_ui_14, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, btn_w - 52);
        lv_obj_center(lbl);

        /* Medal indicator — flex container keeps image and count snapped together */
        int16_t medals = challenge_total_medals(i);

        lv_obj_t *mcont = lv_obj_create(btn);
        lv_obj_remove_style_all(mcont);
        lv_obj_clear_flag(mcont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(mcont, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(mcont, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(mcont, 3, 0);
        lv_obj_set_size(mcont, LV_SIZE_CONTENT, 20);
        lv_obj_align(mcont, LV_ALIGN_RIGHT_MID, -4, 0);
        if (medals <= 0) lv_obj_add_flag(mcont, LV_OBJ_FLAG_HIDDEN);
        user_bar_medal_imgs[i] = mcont;   /* track container for show/hide */

        lv_obj_t *medal_img = lv_image_create(mcont);
        lv_image_set_src(medal_img, &img_gold_medal);
        lv_image_set_scale(medal_img, 64);   /* 25% of 64px → 16×16 */
        lv_obj_clear_flag(medal_img, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *medal_lbl = lv_label_create(mcont);
        lv_obj_set_style_text_font(medal_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(medal_lbl,
            is_active ? C_WHITE : lv_color_hex(0xB8860B), 0);
        char mbuf[8];
        snprintf(mbuf, sizeof(mbuf), medals > 0 ? "x%d" : "x0", (int)medals);
        lv_label_set_text(medal_lbl, mbuf);
        user_bar_medal_lbls[i] = medal_lbl;

        user_bar_btns[i] = btn;
    }
}

/* Delete and recreate the user bar — call after user_count changes */
static void rebuild_user_bar(void) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (user_bar_btns[i]) {
            lv_obj_del(user_bar_btns[i]);
            user_bar_btns[i] = NULL;
        }
        /* Children were deleted with the button */
        user_bar_medal_imgs[i] = NULL;
        user_bar_medal_lbls[i] = NULL;
    }
    if (s_mp) build_user_bar(s_mp);
}

/* ══════════════════════════════════════
 * Build the main Task Viewer UI
 * ══════════════════════════════════════ */
void ui_build(void) {
    lv_obj_t *scr = lv_scr_act();

    /* Prevent the root screen from scrolling — otherwise a slight finger drag
     * in the sidebar triggers LVGL's scroll-chain and the whole screen shifts,
     * swallowing click events (CLICKED never fires on the button). */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    lv_obj_set_style_bg_color(scr, th_bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Full-screen black base — renders as the bezel, sits below all panels */
    lv_obj_t *bezel_base = lv_obj_create(scr);
    lv_obj_remove_style_all(bezel_base);
    lv_obj_set_size(bezel_base, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(bezel_base, 0, 0);
    lv_obj_set_style_bg_color(bezel_base, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bezel_base, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bezel_base, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bezel_base, LV_OBJ_FLAG_SCROLLABLE);

    /* ═══ LEFT SIDEBAR ═══ */
    /* Direct child of scr — bypasses LVGL 9 non-clickable parent hit-test issues
     * and avoids the separate ARGB8888 render-layer that clip_corner creates. */
    lv_obj_t *sb = lv_obj_create(scr);
    lv_obj_remove_style_all(sb);
    lv_obj_set_size(sb, SIDEBAR_W - BEZEL_LEFT, PANEL_H);
    lv_obj_set_pos(sb, BEZEL_LEFT, BEZEL_BW);
    lv_obj_set_style_bg_color(sb, th_sidebar(), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sb, BEZEL_INNER_R, 0);
    lv_obj_set_style_border_width(sb, 1, 0);
    lv_obj_set_style_border_color(sb, th_border(), 0);
    lv_obj_set_style_border_side(sb, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    /* Greeting + Date */
    lbl_greeting = lv_label_create(sb);
    lv_label_set_text(lbl_greeting, TR_GOOD_MORNING);
    lv_obj_set_style_text_color(lbl_greeting, th_muted(), 0);
    lv_obj_set_style_text_font(lbl_greeting, &lv_font_ui_14, 0);
    lv_obj_set_pos(lbl_greeting, 24, 20);

    lbl_date = lv_label_create(sb);
    lv_label_set_text(lbl_date, TR_LOADING);
    lv_obj_set_style_text_color(lbl_date, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_date, &lv_font_ui_14, 0);
    lv_obj_set_pos(lbl_date, 24, 40);

    /* Streak card — tappable to open leaderboard */
    lv_obj_t *sc = lv_obj_create(sb);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, SIDEBAR_W - BEZEL_LEFT - 48, 70);
    lv_obj_set_pos(sc, 24, 72);
    lv_obj_set_style_bg_color(sc, th_card(), 0);
    lv_obj_set_style_bg_opa(sc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sc, 12, 0);
    lv_obj_clear_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sc, cb_open_leaderboard, LV_EVENT_CLICKED, NULL);

    /* Orange accent square */
    lv_obj_t *flame = lv_obj_create(sc);
    lv_obj_remove_style_all(flame);
    lv_obj_set_size(flame, 40, 40);
    lv_obj_set_pos(flame, 12, 15);
    lv_obj_set_style_bg_color(flame, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(flame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(flame, 8, 0);
    lv_obj_clear_flag(flame, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *flame_sym = lv_label_create(flame);
    lv_label_set_text(flame_sym, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(flame_sym, C_WHITE, 0);
    lv_obj_set_style_text_font(flame_sym, icon_font_md(), 0);
    lv_obj_center(flame_sym);

    lbl_streak_num = lv_label_create(sc);
    lv_label_set_text(lbl_streak_num, "0");
    lv_obj_set_style_text_color(lbl_streak_num, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_streak_num, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(lbl_streak_num, 64, 10);

    lbl_streak_label = lv_label_create(sc);
    lv_label_set_text(lbl_streak_label, TR_DAYS_THIS_MONTH);
    lv_obj_set_style_text_color(lbl_streak_label, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_streak_label, &lv_font_ui_14, 0);
    lv_obj_set_pos(lbl_streak_label, 64, 42);

    /* Level card */
    lv_obj_t *lc = lv_obj_create(sb);
    lv_obj_remove_style_all(lc);
    lv_obj_set_size(lc, SIDEBAR_W - BEZEL_LEFT - 48, 64);
    lv_obj_set_pos(lc, 24, 152);
    lv_obj_set_style_bg_color(lc, th_card(), 0);
    lv_obj_set_style_bg_opa(lc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lc, 12, 0);
    lv_obj_clear_flag(lc, LV_OBJ_FLAG_SCROLLABLE);

    lbl_level_name = lv_label_create(lc);
    lv_label_set_text(lbl_level_name, TR_LVL_1);
    lv_obj_set_style_text_color(lbl_level_name, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_level_name, &lv_font_ui_14, 0);
    lv_obj_set_pos(lbl_level_name, 16, 8);

    /* "N dagar till/som" + next/current level name in a flex row */
    lv_obj_t *days_row = lv_obj_create(lc);
    lv_obj_remove_style_all(days_row);
    lv_obj_clear_flag(days_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(days_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(days_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(days_row, 5, 0);
    lv_obj_set_size(days_row, SIDEBAR_W - 80, 18);
    lv_obj_set_pos(days_row, 16, 26);

    lbl_level_days = lv_label_create(days_row);
    lv_label_set_text(lbl_level_days, "");
    lv_obj_set_style_text_color(lbl_level_days, th_muted(), 0);
    lv_obj_set_style_text_font(lbl_level_days, &lv_font_ui_14, 0);

    lbl_level_next = lv_label_create(days_row);
    lv_label_set_text(lbl_level_next, "");
    lv_obj_set_style_text_color(lbl_level_next, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_level_next, &lv_font_ui_14, 0);

    bar_xp = lv_bar_create(lc);
    lv_obj_set_size(bar_xp, SIDEBAR_W - 80, 6);
    lv_obj_set_pos(bar_xp, 16, 50);
    lv_obj_set_style_bg_color(bar_xp, th_track(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_xp, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_xp, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_xp, 3, LV_PART_INDICATOR);

    /* Progress ring */
    arc_progress = lv_arc_create(sb);
    lv_obj_remove_style_all(arc_progress);
    lv_obj_set_size(arc_progress, 130, 130);
    lv_obj_set_pos(arc_progress, (SIDEBAR_W - BEZEL_LEFT - 130) / 2, 265);
    lv_arc_set_rotation(arc_progress, 270);
    lv_arc_set_bg_angles(arc_progress, 0, 360);
    lv_arc_set_range(arc_progress, 0, 100);
    lv_arc_set_value(arc_progress, 0);
    lv_obj_clear_flag(arc_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc_progress, th_track(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_progress, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc_progress, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_progress, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_progress, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_progress, true, LV_PART_INDICATOR);

    lbl_arc_num = lv_label_create(sb);
    lv_label_set_text(lbl_arc_num, "0");
    lv_obj_set_style_text_color(lbl_arc_num, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_arc_num, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(lbl_arc_num, (SIDEBAR_W - BEZEL_LEFT) / 2 - 8, 303);

    lbl_arc_total = lv_label_create(sb);
    lv_label_set_text(lbl_arc_total, TR_OF_ZERO);
    lv_obj_set_style_text_color(lbl_arc_total, th_muted(), 0);
    lv_obj_set_style_text_font(lbl_arc_total, &lv_font_ui_14, 0);
    lv_obj_set_pos(lbl_arc_total, (SIDEBAR_W - BEZEL_LEFT) / 2 - 14, 335);

    /* ── Bottom toolbar (anchored to bottom of sidebar) ── */
    /* Refresh button — plain obj, no theme, no transitions */
    btn_refresh_obj = lv_obj_create(sb);
    lv_obj_remove_style_all(btn_refresh_obj);
    lv_obj_add_flag(btn_refresh_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn_refresh_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn_refresh_obj, SIDEBAR_W - BEZEL_LEFT - 48, 44);
    lv_obj_set_pos(btn_refresh_obj, 24, PANEL_H - 112);
    lv_obj_set_style_bg_color(btn_refresh_obj, th_card(), 0);
    lv_obj_set_style_bg_opa(btn_refresh_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_refresh_obj, 12, 0);
    lv_obj_add_event_cb(btn_refresh_obj, cb_refresh, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rl = lv_label_create(btn_refresh_obj);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH "  " TR_REFRESH_UPPER);
    lv_obj_set_style_text_color(rl, th_fg(), 0);
    lv_obj_set_style_text_font(rl, icon_font_sm(), 0);
    lv_obj_center(rl);

    /* Dark mode + Settings row — plain objs, no theme */
    lv_obj_t *btn_dark = lv_obj_create(sb);
    lv_obj_remove_style_all(btn_dark);
    lv_obj_add_flag(btn_dark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn_dark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn_dark, (SIDEBAR_W - BEZEL_LEFT - 48 - 8) / 2, 44);
    lv_obj_set_pos(btn_dark, 24, PANEL_H - 58);
    lv_obj_set_style_bg_color(btn_dark, th_card(), 0);
    lv_obj_set_style_bg_opa(btn_dark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_dark, 12, 0);
    lv_obj_add_event_cb(btn_dark, cb_toggle_dark, LV_EVENT_CLICKED, NULL);

    lbl_dark_btn = lv_label_create(btn_dark);
    lv_label_set_text(lbl_dark_btn, ui_dark_mode ? LV_SYMBOL_EYE_CLOSE "  " TR_LIGHT_UPPER : LV_SYMBOL_EYE_OPEN "  " TR_DARK_UPPER);
    lv_obj_set_style_text_color(lbl_dark_btn, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_dark_btn, &lv_font_ui_14, 0);
    lv_obj_center(lbl_dark_btn);

    lv_obj_t *btn_settings = lv_obj_create(sb);
    lv_obj_remove_style_all(btn_settings);
    lv_obj_add_flag(btn_settings, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn_settings, (SIDEBAR_W - BEZEL_LEFT - 48 - 8) / 2, 44);
    lv_obj_set_pos(btn_settings, 24 + (SIDEBAR_W - BEZEL_LEFT - 48 - 8) / 2 + 8, PANEL_H - 58);
    lv_obj_set_style_bg_color(btn_settings, th_card(), 0);
    lv_obj_set_style_bg_opa(btn_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_settings, 12, 0);
    lv_obj_add_event_cb(btn_settings, cb_open_settings, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sl = lv_label_create(btn_settings);
    lv_label_set_text(sl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(sl, th_fg(), 0);
    lv_obj_set_style_text_font(sl, icon_font_sm(), 0);
    lv_obj_center(sl);

    /* ═══ RIGHT PANEL ═══ */
    lv_obj_t *mp = lv_obj_create(scr);
    lv_obj_remove_style_all(mp);
    lv_obj_set_size(mp, MAIN_W - BEZEL_RIGHT, PANEL_H);
    lv_obj_set_pos(mp, SIDEBAR_W, BEZEL_BW);
    lv_obj_set_style_bg_color(mp, th_bg(), 0);
    lv_obj_set_style_bg_opa(mp, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mp, BEZEL_INNER_R, 0);
    lv_obj_clear_flag(mp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mp, cb_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(mp, LV_OBJ_FLAG_GESTURE_BUBBLE);
    s_mp = mp;

    /* User selection bar (hidden when only one user exists) */
    build_user_bar(mp);

    /* Clock display (top-left of right panel) */
    lbl_clock = lv_label_create(mp);
    lv_label_set_text(lbl_clock, "00:00");
    lv_obj_set_style_text_color(lbl_clock, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_ui_14, 0);
    lv_obj_set_pos(lbl_clock, 40, 24);

    /* Sleep button — subtle power icon */
    lv_obj_t *btn_sleep = lv_obj_create(mp);
    lv_obj_remove_style_all(btn_sleep);
    lv_obj_add_flag(btn_sleep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn_sleep, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn_sleep, 32, 32);
    lv_obj_set_pos(btn_sleep, MAIN_W - 196, 18);
    lv_obj_add_event_cb(btn_sleep, cb_sleep_display, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sleep_lbl = lv_label_create(btn_sleep);
    lv_label_set_text(sleep_lbl, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(sleep_lbl, th_muted(), 0);
    lv_obj_set_style_text_font(sleep_lbl, icon_font_sm(), 0);
    lv_obj_center(sleep_lbl);

    /* Volume button — speaker icon, between power and WiFi */
    lv_obj_t *btn_vol = lv_obj_create(mp);
    lv_obj_remove_style_all(btn_vol);
    lv_obj_add_flag(btn_vol, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn_vol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn_vol, 32, 32);
    lv_obj_set_pos(btn_vol, MAIN_W - 156, 18);
    lv_obj_add_event_cb(btn_vol, cb_volume_btn, LV_EVENT_CLICKED, NULL);
    lbl_volume_icon = lv_label_create(btn_vol);
    {
        int v = sound_get_volume();
        const char *sym = (v == 0) ? LV_SYMBOL_MUTE :
                          (v < 50) ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX;
        lv_label_set_text(lbl_volume_icon, sym);
    }
    lv_obj_set_style_text_color(lbl_volume_icon, th_muted(), 0);
    lv_obj_set_style_text_font(lbl_volume_icon, icon_font_sm(), 0);
    lv_obj_center(lbl_volume_icon);

    /* WiFi status icon */
    static lv_obj_t *wifi_icon = NULL;
    wifi_icon = lv_label_create(mp);
    lv_label_set_text(wifi_icon, wifi_is_connected() ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(wifi_icon, wifi_is_connected() ? th_muted() : C_ACCENT, 0);
    lv_obj_set_style_text_font(wifi_icon, icon_font_sm(), 0);
    lv_obj_set_pos(wifi_icon, MAIN_W - 116, 26);

    /* Add task button (top-right of right panel) */
    lv_obj_t *btn_add = lv_btn_create(mp);
    lv_obj_set_size(btn_add, 36, 36);
    lv_obj_set_pos(btn_add, MAIN_W - 76, 18);
    lv_obj_set_style_bg_color(btn_add, th_card(), 0);
    lv_obj_set_style_radius(btn_add, 8, 0);
    lv_obj_set_style_border_width(btn_add, 1, 0);
    lv_obj_set_style_border_color(btn_add, th_border(), 0);
    lv_obj_add_event_cb(btn_add, cb_add_task, LV_EVENT_CLICKED, NULL);

    lv_obj_t *al = lv_label_create(btn_add);
    lv_label_set_text(al, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(al, th_muted(), 0);
    lv_obj_set_style_text_font(al, icon_font_sm(), 0);
    lv_obj_center(al);

    /* Task counter + time */
    lbl_task_counter = lv_label_create(mp);
    lv_label_set_text(lbl_task_counter, TR_TASK_COUNTER_INIT);
    lv_obj_set_style_text_color(lbl_task_counter, C_ACCENT, 0);
    lv_obj_set_style_text_font(lbl_task_counter, &lv_font_ui_14, 0);
    lv_obj_set_pos(lbl_task_counter, 44, 170);

    lbl_task_time = lv_label_create(mp);
    lv_label_set_text(lbl_task_time, "");
    lv_obj_set_style_text_color(lbl_task_time, th_muted(), 0);
    lv_obj_set_style_text_font(lbl_task_time, &lv_font_ui_14, 0);
    lv_obj_set_pos(lbl_task_time, 200, 170);

    /* Completed badge */
    badge_completed = lv_obj_create(mp);
    lv_obj_remove_style_all(badge_completed);
    lv_obj_set_size(badge_completed, 120, 28);
    lv_obj_set_pos(badge_completed, MAIN_W - 180, 167);
    lv_obj_set_style_bg_color(badge_completed, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(badge_completed, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge_completed, 14, 0);
    lv_obj_t *bl = lv_label_create(badge_completed);
    lv_label_set_text(bl, TR_KB_DONE);
    lv_obj_set_style_text_color(bl, C_WHITE, 0);
    lv_obj_set_style_text_font(bl, &lv_font_ui_14, 0);
    lv_obj_center(bl);
    lv_obj_add_flag(badge_completed, LV_OBJ_FLAG_HIDDEN);

    /* Challenge series indicator — medal icon + progress label */
    img_challenge_medal_sm = lv_image_create(mp);
    lv_image_set_src(img_challenge_medal_sm, &img_gold_medal);
    lv_image_set_scale(img_challenge_medal_sm, 128);   /* 50% → 32×32 */
    lv_obj_set_pos(img_challenge_medal_sm, 330, 162);
    lv_obj_clear_flag(img_challenge_medal_sm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img_challenge_medal_sm, LV_OBJ_FLAG_HIDDEN);

    lbl_challenge_progress = lv_label_create(mp);
    lv_label_set_text(lbl_challenge_progress, "");
    lv_obj_set_style_text_color(lbl_challenge_progress, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_challenge_progress, &lv_font_ui_14, 0);
    lv_obj_set_pos(lbl_challenge_progress, 368, 172);
    lv_obj_add_flag(lbl_challenge_progress, LV_OBJ_FLAG_HIDDEN);

    /* Hero task title */
    lbl_task_title = lv_label_create(mp);
    lv_label_set_text(lbl_task_title, TR_LOADING);
    lv_obj_set_style_text_color(lbl_task_title, th_fg(), 0);
    lv_obj_set_style_text_font(lbl_task_title, &lv_font_ui_48, 0);
    lv_obj_set_width(lbl_task_title, MAIN_W - 100);
    lv_label_set_long_mode(lbl_task_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_task_title, 44, 210);

    /* Bottom bar */
    dots_container = lv_obj_create(mp);
    lv_obj_remove_style_all(dots_container);
    lv_obj_set_size(dots_container, 400, 14);
    lv_obj_set_pos(dots_container, 40, PANEL_H - 56);
    lv_obj_set_flex_flow(dots_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots_container, 6, 0);
    lv_obj_set_flex_align(dots_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    btn_complete = lv_btn_create(mp);
    lv_obj_set_size(btn_complete, 160, 52);
    lv_obj_set_pos(btn_complete, MAIN_W - 260, PANEL_H - 72);
    lv_obj_set_style_bg_color(btn_complete, C_ACCENT, 0);
    lv_obj_set_style_radius(btn_complete, 12, 0);
    lv_obj_set_style_shadow_width(btn_complete, 12, 0);
    lv_obj_set_style_shadow_color(btn_complete, C_ACCENT, 0);
    lv_obj_set_style_shadow_opa(btn_complete, LV_OPA_30, 0);
    lv_obj_add_event_cb(btn_complete, cb_complete, LV_EVENT_CLICKED, NULL);

    lbl_btn_complete = lv_label_create(btn_complete);
    lv_label_set_text(lbl_btn_complete, TR_BTN_COMPLETE);
    lv_obj_set_style_text_color(lbl_btn_complete, C_WHITE, 0);
    lv_obj_set_style_text_font(lbl_btn_complete, &lv_font_ui_14, 0);
    lv_obj_center(lbl_btn_complete);

    lv_obj_t *btn_next = lv_btn_create(mp);
    lv_obj_set_size(btn_next, 52, 52);
    lv_obj_set_pos(btn_next, MAIN_W - 92, PANEL_H - 72);
    lv_obj_set_style_bg_color(btn_next, th_fg(), 0);
    lv_obj_set_style_radius(btn_next, 12, 0);
    lv_obj_add_event_cb(btn_next, cb_next, LV_EVENT_CLICKED, NULL);

    lv_obj_t *nl = lv_label_create(btn_next);
    lv_label_set_text(nl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(nl, th_bg(), 0);
    lv_obj_set_style_text_font(nl, icon_font_sm(), 0);
    lv_obj_center(nl);

    /* Timer button — left of btn_complete, hidden until a [T] task is current */
    btn_timer = lv_btn_create(mp);
    lv_obj_set_size(btn_timer, 120, 52);
    lv_obj_set_pos(btn_timer, MAIN_W - 388, PANEL_H - 72);
    lv_obj_set_style_bg_color(btn_timer, C_ACCENT, 0);
    lv_obj_set_style_radius(btn_timer, 12, 0);
    lv_obj_set_style_shadow_width(btn_timer, 0, 0);
    lv_obj_add_flag(btn_timer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn_timer, cb_timer_btn, LV_EVENT_CLICKED, NULL);

    lbl_btn_timer = lv_label_create(btn_timer);
    lv_label_set_text(lbl_btn_timer, "");
    lv_obj_set_style_text_color(lbl_btn_timer, C_WHITE, 0);
    lv_obj_set_style_text_font(lbl_btn_timer, &lv_font_ui_14, 0);
    lv_obj_center(lbl_btn_timer);

    /* Register expiry callback — fires in LVGL context via lv_async_call */
    timer_engine_set_expiry_cb(on_timer_expired);

    /* Initial render */
    refresh_dots();
    refresh_progress();
    refresh_task();
    refresh_sidebar();
    ui_refresh_clock();

    /* Clock update timer (every 10 seconds) */
    lv_timer_create(cb_clock_tick, 10000, NULL);

    /* Timer button countdown ticker (every 1 second) */
    lv_timer_create(cb_timer_btn_tick, 1000, NULL);

    /* Bezel = screen black background showing through — no extra objects needed */

    ESP_LOGI(TAG, "Task Viewer UI built (1024x600) [sync-test]");
}

/* ── Clock update ── */
static void cb_clock_tick(lv_timer_t *t) {
    (void)t;
    ui_refresh_clock();
}

void ui_refresh_clock(void) {
    if (!lbl_clock) return;
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(lbl_clock, buf);
}

void ui_refresh_all(void) {
    ui_completed = calendar_get_completed();

    /* Reset LED suppression if tasks are no longer all complete */
    if (ui_completed < cal_task_count) {
        ui_leds_suppressed = false;
    }

    refresh_task();
    refresh_progress();
    refresh_dots();
    refresh_sidebar();
    user_bar_refresh();
    ui_refresh_clock();
    /* Restore refresh button color — fetch is done */
    if (btn_refresh_obj) {
        lv_obj_set_style_bg_color(btn_refresh_obj, th_card(), 0);
    }
    if (!ui_leds_suppressed && !display_sleeping) {
        ws2812_update_progress(ui_completed, cal_task_count);
    }
}

void ui_next_task(void) { cb_next(NULL); }
void ui_prev_task(void) { cb_prev(NULL); }
bool ui_is_complete_shown(void) { return ui_showing_complete || ui_showing_keyboard || ui_showing_settings; }
void ui_complete_current_task(void) { cb_complete(NULL); }
void ui_dismiss_complete(void) { hide_complete_screen(); }

void ui_led_refresh(void) {
    if (!ui_leds_suppressed && !display_sleeping) {
        ws2812_update_progress(ui_completed, cal_task_count);
    }
}

void ui_set_display_sleeping(bool sleeping) {
    display_sleeping = sleeping;
    if (sleeping) {
        ws2812_off();
    } else {
        calendar_request_refresh();
    }
}

void ui_wake_display(void) {
    if (lvgl_port_lock(200)) {
        if (sleep_overlay) {
            lv_obj_del(sleep_overlay);
            sleep_overlay = NULL;
        }
        display_sleeping = false;
        lvgl_port_unlock();
    }
    /* LED restore and calendar refresh are safe outside the lock */
    if (!ui_leds_suppressed) {
        ws2812_update_progress(ui_completed, cal_task_count);
    }
    calendar_request_refresh();
}
