/**
 * shared_config.c — STM32 port. Same mutex pattern as ESP32.
 */
#include "shared_config.h"
#include "project_config.h"
#include <string.h>

static shared_config_t s_config;
static SemaphoreHandle_t s_mutex = NULL;

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
