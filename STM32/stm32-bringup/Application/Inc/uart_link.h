/**
 * @file    uart_link.h
 * @brief   UART driver for the STM32 ↔ ESP32 link (USART2 on PA2/PA3).
 *
 * TX: blocking via HAL_UART_Transmit (simple, reliable for low-rate packets).
 * RX: interrupt-driven into a 256-byte ring buffer (non-blocking reads).
 *
 * The ring buffer is ISR-safe: head is written only in ISR context, tail
 * is written only in task context. Single-producer / single-consumer.
 *
 * Physical layer: 460800 8N1, no flow control.
 * Wired: STM32 PA2 (TX) → ESP32 GPIO18 (RX)
 *        STM32 PA3 (RX) ← ESP32 GPIO17 (TX)
 *        GND ↔ GND (mandatory)
 */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialise the UART link: clears the ring buffer and arms the first
 * HAL_UART_Receive_IT() call. Call once before sending/receiving.
 *
 * NOTE: This starts interrupt-mode RX. While active, the blocking
 * HAL_UART_Receive used by test_esp32.c (menu option 7) will conflict.
 * Do not run test 7 and test 9 simultaneously.
 */
void uart_link_init(void);

/**
 * Send bytes over UART. Blocking with a 100ms timeout.
 * @return true if all bytes were transmitted.
 */
bool uart_link_send(const uint8_t *data, uint32_t len);

/**
 * Pull one byte from the RX ring buffer. Non-blocking.
 * @return true if a byte was available, false if buffer empty.
 */
bool uart_link_get_byte(uint8_t *out);

/**
 * Number of bytes currently waiting in the RX ring buffer.
 */
uint32_t uart_link_bytes_available(void);

#endif /* UART_LINK_H */
