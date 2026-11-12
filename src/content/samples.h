#ifndef LCD_CONTENT_SAMPLES_H
#define LCD_CONTENT_SAMPLES_H

/* MAME sample packs (<id>.zip of WAV files) for machines whose sound chip is not
   dumped: Konami's MSM6373 ADPCM in ktmnt2, kst25 and ktopgun2. */

#include "../hardware/machine.h"
#include <stddef.h>
#include <stdint.h>

#define LCD_MAX_SAMPLES 32u

typedef struct {
    LCDSampleBank bank;
    int16_t *data[LCD_MAX_SAMPLES];
    uint32_t length[LCD_MAX_SAMPLES], rate[LCD_MAX_SAMPLES];
    unsigned loaded;
} LCDSamples;

/* tries each zip path in turn; returns the number of samples loaded (0 when no pack was found) */
unsigned lcd_samples_load(LCDSamples *s, const char *const *names, const char *const *zip_paths, unsigned path_count);
void lcd_samples_free(LCDSamples *s);
/* decodes a PCM WAV (8 or 16 bit, mono or stereo) to mono 16 bit; returns NULL when unsupported */
int16_t *lcd_wav_decode(const uint8_t *wav, size_t size, uint32_t *length, uint32_t *rate);

#endif
