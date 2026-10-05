/**
 * @file    command_dispatcher.h
 * @brief   Turns CLI/web intents into acknowledged UART commands.
 *
 * ESP32 -> STM32 commands are rare, state-changing, and MUST be
 * confirmed — the opposite of the fire-and-forget telemetry stream.
 * This module owns that confirmation: it encodes the packet, sends it,
 * waits for the STM32's MSG_ACK (or MSG_NAK), retries up to three
 * times, and reports the outcome to the caller.
 *
 * execute() is synchronous and may block up to @p timeout_ms. HTTP and
 * CLI callers accept that; the dashboard push path never commands.
 *
 * Sequence numbers start at 0x8000 so they never collide with the
 * heartbeat stream (task_uart_tx counts from 0) — the ACK payload
 * echoes (seq, type), so either stream is identifiable either way.
 */

#ifndef COMMAND_DISPATCHER_H
#define COMMAND_DISPATCHER_H

#include <stdint.h>
#include <stdbool.h>

#include "protocol_types.h"

typedef enum {
    CMD_OK     = 0,   /* STM32 applied the command                      */
    CMD_NAKED  = 1,   /* STM32 rejected it (bad value/length)           */
    CMD_NO_ACK = 2,   /* silent after all retries                       */
    CMD_ERR    = 3,   /* local failure: encode or UART send             */
} cmd_result_t;

/** Create mutexes. Call once at boot. */
void command_dispatcher_init(void);

/**
 * Send one command and wait for its ACK/NAK.
 * @param timeout_ms  total budget across the (up to) 3 attempts
 */
cmd_result_t command_dispatcher_execute(msg_type_t type,
                                        const uint8_t *payload,
                                        uint8_t len,
                                        uint32_t timeout_ms);

/* RX-task hooks: call for every MSG_ACK / MSG_NAK received. */
void command_dispatcher_on_ack(uint16_t acked_seq, uint8_t acked_type);
void command_dispatcher_on_nak(uint16_t nak_seq, uint8_t reason);

/* Retry/timeout counters for the dashboard stats block. */
void command_dispatcher_get_stats(uint32_t *retries, uint32_t *timeouts);

/* ---- Convenience wrappers (shared by CLI and web) ---------------- */

cmd_result_t command_set_threshold(uint8_t id, float value, uint32_t tmo);
cmd_result_t command_set_sample_rate(uint16_t period_ms, uint32_t tmo);
cmd_result_t command_led(uint8_t led_id, bool on, uint32_t tmo);
cmd_result_t command_alarm_reset(uint32_t tmo);
cmd_result_t command_reboot_stm32(uint32_t tmo);
cmd_result_t command_get_status(uint32_t tmo);

#endif /* COMMAND_DISPATCHER_H */
