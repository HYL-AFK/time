#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "app_config.h"

esp_err_t ble_provision_init(const app_config_t *config);
esp_err_t ble_provision_start(void);
void ble_provision_finish(void);
bool ble_provision_is_active(void);
bool ble_provision_is_connected(void);
void ble_provision_report_wifi_connecting(void);
void ble_provision_report_wifi_connected(void);
void ble_provision_report_wifi_failed(void);
