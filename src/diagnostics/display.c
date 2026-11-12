#include "display.h"
#include <string.h>

#define PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define SEG_SCORE 1u
#define SEG_LIVES 29u
#define SEG_BALL 32u
#define SEG_PLAYER 47u
#define SEG_MAX 49u

static uint8_t segments[PIXELS];
static uint32_t background[PIXELS];
static uint32_t pixels32[PIXELS];
static uint16_t pixels16[PIXELS];

static void base_rect(int x, int y, int w, int h, uint32_t colour)
{
    int xx, yy;
    for (yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= (int)LCD_HEIGHT) continue;
        for (xx = x; xx < x + w; ++xx) {
            if (xx >= 0 && xx < (int)LCD_WIDTH)
                background[(unsigned)yy * LCD_WIDTH + (unsigned)xx] = colour;
        }
    }
}

static void seg_rect(int x, int y, int w, int h, unsigned id)
{
    int xx, yy;
    if (id == 0 || id > SEG_MAX) return;
    for (yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= (int)LCD_HEIGHT) continue;
        for (xx = x; xx < x + w; ++xx) {
            if (xx >= 0 && xx < (int)LCD_WIDTH)
                segments[(unsigned)yy * LCD_WIDTH + (unsigned)xx] = (uint8_t)id;
        }
    }
}

static void seg_ellipse(int x, int y, int rx, int ry, unsigned id)
{
    int dx, dy;
    for (dy = -ry; dy <= ry; ++dy)
        for (dx = -rx; dx <= rx; ++dx)
            if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry)
                seg_rect(x + dx, y + dy, 1, 1, id);
}

/* Original tiny 5x7 font; only the glyphs required by this prototype. */
static const uint8_t *glyph(char c)
{
    static const uint8_t chars[][7] = {
        {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, /* A */
        {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, /* B */
        {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, /* C */
        {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}, /* D */
        {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, /* E */
        {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}, /* F */
        {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f}, /* G */
        {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}, /* H */
        {0x1f,0x04,0x04,0x04,0x04,0x04,0x1f}, /* I */
        {0x07,0x02,0x02,0x02,0x12,0x12,0x0c}, /* J */
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1f}, /* L */
        {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, /* M */
        {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
        {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, /* O */
        {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}, /* P */
        {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, /* Q */
        {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}, /* R */
        {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}, /* S */
        {0x1f,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, /* U */
        {0x11,0x11,0x11,0x11,0x11,0x0a,0x04}, /* V */
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, /* W */
        {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}, /* X */
        {0x11,0x11,0x0a,0x04,0x04,0x04,0x04}, /* Y */
        {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}, /* Z */
        {0,0,0,0,0,0,0},                         /* space */
        {0,0,0,0x1f,0,0,0},                     /* dash */
        {0,0x04,0,0,0,0x04,0},                  /* colon */
        {0,0,0,0,0,0x04,0x04},                  /* period */
        {0x02,0x02,0x04,0x04,0x08,0x08,0},      /* slash */
        {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}  /* zero */
    };
    if (c >= 'A' && c <= 'Z') return chars[c - 'A'];
    if (c == ' ') return chars[26];
    if (c == '-') return chars[27];
    if (c == ':') return chars[28];
    if (c == '.') return chars[29];
    if (c == '/') return chars[30];
    if (c == '0') return chars[31];
    return chars[26];
}

static void text_background(int x, int y, const char *s, uint32_t colour)
{
    unsigned row, col;
    while (*s) {
        const uint8_t *p = glyph(*s++);
        for (row = 0; row < 7u; ++row)
            for (col = 0; col < 5u; ++col)
                if (p[row] & (1u << (4u - col)))
                    base_rect(x + (int)col, y + (int)row, 1, 1, colour);
        x += 6;
    }
}

static void text_frame(int x, int y, const char *s, uint32_t colour)
{
    unsigned row, col;
    while (*s) {
        const uint8_t *p = glyph(*s++);
        for (row = 0; row < 7u; ++row) {
            int yy = y + (int)row;
            if (yy < 0 || yy >= (int)LCD_HEIGHT) continue;
            for (col = 0; col < 5u; ++col) {
                int xx = x + (int)col;
                if ((p[row] & (1u << (4u - col))) && xx >= 0 && xx < (int)LCD_WIDTH)
                    pixels32[(unsigned)yy * LCD_WIDTH + (unsigned)xx] = colour;
            }
        }
        x += 6;
    }
}

static void digit_segments(int x, int y, unsigned first)
{
    seg_rect(x + 3, y, 10, 3, first + 0u);  /* top */
    seg_rect(x + 13, y + 3, 3, 9, first + 1u); /* upper-right */
    seg_rect(x + 13, y + 14, 3, 9, first + 2u); /* lower-right */
    seg_rect(x + 3, y + 23, 10, 3, first + 3u); /* bottom */
    seg_rect(x, y + 14, 3, 9, first + 4u);  /* lower-left */
    seg_rect(x, y + 3, 3, 9, first + 5u);  /* upper-left */
    seg_rect(x + 3, y + 12, 10, 3, first + 6u); /* middle */
}

void lcd_display_init(void)
{
    unsigned lane, row;
    const int lane_x[3] = {80,160,240};
    memset(segments, 0, sizeof(segments));
    for (row = 0; row < PIXELS; ++row)
        background[row] = UINT32_C(0x00AABFA0);

    base_rect(0, 0, 320, 3, 0x002D493B);
    base_rect(0, 237, 320, 3, 0x002D493B);
    base_rect(0, 0, 3, 240, 0x002D493B);
    base_rect(317, 0, 3, 240, 0x002D493B);
    base_rect(8, 7, 304, 53, 0x0091A887);
    base_rect(8, 62, 304, 151, 0x0095AB8A);
    base_rect(12, 66, 296, 143, 0x00B8CCAA);
    base_rect(12, 214, 296, 1, 0x00677B64);
    base_rect(16, 226, 288, 1, 0x00839779);
    text_background(17, 14, "LCD LAB", 0x002D493B);
    text_background(17, 26, "CATCH AND CARRY", 0x003E5C49);
    text_background(154, 14, "SCORE", 0x003E5C49);
    text_background(273, 14, "LIVES", 0x003E5C49);
    text_background(19, 219, "LEFT RIGHT   START   SELECT PAUSE", 0x003D5947);
    for (lane = 1; lane < 3u; ++lane) {
        base_rect((int)(lane * 80u + 40u), 72, 1, 123, 0x00A2B797);
    }
    for (row = 0; row < 4u; ++row)
        base_rect(36, 99 + (int)row * 24, 248, 1, 0x00ACC2A0);

    for (lane = 0; lane < 4u; ++lane)
        digit_segments(154 + (int)lane * 23, 29, SEG_SCORE + lane * 7u);
    for (lane = 0; lane < 3u; ++lane) {
        seg_ellipse(278 + (int)lane * 10, 34, 4, 4, SEG_LIVES + lane);
        seg_rect(277 + (int)lane * 10, 35, 3, 6, SEG_LIVES + lane);
    }
    for (lane = 0; lane < LCD_LANES; ++lane) {
        for (row = 0; row < LCD_ROWS; ++row) {
            int y = 80 + (int)row * 24;
            unsigned id = SEG_BALL + lane * LCD_ROWS + row;
            seg_ellipse(lane_x[lane], y, 9, 8, id);
        }
        {
            unsigned id = SEG_PLAYER + lane;
            int x = lane_x[lane];
            seg_ellipse(x, 191, 5, 5, id);
            seg_rect(x - 6, 197, 12, 8, id);
            seg_rect(x - 10, 199, 20, 3, id);
            seg_rect(x - 6, 204, 4, 4, id);
            seg_rect(x + 2, 204, 4, 4, id);
        }
    }
}

static uint64_t active_segments(const LCDGame *g)
{
    static const uint8_t digit_pattern[10] = {
        0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f
    };
    uint64_t active = 0;
    unsigned i, j;
    uint32_t score = g->score % 10000u;
    for (i = 0; i < 4u; ++i) {
        uint8_t pattern = digit_pattern[score % 10u];
        score /= 10u;
        for (j = 0; j < 7u; ++j)
            if (pattern & (1u << j))
                active |= UINT64_C(1) << (SEG_SCORE + (3u - i) * 7u + j);
    }
    for (i = 0; i < 3u - g->misses && i < 3u; ++i)
        active |= UINT64_C(1) << (SEG_LIVES + i);
    for (i = 0; i < LCD_LANES; ++i)
        for (j = 0; j < LCD_ROWS; ++j)
            if (g->balls[i][j])
                active |= UINT64_C(1) << (SEG_BALL + i * LCD_ROWS + j);
    active |= UINT64_C(1) << (SEG_PLAYER + g->lane);
    return active;
}

const void *lcd_display_render(const LCDGame *g, const LCDOptions *o, bool rgb565)
{
    unsigned i;
    uint64_t active = active_segments(g);
    uint32_t on = o->contrast == 2 ? 0x00172E24 :
                  o->contrast == 0 ? 0x00566E58 : 0x002D493B;
    uint32_t off = o->ghost ? 0x009CA993 : 0x00B8CCAA;
    for (i = 0; i < PIXELS; ++i) {
        uint8_t id = segments[i];
        pixels32[i] = id ? ((active & (UINT64_C(1) << id)) ? on : off)
                         : background[i];
    }
    if (g->mode == LCD_READY)
        text_frame(22, 45, "PRESS START", on);
    else if (g->mode == LCD_FINISHED)
        text_frame(22, 45, "GAME OVER - START", on);
    else if (g->paused)
        text_frame(22, 45, "PAUSED", on);
    else
        text_frame(22, 45, "CATCH EACH DROP", on);

    if (!rgb565)
        return pixels32;
    for (i = 0; i < PIXELS; ++i) {
        uint32_t v = pixels32[i];
        pixels16[i] = (uint16_t)(((v >> 8) & 0xf800u) |
                                 ((v >> 5) & 0x07e0u) |
                                 ((v >> 3) & 0x001fu));
    }
    return pixels16;
}
