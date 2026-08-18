/**
 * task_display.h
 */
#ifndef TASK_DISPLAY_H
#define TASK_DISPLAY_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

void task_display_set_queue(QueueHandle_t q);
void task_display(void *arg);

#endif
