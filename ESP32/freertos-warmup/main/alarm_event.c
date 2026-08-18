/**
 * ============================================================
 * alarm_event.c
 * ============================================================
 * Implementation of the alarm event group wrapper.
 *
 * PATTERN: Wrap FreeRTOS primitives behind a clean API.
 *   - Callers don't need to know about EventGroupHandle_t
 *   - We can swap implementations (e.g., task notifications)
 *     without touching caller code
 *   - Easier to add logging / instrumentation centrally
 *
 * This is exactly the pattern we'll use in the real project
 * for queues, mutexes, and task notifications.
 * ============================================================ */

#include "alarm_event.h"
#include "project_config.h"

static EventGroupHandle_t s_alarm_group = NULL;

void alarm_event_init(void) {
    s_alarm_group = xEventGroupCreate();
    configASSERT(s_alarm_group != NULL);
}

void alarm_event_set(uint32_t bits) {
    if (s_alarm_group == NULL) return;
    /* pdTRUE/pdPASS return means a higher-priority task was unblocked
     * and we should yield. In ESP-IDF we usually ignore it here and
     * let the scheduler handle preemption at the next tick. */
    xEventGroupSetBits(s_alarm_group, (EventBits_t)bits);
}

void alarm_event_clear(uint32_t bits) {
    if (s_alarm_group == NULL) return;
    xEventGroupClearBits(s_alarm_group, (EventBits_t)bits);
}

uint32_t alarm_event_get(void) {
    if (s_alarm_group == NULL) return 0;
    return (uint32_t)xEventGroupGetBits(s_alarm_group);
}

uint32_t alarm_event_wait(uint32_t bits, bool clear_on_exit, uint32_t timeout_ms) {
    if (s_alarm_group == NULL) return 0;

    TickType_t ticks = (timeout_ms == 0xffffffff) ?
                        portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    EventBits_t result = xEventGroupWaitBits(
        s_alarm_group,
        (EventBits_t)bits,
        clear_on_exit ? pdTRUE : pdFALSE,   // Clear on exit?
        pdFALSE,                             // Wait for ANY bit (not ALL)
        ticks
    );

    return (uint32_t)(result & bits);
}
