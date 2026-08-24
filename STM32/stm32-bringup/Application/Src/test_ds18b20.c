/**
 * @file    test_ds18b20.c
 * @brief   DS18B20 1-Wire temperature sensor.
 *
 * This is the fussiest test in the set, because 1-Wire is a timing
 * protocol with no clock line: every bit is defined by how long the
 * master holds the line down. So the test checks its own preconditions
 * first (is the cycle counter running?) before blaming the sensor.
 *
 * It also verifies the Maxim CRC on both the ROM code and the
 * scratchpad. That matters more than it sounds: a missing pull-up or a
 * long unshielded wire produces readings that look plausible but are
 * corrupt, and the CRC is the only thing that catches them.
 *
 * But the CRC is checked SECOND, after a stuck-bus test. CRC-8 is linear,
 * so an all-zero frame carries a valid CRC of 0x00 and sails through --
 * then decodes to a convincing 0.00 degC. See is_stuck() below.
 */

#include "bringup_config.h"

#if BRINGUP_TEST_DS18B20

#include "tests.h"
#include "console.h"
#include "bsp.h"

#include "FreeRTOS.h"
#include "task.h"

/* 1-Wire commands */
#define OW_READ_ROM        0x33u
#define OW_SKIP_ROM        0xCCu
#define OW_CONVERT_T       0x44u
#define OW_READ_SCRATCHPAD 0xBEu

/**
 * Maxim/Dallas CRC-8, polynomial x^8 + x^5 + x^4 + 1 in its reflected
 * form (0x8C). Returns 0 when run over data followed by its own CRC byte.
 */
static uint8_t ow_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0u;
    for (int i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (uint8_t)((crc ^ byte) & 0x01u);
            crc >>= 1;
            if (mix != 0u) {
                crc ^= 0x8Cu;
            }
            byte >>= 1;
        }
    }
    return crc;
}

/**
 * Reject a frame that is a single value repeated, BEFORE consulting the
 * CRC.
 *
 * This is not paranoia, it is the one failure mode the CRC cannot catch.
 * CRC-8 is linear, so a run of 0x00 bytes has a CRC of 0x00 and passes --
 * and nine zero bytes then decode to a perfectly plausible 0.00 degC. A
 * board reading exactly zero because DQ is shorted to ground would be
 * reported as a working sensor in a cold room. The all-0xFF case is the
 * mirror image: an idle line with only the pull-up driving it. Neither is
 * a measurement, so both are refused by inspection.
 */
static bool is_stuck(const uint8_t *d, int len)
{
    bool all_zero = true;
    bool all_ones = true;
    for (int i = 0; i < len; i++) {
        if (d[i] != 0x00u) { all_zero = false; }
        if (d[i] != 0xFFu) { all_ones = false; }
    }
    return all_zero || all_ones;
}

static const char *family_name(uint8_t code)
{
    switch (code) {
        case 0x28u: return "DS18B20";
        case 0x22u: return "DS1822";
        case 0x10u: return "DS18S20 (different scaling!)";
        case 0x3Bu: return "MAX31826";
        default:    return "unrecognised family";
    }
}

test_result_t test_ds18b20_run(void)
{
    uint8_t rom[8];
    uint8_t sp[9];

    (void)console_println("-- DS18B20 (1-Wire temperature) ------------------");

    if (!bsp_dwt_ok()) {
        (void)console_println("  The DWT cycle counter is not running, so microsecond");
        (void)console_println("  timing cannot be trusted. 1-Wire will not work; this is");
        (void)console_println("  a core/debug-block problem, not a sensor problem.");
        return TEST_FAIL;
    }

    /* ---- presence ---- */
    if (!bsp_ow_reset()) {
        (void)console_println("  No presence pulse.");
        (void)console_println("  In order of likelihood: the 4.7k pull-up to 3V3 is");
        (void)console_println("  missing, DQ is on the wrong pin, or VDD/GND are swapped.");
        return TEST_FAIL;
    }
    (void)console_println("  Presence pulse detected.");

    /* ---- ROM code, and its CRC ---- */
    (void)bsp_ow_reset();
    bsp_ow_write_byte(OW_READ_ROM);
    for (int i = 0; i < 8; i++) {
        rom[i] = bsp_ow_read_byte();
    }

    (void)console_printf("  ROM: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                         rom[0], rom[1], rom[2], rom[3],
                         rom[4], rom[5], rom[6], rom[7]);

    if (is_stuck(rom, 8)) {
        (void)console_println("  Every ROM byte is the same value. That is a stuck bus, not");
        (void)console_println("  a device -- all 0x00 means DQ is being held low, all 0xFF");
        (void)console_println("  means nothing is driving it at all. Note that the CRC does");
        (void)console_println("  NOT catch this: an all-zero frame has a valid CRC of 0x00.");
        return TEST_FAIL;
    }

    if (ow_crc8(rom, 8) != 0u) {
        (void)console_println("  ROM CRC BAD -- the bus is electrically unreliable.");
        (void)console_println("  Shorten the wire, or fit a proper 4.7k pull-up.");
        (void)console_println("  (Note: READ ROM only works with ONE device on the bus.)");
        return TEST_FAIL;
    }
    (void)console_printf("  ROM CRC ok. Family 0x%02X = %s\r\n",
                         rom[0], family_name(rom[0]));

    /* ---- convert, then read the scratchpad ---- */
    (void)bsp_ow_reset();
    bsp_ow_write_byte(OW_SKIP_ROM);
    bsp_ow_write_byte(OW_CONVERT_T);

    /* A 12-bit conversion takes up to 750 ms. This is a real delay, so
     * yield rather than spin -- the rest of the system keeps running. */
    (void)console_println("  Converting (750 ms) ...");
    vTaskDelay(pdMS_TO_TICKS(800));

    if (!bsp_ow_reset()) {
        (void)console_println("  Sensor vanished after the convert command.");
        return TEST_FAIL;
    }
    bsp_ow_write_byte(OW_SKIP_ROM);
    bsp_ow_write_byte(OW_READ_SCRATCHPAD);
    for (int i = 0; i < 9; i++) {
        sp[i] = bsp_ow_read_byte();
    }

    if (is_stuck(sp, 9)) {
        (void)console_println("  Every scratchpad byte is the same value -- a stuck bus,");
        (void)console_println("  not a reading. An all-zero scratchpad passes the CRC and");
        (void)console_println("  decodes to a convincing 0.00 degC, which is why this is");
        (void)console_println("  checked first.");
        return TEST_FAIL;
    }

    if (ow_crc8(sp, 9) != 0u) {
        (void)console_println("  Scratchpad CRC BAD -- reading discarded.");
        return TEST_FAIL;
    }
    (void)console_println("  Scratchpad CRC ok.");

    /* ---- decode ---- */
    int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | (uint16_t)sp[0]);

    /* Resolution lives in config register bits 6:5. At less than 12 bits
     * the low bits of the reading are undefined and must be masked, or
     * the temperature comes out with plausible-looking garbage decimals. */
    int bits = 12;
    switch ((sp[4] >> 5) & 0x03u) {
        case 0: bits = 9;  raw = (int16_t)(raw & ~0x0007); break;
        case 1: bits = 10; raw = (int16_t)(raw & ~0x0003); break;
        case 2: bits = 11; raw = (int16_t)(raw & ~0x0001); break;
        default: bits = 12; break;
    }

    /* One LSB is 1/16 degC = 62.5 m degC, so raw * 625 / 10 gives m degC. */
    int32_t milli_c = ((int32_t)raw * 625) / 10;

    (void)console_printf("  Resolution: %d bits, raw = 0x%04X\r\n",
                         bits, (uint16_t)raw);
    test_print_milli("  Temperature: ", milli_c, "degC");

    /* ---- sanity ---- */
    /* Reserved scratchpad bytes have fixed values on a genuine part:
     * byte 5 reads 0xFF and byte 7 (COUNT PER C) reads 0x10. Clones
     * sometimes differ, so this is a note rather than a failure -- but if
     * the temperature also looks odd, this is the first hint that the part
     * is not what the family code claims. */
    if (sp[5] != 0xFFu || sp[7] != 0x10u) {
        (void)console_printf("  Note: reserved bytes are %02X and %02X, expected FF and 10.\r\n",
                             sp[5], sp[7]);
        (void)console_println("  Probably a clone. Usually still works; trust it less.");
    }

    if (raw == 0x0550) {
        (void)console_println("  Exactly +85.00 degC is the DS18B20 power-on value:");
        (void)console_println("  the conversion did not actually run. Usually this means");
        (void)console_println("  the sensor is parasite-powered and needs a real 3V3 on VDD.");
        return TEST_FAIL;
    }
    if (milli_c < -55000 || milli_c > 125000) {
        (void)console_println("  Out of the sensor's -55..+125 degC range: not a real reading.");
        return TEST_FAIL;
    }
    if (milli_c < 5000 || milli_c > 45000) {
        (void)console_println("  In range, but not near room temperature -- worth a second");
        (void)console_println("  look unless the sensor really is hot or cold right now.");
    }

    return TEST_PASS;
}

#else  /* BRINGUP_TEST_DS18B20 */

#include "tests.h"
test_result_t test_ds18b20_run(void) { return TEST_SKIP; }

#endif /* BRINGUP_TEST_DS18B20 */
