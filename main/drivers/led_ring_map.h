#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LED_RING_PIXEL_COUNT 24U

uint8_t led_ring_map_logical_to_physical(uint8_t logical_index,
                                         uint8_t first_pixel_offset,
                                         bool clockwise);
