/**
 * task_filter.h
 */
#ifndef TASK_FILTER_H
#define TASK_FILTER_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

void task_filter_set_queues(QueueHandle_t in, QueueHandle_t out);
void task_filter(void *arg);

#endif
