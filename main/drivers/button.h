#pragma once

#include "esp_err.h"

#define CLOCK_BOOT_BUTTON_GPIO 9

typedef void (*button_press_cb_t)(void);

/* 注册两个回调：
 *   on_short_press —— 短按（松开时且未达长按阈值）→ 静音切换
 *   on_long_press  —— 按住 3 秒 → 清空 NVS 并重启 */
esp_err_t button_init(button_press_cb_t on_short_press, button_press_cb_t on_long_press);
