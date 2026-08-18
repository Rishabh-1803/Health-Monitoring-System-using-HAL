/**
 * alarm_event.h — Event group wrapper, same API as ESP32 version.
 */
#ifndef ALARM_EVENT_H
#define ALARM_EVENT_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "event_groups.h"

void     alarm_event_init(void);
void     alarm_event_set(uint32_t bits);
void     alarm_event_clear(uint32_t bits);
uint32_t alarm_event_get(void);
uint32_t alarm_event_wait(uint32_t bits, bool clear_on_exit, uint32_t timeout_ms);

#endif /* ALARM_EVENT_H */
