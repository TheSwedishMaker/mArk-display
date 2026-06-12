/**
 * UI language strings — English by default.
 *
 * For a Swedish build, add to your local main/secrets.h (never committed):
 *     #define UI_LANG_SV 1
 *
 * Every user-visible string on the display lives here. Some strings are
 * load-bearing (TR_ALL_DAY and TR_NO_EVENTS are compared with strcmp in
 * sorting and cache logic), so always use these macros — never retype the
 * literal text.
 */
#pragma once

#include "secrets.h"

#ifdef UI_LANG_SV

/* ── Svenska ── */
#define TR_ALL_DAY        "Hela dagen"
#define TR_TODAY          "Idag"
#define TR_TOMORROW       "Imorgon"
#define TR_YESTERDAY      "Igår"
#define TR_NO_EVENTS      "Inga händelser"
#define TR_NO_TASKS       "Inga uppgifter"
#define TR_GOOD_MORNING   "GODMORGON"
#define TR_GOOD_AFTERNOON "HEJ"
#define TR_GOOD_EVENING   "GOD KVÄLL"
#define TR_DAYS_THIS_MONTH "dagar i månaden"
#define TR_THIS_MONTH     "DENNA MÅNAD"
#define TR_DAYS           "dagar"
#define TR_DAYS_TO_NEXT_FMT "%" PRId32 " dagar till"      /* + level name on next line */
#define TR_DAYS_AT_LVL_FMT  "%" PRId32 " dagar som"
#define TR_DAY_COMPLETE   "Dag avklarad"
#define TR_ALL_TASKS_DONE_FMT "Alla %d uppgifter klara"
#define TR_MONTH_LEVEL_FMT "%" PRId32 " dagar i månaden  •  %s"
#define TR_BACK           "Tillbaka"
#define TR_TAP_TO_CLOSE   "tryck för att stänga"
#define TR_VOLUME         "Volym"
#define TR_TASK_NAME_PLACEHOLDER "Uppgiftsnamn..."
#define TR_USERS_TITLE    "Användare"
#define TR_ACTIVE_TAP     "AKTIV  •  tryck för att ändra kalendrar"
#define TR_TAP_EDIT_CAL   "Tryck för att ändra i kalender"
#define TR_ADD_BTN        "+ Lägg till"
#define TR_CLOSE_UPPER    "AVSLUTA"
#define TR_SAVE_UPPER     "SPARA"
#define TR_NO_TASKS_UPPER "INGA UPPGIFTER"
#define TR_NO_TASKS_TODAY "Inga uppgifter idag"
#define TR_BTN_COMPLETE   "Avklarad"
#define TR_BTN_COMPLETED  "Färdig"
#define TR_LOADING        "Laddar..."
#define TR_OF_ZERO        "av 0"
#define TR_OF_N_FMT       "av %d"
#define TR_REFRESH_UPPER  "UPPDATERA"
#define TR_LIGHT_UPPER    "LJUST"
#define TR_DARK_UPPER     "MÖRKT"
#define TR_TASK_N_OF_M_FMT "UPPGIFT %d AV %d"
#define TR_TASK_COUNTER_INIT "UPPGIFT 1 AV 1"
#define TR_KB_DONE        "KLAR"
#define TR_EDIT_ON_PHONE_FMT "Ändra på din telefon: http://%s/"
#define TR_CONNECT_WIFI   "Anslut till WiFi för att ändra"
#define TR_ADD_URL_VIA_PHONE_FMT "%s - Lägg till URL via telefon"
#define TR_LVL_1          "Lärling"
#define TR_LVL_2          "Upptäckare"
#define TR_LVL_3          "Magiker"
#define TR_LVL_4          "Mästare"
#define TR_LVL_5          "Stormästare"
#define TR_LVL_6          "Tidsväktare"
#define TR_DAY_NAMES      {"Söndag","Måndag","Tisdag","Onsdag","Torsdag","Fredag","Lördag"}
#define TR_MONTH_NAMES    {"Jan","Feb","Mar","Apr","Maj","Jun","Jul","Aug","Sep","Okt","Nov","Dec"}

#else

/* ── English (default) ── */
#define TR_ALL_DAY        "All day"
#define TR_TODAY          "Today"
#define TR_TOMORROW       "Tomorrow"
#define TR_YESTERDAY      "Yesterday"
#define TR_NO_EVENTS      "No events"
#define TR_NO_TASKS       "No tasks"
#define TR_GOOD_MORNING   "GOOD MORNING"
#define TR_GOOD_AFTERNOON "GOOD AFTERNOON"
#define TR_GOOD_EVENING   "GOOD EVENING"
#define TR_DAYS_THIS_MONTH "days this month"
#define TR_THIS_MONTH     "THIS MONTH"
#define TR_DAYS           "days"
#define TR_DAYS_TO_NEXT_FMT "%" PRId32 " days to"         /* + level name on next line */
#define TR_DAYS_AT_LVL_FMT  "%" PRId32 " days as"
#define TR_DAY_COMPLETE   "Day Complete"
#define TR_ALL_TASKS_DONE_FMT "All %d tasks done"
#define TR_MONTH_LEVEL_FMT "%" PRId32 " days this month  •  %s"
#define TR_BACK           "Back"
#define TR_TAP_TO_CLOSE   "tap anywhere to close"
#define TR_VOLUME         "Volume"
#define TR_TASK_NAME_PLACEHOLDER "Task name..."
#define TR_USERS_TITLE    "Users"
#define TR_ACTIVE_TAP     "ACTIVE  •  tap to edit calendars"
#define TR_TAP_EDIT_CAL   "Tap to edit calendars"
#define TR_ADD_BTN        "+ Add"
#define TR_CLOSE_UPPER    "CLOSE"
#define TR_SAVE_UPPER     "SAVE"
#define TR_NO_TASKS_UPPER "NO TASKS"
#define TR_NO_TASKS_TODAY "No tasks today"
#define TR_BTN_COMPLETE   "Complete"
#define TR_BTN_COMPLETED  "Done"
#define TR_LOADING        "Loading..."
#define TR_OF_ZERO        "of 0"
#define TR_OF_N_FMT       "of %d"
#define TR_REFRESH_UPPER  "REFRESH"
#define TR_LIGHT_UPPER    "LIGHT"
#define TR_DARK_UPPER     "DARK"
#define TR_TASK_N_OF_M_FMT "TASK %d OF %d"
#define TR_TASK_COUNTER_INIT "TASK 1 OF 1"
#define TR_KB_DONE        "DONE"
#define TR_EDIT_ON_PHONE_FMT "Edit on your phone: http://%s/"
#define TR_CONNECT_WIFI   "Connect to WiFi to edit"
#define TR_ADD_URL_VIA_PHONE_FMT "%s - add URL via phone"
#define TR_LVL_1          "Starter"
#define TR_LVL_2          "Consistent"
#define TR_LVL_3          "Dedicated"
#define TR_LVL_4          "Unstoppable"
#define TR_LVL_5          "Legend"
#define TR_LVL_6          "Titan"
#define TR_DAY_NAMES      {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"}
#define TR_MONTH_NAMES    {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"}

#endif
