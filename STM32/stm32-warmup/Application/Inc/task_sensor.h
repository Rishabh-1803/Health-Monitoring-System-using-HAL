/**
 * task_sensor.h
 */
#ifndef TASK_SENSOR_H
#define TASK_SENSOR_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

void task_sensor_set_queue(QueueHandle_t q);
void task_sensor(void *arg);

#endif
