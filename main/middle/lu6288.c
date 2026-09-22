#include "lu6288.h"

#include <stddef.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define LU6288_UART_NUM UART_NUM_1
#define LU6288_UART_RX_GPIO 0
#define LU6288_UART_TX_GPIO 1
#define LU6288_UART_BAUD_RATE 9600
#define LU6288_UART_BUFFER_SIZE 256
#define LU6288_MAX_TEXT_BYTES 250U

static const char *TAG = "lu6288";
static bool s_initialized;

esp_err_t lu6288_init(void)
{
    const uart_config_t config = {
        .baud_rate = LU6288_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(LU6288_UART_NUM, &config);
    if (err != ESP_OK) return err;
    err = uart_set_pin(LU6288_UART_NUM, LU6288_UART_TX_GPIO, LU6288_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(LU6288_UART_NUM, LU6288_UART_BUFFER_SIZE,
                              LU6288_UART_BUFFER_SIZE, 0, NULL, 0);
    if (err == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(TAG, "LU6288 UART ready: TX=GPIO%d RX=GPIO%d baud=%d",
                 LU6288_UART_TX_GPIO, LU6288_UART_RX_GPIO, LU6288_UART_BAUD_RATE);
    }
    return err;
}

/* GB2312 (the module's required charset) byte tables. The module does NOT
 * understand UTF-8 Chinese; sending UTF-8 bytes produces broken/truncated audio. */
static const uint8_t kGbFixedNow[] = {0xCF, 0xD6, 0xD4, 0xDA, 0xCA, 0xC7}; /* 现在是 */
static const uint8_t kGbDotHour[] = {0xB5, 0xE3, 0xD5, 0xFB};               /* 点整 */
/* 零 一 二 三 四 五 六 七 八 九 */
static const uint8_t kGbDigit[10][2] = {
    {0xC1, 0xE3}, {0xD2, 0xBB}, {0xB6, 0xFE}, {0xC8, 0xFD}, {0xCB, 0xC4},
    {0xCE, 0xE5}, {0xC1, 0xF9}, {0xC6, 0xDF}, {0xB0, 0xCB}, {0xBE, 0xC5},
};
static const uint8_t kGbTen[] = {0xCA, 0xAE};                         /* 十 */
static const uint8_t kGbDawn[] = {0xC1, 0xE8, 0xB3, 0xBF};            /* 凌晨 */
static const uint8_t kGbMorning[] = {0xC9, 0xCF, 0xCE, 0xE7};         /* 上午 */
static const uint8_t kGbNoon[] = {0xD6, 0xD0, 0xCE, 0xE7};            /* 中午 */
static const uint8_t kGbAfternoon[] = {0xCF, 0xC2, 0xCE, 0xE7};       /* 下午 */
static const uint8_t kGbEvening[] = {0xCD, 0xED, 0xC9, 0xCF};         /* 晚上 */

/* Build a single "<G>" + payload frame and transmit it in one contiguous write,
 * matching the official reference drivers (one string per command). */
static esp_err_t lu6288_send_frame(const uint8_t *data, size_t len)
{
    if (!s_initialized || data == NULL || len == 0U) return ESP_ERR_INVALID_STATE;
    if (len > LU6288_MAX_TEXT_BYTES) return ESP_ERR_INVALID_SIZE;

    // <G> 是帧头，后接待合成文本；合并成连续帧发送，模块将其解析为一条播报命令。
    uint8_t frame[LU6288_MAX_TEXT_BYTES + 3U];
    frame[0] = '<';
    frame[1] = 'G';
    frame[2] = '>';
    memcpy(frame + 3U, data, len);
    const size_t total = len + 3U;

    ESP_LOGI(TAG, "speech start: text_bytes=%u", (unsigned)len);

    const int written = uart_write_bytes(LU6288_UART_NUM, frame, total);
    if (written != (int)total) {
        ESP_LOGE(TAG, "speech write failed: wrote=%d expected=%u", written, (unsigned)total);
        return ESP_FAIL;
    }
    const esp_err_t wait_err = uart_wait_tx_done(LU6288_UART_NUM, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "speech tx complete: result=%s", esp_err_to_name(wait_err));
    return wait_err;
}

esp_err_t lu6288_speak_raw(const uint8_t *data, size_t len)
{
    return lu6288_send_frame(data, len);
}

esp_err_t lu6288_report_hour(uint8_t hour)
{
    if (hour > 23U) return ESP_ERR_INVALID_ARG;

    /* 12 小时制数字: 24 小时制的 0 点与 12 点都对应 "十二" */
    const uint8_t h12 = (uint8_t)(hour % 12U == 0U ? 12U : hour % 12U);

    /* 按 24 小时制区间选择时段词 */
    const uint8_t *segment;
    if (hour < 6U) {
        segment = kGbDawn;        /* 凌晨 */
    } else if (hour < 12U) {
        segment = kGbMorning;     /* 上午 */
    } else if (hour == 12U) {
        segment = kGbNoon;        /* 中午 */
    } else if (hour < 19U) {
        segment = kGbAfternoon;   /* 下午 */
    } else {
        segment = kGbEvening;     /* 晚上 */
    }

    /* 组装 "<G>现在是[时段][数字]点整" */
    uint8_t frame[32];
    size_t pos = 0U;
    frame[pos++] = '<';
    frame[pos++] = 'G';
    frame[pos++] = '>';
    memcpy(frame + pos, kGbFixedNow, sizeof(kGbFixedNow));
    pos += sizeof(kGbFixedNow);
    memcpy(frame + pos, segment, 2U);
    pos += 2U;
    if (h12 <= 9U) {
        memcpy(frame + pos, kGbDigit[h12], sizeof(kGbDigit[h12]));
        pos += sizeof(kGbDigit[h12]);
    } else {  /* 10-12 -> 十 / 十一 / 十二 */
        memcpy(frame + pos, kGbTen, sizeof(kGbTen));
        pos += sizeof(kGbTen);
        if (h12 == 11U || h12 == 12U) {
            memcpy(frame + pos, kGbDigit[h12 - 10U], sizeof(kGbDigit[h12 - 10U]));
            pos += sizeof(kGbDigit[h12 - 10U]);
        }
    }
    memcpy(frame + pos, kGbDotHour, sizeof(kGbDotHour));
    pos += sizeof(kGbDotHour);

    return lu6288_send_frame(frame, pos);
}
