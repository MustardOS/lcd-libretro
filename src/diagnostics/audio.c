#include "audio.h"

void lcd_audio_start(LCDAudio *a, unsigned sound)
{
    unsigned hz, ms;
    switch (sound) {
        case LCD_SOUND_START: hz = 680; ms = 85; break;
        case LCD_SOUND_SCORE: hz = 1080; ms = 38; break;
        case LCD_SOUND_MISS:  hz = 265; ms = 150; break;
        case LCD_SOUND_END:   hz = 170; ms = 390; break;
        default: return;
    }
    a->phase = 0;
    a->step = (uint32_t)((UINT64_C(1) << 32) * hz / LCD_SAMPLE_RATE);
    a->samples_left = LCD_SAMPLE_RATE * ms / 1000u;
}

void lcd_audio_render(LCDAudio *a, int16_t *stereo, bool enabled)
{
    unsigned i;
    for (i = 0; i < LCD_SAMPLES_PER_FRAME; ++i) {
        int16_t s = 0;
        if (a->samples_left) {
            a->phase += a->step;
            if (enabled)
                s = (a->phase & 0x80000000u) ? 3500 : -3500;
            --a->samples_left;
        }
        stereo[i * 2u] = s;
        stereo[i * 2u + 1u] = s;
    }
}
