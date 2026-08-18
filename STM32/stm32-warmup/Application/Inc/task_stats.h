/**
 * task_stats.h
 */
#ifndef TASK_STATS_H
#define TASK_STATS_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

void task_stats_set_handles(TaskHandle_t sensor, TaskHandle_t filter,
                            TaskHandle_t display, TaskHandle_t logger,
                            TaskHandle_t stats);
void task_stats_set_queues(QueueHandle_t sensor_q, QueueHandle_t display_q);
void task_stats(void *arg);

#endif
