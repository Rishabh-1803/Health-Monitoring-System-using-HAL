/**
 * shared_config.h — Mutex-protected shared configuration.
 * Identical API to the ESP32 version.
 */
#ifndef SHARED_CONFIG_H
#define SHARED_CONFIG_H

#include "FreeRTOS.h"
#include "semphr.h"
#include <stdbool.h>

typedef struct {
    float    threshold_temp_c;
    float    threshold_current_a;
    float    threshold_vibration_g;
    uint32_t sample_period_ms;
    bool     alarms_enabled;
} shared_config_t;

void shared_config_init(void);
void shared_config_get(shared_config_t *out_config);
void shared_config_set(const shared_config_t *new_config);

#endif /* SHARED_CONFIG_H */
