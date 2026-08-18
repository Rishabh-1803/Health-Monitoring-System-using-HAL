/**
 * ============================================================
 * log_queue.h
 * ============================================================
 * THE LOGGER PATTERN — most important concept in this warmup.
 *
 * RULE: No task may call printf() / ESP_LOGI() / etc. directly.
 *       Only TaskLogger may touch the serial output.
 *
 * WHY?
 * --------------------------------
 *   1. SERIAL IS NOT THREAD-SAFE: Two tasks calling printf()
 *      at the same time will interleave characters into
 *      garbage like "Temp=[4Curren.2]t=[5.3]".
 *
 *   2. SERIAL IS SLOW: A 115200 baud UART takes ~1ms to print
 *      a 100-char line. If a sensor task calls printf() every
 *      10ms, it spends 10% of CPU on logging — and blocks
 *      other work.
 *
 *   3. PRIORITY INVERSION: If a high-priority task waits on
 *      a low-priority task that's stuck in printf(), your
 *      real-time guarantees die.
 *
 * SOLUTION:
 * --------------------------------
 *   - Every other task calls log_send() with a format string
 *   - log_send() formats the message into a struct and
 *     pushes it to a queue (microseconds — no I/O)
 *   - TaskLogger pops from the queue at its own pace and
 *     calls ESP_LOGI to do the actual UART write
 *
 * Now:
 *   - Serial writes are serialized (one writer)
 *   - Producer tasks never block on I/O
 *   - If logger can't keep up, queue fills and oldest messages
 *     drop — system keeps running (graceful degradation)
 * ============================================================
 */

#ifndef LOG_QUEUE_H
#define LOG_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

typedef struct {
    log_level_t level;
    char        message[160];   // Fixed-size so queue element size is known
    uint32_t    tick;           // When the log was queued (not when printed)
    const char *tag;            // Source tag, e.g., "SENSOR", "FILTER"
} log_message_t;

/**
 * Initialize the logger queue. Call once at startup,
 * BEFORE starting the logger task.
 */
void log_queue_init(void);

/**
 * Queue a log message (printf-style). Non-blocking.
 * If the queue is full, the message is silently dropped.
 *
 * This is safe to call from any task. NOT safe from ISRs
 * (use log_send_from_isr for that — not implemented in warmup).
 *
 * @param level  Severity
 * @param tag    Short source identifier (string literal preferred)
 * @param fmt    printf-style format string
 */
void log_send(log_level_t level, const char *tag, const char *fmt, ...);

/**
 * Block until a log message is available, then copy it into *out.
 * Called only by TaskLogger.
 *
 * @param out      Destination buffer
 * @param timeout  Max ticks to wait
 * @return         true if a message was received, false on timeout
 */
bool log_queue_receive(log_message_t *out, TickType_t timeout);

/**
 * Returns the current queue depth (number of pending messages).
 * Used by TaskStats for diagnostics.
 */
uint32_t log_queue_depth(void);

/**
 * Returns the configured capacity of the log queue.
 */
uint32_t log_queue_capacity(void);

#endif /* LOG_QUEUE_H */
