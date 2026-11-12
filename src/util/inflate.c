#include "inflate.h"
#include <string.h>

#define FAST_BITS 9u

typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
    uint16_t fast[1u << FAST_BITS]; /* (length << 9 | symbol) for short codes, 0 when longer */
} Huffman;

typedef struct {
    const uint8_t *src;
    size_t src_len, pos;
    uint32_t bitbuf;
    unsigned bitcnt;
    uint8_t *dst;
    size_t dst_len, out;
    int error;
} Inflater;

static unsigned bits(Inflater *s, unsigned need)
{
    uint32_t v;
    while (s->bitcnt < need) {
        if (s->pos >= s->src_len) {
            s->error = 1;
            return 0;
        }
        s->bitbuf |= (uint32_t)s->src[s->pos++] << s->bitcnt;
        s->bitcnt += 8u;
    }
    v = s->bitbuf & ((UINT32_C(1) << need) - 1u);
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return (unsigned)v;
}

static int build(Huffman *h, const uint8_t *lengths, unsigned n)
{
    uint16_t offs[16];
    unsigned i, left = 1;
    memset(h->counts, 0, sizeof(h->counts));
    for (i = 0; i < n; ++i)
        h->counts[lengths[i]]++;
    h->counts[0] = 0;
    for (i = 1; i < 16u; ++i) {
        left <<= 1;
        if (h->counts[i] > left)
            return -1;
        left -= h->counts[i];
    }
    offs[1] = 0;
    for (i = 1; i < 15u; ++i)
        offs[i + 1u] = (uint16_t)(offs[i] + h->counts[i]);
    for (i = 0; i < n; ++i)
        if (lengths[i])
            h->symbols[offs[lengths[i]]++] = (uint16_t)i;

    /* canonical codes, stored bit reversed because the stream is LSB first */
    memset(h->fast, 0, sizeof(h->fast));
    {
        unsigned code = 0, len, k = 0;
        for (len = 1; len <= FAST_BITS; ++len) {
            unsigned c;
            for (c = 0; c < h->counts[len]; ++c, ++k, ++code) {
                unsigned rev = 0, b, fill;
                for (b = 0; b < len; ++b)
                    rev |= (code >> b & 1u) << (len - 1u - b);
                for (fill = rev; fill < (1u << FAST_BITS); fill += 1u << len)
                    h->fast[fill] = (uint16_t)(len << 9 | h->symbols[k]);
            }
            code <<= 1;
        }
    }
    return 0;
}

static int decode(Inflater *s, const Huffman *h)
{
    int code = 0, first = 0, index = 0;
    unsigned len;
    while (s->bitcnt < FAST_BITS && s->pos < s->src_len) {
        s->bitbuf |= (uint32_t)s->src[s->pos++] << s->bitcnt;
        s->bitcnt += 8u;
    }
    if (s->bitcnt >= FAST_BITS) {
        uint16_t e = h->fast[s->bitbuf & ((1u << FAST_BITS) - 1u)];
        if (e) {
            s->bitbuf >>= e >> 9;
            s->bitcnt -= e >> 9;
            return e & 0x1ff;
        }
    }
    for (len = 1; len < 16u; ++len) {
        int count;
        code |= (int)bits(s, 1);
        if (s->error)
            return -1;
        count = h->counts[len];
        if (code - count < first)
            return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->error = 1;
    return -1;
}

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                       4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                       7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(Inflater *s, const Huffman *lit, const Huffman *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (s->out >= s->dst_len)
                return -1;
            s->dst[s->out++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            unsigned len, d;
            sym -= 257;
            if (sym >= 29)
                return -1;
            len = len_base[sym] + bits(s, len_extra[sym]);
            sym = decode(s, dist);
            if (sym < 0 || sym >= 30)
                return -1;
            d = dist_base[sym] + bits(s, dist_extra[sym]);
            if (s->error || d > s->out || len > s->dst_len - s->out)
                return -1;
            while (len--) {
                s->dst[s->out] = s->dst[s->out - d];
                s->out++;
            }
        }
    }
}

static int stored(Inflater *s)
{
    unsigned len, nlen;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->pos + 4u > s->src_len)
        return -1;
    len = s->src[s->pos] | (unsigned)s->src[s->pos + 1u] << 8;
    nlen = s->src[s->pos + 2u] | (unsigned)s->src[s->pos + 3u] << 8;
    s->pos += 4u;
    if (len != (~nlen & 0xffffu) || len > s->src_len - s->pos || len > s->dst_len - s->out)
        return -1;
    memcpy(s->dst + s->out, s->src + s->pos, len);
    s->pos += len;
    s->out += len;
    return 0;
}

static int fixed(Inflater *s)
{
    static Huffman lit, dist;
    static int ready;
    if (!ready) {
        uint8_t l[288];
        unsigned i;
        for (i = 0; i < 144u; ++i) l[i] = 8;
        for (; i < 256u; ++i) l[i] = 9;
        for (; i < 280u; ++i) l[i] = 7;
        for (; i < 288u; ++i) l[i] = 8;
        build(&lit, l, 288);
        for (i = 0; i < 30u; ++i) l[i] = 5;
        build(&dist, l, 30);
        ready = 1;
    }
    return codes(s, &lit, &dist);
}

static int dynamic(Inflater *s)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[320];
    Huffman lencode, lit, dist;
    unsigned nlen = bits(s, 5) + 257u, ndist = bits(s, 5) + 1u, ncode = bits(s, 4) + 4u, i = 0;
    if (s->error || nlen > 286u || ndist > 30u)
        return -1;
    memset(lengths, 0, sizeof(lengths));
    for (i = 0; i < ncode; ++i)
        lengths[order[i]] = (uint8_t)bits(s, 3);
    if (s->error || build(&lencode, lengths, 19) != 0)
        return -1;
    memset(lengths, 0, sizeof(lengths));
    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lencode);
        unsigned rep = 0;
        uint8_t value = 0;
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (i == 0)
                return -1;
            value = lengths[i - 1u];
            rep = 3u + bits(s, 2);
        } else if (sym == 17) {
            rep = 3u + bits(s, 3);
        } else {
            rep = 11u + bits(s, 7);
        }
        if (s->error || i + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[i++] = value;
    }
    if (lengths[256] == 0 || build(&lit, lengths, nlen) != 0 || build(&dist, lengths + nlen, ndist) != 0)
        return -1;
    return codes(s, &lit, &dist);
}

int lcd_inflate(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len, size_t *out_len)
{
    Inflater s;
    unsigned last;
    memset(&s, 0, sizeof(s));
    s.src = src;
    s.src_len = src_len;
    s.dst = dst;
    s.dst_len = dst_len;
    do {
        unsigned type;
        int rc;
        last = bits(&s, 1);
        type = bits(&s, 2);
        if (s.error)
            return -1;
        if (type == 0)
            rc = stored(&s);
        else if (type == 1)
            rc = fixed(&s);
        else if (type == 2)
            rc = dynamic(&s);
        else
            rc = -1;
        if (rc != 0 || s.error)
            return -1;
    } while (!last);
    *out_len = s.out;
    return 0;
}
