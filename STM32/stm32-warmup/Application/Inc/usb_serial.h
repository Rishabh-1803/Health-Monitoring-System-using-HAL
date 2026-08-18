/**
 * ============================================================
 * usb_serial.h — Wrapper around STM32 USB CDC
 * ============================================================
 * WHY THIS WRAPPER EXISTS:
 * CubeMX generates usbd_cdc_if.c with a function CDC_Transmit_FS().
 * But calling that directly from every task is messy. This wrapper:
 *   1. Provides a single clean API (usb_serial_write / usb_serial_print)
 *   2. Will be the ONLY place that touches USB CDC primitives
 *   3. Will be the ONLY consumer of the log_queue messages
 * ============================================================
 */

#ifndef USB_SERIAL_H
#define USB_SERIAL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize the USB serial wrapper (no-op on STM32 since CubeMX
 * handles USB init, but kept for API symmetry with the ESP32 version).
 */
void usb_serial_init(void);

/**
 * Write raw bytes to USB CDC. Blocks until USB accepts the data.
 * Returns true on success.
 */
bool usb_serial_write(const uint8_t *data, uint32_t len);

/**
 * Write a null-terminated string to USB CDC.
 */
bool usb_serial_print(const char *str);

#endif /* USB_SERIAL_H */
