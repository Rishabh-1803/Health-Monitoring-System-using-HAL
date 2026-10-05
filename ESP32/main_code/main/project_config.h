/**
 * project_config.h — ESP32-S3 Phase 2B configuration
 *
 * FreeRTOS priorities + stack sizes for the 2 tasks that run in Phase 2B.
 */
#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

/* Task priorities (higher = higher priority) */
#define TASK_UART_RX_PRIO       5
#define TASK_UART_TX_PRIO       4

/* Stack sizes (in words; ESP32 word = 4 bytes) */
#define TASK_UART_RX_STACK      4096
#define TASK_UART_TX_STACK      2048

/* Timing */
#define HEARTBEAT_PERIOD_MS     1000
#define COMM_FAIL_TIMEOUT_MS   5000

#endif /* PROJECT_CONFIG_H */
