/**
 * @file    test_oled.c
 * @brief   SSD1306 OLED over the bit-banged I2C bus.
 *
 * This carries just enough of an SSD1306 driver to prove the panel:
 * init sequence, a 1 KB frame buffer, page-at-a-time flush, and a 5x7
 * font so it can write words rather than just blocks. The application
 * layer will want its own graphics code later; this exists only to
 * answer "is the display alive and addressed correctly".
 *
 * The test runs an unmistakable sequence -- all pixels on, all off, a
 * checkerboard, then a framed message -- because each step fails in a
 * different, diagnosable way. All-on working but text not means the
 * addressing or the page mapping is wrong. Nothing at all means the
 * bus or the power is wrong, and the scan test will already have said so.
 *
 * Two things this file is careful about, both of which are the kind of
 * mistake that produces a confident wrong diagnosis rather than an
 * obvious failure:
 *
 *   The addressing mode in oled_init() must match the cursor commands in
 *   oled_flush(). They disagreed at one point -- horizontal mode driven
 *   with page-mode commands -- and the symptom was scrambled text with a
 *   perfect all-pixels-on step, which reads as a font bug.
 *
 *   Every flush is checked. A flush that fails silently lets the test
 *   walk to the end and ask the operator what they saw, then explain
 *   their "no" with the wrong cause.
 */

#include "bringup_config.h"

#if BRINGUP_TEST_OLED

#include "tests.h"
#include "console.h"
#include "bsp.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#define OLED_PAGES     (OLED_HEIGHT / 8)
#define FB_SIZE        (OLED_WIDTH * OLED_PAGES)

static uint8_t s_fb[FB_SIZE];
static uint8_t s_addr = OLED_I2C_ADDR_7BIT;

/* ------------------------------------------------------------------ */
/*  5x7 font, ASCII 0x20 ('space') through 0x5F ('_').                */
/*  Each glyph is 5 columns; within a column bit 0 is the top pixel.  */
/*  Lowercase input is folded to uppercase so the table stays small.  */
/* ------------------------------------------------------------------ */
static const uint8_t FONT5X7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */  {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */  {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */  {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */  {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */  {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */  {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */  {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */  {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */  {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */  {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */  {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */  {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */  {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */  {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */  {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */  {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */  {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */  {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */  {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */  {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */  {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */  {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */  {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */  {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */  {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */  {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */  {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */  {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */  {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */  {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */  {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */  {0x40,0x40,0x40,0x40,0x40}, /* _ */
};

/* ------------------------------------------------------------------ */
/*  Panel access                                                      */
/* ------------------------------------------------------------------ */

/* 0x00 as the first byte is the SSD1306 control byte meaning "what
 * follows is a command"; 0x40 means "what follows is display data". */
static bool oled_cmd(uint8_t c)
{
    uint8_t b[2] = { 0x00u, c };
    return bsp_i2c_write(s_addr, b, 2u);
}

static bool oled_init(void)
{
    static const uint8_t seq[] = {
        0xAE,             /* display off while we reconfigure          */
        0x20, 0x02,       /* PAGE addressing mode -- see the note below */
        0x40,             /* start line 0                              */
        0xA1,             /* segment remap: column 127 -> SEG0         */
        0xC8,             /* COM scan reversed; with A1 this un-mirrors */
        0x81, 0x7F,       /* contrast, mid                             */
        0xA6,             /* non-inverted                              */
        0xA4,             /* output follows RAM, not all-on            */
        0xD3, 0x00,       /* no display offset                         */
        0xD5, 0x80,       /* clock divide / oscillator frequency       */
        0xD9, 0x22,       /* pre-charge                                */
        0xDB, 0x20,       /* VCOMH deselect                            */
        0x8D, 0x14,       /* charge pump ON -- omit this and the panel */
                          /* ACKs happily and stays completely dark    */
        0xAF,             /* display on                                */
    };

    /* Page addressing (0x20,0x02) rather than horizontal (0x20,0x00),
     * because that is the mode oled_flush() actually drives. Flush sets
     * the cursor per page with 0xB0|page, 0x00 and 0x10, and the SSD1306
     * datasheet lists all three of those as page-addressing-mode commands
     * only: in horizontal mode they are ignored, the internal pointer
     * keeps auto-advancing from wherever it was, and the image ends up
     * rolled by some number of pages. Solid fills survive that -- which is
     * exactly why the bug would have got through step 1 and only shown up
     * as scrambled text in step 4, pointing the blame at the font code.
     *
     * The alternative would be to keep horizontal mode and set the window
     * with 0x21/0x22. Page mode was chosen because the flush is already
     * written for it and its column pointer wraps within the page, so a
     * short write cannot bleed into the next one. */

    /* Multiplex ratio and COM pin config depend on panel height, so
     * they are sent separately rather than baked into the table. */
    if (!oled_cmd(0xA8u)) { return false; }
#if (OLED_HEIGHT == 32)
    if (!oled_cmd(0x1Fu)) { return false; }
    if (!oled_cmd(0xDAu) || !oled_cmd(0x02u)) { return false; }
#else
    if (!oled_cmd(0x3Fu)) { return false; }
    if (!oled_cmd(0xDAu) || !oled_cmd(0x12u)) { return false; }
#endif

    for (unsigned i = 0; i < sizeof(seq); i++) {
        if (!oled_cmd(seq[i])) {
            return false;
        }
    }
    return true;
}

/* Where the last failed flush gave up. Kept because "failed on page 0"
 * and "failed on page 5" are different faults: the first never worked,
 * the second works until the bus has been driven for a while. */
static int  s_fail_page = -1;
static bool s_fail_was_cmd = false;

static bool oled_flush(void)
{
    uint8_t buf[1 + OLED_WIDTH];
    buf[0] = 0x40u;                              /* data control byte */

    for (int page = 0; page < OLED_PAGES; page++) {
        if (!oled_cmd((uint8_t)(0xB0u | page)) ||  /* page address     */
            !oled_cmd(0x00u) ||                    /* column low  = 0  */
            !oled_cmd(0x10u)) {                    /* column high = 0  */
            s_fail_page = page;
            s_fail_was_cmd = true;
            return false;
        }
        memcpy(&buf[1], &s_fb[page * OLED_WIDTH], OLED_WIDTH);
        if (!bsp_i2c_write(s_addr, buf, sizeof(buf))) {
            s_fail_page = page;
            s_fail_was_cmd = false;
            return false;
        }
    }
    return true;
}

/**
 * Flush, and if it fails say so loudly.
 *
 * Every step must be checked. Ignoring the return value on three of the
 * four steps -- which is what this file used to do -- means a panel that
 * dies half way through still reaches the "did you see all 4 steps?"
 * question, the operator honestly answers no, and the firmware blames the
 * charge pump for what was actually a bus fault it had already detected.
 */
static bool flush_step(const char *what)
{
    if (oled_flush()) {
        return true;
    }

    (void)console_printf("  Flush FAILED during %s: no ACK on page %d (%s).\r\n",
                         what, s_fail_page,
                         s_fail_was_cmd ? "cursor command" : "pixel data");

    if (s_fail_page == 0) {
        (void)console_println("  Failing on the very first page, after init succeeded, points");
        (void)console_println("  at the longer transfer itself: 129 bytes in one go instead");
        (void)console_println("  of 2. Suspect the pull-ups.");
    } else {
        (void)console_println("  It worked for a few pages and then stopped, which is what");
        (void)console_println("  marginal pull-ups or long wires look like -- the bus fails");
        (void)console_println("  under sustained traffic, not at the first byte.");
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Frame buffer drawing                                              */
/* ------------------------------------------------------------------ */

static void fb_fill(uint8_t value)
{
    memset(s_fb, value, sizeof(s_fb));
}

static void fb_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    uint32_t idx  = (uint32_t)((y / 8) * OLED_WIDTH + x);
    uint8_t  mask = (uint8_t)(1u << (y & 7));
    if (on) { s_fb[idx] |= mask; } else { s_fb[idx] &= (uint8_t)~mask; }
}

static void fb_char(int x, int y, char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');       /* fold to the table's range */
    }
    if (c < 0x20 || c > 0x5F) {
        c = '?';
    }
    const uint8_t *glyph = FONT5X7[(uint8_t)c - 0x20u];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            fb_pixel(x + col, y + row, ((glyph[col] >> row) & 1u) != 0u);
        }
    }
}

static void fb_string(int x, int y, const char *s)
{
    while (*s != '\0' && x < OLED_WIDTH) {
        fb_char(x, y, *s++);
        x += 6;                          /* 5 columns plus one of space */
    }
}

static void fb_frame(void)
{
    for (int x = 0; x < OLED_WIDTH; x++) {
        fb_pixel(x, 0, true);
        fb_pixel(x, OLED_HEIGHT - 1, true);
    }
    for (int y = 0; y < OLED_HEIGHT; y++) {
        fb_pixel(0, y, true);
        fb_pixel(OLED_WIDTH - 1, y, true);
    }
}

/* ------------------------------------------------------------------ */

test_result_t test_oled_run(void)
{
    (void)console_println("-- OLED (SSD1306 over software I2C) --------------");

    /* Find the panel first. 0x3C is the usual strap; a few modules ship
     * as 0x3D and the only difference is one resistor. */
    if (bsp_i2c_probe(OLED_I2C_ADDR_7BIT)) {
        s_addr = OLED_I2C_ADDR_7BIT;
    } else if (bsp_i2c_probe(0x3Du)) {
        s_addr = 0x3Du;
        (void)console_println("  Note: found at 0x3D, not the configured 0x3C.");
    } else {
        (void)console_printf("  No ACK at 0x%02X or 0x3D -- the panel is not on the bus.\r\n",
                             OLED_I2C_ADDR_7BIT);
        (void)console_println("  Run the I2C scan test; fix the wiring before retrying.");
        return TEST_FAIL;
    }
    (void)console_printf("  Panel ACKs at 0x%02X. Initialising %dx%d ...\r\n",
                         s_addr, OLED_WIDTH, OLED_HEIGHT);

    if (!oled_init()) {
        (void)console_println("  Init sequence NACKed part-way -- bus is unreliable.");
        return TEST_FAIL;
    }

    /* 1: every pixel on. Proves power, the charge pump and the flush path. */
    (void)console_println("  Step 1/4: all pixels ON");
    fb_fill(0xFFu);
    if (!flush_step("step 1, all pixels on")) { return TEST_FAIL; }
    vTaskDelay(pdMS_TO_TICKS(1200));

    /* 2: all off. Proves it is actually being driven, not just glowing. */
    (void)console_println("  Step 2/4: all pixels OFF");
    fb_fill(0x00u);
    if (!flush_step("step 2, all pixels off")) { return TEST_FAIL; }
    vTaskDelay(pdMS_TO_TICKS(800));

    /* 3: checkerboard. 0x55/0xAA alternating exposes page or column
     *    mapping errors, which a solid fill cannot. */
    (void)console_println("  Step 3/4: checkerboard");
    for (int page = 0; page < OLED_PAGES; page++) {
        for (int x = 0; x < OLED_WIDTH; x++) {
            s_fb[page * OLED_WIDTH + x] = ((x & 1) != 0) ? 0x55u : 0xAAu;
        }
    }
    if (!flush_step("step 3, checkerboard")) { return TEST_FAIL; }
    vTaskDelay(pdMS_TO_TICKS(1200));

    /* 4: framed text. Proves per-pixel addressing end to end. */
    (void)console_println("  Step 4/4: framed text");
    fb_fill(0x00u);
    fb_frame();
    fb_string(10, 10, "OLED OK");
    fb_string(10, 22, "STM32F411");
    fb_string(10, 34, "BRINGUP");
    if (!flush_step("step 4, framed text")) { return TEST_FAIL; }

    /* Every byte was ACKed. Whether any of it reached the glass is a
     * question only the operator can answer, so ask -- and if nobody
     * answers, say so instead of awarding a pass. */
    test_answer_t a = test_ask("  Did you see all 4 steps, ending with the text?");

    if (a == TEST_ANSWER_NO) {
        (void)console_println("  Every byte was ACKed, so the bus is fine and the fault is");
        (void)console_println("  in the panel or its configuration. Nothing at all -> almost");
        (void)console_println("  always the charge pump (0x8D,0x14) or a dead panel. Blocks");
        (void)console_println("  appear but text is scrambled -> it is an SH1106, which needs");
        (void)console_println("  a 2-column offset; that is a driver change, not wiring.");
    } else if (a == TEST_ANSWER_NONE) {
        (void)console_println("  Unconfirmed. All 1024 bytes were ACKed, which proves the bus");
        (void)console_println("  and the address -- but not one pixel is proven lit, because");
        (void)console_println("  the panel cannot be read back. Rerun with someone watching.");
    }
    return test_answer_to_result(a);
}

#else  /* BRINGUP_TEST_OLED */

#include "tests.h"
test_result_t test_oled_run(void) { return TEST_SKIP; }

#endif /* BRINGUP_TEST_OLED */
