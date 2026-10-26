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
#include "link_probe.h"

/* 阶段 1：只跑链路握手，不启动业务通信。
 * 两边固件都验证通过后改回 0 即可恢复原有功能。 */
#define LINK_TEST_MODE 1
#include "runtime_event_log.h"
#include "time_service.h"
#include "timer_service.h"
#include "wifi_manager.h"

static void finish_ble_session(void)
{
    ble_provision_finish();
}

/* BOOT 短按切换静音：0 <-> 30。30 是 ASRPRO 侧的默认音量（g_volume = 30）。
 * 音量加减走语音命令，不在按键上。 */
#define BOOT_VOLUME_NORMAL 30U

static bool s_muted;

static void on_boot_button_short_press(void)
{
    s_muted = !s_muted;
    const uint8_t volume = s_muted ? 0U : (uint8_t)BOOT_VOLUME_NORMAL;
    ESP_LOGI("main", "BOOT short press: %s (volume %u)", s_muted ? "mute" : "unmute",
             (unsigned)volume);
    (void)link_probe_set_volume(volume);
}

static void on_boot_button_long_press(void)
{
    /* 长按 3 秒：清空 NVS（WiFi 凭据 / 闹钟 / 坐标缓存全部丢失）后重启，
     * 回到出厂配网状态。原来这里是“取消同步 + 进配网”，现在改成抹数据。 */
    ESP_LOGW("main", "BOOT long press: erasing NVS and restarting");
    (void)nvs_flash_erase();
    esp_restart();
}

/* 模组心跳状态 -> 灯环。OFFLINE 亮红色呼吸（一直闪到恢复），
 * 其余状态（LINK/READY）立刻撤掉。 */
static void on_link_state(link_state_t state)
{
    clock_display_post_event(state == LINK_STATE_OFFLINE ? CLOCK_EVENT_LINK_OFFLINE
                                                         : CLOCK_EVENT_LINK_RESTORED);
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
#if LINK_TEST_MODE
    if (link_probe_init() != ESP_OK) {
        ESP_LOGW("main", "link probe unavailable");
    }
    link_probe_set_state_callback(on_link_state);
#else
    if (asrpro_init() != ESP_OK) {
        // 音频模块不是时钟启动的硬依赖，未接模块时仍允许灯环正常工作。
        ESP_LOGW("main", "ASRPRO module unavailable");
    }
#endif
    ESP_ERROR_CHECK(time_service_init(&config, finish_ble_session));
    timer_service_init();
#if !ECLOCK_DEMO_MODE
    ESP_ERROR_CHECK(ble_provision_init(&config));
#endif
    ESP_ERROR_CHECK(button_init(on_boot_button_short_press, on_boot_button_long_press));

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
