#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_config.h"
#include "clock_logic.h"

typedef void (*time_service_sync_finished_cb_t)(void);

esp_err_t time_service_init(const app_config_t *config, time_service_sync_finished_cb_t on_finished);
void time_service_start_sync(void);
void time_service_cancel_sync(void);
void time_service_start_demo(void);
bool time_service_get_local(clock_time_t *out);
bool time_service_has_valid_time(void);

/* 播报当前天气：内部用已有的片段拼装（城市 + 天气现象 + 转 + 最高/最低 + 度），
 * 通过 asrpro_say() -> link_probe 的 T_SAY 下发。收到 ASK_WEATHER 时调用。 */
void time_service_speak_weather(void);

/* 语音问时间/天气：只置标志，由 daily_task 真正播报。
 * 不能在串口接收任务里直接调 asrpro_say()——它会等 PONG，
 * 而 PONG 也要靠同一个接收任务读，会自锁 500 ms 并丢帧。 */
void time_service_request_time(void);
void time_service_request_weather(void);

/* 语音设置闹钟:ASRPRO 侧凑齐"日期 + 时段 + 几点几分"后发 T_ALARM_SET,
 * 由 link_probe 在收帧回调里转进来。这里只置标志,daily_task 负责落 NVS、
 * 找空位/反问替换哪一个、以及播确认句 —— 不在串口接收任务里做阻塞操作。
 *   day    0=今天 1=明天 2=每天
 *   period 0=早上 1=上午 2=下午 3=晚上 4=凌晨,0xFF 表示没说(按 hour 反推)
 *   hour   0~23,ASRPRO 已把时段折算进去 */
void time_service_on_alarm_set(uint8_t day, uint8_t period, uint8_t hour, uint8_t minute);

/* "取消闹钟":清空所有槽位并停铃。 */
void time_service_on_alarm_off(void);

/* 播报一段时长，例如「三分二十五秒」。
 * minutes / seconds 由调用方拆好；seconds 可以是 0~59。
 * 两项都为 0 时播「零秒」。 */
void time_service_speak_duration(int minutes, int seconds);
