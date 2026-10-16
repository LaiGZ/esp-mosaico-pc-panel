// SPDX-License-Identifier: Apache-2.0
//
// Device orientation from the BMI270 accelerometer.
//
// Uses the same sensor stack as the ESP-Mosaico factory firmware: the `bmi270`
// component's handle API on the shared I2C bus, rather than the BSP's own
// hand-built `bmi2_dev`.
//
// Why not the BSP helper: esp-mosaico-bsp's `bsp_imu_init()` builds a
// `struct bmi2_dev` by hand with `read_write_len = 32`, while the driver's own
// default (and the value its internal buffer maths assumes) is 46
// (`BMI2_FEAT_SIZE_IN_BYTES`). That mismatch corrupts memory during the
// firmware upload and the device aborts later, in an unrelated task, with no
// usable backtrace. The component wrapper configures the device correctly.
//
// Gravity gives a stable "which way is down" reading, so orientation is taken
// from the accelerometer rather than by integrating the gyroscope (which would
// drift). A candidate angle must hold for several consecutive samples before it
// is reported, so casual handling does not flip the screen.

#include "orientation.h"

#include <math.h>
#include <string.h>

#include "bmi270.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#include "driver/gpio.h"

static const char *TAG = "orientation";

/* Gravity magnitude in g before an orientation is considered. */
#define ORIENTATION_GRAVITY_G       0.70f
/*
 * The BMI270 driver returns raw sensor counts, not g: at the configured
 * +-2 G range the sensitivity is 16384 LSB/g (2^15 / 2). Keep this in step
 * with `BMI2_ACC_RANGE_2G` in configure_sensor().
 */
#define ORIENTATION_ACCEL_LSB_PER_G 16384.0f
/* Consecutive identical samples needed to accept a new orientation.
 * One sample per app tick (500 ms), so two samples settle in about a second. */
#define ORIENTATION_STABLE_SAMPLES  2
/* Retry the sensor bring-up at this interval while it is unavailable. */
#define ORIENTATION_RETRY_US        (5LL * 1000 * 1000)
#define ORIENTATION_I2C_ADDR        0x68
#define ORIENTATION_I2C_ALT_ADDR    0x69

/*
 * The BMI270 is mounted 90 degrees clockwise relative to the display, so the
 * sensor axes do not match the screen axes. The factory firmware derives
 * screen pitch from the sensor's Y/Z pair and screen roll from the sensor's X
 * axis, i.e. the display's vertical gravity component is `acc.y` and its
 * horizontal component is `acc.x`. Those two are therefore what decide which
 * edge is pointing down.
 *
 * If a future board revision mounts the sensor differently, only these two
 * constants change; if 90/270 come out swapped, swap the two assignments.
 */
#define ORIENTATION_DOWN_IS_PLUS_Y      (1)   /* +y:  portrait (0 degrees)   */
#define ORIENTATION_DOWN_IS_PLUS_X      (1)   /* +x:  landscape (90 degrees) */
#define ORIENTATION_ANGLE_FOR_PLUS_X    ORIENTATION_90
#define ORIENTATION_ANGLE_FOR_MINUS_X   ORIENTATION_270

static bmi270_handle_t s_sensor;
static i2c_bus_handle_t s_bus;
static int64_t s_next_attempt_us;
static int     s_candidate = ORIENTATION_UNKNOWN;
static int     s_candidate_count;
static int     s_stable = ORIENTATION_UNKNOWN;
static bool    s_started;
static bool    s_have_sample;
static float   s_last_x;
static float   s_last_y;
static float   s_last_z;

static esp_err_t create_bus(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    /* Shares the port the BSP already brought up for the touch panel;
     * i2c_bus_create() returns the existing bus for that port. */
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "BSP I2C init failed");

    bsp_board_variant_t variant;
    ESP_RETURN_ON_ERROR(bsp_board_variant_get(&variant), TAG, "board variant");
    const bool v1_0 = variant == BSP_BOARD_VARIANT_V1_0;
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = v1_0 ? BSP_I2C_SDA_V1_0 : BSP_I2C_SDA_V1_2,
        .scl_io_num = v1_0 ? BSP_I2C_SCL_V1_0 : BSP_I2C_SCL_V1_2,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    s_bus = i2c_bus_create(BSP_I2C_PORT, &config);
    ESP_RETURN_ON_FALSE(s_bus, ESP_FAIL, TAG, "shared I2C bus unavailable");
    return ESP_OK;
}

static esp_err_t configure_sensor(bmi270_handle_t sensor)
{
    const uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
    struct bmi2_sens_config config[2] = {0};
    config[0].type = BMI2_ACCEL;
    config[1].type = BMI2_GYRO;

    int8_t result = bmi2_set_adv_power_save(BMI2_DISABLE, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "disable advanced power save failed: %d", result);

    result = bmi2_get_sensor_config(config, 2, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "get sensor config failed: %d", result);

    config[0].cfg.acc.odr = BMI2_ACC_ODR_100HZ;
    config[0].cfg.acc.range = BMI2_ACC_RANGE_2G;
    config[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    config[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    config[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
    config[1].cfg.gyr.range = BMI2_GYR_RANGE_500;
    config[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    config[1].cfg.gyr.noise_perf = BMI2_PERF_OPT_MODE;
    config[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;

    result = bmi2_set_sensor_config(config, 2, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "set sensor config failed: %d", result);

    result = bmi270_sensor_enable(sensors, 2, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "enable accel/gyro failed: %d", result);
    return ESP_OK;
}

static esp_err_t try_create_sensor(void)
{
    esp_err_t err = create_bus();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t addr = ORIENTATION_I2C_ADDR;
    bmi270_i2c_config_t config = {
        .i2c_handle = s_bus,
        .i2c_addr = addr,
    };
    err = bmi270_sensor_create(&config, &s_sensor);
    if (err != ESP_OK) {
        config.i2c_addr = ORIENTATION_I2C_ALT_ADDR;
        err = bmi270_sensor_create(&config, &s_sensor);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "BMI270 responded at 0x%02x", ORIENTATION_I2C_ALT_ADDR);
        }
    }
    if (err != ESP_OK) {
        s_sensor = NULL;
        return err;
    }
    return configure_sensor(s_sensor);
}

esp_err_t orientation_start(void)
{
    esp_err_t err = try_create_sensor();
    if (err != ESP_OK) {
        /* Keep the panel usable and retry from orientation_poll(). */
        s_next_attempt_us = esp_timer_get_time() + ORIENTATION_RETRY_US;
        ESP_LOGW(TAG, "BMI270 unavailable (%s); will retry", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    s_candidate = ORIENTATION_UNKNOWN;
    s_candidate_count = 0;
    s_stable = ORIENTATION_UNKNOWN;
    ESP_LOGI(TAG, "BMI270 ready for orientation sensing");
    return ESP_OK;
}

void orientation_stop(void)
{
    if (s_sensor) {
        (void)bmi270_sensor_del(s_sensor);
        s_sensor = NULL;
    }
    /* The I2C bus is shared with touch/battery; never delete it here. */
    s_started = false;
}

bool orientation_ready(void)
{
    return s_sensor != NULL;
}

bool orientation_snapshot(float *x, float *y, float *z, int *angle)
{
    if (!s_have_sample) {
        return false;
    }
    if (x) {
        *x = s_last_x;
    }
    if (y) {
        *y = s_last_y;
    }
    if (z) {
        *z = s_last_z;
    }
    if (angle) {
        *angle = s_stable;
    }
    return true;
}

static int classify(float x, float y)
{
    if (ORIENTATION_DOWN_IS_PLUS_Y * y >= ORIENTATION_GRAVITY_G) {
        return ORIENTATION_0;
    }
    if (ORIENTATION_DOWN_IS_PLUS_Y * y <= -ORIENTATION_GRAVITY_G) {
        return ORIENTATION_180;
    }
    if (ORIENTATION_DOWN_IS_PLUS_X * x >= ORIENTATION_GRAVITY_G) {
        return ORIENTATION_ANGLE_FOR_PLUS_X;
    }
    if (ORIENTATION_DOWN_IS_PLUS_X * x <= -ORIENTATION_GRAVITY_G) {
        return ORIENTATION_ANGLE_FOR_MINUS_X;
    }
    return ORIENTATION_UNKNOWN;    /* face-up/down or in motion */
}

esp_err_t orientation_poll(orientation_t *out_angle, bool *out_changed)
{
    if (!out_angle || !out_changed) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_sensor) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us < s_next_attempt_us) {
            return ESP_ERR_INVALID_STATE;
        }
        s_next_attempt_us = now_us + ORIENTATION_RETRY_US;
        if (orientation_start() != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    struct bmi2_sens_data sample = {0};
    if (bmi2_get_sensor_data(&sample, s_sensor) != BMI2_OK) {
        return ESP_FAIL;
    }
    const float gx = sample.acc.x / ORIENTATION_ACCEL_LSB_PER_G;
    const float gy = sample.acc.y / ORIENTATION_ACCEL_LSB_PER_G;
    const float gz = sample.acc.z / ORIENTATION_ACCEL_LSB_PER_G;
    s_last_x = gx;
    s_last_y = gy;
    s_last_z = gz;
    s_have_sample = true;

    /* Only gravity in the display plane picks an orientation; a device lying
     * flat (gravity on Z) or in motion keeps the settled orientation. */
    const int candidate = classify(gx, gy);
    if (candidate == ORIENTATION_UNKNOWN) {
        /* Flat or being moved: keep the last settled orientation. */
        s_candidate = ORIENTATION_UNKNOWN;
        s_candidate_count = 0;
    } else if (candidate == s_candidate) {
        if (s_candidate_count < ORIENTATION_STABLE_SAMPLES) {
            s_candidate_count++;
        }
    } else {
        s_candidate = candidate;
        s_candidate_count = 1;
    }

    *out_changed = false;
    if (s_candidate != ORIENTATION_UNKNOWN &&
        s_candidate_count >= ORIENTATION_STABLE_SAMPLES &&
        s_candidate != s_stable) {
        s_stable = s_candidate;
        *out_changed = true;
        ESP_LOGI(TAG, "orientation settled at %d degrees", s_stable);
    }

    *out_angle = (orientation_t)s_stable;
    return ESP_OK;
}
