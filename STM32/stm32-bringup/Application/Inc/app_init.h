/**
 * @file    app_init.h
 * @brief   Entry point the CubeMX-generated freertos.c calls.
 */

#ifndef APP_INIT_H
#define APP_INIT_H

/**
 * Create the bring-up task. Called from MX_FREERTOS_Init(), inside the
 * USER CODE block, so a CubeMX regeneration cannot remove it.
 *
 * Runs before the scheduler starts, so it only creates the task -- all
 * real work happens once osKernelStart() is running.
 */
void bringup_app_init(void);

#endif /* APP_INIT_H */
