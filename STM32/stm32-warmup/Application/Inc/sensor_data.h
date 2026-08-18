/**
 * ============================================================
 * sensor_data.h — STM32 FreeRTOS Warmup
 * ============================================================
 * Data structures passed between tasks via queues.
 * Identical to the ESP32 version — portability achieved by
 * sticking to standard C and FreeRTOS APIs.
 * ============================================================
 */

#ifndef SENSOR_DATA_H
#define SENSOR_DATA_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float    temperature_c;
    float    current_a;
    float    vibration_g;
    uint32_t seq;
    uint32_t tick;
} sensor_data_t;

typedef struct {
    float    temperature_c;
    float    current_a;
    float    vibration_g;
    uint32_t seq;
    uint32_t samples_in_window;
} sensor_filtered_t;

#endif /* SENSOR_DATA_H */
