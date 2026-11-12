#include "game.h"
#include <string.h>

static uint32_t next_rng(LCDGame *g)
{
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return x;
}

void lcd_game_reset(LCDGame *g)
{
    uint32_t best = g->best;
    memset(g, 0, sizeof(*g));
    g->best = best;
    g->rng = UINT32_C(0xCB7A51D3);
    g->lane = 1;
    g->mode = LCD_READY;
}

static void start_game(LCDGame *g)
{
    uint32_t best = g->best;
    lcd_game_reset(g);
    g->best = best;
    g->mode = LCD_PLAYING;
    g->balls[1][0] = 1;
}

static unsigned shift_balls(LCDGame *g)
{
    unsigned event = LCD_SOUND_NONE;
    unsigned lane, row;
    for (lane = 0; lane < LCD_LANES; ++lane) {
        if (g->balls[lane][LCD_ROWS - 1]) {
            if (g->lane == lane) {
                ++g->score;
                if (g->score > g->best)
                    g->best = g->score;
                event = LCD_SOUND_SCORE;
            } else {
                ++g->misses;
                event = LCD_SOUND_MISS;
            }
        }
        for (row = LCD_ROWS - 1; row > 0; --row)
            g->balls[lane][row] = g->balls[lane][row - 1];
        g->balls[lane][0] = 0;
    }
    if (g->misses >= 3) {
        g->mode = LCD_FINISHED;
        return LCD_SOUND_END;
    }
    if ((g->frame / 13u) % 4u != 0u || g->score < 3u) {
        unsigned next_lane = next_rng(g) % LCD_LANES;
        g->balls[next_lane][0] = 1;
    }
    return event;
}

unsigned lcd_game_frame(LCDGame *g, uint32_t buttons, bool fast)
{
    uint32_t pressed = buttons & ~g->buttons_previous;
    unsigned event = LCD_SOUND_NONE;
    g->buttons_previous = buttons;

    if (g->mode != LCD_PLAYING) {
        if (pressed & (LCD_BTN_START | LCD_BTN_ACTION)) {
            start_game(g);
            /* Hold buttons after start; no repeated accidental restart. */
            g->buttons_previous = buttons;
            return LCD_SOUND_START;
        }
        return LCD_SOUND_NONE;
    }

    if (pressed & LCD_BTN_PAUSE)
        g->paused = !g->paused;
    if (g->paused)
        return LCD_SOUND_NONE;

    /* First move immediately, then repeat after 12 frames and every 5. */
    if ((buttons & LCD_BTN_LEFT) && !(buttons & LCD_BTN_RIGHT)) {
        if ((g->left_repeat == 0u ||
             (g->left_repeat >= 12u && (g->left_repeat - 12u) % 5u == 0u)) &&
            g->lane > 0u)
            --g->lane;
        if (g->left_repeat < 250u)
            ++g->left_repeat;
        g->right_repeat = 0u;
    } else if ((buttons & LCD_BTN_RIGHT) && !(buttons & LCD_BTN_LEFT)) {
        if ((g->right_repeat == 0u ||
             (g->right_repeat >= 12u && (g->right_repeat - 12u) % 5u == 0u)) &&
            g->lane + 1u < LCD_LANES)
            ++g->lane;
        if (g->right_repeat < 250u)
            ++g->right_repeat;
        g->left_repeat = 0u;
    } else {
        g->left_repeat = 0u;
        g->right_repeat = 0u;
    }

    ++g->frame;
    if (++g->shift_timer >= (fast ? 12u : (g->score >= 60u ? 12u :
                      g->score >= 30u ? 16u : g->score >= 10u ? 20u : 26u))) {
        g->shift_timer = 0;
        event = shift_balls(g);
    }
    return event;
}
