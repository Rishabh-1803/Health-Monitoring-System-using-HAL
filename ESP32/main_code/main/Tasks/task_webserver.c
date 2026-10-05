/**
 * @file    task_webserver.c
 * @brief   Starts the HTTP server once a serving context exists.
 *
 * The server can start the moment EITHER the provisioning AP is up OR
 * the station has an IP. Polling for that condition keeps this task
 * dead simple and immune to event-ordering races at boot.
 */

#include "task_webserver.h"
#include "wifi_manager.h"
#include "http_server.h"
#include "dashboard_data.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "TASK_WEB";

#define WEB_WAIT_STACK   3072
#define WEB_WAIT_PRIO     3
#define WEB_WAIT_MS       500

static void task_webserver(void *arg)
{
    (void)arg;
    while (1) {
        wifi_mgr_state_t st = wifi_manager_get_state();
        if (st == WIFI_MGR_CONNECTED || st == WIFI_MGR_PROVISIONING) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(WEB_WAIT_MS));
    }

    esp_err_t err = http_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_server_start failed (0x%x) — dashboard down",
                 err);
        return;
    }

    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    if (ip[0] != '\0') {
        ESP_LOGI(TAG, "Dashboard: http://%s/", ip);
    }
    vTaskDelete(NULL);
}

void task_webserver_start(void)
{
    xTaskCreate(task_webserver, "web_wait", WEB_WAIT_STACK, NULL,
                WEB_WAIT_PRIO, NULL);
}
