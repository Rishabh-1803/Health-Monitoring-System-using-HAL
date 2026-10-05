/**
 * @file    history_ring.h
 * @brief   Fixed-size ring buffer of decimated telemetry samples.
 *
 * Pure C: no FreeRTOS, no ESP-IDF, so the same file compiles in the
 * firmware AND in the host unit tests (tests/host/).
 *
 * The dashboard keeps a 1 Hz view of the telemetry stream (the STM32
 * samples faster; task_dashboard decimates before pushing). 3600 slots
 * cover one hour. Ordering: index 0 is always the OLDEST retained
 * sample, so clients can walk forward in time.
 */

#ifndef HISTORY_RING_H
#define HISTORY_RING_H

#include <stdint.h>
#include <stdbool.h>

#define HR_CHANNELS 3   /* temperature, current, vibration */

typedef struct {
    uint32_t t_ms;                  /* ESP32 uptime milliseconds          */
    float    v[HR_CHANNELS];        /* channel values                     */
} hr_sample_t;

typedef struct {
    hr_sample_t *buf;               /* storage, owned by the caller       */
    uint16_t     cap;               /* capacity (power of 2 not required) */
    uint16_t     head;              /* next write slot                    */
    uint16_t     count;             /* valid samples (<= cap)             */
} history_ring_t;

/**
 * Bind storage to the ring and empty it.
 * @param storage  caller-owned array of @p cap samples
 */
void history_ring_init(history_ring_t *r, hr_sample_t *storage, uint16_t cap);

/** Append one sample; overwrites the oldest when full. */
void history_ring_push(history_ring_t *r, uint32_t t_ms, const float *vals);

/** Number of retained samples. */
uint16_t history_ring_count(const history_ring_t *r);

/** Read the sample at index 0..count-1 (0 = oldest). NULL if out of range. */
const hr_sample_t *history_ring_at(const history_ring_t *r, uint16_t idx);

/**
 * Copy samples with t_ms >= since_ms into @p out (oldest first).
 * @return number of samples copied (bounded by @p max)
 */
uint16_t history_ring_copy_since(const history_ring_t *r, uint32_t since_ms,
                                 hr_sample_t *out, uint16_t max);

#endif /* HISTORY_RING_H */
