/* Board level tests: input matrix, K wakeup, LCD decay, piezo, ACL, timing and
   deterministic save states, using the original self-test ROM set and the
   Game & Watch wiring definitions (no Nintendo ROM data). */
#include "content/loader.h"
#include "hardware/machine.h"
#include "libretro.h"
#include "test_support.h"

#define BTN(x) (UINT32_C(1) << RETRO_DEVICE_ID_JOYPAD_##x)

static LCDContent content;
static LCDMachine m, m2;
static uint8_t levels[LCD_OSC_HZ];

static void load_selftest(void)
{
    CHECK_EQ(lcd_content_load(&content, "roms/lcdlab_selftest.zip", NULL, 0), 0);
    CHECK(content.kind == LCD_CONTENT_MACHINE);
    CHECK(content.def && strcmp(content.def->id, "lcdlab_selftest") == 0);
    CHECK_EQ(lcd_machine_init(&m, content.def, content.rom, content.rom_size), 0);
}

static void test_counter_and_display(void)
{
    unsigned group;
    /* gamma at reset, then one increment per second of the 32.768kHz divider */
    lcd_machine_run(&m, LCD_OSC_HZ / 4, NULL);
    CHECK_EQ(sm5xx_ram_read(&m.cpu, 0x00), 1);
    lcd_machine_run(&m, LCD_OSC_HZ, NULL);
    CHECK_EQ(sm5xx_ram_read(&m.cpu, 0x00), 2);
    lcd_machine_run(&m, LCD_OSC_HZ * 3, NULL);
    CHECK_EQ(sm5xx_ram_read(&m.cpu, 0x00), 5);

    /* nine shifts per pass: groups 0 and 8 hold counter|8, group 6 holds 15; TW copies W' to W */
    CHECK_EQ(m.cpu.o[0], 0xd);
    CHECK_EQ(m.cpu.o[8], 0xd);
    CHECK_EQ(m.cpu.o[6], 0xf);
    {
        /* W holds the last complete pass: counter|8, K, ~counter|8, ~K&7, 13, 2, 15, 0, counter|8 */
        static const uint8_t expect[9] = {0xd, 0x0, 0xa, 0x7, 0xd, 0x2, 0xf, 0x0, 0xd};
        for (group = 0; group < 9; ++group)
            CHECK_EQ(m.cpu.o[group], expect[group]);
    }
    CHECK(lcd_machine_segment_active(&m, 0, 3, 0));
    CHECK(lcd_machine_segment_active(&m, 0, 0, 1));
    CHECK(!lcd_machine_segment_active(&m, 0, 1, 1));
}

static void test_inputs(void)
{
    /* K1..K3 appear in group 1 (second shift) within a frame */
    lcd_machine_set_buttons(&m, BTN(START) | BTN(L));
    CHECK_EQ(m.ports[0], 0x5);
    lcd_machine_run(&m, 1024, NULL);
    CHECK_EQ(m.cpu.o[1], 0x5);
    CHECK_EQ(m.cpu.o[3], 0x2);
    CHECK(m.cpu.ext_wakeup);
    lcd_machine_set_buttons(&m, 0);
    CHECK(!m.cpu.ext_wakeup);
    lcd_machine_run(&m, 1024, NULL);
    CHECK_EQ(m.cpu.o[1], 0x0);

    /* BA/B are active low on this wiring */
    CHECK_EQ(m.ba, 1);
    lcd_machine_set_buttons(&m, BTN(RIGHT));
    CHECK_EQ(m.ba, 0);
    lcd_machine_set_buttons(&m, BTN(LEFT));
    CHECK_EQ(m.b, 0);
    lcd_machine_set_buttons(&m, 0);
}

static unsigned edges(const uint8_t *v, unsigned n)
{
    unsigned i, e = 0;
    for (i = 1; i < n; ++i)
        e += v[i] != v[i - 1];
    return e;
}

static void test_piezo(void)
{
    unsigned quiet, tone;
    lcd_machine_run(&m, 4096, levels);
    quiet = edges(levels, 4096);
    lcd_machine_set_buttons(&m, BTN(RIGHT));
    lcd_machine_run(&m, 4096, levels);
    tone = edges(levels, 4096);
    lcd_machine_set_buttons(&m, 0);
    CHECK_EQ(quiet, 0);
    CHECK(tone > 20);
}

static void test_decay(void)
{
    /* MAME decay model: on after 8 strobes (pivot), stays on for 17 more */
    static LCDMachine t;
    unsigned i;
    lcd_machine_clone(&t, &m);
    memset(t.display_decay, 0, sizeof(t.display_decay));
    t.display_decay[0][1] = 7;
    CHECK(!lcd_machine_segment_active(&t, 0, 0, 1));
    t.display_decay[0][1] = 8;
    CHECK(lcd_machine_segment_active(&t, 0, 0, 1));
    CHECK_EQ(lcd_machine_segment_level(&t, 0, 0, 1, false), 255);
    CHECK_EQ(lcd_machine_segment_level(&t, 0, 0, 1, true), 8 * 255 / 25);
    CHECK_EQ(lcd_machine_segment_level(&t, 9, 0, 0, false), 0);
    CHECK_EQ(lcd_machine_segment_level(&t, 0, 4, 0, false), 0);
    CHECK_EQ(lcd_machine_segment_level(&t, 0, 0, 2, false), 0);

    /* real strobes: a newly lit group needs 8 strobes (8/1024 s) to appear */
    lcd_machine_clone(&t, &m);
    memset(t.display_decay, 0, sizeof(t.display_decay));
    t.strobe_phase = 0;
    for (i = 0; i < 7; ++i)
        lcd_machine_run(&t, LCD_DECAY_DIVIDER, NULL);
    CHECK(!lcd_machine_segment_active(&t, 6, 0, 0));
    lcd_machine_run(&t, LCD_DECAY_DIVIDER, NULL);
    CHECK(lcd_machine_segment_active(&t, 6, 0, 0));
    /* and stays lit for 17 strobes after the latch clears */
    for (i = 0; i < 40; ++i)
        lcd_machine_run(&t, LCD_DECAY_DIVIDER, NULL);
    t.cpu.bp = 0;
    for (i = 0; i < 17; ++i)
        lcd_machine_run(&t, LCD_DECAY_DIVIDER, NULL);
    CHECK(lcd_machine_segment_active(&t, 6, 0, 0));
    lcd_machine_run(&t, LCD_DECAY_DIVIDER, NULL);
    CHECK(!lcd_machine_segment_active(&t, 6, 0, 0));
}

static void test_acl(void)
{
    lcd_machine_run(&m, 100, NULL);
    lcd_machine_set_buttons(&m, BTN(R3));
    CHECK(m.acl);
    CHECK_EQ(m.cpu.pc, 0x3c0);
    lcd_machine_run(&m, 1000, NULL);
    CHECK_EQ(m.cpu.pc, 0x3c0);
    lcd_machine_set_buttons(&m, 0);
    lcd_machine_run(&m, 100, NULL);
    CHECK(m.cpu.pc != 0x3c0);
}

static void test_states(void)
{
    size_t size = lcd_machine_state_size();
    uint8_t *a = malloc(size), *b = malloc(size);
    unsigned i;
    CHECK(a && b && size > 500 && size < 4096);
    for (i = 0; i < 30; ++i) {
        lcd_machine_set_buttons(&m, i % 3 ? BTN(RIGHT) : BTN(START));
        lcd_machine_run(&m, 546, NULL);
    }
    CHECK(lcd_machine_save(&m, a, size));
    lcd_machine_clone(&m2, &m);
    for (i = 0; i < 120; ++i) {
        lcd_machine_set_buttons(&m, i % 5 ? BTN(LEFT) : 0);
        lcd_machine_run(&m, 546, NULL);
    }
    CHECK(lcd_machine_save(&m, b, size));
    CHECK(lcd_machine_load(&m2, a, size));
    for (i = 0; i < 120; ++i) {
        lcd_machine_set_buttons(&m2, i % 5 ? BTN(LEFT) : 0);
        lcd_machine_run(&m2, 546, NULL);
    }
    CHECK(lcd_machine_save(&m2, a, size));
    CHECK(memcmp(a, b, size) == 0);
    CHECK(!lcd_machine_load(&m2, a, size - 1));
    {
        size_t acc_offset = 4 + 2 * 4 + 1 + 8;
        uint8_t keep = a[acc_offset];
        a[acc_offset] = 0x40;
        CHECK(!lcd_machine_load(&m2, a, size));
        a[acc_offset] = keep;
    }
    free(a);
    free(b);
}

static void test_wiring(void)
{
    static const uint8_t zeros[0x1000];
    const LCDMachineDef *fire = lcd_registry_find_id("gnw_fire");
    const LCDMachineDef *dk = lcd_registry_find_id("gnw_dkong");
    const LCDMachineDef *ball = lcd_registry_find_id("gnw_ball");
    CHECK(fire && dk && ball);
    if (!fire || !dk || !ball)
        return;

    /* Fire: R2-R4 select IN.0-2, Game A on IN.2 bit 2, BA/B active high */
    CHECK_EQ(lcd_machine_init(&m2, fire, zeros, fire->rom.size), 0);
    lcd_machine_set_buttons(&m2, BTN(START) | BTN(RIGHT));
    CHECK_EQ(m2.ba, 1);
    CHECK_EQ(m2.b, 0);
    m2.cpu.bus.write_r(m2.cpu.bus.user, 0x2); /* R2 -> mux bit 0 */
    CHECK_EQ(m2.cpu.bus.read_k(m2.cpu.bus.user), 0);
    m2.cpu.bus.write_r(m2.cpu.bus.user, 0x8); /* R4 -> IN.2 */
    CHECK_EQ(m2.cpu.bus.read_k(m2.cpu.bus.user), 0x4);
    CHECK(m2.cpu.ext_wakeup);
    CHECK_EQ(m2.speaker, 0);
    m2.cpu.bus.write_r(m2.cpu.bus.user, 0x9);
    CHECK_EQ(m2.speaker, 1);

    /* Donkey Kong: S strobes IN.0-2, jump on IN.0 bit 3; B cheat off, no BA port */
    CHECK_EQ(lcd_machine_init(&m2, dk, zeros, dk->rom.size), 0);
    lcd_machine_set_buttons(&m2, BTN(A) | BTN(UP));
    CHECK_EQ(m2.ba, 1);
    CHECK_EQ(m2.b, 1);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0x1);
    CHECK_EQ(m2.cpu.bus.read_k(m2.cpu.bus.user), 0x8);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0x3);
    CHECK_EQ(m2.cpu.bus.read_k(m2.cpu.bus.user), 0xa);
    m2.cpu.bus.write_r(m2.cpu.bus.user, 0x1);
    CHECK_EQ(m2.speaker, 1);
    CHECK_EQ(m2.display_z_len, 2);

    /* Ball: IN.0 tied to ground (always read), BA/B active low */
    CHECK_EQ(lcd_machine_init(&m2, ball, zeros, ball->rom.size), 0);
    lcd_machine_set_buttons(&m2, BTN(SELECT));
    CHECK_EQ(m2.cpu.bus.read_k(m2.cpu.bus.user), 0x2);
    lcd_machine_set_buttons(&m2, BTN(LEFT));
    CHECK_EQ(m2.b, 0);
    CHECK_EQ(m2.ba, 1);

    /* wrong ROM size is rejected */
    CHECK(lcd_machine_init(&m2, ball, zeros, 0x800) != 0);
}

static void test_clock(void)
{
    static const uint8_t zeros[0x1000];
    const LCDMachineDef *helmet = lcd_registry_find_id("gnw_helmet"), *ball = lcd_registry_find_id("gnw_ball");
    const LCDMachineDef *gauntlet = lcd_registry_find_id("tgaunt");
    CHECK(helmet && ball && gauntlet);
    if (!helmet || !ball || !gauntlet)
        return;
    /* Helmet: hour tens carries PM in bit 3 */
    CHECK_EQ(lcd_machine_init(&m2, helmet, zeros, helmet->rom.size), 0);
    CHECK(lcd_machine_set_clock(&m2, 19, 42));
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x36), 0x8);
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x37), 7);
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x38), 4);
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x39), 2);
    CHECK(lcd_machine_set_clock(&m2, 0, 5)); /* midnight shows 12 AM */
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x36), 0x1);
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x37), 2);
    CHECK(lcd_machine_set_clock(&m2, 12, 0)); /* noon is 12 PM */
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x36), 0x9);
    CHECK(!lcd_machine_set_clock(&m2, 24, 0));
    CHECK(!lcd_machine_set_clock(&m2, 1, 60));
    /* Ball: plain digits, SM5A RAM mirror 0x48 = 0x58 = 0x68 = 0x78 */
    CHECK_EQ(lcd_machine_init(&m2, ball, zeros, ball->rom.size), 0);
    CHECK(lcd_machine_set_clock(&m2, 22, 9));
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x78), 1);
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x59), 0);
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x4a), 0);
    CHECK_EQ(sm5xx_ram_read(&m2.cpu, 0x4b), 9);
    /* machines without a verified clock layout are left alone */
    CHECK_EQ(lcd_machine_init(&m2, gauntlet, zeros, gauntlet->rom.size), 0);
    CHECK(!lcd_machine_set_clock(&m2, 10, 10));
}

static int16_t tone[1000];
static const int16_t *const tone_data[1] = {tone};
static const uint32_t tone_length[1] = {1000}, tone_rate[1] = {8000};

static void test_board_variants(void)
{
    static const uint8_t zeros[0x1000];
    const LCDMachineDef *nmare = lcd_registry_find_id("tnmarebc"), *st = lcd_registry_find_id("kst25");
    const LCDMachineDef *altair = lcd_registry_find_id("naltair"), *shuttle = lcd_registry_find_id("trshutvoy");
    const LCDMachineDef *smb3 = lcd_registry_find_id("nsmb3");
    LCDSampleBank bank = {1, tone_data, tone_length, tone_rate};
    uint8_t keys[LCD_MAX_INPUTS];
    unsigned i, five = LCD_MAX_INPUTS;
    int16_t pcm[64];
    uint8_t *state;
    CHECK(nmare && st && altair && shuttle && smb3);
    if (!nmare || !st || !altair || !shuttle || !smb3)
        return;

    /* Nightmare Before Christmas: S5 and S6 drive the same input line */
    CHECK_EQ(lcd_machine_init(&m2, nmare, zeros, nmare->rom.size), 0);
    lcd_machine_set_buttons(&m2, BTN(L));
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0x20);
    CHECK_EQ(m2.inp_mux, 0x10);
    CHECK_EQ(m2.cpu.bus.read_k(m2.cpu.bus.user), 0x1);

    /* Star Trek: S7 falling edge latches the sample from S1-S5, B is the busy line, S8 low stops */
    for (i = 0; i < 1000; ++i)
        tone[i] = (int16_t)(i & 1 ? 8000 : -8000);
    CHECK_EQ(lcd_machine_init(&m2, st, zeros, st->rom.size), 0);
    CHECK_EQ(m2.cpu.bus.read_b(m2.cpu.bus.user), 1);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0xc0);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0x81);
    CHECK_EQ(m2.adpcm_sample, -1); /* no sample pack: silent and never busy, as MAME */
    lcd_machine_attach_samples(&m2, &bank);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0xc0);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0x81);
    CHECK_EQ(m2.adpcm_sample, 0);
    CHECK_EQ(m2.cpu.bus.read_b(m2.cpu.bus.user), 0);
    lcd_machine_run_pcm(&m2, 64, NULL, pcm);
    CHECK(pcm[0] != 0 || pcm[5] != 0);
    lcd_machine_run(&m2, LCD_OSC_HZ / 8u + 64u, NULL); /* 1000 samples at 8 kHz */
    CHECK_EQ(m2.adpcm_sample, -1);
    CHECK_EQ(m2.cpu.bus.read_b(m2.cpu.bus.user), 1);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0xc0);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0x82); /* sample 2 is not in the bank */
    CHECK_EQ(m2.adpcm_sample, -1);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0xc0);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0x81);
    m2.cpu.bus.write_s(m2.cpu.bus.user, 0x01);
    CHECK_EQ(m2.adpcm_sample, -1);
    /* Up and Down are the same contact (PORT_CONDITION on FAKE) */
    lcd_machine_set_buttons(&m2, BTN(UP));
    CHECK_EQ(m2.ports[0], 0x2);
    lcd_machine_set_buttons(&m2, BTN(DOWN));
    CHECK_EQ(m2.ports[0], 0x2);

    /* Altair: the dosimeter dial steps every 5 frames while held */
    CHECK_EQ(lcd_machine_init(&m2, altair, zeros, altair->rom.size), 0);
    for (i = 0; i < 10; ++i)
        lcd_machine_set_buttons(&m2, BTN(L2));
    CHECK_EQ(m2.ports[0] & 0xf, 2);
    lcd_machine_set_buttons(&m2, 0);
    CHECK_EQ(m2.ports[0] & 0xf, 2);
    for (i = 0; i < 70; ++i)
        lcd_machine_set_buttons(&m2, BTN(L2));
    CHECK_EQ(m2.ports[0] & 0xf, 0); /* 16 steps wrap */

    /* Shuttle Voyage: keypad keys come from the keyboard */
    CHECK_EQ(lcd_machine_init(&m2, shuttle, zeros, shuttle->rom.size), 0);
    for (i = 0; i < LCD_MAX_INPUTS && shuttle->inputs[i].label; ++i)
        if (!strcmp(shuttle->inputs[i].label, "5"))
            five = i;
    CHECK(five < LCD_MAX_INPUTS && shuttle->inputs[five].retro_id == LCD_NO_PAD);
    memset(keys, 0, sizeof(keys));
    if (five < LCD_MAX_INPUTS)
        keys[five] = 1;
    lcd_machine_set_input(&m2, 0, keys);
    CHECK_EQ(m2.ports[1], 0x2);

    /* Super Mario Bros. 3 (SM530): SM500 style LCD, KE inputs on K5-K8, state round trip */
    CHECK_EQ(lcd_machine_init(&m2, smb3, zeros, smb3->rom.size), 0);
    CHECK_EQ(m2.display_x_len, 4);
    CHECK_EQ(m2.display_z_len, 1);
    lcd_machine_set_buttons(&m2, BTN(LEFT));
    CHECK_EQ(m2.cpu.bus.read_k(m2.cpu.bus.user), 0x80);
    lcd_machine_run(&m2, 5000, NULL);
    state = malloc(lcd_machine_state_size());
    CHECK(state && lcd_machine_save(&m2, state, lcd_machine_state_size()));
    m2.cpu.subdiv = 0;
    m2.cpu.count_10ms = 7;
    CHECK(state && lcd_machine_load(&m2, state, lcd_machine_state_size()));
    CHECK(m2.cpu.subdiv != 0);
    CHECK(m2.cpu.count_10ms != 7);
    free(state);
}

int main(void)
{
    load_selftest();
    if (test_failures)
        return test_finish("machine_test");
    test_counter_and_display();
    test_inputs();
    test_piezo();
    test_decay();
    test_acl();
    test_states();
    test_wiring();
    test_clock();
    test_board_variants();
    lcd_content_free(&content);
    return test_finish("machine_test (input matrix, wakeup, decay, piezo, ACL, states, wiring, clock, board variants)");
}
