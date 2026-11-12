/* SM5xx CPU unit tests with hand assembled programs and expected values
   derived from the instruction descriptions in MAME 0.289 sm510op/sm500op. */
#include "cpu/sm5xx.h"
#include "test_support.h"

static uint8_t rom[0x1000];
static uint8_t k_in;
static int ba_in = 1, b_in = 1;
static uint8_t last_s, last_r;
static unsigned r_writes;
static uint16_t segs[0x20];

static uint8_t rd_k(void *u) { (void)u; return k_in; }
static int rd_ba(void *u) { (void)u; return ba_in; }
static int rd_b(void *u) { (void)u; return b_in; }
static void wr_s(void *u, uint8_t d) { (void)u; last_s = d; }
static void wr_r(void *u, uint8_t d) { (void)u; last_r = d; r_writes++; }
static void wr_segs(void *u, unsigned o, uint16_t d) { (void)u; if (o < 0x20) segs[o] = d; }

static const SM5xxBus bus = {rd_k, rd_ba, rd_b, wr_s, wr_r, wr_segs, NULL};

static uint8_t next_pl(uint8_t pl)
{
    uint8_t feed = ((pl >> 1 ^ pl) & 1) ? 0 : 0x20;
    return (uint8_t)(feed | (pl >> 1 & 0x1f));
}

/* places bytes at successive LFSR positions of the page starting at base */
static void place(uint16_t base, const uint8_t *code, size_t n)
{
    uint8_t pl = 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        rom[base + pl] = code[i];
        pl = next_pl(pl);
    }
}

static void boot(SM5xx *cpu, SM5xxVariant v, const uint8_t *code, size_t n)
{
    memset(rom, 0, sizeof(rom));
    place(v == SM5XX_SM510 ? 0xdc0 : 0x3c0, code, n);
    sm5xx_init(cpu, v, rom, v == SM5XX_SM510 ? 0x1000 : 0x800, &bus, v == SM5XX_SM510 ? 2 : SM5XX_RMASK_DIRECT);
}

static void steps(SM5xx *cpu, int instructions)
{
    while (instructions-- > 0)
        sm5xx_run(cpu, 1);
}

static void test_lfsr(void)
{
    uint8_t pl = 0, seen[64] = {0};
    unsigned n = 0;
    while (!seen[pl]) {
        seen[pl] = 1;
        pl = next_pl(pl);
        n++;
    }
    CHECK_EQ(n, 63);
    CHECK(!seen[0x3f]);
    CHECK_EQ(next_pl(0x3f), 0x3f);
}

static void test_reset_vectors(void)
{
    SM5xx cpu;
    boot(&cpu, SM5XX_SM510, (const uint8_t[]){0x00}, 1);
    CHECK_EQ(cpu.pc, 0xdc0);
    CHECK_EQ(cpu.bp, 1);
    CHECK_EQ(cpu.gamma, 0);
    boot(&cpu, SM5XX_SM5A, (const uint8_t[]){0x00}, 1);
    CHECK_EQ(cpu.pc, 0x3c0);
    CHECK_EQ(cpu.stack[0], 0x3c0);
    CHECK_EQ(cpu.gamma, 1);
    CHECK_EQ(cpu.r, 0xf);
    CHECK_EQ(cpu.prgmask, 0x7ff);
    CHECK_EQ(cpu.datamask, 0x7f);
}

static void test_arithmetic(void)
{
    SM5xx cpu;
    /* LAX 9; ADX 8 (carry, skip); LAX 3 (skipped); COMA */
    const uint8_t p1[] = {0x29, 0x38, 0x23, 0x0a};
    boot(&cpu, SM5XX_SM510, p1, sizeof(p1));
    steps(&cpu, 2);
    CHECK_EQ(cpu.acc, 1);
    CHECK(cpu.skip);
    steps(&cpu, 2);
    CHECK_EQ(cpu.acc, 0xe);

    /* ADX 10 never skips */
    {
        const uint8_t p2[] = {0x2f, 0x3a};
        boot(&cpu, SM5XX_SM510, p2, sizeof(p2));
        steps(&cpu, 2);
        CHECK_EQ(cpu.acc, 9);
        CHECK(!cpu.skip);
    }

    /* consecutive LAX: only the first loads ACC */
    {
        const uint8_t p3[] = {0x21, 0x22, 0x23};
        boot(&cpu, SM5XX_SM510, p3, sizeof(p3));
        steps(&cpu, 3);
        CHECK_EQ(cpu.acc, 1);
    }

    /* SC; LAX 7; ROT -> acc 0xb, carry 1 (SM510) */
    {
        const uint8_t p4[] = {0x67, 0x27, 0x6b};
        boot(&cpu, SM5XX_SM510, p4, sizeof(p4));
        steps(&cpu, 3);
        CHECK_EQ(cpu.acc, 0xb);
        CHECK_EQ(cpu.c, 1);
    }

    /* ADD11 with carry: LBL 0x05; LAX 8; EXC 0 (ram=8); LAX 9; SC; ADD11 -> 9+8+1 = 18 */
    {
        const uint8_t p5[] = {0x5f, 0x05, 0x28, 0x10, 0x29, 0x67, 0x09};
        boot(&cpu, SM5XX_SM510, p5, sizeof(p5));
        steps(&cpu, 7); /* LBL takes two cycles */
        CHECK_EQ(cpu.acc, 2);
        CHECK_EQ(cpu.c, 1);
        CHECK(cpu.skip);
        CHECK_EQ(sm5xx_ram_read(&cpu, 0x05), 8);
    }
}

static void test_ram(void)
{
    SM5xx cpu;
    /* SM510: LBL 0x3a; LAX 5; EXC 1 -> ram[0x3a]=5, BM ^= 1; LDA 0 reads ram[0x2a] */
    const uint8_t p1[] = {0x5f, 0x3a, 0x25, 0x11, 0x18};
    boot(&cpu, SM5XX_SM510, p1, sizeof(p1));
    steps(&cpu, 4);
    CHECK_EQ(sm5xx_ram_read(&cpu, 0x3a), 5);
    CHECK_EQ(cpu.bm, 2);
    CHECK_EQ(cpu.acc, 0);

    /* SM510 SBM: BM high bit applies for one instruction: LB 0 (BM 0, BL 0); SBM; EXC -> 0x40 */
    {
        const uint8_t p2[] = {0x40, 0x26, 0x02, 0x10};
        boot(&cpu, SM5XX_SM510, p2, sizeof(p2));
        steps(&cpu, 4);
        CHECK_EQ(sm5xx_ram_read(&cpu, 0x40), 6);
        CHECK_EQ(sm5xx_ram_read(&cpu, 0x00), 0);
        CHECK_EQ(cpu.bmask, 0);
    }

    /* SM5A mirrors: BL 0xd-0xf alias 0xc, BM 4-7 alias 4 */
    {
        const uint8_t p3[] = {0x5f, 0x0d, 0x27, 0x10, 0x5f, 0x5c, 0x23, 0x10};
        boot(&cpu, SM5XX_SM5A, p3, sizeof(p3));
        steps(&cpu, 8);
        CHECK_EQ(sm5xx_ram_read(&cpu, 0x0c), 7);
        CHECK_EQ(sm5xx_ram_read(&cpu, 0x0f), 7);
        CHECK_EQ(sm5xx_ram_read(&cpu, 0x4c), 3);
        CHECK_EQ(sm5xx_ram_read(&cpu, 0x7c), 3);
    }

    /* SM500 has unbacked RAM above BL 9 */
    {
        const uint8_t p4[] = {0x5f, 0x0b, 0x27, 0x10};
        boot(&cpu, SM5XX_SM500, p4, sizeof(p4));
        steps(&cpu, 4);
        CHECK_EQ(sm5xx_ram_read(&cpu, 0x0b), 0);
    }

    /* INCB skip: SM510 on wrap to 0, SM5A when BL reaches 8 */
    {
        const uint8_t p5[] = {0x5f, 0x07, 0x64};
        boot(&cpu, SM5XX_SM5A, p5, sizeof(p5));
        steps(&cpu, 3);
        CHECK_EQ(cpu.bl, 8);
        CHECK(cpu.skip);
        boot(&cpu, SM5XX_SM510, p5, sizeof(p5));
        steps(&cpu, 3);
        CHECK(!cpu.skip);
    }
}

static void test_branch_and_stack(void)
{
    SM5xx cpu;
    /* SM510 TM 0x05: pointer at 0x005 = 0x81 -> call 0x400 | (PU 2) ... */
    const uint8_t p1[] = {0xc5};
    boot(&cpu, SM5XX_SM510, p1, sizeof(p1));
    rom[0x005] = 0x81; /* PU=2, PL=1 -> 0x800 | 0x100 | 0x01 */
    rom[0x901] = 0x6f; /* RTN1 */
    sm5xx_run(&cpu, 2);
    CHECK_EQ(cpu.pc, 0x901);
    CHECK_EQ(cpu.stack[0], 0xde0); /* return address after TM at PL 0 -> PL 0x20 */
    sm5xx_run(&cpu, 1);
    CHECK_EQ(cpu.pc, 0xde0);
    CHECK(cpu.skip);

    /* TL long jump: 0x7a 0x45 -> PU=1, PM=0xa, PL=5 */
    {
        const uint8_t p2[] = {0x7a, 0x45};
        boot(&cpu, SM5XX_SM510, p2, sizeof(p2));
        sm5xx_run(&cpu, 2);
        CHECK_EQ(cpu.pc, 0x400 | 0x280 | 0x05);
    }

    /* SM510 two level stack: TML twice then RTN0 twice */
    {
        const uint8_t p3[] = {0x7c, 0x01};
        boot(&cpu, SM5XX_SM510, p3, sizeof(p3));
        /* LFSR: PL 0x01 is followed by 0x00, then 0x20 */
        rom[0x001] = 0x7d;
        rom[0x000] = 0x02;
        rom[0x042] = 0x6e;
        rom[0x020] = 0x6e;
        sm5xx_run(&cpu, 2);
        CHECK_EQ(cpu.pc, 0x001);
        sm5xx_run(&cpu, 2);
        CHECK_EQ(cpu.pc, 0x042);
        CHECK_EQ(cpu.stack[0], 0x020);
        CHECK_EQ(cpu.stack[1], 0xdf0);
        sm5xx_run(&cpu, 1);
        CHECK_EQ(cpu.pc, 0x020);
        sm5xx_run(&cpu, 1);
        CHECK_EQ(cpu.pc, 0xdf0);
    }

    /* SM5A TR stays in the SU page, TRS calls the subroutine page */
    {
        const uint8_t p4[] = {0x85};
        boot(&cpu, SM5XX_SM5A, p4, sizeof(p4));
        sm5xx_run(&cpu, 1);
        CHECK_EQ(cpu.pc, 0x3c5);
        rom[0x3c5] = 0xc7;
        sm5xx_run(&cpu, 1);
        CHECK_EQ(cpu.pc, 0x407);
        CHECK(cpu.rsub);
        rom[0x407] = 0x6e;
        sm5xx_run(&cpu, 1);
        CHECK(!cpu.rsub);
    }

    /* ATPL loads PC low bits from ACC, page from the ATPL address (PL 0x20) */
    {
        const uint8_t p5[] = {0x2a, 0x03};
        boot(&cpu, SM5XX_SM510, p5, sizeof(p5));
        steps(&cpu, 2);
        CHECK_EQ(cpu.pc, 0xdea);
    }
}

static void test_timing(void)
{
    SM5xx cpu;
    /* two byte opcodes cost two cycles, TM costs two */
    const uint8_t p1[] = {0x5f, 0x12, 0x00};
    boot(&cpu, SM5XX_SM510, p1, sizeof(p1));
    sm5xx_run(&cpu, 1);
    CHECK_EQ(cpu.icount, -1);
    CHECK_EQ(cpu.bl, 2);
    sm5xx_run(&cpu, 1);
    CHECK_EQ(cpu.icount, 0);
    CHECK_EQ(cpu.op, 0x5f);

    /* divider: gamma after 32768 ticks, TF4/TF1/DTA see the divider bits */
    {
        unsigned i;
        boot(&cpu, SM5XX_SM510, (const uint8_t[]){0x00}, 1);
        for (i = 0; i < 0x800; ++i)
            sm5xx_divider_tick(&cpu);
        CHECK_EQ(cpu.div, 0x800);
        CHECK_EQ(cpu.gamma, 0);
        for (; i < 0x8000; ++i)
            sm5xx_divider_tick(&cpu);
        CHECK_EQ(cpu.div, 0);
        CHECK_EQ(cpu.gamma, 1);
    }

    /* TIS clears gamma and skips only when it was clear */
    {
        const uint8_t p2[] = {0x58, 0x00, 0x58};
        boot(&cpu, SM5XX_SM510, p2, sizeof(p2));
        cpu.gamma = 1;
        sm5xx_run(&cpu, 1);
        CHECK(!cpu.skip);
        CHECK_EQ(cpu.gamma, 0);
        sm5xx_run(&cpu, 2);
        CHECK(cpu.skip);
    }

    /* SM510 R output from divider bit 2 (4096Hz), R2 inverse phase */
    {
        const uint8_t p3[] = {0x23, 0x61};
        unsigned i, toggles = 0;
        uint8_t prev;
        boot(&cpu, SM5XX_SM510, p3, sizeof(p3));
        steps(&cpu, 2);
        prev = cpu.r_out;
        for (i = 0; i < 64; ++i) {
            sm5xx_divider_tick(&cpu);
            if (cpu.r_out != prev)
                toggles++;
            prev = cpu.r_out;
            CHECK(cpu.r_out == 1 || cpu.r_out == 2);
        }
        CHECK_EQ(toggles, 16);
    }
}

static void test_halt_wakeup(void)
{
    SM5xx cpu;
    /* SM510 CEND, wake on K line to 0x400 */
    const uint8_t p1[] = {0x5d};
    boot(&cpu, SM5XX_SM510, p1, sizeof(p1));
    sm5xx_run(&cpu, 1);
    CHECK(cpu.halt);
    sm5xx_run(&cpu, 10);
    CHECK(cpu.halt);
    CHECK_EQ(cpu.icount, 0);
    sm5xx_set_wakeup(&cpu, true);
    sm5xx_run(&cpu, 2);
    CHECK(!cpu.halt);
    CHECK_EQ(cpu.prev_pc, 0x400);

    /* SM5A CEND (5E 00), wake on gamma to 0x000 with CB cleared */
    {
        const uint8_t p2[] = {0x6d, 0x5e, 0x00};
        boot(&cpu, SM5XX_SM5A, p2, sizeof(p2));
        cpu.gamma = 0;
        sm5xx_run(&cpu, 3);
        CHECK(cpu.halt);
        CHECK_EQ(cpu.cb, 1);
        cpu.gamma = 1;
        sm5xx_run(&cpu, 2);
        CHECK(!cpu.halt);
        CHECK_EQ(cpu.cb, 0);
        CHECK_EQ(cpu.prev_pc, 0x000);
    }
}

static void test_io(void)
{
    SM5xx cpu;
    /* SM510 KTA, TAL/TB pins, WS/WR shift S, ATR */
    const uint8_t p1[] = {0x6a, 0x63, 0x62, 0x63, 0x5e};
    boot(&cpu, SM5XX_SM510, p1, sizeof(p1));
    k_in = 0x1b;
    ba_in = 0;
    steps(&cpu, 1);
    CHECK_EQ(cpu.acc, 0xb);
    steps(&cpu, 3);
    CHECK_EQ(last_s, 0x05);
    steps(&cpu, 1);
    CHECK(!cpu.skip);
    ba_in = 1;

    /* SM5A ATR drives R inverted: ACC 0xe -> R1 high, mux 0 */
    {
        const uint8_t p2[] = {0x2e, 0x01};
        boot(&cpu, SM5XX_SM5A, p2, sizeof(p2));
        steps(&cpu, 2);
        CHECK_EQ(last_r, 0x1);
    }
}

static void test_lcd(void)
{
    SM5xx cpu;
    /* SM510: ram 0x60 bit h -> SEGA common h segment 0; L/X drive BS */
    boot(&cpu, SM5XX_SM510, (const uint8_t[]){0x00}, 1);
    cpu.ram[0x60] = 0x5;
    cpu.ram[0x6f] = 0x8;
    cpu.ram[0x71] = 0x2;
    cpu.l = 0x3;
    cpu.x = 0x1;
    memset(segs, 0, sizeof(segs));
    sm5xx_lcd_update(&cpu);
    CHECK_EQ(segs[0 | SM5XX_SEG_A], 0x0001);
    CHECK_EQ(segs[2 | SM5XX_SEG_A], 0x0001);
    CHECK_EQ(segs[3 | SM5XX_SEG_A], 0x8000);
    CHECK_EQ(segs[1 | SM5XX_SEG_B], 0x0002);
    /* bs = (L >> h & 1) | ((X * 2) >> h & 2) */
    CHECK_EQ(segs[0 | SM5XX_SEG_BS], 0x3);
    CHECK_EQ(segs[1 | SM5XX_SEG_BS], 0x1);
    CHECK_EQ(segs[0 | SM5XX_SEG_C], 0);
    cpu.bc = true;
    sm5xx_lcd_update(&cpu);
    CHECK_EQ(segs[0 | SM5XX_SEG_A], 0);

    /* SM5A: WS/WR shift into W', TW copies to W; H1 shows W, H2 shows W' */
    {
        const uint8_t p2[] = {0x25, 0x63, 0x23, 0x62, 0x5c, 0x29, 0x63};
        boot(&cpu, SM5XX_SM5A, p2, sizeof(p2));
        steps(&cpu, 7);
        CHECK_EQ(cpu.ox[6], 0xd);
        CHECK_EQ(cpu.ox[7], 0x3);
        CHECK_EQ(cpu.ox[8], 0x9);
        CHECK_EQ(cpu.o[7], 0xd);
        CHECK_EQ(cpu.o[8], 0x3);
        memset(segs, 0, sizeof(segs));
        sm5xx_lcd_update(&cpu);
        CHECK_EQ(segs[8 << 1 | 0], 0x3);
        CHECK_EQ(segs[8 << 1 | 1], 0x9);
        cpu.bp = 0;
        sm5xx_lcd_update(&cpu);
        CHECK_EQ(segs[8 << 1 | 1], 0);
    }

    /* SM5A DTW: digit PLA, CN flag selects the second row */
    {
        const uint8_t p3[] = {0x28, 0x5d, 0x60, 0x28, 0x5d};
        boot(&cpu, SM5XX_SM5A, p3, sizeof(p3));
        steps(&cpu, 2);
        CHECK_EQ(cpu.ox[8], 0xe);
        steps(&cpu, 3);
        CHECK_EQ(cpu.ox[8], 0xf);
        CHECK_EQ(cpu.ox[7], 0xe);
    }
}

/* SM530 (sm530.cpp/sm530op.cpp): reset vector, LB, ADX carry, RAM map, LCD, timers, TRS */
static void test_sm530(void)
{
    SM5xx cpu;
    unsigned t;
    /* LAX 5, ADX 11 (carry skips the next), LAX 1, LB 0x35, SDS */
    static const uint8_t code[] = {0x15, 0x0b, 0x11, 0x35, 0x4f};
    boot(&cpu, SM5XX_SM530, code, sizeof(code));
    CHECK_EQ(cpu.pc, 0x3c0);
    CHECK_EQ(cpu.stack_levels, 1);
    CHECK_EQ(cpu.clk_div, 3);
    steps(&cpu, 3);
    CHECK_EQ(cpu.acc, 0); /* 5 + 11 = 16: carry skipped LAX 1 */
    steps(&cpu, 1);
    CHECK_EQ(cpu.bl, 7);
    CHECK_EQ(cpu.bm, 1);

    /* RAM: 0x00-0x3f, LCD A 0x40-0x4b and B 0x50-0x5b mirrored at +0x20, the rest unmapped */
    sm5xx_ram_write(&cpu, 0x63, 9);
    CHECK_EQ(sm5xx_ram_read(&cpu, 0x43), 9);
    sm5xx_ram_write(&cpu, 0x4c, 5);
    CHECK_EQ(sm5xx_ram_read(&cpu, 0x4c), 0);
    sm5xx_ram_write(&cpu, 0x50, 0xa);
    sm5xx_ram_write(&cpu, 0x40, 0x5);
    memset(segs, 0, sizeof(segs));
    sm5xx_lcd_update(&cpu);
    CHECK_EQ(segs[0], 0x5);
    CHECK_EQ(segs[1], 0xa);
    CHECK_EQ(segs[6], 9); /* row A group 3 */
    cpu.ds = false; /* RDS blanks the display */
    sm5xx_lcd_update(&cpu);
    CHECK_EQ(segs[0], 0);

    /* 1/100s counter: 250 of every 256 ticks feed it, one count per 320 */
    for (t = 0; t < 640; ++t)
        sm5xx_divider_tick(&cpu);
    CHECK_EQ(cpu.count_10ms, 1);
    for (t = 640; t < 16384; ++t)
        sm5xx_divider_tick(&cpu);
    CHECK(cpu.gamma & 4); /* 0.5s */
    CHECK(cpu.gamma & 8); /* 0.1s */
    for (t = 16384; t < 32768; ++t)
        sm5xx_divider_tick(&cpu);
    CHECK(cpu.gamma & 2); /* 1s */
    CHECK_EQ(cpu.count_1s, 1);

    /* TRS 0x05: vector byte on page 14 is bitswapped into the page (bits 5,7,6) */
    {
        static const uint8_t trs[] = {0xc5};
        boot(&cpu, SM5XX_SM530, trs, sizeof(trs));
        rom[(14 << 6) | 5] = 0xa5;
        steps(&cpu, 1);
        CHECK_EQ(cpu.pc, (6 << 6) | 5);
        CHECK_EQ(cpu.stack[0] & 0x7c0, 0x3c0);
    }
}

static void test_illegal(void)
{
    SM5xx cpu;
    const uint8_t p1[] = {0x50, 0x5c};
    boot(&cpu, SM5XX_SM510, p1, sizeof(p1));
    steps(&cpu, 2);
    CHECK_EQ(cpu.illegal_count, 2);
}

int main(void)
{
    test_lfsr();
    test_reset_vectors();
    test_arithmetic();
    test_ram();
    test_branch_and_stack();
    test_timing();
    test_halt_wakeup();
    test_io();
    test_lcd();
    test_sm530();
    test_illegal();
    return test_finish("cpu_test (SM5xx instructions, memory, stack, timers, LCD, wakeup)");
}
