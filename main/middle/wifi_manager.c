#include "wifi_manager.h"

#include <string.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "runtime_event_log.h"

#define WIFI_MAX_ATTEMPTS 3U

static const char *TAG = "wifi_manager";
static app_config_t s_config;
static esp_netif_t *s_sta_netif;
static wifi_manager_connected_cb_t s_on_connected;
static wifi_manager_failed_cb_t s_on_failed;
static bool s_started;
static bool s_connected;
static uint8_t s_attempts;
static uint8_t s_candidates[APP_WIFI_PROFILE_MAX];
static uint8_t s_candidate_count;
static uint8_t s_candidate_index;
static bool s_ready_to_connect;

static esp_err_t configure_candidate(void)
{
    if (s_candidate_index >= s_candidate_count) return ESP_ERR_NOT_FOUND;
    wifi_config_t station = {0};
    const app_wifi_profile_t *profile = &s_config.wifi_profiles[s_candidates[s_candidate_index]];
    strlcpy((char *)station.sta.ssid, profile->ssid, sizeof(station.sta.ssid));
    strlcpy((char *)station.sta.password, profile->password, sizeof(station.sta.password));
    station.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    station.sta.pmf_cfg.capable = true;
    station.sta.pmf_cfg.required = false;
    /* 扫全部信道再选信号最好的那个。
     * 默认是 WIFI_FAST_SCAN：扫到第一个同名 AP 就停，同一个 SSID 开了多个
     * 热点/中继时可能连到很弱的那一个，甚至因为第一个候选落在 12/13 信道之外
     * 直接失败。“换了信道就连不上”就是这个默认值造成的。 */
    station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    station.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    return esp_wifi_set_config(WIFI_IF_STA, &station);
}

static void notify_failure(int32_t reason)
{
    s_started = false;
    s_connected = false;
    (void)runtime_event_log_append(RUNTIME_EVENT_WIFI_FAILURE, reason, s_attempts, 0);
    if (s_on_failed != NULL) s_on_failed();
}

static void handle_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_ready_to_connect) {
            const esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) notify_failure((int32_t)err);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && s_started) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        s_connected = false;
        s_attempts++;
        if (disconnected != NULL) {
            ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%u rssi=%d attempt=%u/%u",
                     disconnected->reason, disconnected->rssi, s_attempts, WIFI_MAX_ATTEMPTS);
        }
        if (s_attempts >= WIFI_MAX_ATTEMPTS) {
            if (++s_candidate_index < s_candidate_count && configure_candidate() == ESP_OK) {
                s_attempts = 0;
                ESP_LOGW(TAG, "Wi-Fi candidate failed; trying saved profile %u/%u",
                         s_candidate_index + 1U, s_candidate_count);
                (void)esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Wi-Fi failed after %u attempts", s_attempts);
                notify_failure(disconnected == NULL ? 0 : (int32_t)disconnected->reason);
            }
        } else {
            ESP_LOGI(TAG, "Wi-Fi retry %u/%u", s_attempts + 1U, WIFI_MAX_ATTEMPTS);
            (void)esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_attempts = 0;
        if (s_candidate_index < s_candidate_count) {
            app_config_mark_wifi_used(&s_config,
                                      s_config.wifi_profiles[s_candidates[s_candidate_index]].ssid);
        }
        (void)runtime_event_log_append(RUNTIME_EVENT_WIFI_CONNECTED, 0, 0, 0);
        if (s_on_connected != NULL) s_on_connected();
    }
}

esp_err_t wifi_manager_init(const app_config_t *config,
                            wifi_manager_connected_cb_t on_connected,
                            wifi_manager_failed_cb_t on_failed)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    s_config = *config;
    s_on_connected = on_connected;
    s_on_failed = on_failed;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) return ESP_ERR_NO_MEM;

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, handle_event, NULL, NULL),
                        TAG, "Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, handle_event, NULL, NULL),
                        TAG, "IP event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi RAM storage");
    /* 不设国家码时默认是 "01"（worldwide），只允许信道 1~11。
     * 路由器自动选到 12/13 信道时就完全扫不到 AP，表现为“同一个网络换个信道
     * 就连不上”。按中国区设成 CN，开放 1~13 信道。 */
    const esp_err_t country_err = esp_wifi_set_country_code("CN", true);
    if (country_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi country code setup failed: %s", esp_err_to_name(country_err));
    }
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

bool wifi_manager_has_credentials(void)
{
    return s_config.has_wifi;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    return app_config_save_wifi(&s_config, ssid, password);
}

esp_err_t wifi_manager_start(void)
{
    if (!s_config.has_wifi) return ESP_ERR_INVALID_STATE;
    if (s_started) return ESP_OK;

    s_candidate_count = 0;
    for (uint8_t i = 0; i < s_config.wifi_profile_count && i < APP_WIFI_PROFILE_MAX; ++i) {
        if (s_config.wifi_profiles[i].ssid[0] != '\0') {
            s_candidates[s_candidate_count++] = i;
        }
    }
    if (s_candidate_count == 0) {
        s_candidates[0] = 0;
        s_candidate_count = 1;
        strlcpy(s_config.wifi_profiles[0].ssid, s_config.wifi_ssid,
                sizeof(s_config.wifi_profiles[0].ssid));
        strlcpy(s_config.wifi_profiles[0].password, s_config.wifi_pass,
                sizeof(s_config.wifi_profiles[0].password));
    }
    s_candidate_index = 0;
    s_ready_to_connect = false;
    s_attempts = 0;
    s_started = true;
    esp_err_t err = configure_candidate();
    if (err != ESP_OK) {
        s_started = false;
        (void)runtime_event_log_append(RUNTIME_EVENT_WIFI_FAILURE, (int32_t)err, 0, 0);
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        s_started = false;
        (void)runtime_event_log_append(RUNTIME_EVENT_WIFI_FAILURE, (int32_t)err, 0, 0);
    } else {
        /* 关掉省电:BLE 共存时 MODEM sleep 会延迟收包,把 DHCP 拖到 3 秒以上。
         * 这是插电设备,多耗点电无所谓,换更快的关联和更稳的拿 IP。 */
        const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi power save setup failed: %s", esp_err_to_name(ps_err));
        }
        /* 这里原本有一次阻塞的全信道扫描,用来按 RSSI 给候选 SSID 排序。
         * 实测在 BLE 共存下要 6.9 秒,吃掉了 30 秒连接超时的近四分之一;
         * 而候选 profile 通常只有一两个,排序收益几乎为零 —— 直接连。
         * 连不上会自动重试,3 次失败后换下一个候选(见 handle_event)。 */
        if (configure_candidate() != ESP_OK) {
            notify_failure(ESP_FAIL);
            return ESP_FAIL;
        }
        s_ready_to_connect = true;
        (void)esp_wifi_connect();
    }
    return err;
}

esp_err_t wifi_manager_stop(void)
{
    s_started = false;
    s_connected = false;
    return esp_wifi_stop();
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}
