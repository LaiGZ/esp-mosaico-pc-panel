// SPDX-License-Identifier: Apache-2.0

#include "pc_link.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "pc_link";

/* The paired PC's code lives in NVS so the binding survives reboots. */
#define PC_LINK_NVS_NAMESPACE   "pcstat"
#define PC_LINK_NVS_KEY_PAIR    "pair"
#define PC_LINK_NVS_KEY_PAIR_IP "pairip"
#define PC_LINK_NVS_KEY_HOST    "pairhost"

#define PC_LINK_RX_BUFFER     1024
#define PC_LINK_TASK_STACK    4096
#define PC_LINK_TASK_PRIORITY 4
/* Wake up often enough to notice a stale link while idle. */
#define PC_LINK_POLL_MS       500
/* How often the identity reply may be sent; the PC agent only needs it on
 * connect and after a lease change. */
#define PC_LINK_REPLY_INTERVAL_MS 3000
#define PC_LINK_REPLY_MAX     160

/* One PC agent that has reported to this device. */
typedef struct {
    char     code[PC_LINK_CODE_MAX];
    char     host[PC_STATUS_HOST_MAX];
    char     ip[16];
    uint32_t packets;
    int64_t  last_ms;
} pc_peer_t;

struct pc_link_t {    uint16_t             port;
    int                  sock;
    TaskHandle_t         task;
    SemaphoreHandle_t    lock;
    /* Given by the receive task immediately before it deletes itself, so the
     * owner knows the handle is no longer referenced. */
    SemaphoreHandle_t    stopped;
    pc_link_event_cb_t   cb;
    void                *cb_ctx;
    pc_sample_t          sample;
    pc_link_state_t      state;
    int64_t              last_rx_ms;   /* -1 before the first packet */
    pc_link_identity_t   identity;
    int64_t              last_reply_ms;
    char                 peer[16];     /* sender of the last packet */
    uint32_t             packets;
    /* Several PC agents may report to one device. Only the PC whose pairing
     * code (and address or host name) matches fills the sample shown on
     * screen; the others are just remembered so the pairing page can report
     * that they are reachable. */
    pc_peer_t            peers[PC_LINK_PEER_MAX];
    char                 paired[PC_LINK_CODE_MAX];  /* empty = not paired */
    char                 paired_ip[16];             /* bound PC at pairing time */
    char                 paired_host[PC_STATUS_HOST_MAX];
    bool                 running;
};

/* ------------------------------------------------------------- parsing */

/* The payload is a flat JSON object produced by our own PC agent, so a small
 * scanner is enough and keeps the application free of a JSON dependency. */

static const char *find_value(const char *json, size_t len, const char *key)
{
    const size_t key_len = strlen(key);
    for (size_t i = 0; i + key_len + 2 <= len; ++i) {
        if (json[i] != '"' || memcmp(json + i + 1, key, key_len) != 0 ||
            json[i + 1 + key_len] != '"') {
            continue;
        }
        /* The key must be preceded by '{' or ',' to exclude value strings. */
        size_t back = i;
        while (back > 0 && (json[back - 1] == ' ' || json[back - 1] == '\t')) {
            back--;
        }
        if (back == 0 || (json[back - 1] != '{' && json[back - 1] != ',')) {
            continue;
        }
        size_t j = i + key_len + 2;
        while (j < len && (json[j] == ' ' || json[j] == '\t')) {
            j++;
        }
        if (j >= len || json[j] != ':') {
            continue;
        }
        j++;
        while (j < len && (json[j] == ' ' || json[j] == '\t')) {
            j++;
        }
        return j < len ? json + j : NULL;
    }
    return NULL;
}

static void copy_string(const char *json, size_t len, const char *key, char *out,
                        size_t capacity)
{
    const char *p = find_value(json, len, key);
    if (!p || *p != '"' || capacity == 0) {
        return;
    }
    p++;
    size_t written = 0;
    while (*p && *p != '"' && written + 1 < capacity) {
        if (p[0] == '\\' && p[1]) {
            p++;    /* keep the escaped character as-is */
        }
        out[written++] = *p++;
    }
    out[written] = '\0';
}

static bool read_number(const char *json, size_t len, const char *key,
                        double *out)
{
    const char *p = find_value(json, len, key);
    if (!p) {
        return false;
    }
    char *end = NULL;
    double value = strtod(p, &end);
    if (end == p) {
        return false;
    }
    *out = value;
    return true;
}

static int percent(float used, float total)
{
    if (total <= 0.0f) {
        return 0;
    }
    int value = (int)((used / total) * 100.0f + 0.5f);
    return value < 0 ? 0 : (value > 100 ? 100 : value);
}

static bool parse_sample(const char *json, size_t len, pc_sample_t *out)
{
    pc_sample_t s = {0};
    double number = 0.0;

    copy_string(json, len, "host", s.host, sizeof(s.host));
    copy_string(json, len, "os", s.os, sizeof(s.os));

    if (!s.host[0] && !find_value(json, len, "cpu")) {
        ESP_LOGW(TAG, "ignoring unrecognised status packet (%u bytes)",
                 (unsigned)len);
        return false;
    }

    if (read_number(json, len, "cpu", &number)) {
        s.cpu = (int)number;
    }
    if (read_number(json, len, "mem_used", &number)) {
        s.mem_used_gb = (float)number;
    }
    if (read_number(json, len, "mem_total", &number)) {
        s.mem_total_gb = (float)number;
    }
    s.mem_percent = percent(s.mem_used_gb, s.mem_total_gb);

    if (read_number(json, len, "net_down", &number)) {
        s.net_down_mbps = (float)number;
    }
    if (read_number(json, len, "net_up", &number)) {
        s.net_up_mbps = (float)number;
    }
    if (read_number(json, len, "uptime", &number)) {
        s.uptime_s = (int64_t)number;
    }
    if (read_number(json, len, "ts", &number)) {
        s.pc_epoch_s = (int64_t)number;
    }
    if (read_number(json, len, "cpu_temp", &number)) {
        s.has_cpu_temp = true;
        s.cpu_temp = (int)number;
    }
    if (read_number(json, len, "cpu_power", &number)) {
        s.has_cpu_power = true;
        s.cpu_power = (float)number;
    }
    if (read_number(json, len, "cpu_freq", &number)) {
        s.has_cpu_freq = true;
        s.cpu_freq = (int)number;
    }
    if (read_number(json, len, "gpu", &number)) {
        s.has_gpu = true;
        s.gpu = (int)number;
    }
    if (read_number(json, len, "gpu_temp", &number)) {
        s.has_gpu_temp = true;
        s.gpu_temp = (int)number;
    }
    if (read_number(json, len, "gpu_mem_used", &number)) {
        s.gpu_mem_used_gb = (float)number;
    }
    if (read_number(json, len, "gpu_mem_total", &number)) {
        s.gpu_mem_total_gb = (float)number;
    }
    s.gpu_mem_percent = percent(s.gpu_mem_used_gb, s.gpu_mem_total_gb);
    s.has_gpu_mem = s.gpu_mem_total_gb > 0.0f;

    *out = s;
    return true;
}

/* ------------------------------------------------------------- task */

/* Answer one status packet with this device's identity so the PC can show
 * which device it reached. Sent at most every PC_LINK_REPLY_INTERVAL_MS. */
static void reply_identity(pc_link_handle_t h, const struct sockaddr *dest,
                           socklen_t dest_len, int64_t now_ms, bool active)
{
    if (xSemaphoreTake(h->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const pc_link_identity_t id = h->identity;
    h->last_reply_ms = now_ms;
    xSemaphoreGive(h->lock);

    if (id.name[0] == '\0') {
        return;                     /* identity not published yet */
    }

    char reply[PC_LINK_REPLY_MAX];
    const int len = snprintf(reply, sizeof(reply),
                             "{\"dev\":\"pc_display\",\"name\":\"%s\","
                             "\"model\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\","
                             "\"active\":%s}",
                             id.name, id.model, id.version, id.ip,
                             active ? "true" : "false");
    if (len <= 0 || (size_t)len >= sizeof(reply)) {
        return;
    }
    (void)sendto(h->sock, reply, (size_t)len, 0, dest, dest_len);
}

/* Slot for @p ip: its existing entry, a free one, or the least recently seen
 * entry (never the paired PC's slot, so the bound PC keeps its entry). */
static int peer_slot(pc_link_handle_t h, const char *ip)
{
    int free_slot = -1;
    int oldest = -1;
    for (int i = 0; i < PC_LINK_PEER_MAX; ++i) {
        if (h->peers[i].ip[0] == '\0') {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (strcmp(h->peers[i].ip, ip) == 0) {
            return i;
        }
        const bool is_paired = h->paired[0] &&
                               strcmp(h->peers[i].code, h->paired) == 0;
        if (is_paired) {
            continue;
        }
        if (oldest < 0 || h->peers[i].last_ms < h->peers[oldest].last_ms) {
            oldest = i;
        }
    }
    if (free_slot >= 0) {
        return free_slot;
    }
    return oldest >= 0 ? oldest : 0;
}

static void set_state(pc_link_handle_t h, pc_link_state_t state)
{
    if (xSemaphoreTake(h->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    bool changed = h->state != state;
    h->state = state;
    pc_link_event_cb_t cb = h->cb;
    void *ctx = h->cb_ctx;
    xSemaphoreGive(h->lock);

    if (changed && cb) {
        cb(state, ctx);
    }
}

static void link_task(void *arg)
{
    pc_link_handle_t h = arg;
    char buffer[PC_LINK_RX_BUFFER];

    while (h->running) {
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        int received = recvfrom(h->sock, buffer, sizeof(buffer) - 1, 0,
                                (struct sockaddr *)&source, &source_len);
        if (received > 0) {
            buffer[received] = '\0';
            pc_sample_t sample;
            char code[PC_LINK_CODE_MAX] = "";
            if (xSemaphoreTake(h->lock, portMAX_DELAY) == pdTRUE) {
                sample = h->sample;
                xSemaphoreGive(h->lock);
            }
            if (!parse_sample(buffer, (size_t)received, &sample)) {
                continue;
            }
            (void)copy_string(buffer, (size_t)received, "code", code, sizeof(code));

            const int64_t now_ms = esp_timer_get_time() / 1000;
            char ip[16] = "";
            (void)inet_ntop(AF_INET, &source.sin_addr, ip, sizeof(ip));

            bool accept = false;
            bool paired_sender = false;
            if (xSemaphoreTake(h->lock, portMAX_DELAY) == pdTRUE) {
                const int slot = peer_slot(h, ip);
                pc_peer_t *peer = &h->peers[slot];
                strlcpy(peer->ip, ip, sizeof(peer->ip));
                if (code[0]) {
                    strlcpy(peer->code, code, sizeof(peer->code));
                }
                if (sample.host[0]) {
                    strlcpy(peer->host, sample.host, sizeof(peer->host));
                }
                peer->packets++;
                peer->last_ms = now_ms;

                /* Only the paired PC's samples reach the screen. A packet
                 * without metrics (no "cpu") is a pairing announcement. */
                const bool has_metrics = strstr(buffer, "\"cpu\"") != NULL;
                const bool code_matches = code[0] && h->paired[0] &&
                                          strcmp(code, h->paired) == 0;
                /* The IP may change with DHCP, so the host name counts as
                 * well: a colliding code on an unrelated PC stays out. */
                const bool known_sender =
                    (h->paired_ip[0] == '\0' && h->paired_host[0] == '\0') ||
                    (h->paired_ip[0] && strcmp(ip, h->paired_ip) == 0) ||
                    (h->paired_host[0] && sample.host[0] &&
                     strcmp(sample.host, h->paired_host) == 0);
                paired_sender = code_matches;
                if (code_matches && known_sender && has_metrics) {
                    h->sample = sample;
                    h->last_rx_ms = now_ms;
                    h->packets++;
                    strlcpy(h->peer, ip, sizeof(h->peer));
                    accept = true;
                }
                xSemaphoreGive(h->lock);
            }
            if (accept) {
                set_state(h, PC_LINK_ONLINE);
            }

            if (now_ms - h->last_reply_ms >= PC_LINK_REPLY_INTERVAL_MS) {
                reply_identity(h, (struct sockaddr *)&source, source_len,
                               now_ms, paired_sender);
            }
            continue;
        }

        if (received < 0) {
            /* SO_RCVTIMEO expiry or a transient error: re-check staleness. */
            int64_t age = pc_link_age_ms(h);
            if (age >= 0 && age > PC_LINK_TIMEOUT_MS) {
                set_state(h, PC_LINK_MISSING);
            }
        }
    }

    close(h->sock);
    h->sock = -1;
    xSemaphoreGive(h->stopped);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- public */

static void load_pairing(pc_link_handle_t h)
{
    nvs_handle_t nvs;
    if (nvs_open(PC_LINK_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = sizeof(h->paired);
    if (nvs_get_str(nvs, PC_LINK_NVS_KEY_PAIR, h->paired, &len) != ESP_OK) {
        h->paired[0] = '\0';
    }
    size_t ip_len = sizeof(h->paired_ip);
    if (nvs_get_str(nvs, PC_LINK_NVS_KEY_PAIR_IP, h->paired_ip, &ip_len)
            != ESP_OK) {
        h->paired_ip[0] = '\0';
    }
    size_t host_len = sizeof(h->paired_host);
    if (nvs_get_str(nvs, PC_LINK_NVS_KEY_HOST, h->paired_host, &host_len)
            != ESP_OK) {
        h->paired_host[0] = '\0';
    }
    nvs_close(nvs);
    if (h->paired[0]) {
        ESP_LOGI(TAG, "paired with PC code %s (%s / %s)", h->paired,
                 h->paired_host[0] ? h->paired_host : "-",
                 h->paired_ip[0] ? h->paired_ip : "-");
    }
}

static esp_err_t store_pairing(const char *code, const char *ip,
                               const char *host)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PC_LINK_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (code && code[0]) {
        err = nvs_set_str(nvs, PC_LINK_NVS_KEY_PAIR, code);
        if (err == ESP_OK) {
            err = nvs_set_str(nvs, PC_LINK_NVS_KEY_PAIR_IP, ip ? ip : "");
        }
        if (err == ESP_OK) {
            err = nvs_set_str(nvs, PC_LINK_NVS_KEY_HOST, host ? host : "");
        }
    } else {
        err = nvs_erase_key(nvs, PC_LINK_NVS_KEY_PAIR);
        if (err == ESP_OK) {
            err = nvs_erase_key(nvs, PC_LINK_NVS_KEY_PAIR_IP);
        }
        if (err == ESP_OK) {
            err = nvs_erase_key(nvs, PC_LINK_NVS_KEY_HOST);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

bool pc_link_paired(pc_link_handle_t h)
{
    return h != NULL && h->paired[0] != '\0';
}

esp_err_t pc_link_paired_code(pc_link_handle_t h, char *out, size_t capacity)
{
    ESP_RETURN_ON_FALSE(h && out && capacity, ESP_ERR_INVALID_ARG, TAG, "bad args");
    strlcpy(out, h->paired, capacity);
    return ESP_OK;
}

esp_err_t pc_link_pair(pc_link_handle_t h, const char *code)
{
    ESP_RETURN_ON_FALSE(h && code, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(code[0], ESP_ERR_INVALID_ARG, TAG, "empty code");

    /* Only a code that a PC is actually broadcasting may be bound, so a typo is
     * reported instead of silently doing nothing. The matching PC's address and
     * host name are remembered too and later used to keep a colliding code from
     * another machine out. */
    bool found = false;
    char ip[16] = "";
    char host[PC_STATUS_HOST_MAX] = "";
    if (xSemaphoreTake(h->lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (int i = 0; i < PC_LINK_PEER_MAX; ++i) {
        if (h->peers[i].code[0] && strcmp(h->peers[i].code, code) == 0) {
            found = true;
            strlcpy(ip, h->peers[i].ip, sizeof(ip));
            strlcpy(host, h->peers[i].host, sizeof(host));
            break;
        }
    }
    if (found) {
        strlcpy(h->paired, code, sizeof(h->paired));
        strlcpy(h->paired_ip, ip, sizeof(h->paired_ip));
        strlcpy(h->paired_host, host, sizeof(h->paired_host));
    }
    xSemaphoreGive(h->lock);

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "paired with PC code %s (%s / %s)", code,
             host[0] ? host : "-", ip[0] ? ip : "-");
    return store_pairing(code, ip, host);
}

esp_err_t pc_link_unpair(pc_link_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    if (xSemaphoreTake(h->lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    h->paired[0] = '\0';
    h->paired_ip[0] = '\0';
    h->paired_host[0] = '\0';
    h->last_rx_ms = -1;      /* no sample until a new pairing delivers one */
    memset(&h->sample, 0, sizeof(h->sample));
    xSemaphoreGive(h->lock);
    set_state(h, PC_LINK_NONE);
    return store_pairing(NULL, NULL, NULL);
}

int pc_link_peers(pc_link_handle_t h, pc_link_peer_t *out, int max)
{
    if (!h || !out || max <= 0) {
        return 0;
    }
    if (xSemaphoreTake(h->lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }
    int order[PC_LINK_PEER_MAX];
    int total = 0;
    for (int i = 0; i < PC_LINK_PEER_MAX; ++i) {
        if (h->peers[i].ip[0] != '\0') {
            order[total++] = i;
        }
    }
    /* most recently seen first (tiny N: selection sort) */
    for (int a = 0; a < total; ++a) {
        for (int b = a + 1; b < total; ++b) {
            if (h->peers[order[b]].last_ms > h->peers[order[a]].last_ms) {
                const int tmp = order[a];
                order[a] = order[b];
                order[b] = tmp;
            }
        }
    }
    int count = 0;
    for (int i = 0; i < total && count < max; ++i) {
        const pc_peer_t *peer = &h->peers[order[i]];
        memset(&out[count], 0, sizeof(out[count]));
        strlcpy(out[count].code, peer->code, sizeof(out[count].code));
        strlcpy(out[count].host, peer->host, sizeof(out[count].host));
        strlcpy(out[count].ip, peer->ip, sizeof(out[count].ip));
        out[count].packets = peer->packets;
        out[count].paired = h->paired[0] &&
                            strcmp(peer->code, h->paired) == 0;
        count++;
    }
    xSemaphoreGive(h->lock);
    return count;
}

esp_err_t pc_link_create(pc_link_handle_t *ret_handle, uint16_t port)
{
    ESP_RETURN_ON_FALSE(ret_handle, ESP_ERR_INVALID_ARG, TAG, "null output");

    pc_link_handle_t h = calloc(1, sizeof(struct pc_link_t));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");
    h->port = port;
    h->sock = -1;
    h->last_rx_ms = -1;
    /* Negative so the very first status packet is answered immediately. */
    h->last_reply_ms = -PC_LINK_REPLY_INTERVAL_MS;
    h->state = PC_LINK_NONE;
    load_pairing(h);
    h->lock = xSemaphoreCreateMutex();
    h->stopped = xSemaphoreCreateBinary();
    if (!h->lock || !h->stopped) {
        vSemaphoreDelete(h->lock);
        vSemaphoreDelete(h->stopped);
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *ret_handle = h;
    return ESP_OK;
}

esp_err_t pc_link_start(pc_link_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");

    h->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(h->sock >= 0, ESP_FAIL, TAG, "create socket");

    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = PC_LINK_POLL_MS * 1000,
    };
    setsockopt(h->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(h->port),
    };
    if (bind(h->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(h->sock);
        h->sock = -1;
        ESP_LOGE(TAG, "bind udp %u failed", (unsigned)h->port);
        return ESP_FAIL;
    }

    h->running = true;
    if (xTaskCreate(link_task, "pc_link", PC_LINK_TASK_STACK, h,
                    PC_LINK_TASK_PRIORITY, &h->task) != pdPASS) {
        h->running = false;
        close(h->sock);
        h->sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "listening for PC status on udp/%u", (unsigned)h->port);
    return ESP_OK;
}

void pc_link_delete(pc_link_handle_t h)
{
    if (!h) {
        return;
    }
    h->running = false;
    if (h->task) {
        /* Wait for the receive task to close the socket and release the
         * handle instead of guessing a delay. */
        if (xSemaphoreTake(h->stopped, pdMS_TO_TICKS(PC_LINK_POLL_MS + 1000))
                != pdTRUE) {
            ESP_LOGW(TAG, "receive task did not stop; leaking to stay safe");
            return;
        }
        h->task = NULL;
    }
    if (h->sock >= 0) {
        close(h->sock);
        h->sock = -1;
    }
    if (h->stopped) {
        vSemaphoreDelete(h->stopped);
    }
    if (h->lock) {
        vSemaphoreDelete(h->lock);
    }
    free(h);
}

esp_err_t pc_link_register_cb(pc_link_handle_t h, pc_link_event_cb_t cb, void *ctx)
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

/** Keep announced text safe to embed in the reply JSON. */
static void sanitize_field(char *text, size_t capacity)
{
    for (size_t i = 0; i < capacity && text[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == '"' || c == '\\') {
            text[i] = '-';
        }
    }
}

esp_err_t pc_link_set_identity(pc_link_handle_t h,
                               const pc_link_identity_t *identity)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(identity, ESP_ERR_INVALID_ARG, TAG, "no identity");

    pc_link_identity_t copy = *identity;
    copy.name[sizeof(copy.name) - 1] = '\0';
    copy.model[sizeof(copy.model) - 1] = '\0';
    copy.version[sizeof(copy.version) - 1] = '\0';
    copy.ip[sizeof(copy.ip) - 1] = '\0';
    sanitize_field(copy.name, sizeof(copy.name));
    sanitize_field(copy.model, sizeof(copy.model));
    sanitize_field(copy.version, sizeof(copy.version));
    sanitize_field(copy.ip, sizeof(copy.ip));

    if (xSemaphoreTake(h->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    h->identity = copy;
    xSemaphoreGive(h->lock);
    return ESP_OK;
}

esp_err_t pc_link_snapshot(pc_link_handle_t h, pc_sample_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "bad args");
    if (xSemaphoreTake(h->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    bool have_data = h->last_rx_ms >= 0;
    *out = h->sample;
    xSemaphoreGive(h->lock);
    return have_data ? ESP_OK : ESP_ERR_NOT_FOUND;
}

pc_link_state_t pc_link_state(pc_link_handle_t h)
{
    return h ? h->state : PC_LINK_NONE;
}

int64_t pc_link_age_ms(pc_link_handle_t h)
{
    if (!h || h->last_rx_ms < 0) {
        return -1;
    }
    return esp_timer_get_time() / 1000 - h->last_rx_ms;
}

esp_err_t pc_link_peer(pc_link_handle_t h, char *out, size_t capacity)
{
    ESP_RETURN_ON_FALSE(h && out && capacity, ESP_ERR_INVALID_ARG, TAG, "bad args");
    if (xSemaphoreTake(h->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (h->peer[0] == '\0') {
        strlcpy(out, "0.0.0.0", capacity);
    } else {
        strlcpy(out, h->peer, capacity);
    }
    xSemaphoreGive(h->lock);
    return ESP_OK;
}

uint32_t pc_link_packets(pc_link_handle_t h)
{
    if (!h) {
        return 0;
    }
    return h->packets;
}
