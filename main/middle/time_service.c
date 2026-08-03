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
#define INTERNET_CHECK_TIMEOUT_MS 2000U
#define INTERNET_CHECK_ATTEMPTS 2U
#define INTERNET_CHECK_RETRY_MS 300U
#define SNTP_TIMEOUT_US (5LL * 1000LL * 1000LL)

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

static bool request_utc_offset(int32_t *out)
{
    s_location_length = 0;
    s_location_overflow = false;
    s_location_response[0] = '\0';
    const esp_http_client_config_t config = {
        .url = "https://ipwho.is/",
        .event_handler = location_event,
        .timeout_ms = LOCATION_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "IP timezone client allocation failed");
        return false;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || s_location_overflow) {
        ESP_LOGW(TAG, "IP timezone failed: err=%s status=%d overflow=%d", esp_err_to_name(err),
                 status, s_location_overflow);
        return false;
    }

    cJSON *root = cJSON_Parse(s_location_response);
    cJSON *timezone = root == NULL ? NULL : cJSON_GetObjectItem(root, "timezone");
    cJSON *offset = timezone == NULL ? NULL : cJSON_GetObjectItem(timezone, "offset");
    const bool valid = cJSON_IsNumber(offset) && offset->valueint >= -43200 && offset->valueint <= 50400;
    if (valid) {
        *out = offset->valueint;
        ESP_LOGI(TAG, "IP timezone offset acquired: %ld seconds", (long)*out);
    } else {
        ESP_LOGW(TAG, "IP timezone response has no valid UTC offset");
    }
    cJSON_Delete(root);
    return valid;
}

static bool check_internet(void)
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

static bool wait_for_sntp(int64_t deadline_us)
{
    // SNTP's completion semaphore is one-shot, so every synchronization needs a new session.
    if (s_sntp_initialized) esp_netif_sntp_deinit();
    s_sntp_initialized = false;
    const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("time.cloudflare.com", "time.google.com"));
    if (esp_netif_sntp_init(&config) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP session initialization failed");
        return false;
    }
    s_sntp_initialized = true;

    while (esp_timer_get_time() < deadline_us) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100)) == ESP_OK) return true;
    }
    ESP_LOGW(TAG, "SNTP timed out");
    return false;
}

static void apply_sync(int32_t utc_offset_seconds)
{
    s_utc_offset_seconds = utc_offset_seconds;
    s_time_valid = true;
    const esp_err_t save_err = app_config_save_utc_offset(&s_config, utc_offset_seconds);
    (void)runtime_event_log_append(RUNTIME_EVENT_TIME_SYNCED, utc_offset_seconds,
                                   (int32_t)save_err, 0);
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
        if (!check_internet()) {
            (void)wifi_manager_stop();
            (void)runtime_event_log_append(RUNTIME_EVENT_NETWORK_FAILURE, 0, 0, 0);
            clock_display_post_event(CLOCK_EVENT_SYNC_FAILED);
            s_sync_active = false;
            continue;
        }
        if (sync_was_cancelled(generation)) {
            s_sync_active = false;
            continue;
        }

        clock_display_post_event(CLOCK_EVENT_LOCATION_SYNCING);
        int32_t offset = 0;
        if (!request_utc_offset(&offset)) {
            (void)wifi_manager_stop();
            (void)runtime_event_log_append(RUNTIME_EVENT_LOCATION_FAILURE, 0, 0, 0);
            clock_display_post_event(CLOCK_EVENT_SYNC_FAILED);
            s_sync_active = false;
            continue;
        }
        if (sync_was_cancelled(generation)) {
            s_sync_active = false;
            continue;
        }

        clock_display_post_event(CLOCK_EVENT_SNTP_SYNCING);
        const int64_t sntp_deadline_us = esp_timer_get_time() + SNTP_TIMEOUT_US;
        if (!wait_for_sntp(sntp_deadline_us)) {
            if (s_sntp_initialized) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
            }
            (void)wifi_manager_stop();
            (void)runtime_event_log_append(RUNTIME_EVENT_SNTP_FAILURE, 0, 0, 0);
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
        apply_sync(offset);
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
