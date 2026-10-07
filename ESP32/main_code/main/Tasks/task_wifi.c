/**
 * @file    task_wifi.c
 * @brief   2 s WiFi poller: RSSI + state into the dashboard store.
 *
 * Connection/retry/provisioning is event-driven inside wifi_manager;
 * this task only refreshes facts that have no event (RSSI drift) and
 * keeps the dashboard's wifi block current.
 */

#include "task_wifi.h"
#include "wifi_manager.h"
#include "dashboard_data.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "TASK_WIFI";

#define WIFI_POLL_STACK   4096
#define WIFI_POLL_PRIO     3
#define WIFI_POLL_MS     2000

static void task_wifi(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wifi poller started (%d ms)", WIFI_POLL_MS);

    char ip[16];
    char ssid[33];
    while (1) {
        wifi_manager_poll_rssi();
        wifi_manager_get_ip(ip, sizeof(ip));
        wifi_manager_get_ssid(ssid, sizeof(ssid));
        dashboard_data_note_wifi((int)wifi_manager_get_state(), ssid, ip,
                                 wifi_manager_get_rssi());
        vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_MS));
    }
}

void task_wifi_start(void)
{
    xTaskCreate(task_wifi, "wifi_poll", WIFI_POLL_STACK, NULL,
                WIFI_POLL_PRIO, NULL);
}
