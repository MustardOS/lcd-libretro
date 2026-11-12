// license:BSD-3-Clause
// copyright-holders:hap, Henrik Algestam
/*
  Board level emulation adapted from MAME 0.289 src/mame/handheld/hh_sm510.cpp:
  input multiplexing, K line wakeup, piezo wiring, LCD segment latching and the
  1kHz display decay (deflicker) model. MAME timers are replaced by a fixed
  32.768kHz oscillator tick loop.
*/

#include "machine.h"
#include <string.h>

static uint8_t input_r(const LCDMachine *m)
{
    const LCDMachineDef *d = m->def;
    uint8_t ret = 0;
    int i;
    for (i = 0; i < d->inp_lines; i++)
        if (m->inp_mux >> i & 1)
            ret |= m->ports[i];
    if (d->inp_fixed >= 0)
        ret |= m->ports[d->inp_fixed];
    return ret;
}

static void update_k_line(LCDMachine *m) { sm5xx_set_wakeup(&m->cpu, input_r(m) != 0); }

static void input_w(LCDMachine *m, uint8_t data)
{
    if (m->def->quirks & LCD_QUIRK_S5_S6_TIED)
        data = (uint8_t)((data & 0x1f) | (data >> 1 & 0x10));
    m->inp_mux = data;
    update_k_line(m);
}

static bool sample_present(const LCDMachine *m, unsigned index)
{
    return m->samples && index < m->samples->count && m->samples->data[index] && m->samples->length[index];
}

/* ktmnt2_state::sound_w: S8 low resets, a falling S7 latches the sample number from S1-S5 */
static void adpcm_w(LCDMachine *m, uint8_t data)
{
    if (~data & 0x80) {
        m->adpcm_sample = -1;
    } else if (~data & m->inp_mux & 0x40) {
        unsigned sample = data & 0x1f;
        if (m->adpcm_sample >= 0) {
            if (sample == 0)
                m->adpcm_sample = -1;
        } else if (sample != 0 && sample_present(m, sample - 1u)) {
            m->adpcm_sample = (int8_t)(sample - 1u);
            m->adpcm_pos = 0;
        }
    }
}

static uint8_t bus_read_k(void *user) { return input_r(user); }
static int bus_read_ba(void *user) { return ((LCDMachine *)user)->ba; }

static int bus_read_b(void *user)
{
    const LCDMachine *m = user;
    if (m->def->quirks & LCD_QUIRK_ADPCM)
        return m->adpcm_sample >= 0 ? 0 : 1;
    return m->b;
}

static void bus_write_s(void *user, uint8_t data)
{
    LCDMachine *m = user;
    if (m->def->quirks & LCD_QUIRK_ADPCM)
        adpcm_w(m, data);
    if (m->def->wiring == LCD_WIRING_SM511_TIGER2BIT) {
        /* S1 (+R1) to piezo, the rest to the input mux */
        m->speaker = (uint8_t)((m->speaker & ~2) | (data << 1 & 2));
        input_w(m, data >> 1);
    } else if (m->def->wiring != LCD_WIRING_SM5A_R1_PIEZO_MUX) {
        input_w(m, data);
    }
}

static void bus_write_r(void *user, uint8_t data)
{
    LCDMachine *m = user;
    switch (m->def->wiring) {
    case LCD_WIRING_SM511_TIGER2BIT:
        m->speaker = (uint8_t)((m->speaker & ~1) | (data & 1));
        break;
    case LCD_WIRING_SM510_S_MUX_R2:
        m->speaker = data >> 1 & 1;
        break;
    case LCD_WIRING_SM5A_R1_PIEZO_MUX:
        m->speaker = data & 1;
        input_w(m, data >> 1);
        break;
    default:
        m->speaker = data & 1;
        break;
    }
}

static void bus_write_segs(void *user, unsigned offset, uint16_t data)
{
    LCDMachine *m = user;
    if (offset < LCD_ZX)
        m->display_state[offset] = data;
}

static void set_display_size(LCDMachine *m)
{
    if (m->def->cpu != SM5XX_SM5A && m->def->cpu != SM5XX_SM500 && m->def->cpu != SM5XX_SM530) {
        m->display_x_len = 2;
        m->display_y_len = 16;
        m->display_z_len = 2;
    } else {
        m->display_x_len = 4;
        m->display_y_len = 4;
        m->display_z_len = 1;
    }
}

static void update_display(LCDMachine *m)
{
    unsigned zx_len = 1u << (m->display_x_len + m->display_z_len);
    unsigned limit = (unsigned)m->decay_pivot + m->decay_len;
    unsigned zx, y;
    for (zx = 0; zx < zx_len && zx < LCD_ZX; zx++)
        for (y = 0; y < m->display_y_len && y < LCD_SEGS; y++) {
            uint8_t *decay = &m->display_decay[y][zx];
            if (m->display_state[zx] >> y & 1) {
                if (*decay < limit)
                    (*decay)++;
            } else if (*decay > 0) {
                (*decay)--;
            }
        }
}

int lcd_machine_init(LCDMachine *m, const LCDMachineDef *def, const uint8_t *rom, size_t rom_size)
{
    SM5xxBus bus;
    if (!m || !def || !rom || rom_size != def->rom.size || def->region_size > sizeof(m->rom))
        return -1;
    memset(m, 0, sizeof(*m));
    m->def = def;
    memcpy(m->rom, rom, rom_size);
    bus.read_k = bus_read_k;
    bus.read_ba = bus_read_ba;
    bus.read_b = bus_read_b;
    bus.write_s = bus_write_s;
    bus.write_r = bus_write_r;
    bus.write_segs = bus_write_segs;
    bus.user = m;
    if (sm5xx_init(&m->cpu, def->cpu, m->rom, def->region_size, &bus, def->r_mask_option) != 0)
        return -1;
    lcd_machine_power_on(m);
    return 0;
}

void lcd_machine_clone(LCDMachine *dst, const LCDMachine *src)
{
    *dst = *src;
    dst->cpu.rom = dst->rom;
    if (src->cpu.melody)
        dst->cpu.melody = dst->melody;
    dst->cpu.bus.user = dst;
}

void lcd_machine_attach_melody(LCDMachine *m, const uint8_t *melody, size_t size)
{
    if (!melody || size != sizeof(m->melody))
        return;
    memcpy(m->melody, melody, sizeof(m->melody));
    sm5xx_set_melody(&m->cpu, m->melody);
    lcd_machine_power_on(m);
}

void lcd_machine_power_on(LCDMachine *m)
{
    m->inp_mux = 0;
    m->speaker = 0;
    m->acl = false;
    m->decay_pivot = m->def->decay_pivot ? m->def->decay_pivot : 8;
    m->decay_len = m->def->decay_len ? m->def->decay_len : 17;
    memset(m->display_state, 0, sizeof(m->display_state));
    memset(m->display_decay, 0, sizeof(m->display_decay));
    m->clock_phase = 0;
    m->strobe_phase = 0;
    m->dial = m->dial_phase = 0;
    m->adpcm_sample = -1;
    m->adpcm_pos = 0;
    set_display_size(m);
    lcd_machine_set_buttons(m, 0);
    sm5xx_power_on(&m->cpu);
    update_k_line(m);
}

void lcd_machine_soft_reset(LCDMachine *m)
{
    sm5xx_reset(&m->cpu);
    update_k_line(m);
}

void lcd_machine_attach_samples(LCDMachine *m, const LCDSampleBank *samples)
{
    m->samples = samples;
    if (m->adpcm_sample >= 0 && !sample_present(m, (unsigned)m->adpcm_sample))
        m->adpcm_sample = -1;
}

void lcd_machine_set_buttons(LCDMachine *m, uint32_t buttons) { lcd_machine_set_input(m, buttons, NULL); }

static unsigned mask_shift(uint8_t mask)
{
    unsigned s = 0;
    while (mask && !(mask >> s & 1))
        s++;
    return s;
}

void lcd_machine_set_input(LCDMachine *m, uint32_t buttons, const uint8_t *keys)
{
    const LCDMachineDef *d = m->def;
    const LCDInputDef *in;
    bool acl = false;
    memcpy(m->ports, d->in_rest, sizeof(m->ports));
    m->ba = d->ba_rest < 0 ? 1 : (uint8_t)d->ba_rest;
    m->b = d->b_rest < 0 ? 1 : (uint8_t)d->b_rest;
    for (in = d->inputs; in < d->inputs + LCD_MAX_INPUTS && in->label; ++in) {
        uint8_t *port;
        bool held = (in->retro_id < 32u && (buttons >> in->retro_id & 1)) || (keys && keys[in - d->inputs]);
        if (in->dial && in->port < LCD_IN_PORTS) {
            /* PORT_SENSITIVITY(10) PORT_KEYDELTA(2): one step every 5 frames while held */
            if (held && ++m->dial_phase >= 5u) {
                m->dial_phase = 0;
                m->dial = (uint8_t)((m->dial + 1u) & (in->mask >> mask_shift(in->mask)));
            }
            m->ports[in->port] |= (uint8_t)(m->dial << mask_shift(in->mask) & in->mask);
            continue;
        }
        if (!held)
            continue;
        if (in->port == LCD_PORT_ACL) {
            acl = true;
            continue;
        }
        port = in->port == LCD_PORT_BA ? &m->ba : in->port == LCD_PORT_B ? &m->b : &m->ports[in->port];
        if (in->active_low)
            *port &= (uint8_t)~in->mask;
        else
            *port |= in->mask;
    }

    /* ACL is tied to the reset line: reset on assert, held while asserted */
    if (acl && !m->acl)
        sm5xx_reset(&m->cpu);
    m->acl = acl;
    update_k_line(m);
}

void lcd_machine_run(LCDMachine *m, unsigned ticks, uint8_t *levels) { lcd_machine_run_pcm(m, ticks, levels, NULL); }

static int16_t adpcm_tick(LCDMachine *m)
{
    unsigned s = (unsigned)m->adpcm_sample;
    uint32_t len, ipos, frac;
    const int16_t *data;
    int32_t a, b;
    if (m->adpcm_sample < 0 || !sample_present(m, s)) {
        m->adpcm_sample = -1;
        return 0;
    }
    data = m->samples->data[s];
    len = m->samples->length[s];
    ipos = m->adpcm_pos >> 16;
    if (ipos >= len) {
        m->adpcm_sample = -1;
        return 0;
    }
    /* linear interpolation as samples_device::sound_stream_update */
    frac = m->adpcm_pos & 0xffffu;
    a = data[ipos];
    b = data[(ipos + 1u) % len];
    m->adpcm_pos += (uint32_t)(((uint64_t)m->samples->rate[s] << 16) / LCD_OSC_HZ);
    if ((m->adpcm_pos >> 16) >= len)
        m->adpcm_sample = -1;
    return (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
}

void lcd_machine_run_pcm(LCDMachine *m, unsigned ticks, uint8_t *levels, int16_t *pcm)
{
    unsigned i;
    for (i = 0; i < ticks; ++i) {
        if (++m->clock_phase >= m->cpu.clk_div) {
            m->clock_phase = 0;
            if (!m->acl)
                sm5xx_run(&m->cpu, 1);
        }
        sm5xx_divider_tick(&m->cpu);
        if (++m->strobe_phase >= LCD_DECAY_DIVIDER) {
            m->strobe_phase = 0;
            sm5xx_lcd_update(&m->cpu);
            update_display(m);
        }
        if (levels)
            levels[i] = m->def->wiring == LCD_WIRING_SM511_TIGER2BIT ? (uint8_t)(m->speaker * 85u)
                                                                    : (uint8_t)(m->speaker ? 255u : 0u);
        if (m->adpcm_sample >= 0 || pcm) {
            int16_t v = m->adpcm_sample >= 0 ? adpcm_tick(m) : 0;
            if (pcm)
                pcm[i] = v;
        }
    }
}

bool lcd_machine_boot_to_clock(LCDMachine *m)
{
    const LCDInputDef *in;
    uint32_t time_key = 0;
    for (in = m->def->inputs; in < m->def->inputs + LCD_MAX_INPUTS && in->label; ++in)
        if (!strcmp(in->label, "Time") && in->retro_id < 32u)
            time_key = UINT32_C(1) << in->retro_id;
    if (!time_key)
        return false;
    lcd_machine_run(m, LCD_OSC_HZ, NULL);
    lcd_machine_set_buttons(m, time_key);
    lcd_machine_run(m, LCD_OSC_HZ / 6u, NULL);
    lcd_machine_set_buttons(m, 0);
    lcd_machine_run(m, LCD_OSC_HZ / 3u, NULL);
    return true;
}

bool lcd_machine_set_clock(LCDMachine *m, unsigned hour24, unsigned minute)
{
    const LCDRtcDef *r = lcd_registry_rtc(m->def->id);
    if (!r || !r->present || hour24 > 23u || minute > 59u)
        return false;
    lcd_rtc_write(&m->cpu, r, hour24, minute);
    return true;
}

void lcd_rtc_write(SM5xx *cpu, const LCDRtcDef *r, unsigned hour24, unsigned minute)
{
    unsigned h = r->h24 ? hour24 : (hour24 % 12u ? hour24 % 12u : 12u);
    bool pm = hour24 >= 12u;
    sm5xx_ram_write(cpu, r->h10, (uint8_t)(r->base | h / 10u | (pm ? r->pm : r->am)));
    sm5xx_ram_write(cpu, r->h1, (uint8_t)(h % 10u));
    if (r->ap != 0xffu)
        sm5xx_ram_write(cpu, r->ap, pm ? r->ap_pm : r->ap_am);
    sm5xx_ram_write(cpu, r->m10, (uint8_t)(minute / 10u));
    sm5xx_ram_write(cpu, r->m1, (uint8_t)(minute % 10u));
}

static const uint8_t *decay_at(const LCDMachine *m, unsigned x, unsigned y, unsigned z)
{
    unsigned zx = x << m->display_z_len | z;
    if (z >= (1u << m->display_z_len) || y >= m->display_y_len || zx >= LCD_ZX ||
        zx >= (1u << (m->display_x_len + m->display_z_len)))
        return NULL;
    return &m->display_decay[y][zx];
}

bool lcd_machine_segment_active(const LCDMachine *m, unsigned x, unsigned y, unsigned z)
{
    const uint8_t *d = decay_at(m, x, y, z);
    return d && *d >= m->decay_pivot;
}

uint8_t lcd_machine_segment_level(const LCDMachine *m, unsigned x, unsigned y, unsigned z, bool fade)
{
    const uint8_t *d = decay_at(m, x, y, z);
    unsigned limit = (unsigned)m->decay_pivot + m->decay_len;
    if (!d)
        return 0;
    if (!fade)
        return *d >= m->decay_pivot ? 255 : 0;
    return (uint8_t)(*d * 255u / limit);
}

/* save states: explicit field order, little endian, no pointers or padding */

typedef struct {
    uint8_t *w;
    const uint8_t *r;
    size_t pos, cap;
    bool ok;
} Ser;

static void ser_bytes(Ser *s, void *v, size_t n)
{
    if (!s->ok || s->pos + n > s->cap) {
        s->ok = false;
        return;
    }
    if (s->w)
        memcpy(s->w + s->pos, v, n);
    else if (s->r)
        memcpy(v, s->r + s->pos, n);
    s->pos += n;
}

static void ser_u8(Ser *s, uint8_t *v) { ser_bytes(s, v, 1); }

static void ser_bool(Ser *s, bool *v)
{
    uint8_t b = *v ? 1 : 0;
    ser_u8(s, &b);
    *v = b != 0;
}

static void ser_u16(Ser *s, uint16_t *v)
{
    uint8_t b[2] = {(uint8_t)*v, (uint8_t)(*v >> 8)};
    ser_bytes(s, b, 2);
    *v = (uint16_t)(b[0] | b[1] << 8);
}

static void ser_u32(Ser *s, uint32_t *v)
{
    uint8_t b[4] = {(uint8_t)*v, (uint8_t)(*v >> 8), (uint8_t)(*v >> 16), (uint8_t)(*v >> 24)};
    ser_bytes(s, b, 4);
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static void ser_i32(Ser *s, int *v)
{
    uint32_t u = (uint32_t)*v;
    ser_u32(s, &u);
    *v = (int)u;
}

static void ser_machine(Ser *s, LCDMachine *m)
{
    SM5xx *c = &m->cpu;
    unsigned i;
    ser_i32(s, &c->icount);
    ser_u16(s, &c->pc);
    ser_u16(s, &c->prev_pc);
    ser_u16(s, &c->op);
    ser_u16(s, &c->prev_op);
    ser_u8(s, &c->param);
    for (i = 0; i < 4u; ++i)
        ser_u16(s, &c->stack[i]);
    ser_u8(s, &c->acc);
    ser_u8(s, &c->bl);
    ser_u8(s, &c->bm);
    ser_u8(s, &c->bmask);
    ser_u8(s, &c->c);
    ser_bool(s, &c->skip);
    ser_u8(s, &c->w);
    ser_u8(s, &c->r);
    ser_u8(s, &c->r_out);
    ser_bool(s, &c->ext_wakeup);
    ser_bool(s, &c->halt);
    ser_i32(s, &c->clk_div);
    ser_u8(s, &c->l);
    ser_u8(s, &c->x);
    ser_u8(s, &c->y);
    ser_u8(s, &c->bp);
    ser_bool(s, &c->bc);
    ser_u16(s, &c->div);
    ser_u8(s, &c->gamma);
    ser_bytes(s, c->ox, sizeof(c->ox));
    ser_bytes(s, c->o, sizeof(c->o));
    ser_u8(s, &c->mx);
    ser_u8(s, &c->cb);
    ser_u8(s, &c->s);
    ser_bool(s, &c->rsub);
    ser_u8(s, &c->melody_rd);
    ser_u8(s, &c->melody_step_count);
    ser_u8(s, &c->melody_duty_count);
    ser_u8(s, &c->melody_duty_index);
    ser_u8(s, &c->melody_address);
    ser_u16(s, &c->subdiv);
    ser_u8(s, &c->count_1s);
    ser_u8(s, &c->count_10ms);
    ser_bool(s, &c->ds);
    ser_bytes(s, c->ram, sizeof(c->ram));
    ser_u32(s, &c->illegal_count);

    ser_u16(s, &m->inp_mux);
    ser_bytes(s, m->ports, sizeof(m->ports));
    ser_u8(s, &m->ba);
    ser_u8(s, &m->b);
    ser_bool(s, &m->acl);
    ser_u8(s, &m->speaker);
    ser_u8(s, &m->display_x_len);
    ser_u8(s, &m->display_y_len);
    ser_u8(s, &m->display_z_len);
    ser_u8(s, &m->decay_pivot);
    ser_u8(s, &m->decay_len);
    for (i = 0; i < LCD_ZX; ++i)
        ser_u32(s, &m->display_state[i]);
    ser_bytes(s, m->display_decay, sizeof(m->display_decay));
    ser_u8(s, &m->clock_phase);
    ser_u8(s, &m->strobe_phase);
    ser_u8(s, &m->dial);
    ser_u8(s, &m->dial_phase);
    ser_bytes(s, &m->adpcm_sample, 1);
    ser_u32(s, &m->adpcm_pos);
}

size_t lcd_machine_state_size(void)
{
    static LCDMachine scratch;
    Ser s = {NULL, NULL, 0, (size_t)-1, true};
    ser_machine(&s, &scratch);
    return s.pos;
}

bool lcd_machine_save(const LCDMachine *m, uint8_t *dst, size_t size)
{
    LCDMachine copy = *m;
    Ser s = {dst, NULL, 0, size, true};
    if (!dst)
        return false;
    ser_machine(&s, &copy);
    return s.ok;
}

bool lcd_machine_load(LCDMachine *m, const uint8_t *src, size_t size)
{
    LCDMachine copy = *m;
    Ser s = {NULL, src, 0, size, true};
    if (!src)
        return false;
    ser_machine(&s, &copy);
    if (!s.ok || copy.cpu.clk_div < 1 || copy.cpu.clk_div > 4 || copy.clock_phase >= copy.cpu.clk_div ||
        copy.strobe_phase >= LCD_DECAY_DIVIDER || copy.decay_pivot != m->decay_pivot ||
        copy.decay_len != m->decay_len || copy.display_x_len != m->display_x_len ||
        copy.display_y_len != m->display_y_len || copy.display_z_len != m->display_z_len ||
        copy.cpu.acc > 0xf || copy.cpu.bl > 0xf || copy.cpu.bm > 7 || copy.cpu.c > 1 ||
        copy.cpu.bmask > 0x40 || copy.cpu.mx > 1 || copy.cpu.cb > 1)
        return false;
    *m = copy;
    if (m->adpcm_sample >= 0 && !sample_present(m, (unsigned)m->adpcm_sample))
        m->adpcm_sample = -1;
    return true;
}
