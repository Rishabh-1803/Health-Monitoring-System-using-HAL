/**
 * @file    main.c
 * @brief   ESP32-S3 entry point — Industrial Equipment Health Monitor.
 *
 * Boot sequence (phases light up in order; each is independently
 * testable from the console before the next starts):
 *
 *   1. Banner + chip info
 *   2. Protocol self-test (CRC + packet layer, identical to the STM32)
 *   3. UART link init (460800 8N1 on GPIO17/18) + RX/TX tasks
 *   4. dashboard_data + command dispatcher (telemetry pipeline)
 *   5. WiFi manager (STA; provisioning AP when no credentials)
 *   6. HTTP + WebSocket server (once an IP exists)
 *   7. CLI (esp_console REPL on the USB serial console)
 *   8. Watchdog supervisor + diagnostics + logger + storage
 *
 * app_main returns — ESP-IDF deletes the main task; the FreeRTOS tasks
 * created above keep running forever.
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "uart_link.h"
#include "task_uart_tx.h"
#include "task_uart_rx.h"
#include "task_dashboard.h"
#include "task_wifi.h"
#include "task_webserver.h"
#include "protocol_selftest.h"
#include "dashboard_data.h"
#include "command_dispatcher.h"
#include "wifi_manager.h"
#include "http_server.h"
#include "task_cli.h"
#include "task_watchdog.h"
#include "task_diagnostics.h"
#include "task_logger.h"
#include "logger.h"
#include "littlefs_storage.h"

static const char *TAG = "APP_MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Industrial Monitor ESP32-S3");
    ESP_LOGI(TAG, "  Phase 7 — Watchdog + diagnostics + storage");
    ESP_LOGI(TAG, "========================================");

    /* Chip info */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "Chip: %s rev %d, %d cores",
             CONFIG_IDF_TARGET, chip.revision, chip.cores);
    ESP_LOGI(TAG, "Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());

    /* 2. Protocol self-test FIRST — proves the protocol layer before
     * we exchange real packets with the STM32. */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Running protocol self-test...");
    bool selftest_ok = protocol_selftest_run();
    if (!selftest_ok) {
        ESP_LOGE(TAG, "PROTOCOL SELF-TEST FAILED — halting (no tasks started)");
        return;
    }
    ESP_LOGI(TAG, "Protocol self-test passed");

    /* 4. State stores must exist before the tasks that feed them. */
    dashboard_data_init();
    command_dispatcher_init();
    logger_init();

    /* 5. NVS first (WiFi credentials live there), then the wifi stack.
     * task_webserver starts httpd once an IP (or the provisioning AP)
     * exists, and task_dashboard pushes 1 Hz status to browsers. */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES
        || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs reformat — erasing (wifi creds lost)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    wifi_manager_init();

    /* 8. Optional flash storage + persisted settings. The mirror seeds
     * from what was stored; task_dashboard pushes it to the STM32 once
     * the link is up (and RESP_STATUS verifies the result). */
    littlefs_storage_mount();
    float thr[3];
    uint16_t rate = 200u;
    if (littlefs_storage_load_config(thr, &rate)) {
        dashboard_data_set_config(thr, rate);
        ESP_LOGI(TAG, "stored config applied (rate %u ms)", (unsigned)rate);
    }
    logger_logf(LOG_LVL_INFO, "boot",
                "ESP32 booted, reset reason %d", (int)esp_reset_reason());

    /* 3. UART link + tasks. RX first so it is listening before TX. */
    uart_link_init();
    vTaskDelay(pdMS_TO_TICKS(100));
    task_uart_rx_start();
    vTaskDelay(pdMS_TO_TICKS(50));
    task_uart_tx_start();

    task_dashboard_start();
    task_webserver_start();
    task_wifi_start();
    task_cli_start();
    task_watchdog_start();
    task_diagnostics_start();
    task_logger_start();

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "All services running. Telemetry + dashboard active.");
    ESP_LOGI(TAG, "Wire: ESP32 GPIO17 (TX) -> STM32 PA3 (RX)");
    ESP_LOGI(TAG, "       ESP32 GPIO18 (RX) <- STM32 PA2 (TX)");
    ESP_LOGI(TAG, "       GND <-> GND");
    ESP_LOGI(TAG, "");
}
