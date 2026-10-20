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

/* 让 ASRPRO 按序播放一串预置播报音片段（片段 ID = ASRPRO 固件里 playid 的编号）。
 * 用于"当前时间"这类需要拼接的播报：数字由 0~9 与"十"拼出，避免为每个数字录整句。 */
/* 一条 SAY 最多带多少个片段。最长的一条是"闹钟满了"的播报:
 * 已经设了两个闹钟了(1) + 第一个是(1) + 闹钟1(最多9) + 第二个是(1) + 闹钟2(最多9)
 * + 要替换哪一个(1) = 22 段。
 * 闹钟响铃播报更长:问候2 + 现在时间是1 + 时间7 + 天气14 + 起床1 = 25 段,
 * 所以取 28 留 3 段余量。ASRPRO 侧 tmp/asr_dialog.cpp 的 SAY_MAX 必须同步。 */
#define ASRPRO_SAY_MAX 28U
esp_err_t asrpro_say(const uint16_t *ids, size_t count);

/* 主动播报(整点报时 / 开机问候)：同样是一串片段 ID，但走 T_ANNOUNCE，
 * ASRPRO 只播不改唤醒态。 */
esp_err_t asrpro_announce(const uint16_t *ids, size_t count);
