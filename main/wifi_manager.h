// SPDX-License-Identifier: Apache-2.0
//
// Wi-Fi station manager: credential storage, connection/reconnection and
// network scanning. Owns all esp_wifi interaction for the application.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_state.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wifi_manager_t *wifi_manager_handle_t;

/** Called from the esp_event task whenever the connection state changes. */
typedef void (*wifi_manager_event_cb_t)(wifi_state_t state, void *ctx);

/** Create the manager. Does not touch the radio yet. */
esp_err_t wifi_manager_create(wifi_manager_handle_t *ret_handle);

/** Initialise netif/Wi-Fi and connect using stored credentials when present. */
esp_err_t wifi_manager_start(wifi_manager_handle_t handle);

/** Stop the radio and release the manager. */
void wifi_manager_delete(wifi_manager_handle_t handle);

esp_err_t wifi_manager_register_cb(wifi_manager_handle_t handle,
                                   wifi_manager_event_cb_t cb, void *ctx);

/** True when an SSID has been stored in NVS. */
bool wifi_manager_has_credentials(wifi_manager_handle_t handle);

wifi_state_t wifi_manager_state(wifi_manager_handle_t handle);

/** Current IPv4 address as text, or "0.0.0.0" when not connected. */
esp_err_t wifi_manager_ip(wifi_manager_handle_t handle, char *out, size_t capacity);

/** Station signal strength in dBm, or 0 when not associated. */
int wifi_manager_rssi(wifi_manager_handle_t handle);

/** SSID the station is configured for (empty before provisioning). */
esp_err_t wifi_manager_ssid(wifi_manager_handle_t handle, char *out,
                            size_t capacity);

/**
 * Blocking scan. Call from an application task, never from an event handler.
 * @p out is filled with at most @p max access points; @p count receives the
 * number written.
 */
esp_err_t wifi_manager_scan(wifi_manager_handle_t handle, wifi_ap_t *out, int max,
                            int *count);

/** Persist the credentials and (re)connect. Blocking Wi-Fi calls are avoided. */
esp_err_t wifi_manager_connect(wifi_manager_handle_t handle, const char *ssid,
                               const char *password);

#ifdef __cplusplus
}
#endif
