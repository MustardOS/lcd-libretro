#ifndef LCD_CONTENT_LOADER_H
#define LCD_CONTENT_LOADER_H

#include "registry.h"
#include "../util/zip.h"
#include <stddef.h>
#include <stdint.h>

#define LCD_CONTENT_LIMIT (32u * 1024u * 1024u)
#define LCD_ENTRY_LIMIT (4u * 1024u * 1024u)
#define LCD_BACKGROUND_LIMIT (16u * 1024u * 1024u)

typedef enum {
    LCD_CONTENT_NONE,
    LCD_CONTENT_DIAGNOSTIC, /* original Catch and Carry test manifest */
    LCD_CONTENT_MACHINE     /* MAME ROM set for a supported machine */
} LCDContentKind;

typedef struct {
    LCDContentKind kind;
    const LCDMachineDef *def;
    uint8_t *rom;
    size_t rom_size;
    uint8_t *melody; /* SM511/SM512 melody ROM */
    size_t melody_size;
    char *svg[LCD_MAX_SCREENS];
    size_t svg_size[LCD_MAX_SCREENS];
    char message[256]; /* error or warning text */
    int warning;

    /* Game & Watch FPGA .gnw file: whole file kept for its images and masks */
    uint8_t *gnw;
    LCDMachineDef gnw_def; /* used when the program is not in the registry */

    /* optional background.png at the archive root, decoded to straight RGBA */
    uint8_t *background;
    unsigned background_w, background_h;
} LCDContent;

/* reads path when data is NULL; never keeps a reference to data */
int lcd_content_load(LCDContent *c, const char *path, const void *data, size_t size);
void lcd_content_free(LCDContent *c);
/* frontend system directory: <system>/lcd_lab is searched for .gnw melody ROMs */
void lcd_content_set_system_dir(const char *dir);
int lcd_content_is_manifest(const uint8_t *buf, size_t n);

#endif
