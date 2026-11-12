#ifndef LCD_UTIL_ZIP_H
#define LCD_UTIL_ZIP_H
#include <stddef.h>
#include <stdint.h>

#define LCD_ZIP_NAME_MAX 128u

typedef struct {
    char name[LCD_ZIP_NAME_MAX];
    uint32_t crc;
    uint32_t size;
    uint32_t packed;
    uint16_t method;
    uint32_t local_offset;
} LCDZipEntry;

int lcd_zip_is_archive(const uint8_t *data, size_t size);
/* Calls visit for each central directory entry; stops early when visit returns non-zero. */
int lcd_zip_each(const uint8_t *data, size_t size, int (*visit)(const LCDZipEntry *e, void *user), void *user);
/* Extracts an entry into dst (dst_len must equal e->size); verifies the CRC. */
int lcd_zip_extract(const uint8_t *data, size_t size, const LCDZipEntry *e, uint8_t *dst, size_t dst_len);

#endif
