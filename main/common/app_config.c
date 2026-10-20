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
/* 每个闹钟槽位一组键:alm_h0 / alm_m0 / alm_ep0 / alm_rep0 / alm_pd0,槽位 1 同理。
 * 旧版的单槽位键(alarm_h/alarm_m/alarm_ep)不再读 —— 它们没有"每天/一次性"
 * 这个标志,语义跟现在对不上,升级后按"没有闹钟"处理。 */
#define CONFIG_KEY_ALARM_HOUR "alm_h"
#define CONFIG_KEY_ALARM_MIN "alm_m"
#define CONFIG_KEY_ALARM_EPOCH "alm_ep"
#define CONFIG_KEY_ALARM_REPEAT "alm_rep"
#define CONFIG_KEY_ALARM_PERIOD "alm_pd"

static void load_string(nvs_handle_t handle, const char *key, char *out, size_t out_size)
{
    size_t length = out_size;
    if (nvs_get_str(handle, key, out, &length) != ESP_OK) out[0] = '\0';
}

/* 拼出"前缀+槽位号"的 NVS 键,例如 ("alm_h", 1) -> "alm_h1"。 */
static void alarm_key(char *out, size_t out_size, const char *prefix, uint8_t slot)
{
    snprintf(out, out_size, "%s%u", prefix, (unsigned)slot);
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
    /* 两个闹钟槽位独立读取:EPOCH / HOUR / MIN 三个键齐全且合法才算有效,
     * 任何一项缺失就把该槽位当作空的(不阻止另一个槽位)。 */
    for (uint8_t slot = 0; slot < APP_ALARM_SLOT_MAX; ++slot) {
        char key[16];
        int64_t epoch = 0;
        uint8_t hour = 0;
        uint8_t minute = 0;
        uint8_t period = 0;
        uint8_t repeat = 0;
        alarm_key(key, sizeof(key), CONFIG_KEY_ALARM_EPOCH, slot);
        if (nvs_get_i64(handle, key, &epoch) != ESP_OK || epoch <= 0) continue;
        alarm_key(key, sizeof(key), CONFIG_KEY_ALARM_HOUR, slot);
        if (nvs_get_u8(handle, key, &hour) != ESP_OK || hour > 23U) continue;
        alarm_key(key, sizeof(key), CONFIG_KEY_ALARM_MIN, slot);
        if (nvs_get_u8(handle, key, &minute) != ESP_OK || minute > 59U) continue;
        alarm_key(key, sizeof(key), CONFIG_KEY_ALARM_PERIOD, slot);
        if (nvs_get_u8(handle, key, &period) != ESP_OK || period > 4U) period = 0U;
        alarm_key(key, sizeof(key), CONFIG_KEY_ALARM_REPEAT, slot);
        (void)nvs_get_u8(handle, key, &repeat);
        out->alarms[slot].enabled = true;
        out->alarms[slot].repeat_daily = repeat != 0U;
        out->alarms[slot].hour = hour;
        out->alarms[slot].minute = minute;
        out->alarms[slot].period = period;
        out->alarms[slot].target_epoch = epoch;
    }
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

/* 把一个槽位的 5 个键一次写全;alarm->enabled=false 时改成全部抹掉
 * (键本来就不存在不算错,方便"取消闹钟"重复执行)。 */
static esp_err_t alarm_write_nvs(nvs_handle_t handle, uint8_t slot, const app_alarm_t *alarm)
{
    static const char *const prefixes[5] = {CONFIG_KEY_ALARM_EPOCH, CONFIG_KEY_ALARM_HOUR,
                                           CONFIG_KEY_ALARM_MIN, CONFIG_KEY_ALARM_REPEAT,
                                           CONFIG_KEY_ALARM_PERIOD};
    char key[16];
    esp_err_t err = ESP_OK;
    for (size_t i = 0U; i < 5U && err == ESP_OK; ++i) {
        alarm_key(key, sizeof(key), prefixes[i], slot);
        if (!alarm->enabled) {
            err = nvs_erase_key(handle, key);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
            continue;
        }
        switch (i) {
        case 0U: err = nvs_set_i64(handle, key, alarm->target_epoch); break;
        case 1U: err = nvs_set_u8(handle, key, alarm->hour); break;
        case 2U: err = nvs_set_u8(handle, key, alarm->minute); break;
        case 3U: err = nvs_set_u8(handle, key, alarm->repeat_daily ? 1U : 0U); break;
        default: err = nvs_set_u8(handle, key, alarm->period); break;
        }
    }
    return err;
}

esp_err_t app_config_save_alarm(app_config_t *config, uint8_t slot, bool enabled, bool repeat_daily,
                                uint8_t hour, uint8_t minute, uint8_t period, int64_t target_epoch)
{
    if (config == NULL || slot >= APP_ALARM_SLOT_MAX || hour > 23U || minute > 59U || period > 4U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 设置时必须带一个有效的到点时间戳,否则视为参数错误。 */
    if (enabled && target_epoch <= 0) return ESP_ERR_INVALID_ARG;

    app_alarm_t next = {0};
    next.enabled = enabled;
    next.repeat_daily = repeat_daily;
    next.hour = hour;
    next.minute = minute;
    next.period = period;
    next.target_epoch = target_epoch;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 3, 0);
        return err;
    }
    err = alarm_write_nvs(handle, slot, &next);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 3, 0);
        return err;
    }

    config->alarms[slot] = next;
    return ESP_OK;
}

esp_err_t app_config_clear_alarms(app_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;

    const app_alarm_t empty = {0};
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 3, 0);
        return err;
    }
    for (uint8_t slot = 0; slot < APP_ALARM_SLOT_MAX && err == ESP_OK; ++slot) {
        err = alarm_write_nvs(handle, slot, &empty);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        (void)runtime_event_log_append(RUNTIME_EVENT_STORAGE_FAILURE, (int32_t)err, 3, 0);
        return err;
    }

    for (uint8_t slot = 0; slot < APP_ALARM_SLOT_MAX; ++slot) config->alarms[slot] = empty;
    return ESP_OK;
}

uint8_t app_config_free_alarm_slot(const app_config_t *config)
{
    if (config == NULL) return APP_ALARM_SLOT_MAX;
    for (uint8_t slot = 0; slot < APP_ALARM_SLOT_MAX; ++slot) {
        if (!config->alarms[slot].enabled) return slot;
    }
    return APP_ALARM_SLOT_MAX;
}
