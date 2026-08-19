/**
 * ============================================================
 * main.c — Industrial Monitor ESP32-S3 entry point
 * ============================================================
 *
 * Phase 1: Boot banner only. All tasks/services/drivers are
 * stubs that will be filled in by Phases 2-9.
 *
 * After Phase 2, app_main() will:
 *   1. Print reset reason
 *   2. Init NVS + LittleFS
 *   3. Init UART1 driver
 *   4. Init log_queue + event groups + mutexes
 *   5. Create all 9 tasks
 *   6. Return (ESP-IDF deletes main task automatically)
 * ============================================================ */

#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "project_config.h"

static const char *TAG = "APP_MAIN";

void app_main(void) {
    /* Print boot banner */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Industrial Monitor ESP32-S3");
    ESP_LOGI(TAG, "  Phase 1 — Skeleton Ready");
    ESP_LOGI(TAG, "========================================");

    /* Print chip info (helpful for verifying we're on the right target) */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s (%d cores, %s rev %d)",
        CONFIG_IDF_TARGET,
        chip_info.cores,
        (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi" : "no WiFi",
        chip_info.revision);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Flash: %lu MB %s",
            (unsigned long)(flash_size / (1024 * 1024)),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ?
                "embedded" : "external");
    ESP_LOGI(TAG, "Free heap at boot: %lu bytes",
             (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "FreeRTOS tick rate: %d Hz", configTICK_RATE_HZ);

    /* Print reset reason — Phase 9 will log this to LittleFS too */
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d (%s)",
             reason,
             (reason == ESP_RST_POWERON)    ? "Power-on"      :
             (reason == ESP_RST_EXT)         ? "External"      :
             (reason == ESP_RST_SW)          ? "Software"      :
             (reason == ESP_RST_PANIC)       ? "Panic"         :
             (reason == ESP_RST_INT_WDT)     ? "Int WDT"       :
             (reason == ESP_RST_TASK_WDT)    ? "Task WDT"      :
             (reason == ESP_RST_BROWNOUT)    ? "Brownout"      :
                                               "Unknown");

    ESP_LOGI(TAG, "See docs/ARCHITECTURE.md and docs/UART_PROTOCOL_SPEC.md");
    ESP_LOGI(TAG, "Phase 2 will implement the UART link to STM32");

    /* TODO: Phase 2 — initialize all subsystems and create 9 tasks.
     * For now, app_main returns and ESP-IDF deletes the main task. */

    /* Briefly keep main alive so the monitor shows our banner before exit */
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Phase 1 skeleton exiting — flash Phase 2 firmware when ready");
}
