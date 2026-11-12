/*
  Segment LCD renderer.

  Each SVG screen from the MAME ROM set is parsed and rasterised once with
  nanosvg (MAME 0.289 fork, which records <title> keys). Shapes titled "x.y.z"
  are MAME output names for LCD segments; every other shape is static artwork.
  Artwork below the first segment becomes a background image. Each segment, and
  any artwork drawn after one, becomes a list of (offset, colour, coverage)
  pixels kept in document order, so a frame is one background copy plus a blit
  of the lit segments, as MAME composites the SVG. No SVG or XML is touched
  while running.
*/

#include "lcd_render.h"
#include "../content/gnw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#include "../../third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../../third_party/nanosvg/nanosvgrast.h"

typedef struct {
    unsigned x, y, w, h;
} Rect;

typedef struct {
    LCDSegment *segments;
    unsigned seg_count, seg_cap;
    uint32_t *offset, *colour;
    uint8_t *alpha;
    size_t px_count, px_cap;
} Builder;

static void set_error(char *err, size_t len, const char *msg)
{
    if (err && len)
        snprintf(err, len, "%s", msg);
}

void lcd_render_layout_size(const LCDMachineDef *def, unsigned gap, unsigned *w, unsigned *h)
{
    const LCDScreenDef *s = def->screens;
    switch (def->layout) {
    case LCD_LAYOUT_DUAL_VERTICAL:
        *w = s[0].width > s[1].width ? s[0].width : s[1].width;
        *h = (unsigned)s[0].height + gap + s[1].height;
        break;
    case LCD_LAYOUT_DUAL_HORIZONTAL:
        *w = (unsigned)s[0].width + gap + s[1].width;
        *h = s[0].height > s[1].height ? s[0].height : s[1].height;
        break;
    case LCD_LAYOUT_TRIPLE_HORIZONTAL: { /* gapless in MAME */
        unsigned i;
        *w = 0;
        *h = 0;
        for (i = 0; i < 3; ++i) {
            *w += s[i].width;
            if (s[i].height > *h)
                *h = s[i].height;
        }
        break;
    }
    default:
        *w = s[0].width;
        *h = s[0].height;
        break;
    }
}

static unsigned scaled(unsigned v, double scale)
{
    long r = lround(v * scale);
    return r < 1 ? 1u : (unsigned)r;
}

static void screen_rects(const LCDMachineDef *def, const LCDRenderOptions *opt, unsigned *out_w, unsigned *out_h,
                         Rect rects[LCD_MAX_SCREENS])
{
    unsigned lw, lh, i;
    double sx, sy, scale;
    lcd_render_layout_size(def, opt->gap, &lw, &lh);
    sx = (double)opt->box_w / lw;
    sy = (double)opt->box_h / lh;
    scale = sx < sy ? sx : sy;
    *out_w = scaled(lw, scale);
    *out_h = scaled(lh, scale);
    for (i = 0; i < def->screen_count; ++i) {
        unsigned ux = 0, uy = 0;
        if (i == 1 && def->layout == LCD_LAYOUT_DUAL_VERTICAL)
            uy = (unsigned)def->screens[0].height + opt->gap;
        if (i == 1 && def->layout == LCD_LAYOUT_DUAL_HORIZONTAL)
            ux = (unsigned)def->screens[0].width + opt->gap;
        if (def->layout == LCD_LAYOUT_TRIPLE_HORIZONTAL) {
            unsigned k;
            for (k = 0; k < i; ++k)
                ux += def->screens[k].width;
        }
        rects[i].x = (unsigned)lround(ux * scale);
        rects[i].y = (unsigned)lround(uy * scale);
        rects[i].w = scaled(def->screens[i].width, scale);
        rects[i].h = scaled(def->screens[i].height, scale);
        if (rects[i].x + rects[i].w > *out_w)
            rects[i].w = *out_w - rects[i].x;
        if (rects[i].y + rects[i].h > *out_h)
            rects[i].h = *out_h - rects[i].y;
    }
}

static int parse_key(const char *title, uint8_t *x, uint8_t *y, uint8_t *z)
{
    unsigned a, b, c;
    int n = 0;
    if (!title[0] || sscanf(title, "%u.%u.%u%n", &a, &b, &c, &n) != 3 || title[n] != '\0' || a >= 16u ||
        b >= 32u || c >= 4u)
        return 0;
    *x = (uint8_t)a;
    *y = (uint8_t)b;
    *z = (uint8_t)c;
    return 1;
}

static uint32_t tint_rgb(uint32_t r, uint32_t g, uint32_t b, uint32_t tint)
{
    r = r * (tint >> 16 & 0xffu) / 255u;
    g = g * (tint >> 8 & 0xffu) / 255u;
    b = b * (tint & 0xffu) / 255u;
    return r << 16 | g << 8 | b;
}

static uint32_t blend32(uint32_t dst, uint32_t src, unsigned a)
{
    uint32_t out = 0;
    unsigned shift;
    for (shift = 0; shift < 24u; shift += 8u) {
        unsigned s = src >> shift & 0xffu, d = dst >> shift & 0xffu;
        unsigned t = s * a + d * (255u - a) + 128u;
        out |= (uint32_t)((t + (t >> 8)) >> 8) << shift;
    }
    return out;
}

static uint16_t to565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800u) | ((c >> 5) & 0x07e0u) | ((c >> 3) & 0x001fu));
}

static uint32_t from565(uint16_t c)
{
    uint32_t r = (c >> 11) & 0x1fu, g = (c >> 5) & 0x3fu, b = c & 0x1fu;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static int grow(void **p, size_t elem, size_t need, size_t *cap)
{
    void *n;
    size_t c = *cap ? *cap : 1024u;
    if (need <= *cap)
        return 0;
    while (c < need)
        c *= 2u;
    n = realloc(*p, c * elem);
    if (!n)
        return -1;
    *p = n;
    *cap = c;
    return 0;
}

static int add_pixels(Builder *b, size_t extra)
{
    size_t cap = b->px_cap, need = b->px_count + extra;
    if (need <= cap)
        return 0;
    if (grow((void **)&b->offset, sizeof(uint32_t), need, &cap) != 0)
        return -1;
    cap = b->px_cap;
    if (grow((void **)&b->colour, sizeof(uint32_t), need, &cap) != 0)
        return -1;
    cap = b->px_cap;
    if (grow((void **)&b->alpha, 1, need, &cap) != 0)
        return -1;
    b->px_cap = cap;
    return 0;
}

static void show(NSVGshape *s, unsigned char orig, int on)
{
    s->flags = on && (orig & NSVG_FLAGS_VISIBLE) ? (unsigned char)(s->flags | NSVG_FLAGS_VISIBLE)
                                                 : (unsigned char)(s->flags & ~NSVG_FLAGS_VISIBLE);
}

static NSVGshape *nth_shape(NSVGimage *img, size_t n)
{
    NSVGshape *s = img->shapes;
    while (s && n--)
        s = s->next;
    return s;
}

/* rasterises the visible shapes into a new layer of pixels; segments take the contrast setting */
static int add_layer(Builder *b, NSVGimage *img, NSVGrasterizer *rast, unsigned char *rgba, float scale,
                     const Rect *rect, unsigned frame_w, const LCDRenderOptions *opt, uint8_t x, uint8_t y, uint8_t z,
                     bool always)
{
    NSVGshape *s;
    LCDSegment *seg;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    int bx0, by0, bx1, by1;
    unsigned bw, bh, px, py;
    for (s = img->shapes; s; s = s->next)
        if (s->flags & NSVG_FLAGS_VISIBLE) {
            if (s->bounds[0] < x0) x0 = s->bounds[0];
            if (s->bounds[1] < y0) y0 = s->bounds[1];
            if (s->bounds[2] > x1) x1 = s->bounds[2];
            if (s->bounds[3] > y1) y1 = s->bounds[3];
        }
    bx0 = (int)floorf(x0 * scale) - 1;
    by0 = (int)floorf(y0 * scale) - 1;
    bx1 = (int)ceilf(x1 * scale) + 2;
    by1 = (int)ceilf(y1 * scale) + 2;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > (int)rect->w) bx1 = (int)rect->w;
    if (by1 > (int)rect->h) by1 = (int)rect->h;
    if (bx1 <= bx0 || by1 <= by0)
        return 0;
    bw = (unsigned)(bx1 - bx0);
    bh = (unsigned)(by1 - by0);
    {
        size_t cap = b->seg_cap;
        if (grow((void **)&b->segments, sizeof(LCDSegment), (size_t)b->seg_count + 1u, &cap) != 0)
            return -1;
        b->seg_cap = (unsigned)cap;
    }
    if (add_pixels(b, (size_t)bw * bh) != 0)
        return -1;
    seg = &b->segments[b->seg_count++];
    seg->x = x;
    seg->y = y;
    seg->z = z;
    seg->always = always;
    seg->first = (uint32_t)b->px_count;
    memset(rgba, 0, (size_t)bw * bh * 4u);
    nsvgRasterize(rast, img, (float)-bx0, (float)-by0, scale, rgba, (int)bw, (int)bh, (int)bw * 4);
    for (py = 0; py < bh; ++py)
        for (px = 0; px < bw; ++px) {
            const unsigned char *p = rgba + ((size_t)py * bw + px) * 4u;
            unsigned a = always ? p[3] : p[3] * opt->contrast / 100u;
            if (!a)
                continue;
            b->offset[b->px_count] =
                (uint32_t)((rect->y + (unsigned)by0 + py) * frame_w + rect->x + (unsigned)bx0 + px);
            b->colour[b->px_count] = tint_rgb(p[0], p[1], p[2], opt->tint);
            b->alpha[b->px_count] = (uint8_t)a;
            b->px_count++;
        }
    seg->count = (uint32_t)(b->px_count - seg->first);
    seg->x0 = (uint16_t)(rect->x + (unsigned)bx0);
    seg->y0 = (uint16_t)(rect->y + (unsigned)by0);
    seg->x1 = (uint16_t)(rect->x + (unsigned)bx1);
    seg->y1 = (uint16_t)(rect->y + (unsigned)by1);
    return 0;
}

static int build_screen(Builder *b, uint32_t *bg, unsigned frame_w, const Rect *rect, const LCDSvgSource *src,
                        const LCDRenderOptions *opt, char *err, size_t err_len)
{
    NSVGimage *img;
    NSVGrasterizer *rast;
    NSVGshape *shape;
    unsigned char *rgba = NULL;
    char *text;
    unsigned char *orig_flags = NULL, *keyed = NULL;
    size_t shape_count = 0, i, first_key, run;
    double sx, sy;
    float scale;
    int rc = -1;
    unsigned px, py;

    text = malloc(src->size + 1u);
    if (!text) {
        set_error(err, err_len, "Out of memory parsing SVG");
        return -1;
    }
    memcpy(text, src->data, src->size);
    text[src->size] = '\0';
    img = nsvgParse(text, "px", 72.0f);
    free(text);
    rast = nsvgCreateRasterizer();
    if (!img || !rast || img->width <= 0.0f || img->height <= 0.0f) {
        set_error(err, err_len, "Unable to parse LCD SVG");
        goto done;
    }
    for (shape = img->shapes; shape; shape = shape->next)
        shape_count++;
    orig_flags = malloc(shape_count ? shape_count : 1u);
    keyed = malloc(shape_count ? shape_count : 1u);
    rgba = malloc((size_t)rect->w * rect->h * 4u);
    first_key = shape_count;
    if (!orig_flags || !keyed || !rgba) {
        set_error(err, err_len, "Out of memory rasterising SVG");
        goto done;
    }

    sx = (double)rect->w / img->width;
    sy = (double)rect->h / img->height;
    scale = (float)(sx < sy ? sx : sy);

    for (i = 0, shape = img->shapes; shape; shape = shape->next, ++i) {
        uint8_t x, y, z;
        orig_flags[i] = shape->flags;
        keyed[i] = parse_key(shape->title, &x, &y, &z) != 0;
        if (keyed[i] && (orig_flags[i] & NSVG_FLAGS_VISIBLE) && first_key == shape_count)
            first_key = i;
    }

    /* static artwork below the first segment */
    for (i = 0, shape = img->shapes; shape; shape = shape->next, ++i)
        show(shape, orig_flags[i], !keyed[i] && i < first_key);
    memset(rgba, 0, (size_t)rect->w * rect->h * 4u);
    nsvgRasterize(rast, img, 0, 0, scale, rgba, (int)rect->w, (int)rect->h, (int)rect->w * 4);
    for (py = 0; py < rect->h; ++py)
        for (px = 0; px < rect->w; ++px) {
            const unsigned char *p = rgba + ((size_t)py * rect->w + px) * 4u;
            unsigned a = p[3];
            bg[(size_t)(rect->y + py) * frame_w + rect->x + px] =
                tint_rgb(p[0] * a / 255u, p[1] * a / 255u, p[2] * a / 255u, opt->tint);
        }

    /* then in document order, as MAME draws the SVG: one layer per segment key, and the static
       shapes between segments as layers that are always drawn */
    for (i = first_key, run = first_key; i <= shape_count; ++i) {
        NSVGshape *s;
        size_t j;
        uint8_t x, y, z;
        int seen = 0;
        shape = nth_shape(img, i);
        if (shape && (!keyed[i] || !(orig_flags[i] & NSVG_FLAGS_VISIBLE)))
            continue;
        if (shape)
            for (j = 0, s = img->shapes; s != shape; s = s->next, ++j)
                if (keyed[j] && !strcmp(s->title, shape->title) && (orig_flags[j] & NSVG_FLAGS_VISIBLE))
                    seen = 1;
        if (seen)
            continue;
        if (run < i) {
            int any = 0;
            for (j = 0, s = img->shapes; s; s = s->next, ++j) {
                int on = j >= run && j < i && !keyed[j] && (orig_flags[j] & NSVG_FLAGS_VISIBLE);
                show(s, orig_flags[j], on);
                any |= on;
            }
            if (any && add_layer(b, img, rast, rgba, scale, rect, frame_w, opt, 0xff, 0xff, 0xff, true) != 0)
                goto oom;
        }
        run = i + 1;
        if (!shape)
            break;
        parse_key(shape->title, &x, &y, &z);
        for (j = 0, s = img->shapes; s; s = s->next, ++j)
            show(s, orig_flags[j], keyed[j] && !strcmp(s->title, shape->title));
        if (add_layer(b, img, rast, rgba, scale, rect, frame_w, opt, x, y, z, false) != 0)
            goto oom;
    }
    rc = 0;
    goto done;

oom:
    set_error(err, err_len, "Out of memory building segments");

done:
    free(keyed);
    free(orig_flags);
    free(rgba);
    if (rast)
        nsvgDeleteRasterizer(rast);
    if (img)
        nsvgDelete(img);
    return rc;
}

/* box filtered copy of the background image at the frame size, composited over white */
static uint32_t *scaled_background(const LCDRenderOptions *opt, unsigned w, unsigned h)
{
    uint32_t *out = malloc((size_t)w * h * sizeof(uint32_t));
    unsigned x, y, iw = opt->background_w, ih = opt->background_h;
    if (!out)
        return NULL;
    for (y = 0; y < h; ++y) {
        unsigned y0 = (unsigned)((uint64_t)y * ih / h), y1 = (unsigned)((uint64_t)(y + 1u) * ih / h);
        if (y1 <= y0)
            y1 = y0 + 1u;
        for (x = 0; x < w; ++x) {
            unsigned x0 = (unsigned)((uint64_t)x * iw / w), x1 = (unsigned)((uint64_t)(x + 1u) * iw / w), sx, sy;
            uint64_t sum[3] = {0, 0, 0}, n;
            if (x1 <= x0)
                x1 = x0 + 1u;
            n = (uint64_t)(x1 - x0) * (y1 - y0);
            for (sy = y0; sy < y1; ++sy) {
                const uint8_t *p = opt->background + ((size_t)sy * iw + x0) * 4u;
                for (sx = x0; sx < x1; ++sx, p += 4) {
                    unsigned a = p[3], c;
                    for (c = 0; c < 3; ++c)
                        sum[c] += (p[c] * a + 255u * (255u - a)) / 255u;
                }
            }
            out[(size_t)y * w + x] = (uint32_t)(sum[0] / n) << 16 | (uint32_t)(sum[1] / n) << 8 | (uint32_t)(sum[2] / n);
        }
    }
    return out;
}

static uint32_t multiply_rgb(uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    unsigned shift;
    for (shift = 0; shift < 24u; shift += 8u)
        out |= (((a >> shift & 0xffu) * (b >> shift & 0xffu) + 127u) / 255u) << shift;
    return out;
}

void lcd_render_free(LCDRender *r)
{
    if (!r)
        return;
    free(r->frame);
    free(r->background);
    free(r->segments);
    free(r->pixel_offset);
    free(r->pixel_colour);
    free(r->pixel_alpha);
    free(r->last_levels);
    memset(r, 0, sizeof(*r));
}

int lcd_render_build(LCDRender *r, const LCDMachineDef *def, const LCDSvgSource *svgs, unsigned svg_count,
                     const LCDRenderOptions *opt, bool rgb565, char *err, size_t err_len)
{
    Rect rects[LCD_MAX_SCREENS];
    Builder b;
    uint32_t *bg32;
    size_t pixels, i;
    unsigned w, h, s;

    memset(r, 0, sizeof(*r));
    memset(&b, 0, sizeof(b));
    if (!def || !svgs || svg_count != def->screen_count || !opt || !opt->box_w || !opt->box_h) {
        set_error(err, err_len, "Invalid display configuration");
        return -1;
    }
    screen_rects(def, opt, &w, &h, rects);
    pixels = (size_t)w * h;
    bg32 = calloc(pixels, sizeof(uint32_t));
    if (!bg32) {
        set_error(err, err_len, "Out of memory allocating display");
        return -1;
    }
    {
        LCDRenderOptions o = *opt;
        if (opt->background)
            o.tint = 0xffffff; /* the image takes the place of the tint */
        for (s = 0; s < svg_count; ++s)
            if (build_screen(&b, bg32, w, &rects[s], &svgs[s], &o, err, err_len) != 0)
                goto fail;
    }
    for (i = 0; i < b.seg_count && b.segments[i].always; ++i)
        ;
    if (i == b.seg_count) {
        set_error(err, err_len, "LCD SVG has no segment shapes (x.y.z titles)");
        goto fail;
    }

    /* background.png multiplies the static LCD and every segment pixel (MAME screen multiply) */
    if (opt->background && opt->background_w && opt->background_h) {
        uint32_t *img = scaled_background(opt, w, h);
        if (!img) {
            set_error(err, err_len, "Out of memory scaling background.png");
            goto fail;
        }
        for (i = 0; i < pixels; ++i)
            bg32[i] = multiply_rgb(bg32[i], img[i]);
        for (i = 0; i < b.px_count; ++i)
            b.colour[i] = multiply_rgb(b.colour[i], img[b.offset[i]]);
        free(img);
    }

    /* inactive segments faintly visible: baked into the background */
    if (opt->ghost)
        for (s = 0; s < b.seg_count; ++s)
            for (i = b.segments[s].first; !b.segments[s].always && i < b.segments[s].first + b.segments[s].count; ++i) {
                uint32_t *d = &bg32[b.offset[i]];
                *d = blend32(*d, b.colour[i], (unsigned)b.alpha[i] * opt->ghost / 100u);
            }

    r->rgb565 = rgb565;
    r->width = w;
    r->height = h;
    r->segments = b.segments;
    r->segment_count = b.seg_count;
    r->pixel_offset = b.offset;
    r->pixel_colour = b.colour;
    r->pixel_alpha = b.alpha;
    r->pixel_count = b.px_count;
    r->fade = opt->fade;
    r->rise = opt->rise ? opt->rise : 255;
    r->fall = opt->fall ? opt->fall : 255;
    r->last_levels = calloc(b.seg_count, 1);
    r->frame = malloc(pixels * (rgb565 ? 2u : 4u));
    if (rgb565) {
        uint16_t *bg16 = malloc(pixels * 2u);
        if (bg16)
            for (i = 0; i < pixels; ++i)
                bg16[i] = to565(bg32[i]);
        free(bg32);
        r->background = bg16;
    } else {
        r->background = bg32;
    }
    bg32 = NULL;
    if (!r->last_levels || !r->frame || !r->background) {
        lcd_render_free(r);
        set_error(err, err_len, "Out of memory allocating display");
        return -1;
    }
    return 0;

fail:
    free(bg32);
    free(b.segments);
    free(b.offset);
    free(b.colour);
    free(b.alpha);
    memset(r, 0, sizeof(*r));
    return -1;
}

int lcd_render_build_gnw(LCDRender *r, const uint8_t *gnw, const LCDRenderOptions *opt, bool rgb565, char *err,
                         size_t err_len)
{
    static uint32_t counts[1024];
    unsigned x0 = GNW_SIDE, y0 = GNW_SIDE, x1 = 0, y1 = 0, x, y, n, id, w, h;
    size_t pixels, total = 0, i;
    uint32_t *bg32 = NULL, start[1024];
    GnwRun run;

    memset(r, 0, sizeof(*r));
    /* crop away the black surround the format pads with */
    for (y = 0; y < GNW_SIDE; ++y)
        for (x = 0; x < GNW_SIDE; ++x)
            if (gnw_pixel(gnw, x, y, false) || gnw_pixel(gnw, x, y, true)) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x + 1 > x1) x1 = x + 1;
                if (y + 1 > y1) y1 = y + 1;
            }
    if (x1 <= x0 || y1 <= y0) {
        set_error(err, err_len, ".gnw file has an empty image");
        return -1;
    }
    w = x1 - x0;
    h = y1 - y0;
    pixels = (size_t)w * h;

    memset(counts, 0, sizeof(counts));
    for (n = 0; gnw_run(gnw, n, &run); ++n)
        counts[run.id] += run.length;
    for (id = 0; id < 1024; ++id) {
        start[id] = (uint32_t)total;
        total += counts[id];
        if (counts[id])
            r->segment_count++;
    }
    bg32 = malloc(pixels * sizeof(uint32_t));
    r->segments = calloc(r->segment_count ? r->segment_count : 1u, sizeof(LCDSegment));
    r->pixel_offset = malloc((total ? total : 1u) * sizeof(uint32_t));
    r->pixel_colour = malloc((total ? total : 1u) * sizeof(uint32_t));
    r->pixel_alpha = malloc(total ? total : 1u);
    r->last_levels = calloc(r->segment_count ? r->segment_count : 1u, 1);
    r->frame = malloc(pixels * (rgb565 ? 2u : 4u));
    if (!bg32 || !r->segments || !r->pixel_offset || !r->pixel_colour || !r->pixel_alpha || !r->last_levels ||
        !r->frame) {
        free(bg32);
        lcd_render_free(r);
        set_error(err, err_len, "Out of memory building .gnw display");
        return -1;
    }
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x)
            bg32[(size_t)y * w + x] = gnw_pixel(gnw, x0 + x, y0 + y, false);

    /* pixels grouped per segment id, in id order */
    {
        uint32_t fill[1024];
        unsigned seg = 0;
        memcpy(fill, start, sizeof(fill));
        for (n = 0; gnw_run(gnw, n, &run); ++n)
            for (x = run.x; x < (unsigned)run.x + run.length; ++x) {
                uint32_t k = fill[run.id]++;
                unsigned px = x < x0 ? 0 : x - x0, py = run.y < y0 ? 0 : run.y - y0;
                if (px >= w) px = w - 1;
                if (py >= h) py = h - 1;
                r->pixel_offset[k] = (uint32_t)(py * w + px);
                r->pixel_colour[k] = gnw_pixel(gnw, x, run.y, true);
                r->pixel_alpha[k] = (uint8_t)(255u * opt->contrast / 100u);
            }
        for (id = 0; id < 1024; ++id) {
            LCDSegment *s;
            uint32_t k;
            if (!counts[id])
                continue;
            s = &r->segments[seg++];
            s->x = (uint8_t)(id >> 6);
            s->y = (uint8_t)(id >> 2 & 0xf);
            s->z = (uint8_t)(id & 3);
            s->first = start[id];
            s->count = counts[id];
            s->x0 = (uint16_t)w;
            s->y0 = (uint16_t)h;
            for (k = s->first; k < s->first + s->count; ++k) {
                unsigned py = r->pixel_offset[k] / w, px = r->pixel_offset[k] % w;
                if (px < s->x0) s->x0 = (uint16_t)px;
                if (py < s->y0) s->y0 = (uint16_t)py;
                if (px + 1 > s->x1) s->x1 = (uint16_t)(px + 1);
                if (py + 1 > s->y1) s->y1 = (uint16_t)(py + 1);
            }
        }
    }
    if (opt->ghost)
        for (i = 0; i < total; ++i) {
            uint32_t *d = &bg32[r->pixel_offset[i]];
            *d = blend32(*d, r->pixel_colour[i], (unsigned)r->pixel_alpha[i] * opt->ghost / 100u);
        }
    r->pixel_count = total;
    r->rgb565 = rgb565;
    r->width = w;
    r->height = h;
    r->fade = opt->fade;
    r->rise = opt->rise ? opt->rise : 255;
    r->fall = opt->fall ? opt->fall : 255;
    if (rgb565) {
        uint16_t *bg16 = malloc(pixels * 2u);
        if (bg16)
            for (i = 0; i < pixels; ++i)
                bg16[i] = to565(bg32[i]);
        free(bg32);
        r->background = bg16;
    } else {
        r->background = bg32;
    }
    if (!r->background) {
        lcd_render_free(r);
        set_error(err, err_len, "Out of memory building .gnw display");
        return -1;
    }
    return 0;
}

static uint32_t blend_px(uint32_t d, uint32_t c, unsigned a) { return a == 255u ? c : blend32(d, c, a); }

static void draw_segment(LCDRender *r, const LCDSegment *seg, unsigned level, unsigned x0, unsigned y0, unsigned x1,
                         unsigned y1)
{
    uint32_t i, end = seg->first + seg->count;
    bool clip = seg->x0 < x0 || seg->y0 < y0 || seg->x1 > x1 || seg->y1 > y1;
    for (i = seg->first; i < end; ++i) {
        uint32_t off = r->pixel_offset[i];
        unsigned a = level == 255u ? r->pixel_alpha[i] : r->pixel_alpha[i] * level / 255u;
        if (clip) {
            unsigned py = off / r->width, px = off - py * r->width;
            if (px < x0 || px >= x1 || py < y0 || py >= y1)
                continue;
        }
        if (r->rgb565) {
            uint16_t *d = (uint16_t *)r->frame + off;
            *d = to565(blend_px(from565(*d), r->pixel_colour[i], a));
        } else {
            uint32_t *d = (uint32_t *)r->frame + off;
            *d = blend_px(*d, r->pixel_colour[i], a);
        }
    }
}

const void *lcd_render_frame(LCDRender *r, const LCDMachine *m, bool *changed)
{
    unsigned i, x0 = r->width, y0 = r->height, x1 = 0, y1 = 0, y;
    size_t bpp = r->rgb565 ? 2u : 4u;
    for (i = 0; i < r->segment_count; ++i) {
        const LCDSegment *seg = &r->segments[i];
        unsigned target, cur = r->last_levels[i];
        if (seg->always)
            continue;
        target = lcd_machine_segment_level(m, seg->x, seg->y, seg->z, r->fade);
        uint8_t level = (uint8_t)(target > cur ? (target - cur > r->rise ? cur + r->rise : target)
                                               : (cur - target > r->fall ? cur - r->fall : target));
        if (level != r->last_levels[i]) {
            r->last_levels[i] = level;
            if (seg->x0 < x0) x0 = seg->x0;
            if (seg->y0 < y0) y0 = seg->y0;
            if (seg->x1 > x1) x1 = seg->x1;
            if (seg->y1 > y1) y1 = seg->y1;
        }
    }
    if (!r->valid_frame) {
        x0 = y0 = 0;
        x1 = r->width;
        y1 = r->height;
    }
    if (changed)
        *changed = x1 > x0;
    if (x1 <= x0 || y1 <= y0)
        return r->frame;

    for (y = y0; y < y1; ++y)
        memcpy((uint8_t *)r->frame + ((size_t)y * r->width + x0) * bpp,
               (const uint8_t *)r->background + ((size_t)y * r->width + x0) * bpp, (x1 - x0) * bpp);
    for (i = 0; i < r->segment_count; ++i) {
        const LCDSegment *seg = &r->segments[i];
        if ((seg->always || r->last_levels[i]) && seg->x0 < x1 && seg->x1 > x0 && seg->y0 < y1 && seg->y1 > y0)
            draw_segment(r, seg, seg->always ? 255u : r->last_levels[i], x0, y0, x1, y1);
    }
    r->valid_frame = true;
    return r->frame;
}
