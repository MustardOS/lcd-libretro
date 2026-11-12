// license:BSD-3-Clause
// copyright-holders:hap
// thanks-to:Igor
/*
  Sharp SM5xx MCU family (SM500, SM5A, SM510, SM511, SM512, SM530).

  C11 adaptation of MAME 0.289 src/devices/cpu/sm510/. Instruction semantics,
  memory maps, the program counter LFSR, the divider and the LCD drivers follow
  the upstream implementation. The MAME device, address space, timer and
  callback framework is replaced by plain structures and function pointers.
*/

#include "sm5xx.h"
#include <string.h>

static uint8_t read_k(SM5xx *cpu) { return cpu->bus.read_k ? cpu->bus.read_k(cpu->bus.user) : 0; }
static int read_ba(SM5xx *cpu) { return cpu->bus.read_ba ? cpu->bus.read_ba(cpu->bus.user) : 1; }
static int read_b(SM5xx *cpu) { return cpu->bus.read_b ? cpu->bus.read_b(cpu->bus.user) : 1; }

static void write_s(SM5xx *cpu, uint8_t data)
{
    if (cpu->bus.write_s)
        cpu->bus.write_s(cpu->bus.user, data);
}

static void write_r(SM5xx *cpu, uint8_t data)
{
    if (cpu->bus.write_r)
        cpu->bus.write_r(cpu->bus.user, data);
}

static void write_segs(SM5xx *cpu, unsigned offset, uint16_t data)
{
    if (cpu->bus.write_segs)
        cpu->bus.write_segs(cpu->bus.user, offset, data);
}

static bool is_sm500(const SM5xx *cpu) { return cpu->variant == SM5XX_SM500 || cpu->variant == SM5XX_SM5A; }
static bool is_sm511(const SM5xx *cpu) { return cpu->variant == SM5XX_SM511 || cpu->variant == SM5XX_SM512; }
static bool is_sm530(const SM5xx *cpu) { return cpu->variant == SM5XX_SM530; }
static bool has_melody(const SM5xx *cpu) { return is_sm511(cpu) || is_sm530(cpu); }

uint8_t sm5xx_rom_read(const SM5xx *cpu, uint16_t address)
{
    unsigned a = address & (unsigned)cpu->prgmask;
    switch (cpu->variant) {
    case SM5XX_SM500:
        if (a > 0x4bfu)
            return 0;
        break;
    case SM5XX_SM5A:
        if (a >= 0x700u)
            a = 0x700u | (a & 0x3fu);
        break;
    case SM5XX_SM510:
        if ((a & 0x3ffu) >= 0x2c0u)
            return 0;
        break;
    default: /* SM511/SM512: 4K ROM, SM530: 2K ROM */
        break;
    }
    return a < cpu->rom_size ? cpu->rom[a] : 0;
}

static int ram_index(const SM5xx *cpu, unsigned a)
{
    a &= (unsigned)cpu->datamask;
    switch (cpu->variant) {
    case SM5XX_SM500:
        return ((a & 0xfu) <= 9u && a <= 0x39u) ? (int)a : -1;
    case SM5XX_SM5A:
        if ((a & 0xfu) > 0xcu)
            a = (a & 0x70u) | 0xcu;
        if (a >= 0x40u)
            a = 0x40u | (a & 0xfu);
        return (int)a;
    case SM5XX_SM530:
        /* 64 nibbles, LCD RAM A at 0x40-0x4b and B at 0x50-0x5b, mirrored at +0x20 */
        if (a < 0x40u)
            return (int)a;
        a &= ~0x20u;
        return (a & 0xfu) <= 0xbu ? (int)a : -1;
    default: /* SM510/SM511/SM512: 128 nibbles */
        return (int)a;
    }
}

uint8_t sm5xx_ram_read(const SM5xx *cpu, uint8_t address)
{
    int i = ram_index(cpu, address);
    return i < 0 ? 0 : cpu->ram[i] & 0xf;
}

void sm5xx_ram_write(SM5xx *cpu, uint8_t address, uint8_t value)
{
    int i = ram_index(cpu, address);
    if (i >= 0)
        cpu->ram[i] = value & 0xf;
}

static uint8_t ram_r(SM5xx *cpu)
{
    return sm5xx_ram_read(cpu, (uint8_t)((cpu->bmask | cpu->bm << 4 | cpu->bl) & cpu->datamask));
}

static void ram_w(SM5xx *cpu, uint8_t data)
{
    int i = ram_index(cpu, (unsigned)(cpu->bmask | cpu->bm << 4 | cpu->bl));
    if (i >= 0)
        cpu->ram[i] = data & 0xf;
}

static void pop_stack(SM5xx *cpu)
{
    int i;
    cpu->pc = (uint16_t)(cpu->stack[0] & cpu->prgmask);
    for (i = 0; i < cpu->stack_levels - 1; i++)
        cpu->stack[i] = cpu->stack[i + 1];
}

static void push_stack(SM5xx *cpu)
{
    int i;
    for (i = cpu->stack_levels - 1; i >= 1; i--)
        cpu->stack[i] = cpu->stack[i - 1];
    cpu->stack[0] = cpu->pc;
}

static void do_branch(SM5xx *cpu, uint8_t pu, uint8_t pm, uint8_t pl)
{
    cpu->pc = (uint16_t)(((pu << 10) | (pm << 6 & 0x3c0) | (pl & 0x03f)) & cpu->prgmask);
}

/* SM530 has no Pm */
static void do_branch_sm530(SM5xx *cpu, uint8_t pu, uint8_t pl)
{
    cpu->pc = (uint16_t)(((pu << 6) | (pl & 0x3f)) & cpu->prgmask);
}

static uint8_t bitmask(uint16_t param) { return (uint8_t)(1u << (param & 3u)); }

static void increment_pc(SM5xx *cpu)
{
    /* PL is a 6-bit LFSR: newbit = (bit0 == bit1) */
    int msb = cpu->pagemask >> 1 ^ cpu->pagemask;
    int feed = ((cpu->pc >> 1 ^ cpu->pc) & 1) ? 0 : msb;
    cpu->pc = (uint16_t)(feed | (cpu->pc >> 1 & cpu->pagemask >> 1) | (cpu->pc & ~cpu->pagemask));
}

static void reset_vector(SM5xx *cpu)
{
    if (is_sm530(cpu))
        do_branch_sm530(cpu, 0xf, 0);
    else if (is_sm500(cpu))
        do_branch(cpu, 0, 0xf, 0);
    else
        do_branch(cpu, 3, 7, 0);
}

static void wakeup_vector(SM5xx *cpu)
{
    if (is_sm530(cpu)) {
        do_branch_sm530(cpu, 0, 0);
    } else if (is_sm500(cpu)) {
        cpu->cb = 0;
        do_branch(cpu, 0, 0, 0);
    } else {
        do_branch(cpu, 1, 0, 0);
    }
}

static void clock_melody_sm511(SM5xx *cpu)
{
    /* tone cycle table (SM511/SM512 datasheet fig.5); cmd 0 = rest, 1 = stop, > 13 illegal */
    static const uint8_t lut_tone_cycles[4 * 16] = {
        0, 0, 7, 8, 8, 9, 9, 10, 11, 11, 12, 13, 14, 14, 0, 0,
        0, 0, 8, 8, 9, 9, 10, 11, 11, 12, 13, 13, 14, 15, 0, 0,
        0, 0, 8, 8, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15, 0, 0,
        0, 0, 8, 9, 9, 10, 10, 11, 11, 12, 13, 14, 14, 15, 0, 0};
    uint8_t cmd, out = 0;
    if (!cpu->melody)
        return;
    cmd = cpu->melody[cpu->melody_address] & 0x3f;

    /* clock duty cycle if tone is active */
    if ((cmd & 0xf) >= 2 && (cmd & 0xf) <= 13) {
        int index = cpu->melody_duty_index << 4 | (cmd & 0xf);
        int shift = ~cmd >> 4 & 1; /* OCT */
        out = cpu->melody_duty_index & cpu->melody_rd & 1;
        cpu->melody_duty_count++;
        if (cpu->melody_duty_count >= (lut_tone_cycles[index] << shift)) {
            cpu->melody_duty_count = 0;
            cpu->melody_duty_index = (cpu->melody_duty_index + 1) & 3;
        }
    } else if ((cmd & 0xf) == 1) {
        cpu->melody_rd |= 2; /* melody stop flag */
    }

    /* time base on divider F7/F8 (SM530: F8) */
    if ((cpu->div & (is_sm530(cpu) ? 0xff : 0x7f)) == 0) {
        uint8_t mask = (cmd & 0x20) ? 0x1f : 0x0f;
        cpu->melody_step_count = (cpu->melody_step_count + 1) & mask;
        if (cpu->melody_step_count == 0)
            cpu->melody_address++;
    }

    if (out != cpu->r_out) {
        write_r(cpu, out);
        cpu->r_out = out;
    }
}

static void clock_melody(SM5xx *cpu)
{
    uint8_t out;
    if (has_melody(cpu)) {
        clock_melody_sm511(cpu);
        return;
    }
    if (is_sm500(cpu)) {
        /* R1 from divider or direct control, R2-R4 generic outputs */
        uint8_t mask = (cpu->r_mask_option == SM5XX_RMASK_DIRECT) ? 1 : (cpu->div >> cpu->r_mask_option & 1);
        out = (uint8_t)(((mask & ~cpu->r) | (~cpu->r & 0xe)) & 0xff);
    } else if (cpu->r_mask_option == SM5XX_RMASK_DIRECT) {
        out = cpu->r & 3;
    } else {
        /* from divider, R2 inverse phase */
        out = cpu->div >> cpu->r_mask_option & 1;
        out |= (uint8_t)((out << 1) ^ 2);
        out &= cpu->r;
    }
    if (out != cpu->r_out) {
        write_r(cpu, out);
        cpu->r_out = out;
    }
}

/* SM500 W' latch helpers */

static void shift_w(SM5xx *cpu)
{
    int i;
    for (i = 0; i < cpu->o_pins - 1; i++)
        cpu->ox[i] = cpu->ox[i + 1];
}

static uint8_t get_digit(SM5xx *cpu)
{
    static const uint8_t lut_digits[0x20] = {
        0xe, 0x0, 0xc, 0x8, 0x2, 0xa, 0xe, 0x2, 0xe, 0xa, 0x0, 0x0, 0x2, 0xa, 0x2, 0x2,
        0xb, 0x9, 0x7, 0xf, 0xd, 0xe, 0xe, 0xb, 0xf, 0xf, 0x4, 0x0, 0xd, 0xe, 0x4, 0x0};
    uint8_t sel = cpu->bp >> 3 & 1;
    return (uint8_t)(lut_digits[sel << 4 | cpu->acc] | (~sel & cpu->mx));
}

static void set_su(SM5xx *cpu, uint8_t su)
{
    cpu->stack[0] = (uint16_t)((cpu->stack[0] & ~0x3c0) | (su << 6 & 0x3c0));
}

static uint8_t get_su(const SM5xx *cpu) { return cpu->stack[0] >> 6 & 0xf; }

static void op_lb(SM5xx *cpu)
{
    if (is_sm500(cpu)) {
        cpu->bm = cpu->op & 3;
        cpu->bl = (uint8_t)((cpu->op >> 2 & 3) | ((cpu->op & 0xc) ? 8 : 0));
    } else {
        cpu->bm = (uint8_t)((cpu->bm & 4) | (cpu->op & 3));
        cpu->bl = (uint8_t)((cpu->op >> 2 & 3) | ((cpu->op & 0xc) ? 0xc : 0));
    }
}

static void op_lbl(SM5xx *cpu)
{
    cpu->bl = cpu->param & 0xf;
    cpu->bm = (uint8_t)((cpu->param & cpu->datamask) >> 4);
}

static void op_sbm(SM5xx *cpu)
{
    /* SM510: BM high bit for next opcode only, handled after execute */
    if (is_sm500(cpu))
        cpu->bm |= 4;
}

static void op_rbm(SM5xx *cpu) { cpu->bm &= (uint8_t)~4; }

static void op_exbla(SM5xx *cpu)
{
    uint8_t a = cpu->acc;
    cpu->acc = cpu->bl;
    cpu->bl = a;
}

static void op_incb(SM5xx *cpu)
{
    cpu->bl = (cpu->bl + 1) & 0xf;
    cpu->skip = (cpu->bl == 0);
    if (is_sm500(cpu))
        cpu->skip = (cpu->bl == 8);
    if (is_sm530(cpu))
        cpu->skip = ((cpu->bl & 7) == 0); /* overflow on the 3rd bit */
}

static void op_decb(SM5xx *cpu)
{
    cpu->bl = (cpu->bl - 1) & 0xf;
    cpu->skip = (cpu->bl == 0xf);
}

static void op_atpl(SM5xx *cpu) { cpu->pc = (uint16_t)((cpu->prev_pc & ~0xf) | cpu->acc); }

static void op_rtn0(SM5xx *cpu)
{
    pop_stack(cpu);
    if (is_sm500(cpu))
        cpu->rsub = false;
}

static void op_rtn1(SM5xx *cpu)
{
    op_rtn0(cpu);
    cpu->skip = true;
}

static void op_t(SM5xx *cpu) { cpu->pc = (uint16_t)((cpu->pc & ~cpu->pagemask) | (cpu->op & cpu->pagemask)); }

static void op_tl(SM5xx *cpu) { do_branch(cpu, cpu->param >> 6 & 3, cpu->op & 0xf, cpu->param & 0x3f); }

static void op_tml(SM5xx *cpu)
{
    push_stack(cpu);
    do_branch(cpu, cpu->param >> 6 & 3, cpu->op & 3, cpu->param & 0x3f);
}

static void op_tm(SM5xx *cpu)
{
    uint8_t idx;
    cpu->icount--;
    push_stack(cpu);
    idx = sm5xx_rom_read(cpu, cpu->op & 0x3f);
    do_branch(cpu, idx >> 6 & 3, 4, idx & 0x3f);
}

static void op_comcb(SM5xx *cpu) { cpu->cb ^= 1; }

static void op_ssr(SM5xx *cpu) { set_su(cpu, cpu->op & 0xf); }

static void op_tr(SM5xx *cpu)
{
    cpu->pc = (uint16_t)((cpu->pc & ~0x3f) | (cpu->op & 0x3f));
    if (!cpu->rsub)
        do_branch(cpu, cpu->cb, get_su(cpu), cpu->pc & 0x3f);
}

static void op_trs(SM5xx *cpu)
{
    if (!cpu->rsub) {
        uint8_t su;
        cpu->rsub = true;
        su = get_su(cpu);
        push_stack(cpu);
        do_branch(cpu, cpu->variant == SM5XX_SM5A ? 1 : 0, 0, cpu->op & 0x3f);
        /* E flag was set? */
        if ((cpu->prev_op & 0xf0) == 0x70)
            do_branch(cpu, cpu->cb, su, cpu->pc & 0x3f);
    } else {
        cpu->pc = (uint16_t)((cpu->pc & ~0xff) | (cpu->op << 2 & 0xc0) | (cpu->op & 0xf));
    }
}

static void op_exc(SM5xx *cpu)
{
    uint8_t a = cpu->acc;
    cpu->acc = ram_r(cpu);
    ram_w(cpu, a);
    cpu->bm ^= (uint8_t)(cpu->op & 3);
}

static void op_bdc(SM5xx *cpu) { cpu->bc = (cpu->c != 0); }

static void op_exci(SM5xx *cpu)
{
    op_exc(cpu);
    op_incb(cpu);
}

static void op_excd(SM5xx *cpu)
{
    op_exc(cpu);
    op_decb(cpu);
}

static void op_lda(SM5xx *cpu)
{
    cpu->acc = ram_r(cpu);
    cpu->bm ^= (uint8_t)(cpu->op & 3);
}

static void op_lax(SM5xx *cpu)
{
    /* skip any next LAX */
    if ((cpu->op & ~0xf) != (cpu->prev_op & ~0xf))
        cpu->acc = cpu->op & 0xf;
}

static void op_ptw(SM5xx *cpu)
{
    if (is_sm500(cpu)) {
        cpu->o[cpu->o_pins - 1] = cpu->ox[cpu->o_pins - 1];
        cpu->o[cpu->o_pins - 2] = cpu->ox[cpu->o_pins - 2];
    } else {
        write_s(cpu, cpu->w);
    }
}

static void op_tw(SM5xx *cpu)
{
    int i;
    for (i = 0; i < cpu->o_pins; i++)
        cpu->o[i] = cpu->ox[i];
}

static void op_pdtw(SM5xx *cpu)
{
    cpu->ox[cpu->o_pins - 2] = cpu->ox[cpu->o_pins - 1];
    cpu->ox[cpu->o_pins - 1] = get_digit(cpu);
}

static void op_dtw(SM5xx *cpu)
{
    shift_w(cpu);
    cpu->ox[cpu->o_pins - 1] = get_digit(cpu);
}

static void op_wr(SM5xx *cpu)
{
    if (is_sm500(cpu)) {
        shift_w(cpu);
        cpu->ox[cpu->o_pins - 1] = cpu->acc & 7;
    } else {
        cpu->w = (uint8_t)(cpu->w << 1 | 0);
        if (cpu->variant == SM5XX_SM510)
            write_s(cpu, cpu->w); /* SM510: W is connected directly to S */
    }
}

static void op_ws(SM5xx *cpu)
{
    if (is_sm500(cpu)) {
        shift_w(cpu);
        cpu->ox[cpu->o_pins - 1] = cpu->acc | 8;
    } else {
        cpu->w = (uint8_t)(cpu->w << 1 | 1);
        if (cpu->variant == SM5XX_SM510)
            write_s(cpu, cpu->w);
    }
}

static void op_kta(SM5xx *cpu) { cpu->acc = read_k(cpu) & 0xf; }
static void op_atbp(SM5xx *cpu) { cpu->bp = cpu->acc; }
static void op_atl(SM5xx *cpu) { cpu->l = cpu->acc; }
static void op_atfc(SM5xx *cpu) { cpu->y = cpu->acc; }
static void op_atx(SM5xx *cpu) { cpu->x = cpu->acc; }

/* melody control (SM511/SM512) */
static void op_pre(SM5xx *cpu)
{
    cpu->melody_address = cpu->param;
    cpu->melody_step_count = 0;
}

static void op_sme(SM5xx *cpu) { cpu->melody_rd |= 1; }
static void op_rme(SM5xx *cpu) { cpu->melody_rd &= (uint8_t)~1; }

static void op_tmel(SM5xx *cpu)
{
    cpu->skip = (cpu->melody_rd & 2) != 0;
    cpu->melody_rd &= (uint8_t)~2;
}

static void op_clklo(SM5xx *cpu) { cpu->clk_div = 4; }
static void op_clkhi(SM5xx *cpu) { cpu->clk_div = 2; }
static void op_ats(SM5xx *cpu) { cpu->s = cpu->acc; }

static void op_atr(SM5xx *cpu)
{
    cpu->r = cpu->acc;
    clock_melody(cpu);
}

static void op_add(SM5xx *cpu) { cpu->acc = (cpu->acc + ram_r(cpu)) & 0xf; }

static void op_add11(SM5xx *cpu)
{
    cpu->acc = (uint8_t)(cpu->acc + ram_r(cpu) + cpu->c);
    cpu->c = cpu->acc >> 4 & 1;
    cpu->skip = (cpu->c == 1);
    cpu->acc &= 0xf;
}

static void op_adx(SM5xx *cpu)
{
    cpu->acc = (uint8_t)(cpu->acc + (cpu->op & 0xf));
    cpu->skip = ((cpu->op & 0xf) != 10 && (cpu->acc & 0x10) != 0);
    cpu->acc &= 0xf;
}

static void op_coma(SM5xx *cpu) { cpu->acc ^= 0xf; }

static void op_rot(SM5xx *cpu)
{
    uint8_t c = cpu->acc & 1;
    cpu->acc = (uint8_t)(cpu->acc >> 1 | cpu->c << 3);
    cpu->c = c;
}

static void op_rc(SM5xx *cpu) { cpu->c = 0; }
static void op_sc(SM5xx *cpu) { cpu->c = 1; }

static void op_tb(SM5xx *cpu) { cpu->skip = (read_b(cpu) != 0); }
static void op_tc(SM5xx *cpu) { cpu->skip = !cpu->c; }
static void op_tam(SM5xx *cpu) { cpu->skip = (cpu->acc == ram_r(cpu)); }
static void op_tmi(SM5xx *cpu) { cpu->skip = ((ram_r(cpu) & bitmask(cpu->op)) != 0); }
static void op_ta0(SM5xx *cpu) { cpu->skip = !cpu->acc; }
static void op_tabl(SM5xx *cpu) { cpu->skip = (cpu->acc == cpu->bl); }

static void op_tis(SM5xx *cpu)
{
    cpu->skip = !cpu->gamma;
    cpu->gamma = 0;
}

static void op_tal(SM5xx *cpu) { cpu->skip = (read_ba(cpu) != 0); }
static void op_tf1(SM5xx *cpu) { cpu->skip = ((cpu->div & 0x4000) != 0); }
static void op_tf4(SM5xx *cpu) { cpu->skip = ((cpu->div & 0x0800) != 0); }

static void op_rm(SM5xx *cpu) { ram_w(cpu, ram_r(cpu) & (uint8_t)~bitmask(cpu->op)); }
static void op_sm(SM5xx *cpu) { ram_w(cpu, ram_r(cpu) | bitmask(cpu->op)); }

static void op_rmf(SM5xx *cpu)
{
    cpu->mx = 0;
    cpu->acc = 0;
}

static void op_smf(SM5xx *cpu) { cpu->mx = 1; }
static void op_comcn(SM5xx *cpu) { cpu->bp ^= 8; }

static void op_cend(SM5xx *cpu) { cpu->halt = true; }

static void op_idiv(SM5xx *cpu)
{
    if (is_sm500(cpu))
        cpu->div &= 0x3f;
    else
        cpu->div = 0;
}

static void op_dta(SM5xx *cpu) { cpu->acc = cpu->div >> 11 & 0xf; }

static void op_illegal(SM5xx *cpu) { cpu->illegal_count++; }

/* SM530 instructions (sm530op.cpp) */

static void op_lb_sm530(SM5xx *cpu)
{
    cpu->bl = (uint8_t)((cpu->op << 2 & 8) | (cpu->op & 1) | 6);
    cpu->bm = cpu->op >> 2 & 3;
}

static void op_tl_sm530(SM5xx *cpu)
{
    do_branch_sm530(cpu, (uint8_t)((cpu->op << 2) | (cpu->param >> 6 & 3)), cpu->param & 0x3f);
}

static void op_trs_sm530(SM5xx *cpu)
{
    /* indirect subroutine call, jump vectors are on page 14 */
    uint8_t jump;
    cpu->icount--;
    push_stack(cpu);
    jump = sm5xx_rom_read(cpu, (uint16_t)((14 << 6) | (cpu->op & 0x3f)));
    do_branch_sm530(cpu, (uint8_t)((jump >> 5 & 1) << 2 | (jump >> 7 & 1) << 1 | (jump >> 6 & 1)), jump & 0x1f);
}

static void op_dta_sm530(SM5xx *cpu) { cpu->acc = cpu->count_10ms; }

static void op_adx_sm530(SM5xx *cpu)
{
    cpu->acc = (uint8_t)(cpu->acc + (cpu->op & 0xf));
    cpu->skip = (cpu->acc & 0x10) != 0;
    cpu->acc &= 0xf;
}

static void op_tg(SM5xx *cpu)
{
    cpu->skip = (cpu->gamma & bitmask(cpu->op)) != 0;
    cpu->gamma &= (uint8_t)~bitmask(cpu->op);
}

static void op_keta(SM5xx *cpu) { cpu->acc = read_k(cpu) >> 4 & 0xf; }
static void op_ats_sm530(SM5xx *cpu) { write_s(cpu, cpu->acc); }
static void op_sds(SM5xx *cpu) { cpu->ds = true; }
static void op_rds(SM5xx *cpu) { cpu->ds = false; }

static void op_idiv_sm530(SM5xx *cpu)
{
    cpu->div = cpu->subdiv = 0;
    cpu->count_1s = 0;
}

static void op_inis(SM5xx *cpu) { cpu->count_10ms = 0; }

static void execute_sm510(SM5xx *cpu)
{
    uint8_t op = (uint8_t)cpu->op;
    switch (op & 0xf0) {
    case 0x20: op_lax(cpu); break;
    case 0x30: op_adx(cpu); break;
    case 0x40: op_lb(cpu); break;
    case 0x80: case 0x90: case 0xa0: case 0xb0: op_t(cpu); break;
    case 0xc0: case 0xd0: case 0xe0: case 0xf0: op_tm(cpu); break;
    default:
        switch (op & 0xfc) {
        case 0x04: op_rm(cpu); break;
        case 0x0c: op_sm(cpu); break;
        case 0x10: op_exc(cpu); break;
        case 0x14: op_exci(cpu); break;
        case 0x18: op_lda(cpu); break;
        case 0x1c: op_excd(cpu); break;
        case 0x54: op_tmi(cpu); break;
        case 0x70: case 0x74: case 0x78: op_tl(cpu); break;
        case 0x7c: op_tml(cpu); break;
        default:
            switch (op) {
            case 0x00: break; /* SKIP */
            case 0x01: op_atbp(cpu); break;
            case 0x02: op_sbm(cpu); break;
            case 0x03: op_atpl(cpu); break;
            case 0x08: op_add(cpu); break;
            case 0x09: op_add11(cpu); break;
            case 0x0a: op_coma(cpu); break;
            case 0x0b: op_exbla(cpu); break;
            case 0x51: op_tb(cpu); break;
            case 0x52: op_tc(cpu); break;
            case 0x53: op_tam(cpu); break;
            case 0x58: op_tis(cpu); break;
            case 0x59: op_atl(cpu); break;
            case 0x5a: op_ta0(cpu); break;
            case 0x5b: op_tabl(cpu); break;
            case 0x5d: op_cend(cpu); break;
            case 0x5e: op_tal(cpu); break;
            case 0x5f: op_lbl(cpu); break;
            case 0x60: op_atfc(cpu); break;
            case 0x61: op_atr(cpu); break;
            case 0x62: op_wr(cpu); break;
            case 0x63: op_ws(cpu); break;
            case 0x64: op_incb(cpu); break;
            case 0x65: op_idiv(cpu); break;
            case 0x66: op_rc(cpu); break;
            case 0x67: op_sc(cpu); break;
            case 0x68: op_tf1(cpu); break;
            case 0x69: op_tf4(cpu); break;
            case 0x6a: op_kta(cpu); break;
            case 0x6b: op_rot(cpu); break;
            case 0x6c: op_decb(cpu); break;
            case 0x6d: op_bdc(cpu); break;
            case 0x6e: op_rtn0(cpu); break;
            case 0x6f: op_rtn1(cpu); break;
            default: op_illegal(cpu); break;
            }
            break;
        }
        break;
    }

    /* BM high bit is only valid for 1 step */
    cpu->bmask = (cpu->op == 0x02) ? 0x40 : 0;
}

static void execute_sm511(SM5xx *cpu)
{
    uint8_t op = (uint8_t)cpu->op;
    switch (op & 0xf0) {
    case 0x20: op_lax(cpu); break;
    case 0x30: op_adx(cpu); break;
    case 0x40: op_lb(cpu); break;
    case 0x70: op_tl(cpu); break;
    case 0x80: case 0x90: case 0xa0: case 0xb0: op_t(cpu); break;
    case 0xc0: case 0xd0: case 0xe0: case 0xf0: op_tm(cpu); break;
    default:
        switch (op & 0xfc) {
        case 0x04: op_rm(cpu); break;
        case 0x0c: op_sm(cpu); break;
        case 0x10: op_exc(cpu); break;
        case 0x14: op_exci(cpu); break;
        case 0x18: op_lda(cpu); break;
        case 0x1c: op_excd(cpu); break;
        case 0x54: op_tmi(cpu); break;
        case 0x68: op_tml(cpu); break;
        default:
            switch (op) {
            case 0x00: op_rot(cpu); break;
            case 0x01: op_dta(cpu); break;
            case 0x02: op_sbm(cpu); break;
            case 0x03: op_atpl(cpu); break;
            case 0x08: op_add(cpu); break;
            case 0x09: op_add11(cpu); break;
            case 0x0a: op_coma(cpu); break;
            case 0x0b: op_exbla(cpu); break;
            case 0x50: op_kta(cpu); break;
            case 0x51: op_tb(cpu); break;
            case 0x52: op_tc(cpu); break;
            case 0x53: op_tam(cpu); break;
            case 0x58: op_tis(cpu); break;
            case 0x59: op_atl(cpu); break;
            case 0x5a: op_ta0(cpu); break;
            case 0x5b: op_tabl(cpu); break;
            case 0x5c: op_atx(cpu); break;
            case 0x5d: op_cend(cpu); break;
            case 0x5e: op_tal(cpu); break;
            case 0x5f: op_lbl(cpu); break;
            case 0x61: op_pre(cpu); break;
            case 0x62: op_wr(cpu); break;
            case 0x63: op_ws(cpu); break;
            case 0x64: op_incb(cpu); break;
            case 0x65: op_idiv(cpu); break;
            case 0x66: op_rc(cpu); break;
            case 0x67: op_sc(cpu); break;
            case 0x6c: op_decb(cpu); break;
            case 0x6d: op_ptw(cpu); break;
            case 0x6e: op_rtn0(cpu); break;
            case 0x6f: op_rtn1(cpu); break;
            case 0x60:
                cpu->op = (uint16_t)(cpu->op << 8 | cpu->param);
                switch (cpu->param) {
                case 0x30: op_rme(cpu); break;
                case 0x31: op_sme(cpu); break;
                case 0x32: op_tmel(cpu); break;
                case 0x33: op_atfc(cpu); break;
                case 0x34: op_bdc(cpu); break;
                case 0x35: op_atbp(cpu); break;
                case 0x36: op_clkhi(cpu); break;
                case 0x37: op_clklo(cpu); break;
                default: op_illegal(cpu); break;
                }
                break;
            default: op_illegal(cpu); break;
            }
            break;
        }
        break;
    }

    /* BM high bit is only valid for 1 step */
    cpu->bmask = (cpu->op == 0x02) ? 0x40 : 0;
}

static void execute_sm530(SM5xx *cpu)
{
    uint8_t op = (uint8_t)cpu->op;
    switch (op & 0xf0) {
    case 0x00: op_adx_sm530(cpu); break;
    case 0x10: op_lax(cpu); break;
    case 0x30: op_lb_sm530(cpu); break;
    case 0x80: case 0x90: case 0xa0: case 0xb0: op_t(cpu); break; /* TR */
    case 0xc0: case 0xd0: case 0xe0: case 0xf0: op_trs_sm530(cpu); break;
    default:
        switch (op & 0xfc) {
        case 0x20: op_lda(cpu); break;
        case 0x24: op_exc(cpu); break;
        case 0x28: op_exci(cpu); break;
        case 0x2c: op_excd(cpu); break;
        case 0x40: op_rm(cpu); break;
        case 0x44: op_sm(cpu); break;
        case 0x48: op_tmi(cpu); break; /* TM */
        case 0x60: case 0x64: op_tl_sm530(cpu); break;
        case 0x6c: op_tg(cpu); break;
        default:
            switch (op) {
            case 0x4c: op_incb(cpu); break;
            case 0x4d: op_decb(cpu); break;
            case 0x4e: op_rds(cpu); break;
            case 0x4f: op_sds(cpu); break;
            case 0x50: op_kta(cpu); break;
            case 0x51: op_keta(cpu); break;
            case 0x52: op_dta_sm530(cpu); break;
            case 0x53: op_coma(cpu); break;
            case 0x54: op_add(cpu); break;
            case 0x55: op_add11(cpu); break; /* ADDC */
            case 0x56: op_rc(cpu); break;
            case 0x57: op_sc(cpu); break;
            case 0x58: op_tabl(cpu); break;
            case 0x59: op_tam(cpu); break;
            case 0x5a: op_exbla(cpu); break; /* EXBL */
            case 0x5b: op_tc(cpu); break;
            case 0x5c: op_ats_sm530(cpu); break;
            case 0x5d: break;                /* ATF: F port not connected */
            case 0x5e: op_atbp(cpu); break;
            case 0x68: op_rtn0(cpu); break;  /* RTN */
            case 0x69: op_rtn1(cpu); break;  /* RTNS */
            case 0x6a: op_atpl(cpu); break;
            case 0x6b: op_lbl(cpu); break;
            case 0x70: op_idiv_sm530(cpu); break;
            case 0x71: op_inis(cpu); break;
            case 0x72: op_sbm(cpu); break;   /* SABM */
            case 0x73: break;                /* SABL */
            case 0x74: op_cend(cpu); break;
            case 0x75: op_tmel(cpu); break;
            case 0x76: op_rme(cpu); break;
            case 0x77: op_sme(cpu); break;
            case 0x78: op_pre(cpu); break;
            case 0x79: op_tal(cpu); break;   /* TBA */
            default: op_illegal(cpu); break;
            }
            break;
        }
        break;
    }

    /* SABM/SABL is only valid for 1 step */
    cpu->bmask = (cpu->op == 0x72) ? 0x40 : ((cpu->op == 0x73) ? 0x08 : 0);
}

static void execute_sm500(SM5xx *cpu)
{
    uint8_t op = (uint8_t)cpu->op;
    switch (op & 0xf0) {
    case 0x20: op_lax(cpu); break;
    case 0x30:
        if (cpu->variant == SM5XX_SM500 && op == 0x30)
            op_ats(cpu);
        else
            op_adx(cpu);
        break;
    case 0x40: op_lb(cpu); break;
    case 0x70: op_ssr(cpu); break;
    case 0x80: case 0x90: case 0xa0: case 0xb0: op_tr(cpu); break;
    case 0xc0: case 0xd0: case 0xe0: case 0xf0: op_trs(cpu); break;
    default:
        switch (op & 0xfc) {
        case 0x04: op_rm(cpu); break;
        case 0x0c: op_sm(cpu); break;
        case 0x10: op_exc(cpu); break;
        case 0x14: op_exci(cpu); break;
        case 0x18: op_lda(cpu); break;
        case 0x1c: op_excd(cpu); break;
        case 0x54: op_tmi(cpu); break;
        default:
            switch (op) {
            case 0x00: break; /* SKIP */
            case 0x01: op_atr(cpu); break;
            case 0x02:
                /* SM500: EXKSA (not implemented upstream), SM5A: SBM */
                if (cpu->variant == SM5XX_SM5A)
                    op_sbm(cpu);
                break;
            case 0x03: op_atbp(cpu); break;
            case 0x08: op_add(cpu); break;
            case 0x09: op_add11(cpu); break;
            case 0x0a: op_coma(cpu); break;
            case 0x0b: op_exbla(cpu); break;
            case 0x50: op_tal(cpu); break;
            case 0x51: op_tb(cpu); break;
            case 0x52: op_tc(cpu); break;
            case 0x53: op_tam(cpu); break;
            case 0x58: op_tis(cpu); break;
            case 0x59: op_ptw(cpu); break;
            case 0x5a: op_ta0(cpu); break;
            case 0x5b: op_tabl(cpu); break;
            case 0x5c: op_tw(cpu); break;
            case 0x5d: op_dtw(cpu); break;
            case 0x5f: op_lbl(cpu); break;
            case 0x60: op_comcn(cpu); break;
            case 0x61: op_pdtw(cpu); break;
            case 0x62: op_wr(cpu); break;
            case 0x63: op_ws(cpu); break;
            case 0x64: op_incb(cpu); break;
            case 0x65: op_idiv(cpu); break;
            case 0x66: op_rc(cpu); break;
            case 0x67: op_sc(cpu); break;
            case 0x68: op_rmf(cpu); break;
            case 0x69: op_smf(cpu); break;
            case 0x6a: op_kta(cpu); break;
            case 0x6b:
                /* SM500: EXKFA (not implemented upstream), SM5A: RBM */
                if (cpu->variant == SM5XX_SM5A)
                    op_rbm(cpu);
                break;
            case 0x6c: op_decb(cpu); break;
            case 0x6d: op_comcb(cpu); break;
            case 0x6e: op_rtn0(cpu); break;
            case 0x6f: op_rtn1(cpu); break;
            case 0x5e:
                cpu->op = (uint16_t)(cpu->op << 8 | cpu->param);
                switch (cpu->param) {
                case 0x00: op_cend(cpu); break;
                case 0x04: op_dta(cpu); break;
                default: op_illegal(cpu); break;
                }
                break;
            default: op_illegal(cpu); break;
            }
            break;
        }
        break;
    }
}

static bool op_argument(const SM5xx *cpu)
{
    if (is_sm530(cpu))
        return cpu->op == 0x6b || cpu->op == 0x78 || (cpu->op & 0xf8) == 0x60;
    if (is_sm511(cpu))
        return (cpu->op >= 0x5f && cpu->op <= 0x61) || (cpu->op & 0xf0) == 0x70 || (cpu->op & 0xfc) == 0x68;
    if (is_sm500(cpu))
        return cpu->op == 0x5e || cpu->op == 0x5f;
    return cpu->op == 0x5f || (cpu->op & 0xf0) == 0x70;
}

int sm5xx_init(SM5xx *cpu, SM5xxVariant variant, const uint8_t *rom, size_t rom_size, const SM5xxBus *bus,
               int r_mask_option)
{
    if (!cpu || !rom)
        return -1;
    memset(cpu, 0, sizeof(*cpu));
    cpu->variant = variant;
    cpu->rom = rom;
    cpu->rom_size = rom_size;
    if (bus)
        cpu->bus = *bus;
    cpu->r_mask_option = r_mask_option;
    switch (variant) {
    case SM5XX_SM500:
        cpu->stack_levels = 1;
        cpu->o_pins = 7;
        cpu->prgwidth = 11;
        cpu->datawidth = 6;
        break;
    case SM5XX_SM5A:
        cpu->stack_levels = 1;
        cpu->o_pins = 9;
        cpu->prgwidth = 11;
        cpu->datawidth = 7;
        break;
    case SM5XX_SM510:
    case SM5XX_SM511:
    case SM5XX_SM512:
        cpu->stack_levels = 2;
        cpu->o_pins = 0;
        cpu->prgwidth = 12;
        cpu->datawidth = 7;
        break;
    case SM5XX_SM530:
        cpu->stack_levels = 1;
        cpu->o_pins = 0;
        cpu->prgwidth = 11;
        cpu->datawidth = 7;
        break;
    default:
        return -1;
    }
    cpu->prgmask = (1 << cpu->prgwidth) - 1;
    cpu->datamask = (1 << cpu->datawidth) - 1;
    cpu->pagemask = 0x3f;
    sm5xx_power_on(cpu);
    return 0;
}

void sm5xx_power_on(SM5xx *cpu)
{
    /* device_start zero fill, then device_reset; RAM content is undefined on
       real hardware and zero here, matching MAME's initial state */
    memset(cpu->stack, 0, sizeof(cpu->stack));
    cpu->pc = cpu->prev_pc = 0;
    cpu->op = cpu->prev_op = 0;
    cpu->param = 0;
    cpu->acc = cpu->bl = cpu->bm = cpu->bmask = cpu->c = 0;
    cpu->skip = false;
    cpu->w = cpu->r = cpu->r_out = 0;
    cpu->div = 0;
    cpu->gamma = 0;
    cpu->ext_wakeup = false;
    cpu->l = cpu->x = cpu->y = cpu->bp = 0;
    cpu->bc = false;
    cpu->halt = false;
    cpu->clk_div = 2;
    memset(cpu->ox, 0, sizeof(cpu->ox));
    memset(cpu->o, 0, sizeof(cpu->o));
    cpu->mx = cpu->cb = cpu->s = 0;
    cpu->rsub = false;
    cpu->melody_rd = cpu->melody_step_count = cpu->melody_duty_count = cpu->melody_duty_index = 0;
    cpu->melody_address = 0;
    cpu->subdiv = 0;
    cpu->count_1s = cpu->count_10ms = 0;
    cpu->ds = false;
    memset(cpu->ram, 0, sizeof(cpu->ram));
    cpu->icount = 0;
    cpu->illegal_count = 0;
    sm5xx_reset(cpu);
}

void sm5xx_reset(SM5xx *cpu)
{
    /* ACL */
    cpu->skip = false;
    cpu->halt = false;
    cpu->bmask = 0;
    cpu->op = cpu->prev_op = 0;
    reset_vector(cpu);
    cpu->prev_pc = cpu->pc;

    /* lcd is on (Bp on, BC off, bs(y) off) */
    cpu->bp = 1;
    cpu->bc = false;
    cpu->y = 0;

    cpu->r = cpu->r_out = 0;
    write_r(cpu, 0);

    if (has_melody(cpu)) {
        cpu->melody_rd &= (uint8_t)~1;
        cpu->clk_div = 4; /* 8kHz */
    }

    if (is_sm530(cpu)) {
        cpu->clk_div = 3; /* 3 oscillator clocks per machine cycle */
        cpu->bp = 0;
        cpu->ds = true;
    }

    if (is_sm500(cpu)) {
        push_stack(cpu);
        op_idiv(cpu);
        cpu->gamma = 1;
        cpu->cb = 0;
        cpu->rsub = false;
        cpu->r = 0xf;
    }
}

void sm5xx_set_wakeup(SM5xx *cpu, bool state) { cpu->ext_wakeup = state; }

void sm5xx_set_melody(SM5xx *cpu, const uint8_t *melody) { cpu->melody = melody; }

void sm5xx_divider_tick(SM5xx *cpu)
{
    cpu->div = (cpu->div + 1) & 0x7fff;

    if (is_sm530(cpu)) {
        if (cpu->div == 0) {
            /* gamma on 1s, and on 10s from the seconds counter */
            cpu->gamma |= 2;
            cpu->count_1s = (cpu->count_1s + 1) % 10;
            if (cpu->count_1s == 0)
                cpu->gamma |= 1;
        }
        /* gamma on 0.5s */
        if ((cpu->div & 0x3fff) == 0)
            cpu->gamma |= 4;
        /* secondary timer for 1/100s intervals */
        if ((cpu->div & 0xff) < 250) {
            cpu->subdiv = (cpu->subdiv + 1) % 32000;
            if ((cpu->subdiv % 320) == 0) {
                cpu->count_10ms = (cpu->count_10ms + 1) % 10;
                /* gamma on 0.1s */
                if (cpu->count_10ms == 0)
                    cpu->gamma |= 8;
            }
        }
        clock_melody(cpu);
        return;
    }

    /* 1S(gamma) signal on overflow (falling edge of F1) */
    if (cpu->div == 0)
        cpu->gamma |= 1;

    clock_melody(cpu);
}

static uint16_t get_lcd_row(const SM5xx *cpu, int column, int base)
{
    uint16_t rowdata = 0;
    int i;
    if (base < 0 || cpu->bc || !(cpu->bp & 1))
        return 0;
    for (i = 0; i < 0x10; i++)
        rowdata |= (uint16_t)((cpu->ram[base + i] >> column & 1) << i);
    return rowdata;
}

void sm5xx_lcd_update(SM5xx *cpu)
{
    int h, o;
    if (is_sm530(cpu)) {
        /* 2 rows of 12 groups, 4 segments per group */
        for (h = 0; h < 2; h++)
            for (o = 0; o < 12; o++)
                write_segs(cpu, (unsigned)(o << 1 | h), cpu->ds ? cpu->ram[(h ? 0x50 : 0x40) + o] & 0xf : 0);
        return;
    }
    if (is_sm500(cpu)) {
        for (h = 0; h < 2; h++)
            for (o = 0; o < cpu->o_pins; o++) {
                uint8_t seg = h ? cpu->ox[o] : cpu->o[o];
                write_segs(cpu, (unsigned)(o << 1 | h), (cpu->bp & 1) ? seg : 0);
            }
        return;
    }
    for (h = 0; h < 4; h++) {
        uint8_t blink, bs;
        write_segs(cpu, (unsigned)h | SM5XX_SEG_A, get_lcd_row(cpu, h, 0x60));
        write_segs(cpu, (unsigned)h | SM5XX_SEG_B, get_lcd_row(cpu, h, 0x70));
        write_segs(cpu, (unsigned)h | SM5XX_SEG_C, get_lcd_row(cpu, h, cpu->variant == SM5XX_SM512 ? 0x50 : -1));
        blink = (cpu->div & 0x4000) ? cpu->y : 0;
        bs = (uint8_t)(((cpu->l & ~blink) >> h & 1) | ((cpu->x * 2) >> h & 2));
        write_segs(cpu, (unsigned)h | SM5XX_SEG_BS, (cpu->bc || !(cpu->bp & 1)) ? 0 : bs);
    }
}

static void do_interrupt(SM5xx *cpu)
{
    cpu->icount--;
    cpu->halt = false;
    wakeup_vector(cpu);
}

void sm5xx_run(SM5xx *cpu, int cycles)
{
    cpu->icount += cycles;
    while (cpu->icount > 0) {
        /* in halt mode, wake up after gamma signal or K input */
        if (cpu->halt) {
            if (cpu->ext_wakeup || cpu->gamma) {
                do_interrupt(cpu);
            } else {
                cpu->icount = 0;
                return;
            }
        }

        cpu->icount--;
        cpu->prev_op = cpu->op;
        cpu->prev_pc = cpu->pc;

        cpu->op = sm5xx_rom_read(cpu, cpu->pc);
        increment_pc(cpu);

        if (op_argument(cpu)) {
            cpu->icount--;
            cpu->param = sm5xx_rom_read(cpu, cpu->pc);
            increment_pc(cpu);
        }

        if (cpu->skip) {
            cpu->skip = false;
            cpu->op = 0; /* fake nop */
        } else if (is_sm530(cpu)) {
            execute_sm530(cpu);
        } else if (is_sm500(cpu)) {
            execute_sm500(cpu);
        } else if (is_sm511(cpu)) {
            execute_sm511(cpu);
        } else {
            execute_sm510(cpu);
        }
    }
}
