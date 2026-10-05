/**
 * @file    task_cli.c
 * @brief   Boots the esp_console REPL, then deletes itself.
 *
 * esp_console_start_repl() creates its own long-lived task (with
 * history, tab completion, and line editing from linenoise), so this
 * wrapper exists only to keep the Tasks/ layer in the architecture
 * honest.
 */

#include "task_cli.h"
#include "cli.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void task_cli(void *arg)
{
    (void)arg;
    cli_start();
    vTaskDelete(NULL);
}

void task_cli_start(void)
{
    xTaskCreate(task_cli, "cli_boot", 4096, NULL, 3, NULL);
}
