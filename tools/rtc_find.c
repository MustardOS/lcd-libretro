/* Finds where a game keeps its clock and prints a verified layout for src/content/rtc_layouts.inc.
   Build: make tools (build/rtc_find). Usage: rtc_find set.zip [set.zip ...]

   1. Boot like the core does (power on, press Time) and sample RAM once a minute for 70 minutes:
      nibbles counting 0-9 each minute are minute units, the one carrying from them minute tens.
   2. Write 59 into each minute pair and let the hour roll over 24 times: the nibbles that follow
      the hours give the hour digits and the AM/PM encoding.
   3. Write test times with the layout and check the program keeps them: 3:47 PM ticks to 3:48,
      9:59 to 10:00, 11:59 AM to 12:00 PM, 11:59 PM to 12:00 AM, 12:59 to 1:00, and two
      different times give different LCD contents. Only layouts passing every check are printed. */
#include "content/loader.h"
#include "hardware/machine.h"
#include "libretro.h"
#include <stdio.h>
#include <string.h>

#define MINUTES 70
#define ROLLS 24

static LCDContent c;
static LCDMachine booted, m;

static void run_seconds(LCDMachine *mm, unsigned s)
{
    while (s--)
        lcd_machine_run(mm, LCD_OSC_HZ, NULL);
}

static uint8_t rd(const LCDMachine *mm, unsigned a) { return sm5xx_ram_read(&mm->cpu, (uint8_t)a); }

static int boot(void)
{
    if (lcd_machine_init(&booted, c.def, c.rom, c.rom_size) != 0)
        return -1;
    lcd_machine_attach_melody(&booted, c.melody, c.melody_size);
    return lcd_machine_boot_to_clock(&booted) ? 0 : -1;
}

/* reads a layout back as hour (0-23) and minute; -1 when the nibbles are not a valid time.
   The hour tens nibble is base | digit | (pm ? pm : am), as lcd_rtc_write encodes it. */
static int decode(const LCDMachine *mm, const LCDRtcDef *r, unsigned *hour, unsigned *minute)
{
    unsigned h10 = rd(mm, r->h10), h1 = rd(mm, r->h1), m10 = rd(mm, r->m10), m1 = rd(mm, r->m1), p, d;
    if (m10 > 5 || m1 > 9 || h1 > 9)
        return -1;
    for (p = 0; p < 2; ++p)
        for (d = 0; d < 3; ++d) {
            unsigned h = d * 10u + h1, pm = p;
            if ((unsigned)(r->base | d | (p ? r->pm : r->am)) != h10)
                continue;
            if (r->ap != 0xffu)
                pm = rd(mm, r->ap) == r->ap_pm;
            if (r->h24) {
                if (h > 23)
                    continue;
                *hour = h;
            } else {
                if (h < 1 || h > 12)
                    continue;
                *hour = (h % 12u) + (pm ? 12u : 0u);
            }
            *minute = m10 * 10u + m1;
            return 0;
        }
    return -1;
}

/* writes a time, runs until the minute changes (at most 61 s) and returns the decoded result */
static int step(const LCDRtcDef *r, unsigned hour, unsigned minute, unsigned *oh, unsigned *om)
{
    unsigned s;
    lcd_machine_clone(&m, &booted);
    lcd_rtc_write(&m.cpu, r, hour, minute);
    for (s = 0; s < 61; ++s) {
        run_seconds(&m, 1);
        if (rd(&m, r->m1) != minute % 10u)
            break;
    }
    run_seconds(&m, 1);
    return decode(&m, r, oh, om);
}

static int verify(const LCDRtcDef *r)
{
    static const unsigned tests[][4] = {{15, 47, 15, 48}, {9, 59, 10, 0}, {11, 59, 12, 0}, {23, 59, 0, 0},
                                        {12, 59, 13, 0}, {0, 59, 1, 0}, {21, 59, 22, 0}};
    unsigned i, h, mi;
    uint32_t a[LCD_ZX], b[LCD_ZX];
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        unsigned want_h = tests[i][2];
        if (!r->h24 && r->am == r->pm && r->ap == 0xffu)
            want_h %= 12u; /* no AM/PM kept: compare on a 12 hour dial */
        if (step(r, tests[i][0], tests[i][1], &h, &mi) != 0)
            return 0;
        if (!r->h24 && r->am == r->pm && r->ap == 0xffu)
            h %= 12u;
        if (h != want_h || mi != tests[i][3])
            return 0;
    }
    lcd_machine_clone(&m, &booted);
    lcd_rtc_write(&m.cpu, r, 15, 47);
    run_seconds(&m, 2);
    memcpy(a, m.display_state, sizeof(a));
    lcd_machine_clone(&m, &booted);
    lcd_rtc_write(&m.cpu, r, 8, 12);
    run_seconds(&m, 2);
    memcpy(b, m.display_state, sizeof(b));
    return memcmp(a, b, sizeof(a)) != 0;
}

static int find(const char *path)
{
    static uint8_t mins[MINUTES][128], rolls[ROLLS][128];
    unsigned k, a, b, i, found = 0;
    if (lcd_content_load(&c, path, NULL, 0) != 0) {
        fprintf(stderr, "%s: %s\n", path, c.message);
        return 0;
    }
    if (boot() != 0) {
        fprintf(stderr, "%s: no Time button\n", c.def->id);
        lcd_content_free(&c);
        return 0;
    }
    lcd_machine_clone(&m, &booted);
    run_seconds(&m, 30);
    for (k = 0; k < MINUTES; ++k) {
        for (a = 0; a < 128; ++a)
            mins[k][a] = rd(&m, a);
        run_seconds(&m, 60);
    }
    for (a = 0; a < 128 && !found; ++a) {
        for (k = 1; k < MINUTES && mins[k][a] == (mins[0][a] + k) % 10u; ++k)
            ;
        if (k < MINUTES)
            continue;
        for (b = 0; b < 128 && !found; ++b) {
            unsigned h, x, base_h24;
            LCDRtcDef r;
            for (k = 0; k < MINUTES && mins[k][b] == (mins[0][b] + (mins[0][a] + k) / 10u) % 6u; ++k)
                ;
            if (b == a || k < MINUTES)
                continue;
            /* hour rollovers with this minute pair */
            memset(&r, 0, sizeof(r));
            r.m10 = (uint8_t)b;
            r.m1 = (uint8_t)a;
            lcd_machine_clone(&m, &booted);
            for (i = 0; i < ROLLS; ++i) {
                unsigned s;
                sm5xx_ram_write(&m.cpu, r.m10, 5);
                sm5xx_ram_write(&m.cpu, r.m1, 9);
                for (s = 0; s < 62 && rd(&m, a) == 9; ++s)
                    run_seconds(&m, 1);
                run_seconds(&m, 1);
                for (x = 0; x < 128; ++x)
                    rolls[i][x] = rd(&m, x);
            }
            /* try every starting hour for a 12 or 24 hour clock */
            for (base_h24 = 0; base_h24 < 24 && !found; ++base_h24)
                for (h = 0; h < 2 && !found; ++h) {
                    unsigned h1a, h10a;
                    for (h1a = 0; h1a < 128 && !found; ++h1a) {
                        for (i = 0; i < ROLLS; ++i) {
                            unsigned hr = (base_h24 + 1u + i) % 24u, v = h ? hr : (hr % 12u ? hr % 12u : 12u);
                            if (rolls[i][h1a] != v % 10u)
                                break;
                        }
                        if (i < ROLLS || h1a == a || h1a == b)
                            continue;
                        for (h10a = 0; h10a < 128 && !found; ++h10a) {
                            int am = -1, pm = -1, ok = 1;
                            if (h10a == a || h10a == b || h10a == h1a)
                                continue;
                            for (i = 0; i < ROLLS && ok; ++i) {
                                unsigned hr = (base_h24 + 1u + i) % 24u, v = h ? hr : (hr % 12u ? hr % 12u : 12u);
                                int rest = (int)rolls[i][h10a] - (int)(v / 10u), *slot = hr >= 12 ? &pm : &am;
                                if (rest < 0 || (rest & (int)(v / 10u)))
                                    ok = 0;
                                else if (*slot < 0)
                                    *slot = rest;
                                else if (*slot != rest)
                                    ok = 0;
                            }
                            if (!ok)
                                continue;
                            r.present = 1;
                            r.h1 = (uint8_t)h1a;
                            r.h10 = (uint8_t)h10a;
                            r.h24 = (uint8_t)h;
                            r.base = 0;
                            r.am = (uint8_t)am;
                            r.pm = (uint8_t)pm;
                            r.ap = 0xff;
                            if (!h && am == pm) {
                                /* AM/PM in a nibble of its own: constant within each half day */
                                unsigned ap;
                                for (ap = 0; ap < 128 && !found; ++ap) {
                                    int va = -1, vp = -1;
                                    for (i = 0; i < ROLLS; ++i) {
                                        unsigned hr = (base_h24 + 1u + i) % 24u;
                                        int *slot = hr >= 12 ? &vp : &va;
                                        if (*slot < 0)
                                            *slot = rolls[i][ap];
                                        else if (*slot != rolls[i][ap])
                                            break;
                                    }
                                    if (i < ROLLS || va == vp || ap == a || ap == b || ap == h1a || ap == h10a)
                                        continue;
                                    r.ap = (uint8_t)ap;
                                    r.ap_am = (uint8_t)va;
                                    r.ap_pm = (uint8_t)vp;
                                    r.am = r.pm = 0;
                                    r.base = (uint8_t)am;
                                    if (verify(&r))
                                        found = 1;
                                }
                                if (!found) {
                                    r.ap = 0xff;
                                    r.am = r.pm = 0;
                                    r.base = (uint8_t)am;
                                }
                            }
                            if (!found && verify(&r))
                                found = 1;
                        }
                    }
                }
            if (found)
                printf("{\"%s\", {1, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%x, 0x%x, 0x%x, 0x%02x, %u, %u, %u}},\n",
                       c.def->id, r.h10, r.h1, r.m10, r.m1, r.base, r.am, r.pm, r.ap, r.ap_am, r.ap_pm, r.h24);
        }
    }
    if (!found)
        fprintf(stderr, "%s: no verified clock layout\n", c.def->id);
    fflush(stdout);
    lcd_content_free(&c);
    return (int)found;
}

int main(int argc, char **argv)
{
    int i, n = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: %s set.zip [set.zip ...]\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; ++i)
        n += find(argv[i]);
    fprintf(stderr, "%d of %d sets have a verified clock layout\n", n, argc - 1);
    return 0;
}
