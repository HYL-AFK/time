/* 正计时 / 倒计时。
 *
 * 计时全部在 ESP32 侧用 esp_timer_get_time() 完成：
 * ASRPRO 的 millis() 会被播报阻塞，精度不可靠。
 * ASRPRO 只负责识别指令、播报结果。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIMER_MODE_IDLE = 0,
    TIMER_MODE_COUNTUP,   /* 正计时：累加已用时长 */
    TIMER_MODE_COUNTDOWN, /* 倒计时：到点播报“时间到了” */
} timer_mode_t;

/* 创建 100 ms 周期定时器，负责倒计时到点判断。 */
void timer_service_init(void);

void timer_service_start_countup(void);

/* 停止正计时并播报总时长（例如「三分二十五秒」）。
 * 当前不在正计时状态时什么都不做，只打日志。 */
void timer_service_stop_countup(void);

/* 取消正计时，不播报。 */
void timer_service_cancel(void);

/* 开始倒计时，seconds 为总秒数；已有的计时会被覆盖。 */
void timer_service_start_countdown(uint16_t seconds);

/* 取消倒计时，不播报。 */
void timer_service_cancel_countdown(void);

timer_mode_t timer_service_mode(void);

#ifdef __cplusplus
}
#endif
