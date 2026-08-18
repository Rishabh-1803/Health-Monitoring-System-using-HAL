#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void hello_task(void *pvParameters)
{
    while (1)
    {
        printf("Hello from HelloTask\n");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void counter_task(void *pvParameters)
{
    int count = 0;

    while (1)
    {
        printf("CounterTask: %d\n", count++);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    printf("Starting FreeRTOS multitasking...\n");

    xTaskCreate(
        hello_task,
        "HelloTask",
        2048,
        NULL,
        2,
        NULL
    );

    xTaskCreate(
        counter_task,
        "CounterTask",
        2048,
        NULL,
        5,
        NULL
    );
}