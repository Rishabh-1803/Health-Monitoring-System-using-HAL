/**
 * ============================================================
 * task_display.c
 * ============================================================
 * TaskDisplay — receives filtered samples, "displays" them.
 *
 * WHAT IT TEACHES:
 *   1. Combining data from TWO sources: a queue + an event group
 *   2. The "check alarm state, then act" pattern
 *   3. Periodic vs. event-driven hybrid behavior
 *
 * HYBRID TASK PATTERN
 * -------------------
 * This task has TWO things it cares about:
 *   a) New filtered data arrived (queue)
 *   b) Alarm state changed (event group)
 *
 * We DON'T block forever on the queue, because then we'd miss
 * alarm transitions. We DON'T block forever on the event group,
 * because then we'd miss new data.
 *
 * Solution: block on the queue with a SHORT timeout (say 500ms).
 * When the timeout fires, we know no new data arrived — but we
 * can still check the alarm state. This is a common pattern for
 * tasks that need to react to multiple async sources.
 *
 * In the real project, this becomes the alarm control task on
 * STM32 — it updates LEDs and buzzer based on alarm bits, AND
 * reacts to "operator ack" button presses.
 * ============================================================ */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "project_config.h"
#include "sensor_data.h"
#include "alarm_event.h"
#include "log_queue.h"

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
        /* ---- Block on the queue with a 500ms timeout ----
         * If data arrives: process it.
         * If timeout fires: queue returns pdFALSE, fall through
         * to the alarm-state check below. Either way, we re-check
         * alarms every 500ms at minimum. */
        BaseType_t got_data = xQueueReceive(
            s_input_queue, &data, pdMS_TO_TICKS(500));

        if (got_data == pdTRUE) {
            /* New filtered sample — log it. In the real project
             * this is where we'd update the OLED display. */
            log_send(LOG_LEVEL_INFO, "DISPLAY",
                     "seq=%lu  T=%.1f°C  I=%.2fA  V=%.2fg  (n=%lu)",
                     data.seq, data.temperature_c, data.current_a,
                     data.vibration_g, data.samples_in_window);
        }

        /* ---- Check alarm state (non-blocking) ----
         * Compare current state to last-seen state. If anything
         * changed, log it. This way we don't spam logs — only
         * transitions get logged. */
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

            /* Check for cleared alarms too */
            uint32_t cleared = last_alarm_state & ~current;
            if (cleared) {
                log_send(LOG_LEVEL_INFO, "DISPLAY",
                         "Alarms cleared: bitmask=0x%02lx", cleared);
            }

            last_alarm_state = current;
        }
    }
}
