#include "samples.h"
#include "../util/file.h"
#include "../util/zip.h"
#include <stdlib.h>
#include <string.h>

#define SAMPLE_PACK_LIMIT (32u * 1024u * 1024u)

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int16_t *lcd_wav_decode(const uint8_t *wav, size_t size, uint32_t *length, uint32_t *rate)
{
    size_t pos = 12;
    unsigned channels = 0, bits = 0;
    uint32_t hz = 0;
    if (size < 12 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4))
        return NULL;
    while (pos + 8 <= size) {
        uint32_t len = le32(wav + pos + 4);
        const uint8_t *body = wav + pos + 8;
        if (len > size - pos - 8)
            len = (uint32_t)(size - pos - 8);
        if (!memcmp(wav + pos, "fmt ", 4) && len >= 16) {
            if (le16(body) != 1)
                return NULL;
            channels = le16(body + 2);
            hz = le32(body + 4);
            bits = le16(body + 14);
        } else if (!memcmp(wav + pos, "data", 4) && channels && hz) {
            unsigned frame = channels * (bits / 8u), c;
            uint32_t n, i;
            int16_t *out;
            if ((bits != 8 && bits != 16) || channels > 2 || !frame || len < frame)
                return NULL;
            n = len / frame;
            out = malloc((size_t)n * sizeof(int16_t));
            if (!out)
                return NULL;
            for (i = 0; i < n; ++i) {
                int32_t sum = 0;
                for (c = 0; c < channels; ++c) {
                    const uint8_t *p = body + (size_t)i * frame + c * (bits / 8u);
                    sum += bits == 8 ? ((int32_t)p[0] - 128) * 256 : (int16_t)le16(p);
                }
                out[i] = (int16_t)(sum / (int32_t)channels);
            }
            *length = n;
            *rate = hz;
            return out;
        }
        pos += 8u + len + (len & 1u);
    }
    return NULL;
}

typedef struct {
    LCDSamples *s;
    const char *const *names;
    const uint8_t *zip;
    size_t size;
} Visit;

static int visit(const LCDZipEntry *e, void *user)
{
    Visit *v = user;
    const char *base = strrchr(e->name, '/');
    size_t n;
    unsigned i;
    base = base ? base + 1 : e->name;
    n = strlen(base);
    if (n < 5 || strcmp(base + n - 4, ".wav") || e->size > SAMPLE_PACK_LIMIT)
        return 0;
    for (i = 0; i < LCD_MAX_SAMPLES && v->names[i]; ++i) {
        uint8_t *buf;
        if (v->s->data[i] || strlen(v->names[i]) != n - 4 || strncmp(v->names[i], base, n - 4))
            continue;
        buf = malloc(e->size ? e->size : 1u);
        if (buf && lcd_zip_extract(v->zip, v->size, e, buf, e->size) == 0) {
            v->s->data[i] = lcd_wav_decode(buf, e->size, &v->s->length[i], &v->s->rate[i]);
            if (v->s->data[i])
                v->s->loaded++;
        }
        free(buf);
    }
    return 0;
}

unsigned lcd_samples_load(LCDSamples *s, const char *const *names, const char *const *zip_paths, unsigned path_count)
{
    unsigned p, count = 0;
    memset(s, 0, sizeof(*s));
    if (!names)
        return 0;
    while (count < LCD_MAX_SAMPLES && names[count])
        count++;
    for (p = 0; p < path_count && !s->loaded; ++p) {
        Visit v;
        size_t size = 0;
        uint8_t *zip = lcd_read_file(zip_paths[p], SAMPLE_PACK_LIMIT, &size);
        if (!zip)
            continue;
        if (lcd_zip_is_archive(zip, size)) {
            v.s = s;
            v.names = names;
            v.zip = zip;
            v.size = size;
            lcd_zip_each(zip, size, visit, &v);
        }
        free(zip);
    }
    s->bank.count = count;
    s->bank.data = (const int16_t *const *)s->data;
    s->bank.length = s->length;
    s->bank.rate = s->rate;
    return s->loaded;
}

void lcd_samples_free(LCDSamples *s)
{
    unsigned i;
    for (i = 0; i < LCD_MAX_SAMPLES; ++i)
        free(s->data[i]);
    memset(s, 0, sizeof(*s));
}
