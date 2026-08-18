/**
 * ============================================================
 * project_config.h — STM32 FreeRTOS Warmup
 * ============================================================
 * Central config for all FreeRTOS resources.
 * Same pattern as ESP32 warmup — only the platform differs.
 * ============================================================
 */

#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"

/* ---- TASK PRIORITIES ----
 * Note: STM32 FreeRTOS (CubeMX default) allows priorities 0..31.
 * Higher number = higher priority (same as ESP32).
 * Idle task is priority 0. Don't use priority 0 for your tasks. */
#define TASK_LOGGER_PRIO        5
#define TASK_SENSOR_PRIO        4
#define TASK_FILTER_PRIO        3
#define TASK_DISPLAY_PRIO       2
#define TASK_STATS_PRIO         1

/* ---- STACK SIZES (in WORDS — STM32 word = 4 bytes) ----
 * CubeMX's default configMINIMAL_STACK_SIZE is 128 words (512 bytes).
 * We use larger stacks because vsnprintf and float formatting are
 * stack-hungry. */
#define TASK_SENSOR_STACK       512
#define TASK_FILTER_STACK       512
#define TASK_DISPLAY_STACK      512
#define TASK_LOGGER_STACK       1024
#define TASK_STATS_STACK        1024

/* ---- QUEUE LENGTHS ---- */
#define SENSOR_QUEUE_LEN        10
#define DISPLAY_QUEUE_LEN       10
#define LOGGER_QUEUE_LEN        32

/* ---- TIMING ----
 * CubeMX default tick rate is 1000 Hz (1 ms per tick).
 * pdMS_TO_TICKS() handles the conversion for us, so this works
 * regardless of tick rate. */
#define SENSOR_SAMPLE_PERIOD_MS 100
#define STATS_PRINT_PERIOD_MS   5000

/* ---- ALARM EVENT GROUP BITS ---- */
#define ALARM_BIT_OVERTEMP      (1 << 0)
#define ALARM_BIT_OVERCURRENT   (1 << 1)
#define ALARM_BIT_VIBRATION     (1 << 2)
#define ALARM_BIT_ALL           (ALARM_BIT_OVERTEMP | \
                                 ALARM_BIT_OVERCURRENT | \
                                 ALARM_BIT_VIBRATION)

#endif /* PROJECT_CONFIG_H */
