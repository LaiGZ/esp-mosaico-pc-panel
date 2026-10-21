// SPDX-License-Identifier: Apache-2.0
//
// Desktop mirror receiver: the PC client streams JPEG frames over UDP; this
// module reassembles them and hands the newest complete frame to the UI task.
// Decoding/display is done by ESP-GSP (esp_gsp_set_image), which copies the
// payload immediately, so no frame data lives beyond the poll call.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** UDP port the PC client streams to (status packets keep using 33340). */
#define PC_MIRROR_PORT 33341

/** Upper bound for one encoded frame (480x420 JPEG is tens of KB). */
#define PC_MIRROR_FRAME_MAX (128 * 1024)

/** Wire header: 'P''C', type, frame_id, index, count, payload length. */
#define PC_MIRROR_HEADER_SIZE 11
#define PC_MIRROR_TYPE_FRAME  0

/** Payload bytes per datagram; the sender must use the same stride. */
#define PC_MIRROR_CHUNK       1200

/** Open the receiving socket and start the reassembly task. */
esp_err_t pc_mirror_start(void);

/**
 * Hand the newest complete frame to @p consume, if one is waiting.
 * Runs the callback with the internal lock held; the callback must not block
 * (esp_gsp_set_image copies the payload and returns).
 */
typedef void (*pc_mirror_consume_fn)(const uint8_t *data, size_t size, void *ctx);
void pc_mirror_poll(pc_mirror_consume_fn consume, void *ctx);

/** True when a frame arrived within the last @p idle_ms milliseconds. */
bool pc_mirror_active(uint32_t idle_ms);

#ifdef __cplusplus
}
#endif
