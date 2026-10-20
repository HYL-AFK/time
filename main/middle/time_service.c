#include "time_service.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>
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
#include "nvs.h"

#include "clock_display.h"
#include "asrpro.h"
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

/* 时间无效期间允许的补试次数,每次补试由 daily_task 的 60 s 唤醒驱动。 */
/* 一次开机最多补试多少轮校时。原来只有 3 轮:Wi-Fi 连不上时 3 轮后永久放弃,
 * 界面就一直停在"同步中",看上去像死机。放到 8 轮,配合 daily_task 的周期唤醒,
 * 足够跨过路由器重启、信道抖动这类短时故障。 */
#define SYNC_RETRY_LIMIT 8U

/* 整点报时自测:校时成功后 N 秒先播一次,不用等到真正整点。
 * 功能已验收,改回 0 —— 否则每次上电/重启都会在 15 秒后再响一遍,
 * 叠在真正的整点报时上,听起来就是"一直在响"。
 * 以后再要验证语音链路,临时改回 15 即可。 */
#define HOURLY_SELFTEST_SECONDS 0

/* 闹钟铃声:TF 卡 101.mp3。 */
#define ASRPRO_ALARM_RING_INDEX 101U

/* 闹钟设置成功后的确认音序号;0 = 不播(ASRPRO 识别命令词后自己会回"好")。
 * 若要,往 TF 卡根目录放一条短提示音(如 100.mp3)并把序号填这里,
 * 避开 001~012(整点报时)与 101(闹钟铃声)。 */
#define ASRPRO_ALARM_ACK_INDEX 0U

/* ===== "当前时间"播报音片段 ID,必须与 ASRPRO 固件 setup() 里的命令词 ID 一一对应 =====
 * 数字用拼接播报:0~9 各一段 + "十"一段,即可拼出 0~99 任意数字,无需为每个数字录整句。
 * ASRPRO 侧用 prompt_play_by_cmd_id() 播放这些命令词的回复音。
 * 触发词的 ASRTO 已经播了"现在时间是",所以这里只需补"X点Y分"。 */
#define SPK_POINT 120U           /* 点 */
#define SPK_MINUTE 121U          /* 分 */
#define SPK_OCLOCK 122U          /* 整(整点用,不说"零分") */
#define SPK_DIGIT_BASE 123U      /* 123~132 = 零~九 */
#define SPK_TEN 133U             /* 十 */
#define SPK_NO_TIME 134U         /* 时间还没校准 */

/* ===== 闹钟确认句的拼接片段(与 ASRPRO 命令词 ID 一一对应) =====
 * 确认句由 ESP32 存好之后拼片段播,ASRPRO 不自己播 —— 该说哪句只有这边才知道
 * ("时间还没校准""闹钟满了要替换哪个"都是 ESP32 的状态)。
 * 拼法见 speak_alarm_confirm():日期 + 时段 + 时 + 点 + 分 + 分 + 结尾,
 * 例如"每天/下午/三点/五分/的时候都会叫你"。 */
#define SPK_ALARM_EVERY_DAY 75U   /* 每天 */
#define SPK_ALARM_TODAY 76U       /* 今天 */
#define SPK_ALARM_TOMORROW 77U    /* 明天 */
#define SPK_PERIOD_MORNING 78U    /* 早上 */
#define SPK_PERIOD_FORENOON 79U   /* 上午 */
#define SPK_PERIOD_AFTERNOON 80U  /* 下午 */
#define SPK_PERIOD_EVENING 81U    /* 晚上 */
#define SPK_PERIOD_DAWN 82U       /* 凌晨 */
#define SPK_ALARM_TAIL_DAILY 83U  /* 的时候都会叫你 */
#define SPK_ALARM_TAIL_ONCE 84U   /* 的时候叫你一次 */
#define SPK_ALARM_FULL 85U        /* 已经设了两个闹钟了 */
#define SPK_ALARM_SLOT1 86U       /* 第一个是 */
#define SPK_ALARM_SLOT2 87U       /* 第二个是 */
#define SPK_ALARM_WHICH 88U       /* 要替换哪一个？ */
/* ===== 闹钟响铃播报的片段(ID 与 ASRPRO 词表一一对应) =====
 * 响铃时一边放 TF 卡 101.mp3(板载 MP3,一直响到喊停),一边由 ASRPRO 播这句:
 *   问候 + 现在时间是 + X点Y分 + <城市>天气 + 现象 + 最高X度 + 最低Y度 + 记得按时起床哦
 * 问候语按时段换(早上好/上午好/下午好/晚上好,凌晨并入晚上),用现成的时段词 + "好"。
 * "现在时间是"和"好"都直接复用别的命令词的回复音,不占词表名额。
 * 原 162「今天已经过了，帮你改到明天」从未使用,已从词表删除腾出名额给 402。 */
#define SPK_TIME_NOW 30U          /* 现在时间是(复用"现在几点"的回复音) */
/* 「现在是」引导语。196 那条词表里一直没有,播报时 CHK valid=0、PLAY ret=1,
 * 整句就只剩「上午 十一点整」,听上去像少了一截。
 * 直接用 30:它的 ASRTO 就是「现在时间是」,现成有音频、CMDTBL valid=1,
 * 问"现在几点"走的也是它。比"现在是"多两个字,但语意更完整。 */
#define SPK_NOW SPK_TIME_NOW
#define SPK_GREET_GOOD 250U       /* 好(复用"大声点"的回复音) */
#define SPK_ALARM_WAKEUP 402U     /* 记得按时起床哦 */

/* 断电/离线期间错过的闹钟:超过这个秒数就当作陈旧记录丢弃,不补响 ——
 * 否则重启后校时成功那一刻,会把几天前错过的闹钟突然响出来。
 * daily_task 正常唤醒周期是 60 s,5 分钟余量足够区分"正常到点"与跨重启的旧记录。 */
#define ALARM_MAX_OVERDUE_SECONDS 300

/* 闹钟响铃后先让背景音乐单独响这么久,再开始语音播报 —— 免得人声被铃声盖住。
 * 用一次性定时器而不是 vTaskDelay:延迟期间用户可能喊"关闭闹钟",停铃走的是
 * ASRPRO 接收任务,不能因为 daily_task 在睡觉就跟着卡 5 秒。 */
#define ALARM_SPEAK_DELAY_MS 5000

/* ===== "今天天气"播报音片段 ID,必须与 ASRPRO 固件 setup() 里的命令词 ID 一一对应 =====
 * 天气现象按 WMO 代码细分到 20 种,一种现象一条独立音频,
 * 避免"晴间多云/多云/阴"被压成同一个词。偏移顺序见 weather_fragment()。 */
#define SPK_WEATHER_BASE 136U    /* 136~155 = 20 种天气现象 */
#define SPK_TEMP_MAX 156U        /* 最高 */
#define SPK_TEMP_MIN 157U        /* 最低 */
#define SPK_DEGREE 158U          /* 度 */
#define SPK_BELOW_ZERO 159U      /* 零下 */
#define SPK_NO_WEATHER 160U      /* 天气还没更新 */
#define SPK_TURN 161U            /* 转(用于"晴转多云") */

/* ===== 城市名片段:说哪个城市由 IP 定位结果决定 =====
 * 城市音频必须提前合成(ASRPRO 无 TTS),所以词表里预置了一批城市片段,
 * 运行时按 IP 返回的城市名查表选一个;查不到就用 165(只说"天气",不报地名)。
 * ID 与 ASRPRO 固件 setup() 里 165 / 166~195 一一对应,加城市要两边一起改
 * (但加**别名**只改这里的 k_city_fragments,不用重新生成模型)。
 * 城市从 70 个精简到 30 个 —— 语义词表要腾出位置给"X点Y分"的 144 条。 */
#define SPK_WEATHER_PLAIN 165U   /* 兜底:只说"天气" */

/* 天气数据源:Open-Meteo,免费且无需 API key;经纬度复用 IP 定位接口的返回值。
 * daily 取当天最高/最低温,hourly 取逐小时天气代码,用 9 点与 15 点拼"X转Y"。
 * 响应约 550 字节,可复用 location 的接收缓冲。 */
#define WEATHER_TIMEOUT_MS 4000U
#define WEATHER_URL_MAX 288U
#define WEATHER_URL_FORMAT                                                      \
    "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"           \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min"                 \
    "&hourly=weather_code&forecast_days=1&timezone=auto"

/* 调试开关:置 1 时忽略真实天气,固定播报下面这组值,用来验证平时遇不到的文案
 * (零下、晴转多云)。验证完改回 0 —— 量产必须为 0。 */
#define WEATHER_DEBUG_FORCE 0
#define WEATHER_DEBUG_CODE_AM 1  /* 1 = 晴间多云 */
#define WEATHER_DEBUG_CODE_PM 2  /* 2 = 多云 => 合起来念"晴间多云转多云" */
#define WEATHER_DEBUG_TMAX (-3)
#define WEATHER_DEBUG_TMIN (-8)

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
/* 时间无效期间已补试的次数,校时成功即清零。 */
static volatile uint32_t s_sync_retry_count;
/* 是否已真正发起过校时尝试。补试必须等首次尝试之后,否则开机时 daily_task
 * 会和 main 里的 boot_sync 同时排队,造成"刚校时成功又重新连一次"。 */
static volatile bool s_sync_attempted;
/* 整点报时自测:校时成功后 15 秒播一次,验证语音链路不用等整点。 */
static bool s_hourly_selftest_done;
static time_t s_selftest_base_epoch;
static esp_timer_handle_t s_alarm_speak_timer;
/* 闹钟:ASRPRO 回调只置标志,daily_task 负责写 NVS / 播铃 / 停铃。
 * 两个槽位;"每天"型到点响一次后往后滚 86400 继续响,"今天/明天"型响完即清。 */
static volatile bool s_alarm_set_pending;
static volatile bool s_alarm_clear_pending;
static volatile uint8_t s_alarm_pending_day;    /* 0=今天 1=明天 2=每天 */
static volatile uint8_t s_alarm_pending_period; /* 0=早上 1=上午 2=下午 3=晚上 4=凌晨 */
static volatile uint8_t s_alarm_pending_hour;   /* 0~23,时段已折算进去 */
static volatile uint8_t s_alarm_pending_minute; /* 0,5,10,…,55 */
/* 要写进哪个槽位;APP_ALARM_SLOT_MAX = 还没定(找空位,或两个都满要反问)。 */
static volatile uint8_t s_alarm_pending_slot = APP_ALARM_SLOT_MAX;
/* 两个槽位都占满时置起,等 ASRPRO 回 "ALARM PICK 1|2" 再继续写。
 * 并给一个等待截止时刻(esp_timer 微秒):播完"要替换哪一个"起算,超时自动作废 ——
 * 否则用户过十分钟随口说一句"第一个",会莫名其妙覆盖掉他已设好的闹钟。 */
static volatile bool s_alarm_pick_pending;
static volatile int64_t s_alarm_pick_until_us;

/* 等用户回答"要替换哪一个"的时长。播报本身最长约 6.3 s,留足反应时间。 */
#define ALARM_PICK_TIMEOUT_US (60LL * 1000000LL)

static char s_location_response[LOCATION_RESPONSE_MAX];

/* IP 定位接口顺带返回的经纬度(单位 0.001 度),供天气查询复用。 */
static bool s_has_location;
static int32_t s_latitude_e3;
static int32_t s_longitude_e3;

/* 经纬度落到 NVS。ipwho.is / ipapi.co 这两个 IP 定位服务经常连不上(实测两个
 * 都失败:ESP_ERR_HTTP_CONNECT),一旦那次失败就没有坐标,天气会一直跳过 ——
 * 重启也回不来,因为坐标只存在内存里。把最近一次成功的坐标存下来,开机先读出来兜底。
 * 用独立 namespace 和独立键,不动 app_config 的存储布局:老固件升级上来读不到,
 * 就等于"没有缓存",行为退化成和现在一样,不会出错。
 * 坐标按 0.001 度存成整数,避免依赖浮点 printf。 */
#define COORDS_NVS_NAMESPACE "time_svc"
#define COORDS_KEY_LAT "lat_e3"
#define COORDS_KEY_LON "lon_e3"

static void coords_save(void)
{
    if (s_latitude_e3 == 0 && s_longitude_e3 == 0) return;
    nvs_handle_t handle;
    if (nvs_open(COORDS_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_i32(handle, COORDS_KEY_LAT, s_latitude_e3) == ESP_OK &&
        nvs_set_i32(handle, COORDS_KEY_LON, s_longitude_e3) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

static void coords_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(COORDS_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    int32_t lat = 0;
    int32_t lon = 0;
    if (nvs_get_i32(handle, COORDS_KEY_LAT, &lat) == ESP_OK &&
        nvs_get_i32(handle, COORDS_KEY_LON, &lon) == ESP_OK && (lat != 0 || lon != 0)) {
        s_latitude_e3 = lat;
        s_longitude_e3 = lon;
        ESP_LOGI(TAG, "reused saved coordinates: lat_e3=%ld lon_e3=%ld", (long)lat, (long)lon);
    }
    nvs_close(handle);
}

/* IP 定位接口返回的城市名(通常是拼音),天气播报时用来选城市片段。 */
static char s_city[48];

/* 天气缓存:每次校时成功后抓一次,用户语音问天气时直接拼片段播报,不临时联网。
 * am/pm 是逐小时数据里 9 点与 15 点的天气代码,不同就播成"X转Y"。 */
static bool s_weather_valid;
static int s_weather_code;
static int s_weather_am;
static int s_weather_pm;
static int s_weather_tmax;
static int s_weather_tmin;
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
        /* 顺手取经纬度给天气查询用:ipwho.is 与 ipapi.co 都在根节点给 latitude/longitude。 */
        const cJSON *lat = cJSON_GetObjectItem(root, "latitude");
        const cJSON *lon = cJSON_GetObjectItem(root, "longitude");
        if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
            s_latitude_e3 = (int32_t)(lat->valuedouble * 1000.0);
            s_longitude_e3 = (int32_t)(lon->valuedouble * 1000.0);
            coords_save();   /* 立刻存一份,下次定位服务抽风时用来兜底 */
            s_has_location = true;
        }
        /* 城市名给天气播报选地名用(通常是拼音,例如 "Hangzhou")。 */
        const cJSON *city = cJSON_GetObjectItem(root, "city");
        if (cJSON_IsString(city) && city->valuestring != NULL) {
            snprintf(s_city, sizeof(s_city), "%s", city->valuestring);
        }
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
    s_sync_retry_count = 0;
    ESP_LOGI(TAG, "local time is valid; utc_offset=%ld seconds", (long)utc_offset_seconds);
    if (s_daily_task != NULL) xTaskNotifyGive(s_daily_task);
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

/* 城市名 -> 播报片段。key 是**归一化后**的名字(小写拼音或中文),
 * 由 normalize_city() 统一处理,所以别名可以放心多加 —— 加别名只重烧 ESP32,
 * 不需要重新生成 ASRPRO 模型。 */
typedef struct {
    const char *key;
    uint16_t fragment;
} city_fragment_t;

static const city_fragment_t k_city_fragments[] = {
    {"beijing", 166U},      {"北京", 166U},       /* 166 北京 */
    {"shanghai", 167U},     {"上海", 167U},       /* 167 上海 */
    {"tianjin", 168U},      {"天津", 168U},       /* 168 天津 */
    {"chongqing", 169U},    {"重庆", 169U},       /* 169 重庆 */
    {"taiyuan", 170U},      {"太原", 170U},       /* 170 太原 */
    {"shenyang", 171U},     {"沈阳", 171U},       /* 171 沈阳 */
    {"changchun", 172U},    {"长春", 172U},       /* 172 长春 */
    {"harbin", 173U},       {"haerbin", 173U},    {"哈尔滨", 173U},   /* 173 哈尔滨 */
    {"nanjing", 174U},      {"南京", 174U},       /* 174 南京 */
    {"hangzhou", 175U},     {"杭州", 175U},       /* 175 杭州 */
    {"hefei", 176U},        {"合肥", 176U},       /* 176 合肥 */
    {"fuzhou", 177U},       {"福州", 177U},       /* 177 福州 */
    {"nanchang", 178U},     {"南昌", 178U},       /* 178 南昌 */
    {"jinan", 179U},        {"济南", 179U},       /* 179 济南 */
    {"zhengzhou", 180U},    {"郑州", 180U},       /* 180 郑州 */
    {"wuhan", 181U},        {"武汉", 181U},       /* 181 武汉 */
    {"changsha", 182U},     {"长沙", 182U},       /* 182 长沙 */
    {"guangzhou", 183U},    {"广州", 183U},       /* 183 广州 */
    {"nanning", 184U},      {"南宁", 184U},       /* 184 南宁 */
    {"chengdu", 185U},      {"成都", 185U},       /* 185 成都 */
    {"guiyang", 186U},      {"贵阳", 186U},       /* 186 贵阳 */
    {"kunming", 187U},      {"昆明", 187U},       /* 187 昆明 */
    {"xian", 188U},         {"西安", 188U},       /* 188 西安 */
    {"urumqi", 189U},       {"wulumuqi", 189U},   {"乌鲁木齐", 189U}, /* 189 乌鲁木齐 */
    {"shenzhen", 190U},     {"深圳", 190U},       /* 190 深圳 */
    {"xiamen", 191U},       {"厦门", 191U},       /* 191 厦门 */
    {"ningbo", 192U},       {"宁波", 192U},       /* 192 宁波 */
    {"qingdao", 193U},      {"青岛", 193U},       /* 193 青岛 */
    {"dalian", 194U},       {"大连", 194U},       /* 194 大连 */
    {"suzhou", 195U},       {"苏州", 195U},       /* 195 苏州 */
};

/* 归一化:保留 ASCII 字母数字(转小写)与 UTF-8 字节(中文),丢掉空格/撇号/连字符等。
 * 这样 "Xi'an" 与 "xian"、"Hong Kong" 与 "hongkong" 都能对上。 */
static void normalize_city(const char *src, char *out, size_t size)
{
    size_t n = 0U;
    out[0] = '\0';
    if (src == NULL) return;
    for (const unsigned char *p = (const unsigned char *)src; *p != 0U && n + 1U < size; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            out[n++] = (char)(*p - 'A' + 'a');
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p >= 0x80U) {
            out[n++] = (char)*p;
        }
    }
    out[n] = '\0';
}

/* 城市名 -> 播报片段;查不到就退化成只说"天气"。 */
static uint16_t city_fragment(void)
{
    char key[64];
    normalize_city(s_city, key, sizeof(key));
    if (key[0] != '\0') {
        for (size_t i = 0U; i < sizeof(k_city_fragments) / sizeof(k_city_fragments[0]); i++) {
            if (strcmp(key, k_city_fragments[i].key) == 0) return k_city_fragments[i].fragment;
        }
    }
    ESP_LOGW(TAG, "city not matched: \"%s\" -> play \"天气\" only", s_city);
    return (uint16_t)SPK_WEATHER_PLAIN;
}

/* 把 0.001 度格式化成 "31.230" / "-12.456",供 Open-Meteo 的 URL 使用。 */
static void format_coord(char *out, size_t size, int32_t e3)
{
    const int32_t magnitude = e3 < 0 ? -e3 : e3;
    snprintf(out, size, "%s%ld.%03ld", e3 < 0 ? "-" : "", (long)(magnitude / 1000),
             (long)(magnitude % 1000));
}

/* WMO 天气代码 -> 播报片段;未知代码按"多云"处理。
 * 偏移必须与 ASRPRO 固件 setup() 里 ID 136~155 的排列顺序完全一致:
 *   0晴 1晴间多云 2多云 3阴 4有雾 5毛毛雨 6冻雨 7小雨 8中雨 9大雨
 *   10暴雨 11阵雨 12中阵雨 13阵雪 14强阵雪 15小雪 16中雪 17大雪
 *   18雷阵雨 19雷阵雨伴冰雹 */
static uint16_t weather_fragment(int code)
{
    uint8_t offset;
    switch (code) {
        case 0: offset = 0U; break;                    /* 晴 */
        case 1: offset = 1U; break;                    /* 晴间多云 */
        case 2: offset = 2U; break;                    /* 多云 */
        case 3: offset = 3U; break;                    /* 阴 */
        case 45:
        case 48: offset = 4U; break;                   /* 有雾 */
        case 51: offset = 5U; break;                   /* 毛毛雨 */
        case 56:
        case 57:
        case 66:
        case 67: offset = 6U; break;                   /* 冻雨 */
        case 53:
        case 61: offset = 7U; break;                   /* 小雨 */
        case 55:
        case 63: offset = 8U; break;                   /* 中雨 */
        case 65: offset = 9U; break;                   /* 大雨 */
        case 82: offset = 10U; break;                  /* 暴雨 */
        case 80: offset = 11U; break;                  /* 阵雨 */
        case 81: offset = 12U; break;                  /* 中阵雨 */
        case 85: offset = 13U; break;                  /* 阵雪 */
        case 86: offset = 14U; break;                  /* 强阵雪 */
        case 71:
        case 77: offset = 15U; break;                  /* 小雪 */
        case 73: offset = 16U; break;                  /* 中雪 */
        case 75: offset = 17U; break;                  /* 大雪 */
        case 95: offset = 18U; break;                  /* 雷阵雨 */
        case 96:
        case 99: offset = 19U; break;                  /* 雷阵雨伴冰雹 */
        default: offset = 2U; break;                   /* 未知 -> 多云 */
    }
    return (uint16_t)(SPK_WEATHER_BASE + offset);
}

/* 校时成功后(Wi-Fi 与互联网都确认可用时)抓一次当天天气缓存。
 * 用户问天气时直接播缓存,避免临时联网等 3~5 秒;抓失败则保留上一次的缓存。 */
static void weather_refresh(void)
{
    if (!s_has_location) {
        ESP_LOGW(TAG, "weather skipped: no latitude/longitude");
        return;
    }

    char lat[16];
    char lon[16];
    format_coord(lat, sizeof(lat), s_latitude_e3);
    format_coord(lon, sizeof(lon), s_longitude_e3);

    char url[WEATHER_URL_MAX];
    const int written = snprintf(url, sizeof(url), WEATHER_URL_FORMAT, lat, lon);
    if (written <= 0 || (size_t)written >= sizeof(url)) {
        ESP_LOGW(TAG, "weather url too long");
        return;
    }

    s_location_length = 0U;
    s_location_overflow = false;
    s_location_response[0] = '\0';
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = location_event,
        .timeout_ms = WEATHER_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "weather client allocation failed");
        return;
    }
    esp_http_client_set_header(client, "User-Agent", "ESPARK-ECLOCK/1.0");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || s_location_overflow) {
        ESP_LOGW(TAG, "weather fetch failed: err=%s status=%d overflow=%d", esp_err_to_name(err),
                 status, s_location_overflow);
        return;
    }

    cJSON *root = cJSON_Parse(s_location_response);
    cJSON *daily = root == NULL ? NULL : cJSON_GetObjectItem(root, "daily");
    cJSON *codes = daily == NULL ? NULL : cJSON_GetObjectItem(daily, "weather_code");
    cJSON *tmax = daily == NULL ? NULL : cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON *tmin = daily == NULL ? NULL : cJSON_GetObjectItem(daily, "temperature_2m_min");
    cJSON *code0 = cJSON_IsArray(codes) ? cJSON_GetArrayItem(codes, 0) : NULL;
    cJSON *max0 = cJSON_IsArray(tmax) ? cJSON_GetArrayItem(tmax, 0) : NULL;
    cJSON *min0 = cJSON_IsArray(tmin) ? cJSON_GetArrayItem(tmin, 0) : NULL;
    if (cJSON_IsNumber(code0) && cJSON_IsNumber(max0) && cJSON_IsNumber(min0)) {
        s_weather_code = code0->valueint;
        s_weather_am = code0->valueint; /* hourly 缺失时退化成只报当天总体 */
        s_weather_pm = code0->valueint;
        s_weather_tmax = (int)(max0->valuedouble >= 0.0 ? max0->valuedouble + 0.5 : max0->valuedouble - 0.5);
        s_weather_tmin = (int)(min0->valuedouble >= 0.0 ? min0->valuedouble + 0.5 : min0->valuedouble - 0.5);

        /* 逐小时代码在 timezone=auto 下索引就是当地钟点:9 点代表上午、15 点代表下午。 */
        cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
        cJSON *hcode = hourly == NULL ? NULL : cJSON_GetObjectItem(hourly, "weather_code");
        if (cJSON_IsArray(hcode) && cJSON_GetArraySize(hcode) >= 16) {
            cJSON *am = cJSON_GetArrayItem(hcode, 9);
            cJSON *pm = cJSON_GetArrayItem(hcode, 15);
            if (cJSON_IsNumber(am) && cJSON_IsNumber(pm)) {
                s_weather_am = am->valueint;
                s_weather_pm = pm->valueint;
            }
        }

        s_weather_valid = true;
        ESP_LOGI(TAG, "weather cached: code=%d am=%d pm=%d max=%d min=%d", s_weather_code,
                 s_weather_am, s_weather_pm, s_weather_tmax, s_weather_tmin);
    } else {
        ESP_LOGW(TAG, "weather response incomplete");
    }
    cJSON_Delete(root);
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
        s_sync_attempted = true;
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

        /* SNTP 的等待窗口从"定位结束"这一刻才开始算。
         * 原来 deadline 在 start_sntp_session() 之前就算好了,而中间夹着的
         * IP 定位最坏要 7 秒(两个 provider 各 3.5 s 超时) —— 它会把 8 秒的
         * SNTP 预算吃掉大半,导致校时偶尔超时失败。
         * 两者本来就是并行发起的,改完之后不再互相抢时间,SNTP 稳定拿到完整 8 秒。 */
        const int64_t sntp_deadline_us = esp_timer_get_time() + SNTP_TIMEOUT_US;
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

        /* Wi-Fi 与互联网此刻都已确认可用,顺手抓一份天气缓存(失败不影响校时)。 */
        weather_refresh();

        clock_display_post_event(CLOCK_EVENT_TIME_SYNCED);
        apply_sync(offset, location_succeeded);
        s_sync_active = false;
    }
}

/* Seconds from now until the next full hour (3600 s on the hour). */
static uint32_t seconds_until_hourly_report(const struct tm *local)
{
    const uint32_t minute_sec = (uint32_t)local->tm_min * 60U + (uint32_t)local->tm_sec;
    return minute_sec == 0U ? 3600U : 3600U - minute_sec;
}

/* 距离最近一个闹钟到点的秒数;两个槽位都没设时返回 UINT32_MAX。 */
static uint32_t seconds_until_alarm(void)
{
    const int64_t now = (int64_t)time(NULL);
    uint32_t best = UINT32_MAX;
    for (uint8_t slot = 0; slot < APP_ALARM_SLOT_MAX; ++slot) {
        const app_alarm_t *alarm = &s_config.alarms[slot];
        if (!alarm->enabled) continue;
        if (alarm->target_epoch <= now) return 1U; /* 已到点,尽快唤醒处理 */
        const int64_t delta = alarm->target_epoch - now;
        const uint32_t seconds = delta > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
        if (seconds < best) best = seconds;
    }
    return best;
}

/* 定义在 speak_current_time 附近,但 daily_task 里就要用,先声明。 */
static size_t append_number(uint16_t *out, size_t n, int value);
static size_t append_alarm_summary(uint16_t *out, size_t n, const app_alarm_t *alarm);
static void speak_alarm_confirm(const app_alarm_t *alarm);
static size_t append_weather_summary(uint16_t *out, size_t n);
static void speak_alarm_wakeup(void);
static void speak_hourly_report(int hour);

static void daily_task(void *arg)
{
    (void)arg;
    int last_day = -1;
    int last_report_day = -1;
    int last_report_hour = -1;
    uint32_t last_wait_seconds = UINT32_MAX;
    for (;;) {
        time_t raw = time(NULL) + s_utc_offset_seconds;
        struct tm local = {0};
        gmtime_r(&raw, &local);

        /* 等"要替换哪一个"超时了:及时作废,免得隔很久一句"第一个"突然生效。
         * 真正的把关在 time_service_on_alarm_pick() 里(那边才是关键路径),
         * 这里只是把标志清干净,让日志和状态一致。 */
        if (s_alarm_pick_pending && s_alarm_pick_until_us > 0 &&
            esp_timer_get_time() > s_alarm_pick_until_us) {
            s_alarm_pick_pending = false;
            s_alarm_pick_until_us = 0;
            ESP_LOGW(TAG, "alarm replace prompt expired");
        }

        /* 语音设置/取消闹钟:在这里落 NVS,避免占用 ASRPRO 接收任务。 */
        if (s_alarm_clear_pending) {
            s_alarm_clear_pending = false;
            s_alarm_pick_pending = false;
            s_alarm_pick_until_us = 0;
            s_alarm_pending_slot = APP_ALARM_SLOT_MAX;
            (void)app_config_clear_alarms(&s_config);
            (void)asrpro_stop();                       /* 正在响的闹铃立刻停 */
            (void)esp_timer_stop(s_alarm_speak_timer); /* 还没开口的播报也一起撤掉 */
            ESP_LOGI(TAG, "all alarms cleared by voice");
        }
        if (s_alarm_set_pending) {
            s_alarm_set_pending = false;
            const uint8_t day = s_alarm_pending_day;       /* 0=今天 1=明天 2=每天 */
            const uint8_t period = s_alarm_pending_period; /* 0=早上 … 4=凌晨 */
            const uint8_t hour = s_alarm_pending_hour;     /* 0~23,时段已折算 */
            const uint8_t minute = s_alarm_pending_minute; /* 0,5,…,55 */
            const bool repeat_daily = day == 2U;
            uint8_t slot = s_alarm_pending_slot;
            if (slot >= APP_ALARM_SLOT_MAX) slot = app_config_free_alarm_slot(&s_config);

            if (slot >= APP_ALARM_SLOT_MAX) {
                /* 两个槽位都占着:先把现有的两个念一遍,再问要替换哪一个。
                 * day/period/hour/minute 原样留着 —— PICK 回来会重新置
                 * set_pending 把这里再走一遍,那时 slot 已经有值了。 */
                /* 最长 22 段:已经设了两个闹钟了(1) + 第一个是(1) + 闹钟1(最多9)
                 * + 第二个是(1) + 闹钟2(最多9) + 要替换哪一个(1)。
                 * 单个闹钟最多 9 段 = 日期1 + 时段1 + 时2 + 点1 + 分3 + 分1。 */
                uint16_t ask[24U];
                size_t n = 0U;
                ask[n++] = (uint16_t)SPK_ALARM_FULL;
                ask[n++] = (uint16_t)SPK_ALARM_SLOT1;
                n = append_alarm_summary(ask, n, &s_config.alarms[0]);
                ask[n++] = (uint16_t)SPK_ALARM_SLOT2;
                n = append_alarm_summary(ask, n, &s_config.alarms[1]);
                ask[n++] = (uint16_t)SPK_ALARM_WHICH;
                (void)asrpro_say(ask, n);
                s_alarm_pick_pending = true;
                s_alarm_pick_until_us = esp_timer_get_time() + ALARM_PICK_TIMEOUT_US;
                ESP_LOGI(TAG, "both alarm slots busy; asking which one to replace");
            } else {
                /* 以"当前时刻 + 偏移"求到点时间戳,避开日历换算。
                 * 每天型按"今天"起算,过了就滚到明天,之后天天往后滚;
                 * 一次性型说"明天"就 +86400,说"今天"但已过则顺延一天 ——
                 * 后者属于"悄悄改期",必须明确告诉用户。 */
                const int day_offset = (day == 1U) ? 1 : 0;
                const int now_sec = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
                const int tgt_sec = (int)hour * 3600 + (int)minute * 60;
                int64_t delta = (int64_t)tgt_sec - (int64_t)now_sec + (int64_t)day_offset * 86400;
                bool pushed_to_tomorrow = false;
                if (delta <= 0) {
                    delta += 86400;
                    /* 每天型今天已过就直接从明天起算,这是它的正常语义,不算改期。 */
                    pushed_to_tomorrow = !repeat_daily;
                }
                const int64_t target_epoch = (int64_t)time(NULL) + delta;
                if (app_config_save_alarm(&s_config, slot, true, repeat_daily, hour, minute, period,
                                          target_epoch) == ESP_OK) {
                    ESP_LOGI(TAG, "alarm %u set to %02d:%02d (%s), in %lld s", (unsigned)slot,
                             (int)hour, (int)minute,
                             repeat_daily ? "daily"
                                          : (pushed_to_tomorrow ? "pushed-to-tomorrow"
                                                                : (day ? "tomorrow" : "today")),
                             (long long)delta);
                    /* 确认句会把完整日期 + 时间读回一遍:说了"今天"但那个点已经
                     * 过了会被顺延,读出来自然就是"明天",不用再单独补一句提示。 */
                    speak_alarm_confirm(&s_config.alarms[slot]);
                    if (ASRPRO_ALARM_ACK_INDEX > 0U) {
                        (void)asrpro_play((uint8_t)ASRPRO_ALARM_ACK_INDEX);
                    }
                } else {
                    ESP_LOGW(TAG, "alarm save failed");
                }
                s_alarm_pending_slot = APP_ALARM_SLOT_MAX; /* 用完复位,下次重新找空位 */
            }
        }

        /* 整点报时自测:校时成功后 15 秒先播一次,不用等到真正整点。
         * 只播一次;验证完把 HOURLY_SELFTEST_SECONDS 改成 0 即可关掉。 */
        if (HOURLY_SELFTEST_SECONDS > 0 && !s_hourly_selftest_done &&
            !s_demo_time_active && s_time_valid) {
            if (s_selftest_base_epoch == 0) s_selftest_base_epoch = time(NULL);
            if (time(NULL) - s_selftest_base_epoch >= (time_t)HOURLY_SELFTEST_SECONDS) {
                s_hourly_selftest_done = true;
                ESP_LOGI(TAG, "hourly self-test at %02d:%02d", local.tm_hour, local.tm_min);
                speak_hourly_report(local.tm_hour);
            }
        }

        // 每小时整点播报「现在是 X 点整」,由 ASRPRO 用现成语音片段拼。
        if (!s_demo_time_active && s_time_valid && local.tm_min == 0 &&
            (local.tm_yday != last_report_day || local.tm_hour != last_report_hour)) {
            last_report_day = local.tm_yday;
            last_report_hour = local.tm_hour;
            ESP_LOGI(TAG, "hourly report at %04d-%03d %02d:00",
                     local.tm_year + 1900, local.tm_yday + 1, local.tm_hour);
            speak_hourly_report(local.tm_hour);
        }
        /* 闹钟到点。每轮只处理一个到点的槽位(响铃是独占资源,同时响两个没意义)。
         *   每天型:响完把 target_epoch 往后滚 86400 直到落到将来,不清除 ——
         *           所以断电几天后重启也不会补响一串,滚动直接跳到未来那一刻。
         *   一次性:响完立即清除。断电期间错过的若超过 ALARM_MAX_OVERDUE_SECONDS
         *           就直接丢弃,避免重启校时成功那一刻突然补响几天前的闹钟。 */
        if (!s_demo_time_active && s_time_valid) {
            for (uint8_t slot = 0; slot < APP_ALARM_SLOT_MAX; ++slot) {
                const app_alarm_t *alarm = &s_config.alarms[slot];
                if (!alarm->enabled) continue;
                const int64_t now = (int64_t)time(NULL);
                const int64_t overdue = now - alarm->target_epoch;
                if (overdue < 0) continue;

                const uint8_t a_hour = alarm->hour;
                const uint8_t a_minute = alarm->minute;
                const uint8_t a_period = alarm->period;
                const bool a_daily = alarm->repeat_daily;

                if (overdue > (int64_t)ALARM_MAX_OVERDUE_SECONDS) {
                    /* 错过了(daily_task 正常 60 s 一轮,超过容忍窗就说明是断电/离线期间到点的):
                     *   一次性 -> 直接丢弃,不补响
                     *   每天型 -> 也不补响:滚到下一个将来时刻继续每天响
                     * 两者都不能在这里"响一次" —— 否则早上 9 点开机,
                     * 昨天早上 7 点的闹钟会突然响出来。 */
                    if (a_daily) {
                        int64_t next = alarm->target_epoch;
                        while (next <= now) next += 86400;
                        (void)app_config_save_alarm(&s_config, slot, true, true, a_hour, a_minute,
                                                    a_period, next);
                        ESP_LOGW(TAG, "missed daily alarm %u skipped; next in %lld s",
                                 (unsigned)slot, (long long)(next - now));
                    } else {
                        ESP_LOGW(TAG, "stale one-shot alarm %u dropped: %02d:%02d overdue %lld s",
                                 (unsigned)slot, (int)a_hour, (int)a_minute, (long long)overdue);
                        (void)app_config_save_alarm(&s_config, slot, false, false, 0U, 0U, 0U, 0);
                    }
                    continue;
                }

                ESP_LOGI(TAG, "alarm %u ringing at %02d:%02d (%s)", (unsigned)slot, (int)a_hour,
                         (int)a_minute, a_daily ? "daily" : "one-shot");
                /* 先起背景音乐(板载 MP3,一直响到用户喊停);语音播报延后
                 * ALARM_SPEAK_DELAY_MS,让铃声先单独响一会儿再开口。 */
                if (asrpro_play(ASRPRO_ALARM_RING_INDEX) != ESP_OK) {
                    ESP_LOGW(TAG, "alarm ringtone failed at %02d:%02d", (int)a_hour, (int)a_minute);
                }
                (void)esp_timer_stop(s_alarm_speak_timer);
                (void)esp_timer_start_once(s_alarm_speak_timer,
                                           (uint64_t)ALARM_SPEAK_DELAY_MS * 1000ULL);
                if (a_daily) {
                    /* 滚到第一个"还没到"的时刻,中间错过的那些不补响。 */
                    int64_t next = alarm->target_epoch;
                    while (next <= now) next += 86400;
                    (void)app_config_save_alarm(&s_config, slot, true, true, a_hour, a_minute,
                                                a_period, next);
                    ESP_LOGI(TAG, "daily alarm %u rolled forward", (unsigned)slot);
                } else {
                    (void)app_config_save_alarm(&s_config, slot, false, false, 0U, 0U, 0U, 0);
                    ESP_LOGI(TAG, "one-shot alarm %u cleared after ringing", (unsigned)slot);
                }
                break; /* 一轮只响一个,其它到点的槽位下一轮再说 */
            }
        }
        if (!s_demo_time_active && s_time_valid && local.tm_hour == 3 && local.tm_min == 0 &&
            local.tm_yday != last_day) {
            last_day = local.tm_yday;
            time_service_start_sync();
        }
        /* 开机自动校时若因首次 WiFi 关联超时而失败,原逻辑会永久挂起,时钟停在
         * valid=0 直到手动配网。这里在时间无效期间按 60 s 节奏补试有限次数。
         * 必须等首次尝试真正发起过(s_sync_attempted)再补试,否则开机时 daily_task
         * 会和 main 里的 boot_sync 同时排队,导致"刚校时成功又重新连一次"。 */
        if (!s_demo_time_active && !s_time_valid && !s_sync_active && s_sync_attempted &&
            wifi_manager_has_credentials() && s_sync_retry_count < SYNC_RETRY_LIMIT) {
            ++s_sync_retry_count;
            ESP_LOGW(TAG, "time not synced; auto-resync %u/%u", (unsigned)s_sync_retry_count,
                     SYNC_RETRY_LIMIT);
            time_service_start_sync();
        }
        uint32_t wait_seconds = 60U;
        if (!s_demo_time_active && s_time_valid) {
            wait_seconds = seconds_until_hourly_report(&local);
            const uint32_t alarm_wait = seconds_until_alarm();
            if (alarm_wait < wait_seconds) wait_seconds = alarm_wait;

            /* 自测还没跑过时必须保持短周期（HOURLY_SELFTEST_SECONDS）一轮。
             * 否则上面会直接睡到下一个整点 —— 10:33 上电要等到 11:00 才醒，
             * 15 秒自测就永远等不到。自测播完 s_hourly_selftest_done 置位，
             * 这里自动恢复成"睡到整点"。 */
            if (HOURLY_SELFTEST_SECONDS > 0 && !s_hourly_selftest_done &&
                wait_seconds > (uint32_t)HOURLY_SELFTEST_SECONDS) {
                wait_seconds = (uint32_t)HOURLY_SELFTEST_SECONDS;
            }
        }
        if (wait_seconds == 0U) wait_seconds = 1U;
        if (wait_seconds != last_wait_seconds) {
            ESP_LOGI(TAG, "daily task wait: local=%02d:%02d:%02d valid=%d next_event_in=%u s",
                     local.tm_hour, local.tm_min, local.tm_sec, s_time_valid,
                     (unsigned)wait_seconds);
            last_wait_seconds = wait_seconds;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_seconds * 1000U));
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

/* 把 0~99 追加成播报片段:25 -> 二十五,15 -> 十五,5 -> 五,0 -> 零。 */
static size_t append_number(uint16_t *out, size_t n, int value)
{
    if (value < 0) value = 0;
    if (value > 99) value = 99;
    const int tens = value / 10;
    const int units = value % 10;
    if (tens == 0) {
        out[n++] = (uint16_t)(SPK_DIGIT_BASE + (uint16_t)units);
    } else if (tens == 1) {
        out[n++] = (uint16_t)SPK_TEN;
        if (units != 0) out[n++] = (uint16_t)(SPK_DIGIT_BASE + (uint16_t)units);
    } else {
        out[n++] = (uint16_t)(SPK_DIGIT_BASE + (uint16_t)tens);
        out[n++] = (uint16_t)SPK_TEN;
        if (units != 0) out[n++] = (uint16_t)(SPK_DIGIT_BASE + (uint16_t)units);
    }
    return n;
}

/* 小时 -> 时段片段(12 小时制下区分上午 9 点和晚上 9 点)。
 * 边界与 link_probe.c 的 period_fragment() 保持一致。 */
static uint16_t period_fragment_of(int hour)
{
    if (hour < 6)  return (uint16_t)SPK_PERIOD_DAWN;
    if (hour < 9)  return (uint16_t)SPK_PERIOD_MORNING;
    if (hour < 12) return (uint16_t)SPK_PERIOD_FORENOON;
    if (hour < 18) return (uint16_t)SPK_PERIOD_AFTERNOON;
    return (uint16_t)SPK_PERIOD_EVENING;
}

/* 语音问"现在几点":现在是 + 时段 + X点Y分(12 小时制)。 */
static void speak_current_time(void)
{
    uint16_t ids[ASRPRO_SAY_MAX];
    size_t n = 0U;

    if (!s_time_valid) {
        ids[n++] = (uint16_t)SPK_NO_TIME;
        (void)asrpro_say(ids, n);
        return;
    }

    const time_t raw = time(NULL) + s_utc_offset_seconds;
    struct tm local = {0};
    gmtime_r(&raw, &local);

    ids[n++] = (uint16_t)SPK_NOW;                      /* 现在是(未配置时静音跳过) */
    ids[n++] = period_fragment_of(local.tm_hour);      /* 上午 / 下午 / 晚上 */
    const int hour12 = (local.tm_hour % 12 == 0) ? 12 : (local.tm_hour % 12);
    n = append_number(ids, n, hour12);
    ids[n++] = (uint16_t)SPK_POINT;
    if (local.tm_min == 0) {
        ids[n++] = (uint16_t)SPK_OCLOCK;
    } else {
        /* 分钟不足 10 要补"零":十点零五分,不能说成"十点五分"。 */
        if (local.tm_min < 10) ids[n++] = (uint16_t)SPK_DIGIT_BASE;
        n = append_number(ids, n, local.tm_min);
        ids[n++] = (uint16_t)SPK_MINUTE;
    }
    ESP_LOGI(TAG, "speak time %02d:%02d (%u segments)", local.tm_hour, local.tm_min, (unsigned)n);
    (void)asrpro_say(ids, n);
}

/* 问候语用的时段词:早上(5~8)/上午(9~11)/下午(12~16)/晚上(其余)。
 * 凌晨并进"晚上好"—— 「凌晨好」太机械。与闹钟唤醒播报(speak_alarm_wakeup)
 * 的分档保持一致;词表里没有"中午"这个片段,12 点用"下午好"。 */
static uint16_t greeting_period_of(int hour)
{
    if (hour >= 5 && hour <= 8)  return (uint16_t)SPK_PERIOD_MORNING;
    if (hour >= 9 && hour <= 11) return (uint16_t)SPK_PERIOD_FORENOON;
    if (hour >= 12 && hour <= 16) return (uint16_t)SPK_PERIOD_AFTERNOON;
    return (uint16_t)SPK_PERIOD_EVENING;
}

/* 整点报时:早上好 + 现在是 X点整 + 今天天气。
 * 由 ESP32 主动下发，走 asrpro_announce() -> T_ANNOUNCE ——
 * ASRPRO 收到后只切命令表、不碰状态机，播完不会冒一句"我退下了"。 */
static void speak_hourly_report(int hour)
{
    uint16_t ids[ASRPRO_SAY_MAX];
    size_t n = 0U;

    /* 问候「X上好」= 时段词 + 250「好」(复用"大声点"那条的回复音)。 */
    ids[n++] = greeting_period_of(hour);
    ids[n++] = (uint16_t)SPK_GREET_GOOD;

    /* 现在是 X 点整。196 未配置时此段静音跳过,后面的照旧播。 */
    ids[n++] = (uint16_t)SPK_NOW;
    const int hour12 = (hour % 12 == 0) ? 12 : (hour % 12);
    n = append_number(ids, n, hour12);
    ids[n++] = (uint16_t)SPK_POINT;
    ids[n++] = (uint16_t)SPK_OCLOCK;

    /* 今天天气。天气还没缓存就跳过这一段,问候和时间照旧。 */
    if (s_weather_valid || WEATHER_DEBUG_FORCE) {
        ids[n++] = (uint16_t)SPK_ALARM_TODAY;
        n = append_weather_summary(ids, n);
    }

    ESP_LOGI(TAG, "hourly report %02d:00 (%u segments, weather=%d)", hour, (unsigned)n,
             (int)s_weather_valid);
    (void)asrpro_announce(ids, n);
}

/* 一次性闹钟的到点时刻是不是落在"明天"—— 决定念"今天"还是"明天"。
 * 判据:target 是否 >= "今天 24:00:00"。
 * 注意必须用 >= :target 正好是明天 00:00:00 时,它减 now 恰好等于今天剩余秒数,
 * 用 > 会把它错判成"今天",确认句就会念成「今天…十二点整」。 */
static bool alarm_targets_tomorrow(const app_alarm_t *alarm)
{
    const time_t raw = time(NULL) + s_utc_offset_seconds;
    struct tm local = {0};
    gmtime_r(&raw, &local);
    const int64_t seconds_left_today =
        86400 - (int64_t)(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
    const int64_t now = (int64_t)time(NULL);
    return alarm->target_epoch - now >= seconds_left_today;
}

/* 把一个闹钟念成"日期 + 时段 + 时 + 点 + 分 + 分"的片段序列(不含结尾)。
 *   每天型    -> "每天" + 时段 + …
 *   一次性型  -> 按到点时刻落在今天还是明天念"今天/明天"
 * 用户说"今天"但那个点已经过了会被顺延,这里自然就读成"明天",
 * 所以不用再额外补一句"今天已经过了,帮你改到明天"。
 * 小时用 12 小时制(用户说的就是 1~12 点);分钟 0 分说"整"。 */
static size_t append_alarm_summary(uint16_t *out, size_t n, const app_alarm_t *alarm)
{
    if (alarm->repeat_daily) {
        out[n++] = (uint16_t)SPK_ALARM_EVERY_DAY;
    } else {
        out[n++] = alarm_targets_tomorrow(alarm) ? (uint16_t)SPK_ALARM_TOMORROW
                                                 : (uint16_t)SPK_ALARM_TODAY;
    }
    switch (alarm->period) {
    case 0U: out[n++] = (uint16_t)SPK_PERIOD_MORNING; break;
    case 1U: out[n++] = (uint16_t)SPK_PERIOD_FORENOON; break;
    case 2U: out[n++] = (uint16_t)SPK_PERIOD_AFTERNOON; break;
    case 3U: out[n++] = (uint16_t)SPK_PERIOD_EVENING; break;
    default: out[n++] = (uint16_t)SPK_PERIOD_DAWN; break;
    }
    const int hour12 = (alarm->hour % 12 == 0) ? 12 : (alarm->hour % 12);
    n = append_number(out, n, hour12);
    out[n++] = (uint16_t)SPK_POINT;
    if (alarm->minute == 0U) {
        out[n++] = (uint16_t)SPK_OCLOCK;
    } else {
        /* 闹钟分钟一定是 5 的倍数,直接读"五分/十五分/五十五分",不补"零"。 */
        n = append_number(out, n, (int)alarm->minute);
        out[n++] = (uint16_t)SPK_MINUTE;
    }
    return n;
}

/* 设置成功后的确认句 = append_alarm_summary + 结尾,
 * 例如"每天 / 下午 / 三点 / 五分 / 的时候都会叫你"。
 * 把完整时间读回一遍是有意为之:语音侧是 5 分钟挡位,用户说"三点十三分"可能被
 * 约束解码匹配到最近的挡位,读回来他才发现不对、能马上说"取消闹钟"重设。
 * 段数上限 9(日期1 + 时段1 + 时2 + 点1 + 分2 + 分1 + 结尾1),远小于 ASRPRO_SAY_MAX。 */
static void speak_alarm_confirm(const app_alarm_t *alarm)
{
    uint16_t ids[ASRPRO_SAY_MAX];
    size_t n = 0U;
    n = append_alarm_summary(ids, n, alarm);
    ids[n++] = alarm->repeat_daily ? (uint16_t)SPK_ALARM_TAIL_DAILY
                                   : (uint16_t)SPK_ALARM_TAIL_ONCE;
    ESP_LOGI(TAG, "confirm alarm: %s %02d:%02d period=%u (%u segments)",
             alarm->repeat_daily ? "daily" : "once", (int)alarm->hour, (int)alarm->minute,
             (unsigned)alarm->period, (unsigned)n);
    (void)asrpro_say(ids, n);
}

/* 温度片段:负数先说"零下";0~99 复用数字拼接。 */
static size_t append_temperature(uint16_t *out, size_t n, int value)
{
    if (value < 0) {
        out[n++] = (uint16_t)SPK_BELOW_ZERO;
        value = -value;
    }
    if (value > 99) value = 99;
    return append_number(out, n, value);
}

/* 把天气片段追加进 out,返回新的长度。
 * 内容:[<城市>天气] + [上午现象 (转 下午现象)] + 最高 X 度 + 最低 Y 度;
 * 上午与下午代码相同时只念一个,不啰嗦。
 * 段数上限 14(城市1 + 现象3 + 最高1 + 温度3 + 度1 + 最低1 + 温度3 + 度1)。
 * 单独抽出来是因为闹钟响铃播报(speak_alarm_wakeup)也要用同一套文案。 */
static size_t append_weather_summary(uint16_t *out, size_t n)
{
    int code_am = s_weather_am;
    int code_pm = s_weather_pm;
    int tmax = s_weather_tmax;
    int tmin = s_weather_tmin;
#if WEATHER_DEBUG_FORCE
    /* 调试:忽略真实缓存,固定播报一组值,便于验证"零下""X转Y"这类文案。 */
    code_am = WEATHER_DEBUG_CODE_AM;
    code_pm = WEATHER_DEBUG_CODE_PM;
    tmax = WEATHER_DEBUG_TMAX;
    tmin = WEATHER_DEBUG_TMIN;
#endif

    /* 地名片段:"今天"由触发词的 ASRTO 播过,这里接"<城市>天气";没匹配到城市就只说"天气"。 */
    out[n++] = city_fragment();

    const uint16_t am = weather_fragment(code_am);
    const uint16_t pm = weather_fragment(code_pm);
    out[n++] = am;
    if (am != pm) {
        out[n++] = (uint16_t)SPK_TURN;
        out[n++] = pm;
    }
    out[n++] = (uint16_t)SPK_TEMP_MAX;
    n = append_temperature(out, n, tmax);
    out[n++] = (uint16_t)SPK_DEGREE;
    out[n++] = (uint16_t)SPK_TEMP_MIN;
    n = append_temperature(out, n, tmin);
    out[n++] = (uint16_t)SPK_DEGREE;
    return n;
}

/* 语音问"今天天气":校时成功时已经抓过缓存,这里直接拼片段播报,不临时联网。
 * 触发词的 ASRTO 已经播了"今天",这里只补天气那一段。 */
static void speak_current_weather(void)
{
    uint16_t ids[ASRPRO_SAY_MAX];
    size_t n = 0U;

#if !WEATHER_DEBUG_FORCE
    if (!s_weather_valid) {
        ids[n++] = (uint16_t)SPK_NO_WEATHER;
        (void)asrpro_say(ids, n);
        return;
    }
#endif

    /* 「今天」由这里补上 —— 以前靠触发词的 ASRTO 播，实测那条回复音是空的
     * (CMDTBL valid=1 但播放无声)，拼进序列由播放器按序念，不依赖 ASRTO。 */
    ids[n++] = (uint16_t)SPK_ALARM_TODAY;
    n = append_weather_summary(ids, n);

    ESP_LOGI(TAG, "speak weather: city=\"%s\" am=%d pm=%d max=%d min=%d (%u segments)", s_city,
             s_weather_am, s_weather_pm, s_weather_tmax, s_weather_tmin, (unsigned)n);
    (void)asrpro_say(ids, n);
}

/* 对外入口：收到 ASK_WEATHER 帧时调用，直接复用上面的拼装逻辑。 */
void time_service_speak_weather(void)
{
    speak_current_weather();
}

/* 闹钟响铃播报:问候 + 现在时间是 X点Y分 + 天气 + 记得按时起床哦。
 * 背景音乐由调用方先起(板载 MP3 那条路),这里只管说话,两条路互不阻塞。
 * 天气还没缓存就跳过那一段,整句照样完整;段数上限 25,SAY_MAX 取 28 留了余量。 */
static void speak_alarm_wakeup(void)
{
    uint16_t ids[ASRPRO_SAY_MAX];
    size_t n = 0U;

    const time_t raw = time(NULL) + s_utc_offset_seconds;
    struct tm local = {0};
    gmtime_r(&raw, &local);

    /* 问候语按时段换:早上(5~8)/上午(9~11)/下午(12~16)/晚上(17~23 和 0~4 凌晨)。
     * 凌晨并进"晚上好"—— "凌晨好"太机械。复用现成的时段词 + "好",不占词表名额。 */
    const int h = local.tm_hour;
    if (h >= 5 && h <= 8) {
        ids[n++] = (uint16_t)SPK_PERIOD_MORNING;
    } else if (h >= 9 && h <= 11) {
        ids[n++] = (uint16_t)SPK_PERIOD_FORENOON;
    } else if (h >= 12 && h <= 16) {
        ids[n++] = (uint16_t)SPK_PERIOD_AFTERNOON;
    } else {
        ids[n++] = (uint16_t)SPK_PERIOD_EVENING;
    }
    ids[n++] = (uint16_t)SPK_GREET_GOOD;

    /* 现在时间是 X点Y分 —— 报的是"此刻",不是闹钟设定的时刻。 */
    ids[n++] = (uint16_t)SPK_TIME_NOW;
    const int hour12 = (h % 12 == 0) ? 12 : (h % 12);
    n = append_number(ids, n, hour12);
    ids[n++] = (uint16_t)SPK_POINT;
    if (local.tm_min == 0) {
        ids[n++] = (uint16_t)SPK_OCLOCK;
    } else {
        n = append_number(ids, n, local.tm_min);
        ids[n++] = (uint16_t)SPK_MINUTE;
    }

    if (s_weather_valid || WEATHER_DEBUG_FORCE) {
        n = append_weather_summary(ids, n);
    }

    ids[n++] = (uint16_t)SPK_ALARM_WAKEUP;

    ESP_LOGI(TAG, "alarm wake-up speech: %u segments, now %02d:%02d", (unsigned)n, h,
             local.tm_min);
    (void)asrpro_say(ids, n);
}

/* 从 p 处读一个可带负号的十进制整数,返回新位置;没读到数字返回 NULL。
 * 协议字段宽度不固定(period 可能是 -1),所以不能按固定下标取字符。 */
static const char *parse_int(const char *p, int *out)
{
    bool negative = false;
    if (*p == '-') {
        negative = true;
        ++p;
    }
    if (*p < '0' || *p > '9') return NULL;
    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > 100000) return NULL;
        ++p;
    }
    *out = negative ? -value : value;
    return p;
}

/* 解析 "ALARM " 之后的 "<day> <period> HH:MM";全部字段合法才返回 true。 */
static bool parse_alarm_fields(const char *p, int *day, int *period, int *hour, int *minute)
{
    p = parse_int(p, day);
    if (p == NULL || *p != ' ') return false;
    p = parse_int(p + 1, period);
    if (p == NULL || *p != ' ') return false;
    p = parse_int(p + 1, hour);
    if (p == NULL || *p != ':') return false;
    p = parse_int(p + 1, minute);
    return p != NULL;
}

/* ASRPRO 上报行解析:
 *   ALARM <day> <period> HH:MM  设置闹钟。day:0=今天 1=明天 2=每天;
 *                               period:0=早上 1=上午 2=下午 3=晚上 4=凌晨,-1=未说
 *   ALARM PICK <1|2>            闹钟满了之后回答"要替换哪一个"
 *   ALARM OFF                   取消全部闹钟
 *   ASK TIME / ASK WEATHER      播报当前时间 / 今天天气
 * 运行在 ASRPRO 接收任务上下文,这里只解析并置标志,真正的 NVS 写入/停铃交给
 * daily_task 执行(避免在接收任务里做阻塞操作)。 */
static void on_asrpro_line(const char *line)
{
    if (line == NULL) return;

    /* 语音问时间和天气:直接回片段序列,不涉及 NVS,可以在本任务里做。 */
    if (strncmp(line, "ASK WEATHER", 11U) == 0) {
        speak_current_weather();
        return;
    }
    if (strncmp(line, "ASK TIME", 8U) == 0) {
        speak_current_time();
        return;
    }

    if (strncmp(line, "ALARM OFF", 9U) == 0) {
        s_alarm_clear_pending = true;
        s_alarm_set_pending = false;
        s_alarm_pick_pending = false;
        s_alarm_pick_until_us = 0;
        if (s_daily_task != NULL) xTaskNotifyGive(s_daily_task);
        return;
    }
    /* 闹钟满了之后的回答。没在等、或者已经超时,就忽略 ——
     * 免得误覆盖用户已经设好的槽位。 */
    if (strncmp(line, "ALARM PICK ", 11U) == 0) {
        const char which = line[11];
        const bool waiting =
            s_alarm_pick_pending && esp_timer_get_time() <= s_alarm_pick_until_us;
        if ((which != '1' && which != '2') || !waiting) {
            ESP_LOGW(TAG, "unexpected or expired ALARM PICK: %s", line);
            s_alarm_pick_pending = false;
            s_alarm_pick_until_us = 0;
            return;
        }
        s_alarm_pending_slot = (uint8_t)(which - '1');
        s_alarm_pick_pending = false;
        s_alarm_pick_until_us = 0;
        s_alarm_set_pending = true;
        if (s_daily_task != NULL) xTaskNotifyGive(s_daily_task);
        return;
    }
    if (strncmp(line, "ALARM ", 6U) != 0) return;

    /* 还没校过时就不收:响铃分支要求 s_time_valid,存进去也不会响,
     * 但用户会听到"已经设好了"—— 那就是假成功。
     * 这里直接回一句"时间还没校准"。 */
    if (!s_time_valid) {
        const uint16_t no_time = (uint16_t)SPK_NO_TIME;
        (void)asrpro_say(&no_time, 1U);
        ESP_LOGW(TAG, "alarm rejected: time not synced yet");
        return;
    }

    /* 期望格式:ALARM <day> <period> HH:MM,例如 "ALARM 2 2 15:05"。 */
    int day = 0;
    int period = -1;
    int hour = 0;
    int minute = 0;
    if (!parse_alarm_fields(line + 6U, &day, &period, &hour, &minute)) {
        ESP_LOGW(TAG, "malformed alarm line: %s", line);
        return;
    }
    if (day < 0 || day > 2 || period < -1 || period > 4 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59) {
        ESP_LOGW(TAG, "alarm out of range: %s", line);
        return;
    }

    s_alarm_pending_day = (uint8_t)day;
    /* period 理论上必到(日期词之前 ASRPRO 会挡住没时段的情况),兜个底用 0。 */
    s_alarm_pending_period = (uint8_t)(period < 0 ? 0 : period);
    s_alarm_pending_hour = (uint8_t)hour;
    s_alarm_pending_minute = (uint8_t)minute;
    s_alarm_pending_slot = APP_ALARM_SLOT_MAX; /* 找空位;两个都满就反问 */
    s_alarm_set_pending = true;
    s_alarm_clear_pending = false;
    /* 这是新一轮设置请求:上一次"要替换哪一个"没答完的作废。 */
    s_alarm_pick_pending = false;
    s_alarm_pick_until_us = 0;
    if (s_daily_task != NULL) xTaskNotifyGive(s_daily_task);
}

/* 铃声先单独响 ALARM_SPEAK_DELAY_MS,到点才开口播报。
 * 播报念的是"此刻"的时间,放在延迟之后反而更准。
 * 这里跑在 esp_timer 任务上下文,asrpro_say() 只是往串口写一帧,不做阻塞操作。 */
static void alarm_speak_callback(void *arg)
{
    (void)arg;
    speak_alarm_wakeup();
}

esp_err_t time_service_init(const app_config_t *config, time_service_sync_finished_cb_t on_finished)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    s_config = *config;
    s_utc_offset_seconds = config->has_utc_offset ? config->utc_offset_seconds : 0;
    coords_load();   /* 先拿上次存下的经纬度,IP 定位失败时天气还能查 */
    s_on_finished = on_finished;
    (void)asrpro_set_event_callback(on_asrpro_line);
    const esp_timer_create_args_t alarm_speak_args = {
        .callback = alarm_speak_callback,
        .name = "alarm_speak",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&alarm_speak_args, &s_alarm_speak_timer), "time_service",
                        "alarm speak timer");
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

/* ===== 语音设置闹钟(V1.1 二进制帧入口) =====
 * ASRPRO 的拼装状态机凑齐"日期 + 时段 + 几点几分"后发 T_ALARM_SET,
 * link_probe 在收帧回调里直接调下面这几个函数。这里只置标志、唤醒 daily_task,
 * 定 NVS 写入和确认句播报都放到 daily_task 里做 —— 和以前 ASCII 那条路一样,
 * 不在串口接收任务里做阻塞操作。 */

/* period 为 0xFF(ASRPRO 没说时段)时按 hour 反推一个。
 * 入参的 hour 已经是 24 小时制,落 NVS 只缺 period 这个字段。 */
static uint8_t period_from_hour(uint8_t hour)
{
    if (hour < 6U)  return 4U;   /* 凌晨 */
    if (hour < 9U)  return 0U;   /* 早上 */
    if (hour < 12U) return 1U;   /* 上午 */
    if (hour < 18U) return 2U;   /* 下午 */
    return 3U;                   /* 晚上 */
}

void time_service_on_alarm_set(uint8_t day, uint8_t period, uint8_t hour, uint8_t minute)
{
    /* 还没校过时就不收:响铃分支要求 s_time_valid,存进去也不会响,
     * 但用户会听到"已经设好了"—— 那就是假成功。 */
    if (!s_time_valid) {
        const uint16_t no_time = (uint16_t)SPK_NO_TIME;
        (void)asrpro_say(&no_time, 1U);
        ESP_LOGW(TAG, "alarm rejected: time not synced yet");
        return;
    }
    if (day > 2U || hour > 23U || minute > 59U) {
        ESP_LOGW(TAG, "alarm out of range: day=%u %02u:%02u", (unsigned)day, (unsigned)hour,
                 (unsigned)minute);
        return;
    }
    if (period > 4U) period = period_from_hour(hour);

    s_alarm_pending_day = day;
    s_alarm_pending_period = period;
    s_alarm_pending_hour = hour;
    s_alarm_pending_minute = minute;
    s_alarm_pending_slot = APP_ALARM_SLOT_MAX; /* 找空位;两个都满就反问 */
    s_alarm_set_pending = true;
    s_alarm_clear_pending = false;
    /* 这是新一轮设置请求:上一次"要替换哪一个"没答完的作废。 */
    s_alarm_pick_pending = false;
    s_alarm_pick_until_us = 0;
    if (s_daily_task != NULL) xTaskNotifyGive(s_daily_task);

    ESP_LOGI(TAG, "alarm request: day=%u period=%u %02u:%02u", (unsigned)day, (unsigned)period,
             (unsigned)hour, (unsigned)minute);
}

void time_service_on_alarm_pick(uint8_t which)
{
    /* 没在等、或者已经超时,就忽略 —— 免得隔很久一句"第一个"突然生效。 */
    const bool waiting = s_alarm_pick_pending && esp_timer_get_time() <= s_alarm_pick_until_us;
    if ((which != 1U && which != 2U) || !waiting) {
        ESP_LOGW(TAG, "unexpected or expired alarm pick: %u", (unsigned)which);
        s_alarm_pick_pending = false;
        s_alarm_pick_until_us = 0;
        return;
    }

    s_alarm_pending_slot = (uint8_t)(which - 1U);
    s_alarm_pick_pending = false;
    s_alarm_pick_until_us = 0;
    s_alarm_set_pending = true;
    if (s_daily_task != NULL) xTaskNotifyGive(s_daily_task);

    ESP_LOGI(TAG, "alarm replace slot %u", (unsigned)(which - 1U));
}

void time_service_on_alarm_off(void)
{
    s_alarm_clear_pending = true;
    s_alarm_set_pending = false;
    s_alarm_pick_pending = false;
    s_alarm_pick_until_us = 0;
    if (s_daily_task != NULL) xTaskNotifyGive(s_daily_task);
    ESP_LOGI(TAG, "alarm clear requested");
}
