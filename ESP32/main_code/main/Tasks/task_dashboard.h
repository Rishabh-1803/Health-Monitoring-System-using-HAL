/**
 * @file    task_dashboard.h
 * @brief   FreeRTOS task: 1 Hz history sampler + WebSocket push.
 */
#ifndef TASK_DASHBOARD_H
#define TASK_DASHBOARD_H

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/** Create the dashboard task. Call once at boot. */
void task_dashboard_start(void);

/** Ask the dashboard task to push thresholds + sample rate to the STM32 again. */
void task_dashboard_request_resync(void);

#endif /* TASK_DASHBOARD_H */
