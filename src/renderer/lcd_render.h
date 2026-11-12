#ifndef LCD_RENDERER_LCD_RENDER_H
#define LCD_RENDERER_LCD_RENDER_H

#include "../content/registry.h"
#include "../hardware/machine.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned box_w, box_h;   /* output is fitted inside this box */
    unsigned gap;            /* dual screen gap in MAME layout units (40 upstream) */
    uint8_t contrast;        /* 0..100, segment opacity */
    uint8_t ghost;           /* 0..100, inactive segment visibility */
    uint32_t tint;           /* 0xRRGGBB multiplied over the LCD, MAME uses b3b5b8 */
    bool fade;               /* use the decay counter for gradual segment response */
    uint8_t rise, fall;      /* LCD response per frame (255 instant): slow panels leave trails */
    const uint8_t *background; /* optional straight RGBA image multiplied under the LCD, replaces the tint */
    unsigned background_w, background_h;
} LCDRenderOptions;

typedef struct {
    const char *data;
    size_t size;
} LCDSvgSource;

typedef struct {
    uint32_t first, count;
    uint16_t x0, y0, x1, y1; /* pixel bounds in the frame, x1/y1 exclusive */
    uint8_t x, y, z;
    bool always; /* static artwork above earlier segments, drawn every time its area is redrawn */
} LCDSegment;

typedef struct {
    bool rgb565;
    unsigned width, height;
    void *frame;
    void *background;
    LCDSegment *segments;
    unsigned segment_count;
    uint32_t *pixel_offset;
    uint32_t *pixel_colour; /* XRGB8888 after tint */
    uint8_t *pixel_alpha;
    size_t pixel_count;
    uint8_t *last_levels;
    bool fade;
    uint8_t rise, fall;
    bool valid_frame;
} LCDRender;

/* rasterises the SVG screens once; nothing is parsed during rendering */
int lcd_render_build(LCDRender *r, const LCDMachineDef *def, const LCDSvgSource *svgs, unsigned svg_count,
                     const LCDRenderOptions *opt, bool rgb565, char *err, size_t err_len);
/* builds from a Game & Watch FPGA .gnw file: prepared 720x720 images, cropped to the artwork */
int lcd_render_build_gnw(LCDRender *r, const uint8_t *gnw, const LCDRenderOptions *opt, bool rgb565, char *err,
                         size_t err_len);
void lcd_render_free(LCDRender *r);
/* returns the frame; *changed is false when the previous frame is still valid */
const void *lcd_render_frame(LCDRender *r, const LCDMachine *m, bool *changed);
/* layout size before fitting, in MAME units */
void lcd_render_layout_size(const LCDMachineDef *def, unsigned gap, unsigned *w, unsigned *h);

#endif
