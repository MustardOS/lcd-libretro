#ifndef LCD_CONTENT_GNW_H
#define LCD_CONTENT_GNW_H

/* Reader for the .gnw files of agg23's Game & Watch FPGA core (MiSTer and
   Analogue Pocket), format version 1: 0x100 byte config, 720x720 background
   and mask images byte interleaved, run length segment masks, program ROM.
   See docs/GNW_FORMAT.md. */

#include "registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GNW_SIDE 720u
#define GNW_IMAGE_OFFSET 0x100u
#define GNW_MASK_OFFSET 0x2F7700u
#define GNW_MASK_SIZE 0x2DB40u
#define GNW_ROM_OFFSET 0x325240u

typedef struct {
    uint16_t id; /* x << 6 | y << 2 | z */
    uint16_t x, y, length;
} GnwRun;

bool gnw_detect(const uint8_t *data, size_t size);
/* builds a machine definition from the header when the ROM is not in the registry */
int gnw_make_def(const uint8_t *data, size_t size, LCDMachineDef *def, char *err, size_t err_len);
/* reads run n; returns false at the end of the list */
bool gnw_run(const uint8_t *data, unsigned n, GnwRun *run);
/* background (lit = false) or lit segment colour of a pixel, as 0xRRGGBB */
uint32_t gnw_pixel(const uint8_t *data, unsigned x, unsigned y, bool lit);

#endif
