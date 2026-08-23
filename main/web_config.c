/**
 * Web Configuration Server — ESP-IDF HTTP server
 * Serves a mobile-friendly config page for calendar sources.
 */
#include "web_config.h"
#include "calendar_fetch.h"
#include "user_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "WEBCFG";

/* Calendar source config — shared type from calendar_fetch.h */
extern cal_source_t cal_sources[];
extern int cal_source_count;

static char s_ip_str[20] = "";
static httpd_handle_t s_server = NULL;

/* ── HTML page — responsive two-column desktop layout ── */
static const char HTML_PAGE_1[] =
"<!DOCTYPE html>"
"<html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Task Viewer</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;background:#F5F0E8;color:#2C2C2C;padding:20px;max-width:1200px;margin:0 auto}"
"h1{font-size:22px;font-weight:700;margin-bottom:20px}"
"h2{font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.07em;color:#8A8A7A;margin-bottom:10px}"
"p.sub{color:#8A8A7A;font-size:13px;margin-bottom:14px}"
".layout{display:flex;gap:20px;align-items:flex-start}"
".panel{flex:1;min-width:0;background:#FDFCFA;border-radius:14px;padding:22px;border:1px solid #E0DDD8}"
"@media(max-width:700px){.layout{flex-direction:column}.panel{padding:16px}}"
".tabs{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:14px}"
".tab{padding:7px 16px;border:none;border-radius:8px;background:#E8E6E2;color:#4A4A4A;font-size:14px;font-weight:600;cursor:pointer}"
".tab.on{background:#E8742A;color:#fff}"
".dtabs{display:flex;gap:5px;margin-bottom:14px;overflow-x:auto;padding-bottom:2px}"
".dtab{padding:6px 14px;border:none;border-radius:8px;background:#E8E6E2;color:#4A4A4A;font-size:13px;font-weight:600;cursor:pointer;white-space:nowrap;flex-shrink:0}"
".dtab.on{background:#E8742A;color:#fff}"
".card{background:#fff;border-radius:10px;padding:14px;margin-bottom:8px;border:1.5px solid #E0DDD8;position:relative}"
".card.on{border-color:#E8742A}"
".badge{font-size:11px;padding:2px 8px;border-radius:6px;font-weight:600;display:inline-block;margin-bottom:8px}"
".bg{background:#E8742A22;color:#E8742A}"
".bi{background:#4A90D922;color:#4A90D9}"
"label{display:block;font-size:12px;color:#8A8A7A;margin-bottom:3px;margin-top:8px}"
"input[type=text]{width:100%;padding:8px 10px;border:1.5px solid #D0CECC;border-radius:7px;font-size:14px;font-family:inherit;outline:none;background:#fff;transition:border .15s;color:#2C2C2C}"
"input[type=text]:focus{border-color:#E8742A}"
".ni{font-weight:600;border:none;border-bottom:1.5px solid #D0CECC;border-radius:0;padding:4px 0;margin-bottom:4px;background:transparent;width:calc(100% - 36px)}"
".ni:focus{border-bottom-color:#E8742A;outline:none}"
".tog{display:flex;align-items:center;gap:8px;margin-top:10px;font-size:13px;color:#4A4A4A}"
".tog input{width:16px;height:16px;accent-color:#E8742A;cursor:pointer}"
".rm{position:absolute;top:10px;right:10px;background:none;border:none;font-size:18px;color:#B0ADA8;cursor:pointer;line-height:1;padding:2px 6px}"
".rm:hover{color:#c0392b}"
".add-row{display:flex;gap:8px;margin:10px 0}"
".abtn{flex:1;padding:10px 8px;border:1.5px dashed #C8C5C0;border-radius:8px;background:transparent;font-size:13px;color:#8A8A7A;cursor:pointer;text-align:center}"
".abtn:hover{background:#F0EDE8}"
".sbtn{width:100%;margin-top:14px;padding:12px;background:#E8742A;color:#fff;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer}"
".sbtn:hover{opacity:.9}"
".sbtn:disabled{opacity:.5;cursor:default}"
".st{font-size:13px;color:#E8742A;text-align:center;padding:6px 0;height:26px}"
".tr{display:flex;gap:8px;align-items:flex-start;background:#fff;border-radius:10px;padding:10px 12px;margin-bottom:6px;border:1.5px solid #E0DDD8}"
".ti{flex:1}"
".tt{margin-bottom:6px}"
".tm{font-size:13px}"
".rmt{background:none;border:none;font-size:20px;color:#C0BEBB;cursor:pointer;padding:0 4px;line-height:1;margin-top:2px}"
".rmt:hover{color:#c0392b}"
".wkr{display:flex;align-items:center;gap:6px;font-size:12px;color:#8A8A7A;margin-top:6px;cursor:pointer}"
".wkr input{width:14px;height:14px;accent-color:#E8742A;cursor:pointer}"
"details{margin:10px 0 4px}"
"details summary{font-size:13px;color:#8A8A7A;cursor:pointer;padding:6px 0;list-style:none}"
".hb{background:#F5F0E8;border-radius:8px;padding:14px;font-size:13px;line-height:1.7;margin-top:6px;border:1px solid #E0DDD8}"
".empty{color:#8A8A7A;font-size:13px;padding:12px 0}"
"</style></head><body>"
"<h1>Task Viewer</h1>"
"<div class='layout'><div class='panel'>";
/* Left panel: calendar sources. Ends by closing left panel and opening right panel. */
static const char HTML_CAL_SECTION[] =
"<h2>Calendars</h2>"
"<p class='sub'>Set up calendar sources for each person.</p>"
"<div class='tabs' id='cal-user-tabs'></div>"
"<div id='list'></div>"
"<div class='add-row'>"
"<button class='abtn' onclick='addSource(0)'>+ Google (public)</button>"
"<button class='abtn' onclick='addSource(1)'>+ ICS / iCal</button>"
"</div>"
"<details>"
"<summary>How to get your calendar URL</summary>"
"<div class='hb'>"
"<b style='color:#E8742A'>Google Calendar (private):</b><br>"
"Use ICS / iCal type.<br>"
"1. calendar.google.com &rarr; Settings &rarr; click calendar name<br>"
"2. Scroll to <i>Secret address in iCal format</i> &rarr; copy URL<br><br>"
"<b style='color:#E8742A'>Google Calendar (public):</b><br>"
"Use Google (public) and enter the Gmail address.<br><br>"
"<b style='color:#E8742A'>Apple / iCloud:</b><br>"
"Calendar app &rarr; tap calendar &rarr; Share &rarr; enable Public Calendar &rarr; copy link.<br>"
"Replace <code>webcal://</code> with <code>https://</code>"
"</div>"
"</details>"
"<div class='st' id='cst'></div>"
"<button class='sbtn' id='cBtn' onclick='saveSources()'>Save Calendars</button>"
"</div>"   /* close left panel */
"<div class='panel'>"  /* open right panel */
"<h2>Tasks</h2>"
"<p class='sub'>Add manual tasks for any person &mdash; plan up to a week ahead. Tick <i>Every ...</i> to repeat a task weekly on that weekday.</p>"
"<div class='tabs' id='user-tabs'></div>"
"<div class='dtabs' id='date-tabs'></div>"
"<div id='task-list'></div>"
"<div class='add-row'><button class='abtn' onclick='addTask()'>+ Add task</button></div>"
"<div class='st' id='tst'></div>"
"<button class='sbtn' id='tBtn' onclick='saveTasks()'>Save Tasks</button>"
"</div>"   /* close right panel */
"</div>"   /* close layout */
"<p style='margin-top:16px;font-size:12px;color:#8A8A7A;text-align:center'>"
"<a href='/update' style='color:#8A8A7A'>Firmware update</a></p>";

/* All JavaScript — runs after USERS/ACTIVE_USER are injected by handle_root */
static const char HTML_TASK_SECTION[] =
"<script>"
"var sources=[];var tasks=[];var taskUser=0;var calUser=ACTIVE_USER;"
"var loadSeq=0;var tasksValid=false;"
"var today=new Date();"
"var DATES=(function(){var a=[];for(var i=0;i<7;i++){var d=new Date(today.getFullYear(),today.getMonth(),today.getDate()+i);a.push(d.getFullYear().toString()+String(d.getMonth()+1).padStart(2,'0')+String(d.getDate()).padStart(2,'0'));}return a;})();"
"var taskDate=DATES[0];var taskDateIdx=0;"
"function dlbl(d,i){return i===0?'Today':['Sun','Mon','Tue','Wed','Thu','Fri','Sat'][d.getDay()]+' '+d.getDate();}"
"function wdName(){return ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'][new Date(today.getFullYear(),today.getMonth(),today.getDate()+taskDateIdx).getDay()];}"
"function renderDateTabs(){"
"var h='';"
"for(var i=0;i<7;i++){"
"var d=new Date(today.getFullYear(),today.getMonth(),today.getDate()+i);"
"h+='<button class=\"dtab'+(i===taskDateIdx?' on':'\\'')+'\" onclick=\"switchDate('+i+')\">'+dlbl(d,i)+'</button>';"
"}"
"document.getElementById('date-tabs').innerHTML=h;}"
"function switchDate(i){taskDateIdx=i;taskDate=DATES[i];renderDateTabs();loadTasks();}"
"function renderCalUserTabs(){"
"var h='';"
"USERS.forEach(function(u,i){"
"h+='<button class=\"tab'+(i===calUser?' on':'')+' \" onclick=\"switchCalUser('+i+')\">'+esc(u)+'</button>';});"
"document.getElementById('cal-user-tabs').innerHTML=h;}"
"function switchCalUser(i){calUser=i;renderCalUserTabs();loadSources();}"
"function loadSources(){fetch('/api/sources?user='+calUser).then(r=>r.json()).then(d=>{sources=d;renderSources();});}"
"function renderSources(){"
"var h='';"
"if(!sources.length)h='<p class=\"empty\">No calendar sources. Add one below.</p>';"
"sources.forEach(function(s,i){"
"var bc=s.type===0?'bg':'bi';var bt=s.type===0?'Google (public)':'ICS / iCal';"
"h+='<div class=\"card'+(s.enabled?' on':'')+'\"><button class=\"rm\" onclick=\"rmSource('+i+')\">&times;</button>';"
"h+='<span class=\"badge '+bc+'\">'+bt+'</span><br>';"
"h+='<input class=\"ni\" type=\"text\" value=\"'+esc(s.name)+'\" onchange=\"sources['+i+'].name=this.value\" placeholder=\"Name\">';"
"h+=(s.type===0?'<label>Calendar ID (Gmail address)</label><input type=\"text\" value=\"'+esc(s.url)+'\" onchange=\"sources['+i+'].url=this.value\" placeholder=\"user@gmail.com\" maxlength=\"511\">':'<label>ICS / iCal URL</label><input type=\"text\" value=\"'+esc(s.url)+'\" onchange=\"sources['+i+'].url=this.value\" placeholder=\"https://...\" maxlength=\"511\">');"
"h+='<div class=\"tog\"><input type=\"checkbox\" '+(s.enabled?'checked':'')+' onchange=\"sources['+i+'].enabled=this.checked;renderSources()\"><span>Enabled</span></div></div>';});"
"document.getElementById('list').innerHTML=h;}"
"function addSource(t){if(sources.length>=5)return alert('Max 5 sources');sources.push({type:t,name:t===0?'Google Calendar':'ICS Feed',url:'',enabled:true});renderSources();}"
"function rmSource(i){sources.splice(i,1);renderSources();}"
"function saveSources(){"
"var btn=document.getElementById('cBtn');var st=document.getElementById('cst');"
"btn.disabled=true;btn.textContent='Saving...';"
"fetch('/api/sources?user='+calUser,{method:'POST',headers:{'Content-Type':'application/json','X-Rev':'2'},body:JSON.stringify(sources)})"
".then(r=>r.json()).then(function(){"
"st.textContent=calUser===ACTIVE_USER?'Saved! Display will refresh.':'Saved for '+USERS[calUser]+'.';"
"btn.disabled=false;btn.textContent='Save Calendars';"
"setTimeout(function(){st.textContent='';},3000);"
"}).catch(function(e){btn.disabled=false;btn.textContent='Save Calendars';alert('Error: '+e);});}"
"function renderUserTabs(){"
"var h='';"
"USERS.forEach(function(u,i){"
"h+='<button class=\"tab'+(i===taskUser?' on':'')+' \" onclick=\"switchUser('+i+')\">'+esc(u)+'</button>';});"
"document.getElementById('user-tabs').innerHTML=h;}"
"function switchUser(i){taskUser=i;renderUserTabs();loadTasks();}"
"function loadTasks(){"
"var q=++loadSeq;tasksValid=false;"
"fetch('/api/tasks?user='+taskUser+'&date='+taskDate).then(r=>r.json()).then(function(d){"
"if(q!==loadSeq)return;"  /* stale response from an earlier tab — discard */
"tasks=d;tasksValid=true;renderTasks();"
"}).catch(function(){"
"if(q!==loadSeq)return;"
"tasks=[];renderTasks();"
"document.getElementById('tst').textContent='Load failed - tap a day tab to retry';"
"});}"
"function renderTasks(){"
"var h='';"
"if(!tasks.length)h='<p class=\"empty\">No tasks for this day.</p>';"
"tasks.forEach(function(t,i){"
"h+='<div class=\"tr\"><div class=\"ti\">';"
"h+='<input class=\"tt\" type=\"text\" value=\"'+esc(t.title)+'\" onchange=\"tasks['+i+'].title=this.value\" placeholder=\"Task title\">';"
"h+='<input class=\"tm\" type=\"text\" value=\"'+esc(t.time)+'\" onchange=\"tasks['+i+'].time=this.value\" placeholder=\"Time e.g. 08:30 (optional)\">';"
"h+='<label class=\"wkr\"><input type=\"checkbox\" '+(t.weekly?'checked':'')+' onchange=\"tasks['+i+'].weekly=this.checked\"><span>Every '+wdName()+'</span></label>';"
"h+='</div><button class=\"rmt\" onclick=\"rmTask('+i+')\">&times;</button></div>';});"
"document.getElementById('task-list').innerHTML=h;}"
"function addTask(){tasks.push({title:'',time:'',weekly:false});renderTasks();var el=document.querySelectorAll('.tt');if(el.length)el[el.length-1].focus();}"
"function rmTask(i){tasks.splice(i,1);renderTasks();}"
"function saveTasks(){"
"var btn=document.getElementById('tBtn');var st=document.getElementById('tst');"
"if(!tasksValid){st.textContent='List not loaded - tap a day tab to retry';return;}"  /* saving a stale/failed list would wipe this weekday's tasks */
"btn.disabled=true;btn.textContent='Saving...';"
"fetch('/api/tasks?user='+taskUser+'&date='+taskDate,{method:'POST',headers:{'Content-Type':'application/json','X-Rev':'2'},body:JSON.stringify(tasks)})"
".then(r=>r.json()).then(function(){"
"st.textContent='Saved! Will appear on device.';"
"btn.disabled=false;btn.textContent='Save Tasks';"
"setTimeout(function(){st.textContent='';},3000);"
"}).catch(function(e){btn.disabled=false;btn.textContent='Save Tasks';alert('Error: '+e);});}"
"function esc(s){return(s||'').replace(/&/g,'&amp;').replace(/\"/g,'&quot;').replace(/</g,'&lt;');}"
"renderCalUserTabs();renderUserTabs();renderDateTabs();loadSources();loadTasks();"
"</script></body></html>";

/* Reject writes from outdated cached copies of the page — the pre-guard JS
 * could save one user's task list under another user/date (tab race). The
 * current page sends X-Rev: 2 with every POST; a stale page does not. */
static bool page_rev_ok(httpd_req_t *req) {
    char rev[8] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Rev", rev, sizeof(rev)) != ESP_OK)
        return false;
    return strcmp(rev, "2") == 0;
}

/* ── GET / — serve config page ── */
static esp_err_t handle_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    /* Never let browsers cache the page — a stale copy of the JS can save
     * task lists under the wrong user/date */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    httpd_resp_send_chunk(req, HTML_PAGE_1, strlen(HTML_PAGE_1));

    /* Calendar sources section */
    httpd_resp_send_chunk(req, HTML_CAL_SECTION, strlen(HTML_CAL_SECTION));

    /* Inject USERS JS array so the task manager knows all user names */
    char users_js[280] = "<script>var USERS=[";
    for (int i = 0; i < user_count; i++) {
        char entry[48];
        snprintf(entry, sizeof(entry), "%s\"%s\"", i > 0 ? "," : "", users[i].name);
        strncat(users_js, entry, sizeof(users_js) - strlen(users_js) - 1);
    }
    char active_js[32];
    snprintf(active_js, sizeof(active_js), "];var ACTIVE_USER=%d;</script>", active_user);
    strncat(users_js, active_js, sizeof(users_js) - strlen(users_js) - 1);
    httpd_resp_send_chunk(req, users_js, strlen(users_js));

    /* Task manager section */
    httpd_resp_send_chunk(req, HTML_TASK_SECTION, strlen(HTML_TASK_SECTION));

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /api/sources?user=N — return calendar sources for any user ── */
static esp_err_t handle_get_sources(httpd_req_t *req) {
    char query[16] = {0};
    int user_idx = active_user;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(query, "user", val, sizeof(val)) == ESP_OK)
            user_idx = atoi(val);
    }
    if (user_idx < 0 || user_idx >= MAX_USERS) user_idx = active_user;

    cal_source_t srcs[MAX_CAL_SOURCES];
    int count;
    if (user_idx == active_user) {
        /* Use live array so unsaved edits are visible */
        count = cal_source_count;
        memcpy(srcs, cal_sources, count * sizeof(cal_source_t));
    } else {
        count = calendar_sources_read_user(user_idx, srcs, MAX_CAL_SOURCES);
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "type", srcs[i].type);
        cJSON_AddStringToObject(obj, "name", srcs[i].name);
        cJSON_AddStringToObject(obj, "url",  srcs[i].url);
        cJSON_AddBoolToObject(obj,   "enabled", srcs[i].enabled);
        cJSON_AddItemToArray(arr, obj);
    }
    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* ── POST /api/sources?user=N — save calendar sources for any user ── */
static esp_err_t handle_post_sources(httpd_req_t *req) {
    if (!page_rev_ok(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Page outdated - reload the page");
        return ESP_FAIL;
    }
    char query[16] = {0};
    int user_idx = active_user;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(query, "user", val, sizeof(val)) == ESP_OK)
            user_idx = atoi(val);
    }
    if (user_idx < 0 || user_idx >= MAX_USERS) user_idx = active_user;

    size_t total_len = req->content_len;
    if (total_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large");
        return ESP_FAIL;
    }
    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); return ESP_FAIL; }
        received += (size_t)ret;
    }
    buf[total_len] = '\0';

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    cal_source_t srcs[MAX_CAL_SOURCES];
    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (count >= MAX_CAL_SOURCES) break;
        cJSON *jtype    = cJSON_GetObjectItem(item, "type");
        cJSON *jname    = cJSON_GetObjectItem(item, "name");
        cJSON *jurl     = cJSON_GetObjectItem(item, "url");
        cJSON *jenabled = cJSON_GetObjectItem(item, "enabled");
        srcs[count].type = cJSON_IsNumber(jtype) ? jtype->valueint : 0;
        strncpy(srcs[count].name,
                cJSON_IsString(jname) ? jname->valuestring : "Calendar",
                sizeof(srcs[count].name) - 1);
        srcs[count].name[sizeof(srcs[count].name) - 1] = '\0';
        strncpy(srcs[count].url,
                cJSON_IsString(jurl) ? jurl->valuestring : "",
                CAL_URL_MAX - 1);
        srcs[count].url[CAL_URL_MAX - 1] = '\0';
        srcs[count].enabled = cJSON_IsBool(jenabled) ? cJSON_IsTrue(jenabled) : true;
        count++;
    }
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Updated %d calendar sources for user %d via web", count, user_idx);

    if (user_idx == active_user) {
        /* Update live array and trigger display refresh */
        memcpy(cal_sources, srcs, count * sizeof(cal_source_t));
        cal_source_count = count;
        calendar_sources_save();
        calendar_request_refresh();
    } else {
        /* Write directly to that user's NVS namespace — no refresh needed */
        calendar_sources_write_user(user_idx, srcs, count);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* Weekday (Sunday=0..Saturday=6) for a YYYYMMDD date string */
static int weekday_of_date8(const char *date8) {
    int y = 0, m = 0, d = 0;
    if (sscanf(date8, "%4d%2d%2d", &y, &m, &d) != 3) return -1;
    struct tm t = {0};
    t.tm_year = y - 1900;
    t.tm_mon  = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;   /* midday — immune to DST edge cases */
    if (mktime(&t) == (time_t)-1) return -1;
    return t.tm_wday;
}

static bool task_item_is_weekly(const cJSON *item) {
    const cJSON *jwk = cJSON_GetObjectItem((cJSON *)item, "weekly");
    return cJSON_IsBool(jwk) && cJSON_IsTrue(jwk);
}

static void task_from_json(cal_task_t *dst, const cJSON *item) {
    cJSON *jttl = cJSON_GetObjectItem((cJSON *)item, "title");
    cJSON *jtm  = cJSON_GetObjectItem((cJSON *)item, "time");
    strncpy(dst->title,
            cJSON_IsString(jttl) ? jttl->valuestring : "Task",
            MAX_TITLE_LEN - 1);
    dst->title[MAX_TITLE_LEN - 1] = '\0';
    strncpy(dst->time,
            cJSON_IsString(jtm) ? jtm->valuestring : "",
            sizeof(dst->time) - 1);
    dst->time[sizeof(dst->time) - 1] = '\0';
    dst->id[0] = '\0';
    dst->completed = false;
}

static void append_tasks_json(cJSON *arr, const cal_task_t *tasks, int count, bool weekly) {
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "title", tasks[i].title);
        cJSON_AddStringToObject(obj, "time",  tasks[i].time);
        cJSON_AddBoolToObject(obj,   "weekly", weekly);
        cJSON_AddItemToArray(arr, obj);
    }
}

/* ── GET /api/tasks?user=N&date=YYYYMMDD — return manual tasks for a user/date ── */
static esp_err_t handle_get_tasks(httpd_req_t *req) {
    char query[48] = {0};
    int user_idx = active_user;
    char date8[10] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(query, "user", val, sizeof(val)) == ESP_OK)
            user_idx = atoi(val);
        httpd_query_key_value(query, "date", date8, sizeof(date8));
    }
    if (user_idx < 0 || user_idx >= MAX_USERS) user_idx = 0;
    if (strlen(date8) != 8) {
        time_t now; time(&now);
        struct tm t; localtime_r(&now, &t);
        snprintf(date8, sizeof(date8), "%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    }

    /* One buffer for both loads — each batch is fully serialized into the
     * cJSON array before the buffer is reused. A second array would put
     * ~5.5 KB of locals on the 8 KB httpd task stack. */
    cal_task_t tasks[20];
    cJSON *arr = cJSON_CreateArray();

    int count = manual_tasks_load_user(user_idx, date8, tasks, 20);
    append_tasks_json(arr, tasks, count, false);

    count = weekly_tasks_load_user(user_idx, weekday_of_date8(date8), tasks, 20);
    append_tasks_json(arr, tasks, count, true);
    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

/* ── POST /api/tasks?user=N&date=YYYYMMDD — save manual tasks for a user/date ── */
static esp_err_t handle_post_tasks(httpd_req_t *req) {
    if (!page_rev_ok(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Page outdated - reload the page");
        return ESP_FAIL;
    }
    char query[48] = {0};
    int user_idx = active_user;
    char date8[10] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(query, "user", val, sizeof(val)) == ESP_OK)
            user_idx = atoi(val);
        httpd_query_key_value(query, "date", date8, sizeof(date8));
    }
    if (user_idx < 0 || user_idx >= MAX_USERS) user_idx = 0;
    if (strlen(date8) != 8) {
        time_t now; time(&now);
        struct tm t; localtime_r(&now, &t);
        snprintf(date8, sizeof(date8), "%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    }

    size_t total_len = req->content_len;
    if (total_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large");
        return ESP_FAIL;
    }
    char *buf = malloc(total_len + 1);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    size_t received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); return ESP_FAIL; }
        received += (size_t)ret;
    }
    buf[total_len] = '\0';

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }

    int wday = weekday_of_date8(date8);
    if (wday < 0) {
        /* Never save half a payload: a bad date would silently discard the
         * weekly tasks while reporting success */
        cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad date");
        return ESP_FAIL;
    }

    /* Split into date-bound and weekly tasks with two passes over the parsed
     * document, reusing one buffer — two arrays would put ~5.5 KB of locals
     * on the 8 KB httpd task stack */
    cal_task_t tasks[20];
    int count;
    cJSON *item;

    count = 0;
    cJSON_ArrayForEach(item, arr) {
        if (task_item_is_weekly(item) || count >= 20) continue;
        task_from_json(&tasks[count++], item);
    }
    manual_tasks_save_user(user_idx, date8, tasks, count);

    count = 0;
    cJSON_ArrayForEach(item, arr) {
        if (!task_item_is_weekly(item) || count >= 20) continue;
        task_from_json(&tasks[count++], item);
    }
    weekly_tasks_save_user(user_idx, wday, tasks, count);

    cJSON_Delete(arr);
    calendar_request_refresh();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── Firmware update (OTA) ── */

static const char OTA_PAGE[] =
"<!DOCTYPE html>"
"<html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>mArk Firmware Update</title>"
"<style>"
"body{font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;background:#F5F0E8;color:#2C2C2C;padding:24px;max-width:480px;margin:0 auto}"
"h1{font-size:20px;margin-bottom:8px}"
"p{color:#8A8A7A;font-size:13px;margin-bottom:16px}"
"input[type=file]{width:100%;margin-bottom:14px;font-size:14px}"
"button{width:100%;padding:12px;background:#E8742A;color:#fff;border:none;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer}"
"button:disabled{opacity:.5}"
"#st{margin-top:14px;font-size:14px;color:#E8742A;min-height:20px}"
"a{color:#E8742A}"
"</style></head><body>"
"<h1>Firmware Update</h1>"
"<p>Build the project, then upload <b>build/task_viewer.bin</b>. "
"The display keeps all users, tasks and scores. Do not power off during the update.</p>"
"<input type='file' id='f' accept='.bin'>"
"<button id='b' onclick='up()'>Upload &amp; install</button>"
"<div id='st'></div>"
"<p style='margin-top:20px'><a href='/'>&larr; Back to settings</a></p>"
"<script>"
"function up(){"
"var f=document.getElementById('f').files[0];"
"var st=document.getElementById('st');var b=document.getElementById('b');"
"if(!f){st.textContent='Pick the .bin file first';return;}"
"b.disabled=true;st.textContent='Uploading... keep this page open';"
"fetch('/update',{method:'POST',body:f}).then(function(r){return r.text().then(function(t){return{ok:r.ok,t:t};});})"
".then(function(x){st.textContent=x.ok?x.t+' - display is restarting':'Failed: '+x.t;b.disabled=false;})"
".catch(function(e){st.textContent='Failed: '+e;b.disabled=false;});}"
"</script></body></html>";

static esp_err_t handle_ota_page(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, OTA_PAGE, strlen(OTA_PAGE));
    return ESP_OK;
}

static esp_err_t handle_ota_post(httpd_req_t *req) {
    size_t total = req->content_len;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);

    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    if (total < 0x10000 || total > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a valid firmware size");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: receiving %u bytes into %s", (unsigned)total, part->label);

    esp_ota_handle_t ota;
    if (esp_ota_begin(part, total, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t received = 0;
    bool first_chunk = true;
    while (received < total) {
        size_t want = total - received;
        if (want > 4096) want = 4096;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            esp_ota_abort(ota);
            ESP_LOGE(TAG, "OTA: receive failed at %u bytes", (unsigned)received);
            return ESP_FAIL;
        }
        if (first_chunk) {
            if ((uint8_t)buf[0] != 0xE9) {  /* ESP image magic byte */
                free(buf);
                esp_ota_abort(ota);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not an ESP firmware image");
                return ESP_FAIL;
            }
            first_chunk = false;
        }
        if (esp_ota_write(ota, buf, r) != ESP_OK) {
            free(buf);
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    free(buf);

    if (esp_ota_end(ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image validation failed");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: update written to %s — restarting", part->label);
    httpd_resp_sendstr(req, "Update installed");
    vTaskDelay(pdMS_TO_TICKS(750));  /* let the response reach the browser */
    esp_restart();
    return ESP_OK;  /* not reached */
}

/* ── Start server ── */
void web_config_start(void) {
    /* Get IP address */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }

    httpd_uri_t uri_root       = { .uri = "/",          .method = HTTP_GET,  .handler = handle_root };
    httpd_uri_t uri_get_src    = { .uri = "/api/sources",.method = HTTP_GET,  .handler = handle_get_sources };
    httpd_uri_t uri_post_src   = { .uri = "/api/sources",.method = HTTP_POST, .handler = handle_post_sources };
    httpd_uri_t uri_get_tasks  = { .uri = "/api/tasks",  .method = HTTP_GET,  .handler = handle_get_tasks };
    httpd_uri_t uri_post_tasks = { .uri = "/api/tasks",  .method = HTTP_POST, .handler = handle_post_tasks };
    httpd_uri_t uri_ota_page   = { .uri = "/update",     .method = HTTP_GET,  .handler = handle_ota_page };
    httpd_uri_t uri_ota_post   = { .uri = "/update",     .method = HTTP_POST, .handler = handle_ota_post };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_get_src);
    httpd_register_uri_handler(s_server, &uri_post_src);
    httpd_register_uri_handler(s_server, &uri_get_tasks);
    httpd_register_uri_handler(s_server, &uri_post_tasks);
    httpd_register_uri_handler(s_server, &uri_ota_page);
    httpd_register_uri_handler(s_server, &uri_ota_post);

    /* mDNS — advertise as http://taskviewer.local/ */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("taskviewer");
        mdns_instance_name_set("Task Viewer");
        mdns_service_add("Task Viewer", "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS init failed — taskviewer.local unavailable");
    }

    ESP_LOGI(TAG, "Web config server started at http://%s/ (http://taskviewer.local/)", s_ip_str);
}

const char *web_config_get_ip(void) {
    return s_ip_str;
}
