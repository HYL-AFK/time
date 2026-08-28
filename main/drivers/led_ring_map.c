#include "led_ring_map.h"

uint8_t led_ring_map_logical_to_physical(uint8_t logical_index,
                                         uint8_t first_pixel_offset,
                                         bool clockwise)
{
    const uint8_t logical = (uint8_t)(logical_index % LED_RING_PIXEL_COUNT);
    const uint8_t offset = (uint8_t)(first_pixel_offset % LED_RING_PIXEL_COUNT);
    const uint8_t base = clockwise
                             ? (uint8_t)((offset + logical) % LED_RING_PIXEL_COUNT)
                             : (uint8_t)((offset + LED_RING_PIXEL_COUNT - logical) %
                                         LED_RING_PIXEL_COUNT);
    /* 本灯环物理编号方向与几何方向相反，逆时针 90 度对应加 6 个像素。 */
    const uint8_t rotation = (uint8_t)(LED_RING_PIXEL_COUNT / 4U);

    return (uint8_t)((base + rotation) % LED_RING_PIXEL_COUNT);
}
