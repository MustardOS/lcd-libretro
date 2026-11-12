#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H
#include <stdint.h>
#include <stdbool.h>
#include "game.h"

#define LCD_WIDTH 320u
#define LCD_HEIGHT 240u

typedef struct {
    uint8_t contrast; /* 0 soft, 1 normal, 2 bold */
    bool ghost;
    bool sound;
    bool fast;
} LCDOptions;

void lcd_display_init(void);
const void *lcd_display_render(const LCDGame *g, const LCDOptions *o,
                               bool rgb565);
#endif
