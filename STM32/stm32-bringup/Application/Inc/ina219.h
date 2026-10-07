/**
 * @file    ina219.h
 * @brief   INA219 bidirectional current/voltage sensor (I2C) -- robust driver.
 *
 * The INA219 measures the voltage across an external shunt (10 uV / LSB at
 * every PGA setting) and the bus voltage (4 mV / LSB, 0..26 V). Current is
 * Ohm's law: I = V_shunt / R_shunt, computed here in float so nothing is
 * lost to integer truncation.
 *
 * What this driver guards against (all were real failure modes):
 *   - range clipping   PGA /8 (+-320 mV) is used so a 0.1 ohm shunt reads up
 *                      to 3.2 A. The old PGA /1 setting clipped at 0.4 A.
 *   - wrong device     the config register is read back after writing, so a
 *                      different chip that merely ACKs 0x40 is rejected.
 *   - wrong address    the configured address is tried first, then
 *                      0x40..0x4F (A0/A1 strapping variants).
 *   - brown-out reset  ina219_service() re-checks the config register every
 *                      call (~1 Hz) and rewrites it if the chip fell back
 *                      to its power-on default.
 *   - hot-plug / late  a missing device is re-probed by ina219_service(), so
 *     power-up         plugging the sensor in after boot just works.
 *
 * Every read is ~0.3 ms on the bit-banged bus; safe in the cooperative loop.
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

/** Probe + reset + configure + verify. True only if a real INA219 answered. */
bool ina219_init(void);

/** True while a verified INA219 is believed to be present and responding. */
bool ina219_is_present(void);

/**
 * Housekeeping, call about once per second from the main loop:
 * re-probes when absent, re-programs the config register if the chip was
 * reset, and gives up on a device that has stopped answering.
 */
void ina219_service(void);

/** Shunt voltage in units of 10 uV (signed). False on I2C failure. */
bool ina219_read_shunt_raw(int32_t *raw_10uv);

/** Legacy name kept for compatibility: same value as ina219_read_shunt_raw. */
bool ina219_read_shunt_mv(int32_t *shunt_mv_x100);

/** Bus voltage in millivolts (0..26000). False on I2C failure. */
bool ina219_read_bus_mv(uint32_t *bus_mv);

/** Signed current in amps (float). False on I2C failure. */
bool ina219_read_current_a(float *amps);

/** Signed current in milliamps (rounded). False on I2C failure. */
bool ina219_read_current_ma(int32_t *current_ma);

/** True if the last shunt reading hit the PGA full-scale rail (clipped). */
bool ina219_saturated(void);

/** 7-bit address the device was found at (0 when absent). */
uint8_t ina219_address(void);

#endif /* INA219_H */
