/**
 * @file    task_thingspeak.c
 * @brief   Uploads one sample every 20 s (free ThingSpeak minimum is 15 s).
 */
#include "task_thingspeak.h"
#include "thingspeak.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TS_PERIOD_MS  20000
#define TS_STACK      10240

static void task_thingspeak(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));          /* let WiFi + STM32 settle */
    while (1) {
        if (thingspeak_enabled()) {
            (void)thingspeak_upload_now();
        }
        vTaskDelay(pdMS_TO_TICKS(TS_PERIOD_MS));
    }
}

void task_thingspeak_start(void)
{
    xTaskCreate(task_thingspeak, "thingspeak", TS_STACK, NULL, 2, NULL);
}
