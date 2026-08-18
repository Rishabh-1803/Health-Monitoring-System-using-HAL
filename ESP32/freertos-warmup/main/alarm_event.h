/**
 * ============================================================
 * alarm_event.h
 * ============================================================
 * Wraps the FreeRTOS Event Group used for alarm signaling.
 *
 * WHY AN EVENT GROUP (vs. a queue, vs. a mutex)?
 * ----------------------------------------------
 * Three reasons event groups are perfect for alarms:
 *
 *  1. BROADCAST: many tasks can wait on the same bit. When the
 *     alarm fires, every waiter wakes up at once. Queues are
 *     1-consumer (mostly) — only one task would get the signal.
 *
 *  2. BITWISE: 24 independent signals in one 24-bit group. You
 *     can wait on ANY combination ("wake me if overtemp OR
 *     overcurrent") with a single call.
 *
 *  3. LEVEL-TRIGGERED: once a bit is set, it stays set until
 *     explicitly cleared. A task that checks later still sees
 *     the alarm — unlike a queue where a late reader misses it.
 *
 * In the real project: alarm bits get set by Fault Detection
 * Task on STM32, and the Alarm Control Task + Logger Task +
 * Dashboard Task all react to them simultaneously.
 * ============================================================
 */

#ifndef ALARM_EVENT_H
#define ALARM_EVENT_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/**
 * Initialize the alarm event group. Call once at startup.
 */
void alarm_event_init(void);

/**
 * Set one or more alarm bits.
 * Safe to call from any task (and from ISRs with the _FromISR variant).
 *
 * @param bits  Bitwise OR of ALARM_BIT_xxx to set
 */
void alarm_event_set(uint32_t bits);

/**
 * Clear one or more alarm bits.
 * Typically called when an operator presses "Reset Alarm" button.
 *
 * @param bits  Bitwise OR of ALARM_BIT_xxx to clear
 */
void alarm_event_clear(uint32_t bits);

/**
 * Get the current state of all alarm bits (non-blocking).
 *
 * @return  Bitmask of currently-set alarm bits
 */
uint32_t alarm_event_get(void);

/**
 * Wait for any of the specified alarm bits to be set (blocking).
 *
 * @param bits          Which bits to wait for (OR them together)
 * @param clear_on_exit If true, clear the matched bits before returning
 * @param timeout_ms    Max time to wait (0 = don't wait, portMAX_DELAY = forever)
 * @return              Bits that were set when we woke up
 */
uint32_t alarm_event_wait(uint32_t bits, bool clear_on_exit, uint32_t timeout_ms);

#endif /* ALARM_EVENT_H */
