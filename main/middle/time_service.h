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

/* 语音设置闹钟:ASRPRO 侧凑齐"日期 + 时段 + 几点几分"后发 T_ALARM_SET,
 * 由 link_probe 在收帧回调里转进来。这里只置标志,daily_task 负责落 NVS、
 * 找空位/反问替换哪一个、以及播确认句 —— 不在串口接收任务里做阻塞操作。
 *   day    0=今天 1=明天 2=每天
 *   period 0=早上 1=上午 2=下午 3=晚上 4=凌晨,0xFF 表示没说(按 hour 反推)
 *   hour   0~23,ASRPRO 已把时段折算进去 */
void time_service_on_alarm_set(uint8_t day, uint8_t period, uint8_t hour, uint8_t minute);

/* 两个槽位都满时,用户回"第一个/第二个"。which 为 1 或 2。 */
void time_service_on_alarm_pick(uint8_t which);

/* "取消闹钟":清空所有槽位并停铃。 */
void time_service_on_alarm_off(void);
