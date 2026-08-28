#include "app_config.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"

#include "runtime_event_log.h"

#define CONFIG_NAMESPACE "eclock"
#define CONFIG_KEY_SSID "ssid"
#define CONFIG_KEY_PASS "pass"
#define CONFIG_KEY_OFFSET "utc_offset"
#define CONFIG_KEY_PROFILE_COUNT "wifi_count"
#define CONFIG_KEY_USE_SEQUENCE "wifi_seq"

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
    (void)nvs_get_u8(handle, CONFIG_KEY_PROFILE_COUNT, &out->wifi_profile_count);
    nvs_get_u32(handle, CONFIG_KEY_USE_SEQUENCE, &out->wifi_use_sequence);
    if (out->wifi_profile_count > APP_WIFI_PROFILE_MAX) out->wifi_profile_count = 0;
    for (uint8_t i = 0; i < out->wifi_profile_count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "ssid%u", i);
        load_string(handle, key, out->wifi_profiles[i].ssid,
                    sizeof(out->wifi_profiles[i].ssid));
        snprintf(key, sizeof(key), "pass%u", i);
        load_string(handle, key, out->wifi_profiles[i].password,
                    sizeof(out->wifi_profiles[i].password));
        snprintf(key, sizeof(key), "use%u", i);
        (void)nvs_get_u32(handle, key, &out->wifi_profiles[i].last_used);
    }
    if (out->wifi_profile_count == 0 && out->has_wifi) {
        strlcpy(out->wifi_profiles[0].ssid, out->wifi_ssid, sizeof(out->wifi_profiles[0].ssid));
        strlcpy(out->wifi_profiles[0].password, out->wifi_pass,
                sizeof(out->wifi_profiles[0].password));
        out->wifi_profiles[0].last_used = 1U;
        out->wifi_profile_count = 1U;
        out->wifi_use_sequence = 1U;
    }
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
    uint8_t slot = config->wifi_profile_count;
    bool replacing_existing = false;
    for (uint8_t i = 0; i < config->wifi_profile_count; ++i) {
        if (strcmp(config->wifi_profiles[i].ssid, ssid) == 0) {
            slot = i;
            replacing_existing = true;
            break;
        }
    }
    if (slot >= APP_WIFI_PROFILE_MAX) {
        slot = 0;
        for (uint8_t i = 1; i < APP_WIFI_PROFILE_MAX; ++i) {
            if (config->wifi_profiles[i].last_used < config->wifi_profiles[slot].last_used) slot = i;
        }
    }
    err = nvs_set_str(handle, CONFIG_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, CONFIG_KEY_PASS, password);
    char key[16];
    if (err == ESP_OK) { snprintf(key, sizeof(key), "ssid%u", slot); err = nvs_set_str(handle, key, ssid); }
    if (err == ESP_OK) { snprintf(key, sizeof(key), "pass%u", slot); err = nvs_set_str(handle, key, password); }
    if (err == ESP_OK) { snprintf(key, sizeof(key), "use%u", slot); err = nvs_set_u32(handle, key, config->wifi_use_sequence + 1U); }
    if (err == ESP_OK) err = nvs_set_u8(handle, CONFIG_KEY_PROFILE_COUNT,
                                        config->wifi_profile_count < APP_WIFI_PROFILE_MAX
                                            ? (uint8_t)(config->wifi_profile_count + 1U)
                                            : APP_WIFI_PROFILE_MAX);
    if (err == ESP_OK) err = nvs_set_u32(handle, CONFIG_KEY_USE_SEQUENCE,
                                         config->wifi_use_sequence + 1U);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 1, 0);
        return err;
    }

    strlcpy(config->wifi_ssid, ssid, sizeof(config->wifi_ssid));
    strlcpy(config->wifi_pass, password, sizeof(config->wifi_pass));
    config->has_wifi = true;
    strlcpy(config->wifi_profiles[slot].ssid, ssid, sizeof(config->wifi_profiles[slot].ssid));
    strlcpy(config->wifi_profiles[slot].password, password,
            sizeof(config->wifi_profiles[slot].password));
    config->wifi_profiles[slot].last_used = ++config->wifi_use_sequence;
    if (!replacing_existing && config->wifi_profile_count < APP_WIFI_PROFILE_MAX) {
        config->wifi_profile_count++;
    }
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

void app_config_mark_wifi_used(app_config_t *config, const char *ssid)
{
    if (config == NULL || ssid == NULL) return;
    for (uint8_t i = 0; i < config->wifi_profile_count; ++i) {
        if (strcmp(config->wifi_profiles[i].ssid, ssid) != 0) continue;
        config->wifi_profiles[i].last_used = ++config->wifi_use_sequence;
        nvs_handle_t handle;
        if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            char key[16];
            snprintf(key, sizeof(key), "use%u", i);
            (void)nvs_set_u32(handle, key, config->wifi_profiles[i].last_used);
            (void)nvs_set_u32(handle, CONFIG_KEY_USE_SEQUENCE, config->wifi_use_sequence);
            (void)nvs_commit(handle);
            nvs_close(handle);
        }
        return;
    }
}
