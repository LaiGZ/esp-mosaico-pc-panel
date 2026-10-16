// SPDX-License-Identifier: Apache-2.0

#include "wifi_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "wifi_manager";

#define WIFI_NVS_NAMESPACE    "pcwifi"
#define WIFI_NVS_KEY_SSID     "ssid"
#define WIFI_NVS_KEY_PASS     "pass"
#define WIFI_SSID_MAX         32
#define WIFI_PASS_MAX         64
#define WIFI_RETRY_BACKOFF_MS 5000

/*
 * The driver needs a record buffer sized for the strongest environment we
 * support. It is allocated once per manager instead of once per scan, which
 * keeps scanning off the allocator and out of the fragmentation path.
 */
#define WIFI_SCAN_MAX_HW 16

struct wifi_manager_t {
    wifi_state_t          state;
    SemaphoreHandle_t     lock;
    wifi_manager_event_cb_t cb;
    void                 *cb_ctx;
    esp_netif_t          *sta_netif;
    esp_timer_handle_t    retry_timer;
    /* Kept so the handlers can be unregistered; NULL means "not registered". */
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    /* Reused scan result buffer; owned by the instance for its whole lifetime. */
    wifi_ap_record_t     *scan_records;
    char                  ssid[WIFI_SSID_MAX + 1];
    char                  pass[WIFI_PASS_MAX + 1];
    char                  ip[16];
    bool                  wifi_inited;
    bool                  started;
};

/* ------------------------------------------------------------- helpers */

static void notify(wifi_manager_handle_t h, wifi_state_t state)
{
    if (xSemaphoreTake(h->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    h->state = state;
    wifi_manager_event_cb_t cb = h->cb;
    void *ctx = h->cb_ctx;
    xSemaphoreGive(h->lock);
    if (cb) {
        cb(state, ctx);
    }
}

static void load_credentials(wifi_manager_handle_t h)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = sizeof(h->ssid);
    if (nvs_get_str(nvs, WIFI_NVS_KEY_SSID, h->ssid, &len) != ESP_OK) {
        h->ssid[0] = '\0';
    }
    len = sizeof(h->pass);
    if (nvs_get_str(nvs, WIFI_NVS_KEY_PASS, h->pass, &len) != ESP_OK) {
        h->pass[0] = '\0';
    }
    nvs_close(nvs);
}

static esp_err_t store_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
                        "open nvs");
    esp_err_t err = nvs_set_str(nvs, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_NVS_KEY_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void start_sta(wifi_manager_handle_t h)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, h->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, h->pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    notify(h, WIFI_STATE_CONNECTING);
}

/* esp_wifi_connect() only tries once, so reconnection is owned here. The
 * backoff runs in the esp_timer task and never blocks the event loop. */
static void retry_cb(void *arg)
{
    wifi_manager_handle_t h = arg;
    if (h->started && h->ssid[0]) {
        start_sta(h);
    }
}

/* ------------------------------------------------------------- events */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    wifi_manager_handle_t h = arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (h->ssid[0]) {
            start_sta(h);
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        notify(h, WIFI_STATE_FAILED);
        if (h->started && h->ssid[0] && h->retry_timer) {
            /* Ignored when a backoff is already pending. */
            esp_timer_start_once(h->retry_timer, WIFI_RETRY_BACKOFF_MS * 1000);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        snprintf(h->ip, sizeof(h->ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "connected, ip=%s", h->ip);
        notify(h, WIFI_STATE_CONNECTED);
        return;
    }
}

/* ------------------------------------------------------------- public */

esp_err_t wifi_manager_create(wifi_manager_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(ret_handle, ESP_ERR_INVALID_ARG, TAG, "null output");

    wifi_manager_handle_t h = calloc(1, sizeof(struct wifi_manager_t));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");

    h->lock = xSemaphoreCreateMutex();
    h->scan_records = calloc(WIFI_SCAN_MAX_HW, sizeof(wifi_ap_record_t));
    if (!h->lock || !h->scan_records) {
        vSemaphoreDelete(h->lock);
        free(h->scan_records);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(h->ip, "0.0.0.0", sizeof(h->ip));
    load_credentials(h);
    *ret_handle = h;
    return ESP_OK;
}

esp_err_t wifi_manager_start(wifi_manager_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    if (h->started) {
        return ESP_OK;    /* idempotent: never registers handlers twice */
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!h->sta_netif) {
        h->sta_netif = esp_netif_create_default_wifi_sta();
    }
    ESP_RETURN_ON_FALSE(h->sta_netif, ESP_FAIL, TAG, "create sta netif");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    h->wifi_inited = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              on_wifi_event, h, &h->wifi_handler);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              on_wifi_event, h, &h->ip_handler);
    if (err != ESP_OK) {
        goto fail;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = retry_cb,
        .arg = h,
        .name = "wifi_retry",
    };
    err = esp_timer_create(&timer_args, &h->retry_timer);
    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        goto fail;
    }

    h->started = true;
    return ESP_OK;

fail:
    /* Roll back everything this call acquired so a retry starts clean. */
    ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
    if (h->ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              h->ip_handler);
        h->ip_handler = NULL;
    }
    if (h->wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              h->wifi_handler);
        h->wifi_handler = NULL;
    }
    if (h->retry_timer) {
        esp_timer_delete(h->retry_timer);
        h->retry_timer = NULL;
    }
    if (h->wifi_inited) {
        esp_wifi_deinit();
        h->wifi_inited = false;
    }
    return err;
}

void wifi_manager_delete(wifi_manager_handle_t h)
{
    if (!h) {
        return;
    }
    h->started = false;

    /* Unregister before releasing anything the handlers dereference. */
    if (h->ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              h->ip_handler);
        h->ip_handler = NULL;
    }
    if (h->wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              h->wifi_handler);
        h->wifi_handler = NULL;
    }
    if (h->retry_timer) {
        esp_timer_stop(h->retry_timer);
        esp_timer_delete(h->retry_timer);
        h->retry_timer = NULL;
    }
    if (h->wifi_inited) {
        esp_wifi_stop();
        esp_wifi_deinit();
        h->wifi_inited = false;
    }
    free(h->scan_records);
    if (h->lock) {
        vSemaphoreDelete(h->lock);
    }
    free(h);
}

esp_err_t wifi_manager_register_cb(wifi_manager_handle_t h,
                                   wifi_manager_event_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    if (xSemaphoreTake(h->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    h->cb = cb;
    h->cb_ctx = ctx;
    xSemaphoreGive(h->lock);
    return ESP_OK;
}

bool wifi_manager_has_credentials(wifi_manager_handle_t h)
{
    return h && h->ssid[0] != '\0';
}

wifi_state_t wifi_manager_state(wifi_manager_handle_t h)
{
    return h ? h->state : WIFI_STATE_IDLE;
}

esp_err_t wifi_manager_ip(wifi_manager_handle_t h, char *out, size_t capacity)
{
    ESP_RETURN_ON_FALSE(h && out && capacity, ESP_ERR_INVALID_ARG, TAG, "bad args");
    strlcpy(out, h->ip, capacity);
    return ESP_OK;
}

int wifi_manager_rssi(wifi_manager_handle_t h)
{
    if (!h) {
        return 0;
    }
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

esp_err_t wifi_manager_ssid(wifi_manager_handle_t h, char *out, size_t capacity)
{
    ESP_RETURN_ON_FALSE(h && out && capacity, ESP_ERR_INVALID_ARG, TAG, "bad args");
    strlcpy(out, h->ssid, capacity);
    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_manager_handle_t h, wifi_ap_t *out, int max,
                            int *count)
{
    ESP_RETURN_ON_FALSE(h && out && count, ESP_ERR_INVALID_ARG, TAG, "bad args");
    *count = 0;

    wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&cfg, true), TAG, "scan start");

    /* Blocking scan releases its own results; always drain them, even when we
     * only need the first few, otherwise the driver keeps the AP list alive. */
    uint16_t found = WIFI_SCAN_MAX_HW;
    esp_err_t err = esp_wifi_scan_get_ap_records(&found, h->scan_records);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        return err;
    }

    int written = 0;
    for (uint16_t i = 0; i < found && written < max; ++i) {
        if (h->scan_records[i].ssid[0] == '\0') {
            continue;
        }
        strlcpy(out[written].ssid, (const char *)h->scan_records[i].ssid,
                sizeof(out[written].ssid));
        out[written].rssi = h->scan_records[i].rssi;
        out[written].secure = h->scan_records[i].authmode != WIFI_AUTH_OPEN;
        written++;
    }
    esp_wifi_clear_ap_list();
    *count = written;
    ESP_LOGI(TAG, "scan found %d usable networks", written);
    return ESP_OK;
}

esp_err_t wifi_manager_connect(wifi_manager_handle_t h, const char *ssid,
                               const char *password)
{
    ESP_RETURN_ON_FALSE(h && ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "bad ssid");
    ESP_RETURN_ON_ERROR(store_credentials(ssid, password), TAG, "store creds");

    if (xSemaphoreTake(h->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    strlcpy(h->ssid, ssid, sizeof(h->ssid));
    strlcpy(h->pass, password ? password : "", sizeof(h->pass));
    xSemaphoreGive(h->lock);

    esp_wifi_disconnect();
    start_sta(h);
    return ESP_OK;
}
