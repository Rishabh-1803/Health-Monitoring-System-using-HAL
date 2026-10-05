/**
 * @file    main.c
 * @brief   Phase 2B ESP32 entry point — self-test + UART heartbeat exchange.
 *
 * Boot sequence:
 *   1. Print boot banner + chip info
 *   2. Run protocol self-test (proves CRC + packet layer on ESP32)
 *   3. Initialise UART1 (460800 8N1 on GPIO17/18)
 *   4. Create task_uart_rx (priority 5) — listens for STM32 packets
 *   5. Create task_uart_tx (priority 4) — sends heartbeats every 1s
 *   6. Return — ESP-IDF deletes the main task; our 2 tasks run forever
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "uart_link.h"
#include "task_uart_tx.h"
#include "task_uart_rx.h"
#include "protocol_selftest.h"

static const char *TAG = "APP_MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Industrial Monitor ESP32-S3");
    ESP_LOGI(TAG, "  Phase 2B — UART Heartbeat Exchange");
    ESP_LOGI(TAG, "========================================");

    /* Chip info */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "Chip: %s rev %d, %d cores",
             CONFIG_IDF_TARGET, chip.revision, chip.cores);
    ESP_LOGI(TAG, "Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());

    /* Run protocol self-test FIRST — proves the protocol layer before
     * we exchange real packets with the STM32. */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Running protocol self-test...");
    bool selftest_ok = protocol_selftest_run();
    if (!selftest_ok) {
        ESP_LOGE(TAG, "PROTOCOL SELF-TEST FAILED — do not proceed to heartbeat exchange");
        /* Don't start tasks — the user needs to see the failure first */
        return;
    }
    ESP_LOGI(TAG, "Protocol self-test passed — starting heartbeat exchange");

    /* Initialise UART link to STM32 */
    uart_link_init();

    /* Brief delay to let UART settle */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Start RX task FIRST (so it's ready before TX sends the first packet) */
    task_uart_rx_start();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Start TX task */
    task_uart_tx_start();

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Both tasks running. Heartbeat exchange active.");
    ESP_LOGI(TAG, "Wire: ESP32 GPIO17 (TX) -> STM32 PA3 (RX)");
    ESP_LOGI(TAG, "       ESP32 GPIO18 (RX) <- STM32 PA2 (TX)");
    ESP_LOGI(TAG, "       GND <-> GND");
    ESP_LOGI(TAG, "");

    /* app_main returns — ESP-IDF deletes the main task automatically.
     * Our 2 tasks (uart_tx, uart_rx) continue running forever. */
}
