// SPDX-License-Identifier: Apache-2.0
//
// Shared application state for the ESP-Mosaico PC status panel.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PC_STATUS_HOST_MAX 32
#define PC_STATUS_OS_MAX   24

/** One PC status sample, as received from the PC agent. */
typedef struct {
    char     host[PC_STATUS_HOST_MAX];
    char     os[PC_STATUS_OS_MAX];
    int      cpu;            /* percent, 0..100 */
    int      mem_percent;    /* percent, 0..100 */
    float    mem_used_gb;
    float    mem_total_gb;
    float    net_down_mbps;  /* MB/s */
    float    net_up_mbps;    /* MB/s */
    /* Optional sensors: `has_*` is false when the PC cannot provide them. */
    bool     has_cpu_temp;
    int      cpu_temp;       /* degrees Celsius */
    bool     has_cpu_power;
    float    cpu_power;      /* watts (package) */
    bool     has_cpu_freq;
    int      cpu_freq;       /* MHz (average core clock) */
    bool     has_gpu;
    int      gpu;            /* percent, 0..100 */
    bool     has_gpu_temp;
    int      gpu_temp;       /* degrees Celsius */
    bool     has_gpu_mem;
    float    gpu_mem_used_gb;
    float    gpu_mem_total_gb;
    int      gpu_mem_percent; /* dedicated VRAM in use, 0..100 */
    int64_t  uptime_s;
    int64_t  pc_epoch_s;     /* PC wall-clock time, for the panel clock */
} pc_sample_t;

/** Link state between the device and the PC agent. */
typedef enum {
    PC_LINK_NONE = 0,   /* no sample received yet */
    PC_LINK_MISSING,    /* samples stopped arriving */
    PC_LINK_ONLINE,
} pc_link_state_t;

/** Wi-Fi provisioning / connection state, owned by the wifi manager. */
typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED,
} wifi_state_t;

/** A scanned access point. */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_ap_t;

#define WIFI_SCAN_MAX 6
