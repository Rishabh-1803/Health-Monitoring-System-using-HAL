/**
 * @file    logger.c
 * @brief   Incident log ring.
 */

#include "logger.h"
#include "littlefs_storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    uint32_t t_ms;
    uint8_t  level;
    char     tag[12];
    char     text[LOGGER_LINE_MAX];
} log_line_t;

static log_line_t s_ring[LOGGER_LINES];
static uint8_t    s_head = 0u, s_count = 0u;
static SemaphoreHandle_t s_lock;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

void logger_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_head = 0u;
    s_count = 0u;
}

void logger_logf(uint8_t level, const char *tag, const char *fmt, ...)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    log_line_t *e = &s_ring[s_head];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }

    e->t_ms = now_ms();
    e->level = level;
    snprintf(e->tag, sizeof(e->tag), "%s", (tag != NULL) ? tag : "app");

    s_head = (uint8_t)((s_head + 1u) % LOGGER_LINES);
    if (s_count < LOGGER_LINES) {
        s_count++;
    }
}

void logger_dump_console(int last_n)
{
    if (s_lock == NULL) {
        return;
    }
    if (last_n <= 0 || last_n > LOGGER_LINES) {
        last_n = 20;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t first = (s_count > (uint8_t)last_n)
                    ? (uint8_t)(s_count - (uint8_t)last_n) : 0u;
    for (uint8_t i = first; i < s_count; i++) {
        uint8_t slot = (uint8_t)((s_head + LOGGER_LINES - s_count + i)
                                 % LOGGER_LINES);
        log_line_t *e = &s_ring[slot];
        printf("%6lu.%03lu [%s] %s: %s\n",
               (unsigned long)(e->t_ms / 1000u),
               (unsigned long)(e->t_ms % 1000u),
               e->level == LOG_LVL_ERR ? "E" :
               e->level == LOG_LVL_WARN ? "W" : "I",
               e->tag, e->text);
    }
    xSemaphoreGive(s_lock);
}

size_t logger_build_json(char *buf, size_t cap, int last_n)
{
    if (buf == NULL || cap < 4u) {
        return 0u;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (last_n <= 0 || last_n > LOGGER_LINES) {
        last_n = 32;
    }

    size_t used = 0u;
    int wrote = snprintf(buf, cap, "{\"type\":\"logs\",\"lines\":[");
    if (wrote < 0) {
        return 0u;
    }
    used = (size_t)wrote;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t first = (s_count > (uint8_t)last_n)
                    ? (uint8_t)(s_count - (uint8_t)last_n) : 0u;
    for (uint8_t i = first; i < s_count; i++) {
        uint8_t slot = (uint8_t)((s_head + LOGGER_LINES - s_count + i)
                                 % LOGGER_LINES);
        log_line_t *e = &s_ring[slot];
        int n = snprintf(&buf[used], cap - used,
                         "%s{\"ts\":%lu,\"l\":%u,\"tag\":\"%s\",\"m\":\"%s\"}",
                         (i == first) ? "" : ",",
                         (unsigned long)e->t_ms, (unsigned)e->level,
                         e->tag, e->text);
        if (n < 0 || (size_t)n >= cap - used) {
            break;               /* out of space: stop cleanly         */
        }
        used += (size_t)n;
    }
    xSemaphoreGive(s_lock);

    if (cap - used >= 2u) {
        buf[used++] = ']';
        buf[used++] = '}';
        buf[used] = '\0';
    } else {
        buf[used ? used - 1u : 0u] = '\0';   /* truncated                */
    }
    return used;
}

void logger_flush(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < s_count; i++) {
        uint8_t slot = (uint8_t)((s_head + LOGGER_LINES - s_count + i)
                                 % LOGGER_LINES);
        littlefs_storage_append_log(s_ring[slot].t_ms, s_ring[slot].level,
                                    s_ring[slot].tag, s_ring[slot].text);
    }
    xSemaphoreGive(s_lock);
}
