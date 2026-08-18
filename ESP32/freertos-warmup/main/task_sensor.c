/**
 * ============================================================
 * task_sensor.c
 * ============================================================
 * TaskSensor — simulated industrial sensor acquisition.
 *
 * WHAT IT TEACHES:
 *   1. vTaskDelayUntil() vs vTaskDelay()  ← CRITICAL
 *   2. Sending a STRUCT through a queue
 *   3. Setting event group bits on threshold violation
 *   4. Reading shared config under mutex
 *
 * vTaskDelayUntil — THE PERIODIC SCHEDULING PATTERN
 * -------------------------------------------------
 * vTaskDelay(100)  → "wait 100ms from NOW"
 *   Problem: if the rest of your task body takes 30ms, your
 *   actual period becomes 130ms. Drift accumulates. After
 *   10 cycles you're 300ms behind schedule.
 *
 * vTaskDelayUntil(&last_wake, 100) → "wake up 100ms after
 *   the LAST wake time"
 *   The scheduler compensates for the work time. If your body
 *   takes 30ms, you wait only 70ms. Period stays exactly 100ms
 *   forever (modulo scheduler tick granularity — 1ms on ESP32).
 *
 * For industrial sampling, ALWAYS use vTaskDelayUntil.
 * ============================================================ */

#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "project_config.h"
#include "sensor_data.h"
#include "shared_config.h"
#include "alarm_event.h"
#include "log_queue.h"

/* This queue is created in main.c and passed in via task param.
 * We could also use a file-scope static, but passing via param
 * is cleaner and matches the "no hidden globals" rule. */
static QueueHandle_t s_sensor_queue = NULL;

void task_sensor_set_queue(QueueHandle_t q) {
    s_sensor_queue = q;
}

void task_sensor(void *arg) {
    (void)arg;

    log_send(LOG_LEVEL_INFO, "SENSOR", "TaskSensor started");

    /* ---- Initialize the periodic timing variables ----
     * last_wake_time MUST be initialized to current tick BEFORE
     * the first vTaskDelayUntil call. This is a common bug source. */
    TickType_t last_wake = xTaskGetTickCount();

    uint32_t seq = 0;

    /* ---- Simulated physical values ----
     * In the real project these come from sensor drivers.
     * Here we use sine waves + noise so the filter task has
     * something interesting to smooth. */
    float t = 0.0f;

    while (1) {
        seq++;
        t += 0.1f;  // 0.1 rad per sample = ~1.6s per cycle

        /* ---- Simulate a slowly varying temperature ----
         * Baseline 35°C, +/- 5°C, with noise.
         * Occasionally spike above 50°C threshold to trigger alarm. */
        float temp = 35.0f + 8.0f * sinf(t * 0.3f) +
                     ((float)rand() / RAND_MAX - 0.5f) * 2.0f;

        /* Occasionally inject a fault spike */
        if ((seq % 50) == 0) {
            temp += 15.0f;  // Will exceed threshold — triggers overtemp alarm
        }

        /* ---- Simulate current ----
         * Baseline 3A, oscillates a bit, occasionally spikes. */
        float current = 3.0f + 0.5f * sinf(t * 0.7f) +
                        ((float)rand() / RAND_MAX - 0.5f) * 0.3f;

        if ((seq % 70) == 0) {
            current += 3.0f;  // Spike above 5A threshold
        }

        /* ---- Simulate vibration ----
         * Baseline 1g, noise-driven. Spikes occasionally. */
        float vibration = 1.0f + 0.3f * sinf(t * 1.1f) +
                          ((float)rand() / RAND_MAX - 0.5f) * 0.5f;

        if ((seq % 90) == 0) {
            vibration += 1.5f;  // Spike above 2g threshold
        }

        /* ---- Build the sample struct ---- */
        sensor_data_t sample = {
            .temperature_c = temp,
            .current_a     = current,
            .vibration_g   = vibration,
            .seq           = seq,
            .tick          = xTaskGetTickCount(),
        };

        /* ---- Send to queue (non-blocking) ----
         * Timeout = 0 means: if queue is full, drop immediately.
         * For a 10Hz sensor and queue of 10, this should never happen.
         * If it does, the filter task is too slow — log it. */
        if (s_sensor_queue != NULL) {
            if (xQueueSend(s_sensor_queue, &sample, 0) != pdTRUE) {
                log_send(LOG_LEVEL_WARN, "SENSOR",
                         "Queue full, dropping sample seq=%lu", seq);
            }
        }

        /* ---- Check thresholds and set alarm bits ----
         * Read config under mutex (cheap — just a memcpy). */
        shared_config_t cfg;
        shared_config_get(&cfg);

        if (cfg.alarms_enabled) {
            if (temp       > cfg.threshold_temp_c)      alarm_event_set(ALARM_BIT_OVERTEMP);
            if (current    > cfg.threshold_current_a)   alarm_event_set(ALARM_BIT_OVERCURRENT);
            if (vibration  > cfg.threshold_vibration_g) alarm_event_set(ALARM_BIT_VIBRATION);
        }

        /* ---- THE critical periodic scheduling call ----
         * This task will wake up exactly SENSOR_SAMPLE_PERIOD_MS
         * after last_wake, regardless of how long the body took. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SENSOR_SAMPLE_PERIOD_MS));
    }
}
