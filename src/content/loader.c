#include "loader.h"
#include "../util/file.h"
#include "../util/hash.h"
#include "../util/png.h"
#include "gnw.h"
#include "../util/zip.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_LIMIT 4096u

static void message(LCDContent *c, int warning, const char *fmt, const char *a, const char *b)
{
    snprintf(c->message, sizeof(c->message), fmt, a ? a : "", b ? b : "");
    c->warning = warning;
}

int lcd_content_is_manifest(const uint8_t *buf, size_t n)
{
    static const char marker[] = "game=catch";
    size_t i, mlen = sizeof(marker) - 1u;
    if (!buf || n < 20u || n > MANIFEST_LIMIT || memcmp(buf, "LCD-LAB/1\n", 10u) != 0)
        return 0;
    for (i = 10u; i + mlen <= n; ++i)
        if (memcmp(buf + i, marker, mlen) == 0 && buf[i - 1u] == '\n' &&
            (i + mlen == n || buf[i + mlen] == '\r' || buf[i + mlen] == '\n'))
            return 1;
    return 0;
}

static const char *base_name(const char *name)
{
    const char *s = strrchr(name, '/');
    return s ? s + 1 : name;
}

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

typedef struct {
    LCDZipEntry rom_entry;
    const LCDMachineDef *def;
    const char *prefer_id;
    const LCDMachineDef *want; /* when set: only this machine's program ROM */
} RomSearch;

static int find_rom(const LCDZipEntry *e, void *user)
{
    RomSearch *s = user;
    const LCDMachineDef *def;
    if (s->want)
        def = (e->crc == s->want->rom.crc && e->size == s->want->rom.size) ? s->want : NULL;
    else
        def = lcd_registry_find_rom(e->crc, e->size, s->prefer_id);
    /* a merged archive can hold several revisions: keep looking for the one named like the archive */
    if (def && (!s->def || (s->prefer_id && !strcmp(def->id, s->prefer_id)))) {
        s->def = def;
        s->rom_entry = *e;
        return s->prefer_id && !strcmp(def->id, s->prefer_id);
    }
    return 0;
}

/* archive base name without extension: MAME set names match it */
static void set_name(const char *path, char *out, size_t len)
{
    const char *b = path ? strrchr(path, '/') : NULL, *dot;
    b = b ? b + 1 : (path ? path : "");
    dot = strrchr(b, '.');
    snprintf(out, len, "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
}

typedef struct {
    const LCDRomDef *file;
    LCDZipEntry entry;
    int found;
} FileSearch;

static int find_file(const LCDZipEntry *e, void *user)
{
    FileSearch *s = user;
    if ((e->crc == s->file->crc && e->size == s->file->size) || same_name(base_name(e->name), s->file->name)) {
        s->entry = *e;
        s->found = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    const LCDScreenDef *screen;
    LCDZipEntry entry;
    int by_crc, by_name;
} SvgSearch;

static int find_svg(const LCDZipEntry *e, void *user)
{
    SvgSearch *s = user;
    if (e->crc == s->screen->file.crc && e->size == s->screen->file.size) {
        s->entry = *e;
        s->by_crc = 1;
        return 1;
    }
    if (!s->by_name && same_name(base_name(e->name), s->screen->file.name)) {
        s->entry = *e;
        s->by_name = 1;
    }
    return 0;
}

static uint8_t *extract_limited(const uint8_t *zip, size_t size, const LCDZipEntry *e, size_t extra, uint32_t limit)
{
    uint8_t *buf;
    if (e->size > limit)
        return NULL;
    buf = malloc((size_t)e->size + extra + 1u);
    if (!buf)
        return NULL;
    if (lcd_zip_extract(zip, size, e, buf, e->size) != 0) {
        free(buf);
        return NULL;
    }
    memset(buf + e->size, 0, extra + 1u);
    return buf;
}

static uint8_t *extract(const uint8_t *zip, size_t size, const LCDZipEntry *e, size_t extra)
{
    return extract_limited(zip, size, e, extra, LCD_ENTRY_LIMIT);
}

/* background.png beside the ROM and SVG members, at the archive root */
static int find_background(const LCDZipEntry *e, void *user)
{
    LCDZipEntry *hit = user;
    if (!strchr(e->name, '/') && same_name(e->name, "background.png")) {
        *hit = *e;
        return 1;
    }
    return 0;
}

static void attach_background(LCDContent *c, const uint8_t *zip, size_t size)
{
    LCDZipEntry e;
    uint8_t *png;
    memset(&e, 0, sizeof(e));
    if (lcd_zip_each(zip, size, find_background, &e) != 0 || !e.name[0])
        return;
    png = extract_limited(zip, size, &e, 0, LCD_BACKGROUND_LIMIT);
    if (!png || lcd_png_decode(png, e.size, &c->background_w, &c->background_h, &c->background) != 0)
        message(c, 1, "background.png could not be read; using the plain LCD%s%s", NULL, NULL);
    free(png);
}

static char system_dir[512];

void lcd_content_set_system_dir(const char *dir) { snprintf(system_dir, sizeof(system_dir), "%s", dir ? dir : ""); }

/* split MAME sets keep shared files in the parent: <dir>/<parent>.zip */
static uint8_t *read_parent(const LCDMachineDef *def, const char *path, size_t *size)
{
    char p[1024];
    const char *slash;
    uint8_t *zip;
    if (!def->parent || !path)
        return NULL;
    slash = strrchr(path, '/');
    snprintf(p, sizeof(p), "%.*s%s%s.zip", slash ? (int)(slash - path) : 0, path, slash ? "/" : "", def->parent);
    zip = lcd_read_file(p, LCD_CONTENT_LIMIT, size);
    if (zip && !lcd_zip_is_archive(zip, *size)) {
        free(zip);
        zip = NULL;
    }
    return zip;
}

static int content_from_zip(LCDContent *c, const uint8_t *zip, size_t size, const char *path)
{
    RomSearch rs;
    unsigned i;
    uint8_t *parent = NULL;
    size_t parent_size = 0;
    const uint8_t *rom_zip = zip;
    size_t rom_zip_size = size;
    char name[LCD_ZIP_NAME_MAX];
    int rc = -1;
    memset(&rs, 0, sizeof(rs));
    set_name(path, name, sizeof(name));
    rs.prefer_id = name;
    if (lcd_zip_each(zip, size, find_rom, &rs) != 0) {
        message(c, 0, "Unreadable ZIP archive", NULL, NULL);
        return -1;
    }
    if (!rs.def) {
        /* split clone sharing its parent's program ROM: identified by the archive name */
        const LCDMachineDef *def = lcd_registry_find_id(name);
        if (def && def->parent) {
            parent = read_parent(def, path, &parent_size);
            if (!parent) {
                message(c, 0, "%s needs its parent set %s.zip beside it (the program ROM is there)", def->id,
                        def->parent);
                return -1;
            }
            rs.want = def;
            lcd_zip_each(parent, parent_size, find_rom, &rs);
            rom_zip = parent;
            rom_zip_size = parent_size;
        }
    }
    if (!rs.def) {
        message(c, 0, "No supported program ROM found in archive (checked CRC32 and size)", NULL, NULL);
        goto done;
    }
    c->def = rs.def;
    c->rom_size = rs.def->rom.size;
    c->rom = extract(rom_zip, rom_zip_size, &rs.rom_entry, 0);
    if (!c->rom) {
        message(c, 0, "Program ROM '%s' failed to extract or failed its CRC check", rs.def->rom.name, NULL);
        goto done;
    }
    if (!lcd_sha1_matches(c->rom, c->rom_size, rs.def->rom.sha1)) {
        message(c, 0, "Program ROM '%s' has the right CRC32 but the wrong SHA-1", rs.def->rom.name, NULL);
        goto done;
    }
    for (i = 0; i < rs.def->screen_count; ++i) {
        SvgSearch ss;
        const uint8_t *src = zip;
        size_t src_size = size;
        memset(&ss, 0, sizeof(ss));
        ss.screen = &rs.def->screens[i];
        if (rs.def->builtin_svg) {
            c->svg_size[i] = strlen(rs.def->builtin_svg);
            c->svg[i] = malloc(c->svg_size[i] + 1u);
            if (!c->svg[i]) {
                message(c, 0, "Out of memory", NULL, NULL);
                goto done;
            }
            memcpy(c->svg[i], rs.def->builtin_svg, c->svg_size[i] + 1u);
            continue;
        }
        lcd_zip_each(zip, size, find_svg, &ss);
        if (!ss.by_crc && !ss.by_name) {
            if (!parent)
                parent = read_parent(rs.def, path, &parent_size);
            if (parent) {
                lcd_zip_each(parent, parent_size, find_svg, &ss);
                src = parent;
                src_size = parent_size;
            }
        }
        if (!ss.by_crc && !ss.by_name) {
            if (rs.def->parent)
                message(c, 0, "Missing LCD artwork '%s': it is in the parent set %s.zip, place it beside this one",
                        ss.screen->file.name, rs.def->parent);
            else
                message(c, 0, "Missing LCD artwork '%s' in the %s ROM set", ss.screen->file.name, rs.def->id);
            goto done;
        }
        c->svg[i] = (char *)extract(src, src_size, &ss.entry, 0);
        c->svg_size[i] = ss.entry.size;
        if (!c->svg[i]) {
            message(c, 0, "LCD artwork '%s' failed to extract", ss.screen->file.name, NULL);
            goto done;
        }
        if (!ss.by_crc)
            message(c, 1, "LCD artwork '%s' differs from MAME 0.289 (%s); using it anyway", ss.screen->file.name,
                    "CRC mismatch");
    }
    if (rs.def->melody.size) {
        FileSearch fs;
        const uint8_t *src = zip;
        size_t src_size = size;
        memset(&fs, 0, sizeof(fs));
        fs.file = &rs.def->melody;
        lcd_zip_each(zip, size, find_file, &fs);
        if (!fs.found && !parent)
            parent = read_parent(rs.def, path, &parent_size);
        if (!fs.found && parent) {
            lcd_zip_each(parent, parent_size, find_file, &fs);
            src = parent;
            src_size = parent_size;
        }
        c->melody = fs.found ? extract(src, src_size, &fs.entry, 0) : NULL;
        c->melody_size = fs.entry.size;
        if (!c->melody || c->melody_size != 0x100 || lcd_crc32(0, c->melody, 0x100) != rs.def->melody.crc) {
            message(c, 0, "Missing or bad melody ROM '%s' in the %s ROM set", rs.def->melody.name, rs.def->id);
            goto done;
        }
    }
    c->kind = LCD_CONTENT_MACHINE;
    attach_background(c, zip, size);
    if (!c->background && !parent && rs.def->parent)
        parent = read_parent(rs.def, path, &parent_size);
    if (!c->background && parent)
        attach_background(c, parent, parent_size);
    rc = 0;

done:
    free(parent);
    return rc;
}

/* .gnw files have no melody ROM: take it from the MAME set, matched by CRC, beside the file, one folder up
   or in <system>/lcd_lab */
static int gnw_melody(LCDContent *c, const char *path)
{
    const char *names[2] = {c->def->id, c->def->parent};
    const char *slash = path ? strrchr(path, '/') : NULL;
    char dirs[3][1024];
    unsigned nd = 0, d, k;
    if (slash) {
        snprintf(dirs[nd++], sizeof(dirs[0]), "%.*s", (int)(slash - path), path);
        snprintf(dirs[nd++], sizeof(dirs[0]), "%.*s/..", (int)(slash - path), path);
    }
    if (system_dir[0])
        snprintf(dirs[nd++], sizeof(dirs[0]), "%s/lcd_lab", system_dir);
    for (d = 0; d < nd; ++d)
        for (k = 0; k < 2; ++k) {
            char p[1100];
            size_t size = 0;
            uint8_t *zip;
            FileSearch fs;
            if (!names[k])
                continue;
            snprintf(p, sizeof(p), "%.1023s/%.64s.zip", dirs[d], names[k]);
            zip = lcd_read_file(p, LCD_CONTENT_LIMIT, &size);
            if (!zip)
                continue;
            memset(&fs, 0, sizeof(fs));
            fs.file = &c->def->melody;
            if (lcd_zip_is_archive(zip, size))
                lcd_zip_each(zip, size, find_file, &fs);
            if (fs.found && fs.entry.crc == c->def->melody.crc && fs.entry.size == c->def->melody.size) {
                c->melody = extract(zip, size, &fs.entry, 0);
                c->melody_size = c->melody ? fs.entry.size : 0;
            }
            free(zip);
            if (c->melody)
                return 1;
        }
    return 0;
}

static int content_from_gnw(LCDContent *c, const uint8_t *data, size_t size, const char *path)
{
    const uint8_t *rom = data + GNW_ROM_OFFSET;
    uint32_t rom_size = (uint32_t)(size - GNW_ROM_OFFSET), crc = lcd_crc32(0, rom, rom_size);
    char name[LCD_ZIP_NAME_MAX];
    set_name(path, name, sizeof(name));
    c->def = lcd_registry_find_rom(crc, rom_size, name);
    if (!c->def) {
        if (gnw_make_def(data, size, &c->gnw_def, c->message, sizeof(c->message)) != 0)
            return -1;
        c->def = &c->gnw_def;
    }
    c->gnw = malloc(size);
    c->rom = malloc(rom_size);
    if (!c->gnw || !c->rom) {
        message(c, 0, "Out of memory loading .gnw file", NULL, NULL);
        return -1;
    }
    memcpy(c->gnw, data, size);
    memcpy(c->rom, rom, rom_size);
    c->rom_size = rom_size;
    if (c->def->melody.size && !gnw_melody(c, path))
        message(c, 1, "%s: .gnw files carry no melody ROM; put the MAME set %s.zip beside it or in "
                      "system/lcd_lab for music",
                c->def->id, c->def->parent ? c->def->parent : c->def->id);
    c->kind = LCD_CONTENT_MACHINE;
    return 0;
}

int lcd_content_load(LCDContent *c, const char *path, const void *data, size_t size)
{
    uint8_t *owned = NULL;
    const uint8_t *buf = data;
    int rc = -1;
    memset(c, 0, sizeof(*c));
    if (!buf) {
        if (!path || !path[0]) {
            message(c, 0, "No content supplied", NULL, NULL);
            return -1;
        }
        owned = lcd_read_file(path, LCD_CONTENT_LIMIT, &size);
        if (!owned) {
            message(c, 0, "Unable to read '%s' (missing or larger than 32 MiB)", path, NULL);
            return -1;
        }
        buf = owned;
    }
    if (size > LCD_CONTENT_LIMIT) {
        message(c, 0, "Content is larger than 32 MiB", NULL, NULL);
    } else if (lcd_content_is_manifest(buf, size)) {
        c->kind = LCD_CONTENT_DIAGNOSTIC;
        rc = 0;
    } else if (gnw_detect(buf, size)) {
        rc = content_from_gnw(c, buf, size, path);
    } else if (lcd_zip_is_archive(buf, size)) {
        rc = content_from_zip(c, buf, size, path);
    } else {
        message(c, 0, "Unsupported content: expected a MAME Game & Watch ROM set (.zip) or an LCD-LAB manifest",
                NULL, NULL);
    }
    free(owned);
    if (rc != 0) {
        char keep[sizeof(c->message)];
        memcpy(keep, c->message, sizeof(keep));
        lcd_content_free(c);
        memcpy(c->message, keep, sizeof(keep));
    }
    return rc;
}

void lcd_content_free(LCDContent *c)
{
    unsigned i;
    if (!c)
        return;
    free(c->rom);
    free(c->melody);
    for (i = 0; i < LCD_MAX_SCREENS; ++i)
        free(c->svg[i]);
    free(c->background);
    free(c->gnw);
    memset(c, 0, sizeof(*c));
}
