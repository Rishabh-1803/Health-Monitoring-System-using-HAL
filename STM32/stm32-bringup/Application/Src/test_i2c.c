/**
 * @file    test_i2c.c
 * @brief   Bus scan -- the single most useful bring-up test there is.
 *
 * Walks addresses 0x08..0x77 (the range 7-bit I2C actually permits;
 * 0x00-0x07 and 0x78-0x7F are reserved) and reports every device that
 * ACKs. If a display or sensor does not appear here, nothing further up
 * the stack can possibly work, so run this first.
 *
 * The scan also reports the idle state of both lines before it starts.
 * Two lines reading low is the classic missing-pull-up or wedged-slave
 * symptom, and knowing that up front saves a lot of guessing.
 */

#include "bringup_config.h"

#if BRINGUP_TEST_I2C_SCAN

#include "tests.h"
#include "console.h"
#include "bsp.h"
#include "main.h"

/** Best guess at what lives at a given address, for the report. */
static const char *guess_device(uint8_t addr)
{
    switch (addr) {
        case 0x3C: case 0x3D: return "SSD1306/SH1106 OLED";
        case 0x27: case 0x3F: return "PCF8574 (LCD backpack)";
        case 0x40: case 0x41:
        case 0x44: case 0x45: return "INA219/INA226 current sensor";
        case 0x48: case 0x49:
        case 0x4A: case 0x4B: return "ADS1115 / LM75 / TMP102";
        case 0x50: case 0x51:
        case 0x56: case 0x57: return "AT24Cxx EEPROM";
        case 0x53: case 0x1D: return "ADXL345 accelerometer";
        case 0x5A:            return "MLX90614 IR temperature";
        case 0x68: case 0x69: return "MPU6050 / DS1307 / DS3231";
        case 0x76: case 0x77: return "BMP280/BME280";
        default:              return "unknown";
    }
}

test_result_t test_i2c_scan_run(void)
{
    (void)console_println("-- I2C bus scan (software I2C on PB6/PB7) ---------");

    /* Read the idle levels through the input register. Both should be
     * high, held there by the pull-ups. */
    bool scl_idle = (HAL_GPIO_ReadPin(I2C_SCL_PORT, I2C_SCL_PIN) == GPIO_PIN_SET);
    bool sda_idle = (HAL_GPIO_ReadPin(I2C_SDA_PORT, I2C_SDA_PIN) == GPIO_PIN_SET);

    (void)console_printf("  Idle levels: SCL %s, SDA %s\r\n",
                         scl_idle ? "HIGH (good)" : "LOW  (bad)",
                         sda_idle ? "HIGH (good)" : "LOW  (bad)");

    if (!scl_idle || !sda_idle) {
        (void)console_println("  A line stuck low means one of three things:");
        (void)console_println("    - no pull-up resistor to 3V3 (fit 4.7k)");
        (void)console_println("    - the line is shorted to ground");
        (void)console_println("    - a slave is mid-transfer; trying to free it now...");
        if (bsp_i2c_recover()) {
            (void)console_println("  Recovery clocked SDA free.");
        } else {
            (void)console_println("  Recovery FAILED -- this is wiring, not firmware.");
            return TEST_FAIL;
        }
    }

    (void)console_println("  Scanning 0x08..0x77 ...");

    int found = 0;
    for (uint8_t addr = 0x08u; addr <= 0x77u; addr++) {
        if (bsp_i2c_probe(addr)) {
            found++;
            (void)console_printf("    0x%02X  ACK   %s\r\n", addr, guess_device(addr));
        }
    }

    if (found == 0) {
        (void)console_println("  No devices answered.");
        (void)console_println("  Check: 3V3 and GND to the module, SDA/SCL not swapped,");
        (void)console_println("  and pull-ups present. The internal pull-ups are weak;");
        (void)console_println("  a long jumper usually needs real 4.7k resistors.");
        return TEST_FAIL;
    }

    (void)console_printf("  %d device(s) responded.\r\n", found);
    return TEST_PASS;
}

#else  /* BRINGUP_TEST_I2C_SCAN */

#include "tests.h"
test_result_t test_i2c_scan_run(void) { return TEST_SKIP; }

#endif /* BRINGUP_TEST_I2C_SCAN */
