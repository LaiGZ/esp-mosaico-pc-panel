// SPDX-License-Identifier: Apache-2.0
//
// ESP-Mosaico PC status panel.
//
// The device joins the local Wi-Fi (provisioned on the touch screen), receives
// status datagrams from the PC agent and renders them with ESP-GSP on the
// 480x480 CO5300 display. The retained Vibe Mode / OTA contract is preserved.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "board_display.h"
#include "bsp/esp_mosaico.h"
#include "esp_app_desc.h"
#include "esp_display_present.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heap_watch.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "orientation.h"
#include "pc_link.h"
#include "pc_panel_ui.h"
#include "ui_bundle.h"
#include "wifi_manager.h"

static const char *TAG = "pc_display";

/* Rotation support. APP_USE_IMU brings up the BMI270 for orientation sensing
 * (before the display; see app_main) and enables the periodic reads.
 * APP_OWN_PRESENTER lets the application own the display presenter, which is
 * what makes runtime rotation possible. */
#define APP_USE_IMU          1
#define APP_OWN_PRESENTER    1
/* Diagnostic switch: print the settled angle and the presenter / pause / set /
 * resume error codes on the dashboard host line. The Iris log stream drops
 * bytes on this board, so this is the only dependable readout when the
 * accelerometer mapping or the panel rotation needs checking. Keep 0. */
#define DEBUG_SHOW_IMU       0

#define DEBUG_OPEN_DETAIL_ON_BOOT 0   /* screenshot aid; keep 0 */
/* Verification aid: bind to the first PC that announces itself, so the whole
 * pairing flow can be checked without touching the screen. Keep 0. */
#define DEBUG_AUTO_PAIR 0
/* Verification aid: forget the pairing at 10 s so the pairing page is shown
 * without touching the screen. Keep 0. */
#define DEBUG_FORCE_UNPAIR 0
/* Diagnostic switch: force a rotation cycle at 60 s / 120 s of uptime to check
 * the framework's runtime-rotation path. Keep 0. */
#define DEBUG_FORCE_ROTATION 0
#define APP_TICK_MS          500
#define APP_TASK_STACK       6144
#define APP_TASK_PRIORITY    5
/* Freeze the clock after this long without a PC sample. */
#define APP_CLOCK_STALE_MS   15000
#define APP_SSID_MAX         33

typedef struct {
    esp_gsp_handle_t      ui;
    pc_panel_t           *panel;
    pc_link_handle_t      link;
    wifi_manager_handle_t wifi;
    /* Owned presenter enables runtime rotation; NULL falls back to target-only. */
    struct esp_display_presenter *presenter;
    bool                  imu_ok;
    bool                  imu_reported;
    /* Rotation diagnostics, shown on screen while the log stream is lossy. */
    bool                  presenter_owned;
    int                   rot_pause_err;
    int                   rot_set_err;
    int                   rot_resume_err;

    /* Requests raised by render-task callbacks, serviced by the app task. */
    volatile bool want_scan;
    volatile bool want_save;
    volatile bool want_back;
    volatile bool want_pair;
    volatile bool want_unpair;
    volatile bool want_match;
    volatile int  pending_pick;

    bool         setup_mode;
    bool         have_ssid;
    char         selected_ssid[APP_SSID_MAX];

    wifi_ap_t    aps[WIFI_SCAN_MAX];
    int          ap_count;

    bool         pc_have_sample;
    int64_t      sample_ms;
    pc_sample_t  sample;

    /* Rolling trend window shared by the detail pages: [metric][series][point]. */
    uint8_t      trend[PC_PANEL_METRIC_COUNT][PC_PANEL_SERIES_COUNT]
                      [PC_PANEL_TREND_POINTS];
    /* Scratch windows handed to pc_panel_show_detail (app-task private). */
    uint8_t      detail_first[PC_PANEL_TREND_POINTS];
    uint8_t      detail_second[PC_PANEL_TREND_POINTS];
    unsigned     trend_count;
    unsigned     trend_pos;
    volatile int want_detail;   /* 0 none, else pc_panel_metric_t + 1 */
    int          detail_metric; /* metric of the open trend page */
    unsigned     sample_log_divider;

    /* Device battery (BQ27220 on the shared I2C bus). -1 means "not readable
     * yet"; the gauge is attached lazily so a board without a cell still boots
     * and simply shows --. */
    bool         battery_ok;
    bool         battery_read_ok;
    int          battery_percent;
    int          ui_battery;    /* last value published to the UI (-2 = never) */
    int64_t      battery_next_ms;

    /* Last values pushed to the UI. ESP-GSP setters enqueue work and may
     * allocate temporary storage, so unchanged state is not re-published. */
    bool         ui_link_online;
    bool         ui_link_known;
    bool         ui_host_known;
    char         ui_wait_title[48];
    char         ui_wait_info[128];
    char         ui_wait_ip[24];
    char         ui_clock[16];
    /* Address last announced to the PC agent (see publish_identity). */
    char         link_ip[16];
    /* Pairing page: message shown after a failed attempt. */
    char         pair_error[64];
    char         ui_pair_hint[96];
    /* Pairing flow: false = intro page ("打开客户端"+匹配), true = code entry. */
    bool         pair_on_code;
    char         ui_match_sub[96];
    char         ui_match_ip[24];

    esp_gsp_err_t last_error;
} app_ctx_t;

static app_ctx_t s_app;

/* ------------------------------------------------------------- callbacks */

static void cb_open_setup(void *ctx)
{
    app_ctx_t *a = ctx;
    a->setup_mode = true;
    a->want_scan = true;
}

static void cb_scan(void *ctx)
{
    ((app_ctx_t *)ctx)->want_scan = true;
}

static void cb_pick_ap(void *ctx, int index)
{
    ((app_ctx_t *)ctx)->pending_pick = index;
}

static void cb_save(void *ctx)
{
    ((app_ctx_t *)ctx)->want_save = true;
}

static void cb_back(void *ctx)
{
    ((app_ctx_t *)ctx)->want_back = true;
}

static void cb_open_detail(void *ctx, int metric)
{
    ((app_ctx_t *)ctx)->want_detail = metric + 1;
}

static void cb_pair(void *ctx)
{
    ((app_ctx_t *)ctx)->want_pair = true;
}

static void cb_unpair(void *ctx)
{
    ((app_ctx_t *)ctx)->want_unpair = true;
}

static void cb_match(void *ctx)
{
    ((app_ctx_t *)ctx)->want_match = true;
}

/** Copy one rolling series oldest-first for the requested metric. */
static unsigned ordered_trend(app_ctx_t *a, int metric, int series, uint8_t *out)
{
    unsigned count = a->trend_count < PC_PANEL_TREND_POINTS
                     ? a->trend_count : PC_PANEL_TREND_POINTS;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned idx = (a->trend_pos + PC_PANEL_TREND_POINTS - count + i)
                             % PC_PANEL_TREND_POINTS;
        out[i] = a->trend[metric][series][idx];
    }
    return count;
}

/* ------------------------------------------------------------- helpers */

static void remember(esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        s_app.last_error = err;
    }
}

/*
 * Rotate the live UI: the presenter contract requires the producer to stop
 * submitting first, so the render task is paused, the rotation is
 * reconfigured (which rebuilds its storage/transforms) and then resumed; the
 * resumed frame is forced to repaint in full.
 */
static void apply_rotation(app_ctx_t *a, orientation_t angle)
{
    a->rot_pause_err = -1;
    a->rot_set_err = -1;
    a->rot_resume_err = -1;
    if (!a->presenter || !a->ui) {
        return;
    }
    esp_gsp_esp_lcd_pause_t *pause = NULL;
    esp_err_t err = esp_gsp_esp_lcd_pause(a->ui, 1000, &pause);
    a->rot_pause_err = (int)err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pause for rotation failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_display_presenter_set_rotation(
              a->presenter, (esp_display_present_rotation_t)angle);
    a->rot_set_err = (int)err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_rotation(%d) failed: %s", (int)angle,
                 esp_err_to_name(err));
    }

    esp_gsp_handle_t resumed = NULL;
    const esp_err_t resume_err = esp_gsp_esp_lcd_resume_paused(pause, &resumed);
    a->rot_resume_err = (int)resume_err;
    if (resume_err != ESP_OK) {
        ESP_LOGE(TAG, "resume after rotation failed: %s",
                 esp_err_to_name(resume_err));
        return;
    }
    if (resumed) {
        a->ui = resumed;
    }
    /* The resumed UI repaints everything, so re-publish the current state. */
    a->ui_link_known = false;
    a->ui_host_known = false;
    a->ui_wait_title[0] = '\0';
    a->ui_wait_info[0] = '\0';
    a->ui_wait_ip[0] = '\0';
    a->ui_clock[0] = '\0';
}

/* Publish the waiting page address line only when it changes. */
static void set_wait_ip(app_ctx_t *a, const char *text)
{
    if (strcmp(a->ui_wait_ip, text) == 0) {
        return;
    }
    strlcpy(a->ui_wait_ip, text, sizeof(a->ui_wait_ip));
    remember(pc_panel_set_wait_ip(a->ui, a->panel, text));
}

/* Publish the waiting page text only when it actually changes. */
static void set_wait(app_ctx_t *a, const char *title, const char *info)
{
    if (strcmp(a->ui_wait_title, title) == 0 &&
        strcmp(a->ui_wait_info, info) == 0) {
        return;
    }
    strlcpy(a->ui_wait_title, title, sizeof(a->ui_wait_title));
    strlcpy(a->ui_wait_info, info, sizeof(a->ui_wait_info));
    remember(pc_panel_set_wait(a->ui, a->panel, title, info));
}

/* Publish what the PC agent should show as "the device I reached". Called at
 * startup and whenever the DHCP lease changes. */
static void publish_identity(app_ctx_t *a)
{
    pc_link_identity_t id = {0};
    const esp_app_desc_t *app = esp_app_get_description();

    strlcpy(id.name, "PC 状态面板", sizeof(id.name));
    strlcpy(id.model, "ESP-Mosaico", sizeof(id.model));
    strlcpy(id.version, (app && app->version[0]) ? app->version : "1.0.0",
            sizeof(id.version));
    if (wifi_manager_ip(a->wifi, id.ip, sizeof(id.ip)) != ESP_OK) {
        strlcpy(id.ip, "0.0.0.0", sizeof(id.ip));
    }
    if (strcmp(id.ip, a->link_ip) != 0) {
        strlcpy(a->link_ip, id.ip, sizeof(a->link_ip));
        remember(pc_link_set_identity(a->link, &id));
    }
}

/* Refresh the device information page. It is a root-level sibling of the
 * dashboard, reached with a horizontal swipe, and is only republished while it
 * is the visible page. */
static void publish_info(app_ctx_t *a)
{
    if (pc_panel_page(a->panel) != PC_PANEL_PAGE_INFO) {
        return;
    }

    pc_panel_info_t info = {0};
    const esp_app_desc_t *app = esp_app_get_description();

    strlcpy(info.device_name, "PC 状态面板", sizeof(info.device_name));
    strlcpy(info.device_model, "ESP-Mosaico", sizeof(info.device_model));
    strlcpy(info.device_version, (app && app->version[0]) ? app->version : "-",
            sizeof(info.device_version));
    (void)wifi_manager_ip(a->wifi, info.device_ip, sizeof(info.device_ip));

    const int64_t uptime_s = esp_timer_get_time() / 1000000;
    const int days = (int)(uptime_s / 86400);
    const int hours = (int)((uptime_s % 86400) / 3600);
    const int minutes = (int)((uptime_s % 3600) / 60);
    if (days > 0) {
        snprintf(info.device_uptime, sizeof(info.device_uptime), "%d天%d小时",
                 days, hours);
    } else if (hours > 0) {
        snprintf(info.device_uptime, sizeof(info.device_uptime), "%d小时%d分",
                 hours, minutes);
    } else {
        snprintf(info.device_uptime, sizeof(info.device_uptime), "%d分钟",
                 minutes);
    }
    snprintf(info.device_heap, sizeof(info.device_heap), "%d KB",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    const int rssi = wifi_manager_rssi(a->wifi);
    if (rssi != 0) {
        snprintf(info.wifi, sizeof(info.wifi), "%d dBm", rssi);
    } else {
        strlcpy(info.wifi, "--", sizeof(info.wifi));
    }

    const int64_t age_ms = pc_link_age_ms(a->link);
    if (a->pc_have_sample && age_ms >= 0) {
        strlcpy(info.link_state, "已连接", sizeof(info.link_state));
    } else {
        strlcpy(info.link_state, "等待数据", sizeof(info.link_state));
    }
    strlcpy(info.pc_host, a->sample.host[0] ? a->sample.host : "--",
            sizeof(info.pc_host));
    strlcpy(info.pc_os, a->sample.os[0] ? a->sample.os : "--",
            sizeof(info.pc_os));
    (void)pc_link_peer(a->link, info.pc_ip, sizeof(info.pc_ip));
    snprintf(info.packets, sizeof(info.packets), "%u",
             (unsigned)pc_link_packets(a->link));

    remember(pc_panel_set_info(a->ui, a->panel, &info));
}

/* Status line of the pairing page: how many PCs are reachable, or the result
 * of the last attempt. */
static void update_pair_hint(app_ctx_t *a)
{
    char text[96];
    pc_link_peer_t peers[PC_LINK_PEER_MAX];
    const int count = pc_link_peers(a->link, peers, PC_LINK_PEER_MAX);

    if (a->pair_error[0]) {
        strlcpy(text, a->pair_error, sizeof(text));
    } else if (count == 0) {
        /* Nothing announced yet: the PC client is probably not running. */
        strlcpy(text, "正在等待电脑客户端…", sizeof(text));
    } else if (count == 1 && peers[0].host[0]) {
        /* Name the machine so the user can confirm they are typing the code
         * shown by the PC that is actually in front of them. */
        snprintf(text, sizeof(text), "已发现 %s 请输入配对码", peers[0].host);
    } else {
        snprintf(text, sizeof(text), "已发现 %d 台电脑 请输入配对码", count);
    }
    if (strcmp(text, a->ui_pair_hint) != 0) {
        strlcpy(a->ui_pair_hint, text, sizeof(a->ui_pair_hint));
        remember(pc_panel_set_pair_hint(a->ui, a->panel, text));
    }
}

/* Intro page: tell the user to open the PC client, and show this device's
 * address so the client's target field can be filled in. */
static void update_match_page(app_ctx_t *a)
{
    char ip[24] = "0.0.0.0";
    (void)wifi_manager_ip(a->wifi, ip, sizeof(ip));

    char text[96];
    pc_link_peer_t peers[PC_LINK_PEER_MAX];
    const int count = pc_link_peers(a->link, peers, PC_LINK_PEER_MAX);
    if (count == 1 && peers[0].host[0]) {
        snprintf(text, sizeof(text), "已发现 %s 请在客户端查看配对码",
                 peers[0].host);
    } else {
        strlcpy(text, "请在电脑上打开客户端", sizeof(text));
    }

    if (strcmp(text, a->ui_match_sub) != 0 ||
        strcmp(ip, a->ui_match_ip) != 0) {
        strlcpy(a->ui_match_sub, text, sizeof(a->ui_match_sub));
        strlcpy(a->ui_match_ip, ip, sizeof(a->ui_match_ip));
        remember(pc_panel_set_match_info(a->ui, a->panel, text, ip));
    }
}

static void run_scan(app_ctx_t *a)
{
    char current[33] = "";
    (void)wifi_manager_ssid(a->wifi, current, sizeof(current));

    remember(pc_panel_set_wifi_hint(a->ui, a->panel, "正在搜索网络…"));
    int count = 0;
    esp_err_t err = wifi_manager_scan(a->wifi, a->aps, WIFI_SCAN_MAX, &count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        a->ap_count = 0;
        remember(pc_panel_set_ap_list(a->ui, a->panel, a->aps, 0,
                                      "搜索失败，请点击重新搜索", current));
        return;
    }
    a->ap_count = count;
    if (count == 0) {
        remember(pc_panel_set_ap_list(a->ui, a->panel, a->aps, 0,
                                      "未发现网络，请点击重新搜索", current));
    } else {
        remember(pc_panel_set_ap_list(a->ui, a->panel, a->aps, count, NULL,
                                      current));
        remember(pc_panel_set_wifi_hint(a->ui, a->panel, "点击网络名称开始配置"));
    }
}

static void service_requests(app_ctx_t *a)
{
    if (a->want_pair) {
        a->want_pair = false;
        char code[PC_LINK_CODE_MAX] = "";
        remember(pc_panel_get_pair_code(a->ui, a->panel, code, sizeof(code)));
        if (code[0] == '\0') {
            strlcpy(a->pair_error, "请输入 4 位配对码", sizeof(a->pair_error));
        } else {
            const esp_err_t err = pc_link_pair(a->link, code);
            if (err == ESP_OK) {
                a->pair_error[0] = '\0';
                a->pair_on_code = false;   /* next time start from the intro */
                remember(pc_panel_clear_pair_code(a->ui, a->panel));
                ESP_LOGI(TAG, "paired with code %s", code);
            } else if (err == ESP_ERR_NOT_FOUND) {
                strlcpy(a->pair_error, "未找到该配对码的电脑 请核对",
                        sizeof(a->pair_error));
                remember(pc_panel_clear_pair_code(a->ui, a->panel));
                ESP_LOGW(TAG, "no PC announces code %s", code);
            } else {
                strlcpy(a->pair_error, "配对失败 请重试", sizeof(a->pair_error));
            }
        }
        a->ui_pair_hint[0] = '\0';
        update_pair_hint(a);
    }
    if (a->want_match) {
        a->want_match = false;
        a->pair_on_code = true;      /* 匹配 -> 配对码输入页 */
        a->pair_error[0] = '\0';
        a->ui_pair_hint[0] = '\0';
        remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_PAIR));
        update_pair_hint(a);
    }
    if (a->want_unpair) {
        a->want_unpair = false;
        (void)pc_link_unpair(a->link);
        remember(pc_panel_clear_pair_code(a->ui, a->panel));
        a->pair_error[0] = '\0';
        a->ui_pair_hint[0] = '\0';
        a->ui_host_known = false;
        /* Re-pair goes straight to the code entry page. */
        a->pair_on_code = true;
        remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_PAIR));
        update_pair_hint(a);
        ESP_LOGI(TAG, "pairing cleared");
    }
    if (a->want_scan) {
        a->want_scan = false;
        run_scan(a);
    }
    if (a->want_back) {
        a->want_back = false;
        a->pending_pick = -1;
        const pc_panel_page_t page = pc_panel_page(a->panel);
        if (pc_panel_showing_detail(a->panel) || page == PC_PANEL_PAGE_INFO) {
            /* Leave the trend / information page; refresh() picks the page. */
            remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_DASH));
        } else if (page == PC_PANEL_PAGE_PASS) {
            a->setup_mode = true;    /* back to the network list */
            remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_WIFI));
        } else if (page == PC_PANEL_PAGE_DASH) {
            /* Horizontal swipes on the dashboard open the information page;
             * it is not "back", but the two root pages behave like a pair. */
            remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_INFO));
        } else {
            a->setup_mode = false;   /* leave provisioning */
        }
    }
    if (a->want_detail) {
        const int metric = a->want_detail - 1;
        a->want_detail = 0;
        if (metric >= 0 && metric < PC_PANEL_METRIC_COUNT) {
            const unsigned count =
                ordered_trend(a, metric, PC_PANEL_SERIES_FIRST, a->detail_first);
            (void)ordered_trend(a, metric, PC_PANEL_SERIES_SECOND,
                                a->detail_second);
            a->detail_metric = metric;
            remember(pc_panel_show_detail(a->ui, a->panel,
                                          (pc_panel_metric_t)metric,
                                          a->detail_first, a->detail_second,
                                          count, &a->sample));
        }
    }
    int pick = a->pending_pick;
    if (pick >= 0 && pick < a->ap_count) {
        a->pending_pick = -1;
        strlcpy(a->selected_ssid, a->aps[pick].ssid, sizeof(a->selected_ssid));
        a->have_ssid = a->selected_ssid[0] != '\0';
        remember(pc_panel_set_selected_ssid(a->ui, a->panel, a->selected_ssid));
        remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_PASS));
    }
    if (a->want_save) {
        a->want_save = false;
        char password[64] = {0};
        remember(pc_panel_get_password(a->ui, a->panel, password,
                                       sizeof(password)));
        if (!a->have_ssid) {
            remember(pc_panel_set_wifi_hint(a->ui, a->panel, "请先选择网络"));
        } else {
            ESP_LOGI(TAG, "connecting to \"%s\"", a->selected_ssid);
            set_wait(a, "正在连接无线网络", a->selected_ssid);
            remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_WAIT));
            if (wifi_manager_connect(a->wifi, a->selected_ssid, password) != ESP_OK) {
                set_wait(a, "连接失败", "请重新配置无线网络");
            }
            a->setup_mode = false;
        }
    }
}

static void update_clock(app_ctx_t *a)
{
    char text[16] = "";

    if (!a->pc_have_sample || a->sample.pc_epoch_s <= 0) {
        /* leave empty */
    } else if (esp_timer_get_time() / 1000 - a->sample_ms > APP_CLOCK_STALE_MS) {
        strlcpy(text, "--:--", sizeof(text));
    } else {
        int64_t seconds = a->sample.pc_epoch_s +
                          (esp_timer_get_time() / 1000 - a->sample_ms) / 1000;
        struct tm tm;
        time_t t = (time_t)seconds;
        gmtime_r(&t, &tm);
        snprintf(text, sizeof(text), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min,
                 tm.tm_sec);
    }

    if (strcmp(text, a->ui_clock) != 0) {
        strlcpy(a->ui_clock, text, sizeof(a->ui_clock));
        remember(pc_panel_set_clock(a->ui, a->panel, text));
    }
}

static void refresh(app_ctx_t *a)
{
    pc_sample_t sample;
    bool have_sample = pc_link_snapshot(a->link, &sample) == ESP_OK;
    bool sample_changed = false;

    if (have_sample &&
        (!a->pc_have_sample || memcmp(&sample, &a->sample, sizeof(sample)) != 0)) {
        a->sample = sample;
        a->sample_ms = esp_timer_get_time() / 1000;
        sample_changed = true;
    }
    a->pc_have_sample = a->pc_have_sample || have_sample;
    bool pc_online = pc_link_state(a->link) == PC_LINK_ONLINE;

    /* Only republish what changed: every setter costs a command and may
     * allocate inside the framework. */
    if (!a->ui_link_known || a->ui_link_online != pc_online) {
        a->ui_link_known = true;
        a->ui_link_online = pc_online;
        remember(pc_panel_set_link(a->ui, a->panel, pc_online));
    }

    if (pc_online && a->pc_have_sample) {
        if (!a->ui_host_known) {
            a->ui_host_known = true;
            remember(pc_panel_set_host(
                         a->ui, a->panel,
                         a->sample.host[0] ? a->sample.host : "我的电脑",
                         a->sample.os[0] ? a->sample.os : "电脑"));
        }
        if (sample_changed) {
            /* Keep the retained log ring useful: one summary line per 10 s
             * instead of one per sample. */
            if (++a->sample_log_divider >= 10) {
                a->sample_log_divider = 0;
                ESP_LOGI(TAG,
                         "sample cpu=%d%% mem=%.1f/%.0f net=%.1f/%.1f cpu_temp=%d "
                         "cpu_power=%.0f gpu=%d vram=%.1f/%.1f",
                         a->sample.cpu, (double)a->sample.mem_used_gb,
                         (double)a->sample.mem_total_gb,
                         (double)a->sample.net_down_mbps,
                         (double)a->sample.net_up_mbps,
                         a->sample.has_cpu_temp ? a->sample.cpu_temp : -1,
                         a->sample.has_cpu_power
                             ? (double)a->sample.cpu_power : -1.0,
                         a->sample.has_gpu ? a->sample.gpu : -1,
                         (double)a->sample.gpu_mem_used_gb,
                         (double)a->sample.gpu_mem_total_gb);
            }
            /* Feed the trend windows used by the detail pages. */
            pc_metric_values_t live[PC_PANEL_METRIC_COUNT];
            for (int m = 0; m < PC_PANEL_METRIC_COUNT; ++m) {
                pc_panel_metric_values(&a->sample, (pc_panel_metric_t)m, &live[m]);
                a->trend[m][PC_PANEL_SERIES_FIRST][a->trend_pos] =
                    (uint8_t)live[m].first;
                a->trend[m][PC_PANEL_SERIES_SECOND][a->trend_pos] =
                    (uint8_t)live[m].second;
            }
            a->trend_pos = (a->trend_pos + 1) % PC_PANEL_TREND_POINTS;
            if (a->trend_count < PC_PANEL_TREND_POINTS) {
                a->trend_count++;
            }
            if (pc_panel_showing_detail(a->panel) &&
                a->detail_metric >= 0 && a->detail_metric < PC_PANEL_METRIC_COUNT) {
                remember(pc_panel_detail_append(a->ui, a->panel, &a->sample,
                                                &live[a->detail_metric]));
            }
#if DEBUG_OPEN_DETAIL_ON_BOOT
            /* Screenshot aid: open each page in turn so captures need no
             * physical touch. Must stay disabled in delivered builds. */
            static int debug_step;
            static const int64_t debug_at[] = {15, 75, 135, 195, 255, 315};
            const int64_t debug_now = esp_timer_get_time() / 1000000;
            if (debug_step < 6 && debug_now >= debug_at[debug_step]) {
                if (debug_step == 0) {
                    /* The Wi-Fi page is forced by setup_mode, not by set_page,
                     * because refresh() sends an online panel back to the
                     * dashboard. Kick a scan so the list is populated. */
                    a->setup_mode = true;
                    a->want_scan = true;
                } else if (debug_step == 1) {
                    remember(pc_panel_set_page(a->ui, a->panel,
                                               PC_PANEL_PAGE_INFO));
                } else {
                    a->want_detail = (int)PC_PANEL_METRIC_CPU + debug_step - 2;
                }
                debug_step++;
            }
#endif
            remember(pc_panel_set_host(
                         a->ui, a->panel,
                         a->sample.host[0] ? a->sample.host : "我的电脑",
                         a->sample.os[0] ? a->sample.os : "电脑"));
            remember(pc_panel_update(a->ui, a->panel, &a->sample));
        }
    } else {
        a->ui_host_known = false;
    }
    update_clock(a);

#if DEBUG_SHOW_IMU
    /* Screen readout beats the log stream, which drops bytes on this board. */
    {
        float ax = 0.0f;
        float ay = 0.0f;
        float az = 0.0f;
        int angle = ORIENTATION_UNKNOWN;
        if (orientation_snapshot(&ax, &ay, &az, &angle)) {
            char text[48];
            snprintf(text, sizeof(text), "n%d P%d %d %d %d", angle,
                     a->presenter_owned ? 1 : 0, a->rot_pause_err,
                     a->rot_set_err, a->rot_resume_err);
            remember(pc_panel_set_host(a->ui, a->panel, text,
                                       a->sample.os[0] ? a->sample.os : ""));
        }
    }
#endif

    /* Not bound to a PC yet: the pairing page owns the screen (Wi-Fi first). */
    if (pc_panel_page(a->panel) == PC_PANEL_PAGE_INFO) {
        publish_info(a);
        return;    /* stays until the user swipes back */
    }

    if (a->setup_mode) {
        /* Provisioning owns the screen: show the network list unless the user
         * is already entering a password. */
        if (pc_panel_page(a->panel) != PC_PANEL_PAGE_PASS) {
            remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_WIFI));
        }
        return;
    }

    if (pc_panel_showing_detail(a->panel)) {
        return;    /* the trend page stays until the user swipes back */
    }

    /* An address means we are on the network even if the manager was left in
     * FAILED/IDLE by a boot-time race, so trust the address as well. */
    char ip[24] = "0.0.0.0";
    const bool have_ip = wifi_manager_ip(a->wifi, ip, sizeof(ip)) == ESP_OK &&
                         strcmp(ip, "0.0.0.0") != 0;
    wifi_state_t wifi = wifi_manager_state(a->wifi);
    if (wifi != WIFI_STATE_CONNECTED && have_ip) {
        wifi = WIFI_STATE_CONNECTED;
    }
    if (wifi != WIFI_STATE_CONNECTED) {
        if (wifi == WIFI_STATE_CONNECTING) {
            set_wait(a, "正在连接无线网络", "请稍候…");
        } else if (wifi == WIFI_STATE_FAILED) {
            set_wait(a, "无线网络连接失败", "点击下方按钮重新配置网络");
        } else {
            set_wait(a, "尚未配置无线网络",
                     "点击下方按钮选择 2.4G 网络并输入密码");
        }
        set_wait_ip(a, "");
        remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_WAIT));
        return;
    }

    if (pc_online) {
        remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_DASH));
        return;
    }

    /*
     * Wi-Fi is up but no PC is bound yet: first ask the user to open the PC
     * client ("匹配"), then take the code it shows.
     */
    if (!pc_link_paired(a->link)) {
        if (a->pair_on_code) {
            remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_PAIR));
            update_pair_hint(a);
        } else {
            remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_MATCH));
            update_match_page(a);
        }
        return;
    }

    set_wait(a, "正在等待电脑…", "请确认电脑端上报程序已启动，并处于同一局域网");
    char ip_line[32];
    snprintf(ip_line, sizeof(ip_line), "本机 %s", ip);
    set_wait_ip(a, ip_line);
    remember(pc_panel_set_page(a->ui, a->panel, PC_PANEL_PAGE_WAIT));
}

/* Poll the BQ27220 fuel gauge on the shared I2C bus. The first call attaches
 * it; a failed attach or read keeps the previous value (or "--" when nothing
 * has been read yet), so a board without a battery simply boots and shows --
 * instead of holding up start-up. */
#define APP_BATTERY_PERIOD_MS 15000

static void poll_battery(app_ctx_t *a)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < a->battery_next_ms) {
        return;
    }
    a->battery_next_ms = now_ms + APP_BATTERY_PERIOD_MS;

    int percent = a->battery_percent;
    if (!a->battery_ok && bsp_battery_init() == ESP_OK) {
        a->battery_ok = true;
        ESP_LOGI(TAG, "battery gauge attached");
    }
    if (a->battery_ok) {
        bsp_battery_status_t status;
        const bool read_ok = bsp_battery_read(&status) == ESP_OK;
        if (read_ok) {
            if ((int)status.state_of_charge != percent) {
                ESP_LOGI(TAG, "battery %u%% (%d mV, %d mA, %.1f C)",
                         (unsigned)status.state_of_charge, status.voltage_mv,
                         status.current_ma, status.temperature_c);
            }
            percent = status.state_of_charge;
        } else if (a->battery_read_ok) {
            /* Log the transition only: the retained log ring is small. */
            ESP_LOGW(TAG, "battery read failed");
        }
        a->battery_read_ok = read_ok;
    }

    a->battery_percent = percent;
    if (percent != a->ui_battery) {
        remember(pc_panel_set_battery(a->ui, a->panel, percent));
        a->ui_battery = percent;
    }
}

static void app_task(void *arg)
{
    app_ctx_t *a = arg;
    orientation_t angle = ORIENTATION_UNKNOWN;
    bool angle_changed = false;

    while (true) {
        service_requests(a);
        refresh(a);
#if DEBUG_AUTO_PAIR
        /* Delayed so the pairing page can be captured before it is bound; fires
         * once only, unlike the real "re-pair" path which clears the binding. */
        static bool auto_paired;
        if (!auto_paired && !pc_link_paired(a->link) &&
            esp_timer_get_time() > 25000000) {
            pc_link_peer_t peers[PC_LINK_PEER_MAX];
            if (pc_link_peers(a->link, peers, PC_LINK_PEER_MAX) > 0 &&
                peers[0].code[0] &&
                pc_link_pair(a->link, peers[0].code) == ESP_OK) {
                auto_paired = true;
                ESP_LOGW(TAG, "debug: auto-paired with code %s", peers[0].code);
            }
        }
#endif
#if DEBUG_FORCE_UNPAIR
        {
            /* Exercises exactly the path the "re-pair" button takes. */
            static bool unpaired;
            if (!unpaired && esp_timer_get_time() > 40000000) {
                unpaired = true;
                a->want_unpair = true;
                ESP_LOGW(TAG, "debug: requested unpair");
            }
        }
#endif
        /* Cheap when nothing changed: it only republishes after a lease change. */
        publish_identity(a);

        /* Always poll: the sensor also re-initialises itself here when the
         * bring-up at boot could not reach it. */
        if (orientation_poll(&angle, &angle_changed) == ESP_OK &&
            angle_changed && angle != ORIENTATION_UNKNOWN) {
            apply_rotation(a, angle);
        }

#if DEBUG_FORCE_ROTATION
        {
            static int rot_step;
            const int64_t rot_sec = esp_timer_get_time() / 1000000;
            if (rot_step == 0 && rot_sec >= 60) {
                rot_step = 1;
                ESP_LOGW(TAG, "debug: forcing rotation 90");
                apply_rotation(a, ORIENTATION_90);
            } else if (rot_step == 1 && rot_sec >= 120) {
                rot_step = 2;
                ESP_LOGW(TAG, "debug: forcing rotation 0");
                apply_rotation(a, ORIENTATION_0);
            }
        }
#endif

        /* Report the sensor state once, after the boot-time log flood has
         * passed, so the retained log ring keeps the line. */
        if (!a->imu_reported && esp_timer_get_time() > 12000000) {
            a->imu_reported = true;
            ESP_LOGI(TAG, "orientation sensor: %s",
                     orientation_ready() ? "ready" : "unavailable");
        }

        heap_watch_poll();
        poll_battery(a);
        vTaskDelay(pdMS_TO_TICKS(APP_TICK_MS));
    }
}

/* ------------------------------------------------------------- app_main */

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    iris_ota_support_start();

    /* Battery is unknown until the first gauge poll; -2 forces that first push
     * even when the gauge reports 0 %. */
    s_app.battery_percent = -1;
    s_app.ui_battery = -2;

    /*
     * The IMU is brought up before the display: bsp_imu_init() re-asserts the
     * shared VCC_3V3 rail, which also feeds the panel and PSRAM, so doing it
     * while the UI is live destabilises the system.
     */
#if APP_USE_IMU
    s_app.imu_ok = orientation_start() == ESP_OK;
    if (!s_app.imu_ok) {
        ESP_LOGW(TAG, "IMU unavailable; screen rotation disabled");
    }
#else
    s_app.imu_ok = false;
#endif

    esp_gsp_config_t app_config;
    const esp_err_t bundle_err = ui_bundle_open(&app_config);
    if (bundle_err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC stays active",
                 bundle_err);
        return;
    }

    esp_display_present_target_config_t display;
    ESP_ERROR_CHECK(board_display_init(&display));

    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(board_touch_init(&touch));

    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    /*
     * Runtime rotation needs the application to own the display presenter.
     * Taking over presenter creation destabilised boot on this ESP-GSP build
     * (abort in the Wi-Fi task), so it stays off until the correct presenter
     * parameters are known; the display target path below is the known-good
     * bring-up and orientation handling simply becomes a no-op.
     */
    struct esp_display_presenter *presenter = NULL;
#if APP_OWN_PRESENTER
    if (board_presenter_create(&display, &presenter) == ESP_OK) {
        lcd.presenter = presenter;
        ESP_LOGI(TAG, "display presenter owned; IMU rotation available");
    } else {
        lcd.display = display;
        ESP_LOGW(TAG, "presenter unavailable; using display target (no rotation)");
    }
#else
    lcd.display = display;
#endif
    lcd.touch = touch;

    ESP_ERROR_CHECK(iris_screen_mirror_init());

    esp_gsp_handle_t ui;
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&app_config, &lcd, &ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(ui));

    s_app = (app_ctx_t){.ui = ui, .pending_pick = -1, .presenter = presenter};

    const pc_panel_callbacks_t callbacks = {
        .on_open_setup = cb_open_setup,
        .on_scan = cb_scan,
        .on_pick_ap = cb_pick_ap,
        .on_save = cb_save,
        .on_back = cb_back,
        .on_open_detail = cb_open_detail,
        .on_pair = cb_pair,
        .on_unpair = cb_unpair,
        .on_match = cb_match,
    };
    /* Non-fatal initialisation: log the exact stage/code and keep running so
     * the device stays reachable and diagnosable instead of panicking. */
    esp_gsp_err_t panel_err = pc_panel_create(ui, &callbacks, &s_app,
                                              &s_app.panel);
    if (panel_err != ESP_GSP_OK) {
        ESP_LOGE(TAG, "UI panel init failed at '%s': 0x%x (degraded)",
                 pc_panel_last_stage(), panel_err);
    }

    esp_err_t err = wifi_manager_create(&s_app.wifi);
    if (err == ESP_OK) {
        err = wifi_manager_start(s_app.wifi);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi manager init failed: %s", esp_err_to_name(err));
    } else {
        s_app.setup_mode = !wifi_manager_has_credentials(s_app.wifi);
        if (s_app.setup_mode) {
            s_app.want_scan = true;
        }
    }

    err = pc_link_create(&s_app.link, PC_LINK_DEFAULT_PORT);
    if (err == ESP_OK) {
        err = pc_link_start(s_app.link);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status link init failed: %s", esp_err_to_name(err));
    }

    if (!s_app.panel) {
        ESP_LOGE(TAG, "no UI panel available; refresh task not started");
    } else if (xTaskCreate(app_task, "pc_ui", APP_TASK_STACK, &s_app,
                           APP_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start UI task");
    }

    ESP_LOGI(TAG, "PC status panel ready at %dx%d RGB565", BSP_LCD_H_RES,
             BSP_LCD_V_RES);
}
