#include <assert.h>
#include <stdio.h>

#include "../main/common/app_mode.h"
#include "../main/common/clock_logic.h"
#include "../main/drivers/led_ring_map.h"

static void test_brightness_schedule(void)
{
    assert(clock_brightness_percent(6, 0) == 100);
    assert(clock_brightness_percent(21, 0) == 100);
    assert(clock_brightness_percent(21, 1) == 40);
    assert(clock_brightness_percent(5, 59) == 40);
}

static void test_clock_hand_output_uses_the_twenty_percent_cap(void)
{
    assert(clock_hand_output_cap_percent() == 20U);
}

static void test_unix_time_conversion_keeps_milliseconds_continuous_across_seconds(void)
{
    clock_time_t now = {0};

    clock_time_from_unix_ms(86399999LL, 0, &now);
    assert(now.hour == 23 && now.minute == 59 && now.second == 59 && now.millisecond == 999);

    clock_time_from_unix_ms(86400000LL, 0, &now);
    assert(now.hour == 0 && now.minute == 0 && now.second == 0 && now.millisecond == 0);
}

static void test_minute_hand_crossfades_at_the_midpoint(void)
{
#if ECLOCK_EFFECT_PREVIEW
    const clock_time_t now = {.hour = 12, .minute = 0, .second = 8, .millisecond = 500};
#else
    const clock_time_t now = {.hour = 12, .minute = 2, .second = 29, .millisecond = 500};
#endif
    clock_hand_frame_t hands = {0};

    clock_compute_hands(&now, &hands);

    assert(hands.minute.current_index == 0);
    assert(hands.minute.next_index == 1);
    assert(hands.minute.current_level == 127);
    assert(hands.minute.next_level == 128);
}

static void test_minute_hand_uses_a_slow_ended_breathing_crossfade(void)
{
    clock_hand_frame_t hands = {0};
#if ECLOCK_EFFECT_PREVIEW
    const clock_time_t first_quarter = {
        .hour = 12, .minute = 0, .second = 7, .millisecond = 750,
    };
    const clock_time_t last_quarter = {
        .hour = 12, .minute = 0, .second = 9, .millisecond = 250,
    };
#else
    const clock_time_t first_quarter = {
        .hour = 12, .minute = 2, .second = 29, .millisecond = 250,
    };
    const clock_time_t last_quarter = {
        .hour = 12, .minute = 2, .second = 29, .millisecond = 750,
    };
#endif

    clock_compute_hands(&first_quarter, &hands);
    assert(hands.minute.current_index == 0);
    assert(hands.minute.next_index == 1);
    assert(hands.minute.current_level == 218);
    assert(hands.minute.next_level == 37);

    clock_compute_hands(&last_quarter, &hands);
    assert(hands.minute.current_index == 0);
    assert(hands.minute.next_index == 1);
    assert(hands.minute.current_level == 37);
    assert(hands.minute.next_level == 218);
}

static void test_hour_hand_changes_as_a_single_led(void)
{
    const clock_time_t now = {.hour = 12, .minute = 29, .second = 59, .millisecond = 500};
    clock_hand_frame_t hands = {0};

    clock_compute_hands(&now, &hands);

    assert(hands.hour.current_index == 0);
    assert(hands.hour.next_index == 1);
    assert(hands.hour.current_level == 255);
    assert(hands.hour.next_level == 0);

    const clock_time_t next = {.hour = 12, .minute = 30, .second = 0, .millisecond = 0};
    clock_compute_hands(&next, &hands);
#if ECLOCK_EFFECT_PREVIEW
    assert(hands.hour.current_index == 0);
#else
    assert(hands.hour.current_index == 1);
#endif
    assert(hands.hour.current_level == 255);
    assert(hands.hour.next_level == 0);
}

static void test_overlapping_hands_render_as_teal(void)
{
    const clock_time_t now = {.hour = 12, .minute = 0, .second = 0, .millisecond = 0};
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_render_frame(&now, frame);

    assert(frame[0].red == 0);
    assert(frame[0].green == 255);
    assert(frame[0].blue == 128);
}

static void test_event_queue_preserves_event_order(void)
{
    clock_event_queue_t queue = {0};
    clock_event_t event = CLOCK_EVENT_NONE;

    clock_event_queue_init(&queue);
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_WIFI_CONNECTED));
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_BLE_CONNECTED));
    assert(clock_event_queue_pop(&queue, &event));
    assert(event == CLOCK_EVENT_WIFI_CONNECTED);
    assert(clock_event_queue_pop(&queue, &event));
    assert(event == CLOCK_EVENT_BLE_CONNECTED);
    assert(!clock_event_queue_pop(&queue, &event));
}

static void test_ring_mapping_applies_offset_and_direction(void)
{
    assert(led_ring_map_logical_to_physical(0, 0, true) == 0);
    assert(led_ring_map_logical_to_physical(23, 0, true) == 23);
    assert(led_ring_map_logical_to_physical(0, 5, true) == 5);
    assert(led_ring_map_logical_to_physical(1, 5, true) == 6);
    assert(led_ring_map_logical_to_physical(1, 0, false) == 23);
}

static void test_demo_time_starts_at_noon_and_accumulates_elapsed_time(void)
{
    clock_time_t now = {0};

    clock_time_from_elapsed_ms(0, &now);
    assert(now.hour == 12 && now.minute == 0 && now.second == 0 && now.millisecond == 0);
    clock_time_from_elapsed_ms(3723004, &now);
    assert(now.hour == 13 && now.minute == 2 && now.second == 3 && now.millisecond == 4);
}

static void test_boot_comet_has_six_dimming_tail_leds_after_the_head(void)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_render_boot_comet(0, frame);

    assert(frame[0].red == 255 && frame[0].green == 255 && frame[0].blue == 255);
    assert(frame[23].red == 170 && frame[23].green == 170 && frame[23].blue == 170);
    assert(frame[22].red == 115 && frame[22].green == 115 && frame[22].blue == 115);
    assert(frame[21].red == 75 && frame[21].green == 75 && frame[21].blue == 75);
    assert(frame[20].red == 50 && frame[20].green == 50 && frame[20].blue == 50);
    assert(frame[19].red == 35 && frame[19].green == 35 && frame[19].blue == 35);
    assert(frame[18].red == 20 && frame[18].green == 20 && frame[18].blue == 20);
    assert(frame[17].red == 0 && frame[17].green == 0 && frame[17].blue == 0);
}

static void test_breathing_level_rises_and_falls_in_one_cycle(void)
{
    assert(clock_breathing_level(0) == 64);
    assert(clock_breathing_level(800) == 255);
    assert(clock_breathing_level(1600) == 64);
    assert(clock_breathing_level(2400) == 255);
}

static void test_fast_breathing_level_completes_cycle_twice_as_fast(void)
{
    assert(clock_fast_breathing_level(0) == 64);
    assert(clock_fast_breathing_level(400) == 255);
    assert(clock_fast_breathing_level(800) == 64);
}

static void test_boot_comet_makes_four_full_rotations_in_seven_seconds(void)
{
    assert(clock_boot_comet_head(0) == 0);
    assert(clock_boot_comet_head(1749) == 23);
    assert(clock_boot_comet_head(1750) == 0);
    assert(clock_boot_comet_head(6999) == 23);
}

static void test_wifi_connection_wait_is_limited_to_ten_seconds(void)
{
    assert(clock_wifi_connect_timeout_ms() == 10000U);
}

static void test_state_events_preserve_provisioning_sequence(void)
{
    clock_event_queue_t queue = {0};
    clock_event_t event = CLOCK_EVENT_NONE;

    clock_event_queue_init(&queue);
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_BLE_ADVERTISING));
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_BLE_CONNECTED));
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_WIFI_CONNECTING));
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_WIFI_CONNECTED));
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_NETWORK_CHECKING));
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_SNTP_SYNCING));
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_LOCATION_SYNCING));
    assert(clock_event_queue_push(&queue, CLOCK_EVENT_TIME_SYNCED));
    assert(clock_event_queue_pop(&queue, &event) && event == CLOCK_EVENT_BLE_ADVERTISING);
    assert(clock_event_queue_pop(&queue, &event) && event == CLOCK_EVENT_BLE_CONNECTED);
    assert(clock_event_queue_pop(&queue, &event) && event == CLOCK_EVENT_WIFI_CONNECTING);
    assert(clock_event_queue_pop(&queue, &event) && event == CLOCK_EVENT_WIFI_CONNECTED);
    assert(clock_event_queue_pop(&queue, &event) && event == CLOCK_EVENT_NETWORK_CHECKING);
    assert(clock_event_queue_pop(&queue, &event) && event == CLOCK_EVENT_SNTP_SYNCING);
    assert(clock_event_queue_pop(&queue, &event) && event == CLOCK_EVENT_LOCATION_SYNCING);
    assert(clock_event_queue_pop(&queue, &event) && event == CLOCK_EVENT_TIME_SYNCED);
}

static void test_only_ble_timeout_requests_shutdown(void)
{
    assert(clock_event_requires_shutdown(CLOCK_EVENT_BLE_TIMEOUT));
    assert(!clock_event_requires_shutdown(CLOCK_EVENT_SYNC_FAILED));
    assert(!clock_event_requires_shutdown(CLOCK_EVENT_WIFI_CONNECTED));
}

int main(void)
{
    test_brightness_schedule();
    test_clock_hand_output_uses_the_twenty_percent_cap();
    test_unix_time_conversion_keeps_milliseconds_continuous_across_seconds();
    test_minute_hand_crossfades_at_the_midpoint();
    test_minute_hand_uses_a_slow_ended_breathing_crossfade();
    test_hour_hand_changes_as_a_single_led();
    test_overlapping_hands_render_as_teal();
    test_event_queue_preserves_event_order();
    test_ring_mapping_applies_offset_and_direction();
    test_demo_time_starts_at_noon_and_accumulates_elapsed_time();
    test_boot_comet_has_six_dimming_tail_leds_after_the_head();
    test_breathing_level_rises_and_falls_in_one_cycle();
    test_fast_breathing_level_completes_cycle_twice_as_fast();
    test_boot_comet_makes_four_full_rotations_in_seven_seconds();
    test_wifi_connection_wait_is_limited_to_ten_seconds();
    test_state_events_preserve_provisioning_sequence();
    test_only_ble_timeout_requests_shutdown();
    puts("clock_logic_test: PASS");
    return 0;
}
