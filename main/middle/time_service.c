#include "time_service.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "clock_display.h"
#include "ble_provision.h"
#include "runtime_event_log.h"
#include "wifi_manager.h"

#define LOCATION_RESPONSE_MAX 1536U
#define LOCATION_TIMEOUT_MS 3000U
#define LOCATION_PRIMARY_URL "https://ipwho.is/"
#define LOCATION_FALLBACK_URL "https://ipapi.co/json/"
#define INTERNET_CHECK_TIMEOUT_MS 2000U
#define INTERNET_CHECK_ATTEMPTS 2U
#define INTERNET_CHECK_RETRY_MS 300U
#define SNTP_TIMEOUT_US (8LL * 1000LL * 1000LL)

static const char *TAG = "time_service";

static app_config_t s_config;
static TaskHandle_t s_sync_task;
static TaskHandle_t s_daily_task;
static time_service_sync_finished_cb_t s_on_finished;
static volatile bool s_wifi_connected;
static volatile bool s_wifi_failed;
static bool s_sntp_initialized;
static bool s_time_valid;
static int32_t s_utc_offset_seconds;
static bool s_demo_time_active;
static int64_t s_demo_started_us;
static volatile uint32_t s_sync_generation;
static volatile uint32_t s_active_sync_generation;
static volatile bool s_sync_active;

static char s_location_response[LOCATION_RESPONSE_MAX];
static size_t s_location_length;
static bool s_location_overflow;

typedef enum {
    LOCATION_PROVIDER_IPWHO = 1,
    LOCATION_PROVIDER_IPAPI,
} location_provider_t;

static esp_err_t location_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }
    if (s_location_length + (size_t)event->data_len >= sizeof(s_location_response)) {
        s_location_overflow = true;
        return ESP_OK;
    }
    memcpy(s_location_response + s_location_length, event->data, (size_t)event->data_len);
    s_location_length += (size_t)event->data_len;
    s_location_response[s_location_length] = '\0';
    return ESP_OK;
}

static bool parse_ipapi_utc_offset(const cJSON *root, int32_t *out)
{
    const cJSON *offset = cJSON_GetObjectItem(root, "utc_offset");
    if (!cJSON_IsString(offset) || offset->valuestring == NULL) return false;

    const char *text = offset->valuestring;
    int sign = 1;
    if (*text == '+' || *text == '-') {
        sign = *text == '-' ? -1 : 1;
        text++;
    }
    if (strlen(text) != 4U || text[0] < '0' || text[0] > '9' || text[1] < '0' ||
        text[1] > '9' || text[2] < '0' || text[2] > '9' || text[3] < '0' ||
        text[3] > '9') {
        return false;
    }
    const int hours = (text[0] - '0') * 10 + text[1] - '0';
    const int minutes = (text[2] - '0') * 10 + text[3] - '0';
    const int32_t seconds = sign * (hours * 3600 + minutes * 60);
    if (hours > 14 || minutes > 59 || seconds < -43200 || seconds > 50400) return false;
    *out = seconds;
    return true;
}

static bool request_utc_offset_from_provider(const char *url, location_provider_t provider,
                                             int32_t *out, int32_t *out_error,
                                             int32_t *out_status)
{
    s_location_length = 0;
    s_location_overflow = false;
    s_location_response[0] = '\0';
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = location_event,
        .timeout_ms = LOCATION_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        *out_error = ESP_ERR_NO_MEM;
        *out_status = 0;
        ESP_LOGW(TAG, "IP timezone client allocation failed: provider=%d", provider);
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "ESPARK-ECLOCK/1.0");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    *out_error = err;
    *out_status = status;
    if (err != ESP_OK || status != 200 || s_location_overflow) {
        ESP_LOGW(TAG, "IP timezone failed: provider=%d err=%s status=%d overflow=%d", provider,
                 esp_err_to_name(err), status, s_location_overflow);
        return false;
    }

    cJSON *root = cJSON_Parse(s_location_response);
    cJSON *timezone = root == NULL ? NULL : cJSON_GetObjectItem(root, "timezone");
    cJSON *offset = timezone == NULL ? NULL : cJSON_GetObjectItem(timezone, "offset");
    const bool valid = provider == LOCATION_PROVIDER_IPWHO
                           ? cJSON_IsNumber(offset) && offset->valueint >= -43200 &&
                                 offset->valueint <= 50400
                           : parse_ipapi_utc_offset(root, out);
    if (valid) {
        if (provider == LOCATION_PROVIDER_IPWHO) *out = offset->valueint;
        ESP_LOGI(TAG, "IP timezone offset acquired: provider=%d offset=%ld seconds", provider,
                 (long)*out);
    } else {
        *out_error = ESP_FAIL;
        ESP_LOGW(TAG, "IP timezone response has no valid UTC offset: provider=%d", provider);
    }
    cJSON_Delete(root);
    return valid;
}

static bool request_utc_offset(int32_t *out, int32_t *out_error, int32_t *out_status,
                               uint32_t *out_flags)
{
    if (request_utc_offset_from_provider(LOCATION_PRIMARY_URL, LOCATION_PROVIDER_IPWHO, out,
                                         out_error, out_status)) {
        *out_flags = LOCATION_PROVIDER_IPWHO;
        return true;
    }
    if (request_utc_offset_from_provider(LOCATION_FALLBACK_URL, LOCATION_PROVIDER_IPAPI, out,
                                         out_error, out_status)) {
        *out_flags = LOCATION_PROVIDER_IPAPI;
        return true;
    }
    *out_flags = LOCATION_PROVIDER_IPAPI;
    return false;
}

static bool check_internet(int32_t *out_error, int32_t *out_status)
{
    for (unsigned attempt = 1; attempt <= INTERNET_CHECK_ATTEMPTS; ++attempt) {
        const esp_http_client_config_t config = {
            .url = "https://www.baidu.com/favicon.ico",
            .timeout_ms = INTERNET_CHECK_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) return false;

        const esp_err_t err = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        *out_error = err;
        *out_status = status;
        if (err == ESP_OK && status >= 200 && status < 400) {
            ESP_LOGI(TAG, "Internet HTTPS check passed");
            return true;
        }
        ESP_LOGW(TAG, "Internet HTTPS check attempt %u/%u failed: err=%s status=%d", attempt,
                 INTERNET_CHECK_ATTEMPTS, esp_err_to_name(err), status);
        if (attempt < INTERNET_CHECK_ATTEMPTS) vTaskDelay(pdMS_TO_TICKS(INTERNET_CHECK_RETRY_MS));
    }
    return false;
}

static esp_err_t start_sntp_session(void)
{
    // SNTP's completion semaphore is one-shot, so every synchronization needs a new session.
    if (s_sntp_initialized) esp_netif_sntp_deinit();
    s_sntp_initialized = false;
    const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "ntp.tencent.com"));
    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP session initialization failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SNTP started: ntp.aliyun.com, ntp.tencent.com, timeout=%d ms",
                 (int)(SNTP_TIMEOUT_US / 1000LL));
    }
    if (err != ESP_OK) return err;
    s_sntp_initialized = true;
    return ESP_OK;
}

static bool wait_for_sntp(int64_t deadline_us)
{
    if (esp_netif_sntp_sync_wait(0) == ESP_OK) return true;
    while (esp_timer_get_time() < deadline_us) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100)) == ESP_OK) return true;
    }
    ESP_LOGW(TAG, "SNTP timed out");
    return false;
}

static void apply_sync(int32_t utc_offset_seconds, bool persist_utc_offset)
{
    s_utc_offset_seconds = utc_offset_seconds;
    s_time_valid = true;
    const esp_err_t save_err = persist_utc_offset
                                   ? app_config_save_utc_offset(&s_config, utc_offset_seconds)
                                   : ESP_OK;
    (void)runtime_event_log_append(RUNTIME_EVENT_TIME_SYNCED, utc_offset_seconds,
                                   (int32_t)save_err, persist_utc_offset ? 1U : 0U);
    if (s_sntp_initialized) {
        esp_netif_sntp_deinit();
        s_sntp_initialized = false;
    }
    (void)wifi_manager_stop();
    clock_display_request_refresh();
    if (s_on_finished != NULL) s_on_finished();
}

static bool sync_was_cancelled(uint32_t generation)
{
    return s_sync_generation != generation;
}

static void sync_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_sync_active) continue;
        s_sync_active = true;
        const uint32_t generation = s_sync_generation;
        s_active_sync_generation = generation;
        if (!wifi_manager_has_credentials()) {
            (void)runtime_event_log_append(RUNTIME_EVENT_WIFI_FAILURE, ESP_ERR_INVALID_STATE, 0, 0);
            if (!sync_was_cancelled(generation)) clock_display_post_event(CLOCK_EVENT_SYNC_FAILED);
            s_sync_active = false;
            continue;
        }

        s_wifi_connected = false;
        s_wifi_failed = false;
        const int64_t connect_deadline_us = esp_timer_get_time() +
                                            (int64_t)clock_wifi_connect_timeout_ms() * 1000LL;
        if (wifi_manager_start() != ESP_OK) s_wifi_failed = true;
        while (!s_wifi_connected && !s_wifi_failed && !sync_was_cancelled(generation) &&
               esp_timer_get_time() < connect_deadline_us) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (sync_was_cancelled(generation)) {
            s_sync_active = false;
            continue;
        }
        if (!s_wifi_connected) {
            (void)wifi_manager_stop();
            ble_provision_report_wifi_failed();
            if (!s_wifi_failed) {
                (void)runtime_event_log_append(RUNTIME_EVENT_WIFI_FAILURE, ESP_ERR_TIMEOUT, 0,
                                               0);
            }
            clock_display_post_event(CLOCK_EVENT_SYNC_FAILED);
            s_sync_active = false;
            continue;
        }

        clock_display_post_event(CLOCK_EVENT_NETWORK_CHECKING);
        int32_t internet_error = ESP_OK;
        int32_t internet_status = 0;
        if (!check_internet(&internet_error, &internet_status)) {
            (void)wifi_manager_stop();
            (void)runtime_event_log_append(RUNTIME_EVENT_NETWORK_FAILURE, internet_error,
                                           internet_status, 0);
            clock_display_post_event(CLOCK_EVENT_SYNC_FAILED);
            s_sync_active = false;
            continue;
        }
        if (sync_was_cancelled(generation)) {
            s_sync_active = false;
            continue;
        }

        const int64_t sntp_deadline_us = esp_timer_get_time() + SNTP_TIMEOUT_US;
        const esp_err_t sntp_start_err = start_sntp_session();
        if (sntp_start_err != ESP_OK) {
            (void)wifi_manager_stop();
            (void)runtime_event_log_append(RUNTIME_EVENT_SNTP_FAILURE, sntp_start_err, 0, 0);
            clock_display_post_event(CLOCK_EVENT_SYNC_FAILED);
            s_sync_active = false;
            continue;
        }

        clock_display_post_event(CLOCK_EVENT_LOCATION_SYNCING);
        int32_t offset = 0;
        int32_t location_error = ESP_OK;
        int32_t location_status = 0;
        uint32_t location_provider = 0;
        const bool location_succeeded = request_utc_offset(&offset, &location_error,
                                                            &location_status, &location_provider);
        if (!location_succeeded) {
            (void)runtime_event_log_append(RUNTIME_EVENT_LOCATION_FAILURE, location_error,
                                           location_status, location_provider);
            ESP_LOGW(TAG, "IP timezone unavailable; using %s UTC offset",
                     s_config.has_utc_offset ? "saved" : "UTC+8 default");
        }
        offset = clock_select_utc_offset(s_config.has_utc_offset, s_config.utc_offset_seconds,
                                         location_succeeded, offset);
        if (sync_was_cancelled(generation)) {
            if (s_sntp_initialized) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
            }
            s_sync_active = false;
            continue;
        }

        clock_display_post_event(CLOCK_EVENT_SNTP_SYNCING);
        if (!wait_for_sntp(sntp_deadline_us)) {
            if (s_sntp_initialized) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
            }
            (void)wifi_manager_stop();
            (void)runtime_event_log_append(RUNTIME_EVENT_SNTP_FAILURE, ESP_ERR_TIMEOUT, 0, 0);
            clock_display_post_event(CLOCK_EVENT_SYNC_FAILED);
            s_sync_active = false;
            continue;
        }
        ESP_LOGI(TAG, "SNTP time acquired");
        if (sync_was_cancelled(generation)) {
            if (s_sntp_initialized) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
            }
            s_sync_active = false;
            continue;
        }

        clock_display_post_event(CLOCK_EVENT_TIME_SYNCED);
        apply_sync(offset, location_succeeded);
        s_sync_active = false;
    }
}

static void daily_task(void *arg)
{
    (void)arg;
    int last_day = -1;
    for (;;) {
        time_t raw = time(NULL) + s_utc_offset_seconds;
        struct tm local = {0};
        gmtime_r(&raw, &local);
        if (!s_demo_time_active && s_time_valid && local.tm_hour == 3 && local.tm_min == 0 &&
            local.tm_yday != last_day) {
            last_day = local.tm_yday;
            time_service_start_sync();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void on_wifi_connected(void)
{
    s_wifi_connected = true;
    ble_provision_report_wifi_connected();
    if (s_sync_active && s_active_sync_generation == s_sync_generation) {
        clock_display_post_event(CLOCK_EVENT_WIFI_CONNECTED);
    }
}

static void on_wifi_failed(void)
{
    s_wifi_failed = true;
}

esp_err_t time_service_init(const app_config_t *config, time_service_sync_finished_cb_t on_finished)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    s_config = *config;
    s_utc_offset_seconds = config->has_utc_offset ? config->utc_offset_seconds : 0;
    s_on_finished = on_finished;
    ESP_RETURN_ON_ERROR(wifi_manager_init(config, on_wifi_connected, on_wifi_failed), "time_service",
                        "Wi-Fi manager init");
    if (xTaskCreate(sync_task, "time_sync", 6144, NULL, 4, &s_sync_task) != pdPASS) return ESP_ERR_NO_MEM;
    return xTaskCreate(daily_task, "daily_sync", 3072, NULL, 3, &s_daily_task) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

void time_service_start_sync(void)
{
    if (!s_demo_time_active && s_sync_task != NULL) {
        clock_display_post_event(CLOCK_EVENT_WIFI_CONNECTING);
        xTaskNotifyGive(s_sync_task);
    }
}

void time_service_cancel_sync(void)
{
    ++s_sync_generation;
    s_wifi_connected = false;
    s_wifi_failed = true;
    (void)wifi_manager_stop();
}

void time_service_start_demo(void)
{
    s_demo_started_us = esp_timer_get_time();
    s_demo_time_active = true;
    s_time_valid = true;
    clock_display_request_refresh();
}

bool time_service_get_local(clock_time_t *out)
{
    if (out == NULL || !s_time_valid) return false;
    if (s_demo_time_active) {
        const int64_t elapsed_us = esp_timer_get_time() - s_demo_started_us;
        clock_time_from_elapsed_ms(elapsed_us > 0 ? (uint64_t)(elapsed_us / 1000LL) : 0U, out);
        return true;
    }
    struct timeval now = {0};
    gettimeofday(&now, NULL);
    const int64_t utc_epoch_ms = (int64_t)now.tv_sec * 1000LL + now.tv_usec / 1000LL;
    clock_time_from_unix_ms(utc_epoch_ms, s_utc_offset_seconds, out);
    return true;
}

bool time_service_has_valid_time(void)
{
    return s_time_valid;
}
