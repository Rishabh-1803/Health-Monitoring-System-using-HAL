/**
 * @file    task_uart_rx.h
 * @brief   FreeRTOS task: receive, decode and dispatch STM32 packets.
 */
#ifndef TASK_UART_RX_H
#define TASK_UART_RX_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>

/**
 * Create the UART RX task. Call once at boot.
 */
void task_uart_rx_start(void);

/**
 * Last FreeRTOS tick when a valid packet was received from the STM32.
 * Updated by the RX task, read by the TX task for comm-fail detection.
 */
extern volatile uint32_t g_last_rx_tick;

/** Packet counters for the dashboard stats block. NULL args are skipped. */
void task_uart_rx_get_stats(uint32_t *rx_packets, uint32_t *crc_fails,
                            uint32_t *alarms, uint32_t *unknown);

#endif /* TASK_UART_RX_H */
