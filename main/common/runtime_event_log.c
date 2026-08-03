#include "runtime_event_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define RUNTIME_EVENT_LOG_CAPACITY 16U
#define RUNTIME_EVENT_RECORD_SIZE 32U
#define RUNTIME_EVENT_MAGIC 0x524CU
#define RUNTIME_EVENT_STORE_MAGIC 0x52544C47U
#define RUNTIME_EVENT_VERSION 1U
#define RUNTIME_EVENT_NVS_NAMESPACE "eclock_log"
#define RUNTIME_EVENT_NVS_KEY "events"
#define RUNTIME_EVENT_TIME_VALID_FLAG (1U << 0)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint16_t capacity;
    uint16_t write_index;
    uint16_t read_index;
    uint16_t count;
    uint8_t reserved[8];
    uint32_t next_sequence;
    uint32_t header_crc32;
} runtime_event_header_t;

typedef struct __attribute__((packed)) {
    runtime_event_header_t header;
    runtime_event_record_t records[RUNTIME_EVENT_LOG_CAPACITY];
} runtime_event_store_t;

_Static_assert(sizeof(runtime_event_header_t) == 32, "runtime event header layout changed");
_Static_assert(sizeof(runtime_event_record_t) == RUNTIME_EVENT_RECORD_SIZE,
               "runtime event record layout changed");

static const char *TAG = "runtime_event_log";
static runtime_event_store_t s_store;
static SemaphoreHandle_t s_lock;
static bool s_initialized;

static uint32_t crc32(const void *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static uint32_t header_crc(const runtime_event_header_t *header)
{
    return crc32(header, offsetof(runtime_event_header_t, header_crc32));
}

static uint32_t record_crc(const runtime_event_record_t *record)
{
    return crc32(record, offsetof(runtime_event_record_t, record_crc32));
}

static bool record_valid(const runtime_event_record_t *record)
{
    return record->magic == RUNTIME_EVENT_MAGIC && record->version == RUNTIME_EVENT_VERSION &&
           record->record_crc32 == record_crc(record);
}

static bool store_valid(const runtime_event_store_t *store)
{
    const runtime_event_header_t *header = &store->header;
    if (header->magic != RUNTIME_EVENT_STORE_MAGIC || header->version != RUNTIME_EVENT_VERSION ||
        header->record_size != RUNTIME_EVENT_RECORD_SIZE ||
        header->capacity != RUNTIME_EVENT_LOG_CAPACITY ||
        header->write_index >= RUNTIME_EVENT_LOG_CAPACITY ||
        header->read_index >= RUNTIME_EVENT_LOG_CAPACITY ||
        header->count > RUNTIME_EVENT_LOG_CAPACITY ||
        header->header_crc32 != header_crc(header)) {
        return false;
    }

    for (uint16_t i = 0; i < header->count; ++i) {
        const uint16_t index = (uint16_t)((header->read_index + i) % RUNTIME_EVENT_LOG_CAPACITY);
        if (!record_valid(&store->records[index])) return false;
    }
    return true;
}

static void store_reset(runtime_event_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->header.magic = RUNTIME_EVENT_STORE_MAGIC;
    store->header.version = RUNTIME_EVENT_VERSION;
    store->header.record_size = RUNTIME_EVENT_RECORD_SIZE;
    store->header.capacity = RUNTIME_EVENT_LOG_CAPACITY;
    store->header.next_sequence = 1U;
    store->header.header_crc32 = header_crc(&store->header);
}

static esp_err_t store_commit(const runtime_event_store_t *store)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(RUNTIME_EVENT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, RUNTIME_EVENT_NVS_KEY, store, sizeof(*store));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t runtime_event_log_init(void)
{
    if (s_initialized) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    bool valid = false;
    nvs_handle_t handle;
    if (nvs_open(RUNTIME_EVENT_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(s_store);
        if (nvs_get_blob(handle, RUNTIME_EVENT_NVS_KEY, &s_store, &size) == ESP_OK &&
            size == sizeof(s_store)) {
            valid = store_valid(&s_store);
        }
        nvs_close(handle);
    }
    if (!valid) store_reset(&s_store);
    s_initialized = true;
    ESP_LOGI(TAG, "event log ready: valid=%d capacity=%u", valid,
             (unsigned)RUNTIME_EVENT_LOG_CAPACITY);
    return ESP_OK;
}

esp_err_t runtime_event_log_append(runtime_event_type_t type, int32_t value1, int32_t value2,
                                   uint32_t flags)
{
    if (!s_initialized || type == 0) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const runtime_event_store_t previous = s_store;
    const uint16_t index = s_store.header.write_index;
    runtime_event_record_t *record = &s_store.records[index];
    memset(record, 0, sizeof(*record));
    record->magic = RUNTIME_EVENT_MAGIC;
    record->version = RUNTIME_EVENT_VERSION;
    record->event_type = (uint8_t)type;
    const time_t now = time(NULL);
    if (now >= 1577836800) {
        record->timestamp = (uint64_t)now;
        flags |= RUNTIME_EVENT_TIME_VALID_FLAG;
    }
    record->value1 = value1;
    record->value2 = value2;
    record->flags = flags;
    record->sequence = s_store.header.next_sequence++;
    record->record_crc32 = record_crc(record);

    s_store.header.write_index = (uint16_t)((index + 1U) % RUNTIME_EVENT_LOG_CAPACITY);
    if (s_store.header.count < RUNTIME_EVENT_LOG_CAPACITY) {
        s_store.header.count++;
    } else {
        s_store.header.read_index =
            (uint16_t)((s_store.header.read_index + 1U) % RUNTIME_EVENT_LOG_CAPACITY);
    }
    s_store.header.header_crc32 = header_crc(&s_store.header);

    const esp_err_t err = store_commit(&s_store);
    if (err != ESP_OK) {
        s_store = previous;
        ESP_LOGW(TAG, "event append failed: type=%u err=%s", (unsigned)type,
                 esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t runtime_event_log_get(uint16_t offset_from_oldest, runtime_event_record_t *out)
{
    if (!s_initialized || out == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (offset_from_oldest >= s_store.header.count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const uint16_t index = (uint16_t)((s_store.header.read_index + offset_from_oldest) %
                                      RUNTIME_EVENT_LOG_CAPACITY);
    *out = s_store.records[index];
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

uint16_t runtime_event_log_count(void)
{
    if (!s_initialized) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t count = s_store.header.count;
    xSemaphoreGive(s_lock);
    return count;
}

void runtime_event_log_dump(void)
{
    if (!s_initialized) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ESP_LOGI(TAG, "event log count=%u", (unsigned)s_store.header.count);
    for (uint16_t offset = 0; offset < s_store.header.count; ++offset) {
        const uint16_t index =
            (uint16_t)((s_store.header.read_index + offset) % RUNTIME_EVENT_LOG_CAPACITY);
        const runtime_event_record_t *record = &s_store.records[index];
        ESP_LOGI(TAG, "event seq=%lu type=%u ts=%llu v1=%ld v2=%ld flags=0x%08lx",
                 (unsigned long)record->sequence, (unsigned)record->event_type,
                 (unsigned long long)record->timestamp, (long)record->value1,
                 (long)record->value2, (unsigned long)record->flags);
    }
    xSemaphoreGive(s_lock);
}

esp_err_t runtime_event_log_clear(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const runtime_event_store_t previous = s_store;
    store_reset(&s_store);
    const esp_err_t err = store_commit(&s_store);
    if (err != ESP_OK) s_store = previous;
    xSemaphoreGive(s_lock);
    return err;
}
