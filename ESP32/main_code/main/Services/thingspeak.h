/**
 * @file    thingspeak.h
 * @brief   Uploads telemetry to a ThingSpeak channel (Write API key).
 *
 * Fields: 1 temp C | 2 current mA | 3 vibration g | 4 alarm bits
 *         5 STM32 CPU % | 6 WiFi RSSI dBm | 7 STM32 link (1/0) | 8 sensor bits
 */
#ifndef THINGSPEAK_H
#define THINGSPEAK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define THINGSPEAK_KEY_MAX  32

void thingspeak_init(void);                       /* log key status */
bool thingspeak_enabled(void);
void thingspeak_get_key_masked(char *buf, size_t cap);  /* ****ABCD      */
/** One upload attempt now (blocking). Returns true when ThingSpeak
 *  accepted it (entry id > 0). */
bool thingspeak_upload_now(void);
void thingspeak_status_json(char *buf, size_t cap);

#endif
