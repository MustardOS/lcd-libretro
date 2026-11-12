#ifndef LCD_AUDIO_H
#define LCD_AUDIO_H
#include <stdint.h>
#include <stdbool.h>
#include "game.h"

#define LCD_SAMPLE_RATE 48000u
#define LCD_FPS 60u
#define LCD_SAMPLES_PER_FRAME (LCD_SAMPLE_RATE / LCD_FPS)

typedef struct {
    uint32_t phase;
    uint32_t step;
    uint32_t samples_left;
} LCDAudio;

void lcd_audio_start(LCDAudio *a, unsigned sound);
void lcd_audio_render(LCDAudio *a, int16_t *stereo, bool enabled);
#endif
