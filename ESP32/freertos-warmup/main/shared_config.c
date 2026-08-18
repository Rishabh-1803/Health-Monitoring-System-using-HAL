/**
 * ============================================================
 * shared_config.c
 * ============================================================
 * Implementation of the shared-config mutex pattern.
 *
 * KEY POINTS:
 *   1. The mutex itself is static (file-private). Callers can't
 *      accidentally take/give it out of order — they can only
 *      go through shared_config_get/set.
 *   2. We hold the mutex for the SHORTEST time possible: just
 *      the memcpy. Don't do logging or long computation while
 *      holding a mutex — that blocks other tasks.
 *   3. We use xSemaphoreTake with a timeout (not portMAX_DELAY)
 *      so a buggy task can't deadlock the system forever.
 * ============================================================ */

#include "shared_config.h"
#include "project_config.h"
#include <string.h>

static shared_config_t s_config;
static SemaphoreHandle_t s_mutex = NULL;

/* Default values — reasonable for a generic "machine" */
static const shared_config_t s_default_config = {
    .threshold_temp_c      = 50.0f,
    .threshold_current_a   = 5.0f,
    .threshold_vibration_g = 2.0f,
    .sample_period_ms      = SENSOR_SAMPLE_PERIOD_MS,
    .alarms_enabled        = true,
};

void shared_config_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);
    memcpy(&s_config, &s_default_config, sizeof(s_config));
}

void shared_config_get(shared_config_t *out_config) {
    if (out_config == NULL || s_mutex == NULL) return;

    /* Wait up to 100 ticks for the mutex. If we can't get it
     * within that time, something is seriously wrong — log it
     * (in real project) and return stale or zeroed data. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(out_config, &s_config, sizeof(s_config));
        xSemaphoreGive(s_mutex);
    }
}

void shared_config_set(const shared_config_t *new_config) {
    if (new_config == NULL || s_mutex == NULL) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&s_config, new_config, sizeof(s_config));
        xSemaphoreGive(s_mutex);
    }
}
