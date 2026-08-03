#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CLOCK_RING_LED_COUNT 24U
#define CLOCK_EVENT_QUEUE_CAPACITY 8U
#define CLOCK_BOOT_ANIMATION_DURATION_MS 7000U

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} clock_rgb_t;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t millisecond;
} clock_time_t;

typedef struct {
    uint8_t current_index;
    uint8_t next_index;
    uint8_t current_level;
    uint8_t next_level;
} clock_hand_sample_t;

typedef struct {
    clock_hand_sample_t hour;
    clock_hand_sample_t minute;
} clock_hand_frame_t;

typedef enum {
    CLOCK_EVENT_NONE = 0,
    CLOCK_EVENT_BLE_ADVERTISING,
    CLOCK_EVENT_BLE_CONNECTED,
    CLOCK_EVENT_WIFI_CONNECTING,
    CLOCK_EVENT_WIFI_CONNECTED,
    CLOCK_EVENT_NETWORK_CHECKING,
    CLOCK_EVENT_SNTP_SYNCING,
    CLOCK_EVENT_LOCATION_SYNCING,
    CLOCK_EVENT_TIME_SYNCING,
    CLOCK_EVENT_TIME_SYNCED,
    CLOCK_EVENT_BLE_TIMEOUT,
    CLOCK_EVENT_SYNC_FAILED,
    /* Kept for the disabled demo path and source compatibility. */
    CLOCK_EVENT_BLE_CONNECTING,
    CLOCK_EVENT_DEMO_READY,
} clock_event_t;

typedef struct {
    clock_event_t events[CLOCK_EVENT_QUEUE_CAPACITY];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} clock_event_queue_t;

uint8_t clock_brightness_percent(uint8_t hour, uint8_t minute);
uint8_t clock_hand_output_cap_percent(void);
uint8_t clock_breathing_level(uint32_t elapsed_ms);
uint8_t clock_fast_breathing_level(uint32_t elapsed_ms);
uint8_t clock_boot_comet_head(uint32_t elapsed_ms);
uint32_t clock_wifi_connect_timeout_ms(void);
void clock_time_from_elapsed_ms(uint64_t elapsed_ms, clock_time_t *out);
void clock_time_from_unix_ms(int64_t utc_epoch_ms, int32_t utc_offset_seconds,
                             clock_time_t *out);
void clock_render_boot_comet(uint8_t head_index, clock_rgb_t frame[CLOCK_RING_LED_COUNT]);
void clock_compute_hands(const clock_time_t *now, clock_hand_frame_t *out);
void clock_render_frame(const clock_time_t *now, clock_rgb_t frame[CLOCK_RING_LED_COUNT]);

void clock_event_queue_init(clock_event_queue_t *queue);
bool clock_event_queue_push(clock_event_queue_t *queue, clock_event_t event);
bool clock_event_queue_pop(clock_event_queue_t *queue, clock_event_t *out);
bool clock_event_requires_shutdown(clock_event_t event);
