/**
 * @file    task_watchdog.c
 * @brief   Layer 1: hardware TWDT. Layer 2: application supervisor.
 *
 * Layer 1 — the task watchdog (esp_task_wdt) reconfigured to 15 s with
 * panic-on-timeout. Subscriptions are opt-in per task: uart_rx, uart_tx
 * and the dashboard task reset it every pass, so a real hang in any of
 * them resets the chip within 15 s. Event-driven tasks (httpd thread,
 * REPL, wifi) are deliberately NOT subscribed — waiting for a client
 * is not a fault.
 *
 * Layer 2 — the supervisor polls every 5 s:
 *   - If the STM32 was linked at least once and has now been silent
 *     60 s: try MSG_CMD_REBOOT (maybe it hung in a state that still
 *     services UART RX), then wait another 60 s.
 *   - Still silent after the escalation: reboot the ESP32 too. A node
 *     that cannot see its sensor head has nothing to serve.
 *   - If the STM32 was NEVER linked (bench setup, STM32 in the menu),
 *     no escalation ever fires — the board is not a production node.
 *   - Free heap under 15 KB: log an error once per 5 minutes.
 */

#include "task_watchdog.h"
#include "dashboard_data.h"
#include "command_dispatcher.h"
#include "logger.h"

#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WATCHDOG";

#define WDT_TASK_STACK   3072
#define WDT_TASK_PRIO     2
#define SUPERVISE_MS    5000

#define STM32_SILENT_SOFT_MS   60000u   /* try rebooting the STM32     */
#define STM32_SILENT_HARD_MS  120000u   /* then reboot ourselves       */
#define HEAP_LOW_BYTES         15360u
#define HEAP_LOG_PERIOD_MS   300000u

static void task_watchdog(void *arg)
{
    (void)arg;

    /* Layer 1: hardware watchdog, 15 s, panic. */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 15000,
        .idle_core_mask = 0,          /* idle tasks NOT watched: a busy
                                       * app task should panic, not the
                                       * idle-starvation heuristic    */
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (err != ESP_OK) {
        /* Init-not-done path (CONFIG off): try init, else log once. */
        err = esp_task_wdt_init(&wdt_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TWDT unavailable (0x%x) — supervisor only", err);
        }
    }
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "supervisor started (5 s poll, TWDT 15 s panic)");

    uint32_t silent_since = 0u;       /* 0 = link currently healthy    */
    bool escalated = false;
    uint32_t last_heap_warn = 0u;

    while (1) {
        esp_task_wdt_reset();

        uint32_t now = dashboard_data_uptime_ms();
        uint32_t age = dashboard_data_telemetry_age_ms();

        if (dashboard_data_ever_linked()) {
            if (age == UINT32_MAX || age > 30000u) {
                if (silent_since == 0u) {
                    silent_since = now;
                    logger_logf(LOG_LVL_WARN, "supervisor",
                                "STM32 silent (age %s)",
                                (age == UINT32_MAX) ? "?" : "30s+");
                }
                uint32_t silent_ms = now - silent_since;
                if (!escalated && silent_ms > STM32_SILENT_SOFT_MS) {
                    escalated = true;
                    logger_logf(LOG_LVL_ERR, "supervisor",
                                "STM32 silent %lus — sending reboot",
                                (unsigned long)(silent_ms / 1000u));
                    (void)command_reboot_stm32(3000u);
                } else if (escalated
                           && silent_ms > STM32_SILENT_HARD_MS) {
                    logger_logf(LOG_LVL_ERR, "supervisor",
                                "STM32 silent %lus — rebooting ESP32",
                                (unsigned long)(silent_ms / 1000u));
                    logger_flush();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            } else {
                if (silent_since != 0u) {
                    logger_logf(LOG_LVL_INFO, "supervisor",
                                "STM32 link recovered");
                }
                silent_since = 0u;
                escalated = false;
            }
        }

        if (esp_get_free_heap_size() < HEAP_LOW_BYTES
            && (now - last_heap_warn) > HEAP_LOG_PERIOD_MS) {
            last_heap_warn = now;
            logger_logf(LOG_LVL_ERR, "supervisor", "heap low: %lu bytes",
                        (unsigned long)esp_get_free_heap_size());
        }

        vTaskDelay(pdMS_TO_TICKS(SUPERVISE_MS));
    }
}

void task_watchdog_start(void)
{
    xTaskCreate(task_watchdog, "supervisor", WDT_TASK_STACK, NULL,
                WDT_TASK_PRIO, NULL);
}
