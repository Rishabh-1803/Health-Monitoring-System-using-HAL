/**
 * @file    command_dispatcher.c
 * @brief   Acknowledged command execution with bounded retries.
 *
 * Concurrency: a mutex serialises execute() callers; a binary semaphore
 * carries the RX-task reply. on_ack()/on_nak() NEVER take the busy
 * mutex — they only compare the pending (seq, type) pair and give the
 * reply semaphore — so an execute() waiting for a reply can never
 * deadlock against the RX task that delivers it.
 */

#include "command_dispatcher.h"
#include "uart_link.h"
#include "packet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "CMD_DISPATCH";

#define CMD_MAX_ATTEMPTS   3
#define CMD_SEQ_START      0x8000u
#define CMD_MIN_TRY_MS     150u

static SemaphoreHandle_t s_busy;
static SemaphoreHandle_t s_reply;

static volatile uint16_t s_pending_seq;
static volatile uint8_t  s_pending_type;
static volatile int      s_result;      /* 0=ACK 1=NAK 2=timeout */

static uint16_t s_next_seq = CMD_SEQ_START;
static uint32_t s_retries, s_timeouts;

void command_dispatcher_init(void)
{
    s_busy  = xSemaphoreCreateMutex();
    s_reply = xSemaphoreCreateBinary();
}

static cmd_result_t send_once(msg_type_t type, const uint8_t *payload,
                              uint8_t len, uint32_t wait_ms)
{
    /* Drain any stale reply token before arming a new one. */
    while (xSemaphoreTake(s_reply, 0) == pdTRUE) {
    }

    s_result       = 2;
    s_pending_type = (uint8_t)type;
    s_pending_seq  = s_next_seq++;

    uint8_t pkt[PACKET_MAX_SIZE];
    int n = packet_encode(pkt, sizeof(pkt), type, s_pending_seq,
                          payload, len);
    if (n <= 0 || !uart_link_send(pkt, (uint32_t)n)) {
        return CMD_ERR;
    }

    if (xSemaphoreTake(s_reply, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return CMD_NO_ACK;
    }
    return (s_result == 0) ? CMD_OK : CMD_NAKED;
}

cmd_result_t command_dispatcher_execute(msg_type_t type,
                                        const uint8_t *payload,
                                        uint8_t len,
                                        uint32_t timeout_ms)
{
    if (s_busy == NULL) {
        return CMD_ERR;
    }
    xSemaphoreTake(s_busy, portMAX_DELAY);

    uint32_t per_try = timeout_ms / CMD_MAX_ATTEMPTS;
    if (per_try < CMD_MIN_TRY_MS) {
        per_try = CMD_MIN_TRY_MS;
    }

    cmd_result_t r = CMD_ERR;
    for (int attempt = 0; attempt < CMD_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            s_retries++;
            ESP_LOGW(TAG, "no ACK for type=0x%02X seq=%u — retry %d/%d",
                     (unsigned)type, (unsigned)s_pending_seq,
                     attempt + 1, CMD_MAX_ATTEMPTS);
        }
        r = send_once(type, payload, len, per_try);
        if (r != CMD_NO_ACK) {
            break;                  /* ACK, NAK, or local error: done    */
        }
    }
    if (r == CMD_NO_ACK) {
        s_timeouts++;
    }

    s_pending_type = 0u;            /* disarm                            */
    xSemaphoreGive(s_busy);
    return r;
}

void command_dispatcher_on_ack(uint16_t acked_seq, uint8_t acked_type)
{
    if (acked_type != 0u && acked_type == s_pending_type
        && acked_seq == s_pending_seq) {
        s_result = 0;
        (void)xSemaphoreGive(s_reply);
    }
}

void command_dispatcher_on_nak(uint16_t nak_seq, uint8_t reason)
{
    if (nak_seq == s_pending_seq && s_pending_type != 0u) {
        s_result = 1;
        ESP_LOGW(TAG, "NAK for seq=%u reason=%u",
                 (unsigned)nak_seq, (unsigned)reason);
        (void)xSemaphoreGive(s_reply);
    }
}

void command_dispatcher_get_stats(uint32_t *retries, uint32_t *timeouts)
{
    if (retries != NULL)  { *retries = s_retries; }
    if (timeouts != NULL) { *timeouts = s_timeouts; }
}

/* ================================================================== */
/*  Convenience wrappers                                              */
/* ================================================================== */

cmd_result_t command_set_threshold(uint8_t id, float value, uint32_t tmo)
{
    cmd_set_threshold_payload_t p;
    memset(&p, 0, sizeof(p));
    p.threshold_id = id;
    p.value        = value;
    return command_dispatcher_execute(MSG_CMD_SET_THRESHOLD,
                                      (const uint8_t *)&p, sizeof(p), tmo);
}

cmd_result_t command_set_sample_rate(uint16_t period_ms, uint32_t tmo)
{
    cmd_set_sample_rate_payload_t p;
    memset(&p, 0, sizeof(p));
    p.sample_period_ms = period_ms;
    return command_dispatcher_execute(MSG_CMD_SET_SAMPLE_RATE,
                                      (const uint8_t *)&p, sizeof(p), tmo);
}

cmd_result_t command_led(uint8_t led_id, bool on, uint32_t tmo)
{
    cmd_led_payload_t p;
    memset(&p, 0, sizeof(p));
    p.led_id = led_id;
    return command_dispatcher_execute(
        on ? MSG_CMD_LED_ON : MSG_CMD_LED_OFF,
        (const uint8_t *)&p, sizeof(p), tmo);
}

cmd_result_t command_alarm_reset(uint32_t tmo)
{
    return command_dispatcher_execute(MSG_CMD_RESET_ALARM, NULL, 0u, tmo);
}

cmd_result_t command_reboot_stm32(uint32_t tmo)
{
    return command_dispatcher_execute(MSG_CMD_REBOOT, NULL, 0u, tmo);
}

cmd_result_t command_get_status(uint32_t tmo)
{
    return command_dispatcher_execute(MSG_CMD_GET_STATUS, NULL, 0u, tmo);
}
