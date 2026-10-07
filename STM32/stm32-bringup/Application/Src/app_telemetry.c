/**
 * @file    app_telemetry.c
 * @brief   Phase 3 — continuous telemetry application for the STM32 node.
 *
 * One cooperative task, one 10 ms tick. Everything the application does is
 * a bounded slice of work executed inside that tick, so the loop can never
 * block for longer than one tick and the independent watchdog (fed every
 * pass) always sees a live loop.
 *
 * Per tick:
 *   0. (1 Hz) I2C sensor supervision: re-detect / re-configure / un-hang
 *   1. INA219 (I2C)  : shunt current, 8-sample rolling average   (~fast)
 *   2. MPU6050 (I2C) : accel magnitude, high-passed to vibration (~fast)
 *   3. DS18B20       : non-blocking state machine (see below)     (~fast)
 *   4. UART RX       : drain ring buffer, decode, run commands    (~fast)
 *   5. Alarm engine  : hysteresis state machine per channel       (~fast)
 *   6. Outputs       : LED blink, buzzer timeout, relay latch     (~fast)
 *   7. OLED          : one page flushed per tick (8 ticks = full)  (~fast)
 *
 * Then, when due:
 *   - MSG_TELEMETRY every sample_period_ms (default 200 ms, 50..1000)
 *   - MSG_HEARTBEAT every 1000 ms
 *   - console status line every 0.5 s (real-time)
 *
 * DS18B20 state machine (a 12-bit conversion takes up to 750 ms and the
 * old test just slept through it; the application cannot afford that):
 *
 *   IDLE --(read period elapsed)--> CONVERT (issue SKIP_ROM+0x44)
 *   CONVERT --(>= 800 ms)--> READ (issue SKIP_ROM+0xBE, pull 9 bytes)
 *   READ --> validate (stuck-bus check, CRC-8, +85 C sentinel, range)
 *            ok    -> filtered temperature, clear the fail streak
 *            bad   -> fail streak++; 5 in a row = SENSOR_FAIL alarm
 *   READ --> IDLE
 *
 * Vibration: the MPU6050 accelerometer gives REAL 3-axis acceleration. Each
 * axis has a slow baseline (gravity + mounting tilt) subtracted, and the
 * reported vibration_g is the RMS of what is left, in true g -- directly
 * comparable to the threshold, no calibration factor needed.
 * Current: the INA219 measures the shunt voltage over I2C (PGA /8, so a
 * 0.1 ohm shunt reads up to 3.2 A); an 8-sample rolling average keeps the
 * number real-time yet noise-stable.
 * Sensor health: both I2C sensors are supervised once a second
 * (ina219_service / mpu6050_service): a missing, reset or hung sensor is
 * re-detected and re-configured automatically, and a sustained failure
 * clears its sensor-ok bit and zeroes its value instead of leaving a
 * stale number on the dashboard.
 *
 * Watchdog: driven at register level (IWDG->KR/PRL/RLR) rather than
 * through HAL, because the CubeMX project never enabled the IWDG module
 * and editing stm32f4xx_hal_conf.h would be wiped by the next .ioc
 * regeneration. Prescaler 256 + reload 4095 gives a nominal 32.7 s period
 * (worst case ~22 s across the LSI tolerance) — far longer than one 10 ms
 * tick, far shorter than any real hang deserves to survive. It is fed by
 * this loop and by the console's interactive wait paths, so a hung
 * application resets while a human poking the menu does not.
 *
 * Telemetry is fire-and-forget: CRC-16 protects every frame and a fresh
 * sample follows 200 ms later, so a lost frame costs less than a
 * retransmission protocol would. Commands (ESP32 -> STM32) are the
 * opposite: rare, state-changing, and acknowledged with MSG_ACK/MSG_NAK.
 */

#include "app_telemetry.h"
#include "bringup_config.h"
#include "bsp.h"
#include "console.h"
#include "uart_link.h"
#include "packet.h"
#include "protocol_types.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "oled.h"
#include "ina219.h"
#include "mpu6050.h"

/* ================================================================== */
/*  Tunables                                                          */
/* ================================================================== */

#define APP_TICK_MS             10u     /* base loop period                */
#define HB_PERIOD_MS            1000u   /* STM32 heartbeat to ESP32        */
#define STATUS_PRINT_MS         500u    /* console status line (0.5 s)     */

#define DS_READ_PERIOD_MS       2000u   /* temperature read interval       */
#define DS_CONVERT_WAIT_MS      800u    /* >= 750 ms worst-case conversion */


#define ALARM_CONFIRM_MS        1500u   /* above threshold this long = on  */
#define ALARM_CLEAR_MS          3000u   /* below clear level this long = off */
#define COMM_FAIL_MS            5000u   /* no packet from ESP32            */
#define DS_FAIL_STREAK          5u      /* consecutive bad reads = fail    */

#define CUR_AVG_N               8u      /* current rolling-average depth   */
#define VIB_BASELINE_ALPHA      0.05f   /* slow gravity/tilt tracker/axis  */
#define VIB_EMA_ALPHA           0.15f   /* smoothing of the vibration power*/
#define SENSOR_OK_FAIL_TICKS    5u      /* bad reads before ok-bit clears  */
#define SENSOR_ZERO_FAIL_TICKS  25u     /* bad reads before value -> 0     */
#define SENSOR_SERVICE_MS       1000u   /* I2C sensor supervision period   */
#define DIAG_PRINT_MS           2000u   /* I2C sensor diagnostics line     */
#define OLED_REBUILD_MS         250u    /* framebuffer refresh interval    */
#define EMA_TEMP                0.25f   /* temperature filter coefficient  */

#define BUZZER_ON_ALARM_MS      3000u   /* buzzer duration on raise        */

/* Relay trips on these alarms (the "industrial trip" set). */
#define ALARM_TRIP_MASK         (ALARM_BIT_OVERTEMP | ALARM_BIT_OVERCURRENT)

/* ================================================================== */
/*  Types                                                             */
/* ================================================================== */

typedef enum {
    DS_IDLE = 0,        /* waiting for the next read period          */
    DS_CONVERT,         /* conversion running, waiting >= 750 ms     */
    DS_READ,            /* pull the 9-byte scratchpad               */
} ds_state_t;

typedef struct {
    float    threshold;         /* raise level                          */
    float    clear_level;       /* hysteresis clear level               */
    bool     active;            /* alarm currently raised               */
    uint32_t above_ms;          /* ms continuously above threshold      */
    uint32_t below_ms;          /* ms continuously below clear level    */
} alarm_channel_t;

/* ================================================================== */
/*  State                                                             */
/* ================================================================== */

/* Filters + measured values */
static float    s_temp_c        = 0.0f;
static float    s_current_a     = 0.0f;
static float    s_vib_g_est     = 0.0f;
static bool     s_temp_valid    = false;

/* DS18B20 state machine */
static ds_state_t s_ds_state    = DS_IDLE;
static uint32_t   s_ds_conv_start = 0u;
static uint32_t   s_ds_next_read  = 0u;
static uint32_t   s_ds_fail_streak = 0u;
static uint8_t    s_ds_scratch[9];


/* Alarm channels (index = threshold id: 0 temp, 1 current, 2 vibration) */
static alarm_channel_t s_alarms[3] = {
    { .threshold = 60.0f, .clear_level = 57.0f, 0, 0, 0 },  /* temp   */
    { .threshold = DEFAULT_THRESH_CURRENT_A,
      .clear_level = DEFAULT_THRESH_CURRENT_A * 0.9f, 0, 0, 0 },  /* current*/
    { .threshold = 0.5f,  .clear_level = 0.45f, 0, 0, 0 },  /* vib (g)*/
};
static uint8_t s_alarm_bits     = 0u;    /* composed ALARM_BIT_*         */
static bool    s_comm_fail      = false;
static bool    s_sensor_fail    = false;

/* Link + protocol */
static uint16_t s_seq_tel       = 0u;    /* telemetry sequence           */
static uint16_t s_seq_hb        = 0x4000u; /* heartbeat sequence (own range) */
static uint32_t s_last_rx_tick  = 0u;    /* last packet from ESP32       */
static uint32_t s_sample_period_ms = 200u;

/* Outputs */
static bool     s_led_green_override = false;  /* LED_ON command latched */
static bool     s_led_green_state    = false;
static bool     s_buzzer_muted       = false;
static uint32_t s_buzzer_until       = 0u;
static bool     s_relay_latched      = false;

/* CPU load estimate: DWT cycles spent working vs wall period */
static uint32_t s_work_cycles_acc = 0u;
static uint32_t s_window_start_cy = 0u;
static uint32_t s_window_ms       = 0u;
static uint16_t s_cpu_load_10000  = 0u;   /* 0..10000 = 0..100.00 %      */

/* Console cadence */
static uint32_t s_next_status     = 0u;

/* INA219 / MPU6050 / OLED app-mode state */
static bool     s_ina219_ok     = false;
static bool     s_mpu6050_ok    = false;
static float    s_cur_buf[CUR_AVG_N];   /* rolling current samples */
static uint32_t s_cur_idx       = 0u;
static bool     s_cur_full      = false;
static float    s_vib_base[3]   = { 0.0f, 0.0f, 0.0f }; /* per-axis gravity */
static bool     s_vib_seeded    = false;
static float    s_vib_pwr       = 0.0f;  /* EMA of dynamic accel power (g^2) */
static uint32_t s_cur_fail      = 0u;    /* consecutive bad INA219 reads     */
static uint32_t s_vib_fail      = 0u;    /* consecutive bad MPU6050 reads    */
static uint32_t s_next_service  = 0u;
static uint32_t s_next_diag     = 0u;
static uint8_t  s_oled_page     = 0u;    /* round-robin page flush */
static uint32_t s_next_oled_rebuild = 0u;

/* ================================================================== */
/*  Watchdog — register level, deliberately not HAL                   */
/* ================================================================== */

#define IWDG_KEY_WRITE   0x5555u
#define IWDG_KEY_FEED    0xAAAAu
#define IWDG_KEY_START   0xCCCCu
#define IWDG_PRESC_256   0x06u     /* PR register value for /256         */

static bool s_wdt_started = false;

static void wdt_start(void)
{
    if (s_wdt_started) {
        return;                     /* IWDG cannot be stopped once started */
    }
    IWDG->KR  = IWDG_KEY_WRITE;     /* unlock                             */
    IWDG->PR  = IWDG_PRESC_256;     /* LSI/256 -> 125 Hz nominal          */
    IWDG->RLR = 4095u;              /* ~32.7 s nominal, ~22 s worst case  */
    IWDG->KR  = IWDG_KEY_FEED;      /* reload the counter                 */
    IWDG->KR  = IWDG_KEY_START;     /* start: also forces LSI on          */
    s_wdt_started = true;
}

static inline void wdt_feed(void)
{
    if (s_wdt_started) {
        IWDG->KR = IWDG_KEY_FEED;
    }
}

/* Console interactive paths call this so a human in the menu never
 * triggers a reset while the application is not running. */
void app_wdt_kick(void)
{
    wdt_feed();
}

/* ================================================================== */
/*  Small helpers                                                     */
/* ================================================================== */

/** Maxim CRC-8 over data followed by its CRC byte: returns 0 when valid. */
static uint8_t ow_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0u;
    for (int i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (uint8_t)((crc ^ byte) & 0x01u);
            crc >>= 1;
            if (mix != 0u) {
                crc ^= 0x8Cu;
            }
            byte >>= 1;
        }
    }
    return crc;
}

/** All-0x00 / all-0xFF frame = stuck bus, not a measurement. */
static bool frame_stuck(const uint8_t *d, int len)
{
    bool all_zero = true, all_ones = true;
    for (int i = 0; i < len; i++) {
        if (d[i] != 0x00u) { all_zero = false; }
        if (d[i] != 0xFFu) { all_ones  = false; }
    }
    return all_zero || all_ones;
}

static bool send_packet(msg_type_t type, uint16_t seq,
                        const uint8_t *payload, uint8_t len)
{
    uint8_t pkt[PACKET_MAX_SIZE];
    int n = packet_encode(pkt, sizeof(pkt), type, seq, payload, len);
    if (n <= 0) {
        return false;
    }
    return uart_link_send(pkt, (uint32_t)n);
}

static void send_ack(uint16_t seq, msg_type_t acked_type)
{
    ack_payload_t a;
    a.acked_seq  = seq;
    a.acked_type = (uint8_t)acked_type;
    a.reserved   = 0u;
    (void)send_packet(MSG_ACK, seq, (const uint8_t *)&a, sizeof(a));
}

static void send_nak(uint16_t seq, uint8_t reason)
{
    nak_payload_t n;
    n.nak_seq    = seq;
    n.nak_reason = reason;
    n.reserved   = 0u;
    (void)send_packet(MSG_NAK, seq, (const uint8_t *)&n, sizeof(n));
}

static void send_debug(uint8_t level, const char *msg)
{
    uint8_t buf[1 + 96];
    size_t m = strlen(msg);
    if (m > 96u) {
        m = 96u;
    }
    buf[0] = level;
    memcpy(&buf[1], msg, m);
    (void)send_packet(MSG_DEBUG_LOG, s_seq_tel, buf, (uint8_t)(1u + m));
}

/* Scale used for value_at_trigger: x100 keeps two decimals and fits int16. */
#define ALARM_VALUE_SCALE 100

static void send_alarm_event(uint8_t bit, const char *name,
                             bool raised, float value, int scale)
{
    alarm_payload_t p;
    p.alarm_bit      = bit;
    p.state          = raised ? 1u : 0u;
    p.value_at_trigger = (int16_t)(value * (float)scale);
    p.tick_at_trigger = (uint32_t)xTaskGetTickCount();
    (void)send_packet(MSG_ALARM, s_seq_tel, (const uint8_t *)&p, sizeof(p));

    (void)console_printf("[EVT] %s: %s (value %d/100)\r\n",
                         name, raised ? "RAISED" : "CLEARED",
                         (int)(value * 100.0f));
}

/* ================================================================== */
/*  Sensor: INA219 current on I2C (replaces ACS712 analog path)      */
/* ================================================================== */

/* Reads the shunt voltage over I2C every tick (100 Hz) and keeps an
 * 8-sample rolling average -- the depth the operator asked for -- so the
 * displayed current is real-time yet noise-stable. A failed I2C read
 * leaves the last average in place and clears the sensor-ok bit so the
 * dashboard can show the fault instead of a confident zero. */
static void sample_current(void)
{
    float amps = 0.0f;
    if (!ina219_read_current_a(&amps)) {
        s_cur_fail++;
        if (s_cur_fail >= SENSOR_OK_FAIL_TICKS) {
            s_ina219_ok = false;
        }
        if (s_cur_fail == SENSOR_ZERO_FAIL_TICKS) {
            /* Sensor is gone for good: a frozen last value would sit on the
             * dashboard (and could hold an alarm) forever. Show 0 + fault. */
            memset(s_cur_buf, 0, sizeof(s_cur_buf));
            s_cur_idx  = 0u;
            s_cur_full = false;
            s_current_a = 0.0f;
        }
        return;
    }
    s_cur_fail  = 0u;
    s_ina219_ok = true;

    if (amps < 0.0f) {
        amps = -amps;               /* rectify: load current, not sign  */
    }
    if (amps * 1000.0f < (float)INA219_DEADBAND_MA) {
        amps = 0.0f;                /* offset/noise floor, not load     */
    }

    s_cur_buf[s_cur_idx] = amps;
    s_cur_idx = (s_cur_idx + 1u) % CUR_AVG_N;
    if (s_cur_idx == 0u) {
        s_cur_full = true;
    }

    uint32_t n = s_cur_full ? CUR_AVG_N : s_cur_idx;
    if (n == 0u) {
        s_current_a = amps;
        return;
    }
    float sum = 0.0f;
    for (uint32_t i = 0u; i < n; i++) {
        sum += s_cur_buf[i];
    }
    s_current_a = sum / (float)n;   /* 8-sample moving average           */
}

/* ================================================================== */
/*  Sensor: MPU6050 acceleration on I2C (replaces SW-420 digital)    */
/* ================================================================== */

/* Vibration is a REAL quantity: per-axis, the slow baseline (gravity and
 * the mounting tilt) is tracked with a small alpha and subtracted, and
 * vibration_g is the RMS of the remaining 3-axis dynamic acceleration,
 * smoothed on the power (g^2) so it is always >= 0 and unbiased.
 *
 * Working per axis (not on the vector magnitude) matters: for vibration
 * perpendicular to gravity the magnitude barely changes (second order),
 * so a magnitude-only detector misses sideways shaking entirely.
 *
 * The baseline is seeded from the first good sample, so there is no
 * start-up transient and no assumption about orientation or about the
 * individual sensor's offset. */
static void sample_vibration(void)
{
    float a[3];
    if (!mpu6050_read_accel_g(&a[0], &a[1], &a[2])) {
        s_vib_fail++;
        if (s_vib_fail >= SENSOR_OK_FAIL_TICKS) {
            s_mpu6050_ok = false;
        }
        if (s_vib_fail == SENSOR_ZERO_FAIL_TICKS) {
            s_vib_g_est  = 0.0f;
            s_vib_pwr    = 0.0f;
            s_vib_seeded = false;       /* re-seed when it comes back      */
        }
        return;
    }
    s_vib_fail   = 0u;
    s_mpu6050_ok = true;

    if (!s_vib_seeded) {
        for (int i = 0; i < 3; i++) { s_vib_base[i] = a[i]; }
        s_vib_pwr    = 0.0f;
        s_vib_g_est  = 0.0f;
        s_vib_seeded = true;
        return;
    }

    float p = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = a[i] - s_vib_base[i];          /* dynamic part          */
        p += d * d;
        s_vib_base[i] += VIB_BASELINE_ALPHA * d; /* follow gravity/tilt   */
    }
    s_vib_pwr += VIB_EMA_ALPHA * (p - s_vib_pwr);
    if (s_vib_pwr < 0.0f) { s_vib_pwr = 0.0f; }
    s_vib_g_est = sqrtf(s_vib_pwr);              /* RMS, in g             */
}

/* ================================================================== */
/*  Sensor: DS18B20 on PB5 (non-blocking state machine)               */
/* ================================================================== */

#define OW_SKIP_ROM        0xCCu
#define OW_CONVERT_T       0x44u
#define OW_READ_SCRATCHPAD 0xBEu

static void ds_tick(uint32_t now_ms)
{
    switch (s_ds_state) {

    case DS_IDLE:
        if ((now_ms - s_ds_next_read) >= DS_READ_PERIOD_MS) {
            if (bsp_ow_reset()) {
                bsp_ow_write_byte(OW_SKIP_ROM);
                bsp_ow_write_byte(OW_CONVERT_T);
                s_ds_conv_start = now_ms;
                s_ds_state = DS_CONVERT;
            } else {
                /* No presence: count it as a failed read. */
                s_ds_fail_streak++;
                s_ds_next_read = now_ms;
            }
        }
        break;

    case DS_CONVERT:
        if ((now_ms - s_ds_conv_start) >= DS_CONVERT_WAIT_MS) {
            if (bsp_ow_reset()) {
                bsp_ow_write_byte(OW_SKIP_ROM);
                bsp_ow_write_byte(OW_READ_SCRATCHPAD);
                for (int i = 0; i < 9; i++) {
                    s_ds_scratch[i] = bsp_ow_read_byte();
                }
                s_ds_state = DS_READ;
            } else {
                s_ds_fail_streak++;
                s_ds_next_read = now_ms;
                s_ds_state = DS_IDLE;
            }
        }
        break;

    case DS_READ: {
        s_ds_state = DS_IDLE;
        s_ds_next_read = now_ms;

        if (frame_stuck(s_ds_scratch, 9) || ow_crc8(s_ds_scratch, 9) != 0u) {
            s_ds_fail_streak++;
            break;
        }

        int16_t raw = (int16_t)(((uint16_t)s_ds_scratch[1] << 8)
                                | (uint16_t)s_ds_scratch[0]);
        /* Mask undefined low bits below 12-bit resolution. */
        switch ((s_ds_scratch[4] >> 5) & 0x03u) {
            case 0: raw = (int16_t)(raw & ~0x0007); break;
            case 1: raw = (int16_t)(raw & ~0x0003); break;
            case 2: raw = (int16_t)(raw & ~0x0001); break;
            default: break;
        }

        int32_t milli_c = ((int32_t)raw * 625) / 10;

        if (raw == 0x0550) {
            /* +85 C sentinel: conversion never ran. */
            s_ds_fail_streak++;
            break;
        }
        if (milli_c < -55000 || milli_c > 125000) {
            s_ds_fail_streak++;
            break;
        }

        float t = (float)milli_c / 1000.0f;
        if (!s_temp_valid) {
            s_temp_c = t;            /* first reading: seed the filter  */
            s_temp_valid = true;
        } else {
            s_temp_c += EMA_TEMP * (t - s_temp_c);
        }
        s_ds_fail_streak = 0u;
        break;
    }

    default:
        s_ds_state = DS_IDLE;
        break;
    }
}

/* ================================================================== */
/*  Alarm engine (hysteresis per channel)                             */
/* ================================================================== */

static void alarm_engine_tick(uint32_t now_ms, uint32_t dt_ms)
{
    float values[3] = { s_temp_c, s_current_a, s_vib_g_est };

    for (int ch = 0; ch < 3; ch++) {
        alarm_channel_t *a = &s_alarms[ch];
        bool have = (ch == 0) ? (s_temp_valid && !s_sensor_fail) : true;
        if (!have) {
            values[ch] = 0.0f;      /* no reading: cannot judge this one */
        }

        if (!a->active) {
            if (values[ch] >= a->threshold) {
                a->above_ms += dt_ms;
                a->below_ms = 0u;
                if (a->above_ms >= ALARM_CONFIRM_MS) {
                    a->active = true;
                    send_alarm_event((uint8_t)(1u << ch),
                                     (ch == 0) ? "overtemp" :
                                     (ch == 1) ? "overcurrent" : "vibration",
                                     true, values[ch], ALARM_VALUE_SCALE);
                    if ((1u << ch) & ALARM_TRIP_MASK) {
                        s_relay_latched = true;
                        bsp_relay_set(true);
                    }
                    if (!s_buzzer_muted) {
                        s_buzzer_until = now_ms + BUZZER_ON_ALARM_MS;
                    }
                }
            } else {
                a->above_ms = 0u;
            }
        } else {
            if (values[ch] <= a->clear_level) {
                a->below_ms += dt_ms;
                a->above_ms = 0u;
                if (a->below_ms >= ALARM_CLEAR_MS) {
                    a->active = false;
                    send_alarm_event((uint8_t)(1u << ch),
                                     (ch == 0) ? "overtemp" :
                                     (ch == 1) ? "overcurrent" : "vibration",
                                     false, values[ch], ALARM_VALUE_SCALE);
                }
            } else {
                a->below_ms = 0u;
            }
        }
    }

    /* Sensor-fail alarm (bit 3) and comm-fail alarm (bit 4). */
    bool sf = (s_ds_fail_streak >= DS_FAIL_STREAK);
    if (sf != s_sensor_fail) {
        s_sensor_fail = sf;
        send_alarm_event(ALARM_BIT_SENSOR_FAIL, "sensor-fail", sf,
                         (float)s_ds_fail_streak, 1);
    }
    if (s_comm_fail != ((s_alarm_bits & ALARM_BIT_COMM_FAIL) != 0u)) {
        send_alarm_event(ALARM_BIT_COMM_FAIL, "comm-fail", s_comm_fail,
                         0.0f, 1);
    }

    /* Recompose the alarm bitmask (also used by telemetry). */
    uint8_t bits = 0u;
    for (int ch = 0; ch < 3; ch++) {
        if (s_alarms[ch].active) {
            bits |= (uint8_t)(1u << ch);
        }
    }
    if (s_sensor_fail) {
        bits |= ALARM_BIT_SENSOR_FAIL;
    }
    if (s_comm_fail) {
        bits |= ALARM_BIT_COMM_FAIL;
    }
    s_alarm_bits = bits;
}

/* ================================================================== */
/*  Outputs: LED / buzzer / relay                                     */
/* ================================================================== */

static void outputs_tick(uint32_t now_ms, uint32_t tick_index)
{
    /* PC13: override (LED_ON) wins, else 1 Hz normal / fast on alarm. */
    if (s_led_green_override) {
        if (s_led_green_state != true) {
            s_led_green_state = true;
            bsp_led_set(true);
        }
    } else {
        uint32_t period = (s_alarm_bits != 0u) ? 150u : 500u;
        bool on = ((tick_index % (period / APP_TICK_MS))
                   < (period / (2u * APP_TICK_MS)));
        if (on != s_led_green_state) {
            s_led_green_state = on;
            bsp_led_set(on);
        }
    }

    /* Buzzer: active until its deadline. */
    if (s_buzzer_until != 0u) {
        if (now_ms >= s_buzzer_until) {
            s_buzzer_until = 0u;
            bsp_buzzer_off();
        } else {
            bsp_buzzer_tone(BUZZER_DEFAULT_HZ);
        }
    }
}

/* ================================================================== */
/*  Telemetry + heartbeat                                             */
/* ================================================================== */

static void send_telemetry(uint32_t now_ms)
{
    telemetry_payload_t p;
    p.temperature_c        = s_temp_valid ? s_temp_c : 0.0f;
    p.current_a            = s_current_a;
    p.vibration_g          = s_vib_g_est;
    p.alarm_bits           = s_alarm_bits;
    p.sensor_status        = (s_temp_valid && !s_sensor_fail
                                ? SENSOR_BIT_DS18B20_OK : 0u)
                             | (s_ina219_ok ? SENSOR_BIT_INA219_OK : 0u)
                             | (s_mpu6050_ok ? SENSOR_BIT_MPU6050_OK : 0u);
    p.cpu_load_pct         = s_cpu_load_10000;
    p.free_heap_bytes_div16 = (uint16_t)(xPortGetFreeHeapSize() / 16u);
    p.uptime_seconds       = (uint16_t)(now_ms / 1000u);

    (void)send_packet(MSG_TELEMETRY, s_seq_tel, (const uint8_t *)&p,
                      sizeof(p));
    s_seq_tel++;
}

static void send_status_response(void)
{
    resp_status_payload_t r;
    r.temperature_c         = s_temp_valid ? s_temp_c : 0.0f;
    r.current_a             = s_current_a;
    r.vibration_g           = s_vib_g_est;
    r.alarm_bits            = s_alarm_bits;
    r.sensor_status         = (s_temp_valid && !s_sensor_fail
                                 ? SENSOR_BIT_DS18B20_OK : 0u)
                              | (s_ina219_ok ? SENSOR_BIT_INA219_OK : 0u)
                              | (s_mpu6050_ok ? SENSOR_BIT_MPU6050_OK : 0u);
    r.cpu_load_pct          = (uint8_t)(s_cpu_load_10000 / 100u);
    r.task_states           = 0u;   /* single-task app: always healthy */
    r.free_heap_bytes_div16 = (uint16_t)(xPortGetFreeHeapSize() / 16u);
    r.uptime_seconds        = (uint16_t)(HAL_GetTick() / 1000u);
    r.threshold_temp_c      = s_alarms[0].threshold;
    r.threshold_current_a   = s_alarms[1].threshold;
    r.threshold_vibration_g = s_alarms[2].threshold;

    (void)send_packet(MSG_RESP_STATUS, s_seq_tel, (const uint8_t *)&r,
                      sizeof(r));
}

/* ================================================================== */
/*  Command handling (ESP32 -> STM32)                                 */
/* ================================================================== */

static void apply_threshold(uint8_t id, float value)
{
    switch (id) {
        case THRESHOLD_ID_TEMPERATURE:
            if (value < 0.0f)   { value = 0.0f; }
            if (value > 125.0f) { value = 125.0f; }
            s_alarms[0].threshold  = value;
            s_alarms[0].clear_level = value * 0.95f;
            break;
        case THRESHOLD_ID_CURRENT:
            if (value < 0.0f)  { value = 0.0f; }
            if (value > 30.0f) { value = 30.0f; }
            s_alarms[1].threshold  = value;
            s_alarms[1].clear_level = value * 0.9f;
            if (value * 1000.0f > (float)INA219_FULL_SCALE_MA) {
                /* The alarm could never fire: the sensor clips first. */
                (void)console_printf("[WARN] current threshold %d/100 A is above the INA219 range (%u mA)\r\n",
                                     (int)(value * 100.0f),
                                     (unsigned)INA219_FULL_SCALE_MA);
                send_debug(1u, "current threshold above INA219 range");
            }
            break;
        case THRESHOLD_ID_VIBRATION:
            if (value < 0.0f) { value = 0.0f; }
            if (value > 10.0f) { value = 10.0f; }
            s_alarms[2].threshold  = value;
            s_alarms[2].clear_level = value * 0.9f;
            break;
        default:
            break;
    }
}

static void handle_packet(const decoded_packet_t *p, uint32_t now_ms)
{
    s_last_rx_tick = now_ms;

    switch (p->type) {

    case MSG_HEARTBEAT:
        /* Liveness only — heartbeats are not acknowledged. */
        break;

    case MSG_CMD_SET_THRESHOLD: {
        if (p->payload_len != sizeof(cmd_set_threshold_payload_t)) {
            send_nak(p->seq, NAK_REASON_BAD_LENGTH);
            break;
        }
        cmd_set_threshold_payload_t c;
        memcpy(&c, p->payload, sizeof(c));
        if (c.threshold_id > 2u) {
            send_nak(p->seq, NAK_REASON_BAD_LENGTH);
            break;
        }
        apply_threshold(c.threshold_id, c.value);
        (void)console_printf("[CMD] threshold %u -> %d/100\r\n",
                             (unsigned)c.threshold_id,
                             (int)(c.value * 100.0f));
        send_debug(0u, "threshold updated");
        send_ack(p->seq, p->type);
        break;
    }

    case MSG_CMD_SET_SAMPLE_RATE: {
        if (p->payload_len != sizeof(cmd_set_sample_rate_payload_t)) {
            send_nak(p->seq, NAK_REASON_BAD_LENGTH);
            break;
        }
        cmd_set_sample_rate_payload_t c;
        memcpy(&c, p->payload, sizeof(c));
        if (c.sample_period_ms < 50u || c.sample_period_ms > 1000u) {
            send_nak(p->seq, NAK_REASON_BAD_LENGTH);
            break;
        }
        s_sample_period_ms = c.sample_period_ms;
        (void)console_printf("[CMD] sample period -> %u ms\r\n",
                             (unsigned)s_sample_period_ms);
        send_ack(p->seq, p->type);
        break;
    }

    case MSG_CMD_LED_ON:
    case MSG_CMD_LED_OFF: {
        if (p->payload_len != sizeof(cmd_led_payload_t)) {
            send_nak(p->seq, NAK_REASON_BAD_LENGTH);
            break;
        }
        cmd_led_payload_t c;
        memcpy(&c, p->payload, sizeof(c));
        bool on = (p->type == MSG_CMD_LED_ON);
        switch (c.led_id) {
            case LED_ID_GREEN:
                s_led_green_override = true;
                s_led_green_state = on;
                bsp_led_set(on);
                break;
            case LED_ID_RED:                 /* the relay plays "red"    */
                s_relay_latched = on;
                bsp_relay_set(on);
                break;
            case LED_ID_YELLOW:              /* not wired: acknowledge   */
                (void)console_println("[CMD] LED yellow: not wired, no-op");
                break;
            case LED_ID_ALL:
                s_led_green_override = true;
                s_led_green_state = on;
                bsp_led_set(on);
                s_relay_latched = on;
                bsp_relay_set(on);
                break;
            default:
                break;
        }
        send_ack(p->seq, p->type);
        break;
    }

    case MSG_CMD_BUZZER_OFF:
        s_buzzer_muted = true;
        s_buzzer_until = 0u;
        bsp_buzzer_off();
        (void)console_println("[CMD] buzzer muted");
        send_ack(p->seq, p->type);
        break;

    case MSG_CMD_RESET_ALARM:
        /* Operator acknowledge: clear every alarm, release the trip. */
        for (int ch = 0; ch < 3; ch++) {
            s_alarms[ch].active = false;
            s_alarms[ch].above_ms = 0u;
            s_alarms[ch].below_ms = 0u;
        }
        s_sensor_fail = (s_ds_fail_streak >= DS_FAIL_STREAK);
        s_buzzer_muted = false;
        s_buzzer_until = 0u;
        bsp_buzzer_off();
        s_relay_latched = false;
        bsp_relay_set(false);
        (void)console_println("[CMD] alarms reset by operator");
        send_debug(0u, "alarms reset");
        send_ack(p->seq, p->type);
        break;

    case MSG_CMD_GET_STATUS:
        send_ack(p->seq, p->type);
        send_status_response();
        break;

    case MSG_CMD_REBOOT:
        send_ack(p->seq, p->type);
        (void)console_println("[CMD] rebooting STM32...");
        (void)console_println("");
        vTaskDelay(pdMS_TO_TICKS(100));  /* let the ACK + print escape */
        NVIC_SystemReset();
        break;                            /* not reached                */

    case MSG_RESET:
        send_ack(p->seq, p->type);
        vTaskDelay(pdMS_TO_TICKS(100));
        NVIC_SystemReset();
        break;

    case MSG_TELEMETRY:        /* wrong direction: ours to send,      */
    case MSG_ALARM:            /* not to receive. Ignore silently.    */
    case MSG_RESP_STATUS:
    case MSG_ACK:              /* we keep no pending-command table,   */
    case MSG_NAK:              /* telemetry is fire-and-forget.       */
    case MSG_DEBUG_LOG:
        break;

    default:
        send_nak(p->seq, NAK_REASON_UNKNOWN_TYPE);
        break;
    }
}

static void rx_drain(uint32_t now_ms)
{
    static packet_decoder_t dec;
    static bool dec_inited = false;
    if (!dec_inited) {
        packet_decoder_init(&dec);
        dec_inited = true;
    }

    uint8_t byte;
    while (uart_link_get_byte(&byte)) {
        packet_result_t r = packet_decode_byte(&dec, byte);
        if (r == PACKET_OK) {
            handle_packet(&dec.decoded, now_ms);
        }
        /* CRC/framing errors: next frame resynchronises the decoder. */
    }
}

/* ================================================================== */
/*  Console status                                                    */
/* ================================================================== */

static void print_status_line(void)
{
    (void)console_printf(
        "T %d/10 C   I %d/100 A   V %d/100 g   ALM 0x%02X   CPU %u/100 %%   %s\r\n",
        (int)(s_temp_c * 10.0f),
        (int)(s_current_a * 100.0f),
        (int)(s_vib_g_est * 100.0f),
        (unsigned)s_alarm_bits,
        (unsigned)(s_cpu_load_10000 / 100u),
        s_comm_fail ? "COMM-FAIL" : "link ok");
}

/* I2C sensor diagnostics: the line to read first when a value looks wrong.
 * Integer-only formatting so the build needs no float printf. */
static void print_diag_line(void)
{
    char ina[56];
    char mpu[48];

    if (ina219_is_present()) {
        uint32_t bus_mv = 0u;
        int32_t  raw    = 0;
        (void)ina219_read_bus_mv(&bus_mv);
        (void)ina219_read_shunt_raw(&raw);
        snprintf(ina, sizeof(ina), "0x%02X bus %u.%02uV shunt %ld*10uV%s",
                 (unsigned)ina219_address(),
                 (unsigned)(bus_mv / 1000u), (unsigned)((bus_mv % 1000u) / 10u),
                 (long)raw, ina219_saturated() ? " CLIPPED" : "");
    } else {
        snprintf(ina, sizeof(ina), "NOT FOUND");
    }

    if (mpu6050_is_present()) {
        snprintf(mpu, sizeof(mpu), "0x%02X who 0x%02X",
                 (unsigned)mpu6050_address(), (unsigned)mpu6050_who_am_i());
    } else {
        snprintf(mpu, sizeof(mpu), "NOT FOUND");
    }

    (void)console_printf("[I2C] INA219 %s | MPU6050 %s | bus err %lu rec %lu\r\n",
                         ina, mpu,
                         (unsigned long)bsp_i2c_error_count(),
                         (unsigned long)bsp_i2c_recover_count());
}

/* ================================================================== */
/*  OLED live status page                                             */
/* ================================================================== */

/* Renders the current telemetry into the OLED framebuffer. The buffer
 * is rebuilt every OLED_REBUILD_MS; one page is then flushed per tick so
 * the whole 128x64 screen refreshes in eight ticks (~80 ms) without ever
 * blocking the loop long enough to overflow the UART RX ring. Numbers use
 * integer-only snprintf specs so the build needs no float-printf support. */
static void oled_render_status(void)
{
    char line[24];
    int t_int  = (int)s_temp_c;
    int t_frac = (int)((s_temp_c - (float)t_int) * 10.0f);
    if (t_frac < 0) { t_frac = -t_frac; }
    int i_int  = (int)s_current_a;
    int i_frac = (int)((s_current_a - (float)i_int) * 100.0f);
    if (i_frac < 0) { i_frac = -i_frac; }
    int v_int  = (int)s_vib_g_est;
    int v_frac = (int)((s_vib_g_est - (float)v_int) * 100.0f);
    if (v_frac < 0) { v_frac = -v_frac; }
    uint32_t up = HAL_GetTick() / 1000u;
    uint32_t hh = up / 3600u;
    uint32_t mm = (up / 60u) % 60u;
    uint32_t ss = up % 60u;

    oled_clear();
    oled_text(0, 0, "HEALTH MONITOR");

    snprintf(line, sizeof(line), "T %d.%d C", t_int, t_frac);
    oled_text(0, 10, line);
    snprintf(line, sizeof(line), "I %d.%02d A", i_int, i_frac);
    oled_text(72, 10, line);

    snprintf(line, sizeof(line), "V %d.%02d g", v_int, v_frac);
    oled_text(0, 20, line);

    if (s_alarm_bits == 0u) {
        oled_text(0, 30, "ALM OK");
    } else {
        snprintf(line, sizeof(line), "ALM 0x%02X", s_alarm_bits);
        oled_text(0, 30, line);
    }
    snprintf(line, sizeof(line), "S:%c%c%c",
             (s_temp_valid && !s_sensor_fail) ? (char)0x54 : (char)0x2D,
             s_ina219_ok  ? (char)0x49 : (char)0x2D,
             s_mpu6050_ok ? (char)0x56 : (char)0x2D);
    oled_text(96, 30, line);

    snprintf(line, sizeof(line), "UP %02u:%02u:%02u",
             (unsigned)hh, (unsigned)mm, (unsigned)ss);
    oled_text(0, 40, line);

    snprintf(line, sizeof(line), "CPU %u%%",
             (unsigned)(s_cpu_load_10000 / 100u));
    oled_text(0, 50, line);
    oled_text(80, 50, s_comm_fail ? "NO LINK" : "LINK OK");
}

/* ================================================================== */
/*  Application main loop                                             */
/* ================================================================== */

void app_telemetry_run(void)
{
    (void)console_println("");
    (void)console_println("==================================================");
    (void)console_println(" Telemetry application (Phase 3)");
    (void)console_println(" Press any key to return to the bring-up menu.");
    (void)console_println("==================================================");

    uart_link_init();           /* (re)arm interrupt RX                  */
    console_flush_rx();
    wdt_start();

    /* Bring up the I2C sensors and the OLED status display. Each is
     * optional: a missing device clears its ok-bit and the app keeps
     * running on whatever it can read, so a loose wire degrades the
     * dashboard instead of hanging the loop. */
    (void)bsp_i2c_recover();    /* free a bus left wedged by a previous run */
    (void)ina219_init();
    (void)mpu6050_init();
    s_ina219_ok  = ina219_is_present();
    s_mpu6050_ok = mpu6050_is_present();
    print_diag_line();
    if (!ina219_is_present()) {
        (void)console_println("[WARN] INA219 not found -- check wiring/address; will keep retrying");
    }
    if (!mpu6050_is_present()) {
        (void)console_println("[WARN] MPU6050 not found -- check wiring/AD0; will keep retrying");
    }
    if (oled_init()) {
        oled_clear();
        oled_text(0, 0, "HEALTH MONITOR");
        oled_text(0, 20, "BOOTING...");
        (void)oled_flush_all();
    }

    /* State reset (the menu may have run tests in between). */
    memset(s_cur_buf, 0, sizeof(s_cur_buf));
    s_cur_idx = 0u;
    s_cur_full = false;
    s_vib_seeded = false;
    s_vib_pwr = 0.0f;
    s_vib_g_est = 0.0f;
    s_cur_fail = 0u;
    s_vib_fail = 0u;
    s_current_a = 0.0f;
    s_next_service = HAL_GetTick() + SENSOR_SERVICE_MS;
    s_next_diag = HAL_GetTick() + DIAG_PRINT_MS;
    s_ds_state = DS_IDLE;
    s_ds_next_read = HAL_GetTick();
    s_last_rx_tick = HAL_GetTick();
    s_next_status = HAL_GetTick() + STATUS_PRINT_MS;
    s_next_oled_rebuild = HAL_GetTick() + OLED_REBUILD_MS;
    s_window_start_cy = bsp_cycles();
    s_window_ms = 0u;
    s_work_cycles_acc = 0u;

    uint32_t last_loop_ms = HAL_GetTick();
    uint32_t next_tel = HAL_GetTick() + s_sample_period_ms;
    uint32_t next_hb  = HAL_GetTick() + HB_PERIOD_MS;
    uint32_t tick_index = 0u;
    bool banner_sent = false;

    for (;;) {
        uint32_t now = HAL_GetTick();

        /* ---- bounded work slice ---- */
        uint32_t work_start = bsp_cycles();

        if (!banner_sent && now >= 500u) {
            send_debug(0u, "STM32 telemetry app booted");
            banner_sent = true;
        }

        /* Sensor supervision (~1 Hz): re-detect / re-configure / un-hang. */
        if ((int32_t)(now - s_next_service) >= 0) {
            s_next_service = now + SENSOR_SERVICE_MS;
            ina219_service();
            mpu6050_service();
        }

        sample_current();
        sample_vibration();
        ds_tick(now);
        rx_drain(now);

        /* Comm-fail detection (ESP32 silent?). */
        if (!s_comm_fail && (now - s_last_rx_tick) > COMM_FAIL_MS) {
            s_comm_fail = true;
            (void)console_println("[EVT] COMM FAIL: no ESP32 packet in 5 s");
        } else if (s_comm_fail && (now - s_last_rx_tick) <= COMM_FAIL_MS) {
            s_comm_fail = false;
            (void)console_println("[EVT] COMM RESTORED");
        }

        uint32_t dt_ms = now - last_loop_ms;
        last_loop_ms = now;
        alarm_engine_tick(now, dt_ms);
        outputs_tick(now, tick_index);

        /* Periodic transmits. */
        if ((int32_t)(now - next_tel) >= 0) {
            next_tel = now + s_sample_period_ms;
            send_telemetry(now);
        }
        if ((int32_t)(now - next_hb) >= 0) {
            next_hb = now + HB_PERIOD_MS;
            (void)send_packet(MSG_HEARTBEAT, s_seq_hb, NULL, 0u);
            s_seq_hb++;
        }

        /* Console status line. */
        if ((int32_t)(now - s_next_status) >= 0) {
            s_next_status = now + STATUS_PRINT_MS;
            if (console_can_print_now()) { print_status_line(); }
        }
        if ((int32_t)(now - s_next_diag) >= 0) {
            s_next_diag = now + DIAG_PRINT_MS;
            if (console_can_print_now()) { print_diag_line(); }
        }

        /* OLED: rebuild the framebuffer on a rolling cadence, then
         * flush ONE page per tick (round-robin). At 250 kHz I2C a
         * single page is ~4.6 ms, well inside the 5.6 ms the 256-byte
         * UART RX ring takes to fill at 460800 baud. */
        if ((int32_t)(now - s_next_oled_rebuild) >= 0) {
            s_next_oled_rebuild = now + OLED_REBUILD_MS;
            oled_render_status();
        }
        (void)oled_flush_page(s_oled_page);
        s_oled_page = (uint8_t)((s_oled_page + 1u) & 7u);

        /* CPU load bookkeeping over the last telemetry window. */
        s_work_cycles_acc += bsp_elapsed_us(work_start) * 96u; /* cycles  */
        s_window_ms += dt_ms;
        if (s_window_ms >= s_sample_period_ms) {
            uint32_t window_cycles = bsp_elapsed_us(s_window_start_cy) * 96u;
            if (window_cycles > 0u) {
                uint32_t pct = (s_work_cycles_acc * 10000u) / window_cycles;
                if (pct > 10000u) {
                    pct = 10000u;
                }
                s_cpu_load_10000 = (uint16_t)pct;
            }
            s_window_start_cy = bsp_cycles();
            s_work_cycles_acc = 0u;
            s_window_ms = 0u;
        }

        /* Exit on keypress. */
        if (console_key_pressed()) {
            console_flush_rx();
            break;
        }

        tick_index++;

        /* ---- end of work slice: feed, sleep ---- */
        wdt_feed();
        vTaskDelay(pdMS_TO_TICKS(APP_TICK_MS));
    }

    /* Leave the outputs in a safe state when handing back to the menu. */
    bsp_buzzer_off();
    bsp_relay_set(false);
    bsp_led_set(false);
    s_led_green_override = false;
    (void)console_println("");
    (void)console_println("Telemetry app stopped. Back to the menu.");
}
