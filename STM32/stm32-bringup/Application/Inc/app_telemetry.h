/**
 * @file    app_telemetry.h
 * @brief   Phase 3 — the continuous telemetry application.
 *
 * app_telemetry_run() is the telemetry application proper. It samples the
 * sensors, filters the readings, runs the alarm engine with hysteresis,
 * streams MSG_TELEMETRY to the ESP32, answers ESP32 commands, and feeds
 * the independent watchdog. It returns when any key is pressed on the
 * console, so the bring-up menu can take over again (see app_init.c).
 *
 * Application mapping (only three outputs exist on this board):
 *   LED_ID_GREEN  -> PC13 onboard LED
 *   LED_ID_RED    -> PB1 relay (industrial trip)
 *   LED_ID_YELLOW -> not wired; acknowledged, logged, no physical effect
 */

#ifndef APP_TELEMETRY_H
#define APP_TELEMETRY_H

/**
 * Run the telemetry application until a console keypress.
 * Safe to call repeatedly (menu item 'r' does exactly that).
 */
void app_telemetry_run(void);

/**
 * Feed the independent watchdog, if the telemetry application has
 * started it. Console wait paths (readline, key polling) call this so a
 * human in the bring-up menu never trips a reset. When the telemetry
 * loop itself is the only caller, a hung loop means no feed — which is
 * exactly the reset condition we want.
 */
void app_wdt_kick(void);

#endif /* APP_TELEMETRY_H */
