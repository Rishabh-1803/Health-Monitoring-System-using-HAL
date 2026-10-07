/**
 * @file    mpu6050.h
 * @brief   MPU6050 6-axis accel/gyro driver (I2C) used as the vibration source.
 *
 * The accelerometer is read as three signed axes in g. The application
 * high-passes each axis and reports the RMS of the dynamic part as the
 * vibration level, which is a real physical quantity directly comparable to
 * the threshold the operator sets.
 *
 * What this driver guards against (all were real failure modes):
 *   - clone / variant chips   WHO_AM_I is NOT required to be exactly 0x68;
 *                             MPU6050 clones and MPU6500/9250 parts (0x70,
 *                             0x71, 0x98 ...) share the same accel register
 *                             map and work fine.
 *   - AD0 strapped high       0x68 and 0x69 are both probed.
 *   - stuck in SLEEP          a full device reset + wake with PLL clock.
 *   - power glitch / relay    mpu6050_service() re-checks PWR_MGMT_1 about
 *     click resets the chip   once per second and re-configures it.
 *   - frozen / zeroed data    all-0x00 / all-0xFF bursts are rejected, and a
 *                             sensor returning bit-identical data for ~4 s
 *                             is treated as hung and re-initialised.
 *   - missing at boot         mpu6050_service() keeps re-probing.
 */
#ifndef MPU6050_H
#define MPU6050_H

#include <stdbool.h>
#include <stdint.h>

/** MPU6050 register addresses. */
#define MPU6050_REG_SMPLRT_DIV   0x19u
#define MPU6050_REG_CONFIG       0x1Au
#define MPU6050_REG_GYRO_CONFIG  0x1Bu
#define MPU6050_REG_ACCEL_CONFIG 0x1Cu
#define MPU6050_REG_ACCEL_XOUT_H 0x3Bu
#define MPU6050_REG_TEMP_OUT_H   0x41u
#define MPU6050_REG_PWR_MGMT_1   0x6Bu
#define MPU6050_REG_PWR_MGMT_2   0x6Cu
#define MPU6050_REG_WHO_AM_I     0x75u

/** Probe + reset + wake + configure. True if a device answered and is awake. */
bool mpu6050_init(void);

/** True while a configured MPU6050 is believed present and responding. */
bool mpu6050_is_present(void);

/**
 * Housekeeping, call about once per second from the main loop: re-probes when
 * absent, re-configures if the chip was reset, re-initialises a hung one.
 */
void mpu6050_service(void);

/** Read the three accelerometer axes in g. False on failure (out unchanged). */
bool mpu6050_read_accel_g(float *ax_g, float *ay_g, float *az_g);

/** Total acceleration magnitude in g (about 1.0 at rest). */
bool mpu6050_read_magnitude_g(float *mag_g);

/** Die temperature in degrees Celsius. */
bool mpu6050_read_temp_c(float *temp_c);

/** 7-bit address in use (0x68/0x69), 0 when absent. */
uint8_t mpu6050_address(void);

/** Value read from WHO_AM_I at the last init (diagnostics). */
uint8_t mpu6050_who_am_i(void);

#endif /* MPU6050_H */
