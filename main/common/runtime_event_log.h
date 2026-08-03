#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    RUNTIME_EVENT_BOOT = 1,
    RUNTIME_EVENT_BLE_CONNECTED,
    RUNTIME_EVENT_BLE_DISCONNECTED,
    RUNTIME_EVENT_BLE_TIMEOUT,
    RUNTIME_EVENT_BLE_SECURITY_FAILURE,
    RUNTIME_EVENT_WIFI_CONNECTED,
    RUNTIME_EVENT_WIFI_FAILURE,
    RUNTIME_EVENT_NETWORK_FAILURE,
    RUNTIME_EVENT_LOCATION_FAILURE,
    RUNTIME_EVENT_SNTP_FAILURE,
    RUNTIME_EVENT_TIME_SYNCED,
    RUNTIME_EVENT_LED_FAILURE,
    RUNTIME_EVENT_LED_RECOVERED,
    RUNTIME_EVENT_BUTTON_LONG_PRESS,
    RUNTIME_EVENT_STORAGE_FAILURE,
} runtime_event_type_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t event_type;
    uint64_t timestamp;
    int32_t value1;
    int32_t value2;
    uint32_t flags;
    uint32_t sequence;
    uint32_t record_crc32;
} runtime_event_record_t;

esp_err_t runtime_event_log_init(void);
esp_err_t runtime_event_log_append(runtime_event_type_t type, int32_t value1, int32_t value2,
                                   uint32_t flags);
esp_err_t runtime_event_log_get(uint16_t offset_from_oldest, runtime_event_record_t *out);
uint16_t runtime_event_log_count(void);
void runtime_event_log_dump(void);
esp_err_t runtime_event_log_clear(void);
