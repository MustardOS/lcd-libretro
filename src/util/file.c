#include "file.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t *lcd_read_file(const char *path, size_t limit, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long len;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && (unsigned long)len <= limit &&
        fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)len);
        if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) {
            free(buf);
            buf = NULL;
        }
        *size = (size_t)len;
    }
    fclose(f);
    return buf;
}
