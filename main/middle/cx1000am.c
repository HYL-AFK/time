#include "cx1000am.h"

#include <stddef.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define CX1000AM_UART_NUM UART_NUM_1
#define CX1000AM_UART_RX_GPIO 0
#define CX1000AM_UART_TX_GPIO 1
#define CX1000AM_UART_BAUD_RATE 9600
#define CX1000AM_UART_BUFFER_SIZE 256
#define CX1000AM_FRAME_START 0x7EU
#define CX1000AM_FRAME_END 0xEFU
#define CX1000AM_PLAY_TRACK_COMMAND 0x07U

static const char *TAG = "cx1000am";
static bool s_initialized;

static uint8_t checksum(const uint8_t *data, size_t length)
{
    uint16_t sum = 0;
    for (size_t index = 0; index < length; ++index) sum += data[index];
    return (uint8_t)sum;
}

esp_err_t cx1000am_init(void)
{
    const uart_config_t config = {
        .baud_rate = CX1000AM_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(CX1000AM_UART_NUM, &config);
    if (err != ESP_OK) return err;
    err = uart_set_pin(CX1000AM_UART_NUM, CX1000AM_UART_TX_GPIO, CX1000AM_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(CX1000AM_UART_NUM, CX1000AM_UART_BUFFER_SIZE,
                              CX1000AM_UART_BUFFER_SIZE, 0, NULL, 0);
    if (err == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(TAG, "CX1000AM UART ready: TX=GPIO%d RX=GPIO%d baud=%d",
                 CX1000AM_UART_TX_GPIO, CX1000AM_UART_RX_GPIO, CX1000AM_UART_BAUD_RATE);
    }
    return err;
}

esp_err_t cx1000am_play_track(uint16_t track)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    // CX1000AM 指定曲目播放: 7E, length, FF, FF, 07, track_hi, track_lo, checksum, EF.
    uint8_t frame[9] = {
        CX1000AM_FRAME_START,
        9U,
        0xFFU,
        0xFFU,
        CX1000AM_PLAY_TRACK_COMMAND,
        (uint8_t)(track >> 8U),
        (uint8_t)(track & 0xFFU),
        0U,
        CX1000AM_FRAME_END,
    };
    frame[7] = checksum(frame, 7U);
    const int written = uart_write_bytes(CX1000AM_UART_NUM, frame, sizeof(frame));
    if (written != (int)sizeof(frame)) return ESP_FAIL;
    return uart_wait_tx_done(CX1000AM_UART_NUM, pdMS_TO_TICKS(100));
}

bool cx1000am_report_hour(uint8_t hour)
{
    uint16_t track = 0;
    if (hour == 9U) track = 1U;       // 00001.mp3: 现在是上午九点整
    else if (hour == 17U) track = 2U; // 00002.mp3: 现在是下午五点整
    else return false;

    return cx1000am_play_track(track) == ESP_OK;
}
