/**
 * @file    oled.h
 * @brief   Reusable SSD1306 OLED driver (128x64, software I2C).
 *
 * Extracted from test_oled.c so the telemetry application can show a live
 * status page instead of going dark the moment the menu hands over.
 *
 * Usage pattern for a cooperative loop (one page per tick keeps each slice
 * bounded; a full 8-page flush at 250 kHz I2C is ~37 ms which is too long
 * for a 10 ms tick that must also drain a 256-byte UART ring):
 *
 *   oled_init();                 // once, at app start
 *   for (;;) {
 *       ...
 *       if (time_to_refresh) {   // e.g. every 250 ms
 *           oled_clear();
 *           oled_text(0, 0, "TEMP  25.4 C");
 *           oled_text(0, 9, "CUR   0.42 A");
 *           ...
 *       }
 *       oled_flush_page(page++ & 7);   // one page per tick
 *   }
 *
 * The framebuffer is static RAM (1 KB). Text is drawn into it; nothing
 * reaches the panel until oled_flush_page/flush_all is called.
 */
#ifndef OLED_H
#define OLED_H

#include <stdbool.h>
#include <stdint.h>

/** Probe + init the panel. Returns true if a panel ACKed and initialised. */
bool oled_init(void);

/** True if oled_init() found a panel. */
bool oled_is_present(void);

/** Clear the framebuffer to black. Does not touch the panel. */
void oled_clear(void);

/**
 * Draw a NUL-terminated string into the framebuffer.
 * @param x  column 0..127 (pixels)
 * @param y  row 0..63 (pixels); each text line is 8 px tall
 * Lowercase is folded to uppercase (the font table covers 0x20..0x5F).
 */
void oled_text(int x, int y, const char *s);

/**
 * Draw a single character into the framebuffer (same folding as oled_text).
 */
void oled_draw_char(int x, int y, char c);

/** Set/clear a single pixel in the framebuffer. */
void oled_pixel(int x, int y, bool on);

/**
 * Push one 8-pixel page (0..7) from the framebuffer to the panel.
 * Returns true on success. ~4.6 ms at 250 kHz I2C.
 */
bool oled_flush_page(uint8_t page);

/** Push all 8 pages. ~37 ms at 250 kHz — use only when a long block is OK. */
bool oled_flush_all(void);

#endif /* OLED_H */
