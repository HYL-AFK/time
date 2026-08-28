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
#define CLOCK_HOUR_STEP_MS 3600000U
#define CLOCK_TRANSITION_MS 1000U
#endif
#define CLOCK_DEMO_START_MS (12ULL * 60ULL * 60ULL * 1000ULL)
#define CLOCK_DAY_MS (24ULL * 60ULL * 60ULL * 1000ULL)
#define CLOCK_BOOT_COMET_TAIL_LED_COUNT 6U
#define CLOCK_BOOT_COMET_ROTATIONS 4U
#define CLOCK_HOUR_COMET_DURATION_MS 1200U
#define CLOCK_HOUR_COMET_STEPS (CLOCK_RING_LED_COUNT + 1U)
#define CLOCK_BREATHING_MIN_LEVEL 64U
#define CLOCK_BREATHING_HALF_PERIOD_MS 800U
#define CLOCK_BREATHING_PERIOD_MS (2U * CLOCK_BREATHING_HALF_PERIOD_MS)
#define CLOCK_STATUS_RISE_MS 500U
#define CLOCK_STATUS_FALL_MS 800U
#define CLOCK_STATUS_PERIOD_MS 3400U
#define CLOCK_STATUS_BREATHING_MS (CLOCK_STATUS_RISE_MS + CLOCK_STATUS_FALL_MS)
#define CLOCK_WIFI_CONNECT_TIMEOUT_MS 10000U
#define CLOCK_DEFAULT_UTC_OFFSET_SECONDS (8 * 60 * 60)
#define CLOCK_HAND_BREATHING_LUT_STEPS 16U
#define CLOCK_HAND_OUTPUT_CAP_PERCENT 20U

/* sin^2(theta) samples from 0 to pi/2: soft at each end, quicker in the middle. */
static const uint8_t s_hand_breathing_lut[CLOCK_HAND_BREATHING_LUT_STEPS + 1U] = {
    0U, 2U, 10U, 21U, 37U, 57U, 79U, 103U, 128U,
    152U, 176U, 198U, 218U, 234U, 245U, 253U, 255U,
};

static const uint8_t s_comet_tail_levels[CLOCK_BOOT_COMET_TAIL_LED_COUNT] = {
    170U, 115U, 85U, 75U, 65U, 60U,
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

static clock_hand_sample_t make_hand_sample(uint32_t elapsed_ms, uint32_t step_ms,
                                            uint8_t index_step, bool crossfade)
{
    const uint32_t phase_ms = elapsed_ms % step_ms;
    const uint8_t current_index =
        (uint8_t)(((elapsed_ms / step_ms) * index_step) % CLOCK_RING_LED_COUNT);
    clock_hand_sample_t sample = {
        .current_index = current_index,
        .next_index = (uint8_t)((current_index + index_step) % CLOCK_RING_LED_COUNT),
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
    if (hour > 6U && hour < 21U) return 74U;
    if (hour == 6U || (hour == 21U && minute == 0U)) return 74U;
    return 15U;
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

uint8_t clock_status_breathing_level(uint32_t elapsed_ms)
{
    const uint32_t phase = elapsed_ms % CLOCK_STATUS_PERIOD_MS;
    if (phase >= CLOCK_STATUS_BREATHING_MS) return 0U;
    if (phase <= CLOCK_STATUS_RISE_MS) {
        return (uint8_t)((phase * CLOCK_FULL_LEVEL) / CLOCK_STATUS_RISE_MS);
    }
    return (uint8_t)(((CLOCK_STATUS_BREATHING_MS - phase) * CLOCK_FULL_LEVEL) /
                     CLOCK_STATUS_FALL_MS);
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

int32_t clock_select_utc_offset(bool has_saved_offset, int32_t saved_offset,
                                bool has_fresh_offset, int32_t fresh_offset)
{
    if (has_fresh_offset) return fresh_offset;
    if (has_saved_offset) return saved_offset;
    return CLOCK_DEFAULT_UTC_OFFSET_SECONDS;
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
    if (frame == NULL) return;

    memset(frame, 0, sizeof(clock_rgb_t) * CLOCK_RING_LED_COUNT);
    const uint8_t head = (uint8_t)(head_index % CLOCK_RING_LED_COUNT);
    frame[head] = (clock_rgb_t){.red = CLOCK_FULL_LEVEL, .green = CLOCK_FULL_LEVEL,
                                .blue = CLOCK_FULL_LEVEL};
    for (uint8_t tail = 0; tail < CLOCK_BOOT_COMET_TAIL_LED_COUNT; ++tail) {
        const uint8_t index = (uint8_t)(
            (head + CLOCK_RING_LED_COUNT - (tail + 1U)) % CLOCK_RING_LED_COUNT);
        const uint8_t level = s_comet_tail_levels[tail];
        frame[index] = (clock_rgb_t){.red = level, .green = level, .blue = level};
    }
}

void clock_render_boot_diagnostic_white(clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    if (frame == NULL) return;
    for (uint8_t index = 0; index < CLOCK_RING_LED_COUNT; ++index) {
        frame[index] = (clock_rgb_t){.red = CLOCK_FULL_LEVEL,
                                     .green = CLOCK_FULL_LEVEL,
                                     .blue = CLOCK_FULL_LEVEL};
    }
}

void clock_render_boot_diagnostic_switch(uint32_t elapsed_ms,
                                         clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    if (frame == NULL) return;

    /* One lit pixel against an all-off frame isolates the first local value boundary. */
    memset(frame, 0, sizeof(clock_rgb_t) * CLOCK_RING_LED_COUNT);
    const uint8_t level = ((elapsed_ms / 20U) & 1U) == 0U ? 255U : 160U;
    frame[0] = (clock_rgb_t){.red = level, .green = level, .blue = level};
}

void clock_render_boot_diagnostic_channel(uint32_t elapsed_ms,
                                          clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    if (frame == NULL) return;

    memset(frame, 0, sizeof(clock_rgb_t) * CLOCK_RING_LED_COUNT);
    const uint8_t level = ((elapsed_ms / 20U) & 1U) == 0U ? 255U : 0U;
    const uint8_t channel = (uint8_t)((elapsed_ms / 1000U) % 3U);
    if (channel == 0U) frame[0].red = level;
    else if (channel == 1U) frame[0].green = level;
    else frame[0].blue = level;
}

void clock_render_boot_diagnostic_mixed(uint32_t elapsed_ms,
                                        clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    if (frame == NULL) return;

    memset(frame, 0, sizeof(clock_rgb_t) * CLOCK_RING_LED_COUNT);
    switch ((elapsed_ms / 1000U) % 5U) {
    case 0U: frame[0] = (clock_rgb_t){255U, 255U, 255U}; break;
    case 1U: frame[0] = (clock_rgb_t){160U, 160U, 160U}; break;
    case 2U: frame[0] = (clock_rgb_t){255U, 255U, 0U}; break;
    case 3U: frame[0] = (clock_rgb_t){0U, 255U, 255U}; break;
    default: frame[0] = (clock_rgb_t){255U, 0U, 255U}; break;
    }
}

void clock_render_boot_diagnostic_transitions(uint32_t elapsed_ms,
                                              clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    if (frame == NULL) return;

    memset(frame, 0, sizeof(clock_rgb_t) * CLOCK_RING_LED_COUNT);
    switch ((elapsed_ms / 1000U) % 7U) {
    case 1U: frame[0] = (clock_rgb_t){255U, 255U, 0U}; break;
    case 3U: frame[0] = (clock_rgb_t){255U, 0U, 255U}; break;
    case 5U: frame[0] = (clock_rgb_t){255U, 255U, 255U}; break;
    default: break;
    }
}

void clock_render_boot_diagnostic_positions(uint32_t elapsed_ms,
                                            clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    if (frame == NULL) return;

    memset(frame, 0, sizeof(clock_rgb_t) * CLOCK_RING_LED_COUNT);
    const uint8_t position = (uint8_t)((elapsed_ms / 1000U) % 4U);
    frame[position] = (clock_rgb_t){255U, 255U, 255U};
}

static void add_scaled_pixel(clock_rgb_t *pixel, uint8_t level, uint16_t weight)
{
    const uint16_t scaled = (uint16_t)level * weight / 255U;
    if (scaled > pixel->red) pixel->red = (uint8_t)scaled;
    if (scaled > pixel->green) pixel->green = (uint8_t)scaled;
    if (scaled > pixel->blue) pixel->blue = (uint8_t)scaled;
}

void clock_render_boot_comet_progress(uint32_t elapsed_ms,
                                      clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    if (frame == NULL) return;
    memset(frame, 0, sizeof(clock_rgb_t) * CLOCK_RING_LED_COUNT);
    const uint64_t numerator = ((uint64_t)(elapsed_ms % CLOCK_BOOT_ANIMATION_DURATION_MS) *
                                CLOCK_RING_LED_COUNT * CLOCK_BOOT_COMET_ROTATIONS * 255ULL);
    const uint32_t position = (uint32_t)(numerator / CLOCK_BOOT_ANIMATION_DURATION_MS);
    const uint8_t head = (uint8_t)((position / 255U) % CLOCK_RING_LED_COUNT);
    const uint8_t fraction = (uint8_t)(position % 255U);
    for (uint8_t tail = 0; tail <= CLOCK_BOOT_COMET_TAIL_LED_COUNT; ++tail) {
        const uint8_t level = tail == 0U ? CLOCK_FULL_LEVEL : s_comet_tail_levels[tail - 1U];
        const uint8_t index = (uint8_t)((head + CLOCK_RING_LED_COUNT - tail) % CLOCK_RING_LED_COUNT);
        const uint8_t next = (uint8_t)((index + 1U) % CLOCK_RING_LED_COUNT);
        const uint16_t current_weight = tail == 0U ? 255U : (uint16_t)(255U - fraction);
        add_scaled_pixel(&frame[index], level, current_weight);
        /* The head remains solid; only the six trailing segments crossfade. */
        if (tail == 0U || next != head) add_scaled_pixel(&frame[next], level, fraction);
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

    out->minute = make_hand_sample(minute_elapsed_ms, CLOCK_MINUTE_STEP_MS, 1U, true);
    out->hour = make_hand_sample(hour_elapsed_ms, CLOCK_HOUR_STEP_MS, 2U, false);
    const uint32_t hour_phase_ms = hour_elapsed_ms % CLOCK_HOUR_STEP_MS;
    out->hour_comet_active = hour_phase_ms < CLOCK_HOUR_COMET_DURATION_MS;
    out->hour_comet_head_index = out->hour.current_index;
    out->hour_comet_fraction = 0U;
    if (out->hour_comet_active) {
        const uint8_t previous_index = (uint8_t)(
            (out->hour.current_index + CLOCK_RING_LED_COUNT - 2U) % CLOCK_RING_LED_COUNT);
        const uint32_t traveled_fixed =
            (uint32_t)(((uint64_t)hour_phase_ms * CLOCK_HOUR_COMET_STEPS * 255ULL) /
                        CLOCK_HOUR_COMET_DURATION_MS);
        const uint32_t traveled_steps = traveled_fixed / 255U;
        out->hour_comet_fraction = (uint8_t)(traveled_fixed % 255U);
        out->hour_comet_head_index =
            (uint8_t)((previous_index + traveled_steps) % CLOCK_RING_LED_COUNT);
    }
}

static void set_pixel(clock_rgb_t *pixel, uint8_t red, uint8_t green, uint8_t blue)
{
    pixel->red = red;
    pixel->green = green;
    pixel->blue = blue;
}

static void render_hour_comet(uint8_t head, uint8_t fraction,
                              uint8_t levels[CLOCK_RING_LED_COUNT])
{
    for (uint8_t tail = 0; tail <= CLOCK_BOOT_COMET_TAIL_LED_COUNT; ++tail) {
        const uint8_t index = (uint8_t)((head + CLOCK_RING_LED_COUNT - tail) % CLOCK_RING_LED_COUNT);
        const uint8_t next = (uint8_t)((index + 1U) % CLOCK_RING_LED_COUNT);
        const uint8_t level = tail == 0U ? CLOCK_FULL_LEVEL : s_comet_tail_levels[tail - 1U];
        const uint16_t current = tail == 0U
                                     ? level
                                     : (uint16_t)level * (uint16_t)(255U - fraction) / 255U;
        const uint16_t shifted = (uint16_t)level * fraction / 255U;
        if (current > levels[index]) levels[index] = (uint8_t)current;
        if (tail == 0U || next != head) {
            if (shifted > levels[next]) levels[next] = (uint8_t)shifted;
        }
    }
}

void clock_render_frame(const clock_time_t *now, clock_rgb_t frame[CLOCK_RING_LED_COUNT])
{
    if (now == NULL || frame == NULL) return;

    clock_hand_frame_t hands = {0};
    clock_compute_hands(now, &hands);
    uint8_t hour_levels[CLOCK_RING_LED_COUNT] = {0};
    uint8_t minute_levels[CLOCK_RING_LED_COUNT] = {0};
    if (hands.hour_comet_active) {
        render_hour_comet(hands.hour_comet_head_index, hands.hour_comet_fraction, hour_levels);
    } else {
        hour_levels[hands.hour.current_index] = hands.hour.current_level;
        hour_levels[hands.hour.next_index] = hands.hour.next_level;
    }
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

clock_status_t clock_status_transition(clock_status_t current, clock_event_t event)
{
    switch (event) {
    case CLOCK_EVENT_SNTP_SYNCING:
    case CLOCK_EVENT_LOCATION_SYNCING:
    case CLOCK_EVENT_TIME_SYNCING:
        return current == CLOCK_STATUS_FAULT ? current : CLOCK_STATUS_CALIBRATING;
    case CLOCK_EVENT_SYNC_FAILED:
        return CLOCK_STATUS_FAULT;
    case CLOCK_EVENT_TIME_SYNCED:
        return CLOCK_STATUS_WORKING;
    default:
        return current;
    }
}

clock_status_t clock_status_report_fault(void)
{
    return CLOCK_STATUS_FAULT;
}

void clock_apply_status_indicator(clock_rgb_t frame[CLOCK_RING_LED_COUNT], clock_status_t status,
                                  uint32_t elapsed_ms)
{
    if (frame == NULL || status == CLOCK_STATUS_UNAVAILABLE) return;

    const uint8_t level = clock_status_breathing_level(elapsed_ms);
    const clock_rgb_t existing = frame[12];
    const bool existing_white = existing.red != 0U && existing.red == existing.green &&
                                existing.green == existing.blue;
    switch (status) {
    case CLOCK_STATUS_WORKING:
        if (!existing_white) frame[12] = (clock_rgb_t){.red = 0, .green = level, .blue = 0};
        break;
    case CLOCK_STATUS_CALIBRATING:
        if (!existing_white) frame[12] = (clock_rgb_t){.red = 0, .green = level, .blue = level};
        break;
    case CLOCK_STATUS_FAULT:
        frame[12] = (clock_rgb_t){.red = level, .green = 0, .blue = 0};
        break;
    default:
        break;
    }
}

uint32_t clock_display_refresh_interval_ms(const clock_time_t *now, clock_status_t status,
                                           uint32_t status_elapsed_ms)
{
    if (now == NULL) return 1000U;

    clock_hand_frame_t hands = {0};
    clock_compute_hands(now, &hands);
    const uint32_t minute_elapsed_ms =
        (((uint32_t)now->minute * 60U + now->second) * 1000U) + now->millisecond;
    const bool minute_transition = (minute_elapsed_ms % 150000U) >= 149000U;
    if (hands.hour_comet_active || minute_transition) return 10U;
    if (status != CLOCK_STATUS_UNAVAILABLE) {
        (void)status_elapsed_ms;
        return 100U;
    }
    return 1000U;
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
