#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_mode.h"
#include "ble_provision.h"
#include "button.h"
#include "clock_display.h"
#include "asrpro.h"
#include "runtime_event_log.h"
#include "time_service.h"
#include "wifi_manager.h"

static void finish_ble_session(void)
{
    ble_provision_finish();
}

static void on_boot_button_long_press(void)
{
#if !ECLOCK_DEMO_MODE
    time_service_cancel_sync();
    (void)ble_provision_start();
#endif
}

static void boot_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CLOCK_BOOT_ANIMATION_DURATION_MS));
    if (!ble_provision_is_connected()) time_service_start_sync();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(runtime_event_log_init());
    (void)runtime_event_log_append(RUNTIME_EVENT_BOOT, (int32_t)esp_reset_reason(),
                                   (int32_t)esp_get_free_heap_size(), ECLOCK_DEMO_MODE);
    runtime_event_log_dump();

    app_config_t config = {0};
    ESP_ERROR_CHECK(app_config_init(&config));
    ESP_ERROR_CHECK(clock_display_init());
    if (asrpro_init() != ESP_OK) {
        // 音频模块不是时钟启动的硬依赖，未接模块时仍允许灯环正常工作。
        ESP_LOGW("main", "ASRPRO module unavailable");
    }
    ESP_ERROR_CHECK(time_service_init(&config, finish_ble_session));
#if !ECLOCK_DEMO_MODE
    ESP_ERROR_CHECK(ble_provision_init(&config));
#endif
    ESP_ERROR_CHECK(button_init(on_boot_button_long_press));

#if ECLOCK_DEMO_MODE
    clock_display_post_event(CLOCK_EVENT_BLE_CONNECTING);
    clock_display_post_event(CLOCK_EVENT_BLE_CONNECTED);
    clock_display_post_event(CLOCK_EVENT_WIFI_CONNECTING);
    clock_display_post_event(CLOCK_EVENT_WIFI_CONNECTED);
    clock_display_post_event(CLOCK_EVENT_DEMO_READY);
#else
    ESP_ERROR_CHECK(ble_provision_start());
    if (wifi_manager_has_credentials() &&
        xTaskCreate(boot_sync_task, "boot_sync", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
#endif
}
