#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "app_config.h"

typedef void (*wifi_manager_connected_cb_t)(void);
typedef void (*wifi_manager_failed_cb_t)(void);

esp_err_t wifi_manager_init(const app_config_t *config,
                            wifi_manager_connected_cb_t on_connected,
                            wifi_manager_failed_cb_t on_failed);
bool wifi_manager_has_credentials(void);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_stop(void);
bool wifi_manager_is_connected(void);
