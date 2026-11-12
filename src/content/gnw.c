#include "gnw.h"
#include "../../include/libretro.h"
#include <stdio.h>
#include <string.h>

#define J(x) RETRO_DEVICE_ID_JOYPAD_##x

bool gnw_detect(const uint8_t *data, size_t size)
{
    size_t rom = size > GNW_ROM_OFFSET ? size - GNW_ROM_OFFSET : 0;
    return data && (rom == 0x740u || rom == 0x800u || rom == 0x1000u) && data[0] == 1 && data[1] <= 8 &&
           data[2] <= 2;
}

bool gnw_run(const uint8_t *data, unsigned n, GnwRun *run)
{
    const uint8_t *p = data + GNW_MASK_OFFSET + (size_t)n * 5u;
    uint64_t v = 0;
    unsigned i;
    if ((size_t)n * 5u + 5u > GNW_MASK_SIZE)
        return false;
    for (i = 0; i < 5; ++i)
        v |= (uint64_t)p[i] << (8u * i);
    run->id = (uint16_t)(v & 0x3ff);
    run->x = (uint16_t)(v >> 10 & 0x3ff);
    run->y = (uint16_t)(v >> 20 & 0x3ff);
    run->length = (uint16_t)(v >> 30 & 0x3ff);
    return run->length != 0 && run->x + run->length <= GNW_SIDE && run->y < GNW_SIDE;
}

uint32_t gnw_pixel(const uint8_t *data, unsigned x, unsigned y, bool lit)
{
    const uint8_t *p = data + GNW_IMAGE_OFFSET + ((size_t)y * GNW_SIDE + x) * 6u + (lit ? 1u : 0u);
    return (uint32_t)p[0] << 16 | (uint32_t)p[2] << 8 | p[4];
}

/* format input codes -> libretro button, label (same scheme as tools/mame_registry.py) */
static const struct {
    int retro_id;
    const char *label;
} inputs[30] = {
    {J(UP), "Up"},       {J(DOWN), "Down"},        {J(LEFT), "Left"},          {J(RIGHT), "Right"},
    {J(A), "Button 1"},  {J(B), "Button 2"},       {J(Y), "Button 3"},         {J(X), "Button 4"},
    {J(L2), "Button 5"}, {J(R2), "Button 6"},      {J(L3), "Button 7"},        {J(R), "Button 8"},
    {J(L), "Time"},      {J(START), "Game A"},     {J(SELECT), "Game B"},      {J(R3), "ACL"},
    {J(R), "Alarm"},     {J(UP), "Left Up"},       {J(DOWN), "Left Down"},     {J(LEFT), "Left Left"},
    {J(RIGHT), "Left Right"}, {J(X), "Right Up"},  {J(B), "Right Down"},       {J(Y), "Right Left"},
    {J(A), "Right Right"}, {J(R2), "Sound"},       {J(START), "Power On"},     {J(L2), "Power Off"},
    {J(L3), "Key"},      {J(L3), "Custom"},
};

static const unsigned fallback[] = {J(L2), J(R2), J(L3), J(X), J(Y), J(B), J(A), J(R), J(L), J(SELECT)};

int gnw_make_def(const uint8_t *data, size_t size, LCDMachineDef *def, char *err, size_t err_len)
{
    static const SM5xxVariant cpus[9] = {SM5XX_SM510, SM5XX_SM511, SM5XX_SM512, SM5XX_SM510, SM5XX_SM5A,
                                         SM5XX_SM510, SM5XX_SM511, SM5XX_SM511, SM5XX_SM5A};
    const uint8_t *in = data + 8;
    unsigned port, bit, n = 0, used = 0, highest = 0;
    uint8_t ground;
    if (!gnw_detect(data, size)) {
        snprintf(err, err_len, "Not a version 1 .gnw file");
        return -1;
    }
    if (data[1] == 3) {
        snprintf(err, err_len, "This .gnw needs the SM530 CPU, which LCD Lab does not emulate yet");
        return -1;
    }
    memset(def, 0, sizeof(*def));
    def->id = "gnw_file";
    def->title = "Game & Watch FPGA (.gnw)";
    def->year = "";
    def->cpu = cpus[data[1]];
    def->r_mask_option = (data[1] == 0) ? 2 : SM5XX_RMASK_DIRECT;
    def->wiring = def->cpu == SM5XX_SM5A ? LCD_WIRING_SM5A_R1_PIEZO_MUX
                  : data[1] == 7         ? LCD_WIRING_SM511_TIGER2BIT
                                         : LCD_WIRING_SM510_S_MUX_R1;
    def->rom.name = "gnw rom";
    def->rom.size = (uint32_t)(size - GNW_ROM_OFFSET);
    def->region_size = def->rom.size <= 0x800u ? 0x800u : 0x1000u;
    def->screen_count = 1;
    def->ba_rest = 1;
    def->b_rest = 1;

    /* 8 S ports of 4 bits, then B, BA, ACL, grounded port (1 based, 0 when none) */
    for (port = 0; port < 11; ++port)
        for (bit = 0; bit < (port < 8 ? 4u : 1u); ++bit) {
            uint8_t v = in[port < 8 ? port * 4u + bit : 32u + (port - 8u)];
            unsigned code = v & 0x1f, id;
            bool low = (v & 0x80) != 0;
            LCDInputDef *d;
            if ((v & 0x7f) == 0x7f) {
                if (low && port == 8)
                    def->b_rest = 1;
                continue;
            }
            if (code >= 30 || n >= LCD_MAX_INPUTS - 1)
                continue;
            id = (unsigned)inputs[code].retro_id;
            if (used >> id & 1) {
                unsigned k;
                for (k = 0; k < sizeof(fallback) / sizeof(fallback[0]) && (used >> fallback[k] & 1); ++k)
                    ;
                if (k == sizeof(fallback) / sizeof(fallback[0]))
                    continue;
                id = fallback[k];
            }
            used |= 1u << id;
            d = &def->inputs[n++];
            d->retro_id = id;
            d->mask = (uint8_t)(1u << bit);
            d->active_low = low;
            d->label = inputs[code].label;
            if (port < 8) {
                d->port = (uint8_t)(LCD_PORT_IN0 + port);
                if (low)
                    def->in_rest[port] |= d->mask;
                if (port + 1u > highest)
                    highest = port + 1u;
            } else {
                d->port = port == 8 ? LCD_PORT_B : port == 9 ? LCD_PORT_BA : LCD_PORT_ACL;
                if (port == 8)
                    def->b_rest = low ? 1 : 0;
                if (port == 9)
                    def->ba_rest = low ? 1 : 0;
            }
        }
    ground = in[35];
    if (ground && ground <= 8) {
        def->inp_fixed = (int8_t)(ground - 1);
        def->inp_lines = (uint8_t)(ground - 1);
    } else {
        def->inp_fixed = -1;
        def->inp_lines = (uint8_t)highest;
    }
    return 0;
}
