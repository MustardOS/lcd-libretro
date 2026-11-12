#include "hash.h"
#include <string.h>

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256u; ++i) {
        c = i;
        for (j = 0; j < 8u; ++j)
            c = (c & 1u) ? (c >> 1) ^ UINT32_C(0xEDB88320) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = 1;
}

uint32_t lcd_crc32(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *p = data;
    if (!crc_ready)
        crc_init();
    crc = ~crc;
    while (size--)
        crc = crc_table[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

static uint32_t rol(uint32_t v, unsigned n) { return (v << n) | (v >> (32u - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *b)
{
    uint32_t w[80], a, bb, c, d, e, f, k, t;
    unsigned i;
    for (i = 0; i < 16u; ++i)
        w[i] = (uint32_t)b[i * 4u] << 24 | (uint32_t)b[i * 4u + 1u] << 16 |
               (uint32_t)b[i * 4u + 2u] << 8 | b[i * 4u + 3u];
    for (i = 16; i < 80u; ++i)
        w[i] = rol(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1);
    a = h[0]; bb = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80u; ++i) {
        if (i < 20u) { f = (bb & c) | (~bb & d); k = UINT32_C(0x5A827999); }
        else if (i < 40u) { f = bb ^ c ^ d; k = UINT32_C(0x6ED9EBA1); }
        else if (i < 60u) { f = (bb & c) | (bb & d) | (c & d); k = UINT32_C(0x8F1BBCDC); }
        else { f = bb ^ c ^ d; k = UINT32_C(0xCA62C1D6); }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}

void lcd_sha1(const void *data, size_t size, uint8_t digest[20])
{
    uint32_t h[5] = {UINT32_C(0x67452301), UINT32_C(0xEFCDAB89), UINT32_C(0x98BADCFE),
                     UINT32_C(0x10325476), UINT32_C(0xC3D2E1F0)};
    const uint8_t *p = data;
    uint8_t tail[128];
    size_t full = size / 64u * 64u, rest = size - full, tail_len;
    uint64_t bits = (uint64_t)size * 8u;
    size_t i;
    for (i = 0; i < full; i += 64u)
        sha1_block(h, p + i);
    memset(tail, 0, sizeof(tail));
    if (rest)
        memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    tail_len = rest + 9u > 64u ? 128u : 64u;
    for (i = 0; i < 8u; ++i)
        tail[tail_len - 1u - i] = (uint8_t)(bits >> (8u * i));
    sha1_block(h, tail);
    if (tail_len == 128u)
        sha1_block(h, tail + 64);
    for (i = 0; i < 20u; ++i)
        digest[i] = (uint8_t)(h[i / 4u] >> (24u - 8u * (i % 4u)));
}

int lcd_sha1_matches(const void *data, size_t size, const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[20];
    unsigned i;
    if (!hex || strlen(hex) != 40u)
        return 0;
    lcd_sha1(data, size, digest);
    for (i = 0; i < 20u; ++i)
        if (hex[i * 2u] != digits[digest[i] >> 4] || hex[i * 2u + 1u] != digits[digest[i] & 15u])
            return 0;
    return 1;
}
