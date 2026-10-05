/**
 * @file    task_uart_tx.c
 * @brief   Sends MSG_HEARTBEAT packets to the STM32 every 1 second.
 *
 * Uses vTaskDelayUntil for exact 1 Hz timing (no drift, even under load).
 * Sequence number wraps from 65535 to 0 automatically (uint16_t overflow).
 *
 * Comm-fail detection: if no packet is received from the STM32 in 5 s,
 * the state flips (into dashboard_data, which raises the comm-fail
 * alarm and records an event) and recovery flips it back.
 */

#include "task_uart_tx.h"
#include "task_uart_rx.h"
#include "uart_link.h"
#include "packet.h"
#include "protocol_types.h"
#include "dashboard_data.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "logger.h"

static const char *TAG = "UART_TX";

#define TX_STACK_SIZE   2048
#define TX_PRIORITY      4
#define HB_PERIOD_MS     1000
#define COMM_FAIL_MS     5000

static uint32_t s_tx_packets;

void task_uart_tx(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "TX task started — sending heartbeat every %d ms", HB_PERIOD_MS);

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    uint16_t seq = 0;
    TickType_t last_wake = xTaskGetTickCount();
    bool comm_fail = false;

    while (1) {
        esp_task_wdt_reset(NULL);

        /* Build and send a heartbeat packet */
        uint8_t pkt[PACKET_MAX_SIZE];
        int len = packet_encode(pkt, sizeof(pkt),
                                 MSG_HEARTBEAT, seq, NULL, 0);
        if (len > 0 && uart_link_send(pkt, (uint32_t)len)) {
            s_tx_packets++;
        } else {
            ESP_LOGE(TAG, "Failed to send heartbeat (seq=%u)", (unsigned)seq);
        }
        seq++;

        /* Comm-fail state machine (peer = STM32). The dashboard mirrors
         * it as the comm-fail alarm bit + event log entries. */
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t elapsed = now - g_last_rx_tick;
        if (!comm_fail && elapsed > COMM_FAIL_MS) {
            comm_fail = true;
            ESP_LOGE(TAG, "*** COMM FAIL *** (no packet from STM32 in %lu ms)",
                     (unsigned long)elapsed);
            logger_logf(LOG_LVL_ERR, "link", "COMM FAIL (silent %lu ms)",
                        (unsigned long)elapsed);
            dashboard_data_set_comm_fail(true);
        } else if (comm_fail && elapsed <= COMM_FAIL_MS) {
            comm_fail = false;
            ESP_LOGW(TAG, "*** COMM RESTORED ***");
            logger_logf(LOG_LVL_INFO, "link", "COMM RESTORED");
            dashboard_data_set_comm_fail(false);
        }

        /* Wait exactly 1 second (vTaskDelayUntil prevents drift) */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HB_PERIOD_MS));
    }
}

void task_uart_tx_start(void)
{
    xTaskCreate(task_uart_tx, "uart_tx", TX_STACK_SIZE, NULL, TX_PRIORITY, NULL);
}

uint32_t task_uart_tx_get_packets(void)
{
    return s_tx_packets;
}
