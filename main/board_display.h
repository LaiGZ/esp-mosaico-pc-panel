#pragma once

#include "esp_err.h"
#include "esp_gsp_esp_lcd.h"

struct esp_display_presenter;

esp_err_t board_display_init(esp_display_present_target_config_t *out_target);
esp_err_t board_touch_init(esp_lcd_touch_handle_t *out_touch);

/**
 * Create the display presenter ourselves so the application can reconfigure
 * rotation at runtime (the presenter must be quiesced by its owner).
 * Callers fall back to `board_display_init()`'s target when this fails.
 */
esp_err_t board_presenter_create(const esp_display_present_target_config_t *target,
                                 struct esp_display_presenter **out_presenter);
