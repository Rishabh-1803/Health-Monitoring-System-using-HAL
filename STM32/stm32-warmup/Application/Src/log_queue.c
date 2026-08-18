/**
 * log_queue.c — STM32 port of the logger queue.
 *
 * WARNING: %f in printf on STM32 newlib-nano does NOT work by default.
 * You must add `-u _printf_float` to the linker flags in CubeIDE.
 * (See STEPS_CUBEIDE_SETUP.md for how to do this.)
 */
#include "log_queue.h"
#include "project_config.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static QueueHandle_t s_log_queue = NULL;
static uint32_t      s_drop_count = 0;

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
    vsnprintf(msg.message, sizeof(msg.message), fmt, args);
    va_end(args);

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
