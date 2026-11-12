#include "diagnostics/game.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    LCDGame g = {0};
    unsigned i;
    lcd_game_reset(&g);
    assert(g.mode == LCD_READY && g.lane == 1);
    assert(lcd_game_frame(&g, LCD_BTN_START, false) == LCD_SOUND_START);
    assert(g.mode == LCD_PLAYING && g.balls[1][0]);
    lcd_game_frame(&g, 0, false);

    /* Right immediately, hold, then left; no spurious movement. */
    lcd_game_frame(&g, LCD_BTN_RIGHT, false);
    assert(g.lane == 2);
    for (i = 0; i < 20; ++i)
        lcd_game_frame(&g, LCD_BTN_RIGHT, false);
    assert(g.lane == 2);
    lcd_game_frame(&g, LCD_BTN_LEFT, false);
    assert(g.lane == 1);
    lcd_game_frame(&g, 0, false);
    lcd_game_frame(&g, LCD_BTN_LEFT, false);
    assert(g.lane == 0);
    lcd_game_frame(&g, 0, false);

    /* Pause must freeze gameplay clocks. */
    lcd_game_frame(&g, LCD_BTN_PAUSE, false);
    assert(g.paused);
    {
        unsigned before = g.frame;
        for (i = 0; i < 300; ++i) lcd_game_frame(&g, 0, false);
        assert(g.frame == before);
    }
    lcd_game_frame(&g, LCD_BTN_PAUSE, false);
    assert(!g.paused);

    /* Inject a nearly-expired object and test success. */
    memset(g.balls, 0, sizeof(g.balls));
    g.lane = 0;
    g.balls[0][4] = 1;
    g.shift_timer = 25;
    assert(lcd_game_frame(&g, 0, false) == LCD_SOUND_SCORE);
    assert(g.score == 1 && g.best == 1 && g.misses == 0);

    /* Three misses end the round and retain high score. */
    for (i = 0; i < 3; ++i) {
        memset(g.balls, 0, sizeof(g.balls));
        g.lane = 0;
        g.balls[2][4] = 1;
        g.shift_timer = 25;
        (void)lcd_game_frame(&g, 0, false);
        assert(g.misses == i + 1);
    }
    assert(g.mode == LCD_FINISHED);
    lcd_game_frame(&g, LCD_BTN_START, false);
    assert(g.mode == LCD_PLAYING && g.score == 0 && g.best == 1);
    printf("PASS: gameplay movement, repeat, pause, score, misses, restart.\n");
    return 0;
}
