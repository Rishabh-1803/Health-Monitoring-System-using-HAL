/**
 * ============================================================
 * task_stats.c
 * ============================================================
 * TaskStats — periodic diagnostics printer.
 *
 * WHAT IT TEACHES:
 *   1. uxTaskGetStackHighWaterMark — the #1 tool for finding
 *      stack-size problems BEFORE they crash your system
 *   2. uxTaskGetSystemState — captures snapshot of ALL tasks
 *   3. Queue depth monitoring — detecting backpressure
 *
 * STACK HIGH WATER MARK — READ THIS CAREFULLY
 * -------------------------------------------
 * FreeRTOS paints the unused portion of every task's stack with
 * a known pattern (0xA5) at task creation. As the task runs and
 * uses more stack, it overwrites that pattern.
 *
 * uxTaskGetStackHighWaterMark(NULL) tells you:
 *   "How many words of stack are STILL untouched?"
 *
 * If this returns 0 (or close to 0), you're about to overflow.
 * If it returns 50% of stack size, your stack is 2x too big —
 * trim it and save heap.
 *
 * INDUSTRY RULE: At any time, you should have AT LEAST 20% of
 * your stack as "high water" (untouched). Less = danger zone.
 *
 * We'll use this exact pattern in the real project's Diagnostics
 * Task to feed stack info to the web dashboard.
 * ============================================================ */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "project_config.h"
#include "log_queue.h"

/* To get CPU % per task, ESP-IDF needs runtime stats enabled.
 * That's set in sdkconfig.defaults (CONFIG_FREERTOS_USE_TRACE_FACILITY etc).
 * If you don't see CPU %, that's the missing config. */

/* Task handles — passed in from main so we can query each task's
 * stack high water mark. (NULL also works inside the task itself.) */
static TaskHandle_t s_handle_sensor  = NULL;
static TaskHandle_t s_handle_filter  = NULL;
static TaskHandle_t s_handle_display = NULL;
static TaskHandle_t s_handle_logger  = NULL;
static TaskHandle_t s_handle_stats   = NULL;

/* Input queue handles — for depth reporting */
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

/* Helper: print stack high-water for one task */
static void log_stack(const char *name, TaskHandle_t h) {
    if (h == NULL) return;
    /* Returns unused stack in WORDS. Multiply by 4 for bytes on ESP32. */
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

        /* ---- Free heap ---- */
        log_send(LOG_LEVEL_INFO, "STATS",
                 "  free_heap=%lu bytes  min_ever=%lu bytes",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size());

        /* ---- Stack high water marks ---- */
        log_stack("sensor",  s_handle_sensor);
        log_stack("filter",  s_handle_filter);
        log_stack("display", s_handle_display);
        log_stack("logger",  s_handle_logger);
        log_stack("stats",   s_handle_stats);

        /* ---- Queue depths ---- */
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

                /* ---- Task states (brief) ----
         * We could call uxTaskGetSystemState() for full detail,
         * but that's heavyweight. Just log our task states briefly. */
        log_send(LOG_LEVEL_INFO, "STATS",
                 "  tasks: sensor=%s filter=%s display=%s logger=%s",
                 pcTaskGetName(s_handle_sensor),
                 pcTaskGetName(s_handle_filter),
                 pcTaskGetName(s_handle_display),
                 pcTaskGetName(s_handle_logger));

        log_send(LOG_LEVEL_INFO, "STATS", "==== End diagnostics ====");

        /* ---- Periodic wakeup ---- */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(STATS_PRINT_PERIOD_MS));
    }
}
