/**
 * @file    task_uart_tx.c
 * @brief   Sends MSG_HEARTBEAT packets to the STM32 every 1 second.
 *
 * Uses vTaskDelayUntil for exact 1 Hz timing (no drift, even under load).
 * Sequence number wraps from 65535 to 0 automatically (uint16_t overflow).
 *
 * Also checks comm-fail: if no packet received from STM32 in 5 seconds,
 * logs a warning. When a packet arrives again, logs recovery.
 */

#include "task_uart_tx.h"
#include "uart_link.h"
#include "packet.h"
#include "protocol_types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "UART_TX";

#define TX_STACK_SIZE   2048
#define TX_PRIORITY      4
#define HB_PERIOD_MS     1000

/* Shared with task_uart_rx via this extern — declared in task_uart_rx.c */
extern volatile uint32_t g_last_rx_tick;

void task_uart_tx(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "TX task started — sending heartbeat every %d ms", HB_PERIOD_MS);

    uint16_t seq = 0;
    TickType_t last_wake = xTaskGetTickCount();
    bool comm_fail = false;

    while (1) {
        /* Build and send a heartbeat packet */
        uint8_t pkt[PACKET_MAX_SIZE];
        int len = packet_encode(pkt, sizeof(pkt),
                                 MSG_HEARTBEAT, seq, NULL, 0);
        if (len > 0 && uart_link_send(pkt, (uint32_t)len)) {
            ESP_LOGI(TAG, "Heartbeat sent (seq=%u)", (unsigned)seq);
        } else {
            ESP_LOGE(TAG, "Failed to send heartbeat (seq=%u)", (unsigned)seq);
        }
        seq++;

        /* Check comm-fail (peer = STM32) */
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t elapsed = now - g_last_rx_tick;
        if (!comm_fail && elapsed > 5000) {
            comm_fail = true;
            ESP_LOGE(TAG, "*** COMM FAIL *** (no packet from STM32 in %lu ms)",
                     (unsigned long)elapsed);
        }

        /* Wait exactly 1 second (vTaskDelayUntil prevents drift) */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HB_PERIOD_MS));
    }
}

void task_uart_tx_start(void)
{
    xTaskCreate(task_uart_tx, "uart_tx", TX_STACK_SIZE, NULL, TX_PRIORITY, NULL);
}
