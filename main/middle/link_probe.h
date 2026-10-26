#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 链路层 + 业务层（V1.1 变长帧）。
 *
 * 与 asrpro.c 互斥：两者都用 UART1，由 main.c 里的 LINK_TEST_MODE 宏选择其一。
 *
 *   53 54 | LEN | SEQ | TYPE | PAYLOAD(N) | CRC_L CRC_H | 43 21
 *   LEN = N + 5   总长度 = N + 9   CRC16-Modbus 覆盖 LEN..PAYLOAD
 */

typedef enum {
    LINK_STATE_INIT = 0,   /* 刚上电，尚未通信 */
    LINK_STATE_LINK,       /* 收到过合法回包 */
    LINK_STATE_READY,      /* 心跳连续成功 */
    LINK_STATE_OFFLINE,    /* 连续 3 次心跳超时 */
} link_state_t;

typedef struct {
    link_state_t state;
    uint8_t      power_mode;    /* 0x00 正常 / 0x01 降频待机 / 0x02 播放中 */
    uint16_t     peer_rx;       /* ASRPRO 累计收帧数 */
    uint16_t     peer_crc_err;  /* ASRPRO 累计 CRC 错帧数 */
    uint32_t     peer_pong;     /* ASRPRO 累计发出回包数 */
    uint32_t     rtt_ms;        /* 最近一次往返耗时 */
    uint32_t     ok_count;      /* 本机成功次数 */
    uint32_t     timeout_count; /* 本机超时次数 */
} link_status_t;

esp_err_t link_probe_init(void);

/* 链路状态变化回调（INIT/LINK/READY/OFFLINE）。
 * 心跳连续 3 次超时就会报 OFFLINE，上层用它点亮灯环的异常提示。 */
typedef void (*link_state_cb_t)(link_state_t state);
void link_probe_set_state_callback(link_state_cb_t cb);
void      link_probe_get_status(link_status_t *out);
const char *link_state_name(link_state_t s);

/* 主动请求 ASRPRO 切换功耗模式（内部发送 SLEEP/WAKE 指令并等待回包）。 */
esp_err_t link_probe_wake(void);
esp_err_t link_probe_sleep(void);

/* 让 ASRPRO 播一段测试音，验证喇叭通路。 */
esp_err_t link_probe_say(void);

/* 回复"当前时间"的片段序列（收到 ASK_TIME 时由内部自动调用，也可外部直接调）。 */
esp_err_t link_probe_say_time(void);

/* 通用 SAY：把一串片段 ID（16 位）发给 ASRPRO 顺序播放。
 * time_service 的天气/闹钟等“对用户提问的回应”走这里（T_SAY）。 */
esp_err_t link_probe_send_say(const uint16_t *ids, int count);

/* 主动播报：整点报时、开机问候。走 T_ANNOUNCE，ASRPRO 只播片段，
 * 不会进入 20 秒唤醒态（否则报时完会自己冒出一句“我退下了”）。 */
esp_err_t link_probe_announce(const uint16_t *ids, int count);

/* 播放 TF 卡根目录的第 index 个文件（整点提示音 001~012 / 闹钟铃声 101）。 */
esp_err_t link_probe_play(uint8_t index);

/* 立即停止当前音频播放。 */
esp_err_t link_probe_stop(void);

/* 设置音量（0~100）。BOOT 短按静音就是调到 0。 */
esp_err_t link_probe_set_volume(uint8_t volume);

/* 告知 ASRPRO 两个闹钟槽位已满：播“已帮你找到两个…需要帮你取消闹钟”，
 * 并进入“是否全部取消”的确认状态。 */
esp_err_t link_probe_alarm_full(void);
