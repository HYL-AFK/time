#include "led_ring.h"

#include <string.h>

#include "led_strip.h"
#include "led_strip_rmt.h"

#include "led_ring_map.h"
#include "runtime_event_log.h"

static led_strip_handle_t s_strip;
static esp_err_t s_last_error = ESP_OK;
static uint32_t s_error_count;
static bool s_error_active;

static esp_err_t record_error(esp_err_t err, bool clear_strip)
{
    if (err == ESP_OK) {
        if (s_error_active) {
            (void)runtime_event_log_append(RUNTIME_EVENT_LED_RECOVERED, (int32_t)s_last_error,
                                           (int32_t)s_error_count, 0);
            s_error_active = false;
        }
        return ESP_OK;
    }

    s_last_error = err;
    s_error_count++;
    if (!s_error_active) {
        (void)runtime_event_log_append(RUNTIME_EVENT_LED_FAILURE, (int32_t)err,
                                       (int32_t)s_error_count, 0);
        s_error_active = true;
    }
    if (clear_strip && s_strip != NULL) (void)led_strip_clear(s_strip);
    return err;
}

static uint8_t scale_channel(uint8_t value, uint8_t logical_brightness_percent,
                             uint8_t output_cap_percent)
{
    if (logical_brightness_percent > 100U) logical_brightness_percent = 100U;
    if (output_cap_percent > 100U) output_cap_percent = 100U;
    const uint16_t scale = (uint16_t)logical_brightness_percent * output_cap_percent;
    return (uint8_t)(((uint32_t)value * scale) / 10000U);
}

esp_err_t led_ring_init(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LED_RING_DATA_GPIO,
        .max_leds = CLOCK_RING_LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10U * 1000U * 1000U,
        // A 24-pixel ring fits in RMT memory; ESP32-C3 does not need DMA here.
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) return record_error(err, false);
    return record_error(led_strip_clear(s_strip), false);
}

esp_err_t led_ring_show(const clock_rgb_t frame[CLOCK_RING_LED_COUNT], uint8_t logical_brightness_percent)
{
    return led_ring_show_limited(frame, logical_brightness_percent,
                                 LED_RING_SAFE_MAX_PERCENT);
}

esp_err_t led_ring_show_limited(const clock_rgb_t frame[CLOCK_RING_LED_COUNT],
                                uint8_t logical_brightness_percent,
                                uint8_t output_cap_percent)
{
    if (s_strip == NULL || frame == NULL) return ESP_ERR_INVALID_STATE;

    for (uint8_t logical = 0; logical < CLOCK_RING_LED_COUNT; ++logical) {
        const uint8_t physical = led_ring_map_logical_to_physical(
            logical, LED_RING_FIRST_PIXEL_OFFSET, LED_RING_CLOCKWISE);
        const clock_rgb_t color = frame[logical];
        esp_err_t err = led_strip_set_pixel(s_strip, physical,
                                             scale_channel(color.red, logical_brightness_percent,
                                                           output_cap_percent),
                                             scale_channel(color.green, logical_brightness_percent,
                                                           output_cap_percent),
                                             scale_channel(color.blue, logical_brightness_percent,
                                                           output_cap_percent));
        if (err != ESP_OK) return record_error(err, true);
    }
    return record_error(led_strip_refresh(s_strip), true);
}

esp_err_t led_ring_show_solid(clock_rgb_t color, uint8_t logical_brightness_percent)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT];
    for (uint8_t index = 0; index < CLOCK_RING_LED_COUNT; ++index) frame[index] = color;
    return led_ring_show(frame, logical_brightness_percent);
}

esp_err_t led_ring_show_calibration_step(uint8_t logical_index, clock_rgb_t color)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};
    frame[logical_index % CLOCK_RING_LED_COUNT] = color;
    return led_ring_show(frame, 100U);
}

esp_err_t led_ring_last_error(void)
{
    return s_last_error;
}

uint32_t led_ring_error_count(void)
{
    return s_error_count;
}
