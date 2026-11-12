// license:BSD-3-Clause
// copyright-holders:hap
/*
  Sharp SM5xx MCU family (SM500, SM5A, SM510, SM511, SM512, SM530).

  C11 adaptation of MAME 0.289 src/devices/cpu/sm510/. See docs/PROVENANCE.md
  for the changes made while removing the MAME device framework.
*/

#ifndef LCD_CPU_SM5XX_H
#define LCD_CPU_SM5XX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SM5XX_SM500 = 0,
    SM5XX_SM5A = 1,
    SM5XX_SM510 = 2,
    SM5XX_SM511 = 3, /* SM510 with melody controller and 8kHz/16kHz clock select */
    SM5XX_SM512 = 4, /* SM511 with a third LCD RAM bank */
    SM5XX_SM530 = 5  /* 2K ROM, SM500 style LCD outputs, 1/100s timer, 3 clocks per cycle */
} SM5xxVariant;

#define SM5XX_RMASK_DIRECT (-1)

/* LCD common offsets for SM510 segment output */
enum {
    SM5XX_SEG_A = 0x00,
    SM5XX_SEG_B = 0x04,
    SM5XX_SEG_BS = 0x08,
    SM5XX_SEG_C = 0x0c
};

typedef struct {
    uint8_t (*read_k)(void *user);
    int (*read_ba)(void *user);
    int (*read_b)(void *user);
    void (*write_s)(void *user, uint8_t data);
    void (*write_r)(void *user, uint8_t data);
    void (*write_segs)(void *user, unsigned offset, uint16_t data);
    void *user;
} SM5xxBus;

typedef struct {
    /* configuration, fixed after sm5xx_init */
    SM5xxVariant variant;
    const uint8_t *rom;
    size_t rom_size;
    const uint8_t *melody; /* SM511/SM512 melody ROM (256 bytes) or NULL */
    SM5xxBus bus;
    int prgwidth, datawidth, prgmask, datamask, pagemask;
    int stack_levels;
    int o_pins;
    int r_mask_option;

    /* execution state */
    int icount;
    uint16_t pc, prev_pc;
    uint16_t op, prev_op;
    uint8_t param;
    uint16_t stack[4];
    uint8_t acc, bl, bm, bmask, c;
    bool skip;
    uint8_t w, r, r_out;
    bool ext_wakeup, halt;
    int clk_div;

    /* lcd driver */
    uint8_t l, x, y, bp;
    bool bc;

    /* divider */
    uint16_t div;
    uint8_t gamma;

    /* melody controller (SM511/SM512) */
    uint8_t melody_rd, melody_step_count, melody_duty_count, melody_duty_index, melody_address;

    /* SM530 */
    uint16_t subdiv;
    uint8_t count_1s, count_10ms;
    bool ds;

    /* SM500 family */
    uint8_t ox[9], o[9];
    uint8_t mx, cb, s;
    bool rsub;

    /* internal RAM (data address space, unmapped areas are not backed) */
    uint8_t ram[128];

    /* diagnostics */
    uint32_t illegal_count;
} SM5xx;

/* rom must stay valid for the lifetime of the CPU */
int sm5xx_init(SM5xx *cpu, SM5xxVariant variant, const uint8_t *rom, size_t rom_size, const SM5xxBus *bus,
               int r_mask_option);
/* SM511/SM512: attaches the 256 byte melody ROM (pointer kept) */
void sm5xx_set_melody(SM5xx *cpu, const uint8_t *melody);
/* device_reset equivalent (ACL / soft reset); power-on also clears state first */
void sm5xx_power_on(SM5xx *cpu);
void sm5xx_reset(SM5xx *cpu);
/* adds cycles to the budget and executes while it is positive */
void sm5xx_run(SM5xx *cpu, int cycles);
/* one oscillator tick of the 15-bit divider */
void sm5xx_divider_tick(SM5xx *cpu);
/* LCD driver strobe (1kHz in MAME) */
void sm5xx_lcd_update(SM5xx *cpu);
void sm5xx_set_wakeup(SM5xx *cpu, bool state);

uint8_t sm5xx_rom_read(const SM5xx *cpu, uint16_t address);
uint8_t sm5xx_ram_read(const SM5xx *cpu, uint8_t address);
void sm5xx_ram_write(SM5xx *cpu, uint8_t address, uint8_t value);

#endif
