/* Content loading: hashes, ZIP/inflate, machine identification, rejection of
   unknown or damaged content, and a bounded mutation fuzz pass. */
#include "content/loader.h"
#include "util/hash.h"
#include "util/inflate.h"
#include "util/zip.h"
#include "content/samples.h"
#include "test_support.h"

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffffu); put16(p + 2, v >> 16); }

/* builds a stored (uncompressed) ZIP with the given entries */
static size_t make_zip(uint8_t *out, const char **names, const uint8_t **data, const size_t *sizes, unsigned n)
{
    size_t pos = 0, cd_start, offsets[8];
    unsigned i;
    for (i = 0; i < n; ++i) {
        size_t nl = strlen(names[i]);
        offsets[i] = pos;
        memset(out + pos, 0, 30);
        put32(out + pos, 0x04034b50);
        put16(out + pos + 4, 20);
        put32(out + pos + 14, lcd_crc32(0, data[i], sizes[i]));
        put32(out + pos + 18, (uint32_t)sizes[i]);
        put32(out + pos + 22, (uint32_t)sizes[i]);
        put16(out + pos + 26, (unsigned)nl);
        memcpy(out + pos + 30, names[i], nl);
        memcpy(out + pos + 30 + nl, data[i], sizes[i]);
        pos += 30 + nl + sizes[i];
    }
    cd_start = pos;
    for (i = 0; i < n; ++i) {
        size_t nl = strlen(names[i]);
        memset(out + pos, 0, 46);
        put32(out + pos, 0x02014b50);
        put32(out + pos + 16, lcd_crc32(0, data[i], sizes[i]));
        put32(out + pos + 20, (uint32_t)sizes[i]);
        put32(out + pos + 24, (uint32_t)sizes[i]);
        put16(out + pos + 28, (unsigned)nl);
        put32(out + pos + 42, (uint32_t)offsets[i]);
        memcpy(out + pos + 46, names[i], nl);
        pos += 46 + nl;
    }
    memset(out + pos, 0, 22);
    put32(out + pos, 0x06054b50);
    put16(out + pos + 8, n);
    put16(out + pos + 10, n);
    put32(out + pos + 12, (uint32_t)(pos - cd_start));
    put32(out + pos + 16, (uint32_t)cd_start);
    return pos + 22;
}

static void test_hashes(void)
{
    uint8_t d[20];
    static const char abc_sha1[] = "a9993e364706816aba3e25717850c26c9cd0d89d";
    char big[1000];
    CHECK_EQ(lcd_crc32(0, "123456789", 9), 0xCBF43926u);
    CHECK(lcd_sha1_matches("abc", 3, abc_sha1));
    CHECK(!lcd_sha1_matches("abd", 3, abc_sha1));
    CHECK(!lcd_sha1_matches("abc", 3, "a9993e"));
    lcd_sha1("", 0, d);
    CHECK_EQ(d[0], 0xda);
    CHECK_EQ(d[19], 0x09);
    memset(big, 'a', sizeof(big));
    /* 1000 x 'a' crosses the two block padding case at 56 mod 64 bytes */
    CHECK(lcd_sha1_matches(big, sizeof(big), "291e9a6c66994949b57ba5e650361e98fc36b1ba"));
    CHECK(lcd_sha1_matches(big, 56, "c2db330f6083854c99d4b5bfb6e8f29f201be699"));
}

static void test_inflate_vectors(void)
{
    /* raw deflate of "hello hello hello hello" (zlib level 9): fixed Huffman with a back reference */
    static const uint8_t fixed[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0x01};
    static const uint8_t stored[] = {0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c'};
    uint8_t out[64];
    size_t n = 0;
    CHECK_EQ(lcd_inflate(fixed, sizeof(fixed), out, sizeof(out), &n), 0);
    CHECK_EQ(n, 23);
    CHECK(memcmp(out, "hello hello hello hello", 23) == 0);
    CHECK_EQ(lcd_inflate(stored, sizeof(stored), out, sizeof(out), &n), 0);
    CHECK_EQ(n, 3);
    CHECK(memcmp(out, "abc", 3) == 0);
    CHECK(lcd_inflate(fixed, sizeof(fixed), out, 10, &n) != 0);
    CHECK(lcd_inflate(fixed, 4, out, sizeof(out), &n) != 0);
    CHECK(lcd_inflate((const uint8_t *)"\x07", 1, out, sizeof(out), &n) != 0);
}

static void test_selftest_set(const uint8_t *zip, size_t size)
{
    LCDContent c;
    CHECK_EQ(lcd_content_load(&c, NULL, zip, size), 0);
    CHECK(c.kind == LCD_CONTENT_MACHINE && c.def && !strcmp(c.def->id, "lcdlab_selftest"));
    CHECK_EQ(c.rom_size, 0x740);
    CHECK(c.svg[0] && c.svg_size[0] == 6668);
    CHECK_EQ(c.message[0], 0);
    lcd_content_free(&c);

    /* the same set read from disk by path */
    CHECK_EQ(lcd_content_load(&c, "roms/lcdlab_selftest.zip", NULL, 0), 0);
    lcd_content_free(&c);
}

static void test_rejections(void)
{
    LCDContent c;
    CHECK(lcd_content_load(&c, NULL, "not a rom set at all", 20) != 0);
    CHECK(strstr(c.message, "Unsupported") != NULL);
    CHECK(lcd_content_load(&c, "/nonexistent/gnw_ball.zip", NULL, 0) != 0);
    CHECK(strstr(c.message, "Unable to read") != NULL);
    CHECK(lcd_content_load(&c, NULL, NULL, 0) != 0);
    CHECK_EQ(lcd_content_load(&c, NULL, "LCD-LAB/1\ngame=catch\n", 21), 0);
    CHECK(c.kind == LCD_CONTENT_DIAGNOSTIC);
    CHECK(!lcd_content_is_manifest((const uint8_t *)"LCD-LAB/1\ngame=catcher\n", 23));
    CHECK(lcd_zip_each((const uint8_t *)"PK\x05\x06", 4, NULL, NULL) != 0);
}

typedef struct {
    LCDZipEntry e[4];
    unsigned n;
} Entries;

static int collect(const LCDZipEntry *ent, void *user)
{
    Entries *s = user;
    if (s->n < 4)
        s->e[s->n++] = *ent;
    return 0;
}

static void test_variants(const uint8_t *zip, size_t size)
{
    static uint8_t built[32768];
    Entries list;
    uint8_t *rom, *svg;
    const char *names[2];
    const uint8_t *data[2];
    size_t sizes[2], n;
    LCDContent c;

    memset(&list, 0, sizeof(list));
    CHECK_EQ(lcd_zip_each(zip, size, collect, &list), 0);
    CHECK_EQ(list.n, 2);
    rom = malloc(list.e[0].size);
    svg = malloc(list.e[1].size);
    CHECK_EQ(lcd_zip_extract(zip, size, &list.e[0], rom, list.e[0].size), 0);
    CHECK_EQ(lcd_zip_extract(zip, size, &list.e[1], svg, list.e[1].size), 0);
    CHECK(lcd_zip_extract(zip, size, &list.e[0], rom, list.e[0].size - 1) != 0);

    /* stored archive, renamed members in a sub folder: still identified by CRC */
    names[0] = "set/whatever.bin";
    names[1] = "set/screen.svg";
    data[0] = rom;
    data[1] = svg;
    sizes[0] = list.e[0].size;
    sizes[1] = list.e[1].size;
    n = make_zip(built, names, data, sizes, 2);
    CHECK_EQ(lcd_content_load(&c, NULL, built, n), 0);
    lcd_content_free(&c);

    /* missing artwork */
    n = make_zip(built, names, data, sizes, 1);
    CHECK(lcd_content_load(&c, NULL, built, n) != 0);
    CHECK(strstr(c.message, "Missing LCD artwork") != NULL);

    /* artwork with the expected name but different content: accepted with a warning */
    {
        uint8_t *alt = malloc(sizes[1] + 8);
        memcpy(alt, svg, sizes[1]);
        memcpy(alt + sizes[1] - 7, "<!--x-->\n", 9);
        names[1] = "lcdlab_selftest.svg";
        data[1] = alt;
        sizes[1] += 2;
        n = make_zip(built, names, data, sizes, 2);
        CHECK_EQ(lcd_content_load(&c, NULL, built, n), 0);
        CHECK(c.warning && strstr(c.message, "differs") != NULL);
        lcd_content_free(&c);
        free(alt);
        data[1] = svg;
        sizes[1] = list.e[1].size;
    }

    /* unknown program ROM (one byte changed) */
    rom[0x3c0] ^= 0x01;
    n = make_zip(built, names, data, sizes, 2);
    CHECK(lcd_content_load(&c, NULL, built, n) != 0);
    CHECK(strstr(c.message, "No supported") != NULL);
    rom[0x3c0] ^= 0x01;

    /* central directory claims a CRC that the data does not match */
    n = make_zip(built, names, data, sizes, 2);
    built[30 + strlen(names[0]) + 0x3c0] ^= 0x10;
    CHECK(lcd_content_load(&c, NULL, built, n) != 0);
    CHECK(strstr(c.message, "CRC") != NULL);

    free(rom);
    free(svg);
}

static void test_fuzz(const uint8_t *zip, size_t size)
{
    uint8_t *buf = malloc(size);
    uint32_t seed = 12345u;
    unsigned i, j, loaded = 0;
    for (i = 0; i < 3000u; ++i) {
        LCDContent c;
        size_t len = size;
        memcpy(buf, zip, size);
        for (j = 0; j < 1u + i % 6u; ++j) {
            seed = seed * 1103515245u + 12345u;
            buf[(seed >> 8) % size] = (uint8_t)(seed >> 24);
        }
        if (i % 7u == 0)
            len = (seed >> 4) % size;
        if (lcd_content_load(&c, NULL, buf, len) == 0) {
            loaded++;
            lcd_content_free(&c);
        }
    }
    /* mutations that only touch unused bytes may still load; corrupt data never does */
    CHECK(loaded < 3000u);
    free(buf);
}

static void test_split(void)
{
    LCDContent c;
    size_t size = 0;
    uint8_t *clone;
    CHECK_EQ(lcd_content_load(&c, "tests/fixtures/split/lcdlab_selftest_b.zip", NULL, 0), 0);
    CHECK(c.def && !strcmp(c.def->id, "lcdlab_selftest_b"));
    CHECK(c.svg[0] && c.svg_size[0] == 6668);
    CHECK(c.background != NULL); /* background.png from the parent archive too */
    lcd_content_free(&c);
    /* without the parent beside it: a clear message instead of a silent failure */
    clone = test_read_file("tests/fixtures/split/lcdlab_selftest_b.zip", &size);
    CHECK(clone && lcd_content_load(&c, "/nonexistent/lcdlab_selftest_b.zip", clone, size) != 0);
    CHECK(strstr(c.message, "parent set lcdlab_selftest.zip") != NULL);
    free(clone);

    /* shared program ROM: the clone archive has only its screen; the ROM comes from the parent */
    CHECK_EQ(lcd_content_load(&c, "tests/fixtures/split/lcdlab_selftest_c.zip", NULL, 0), 0);
    CHECK(c.def && !strcmp(c.def->id, "lcdlab_selftest_c"));
    CHECK(c.rom && c.rom_size == 0x740);
    lcd_content_free(&c);
    /* the parent archive itself is still identified as the parent, not as the clone */
    CHECK_EQ(lcd_content_load(&c, "tests/fixtures/split/lcdlab_selftest.zip", NULL, 0), 0);
    CHECK(c.def && !strcmp(c.def->id, "lcdlab_selftest"));
    lcd_content_free(&c);
    clone = test_read_file("tests/fixtures/split/lcdlab_selftest_c.zip", &size);
    CHECK(clone && lcd_content_load(&c, "/nowhere/lcdlab_selftest_c.zip", clone, size) != 0);
    CHECK(strstr(c.message, "needs its parent set lcdlab_selftest.zip") != NULL);
    free(clone);
}

static void put_le(uint8_t *p, uint32_t v, unsigned n)
{
    while (n--) {
        *p++ = (uint8_t)v;
        v >>= 8;
    }
}

/* MAME sample packs: PCM WAV, 8 or 16 bit, stereo averaged to mono */
static void test_wav(void)
{
    uint8_t wav[44 + 16];
    uint32_t len = 0, rate = 0;
    int16_t *pcm;
    memset(wav, 0, sizeof(wav));
    memcpy(wav, "RIFF", 4);
    put_le(wav + 4, sizeof(wav) - 8, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    put_le(wav + 16, 16, 4);
    put_le(wav + 20, 1, 2);
    put_le(wav + 22, 2, 2);
    put_le(wav + 24, 11025, 4);
    put_le(wav + 28, 11025 * 4, 4);
    put_le(wav + 32, 4, 2);
    put_le(wav + 34, 16, 2);
    memcpy(wav + 36, "data", 4);
    put_le(wav + 40, 16, 4);
    put_le(wav + 44, 1000, 2);
    put_le(wav + 46, 3000, 2);
    put_le(wav + 48, (uint32_t)(uint16_t)-4000, 2);
    put_le(wav + 50, 0, 2);
    pcm = lcd_wav_decode(wav, sizeof(wav), &len, &rate);
    CHECK(pcm != NULL);
    CHECK_EQ(len, 4);
    CHECK_EQ(rate, 11025);
    if (pcm) {
        CHECK_EQ(pcm[0], 2000);
        CHECK_EQ(pcm[1], -2000);
    }
    free(pcm);
    put_le(wav + 22, 1, 2); /* 8 bit mono */
    put_le(wav + 34, 8, 2);
    put_le(wav + 32, 1, 2);
    wav[44] = 0xff;
    wav[45] = 0x00;
    pcm = lcd_wav_decode(wav, sizeof(wav), &len, &rate);
    CHECK(pcm != NULL);
    CHECK_EQ(len, 16);
    if (pcm) {
        CHECK_EQ(pcm[0], 127 * 256);
        CHECK_EQ(pcm[1], -128 * 256);
    }
    free(pcm);
    put_le(wav + 20, 3, 2); /* float: refused */
    CHECK(lcd_wav_decode(wav, sizeof(wav), &len, &rate) == NULL);
    CHECK(lcd_wav_decode(wav, 20, &len, &rate) == NULL);
}

int main(void)
{
    size_t size = 0;
    uint8_t *zip = test_read_file("roms/lcdlab_selftest.zip", &size);
    CHECK(zip != NULL);
    if (!zip)
        return test_finish("loader_test");
    test_hashes();
    test_inflate_vectors();
    test_selftest_set(zip, size);
    test_rejections();
    test_variants(zip, size);
    test_split();
    test_fuzz(zip, size);
    test_wav();
    free(zip);
    return test_finish("loader_test (hashes, ZIP, inflate, identification, rejection, fuzz, WAV)");
}
