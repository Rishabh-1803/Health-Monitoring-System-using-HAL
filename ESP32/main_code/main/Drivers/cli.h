/**
 * @file    cli.h
 * @brief   USB-serial REPL: inspection + control commands.
 *
 * Transport: UART0 (the USB-UART bridge, 115200) — the same channel
 * the ESP_LOG output uses, which is exactly what the sdkconfig's
 * CONFIG_ESP_CONSOLE_UART_DEFAULT selects.
 *
 * Every command that talks to the STM32 goes through the command
 * dispatcher, so the CLI gets the same ACK/retry semantics as the web
 * dashboard. Interactive budget: 1500 ms (3 x 500 ms attempts).
 */

#ifndef CLI_H
#define CLI_H

/** Register all commands and start the REPL task. Call once at boot. */
void cli_start(void);

#endif /* CLI_H */
