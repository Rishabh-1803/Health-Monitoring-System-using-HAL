/**
 * ============================================================
 * sensor_data.h
 * ============================================================
 * Defines the data structures passed between tasks via queues.
 *
 * WHY A STRUCT (not just int)?
 * --------------------------------
 * Real sensor data is multi-dimensional: temp, current, vibration
 * all come together at one instant. Sending them as a struct:
 *   1. Keeps them time-aligned (a temp reading from t=100ms must
 *      not get paired with current from t=150ms)
 *   2. Is atomic from the consumer's view — one queue send =
 *      one complete sample
 *   3. Scales: add a field later (e.g., timestamp) without
 *      touching the queue mechanism
 * ============================================================
 */

#ifndef SENSOR_DATA_H
#define SENSOR_DATA_H

#include <stdint.h>

/**
 * Raw sensor sample coming out of the simulated sensor task.
 * In the real project, this struct will be populated by the
 * sensor drivers (DS18B20, INA219, MPU6050).
 */
typedef struct {
    float    temperature_c;   // Temperature in Celsius
    float    current_a;       // Current in Amperes
    float    vibration_g;     // Vibration in g (acceleration)
    uint32_t seq;             // Monotonic sequence number (for debugging drops)
    uint32_t tick;            // FreeRTOS tick at sampling time
} sensor_data_t;

/**
 * Filtered sample produced by TaskFilter.
 *
 * Same fields as raw, but values are smoothed (running average).
 * The `samples_in_window` field tells the consumer how many
 * samples contributed to the average — useful for early-startup
 * when the window isn't full yet.
 */
typedef struct {
    float    temperature_c;
    float    current_a;
    float    vibration_g;
    uint32_t seq;
    uint32_t samples_in_window;  // How many samples went into this average
} sensor_filtered_t;

#endif /* SENSOR_DATA_H */
