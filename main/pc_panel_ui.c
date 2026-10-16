// SPDX-License-Identifier: Apache-2.0

/* The scene exposes stable raw object/bind/action keys; the keyboard
 * attachment and event dispatch rely on them (see the ESP-GSP keyboard guide). */
#define GSP_BUNDLE_ENABLE_RAW_IDS 1

#include "pc_panel_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bundle_gsp.h"
#include "esp_gsp.h"

/* Horizontal swipes navigate the application's own page stack, so the scene
 * stays a single scene and the framework's scene-swipe stays unused. */
#define SWIPE_CLAIM_PX  24   /* clearly horizontal: consume samples */
#define SWIPE_COMMIT_PX 70   /* travel that commits a page change */

/* Digits accepted on the pairing page (the PC announces a 4-digit code). */
#define PC_PANEL_PAIR_MAX 4

/*
 * ESP-GSP only exposes a runtime property for state the scene declared as a
 * bind with the matching target, so every value this module writes has an
 * authored `bind`/`bind_target` pair and goes through the bind-based setters.
 * Progress `value` is the exception: it is the control's canonical value.
 */

struct pc_panel {
    pc_panel_callbacks_t cbs;
    void                *ctx;
    pc_panel_page_t      page;
    /* Trend page state. */
    pc_panel_metric_t    detail_metric;
    int                  detail_lowest[PC_PANEL_SERIES_COUNT];
    int                  detail_highest[PC_PANEL_SERIES_COUNT];
    /* Horizontal-swipe tracking for page navigation. */
    int32_t              swipe_x0;
    int32_t              swipe_y0;
    int32_t              swipe_last_x;
    int32_t              swipe_last_y;
    bool                 swipe_tracking;
    bool                 swipe_claiming;
    bool                 swipe_fired;
    /* Digits typed on the pairing page's numeric keypad (UI state). */
    char                 pair_code[PC_PANEL_PAIR_MAX + 1];
};

/* Last step attempted by a fallible entry point, for caller-side diagnostics. */
static const char *s_stage = "";

static esp_gsp_err_t fail(const char *stage, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        s_stage = stage;
    }
    return err;
}

const char *pc_panel_last_stage(void)
{
    return s_stage;
}

/* ------------------------------------------------------------- helpers */

static esp_gsp_err_t put_text(esp_gsp_handle_t ui, uint16_t bind,
                              const char *text)
{
    return esp_gsp_set_text(ui, bind, text ? text : "");
}

static esp_gsp_err_t put_visible(esp_gsp_handle_t ui, uint16_t bind, bool visible)
{
    return esp_gsp_set_visible(ui, bind, visible);
}

static esp_gsp_err_t put_value(esp_gsp_handle_t ui, gsp_component_key_t key,
                               int32_t value)
{
    return esp_gsp_component_set_value(ui, key, value);
}

static void format_gb(char *out, size_t capacity, float value)
{
    if (value >= 100.0f) {
        snprintf(out, capacity, "%.0f", value);
    } else {
        snprintf(out, capacity, "%.1f", value);
    }
}

/* ------------------------------------------------------------- pages */

static esp_gsp_err_t apply_page(esp_gsp_handle_t ui, pc_panel_t *p)
{
    esp_gsp_err_t err = put_visible(ui, GSP_PC_BIND_PAGE_WAIT,
                                    p->page == PC_PANEL_PAGE_WAIT);
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_PAGE_WIFI,
                          p->page == PC_PANEL_PAGE_WIFI);
    }
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_PAGE_PASS,
                          p->page == PC_PANEL_PAGE_PASS);
    }
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_DETAIL_CPU_PAGE,
                          p->page == PC_PANEL_PAGE_DETAIL_CPU);
    }
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_DETAIL_MEM_PAGE,
                          p->page == PC_PANEL_PAGE_DETAIL_MEM);
    }
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_DETAIL_GPU_PAGE,
                          p->page == PC_PANEL_PAGE_DETAIL_GPU);
    }
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_DETAIL_NET_PAGE,
                          p->page == PC_PANEL_PAGE_DETAIL_NET);
    }
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_INFO_PAGE,
                          p->page == PC_PANEL_PAGE_INFO);
    }
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_PAIR_PAGE,
                          p->page == PC_PANEL_PAGE_PAIR);
    }
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_MATCH_PAGE,
                          p->page == PC_PANEL_PAGE_MATCH);
    }
    if (err != ESP_GSP_OK) {
        return err;
    }
    /*
     * Software keyboards belong to the page that shows them and take no bind of
     * their own, so the keyboard of the visible page is attached here: the
     * password page's for the password field, the pairing page's for the code.
     */
    switch (p->page) {
    case PC_PANEL_PAGE_PASS:
        return esp_gsp_keyboard_attach(ui, GSP_PC_ACT_ID_KBD_KEY,
                                       GSP_PC_BIND_PASS_TEXT);
    default:
        /* The pairing page uses its own numeric keypad, so no keyboard is
         * attached there. */
        return esp_gsp_keyboard_attach(ui, ESP_GSP_KEYBOARD_NONE, 0);
    }
}

/* ------------------------------------------------------------- events */

static void on_ui_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    pc_panel_t *p = ctx;
    if (!p || !event || event->type != ESP_GSP_EVENT_CALL) {
        return;
    }

    const pc_panel_callbacks_t *cb = &p->cbs;

    /* Numeric keypad of the pairing page: append / delete a digit. */
    static const uint16_t digit_ids[10] = {
        GSP_PC_ACT_ID_PAIR_DIGIT_0, GSP_PC_ACT_ID_PAIR_DIGIT_1,
        GSP_PC_ACT_ID_PAIR_DIGIT_2, GSP_PC_ACT_ID_PAIR_DIGIT_3,
        GSP_PC_ACT_ID_PAIR_DIGIT_4, GSP_PC_ACT_ID_PAIR_DIGIT_5,
        GSP_PC_ACT_ID_PAIR_DIGIT_6, GSP_PC_ACT_ID_PAIR_DIGIT_7,
        GSP_PC_ACT_ID_PAIR_DIGIT_8, GSP_PC_ACT_ID_PAIR_DIGIT_9,
    };
    for (int i = 0; i < 10; ++i) {
        if (event->action_id != digit_ids[i]) {
            continue;
        }
        const size_t len = strlen(p->pair_code);
        if (len < PC_PANEL_PAIR_MAX) {
            p->pair_code[len] = (char)('0' + i);
            p->pair_code[len + 1] = '\0';
        }
        (void)put_text(ui, GSP_PC_BIND_PAIR_TEXT, p->pair_code);
        return;
    }
    if (event->action_id == GSP_PC_ACT_ID_PAIR_DELETE) {
        const size_t len = strlen(p->pair_code);
        if (len > 0) {
            p->pair_code[len - 1] = '\0';
        }
        (void)put_text(ui, GSP_PC_BIND_PAIR_TEXT, p->pair_code);
        return;
    }

    if (event->action_id == GSP_PC_ACT_ID_OPEN_SETUP && cb->on_open_setup) {
        cb->on_open_setup(p->ctx);
        return;
    }
    if (event->action_id == GSP_PC_ACT_ID_WIFI_SCAN && cb->on_scan) {
        cb->on_scan(p->ctx);
        return;
    }
    if (event->action_id == GSP_PC_ACT_ID_WIFI_SAVE && cb->on_save) {
        cb->on_save(p->ctx);
        return;
    }
    if (event->action_id == GSP_PC_ACT_ID_WIFI_BACK && cb->on_back) {
        cb->on_back(p->ctx);
        return;
    }
    if (cb->on_open_detail) {
        if (event->action_id == GSP_PC_ACT_ID_OPEN_CPU) {
            cb->on_open_detail(p->ctx, PC_PANEL_METRIC_CPU);
            return;
        }
        if (event->action_id == GSP_PC_ACT_ID_OPEN_MEM) {
            cb->on_open_detail(p->ctx, PC_PANEL_METRIC_MEM);
            return;
        }
        if (event->action_id == GSP_PC_ACT_ID_OPEN_GPU) {
            cb->on_open_detail(p->ctx, PC_PANEL_METRIC_GPU);
            return;
        }
        if (event->action_id == GSP_PC_ACT_ID_OPEN_NET) {
            cb->on_open_detail(p->ctx, PC_PANEL_METRIC_NET);
            return;
        }
    }
    if (event->action_id == GSP_PC_ACT_ID_PAIR_CONFIRM && cb->on_pair) {
        cb->on_pair(p->ctx);
        return;
    }
    if (event->action_id == GSP_PC_ACT_ID_PAIR_START && cb->on_match) {
        cb->on_match(p->ctx);
        return;
    }
    if (event->action_id == GSP_PC_ACT_ID_PAIR_RESET && cb->on_unpair) {
        cb->on_unpair(p->ctx);
        return;
    }
    if (cb->on_pick_ap) {
        static const uint16_t pick_ids[WIFI_SCAN_MAX] = {
            GSP_PC_ACT_ID_AP_PICK_0, GSP_PC_ACT_ID_AP_PICK_1,
            GSP_PC_ACT_ID_AP_PICK_2, GSP_PC_ACT_ID_AP_PICK_3,
            GSP_PC_ACT_ID_AP_PICK_4, GSP_PC_ACT_ID_AP_PICK_5,
        };
        for (int i = 0; i < WIFI_SCAN_MAX; ++i) {
            if (event->action_id == pick_ids[i]) {
                cb->on_pick_ap(p->ctx, i);
                return;
            }
        }
    }
}

/* ------------------------------------------------------------- swipe */

/*
 * A horizontal swipe always means "one step back", matching the on-screen
 * hint. The application owns the page stack, so forward navigation stays on
 * explicit controls (the dashboard's 设置 button, the waiting page's button).
 */
static void request_navigation(pc_panel_t *p)
{
    if (p->cbs.on_back) {
        p->cbs.on_back(p->ctx);
    }
}

/*
 * Top-level input interceptor: reports every mapped touch sample before scene
 * hit-testing. A clearly horizontal drag is claimed (and consumed) so buttons
 * underneath do not fire, and commits one step back once it travels far enough.
 * A plain tap is never consumed, so controls keep working normally.
 */
static bool on_input(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed,
                     void *ctx)
{
    (void)ui;
    pc_panel_t *p = ctx;
    if (!p) {
        return false;
    }

    if (pressed) {
        if (!p->swipe_tracking) {
            p->swipe_tracking = true;
            p->swipe_claiming = false;
            p->swipe_fired = false;
            p->swipe_x0 = x;
            p->swipe_y0 = y;
        }
        p->swipe_last_x = x;
        p->swipe_last_y = y;

        const int32_t dx = x - p->swipe_x0;
        const int32_t dy = y - p->swipe_y0;
        if (!p->swipe_claiming && abs(dx) >= SWIPE_CLAIM_PX && abs(dx) > abs(dy)) {
            p->swipe_claiming = true;
        }
        if (p->swipe_claiming) {
            /* Commit as soon as the distance is reached so navigation feels
             * immediate even if no release sample follows a fast flick. */
            if (!p->swipe_fired && abs(dx) >= SWIPE_COMMIT_PX) {
                p->swipe_fired = true;
                request_navigation(p);
            }
            return true;
        }
        return false;
    }

    const bool consumed = p->swipe_claiming;
    if (p->swipe_tracking && !p->swipe_fired) {
        const int32_t dx = p->swipe_last_x - p->swipe_x0;
        const int32_t dy = p->swipe_last_y - p->swipe_y0;
        if (abs(dx) >= SWIPE_COMMIT_PX && abs(dx) > abs(dy)) {
            request_navigation(p);
        }
    }
    p->swipe_tracking = false;
    p->swipe_claiming = false;
    p->swipe_fired = false;
    return consumed;
}

/* ------------------------------------------------------------- public */

esp_gsp_err_t pc_panel_create(esp_gsp_handle_t ui, const pc_panel_callbacks_t *cbs,
                              void *ctx, pc_panel_t **ret_panel)
{
    if (!ui || !cbs || !ret_panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }

    pc_panel_t *p = calloc(1, sizeof(struct pc_panel));
    if (!p) {
        return fail("alloc", ESP_GSP_ERR_NO_MEM);
    }
    p->cbs = *cbs;
    p->ctx = ctx;
    p->page = PC_PANEL_PAGE_DASH;
    s_stage = "";

    esp_gsp_err_t err = esp_gsp_keyboard_attach(ui, GSP_PC_ACT_ID_KBD_KEY,
                                                GSP_PC_BIND_PASS_TEXT);
    if (err != ESP_GSP_OK) {
        free(p);
        return fail("keyboard_attach", err);
    }

    err = put_visible(ui, GSP_PC_BIND_LINK_DOT_OK, false);
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_LINK_DOT_BAD, true);
    }
    if (err != ESP_GSP_OK) {
        (void)esp_gsp_keyboard_attach(ui, ESP_GSP_KEYBOARD_NONE, 0);
        free(p);
        return fail("dot_visibility", err);
    }
    err = esp_gsp_on_event(ui, on_ui_event, p);
    if (err != ESP_GSP_OK) {
        (void)esp_gsp_keyboard_attach(ui, ESP_GSP_KEYBOARD_NONE, 0);
        free(p);
        return fail("event_registration", err);
    }

    err = esp_gsp_set_input_interceptor(ui, on_input, p);
    if (err != ESP_GSP_OK) {
        (void)esp_gsp_on_event(ui, NULL, NULL);
        (void)esp_gsp_keyboard_attach(ui, ESP_GSP_KEYBOARD_NONE, 0);
        free(p);
        return fail("input_interceptor", err);
    }

    err = apply_page(ui, p);
    if (err != ESP_GSP_OK) {
        (void)esp_gsp_keyboard_attach(ui, ESP_GSP_KEYBOARD_NONE, 0);
        free(p);
        return fail("page_visibility", err);
    }

    *ret_panel = p;
    return ESP_GSP_OK;
}

void pc_panel_destroy(esp_gsp_handle_t ui, pc_panel_t *panel)
{
    if (!panel) {
        return;
    }
    if (ui) {
        (void)esp_gsp_set_input_interceptor(ui, NULL, NULL);
        (void)esp_gsp_on_event(ui, NULL, NULL);
        (void)esp_gsp_keyboard_attach(ui, ESP_GSP_KEYBOARD_NONE, 0);
    }
    free(panel);
}

esp_gsp_err_t pc_panel_set_page(esp_gsp_handle_t ui, pc_panel_t *panel,
                                pc_panel_page_t page)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    if (panel->page == page) {
        return ESP_GSP_OK;
    }
    panel->page = page;
    return apply_page(ui, panel);
}

pc_panel_page_t pc_panel_page(const pc_panel_t *panel)
{
    return panel ? panel->page : PC_PANEL_PAGE_DASH;
}

esp_gsp_err_t pc_panel_set_link(esp_gsp_handle_t ui, pc_panel_t *panel,
                                bool online)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    /* The dot in front of the host name is the whole indicator: green when
     * samples arrive, red when they stop. */
    esp_gsp_err_t err = put_visible(ui, GSP_PC_BIND_LINK_DOT_OK, online);
    if (err == ESP_GSP_OK) {
        err = put_visible(ui, GSP_PC_BIND_LINK_DOT_BAD, !online);
    }
    return err;
}

esp_gsp_err_t pc_panel_set_host(esp_gsp_handle_t ui, pc_panel_t *panel,
                                const char *name, const char *subtitle)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    esp_gsp_err_t err = put_text(ui, GSP_PC_BIND_HOST_NAME, name);
    if (err == ESP_GSP_OK) {
        err = put_text(ui, GSP_PC_BIND_HOST_SUB, subtitle);
    }
    return err;
}

esp_gsp_err_t pc_panel_update(esp_gsp_handle_t ui, pc_panel_t *panel,
                              const pc_sample_t *sample)
{
    if (!ui || !panel || !sample) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    char value[32];
    char note[48];
    esp_gsp_err_t err;

    snprintf(value, sizeof(value), "%d", sample->cpu);
    err = put_text(ui, GSP_PC_BIND_CPU_VAL, value);
    if (err == ESP_GSP_OK) {
        err = put_value(ui, GSP_PC_OBJ_KEY_CPU_BAR, sample->cpu);
    }
    if (err == ESP_GSP_OK) {
        /* The badge only exists when the PC actually reports a temperature
         * sensor; an empty label would draw a placeholder glyph instead. */
        err = esp_gsp_component_set_visible(ui, GSP_PC_OBJ_KEY_CPU_TEMP,
                                            sample->has_cpu_temp);
    }
    if (err == ESP_GSP_OK && sample->has_cpu_temp) {
        snprintf(value, sizeof(value), "%d°C", sample->cpu_temp);
        err = put_text(ui, GSP_PC_BIND_CPU_TEMP, value);
    }

    if (err == ESP_GSP_OK) {
        format_gb(value, sizeof(value), sample->mem_used_gb);
        err = put_text(ui, GSP_PC_BIND_MEM_VAL, value);
    }
    if (err == ESP_GSP_OK) {
        err = put_value(ui, GSP_PC_OBJ_KEY_MEM_BAR, sample->mem_percent);
    }
    if (err == ESP_GSP_OK) {
        /* The number carries a "GB" unit label of its own, so the note line
         * holds the total and the percentage instead. */
        snprintf(note, sizeof(note), "共 %.0f GB · 已用 %d%%",
                 (double)sample->mem_total_gb, sample->mem_percent);
        err = put_text(ui, GSP_PC_BIND_MEM_NOTE, note);
    }

    /* GPU card: usage is the big read-out, dedicated VRAM fills the note line. */
    if (err == ESP_GSP_OK) {
        if (sample->has_gpu) {
            snprintf(value, sizeof(value), "%d", sample->gpu);
        } else {
            snprintf(value, sizeof(value), "--");
        }
        err = put_text(ui, GSP_PC_BIND_GPU_VAL, value);
    }
    if (err == ESP_GSP_OK) {
        err = put_value(ui, GSP_PC_OBJ_KEY_GPU_BAR,
                        sample->has_gpu ? sample->gpu : 0);
    }
    if (err == ESP_GSP_OK) {
        /* Like the CPU badge, an empty bound label would show a placeholder. */
        err = esp_gsp_component_set_visible(ui, GSP_PC_OBJ_KEY_GPU_TEMP,
                                            sample->has_gpu_temp);
    }
    if (err == ESP_GSP_OK && sample->has_gpu_temp) {
        snprintf(value, sizeof(value), "%d°C", sample->gpu_temp);
        err = put_text(ui, GSP_PC_BIND_GPU_TEMP, value);
    }
    if (err == ESP_GSP_OK) {
        if (sample->has_gpu_mem) {
            snprintf(note, sizeof(note), "显存 %.2f / %.1f GB",
                     (double)sample->gpu_mem_used_gb,
                     (double)sample->gpu_mem_total_gb);
        } else {
            snprintf(note, sizeof(note), "显存 --");
        }
        err = put_text(ui, GSP_PC_BIND_GPU_NOTE, note);
    }

    if (err == ESP_GSP_OK) {
        snprintf(value, sizeof(value), "%.1f", sample->net_down_mbps);
        err = put_text(ui, GSP_PC_BIND_NET_DOWN, value);
    }
    if (err == ESP_GSP_OK) {
        snprintf(value, sizeof(value), "%.1f", sample->net_up_mbps);
        err = put_text(ui, GSP_PC_BIND_NET_UP, value);
    }

    if (err == ESP_GSP_OK) {
        /* CPU card note line: the sensors that have no card of their own.
         * Temperature already has a badge at the top of the card. */
        if (sample->has_cpu_power && sample->has_cpu_freq) {
            snprintf(note, sizeof(note), "%.0fW · %.2fGHz",
                     (double)sample->cpu_power, sample->cpu_freq / 1000.0f);
        } else if (sample->has_cpu_power) {
            snprintf(note, sizeof(note), "%.0fW", (double)sample->cpu_power);
        } else if (sample->has_cpu_freq) {
            snprintf(note, sizeof(note), "%.2fGHz", sample->cpu_freq / 1000.0f);
        } else {
            snprintf(note, sizeof(note), "--");
        }
        err = put_text(ui, GSP_PC_BIND_CPU_NOTE, note);
    }
    if (err == ESP_GSP_OK) {
        const int64_t days = sample->uptime_s / 86400;
        const int64_t hours = (sample->uptime_s % 86400) / 3600;
        if (days > 0) {
            snprintf(note, sizeof(note), "运行 %d天%d小时", (int)days, (int)hours);
        } else {
            snprintf(note, sizeof(note), "运行 %d小时", (int)hours);
        }
        err = put_text(ui, GSP_PC_BIND_FT_UPTIME, note);
    }
    return err;
}

esp_gsp_err_t pc_panel_set_ap_list(esp_gsp_handle_t ui, pc_panel_t *panel,
                                   const wifi_ap_t *aps, int count,
                                   const char *hint, const char *current_ssid)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    /* Button rows with `hidden` expose visibility as a component property,
     * not a bind, so rows use the component API while their text uses binds. */
    static const gsp_component_key_t rows[WIFI_SCAN_MAX] = {
        GSP_PC_OBJ_KEY_AP_ROW_0, GSP_PC_OBJ_KEY_AP_ROW_1, GSP_PC_OBJ_KEY_AP_ROW_2,
        GSP_PC_OBJ_KEY_AP_ROW_3, GSP_PC_OBJ_KEY_AP_ROW_4, GSP_PC_OBJ_KEY_AP_ROW_5,
    };
    static const uint16_t texts[WIFI_SCAN_MAX] = {
        GSP_PC_BIND_AP_TEXT_0, GSP_PC_BIND_AP_TEXT_1, GSP_PC_BIND_AP_TEXT_2,
        GSP_PC_BIND_AP_TEXT_3, GSP_PC_BIND_AP_TEXT_4, GSP_PC_BIND_AP_TEXT_5,
    };

    if (count > WIFI_SCAN_MAX) {
        count = WIFI_SCAN_MAX;
    }
    esp_gsp_err_t err = ESP_GSP_OK;
    for (int i = 0; i < WIFI_SCAN_MAX; ++i) {
        bool used = i < count;
        if (err == ESP_GSP_OK) {
            err = esp_gsp_component_set_visible(ui, rows[i], used);
        }
        if (err != ESP_GSP_OK) {
            break;
        }
        if (used) {
            char label[64];
            const char *ssid = aps[i].ssid[0] ? aps[i].ssid : "隐藏网络";
            /* Mark the network this device is already configured for, so the
             * user can tell which one is current. */
            if (current_ssid && current_ssid[0] &&
                strcmp(ssid, current_ssid) == 0) {
                snprintf(label, sizeof(label), "%s    已连接    %d dBm", ssid,
                         aps[i].rssi);
            } else {
                snprintf(label, sizeof(label), "%s    %d dBm", ssid,
                         aps[i].rssi);
            }
            err = put_text(ui, texts[i], label);
        }
    }
    if (err == ESP_GSP_OK && hint) {
        err = put_text(ui, GSP_PC_BIND_WIFI_HINT, hint);
    }
    return err;
}

esp_gsp_err_t pc_panel_set_selected_ssid(esp_gsp_handle_t ui, pc_panel_t *panel,
                                         const char *ssid)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    char label[48];
    snprintf(label, sizeof(label), "网络：%s", ssid ? ssid : "");
    return put_text(ui, GSP_PC_BIND_PASS_SSID, label);
}

esp_gsp_err_t pc_panel_get_password(esp_gsp_handle_t ui, pc_panel_t *panel,
                                    char *out, size_t capacity)
{
    if (!ui || !panel || !out || capacity == 0) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    return esp_gsp_keyboard_text(ui, out, capacity);
}

esp_gsp_err_t pc_panel_set_clock(esp_gsp_handle_t ui, pc_panel_t *panel,
                                 const char *text)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    return put_text(ui, GSP_PC_BIND_FT_CLOCK, text);
}

esp_gsp_err_t pc_panel_set_wait(esp_gsp_handle_t ui, pc_panel_t *panel,
                                const char *title, const char *info)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    esp_gsp_err_t err = put_text(ui, GSP_PC_BIND_WAIT_TITLE, title);
    if (err == ESP_GSP_OK) {
        err = put_text(ui, GSP_PC_BIND_WAIT_INFO, info);
    }
    return err;
}

esp_gsp_err_t pc_panel_set_wifi_hint(esp_gsp_handle_t ui, pc_panel_t *panel,
                                     const char *hint)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    return put_text(ui, GSP_PC_BIND_WIFI_HINT, hint);
}

esp_gsp_err_t pc_panel_set_wait_ip(esp_gsp_handle_t ui, pc_panel_t *panel,
                                   const char *text)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    return put_text(ui, GSP_PC_BIND_WAIT_IP, text);
}

esp_gsp_err_t pc_panel_set_info(esp_gsp_handle_t ui, pc_panel_t *panel,
                                const pc_panel_info_t *info)
{
    if (!ui || !panel || !info) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    static const uint16_t binds[] = {
        GSP_PC_BIND_INFO_DEV_NAME, GSP_PC_BIND_INFO_DEV_MODEL,
        GSP_PC_BIND_INFO_DEV_VERSION, GSP_PC_BIND_INFO_DEV_IP,
        GSP_PC_BIND_INFO_DEV_UPTIME, GSP_PC_BIND_INFO_DEV_HEAP,
        GSP_PC_BIND_INFO_LINK_STATE, GSP_PC_BIND_INFO_WIFI,
        GSP_PC_BIND_INFO_PC_HOST, GSP_PC_BIND_INFO_PC_OS,
        GSP_PC_BIND_INFO_PC_IP, GSP_PC_BIND_INFO_PACKETS,
    };
    const char *values[] = {
        info->device_name, info->device_model, info->device_version,
        info->device_ip, info->device_uptime, info->device_heap,
        info->link_state, info->wifi, info->pc_host, info->pc_os,
        info->pc_ip, info->packets,
    };

    esp_gsp_err_t err = ESP_GSP_OK;
    for (size_t i = 0; i < sizeof(binds) / sizeof(binds[0]); ++i) {
        err = put_text(ui, binds[i], values[i]);
        if (err != ESP_GSP_OK) {
            break;
        }
    }
    return err;
}

esp_gsp_err_t pc_panel_set_pair_hint(esp_gsp_handle_t ui, pc_panel_t *panel,
                                     const char *text)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    return put_text(ui, GSP_PC_BIND_PAIR_SUB, text);
}

esp_gsp_err_t pc_panel_get_pair_code(esp_gsp_handle_t ui, pc_panel_t *panel,
                                     char *out, size_t capacity)
{
    if (!ui || !panel || !out || capacity == 0) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    strlcpy(out, panel->pair_code, capacity);
    return ESP_GSP_OK;
}

esp_gsp_err_t pc_panel_set_match_info(esp_gsp_handle_t ui, pc_panel_t *panel,
                                      const char *instruction, const char *ip)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    char line[32];
    snprintf(line, sizeof(line), "本机 %s", ip ? ip : "0.0.0.0");
    esp_gsp_err_t err = put_text(ui, GSP_PC_BIND_MATCH_SUB,
                                 instruction ? instruction : "");
    if (err == ESP_GSP_OK) {
        err = put_text(ui, GSP_PC_BIND_MATCH_IP, line);
    }
    return err;
}

esp_gsp_err_t pc_panel_clear_pair_code(esp_gsp_handle_t ui, pc_panel_t *panel)
{
    if (!ui || !panel) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    panel->pair_code[0] = '\0';
    return put_text(ui, GSP_PC_BIND_PAIR_TEXT, panel->pair_code);
}

/* ------------------------------------------------------------ detail pages */
void pc_panel_metric_values(const pc_sample_t *sample, pc_panel_metric_t metric,
                            pc_metric_values_t *out)
{
    if (!sample || !out) {
        return;
    }
    *out = (pc_metric_values_t){0};

    switch (metric) {
    case PC_PANEL_METRIC_CPU:
        out->first_absent = !sample->has_cpu_temp;
        out->first = sample->has_cpu_temp ? sample->cpu_temp : 0;
        out->second = sample->cpu;
        break;
    case PC_PANEL_METRIC_MEM:
        out->first = sample->mem_percent;
        out->second = (int)(sample->mem_used_gb + 0.5f);   /* whole GB */
        break;
    case PC_PANEL_METRIC_GPU:
        out->first_absent = !sample->has_gpu_temp;
        out->first = sample->has_gpu_temp ? sample->gpu_temp : 0;
        out->second = sample->gpu_mem_percent;
        break;
    case PC_PANEL_METRIC_NET:
        /* Rates share a fixed 0..PC_PANEL_RATE_MAX scale; the caption line
         * carries the fractional values. */
        out->first = (int)(sample->net_down_mbps + 0.5f);
        out->second = (int)(sample->net_up_mbps + 0.5f);
        break;
    default:
        break;
    }
}

static pc_panel_page_t metric_page(pc_panel_metric_t metric)
{
    switch (metric) {
    case PC_PANEL_METRIC_MEM:
        return PC_PANEL_PAGE_DETAIL_MEM;
    case PC_PANEL_METRIC_GPU:
        return PC_PANEL_PAGE_DETAIL_GPU;
    case PC_PANEL_METRIC_NET:
        return PC_PANEL_PAGE_DETAIL_NET;
    default:
        return PC_PANEL_PAGE_DETAIL_CPU;
    }
}

/*
 * Every metric has its own authored page, so a chart line and its read-outs keep
 * the colour of the dashboard card they came from. The two series are always
 * "temperature, then load" where a temperature sensor exists, and "load, then
 * capacity" where it does not, so the helpers below dispatch on the metric only.
 */

static const char *first_label(pc_panel_metric_t metric)
{
    switch (metric) {
    case PC_PANEL_METRIC_MEM:
        return "占用 %";
    case PC_PANEL_METRIC_NET:
        return "下载 MB/s";
    default:
        return "温度 °C";
    }
}

static const char *second_label(pc_panel_metric_t metric)
{
    switch (metric) {
    case PC_PANEL_METRIC_MEM:
        return "已用 GB";
    case PC_PANEL_METRIC_GPU:
        return "显存 %";
    case PC_PANEL_METRIC_NET:
        return "上传 MB/s";
    default:
        return "占用 %";
    }
}

static uint16_t bind_value(pc_panel_metric_t metric)
{
    switch (metric) {
    case PC_PANEL_METRIC_MEM:
        return GSP_PC_BIND_DETAIL_MEM_VALUE;
    case PC_PANEL_METRIC_GPU:
        return GSP_PC_BIND_DETAIL_GPU_VALUE;
    case PC_PANEL_METRIC_NET:
        return GSP_PC_BIND_DETAIL_NET_VALUE;
    default:
        return GSP_PC_BIND_DETAIL_CPU_VALUE;
    }
}

static uint16_t bind_info(pc_panel_metric_t metric)
{
    switch (metric) {
    case PC_PANEL_METRIC_MEM:
        return GSP_PC_BIND_DETAIL_MEM_INFO;
    case PC_PANEL_METRIC_GPU:
        return GSP_PC_BIND_DETAIL_GPU_INFO;
    case PC_PANEL_METRIC_NET:
        return GSP_PC_BIND_DETAIL_NET_INFO;
    default:
        return GSP_PC_BIND_DETAIL_CPU_INFO;
    }
}

static uint16_t bind_first_cap(pc_panel_metric_t metric)
{
    switch (metric) {
    case PC_PANEL_METRIC_MEM:
        return GSP_PC_BIND_DETAIL_MEM_USE_CAP;
    case PC_PANEL_METRIC_GPU:
        return GSP_PC_BIND_DETAIL_GPU_TEMP_CAP;
    case PC_PANEL_METRIC_NET:
        return GSP_PC_BIND_DETAIL_NET_DL_CAP;
    default:
        return GSP_PC_BIND_DETAIL_CPU_TEMP_CAP;
    }
}

static uint16_t bind_second_cap(pc_panel_metric_t metric)
{
    switch (metric) {
    case PC_PANEL_METRIC_MEM:
        return GSP_PC_BIND_DETAIL_MEM_USED_CAP;
    case PC_PANEL_METRIC_GPU:
        return GSP_PC_BIND_DETAIL_GPU_VRAM_CAP;
    case PC_PANEL_METRIC_NET:
        return GSP_PC_BIND_DETAIL_NET_UL_CAP;
    default:
        return GSP_PC_BIND_DETAIL_CPU_USE_CAP;
    }
}

static esp_gsp_err_t chart_set_series(esp_gsp_handle_t ui,
                                      pc_panel_metric_t metric,
                                      pc_panel_series_t series,
                                      const int32_t *values)
{
    const size_t count = PC_PANEL_TREND_POINTS;
    switch (metric) {
    case PC_PANEL_METRIC_MEM:
        return series == PC_PANEL_SERIES_FIRST
               ? gsp_pc_detail_mem_use_chart_set_series(ui, values, count)
               : gsp_pc_detail_mem_used_chart_set_series(ui, values, count);
    case PC_PANEL_METRIC_GPU:
        return series == PC_PANEL_SERIES_FIRST
               ? gsp_pc_detail_gpu_temp_chart_set_series(ui, values, count)
               : gsp_pc_detail_gpu_vram_chart_set_series(ui, values, count);
    case PC_PANEL_METRIC_NET:
        return series == PC_PANEL_SERIES_FIRST
               ? gsp_pc_detail_net_dl_chart_set_series(ui, values, count)
               : gsp_pc_detail_net_ul_chart_set_series(ui, values, count);
    default:
        return series == PC_PANEL_SERIES_FIRST
               ? gsp_pc_detail_cpu_temp_chart_set_series(ui, values, count)
               : gsp_pc_detail_cpu_use_chart_set_series(ui, values, count);
    }
}

static esp_gsp_err_t chart_append(esp_gsp_handle_t ui, pc_panel_metric_t metric,
                                  pc_panel_series_t series, int value)
{
    switch (metric) {
    case PC_PANEL_METRIC_MEM:
        return series == PC_PANEL_SERIES_FIRST
               ? gsp_pc_detail_mem_use_chart_append(ui, value)
               : gsp_pc_detail_mem_used_chart_append(ui, value);
    case PC_PANEL_METRIC_GPU:
        return series == PC_PANEL_SERIES_FIRST
               ? gsp_pc_detail_gpu_temp_chart_append(ui, value)
               : gsp_pc_detail_gpu_vram_chart_append(ui, value);
    case PC_PANEL_METRIC_NET:
        return series == PC_PANEL_SERIES_FIRST
               ? gsp_pc_detail_net_dl_chart_append(ui, value)
               : gsp_pc_detail_net_ul_chart_append(ui, value);
    default:
        return series == PC_PANEL_SERIES_FIRST
               ? gsp_pc_detail_cpu_temp_chart_append(ui, value)
               : gsp_pc_detail_cpu_use_chart_append(ui, value);
    }
}

/** The page's big read-out, in its own unit. */
static void format_primary(char *out, size_t capacity, pc_panel_metric_t metric,
                           const pc_sample_t *s)
{
    switch (metric) {
    case PC_PANEL_METRIC_CPU:
        snprintf(out, capacity, "%d", s->cpu);
        break;
    case PC_PANEL_METRIC_MEM:
        snprintf(out, capacity, "%d", s->mem_percent);
        break;
    case PC_PANEL_METRIC_GPU:
        snprintf(out, capacity, "%d", s->has_gpu ? s->gpu : 0);
        break;
    default:
        /* Network: the download rate is the headline number. */
        snprintf(out, capacity, "%.1f", (double)s->net_down_mbps);
        break;
    }
}

/** Append @p text to a bounded string, ignoring anything that does not fit. */
static void append_text(char *out, size_t capacity, const char *text)
{
    const size_t len = strlen(out);
    if (len + 1 >= capacity) {
        return;
    }
    strlcpy(out + len, text, capacity - len);
}

/** One line with every value the PC reported for this metric; missing sensors
 *  stay as "--" rather than disappearing, so the line keeps its shape. */
static void format_info(char *out, size_t capacity, pc_panel_metric_t metric,
                        const pc_sample_t *s)
{
    char part[40];
    out[0] = '\0';

    switch (metric) {
    case PC_PANEL_METRIC_CPU:
        if (s->has_cpu_temp) {
            snprintf(part, sizeof(part), "温度 %d°C", s->cpu_temp);
        } else {
            snprintf(part, sizeof(part), "温度 --");
        }
        append_text(out, capacity, part);
        append_text(out, capacity, " · ");
        if (s->has_cpu_power) {
            snprintf(part, sizeof(part), "功率 %.0fW", (double)s->cpu_power);
        } else {
            snprintf(part, sizeof(part), "功率 --");
        }
        append_text(out, capacity, part);
        append_text(out, capacity, " · ");
        if (s->has_cpu_freq) {
            snprintf(part, sizeof(part), "%.2fGHz", s->cpu_freq / 1000.0f);
        } else {
            snprintf(part, sizeof(part), "--");
        }
        append_text(out, capacity, part);
        break;

    case PC_PANEL_METRIC_MEM:
        snprintf(part, sizeof(part), "已用 %.1f / %.1f GB",
                 (double)s->mem_used_gb, (double)s->mem_total_gb);
        append_text(out, capacity, part);
        break;

    case PC_PANEL_METRIC_GPU:
        if (s->has_gpu_temp) {
            snprintf(part, sizeof(part), "温度 %d°C", s->gpu_temp);
        } else {
            snprintf(part, sizeof(part), "温度 --");
        }
        append_text(out, capacity, part);
        append_text(out, capacity, " · ");
        if (s->has_gpu_mem) {
            snprintf(part, sizeof(part), "显存 %.2f / %.1f GB",
                     (double)s->gpu_mem_used_gb, (double)s->gpu_mem_total_gb);
        } else {
            snprintf(part, sizeof(part), "显存 --");
        }
        append_text(out, capacity, part);
        break;

    default: {
        /* Network: upload and the two directions combined. */
        const double total = (double)s->net_down_mbps + (double)s->net_up_mbps;
        snprintf(part, sizeof(part), "上传 %.1f MB/s · 总计 %.1f MB/s",
                 (double)s->net_up_mbps, total);
        append_text(out, capacity, part);
        break;
    }
    }
}

/** "<label> · 最低 n · 最高 m" (the label carries the unit), or a note when the
 *  PC has no sensor behind this series. */
static void format_caption(char *out, size_t capacity, const char *label,
                           int lowest, int highest, bool absent)
{
    char part[32];
    out[0] = '\0';
    append_text(out, capacity, label);
    if (absent) {
        append_text(out, capacity, " · 无数据");
        return;
    }
    snprintf(part, sizeof(part), " · 最低 %d · 最高 %d", lowest, highest);
    append_text(out, capacity, part);
}

static esp_gsp_err_t set_detail_readouts(esp_gsp_handle_t ui, pc_panel_t *p,
                                         const pc_sample_t *sample,
                                         const pc_metric_values_t *values)
{
    char text[64];
    esp_gsp_err_t err;

    format_primary(text, sizeof(text), p->detail_metric, sample);
    err = put_text(ui, bind_value(p->detail_metric), text);

    if (err == ESP_GSP_OK) {
        format_info(text, sizeof(text), p->detail_metric, sample);
        err = put_text(ui, bind_info(p->detail_metric), text);
    }
    if (err == ESP_GSP_OK) {
        format_caption(text, sizeof(text), first_label(p->detail_metric),
                       p->detail_lowest[PC_PANEL_SERIES_FIRST],
                       p->detail_highest[PC_PANEL_SERIES_FIRST],
                       values->first_absent);
        err = put_text(ui, bind_first_cap(p->detail_metric), text);
    }
    if (err == ESP_GSP_OK) {
        format_caption(text, sizeof(text), second_label(p->detail_metric),
                       p->detail_lowest[PC_PANEL_SERIES_SECOND],
                       p->detail_highest[PC_PANEL_SERIES_SECOND], false);
        err = put_text(ui, bind_second_cap(p->detail_metric), text);
    }
    return err;
}

esp_gsp_err_t pc_panel_show_detail(esp_gsp_handle_t ui, pc_panel_t *panel,
                                   pc_panel_metric_t metric,
                                   const uint8_t *first, const uint8_t *second,
                                   size_t count, const pc_sample_t *sample)
{
    if (!ui || !panel || !sample) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    if (count > PC_PANEL_TREND_POINTS) {
        count = PC_PANEL_TREND_POINTS;
    }

    /* Oldest-first window padded at the front so a series is always full. One
     * buffer is reused for both charts to keep the stack cost low. */
    int32_t values[PC_PANEL_TREND_POINTS];
    const size_t pad = PC_PANEL_TREND_POINTS - count;

    panel->detail_metric = metric;
    for (int s = 0; s < PC_PANEL_SERIES_COUNT; ++s) {
        const uint8_t *history = s == PC_PANEL_SERIES_FIRST ? first : second;
        int lowest = 256;
        int highest = -1;
        for (size_t i = 0; i < PC_PANEL_TREND_POINTS; ++i) {
            int value = 0;
            if (i >= pad && history) {
                value = history[i - pad];
                if (value < lowest) {
                    lowest = value;
                }
                if (value > highest) {
                    highest = value;
                }
            }
            values[i] = value;
        }
        if (count == 0) {
            lowest = 0;
            highest = 0;
        }
        panel->detail_lowest[s] = lowest;
        panel->detail_highest[s] = highest;
    }

    pc_metric_values_t live;
    pc_panel_metric_values(sample, metric, &live);

    panel->page = metric_page(metric);
    esp_gsp_err_t err = apply_page(ui, panel);
    for (int s = 0; err == ESP_GSP_OK && s < PC_PANEL_SERIES_COUNT; ++s) {
        const uint8_t *history = s == PC_PANEL_SERIES_FIRST ? first : second;
        if (s == PC_PANEL_SERIES_FIRST && live.first_absent) {
            continue;   /* no sensor: leave the chart empty instead of zeroed */
        }
        /* Rebuild this series' window (the loop above only measured it). */
        for (size_t i = 0; i < PC_PANEL_TREND_POINTS; ++i) {
            values[i] = (i >= pad && history) ? history[i - pad] : 0;
        }
        err = chart_set_series(ui, metric, (pc_panel_series_t)s, values);
    }
    if (err == ESP_GSP_OK) {
        err = set_detail_readouts(ui, panel, sample, &live);
    }
    return err;
}

esp_gsp_err_t pc_panel_detail_append(esp_gsp_handle_t ui, pc_panel_t *panel,
                                     const pc_sample_t *sample,
                                     const pc_metric_values_t *values)
{
    if (!ui || !panel || !sample || !values) {
        return ESP_GSP_ERR_INVALID_ARG;
    }

    const int value[PC_PANEL_SERIES_COUNT] = {values->first, values->second};
    esp_gsp_err_t err = ESP_GSP_OK;
    for (int s = 0; s < PC_PANEL_SERIES_COUNT; ++s) {
        if (s == PC_PANEL_SERIES_FIRST && values->first_absent) {
            continue;
        }
        if (value[s] < panel->detail_lowest[s]) {
            panel->detail_lowest[s] = value[s];
        }
        if (value[s] > panel->detail_highest[s]) {
            panel->detail_highest[s] = value[s];
        }
        err = chart_append(ui, panel->detail_metric, (pc_panel_series_t)s,
                           value[s]);
        if (err != ESP_GSP_OK) {
            return err;
        }
    }
    return set_detail_readouts(ui, panel, sample, values);
}

bool pc_panel_showing_detail(const pc_panel_t *panel)
{
    return panel && (panel->page == PC_PANEL_PAGE_DETAIL_CPU ||
                     panel->page == PC_PANEL_PAGE_DETAIL_MEM ||
                     panel->page == PC_PANEL_PAGE_DETAIL_GPU ||
                     panel->page == PC_PANEL_PAGE_DETAIL_NET);
}
