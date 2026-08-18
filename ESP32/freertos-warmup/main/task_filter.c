/**
 * ============================================================
 * task_filter.c
 * ============================================================
 * TaskFilter — receives raw samples, applies a moving-average
 * filter, forwards filtered samples downstream.
 *
 * WHAT IT TEACHES:
 *   1. BLOCKING on xQueueReceive (consumer pattern)
 *   2. Ring buffer for moving average (no dynamic allocation)
 *   3. Sending a DIFFERENT struct through a DIFFERENT queue
 *   4. Why "block on input, non-block on output" is the standard
 *
 * WHY BLOCK ON RECEIVE?
 * --------------------
 * The filter task has nothing to do when there's no input. We
 * could poll the queue with timeout=0 in a loop, but that
 * wastes CPU. Blocking puts the task to sleep until data arrives
 * — the scheduler runs other tasks while we wait. This is the
 * essence of efficient RTOS programming:
 *
 *   "A well-designed RTOS task spends MOST of its time blocked."
 *
 * WHY NON-BLOCKING ON SEND?
 * ------------------------
 * If the display queue is full, we don't want to wait — that
 * would delay the next sample's processing. We drop the filtered
 * sample and move on. In the real project, we'd log this.
 * ============================================================ */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "project_config.h"
#include "sensor_data.h"
#include "log_queue.h"

#define FILTER_WINDOW_SIZE  5

static QueueHandle_t s_input_queue  = NULL;  // Raw sensor data
static QueueHandle_t s_output_queue = NULL;  // Filtered data

void task_filter_set_queues(QueueHandle_t in, QueueHandle_t out) {
    s_input_queue  = in;
    s_output_queue = out;
}

void task_filter(void *arg) {
    (void)arg;

    log_send(LOG_LEVEL_INFO, "FILTER", "TaskFilter started");

    /* ---- Ring buffer for moving average ----
     * A ring buffer is the classic way to maintain a sliding
     * window without shifting memory. We track:
     *   - buffer[]: the values
     *   - index:    where the next write goes (wraps around)
     *   - count:    how many slots are filled (for early-startup)
     *   - sum:      running sum (so we don't re-add everything) */
    float temp_buf[FILTER_WINDOW_SIZE]      = {0};
    float current_buf[FILTER_WINDOW_SIZE]   = {0};
    float vibration_buf[FILTER_WINDOW_SIZE] = {0};
    int   idx    = 0;
    int   count  = 0;
    float t_sum  = 0, c_sum = 0, v_sum = 0;

    sensor_data_t raw;

    while (1) {
        /* ---- Block until a sample arrives ----
         * portMAX_DELAY = wait forever. The task is NOT consuming
         * CPU while blocked — it's in the SUSPENDED state internally. */
        if (xQueueReceive(s_input_queue, &raw, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* ---- Update ring buffer and running sums ----
         * Subtract the old value, add the new value, store new value. */
        t_sum -= temp_buf[idx];
        c_sum -= current_buf[idx];
        v_sum -= vibration_buf[idx];

        temp_buf[idx]      = raw.temperature_c;
        current_buf[idx]   = raw.current_a;
        vibration_buf[idx] = raw.vibration_g;

        t_sum += raw.temperature_c;
        c_sum += raw.current_a;
        v_sum += raw.vibration_g;

        idx = (idx + 1) % FILTER_WINDOW_SIZE;
        if (count < FILTER_WINDOW_SIZE) count++;

        /* ---- Compute averages ---- */
        sensor_filtered_t filtered = {
            .temperature_c      = t_sum / count,
            .current_a          = c_sum / count,
            .vibration_g        = v_sum / count,
            .seq                = raw.seq,
            .samples_in_window  = (uint32_t)count,
        };

        /* ---- Forward to display task (non-blocking) ---- */
        if (s_output_queue != NULL) {
            xQueueSend(s_output_queue, &filtered, 0);
        }
    }
}
