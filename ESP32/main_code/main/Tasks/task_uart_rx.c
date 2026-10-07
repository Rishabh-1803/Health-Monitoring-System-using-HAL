/**
 * @file    task_uart_rx.c
 * @brief   Receives bytes from UART1, decodes packets, dispatches them.
 *
 * Blocks on uart_read_bytes with a 100 ms timeout so the task doesn't
 * burn CPU when no data is arriving. At 460800 baud, bytes arrive
 * every ~22 us during a burst, so the timeout is just a safety net.
 *
 * Dispatch map (Phase 4):
 *   MSG_HEARTBEAT     -> liveness only (g_last_rx_tick)
 *   MSG_TELEMETRY     -> dashboard_data_telemetry_arrived()
 *   MSG_ALARM         -> dashboard_data_alarm_arrived()
 *   MSG_RESP_STATUS   -> dashboard_data_status_arrived()
 *   MSG_ACK / MSG_NAK -> command_dispatcher reply hooks
 *   MSG_DEBUG_LOG     -> dashboard event log (+ ESP_LOG)
 *
 * g_last_rx_tick is read by task_uart_tx for comm-fail detection.
 */

#include "task_uart_rx.h"
#include "project_config.h"
#include "uart_link.h"
#include "packet.h"
#include "protocol_types.h"
#include "dashboard_data.h"
#include "command_dispatcher.h"
#include "task_dashboard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "logger.h"
#include <string.h>

static const char *TAG = "UART_RX";

#define RX_STACK_SIZE   4096
#define RX_PRIORITY      5
#define RX_BYTE_TIMEOUT pdMS_TO_TICKS(100)

/* Initialised to boot tick so the TX task doesn't immediately fire comm-fail */
volatile uint32_t g_last_rx_tick = 0;

#if HMS_UART_ECHO_PROBE
/* Optional bring-up helper: the STM32 test menu (option 7, echo) sends the raw
 * text "STM32-BRINGUP-PING\r\n" and passes if anything comes back. The real
 * protocol is framed, so a plain string would otherwise be dropped silently.
 * When the exact probe string is seen, send it straight back. It cannot clash
 * with real frames because those never contain this ASCII sequence. */
static const char ECHO_PROBE[] = "STM32-BRINGUP-PING\r\n";
static uint8_t    s_echo_idx;

static void echo_probe_feed(uint8_t b)
{
    const uint8_t n = (uint8_t)(sizeof(ECHO_PROBE) - 1u);
    if (b == (uint8_t)ECHO_PROBE[s_echo_idx]) {
        if (++s_echo_idx == n) {
            s_echo_idx = 0u;
            (void)uart_link_send((const uint8_t *)ECHO_PROBE, n);
            ESP_LOGI(TAG, "bring-up echo probe answered");
        }
    } else {
        s_echo_idx = (b == (uint8_t)ECHO_PROBE[0]) ? 1u : 0u;
    }
}
#endif /* HMS_UART_ECHO_PROBE */

/* Counters, read by task_dashboard for the status JSON. */
static uint32_t s_rx_packets, s_crc_fails, s_alarms, s_unknown;

void task_uart_rx(void *arg)
{
    (void)arg;
    g_last_rx_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "RX task started — listening for STM32 packets");

    /* TWDT: this task must always return to its poll loop within the
     * 15 s window — the watchdog's whole opinion of RX health. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    packet_decoder_t dec;
    packet_decoder_init(&dec);

    while (1) {
        esp_task_wdt_reset();

        uint8_t byte;
        if (!uart_link_get_byte(&byte, RX_BYTE_TIMEOUT)) {
            continue;   /* timeout — loop back, nothing to decode */
        }

#if HMS_UART_ECHO_PROBE
        echo_probe_feed(byte);
#endif
        packet_result_t r = packet_decode_byte(&dec, byte);

        if (r == PACKET_OK) {
            s_rx_packets++;
            g_last_rx_tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            switch (dec.decoded.type) {

            case MSG_HEARTBEAT:
                break;      /* liveness only — no ACK, no log noise      */

            case MSG_TELEMETRY: {
                if (dec.decoded.payload_len == sizeof(telemetry_payload_t)) {
                    telemetry_payload_t t;
                    memcpy(&t, dec.decoded.payload, sizeof(t));
                    dashboard_data_telemetry_arrived(&t);
                } else {
                    ESP_LOGW(TAG, "telemetry len %u != %u — dropped",
                             (unsigned)dec.decoded.payload_len,
                             (unsigned)sizeof(telemetry_payload_t));
                }
                break;
            }

            case MSG_ALARM: {
                s_alarms++;
                if (dec.decoded.payload_len == sizeof(alarm_payload_t)) {
                    alarm_payload_t a;
                    memcpy(&a, dec.decoded.payload, sizeof(a));
                    ESP_LOGW(TAG, "ALARM bit=0x%02X state=%u value=%d",
                             (unsigned)a.alarm_bit, (unsigned)a.state,
                             (int)a.value_at_trigger);
                    logger_logf(LOG_LVL_WARN, "alarm",
                                "%s bit=0x%02X value=%d/100",
                                (a.state != 0u) ? "raise" : "clear",
                                (unsigned)a.alarm_bit,
                                (int)a.value_at_trigger);
                    logger_flush();   /* incident-log durability path  */
                    dashboard_data_alarm_arrived(&a);
                }
                break;
            }

            case MSG_RESP_STATUS: {
                if (dec.decoded.payload_len == sizeof(resp_status_payload_t)) {
                    resp_status_payload_t s;
                    memcpy(&s, dec.decoded.payload, sizeof(s));
                    dashboard_data_status_arrived(&s);
                }
                break;
            }

            case MSG_ACK: {
                if (dec.decoded.payload_len == sizeof(ack_payload_t)) {
                    ack_payload_t a;
                    memcpy(&a, dec.decoded.payload, sizeof(a));
                    command_dispatcher_on_ack(a.acked_seq, a.acked_type);
                }
                break;
            }

            case MSG_NAK: {
                if (dec.decoded.payload_len == sizeof(nak_payload_t)) {
                    nak_payload_t n;
                    memcpy(&n, dec.decoded.payload, sizeof(n));
                    command_dispatcher_on_nak(n.nak_seq, n.nak_reason);
                }
                break;
            }

            case MSG_DEBUG_LOG: {
                if (dec.decoded.payload_len >= 1u) {
                    uint8_t level = dec.decoded.payload[0];
                    char msg[97];
                    size_t m = dec.decoded.payload_len - 1u;
                    if (m > sizeof(msg) - 1u) {
                        m = sizeof(msg) - 1u;
                    }
                    memcpy(msg, &dec.decoded.payload[1], m);
                    msg[m] = '\0';
                    ESP_LOGI(TAG, "[STM32] %s", msg);
                    dashboard_data_debug_log_arrived(level, msg);
                    if (strcmp(msg, "STM32 telemetry app booted") == 0) {
                        /* STM32 restarted and lost its thresholds. */
                        task_dashboard_request_resync();
                    }
                }
                break;
            }

            default:
                s_unknown++;
                ESP_LOGW(TAG, "unknown type 0x%02X (seq=%u) — ignored",
                         (unsigned)dec.decoded.type,
                         (unsigned)dec.decoded.seq);
                break;
            }
        }
        else if (r == PACKET_ERR_CRC) {
            s_crc_fails++;
            ESP_LOGW(TAG, "CRC mismatch — packet dropped (total: %lu)",
                     (unsigned long)s_crc_fails);
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

void task_uart_rx_get_stats(uint32_t *rx_packets, uint32_t *crc_fails,
                            uint32_t *alarms, uint32_t *unknown)
{
    if (rx_packets != NULL) { *rx_packets = s_rx_packets; }
    if (crc_fails   != NULL) { *crc_fails   = s_crc_fails; }
    if (alarms      != NULL) { *alarms      = s_alarms; }
    if (unknown     != NULL) { *unknown     = s_unknown; }
}
