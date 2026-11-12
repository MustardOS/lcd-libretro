/* Segment renderer: SVG key extraction, layout fitting, single/dual screens,
   XRGB8888/RGB565 output, unchanged frame detection, draw order and .gnw files. */
#include "content/loader.h"
#include "renderer/lcd_render.h"
#include "util/png.h"
#include "util/zip.h"
#include "test_support.h"

static LCDContent content;
static LCDMachine m;

static LCDRenderOptions defaults(void)
{
    LCDRenderOptions o;
    o.box_w = 640;
    o.box_h = 480;
    o.gap = 40;
    o.contrast = 100;
    o.ghost = 0;
    o.tint = 0xb3b5b8;
    o.fade = false;
    return o;
}

static void test_single(void)
{
    LCDRender r;
    LCDSvgSource src = {content.svg[0], content.svg_size[0]};
    LCDRenderOptions o = defaults();
    char err[128];
    bool changed;
    const uint32_t *frame;
    size_t i, differing = 0;

    CHECK_EQ(lcd_render_build(&r, content.def, &src, 1, &o, false, err, sizeof(err)), 0);
    /* 720x640 fitted into 640x480 */
    CHECK_EQ(r.width, 540);
    CHECK_EQ(r.height, 480);
    CHECK_EQ(r.segment_count, 72);
    CHECK(r.pixel_count > 72u * 500u);

    /* nothing lit: the frame is the background */
    frame = lcd_render_frame(&r, &m, &changed);
    CHECK(changed);
    CHECK(memcmp(frame, r.background, (size_t)r.width * r.height * 4u) == 0);
    /* the corner shows the white SVG background multiplied by the MAME tint */
    CHECK_EQ(frame[(size_t)r.width * 60 + 60] & 0xffffff, 0xb3b5b8); /* white gap between cells x MAME tint */
    lcd_render_frame(&r, &m, &changed);
    CHECK(!changed);

    /* run the self-test: segments light and the frame changes */
    lcd_machine_run(&m, LCD_OSC_HZ * 2, NULL);
    frame = lcd_render_frame(&r, &m, &changed);
    CHECK(changed);
    for (i = 0; i < (size_t)r.width * r.height; ++i)
        differing += frame[i] != ((const uint32_t *)r.background)[i];
    CHECK(differing > 1000);
    lcd_render_free(&r);

    /* RGB565 output and options */
    o.ghost = 15;
    o.contrast = 60;
    o.box_w = 320;
    o.box_h = 240;
    CHECK_EQ(lcd_render_build(&r, content.def, &src, 1, &o, true, err, sizeof(err)), 0);
    CHECK_EQ(r.width, 270);
    CHECK_EQ(r.height, 240);
    CHECK(r.rgb565);
    lcd_render_frame(&r, &m, &changed);
    CHECK(changed);
    lcd_render_free(&r);
}

static void test_layouts(void)
{
    LCDMachineDef dual = *content.def;
    LCDSvgSource src[2] = {{content.svg[0], content.svg_size[0]}, {content.svg[0], content.svg_size[0]}};
    LCDRenderOptions o = defaults();
    LCDRender r;
    unsigned w, h;
    char err[128];

    dual.screen_count = 2;
    dual.screens[1] = dual.screens[0];
    dual.layout = LCD_LAYOUT_DUAL_VERTICAL;
    lcd_render_layout_size(&dual, 40, &w, &h);
    CHECK_EQ(w, 720);
    CHECK_EQ(h, 640 + 40 + 640);
    CHECK_EQ(lcd_render_build(&r, &dual, src, 2, &o, false, err, sizeof(err)), 0);
    CHECK_EQ(r.height, 480);
    CHECK_EQ(r.segment_count, 144);
    lcd_render_free(&r);

    dual.layout = LCD_LAYOUT_DUAL_HORIZONTAL;
    o.gap = 0;
    lcd_render_layout_size(&dual, 0, &w, &h);
    CHECK_EQ(w, 1440);
    CHECK_EQ(h, 640);
    CHECK_EQ(lcd_render_build(&r, &dual, src, 2, &o, false, err, sizeof(err)), 0);
    CHECK_EQ(r.width, 640);
    CHECK_EQ(r.height, 284);
    lcd_render_free(&r);

    /* wrong screen count and broken SVG are reported */
    CHECK(lcd_render_build(&r, &dual, src, 1, &o, false, err, sizeof(err)) != 0);
    {
        LCDSvgSource bad = {"<svg></svg>", 11};
        CHECK(lcd_render_build(&r, content.def, &bad, 1, &o, false, err, sizeof(err)) != 0);
        CHECK(strlen(err) > 0);
    }
    {
        static const char plain[] =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><rect width=\"5\" height=\"5\"/></svg>";
        LCDSvgSource nokeys = {plain, sizeof(plain) - 1};
        CHECK(lcd_render_build(&r, content.def, &nokeys, 1, &o, false, err, sizeof(err)) != 0);
        CHECK(strstr(err, "segment") != NULL);
    }
}

static int near(uint32_t px, unsigned r, unsigned g, unsigned b, unsigned tol)
{
    int dr = (int)(px >> 16 & 0xff) - (int)r, dg = (int)(px >> 8 & 0xff) - (int)g, db = (int)(px & 0xff) - (int)b;
    return abs(dr) <= (int)tol && abs(dg) <= (int)tol && abs(db) <= (int)tol;
}

static void test_background(void)
{
    LCDContent bc;
    LCDRender r;
    LCDRenderOptions o = defaults();
    LCDSvgSource src;
    char err[128];
    const uint32_t *f;

    CHECK_EQ(lcd_content_load(&bc, "tests/fixtures/selftest_background.zip", NULL, 0), 0);
    CHECK(bc.background != NULL);
    CHECK_EQ(bc.background_w, 240);
    CHECK_EQ(bc.background_h, 208);
    if (!bc.background)
        return;
    /* straight RGBA from the decoder: frame colour and the ramp */
    CHECK(bc.background[0] == 60 && bc.background[1] == 40 && bc.background[2] == 30 && bc.background[3] == 255);
    CHECK(bc.background[(26 * 240 + 26) * 4] == 220 - 26 / 4);
    CHECK(bc.background[(26 * 240 + 26) * 4 + 2] == 150 + 26 / 8);

    src.data = bc.svg[0];
    src.size = bc.svg_size[0];
    o.background = bc.background;
    o.background_w = bc.background_w;
    o.background_h = bc.background_h;
    CHECK_EQ(lcd_render_build(&r, bc.def, &src, 1, &o, false, err, sizeof(err)), 0);
    f = lcd_render_frame(&r, &m, NULL);
    /* white LCD x image: the image shows through and the tint is not applied */
    CHECK(near(f[0], 60, 40, 30, 0)); /* corner: outside the SVG border stroke */
    CHECK(near(f[60 * 540 + 60], 214, 200, 153, 2));
    lcd_render_free(&r);
    lcd_content_free(&bc);
}

static int find_png(const LCDZipEntry *e, void *user)
{
    if (!strcmp(e->name, "background.png")) {
        *(LCDZipEntry *)user = *e;
        return 1;
    }
    return 0;
}

static void test_png(void)
{
    size_t size = 0;
    uint8_t *zip = test_read_file("tests/fixtures/selftest_background.zip", &size), *png, *px = NULL;
    LCDZipEntry e;
    unsigned w = 0, h = 0, y, ok = 1;
    memset(&e, 0, sizeof(e));
    CHECK(zip && lcd_zip_each(zip, size, find_png, &e) == 0 && e.size > 0);
    if (!zip || !e.size) {
        free(zip);
        return;
    }
    png = malloc(e.size);
    CHECK(png && lcd_zip_extract(zip, size, &e, png, e.size) == 0);
    /* rows cycle through all five filter types */
    CHECK_EQ(lcd_png_decode(png, e.size, &w, &h, &px), 0);
    for (y = 8; y < 200 && px; ++y)
        ok &= px[(y * 240 + 100) * 4] == 220 - 100 / 4 && px[(y * 240 + 100) * 4 + 2] == 150 + y / 8;
    CHECK(ok);
    free(px);
    CHECK(lcd_png_decode(png, e.size / 2, &w, &h, &px) != 0);
    png[28] = 1; /* interlace method */
    CHECK(lcd_png_decode(png, e.size, &w, &h, &px) != 0);
    CHECK(lcd_png_decode((const uint8_t *)"not a png at all, sorry", 23, &w, &h, &px) != 0);
    free(png);
    free(zip);
}

static void test_gnw(void)
{
    LCDContent g;
    LCDMachine gm;
    LCDRender r;
    LCDRenderOptions o = defaults();
    char err[128];
    const uint32_t *f;
    unsigned i, lit = 0;

    /* program in the registry: the MAME derived definition is used, graphics come from the file */
    CHECK_EQ(lcd_content_load(&g, LCD_FIXTURES "/selftest.gnw", NULL, 0), 0);
    CHECK(g.def && !strcmp(g.def->id, "lcdlab_selftest"));
    CHECK(g.gnw != NULL);
    CHECK_EQ(lcd_render_build_gnw(&r, g.gnw, &o, false, err, sizeof(err)), 0);
    CHECK_EQ(r.width, 720);
    CHECK_EQ(r.height, 720);
    CHECK_EQ(r.segment_count, 72);
    CHECK_EQ(r.pixel_count, 72u * 56u * 56u);
    CHECK_EQ(lcd_machine_init(&gm, g.def, g.rom, g.rom_size), 0);
    lcd_machine_run(&gm, LCD_OSC_HZ * 2, NULL);
    f = lcd_render_frame(&r, &gm, NULL);
    CHECK_EQ(f[0] & 0xffffff, 0xc8c8b4);
    for (i = 0; i < r.segment_count; ++i)
        lit += r.last_levels[i] != 0;
    CHECK(lit > 10);
    /* a lit segment shows the mask colour */
    for (i = 0; i < r.segment_count; ++i)
        if (r.last_levels[i]) {
            CHECK_EQ(f[r.pixel_offset[r.segments[i].first]] & 0xffffff, 0x101010);
            break;
        }
    lcd_render_free(&r);
    lcd_content_free(&g);

    /* unknown program: the machine comes from the .gnw header */
    CHECK_EQ(lcd_content_load(&g, LCD_FIXTURES "/unknown.gnw", NULL, 0), 0);
    CHECK(g.def == &g.gnw_def && g.def->cpu == SM5XX_SM5A);
    CHECK_EQ(g.def->inp_fixed, 0);
    CHECK_EQ(g.def->inp_lines, 0);
    CHECK_EQ(g.def->b_rest, 1);
    CHECK(g.def->inputs[0].label && !strcmp(g.def->inputs[0].label, "Time"));
    CHECK_EQ(lcd_machine_init(&gm, g.def, g.rom, g.rom_size), 0);
    lcd_machine_run(&gm, LCD_OSC_HZ * 3, NULL);
    CHECK(sm5xx_ram_read(&gm.cpu, 0x00) >= 2); /* the self-test counter runs */
    lcd_content_free(&g);

    /* not a .gnw: wrong version byte */
    {
        size_t size = 0;
        uint8_t *d = test_read_file(LCD_FIXTURES "/selftest.gnw", &size);
        CHECK(d != NULL);
        if (d) {
            d[0] = 2;
            CHECK(lcd_content_load(&g, NULL, d, size) != 0);
            free(d);
        }
    }
}

static void test_condition(void)
{
    LCDRender r;
    LCDRenderOptions o = defaults();
    LCDSvgSource src = {content.svg[0], content.svg_size[0]};
    static LCDMachine t;
    char err[128];
    unsigned i, seg = 0, frames = 0;
    o.rise = 40;
    o.fall = 10;
    CHECK_EQ(lcd_render_build(&r, content.def, &src, 1, &o, false, err, sizeof(err)), 0);
    lcd_machine_clone(&t, &m);
    lcd_machine_run(&t, LCD_OSC_HZ * 2, NULL);
    for (i = 0; i < 10; ++i)
        lcd_render_frame(&r, &t, NULL);
    for (i = 0; i < r.segment_count; ++i)
        if (r.last_levels[i] == 255) {
            seg = i;
            break;
        }
    CHECK_EQ(r.last_levels[seg], 255);
    /* blank the LCD: the slow panel fades instead of switching off */
    t.cpu.bp = 0;
    lcd_machine_run(&t, LCD_OSC_HZ, NULL);
    lcd_render_frame(&r, &t, NULL);
    CHECK_EQ(r.last_levels[seg], 245);
    while (r.last_levels[seg] && frames < 100) {
        lcd_render_frame(&r, &t, NULL);
        frames++;
    }
    CHECK_EQ(frames, 25); /* 255 / 10 per frame */
    lcd_render_free(&r);
}

/* MAME draws the SVG in document order: static shapes after a segment cover it */
static void test_order(void)
{
    static const char svg[] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">"
        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#ffffff\"/>"
        "<rect x=\"10\" y=\"10\" width=\"40\" height=\"40\" fill=\"#000000\"><title>0.0.0</title></rect>"
        "<rect x=\"20\" y=\"20\" width=\"10\" height=\"10\" fill=\"#ff0000\"/></svg>";
    LCDMachineDef def = *content.def;
    LCDSvgSource src = {svg, sizeof(svg) - 1};
    LCDRenderOptions o = defaults();
    LCDRender r;
    const uint32_t *px;
    char err[128];
    unsigned i, overlays = 0;
    def.screens[0].width = 100;
    def.screens[0].height = 100;
    o.box_w = o.box_h = 100;
    o.tint = 0xffffff;
    CHECK_EQ(lcd_render_build(&r, &def, &src, 1, &o, false, err, sizeof(err)), 0);
    for (i = 0; i < r.segment_count; ++i)
        overlays += r.segments[i].always;
    CHECK_EQ(r.segment_count, 2);
    CHECK_EQ(overlays, 1);
    lcd_machine_power_on(&m);
    m.display_decay[0][0] = (uint8_t)(m.decay_pivot + 1u);
    px = lcd_render_frame(&r, &m, NULL);
    CHECK_EQ(px[15 * r.width + 15] & 0xffffff, 0x000000);
    CHECK_EQ(px[25 * r.width + 25] & 0xffffff, 0xff0000);
    CHECK_EQ(px[5 * r.width + 5] & 0xffffff, 0xffffff);
    m.display_decay[0][0] = 0;
    px = lcd_render_frame(&r, &m, NULL);
    CHECK_EQ(px[15 * r.width + 15] & 0xffffff, 0xffffff);
    CHECK_EQ(px[25 * r.width + 25] & 0xffffff, 0xff0000);
    lcd_render_free(&r);

    /* built-in LED layout (elbaskb.lay): 26 lamps and 4 digits of 7 segments, no overlays */
    {
        const LCDMachineDef *bb = lcd_registry_find_id("elbaskb");
        LCDSvgSource led;
        CHECK(bb && bb->builtin_svg && (bb->quirks & LCD_QUIRK_LED));
        if (!bb || !bb->builtin_svg)
            return;
        led.data = bb->builtin_svg;
        led.size = strlen(bb->builtin_svg);
        o = defaults();
        CHECK_EQ(lcd_render_build(&r, bb, &led, 1, &o, false, err, sizeof(err)), 0);
        overlays = 0;
        for (i = 0; i < r.segment_count; ++i)
            overlays += r.segments[i].always;
        CHECK_EQ(r.segment_count, 54);
        CHECK_EQ(overlays, 0);
        lcd_render_free(&r);
    }
}

int main(void)
{
    CHECK_EQ(lcd_content_load(&content, "roms/lcdlab_selftest.zip", NULL, 0), 0);
    CHECK_EQ(lcd_machine_init(&m, content.def, content.rom, content.rom_size), 0);
    if (test_failures)
        return test_finish("render_test");
    test_single();
    test_layouts();
    test_background();
    test_png();
    test_gnw();
    test_condition();
    test_order();
    lcd_content_free(&content);
    return test_finish("render_test (SVG keys, layouts, pixel formats, dirty tracking, background.png, PNG, .gnw, LCD condition, draw order, LED layout)");
}
