#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_WIFI_SSID_MAX_LEN 32
#define APP_WIFI_PASS_MAX_LEN 64

typedef struct {
    char wifi_ssid[APP_WIFI_SSID_MAX_LEN + 1];
    char wifi_pass[APP_WIFI_PASS_MAX_LEN + 1];
    bool has_wifi;
    int32_t utc_offset_seconds;
    bool has_utc_offset;
} app_config_t;

esp_err_t app_config_init(app_config_t *out);
esp_err_t app_config_save_wifi(app_config_t *config, const char *ssid, const char *password);
esp_err_t app_config_save_utc_offset(app_config_t *config, int32_t seconds);
