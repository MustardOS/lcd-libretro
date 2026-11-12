#ifndef LCD_CONTENT_REGISTRY_H
#define LCD_CONTENT_REGISTRY_H

#include "../cpu/sm5xx.h"
#include <stddef.h>
#include <stdint.h>

#define LCD_MAX_SCREENS 3
#define LCD_MAX_INPUTS 40
#define LCD_NO_PAD 0xffu /* input reachable from the keyboard only */
#define LCD_IN_PORTS 9

typedef enum {
    LCD_PORT_IN0 = 0, /* IN.0 .. IN.8 multiplexed K input lines */
    LCD_PORT_BA = 9,
    LCD_PORT_B = 10,
    LCD_PORT_ACL = 11
} LCDPort;

typedef enum {
    LCD_WIRING_SM5A_R1_PIEZO_MUX, /* hh_sm510 sm5a_common: R1 piezo, R2-R4 input mux */
    LCD_WIRING_SM510_S_MUX_R1,    /* hh_sm510 sm510_common: S input mux, R1 piezo */
    LCD_WIRING_SM510_S_MUX_R2,    /* as above with the piezo on R2 (gnw_mickdon) */
    LCD_WIRING_SM511_TIGER2BIT    /* sm511_tiger2bit: piezo from R1 and S1 through two resistors, S2+ input mux */
} LCDWiring;

typedef enum {
    LCD_LAYOUT_SINGLE,
    LCD_LAYOUT_DUAL_VERTICAL,  /* hh_sm510_dualv.lay "Over-Under" */
    LCD_LAYOUT_DUAL_HORIZONTAL, /* hh_sm510_dualh.lay "Side-by-Side" */
    LCD_LAYOUT_TRIPLE_HORIZONTAL /* hh_sm510_tripleh.lay "Triple Side-by-Side (Gapless)" */
} LCDLayout;

typedef struct {
    unsigned retro_id; /* RETRO_DEVICE_ID_JOYPAD_*, or LCD_NO_PAD */
    uint8_t port;      /* LCDPort */
    uint8_t mask;
    uint8_t active_low;
    const char *label;
    uint16_t key;      /* RETROK_* from MAME's PORT_CODE, 0 when none */
    uint8_t dial;      /* IPT_DIAL: held, it steps the value in mask upwards */
} LCDInputDef;

enum {
    LCD_QUIRK_S5_S6_TIED = 1, /* tnmarebc: S5 and S6 drive the same input line */
    LCD_QUIRK_ADPCM = 2,      /* Konami: MSM6373 ADPCM on S1-S5/S7/S8, busy on B (MAME plays samples) */
    LCD_QUIRK_LED = 4         /* LED display drawn from a built-in layout, no LCD tint */
};

typedef struct {
    const char *name;
    uint32_t size;
    uint32_t crc;
    const char *sha1;
} LCDRomDef;

typedef struct {
    LCDRomDef file;
    uint16_t width, height; /* MAME svg screen size */
} LCDScreenDef;

/* clock kept by the game program in RAM: BCD digits of a 12 hour (or 24 hour) clock.
   hour tens nibble = base | digit | (pm ? pm : am); some programs keep AM/PM in a nibble of its own */
typedef struct {
    uint8_t present;
    uint8_t h10, h1, m10, m1; /* RAM addresses */
    uint8_t base, am, pm;
    uint8_t ap, ap_am, ap_pm; /* separate AM/PM nibble address and values, ap = 0xff when none */
    uint8_t h24;              /* hours run 0-23 */
} LCDRtcDef;

typedef struct {
    const char *id;    /* MAME machine name */
    const char *title;
    const char *year;
    SM5xxVariant cpu;
    int r_mask_option;
    LCDWiring wiring;
    uint32_t region_size;
    LCDRomDef rom;
    LCDLayout layout;
    unsigned screen_count;
    LCDScreenDef screens[LCD_MAX_SCREENS];
    uint8_t inp_lines;
    int8_t inp_fixed;              /* IN line tied to ground, -1 when none */
    uint8_t in_rest[LCD_IN_PORTS]; /* port values with nothing pressed */
    int8_t ba_rest, b_rest;        /* -1 when the port does not exist (reads 1) */
    LCDInputDef inputs[LCD_MAX_INPUTS]; /* terminated by a NULL label */
    const char *parent;          /* MAME parent set for split clones, NULL when none */
    uint8_t decay_pivot, decay_len; /* LCD decay in 1kHz ticks, 0 for MAME's defaults (8/17) */
    LCDRomDef melody;               /* SM511/SM512/SM530 melody ROM, size 0 when none */
    uint8_t quirks;                 /* LCD_QUIRK_* */
    const char *const *samples;     /* LCD_QUIRK_ADPCM: MAME sample names in order, NULL terminated */
    const char *builtin_svg;        /* screen drawn from a built-in layout (LCD_QUIRK_LED) */
} LCDMachineDef;

/* several machines can share a program ROM: prefer_id (the archive name) picks between them */
const LCDMachineDef *lcd_registry_find_rom(uint32_t crc, uint32_t size, const char *prefer_id);
const LCDMachineDef *lcd_registry_find_id(const char *id);
/* verified clock RAM layout for a machine, NULL when none is known */
const LCDRtcDef *lcd_registry_rtc(const char *id);

#endif
