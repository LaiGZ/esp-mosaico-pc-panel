// SPDX-License-Identifier: Apache-2.0
//
// Receives PC status JSON datagrams from the PC agent and keeps the latest
// sample. Owns the UDP socket and its receive task.

#pragma once

#include <stdint.h>

#include "app_state.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PC_LINK_DEFAULT_PORT 33340
/** Samples older than this mark the PC as missing. */
#define PC_LINK_TIMEOUT_MS   5000
/** Length of the pairing code shown by the PC client (4 digits + NUL). */
#define PC_LINK_CODE_MAX     8
/** PC agents remembered for pairing and the information page. */
#define PC_LINK_PEER_MAX     4

typedef struct pc_link_t *pc_link_handle_t;

/** Called when the link state changes (receive task context). */
typedef void (*pc_link_event_cb_t)(pc_link_state_t state, void *ctx);

/** A PC agent that announced itself on the LAN. */
typedef struct {
    char     code[PC_LINK_CODE_MAX];  /* pairing code it broadcasts */
    char     host[PC_STATUS_HOST_MAX];
    char     ip[16];
    uint32_t packets;
    bool     paired;                  /* this is the PC the device is bound to */
} pc_link_peer_t;

/** Fill the peer table (most recently active first). Returns the count filled. */
int pc_link_peers(pc_link_handle_t handle, pc_link_peer_t *out, int max);

/** True once a pairing code has been accepted; the device shows PC data only
 *  from that PC. */
bool pc_link_paired(pc_link_handle_t handle);

/** Copy the paired code (empty string when not paired). */
esp_err_t pc_link_paired_code(pc_link_handle_t handle, char *out, size_t capacity);

/**
 * Bind the device to the PC broadcasting @p code.
 *
 * @return ESP_OK when such a PC has been heard from (and the code is stored in
 *         NVS), ESP_ERR_NOT_FOUND when no announced code matches, so a typo is
 *         reported instead of silently doing nothing.
 */
esp_err_t pc_link_pair(pc_link_handle_t handle, const char *code);

/** Forget the pairing; the device returns to its pairing page. */
esp_err_t pc_link_unpair(pc_link_handle_t handle);

/**
 * Identity announced back to the PC agent. The agent shows it as the name of
 * the device it reached, which is friendlier than a bare IP address and also
 * proves the device is actually receiving (not just that packets were sent).
 */
typedef struct {
    char name[24];      /* human readable, e.g. "PC 状态面板" */
    char model[16];     /* board, e.g. "ESP-Mosaico" */
    char version[12];   /* application version */
    char ip[16];        /* current device address */
} pc_link_identity_t;

esp_err_t pc_link_create(pc_link_handle_t *ret_handle, uint16_t port);
esp_err_t pc_link_start(pc_link_handle_t handle);
void pc_link_delete(pc_link_handle_t handle);

/**
 * Publish the identity sent back to the PC agent. Safe to call at runtime,
 * for example when the DHCP lease changes; replies are rate limited inside.
 */
esp_err_t pc_link_set_identity(pc_link_handle_t handle,
                               const pc_link_identity_t *identity);

esp_err_t pc_link_register_cb(pc_link_handle_t handle, pc_link_event_cb_t cb,
                              void *ctx);

/** Copy the latest sample. Returns ESP_ERR_NOT_FOUND before the first packet. */
esp_err_t pc_link_snapshot(pc_link_handle_t handle, pc_sample_t *out);

pc_link_state_t pc_link_state(pc_link_handle_t handle);

/** Milliseconds since the last received packet, or -1 when none arrived yet. */
int64_t pc_link_age_ms(pc_link_handle_t handle);

/** Address of the last sender as text, or "0.0.0.0" before the first packet. */
esp_err_t pc_link_peer(pc_link_handle_t handle, char *out, size_t capacity);

/** Number of accepted status packets since start. */
uint32_t pc_link_packets(pc_link_handle_t handle);

#ifdef __cplusplus
}
#endif
