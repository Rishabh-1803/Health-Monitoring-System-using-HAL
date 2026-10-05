/**
 * @file    system_stats.h
 * @brief   ESP32-side facts: CPU load, task stack watermarks, heap.
 *
 * CPU load uses the FreeRTOS runtime-statistics counters: sample
 * uxTaskGetSystemState() once a second, add up the IDLE tasks' run
 * time, and report 100% minus their share. Requires
 * CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y (sdkconfig.defaults); when
 * that is off, or the trace facility is off, the API returns -1 and
 * the dashboard shows "null" rather than a fabricated number.
 *
 * Stack watermarks in ESP-IDF are bytes (not the vanilla-FreeRTOS
 * words), which is what we report.
 */

#ifndef SYSTEM_STATS_H
#define SYSTEM_STATS_H

#include <stdint.h>

/** CPU load 0..100, or -1 when unavailable. */
int system_stats_cpu_load(void);

/** Bytes of the smallest task-stack watermark (0 = unknown). */
uint32_t system_stats_min_stack_watermark(void);

/** Print the per-task table (name/state/core/prio/watermark) to stdout. */
void system_stats_dump_tasks_console(void);

/** Poll once (call at ~1 Hz from task_dashboard); refreshes caches. */
void system_stats_poll(void);

#endif /* SYSTEM_STATS_H */
