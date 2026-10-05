/**
 * @file    task_uart_rx.c
 * @brief   Receives bytes from UART1, feeds them to the packet decoder,
 *          logs every received packet.
 *
 * Blocks on uart_read_bytes with a 100ms timeout so the task doesn't
 * burn CPU when no data is arriving. At 460800 baud, bytes arrive
 * every ~22µs during a burst, so the 100ms timeout is just a safety net.
 *
 * On PACKET_OK: logs the packet type + seq, updates g_last_rx_tick.
 * On PACKET_ERR_CRC: logs a warning (CRC mismatch — packet dropped).
 * On PACKET_ERR_HEADER: normal during sync, silently ignored.
 *
 * g_last_rx_tick is read by task_uart_tx for comm-fail detection.
 */

#include "task_uart_rx.h"
#include "uart_link.h"
#include "packet.h"
#include "protocol_types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "UART_RX";

#define RX_STACK_SIZE   4096
#define RX_PRIORITY      5
#define RX_BYTE_TIMEOUT pdMS_TO_TICKS(100)

/* Initialised to boot tick so the TX task doesn't immediately fire comm-fail */
volatile uint32_t g_last_rx_tick = 0;

void task_uart_rx(void *arg)
{
    (void)arg;
    g_last_rx_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "RX task started — listening for STM32 packets");

    packet_decoder_t dec;
    packet_decoder_init(&dec);

    uint32_t received = 0, crc_fail = 0;

    while (1) {
        uint8_t byte;
        if (!uart_link_get_byte(&byte, RX_BYTE_TIMEOUT)) {
            continue;   /* timeout — loop back, nothing to decode */
        }

        packet_result_t r = packet_decode_byte(&dec, byte);

        if (r == PACKET_OK) {
            received++;
            g_last_rx_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            const char *type_str;
            switch (dec.decoded.type) {
                case MSG_HEARTBEAT:         type_str = "HB"; break;
                case MSG_TELEMETRY:         type_str = "TEL"; break;
                case MSG_ALARM:             type_str = "ALM"; break;
                case MSG_ACK:               type_str = "ACK"; break;
                case MSG_NAK:               type_str = "NAK"; break;
                case MSG_CMD_GET_STATUS:    type_str = "CMD_GET_STATUS"; break;
                case MSG_RESET:             type_str = "RESET"; break;
                default:                    type_str = "PKT"; break;
            }

            ESP_LOGI(TAG, "%s received (seq=%u, len=%u)",
                     type_str, (unsigned)dec.decoded.seq,
                     (unsigned)dec.decoded.payload_len);

            /* TODO: Phase 3+ — dispatch to handler based on packet type.
             * For now, heartbeats + telemetry just get logged. */
        }
        else if (r == PACKET_ERR_CRC) {
            crc_fail++;
            ESP_LOGW(TAG, "CRC mismatch — packet dropped (total CRC fails: %lu)",
                     (unsigned long)crc_fail);
        }
        else if (r == PACKET_ERR_FOOTER) {
            ESP_LOGW(TAG, "Footer mismatch — framing lost");
        }
        /* PACKET_ERR_HEADER / PACKET_ERR_LENGTH / PACKET_NEED_MORE:
         * Normal during sync. Silently ignored. */
    }
}

void task_uart_rx_start(void)
{
    xTaskCreate(task_uart_rx, "uart_rx", RX_STACK_SIZE, NULL, RX_PRIORITY, NULL);
}
