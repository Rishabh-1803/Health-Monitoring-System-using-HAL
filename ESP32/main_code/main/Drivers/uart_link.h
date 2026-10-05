/**
 * @file    uart_link.h
 * @brief   UART driver for the ESP32 ↔ STM32 link (UART1 on GPIO17/18).
 *
 * Uses ESP-IDF's uart_driver_install with a built-in ring buffer.
 * TX: uart_write_bytes (blocking with short timeout).
 * RX: uart_read_bytes with timeout (can block task until byte arrives).
 *
 * Physical layer: 460800 8N1, no flow control.
 * Wired: ESP32 GPIO17 (TX) → STM32 PA3 (RX)
 *        ESP32 GPIO18 (RX) ← STM32 PA2 (TX)
 *        GND ↔ GND (mandatory)
 */
#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

/**
 * Initialise UART1 at 460800 8N1 on GPIO17 (TX) / GPIO18 (RX).
 * Call once at boot.
 */
void uart_link_init(void);

/**
 * Send bytes over UART. Blocking.
 * @return true if all bytes were transmitted.
 */
bool uart_link_send(const uint8_t *data, uint32_t len);

/**
 * Read one byte from the UART RX buffer.
 * @param timeout   Max ticks to wait (portMAX_DELAY = forever)
 * @return true if a byte was read, false on timeout.
 */
bool uart_link_get_byte(uint8_t *out, TickType_t timeout);

/**
 * Number of bytes currently waiting in the RX buffer.
 */
uint32_t uart_link_bytes_available(void);

#endif /* UART_LINK_H */
