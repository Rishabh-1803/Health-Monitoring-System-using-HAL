/**
 * ============================================================
 * app_init.c — Boot sequence for the warmup app
 * ============================================================
 * Same boot order as the ESP32 version:
 *   1. Init log_queue first (so any subsequent code can log)
 *   2. Init shared_config (mutex)
 *   3. Init alarm_event (event group)
 *   4. Create queues
 *   5. Inject queue handles into task modules
 *   6. Create tasks
 *   7. Inject task handles into TaskStats
 *
 * Called from CubeMX-generated MX_FREERTOS_Init() in freertos.c.
 * The scheduler starts AFTER MX_FREERTOS_Init() returns.
 * ============================================================ */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "project_config.h"
#include "sensor_data.h"
#include "shared_config.h"
#include "alarm_event.h"
#include "log_queue.h"
#include "usb_serial.h"
#include "task_sensor.h"
#include "task_filter.h"
#include "task_display.h"
#include "task_logger.h"
#include "task_stats.h"
#include "app_init.h"

void warmup_app_init(void) {
    /* Step 1: USB CDC + Logger queue first */
    usb_serial_init();
    log_queue_init();

    /* Step 2 + 3: Mutexes and event groups */
    shared_config_init();
    alarm_event_init();

    /* Step 4: Queues */
    QueueHandle_t q_sensor  = xQueueCreate(SENSOR_QUEUE_LEN,  sizeof(sensor_data_t));
    QueueHandle_t q_display = xQueueCreate(DISPLAY_QUEUE_LEN, sizeof(sensor_filtered_t));
    configASSERT(q_sensor != NULL && q_display != NULL);

    /* Step 5: Inject queue handles */
    task_sensor_set_queue(q_sensor);
    task_filter_set_queues(q_sensor, q_display);
    task_display_set_queue(q_display);
    task_stats_set_queues(q_sensor, q_display);

    /* Step 6: Create tasks.
     * Order matters for the first few ticks — logger first so it can
     * drain the queue as soon as other tasks log. */
    TaskHandle_t h_sensor, h_filter, h_display, h_logger, h_stats;

    xTaskCreate(task_logger,  "logger",  TASK_LOGGER_STACK,  NULL, TASK_LOGGER_PRIO,  &h_logger);
    xTaskCreate(task_sensor,  "sensor",  TASK_SENSOR_STACK,  NULL, TASK_SENSOR_PRIO,  &h_sensor);
    xTaskCreate(task_filter,  "filter",  TASK_FILTER_STACK,  NULL, TASK_FILTER_PRIO,  &h_filter);
    xTaskCreate(task_display, "display", TASK_DISPLAY_STACK, NULL, TASK_DISPLAY_PRIO, &h_display);
    xTaskCreate(task_stats,   "stats",   TASK_STATS_STACK,   NULL, TASK_STATS_PRIO,   &h_stats);

    configASSERT(h_sensor && h_filter && h_display && h_logger && h_stats);

    /* Step 7: Inject task handles into TaskStats */
    task_stats_set_handles(h_sensor, h_filter, h_display, h_logger, h_stats);
}
