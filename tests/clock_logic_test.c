#include <assert.h>
#include <stdio.h>

#include "../main/common/app_mode.h"
#include "../main/common/clock_logic.h"
#include "../main/drivers/led_ring_map.h"

static void test_brightness_schedule(void)
{
    assert(clock_brightness_percent(6, 0) == 74);
    assert(clock_brightness_percent(21, 0) == 74);
    assert(clock_brightness_percent(21, 1) == 15);
    assert(clock_brightness_percent(5, 59) == 15);
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

static void test_utc_offset_prefers_fresh_location_then_saved_value_then_utc_plus_eight(void)
{
    assert(clock_select_utc_offset(false, 0, false, 0) == 28800);
    assert(clock_select_utc_offset(true, 3600, false, 0) == 3600);
    assert(clock_select_utc_offset(true, 3600, true, -18000) == -18000);
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

static void test_hour_hand_jumps_two_leds_on_the_hour(void)
{
    const clock_time_t now = {.hour = 12, .minute = 59, .second = 59, .millisecond = 500};
    clock_hand_frame_t hands = {0};

    clock_compute_hands(&now, &hands);

    assert(hands.hour.current_index == 0);
    assert(hands.hour.next_index == 2);
    assert(hands.hour.current_level == 255);
    assert(hands.hour.next_level == 0);

    const clock_time_t half_hour = {.hour = 13, .minute = 30, .second = 0, .millisecond = 0};
    clock_compute_hands(&half_hour, &hands);
#if ECLOCK_EFFECT_PREVIEW
    assert(hands.hour.current_index == 0);
#else
    assert(hands.hour.current_index == 2);
#endif
    const clock_time_t next = {.hour = 13, .minute = 0, .second = 0, .millisecond = 0};
    clock_compute_hands(&next, &hands);
#if ECLOCK_EFFECT_PREVIEW
    assert(hands.hour.current_index == 0);
#else
    assert(hands.hour.current_index == 2);
#endif
    assert(hands.hour.current_level == 255);
    assert(hands.hour.next_level == 0);
}

static void test_hour_hand_comet_runs_one_circle_before_landing_on_the_new_position(void)
{
    const clock_time_t starts = {.hour = 13, .minute = 0, .second = 0, .millisecond = 0};
    clock_hand_frame_t hands = {0};
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_compute_hands(&starts, &hands);
    assert(hands.hour_comet_active);
    assert(hands.hour_comet_head_index == 0);
    clock_render_frame(&starts, frame);
    assert(frame[0].red == 0 && frame[0].green == 255 && frame[0].blue == 128);
    assert(frame[23].red == 170 && frame[23].green == 170 && frame[23].blue == 170);
    assert(frame[22].red == 115 && frame[22].green == 115 && frame[22].blue == 115);
    assert(frame[18].red == 60 && frame[18].green == 60 && frame[18].blue == 60);

    const clock_time_t finished = {.hour = 13, .minute = 0, .second = 1, .millisecond = 200};
    clock_compute_hands(&finished, &hands);
    assert(!hands.hour_comet_active);
    assert(hands.hour.current_index == 2);
}

static void test_overlapping_hands_render_as_teal(void)
{
    const clock_time_t now = {.hour = 12, .minute = 0, .second = 1, .millisecond = 200};
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
    assert(led_ring_map_logical_to_physical(0, 0, true) == 6);
    assert(led_ring_map_logical_to_physical(6, 0, true) == 12);
    assert(led_ring_map_logical_to_physical(0, 5, true) == 11);
    assert(led_ring_map_logical_to_physical(1, 5, true) == 12);
    assert(led_ring_map_logical_to_physical(1, 0, false) == 5);
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
    assert(frame[21].red == 85 && frame[21].green == 85 && frame[21].blue == 85);
    assert(frame[20].red == 75 && frame[20].green == 75 && frame[20].blue == 75);
    assert(frame[19].red == 65 && frame[19].green == 65 && frame[19].blue == 65);
    assert(frame[18].red == 60 && frame[18].green == 60 && frame[18].blue == 60);
    assert(frame[17].red == 0 && frame[17].green == 0 && frame[17].blue == 0);

    for (uint32_t elapsed_ms = 0; elapsed_ms < CLOCK_BOOT_ANIMATION_DURATION_MS;
         elapsed_ms += 10U) {
        const uint8_t moving_head = clock_boot_comet_head(elapsed_ms);
        clock_render_boot_comet(moving_head, frame);
        for (uint8_t index = 0; index < CLOCK_RING_LED_COUNT; ++index) {
            assert(frame[index].red == frame[index].green);
            assert(frame[index].green == frame[index].blue);
        }
    }
}

static void test_comet_tail_avoids_subthreshold_brightness(void)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_render_boot_comet(0, frame);

    /* The ring applies a 10%% electrical cap, so raw level 60 remains at 6/255. */
    for (uint8_t index = 18; index < CLOCK_RING_LED_COUNT; ++index) {
        assert(frame[index].red >= 60U);
        assert(frame[index].red == frame[index].green);
        assert(frame[index].green == frame[index].blue);
    }
}

static void test_boot_diagnostic_frame_is_solid_white(void)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_render_boot_diagnostic_white(frame);

    for (uint8_t index = 0; index < CLOCK_RING_LED_COUNT; ++index) {
        assert(frame[index].red == 255U);
        assert(frame[index].green == 255U);
        assert(frame[index].blue == 255U);
    }
}

static void test_boot_diagnostic_switch_changes_only_frame_values(void)
{
    clock_rgb_t first[CLOCK_RING_LED_COUNT] = {0};
    clock_rgb_t second[CLOCK_RING_LED_COUNT] = {0};

    clock_render_boot_diagnostic_switch(0U, first);
    clock_render_boot_diagnostic_switch(20U, second);

    assert(first[0].red == 255U && first[0].green == 255U && first[0].blue == 255U);
    assert(second[0].red == 160U && second[0].green == 160U && second[0].blue == 160U);
    assert(first[6].red == 0U && first[6].green == 0U && first[6].blue == 0U);
    assert(second[6].red == 0U && second[6].green == 0U && second[6].blue == 0U);
    assert(first[23].red == 0U && second[23].red == 0U);
    for (uint8_t index = 0; index < CLOCK_RING_LED_COUNT; ++index) {
        assert(first[index].red == first[index].green && first[index].green == first[index].blue);
        assert(second[index].red == second[index].green && second[index].green == second[index].blue);
    }
}

static void test_boot_diagnostic_channel_selects_one_rgb_component(void)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_render_boot_diagnostic_channel(0U, frame);
    assert(frame[0].red == 255U && frame[0].green == 0U && frame[0].blue == 0U);
    clock_render_boot_diagnostic_channel(1000U, frame);
    assert(frame[0].red == 0U && frame[0].green == 255U && frame[0].blue == 0U);
    clock_render_boot_diagnostic_channel(2000U, frame);
    assert(frame[0].red == 0U && frame[0].green == 0U && frame[0].blue == 255U);
    clock_render_boot_diagnostic_channel(20U, frame);
    assert(frame[0].red == 0U && frame[0].green == 0U && frame[0].blue == 0U);
}

static void test_boot_diagnostic_mixed_cycles_through_fixed_colors(void)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_render_boot_diagnostic_mixed(0U, frame);
    assert(frame[0].red == 255U && frame[0].green == 255U && frame[0].blue == 255U);
    clock_render_boot_diagnostic_mixed(1000U, frame);
    assert(frame[0].red == 160U && frame[0].green == 160U && frame[0].blue == 160U);
    clock_render_boot_diagnostic_mixed(2000U, frame);
    assert(frame[0].red == 255U && frame[0].green == 255U && frame[0].blue == 0U);
    clock_render_boot_diagnostic_mixed(3000U, frame);
    assert(frame[0].red == 0U && frame[0].green == 255U && frame[0].blue == 255U);
    clock_render_boot_diagnostic_mixed(4000U, frame);
    assert(frame[0].red == 255U && frame[0].green == 0U && frame[0].blue == 255U);
    for (uint8_t index = 1; index < CLOCK_RING_LED_COUNT; ++index)
        assert(frame[index].red == 0U && frame[index].green == 0U && frame[index].blue == 0U);
}

static void test_boot_diagnostic_transitions_inserts_off_gaps(void)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_render_boot_diagnostic_transitions(0U, frame);
    assert(frame[0].red == 0U && frame[0].green == 0U && frame[0].blue == 0U);
    clock_render_boot_diagnostic_transitions(1000U, frame);
    assert(frame[0].red == 255U && frame[0].green == 255U && frame[0].blue == 0U);
    clock_render_boot_diagnostic_transitions(2000U, frame);
    assert(frame[0].red == 0U && frame[0].green == 0U && frame[0].blue == 0U);
    clock_render_boot_diagnostic_transitions(3000U, frame);
    assert(frame[0].red == 255U && frame[0].green == 0U && frame[0].blue == 255U);
    clock_render_boot_diagnostic_transitions(5000U, frame);
    assert(frame[0].red == 255U && frame[0].green == 255U && frame[0].blue == 255U);
}

static void test_boot_diagnostic_positions_selects_one_data_position(void)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_render_boot_diagnostic_positions(0U, frame);
    assert(frame[0].red == 255U && frame[1].red == 0U);
    clock_render_boot_diagnostic_positions(1000U, frame);
    assert(frame[0].red == 0U && frame[1].red == 255U);
    clock_render_boot_diagnostic_positions(2000U, frame);
    assert(frame[1].red == 0U && frame[2].red == 255U);
    for (uint8_t index = 4; index < CLOCK_RING_LED_COUNT; ++index)
        assert(frame[index].red == 0U && frame[index].green == 0U && frame[index].blue == 0U);
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

static void test_status_indicator_transitions_and_latches_fault(void)
{
    clock_status_t status = CLOCK_STATUS_UNAVAILABLE;

    status = clock_status_transition(status, CLOCK_EVENT_SNTP_SYNCING);
    assert(status == CLOCK_STATUS_CALIBRATING);
    status = clock_status_transition(status, CLOCK_EVENT_SYNC_FAILED);
    assert(status == CLOCK_STATUS_FAULT);
    status = clock_status_transition(status, CLOCK_EVENT_WIFI_CONNECTED);
    assert(status == CLOCK_STATUS_FAULT);
    status = clock_status_transition(status, CLOCK_EVENT_TIME_SYNCED);
    assert(status == CLOCK_STATUS_WORKING);
    assert(clock_status_report_fault() == CLOCK_STATUS_FAULT);
}

static void test_status_indicator_uses_six_oclock_with_priority(void)
{
    clock_time_t now = {.hour = 12, .minute = 2, .second = 29, .millisecond = 500};
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};

    clock_render_frame(&now, frame);
    clock_apply_status_indicator(frame, CLOCK_STATUS_WORKING, 0);
    assert(frame[12].red == 0 && frame[12].green == clock_status_breathing_level(0) && frame[12].blue == 0);
    assert(frame[6].red == 0 && frame[6].green == 0 && frame[6].blue == 0);
    assert(frame[0].red != 0 || frame[0].green != 0 || frame[0].blue != 0);

    frame[12] = (clock_rgb_t){.red = 255, .green = 255, .blue = 255};
    clock_apply_status_indicator(frame, CLOCK_STATUS_CALIBRATING, 500);
    assert(frame[12].red == 255 && frame[12].green == 255 && frame[12].blue == 255);
    clock_apply_status_indicator(frame, CLOCK_STATUS_FAULT, 500);
    assert(frame[12].red == 255 && frame[12].green == 0 && frame[12].blue == 0);
}

static void test_status_breathing_reaches_zero_between_pulses(void)
{
    assert(clock_status_breathing_level(0) == 0);
    assert(clock_status_breathing_level(500) == 255);
    assert(clock_status_breathing_level(1300) == 0);
    assert(clock_status_breathing_level(2000) == 0);
    assert(clock_status_breathing_level(3000) == 0);
    assert(clock_status_breathing_level(3400) == 0);
}

static void test_display_refresh_uses_low_rate_only_when_static(void)
{
    const clock_time_t static_time = {.hour = 12, .minute = 10, .second = 0, .millisecond = 0};
    const clock_time_t minute_transition = {
        .hour = 12, .minute = 12, .second = 29, .millisecond = 500,
    };
    const clock_time_t hour_comet = {.hour = 13, .minute = 0, .second = 0, .millisecond = 0};

    assert(clock_display_refresh_interval_ms(&static_time, CLOCK_STATUS_UNAVAILABLE, 0) == 1000U);
    assert(clock_display_refresh_interval_ms(&static_time, CLOCK_STATUS_WORKING, 0) == 100U);
    assert(clock_display_refresh_interval_ms(&minute_transition, CLOCK_STATUS_WORKING, 0) == 10U);
    assert(clock_display_refresh_interval_ms(&hour_comet, CLOCK_STATUS_WORKING, 0) == 10U);
}

int main(void)
{
    test_brightness_schedule();
    test_clock_hand_output_uses_the_twenty_percent_cap();
    test_unix_time_conversion_keeps_milliseconds_continuous_across_seconds();
    test_utc_offset_prefers_fresh_location_then_saved_value_then_utc_plus_eight();
    test_minute_hand_crossfades_at_the_midpoint();
    test_minute_hand_uses_a_slow_ended_breathing_crossfade();
    test_hour_hand_jumps_two_leds_on_the_hour();
    test_hour_hand_comet_runs_one_circle_before_landing_on_the_new_position();
    test_overlapping_hands_render_as_teal();
    test_event_queue_preserves_event_order();
    test_ring_mapping_applies_offset_and_direction();
    test_demo_time_starts_at_noon_and_accumulates_elapsed_time();
    test_boot_comet_has_six_dimming_tail_leds_after_the_head();
    test_comet_tail_avoids_subthreshold_brightness();
    test_boot_diagnostic_frame_is_solid_white();
    test_boot_diagnostic_switch_changes_only_frame_values();
    test_boot_diagnostic_channel_selects_one_rgb_component();
    test_boot_diagnostic_mixed_cycles_through_fixed_colors();
    test_boot_diagnostic_transitions_inserts_off_gaps();
    test_boot_diagnostic_positions_selects_one_data_position();
    test_breathing_level_rises_and_falls_in_one_cycle();
    test_fast_breathing_level_completes_cycle_twice_as_fast();
    test_boot_comet_makes_four_full_rotations_in_seven_seconds();
    test_wifi_connection_wait_is_limited_to_ten_seconds();
    test_state_events_preserve_provisioning_sequence();
    test_only_ble_timeout_requests_shutdown();
    test_status_indicator_transitions_and_latches_fault();
    test_status_indicator_uses_six_oclock_with_priority();
    test_status_breathing_reaches_zero_between_pulses();
    test_display_refresh_uses_low_rate_only_when_static();
    puts("clock_logic_test: PASS");
    return 0;
}
