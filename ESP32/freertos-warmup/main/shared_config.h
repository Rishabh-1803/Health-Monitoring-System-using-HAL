/**
 * ============================================================
 * shared_config.h
 * ============================================================
 * Demonstrates the MUTEX pattern for protecting shared state.
 *
 * WHY A MUTEX?
 * --------------------------------
 * If multiple tasks can read AND write the same struct, you
 * have a race condition. Example: Task A reads threshold.temp
 * while Task B is mid-update — A gets a torn value (high word
 * from old value, low word from new value).
 *
 * A mutex guarantees only one task can touch the struct at a
 * time. Readers and writers all take the mutex first.
 *
 * MUTEX vs. QUEUE?
 * --------------------------------
 * - Use a QUEUE for streaming data (one producer, one consumer)
 * - Use a MUTEX for shared STATE (multiple readers / writers,
 *   latest value always wins, no history kept)
 *
 * In the real project: thresholds, sampling rate, alarm enable
 * flags all live in a shared_config struct protected by a mutex.
 * ============================================================
 */

#ifndef SHARED_CONFIG_H
#define SHARED_CONFIG_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * System-wide configuration that any task can read,
 * and that the CLI / web dashboard can modify.
 *
 * In the warmup, we don't actually have a CLI yet, but
 * TaskSensor modifies these occasionally to simulate
 * runtime config changes (so you can see the mutex in action).
 */
typedef struct {
    float    threshold_temp_c;      // Alarm if temp exceeds this
    float    threshold_current_a;   // Alarm if current exceeds this
    float    threshold_vibration_g; // Alarm if vibration exceeds this
    uint32_t sample_period_ms;      // Sensor sample rate (modifies behavior)
    bool     alarms_enabled;        // Master alarm enable/disable
} shared_config_t;

/**
 * Initialize the shared config + its mutex.
 * MUST be called once before any task uses the config.
 */
void shared_config_init(void);

/**
 * Thread-safe read of the config.
 * Copies the current values into the caller's struct.
 *
 * @param out_config  Pointer to caller's shared_config_t to fill
 */
void shared_config_get(shared_config_t *out_config);

/**
 * Thread-safe write of the config.
 * Copies the caller's values into the shared struct.
 *
 * @param new_config  Pointer to new values to apply
 */
void shared_config_set(const shared_config_t *new_config);

#endif /* SHARED_CONFIG_H */
