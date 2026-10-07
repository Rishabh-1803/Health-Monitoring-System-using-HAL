/**
 * @file    task_dashboard.c
 * @brief   1 Hz heartbeat of the web dashboard.
 *
 * Every second it:
 *   1. samples the latest telemetry into the history ring (charts data)
 *   2. collects link + command counters into the status store
 *   3. records ESP32-side facts (heap, uptime, CPU from system_stats)
 *   4. builds the status JSON and broadcasts it over WebSocket
 *
 * Once a minute it appends the current sample to the LittleFS history
 * CSV (flash-friendly cadence), and once — the first time the STM32
 * link comes up after boot — it re-pushes the persisted settings so
 * the two nodes agree on thresholds and the sample rate, then asks
 * for a RESP_STATUS to confirm the mirror.
 */

#include "task_dashboard.h"
#include "dashboard_data.h"
#include "websocket_server.h"
#include "command_dispatcher.h"
#include "task_uart_rx.h"
#include "task_uart_tx.h"
#include "system_stats.h"
#include "littlefs_storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

static const char *TAG = "TASK_DASH";

#define DASH_STACK   8192
#define DASH_PRIO     4
#define DASH_PERIOD_MS 1000

#define HIST_FLASH_EVERY_S  60u      /* one CSV line per minute       */

static bool s_config_synced = false;
static uint32_t s_flash_counter = 0u;

static void push_link_stats(void)
{
    uint32_t rx = 0, crc = 0, alarms = 0, unknown = 0;
    task_uart_rx_get_stats(&rx, &crc, &alarms, &unknown);
    uint32_t tx = task_uart_tx_get_packets();

    uint32_t retries = 0, timeouts = 0;
    command_dispatcher_get_stats(&retries, &timeouts);

    dashboard_data_set_link_stats(rx, crc, tx, retries, timeouts);
}

static void sync_config_once(void)
{
    /* Wait until the STM32 is answering, then push the mirror once and
     * verify with a status request (its RESP_STATUS refreshes the
     * mirror from the STM32's own view). */
    if (s_config_synced || !dashboard_data_link_up()
        || !dashboard_data_ever_linked()) {
        return;
    }
    s_config_synced = true;

    float thr[3];
    dashboard_data_get_thresholds(thr);
    for (uint8_t id = 0; id < 3u; id++) {
        (void)command_set_threshold(id, thr[id], 900u);
    }
    (void)command_set_sample_rate(dashboard_data_get_sample_rate(), 900u);
    (void)command_get_status(900u);
    ESP_LOGI(TAG, "config sync pushed (rate=%u ms)",
             (unsigned)dashboard_data_get_sample_rate());
}

static void task_dashboard(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "dashboard task started (1 Hz)");

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    /* Static, not on the stack: the 3.5 KB buffer plus float formatting was
     * most of the old 6 KB stack. Only this task touches it. */
    static char json[3584];
    while (1) {
        esp_task_wdt_reset();

        dashboard_data_sample_tick();
        push_link_stats();
        dashboard_data_note_esp_stats(esp_get_free_heap_size(),
                                      esp_get_minimum_free_heap_size(),
                                      system_stats_cpu_load());
        sync_config_once();

        /* Flash history: one line per minute. */
        if (++s_flash_counter >= HIST_FLASH_EVERY_S) {
            s_flash_counter = 0u;
            hr_sample_t last;
            if (dashboard_data_copy_history_since(
                    dashboard_data_uptime_ms() - 2000u, &last, 1u) == 1u) {
                littlefs_storage_append_history(last.t_ms, last.v);
            }
        }

        size_t n = dashboard_data_build_status_json(json, sizeof(json));
        if (n > 0u && n < sizeof(json)) {
            websocket_server_broadcast(json, n);
        } else if (n >= sizeof(json)) {
            ESP_LOGE(TAG, "status JSON truncated (%u >= %u)",
                     (unsigned)n, (unsigned)sizeof(json));
        }

        vTaskDelay(pdMS_TO_TICKS(DASH_PERIOD_MS));
    }
}

void task_dashboard_start(void)
{
    xTaskCreate(task_dashboard, "dashboard", DASH_STACK, NULL,
                DASH_PRIO, NULL);
}
