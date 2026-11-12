#ifndef LCD_HARDWARE_MACHINE_H
#define LCD_HARDWARE_MACHINE_H

#include "../content/registry.h"
#include "../cpu/sm5xx.h"
#include <stdbool.h>
#include <stdint.h>

#define LCD_OSC_HZ 32768u
#define LCD_DECAY_DIVIDER 32u /* 1024Hz LCD strobe and decay ticks */
#define LCD_ZX 0x20
#define LCD_SEGS 0x20

/* decoded MAME samples (Konami ADPCM stand-in), owned by the caller */
typedef struct {
    unsigned count;
    const int16_t *const *data; /* NULL for a missing sample */
    const uint32_t *length, *rate;
} LCDSampleBank;

typedef struct {
    const LCDMachineDef *def;
    const LCDSampleBank *samples;
    SM5xx cpu;
    uint8_t rom[0x1000];
    uint8_t melody[0x100];

    /* board state (hh_sm510_state) */
    uint16_t inp_mux;
    uint8_t ports[LCD_IN_PORTS];
    uint8_t ba, b;
    bool acl;
    uint8_t speaker;
    uint8_t display_x_len, display_y_len, display_z_len;
    uint8_t decay_pivot, decay_len;
    uint8_t dial, dial_phase;
    int8_t adpcm_sample; /* playing sample index, -1 when idle */
    uint32_t adpcm_pos;  /* in source samples, 16.16 fixed point */
    uint32_t display_state[LCD_ZX];
    uint8_t display_decay[LCD_SEGS][LCD_ZX];

    /* scheduler */
    uint8_t clock_phase;
    uint8_t strobe_phase;
} LCDMachine;

int lcd_machine_init(LCDMachine *m, const LCDMachineDef *def, const uint8_t *rom, size_t rom_size);
void lcd_machine_power_on(LCDMachine *m);
/* SM511/SM512: copies the 256 byte melody ROM and powers on again */
void lcd_machine_attach_melody(LCDMachine *m, const uint8_t *melody, size_t size);
/* copies a machine; plain struct assignment would leave the CPU bound to src */
void lcd_machine_clone(LCDMachine *dst, const LCDMachine *src);
void lcd_machine_soft_reset(LCDMachine *m);
/* buttons: bit n set when RETRO_DEVICE_ID_JOYPAD n is held */
void lcd_machine_set_buttons(LCDMachine *m, uint32_t buttons);
/* as above, plus keys[i] != 0 when the keyboard key of def->inputs[i] is held (keys may be NULL);
   call once per frame: dials step with the frame count */
void lcd_machine_set_input(LCDMachine *m, uint32_t buttons, const uint8_t *keys);
/* LCD_QUIRK_ADPCM: sample bank used for playback and the busy line (NULL: silent, never busy, as MAME) */
void lcd_machine_attach_samples(LCDMachine *m, const LCDSampleBank *samples);
/* runs oscillator ticks; one speaker level (0..255) per tick is written to levels when non-NULL */
void lcd_machine_run(LCDMachine *m, unsigned ticks, uint8_t *levels);
/* as above, with the sample channel per tick written to pcm when non-NULL */
void lcd_machine_run_pcm(LCDMachine *m, unsigned ticks, uint8_t *levels, int16_t *pcm);

/* writes a time of day into the game's clock RAM (machines with an RTC layout) */
bool lcd_machine_set_clock(LCDMachine *m, unsigned hour24, unsigned minute);
/* powers on like a user would: lets the program boot, then presses Time to leave the segment test;
   false when the machine has no Time button */
bool lcd_machine_boot_to_clock(LCDMachine *m);
/* encodes a time of day into RAM with a given layout */
void lcd_rtc_write(SM5xx *cpu, const LCDRtcDef *r, unsigned hour24, unsigned minute);

/* segment brightness 0..255 for output x.y.z, following MAME's decay model */
uint8_t lcd_machine_segment_level(const LCDMachine *m, unsigned x, unsigned y, unsigned z, bool fade);
bool lcd_machine_segment_active(const LCDMachine *m, unsigned x, unsigned y, unsigned z);

size_t lcd_machine_state_size(void);
bool lcd_machine_save(const LCDMachine *m, uint8_t *dst, size_t size);
bool lcd_machine_load(LCDMachine *m, const uint8_t *src, size_t size);

#endif
