/**
 * alarm_event.c — STM32 port of the alarm event group wrapper.
 */
#include "alarm_event.h"
#include "project_config.h"

static EventGroupHandle_t s_alarm_group = NULL;

void alarm_event_init(void) {
    s_alarm_group = xEventGroupCreate();
    configASSERT(s_alarm_group != NULL);
}

void alarm_event_set(uint32_t bits) {
    if (s_alarm_group == NULL) return;
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
        clear_on_exit ? pdTRUE : pdFALSE,
        pdFALSE,
        ticks
    );

    return (uint32_t)(result & bits);
}
