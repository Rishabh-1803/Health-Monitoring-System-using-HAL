/**
 * ============================================================
 * task_sensor.c — STM32 port
 * ============================================================
 * Same logic as ESP32 version. Simulated sensor data:
 *   - Sine wave + noise for temperature, current, vibration
 *   - Periodic spikes to trigger alarms
 *   - Thresholds checked against shared_config
 *   - Alarm bits set in the event group
 *
 * When your real hardware (DS18B20, INA219, MPU6050) arrives,
 * you'll replace the sin/noise simulation with real driver calls.
 * The downstream tasks (filter, display, logger) won't change.
 * ============================================================
 */

#include <math.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "project_config.h"
#include "sensor_data.h"
#include "shared_config.h"
#include "alarm_event.h"
#include "log_queue.h"
#include "task_sensor.h"

static QueueHandle_t s_sensor_queue = NULL;

void task_sensor_set_queue(QueueHandle_t q) {
    s_sensor_queue = q;
}

void task_sensor(void *arg) {
    (void)arg;

    log_send(LOG_LEVEL_INFO, "SENSOR", "TaskSensor started");

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t seq = 0;
    float t = 0.0f;

    /* STM32F411 has no built-in RNG. Seed the C library rand() with
     * a fixed value so behavior is repeatable during debugging. */
    srand(0xCAFEBABE);

    while (1) {
        seq++;
        t += 0.1f;

        /* Simulated sensor values (same as ESP32 version) */
        float temp = 35.0f + 8.0f * sinf(t * 0.3f) +
                     ((float)rand() / RAND_MAX - 0.5f) * 2.0f;
        if ((seq % 50) == 0) temp += 15.0f;

        float current = 3.0f + 0.5f * sinf(t * 0.7f) +
                        ((float)rand() / RAND_MAX - 0.5f) * 0.3f;
        if ((seq % 70) == 0) current += 3.0f;

        float vibration = 1.0f + 0.3f * sinf(t * 1.1f) +
                           ((float)rand() / RAND_MAX - 0.5f) * 0.5f;
        if ((seq % 90) == 0) vibration += 1.5f;

        sensor_data_t sample = {
            .temperature_c = temp,
            .current_a     = current,
            .vibration_g   = vibration,
            .seq           = seq,
            .tick          = xTaskGetTickCount(),
        };

        if (s_sensor_queue != NULL) {
            if (xQueueSend(s_sensor_queue, &sample, 0) != pdTRUE) {
                log_send(LOG_LEVEL_WARN, "SENSOR",
                         "Queue full, dropping sample seq=%lu", seq);
            }
        }

        shared_config_t cfg;
        shared_config_get(&cfg);

        if (cfg.alarms_enabled) {
            if (temp       > cfg.threshold_temp_c)      alarm_event_set(ALARM_BIT_OVERTEMP);
            if (current    > cfg.threshold_current_a)   alarm_event_set(ALARM_BIT_OVERCURRENT);
            if (vibration  > cfg.threshold_vibration_g) alarm_event_set(ALARM_BIT_VIBRATION);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SENSOR_SAMPLE_PERIOD_MS));
    }
}
