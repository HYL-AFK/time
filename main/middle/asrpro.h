#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 与 ASRPRO(鹿小班 V5.0)通过 UART 通信。 */
esp_err_t asrpro_init(void);

/* 注册事件回调：ASRPRO 每上报一行即回调一次（传入的字符串不含换行符）。
 * 回调运行在接收任务上下文，请保持简短、不要阻塞。 */
esp_err_t asrpro_set_event_callback(void (*cb)(const char *line));

/* 播放 TF 卡中第 index 首音频（1 起，与 ASRPRO 固件里 MP3 选择文件的序号对应）。 */
esp_err_t asrpro_play(uint8_t index);
/* 停止当前播放。 */
esp_err_t asrpro_stop(void);
/* 设置 MP3 音量（0-100）。 */
esp_err_t asrpro_set_volume(uint8_t volume);
/* 下一首 / 上一首。 */
esp_err_t asrpro_next(void);
esp_err_t asrpro_prev(void);

/* 发送一行原始命令（自动补 '\n'），供后续扩展功能使用。 */
esp_err_t asrpro_send_line(const char *line);
