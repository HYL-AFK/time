#include "timer_service.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "asrpro.h"
#include "time_service.h"

#define TAG "timer"

/* 倒计时到点时播报的片段：432 = “您好，时间到了”。 */
#define SPK_TIME_UP 432U

#define TICK_PERIOD_US (100 * 1000)
#define TIMER_TASK_STACK 3072
#define TIMER_TASK_PRIO  4

/* 播报必须离开串口接收任务和 esp_timer 回调：
 * - link_rx_task 里调 asrpro_say() 会等 PONG，而 PONG 也要靠它自己读 → 自死锁 500 ms；
 * - esp_timer 回调里更不能调 vTaskDelay。
 * 所以接收侧只投消息，真正的播报由本任务执行。 */
typedef enum {
    TIMER_MSG_STOP_COUNTUP = 0,  /* 正计时结束，播报总时长 */
    TIMER_MSG_TIME_UP,           /* 倒计时到点 */
} timer_msg_kind_t;

typedef struct {
    timer_msg_kind_t kind;
    int minutes;
    int seconds;
} timer_msg_t;

static timer_mode_t        s_mode;
static int64_t             s_started_us;   /* 正计时起点 */
static int64_t             s_deadline_us;  /* 倒计时截止点 */
static esp_timer_handle_t  s_tick_timer;
static QueueHandle_t       s_queue;
static TaskHandle_t        s_task;

static void timer_task(void *arg)
{
    (void)arg;
    timer_msg_t msg;

    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        if (msg.kind == TIMER_MSG_STOP_COUNTUP) {
            time_service_speak_duration(msg.minutes, msg.seconds);
        } else {
            /* 先响 4 秒 TF 卡音乐，再让语音把它接过去。
             * 这个 MP3 模块不支持混音：语音一开口就会停掉 TF 卡播放，
             * 正好当"音乐淡出、语音接管"，不需要额外的 stop 指令。 */
            (void)asrpro_play(201U);
            vTaskDelay(pdMS_TO_TICKS(4000));
            const uint16_t id = (uint16_t)SPK_TIME_UP;
            (void)asrpro_announce(&id, 1U);
        }
    }
}

static void timer_tick(void *arg)
{
    (void)arg;

    if (s_mode != TIMER_MODE_COUNTDOWN) return;
    if (esp_timer_get_time() < s_deadline_us) return;

    /* 到点先退出计时态，防止重复入队。这里只投递，不做任何阻塞操作。 */
    s_mode = TIMER_MODE_IDLE;
    const timer_msg_t msg = { .kind = TIMER_MSG_TIME_UP };
    (void)xQueueSend(s_queue, &msg, 0);
    ESP_LOGI(TAG, "countdown done");
}

void timer_service_init(void)
{
    s_mode = TIMER_MODE_IDLE;
    s_started_us = 0;
    s_deadline_us = 0;

    if (s_task != NULL) return;

    s_queue = xQueueCreate(4, sizeof(timer_msg_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }
    if (xTaskCreate(timer_task, "timer_task", TIMER_TASK_STACK, NULL, TIMER_TASK_PRIO,
                    &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }

    const esp_timer_create_args_t args = {
        .callback = timer_tick,
        .arg = NULL,
        .name = "timer_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, TICK_PERIOD_US));
}

void timer_service_start_countup(void)
{
    s_mode = TIMER_MODE_COUNTUP;
    s_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "countup start");
}

void timer_service_stop_countup(void)
{
    if (s_mode != TIMER_MODE_COUNTUP) {
        ESP_LOGW(TAG, "stop requested but not counting up");
        return;
    }

    const int64_t elapsed_us = esp_timer_get_time() - s_started_us;
    s_mode = TIMER_MODE_IDLE;

    int total = (int)(elapsed_us / 1000000LL);
    if (total < 0) total = 0;
    ESP_LOGI(TAG, "countup stop: %d s", total);

    /* 只算时长、投消息；播报由 timer_task 做，避免阻塞串口接收。 */
    const timer_msg_t msg = {
        .kind = TIMER_MSG_STOP_COUNTUP,
        .minutes = total / 60,
        .seconds = total % 60,
    };
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, duration dropped");
    }
}

void timer_service_cancel(void)
{
    /* 只取消正计时；倒计时由 cancel_countdown 负责，
     * 否则正计时中误说一句“取消倒计时”会把正计时也清掉。 */
    if (s_mode != TIMER_MODE_COUNTUP) return;
    ESP_LOGI(TAG, "countup cancelled");
    s_mode = TIMER_MODE_IDLE;
}

void timer_service_start_countdown(uint16_t seconds)
{
    if (seconds == 0U) {
        ESP_LOGW(TAG, "countdown rejected: 0 s");
        return;
    }

    s_mode = TIMER_MODE_COUNTDOWN;
    s_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
    ESP_LOGI(TAG, "countdown start: %u s", (unsigned)seconds);
}

void timer_service_cancel_countdown(void)
{
    if (s_mode != TIMER_MODE_COUNTDOWN) return;
    ESP_LOGI(TAG, "countdown cancelled");
    s_mode = TIMER_MODE_IDLE;
}

timer_mode_t timer_service_mode(void)
{
    return s_mode;
}
