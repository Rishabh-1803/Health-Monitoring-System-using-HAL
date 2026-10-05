/**
 * @file    task_uart_rx.h
 * @brief   FreeRTOS task: receive and decode packets from STM32.
 */
#ifndef TASK_UART_RX_H
#define TASK_UART_RX_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Create the UART RX task. Call once at boot.
 */
void task_uart_rx_start(void);

/**
 * Last FreeRTOS tick when a valid packet was received from the STM32.
 * Updated by the RX task, read by the TX task for comm-fail detection.
 */
extern volatile uint32_t g_last_rx_tick;

#endif /* TASK_UART_RX_H */
