/**
 * @file    crc16.h
 * @brief   CRC-16/CCITT (poly 0x1021, init 0xFFFF) — shared between STM32 and ESP32.
 *
 * Used by the binary UART protocol to detect transmission errors.
 * Both sides must compute the SAME value for the same input — this file
 * is therefore byte-identical on both MCUs.
 *
 * Test vectors (verified):
 *   ""              -> 0xFFFF
 *   "123456789"     -> 0x29B1
 *   "A"             -> 0xB915
 *   {0xAA,0x00,0x01,0x00,0x00} -> 0x4AF6
 */

#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

/**
 * Compute CRC-16/CCITT over a byte buffer.
 * @param data  Pointer to data bytes
 * @param len   Number of bytes
 * @return      16-bit CRC value
 */
uint16_t crc16_ccitt(const uint8_t *data, size_t len);

#endif /* CRC16_H */
