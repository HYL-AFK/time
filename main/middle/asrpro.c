#include "asrpro.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ASRPRO（鹿小班 V5.0）侧固定使用其串口2：PA5(TX) / PA6(RX)。
 * ESP32-C3 侧默认复用 LU6288 原有的 UART1（GPIO0/1），即“用 ASRPRO 替换 LU6288”的接线。
 * 若要与 LU6288 共存，请改用另一路 UART，并把日志切到 USB Serial/JTAG 释放 UART0。 */
#define ASRPRO_UART_NUM UART_NUM_1
#define ASRPRO_UART_TX_GPIO 1
#define ASRPRO_UART_RX_GPIO 0
#define ASRPRO_UART_BAUD_RATE 115200
#define ASRPRO_UART_BUFFER_SIZE 512
#define ASRPRO_LINE_MAX 64U
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

esp_err_t asrpro_set_event_callback(void (*cb)(const char *line))
{
    s_event_cb = cb;
    return ESP_OK;
}

/* 逐字节累积，遇到换行即认为一帧结束；超长行丢弃并等待下一次同步。 */
static void asrpro_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    char line[ASRPRO_LINE_MAX];
    size_t pos = 0U;

    for (;;) {
        const int n = uart_read_bytes(ASRPRO_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n <= 0) continue;
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
