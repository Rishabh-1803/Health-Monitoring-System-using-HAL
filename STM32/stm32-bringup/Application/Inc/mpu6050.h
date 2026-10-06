/**
 * @file    mpu6050.h
 * @brief   MPU6050 6-axis accel/gyro driver (I2C) used as the vibration source.
 *
 * Replaces the SW-420 digital switch path. The MPU6050 is read for its
 * accelerometer; vibration is the high-passed magnitude of the total
 * acceleration vector in g, which is a real (not estimated) quantity and is
 * directly comparable to the threshold the operator sets.
 *
 * Config defaults (bringup_config.h): address 0x68 (AD0=GND), accel range
 * +-2 g (16384 LSB/g). Change MPU6050_ACCEL_RANGE for a different FSR; the
 * divisor is derived from it so the g math stays correct.
 *
 * All reads are non-blocking (~1.5 ms at 250 kHz I2C) and safe to call from
 * the cooperative telemetry loop.
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
#define MPU6050_REG_PWR_MGMT_1   0x6Bu
#define MPU6050_REG_WHO_AM_I     0x75u

/** Probe + wake + configure the MPU6050. Returns true if WHO_AM_I matched. */
bool mpu6050_init(void);

/** True after a successful mpu6050_init(). */
bool mpu6050_is_present(void);

/**
 * Read the three accelerometer axes in g (float, signed).
 * Returns false on I2C failure (out unchanged).
 */
bool mpu6050_read_accel_g(float *ax_g, float *ay_g, float *az_g);

/**
 * Read the total acceleration magnitude in g: sqrt(ax^2+ay^2+az^2).
 * At rest this is ~1.0 g (gravity). Vibration is the deviation above the
 * slow baseline, which the app high-passes.
 */
bool mpu6050_read_magnitude_g(float *mag_g);

/**
 * Read the die temperature in degrees Celsius.
 */
bool mpu6050_read_temp_c(float *temp_c);

#endif /* MPU6050_H */
