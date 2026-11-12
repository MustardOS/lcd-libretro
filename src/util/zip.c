#include "zip.h"
#include "hash.h"
#include "inflate.h"
#include <string.h>

#define ZIP_MAX_ENTRIES 4096u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int lcd_zip_is_archive(const uint8_t *data, size_t size)
{
    return data && size >= 22u && rd32(data) == UINT32_C(0x04034b50);
}

static int find_end(const uint8_t *data, size_t size, size_t *end)
{
    size_t i, lowest = size > 22u + 65535u ? size - 22u - 65535u : 0;
    if (size < 22u)
        return -1;
    for (i = size - 22u + 1u; i-- > lowest;)
        if (rd32(data + i) == UINT32_C(0x06054b50)) {
            *end = i;
            return 0;
        }
    return -1;
}

int lcd_zip_each(const uint8_t *data, size_t size, int (*visit)(const LCDZipEntry *e, void *user), void *user)
{
    size_t end, pos;
    unsigned count, i;
    if (!data || find_end(data, size, &end) != 0)
        return -1;
    count = rd16(data + end + 10u);
    pos = rd32(data + end + 16u);
    if (count > ZIP_MAX_ENTRIES || pos > end)
        return -1;
    for (i = 0; i < count; ++i) {
        LCDZipEntry e;
        unsigned name_len, extra_len, comment_len;
        if (pos + 46u > end || rd32(data + pos) != UINT32_C(0x02014b50))
            return -1;
        name_len = rd16(data + pos + 28u);
        extra_len = rd16(data + pos + 30u);
        comment_len = rd16(data + pos + 32u);
        if (pos + 46u + name_len + extra_len + comment_len > end)
            return -1;
        memset(&e, 0, sizeof(e));
        e.method = rd16(data + pos + 10u);
        e.crc = rd32(data + pos + 16u);
        e.packed = rd32(data + pos + 20u);
        e.size = rd32(data + pos + 24u);
        e.local_offset = rd32(data + pos + 42u);
        if (name_len < LCD_ZIP_NAME_MAX) {
            memcpy(e.name, data + pos + 46u, name_len);
            if (visit(&e, user))
                return 0;
        }
        pos += 46u + name_len + extra_len + comment_len;
    }
    return 0;
}

int lcd_zip_extract(const uint8_t *data, size_t size, const LCDZipEntry *e, uint8_t *dst, size_t dst_len)
{
    size_t start, out = 0;
    unsigned name_len, extra_len;
    if (!data || !e || dst_len != e->size || (size_t)e->local_offset + 30u > size ||
        rd32(data + e->local_offset) != UINT32_C(0x04034b50))
        return -1;
    name_len = rd16(data + e->local_offset + 26u);
    extra_len = rd16(data + e->local_offset + 28u);
    start = (size_t)e->local_offset + 30u + name_len + extra_len;
    if (start > size || e->packed > size - start)
        return -1;
    if (e->method == 0) {
        if (e->packed != e->size)
            return -1;
        memcpy(dst, data + start, e->size);
        out = e->size;
    } else if (e->method == 8) {
        if (lcd_inflate(data + start, e->packed, dst, dst_len, &out) != 0)
            return -1;
    } else {
        return -1;
    }
    if (out != e->size || lcd_crc32(0, dst, out) != e->crc)
        return -1;
    return 0;
}
