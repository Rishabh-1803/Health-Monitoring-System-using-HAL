#ifndef PROTOCOL_TYPES_H
#define PROTOCOL_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#define PACKET_HEADER_BYTE      0xAA
#define PACKET_FOOTER_BYTE       0x55
#define PACKET_MAX_PAYLOAD      128
#define PACKET_OVERHEAD_SIZE    8
#define PACKET_MAX_SIZE         (PACKET_OVERHEAD_SIZE + PACKET_MAX_PAYLOAD)

#define ALARM_BIT_OVERTEMP          0x01
#define ALARM_BIT_OVERCURRENT       0x02
#define ALARM_BIT_VIBRATION         0x04
#define ALARM_BIT_SENSOR_FAIL       0x08
#define ALARM_BIT_COMM_FAIL         0x10
#define ALARM_BIT_ALL               0x1F

#define SENSOR_BIT_DS18B20_OK       0x01
#define SENSOR_BIT_MPU6050_OK       0x02
#define SENSOR_BIT_INA219_OK        0x04
#define SENSOR_BIT_ALL_OK           0x07

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

typedef enum {
    NAK_REASON_CRC_FAIL = 0, NAK_REASON_UNKNOWN_TYPE = 1,
    NAK_REASON_BAD_LENGTH = 2, NAK_REASON_OUT_OF_MEMORY = 3,
} nak_reason_t;

typedef enum {
    RESET_REASON_POWERON=0, RESET_REASON_EXTERNAL=1, RESET_REASON_WATCHDOG=2,
    RESET_REASON_SOFTWARE=3, RESET_REASON_BROWNOUT=4, RESET_REASON_PANIC=5,
} reset_reason_t;

typedef enum {
    THRESHOLD_ID_TEMPERATURE=0, THRESHOLD_ID_CURRENT=1, THRESHOLD_ID_VIBRATION=2,
} threshold_id_t;

typedef enum {
    LED_ID_GREEN=0, LED_ID_YELLOW=1, LED_ID_RED=2, LED_ID_ALL=3,
} led_id_t;

#pragma pack(push, 1)

typedef struct {
    float    temperature_c;
    float    current_a;
    float    vibration_g;
    uint8_t  alarm_bits;
    uint8_t  sensor_status;
    uint16_t cpu_load_pct;
    uint16_t free_heap_bytes_div16;
    uint16_t uptime_seconds;
} telemetry_payload_t;

typedef struct {
    uint8_t  alarm_bit;
    uint8_t  state;
    int16_t  value_at_trigger;
    uint32_t tick_at_trigger;
} alarm_payload_t;

typedef struct {
    uint16_t acked_seq;
    uint8_t  acked_type;
    uint8_t  reserved;
} ack_payload_t;

typedef struct {
    uint16_t nak_seq;
    uint8_t  nak_reason;
    uint8_t  reserved;
} nak_payload_t;

typedef struct {
    uint8_t  threshold_id;
    uint8_t  reserved;
    float    value;
    uint8_t  reserved2[2];
} cmd_set_threshold_payload_t;

typedef struct {
    uint16_t sample_period_ms;
    uint8_t  reserved[2];
} cmd_set_sample_rate_payload_t;

typedef struct {
    uint8_t  led_id;
    uint8_t  reserved;
} cmd_led_payload_t;

typedef struct {
    float    temperature_c;
    float    current_a;
    float    vibration_g;
    uint8_t  alarm_bits;
    uint8_t  sensor_status;
    uint8_t  cpu_load_pct;
    uint8_t  task_states;
    uint16_t free_heap_bytes_div16;
    uint16_t uptime_seconds;
    float    threshold_temp_c;
    float    threshold_current_a;
    float    threshold_vibration_g;
} resp_status_payload_t;

typedef struct {
    uint8_t  reset_reason;
} reset_payload_t;

typedef struct {
    uint8_t  level;
} debug_log_payload_t;

#pragma pack(pop)

typedef struct {
    msg_type_t  type;
    uint16_t    seq;
    uint8_t     payload[PACKET_MAX_PAYLOAD];
    uint8_t     payload_len;
    bool        crc_valid;
} decoded_packet_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(telemetry_payload_t) == 20, "telemetry_payload_t must be 20 bytes");
_Static_assert(sizeof(alarm_payload_t) == 8, "alarm_payload_t must be 8 bytes");
_Static_assert(sizeof(ack_payload_t) == 4, "ack_payload_t must be 4 bytes");
_Static_assert(sizeof(nak_payload_t) == 4, "nak_payload_t must be 4 bytes");
_Static_assert(sizeof(cmd_set_threshold_payload_t) == 8, "cmd_set_threshold must be 8");
_Static_assert(sizeof(cmd_set_sample_rate_payload_t) == 4, "cmd_set_sample_rate must be 4");
_Static_assert(sizeof(cmd_led_payload_t) == 2, "cmd_led must be 2");
_Static_assert(sizeof(resp_status_payload_t) == 32, "resp_status must be 32");
_Static_assert(sizeof(reset_payload_t) == 1, "reset must be 1");
_Static_assert(sizeof(debug_log_payload_t) == 1, "debug_log must be 1");
#endif

#endif /* PROTOCOL_TYPES_H */