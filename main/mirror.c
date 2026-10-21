// SPDX-License-Identifier: Apache-2.0
//
// Mirror receiver: reassembles chunked JPEG frames arriving over UDP and hands
// the newest complete frame to the UI task (see mirror.h).

#include <errno.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mirror.h"

static const char *TAG = "pc_mirror";

#define RX_SOCKET_BUFFER   (256 * 1024)
#define RX_TASK_STACK      4096
#define RX_TASK_PRIORITY   4
#define RX_LOCK_TIMEOUT_MS 20

/* Frame being assembled. */
static uint8_t *s_staging;
static size_t s_staging_len;
static uint16_t s_staging_id;
static uint16_t s_staging_chunks;
static uint16_t s_staging_have;
static bool s_staging_open;

/* Newest complete frame, waiting for the UI task. */
static uint8_t *s_ready;
static size_t s_ready_len;
static bool s_ready_open;

static SemaphoreHandle_t s_lock;
static volatile int64_t s_last_frame_ms;

static void release_buffers(void)
{
    heap_caps_free(s_staging);
    heap_caps_free(s_ready);
    s_staging = NULL;
    s_ready = NULL;
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t packet[1600];

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int buffer = RX_SOCKET_BUFFER;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PC_MIRROR_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(udp/%d) failed: errno %d", PC_MIRROR_PORT, errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on udp/%d", PC_MIRROR_PORT);

    while (true) {
        const int received = recv(sock, packet, sizeof(packet), 0);
        if (received < PC_MIRROR_HEADER_SIZE) {
            continue;
        }
        if (packet[0] != 'P' || packet[1] != 'C' || packet[2] != PC_MIRROR_TYPE_FRAME) {
            continue;
        }
        const uint16_t id = (uint16_t)(packet[3] | (packet[4] << 8));
        const uint16_t index = (uint16_t)(packet[5] | (packet[6] << 8));
        const uint16_t count = (uint16_t)(packet[7] | (packet[8] << 8));
        const uint16_t length = (uint16_t)(packet[9] | (packet[10] << 8));
        if (count == 0 || index >= count || length == 0 ||
            PC_MIRROR_HEADER_SIZE + (int)length > received) {
            continue;
        }
        const size_t offset = (size_t)index * PC_MIRROR_CHUNK;
        if (offset + length > PC_MIRROR_FRAME_MAX) {
            continue;
        }

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(RX_LOCK_TIMEOUT_MS)) != pdTRUE) {
            continue;
        }
        if (!s_staging_open || s_staging_id != id || s_staging_chunks != count) {
            s_staging_open = true;
            s_staging_id = id;
            s_staging_chunks = count;
            s_staging_have = 0;
            s_staging_len = 0;
        }
        memcpy(s_staging + offset, packet + PC_MIRROR_HEADER_SIZE, length);
        s_staging_have++;
        if (offset + length > s_staging_len) {
            s_staging_len = offset + length;
        }
        if (s_staging_have >= count) {
            /* Publish by swapping buffers: no 100 KB copy on the receive path. */
            uint8_t *swap = s_ready;
            s_ready = s_staging;
            s_staging = swap;
            s_ready_len = s_staging_len;
            s_ready_open = true;
            s_staging_open = false;
            s_last_frame_ms = esp_timer_get_time() / 1000;
            /* One line per ~7 s at 8 fps: enough to diagnose without flooding
             * the small retained log ring. */
            static uint32_t frames;
            if (frames++ % 60 == 0) {
                ESP_LOGI(TAG, "frame %u: %u B", (unsigned)frames, (unsigned)s_ready_len);
            }
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t pc_mirror_start(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_staging = heap_caps_malloc(PC_MIRROR_FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ready = heap_caps_malloc(PC_MIRROR_FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_staging || !s_ready) {
        ESP_LOGE(TAG, "no PSRAM for frame buffers");
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(rx_task, "pc_mirror", RX_TASK_STACK, NULL, RX_TASK_PRIORITY,
                    NULL) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void pc_mirror_poll(pc_mirror_consume_fn consume, void *ctx)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(RX_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return;
    }
    if (s_ready_open && consume) {
        consume(s_ready, s_ready_len, ctx);
        s_ready_open = false;
    }
    xSemaphoreGive(s_lock);
}

bool pc_mirror_active(uint32_t idle_ms)
{
    const int64_t last = s_last_frame_ms;
    if (last <= 0) {
        return false;
    }
    return (esp_timer_get_time() / 1000 - last) <= (int64_t)idle_ms;
}
