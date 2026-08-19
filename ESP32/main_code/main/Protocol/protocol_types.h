/**
 * protocol_types.h — Message type enum + payload struct definitions
 *
 * Phase 1: skeleton only. Phase 2 implements all types from spec.
 * Reference: docs/UART_PROTOCOL_SPEC.md §4
 *
 * NOTE: This file MUST be byte-identical to the STM32 version.
 * Same code, compiled on both platforms.
 */
#ifndef PROTOCOL_TYPES_H
#define PROTOCOL_TYPES_H

#include <stdint.h>

/* ---- Packet framing constants (UART_PROTOCOL_SPEC.md §3) ---- */
#define PACKET_HEADER_BYTE      0xAA
#define PACKET_FOOTER_BYTE      0x55
#define PACKET_MAX_PAYLOAD      128
#define PACKET_OVERHEAD_SIZE    8   /* hdr + len + type + seq(2) + crc(2) + ftr */

/* ---- Message types (UART_PROTOCOL_SPEC.md §4) ---- */
typedef enum {
    MSG_HEARTBEAT           = 0x01,
    MSG_TELEMETRY           = 0x02,
    MSG_ALARM               = 0x03,
    MSG_ACK                 = 0x04,
    MSG_NAK                 = 0x05,
    MSG_CMD_SET_THRESHOLD   = 0x10,
    MSG_CMD_SET_SAMPLE_RATE = 0x11,
    MSG_CMD_LED_ON          = 0x12,
    MSG_CMD_LED_OFF         = 0x13,
    MSG_CMD_BUZZER_OFF      = 0x14,
    MSG_CMD_RESET_ALARM     = 0x15,
    MSG_CMD_REBOOT          = 0x16,
    MSG_CMD_GET_STATUS      = 0x17,
    MSG_RESP_STATUS         = 0x80,
    MSG_DEBUG_LOG           = 0xFE,
    MSG_RESET               = 0xFF,
} msg_type_t;

/* ---- Payload structs (UART_PROTOCOL_SPEC.md §4.1) ----
 * TODO: Phase 2 — implement these structs matching the spec byte-by-byte. */

typedef struct {
    /* 20 bytes total — see spec §4.1 TELEMETRY */
    /* TODO: Phase 2 */
} telemetry_payload_t;

typedef struct {
    /* 8 bytes total — see spec §4.1 ALARM */
    /* TODO: Phase 2 */
} alarm_payload_t;

typedef struct {
    /* 4 bytes total — see spec §4.1 ACK */
    /* TODO: Phase 2 */
} ack_payload_t;

#endif /* PROTOCOL_TYPES_H */
