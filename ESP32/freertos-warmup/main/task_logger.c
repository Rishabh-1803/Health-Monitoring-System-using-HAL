/**
 * ============================================================
 * task_logger.c
 * ============================================================
 * TaskLogger — the ONLY task allowed to call ESP_LOGI / printf.
 *
 * WHAT IT TEACHES:
 *   1. The single-writer pattern ( serialization of a shared resource )
 *   2. Blocking on a queue forever ( portMAX_DELAY )
 *   3. Why logger task gets the HIGHEST priority
 *
 * WHY HIGHEST PRIORITY?
 * --------------------
 * Logs are critical for debugging. If a flood of high-priority
 * work arrives (alarms, sensor spikes), the log queue fills up.
 * If the logger has low priority, it never gets CPU time, the
 * queue overflows, and you lose the EXACT logs you needed to
 * debug the problem.
 *
 * By making logger highest priority, it preempts everything
 * whenever a log arrives, drains the queue quickly, then goes
 * back to sleep. Logs are NEVER lost in normal operation.
 *
 * BUT — we don't want logger to starve other tasks. Solution:
 * the logger task does minimal work (just ESP_LOGI) and then
 * blocks again. Each "burst" of work is microseconds.
 *
 * TRADE-OFF: Note the mutex held by shared_config has a 100ms
 * timeout — if logger was low priority and got stuck on UART,
 * other tasks waiting on config could deadlock. Priority
 * prevents this.
 * ============================================================ */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "project_config.h"
#include "log_queue.h"

static const char *TAG = "LOGGER";

/* Map our internal log levels to ESP-IDF's log colors */
static const char *level_str(log_level_t l) {
    switch (l) {
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERR ";
        default:              return "??? ";
    }
}

void task_logger(void *arg) {
    (void)arg;

    /* Use ESP_LOGI here because this IS the logger task —
     * the one exception to the "no direct logging" rule.
     * This boot banner confirms the logger is alive. */
    ESP_LOGI(TAG, "TaskLogger started — owns serial output");

    log_message_t msg;

    while (1) {
        /* ---- Block FOREVER until a log message arrives ----
         * This is the textbook consumer pattern. The task
         * consumes zero CPU while waiting. */
        if (!log_queue_receive(&msg, portMAX_DELAY)) {
            continue;
        }

        /* ---- Print to UART ----
         * We use ESP_LOGI's color formatting for readability.
         * In the real project we'd also write to LittleFS here
         * for persistent logging. */
        printf("[%6lu] [%s] [%s] %s\n",
               msg.tick,
               msg.tag ? msg.tag : "??",
               level_str(msg.level),
               msg.message);
    }
}
