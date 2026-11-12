#ifndef LCD_UTIL_PNG_H
#define LCD_UTIL_PNG_H
#include <stddef.h>
#include <stdint.h>

#define LCD_PNG_MAX_SIDE 8192u

/* Decodes a non-interlaced PNG to straight (non-premultiplied) RGBA8.
   Supports colour types 0, 2, 3, 4 and 6 at 8 bits, palette at 1/2/4/8 bits.
   Returns 0 and a malloc'd buffer the caller frees. */
int lcd_png_decode(const uint8_t *data, size_t size, unsigned *width, unsigned *height, uint8_t **rgba);

#endif
