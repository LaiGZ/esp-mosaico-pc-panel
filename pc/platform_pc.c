// SPDX-License-Identifier: Apache-2.0
//
// Host (simulator) adapter. Feeds the shared portable UI with synthetic PC
// samples so the dashboard, link state and page transitions can be exercised
// without hardware. Wi-Fi and UDP reception stay on the device.

#include <stdio.h>
#include <string.h>

#include "esp_gsp.h"
#include "gsp_sim_bridge.h"
#include "pc_panel_ui.h"

#define SIM_TICK_MS 500

static pc_panel_t *s_panel;
static void   *s_timer;
static unsigned s_tick;

/* Rolling window for the detail pages, mirroring what main.c keeps on device. */
static uint8_t s_trend[PC_PANEL_METRIC_COUNT][PC_PANEL_SERIES_COUNT]
                       [PC_PANEL_TREND_POINTS];
static unsigned s_pos;
static unsigned s_count;
static int      s_detail;      /* metric shown by the preview cycle */
static int      s_phase = -1;

static void on_open_setup(void *ctx)
{
    (void)ctx;
    fprintf(stderr, "sim: open_setup -> wifi page\n");
}
static void on_scan(void *ctx)
{
    (void)ctx;
    fprintf(stderr, "sim: scan requested\n");
}
static void on_pick_ap(void *ctx, int index)
{
    (void)ctx;
    fprintf(stderr, "sim: pick ap %d\n", index);
}
static void on_save(void *ctx)
{
    (void)ctx;
    fprintf(stderr, "sim: save requested\n");
}
static void on_back(void *ctx)
{
    (void)ctx;
    fprintf(stderr, "sim: back requested\n");
}

static void on_open_detail(void *ctx, int metric)
{
    (void)ctx;
    fprintf(stderr, "sim: open detail metric=%d\n", metric);
}

/* Copy one rolling window oldest-first, as main.c does on device. */
static unsigned ordered(int metric, int series, uint8_t *out)
{
    for (unsigned i = 0; i < s_count; ++i) {
        out[i] = s_trend[metric][series]
                        [(s_pos + PC_PANEL_TREND_POINTS - s_count + i)
                         % PC_PANEL_TREND_POINTS];
    }
    return s_count;
}

static void open_detail(esp_gsp_handle_t ui, const pc_sample_t *s)
{
    static uint8_t first[PC_PANEL_TREND_POINTS];
    static uint8_t second[PC_PANEL_TREND_POINTS];
    const unsigned count = ordered(s_detail, PC_PANEL_SERIES_FIRST, first);
    (void)ordered(s_detail, PC_PANEL_SERIES_SECOND, second);
    (void)pc_panel_show_detail(ui, s_panel, (pc_panel_metric_t)s_detail, first,
                               second, count, s);
}

static void feed(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    ++s_tick;

    pc_sample_t s = {0};
    strcpy(s.host, "DESKTOP-8K2QM");
    strcpy(s.os, "Windows 11");
    s.cpu = (int)(s_tick * 7 % 100);
    s.has_cpu_temp = true;
    s.cpu_temp = 48 + (int)(s_tick % 12);
    s.has_cpu_power = true;
    s.cpu_power = 45.0f + (float)(s_tick % 30);
    s.has_cpu_freq = true;
    s.cpu_freq = 3800 + (int)(s_tick % 700);
    s.mem_used_gb = 11.2f + (float)(s_tick % 7) * 0.4f;
    s.mem_total_gb = 32.0f;
    s.mem_percent = (int)(s.mem_used_gb / s.mem_total_gb * 100.0f);
    s.net_down_mbps = (float)(s_tick % 200) / 10.0f;
    s.net_up_mbps = (float)(s_tick % 40) / 10.0f;
    s.has_gpu = true;
    s.gpu = 20 + (int)(s_tick % 35);
    s.has_gpu_temp = true;
    s.gpu_temp = 44 + (int)(s_tick % 6);
    s.gpu_mem_used_gb = 1.1f + (float)(s_tick % 5) * 0.1f;
    s.gpu_mem_total_gb = 2.0f;
    s.gpu_mem_percent = 60;
    s.has_gpu_mem = true;
    s.uptime_s = 3 * 86400 + 4 * 3600;
    s.pc_epoch_s = 0; /* clock text is device-owned */

    (void)pc_panel_set_host(ui, s_panel, s.host, s.os);
    (void)pc_panel_update(ui, s_panel, &s);
    (void)pc_panel_set_link(ui, s_panel, true);
    (void)pc_panel_set_clock(ui, s_panel, "21:03:44");

    for (int m = 0; m < PC_PANEL_METRIC_COUNT; ++m) {
        pc_metric_values_t values;
        pc_panel_metric_values(&s, (pc_panel_metric_t)m, &values);
        s_trend[m][PC_PANEL_SERIES_FIRST][s_pos] = (uint8_t)values.first;
        s_trend[m][PC_PANEL_SERIES_SECOND][s_pos] = (uint8_t)values.second;
        if (pc_panel_showing_detail(s_panel) && s_detail == m) {
            (void)pc_panel_detail_append(ui, s_panel, &s, &values);
        }
    }
    s_pos = (s_pos + 1) % PC_PANEL_TREND_POINTS;
    if (s_count < PC_PANEL_TREND_POINTS) {
        s_count++;
    }

    /* Cycle through every screen so a preview covers all of them. Each phase
     * lasts 20 ticks (10 s). */
    const int phase = (int)((s_tick / 20) % 8);
    if (phase == s_phase) {
        if (/* info page */ s_phase == 7) {
            pc_panel_info_t info = {0};
            strcpy(info.device_name, "PC 状态面板");
            strcpy(info.device_model, "ESP-Mosaico");
            strcpy(info.device_version, "1.0.0");
            strcpy(info.device_ip, "192.168.1.42");
            strcpy(info.device_uptime, "3天4小时");
            strcpy(info.device_heap, "192 KB");
            strcpy(info.link_state, "已连接 · 0 秒前");
            strcpy(info.wifi, "-42 dBm");
            strcpy(info.pc_host, s.host);
            strcpy(info.pc_os, s.os);
            strcpy(info.pc_ip, "192.168.1.7");
            sprintf(info.packets, "%u", s_tick);
            (void)pc_panel_set_info(ui, s_panel, &info);
        }
        return;
    }
    s_phase = phase;
    switch (phase) {
    case 1:
        (void)pc_panel_set_page(ui, s_panel, PC_PANEL_PAGE_WAIT);
        (void)pc_panel_set_wait(ui, s_panel, "正在等待电脑…",
                                "请确认电脑端上报程序已启动，并处于同一局域网");
        break;
    case 2: {
        static const wifi_ap_t aps[3] = {
            {"MyHome-2.4G", -42, true},
            {"Office-Guest", -68, true},
            {"TP-LINK_5G", -85, true},
        };
        (void)pc_panel_set_page(ui, s_panel, PC_PANEL_PAGE_WIFI);
        (void)pc_panel_set_ap_list(ui, s_panel, aps, 3, "点击网络名称开始配置",
                                   "MyHome-2.4G");
        break;
    }
    case 3:
    case 4:
    case 5:
    case 6:
        /* One detail page per phase, so all four preview in turn. */
        s_detail = phase - 3;
        open_detail(ui, &s);
        break;
    case 7:
        (void)pc_panel_set_page(ui, s_panel, PC_PANEL_PAGE_INFO);
        break;
    default:
        (void)pc_panel_set_page(ui, s_panel, PC_PANEL_PAGE_DASH);
        break;
    }
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    const pc_panel_callbacks_t cbs = {
        .on_open_setup = on_open_setup,
        .on_scan = on_scan,
        .on_pick_ap = on_pick_ap,
        .on_save = on_save,
        .on_back = on_back,
        .on_open_detail = on_open_detail,
    };
    esp_gsp_err_t err = pc_panel_create(ui, &cbs, NULL, &s_panel);
    if (err != ESP_GSP_OK) {
        return err;
    }
    s_timer = esp_gsp_timer_create(ui, SIM_TICK_MS, feed, NULL);
    return s_timer ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    if (s_timer) {
        (void)esp_gsp_timer_delete(ui, s_timer);
        s_timer = NULL;
    }
    pc_panel_destroy(ui, s_panel);
    s_panel = NULL;
    fprintf(stderr, "pc_panel: ticks=%u\n", s_tick);
}
