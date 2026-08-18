/**
 * ============================================================
 * task_stats.c — STM32 port
 * ============================================================
 * Differences from ESP32 version:
 *   - xPortGetFreeHeapSize() instead of esp_get_free_heap_size()
 *   - No "min_ever" stat (FreeRTOS doesn't track it by default
 *     unless configUSE_MALLOC_FAILED_HOOK is enabled)
 *   - FreeRTOS tick rate on STM32 CubeMX default is 1000 Hz
 *     (1 ms per tick), so the tick-to-ms conversion is 1:1.
 * ============================================================ */

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "project_config.h"
#include "log_queue.h"
#include "task_stats.h"

static TaskHandle_t s_handle_sensor  = NULL;
static TaskHandle_t s_handle_filter  = NULL;
static TaskHandle_t s_handle_display = NULL;
static TaskHandle_t s_handle_logger  = NULL;
static TaskHandle_t s_handle_stats   = NULL;

static QueueHandle_t s_q_sensor  = NULL;
static QueueHandle_t s_q_display = NULL;

void task_stats_set_handles(TaskHandle_t sensor, TaskHandle_t filter,
                            TaskHandle_t display, TaskHandle_t logger,
                            TaskHandle_t stats) {
    s_handle_sensor  = sensor;
    s_handle_filter  = filter;
    s_handle_display = display;
    s_handle_logger  = logger;
    s_handle_stats   = stats;
}

void task_stats_set_queues(QueueHandle_t sensor_q, QueueHandle_t display_q) {
    s_q_sensor  = sensor_q;
    s_q_display = display_q;
}

static void log_stack(const char *name, TaskHandle_t h) {
    if (h == NULL) return;
    UBaseType_t hw = uxTaskGetStackHighWaterMark(h);
    log_send(LOG_LEVEL_INFO, "STATS",
             "  %-10s stack_free=%lu words (%lu bytes)",
             name, (unsigned long)hw, (unsigned long)(hw * 4));
}

void task_stats(void *arg) {
    (void)arg;

    log_send(LOG_LEVEL_INFO, "STATS", "TaskStats started");

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t report_num = 0;

    while (1) {
        report_num++;

        log_send(LOG_LEVEL_INFO, "STATS",
                 "==== Diagnostics Report #%lu (uptime=%lu ms) ====",
                 report_num, (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));

        log_send(LOG_LEVEL_INFO, "STATS",
                 "  free_heap=%lu bytes",
                 (unsigned long)xPortGetFreeHeapSize());

        log_stack("sensor",  s_handle_sensor);
        log_stack("filter",  s_handle_filter);
        log_stack("display", s_handle_display);
        log_stack("logger",  s_handle_logger);
        log_stack("stats",   s_handle_stats);

        if (s_q_sensor) {
            log_send(LOG_LEVEL_INFO, "STATS",
                     "  queue[ sensor ]=%lu/%d",
                     (unsigned long)uxQueueMessagesWaiting(s_q_sensor),
                     SENSOR_QUEUE_LEN);
        }
        if (s_q_display) {
            log_send(LOG_LEVEL_INFO, "STATS",
                     "  queue[ display]=%lu/%d",
                     (unsigned long)uxQueueMessagesWaiting(s_q_display),
                     DISPLAY_QUEUE_LEN);
        }
        log_send(LOG_LEVEL_INFO, "STATS",
                 "  queue[ logger ]=%lu/%d",
                 (unsigned long)log_queue_depth(),
                 LOGGER_QUEUE_LEN);

        log_send(LOG_LEVEL_INFO, "STATS",
                 "  tasks: sensor=%s filter=%s display=%s logger=%s",
                 pcTaskGetTaskName(s_handle_sensor),
                 pcTaskGetTaskName(s_handle_filter),
                 pcTaskGetTaskName(s_handle_display),
                 pcTaskGetTaskName(s_handle_logger));

        log_send(LOG_LEVEL_INFO, "STATS", "==== End diagnostics ====");

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(STATS_PRINT_PERIOD_MS));
    }
}
