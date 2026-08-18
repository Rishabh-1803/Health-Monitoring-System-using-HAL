/**
 * ============================================================
 * task_logger.c — STM32 port
 * ============================================================
 * THE ONLY TASK ALLOWED TO CALL USB CDC / printf.
 *
 * On STM32, we use usb_serial_print() (our wrapper around
 * CDC_Transmit_FS) instead of ESP_LOGI/printf.
 *
 * WHY HIGHEST PRIORITY?
 *   - Logs are critical for debugging
 *   - If logger gets starved, queue overflows → lost logs
 *   - High priority + minimal work = quick drain
 *
 * NOTE: This task does blocking USB writes. That's OK because:
 *   1. It's the only task doing USB — no contention
 *   2. USB CDC on STM32 is fast (full-speed USB = 12 Mbps)
 *   3. The logger task spends most of its time blocked on the queue
 * ============================================================ */

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "project_config.h"
#include "log_queue.h"
#include "usb_serial.h"
#include "task_logger.h"

static const char *level_str(log_level_t l) {
    switch (l) {
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERR ";
        default:              return "??? ";
    }
}

void task_logger(void *arg)
{
    (void)arg;

    /*
     * Give USB CDC time to enumerate with the host PC before
     * sending the first log message.
     */
    vTaskDelay(pdMS_TO_TICKS(2000));

    usb_serial_print("=== STM32 FreeRTOS Warmup — Logger started ===\r\n");

    log_message_t msg;

    while (1)
    {
        if (!log_queue_receive(&msg, portMAX_DELAY))
        {
            continue;
        }

        char line[256];

        int len = snprintf(line, sizeof(line),
            "[%6lu] [%-7s] [%s] %s\r\n",
            msg.tick,
            msg.tag ? msg.tag : "??",
            level_str(msg.level),
            msg.message);

        if (len > 0)
        {
            usb_serial_write((const uint8_t *)line, (uint32_t)len);
        }
    }
}
