/**
 * task_filter.c — STM32 port. 5-sample moving average via ring buffer.
 * Identical logic to ESP32 version.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "project_config.h"
#include "sensor_data.h"
#include "log_queue.h"
#include "task_filter.h"

#define FILTER_WINDOW_SIZE  5

static QueueHandle_t s_input_queue  = NULL;
static QueueHandle_t s_output_queue = NULL;

void task_filter_set_queues(QueueHandle_t in, QueueHandle_t out) {
    s_input_queue  = in;
    s_output_queue = out;
}

void task_filter(void *arg) {
    (void)arg;

    log_send(LOG_LEVEL_INFO, "FILTER", "TaskFilter started");

    float temp_buf[FILTER_WINDOW_SIZE]      = {0};
    float current_buf[FILTER_WINDOW_SIZE]   = {0};
    float vibration_buf[FILTER_WINDOW_SIZE] = {0};
    int   idx    = 0;
    int   count  = 0;
    float t_sum  = 0, c_sum = 0, v_sum = 0;

    sensor_data_t raw;

    while (1) {
        if (xQueueReceive(s_input_queue, &raw, portMAX_DELAY) != pdTRUE) {
            continue;
        }

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

        sensor_filtered_t filtered = {
            .temperature_c      = t_sum / count,
            .current_a          = c_sum / count,
            .vibration_g        = v_sum / count,
            .seq                = raw.seq,
            .samples_in_window  = (uint32_t)count,
        };

        if (s_output_queue != NULL) {
            xQueueSend(s_output_queue, &filtered, 0);
        }
    }
}
