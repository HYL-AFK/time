#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "clock_logic.h"

#define LED_RING_DATA_GPIO 3
#define LED_RING_FIRST_PIXEL_OFFSET 0U
#define LED_RING_CLOCKWISE true
#define LED_RING_SAFE_MAX_PERCENT 10U

esp_err_t led_ring_init(void);
esp_err_t led_ring_show(const clock_rgb_t frame[CLOCK_RING_LED_COUNT], uint8_t logical_brightness_percent);
esp_err_t led_ring_show_limited(const clock_rgb_t frame[CLOCK_RING_LED_COUNT],
                                uint8_t logical_brightness_percent,
                                uint8_t output_cap_percent);
esp_err_t led_ring_show_solid(clock_rgb_t color, uint8_t logical_brightness_percent);
esp_err_t led_ring_show_calibration_step(uint8_t logical_index, clock_rgb_t color);
esp_err_t led_ring_last_error(void);
uint32_t led_ring_error_count(void);
