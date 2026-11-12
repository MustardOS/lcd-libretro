#include "png.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static int unfilter(uint8_t *raw, unsigned rows, size_t stride, unsigned bpp)
{
    uint8_t *prev = NULL;
    unsigned y;
    for (y = 0; y < rows; ++y) {
        uint8_t *line = raw + y * (stride + 1u);
        uint8_t type = line[0];
        uint8_t *cur = line + 1;
        size_t x;
        if (type > 4)
            return -1;
        for (x = 0; x < stride; ++x) {
            int a = x >= bpp ? cur[x - bpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            switch (type) {
            case 1: cur[x] = (uint8_t)(cur[x] + a); break;
            case 2: cur[x] = (uint8_t)(cur[x] + b); break;
            case 3: cur[x] = (uint8_t)(cur[x] + ((a + b) >> 1)); break;
            case 4: cur[x] = (uint8_t)(cur[x] + paeth(a, b, c)); break;
            default: break;
            }
        }
        prev = cur;
    }
    return 0;
}

int lcd_png_decode(const uint8_t *data, size_t size, unsigned *width, unsigned *height, uint8_t **rgba)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    uint8_t palette[256][4];
    unsigned w = 0, h = 0, depth = 0, ctype = 0, channels, bpp, y, x, palette_n = 0;
    uint8_t *idat = NULL, *raw = NULL, *out = NULL;
    size_t idat_len = 0, pos = 8, stride, raw_len, produced;
    int ok = 0;

    *rgba = NULL;
    if (!data || size < 33 || memcmp(data, sig, 8) != 0)
        return -1;
    memset(palette, 255, sizeof(palette));
    while (pos + 12u <= size) {
        uint32_t len = be32(data + pos);
        const uint8_t *type = data + pos + 4, *body = data + pos + 8;
        if (len > size - pos - 12u)
            goto done;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(body);
            h = be32(body + 4);
            depth = body[8];
            ctype = body[9];
            if (body[10] || body[11] || body[12]) /* compression, filter, interlace */
                goto done;
        } else if (!memcmp(type, "PLTE", 4)) {
            for (palette_n = 0; palette_n < len / 3u && palette_n < 256u; ++palette_n)
                memcpy(palette[palette_n], body + palette_n * 3u, 3);
        } else if (!memcmp(type, "tRNS", 4) && ctype == 3) {
            for (x = 0; x < len && x < 256u; ++x)
                palette[x][3] = body[x];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *n = realloc(idat, idat_len + len + 1u);
            if (!n)
                goto done;
            idat = n;
            memcpy(idat + idat_len, body, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12u + len;
    }
    if (!w || !h || w > LCD_PNG_MAX_SIDE || h > LCD_PNG_MAX_SIDE || !idat || idat_len < 6)
        goto done;
    channels = ctype == 0 ? 1u : ctype == 2 ? 3u : ctype == 3 ? 1u : ctype == 4 ? 2u : ctype == 6 ? 4u : 0u;
    if (!channels || (ctype != 3 && depth != 8) || (ctype == 3 && depth != 1 && depth != 2 && depth != 4 && depth != 8))
        goto done;
    stride = ((size_t)w * channels * depth + 7u) / 8u;
    bpp = (channels * depth + 7u) / 8u;
    raw_len = (stride + 1u) * h;
    raw = malloc(raw_len);
    out = malloc((size_t)w * h * 4u);
    /* zlib wrapper: 2 byte header, deflate data, 4 byte Adler-32 */
    if (!raw || !out || (idat[0] & 0x0f) != 8 || (idat[1] & 0x20) ||
        lcd_inflate(idat + 2, idat_len - 2, raw, raw_len, &produced) != 0 || produced != raw_len ||
        unfilter(raw, h, stride, bpp) != 0)
        goto done;
    for (y = 0; y < h; ++y) {
        const uint8_t *s = raw + y * (stride + 1u) + 1;
        uint8_t *d = out + (size_t)y * w * 4u;
        for (x = 0; x < w; ++x, d += 4) {
            switch (ctype) {
            case 0: d[0] = d[1] = d[2] = s[x]; d[3] = 255; break;
            case 2: memcpy(d, s + x * 3u, 3); d[3] = 255; break;
            case 4: d[0] = d[1] = d[2] = s[x * 2u]; d[3] = s[x * 2u + 1u]; break;
            case 6: memcpy(d, s + x * 4u, 4); break;
            default: {
                unsigned per = 8u / depth, bit = (x % per) * depth;
                unsigned idx = (s[x / per] >> (8u - depth - bit)) & ((1u << depth) - 1u);
                if (idx >= palette_n)
                    goto done;
                memcpy(d, palette[idx], 4);
            }
            }
        }
    }
    ok = 1;
    *width = w;
    *height = h;
    *rgba = out;
    out = NULL;

done:
    free(idat);
    free(raw);
    free(out);
    return ok ? 0 : -1;
}
