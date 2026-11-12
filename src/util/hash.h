#ifndef LCD_UTIL_HASH_H
#define LCD_UTIL_HASH_H
#include <stddef.h>
#include <stdint.h>

uint32_t lcd_crc32(uint32_t crc, const void *data, size_t size);
void lcd_sha1(const void *data, size_t size, uint8_t digest[20]);
int lcd_sha1_matches(const void *data, size_t size, const char *hex);

#endif
