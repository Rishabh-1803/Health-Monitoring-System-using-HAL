/**
 * @file    ina219.h
 * @brief   INA219 bidirectional current/voltage/power sensor (I2C).
 *
 * Replaces the ACS712 analog path. The INA219 measures the voltage dropped
 * across an external shunt resistor and reports it as a signed 16-bit value
 * (10 uV LSB at PGA /1), plus the bus voltage (4 mV LSB, 0..26 V). Current
 * follows from Ohm's law: I = V_shunt / R_shunt.
 *
 * Config defaults (bringup_config.h): address 0x40, shunt 0.1 ohm, PGA /1
 * (40 mV range -> +-2 A at 0.1 ohm), 12-bit continuous. Change SHUNT_OHM if
 * your board uses a different sense resistor; the math tracks it.
 *
 * All public reads are non-blocking (~1 ms at 250 kHz I2C) and safe to call
 * from the cooperative telemetry loop.
 */
#ifndef INA219_H
#define INA219_H

#include <stdbool.h>
#include <stdint.h>

/** INA219 register addresses. */
#define INA219_REG_CONFIG       0x00u
#define INA219_REG_SHUNT_V      0x01u
#define INA219_REG_BUS_V        0x02u
#define INA219_REG_POWER        0x03u
#define INA219_REG_CURRENT      0x04u
#define INA219_REG_CALIB        0x05u

/** Probe + configure the INA219. Returns true if the device ACKed. */
bool ina219_init(void);

/** True after a successful ina219_init(). */
bool ina219_is_present(void);

/**
 * Read the shunt voltage in millivolts (signed). Negative = current flowing
 * the "reverse" direction; the app rectifies it for a load-current display.
 * Returns false on I2C failure (out unchanged).
 */
bool ina219_read_shunt_mv(int32_t *shunt_mv_x100);

/**
 * Read the bus voltage in millivolts (0..26000). Returns false on failure.
 */
bool ina219_read_bus_mv(uint32_t *bus_mv);

/**
 * Convenience: shunt current in milliamps (signed), derived from the shunt
 * voltage and the configured R_shunt. Returns false on I2C failure.
 */
bool ina219_read_current_ma(int32_t *current_ma);

#endif /* INA219_H */
