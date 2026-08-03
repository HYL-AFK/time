#include "clock_logic.h"

#include <string.h>

#include "app_mode.h"

#define CLOCK_FULL_LEVEL 255U
#if ECLOCK_EFFECT_PREVIEW
#define CLOCK_MINUTE_STEP_MS 10000U
#define CLOCK_HOUR_STEP_MS 20000U
#define CLOCK_TRANSITION_MS 3000U
#else
#define CLOCK_MINUTE_STEP_MS 150000U
#define CLOCK_HOUR_STEP_MS 1800000U
#define CLOCK_TRANSITION_MS 1000U
#endif
#define CLOCK_DEMO_START_MS (12ULL * 60ULL * 60ULL * 1000ULL)
#define CLOCK_DAY_MS (24ULL * 60ULL * 60ULL * 1000ULL)
#define CLOCK_BOOT_COMET_TAIL_LED_COUNT 6U
#define CLOCK_BOOT_COMET_ROTATIONS 4U
#define CLOCK_BREATHING_MIN_LEVEL 64U
#define CLOCK_BREATHING_HALF_PERIOD_MS 800U
#define CLOCK_BREATHING_PERIOD_MS (2U * CLOCK_BREATHING_HALF_PERIOD_MS)
#define CLOCK_WIFI_CONNECT_TIMEOUT_MS 10000U
#define CLOCK_HAND_BREATHING_LUT_STEPS 16U
#define CLOCK_HAND_OUTPUT_CAP_PERCENT 20U

/* sin^2(theta) samples from 0 to pi/2: soft at each end, quicker in the middle. */
static const uint8_t s_hand_breathing_lut[CLOCK_HAND_BREATHING_LUT_STEPS + 1U] = {
    0U, 2U, 10U, 21U, 37U, 57U, 79U, 103U, 128U,
    152U, 176U, 198U, 218U, 234U, 245U, 253U, 255U,
};

static uint8_t hand_breathing_level(uint32_t progress_ms)
{
    if (progress_ms >= CLOCK_TRANSITION_MS) return CLOCK_FULL_LEVEL;

    const uint32_t scaled = progress_ms * CLOCK_HAND_BREATHING_LUT_STEPS;
    const uint32_t index = scaled / CLOCK_TRANSITION_MS;
    const uint32_t remainder = scaled % CLOCK_TRANSITION_MS;
    const uint8_t lower = s_hand_breathing_lut[index];
    const uint8_t upper = s_hand_breathing_lut[index + 1U];
    return (uint8_t)(lower + ((uint32_t)(upper - lower) * remainder) /
                                 CLOCK_TRANSITION_MS);
}

static clock_hand_sample_t make_hand_sample(uint32_t elapsed_ms, uint32_t step_ms, bool crossfade)
{
    const uint32_t phase_ms = elapsed_ms % step_ms;
    const uint8_t current_index = (uint8_t)((elapsed_ms / step_ms) % CLOCK_RING_LED_COUNT);
    clock_hand_sample_t sample = {
        .current_index = current_index,
        .next_index = (uint8_t)((current_index + 1U) % CLOCK_RING_LED_COUNT),
        .current_level = CLOCK_FULL_LEVEL,
        .next_level = 0,
    };

    if (crossfade && phase_ms >= step_ms - CLOCK_TRANSITION_MS) {
        const uint32_t progress_ms = phase_ms - (step_ms - CLOCK_TRANSITION_MS);
        sample.next_level = hand_breathing_level(progress_ms);
        sample.current_level = (uint8_t)(CLOCK_FULL_LEVEL - sample.next_level);
    }

    return sample;
}

uint8_t clock_brightness_percent(uint8_t hour, uint8_t minute)
{
    if (hour > 6U && hour < 21U) return 100U;
    if (hour == 6U || (hour == 21U && minute == 0U)) return 100U;
    return 40U;
}

uint8_t clock_hand_output_cap_percent(void)
{
    return CLOCK_HAND_OUTPUT_CAP_PERCENT;
}

uint8_t clock_breathing_level(uint32_t elapsed_ms)
{
    const uint32_t phase = elapsed_ms % CLOCK_BREATHING_PERIOD_MS;
    const uint32_t ramp_ms = phase <= CLOCK_BREATHING_HALF_PERIOD_MS
                                 ? phase
                                 : CLOCK_BREATHING_PERIOD_MS - phase;
    return (uint8_t)(CLOCK_BREATHING_MIN_LEVEL +
                     (ramp_ms * (CLOCK_FULL_LEVEL - CLOCK_BREATHING_MIN_LEVEL)) /
                         CLOCK_BREATHING_HALF_PERIOD_MS);
}

uint8_t clock_fast_breathing_level(uint32_t elapsed_ms)
{
    return clock_breathing_level(elapsed_ms * 2U);
}

uint8_t clock_boot_comet_head(uint32_t elapsed_ms)
{
    const uint64_t steps = ((uint64_t)(elapsed_ms % CLOCK_BOOT_ANIMATION_DURATION_MS) *
                            CLOCK_RING_LED_COUNT * CLOCK_BOOT_COMET_ROTATIONS) /
                           CLOCK_BOOT_ANIMATION_DURATION_MS;
    return (uint8_t)(steps % CLOCK_RING_LED_COUNT);
}

uint32_t clock_wifi_connect_timeout_ms(void)
{
    return CLOCK_WIFI_CONNECT_TIMEOUT_MS;
}

void clock_time_from_elapsed_ms(uint64_t elapsed_ms, clock_time_t *out)
{
    if (out == NULL) return;

    const uint64_t local_ms = (CLOCK_DEMO_START_MS + (elapsed_ms % CLOCK_DAY_MS)) % CLOCK_DAY_MS;
    out->hour = (uint8_t)(local_ms / (60ULL * 60ULL * 1000ULL));
    out->minute = (uint8_t)((local_ms / (60ULL * 1000ULL)) % 60ULL);
    out->second = (uint8_t)((local_ms / 1000ULL) % 60ULL);
    out->millisecond = (uint16_t)(local_ms % 1000ULL);
}

void clock_time_from_unix_ms(int64_t utc_epoch_ms, int32_t utc_offset_seconds,
                             clock_time_t *out)
{
    if (out == NULL) return;

    int64_t local_ms = utc_epoch_ms + (int64_t)utc_offset_seconds * 1000LL;
    local_ms %= (int64_t)CLOCK_DAY_MS;
    if (local_ms < 0) local_ms += (int64_t)CLOCK_DAY_MS;

    out->hour = (uint8_t)(local_ms / (60LL * 60LL * 1000LL));
    out->minute = (uint8_t)((local_ms / (60LL * 1000LL)) % 60LL);
    out->second = (uint8_t)((local_ms / 1000LL) % 60LL);
    out->millisecond = (uint16_t)(local_ms % 1000LL);
}

void clock_render_boot_comet(uint8_t head_index, clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    static const uint8_t tail_levels[CLOCK_BOOT_COMET_TAIL_LED_COUNT] = {
        170U, 115U, 75U, 50U, 35U, 20U,
    };
    if (frame == NULL) return;

    memset(frame, 0, sizeof(clock_rgb_t) * CLOCK_RING_LED_COUNT);
    const uint8_t head = (uint8_t)(head_index % CLOCK_RING_LED_COUNT);
    frame[head] = (clock_rgb_t){.red = CLOCK_FULL_LEVEL, .green = CLOCK_FULL_LEVEL,
                                .blue = CLOCK_FULL_LEVEL};
    for (uint8_t tail = 0; tail < CLOCK_BOOT_COMET_TAIL_LED_COUNT; ++tail) {
        const uint8_t index = (uint8_t)(
            (head + CLOCK_RING_LED_COUNT - (tail + 1U)) % CLOCK_RING_LED_COUNT);
        const uint8_t level = tail_levels[tail];
        frame[index] = (clock_rgb_t){.red = level, .green = level, .blue = level};
    }
}

void clock_compute_hands(const clock_time_t *now, clock_hand_frame_t *out)
{
    if (now == NULL || out == NULL) return;

    const uint32_t milliseconds = now->millisecond > 999U ? 999U : now->millisecond;
    const uint32_t minute_elapsed_ms =
        ((uint32_t)now->minute * 60U + now->second) * 1000U + milliseconds;
#if ECLOCK_EFFECT_PREVIEW
    const uint32_t hour_elapsed_ms = 0U;
#else
    const uint32_t hour_elapsed_ms =
        (((uint32_t)(now->hour % 12U) * 60U + now->minute) * 60U + now->second) * 1000U +
        milliseconds;
#endif

    out->minute = make_hand_sample(minute_elapsed_ms, CLOCK_MINUTE_STEP_MS, true);
    out->hour = make_hand_sample(hour_elapsed_ms, CLOCK_HOUR_STEP_MS, false);
}

static void set_pixel(clock_rgb_t *pixel, uint8_t red, uint8_t green, uint8_t blue)
{
    pixel->red = red;
    pixel->green = green;
    pixel->blue = blue;
}

void clock_render_frame(const clock_time_t *now, clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    if (now == NULL || frame == NULL) return;

    clock_hand_frame_t hands = {0};
    clock_compute_hands(now, &hands);
    uint8_t hour_levels[CLOCK_RING_LED_COUNT] = {0};
    uint8_t minute_levels[CLOCK_RING_LED_COUNT] = {0};
    hour_levels[hands.hour.current_index] = hands.hour.current_level;
    hour_levels[hands.hour.next_index] = hands.hour.next_level;
    minute_levels[hands.minute.current_index] = hands.minute.current_level;
    minute_levels[hands.minute.next_index] = hands.minute.next_level;

    for (uint8_t index = 0; index < CLOCK_RING_LED_COUNT; ++index) {
        const uint8_t hour = hour_levels[index];
        const uint8_t minute = minute_levels[index];
        if (hour != 0U && minute != 0U) {
            const uint8_t level = hour > minute ? hour : minute;
            set_pixel(&frame[index], 0U, level, (uint8_t)((level + 1U) / 2U));
        } else if (hour != 0U) {
            set_pixel(&frame[index], hour, hour, hour);
        } else if (minute != 0U) {
            set_pixel(&frame[index], 0, minute, 0);
        } else {
            set_pixel(&frame[index], 0, 0, 0);
        }
    }
}

void clock_event_queue_init(clock_event_queue_t *queue)
{
    if (queue != NULL) memset(queue, 0, sizeof(*queue));
}

bool clock_event_queue_push(clock_event_queue_t *queue, clock_event_t event)
{
    if (queue == NULL || event == CLOCK_EVENT_NONE ||
        queue->count == CLOCK_EVENT_QUEUE_CAPACITY) {
        return false;
    }

    queue->events[queue->tail] = event;
    queue->tail = (uint8_t)((queue->tail + 1U) % CLOCK_EVENT_QUEUE_CAPACITY);
    queue->count++;
    return true;
}

bool clock_event_queue_pop(clock_event_queue_t *queue, clock_event_t *out)
{
    if (queue == NULL || out == NULL || queue->count == 0U) return false;

    *out = queue->events[queue->head];
    queue->head = (uint8_t)((queue->head + 1U) % CLOCK_EVENT_QUEUE_CAPACITY);
    queue->count--;
    return true;
}

bool clock_event_requires_shutdown(clock_event_t event)
{
    return event == CLOCK_EVENT_BLE_TIMEOUT;
}
