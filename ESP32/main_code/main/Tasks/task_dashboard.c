/**
 * @file    task_dashboard.c
 * @brief   1 Hz heartbeat of the web dashboard.
 *
 * Every second it:
 *   1. samples the latest telemetry into the history ring (charts data)
 *   2. collects link + command counters into the status store
 *   3. records ESP32-side facts (heap, uptime)
 *   4. builds the status JSON and broadcasts it over WebSocket
 *
 * Also, once — the first time the STM32 link comes up after boot — it
 * re-pushes any locally-persisted settings so the two nodes agree on
 * thresholds and the sample rate. (Persistence itself lands with the
 * settings store; until then the push uses the in-memory mirror.)
 */

#include "task_dashboard.h"
#include "dashboard_data.h"
#include "websocket_server.h"
#include "command_dispatcher.h"
#include "task_uart_rx.h"
#include "task_uart_tx.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "TASK_DASH";

#define DASH_STACK   6144
#define DASH_PRIO     4
#define DASH_PERIOD_MS 1000

static bool s_config_synced = false;

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
    /* Wait until the STM32 is answering, then push the mirror once. */
    if (s_config_synced || !dashboard_data_link_up()
        || !dashboard_data_ever_linked()) {
        return;
    }
    s_config_synced = true;

    bool any = false;
    for (uint8_t id = 0; id < 3u; id++) {
        float v = dashboard_data_get_threshold(id);
        if (v > 0.0f) {
            cmd_result_t r = command_set_threshold(id, v, 900u);
            any = any || (r == CMD_OK);
        }
    }
    uint16_t rate = dashboard_data_get_sample_rate();
    (void)command_set_sample_rate(rate, 900u);
    ESP_LOGI(TAG, "config sync %s (rate=%u ms)",
             any ? "pushed" : "not acked — STM32 defaults stand",
             (unsigned)rate);
}

static void task_dashboard(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "dashboard task started (1 Hz)");

    char json[2400];
    while (1) {
        dashboard_data_sample_tick();
        push_link_stats();
        dashboard_data_note_esp_stats(esp_get_free_heap_size(),
                                      esp_get_minimum_free_heap_size(),
                                      -1);    /* CPU: system_stats (P7)  */
        sync_config_once();

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
