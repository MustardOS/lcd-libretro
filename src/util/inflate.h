#ifndef LCD_UTIL_INFLATE_H
#define LCD_UTIL_INFLATE_H
#include <stddef.h>
#include <stdint.h>

/* Raw DEFLATE (RFC 1951). Returns 0 and the produced length on success. */
int lcd_inflate(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len, size_t *out_len);

#endif
