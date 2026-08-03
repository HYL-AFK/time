#include "app_config.h"

#include <string.h>

#include "nvs.h"

#include "runtime_event_log.h"

#define CONFIG_NAMESPACE "eclock"
#define CONFIG_KEY_SSID "ssid"
#define CONFIG_KEY_PASS "pass"
#define CONFIG_KEY_OFFSET "utc_offset"

static void load_string(nvs_handle_t handle, const char *key, char *out, size_t out_size)
{
    size_t length = out_size;
    if (nvs_get_str(handle, key, out, &length) != ESP_OK) out[0] = '\0';
}

esp_err_t app_config_init(app_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 0, 0);
        return err;
    }

    load_string(handle, CONFIG_KEY_SSID, out->wifi_ssid, sizeof(out->wifi_ssid));
    load_string(handle, CONFIG_KEY_PASS, out->wifi_pass, sizeof(out->wifi_pass));
    out->has_wifi = out->wifi_ssid[0] != '\0';
    out->has_utc_offset = nvs_get_i32(handle, CONFIG_KEY_OFFSET, &out->utc_offset_seconds) == ESP_OK;
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t app_config_save_wifi(app_config_t *config, const char *ssid, const char *password)
{
    if (config == NULL || ssid == NULL || password == NULL || ssid[0] == '\0' ||
        strlen(ssid) > APP_WIFI_SSID_MAX_LEN || strlen(password) > APP_WIFI_PASS_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 1, 0);
        return err;
    }
    err = nvs_set_str(handle, CONFIG_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, CONFIG_KEY_PASS, password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 1, 0);
        return err;
    }

    strlcpy(config->wifi_ssid, ssid, sizeof(config->wifi_ssid));
    strlcpy(config->wifi_pass, password, sizeof(config->wifi_pass));
    config->has_wifi = true;
    return ESP_OK;
}

esp_err_t app_config_save_utc_offset(app_config_t *config, int32_t seconds)
{
    if (config == NULL || seconds < -43200 || seconds > 50400) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 2, 0);
        return err;
    }
    err = nvs_set_i32(handle, CONFIG_KEY_OFFSET, seconds);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        config->utc_offset_seconds = seconds;
        config->has_utc_offset = true;
    }
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 2, 0);
    }
    return err;
}
