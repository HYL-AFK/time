#include "ble_provision.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_bt.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "store/config/ble_store_config.h"

#include "clock_display.h"
#include "blufi_security.h"
#include "runtime_event_log.h"
#include "time_service.h"
#include "wifi_manager.h"

#define BLE_WINDOW_US (3LL * 60LL * 1000LL * 1000LL)

static const char *TAG = "ble_provision";
static bool s_stack_started;
static bool s_active;
static bool s_connected;
static uint8_t s_address_type;
static char s_name[32];
static char s_ssid[APP_WIFI_SSID_MAX_LEN + 1];
static char s_password[APP_WIFI_PASS_MAX_LEN + 1];
static esp_timer_handle_t s_window_timer;
static int64_t s_window_deadline_us;
static void on_blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);
static esp_blufi_callbacks_t s_callbacks = {
    .event_cb = on_blufi_event,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

void ble_store_config_init(void);

static void start_advertising(void);

static void report_wifi_state(esp_blufi_sta_conn_state_t state)
{
    if (!s_connected) return;

    esp_blufi_extra_info_t info = {0};
    if (s_ssid[0] != '\0') {
        info.sta_ssid = (uint8_t *)s_ssid;
        info.sta_ssid_len = (int)strlen(s_ssid);
    }
    ESP_LOGI(TAG, "BluFi Wi-Fi report state=%d", state);
    const esp_err_t err = esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, state, 0, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BluFi Wi-Fi report failed: %s", esp_err_to_name(err));
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    return esp_blufi_handle_gap_events(event, arg);
}

static void stop_window(void)
{
    if (s_window_timer != NULL) (void)esp_timer_stop(s_window_timer);
}

static void start_advertising(void)
{
    if (!s_active || s_connected) return;
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields scan_response = {0};
    struct ble_gap_adv_params params = {0};
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_address_type) != 0) return;

    scan_response.name = (uint8_t *)s_name;
    scan_response.name_len = strlen(s_name);
    scan_response.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&scan_response) != 0) return;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = (ble_uuid16_t[]){BLE_UUID16_INIT(BLUFI_APP_UUID)};
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) return;

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(s_address_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL) == 0) {
        ESP_LOGI(TAG, "BluFi advertising as %s", s_name);
        clock_display_post_event(CLOCK_EVENT_BLE_ADVERTISING);
    }
}

static void window_expired(void *arg)
{
    (void)arg;
    if (!s_connected && s_active) {
        (void)esp_blufi_adv_stop();
        s_active = false;
        (void)runtime_event_log_append(RUNTIME_EVENT_BLE_TIMEOUT, 0, 0, 0);
        clock_display_post_event(CLOCK_EVENT_BLE_TIMEOUT);
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset: %d", reason);
}

static void on_sync(void)
{
    ble_svc_gap_device_name_set(s_name);
    esp_blufi_profile_init();
}

static void on_blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        if (s_active) start_advertising();
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        if (blufi_security_init() != ESP_OK) {
            ESP_LOGE(TAG, "BluFi security initialization failed");
            (void)runtime_event_log_append(RUNTIME_EVENT_BLE_SECURITY_FAILURE, 0, 0, 0);
            esp_blufi_disconnect();
            break;
        }
        s_connected = true;
        (void)runtime_event_log_append(RUNTIME_EVENT_BLE_CONNECTED, 0, 0, 0);
        time_service_cancel_sync();
        (void)esp_blufi_adv_stop();
        clock_display_post_event(CLOCK_EVENT_BLE_CONNECTED);
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        blufi_security_deinit();
        s_connected = false;
        (void)runtime_event_log_append(RUNTIME_EVENT_BLE_DISCONNECTED, 0, 0, 0);
        if (s_active && esp_timer_get_time() < s_window_deadline_us) {
            start_advertising();
        } else if (s_active) {
            s_active = false;
            (void)runtime_event_log_append(RUNTIME_EVENT_BLE_TIMEOUT, 0, 0, 0);
            clock_display_post_event(CLOCK_EVENT_BLE_TIMEOUT);
        }
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len > 0 && param->sta_ssid.ssid_len <= APP_WIFI_SSID_MAX_LEN) {
            memcpy(s_ssid, param->sta_ssid.ssid, (size_t)param->sta_ssid.ssid_len);
            s_ssid[param->sta_ssid.ssid_len] = '\0';
        }
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len >= 0 &&
            param->sta_passwd.passwd_len <= APP_WIFI_PASS_MAX_LEN) {
            memcpy(s_password, param->sta_passwd.passwd, (size_t)param->sta_passwd.passwd_len);
            s_password[param->sta_passwd.passwd_len] = '\0';
        }
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        if (wifi_manager_save_credentials(s_ssid, s_password) == ESP_OK) {
            ble_provision_report_wifi_connecting();
            time_service_cancel_sync();
            time_service_start_sync();
        } else {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
        }
        break;
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;
    default:
        break;
    }
}

static esp_err_t start_stack(void)
{
    if (s_stack_started) return ESP_OK;
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&controller_config), TAG, "controller init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "controller enable");
    ESP_RETURN_ON_ERROR(esp_nimble_init(), TAG, "NimBLE init");

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = 4;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    if (esp_blufi_gatt_svr_init() != 0) return ESP_FAIL;
    ble_store_config_init();
    esp_blufi_btc_init();
    ESP_RETURN_ON_ERROR(esp_blufi_register_callbacks(&s_callbacks), TAG, "BluFi callbacks");
    ESP_RETURN_ON_ERROR(esp_nimble_enable(host_task), TAG, "NimBLE host");
    s_stack_started = true;
    return ESP_OK;
}

esp_err_t ble_provision_init(const app_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    (void)config;
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BT), TAG, "read BT MAC");
    snprintf(s_name, sizeof(s_name), "ESPARK-ECLOCK-%02X%02X%02X", mac[3], mac[4], mac[5]);
    const esp_timer_create_args_t timer_args = {
        .callback = window_expired,
        .name = "ble_window",
    };
    return esp_timer_create(&timer_args, &s_window_timer);
}

esp_err_t ble_provision_start(void)
{
    s_active = true;
    s_ssid[0] = '\0';
    s_password[0] = '\0';
    ESP_RETURN_ON_ERROR(start_stack(), TAG, "BluFi stack");
    if (s_connected) {
        esp_blufi_disconnect();
    } else {
        (void)esp_blufi_adv_stop();
    }
    s_window_deadline_us = esp_timer_get_time() + BLE_WINDOW_US;
    if (s_window_timer != NULL) {
        (void)esp_timer_stop(s_window_timer);
        ESP_RETURN_ON_ERROR(esp_timer_start_once(s_window_timer, BLE_WINDOW_US), TAG, "BLE timer");
    }
    start_advertising();
    return ESP_OK;
}

void ble_provision_finish(void)
{
    stop_window();
    s_active = false;
    if (s_stack_started) {
        esp_blufi_adv_stop();
        if (s_connected) esp_blufi_disconnect();
    }
    blufi_security_deinit();
    s_connected = false;
}

bool ble_provision_is_active(void)
{
    return s_active;
}

bool ble_provision_is_connected(void)
{
    return s_connected;
}

void ble_provision_report_wifi_connecting(void)
{
    report_wifi_state(ESP_BLUFI_STA_CONNECTING);
}

void ble_provision_report_wifi_connected(void)
{
    report_wifi_state(ESP_BLUFI_STA_CONN_SUCCESS);
}

void ble_provision_report_wifi_failed(void)
{
    report_wifi_state(ESP_BLUFI_STA_CONN_FAIL);
}
