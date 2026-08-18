/**
 * ============================================================
 * log_queue.c
 * ============================================================
 * Implementation of the logger queue.
 *
 * IMPORTANT DESIGN DECISIONS:
 *
 * 1. The QueueHandle_t is STATIC (file-private). No other file
 *    can call xQueueSend on it directly. This enforces the
 *    "only log_send() can enqueue" rule at the compiler level.
 *
 * 2. log_send() uses xQueueSend with timeout=0 (non-blocking).
 *    If the queue is full, we DROP the message rather than
 *    block the caller. This is intentional — in an alarm
 *    situation, we'd rather lose a log than freeze the sensor
 *    task. In the real project we'll add a drop counter for
 *    observability.
 *
 * 3. Message size is FIXED (160 bytes for the string). Variable-
 *    length messages in queues are a headache — pointers require
 *    careful memory ownership. Fixed-size = simple, safe, fast.
 * ============================================================ */

#include "log_queue.h"
#include "project_config.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static QueueHandle_t s_log_queue = NULL;
static uint32_t      s_drop_count = 0;  // Not exposed in warmup, but real projects track this

void log_queue_init(void) {
    s_log_queue = xQueueCreate(LOGGER_QUEUE_LEN, sizeof(log_message_t));
    configASSERT(s_log_queue != NULL);
}

void log_send(log_level_t level, const char *tag, const char *fmt, ...) {
    if (s_log_queue == NULL) return;

    log_message_t msg = {0};
    msg.level = level;
    msg.tag   = tag;
    msg.tick  = xTaskGetTickCount();

    va_list args;
    va_start(args, fmt);
    /* vsnprintf truncates safely if output exceeds buffer — never overflows */
    vsnprintf(msg.message, sizeof(msg.message), fmt, args);
    va_end(args);

    /* Non-blocking send. On failure (queue full), increment drop counter. */
    if (xQueueSend(s_log_queue, &msg, 0) != pdTRUE) {
        s_drop_count++;
    }
}

bool log_queue_receive(log_message_t *out, TickType_t timeout) {
    if (s_log_queue == NULL || out == NULL) return false;
    return (xQueueReceive(s_log_queue, out, timeout) == pdTRUE);
}

uint32_t log_queue_depth(void) {
    if (s_log_queue == NULL) return 0;
    return (uint32_t)uxQueueMessagesWaiting(s_log_queue);
}

uint32_t log_queue_capacity(void) {
    return LOGGER_QUEUE_LEN;
}
