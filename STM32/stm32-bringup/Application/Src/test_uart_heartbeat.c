/**
 * @file    test_uart_heartbeat.c
 * @brief   Menu option [9] — Phase 2B heartbeat exchange over UART.
 *
 * This test verifies the full pipeline:
 *   packet_encode() → uart_link_send() → wire → uart_link_get_byte()
 *   → packet_decode_byte() → decoded packet
 *
 * It runs a 1 Hz heartbeat loop in the bring-up task's context:
 *   - Sends MSG_HEARTBEAT every 1 second
 *   - Polls the RX ring buffer for incoming bytes
 *   - Feeds bytes to the packet decoder state machine
 *   - Logs every sent/received packet
 *   - Detects comm-fail (no packet in 5 seconds)
 *   - Detects comm-restore (packet received after comm-fail)
 *
 * Modes:
 *   [l] Loopback — jumper PA2 (TX) to PA3 (RX). The STM32 talks to itself.
 *       This isolates the STM32 half completely. If loopback works, the
 *       protocol layer + UART driver + ring buffer are all proven.
 *   [e] ESP32 — cross the wires to the ESP32 running the Phase 2B firmware.
 *       This is the real end-to-end test.
 *
 * Both modes run at 460800 8N1 (per Phase 2B spec).
 *
 * Press any key to stop and return to the menu.
 */

#include "bringup_config.h"

#if BRINGUP_TEST_UART_HEARTBEAT

#include "tests.h"
#include "console.h"
#include "uart_link.h"
#include "packet.h"
#include "protocol_types.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"

/* ---- Config (kept here rather than in bringup_config.h for locality) ---- */
#define HB_PERIOD_MS         1000u
#define HB_COMM_FAIL_MS      5000u
#define HB_POLL_DELAY_MS     10u

static test_result_t heartbeat_loop(void)
{
    uart_link_init();

    packet_decoder_t dec;
    packet_decoder_init(&dec);

    uint16_t seq = 0u;
    uint32_t last_tx_tick  = HAL_GetTick();
    uint32_t last_rx_tick  = HAL_GetTick();

    uint32_t sent = 0u, received = 0u, crc_fail = 0u, framing_err = 0u;
    bool comm_fail = false;

    (void)console_println("  Running. Press any key to stop.");
    (void)console_println("");
    console_flush_rx();

    for (;;) {
        uint32_t now = HAL_GetTick();

        /* ---- Send heartbeat every 1 second ---- */
        if ((now - last_tx_tick) >= HB_PERIOD_MS) {
            uint8_t pkt[PACKET_MAX_SIZE];
            int len = packet_encode(pkt, sizeof(pkt),
                                     MSG_HEARTBEAT, seq, NULL, 0u);
            if (len > 0 && uart_link_send(pkt, (uint32_t)len)) {
                sent++;
                (void)console_printf("  [TX] HB seq=%u\r\n", (unsigned)seq);
            }
            seq++;
            last_tx_tick = now;
        }

        /* ---- Drain RX ring buffer ---- */
        uint8_t byte;
        while (uart_link_get_byte(&byte)) {
            packet_result_t r = packet_decode_byte(&dec, byte);
            if (r == PACKET_OK) {
                received++;
                last_rx_tick = now;
                if (comm_fail) {
                    comm_fail = false;
                    (void)console_println("  *** COMM RESTORED ***");
                }
                (void)console_printf("  [RX] %s seq=%u len=%u\r\n",
                    (dec.decoded.type == MSG_HEARTBEAT) ? "HB" : "PKT",
                    (unsigned)dec.decoded.seq,
                    (unsigned)dec.decoded.payload_len);
            } else if (r == PACKET_ERR_CRC) {
                crc_fail++;
            } else if (r == PACKET_ERR_FOOTER) {
                framing_err++;
            }
            /* PACKET_ERR_HEADER / PACKET_ERR_LENGTH are normal during sync */
        }

        /* ---- Comm-fail detection (5 second timeout) ---- */
        if (!comm_fail && (now - last_rx_tick) > HB_COMM_FAIL_MS) {
            comm_fail = true;
            (void)console_println("  *** COMM FAIL *** (no packet in 5s)");
        }

        /* ---- Check for keypress to stop ---- */
        if (console_key_pressed()) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(HB_POLL_DELAY_MS));
    }

    /* ---- Summary ---- */
    (void)console_println("");
    (void)console_println("  -- Summary --------------------------------------");
    (void)console_printf("  Sent:           %lu\r\n", (unsigned long)sent);
    (void)console_printf("  Received:       %lu\r\n", (unsigned long)received);
    (void)console_printf("  CRC failures:   %lu\r\n", (unsigned long)crc_fail);
    (void)console_printf("  Framing errors: %lu\r\n", (unsigned long)framing_err);
    (void)console_println("");

    console_flush_rx();

    if (received == 0u) {
        (void)console_println("  No packets received. Check wiring / jumper / ESP32 firmware.");
        return TEST_FAIL;
    }
    if (crc_fail > 0u) {
        (void)console_println("  Packets received but some had CRC errors.");
        (void)console_println("  Usually marginal wiring or baud-rate mismatch.");
        return TEST_FAIL;
    }
    (void)console_println("  Heartbeat exchange working cleanly.");
    return TEST_PASS;
}

test_result_t test_uart_heartbeat_run(void)
{
    char line[CONSOLE_LINE_MAX];

    (void)console_println("-- UART heartbeat (Phase 2B) at 460800 8N1 -----");
    (void)console_println("    [l] loopback  (jumper PA2 to PA3, STM32 only)");
    (void)console_println("    [e] ESP32    (cross to ESP32 GPIO17/18)");
    (void)console_println("    [q] back");

    console_flush_rx();
    (void)console_print("  Choose: ");
    if (!console_readline(line, sizeof(line), 30000u)) {
        return TEST_ABORT;
    }

    switch (line[0]) {
        case 'l': case 'L':
            (void)console_println("  Loopback mode. Jumper PA2 (TX) directly to PA3 (RX).");
            return heartbeat_loop();
        case 'e': case 'E':
            (void)console_println("  ESP32 mode. Wire STM32 PA2->ESP32 GPIO18, PA3<-GPIO17, GND<->GND.");
            return heartbeat_loop();
        default:
            return TEST_SKIP;
    }
}

#else  /* BRINGUP_TEST_UART_HEARTBEAT */

#include "tests.h"
test_result_t test_uart_heartbeat_run(void) { return TEST_SKIP; }

#endif /* BRINGUP_TEST_UART_HEARTBEAT */
