#include "clock_display.h"

#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "led_ring.h"
#include "ble_provision.h"
#include "time_service.h"

#define DISPLAY_FRAME_MS 20U
#define DISPLAY_BOOT_US ((int64_t)CLOCK_BOOT_ANIMATION_DURATION_MS * 1000LL)
#define DISPLAY_STATUS_US (1LL * 1000LL * 1000LL)
#define DISPLAY_FLASH_US (120LL * 1000LL)
#define DISPLAY_SUCCESS_US ((4LL * DISPLAY_FLASH_US) + DISPLAY_STATUS_US)

typedef enum {
    DISPLAY_PAGE_CLOCK,
    DISPLAY_PAGE_BLE_WAIT,
    DISPLAY_PAGE_WIFI_CREDENTIALS_WAIT,
    DISPLAY_PAGE_WIFI_WAIT,
    DISPLAY_PAGE_TIME_WAIT,
    DISPLAY_PAGE_BLE_SUCCESS,
    DISPLAY_PAGE_WIFI_SUCCESS,
    DISPLAY_PAGE_TIME_SUCCESS,
    DISPLAY_PAGE_FAILURE,
    DISPLAY_PAGE_BLE_TIMEOUT,
} display_page_t;

static QueueHandle_t s_events;
static TaskHandle_t s_task;

static clock_rgb_t color_for_page(display_page_t page)
{
    switch (page) {
    case DISPLAY_PAGE_BLE_WAIT:
    case DISPLAY_PAGE_BLE_SUCCESS:
        return (clock_rgb_t){.red = 0, .green = 0, .blue = 255};
    case DISPLAY_PAGE_WIFI_CREDENTIALS_WAIT:
    case DISPLAY_PAGE_WIFI_WAIT:
    case DISPLAY_PAGE_WIFI_SUCCESS:
        return (clock_rgb_t){.red = 0, .green = 255, .blue = 0};
    case DISPLAY_PAGE_TIME_WAIT:
    case DISPLAY_PAGE_TIME_SUCCESS:
        return (clock_rgb_t){.red = 0, .green = 255, .blue = 255};
    case DISPLAY_PAGE_FAILURE:
        return (clock_rgb_t){.red = 255, .green = 0, .blue = 0};
    case DISPLAY_PAGE_BLE_TIMEOUT:
        return (clock_rgb_t){.red = 160, .green = 0, .blue = 255};
    default:
        return (clock_rgb_t){0};
    }
}

static bool page_is_breathing(display_page_t page)
{
    return page == DISPLAY_PAGE_BLE_WAIT || page == DISPLAY_PAGE_WIFI_CREDENTIALS_WAIT ||
           page == DISPLAY_PAGE_WIFI_WAIT ||
           page == DISPLAY_PAGE_TIME_WAIT || page == DISPLAY_PAGE_FAILURE ||
           page == DISPLAY_PAGE_BLE_TIMEOUT;
}

static bool page_is_transient(display_page_t page)
{
    return page == DISPLAY_PAGE_BLE_SUCCESS || page == DISPLAY_PAGE_WIFI_SUCCESS ||
           page == DISPLAY_PAGE_TIME_SUCCESS || page == DISPLAY_PAGE_FAILURE ||
           page == DISPLAY_PAGE_BLE_TIMEOUT;
}

static int64_t page_duration_us(display_page_t page)
{
    if (page == DISPLAY_PAGE_BLE_SUCCESS || page == DISPLAY_PAGE_WIFI_SUCCESS) {
        return DISPLAY_SUCCESS_US;
    }
    if (page == DISPLAY_PAGE_TIME_SUCCESS || page == DISPLAY_PAGE_FAILURE ||
        page == DISPLAY_PAGE_BLE_TIMEOUT) {
        return DISPLAY_STATUS_US;
    }
    return 0;
}

static void render_status(display_page_t page, int64_t elapsed_us)
{
    clock_rgb_t color = color_for_page(page);
    if (page_is_breathing(page)) {
        const uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000LL);
        const uint8_t level = page == DISPLAY_PAGE_WIFI_CREDENTIALS_WAIT
                                  ? clock_fast_breathing_level(elapsed_ms)
                                  : clock_breathing_level(elapsed_ms);
        color.red = (uint8_t)(((uint16_t)color.red * level) / 255U);
        color.green = (uint8_t)(((uint16_t)color.green * level) / 255U);
        color.blue = (uint8_t)(((uint16_t)color.blue * level) / 255U);
    } else if (page == DISPLAY_PAGE_BLE_SUCCESS || page == DISPLAY_PAGE_WIFI_SUCCESS) {
        const bool flash_on = elapsed_us < DISPLAY_FLASH_US ||
                              (elapsed_us >= 2LL * DISPLAY_FLASH_US &&
                               elapsed_us < 3LL * DISPLAY_FLASH_US);
        const bool solid_on = elapsed_us >= 4LL * DISPLAY_FLASH_US;
        if (!flash_on && !solid_on) color = (clock_rgb_t){0};
    }
    (void)led_ring_show_solid(color, 100U);
}

static void render_boot(int64_t elapsed_us)
{
    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};
    const uint8_t head = clock_boot_comet_head((uint32_t)(elapsed_us / 1000LL));
    clock_render_boot_comet(head, frame);
    (void)led_ring_show(frame, 100U);
}

static void render_clock(void)
{
    clock_time_t now = {0};
    if (!time_service_get_local(&now)) {
        clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};
        frame[0] = (clock_rgb_t){.red = 255, .green = 255, .blue = 255};
        (void)led_ring_show(frame, 100U);
        return;
    }

    clock_rgb_t frame[CLOCK_RING_LED_COUNT] = {0};
    clock_render_frame(&now, frame);
    (void)led_ring_show_limited(frame, clock_brightness_percent(now.hour, now.minute),
                                clock_hand_output_cap_percent());
}

static void enter_deep_sleep(void)
{
    time_service_cancel_sync();
    ble_provision_finish();
    (void)led_ring_show_solid((clock_rgb_t){0}, 100U);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_deep_sleep_start();
}

static bool set_page_from_event(clock_event_t event, display_page_t *page)
{
    switch (event) {
    case CLOCK_EVENT_BLE_ADVERTISING:
    case CLOCK_EVENT_BLE_CONNECTING:
        *page = DISPLAY_PAGE_BLE_WAIT;
        return true;
    case CLOCK_EVENT_BLE_CONNECTED:
        *page = DISPLAY_PAGE_BLE_SUCCESS;
        return true;
    case CLOCK_EVENT_WIFI_CONNECTING:
        *page = DISPLAY_PAGE_WIFI_WAIT;
        return true;
    case CLOCK_EVENT_WIFI_CONNECTED:
        *page = DISPLAY_PAGE_WIFI_SUCCESS;
        return true;
    case CLOCK_EVENT_NETWORK_CHECKING:
    case CLOCK_EVENT_SNTP_SYNCING:
    case CLOCK_EVENT_LOCATION_SYNCING:
    case CLOCK_EVENT_TIME_SYNCING:
        *page = DISPLAY_PAGE_TIME_WAIT;
        return true;
    case CLOCK_EVENT_TIME_SYNCED:
        *page = DISPLAY_PAGE_TIME_SUCCESS;
        return true;
    case CLOCK_EVENT_SYNC_FAILED:
        *page = DISPLAY_PAGE_FAILURE;
        return true;
    case CLOCK_EVENT_BLE_TIMEOUT:
        *page = DISPLAY_PAGE_BLE_TIMEOUT;
        return true;
    default:
        return false;
    }
}

static display_page_t page_after_transient(display_page_t page)
{
    switch (page) {
    case DISPLAY_PAGE_BLE_SUCCESS:
        return DISPLAY_PAGE_WIFI_CREDENTIALS_WAIT;
    case DISPLAY_PAGE_FAILURE:
        if (time_service_has_valid_time()) return DISPLAY_PAGE_CLOCK;
        if (ble_provision_is_connected()) return DISPLAY_PAGE_WIFI_CREDENTIALS_WAIT;
        return DISPLAY_PAGE_BLE_WAIT;
    case DISPLAY_PAGE_WIFI_SUCCESS:
        return DISPLAY_PAGE_TIME_WAIT;
    default:
        return DISPLAY_PAGE_CLOCK;
    }
}

static void display_task(void *arg)
{
    (void)arg;
    const int64_t boot_started_us = esp_timer_get_time();
    int64_t page_started_us = 0;
    display_page_t page = DISPLAY_PAGE_CLOCK;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us - boot_started_us < DISPLAY_BOOT_US) {
            render_boot(now_us - boot_started_us);
        } else {
            if (page_is_transient(page) &&
                now_us - page_started_us >= page_duration_us(page)) {
                const clock_event_t completed_event =
                    page == DISPLAY_PAGE_BLE_TIMEOUT ? CLOCK_EVENT_BLE_TIMEOUT : CLOCK_EVENT_NONE;
                page = page_after_transient(page);
                page_started_us = now_us;
                if (clock_event_requires_shutdown(completed_event)) enter_deep_sleep();
            }

            if (!page_is_transient(page)) {
                clock_event_t event = CLOCK_EVENT_NONE;
                while (xQueueReceive(s_events, &event, 0) == pdTRUE) {
                    if (event == CLOCK_EVENT_DEMO_READY) {
                        time_service_start_demo();
                        continue;
                    }
                    display_page_t next_page = page;
                    if (set_page_from_event(event, &next_page)) {
                        page = next_page;
                        page_started_us = now_us;
                        if (page_is_transient(page)) break;
                    }
                }
            }

            if (page == DISPLAY_PAGE_CLOCK) {
                render_clock();
            } else {
                render_status(page, now_us - page_started_us);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_FRAME_MS));
    }
}

esp_err_t clock_display_init(void)
{
    s_events = xQueueCreate(CLOCK_EVENT_QUEUE_CAPACITY, sizeof(clock_event_t));
    if (s_events == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = led_ring_init();
    if (err != ESP_OK) return err;
    return xTaskCreate(display_task, "clock_display", 4096, NULL, 4, &s_task) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

void clock_display_post_event(clock_event_t event)
{
    if (s_events != NULL && event != CLOCK_EVENT_NONE) (void)xQueueSend(s_events, &event, 0);
}

void clock_display_request_refresh(void)
{
    if (s_task != NULL) xTaskNotifyGive(s_task);
}
