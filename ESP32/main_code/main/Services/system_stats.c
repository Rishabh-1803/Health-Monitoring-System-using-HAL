/**
 * @file    system_stats.c
 * @brief   Runtime statistics for the dashboard and the CLI.
 */

#include "system_stats.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG __attribute__((unused)) = "SYS_STATS";

#define STATS_MAX_TASKS  24

#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
#define STATS_HAVE_CPU 1
#else
#define STATS_HAVE_CPU 0
#endif

static int      s_cpu = -1;
static uint32_t s_min_wm = 0;

#if STATS_HAVE_CPU
static TaskStatus_t s_tasks[STATS_MAX_TASKS];
static uint32_t s_prev_idle = 0u;    /* summed idle runtime           */
static uint32_t s_prev_total = 0u;   /* summed runtime of all tasks   */
#endif

void system_stats_poll(void)
{
#if STATS_HAVE_CPU
    uint32_t total = 0u;
    UBaseType_t n = uxTaskGetSystemState(s_tasks, STATS_MAX_TASKS, &total);
    if (n == 0u) {
        s_cpu = -1;
        return;
    }

    uint32_t idle = 0u;
    uint32_t min_wm = 0xFFFFFFFFu;
    for (UBaseType_t i = 0; i < n; i++) {
        /* ESP-IDF names its idle tasks "IDLE0"/"IDLE1". */
        if (strncmp(s_tasks[i].pcTaskName, "IDLE", 4) == 0) {
            idle += (uint32_t)s_tasks[i].ulRunTimeCounter;
        }
        if (s_tasks[i].usStackHighWaterMark < min_wm) {
            min_wm = s_tasks[i].usStackHighWaterMark;
        }
    }
    s_min_wm = min_wm;

    uint32_t d_total = total - s_prev_total;
    uint32_t d_idle  = idle  - s_prev_idle;
    s_prev_total = total;
    s_prev_idle  = idle;

    if (d_total > 0u) {
        int pct = 100 - (int)((d_idle * 100u) / d_total);
        if (pct < 0)   { pct = 0; }
        if (pct > 100) { pct = 100; }
        s_cpu = pct;
    }
#elif defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
    /* Watermarks still available without runtime counters. */
    TaskStatus_t tasks[STATS_MAX_TASKS];
    UBaseType_t n = uxTaskGetSystemState(tasks, STATS_MAX_TASKS, NULL);
    uint32_t min_wm = 0xFFFFFFFFu;
    for (UBaseType_t i = 0; i < n; i++) {
        if (tasks[i].usStackHighWaterMark < min_wm) {
            min_wm = tasks[i].usStackHighWaterMark;
        }
    }
    s_min_wm = (min_wm == 0xFFFFFFFFu) ? 0u : min_wm;
    s_cpu = -1;
#else
    (void)TAG;
    s_cpu = -1;
    s_min_wm = 0u;
#endif
}

int system_stats_cpu_load(void)
{
    return s_cpu;
}

void system_stats_dump_tasks_console(void)
{
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
    TaskStatus_t tasks[STATS_MAX_TASKS];
    UBaseType_t n = uxTaskGetSystemState(tasks, STATS_MAX_TASKS, NULL);
    if (n == 0u) {
        printf("task list unavailable\n");
        return;
    }
    printf("%-16s %-10s %-4s %-5s %s\n", "name", "state", "core", "prio",
           "stack-free-B");
    for (UBaseType_t i = 0; i < n; i++) {
        const char *st;
        switch ((int)tasks[i].eCurrentState) {
            case 0: st = "RUN"; break;      /* eRunning in IDF maps    */
            case 1: st = "RDY"; break;
            case 2: st = "BLKD"; break;
            case 3: st = "SUSP"; break;
            case 4: st = "DEL"; break;
            default: st = "?"; break;
        }
        printf("%-16s %-10s %-4d %-5u %u\n",
               tasks[i].pcTaskName, st,
               
#if defined(CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID)
               (int)tasks[i].xCoreID,
#else
               -1,
#endif
               (unsigned)tasks[i].uxCurrentPriority,
               (unsigned)tasks[i].usStackHighWaterMark);
    }
#else
    printf("task introspection needs CONFIG_FREERTOS_USE_TRACE_FACILITY\n");
#endif
}

uint32_t system_stats_min_stack_watermark(void)
{
    return s_min_wm;
}
