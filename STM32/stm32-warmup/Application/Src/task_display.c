/**
 * task_display.c — STM32 port. Same hybrid pattern: queue + event group.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "project_config.h"
#include "sensor_data.h"
#include "alarm_event.h"
#include "log_queue.h"
#include "task_display.h"

static QueueHandle_t s_input_queue = NULL;

void task_display_set_queue(QueueHandle_t q) {
    s_input_queue = q;
}

void task_display(void *arg) {
    (void)arg;

    log_send(LOG_LEVEL_INFO, "DISPLAY", "TaskDisplay started");

    sensor_filtered_t data;
    uint32_t last_alarm_state = 0;

    while (1) {
        BaseType_t got_data = xQueueReceive(
            s_input_queue, &data, pdMS_TO_TICKS(500));

        if (got_data == pdTRUE) {
            log_send(LOG_LEVEL_INFO, "DISPLAY",
                     "seq=%lu  T=%.1fC  I=%.2fA  V=%.2fg  (n=%lu)",
                     data.seq, data.temperature_c, data.current_a,
                     data.vibration_g, data.samples_in_window);
        }

        uint32_t current = alarm_event_get();
        if (current != last_alarm_state) {
            if (current & ALARM_BIT_OVERTEMP) {
                log_send(LOG_LEVEL_ERROR, "DISPLAY",
                         "ALARM RAISED: Over-temperature!");
            }
            if (current & ALARM_BIT_OVERCURRENT) {
                log_send(LOG_LEVEL_ERROR, "DISPLAY",
                         "ALARM RAISED: Over-current!");
            }
            if (current & ALARM_BIT_VIBRATION) {
                log_send(LOG_LEVEL_ERROR, "DISPLAY",
                         "ALARM RAISED: Vibration!");
            }

            uint32_t cleared = last_alarm_state & ~current;
            if (cleared) {
                log_send(LOG_LEVEL_INFO, "DISPLAY",
                         "Alarms cleared: bitmask=0x%02lx", cleared);
            }

            last_alarm_state = current;
        }
    }
}
