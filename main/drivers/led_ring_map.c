#include "led_ring_map.h"

uint8_t led_ring_map_logical_to_physical(uint8_t logical_index,
                                         uint8_t first_pixel_offset,
                                         bool clockwise)
{
    const uint8_t logical = (uint8_t)(logical_index % LED_RING_PIXEL_COUNT);
    const uint8_t offset = (uint8_t)(first_pixel_offset % LED_RING_PIXEL_COUNT);
    if (clockwise) return (uint8_t)((offset + logical) % LED_RING_PIXEL_COUNT);

    return (uint8_t)((offset + LED_RING_PIXEL_COUNT - logical) % LED_RING_PIXEL_COUNT);
}
