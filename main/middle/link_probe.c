#include "link_probe.h"

#include <string.h>

#include "clock_logic.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "time_service.h"
#include "timer_service.h"

/* ============================================================================
 * 链路层测试（阶段 1）
 * ----------------------------------------------------------------------------
 * 只做三件事：发心跳、收回包、维护状态机。
 * 不涉及任何业务（时间/天气/闹钟），这样链路本身通不通一目了然。
 * ========================================================================== */

#define TAG "link"

#define LINK_UART_NUM        UART_NUM_1
#define LINK_UART_TX_GPIO    1
#define LINK_UART_RX_GPIO    0
/* 9600 而不是 115200:实测 115200 下 ASRPRO 收到的帧前 7 字节正确、
 * 第 8 字节开始漂移(cmd 0x01 -> 0x08),CRC 必然失败,所以对端从不回包。
 * 先用低速把链路跑通,再逐级往上试。 */
#define LINK_UART_BAUD       9600
#define LINK_UART_BUF         512

/* ===== V1.1 变长帧 =====
 *   53 54 | LEN | SEQ | TYPE | PAYLOAD(N) | CRC_L CRC_H | 43 21
 *   LEN = N + 5,总长度 = N + 10
 */
#define FRAME_MAX    64
#define PAYLOAD_MAX  48

/* A -> E */
#define T_PING         0x01
#define T_STAT         0x02
#define T_ASK_TIME     0x30
#define T_ASK_WEATHER  0x31
#define T_ALARM_SET    0x32
#define T_ALARM_OFF    0x34
#define T_ALARM_FULL   0x35   /* ESP32 告知两个槽位已满，ASRPRO 播提示并进入取消确认 */
#define T_TIMER_START       0x40   /* 正计时开始 */
#define T_TIMER_STOP        0x41   /* 正计时停止，ESP32 拼时长播报 */
#define T_TIMER_CANCEL      0x42   /* 取消计时 */
#define T_COUNTDOWN_SET     0x43   /* 载荷: 秒数(2B 大端) */
#define T_COUNTDOWN_CANCEL  0x44   /* 取消倒计时 */

/* E -> A */
#define T_PONG         0x80
#define T_SAY          0x20
/* ESP32 主动播报(整点报时 / 开机问候)。
 * 和 T_SAY 的载荷完全一样(一串 16 位片段 ID)，区别只在 ASRPRO 的处理：
 * T_SAY 是对用户提问的回应，要续上 20 秒唤醒态方便连续对话；
 * T_ANNOUNCE 却是机器自己开口，用户没说话，不能让它进唤醒态 ——
 * 否则每次整点报时完 20 秒就会冒出一句「我退下了」。 */
#define T_ANNOUNCE     0x21
#define T_SLEEP        0x10
#define T_WAKE         0x11
#define T_STOP         0x12
#define T_VOL          0x13
#define T_PLAY         0x14

#define PWR_NORMAL  0x00
#define PWR_LIGHT   0x01
#define PWR_PLAYING 0x02

static const uint8_t kHead[2] = { 0x53, 0x54 };
static const uint8_t kTail[2] = { 0x43, 0x21 };

#define PING_INTERVAL_MS   1000
#define PONG_TIMEOUT_MS     300
#define MISS_MAX                3

static link_status_t s_status;
static uint8_t       s_seq;            /* 帧序号，每发一帧自增 */
/* 心跳任务、接收任务、timer_task 都会发帧，s_seq 和 UART 写入必须串行，
 * 否则会出现重复 SEQ、以及两帧字节交错的脏帧。 */
static SemaphoreHandle_t s_tx_mutex;
static int           s_miss;
static uint32_t      s_last_tx_ms;     /* 最近一次发送时刻，用来算 rtt */
static uint8_t       s_last_pong_seq;  /* 最近一次回包带回的 SEQ */
static link_state_cb_t s_state_cb;     /* 状态变化回调，用于灯环异常提示 */

/* 最近一次 WAKE/SLEEP 请求的回包等待 */
static volatile int  s_resp_flag;

/* ===== CRC16-Modbus ===== */
static uint16_t crc16_modbus(const uint8_t *p, int n)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ===== 组装并发送定长帧 =====
 * 载荷固定 PAYLOAD_MAX(48) 字节，不足补 0，LEN 恒为 53、整帧恒为 57 字节。
 * 定长的好处：两端都不需要先读 LEN 才能确定边界，收到 53 54 后
 * 直接按 57 字节收满就能解，不会因为某帧 LEN 错一位而丢一片数据。 */
static esp_err_t send_frame(uint8_t type, const uint8_t *pl, uint8_t n)
{
    if (n > PAYLOAD_MAX) n = PAYLOAD_MAX;

    if (s_tx_mutex != NULL) xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    uint8_t f[FRAME_MAX];
    f[0] = kHead[0];
    f[1] = kHead[1];
    f[2] = (uint8_t)(PAYLOAD_MAX + 5);   /* LEN 固定 53 */
    f[3] = s_seq++;                      /* SEQ */
    f[4] = type;
    /* 载荷补 0xFF：SAY 类载荷是 16 位片段 ID，0 可能是合法 ID，
     * 用 0xFFFF 补齐，ASRPRO 收到就能识别出“后面没有更多片段”并停。 */
    for (uint8_t i = 0; i < PAYLOAD_MAX; i++) f[5 + i] = 0xFF;
    for (uint8_t i = 0; i < n; i++) f[5 + i] = (pl != NULL) ? pl[i] : 0;

    const uint16_t c = crc16_modbus(f + 2, PAYLOAD_MAX + 3);
    f[5 + PAYLOAD_MAX] = (uint8_t)(c & 0xFF);
    f[6 + PAYLOAD_MAX] = (uint8_t)((c >> 8) & 0xFF);
    f[7 + PAYLOAD_MAX] = kTail[0];
    f[8 + PAYLOAD_MAX] = kTail[1];

    const int total = PAYLOAD_MAX + 9;   /* 57 */
    s_last_tx_ms = now_ms();
    const esp_err_t err = uart_write_bytes(LINK_UART_NUM, (const char *)f, total) == total
                          ? ESP_OK : ESP_FAIL;
    if (s_tx_mutex != NULL) xSemaphoreGive(s_tx_mutex);
    return err;
}

/* ===== 片段 ID 表(与 ASRPRO 词表一一对应) ===== */
#define FRAG_HOUR      120   /* 点 */
#define FRAG_MINUTE    121   /* 分 */
#define FRAG_WHOLE     122   /* 整 */
#define FRAG_DIGIT0    123   /* 零..九 = 123..132 */
#define FRAG_TEN       133   /* 十 */
#define FRAG_NO_TIME   134   /* 时间还没校准 */
#define FRAG_ALARM_OK  135   /* 好的，已经帮你设定好闹钟了 */

/* 时段片段:12 小时制下靠它区分上午 9 点和晚上 9 点。
 * 这几条是闹钟确认句用过的词(75~82 那一批),语音资源已经在固件里，
 * 不需要新建命令词。 */
#define FRAG_DAWN      82    /* 凌晨   0:00 ~ 5:59 */
#define FRAG_EARLY     78    /* 早上   6:00 ~ 8:59 */
#define FRAG_MORNING   79    /* 上午   9:00 ~ 11:59 */
#define FRAG_AFTERNOON 80    /* 下午  12:00 ~ 17:59 */
#define FRAG_EVENING   81    /* 晚上  18:00 ~ 23:59 */

/* 引导语「现在时间是」,用命令词 30 的 ASRTO —— 现成有音频、CMDTBL valid=1。
 * 以前预留的 196 词表里一直没有(CHK valid=0、PLAY ret=1),
 * 整句就只剩「上午 十一点整」,听上去少了一截。 */
#define FRAG_NOW       30

#define SAY_ID_MAX     24

/* 发一串片段 ID(16 位大端):ASRPRO 逐个 prompt_play_by_cmd_id 播。
 * type 用 T_SAY(对话回应) 或 T_ANNOUNCE(主动播报)。 */
static esp_err_t send_ids(uint8_t type, const uint16_t *ids, int n)
{
    uint8_t pl[PAYLOAD_MAX];
    if (n > (int)(PAYLOAD_MAX / 2)) n = (int)(PAYLOAD_MAX / 2);
    for (int i = 0; i < n; i++) {
        pl[i * 2]     = (uint8_t)(ids[i] >> 8);
        pl[i * 2 + 1] = (uint8_t)(ids[i] & 0xFF);
    }
    return send_frame(type, pl, (uint8_t)(n * 2));
}

static esp_err_t send_say_ids(const uint16_t *ids, int n)
{
    return send_ids(T_SAY, ids, n);
}

/* 0~99 的读法: 5 -> 零/五, 15 -> 十/五, 23 -> 二/十/三
 * pad_zero: 个位数要不要先读「零」。分钟要(9:05 读作"九点零五分"),
 * 小时不要("九点",不是"零九点")。 */
static int push_two_digit(uint16_t *out, int p, int v, int pad_zero)
{
    if (v < 10) {
        if (pad_zero) out[p++] = (uint16_t)FRAG_DIGIT0;   /* 零 */
        out[p++] = (uint16_t)(FRAG_DIGIT0 + v);
    } else if (v < 20) {
        out[p++] = FRAG_TEN;
        if (v > 10) out[p++] = (uint16_t)(FRAG_DIGIT0 + (v - 10));
    } else {
        out[p++] = (uint16_t)(FRAG_DIGIT0 + v / 10);
        out[p++] = FRAG_TEN;
        if (v % 10) out[p++] = (uint16_t)(FRAG_DIGIT0 + (v % 10));
    }
    return p;
}

/* 小时 -> 时段片段。边界按中文习惯：
 * 0~5 凌晨 / 6~8 早上 / 9~11 上午 / 12~17 下午 / 18~23 晚上。 */
static uint16_t period_fragment(int hour)
{
    if (hour < 6)  return FRAG_DAWN;
    if (hour < 9)  return FRAG_EARLY;
    if (hour < 12) return FRAG_MORNING;
    if (hour < 18) return FRAG_AFTERNOON;
    return FRAG_EVENING;
}

/* 回一段 SAY:当前时间(12 小时制 + 时段前缀)。
 * 「晚上九点五十分」比「二十一点五十分」更好读，也不用为 13~23 点单独做词。
 *
 * 引导语用 30「现在时间是」—— 现成有音频、CMDTBL valid=1，
 * 问"现在几点"走的也是它。以前用的 196 词表里一直没有，
 * 播报时 CHK valid=0、PLAY ret=1，整句就少了开头一截。 */
esp_err_t link_probe_say_time(void)
{
    clock_time_t t;
    uint16_t ids[SAY_ID_MAX];
    int n = 0;

    if (!time_service_get_local(&t)) {
        ids[n++] = FRAG_NO_TIME;
        return send_say_ids(ids, n);
    }

    ids[n++] = FRAG_NOW;                        /* 现在时间是 */
    ids[n++] = period_fragment((int)t.hour);

    int h = t.hour % 12;
    if (h == 0) h = 12;                 /* 0 点读作十二点 */
    n = push_two_digit(ids, n, h, 0);
    ids[n++] = FRAG_HOUR;

    if (t.minute == 0) {
        ids[n++] = FRAG_WHOLE;          /* 八点整 */
    } else {
        n = push_two_digit(ids, n, t.minute, 1);   /* 9:05 -> 零五分 */
        ids[n++] = FRAG_MINUTE;
    }

    ESP_LOGI(TAG, "say time %02u:%02u -> %d fragments", t.hour, t.minute, n);
    return send_say_ids(ids, n);
}

/* 通用入口:time_service 的天气/闹钟/整点报时都走这里 */
esp_err_t link_probe_send_say(const uint16_t *ids, int count)
{
    if (ids == NULL || count <= 0) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "say %d fragments", count);
    return send_say_ids(ids, count);
}

/* ESP32 主动播报:整点报时、开机问候。
 * 走 T_ANNOUNCE，ASRPRO 只播片段，不进入 20 秒唤醒态。 */
esp_err_t link_probe_announce(const uint16_t *ids, int count)
{
    if (ids == NULL || count <= 0) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "announce %d fragments", count);
    return send_ids(T_ANNOUNCE, ids, count);
}

/* 播放 TF 卡根目录的音频文件（整点报时的 001~012、闹钟铃声 101）。
 * 载荷只有 1 字节文件序号，ASRPRO 侧调 playMp3Folder(0, n)。
 * 以前的 asrpro_play() 走 ASCII "P<n>"，但 LINK_TEST_MODE 下 asrpro_init()
 * 根本不会执行（跟 link_probe 互斥），所以那条路一直是静默失败。 */
esp_err_t link_probe_play(uint8_t index)
{
    if (index == 0U) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "play file %u", (unsigned)index);
    return send_frame(T_PLAY, &index, 1);
}

esp_err_t link_probe_stop(void)
{
    ESP_LOGI(TAG, "stop playback");
    return send_frame(T_STOP, NULL, 0);
}

/* 音量 0~100。短按 BOOT 静音就是调 0，之后还能恢复。 */
esp_err_t link_probe_set_volume(uint8_t volume)
{
    if (volume > 100U) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "volume %u", (unsigned)volume);
    return send_frame(T_VOL, &volume, 1);
}

/* 两个闹钟槽位都满了：让 ASRPRO 播“已帮你找到两个…需要帮你取消闹钟”，
 * 同时进入“是否全部取消”的确认状态。 */
esp_err_t link_probe_alarm_full(void)
{
    ESP_LOGI(TAG, "alarm slots full");
    return send_frame(T_ALARM_FULL, NULL, 0);
}

/* ===== 解析定长回包（载荷恒为 PAYLOAD_MAX 字节） ===== */
static bool parse_response(const uint8_t *f)
{
    if (f[0] != kHead[0] || f[1] != kHead[1]) return false;

    const uint8_t n = (uint8_t)PAYLOAD_MAX;   /* 定长：不再从 LEN 推导 */
    if (f[2] != (uint8_t)(PAYLOAD_MAX + 5)) return false;
    if (f[7 + n] != kTail[0] || f[8 + n] != kTail[1]) return false;

    const uint16_t want = (uint16_t)f[5 + n] | ((uint16_t)f[6 + n] << 8);
    if (want != crc16_modbus(f + 2, n + 3)) {
        ESP_LOGW(TAG, "response CRC error");
        return false;
    }

    /* ---- 业务请求:ASRPRO 上报,ESP32 回一段 SAY ---- */
    /* 只置标志，由 time_service 的 daily_task 播报：
     * 在接收任务里直接 asrpro_say() 会等 PONG，而 PONG 也得这个任务读，会自锁。 */
    if (f[4] == T_ASK_TIME) {
        ESP_LOGI(TAG, "peer asks TIME");
        time_service_request_time();
        return true;
    }

    if (f[4] == T_ASK_WEATHER) {
        /* 天气缓存由校时成功时预先拉取，这里不联网。 */
        ESP_LOGI(TAG, "peer asks WEATHER");
        time_service_request_weather();
        return true;
    }

    /* ---- 闹钟:ASRPRO 侧的拼装状态机凑齐要素后上报 ----
     * 这里只管转发,真正的 NVS 写入、找空位/反问替换哪个、播确认句
     * 都在 time_service 的 daily_task 里做,不阻塞串口接收任务。 */
    if (f[4] == T_ALARM_SET && n >= 4) {
        ESP_LOGI(TAG, "peer sets alarm: day=%u period=%u %02u:%02u", (unsigned)f[5],
                 (unsigned)f[6], (unsigned)f[7], (unsigned)f[8]);
        time_service_on_alarm_set(f[5], f[6], f[7], f[8]);
        return true;
    }

    if (f[4] == T_ALARM_OFF) {
        ESP_LOGI(TAG, "peer clears alarms");
        time_service_on_alarm_off();
        return true;
    }

    /* ---- 计时器 ---- */
    if (f[4] == T_TIMER_START) {
        ESP_LOGI(TAG, "peer starts stopwatch");
        timer_service_start_countup();
        return true;
    }

    if (f[4] == T_TIMER_STOP) {
        ESP_LOGI(TAG, "peer stops stopwatch");
        timer_service_stop_countup();
        return true;
    }

    if (f[4] == T_TIMER_CANCEL) {
        ESP_LOGI(TAG, "peer cancels stopwatch");
        timer_service_cancel();
        return true;
    }

    if (f[4] == T_COUNTDOWN_SET && n >= 2) {
        const uint16_t seconds = ((uint16_t)f[5] << 8) | (uint16_t)f[6];
        ESP_LOGI(TAG, "peer sets countdown %u s", (unsigned)seconds);
        timer_service_start_countdown(seconds);
        return true;
    }

    if (f[4] == T_COUNTDOWN_CANCEL) {
        ESP_LOGI(TAG, "peer cancels countdown");
        timer_service_cancel_countdown();
        return true;
    }

    if (f[4] != T_PONG || n < 6) return false;

    /* PONG 载荷: status(1) rx(2) crc_err(2) pwr(1) */
    const uint8_t *pl = f + 5;
    s_status.peer_rx      = ((uint16_t)pl[1] << 8) | pl[2];
    s_status.peer_crc_err = ((uint16_t)pl[3] << 8) | pl[4];
    s_status.power_mode   = pl[5];

    const uint32_t now = now_ms();
    s_status.rtt_ms = (now >= s_last_tx_ms) ? (now - s_last_tx_ms) : 0;
    s_last_pong_seq = f[3];
    s_resp_flag = 1;
    return true;
}

void link_probe_set_state_callback(link_state_cb_t cb)
{
    s_state_cb = cb;
}

const char *link_state_name(link_state_t s)
{
    switch (s) {
        case LINK_STATE_INIT:    return "INIT";
        case LINK_STATE_LINK:    return "LINK";
        case LINK_STATE_READY:   return "READY";
        case LINK_STATE_OFFLINE: return "OFFLINE";
        default:                 return "?";
    }
}

static const char *power_name(uint8_t p)
{
    switch (p) {
        case PWR_NORMAL:  return "NORMAL";
        case PWR_LIGHT:   return "LIGHT";
        case PWR_PLAYING: return "PLAYING";
        default:          return "UNKNOWN";
    }
}

/* ===== 接收任务：按 53 54 对齐，按 LEN 收满整帧 ===== */
static void link_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[FRAME_MAX];
    int len = 0;
    uint32_t raw = 0;

    for (;;) {
        uint8_t b;
        if (uart_read_bytes(LINK_UART_NUM, &b, 1, pdMS_TO_TICKS(200)) != 1) continue;

        /* 原始字节计数:只要收到过任何字节,就说明物理链路是通的,
         * 问题在帧格式/波特率;一个字节都没有则是接线或对端没上电。 */
        raw++;
        if (raw <= 40 || (raw % 200) == 0) {
            ESP_LOGW(TAG, "raw %u byte(s), last=0x%02X", (unsigned)raw, b);
        }

        if (len == 0 && b != kHead[0]) continue;
        if (len == 1 && b != kHead[1]) { len = 0; continue; }

        if (len >= FRAME_MAX) len = 0;
        buf[len++] = b;

        /* 第 3 字节是 LEN,总长度 = LEN + 4 */
        if (len >= 3) {
            const int total = (int)buf[2] + 4;
            if (total < 9 || total > FRAME_MAX) { len = 0; continue; }
            if (len >= total) {
                parse_response(buf);
                len = 0;
            }
        }
    }
}

/* ===== 心跳任务：每秒 PING，三次超时判 OFFLINE ===== */
static void link_hb_task(void *arg)
{
    (void)arg;
    link_status_t prev = { 0 };
    int said = 0;

    for (;;) {
        /* 链路稳定 10 秒后自动发一次 SAY,验证喇叭与 TF 卡通路。
         * 只发一次,避免每次上电都响。 */
        if (!said && s_status.state == LINK_STATE_READY && s_status.ok_count >= 10) {
            said = 1;
            ESP_LOGI(TAG, "auto SAY: speaker check");
            (void)send_frame(T_SAY, NULL, 0);
        }

        s_resp_flag = 0;

        if (send_frame(T_PING, NULL, 0) != ESP_OK) {
            ESP_LOGW(TAG, "uart write failed");
        }

        /* 等回包最多 PONG_TIMEOUT_MS */
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PONG_TIMEOUT_MS);
        while ((int32_t)(deadline - xTaskGetTickCount()) > 0 && !s_resp_flag) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (s_resp_flag) {
            s_miss = 0;
            s_status.ok_count++;
            if (s_status.state == LINK_STATE_INIT || s_status.state == LINK_STATE_OFFLINE) {
                ESP_LOGI(TAG, "state -> LINK");
                s_status.state = LINK_STATE_LINK;
                if (s_state_cb != NULL) s_state_cb(s_status.state);
            } else if (s_status.state == LINK_STATE_LINK) {
                ESP_LOGI(TAG, "state -> READY");
                s_status.state = LINK_STATE_READY;
                if (s_state_cb != NULL) s_state_cb(s_status.state);
            }

            /* 只在数据有变化时打印，避免每秒刷屏 */
            if (s_status.peer_rx != prev.peer_rx || s_last_pong_seq != prev.peer_pong
                || s_status.power_mode != prev.power_mode) {
                ESP_LOGI(TAG, "state=%s rtt=%ums power=%s peer[ok=%u crc=%u pong=%u]",
                         link_state_name(s_status.state), (unsigned)s_status.rtt_ms,
                         power_name(s_status.power_mode),
                         (unsigned)s_status.peer_rx, (unsigned)s_status.peer_crc_err,
                         (unsigned)s_last_pong_seq);
                prev = s_status;
            }
        } else {
            s_status.timeout_count++;
            if (++s_miss >= MISS_MAX) {
                if (s_status.state != LINK_STATE_OFFLINE) {
                    ESP_LOGE(TAG, "state -> OFFLINE (3 misses, %u timeouts)",
                             (unsigned)s_status.timeout_count);
                    s_status.state = LINK_STATE_OFFLINE;
                    if (s_state_cb != NULL) s_state_cb(s_status.state);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS > PONG_TIMEOUT_MS
                                 ? PING_INTERVAL_MS - PONG_TIMEOUT_MS : 0));
    }
}

static esp_err_t request_power(uint8_t type, const char *name)
{
    if (s_status.state != LINK_STATE_READY) {
        ESP_LOGW(TAG, "%s rejected: state=%s", name, link_state_name(s_status.state));
        return ESP_ERR_INVALID_STATE;
    }
    s_resp_flag = 0;
    const esp_err_t e = send_frame(type, NULL, 0);
    if (e != ESP_OK) return e;

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0 && !s_resp_flag) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_resp_flag) {
        ESP_LOGW(TAG, "%s timeout", name);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "%s ok, power now %s", name, power_name(s_status.power_mode));
    return ESP_OK;
}

esp_err_t link_probe_wake(void)  { return request_power(T_WAKE,  "WAKE"); }
esp_err_t link_probe_sleep(void) { return request_power(T_SLEEP, "SLEEP"); }
esp_err_t link_probe_say(void)   { return request_power(T_SAY,   "SAY"); }

void link_probe_get_status(link_status_t *out)
{
    if (out != NULL) *out = s_status;
}

esp_err_t link_probe_init(void)
{
    if (s_status.state != LINK_STATE_INIT) return ESP_OK;
    s_status.state = LINK_STATE_INIT;

    if (s_tx_mutex == NULL) s_tx_mutex = xSemaphoreCreateMutex();

    const uart_config_t cfg = {
        .baud_rate  = LINK_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(LINK_UART_NUM, LINK_UART_BUF, LINK_UART_BUF, 0, NULL, 0);
    if (err != ESP_OK) return err;

    err = uart_param_config(LINK_UART_NUM, &cfg);
    if (err != ESP_OK) { uart_driver_delete(LINK_UART_NUM); return err; }

    err = uart_set_pin(LINK_UART_NUM, LINK_UART_TX_GPIO, LINK_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) { uart_driver_delete(LINK_UART_NUM); return err; }

    if (xTaskCreate(link_rx_task, "link_rx", 3072, NULL, 9, NULL) != pdPASS) {
        uart_driver_delete(LINK_UART_NUM);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(link_hb_task, "link_hb", 3072, NULL, 5, NULL) != pdPASS) {
        uart_driver_delete(LINK_UART_NUM);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "link probe ready: TX=GPIO%d RX=GPIO%d baud=%d",
             LINK_UART_TX_GPIO, LINK_UART_RX_GPIO, LINK_UART_BAUD);
    return ESP_OK;
}
