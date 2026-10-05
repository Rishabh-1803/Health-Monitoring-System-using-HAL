/**
 * @file    dashboard_data.h
 * @brief   Live state cache: latest telemetry, alarms, events, history.
 *
 * THE thread boundary. The UART RX task (producer), the dashboard task
 * (1 Hz sampler + JSON builder), and HTTP/CLI callers (readers, config
 * notes) all meet here — and nowhere else. Every accessor takes an
 * internal mutex, so callers never need their own locking.
 *
 * Rate domains, deliberately separated:
 *   - STM32 telemetry arrives at the sample rate (default 5 Hz)
 *   - task_dashboard samples the LATEST values at exactly 1 Hz into the
 *     history ring (one hour of data, 3600 x 16 bytes)
 *   - WebSocket clients are pushed the status JSON at 1 Hz
 */

#ifndef DASHBOARD_DATA_H
#define DASHBOARD_DATA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "protocol_types.h"
#include "history_ring.h"

/* ---- Event log (fixed, overwritten oldest-first) ---- */
#define DASH_EVENT_LOG_LEN   24
#define DASH_EVENT_TEXT_MAX  20

typedef struct {
    uint32_t t_ms;                              /* ESP32 uptime at event */
    uint8_t  kind;    /* 0=alarm raised 1=alarm cleared 2=info           */
    uint8_t  bit;     /* alarm bit, 0 for info events                    */
    int32_t  val_x100;/* value at trigger, x100                          */
    char     text[DASH_EVENT_TEXT_MAX + 1];
} dash_event_t;

/* ---- WiFi states reported in the status JSON ---- */
enum {
    DASH_WIFI_PROVISIONING = 0,   /* SoftAP + captive portal              */
    DASH_WIFI_CONNECTING   = 1,
    DASH_WIFI_CONNECTED    = 2,
};

/* ================================================================== */
/*  Init                                                              */
/* ================================================================== */

void dashboard_data_init(void);

/* ================================================================== */
/*  Producers (UART RX task + friends)                                */
/* ================================================================== */

/** A MSG_TELEMETRY payload arrived. Updates the live view. */
void dashboard_data_telemetry_arrived(const telemetry_payload_t *t);

/** A MSG_ALARM payload arrived (raise or clear). */
void dashboard_data_alarm_arrived(const alarm_payload_t *a);

/** A MSG_RESP_STATUS payload arrived (thresholds mirror). */
void dashboard_data_status_arrived(const resp_status_payload_t *s);

/** A MSG_DEBUG_LOG payload arrived from the STM32. */
void dashboard_data_debug_log_arrived(uint8_t level, const char *msg);

/** Comm-fail state changed (driven by task_uart_tx's 5 s rule). */
void dashboard_data_set_comm_fail(bool fail);

/** Link counters for the status JSON (called by task_dashboard). */
void dashboard_data_set_link_stats(uint32_t rx_pkts, uint32_t crc_errors,
                                   uint32_t tx_pkts, uint32_t tx_retries,
                                   uint32_t cmd_timeouts);

/* ================================================================== */
/*  Config mirror (CLI / web change what we push to the STM32)         */
/* ================================================================== */

void dashboard_data_note_threshold(uint8_t id, float value);
void dashboard_data_note_sample_rate(uint16_t period_ms);

/** Boot-time seeding from the persisted config. */
void dashboard_data_set_config(const float thr[3], uint16_t period_ms);

/** All three thresholds at once (for persistence). */
void dashboard_data_get_thresholds(float out[3]);

/* ================================================================== */
/*  ESP32-side facts (wifi manager, stats sampler)                     */
/* ================================================================== */

void dashboard_data_note_wifi(int state, const char *ssid,
                              const char *ip, int rssi);
void dashboard_data_note_esp_stats(uint32_t free_heap, uint32_t min_heap,
                                   int cpu_pct);

/* ================================================================== */
/*  Readers                                                           */
/* ================================================================== */

/**
 * Build the full status JSON into buf (NUL-terminated).
 * @return bytes written (excluding NUL); >= cap means truncated.
 */
size_t dashboard_data_build_status_json(char *buf, size_t cap);

/** 1 Hz sampler: push the current live values into the history ring. */
void dashboard_data_sample_tick(void);

/** Copy history samples with t_ms >= since_ms (oldest first). */
uint16_t dashboard_data_copy_history_since(uint32_t since_ms,
                                           hr_sample_t *out, uint16_t max);

/** Plain-C snapshot for the CLI (no JSON involved). */
typedef struct {
    float    temp_c, current_a, vib_g;
    uint8_t  alarm_bits, sensor_status;
    uint16_t stm32_uptime_s;
    uint32_t stm32_heap;
    uint16_t stm32_cpu_10000;
    bool     link_up;
    bool     ever_linked;
    uint32_t telemetry_age_ms;
    float    thr[3];
    uint16_t sample_period_ms;
} dash_snapshot_t;

void dashboard_data_snapshot(dash_snapshot_t *out);

/** Milliseconds since boot. */
uint32_t dashboard_data_uptime_ms(void);

/** True once any telemetry has been received since boot. */
bool dashboard_data_ever_linked(void);

/** True when the STM32 link is currently up (comm-fail = false). */
bool dashboard_data_link_up(void);

/** Age of the newest telemetry sample in ms (UINT32_MAX when none). */
uint32_t dashboard_data_telemetry_age_ms(void);

float    dashboard_data_get_threshold(uint8_t id);
uint16_t dashboard_data_get_sample_rate(void);

#endif /* DASHBOARD_DATA_H */
