// SPDX-License-Identifier: Apache-2.0

#include "heap_watch.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "heap_watch";

#define HEAP_WATCH_INTERVAL_MS 10000
#define HEAP_WATCH_WARN_FRAG_PCT 50
#define HEAP_WATCH_WARN_LOW_BYTES 16384

/* The application's allocation-sensitive pool. */
#define HEAP_WATCH_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static int64_t s_next_ms;

static size_t fragmentation_pct(size_t free_bytes, size_t largest)
{
    if (free_bytes == 0) {
        return 0;
    }
    return 100 - (largest * 100) / free_bytes;
}

void heap_watch_poll(void)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < s_next_ms) {
        return;
    }
    s_next_ms = now_ms + HEAP_WATCH_INTERVAL_MS;

    const size_t free_bytes = heap_caps_get_free_size(HEAP_WATCH_CAPS);
    const size_t largest = heap_caps_get_largest_free_block(HEAP_WATCH_CAPS);
    const size_t low_water = heap_caps_get_minimum_free_size(HEAP_WATCH_CAPS);
    const size_t frag = fragmentation_pct(free_bytes, largest);

    ESP_LOGI(TAG, "internal: free=%u largest=%u min_free=%u frag=%u%%",
             (unsigned)free_bytes, (unsigned)largest, (unsigned)low_water,
             (unsigned)frag);

#if CONFIG_SPIRAM
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free > 0) {
        const size_t psram_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "psram:   free=%u largest=%u frag=%u%%",
                 (unsigned)psram_free, (unsigned)psram_largest,
                 (unsigned)fragmentation_pct(psram_free, psram_largest));
    }
#endif

    if (frag >= HEAP_WATCH_WARN_FRAG_PCT) {
        ESP_LOGW(TAG, "high fragmentation: %u%% (free=%u largest=%u)",
                 (unsigned)frag, (unsigned)free_bytes, (unsigned)largest);
    }
    if (low_water < HEAP_WATCH_WARN_LOW_BYTES) {
        ESP_LOGW(TAG, "low internal heap watermark: %u bytes",
                 (unsigned)low_water);
    }
}
