/**
 * @file    oled.c
 * @brief   SSD1306 OLED driver over the bit-banged I2C bus.
 *
 * Extracted from test_oled.c. The init sequence, page-addressing choice and
 * 5x7 font are unchanged (they were proven on the bench); what changed is
 * that the panel is now first-class: the telemetry app renders a live status
 * page instead of leaving the glass dark.
 *
 * One page per flush keeps the work slice bounded (~4.6 ms at 250 kHz), so a
 * cooperative 10 ms loop can refresh the whole 128x64 screen in eight ticks
 * (~80 ms) without starving the UART RX ring buffer.
 */
#include "bringup_config.h"

#if BRINGUP_TEST_OLED

#include "oled.h"
#include "bsp.h"

#include <string.h>

#define OLED_PAGES     (OLED_HEIGHT / 8)
#define FB_SIZE        (OLED_WIDTH * OLED_PAGES)

static uint8_t s_fb[FB_SIZE];
static uint8_t s_addr = OLED_I2C_ADDR_7BIT;
static bool    s_present = false;

/* ------------------------------------------------------------------ */
/*  5x7 font, ASCII 0x20 (' ') through 0x5F ('_').                     */
/*  Each glyph is 5 columns; within a column bit 0 is the top pixel.   */
/*  Lowercase input is folded to uppercase so the table stays small.   */
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

/* 0x00 control byte = "command follows"; 0x40 = "display data follows". */
static bool oled_cmd(uint8_t c)
{
    uint8_t b[2] = { 0x00u, c };
    return bsp_i2c_write(s_addr, b, 2u);
}

bool oled_init(void)
{
    /* Find the panel first. 0x3C is the usual strap; a few modules are 0x3D. */
    if (bsp_i2c_probe(OLED_I2C_ADDR_7BIT)) {
        s_addr = OLED_I2C_ADDR_7BIT;
    } else if (bsp_i2c_probe(0x3Du)) {
        s_addr = 0x3Du;
    } else {
        s_present = false;
        return false;
    }

    static const uint8_t seq[] = {
        0xAE,             /* display off while we reconfigure          */
        0x20, 0x02,       /* PAGE addressing mode (matches flush cmds) */
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
        0x8D, 0x14,       /* charge pump ON (omit and panel stays dark)*/
        0xAF,             /* display on                                */
    };

    /* Multiplex ratio + COM pin config depend on panel height. */
    if (!oled_cmd(0xA8u)) { s_present = false; return false; }
#if (OLED_HEIGHT == 32)
    if (!oled_cmd(0x1Fu)) { s_present = false; return false; }
    if (!oled_cmd(0xDAu) || !oled_cmd(0x02u)) { s_present = false; return false; }
#else
    if (!oled_cmd(0x3Fu)) { s_present = false; return false; }
    if (!oled_cmd(0xDAu) || !oled_cmd(0x12u)) { s_present = false; return false; }
#endif

    for (unsigned i = 0; i < sizeof(seq); i++) {
        if (!oled_cmd(seq[i])) { s_present = false; return false; }
    }

    memset(s_fb, 0, sizeof(s_fb));
    s_present = true;
    return true;
}

bool oled_is_present(void) { return s_present; }

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    uint32_t idx = (uint32_t)((y / 8) * OLED_WIDTH + x);
    uint8_t  mask = (uint8_t)(1u << (y & 7));
    if (on) { s_fb[idx] |= mask; } else { s_fb[idx] &= (uint8_t)~mask; }
}

void oled_draw_char(int x, int y, char c)
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
            oled_pixel(x + col, y + row, ((glyph[col] >> row) & 1u) != 0u);
        }
    }
}

void oled_text(int x, int y, const char *s)
{
    if (!s) { return; }
    while (*s != '\0' && x < OLED_WIDTH) {
        oled_draw_char(x, y, *s++);
        x += 6;                          /* 5 columns + 1 of space */
    }
}

bool oled_flush_page(uint8_t page)
{
    if (!s_present || page >= OLED_PAGES) {
        return false;
    }
    uint8_t buf[1 + OLED_WIDTH];
    buf[0] = 0x40u;                              /* data control byte */

    if (!oled_cmd((uint8_t)(0xB0u | page)) ||     /* page address     */
        !oled_cmd(0x00u) ||                       /* column low  = 0  */
        !oled_cmd(0x10u)) {                        /* column high = 0  */
        return false;
    }
    memcpy(&buf[1], &s_fb[page * OLED_WIDTH], OLED_WIDTH);
    return bsp_i2c_write(s_addr, buf, sizeof(buf));
}

bool oled_flush_all(void)
{
    if (!s_present) { return false; }
    for (uint8_t p = 0; p < OLED_PAGES; p++) {
        if (!oled_flush_page(p)) {
            return false;
        }
    }
    return true;
}

#else  /* BRINGUP_TEST_OLED */

/* OLED compile-out: provide stubs so the app links even when the panel
 * test is disabled in config. */
#include "oled.h"
bool oled_init(void)        { return false; }
bool oled_is_present(void)  { return false; }
void oled_clear(void)       {}
void oled_text(int x, int y, const char *s) { (void)x; (void)y; (void)s; }
void oled_draw_char(int x, int y, char c)   { (void)x; (void)y; (void)c; }
void oled_pixel(int x, int y, bool on)      { (void)x; (void)y; (void)on; }
bool oled_flush_page(uint8_t page)          { (void)page; return false; }
bool oled_flush_all(void)                   { return false; }

#endif /* BRINGUP_TEST_OLED */
