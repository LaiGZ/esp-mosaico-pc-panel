// SPDX-License-Identifier: Apache-2.0
//
// Portable ESP-GSP UI for the PC status panel. Compiles for both the device
// (ESP-IDF) and the Host simulator: it only depends on esp_gsp.h and the
// generated bundle header, never on board, FreeRTOS or ESP-Iris APIs.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_state.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PC_PANEL_PAGE_DASH = 0,
    PC_PANEL_PAGE_WAIT,
    PC_PANEL_PAGE_WIFI,
    PC_PANEL_PAGE_PASS,
    PC_PANEL_PAGE_DETAIL_CPU,
    PC_PANEL_PAGE_DETAIL_MEM,
    PC_PANEL_PAGE_DETAIL_GPU,
    PC_PANEL_PAGE_DETAIL_NET,
    PC_PANEL_PAGE_INFO,
    PC_PANEL_PAGE_PAIR,
    PC_PANEL_PAGE_MATCH,
} pc_panel_page_t;

/** Metrics that have a detail page with live trend charts. */
typedef enum {
    PC_PANEL_METRIC_CPU = 0,
    PC_PANEL_METRIC_MEM,
    PC_PANEL_METRIC_GPU,
    PC_PANEL_METRIC_NET,
    PC_PANEL_METRIC_COUNT,
} pc_panel_metric_t;

/** The two trend series every detail page draws. */
typedef enum {
    PC_PANEL_SERIES_FIRST = 0,  /* temperature, or usage where there is no sensor */
    PC_PANEL_SERIES_SECOND,     /* usage, memory used, VRAM in use or upload */
    PC_PANEL_SERIES_COUNT,
} pc_panel_series_t;

/**
 * One sample mapped onto a detail page's trend series. Values are percentages
 * of the chart's authored range, so they stay small integers; the page's big
 * read-out and information line are formatted from the sample itself, because
 * the network page shows fractional rates.
 */
typedef struct {
    int  first;          /* first chart value */
    int  second;         /* second chart value */
    bool first_absent;   /* no sensor feeds the first chart (PC cannot report it) */
} pc_metric_values_t;

/** Every value shown on the device information page, pre-formatted by the
 *  caller (which owns the clock, the heap and the link handles). */
typedef struct {
    char device_name[24];
    char device_model[16];
    char device_version[12];
    char device_ip[16];
    char device_uptime[24];
    char device_heap[16];
    char link_state[24];
    char wifi[16];
    char pc_host[PC_STATUS_HOST_MAX];
    char pc_os[PC_STATUS_OS_MAX];
    char pc_ip[16];
    char packets[12];
} pc_panel_info_t;

/** Trend window length; must match the scene's chart series capacity. */
#define PC_PANEL_TREND_POINTS 60

/** Authored maximum of both network trend charts (MB/s). Rates above it clip
 *  visually, while the caption lines keep the real numbers. */
#define PC_PANEL_RATE_MAX 100

/** Device-side actions triggered by the compiled scene. */
typedef struct {
    void (*on_open_setup)(void *ctx);
    void (*on_scan)(void *ctx);
    void (*on_pick_ap)(void *ctx, int index);
    void (*on_save)(void *ctx);
    /* Back is raised by the on-screen control and by horizontal swipes in
     * either direction, so the application keeps a single navigation owner. */
    void (*on_back)(void *ctx);
    /** A dashboard card was tapped; @p metric is a pc_panel_metric_t. */
    void (*on_open_detail)(void *ctx, int metric);
    /** The pairing page's confirm button was pressed. */
    void (*on_pair)(void *ctx);
    /** "Re-pair" was pressed on the information page. */
    void (*on_unpair)(void *ctx);
    /** The intro page's "匹配" button was pressed. */
    void (*on_match)(void *ctx);
} pc_panel_callbacks_t;

typedef struct pc_panel pc_panel_t;

/** Bind to a running UI, attach the keyboard and show the dashboard. */
esp_gsp_err_t pc_panel_create(esp_gsp_handle_t ui, const pc_panel_callbacks_t *cbs,
                              void *ctx, pc_panel_t **ret_panel);

/** Fill the device information page. */
esp_gsp_err_t pc_panel_set_info(esp_gsp_handle_t ui, pc_panel_t *panel,
                                const pc_panel_info_t *info);

/** Set the pairing page's instruction / result line. */
esp_gsp_err_t pc_panel_set_pair_hint(esp_gsp_handle_t ui, pc_panel_t *panel,
                                     const char *text);

/** Set the intro page's instruction line and the device's own address. */
esp_gsp_err_t pc_panel_set_match_info(esp_gsp_handle_t ui, pc_panel_t *panel,
                                      const char *instruction, const char *ip);

/** Read the code typed on the pairing page (digits only). */
esp_gsp_err_t pc_panel_get_pair_code(esp_gsp_handle_t ui, pc_panel_t *panel,
                                     char *out, size_t capacity);

/** Clear the code field (after a successful pairing or when un-pairing). */
esp_gsp_err_t pc_panel_clear_pair_code(esp_gsp_handle_t ui, pc_panel_t *panel);

/** Detach the event callback and release the panel. */
void pc_panel_destroy(esp_gsp_handle_t ui, pc_panel_t *panel);

esp_gsp_err_t pc_panel_set_page(esp_gsp_handle_t ui, pc_panel_t *panel,
                                pc_panel_page_t page);
pc_panel_page_t pc_panel_page(const pc_panel_t *panel);

/** Refresh every dashboard value from one PC sample. */
esp_gsp_err_t pc_panel_update(esp_gsp_handle_t ui, pc_panel_t *panel,
                              const pc_sample_t *sample);

/** Toggle the connection dot (green when online, red when not). */
esp_gsp_err_t pc_panel_set_link(esp_gsp_handle_t ui, pc_panel_t *panel,
                                bool online);

/**
 * Fill the access-point rows (max WIFI_SCAN_MAX) and the footer hint.
 * @p current_ssid is the network the device is configured for; that row is
 * marked as connected. Pass NULL when nothing is configured yet.
 */
esp_gsp_err_t pc_panel_set_ap_list(esp_gsp_handle_t ui, pc_panel_t *panel,
                                   const wifi_ap_t *aps, int count,
                                   const char *hint, const char *current_ssid);

/** Show the SSID awaiting a password. */
esp_gsp_err_t pc_panel_set_selected_ssid(esp_gsp_handle_t ui, pc_panel_t *panel,
                                         const char *ssid);

/** Read the password typed into the on-screen keyboard. */
esp_gsp_err_t pc_panel_get_password(esp_gsp_handle_t ui, pc_panel_t *panel,
                                    char *out, size_t capacity);

/** Replace the dashboard host line (device name / OS). */
esp_gsp_err_t pc_panel_set_host(esp_gsp_handle_t ui, pc_panel_t *panel,
                                const char *name, const char *subtitle);

/**
 * Name of the step that last failed inside this module, or "" when the last
 * call succeeded. Lets the (non-portable) caller log the failure without this
 * file depending on a logging stack.
 */
const char *pc_panel_last_stage(void);

/** Set the waiting page title and detail line. */
esp_gsp_err_t pc_panel_set_wait(esp_gsp_handle_t ui, pc_panel_t *panel,
                                const char *title, const char *info);

/** Set the waiting page address line (empty shows nothing). */
esp_gsp_err_t pc_panel_set_wait_ip(esp_gsp_handle_t ui, pc_panel_t *panel,
                                   const char *text);

/** Set the Wi-Fi page footer hint (scan progress / result). */
esp_gsp_err_t pc_panel_set_wifi_hint(esp_gsp_handle_t ui, pc_panel_t *panel,
                                     const char *hint);

/** Set the footer clock text (the panel has no time source of its own). */
esp_gsp_err_t pc_panel_set_clock(esp_gsp_handle_t ui, pc_panel_t *panel,
                                 const char *text);

/**
 * Map one sample onto the trend series of @p metric. Values are percentages,
 * except the memory page's second series, which is whole GB. Charts without a
 * sensor behind them report @c first_absent and a zero value.
 */
void pc_panel_metric_values(const pc_sample_t *sample, pc_panel_metric_t metric,
                            pc_metric_values_t *out);

/**
 * Open the trend page for @p metric and seed its two charts from @p first and
 * @p second (oldest first, padded with zeros when shorter than
 * PC_PANEL_TREND_POINTS), then fill the read-out line from @p sample.
 */
esp_gsp_err_t pc_panel_show_detail(esp_gsp_handle_t ui, pc_panel_t *panel,
                                   pc_panel_metric_t metric,
                                   const uint8_t *first, const uint8_t *second,
                                   size_t count, const pc_sample_t *sample);

/** Append one sample to the open trend page and refresh its read-outs. */
esp_gsp_err_t pc_panel_detail_append(esp_gsp_handle_t ui, pc_panel_t *panel,
                                     const pc_sample_t *sample,
                                     const pc_metric_values_t *values);

/** True while the trend page is the active page. */
bool pc_panel_showing_detail(const pc_panel_t *panel);

#ifdef __cplusplus
}
#endif
