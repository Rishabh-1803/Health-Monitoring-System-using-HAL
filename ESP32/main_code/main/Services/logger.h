/**
 * @file    logger.h
 * @brief   RAM log ring with optional LittleFS flush.
 *
 * Deliberately NOT a redirect of ESP_LOG: capturing the whole log
 * stream risks recursion and floods the ring with chatter. Instead,
 * the interesting moments (alarms, comm transitions, wifi changes,
 * supervisor escalations, config writes) call logger_logf() directly,
 * so the ring stays a readable incident log.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stddef.h>

#define LOG_LVL_INFO  0
#define LOG_LVL_WARN  1
#define LOG_LVL_ERR   2

#define LOGGER_LINES     64
#define LOGGER_LINE_MAX  128

void logger_init(void);

/** Append one formatted line to the ring (truncated at 127 chars). */
void logger_logf(uint8_t level, const char *tag, const char *fmt, ...)
        __attribute__((format(printf, 3, 4)));

/** Dump the last n lines to the console (CLI `logs`). */
void logger_dump_console(int last_n);

/**
 * Build a JSON array of the last n lines into buf.
 * @return bytes written (excluding NUL).
 */
size_t logger_build_json(char *buf, size_t cap, int last_n);

/** Flush the whole ring to LittleFS (no-op when storage is off). */
void logger_flush(void);

#endif /* LOGGER_H */
