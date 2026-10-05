/**
 * @file    protocol_selftest.h
 * @brief   Protocol self-test for ESP32 (same tests as STM32 menu option 8).
 */
#ifndef PROTOCOL_SELFTEST_H
#define PROTOCOL_SELFTEST_H

#include <stdbool.h>

/**
 * Run all protocol self-tests. Output via ESP_LOGI.
 * @return true if all pass.
 */
bool protocol_selftest_run(void);

#endif /* PROTOCOL_SELFTEST_H */
