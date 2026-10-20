#include "asrpro.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 发送侧的 SAY 转到 V1.1 二进制链路（见 asrpro_say 的实现）。 */
#include "link_probe.h"

/* ASRPRO（鹿小班 V5.0）侧固定使用其串口2：PA5(TX) / PA6(RX)。
 * ESP32-C3 侧默认复用 LU6288 原有的 UART1（GPIO0/1），即“用 ASRPRO 替换 LU6288”的接线。
 * 若要与 LU6288 共存，请改用另一路 UART，并把日志切到 USB Serial/JTAG 释放 UART0。 */
#define ASRPRO_UART_NUM UART_NUM_1
#define ASRPRO_UART_TX_GPIO 1
#define ASRPRO_UART_RX_GPIO 0
#define ASRPRO_UART_BAUD_RATE 115200
#define ASRPRO_UART_BUFFER_SIZE 512
/* 收发一行命令的缓冲区。SAY 是最长的一种:24 个片段 ID,每个 5 位时
 * "SAY" + 24×" 65535" + '\0' = 148 字节,取 160 留余量。 */
#define ASRPRO_LINE_MAX 160U
#define ASRPRO_RX_TASK_STACK 3072
#define ASRPRO_RX_TASK_PRIORITY 4

static const char *TAG = "asrpro";
static bool s_initialized;
static void (*s_event_cb)(const char *line);

/* 发送一行命令，"P1"/"STOP"/"VOL 60" 等，统一以 '\n' 结尾作为帧分隔。 */
esp_err_t asrpro_send_line(const char *line)
{
    if (!s_initialized || line == NULL) return ESP_ERR_INVALID_STATE;

    const size_t len = strlen(line);
    if (len == 0U || len >= ASRPRO_LINE_MAX) return ESP_ERR_INVALID_ARG;

    char frame[ASRPRO_LINE_MAX + 1U];
    memcpy(frame, line, len);
    frame[len] = '\n';

    const int written = uart_write_bytes(ASRPRO_UART_NUM, frame, len + 1U);
    if (written != (int)(len + 1U)) {
        ESP_LOGE(TAG, "send failed: '%s' wrote=%d expected=%u", line, written, (unsigned)(len + 1U));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "tx: %s", line);
    return ESP_OK;
}

esp_err_t asrpro_play(uint8_t index)
{
    if (index == 0U) return ESP_ERR_INVALID_ARG;
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "P%u", (unsigned)index);
    return asrpro_send_line(cmd);
}

esp_err_t asrpro_stop(void)
{
    return asrpro_send_line("STOP");
}

esp_err_t asrpro_set_volume(uint8_t volume)
{
    if (volume > 100U) return ESP_ERR_INVALID_ARG;
    char cmd[12];
    snprintf(cmd, sizeof(cmd), "VOL %u", (unsigned)volume);
    return asrpro_send_line(cmd);
}

esp_err_t asrpro_next(void)
{
    return asrpro_send_line("NEXT");
}

esp_err_t asrpro_prev(void)
{
    return asrpro_send_line("PREV");
}

/* 拼出 "SAY <id> <id> ..." 并下发；ASRPRO 收到后按序播放这些播报音片段。 */
esp_err_t asrpro_say(const uint16_t *ids, size_t count)
{
    /* V1.1 二进制链路:直接交给 link_probe 发 T_SAY(片段 ID 走 16 位大端载荷)。
     * 保留本文件原有的 API 形状,time_service 里那十几处调用一个字都不用改。
     * 旧的 ASCII "SAY 120 131 ..." 行协议已废弃 —— 它在 9 月能跑,但无法承载
     * SEQ/CRC,也无法和现在的二进制帧共存。 */
    if (ids == NULL || count == 0U || count > ASRPRO_SAY_MAX) return ESP_ERR_INVALID_ARG;
    return link_probe_send_say(ids, (int)count);
}

/* 主动播报(整点报时 / 开机问候):同样走 V1.1 链路，但用 T_ANNOUNCE。
 * 区别是 ASRPRO 收到后只播片段，不会把设备拉进 20 秒唤醒态 ——
 * 报时是机器自己开口，用户没说话，不该跟着播"我退下了"。 */
esp_err_t asrpro_announce(const uint16_t *ids, size_t count)
{
    if (ids == NULL || count == 0U || count > ASRPRO_SAY_MAX) return ESP_ERR_INVALID_ARG;
    return link_probe_announce(ids, (int)count);
}

esp_err_t asrpro_set_event_callback(void (*cb)(const char *line))
{
    s_event_cb = cb;
    return ESP_OK;
}

/* 逐字节累积，遇到换行即认为一帧结束；超长行丢弃并等待下一次同步。 */
/* 链路自检:ASRPRO 正常时上电打 "INIT OK",每次识别打 "HIT <id>",每条指令打一行协议。
 * 这里统计【原始字节数】—— 只要收到过任何字节,就说明 PA5/PA6 这条物理链路是通的,
 * 哪怕数据没有换行(旧日志只打完整行,收到残缺字节时完全看不见,存在盲区)。
 * 反过来,连续 5 秒一个字节都没有就告一次警,这样只看 ESP32 日志就能判断
 * ASRPRO 到底有没有在发东西。 */
static uint32_t s_rx_bytes;
static uint32_t s_rx_bytes_reported;
static int s_silent_reported;
static TickType_t s_last_rx_tick;

static void asrpro_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    char line[ASRPRO_LINE_MAX];
    size_t pos = 0U;

    for (;;) {
        const int n = uart_read_bytes(ASRPRO_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n <= 0) {
            const TickType_t now = xTaskGetTickCount();
            if (!s_silent_reported && (now - s_last_rx_tick) > pdMS_TO_TICKS(5000)) {
                ESP_LOGW(TAG, "link silent: 0 byte in 5s (total=%u)", (unsigned)s_rx_bytes);
                s_silent_reported = 1;
            }
            continue;
        }
        s_last_rx_tick = xTaskGetTickCount();
        s_silent_reported = 0;
        s_rx_bytes += (uint32_t)n;
        if (s_rx_bytes != s_rx_bytes_reported) {
            /* 把原始字节按十六进制打出来。ASRPRO 那边没有可用的调试口,
             * 只能从 ESP32 侧反推: 正常文本应该是 49 4E 49 54 20 4F 4B ("INIT OK"),
             * 如果全是 00/FF 或者规律的高位字节,说明波特率还是对不上。 */
            ESP_LOGW(TAG, "raw %d byte(s) in, total=%u", n, (unsigned)s_rx_bytes);
            ESP_LOG_BUFFER_HEX(TAG, buf, (uint32_t)n);
            s_rx_bytes_reported = s_rx_bytes;
        }
        for (int i = 0; i < n; ++i) {
            const char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (pos == 0U) continue;
                line[pos] = '\0';
                ESP_LOGI(TAG, "rx: %s", line);
                if (s_event_cb != NULL) s_event_cb(line);
                pos = 0U;
            } else if (pos < ASRPRO_LINE_MAX - 1U) {
                line[pos++] = c;
            } else {
                pos = 0U;
            }
        }
    }
}

/* 判断一段原始字节里有没有出现过完整的 "PING"。
 * 不引 <string.h> 的 memcmp,自己逐字节比,省得再动头文件。 */
static int has_ping(const uint8_t *p, int n)
{
    for (int i = 0; i + 4 <= n; ++i) {
        if (p[i] == 'P' && p[i + 1] == 'I' && p[i + 2] == 'N' && p[i + 3] == 'G') return 1;
    }
    return 0;
}

/* 自动扫描 ASRPRO 的通信波特率。
 * ASRPRO 侧 app() 里每秒往串口0 发一行 "PING" 当信标:不管速率对不对,
 * ESP32 都会一直收到字节,但只有速率匹配的那一档才能解出完整的 "PING"。
 * 串口0(PB5/PB6)是 SDK 的日志/烧录口,实际速率由 SDK 决定,靠猜试不出来。
 * 每一档跑 3 秒(足够收到 2~3 个 PING),命中就固定下来,后续协议沿用。 */
static __attribute__((unused)) int asrpro_autobaud(void)
{
    static const uint32_t k_bauds[] = {
        9600, 19200, 38400, 57600, 115200, 230400,
        460800, 921600, 1000000, 1500000, 2000000,
    };
    uint8_t buf[128];
    const size_t total = sizeof(k_bauds) / sizeof(k_bauds[0]);

    for (size_t bi = 0; bi < total; ++bi) {
        const uint32_t baud = k_bauds[bi];
        (void)uart_set_baudrate(ASRPRO_UART_NUM, baud);
        (void)uart_flush_input(ASRPRO_UART_NUM);
        ESP_LOGW(TAG, "autobaud: try %u ...", (unsigned)baud);

        int hits = 0;
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
            const int n = uart_read_bytes(ASRPRO_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
            if (n <= 0) continue;
            if (has_ping(buf, n) && ++hits >= 2) break;
        }

        if (hits >= 2) {
            ESP_LOGW(TAG, "autobaud: *** FOUND %u baud ***", (unsigned)baud);
            (void)uart_flush_input(ASRPRO_UART_NUM);
            return 1;
        }
        ESP_LOGW(TAG, "autobaud: %u -> no match", (unsigned)baud);
    }

    ESP_LOGE(TAG, "autobaud: nothing matched, fall back to %d", ASRPRO_UART_BAUD_RATE);
    (void)uart_set_baudrate(ASRPRO_UART_NUM, ASRPRO_UART_BAUD_RATE);
    (void)uart_flush_input(ASRPRO_UART_NUM);
    return 0;
}

esp_err_t asrpro_init(void)
{
    const uart_config_t config = {
        .baud_rate = ASRPRO_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(ASRPRO_UART_NUM, &config);
    if (err != ESP_OK) return err;
    err = uart_set_pin(ASRPRO_UART_NUM, ASRPRO_UART_TX_GPIO, ASRPRO_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(ASRPRO_UART_NUM, ASRPRO_UART_BUFFER_SIZE,
                              ASRPRO_UART_BUFFER_SIZE, 0, NULL, 0);
    if (err != ESP_OK) return err;

    if (xTaskCreate(asrpro_rx_task, "asrpro_rx", ASRPRO_RX_TASK_STACK, NULL,
                    ASRPRO_RX_TASK_PRIORITY, NULL) != pdPASS) {
        uart_driver_delete(ASRPRO_UART_NUM);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "ASRPRO UART ready: TX=GPIO%d RX=GPIO%d baud=%d",
             ASRPRO_UART_TX_GPIO, ASRPRO_UART_RX_GPIO, ASRPRO_UART_BAUD_RATE);
    return ESP_OK;
}
