/**
 * project_config.h — ESP32-S3 Industrial Monitor
 *
 * Mirrors the STM32 project_config.h structure but adapted for
 * ESP32-S3 (larger stacks for WiFi, different priorities for
 * ESP-IDF internals).
 *
 * Reference: docs/ARCHITECTURE.md §5.2 (ESP32 Task List)
 */
#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

/* ---- TASK PRIORITIES ----
 * Higher number = higher priority.
 * ESP-IDF's WiFi/BLE stack runs at 18-22 — don't go that high.
 * See ARCHITECTURE.md §5.2 for rationale. */
#define TASK_LOGGER_PRIO            7
#define TASK_UART_RX_PRIO           5
#define TASK_WIFI_PRIO              6
#define TASK_WEB_SERVER_PRIO        4
#define TASK_CLI_PRIO                4
#define TASK_DASHBOARD_PRIO         3
#define TASK_UART_TX_PRIO           3
#define TASK_DIAGNOSTICS_PRIO       2
#define TASK_WATCHDOG_PRIO          6

/* ---- STACK SIZES (in WORDS; ESP32 word = 4 bytes) ----
 * ESP-IDF's WiFi stack is heavy — these are 4-8x larger than STM32. */
#define TASK_WIFI_STACK              4096
#define TASK_WEB_SERVER_STACK        8192
#define TASK_DASHBOARD_STACK         4096
#define TASK_UART_RX_STACK           1024
#define TASK_UART_TX_STACK           1024
#define TASK_LOGGER_STACK            4096
#define TASK_CLI_STACK                4096
#define TASK_DIAGNOSTICS_STACK       3072
#define TASK_WATCHDOG_STACK          512

/* ---- QUEUE LENGTHS ---- */
#define Q_LOG_LEN                    64
#define Q_TELEMETRY_LEN              16
#define Q_COMMAND_TO_STM32_LEN      8
#define Q_COMMAND_FROM_STM32_LEN    8
#define Q_DASHBOARD_UPDATE_LEN       8

/* ---- TIMING ----
 * ESP-IDF tick rate is 100 Hz (10 ms per tick). pdMS_TO_TICKS handles conversion. */
#define TELEMETRY_PERIOD_MS          1000    /* 1 Hz to browser via WebSocket */
#define DIAGNOSTICS_PERIOD_MS        5000
#define HEARTBEAT_PERIOD_MS          1000
#define HEARTBEAT_TIMEOUT_MS         5000

/* ---- ALARM EVENT GROUP BITS (mirror STM32) ---- */
#define ALARM_BIT_OVERTEMP          (1 << 0)
#define ALARM_BIT_OVERCURRENT       (1 << 1)
#define ALARM_BIT_VIBRATION         (1 << 2)
#define ALARM_BIT_SENSOR_FAIL       (1 << 3)
#define ALARM_BIT_COMM_FAIL         (1 << 4)
#define ALARM_BIT_ALL               (ALARM_BIT_OVERTEMP | \
                                     ALARM_BIT_OVERCURRENT | \
                                     ALARM_BIT_VIBRATION | \
                                     ALARM_BIT_SENSOR_FAIL | \
                                     ALARM_BIT_COMM_FAIL)

/* ---- SYSTEM EVENT GROUP BITS ---- */
#define SYS_BOOT_DONE              (1 << 0)
#define SYS_CONFIG_CHANGED         (1 << 1)
#define SYS_WIFI_CONNECTED         (1 << 2)
#define SYS_STM32_CONNECTED        (1 << 3)

/* ---- UART LINK PARAMETERS (must match STM32 — see UART_PROTOCOL_SPEC.md §1) ---- */
#define UART_LINK_NUM              UART_NUM_1
#define UART_LINK_TX_PIN           17       /* GPIO17 */
#define UART_LINK_RX_PIN           18       /* GPIO18 */
#define UART_LINK_BAUDRATE         460800
#define UART_LINK_TX_BUF_SIZE      256
#define UART_LINK_RX_BUF_SIZE      512      /* Larger to absorb bursts */
#define UART_LINK_MAX_PAYLOAD      128

/* ---- ACK/RETRANSMIT CONSTANTS (mirror STM32) ---- */
#define ACK_TIMEOUT_MS             200
#define MAX_RETRIES                 3
#define RECENT_SEQ_HISTORY_SIZE     8

#endif /* PROJECT_CONFIG_H */
