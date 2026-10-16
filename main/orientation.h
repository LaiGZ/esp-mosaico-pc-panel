// SPDX-License-Identifier: Apache-2.0
//
// Device orientation from the BMI270 accelerometer.
//
// Gravity gives a stable "which way is down" reading, so orientation is taken
// from the accelerometer rather than by integrating the gyroscope (which would
// drift). A candidate angle must hold for several consecutive samples before it
// is reported, so casual handling does not flip the screen.

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Rotation angles in degrees, matching the display presenter's rotations. */
typedef enum {
    ORIENTATION_0 = 0,
    ORIENTATION_90 = 90,
    ORIENTATION_180 = 180,
    ORIENTATION_270 = 270,
    ORIENTATION_UNKNOWN = -1,
} orientation_t;

/** Start the accelerometer. Returns an error when the IMU is unavailable. */
esp_err_t orientation_start(void);
void orientation_stop(void);

/** True once the accelerometer has been configured successfully. */
bool orientation_ready(void);

/**
 * Read back the latest accelerometer sample and settled angle (for diagnostics
 * and for showing the sensor state in the UI).
 * @return false when no sample has been read yet.
 */
bool orientation_snapshot(float *x, float *y, float *z, int *angle);

/**
 * Sample the IMU and return the settled orientation.
 *
 * @param out_angle      Receives the current angle once it is stable.
 * @param out_changed    True when the stable angle differs from the previous
 *                       call's stable angle (edge-triggered).
 * @return ESP_OK when a reading was taken; the outputs are left untouched on
 *         failure.
 */
esp_err_t orientation_poll(orientation_t *out_angle, bool *out_changed);

#ifdef __cplusplus
}
#endif
