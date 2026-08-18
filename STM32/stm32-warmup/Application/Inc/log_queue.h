/**
 * log_queue.h — Same API as ESP32 version. Single-writer pattern.
 */
#ifndef LOG_QUEUE_H
#define LOG_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"

typedef enum {
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

typedef struct {
    log_level_t level;
    char        message[160];
    uint32_t    tick;
    const char *tag;
} log_message_t;

void    log_queue_init(void);
void    log_send(log_level_t level, const char *tag, const char *fmt, ...);
bool    log_queue_receive(log_message_t *out, TickType_t timeout);
uint32_t log_queue_depth(void);
uint32_t log_queue_capacity(void);

#endif /* LOG_QUEUE_H */
