/**
 * @file    task_logger.c
 * @brief   5-minute flush of the incident log to flash.
 *
 * logger_flush() is also called on alarms (from the RX task hook) and
 * on supervisor escalations; this task is the safety net that bounds
 * the loss window even when nothing dramatic happens.
 */

#include "task_logger.h"
#include "logger.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LOGGER_TASK_STACK   3072
#define LOGGER_TASK_PRIO     1
#define LOGGER_FLUSH_MS   300000u     /* 5 minutes */

static void task_logger(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LOGGER_FLUSH_MS));
        logger_flush();
    }
}

void task_logger_start(void)
{
    xTaskCreate(task_logger, "log_flush", LOGGER_TASK_STACK, NULL,
                LOGGER_TASK_PRIO, NULL);
}
