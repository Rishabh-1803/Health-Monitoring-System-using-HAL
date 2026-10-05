/**
 * @file    packet.h
 * @brief   Packet encoder + decoder (streaming state machine) for the
 *          STM32↔ESP32 binary UART protocol.
 *
 * SHARED CODE — byte-identical on STM32 and ESP32.
 *
 * The decoder is a streaming state machine. Feed it one byte at a time
 * as bytes arrive from UART. It returns PACKET_OK when a complete valid
 * packet has been received, PACKET_NEED_MORE while waiting, or
 * PACKET_ERR_* on framing errors.
 *
 * Packet structure (UART_PROTOCOL_SPEC.md §3):
 *
 *   [0xAA] [len] [type] [seq_lo] [seq_hi] [payload...] [crc_lo] [crc_hi] [0x55]
 *
 * The CRC covers bytes 0..4+len (header + len + type + seq + payload).
 */

#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "protocol_types.h"

/* ---- Decoder result codes ---- */
typedef enum {
    PACKET_OK            =  0,   /* Complete valid packet in dec->decoded      */
    PACKET_NEED_MORE     =  1,   /* Need more bytes; current byte consumed     */
    PACKET_ERR_HEADER    = -1,   /* Unexpected byte when looking for header    */
    PACKET_ERR_LENGTH    = -2,   /* Length field > PACKET_MAX_PAYLOAD          */
    PACKET_ERR_FOOTER    = -3,   /* Footer byte mismatch — framing lost        */
    PACKET_ERR_CRC       = -4,   /* Footer OK but CRC mismatch — packet dropped*/
    PACKET_ERR_OVERFLOW  = -5,   /* Payload buffer overflow (shouldn't happen) */
} packet_result_t;

/* ---- Encoder ---- */

/**
 * Build a complete packet in out_buf.
 *
 * @param out_buf     Destination buffer (>= payload_len + PACKET_OVERHEAD_SIZE)
 * @param out_cap     Capacity of out_buf
 * @param type        Message type (one of MSG_*)
 * @param seq         Sequence number (caller increments per packet)
 * @param payload      Pointer to payload bytes (NULL allowed if payload_len==0)
 * @param payload_len  Payload length (0..PACKET_MAX_PAYLOAD)
 *
 * @return >0 on success (total packet size in bytes)
 *         -1 if out_buf is NULL
 *         -2 if payload_len > PACKET_MAX_PAYLOAD
 *         -3 if out_cap is too small
 */
int packet_encode(uint8_t *out_buf, size_t out_cap,
                  msg_type_t type, uint16_t seq,
                  const uint8_t *payload, uint8_t payload_len);

/* ---- Decoder (state machine) ---- */

typedef struct {
    /* Internal state — do not touch from outside */
    uint8_t  state;
    uint8_t  length;
    uint8_t  type;
    uint16_t seq;
    uint8_t  payload_buf[PACKET_MAX_PAYLOAD];
    uint8_t  payload_idx;
    uint16_t crc_recv;
    uint8_t  crc_idx;
    uint8_t  footer;

    /* Output — valid only after packet_decode_byte() returns PACKET_OK */
    decoded_packet_t decoded;
} packet_decoder_t;

/** Initialize a decoder instance. Call once before feeding any bytes. */
void packet_decoder_init(packet_decoder_t *dec);

/**
 * Feed one byte to the decoder.
 *
 * Returns:
 *   PACKET_OK         — complete valid packet in dec->decoded. Read it now.
 *   PACKET_NEED_MORE  — need more bytes; current byte was consumed.
 *   PACKET_ERR_*      — framing/CRC error; decoder has auto-resynced.
 */
packet_result_t packet_decode_byte(packet_decoder_t *dec, uint8_t byte);

/** Reset the decoder state machine (e.g., after a comm timeout). */
void packet_decoder_reset(packet_decoder_t *dec);

#endif /* PACKET_H */
