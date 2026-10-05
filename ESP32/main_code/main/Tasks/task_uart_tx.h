/**
 * @file    task_uart_tx.h
 * @brief   FreeRTOS task: send heartbeat packets to STM32 every 1 second.
 */
#ifndef TASK_UART_TX_H
#define TASK_UART_TX_H

/**
 * Create the UART TX task. Call once at boot.
 */
void task_uart_tx_start(void);

#endif /* TASK_UART_TX_H */
