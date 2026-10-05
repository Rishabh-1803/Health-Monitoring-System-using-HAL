/**
 * @file    protocol_types.h
 * @brief   Message types + payload struct definitions for the STM32↔ESP32
 *          binary UART protocol.
 *
 * SHARED CODE — byte-identical on STM32 and ESP32. Both sides must agree
 * on every byte offset, so the structs are packed with #pragma pack(1).
 *
 * Reference: docs/UART_PROTOCOL_SPEC.md §4
 *
 * All multi-byte fields are LITTLE-ENDIAN on the wire. Both STM32
 * (Cortex-M4) and ESP32 (Xtensa) are natively little-endian, so no
 * byte-swapping is needed on either side.
 */

#ifndef PROTOCOL_TYPES_H
#define PROTOCOL_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Packet framing constants ---- */
#define PACKET_HEADER_BYTE      0xAA
#define PACKET_FOOTER_BYTE       0x55
#define PACKET_MAX_PAYLOAD      128
#define PACKET_OVERHEAD_SIZE    8   /* hdr(1)+len(1)+type(1)+seq(2)+crc(2)+ftr(1) */
#define PACKET_MAX_SIZE         (PACKET_OVERHEAD_SIZE + PACKET_MAX_PAYLOAD)   /* 136 */

/* ---- Alarm bit definitions ---- */
#define ALARM_BIT_OVERTEMP          0x01
#define ALARM_BIT_OVERCURRENT       0x02
#define ALARM_BIT_VIBRATION         0x04
#define ALARM_BIT_SENSOR_FAIL       0x08
#define ALARM_BIT_COMM_FAIL         0x10
#define ALARM_BIT_ALL               0x1F

/* ---- Sensor status bit definitions ---- */
#define SENSOR_BIT_DS18B20_OK       0x01
#define SENSOR_BIT_MPU6050_OK       0x02
#define SENSOR_BIT_INA219_OK        0x04
#define SENSOR_BIT_ALL_OK           0x07

/* ---- Message types ---- */
typedef enum {
    MSG_HEARTBEAT           = 0x01,   /* Both directions, 0-byte payload        */
    MSG_TELEMETRY           = 0x02,   /* STM32->ESP32, 20-byte payload          */
    MSG_ALARM               = 0x03,   /* STM32->ESP32, 8-byte payload           */
    MSG_ACK                 = 0x04,   /* Both directions, 4-byte payload        */
    MSG_NAK                 = 0x05,   /* Both directions, 4-byte payload        */
    MSG_CMD_SET_THRESHOLD   = 0x10,   /* ESP32->STM32, 8-byte payload           */
    MSG_CMD_SET_SAMPLE_RATE = 0x11,   /* ESP32->STM32, 4-byte payload           */
    MSG_CMD_LED_ON          = 0x12,   /* ESP32->STM32, 2-byte payload           */
    MSG_CMD_LED_OFF         = 0x13,   /* ESP32->STM32, 2-byte payload           */
    MSG_CMD_BUZZER_OFF      = 0x14,   /* ESP32->STM32, 0-byte payload           */
    MSG_CMD_RESET_ALARM     = 0x15,   /* ESP32->STM32, 0-byte payload           */
    MSG_CMD_REBOOT          = 0x16,   /* ESP32->STM32, 0-byte payload           */
    MSG_CMD_GET_STATUS      = 0x17,   /* ESP32->STM32, 0-byte payload           */
    MSG_RESP_STATUS         = 0x80,   /* STM32->ESP32, 32-byte payload          */
    MSG_DEBUG_LOG           = 0xFE,   /* Both directions, 0..128-byte payload   */
    MSG_RESET               = 0xFF,   /* Both directions, 1-byte payload        */
} msg_type_t;

/* ---- NAK reason codes ---- */
typedef enum {
    NAK_REASON_CRC_FAIL         = 0,
    NAK_REASON_UNKNOWN_TYPE     = 1,
    NAK_REASON_BAD_LENGTH       = 2,
    NAK_REASON_OUT_OF_MEMORY    = 3,
} nak_reason_t;

/* ---- Reset reason codes ---- */
typedef enum {
    RESET_REASON_POWERON        = 0,
    RESET_REASON_EXTERNAL       = 1,
    RESET_REASON_WATCHDOG       = 2,
    RESET_REASON_SOFTWARE       = 3,
    RESET_REASON_BROWNOUT       = 4,
    RESET_REASON_PANIC          = 5,
} reset_reason_t;

/* ---- Threshold IDs ---- */
typedef enum {
    THRESHOLD_ID_TEMPERATURE    = 0,
    THRESHOLD_ID_CURRENT        = 1,
    THRESHOLD_ID_VIBRATION      = 2,
} threshold_id_t;

/* ---- LED IDs ---- */
typedef enum {
    LED_ID_GREEN                = 0,
    LED_ID_YELLOW               = 1,
    LED_ID_RED                  = 2,
    LED_ID_ALL                  = 3,
} led_id_t;

/* ============================================================ */
/* PAYLOAD STRUCTS — packed, no padding                         */
/* ============================================================ */

#pragma pack(push, 1)

/** MSG_TELEMETRY payload (20 bytes) */
typedef struct {
    float    temperature_c;            /* 0..3   Filtered temperature (C)       */
    float    current_a;                /* 4..7   Filtered current (A)            */
    float    vibration_g;              /* 8..11  Filtered vibration (g, RMS)     */
    uint8_t  alarm_bits;               /* 12     Bitmask of active alarms        */
    uint8_t  sensor_status;            /* 13     Bitmask of sensor health        */
    uint16_t cpu_load_pct;             /* 14..15 CPU usage (0..10000 = 0..100%)  */
    uint16_t free_heap_bytes_div16;    /* 16..17 Free heap / 16                  */
    uint16_t uptime_seconds;           /* 18..19 Seconds since boot (wraps 18.2h)*/
} telemetry_payload_t;

/** MSG_ALARM payload (8 bytes) */
typedef struct {
    uint8_t  alarm_bit;                /* 0   Which alarm: 0=temp,1=curr,2=vib...*/
    uint8_t  state;                    /* 1   0 = cleared, 1 = raised            */
    int16_t  value_at_trigger;         /* 2..3 Scaled value at trigger           */
    uint32_t tick_at_trigger;          /* 4..7 FreeRTOS tick when alarm fired    */
} alarm_payload_t;

/** MSG_ACK payload (4 bytes) */
typedef struct {
    uint16_t acked_seq;                /* 0..1 Seq # of the packet being acked   */
    uint8_t  acked_type;               /* 2   Type of the acked packet           */
    uint8_t  reserved;                 /* 3   Always 0                           */
} ack_payload_t;

/** MSG_NAK payload (4 bytes) */
typedef struct {
    uint16_t nak_seq;                  /* 0..1 Seq # of rejected packet          */
    uint8_t  nak_reason;               /* 2   See nak_reason_t                   */
    uint8_t  reserved;                 /* 3   Always 0                           */
} nak_payload_t;

/** MSG_CMD_SET_THRESHOLD payload (8 bytes) */
typedef struct {
    uint8_t  threshold_id;             /* 0   0=temp, 1=curr, 2=vib              */
    uint8_t  reserved;                 /* 1   Always 0                           */
    float    value;                    /* 2..5 New threshold value               */
    uint8_t  reserved2[2];             /* 6..7 Always 0                          */
} cmd_set_threshold_payload_t;

/** MSG_CMD_SET_SAMPLE_RATE payload (4 bytes) */
typedef struct {
    uint16_t sample_period_ms;         /* 0..1 New period in ms (50..1000)       */
    uint8_t  reserved[2];              /* 2..3 Always 0                          */
} cmd_set_sample_rate_payload_t;

/** MSG_CMD_LED_ON / MSG_CMD_LED_OFF payload (2 bytes) */
typedef struct {
    uint8_t  led_id;                   /* 0   0=green, 1=yellow, 2=red, 3=all    */
    uint8_t  reserved;                 /* 1   Always 0                           */
} cmd_led_payload_t;

/** MSG_RESP_STATUS payload (32 bytes) */
typedef struct {
    float    temperature_c;            /* 0..3   Current filtered temperature    */
    float    current_a;                /* 4..7   Current filtered current        */
    float    vibration_g;              /* 8..11  Current filtered vibration      */
    uint8_t  alarm_bits;               /* 12    Current alarm bitmask            */
    uint8_t  sensor_status;            /* 13    Sensor health bitmask            */
    uint8_t  cpu_load_pct;             /* 14    CPU % (0..100)                   */
    uint8_t  task_states;              /* 15    Bitmask of task states           */
    uint16_t free_heap_bytes_div16;    /* 16..17 Free heap / 16                  */
    uint16_t uptime_seconds;           /* 18..19 Uptime (wraps 18.2h)            */
    float    threshold_temp_c;         /* 20..23 Current temp threshold          */
    float    threshold_current_a;      /* 24..27 Current threshold               */
    float    threshold_vibration_g;    /* 28..31 Current threshold               */
} resp_status_payload_t;

/** MSG_RESET payload (1 byte) */
typedef struct {
    uint8_t  reset_reason;             /* 0   See reset_reason_t                 */
} reset_payload_t;

/** MSG_DEBUG_LOG payload (1 byte header + message) */
typedef struct {
    uint8_t  level;                    /* 0   0=INFO, 1=WARN, 2=ERROR            */
    /* followed by message bytes (no length field — use packet Length - 1) */
} debug_log_payload_t;

#pragma pack(pop)

/* ============================================================ */
/* DECODED PACKET STRUCT (filled in by the decoder)            */
/* ============================================================ */

typedef struct {
    msg_type_t  type;
    uint16_t    seq;
    uint8_t     payload[PACKET_MAX_PAYLOAD];
    uint8_t     payload_len;
    bool        crc_valid;
} decoded_packet_t;

/* ============================================================ */
/* COMPILE-TIME LAYOUT CHECKS                                   */
/* ============================================================ */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(telemetry_payload_t)           == 20, "telemetry_payload_t must be 20 bytes");
_Static_assert(sizeof(alarm_payload_t)               == 8,  "alarm_payload_t must be 8 bytes");
_Static_assert(sizeof(ack_payload_t)                 == 4,  "ack_payload_t must be 4 bytes");
_Static_assert(sizeof(nak_payload_t)                 == 4,  "nak_payload_t must be 4 bytes");
_Static_assert(sizeof(cmd_set_threshold_payload_t)   == 8,  "cmd_set_threshold_payload_t must be 8 bytes");
_Static_assert(sizeof(cmd_set_sample_rate_payload_t) == 4,  "cmd_set_sample_rate_payload_t must be 4 bytes");
_Static_assert(sizeof(cmd_led_payload_t)             == 2,  "cmd_led_payload_t must be 2 bytes");
_Static_assert(sizeof(resp_status_payload_t)         == 32, "resp_status_payload_t must be 32 bytes");
_Static_assert(sizeof(reset_payload_t)               == 1,  "reset_payload_t must be 1 byte");
_Static_assert(sizeof(debug_log_payload_t)           == 1,  "debug_log_payload_t must be 1 byte");
#endif

#endif /* PROTOCOL_TYPES_H */
