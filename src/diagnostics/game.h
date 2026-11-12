#ifndef LCD_GAME_H
#define LCD_GAME_H
#include <stdint.h>
#include <stdbool.h>

#define LCD_LANES 3
#define LCD_ROWS 5

enum lcd_mode { LCD_READY = 0, LCD_PLAYING = 1, LCD_FINISHED = 2 };
enum lcd_sound_event { LCD_SOUND_NONE = 0, LCD_SOUND_START = 1,
                       LCD_SOUND_SCORE = 2, LCD_SOUND_MISS = 3,
                       LCD_SOUND_END = 4 };

enum lcd_button {
    LCD_BTN_LEFT   = 1u << 0,
    LCD_BTN_RIGHT  = 1u << 1,
    LCD_BTN_START  = 1u << 2,
    LCD_BTN_ACTION = 1u << 3,
    LCD_BTN_PAUSE  = 1u << 4
};

typedef struct {
    uint32_t rng;
    uint32_t frame;
    uint32_t score;
    uint32_t best;
    uint32_t buttons_previous;
    uint8_t balls[LCD_LANES][LCD_ROWS];
    uint8_t lane;
    uint8_t misses;
    uint8_t mode;
    uint8_t paused;
    uint8_t shift_timer;
    uint8_t left_repeat;
    uint8_t right_repeat;
    uint8_t reserved;
} LCDGame;

void lcd_game_reset(LCDGame *g);
unsigned lcd_game_frame(LCDGame *g, uint32_t buttons, bool fast); 
#endif
