#include "button.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "runtime_event_log.h"

#define BUTTON_POLL_MS 50
#define BUTTON_LONG_PRESS_US (3LL * 1000LL * 1000LL)

static button_long_press_cb_t s_callback;

static void button_task(void *arg)
{
    (void)arg;
    int64_t pressed_since_us = 0;
    bool reported = false;

    for (;;) {
        const bool pressed = gpio_get_level(CLOCK_BOOT_BUTTON_GPIO) == 0;
        if (!pressed) {
            pressed_since_us = 0;
            reported = false;
        } else if (pressed_since_us == 0) {
            pressed_since_us = esp_timer_get_time();
        } else if (!reported && esp_timer_get_time() - pressed_since_us >= BUTTON_LONG_PRESS_US) {
            reported = true;
            (void)runtime_event_log_append(RUNTIME_EVENT_BUTTON_LONG_PRESS, 0, 0, 0);
            if (s_callback != NULL) s_callback();
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_init(button_long_press_cb_t on_long_press)
{
    s_callback = on_long_press;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CLOCK_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    return xTaskCreate(button_task, "clock_button", 2048, NULL, 4, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
