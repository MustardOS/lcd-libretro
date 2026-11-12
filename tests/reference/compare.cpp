// Lockstep differential test: MAME 0.289 SM5xx reference vs the C11 port.
// Random programs, random inputs, wakeups and resets; every architectural
// register, RAM nibble and output pin event is compared after every tick.
#include "emu.h"
#include "sm510.h"
#include "sm500.h"
#include "sm511.h"
#include "sm5a.h"
#include "sm530.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <set>
#include <type_traits>
extern "C" {
#include "cpu/sm5xx.h"
}

struct Pins {
    u8 k = 0;
    int ba = 1, b = 1;
    std::vector<u32> events;
};

static Pins ref_pins, port_pins;
static std::set<unsigned> coverage[6];
static unsigned long halts[6], wakeups[6];

static uint8_t c_read_k(void *) { return port_pins.k; }
static int c_read_ba(void *) { return port_pins.ba; }
static int c_read_b(void *) { return port_pins.b; }
static void c_write_s(void *, uint8_t d) { port_pins.events.push_back(0x10000u | d); }
static void c_write_r(void *, uint8_t d) { port_pins.events.push_back(0x20000u | d); }
static void c_write_segs(void *, unsigned o, uint16_t d) { port_pins.events.push_back(0x40000000u | o << 16 | d); }

static std::vector<u8> melody_rom;

template <typename D> static void build_maps(D &dev, std::vector<u8> &rom)
{
    if (!melody_rom.empty())
        dev.m_melody_rom.ptr = melody_rom.data();
    auto cfg = dev.memory_space_config();
    for (auto &c : cfg) {
        address_space &sp = dev.space(c.first);
        sp.map.owner = &dev;
        c.second->ctor(sp.map);
        for (auto &e : sp.map.entries)
            if (e->kind == 2) {
                e->m_ram.assign(e->end - e->start + 1, 0);
                if (e->m_share == "lcd_ram_a") { dev.m_lcd_ram_a.ptr = e->m_ram.data(); dev.m_lcd_ram_a.size = e->m_ram.size(); }
                if (e->m_share == "lcd_ram_b") { dev.m_lcd_ram_b.ptr = e->m_ram.data(); dev.m_lcd_ram_b.size = e->m_ram.size(); }
                if (e->m_share == "lcd_ram_c") dev.m_lcd_ram_c.ptr = e->m_ram.data();
            }
        if (c.first == AS_PROGRAM)
            sp.rom = &rom;
    }
    dev.m_read_k.set([] { return ref_pins.k; });
    dev.m_read_ba.set([] { return ref_pins.ba; });
    dev.m_read_b.set([] { return ref_pins.b; });
    dev.m_write_s.set([](u8 d) { ref_pins.events.push_back(0x10000u | d); });
    dev.m_write_r.set([](u8 d) { ref_pins.events.push_back(0x20000u | d); });
    dev.m_write_segs.set([](offs_t o, u16 d) { ref_pins.events.push_back(0x40000000u | o << 16 | d); });
    dev.device_start();
    dev.m_icount = 0;
    dev.device_reset();
}

#define CHECK(field, a, b)                                                                                        \
    if ((unsigned)(a) != (unsigned)(b)) {                                                                       \
        std::printf("MISMATCH %s seed=%u tick=%lu %s: mame=%X port=%X (pc mame=%03X port=%03X op=%02X)\n", name,   \
                    seed, (unsigned long)tick, field, (unsigned)(a), (unsigned)(b), ref.m_pc, cpu.pc, ref.m_op);   \
        return false;                                                                                             \
    }

template <typename D>
static bool run_case(const char *name, SM5xxVariant variant, int rmask, size_t rom_size, unsigned seed,
                     unsigned long ticks, unsigned data_size, int o_pins)
{
    std::mt19937 rng(seed);
    std::vector<u8> rom(rom_size);
    for (auto &b : rom)
        b = (u8)rng();
    /* bias some programs towards halting and I/O so wakeup paths are exercised */
    if (seed & 1)
        for (size_t i = 0; i + 1 < rom.size(); i += 7) {
            if (variant == SM5XX_SM530) {
                /* halt, key inputs, melody on, timers */
                static const u8 picks[6][2] = {{0x74, 0x00}, {0x51, 0x00}, {0x77, 0x00}, {0x6c, 0x00}, {0x52, 0x00}, {0x78, 0x00}};
                const u8 *p = picks[rng() % 6];
                rom[i] = p[0];
                rom[i + 1] = p[1] ? p[1] : rom[i + 1];
            } else if (variant == SM5XX_SM511 || variant == SM5XX_SM512) {
                /* halt, key input, melody on and clock select */
                static const u8 picks[4][2] = {{0x5d, 0x00}, {0x50, 0x00}, {0x60, 0x31}, {0x60, 0x36}};
                const u8 *p = picks[rng() & 3];
                rom[i] = p[0];
                rom[i + 1] = p[1] ? p[1] : rom[i + 1];
            } else if (variant == SM5XX_SM510) {
                rom[i] = (u8)(rng() & 1 ? 0x5d : 0x6a);
            } else {
                rom[i] = rng() & 1 ? 0x5e : 0x6a;
                rom[i + 1] = rng() & 1 ? 0x00 : 0x04;
            }
        }

    melody_rom.clear();
    if (variant == SM5XX_SM511 || variant == SM5XX_SM512 || variant == SM5XX_SM530) {
        /* valid melody commands: 0..13 with octave/length bits, as the datasheet allows */
        melody_rom.resize(0x100);
        for (auto &b : melody_rom)
            b = (u8)((rng() % 14) | (rng() & 0x30));
    }
    machine_config mc;
    D ref(mc, "ref", nullptr, 32768);
    ref.set_r_mask_option(rmask);
    ref_pins = Pins();
    port_pins = Pins();
    build_maps(ref, rom);

    SM5xx cpu;
    SM5xxBus bus = {c_read_k, c_read_ba, c_read_b, c_write_s, c_write_r, c_write_segs, nullptr};
    sm5xx_init(&cpu, variant, rom.data(), rom.size(), &bus, rmask);
    if (!melody_rom.empty()) {
        sm5xx_set_melody(&cpu, melody_rom.data());
        sm5xx_reset(&cpu);
    }
    ref_pins.events.clear();
    port_pins.events.clear();

    emu_timer *div_timer = ref.timers[0], *lcd_timer = ref.timers[1];
    unsigned clock_phase = 0, strobe = 0;
    bool was_halted = false;
    for (unsigned long tick = 0; tick < ticks; ++tick) {
        if ((tick & 255) == 0) {
            u8 k = (u8)(rng() & (variant == SM5XX_SM530 ? 0xff : 0xf));
            int ba = rng() & 1, b = rng() & 1;
            ref_pins.k = port_pins.k = k;
            ref_pins.ba = port_pins.ba = ba;
            ref_pins.b = port_pins.b = b;
            int wake = rng() & 1;
            ref.execute_set_input(0, wake);
            sm5xx_set_wakeup(&cpu, wake);
        }
        if ((rng() % 20000u) == 0) {
            ref.device_reset();
            sm5xx_reset(&cpu);
        }
        if (++clock_phase >= (unsigned)(variant == SM5XX_SM530 ? 3 : ref.m_clk_div)) {
            clock_phase = 0;
            ref.m_icount += 1;
            ref.execute_run();
            sm5xx_run(&cpu, 1);
        }
        div_timer->cb(0);
        sm5xx_divider_tick(&cpu);
        if (++strobe >= 32) {
            strobe = 0;
            lcd_timer->cb(0);
            sm5xx_lcd_update(&cpu);
        }

        if (variant != SM5XX_SM530)
            CHECK("clk_div", ref.m_clk_div, cpu.clk_div);
        CHECK("melody_rd", ref.m_melody_rd, cpu.melody_rd);
        CHECK("melody_addr", ref.m_melody_address, cpu.melody_address);
        CHECK("melody_step", ref.m_melody_step_count, cpu.melody_step_count);
        CHECK("melody_duty", ref.m_melody_duty_count, cpu.melody_duty_count);
        CHECK("melody_idx", ref.m_melody_duty_index, cpu.melody_duty_index);
        CHECK("pc", ref.m_pc, cpu.pc);
        CHECK("prev_pc", ref.m_prev_pc, cpu.prev_pc);
        CHECK("op", ref.m_op, cpu.op);
        CHECK("icount", ref.m_icount, cpu.icount);
        CHECK("acc", ref.m_acc, cpu.acc);
        CHECK("bl", ref.m_bl, cpu.bl);
        CHECK("bm", ref.m_bm, cpu.bm);
        CHECK("bmask", ref.m_bmask, cpu.bmask);
        CHECK("c", ref.m_c, cpu.c);
        CHECK("skip", ref.m_skip, cpu.skip);
        CHECK("w", ref.m_w, cpu.w);
        CHECK("r", ref.m_r, cpu.r);
        CHECK("r_out", ref.m_r_out, cpu.r_out);
        CHECK("halt", ref.m_halt, cpu.halt);
        CHECK("div", ref.m_div, cpu.div);
        CHECK("gamma", ref.m_gamma, cpu.gamma);
        CHECK("l", ref.m_l, cpu.l);
        CHECK("x", ref.m_x, cpu.x);
        CHECK("y", ref.m_y, cpu.y);
        CHECK("bp", ref.m_bp, cpu.bp);
        CHECK("bc", ref.m_bc, cpu.bc);
        for (int i = 0; i < ref.m_stack_levels; ++i)
            CHECK("stack", ref.m_stack[i], cpu.stack[i]);
        if constexpr (std::is_base_of<sm500_device, D>::value) {
            for (int i = 0; i < ref.m_o_pins; ++i) {
                CHECK("W'", ref.m_ox[i], cpu.ox[i]);
                CHECK("W", ref.m_o[i], cpu.o[i]);
            }
            CHECK("cb", ref.m_cb, cpu.cb);
            CHECK("rsub", ref.m_rsub, cpu.rsub);
            CHECK("mx", ref.m_mx, cpu.mx);
            CHECK("s", ref.m_s, cpu.s);
        }
        if constexpr (std::is_base_of<sm530_device, D>::value) {
            CHECK("subdiv", ref.m_subdiv, cpu.subdiv);
            CHECK("count_1s", ref.m_count_1s, cpu.count_1s);
            CHECK("count_10ms", ref.m_count_10ms, cpu.count_10ms);
            CHECK("ds", ref.m_ds, cpu.ds);
        }
        coverage[variant].insert(cpu.op);
        if (cpu.halt && !was_halted) halts[variant]++;
        if (!cpu.halt && was_halted) wakeups[variant]++;
        was_halted = cpu.halt;
        for (unsigned a = 0; a < data_size; ++a)
            CHECK("ram", ref.m_data->read_byte(a) & 0xf, sm5xx_ram_read(&cpu, (uint8_t)a));
        if (ref_pins.events != port_pins.events) {
            std::printf("MISMATCH %s seed=%u tick=%lu pin events (%zu vs %zu)\n", name, seed, tick,
                        ref_pins.events.size(), port_pins.events.size());
            return false;
        }
        ref_pins.events.clear();
        port_pins.events.clear();
        (void)o_pins;
    }
    return true;
}


/* Game & Watch SM5A board wiring (hh_sm510 sm5a_common): R1 piezo, R2-R4 input mux */
struct Board {
    u8 mux = 0, in[3] = {0, 0, 0}, ba = 1, b = 1, speaker = 0;
    u8 k() const
    {
        u8 r = 0;
        for (int i = 0; i < 3; ++i)
            if (mux >> i & 1)
                r |= in[i];
        return r;
    }
};
static Board ref_board, port_board;
static sm5a_device *ref_dev;
static SM5xx *port_cpu;

static uint8_t b_read_k(void *) { return port_board.k(); }
static int b_read_ba(void *) { return port_board.ba; }
static int b_read_b(void *) { return port_board.b; }
static void b_write_r(void *, uint8_t d)
{
    port_board.speaker = d & 1;
    port_board.mux = d >> 1;
    sm5xx_set_wakeup(port_cpu, port_board.k() != 0);
    port_pins.events.push_back(0x20000u | d);
}

/* scripted play: Game A, then alternate Left/Right, Game B, Time */
static void script(unsigned long tick, Board &bd)
{
    unsigned long t = tick / 3277; /* ~0.1s steps */
    bd.in[2] = (t >= 10 && t < 12) ? 0x4 : (t >= 1800 && t < 1802) ? 0x2 : (t >= 3000 && t < 3003) ? 0x1 : 0;
    bd.ba = !(t >= 20 && t % 10 < 3);
    bd.b = !(t >= 20 && t % 10 >= 5 && t % 10 < 8);
}

static bool run_rom(const std::vector<u8> &rom_in, unsigned long ticks)
{
    std::vector<u8> rom(0x800, 0);
    std::copy(rom_in.begin(), rom_in.end(), rom.begin());
    machine_config mc;
    sm5a_device ref(mc, "ref", nullptr, 32768);
    ref.set_r_mask_option(sm510_base_device::RMASK_DIRECT);
    ref_pins = Pins();
    port_pins = Pins();
    ref_board = Board();
    port_board = Board();
    build_maps(ref, rom);
    ref_dev = &ref;
    ref.m_read_k.set([] { return ref_board.k(); });
    ref.m_read_ba.set([] { return (int)ref_board.ba; });
    ref.m_read_b.set([] { return (int)ref_board.b; });
    ref.m_write_r.set([](u8 d) {
        ref_board.speaker = d & 1;
        ref_board.mux = d >> 1;
        ref_dev->execute_set_input(0, ref_board.k() != 0);
        ref_pins.events.push_back(0x20000u | d);
    });
    ref.device_reset();
    ref_pins.events.clear();

    SM5xx cpu;
    SM5xxBus bus = {b_read_k, b_read_ba, b_read_b, nullptr, b_write_r, c_write_segs, nullptr};
    port_cpu = &cpu;
    sm5xx_init(&cpu, SM5XX_SM5A, rom.data(), rom.size(), &bus, SM5XX_RMASK_DIRECT);
    port_pins.events.clear();

    emu_timer *div_timer = ref.timers[0], *lcd_timer = ref.timers[1];
    unsigned clock_phase = 0, strobe = 0, seed = 0;
    unsigned long seg_writes = 0, speaker_edges = 0, distinct_pcs = 0;
    std::set<unsigned> pcs;
    const char *name = "ROM";
    u8 last_speaker = 0;
    for (unsigned long tick = 0; tick < ticks; ++tick) {
        Board before = ref_board;
        script(tick, ref_board);
        script(tick, port_board);
        if (ref_board.in[2] != before.in[2] || ref_board.ba != before.ba || ref_board.b != before.b) {
            ref.execute_set_input(0, ref_board.k() != 0);
            sm5xx_set_wakeup(&cpu, port_board.k() != 0);
        }
        if (++clock_phase >= 2) {
            clock_phase = 0;
            ref.m_icount += 1;
            ref.execute_run();
            sm5xx_run(&cpu, 1);
        }
        div_timer->cb(0);
        sm5xx_divider_tick(&cpu);
        if (++strobe >= 32) {
            strobe = 0;
            lcd_timer->cb(0);
            sm5xx_lcd_update(&cpu);
        }
        CHECK("pc", ref.m_pc, cpu.pc);
        CHECK("acc", ref.m_acc, cpu.acc);
        CHECK("bl", ref.m_bl, cpu.bl);
        CHECK("bm", ref.m_bm, cpu.bm);
        CHECK("c", ref.m_c, cpu.c);
        CHECK("div", ref.m_div, cpu.div);
        CHECK("halt", ref.m_halt, cpu.halt);
        CHECK("cb", ref.m_cb, cpu.cb);
        for (int i = 0; i < 9; ++i) {
            CHECK("W'", ref.m_ox[i], cpu.ox[i]);
            CHECK("W", ref.m_o[i], cpu.o[i]);
        }
        for (unsigned a = 0; a < 0x80; ++a)
            CHECK("ram", ref.m_data->read_byte(a) & 0xf, sm5xx_ram_read(&cpu, (uint8_t)a));
        if (ref_pins.events != port_pins.events) {
            std::printf("MISMATCH ROM tick=%lu pin events\n", tick);
            return false;
        }
        for (u32 e : port_pins.events)
            seg_writes += (e >> 30) & 1;
        ref_pins.events.clear();
        port_pins.events.clear();
        if (port_board.speaker != last_speaker)
            speaker_edges++;
        last_speaker = port_board.speaker;
        pcs.insert(cpu.pc);
    }
    distinct_pcs = pcs.size();
    std::printf("PASS: ROM ran %lu ticks (%.1f s) in lockstep with MAME 0.289: %lu distinct PCs, "
                "%lu segment writes, %lu piezo edges\n",
                ticks, ticks / 32768.0, distinct_pcs, seg_writes, speaker_edges);
    (void)seed;
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 2 && std::string(argv[1]) == "--rom") {
        FILE *f = std::fopen(argv[2], "rb");
        std::vector<u8> rom(0x740);
        if (!f || std::fread(rom.data(), 1, rom.size(), f) != rom.size()) {
            std::printf("cannot read %s\n", argv[2]);
            return 2;
        }
        std::fclose(f);
        return run_rom(rom, argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 32768ul * 600) ? 0 : 1;
    }
    unsigned programs = argc > 1 ? (unsigned)std::atoi(argv[1]) : 200;
    unsigned long ticks = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 200000ul;
    unsigned seed, fails = 0;
    for (seed = 1; seed <= programs; ++seed) {
        if (!run_case<sm5a_device>("SM5A", SM5XX_SM5A, sm510_base_device::RMASK_DIRECT, 0x800, seed, ticks, 0x80, 9))
            fails++;
        if (!run_case<sm510_device>("SM510", SM5XX_SM510, 2, 0x1000, seed, ticks, 0x80, 0))
            fails++;
        if (!run_case<sm510_device>("SM510-direct", SM5XX_SM510, sm510_base_device::RMASK_DIRECT, 0x1000, seed,
                                    ticks / 4, 0x80, 0))
            fails++;
        if (!run_case<sm500_device>("SM500", SM5XX_SM500, 11, 0x800, seed, ticks / 4, 0x40, 7))
            fails++;
        if (!run_case<sm511_device>("SM511", SM5XX_SM511, sm510_base_device::RMASK_DIRECT, 0x1000, seed, ticks, 0x80, 0))
            fails++;
        if (!run_case<sm512_device>("SM512", SM5XX_SM512, sm510_base_device::RMASK_DIRECT, 0x1000, seed, ticks / 2, 0x80,
                                    0))
            fails++;
        if (!run_case<sm530_device>("SM530", SM5XX_SM530, sm510_base_device::RMASK_DIRECT, 0x800, seed, ticks, 0x80, 0))
            fails++;
        if (fails)
            break;
    }
    if (fails) {
        std::printf("FAIL: MAME 0.289 reference comparison\n");
        return 1;
    }
    static const char *const vnames[6] = {"SM500", "SM5A", "SM510", "SM511", "SM512", "SM530"};
    for (int v = 0; v < 6; ++v)
        std::printf("  %s: %zu distinct opcodes executed, %lu halts, %lu wakeups\n", vnames[v], coverage[v].size(),
                    halts[v], wakeups[v]);
    std::printf("PASS: %u random programs x 7 CPU configurations, %lu ticks each, matched MAME 0.289 "
                "register, RAM and pin traces\n", programs, ticks);
    return 0;
}
