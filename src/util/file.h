#ifndef LCD_UTIL_FILE_H
#define LCD_UTIL_FILE_H
#include <stddef.h>
#include <stdint.h>

/* Reads a whole file of at most limit bytes; NULL when missing, empty, larger or unreadable. */
uint8_t *lcd_read_file(const char *path, size_t limit, size_t *size);

#endif
