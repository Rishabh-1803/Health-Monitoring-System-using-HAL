/**
 * @file    task_diagnostics.c
 * @brief   10 s health sweep: CPU, watermarks, storage heartbeat.
 *
 * Keeps dashboard_data's ESP32 block fresh between the 1 Hz pushes
 * (CPU load is computed here, not in the dashboard task, so a slow
 * uxTaskGetSystemState never delays the WS push) and records a
 * one-line health summary once a minute.
 */

#include "task_diagnostics.h"
#include "dashboard_data.h"
#include "system_stats.h"
#include "logger.h"
#include "websocket_server.h"
#include "littlefs_storage.h"

#include "esp_system.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG __attribute__((unused)) = "DIAG";

#define DIAG_STACK   4096
#define DIAG_PRIO     2
#define DIAG_MS     10000

static void task_diagnostics(void *arg)
{
    (void)arg;
    uint32_t sweeps = 0u;

    while (1) {
        system_stats_poll();          /* refresh CPU + watermark caches */

        if ((sweeps % 6u) == 0u) {    /* once a minute */
            logger_logf(LOG_LVL_INFO, "diag",
                        "heap %lu KB  min-wm %lu B  ws %d  cpu %d%%",
                        (unsigned long)(esp_get_free_heap_size() / 1024u),
                        (unsigned long)system_stats_min_stack_watermark(),
                        websocket_server_client_count(),
                        system_stats_cpu_load());
            logger_flush();           /* 5-min cadence inside storage  */
        }

        vTaskDelay(pdMS_TO_TICKS(DIAG_MS));
        sweeps++;
    }
}

void task_diagnostics_start(void)
{
    xTaskCreate(task_diagnostics, "diag", DIAG_STACK, NULL,
                DIAG_PRIO, NULL);
}
