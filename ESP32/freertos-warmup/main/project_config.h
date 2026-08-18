/**
 * ============================================================
 * project_config.h
 * ============================================================
 * Central configuration for ALL FreeRTOS resources in this
 * warmup project. Centralizing here means:
 *   1. You can tune priorities / stack sizes from one place
 *   2. You can't accidentally create a queue with size 10 in
 *      one file and try to read 20 from another file
 *   3. It mirrors the pattern we'll use in the real project
 * ============================================================
 */

#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

/* ============================================================
 * TASK PRIORITIES
 * ============================================================
 * In FreeRTOS, HIGHER number = HIGHER priority.
 *
 * Design decision: Logger is highest priority (5) so log
 * messages never back up and starve the system. Sensor is
 * next (4) because fresh data is the whole point. Filter
 * and Display are consumers (lower priority). Stats is
 * lowest — it's just informational.
 *
 * Note: ESP-IDF's own WiFi/Bluetooth tasks run at priorities
 * 18-22, well above ours. That's intentional — those need
 * real-time responsiveness for the radio hardware.
 * ============================================================ */
#define TASK_LOGGER_PRIO        5
#define TASK_SENSOR_PRIO        4
#define TASK_FILTER_PRIO        3
#define TASK_DISPLAY_PRIO       2
#define TASK_STATS_PRIO         1

/* ============================================================
 * STACK SIZES (in WORDS, not bytes — ESP32 word = 4 bytes)
 * ============================================================
 * ESP32: stack is allocated from heap, measured in words.
 *   2048 words = 8192 bytes
 *
 * Rule of thumb: start generous, then use uxTaskGetStackHighWaterMark
 * to measure actual usage and trim. NEVER run a stack tight —
 * stack overflow is the #1 cause of mysterious FreeRTOS crashes.
 *
 * Logger needs more stack because vsnprintf() is stack-hungry.
 * ============================================================ */
#define TASK_SENSOR_STACK       2048
#define TASK_FILTER_STACK       2048
#define TASK_DISPLAY_STACK      2048
#define TASK_LOGGER_STACK       4096
#define TASK_STATS_STACK        4096

/* ============================================================
 * QUEUE LENGTHS
 * ============================================================
 * Each queue holds N items of fixed size.
 *   - Too small: producers block (or drop) under bursts
 *   - Too large: wastes heap, hides backpressure problems
 *
 * For sensor data flowing at 10 Hz, queue of 10 = 1 second
 * of buffer. Plenty.
 * ============================================================ */
#define SENSOR_QUEUE_LEN        10
#define DISPLAY_QUEUE_LEN       10
#define LOGGER_QUEUE_LEN        32

/* ============================================================
 * TIMING
 * ============================================================ */
#define SENSOR_SAMPLE_PERIOD_MS 100     // 10 Hz sampling
#define STATS_PRINT_PERIOD_MS   5000    // Print diagnostics every 5s

/* ============================================================
 * ALARM EVENT GROUP BITS
 * ============================================================
 * Event groups let multiple tasks wait on / signal bitwise flags.
 * Each "bit" is an independent signal — perfect for alarms where
 * you want one place to set them (sensor task) and many places
 * to check them (display, logger, future actuator tasks).
 *
 * Bit 0 = over-temperature alarm
 * Bit 1 = over-current alarm
 * Bit 2 = vibration alarm
 * ============================================================ */
#define ALARM_BIT_OVERTEMP      (1 << 0)
#define ALARM_BIT_OVERCURRENT   (1 << 1)
#define ALARM_BIT_VIBRATION     (1 << 2)
#define ALARM_BIT_ALL           (ALARM_BIT_OVERTEMP | \
                                 ALARM_BIT_OVERCURRENT | \
                                 ALARM_BIT_VIBRATION)

#endif /* PROJECT_CONFIG_H */
