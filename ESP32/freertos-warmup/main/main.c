/**
 * ============================================================
 * main.c
 * ============================================================
 * Entry point for the FreeRTOS warmup project.
 *
 * WHAT IT TEACHES:
 *   1. The boot sequence pattern: init resources → create queues
 *      → start tasks → vTaskStartScheduler
 *   2. Passing handles between modules cleanly
 *   3. Why configASSERT matters during development
 *
 * BOOT ORDER MATTERS
 * ------------------
 * The order here is deliberate:
 *   1. Initialize log_queue FIRST — so any subsequent init code
 *      can already call log_send() (even though no logger task
 *      is draining the queue yet — messages will buffer).
 *   2. Init shared_config + alarm_event (their mutexes/groups).
 *   3. Create queues.
 *   4. Inject queue handles into each task module via setters.
 *      (Alternative: pass handles via xTaskCreate parameter. Both
 *       work; setters keep main.c cleaner.)
 *   5. Create tasks. Tasks start running IMMEDIATELY if their
 *      priority exceeds the current context's priority, so all
 *      handles must be set BEFORE xTaskCreate.
 *   6. main() returns — ESP-IDF's main runs on a task that gets
 *      deleted automatically. Our work continues in the created
 *      tasks.
 * ============================================================ */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"

#include "project_config.h"
#include "sensor_data.h"
#include "shared_config.h"
#include "alarm_event.h"
#include "log_queue.h"

/* Function prototypes for task entry points (defined in their files) */
extern void task_sensor(void *arg);
extern void task_filter(void *arg);
extern void task_display(void *arg);
extern void task_logger(void *arg);
extern void task_stats(void *arg);

/* Setters defined in each task's .c file to receive queue/handle refs */
extern void task_sensor_set_queue(QueueHandle_t q);
extern void task_filter_set_queues(QueueHandle_t in, QueueHandle_t out);
extern void task_display_set_queue(QueueHandle_t q);
extern void task_stats_set_handles(TaskHandle_t sensor, TaskHandle_t filter,
                                   TaskHandle_t display, TaskHandle_t logger,
                                   TaskHandle_t stats);
extern void task_stats_set_queues(QueueHandle_t sensor_q, QueueHandle_t display_q);

static const char *TAG = "MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "=== FreeRTOS Warmup Boot ===");
    ESP_LOGI(TAG, "FreeRTOS tick rate: %d Hz", configTICK_RATE_HZ);
    ESP_LOGI(TAG, "Free heap at boot: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    /* ---- Step 1: init subsystems (no tasks running yet) ---- */
    log_queue_init();
    shared_config_init();
    alarm_event_init();

    /* ---- Step 2: create queues ---- */
    QueueHandle_t q_sensor  = xQueueCreate(SENSOR_QUEUE_LEN,  sizeof(sensor_data_t));
    QueueHandle_t q_display = xQueueCreate(DISPLAY_QUEUE_LEN, sizeof(sensor_filtered_t));
    configASSERT(q_sensor != NULL && q_display != NULL);

    /* ---- Step 3: inject queue handles into task modules ----
     * Doing this BEFORE xTaskCreate so the tasks see valid handles
     * from the very first iteration. */
    task_sensor_set_queue(q_sensor);
    task_filter_set_queues(q_sensor, q_display);
    task_display_set_queue(q_display);
    task_stats_set_queues(q_sensor, q_display);

    /* ---- Step 4: create tasks ---- */
    TaskHandle_t h_sensor, h_filter, h_display, h_logger, h_stats;

    xTaskCreate(task_logger,  "logger",  TASK_LOGGER_STACK,  NULL, TASK_LOGGER_PRIO,  &h_logger);
    xTaskCreate(task_sensor,  "sensor",  TASK_SENSOR_STACK,  NULL, TASK_SENSOR_PRIO,  &h_sensor);
    xTaskCreate(task_filter,  "filter",  TASK_FILTER_STACK,  NULL, TASK_FILTER_PRIO,  &h_filter);
    xTaskCreate(task_display, "display", TASK_DISPLAY_STACK, NULL, TASK_DISPLAY_PRIO, &h_display);
    xTaskCreate(task_stats,   "stats",   TASK_STATS_STACK,   NULL, TASK_STATS_PRIO,   &h_stats);

    configASSERT(h_sensor && h_filter && h_display && h_logger && h_stats);

    /* Now that we have task handles, give them to TaskStats */
    task_stats_set_handles(h_sensor, h_filter, h_display, h_logger, h_stats);

    ESP_LOGI(TAG, "All 5 tasks created. Logger owns serial from here on.");
    ESP_LOGI(TAG, "(you should see boot messages from each task below)");

    /* app_main's task is automatically deleted by ESP-IDF after return.
     * Our 5 tasks continue running forever. */
}
