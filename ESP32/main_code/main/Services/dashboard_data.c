/**
 * @file    dashboard_data.c
 * @brief   Live state cache implementation.
 *
 * One mutex guards everything. Every producer is quick (memcpy-scale),
 * and the two potentially slow readers — the 1 Hz JSON build and the
 * bulk history copy — take at most a few hundred microseconds, so a
 * priority-inverted producer would still meet its 200 ms deadline with
 * three orders of magnitude to spare.
 *
 * The history storage is a static array (57.6 KB). It could be heap
 * allocated, but a fixed .bss block survives heap fragmentation and
 * makes the memory budget visible in the map file.
 */

#include "dashboard_data.h"
#include "json_util.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

/* ================================================================== */
/*  State                                                             */
/* ================================================================== */

static SemaphoreHandle_t s_lock;

/* Live STM32 view (from the newest telemetry packet). */
static float    s_temp_c        = 0.0f;
static float    s_current_a     = 0.0f;
static float    s_vib_g         = 0.0f;
static uint8_t  s_alarm_bits    = 0u;
static uint8_t  s_sensor_status = 0u;
static uint16_t s_stm32_cpu_10000 = 0u;
static uint32_t s_stm32_heap    = 0u;
static uint16_t s_stm32_uptime  = 0u;
static uint32_t s_last_tel_ms   = 0u;
static bool     s_ever_linked   = false;
static bool     s_comm_fail     = false;

/* Config mirror. Defaults match the STM32 firmware — the values here
 * are what the dashboard shows until a RESP_STATUS or a local note
 * refreshes them. */
static float    s_thr[3]        = { 60.0f, 2.5f, 0.5f };   /* INA219 0.1 ohm reads up to 3.2 A */
static uint16_t s_sample_ms     = 200u;

/* ESP32-side facts. */
static int      s_wifi_state    = DASH_WIFI_CONNECTING;
static char     s_wifi_ssid[33] = "";
static char     s_wifi_ip[16]   = "";
static int      s_wifi_rssi     = 0;
static uint32_t s_esp_heap      = 0u;
static uint32_t s_esp_min_heap  = 0u;
static int      s_esp_cpu_pct   = -1;   /* -1 until measured */

/* Link stats. */
static uint32_t s_rx_pkts, s_crc_errors, s_tx_pkts, s_tx_retries;
static uint32_t s_cmd_timeouts;

/* History ring: 1 Hz x 3600 s. */
#define DASH_HISTORY_CAP 3600u
static hr_sample_t    s_hist_buf[DASH_HISTORY_CAP];
static history_ring_t s_hist;

/* Event log. */
static dash_event_t s_events[DASH_EVENT_LOG_LEN];
static uint8_t      s_ev_head = 0u, s_ev_count = 0u;

/* ================================================================== */
/*  Helpers                                                           */
/* ================================================================== */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static const char *alarm_name(uint8_t bit)
{
    switch (bit) {
        case ALARM_BIT_OVERTEMP:     return "overtemp";
        case ALARM_BIT_OVERCURRENT:  return "overcurrent";
        case ALARM_BIT_VIBRATION:    return "vibration";
        case ALARM_BIT_SENSOR_FAIL:  return "sensor-fail";
        case ALARM_BIT_COMM_FAIL:    return "comm-fail";
        default:                     return "alarm";
    }
}

static void event_push(uint8_t kind, uint8_t bit, int32_t val_x100,
                       const char *text)
{
    dash_event_t *e = &s_events[s_ev_head];
    e->t_ms = now_ms();
    e->kind = kind;
    e->bit  = bit;
    e->val_x100 = val_x100;
    (void)snprintf(e->text, sizeof(e->text), "%s",
                   (text != NULL) ? text : alarm_name(bit));
    s_ev_head = (uint8_t)((s_ev_head + 1u) % DASH_EVENT_LOG_LEN);
    if (s_ev_count < DASH_EVENT_LOG_LEN) {
        s_ev_count++;
    }
}

/* ================================================================== */
/*  Init                                                              */
/* ================================================================== */

void dashboard_data_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    history_ring_init(&s_hist, s_hist_buf, DASH_HISTORY_CAP);
}

/* ================================================================== */
/*  Producers                                                         */
/* ================================================================== */

void dashboard_data_telemetry_arrived(const telemetry_payload_t *t)
{
    if (t == NULL) {
        return;
    }
    lock();
    s_temp_c          = t->temperature_c;
    s_current_a       = t->current_a;
    s_vib_g           = t->vibration_g;
    s_alarm_bits      = t->alarm_bits;
    s_sensor_status   = t->sensor_status;
    s_stm32_cpu_10000 = t->cpu_load_pct;
    s_stm32_heap      = (uint32_t)t->free_heap_bytes_div16 * 16u;
    s_stm32_uptime    = t->uptime_seconds;
    s_last_tel_ms     = now_ms();
    s_ever_linked     = true;
    if (s_comm_fail) {
        s_comm_fail = false;
        event_push(1, ALARM_BIT_COMM_FAIL, 0, "comm-fail");
    }
    unlock();
}

void dashboard_data_alarm_arrived(const alarm_payload_t *a)
{
    if (a == NULL) {
        return;
    }
    lock();
    if (a->state != 0u) {
        s_alarm_bits |= a->alarm_bit;
    } else {
        s_alarm_bits &= (uint8_t)~a->alarm_bit;
    }
    event_push((a->state != 0u) ? 0u : 1u, a->alarm_bit,
               a->value_at_trigger, NULL);
    unlock();
}

void dashboard_data_status_arrived(const resp_status_payload_t *s)
{
    if (s == NULL) {
        return;
    }
    lock();
    s_thr[0] = s->threshold_temp_c;
    s_thr[1] = s->threshold_current_a;
    s_thr[2] = s->threshold_vibration_g;
    s_temp_c        = s->temperature_c;
    s_current_a     = s->current_a;
    s_vib_g         = s->vibration_g;
    s_alarm_bits    = s->alarm_bits;
    s_sensor_status = s->sensor_status;
    s_stm32_cpu_10000 = (uint16_t)(s->cpu_load_pct * 100u);
    s_stm32_heap    = (uint32_t)s->free_heap_bytes_div16 * 16u;
    s_stm32_uptime  = s->uptime_seconds;
    unlock();
}

void dashboard_data_debug_log_arrived(uint8_t level, const char *msg)
{
    if (msg == NULL) {
        return;
    }
    lock();
    event_push(2u, 0u, (int32_t)level, msg);
    unlock();
}

void dashboard_data_set_comm_fail(bool fail)
{
    lock();
    if (fail != s_comm_fail) {
        s_comm_fail = fail;
        event_push(fail ? 0u : 1u, ALARM_BIT_COMM_FAIL, 0, "comm-fail");
    }
    unlock();
}

void dashboard_data_set_link_stats(uint32_t rx_pkts, uint32_t crc_errors,
                                   uint32_t tx_pkts, uint32_t tx_retries,
                                   uint32_t cmd_timeouts)
{
    lock();
    s_rx_pkts      = rx_pkts;
    s_crc_errors   = crc_errors;
    s_tx_pkts      = tx_pkts;
    s_tx_retries   = tx_retries;
    s_cmd_timeouts = cmd_timeouts;
    unlock();
}

/* ================================================================== */
/*  Config mirror                                                     */
/* ================================================================== */

void dashboard_data_note_threshold(uint8_t id, float value)
{
    lock();
    if (id < 3u) {
        s_thr[id] = value;
        char tag[12];
        (void)snprintf(tag, sizeof(tag), "thr-%s",
                       (id == 0) ? "temp" : (id == 1) ? "cur" : "vib");
        event_push(2u, 0u, (int32_t)(value * 100.0f), tag);
    }
    unlock();
}

void dashboard_data_note_sample_rate(uint16_t period_ms)
{
    lock();
    s_sample_ms = period_ms;
    unlock();
}

void dashboard_data_set_config(const float thr[3], uint16_t period_ms)
{
    if (thr == NULL) {
        return;
    }
    lock();
    s_thr[0] = thr[0];
    s_thr[1] = thr[1];
    s_thr[2] = thr[2];
    s_sample_ms = period_ms;
    unlock();
}

void dashboard_data_get_thresholds(float out[3])
{
    if (out == NULL) {
        return;
    }
    lock();
    out[0] = s_thr[0];
    out[1] = s_thr[1];
    out[2] = s_thr[2];
    unlock();
}

/* ================================================================== */
/*  ESP32-side facts                                                  */
/* ================================================================== */

void dashboard_data_note_wifi(int state, const char *ssid,
                              const char *ip, int rssi)
{
    lock();
    if (state != s_wifi_state) {
        static const char *names[3] = { "wifi-ap", "wifi-conn", "wifi-up" };
        event_push(2u, 0u, 0, names[(state >= 0 && state <= 2) ? state : 1]);
    }
    s_wifi_state = state;
    if (ssid != NULL) {
        (void)snprintf(s_wifi_ssid, sizeof(s_wifi_ssid), "%s", ssid);
    }
    if (ip != NULL) {
        (void)snprintf(s_wifi_ip, sizeof(s_wifi_ip), "%s", ip);
    }
    s_wifi_rssi = rssi;
    unlock();
}

void dashboard_data_note_esp_stats(uint32_t free_heap, uint32_t min_heap,
                                   int cpu_pct)
{
    lock();
    s_esp_heap     = free_heap;
    s_esp_min_heap = min_heap;
    s_esp_cpu_pct  = cpu_pct;
    unlock();
}

/* ================================================================== */
/*  Readers                                                           */
/* ================================================================== */

void dashboard_data_sample_tick(void)
{
    lock();
    float vals[3] = { s_temp_c, s_current_a, s_vib_g };
    history_ring_push(&s_hist, now_ms(), vals);
    unlock();
}

uint16_t dashboard_data_copy_history_since(uint32_t since_ms,
                                           hr_sample_t *out, uint16_t max)
{
    lock();
    uint16_t n = history_ring_copy_since(&s_hist, since_ms, out, max);
    unlock();
    return n;
}

void dashboard_data_snapshot(dash_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    lock();
    uint32_t t = now_ms();
    out->temp_c = s_temp_c;
    out->current_a = s_current_a;
    out->vib_g = s_vib_g;
    out->alarm_bits = s_alarm_bits;
    out->sensor_status = s_sensor_status;
    out->stm32_uptime_s = s_stm32_uptime;
    out->stm32_heap = s_stm32_heap;
    out->stm32_cpu_10000 = s_stm32_cpu_10000;
    out->link_up = !s_comm_fail;
    out->ever_linked = s_ever_linked;
    out->telemetry_age_ms = s_ever_linked ? (t - s_last_tel_ms) : UINT32_MAX;
    out->thr[0] = s_thr[0];
    out->thr[1] = s_thr[1];
    out->thr[2] = s_thr[2];
    out->sample_period_ms = s_sample_ms;
    unlock();
}

uint32_t dashboard_data_uptime_ms(void)
{
    return now_ms();
}

bool dashboard_data_ever_linked(void)
{
    return s_ever_linked;        /* set under lock, read as a unit */
}

bool dashboard_data_link_up(void)
{
    return !s_comm_fail;
}

uint32_t dashboard_data_telemetry_age_ms(void)
{
    if (!s_ever_linked) {
        return UINT32_MAX;
    }
    return now_ms() - s_last_tel_ms;
}

float dashboard_data_get_threshold(uint8_t id)
{
    float v = 0.0f;
    lock();
    if (id < 3u) {
        v = s_thr[id];
    }
    unlock();
    return v;
}

uint16_t dashboard_data_get_sample_rate(void)
{
    return s_sample_ms;
}

/* ================================================================== */
/*  Status JSON                                                       */
/* ================================================================== */

size_t dashboard_data_build_status_json(char *buf, size_t cap)
{
    jsonw_t w;
    jsonw_init(&w, buf, cap);

    lock();
    uint32_t t    = now_ms();
    uint32_t age  = s_ever_linked ? (t - s_last_tel_ms) : 0xFFFFFFFFu;
    float    temp = s_temp_c, cur = s_current_a, vib = s_vib_g;
    uint8_t  alm  = s_alarm_bits, sst = s_sensor_status;
    uint16_t cpu  = s_stm32_cpu_10000;
    uint32_t heap = s_stm32_heap;
    uint16_t up   = s_stm32_uptime;
    float    t0 = s_thr[0], t1 = s_thr[1], t2 = s_thr[2];
    uint16_t rate = s_sample_ms;
    bool     link = !s_comm_fail;
    int      wstate = s_wifi_state;
    char     ssid[33], ip[16];
    memcpy(ssid, s_wifi_ssid, sizeof(ssid));
    memcpy(ip,   s_wifi_ip,   sizeof(ip));
    int      rssi = s_wifi_rssi;
    uint32_t eheap = s_esp_heap, emin = s_esp_min_heap;
    int      ecpu = s_esp_cpu_pct;
    uint32_t rx = s_rx_pkts, crc = s_crc_errors, tx = s_tx_pkts;
    uint32_t retr = s_tx_retries, cto = s_cmd_timeouts;
    dash_event_t evs[DASH_EVENT_LOG_LEN];
    uint8_t ev_count = s_ev_count;
    {
        /* Copy the event log oldest-first. */
        for (uint8_t i = 0; i < s_ev_count; i++) {
            uint8_t slot = (uint8_t)((s_ev_head + DASH_EVENT_LOG_LEN
                                      - s_ev_count + i) % DASH_EVENT_LOG_LEN);
            evs[i] = s_events[slot];
        }
    }
    unlock();

    jsonw_raw(&w, "{\"type\":\"status\"");
    jsonw_raw(&w, ",\"ts\":");   jsonw_int(&w, (long long)t);
    jsonw_raw(&w, ",\"link\":"); jsonw_bool(&w, link);
    jsonw_raw(&w, ",\"age\":");  jsonw_int(&w, (long long)age);
    jsonw_raw(&w, ",\"temp\":"); jsonw_num(&w, temp);
    jsonw_raw(&w, ",\"cur\":");  jsonw_numf(&w, cur, 3);   /* mA resolution */
    jsonw_raw(&w, ",\"vib\":");  jsonw_numf(&w, vib, 3);
    jsonw_raw(&w, ",\"alm\":");  jsonw_int(&w, alm);
    jsonw_raw(&w, ",\"sstat\":");jsonw_int(&w, sst);
    jsonw_raw(&w, ",\"up\":");   jsonw_int(&w, (long long)up);
    jsonw_raw(&w, ",\"cpu\":");  jsonw_num(&w, (double)cpu / 100.0);
    jsonw_raw(&w, ",\"heap\":"); jsonw_int(&w, (long long)heap);
    jsonw_raw(&w, ",\"th\":{");
    jsonw_raw(&w, "\"t\":");     jsonw_num(&w, t0);
    jsonw_raw(&w, ",\"c\":");    jsonw_num(&w, t1);
    jsonw_raw(&w, ",\"v\":");    jsonw_num(&w, t2);
    jsonw_raw(&w, "}");
    jsonw_raw(&w, ",\"rate\":"); jsonw_int(&w, rate);
    jsonw_raw(&w, ",\"esp\":{");
    jsonw_raw(&w, "\"up\":");    jsonw_int(&w, (long long)(t / 1000u));
    jsonw_raw(&w, ",\"heap\":"); jsonw_int(&w, (long long)eheap);
    jsonw_raw(&w, ",\"min\":");  jsonw_int(&w, (long long)emin);
    jsonw_raw(&w, ",\"cpu\":");
    if (ecpu < 0) { jsonw_raw(&w, "null"); } else { jsonw_int(&w, ecpu); }
    jsonw_raw(&w, ",\"rssi\":"); jsonw_int(&w, rssi);
    jsonw_raw(&w, ",\"wifi\":"); jsonw_int(&w, wstate);
    jsonw_raw(&w, ",\"ssid\":"); jsonw_str(&w, ssid);
    jsonw_raw(&w, ",\"ip\":");   jsonw_str(&w, ip);
    jsonw_raw(&w, "}");
    jsonw_raw(&w, ",\"stats\":{");
    jsonw_raw(&w, "\"rx\":");    jsonw_int(&w, (long long)rx);
    jsonw_raw(&w, ",\"crc\":");  jsonw_int(&w, (long long)crc);
    jsonw_raw(&w, ",\"tx\":");   jsonw_int(&w, (long long)tx);
    jsonw_raw(&w, ",\"retries\":"); jsonw_int(&w, (long long)retr);
    jsonw_raw(&w, ",\"timeouts\":"); jsonw_int(&w, (long long)cto);
    jsonw_raw(&w, "}");

    jsonw_raw(&w, ",\"ev\":[");
    uint8_t first = (ev_count > 12u) ? (uint8_t)(ev_count - 12u) : 0u;
    for (uint8_t i = first; i < ev_count; i++) {
        if (i != first) {
            jsonw_raw(&w, ",");
        }
        jsonw_raw(&w, "{\"ts\":");
        jsonw_int(&w, (long long)evs[i].t_ms);
        jsonw_raw(&w, ",\"k\":");
        jsonw_int(&w, evs[i].kind);
        jsonw_raw(&w, ",\"b\":");
        jsonw_int(&w, evs[i].bit);
        jsonw_raw(&w, ",\"v\":");
        jsonw_int(&w, (long long)evs[i].val_x100);
        jsonw_raw(&w, ",\"s\":");
        jsonw_str(&w, evs[i].text);
        jsonw_raw(&w, "}");
    }
    jsonw_raw(&w, "]}");

    return jsonw_len(&w);
}

size_t dashboard_data_build_live_json(char *buf, size_t cap)
{
    jsonw_t w;
    jsonw_init(&w, buf, cap);
    lock();
    uint32_t t = now_ms();
    uint32_t age = s_ever_linked ? (t - s_last_tel_ms) : 0xFFFFFFFFu;
    float temp = s_temp_c, cur = s_current_a, vib = s_vib_g;
    uint8_t alm = s_alarm_bits, sst = s_sensor_status;
    bool link = !s_comm_fail;
    unlock();

    jsonw_raw(&w, "{\"type\":\"live\",\"ts\":");  jsonw_int(&w, (long long)t);
    jsonw_raw(&w, ",\"link\":");  jsonw_bool(&w, link);
    jsonw_raw(&w, ",\"age\":");   jsonw_int(&w, (long long)age);
    jsonw_raw(&w, ",\"temp\":");  jsonw_num(&w, temp);
    jsonw_raw(&w, ",\"cur\":");   jsonw_numf(&w, cur, 3);
    jsonw_raw(&w, ",\"vib\":");   jsonw_numf(&w, vib, 3);
    jsonw_raw(&w, ",\"alm\":");   jsonw_int(&w, alm);
    jsonw_raw(&w, ",\"sstat\":"); jsonw_int(&w, sst);
    jsonw_raw(&w, "}");
    return jsonw_len(&w);
}
