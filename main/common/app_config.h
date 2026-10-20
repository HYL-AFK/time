#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_WIFI_SSID_MAX_LEN 32
#define APP_WIFI_PASS_MAX_LEN 64
#define APP_WIFI_PROFILE_MAX 3

/* 闹钟槽位数量:语音侧最多能存两个,第三个会反问"要替换哪一个"。 */
#define APP_ALARM_SLOT_MAX 2

typedef struct {
    char ssid[APP_WIFI_SSID_MAX_LEN + 1];
    char password[APP_WIFI_PASS_MAX_LEN + 1];
    uint32_t last_used;
} app_wifi_profile_t;

/* 一个闹钟槽位。一条语音设置 = 一个槽位。
 * repeat_daily=true 表示"每天"型(重复闹钟):target_epoch 响完后往后滚 86400,
 * 不设过期;false 表示"今天"/"明天"(一次性):响完即清,超过容忍窗就丢弃。 */
typedef struct {
    bool enabled;
    bool repeat_daily;
    uint8_t hour;           /* 0~23,时段已折算进小时 */
    uint8_t minute;         /* 0,5,10,…,55 —— 语音侧是 5 分钟挡位 */
    uint8_t period;         /* 0=早上 1=上午 2=下午 3=晚上 4=凌晨;拼确认句要按原话读回 */
    int64_t target_epoch;   /* 下一次该响的 UTC 时刻 */
} app_alarm_t;

typedef struct {
    char wifi_ssid[APP_WIFI_SSID_MAX_LEN + 1];
    char wifi_pass[APP_WIFI_PASS_MAX_LEN + 1];
    bool has_wifi;
    int32_t utc_offset_seconds;
    bool has_utc_offset;
    app_wifi_profile_t wifi_profiles[APP_WIFI_PROFILE_MAX];
    uint8_t wifi_profile_count;
    uint32_t wifi_use_sequence;
    app_alarm_t alarms[APP_ALARM_SLOT_MAX];
} app_config_t;

esp_err_t app_config_init(app_config_t *out);
esp_err_t app_config_save_wifi(app_config_t *config, const char *ssid, const char *password);
void app_config_mark_wifi_used(app_config_t *config, const char *ssid);
esp_err_t app_config_save_utc_offset(app_config_t *config, int32_t seconds);
/* 保存第 slot 个闹钟(0~APP_ALARM_SLOT_MAX-1);enabled=false 时清空该槽位,
 * 其余参数忽略。target_epoch 为下一次该响的 UTC 时刻,必须是正数。 */
esp_err_t app_config_save_alarm(app_config_t *config, uint8_t slot, bool enabled, bool repeat_daily,
                                uint8_t hour, uint8_t minute, uint8_t period, int64_t target_epoch);
/* 清空全部闹钟槽位("取消闹钟"用)。 */
esp_err_t app_config_clear_alarms(app_config_t *config);
/* 返回第一个空闲槽位下标;两个都占用时返回 APP_ALARM_SLOT_MAX。 */
uint8_t app_config_free_alarm_slot(const app_config_t *config);
