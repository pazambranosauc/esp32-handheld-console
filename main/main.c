#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_log.h"
#include "ssd1306.h"

//  HARDWARE CONSTANTS
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_NUM 0
#define I2C_MASTER_FREQ_HZ 400000
#define SSD1306_I2C_ADDRESS 0x3C

#define JOY_VRX_CHANNEL ADC1_CHANNEL_6
#define JOY_VRY_CHANNEL ADC1_CHANNEL_7
#define JOY_SW_GPIO 32

#define JOY_DEADZONE 1000
#define JOY_CENTER 2048

#define LOOP_PERIOD_MS 50     /* 20 fps */
#define INPUT_DEBOUNCE_MS 200 /* menu navigation debounce */
#define GAME_DEBOUNCE_MS 120  /* tighter debounce inside games */

#define SCREEN_W 128
#define SCREEN_H 64

static const char *TAG = "GameConsole";

//  GLOBAL STATE
ssd1306_handle_t ssd1306_dev = NULL;

typedef enum
{
    SCREEN_HOME,
    SCREEN_FLAPPY_SPLASH,
    SCREEN_FLAPPY_GAME,
    SCREEN_FLAPPY_OVER,
    SCREEN_SNAKE_SPLASH,
    SCREEN_SNAKE_GAME,
    SCREEN_SNAKE_OVER,
    SCREEN_TETRIS_SPLASH,
    SCREEN_TETRIS_GAME,
    SCREEN_TETRIS_OVER,
} screen_t;

static screen_t current_screen = SCREEN_HOME;
static int menu_cursor = 0;
static bool redraw_needed = true;
static int splash_cursor = 0; /* 0=PLAY  1=EXIT  (splash screens) */
static int over_cursor = 0;   /* 0=AGAIN 1=EXIT  (game-over screen) */
static TickType_t last_input_tick = 0;

#define MENU_ITEMS 3
static const char *menu_labels[MENU_ITEMS] = {
    "1 Bird", "2 Snake", "3 Tetris"};

//  SIMPLE XOR-SHIFT RNG
static uint32_t rng_state = 0xDEADBEEF;
static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static int rng_range(int lo, int hi) /* [lo, hi) */
{
    return lo + (int)(rng_next() % (uint32_t)(hi - lo));
}

//  DRAW PRIMITIVES
static void draw_pixel(int x, int y, uint8_t c)
{
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        ssd1306_fill_point(ssd1306_dev, (uint8_t)x, (uint8_t)y, c);
}

static void draw_filled_rect(int x, int y, int w, int h, uint8_t c)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            draw_pixel(col, row, c);
}

static void draw_rect(int x, int y, int w, int h, uint8_t c)
{
    for (int col = x; col < x + w; col++)
    {
        draw_pixel(col, y, c);
        draw_pixel(col, y + h - 1, c);
    }
    for (int row = y; row < y + h; row++)
    {
        draw_pixel(x, row, c);
        draw_pixel(x + w - 1, row, c);
    }
}

static void draw_hline(int x, int y, int len, uint8_t c)
{
    for (int i = 0; i < len; i++)
        draw_pixel(x + i, y, c);
}

static void draw_vline(int x, int y, int len, uint8_t c)
{
    for (int i = 0; i < len; i++)
        draw_pixel(x, y + i, c);
}

/* Bresenham midpoint circle — outline and filled variants */
static void _circle_pts(int cx, int cy, int px, int py, uint8_t c, bool fill)
{
    if (fill)
    {
        draw_hline(cx - px, cy + py, 2 * px + 1, c);
        draw_hline(cx - px, cy - py, 2 * px + 1, c);
        draw_hline(cx - py, cy + px, 2 * py + 1, c);
        draw_hline(cx - py, cy - px, 2 * py + 1, c);
    }
    else
    {
        draw_pixel(cx + px, cy + py, c);
        draw_pixel(cx - px, cy + py, c);
        draw_pixel(cx + px, cy - py, c);
        draw_pixel(cx - px, cy - py, c);
        draw_pixel(cx + py, cy + px, c);
        draw_pixel(cx - py, cy + px, c);
        draw_pixel(cx + py, cy - px, c);
        draw_pixel(cx - py, cy - px, c);
    }
}
static void _circle(int cx, int cy, int r, uint8_t c, bool fill)
{
    int x = 0, y = r, d = 1 - r;
    while (x <= y)
    {
        _circle_pts(cx, cy, x, y, c, fill);
        d += (d < 0) ? (2 * x + 3) : (2 * (x - y) + 5);
        if (d >= 0)
            y--;
        x++;
    }
}
static void draw_circle(int cx, int cy, int r, uint8_t c)
{
    _circle(cx, cy, r, c, false);
}
static void draw_filled_circle(int cx, int cy, int r, uint8_t c)
{
    _circle(cx, cy, r, c, true);
}

//  SHARED UI HELPERS
/* Generic splash layout: title + divider + ground + PLAY/EXIT buttons */
static void draw_splash_generic(const char *title)
{
    ssd1306_clear_screen(ssd1306_dev, 0x00);
    ssd1306_draw_string(ssd1306_dev, 10, 0, (const uint8_t *)title, 12, 1);
    draw_hline(0, 11, SCREEN_W, 1);
    draw_hline(0, 52, SCREEN_W, 1);

    if (splash_cursor == 0)
    {
        draw_filled_rect(8, 54, 46, 10, 1);
        ssd1306_draw_string(ssd1306_dev, 14, 55, (const uint8_t *)"PLAY", 12, 0);
    }
    else
    {
        draw_rect(8, 54, 46, 10, 1);
        ssd1306_draw_string(ssd1306_dev, 14, 55, (const uint8_t *)"PLAY", 12, 1);
    }
    if (splash_cursor == 1)
    {
        draw_filled_rect(74, 54, 46, 10, 1);
        ssd1306_draw_string(ssd1306_dev, 80, 55, (const uint8_t *)"EXIT", 12, 0);
    }
    else
    {
        draw_rect(74, 54, 46, 10, 1);
        ssd1306_draw_string(ssd1306_dev, 80, 55, (const uint8_t *)"EXIT", 12, 1);
    }
}

static void draw_game_over(int score)
{
    ssd1306_clear_screen(ssd1306_dev, 0x00);
    ssd1306_draw_string(ssd1306_dev, 26, 4, (const uint8_t *)"GAME OVER", 12, 1);
    char buf[20];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    ssd1306_draw_string(ssd1306_dev, 30, 18, (const uint8_t *)buf, 12, 1);
    draw_hline(0, 30, SCREEN_W, 1);

    /* AGAIN button */
    if (over_cursor == 0)
    {
        draw_filled_rect(4, 34, 52, 12, 1);
        ssd1306_draw_string(ssd1306_dev, 8, 36, (const uint8_t *)"AGAIN", 12, 0);
    }
    else
    {
        draw_rect(4, 34, 52, 12, 1);
        ssd1306_draw_string(ssd1306_dev, 8, 36, (const uint8_t *)"AGAIN", 12, 1);
    }
    /* EXIT button */
    if (over_cursor == 1)
    {
        draw_filled_rect(72, 34, 52, 12, 1);
        ssd1306_draw_string(ssd1306_dev, 80, 36, (const uint8_t *)"EXIT", 12, 0);
    }
    else
    {
        draw_rect(72, 34, 52, 12, 1);
        ssd1306_draw_string(ssd1306_dev, 80, 36, (const uint8_t *)"EXIT", 12, 1);
    }

    ssd1306_refresh_gram(ssd1306_dev);
}

/* Home / game-selector screen */
static void draw_home(void)
{
    ssd1306_clear_screen(ssd1306_dev, 0x00);
    ssd1306_draw_string(ssd1306_dev, 10, 0, (const uint8_t *)"GAME SELECTOR", 12, 1);
    draw_hline(0, 11, SCREEN_W, 1);
    for (int i = 0; i < MENU_ITEMS; i++)
    {
        uint8_t y = 14 + i * 12;
        if (i == menu_cursor)
        {
            draw_filled_rect(0, y - 1, SCREEN_W, 13, 1);
            ssd1306_draw_string(ssd1306_dev, 2, y, (const uint8_t *)">", 12, 0);
            ssd1306_draw_string(ssd1306_dev, 14, y, (const uint8_t *)menu_labels[i], 12, 0);
        }
        else
        {
            ssd1306_draw_string(ssd1306_dev, 14, y, (const uint8_t *)menu_labels[i], 12, 1);
        }
    }
    ssd1306_refresh_gram(ssd1306_dev);
}

#define SMILEY_R 4 /* smaller bird — was 6 */
#define SMILEY_W 10
#define SMILEY_H 10

static void draw_smiley(int cx, int cy)
{
    draw_circle(cx, cy, SMILEY_R, 1);
    draw_filled_circle(cx - 2, cy - 2, 1, 1); /* left eye  */
    draw_filled_circle(cx + 2, cy - 2, 1, 1); /* right eye */
    draw_hline(cx - 2, cy + 2, 5, 1);         /* mouth     */
}

//  FLAPPY BIRD
#define FB_GRAVITY 1
#define FB_FLAP -5
#define FB_PIPE_SPEED 2
#define FB_PIPE_W 8
#define FB_GAP_H 28 /* wider gap — was 18 */
#define FB_GROUND_Y 56
#define FB_PIPE_SPACING 55
#define FB_NUM_PIPES 3
#define FB_BIRD_X 20 /* fixed horizontal position of bird */

typedef struct
{
    int x, gap_y;
    bool scored;
} fb_pipe_t;

static struct
{
    int bird_y, bird_vy;
    fb_pipe_t pipes[FB_NUM_PIPES];
    int score;
    bool alive;
} fb;

static void fb_reset_pipes(void)
{
    for (int i = 0; i < FB_NUM_PIPES; i++)
    {
        fb.pipes[i].x = SCREEN_W + i * FB_PIPE_SPACING;
        fb.pipes[i].gap_y = rng_range(14, FB_GROUND_Y - FB_GAP_H - 2);
        fb.pipes[i].scored = false;
    }
}

static void fb_start(void)
{
    fb.bird_y = 30;
    fb.bird_vy = 0;
    fb.score = 0;
    fb.alive = true;
    fb_reset_pipes();
}

static void fb_draw_pipe_shape(fb_pipe_t *p)
{
    /* top pipe */
    draw_filled_rect(p->x + 1, 12, FB_PIPE_W - 2, p->gap_y - 12, 1);
    draw_filled_rect(p->x, p->gap_y - 4, FB_PIPE_W, 4, 1);
    /* bottom pipe */
    int bot = p->gap_y + FB_GAP_H;
    draw_filled_rect(p->x, bot, FB_PIPE_W, 4, 1);
    draw_filled_rect(p->x + 1, bot + 4, FB_PIPE_W - 2, FB_GROUND_Y - (bot + 4), 1);
}

static void fb_tick(void)
{
    fb.bird_vy += FB_GRAVITY;
    fb.bird_y += fb.bird_vy;

    if (fb.bird_y + SMILEY_R >= FB_GROUND_Y || fb.bird_y - SMILEY_R <= 11)
    {
        fb.alive = false;
        return;
    }
    for (int i = 0; i < FB_NUM_PIPES; i++)
    {
        fb.pipes[i].x -= FB_PIPE_SPEED;
        if (fb.pipes[i].x + FB_PIPE_W < 0)
        {
            fb.pipes[i].x = SCREEN_W + 4;
            fb.pipes[i].gap_y = rng_range(14, FB_GROUND_Y - FB_GAP_H - 2);
            fb.pipes[i].scored = false;
        }
        if (!fb.pipes[i].scored && fb.pipes[i].x + FB_PIPE_W < FB_BIRD_X - SMILEY_R)
        {
            fb.score++;
            fb.pipes[i].scored = true;
        }
        bool in_x = (FB_BIRD_X + SMILEY_R > fb.pipes[i].x) &&
                    (FB_BIRD_X - SMILEY_R < fb.pipes[i].x + FB_PIPE_W);
        bool in_y = (fb.bird_y - SMILEY_R < fb.pipes[i].gap_y) ||
                    (fb.bird_y + SMILEY_R > fb.pipes[i].gap_y + FB_GAP_H);
        if (in_x && in_y)
        {
            fb.alive = false;
            return;
        }
    }
}

static void fb_draw_game(void)
{
    ssd1306_clear_screen(ssd1306_dev, 0x00);
    draw_hline(0, 11, SCREEN_W, 1);
    draw_hline(0, FB_GROUND_Y, SCREEN_W, 1);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", fb.score);
    ssd1306_draw_string(ssd1306_dev, 0, 0, (const uint8_t *)buf, 12, 1);
    for (int i = 0; i < FB_NUM_PIPES; i++)
        fb_draw_pipe_shape(&fb.pipes[i]);
    draw_smiley(FB_BIRD_X, fb.bird_y);
    ssd1306_refresh_gram(ssd1306_dev);
}

static void fb_draw_splash(void)
{
    draw_splash_generic("FLAPPY BIRD");
    /* decorative pipes */
    fb_pipe_t d1 = {.x = 10, .gap_y = 30, .scored = false};
    fb_pipe_t d2 = {.x = 90, .gap_y = 26, .scored = false};
    fb_draw_pipe_shape(&d1);
    fb_draw_pipe_shape(&d2);
    draw_smiley(56, 31);
    ssd1306_refresh_gram(ssd1306_dev);
}

//    SNAKE
#define SN_CELL 4
#define SN_COLS (SCREEN_W / SN_CELL)        /* 32 */
#define SN_ROWS ((SCREEN_H - 12) / SN_CELL) /* 13 */
#define SN_HUD_H 12
#define SN_MAX 128

typedef struct
{
    int8_t x, y;
} sn_pt_t;

static struct
{
    sn_pt_t body[SN_MAX];
    int len;
    sn_pt_t dir;
    sn_pt_t food;
    int score;
    bool alive;
} sn;

static bool sn_on_body(int x, int y)
{
    for (int i = 0; i < sn.len; i++)
        if (sn.body[i].x == x && sn.body[i].y == y)
            return true;
    return false;
}
static void sn_place_food(void)
{
    int x, y;
    do
    {
        x = rng_range(0, SN_COLS);
        y = rng_range(0, SN_ROWS);
    } while (sn_on_body(x, y));
    sn.food = (sn_pt_t){(int8_t)x, (int8_t)y};
}
static void sn_start(void)
{
    memset(&sn, 0, sizeof(sn));
    sn.len = 3;
    sn.body[0] = (sn_pt_t){SN_COLS / 2, SN_ROWS / 2};
    sn.body[1] = (sn_pt_t){SN_COLS / 2 - 1, SN_ROWS / 2};
    sn.body[2] = (sn_pt_t){SN_COLS / 2 - 2, SN_ROWS / 2};
    sn.dir = (sn_pt_t){1, 0};
    sn.alive = true;
    sn_place_food();
}
static void sn_tick(void)
{
    sn_pt_t head = {(int8_t)(sn.body[0].x + sn.dir.x),
                    (int8_t)(sn.body[0].y + sn.dir.y)};
    if (head.x < 0 || head.x >= SN_COLS || head.y < 0 || head.y >= SN_ROWS)
    {
        sn.alive = false;
        return;
    }
    for (int i = 0; i < sn.len - 1; i++)
        if (sn.body[i].x == head.x && sn.body[i].y == head.y)
        {
            sn.alive = false;
            return;
        }
    bool ate = (head.x == sn.food.x && head.y == sn.food.y);
    if (ate && sn.len < SN_MAX)
        sn.len++;
    for (int i = sn.len - 1; i > 0; i--)
        sn.body[i] = sn.body[i - 1];
    sn.body[0] = head;
    if (ate)
    {
        sn.score++;
        sn_place_food();
    }
}
static void sn_draw_game(void)
{
    ssd1306_clear_screen(ssd1306_dev, 0x00);
    char buf[16];
    snprintf(buf, sizeof(buf), "Score:%d", sn.score);
    ssd1306_draw_string(ssd1306_dev, 0, 0, (const uint8_t *)buf, 12, 1);
    draw_hline(0, SN_HUD_H - 1, SCREEN_W, 1);
    int yo = SN_HUD_H;
    /* food */
    draw_filled_rect(sn.food.x * SN_CELL + 1,
                     sn.food.y * SN_CELL + yo + 1,
                     SN_CELL - 2, SN_CELL - 2, 1);
    /* body */
    for (int i = 0; i < sn.len; i++)
    {
        int px = sn.body[i].x * SN_CELL;
        int py = sn.body[i].y * SN_CELL + yo;
        if (i == 0)
            draw_filled_rect(px, py, SN_CELL, SN_CELL, 1);
        else
            draw_rect(px, py, SN_CELL, SN_CELL, 1);
    }
    ssd1306_refresh_gram(ssd1306_dev);
}
static void sn_draw_splash(void)
{
    draw_splash_generic("    SNAKE");
    /* decorative snake body */
    for (int i = 0; i < 8; i++)
        draw_filled_rect(18 + i * (SN_CELL + 1), 28, SN_CELL, SN_CELL, 1);
    draw_filled_circle(18 + 8 * (SN_CELL + 1) + SN_CELL / 2, 28 + SN_CELL / 2, 3, 1);
    ssd1306_refresh_gram(ssd1306_dev);
}

//    TETRIS
#define TT_COLS 10
#define TT_ROWS 16
#define TT_CELL 4
#define TT_BOARD_X ((SCREEN_W - TT_COLS * TT_CELL) / 2) /* 44 */
#define TT_TICK_MS 400

static const int8_t TT_SHAPES[7][4][2] = {
    /* I */ {{0, 0}, {0, 1}, {0, 2}, {0, 3}},
    /* O */ {{0, 0}, {0, 1}, {1, 0}, {1, 1}},
    /* T */ {{0, 1}, {1, 0}, {1, 1}, {1, 2}},
    /* S */ {{0, 1}, {0, 2}, {1, 0}, {1, 1}},
    /* Z */ {{0, 0}, {0, 1}, {1, 1}, {1, 2}},
    /* J */ {{0, 0}, {1, 0}, {1, 1}, {1, 2}},
    /* L */ {{0, 2}, {1, 0}, {1, 1}, {1, 2}},
};

static struct
{
    uint8_t board[TT_ROWS][TT_COLS];
    int8_t piece[4][2];
    int px, py;
    int score, lines;
    bool alive;
    TickType_t last_grav;
} tt;

static bool tt_valid(int px, int py, const int8_t p[4][2])
{
    for (int i = 0; i < 4; i++)
    {
        int c = px + p[i][1], r = py + p[i][0];
        if (c < 0 || c >= TT_COLS || r < 0 || r >= TT_ROWS)
            return false;
        if (tt.board[r][c])
            return false;
    }
    return true;
}
static void tt_spawn(void)
{
    int k = rng_range(0, 7);
    memcpy(tt.piece, TT_SHAPES[k], sizeof(tt.piece));
    tt.px = TT_COLS / 2 - 1;
    tt.py = 0;
}
static void tt_lock_and_clear(void)
{
    for (int i = 0; i < 4; i++)
        tt.board[tt.py + tt.piece[i][0]][tt.px + tt.piece[i][1]] = 1;
    for (int r = TT_ROWS - 1; r >= 0; r--)
    {
        bool full = true;
        for (int c = 0; c < TT_COLS; c++)
            if (!tt.board[r][c])
            {
                full = false;
                break;
            }
        if (full)
        {
            for (int rr = r; rr > 0; rr--)
                memcpy(tt.board[rr], tt.board[rr - 1], TT_COLS);
            memset(tt.board[0], 0, TT_COLS);
            tt.score += 10;
            tt.lines++;
            r++;
        }
    }
}
static void tt_rotate(void)
{
    int8_t rot[4][2];
    for (int i = 0; i < 4; i++)
    {
        rot[i][0] = tt.piece[i][1];
        rot[i][1] = -tt.piece[i][0];
    }
    int8_t mr = 127, mc = 127;
    for (int i = 0; i < 4; i++)
    {
        if (rot[i][0] < mr)
            mr = rot[i][0];
        if (rot[i][1] < mc)
            mc = rot[i][1];
    }
    for (int i = 0; i < 4; i++)
    {
        rot[i][0] -= mr;
        rot[i][1] -= mc;
    }
    if (tt_valid(tt.px, tt.py, rot))
        memcpy(tt.piece, rot, sizeof(tt.piece));
}
static void tt_start(void)
{
    memset(&tt, 0, sizeof(tt));
    tt.alive = true;
    tt.last_grav = xTaskGetTickCount();
    tt_spawn();
}
static void tt_gravity(void)
{
    if (tt_valid(tt.px, tt.py + 1, tt.piece))
    {
        tt.py++;
    }
    else
    {
        tt_lock_and_clear();
        tt_spawn();
        if (!tt_valid(tt.px, tt.py, tt.piece))
            tt.alive = false;
    }
}
static void tt_draw_game(void)
{
    ssd1306_clear_screen(ssd1306_dev, 0x00);
    draw_rect(TT_BOARD_X - 1, 0, TT_COLS * TT_CELL + 2, TT_ROWS * TT_CELL + 2, 1);
    for (int r = 0; r < TT_ROWS; r++)
        for (int c = 0; c < TT_COLS; c++)
            if (tt.board[r][c])
                draw_filled_rect(TT_BOARD_X + c * TT_CELL, r * TT_CELL, TT_CELL - 1, TT_CELL - 1, 1);
    for (int i = 0; i < 4; i++)
        draw_rect(TT_BOARD_X + (tt.px + tt.piece[i][1]) * TT_CELL,
                  (tt.py + tt.piece[i][0]) * TT_CELL, TT_CELL - 1, TT_CELL - 1, 1);
    char buf[12];
    snprintf(buf, sizeof(buf), "Sc:%d", tt.score);
    ssd1306_draw_string(ssd1306_dev, 0, 0, (const uint8_t *)buf, 12, 1);
    snprintf(buf, sizeof(buf), "L:%d", tt.lines);
    ssd1306_draw_string(ssd1306_dev, 0, 12, (const uint8_t *)buf, 12, 1);
    ssd1306_refresh_gram(ssd1306_dev);
}
static void tt_draw_splash(void)
{
    draw_splash_generic("   TETRIS");
    int bx = 50, by = 22;
    draw_filled_rect(bx + TT_CELL, by, TT_CELL, TT_CELL, 1);
    draw_filled_rect(bx, by + TT_CELL, TT_CELL, TT_CELL, 1);
    draw_filled_rect(bx + TT_CELL, by + TT_CELL, TT_CELL, TT_CELL, 1);
    draw_filled_rect(bx + TT_CELL * 2, by + TT_CELL, TT_CELL, TT_CELL, 1);
    ssd1306_refresh_gram(ssd1306_dev);
}

//  HARDWARE INIT
static void oled_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0));
    ssd1306_dev = ssd1306_create(I2C_MASTER_NUM, SSD1306_I2C_ADDRESS);
    if (!ssd1306_dev)
    {
        ESP_LOGE(TAG, "SSD1306 failed");
        return;
    }
    ssd1306_refresh_gram(ssd1306_dev);
    ssd1306_clear_screen(ssd1306_dev, 0x00);
    ESP_LOGI(TAG, "OLED ready");
}

static void joystick_init(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(JOY_VRX_CHANNEL, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(JOY_VRY_CHANNEL, ADC_ATTEN_DB_11);
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << JOY_SW_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
    ESP_LOGI(TAG, "Joystick ready");
}

static void read_input(void)
{
    TickType_t now = xTaskGetTickCount();
    int vrx = adc1_get_raw(JOY_VRX_CHANNEL);
    int vry = adc1_get_raw(JOY_VRY_CHANNEL);
    bool btn = (gpio_get_level(JOY_SW_GPIO) == 0);

    bool up = (vry < JOY_CENTER - JOY_DEADZONE);
    bool down = (vry > JOY_CENTER + JOY_DEADZONE);
    bool left = (vrx < JOY_CENTER - JOY_DEADZONE);
    bool right = (vrx > JOY_CENTER + JOY_DEADZONE);

    bool deb = (now - last_input_tick) >= pdMS_TO_TICKS(INPUT_DEBOUNCE_MS);
    bool gdeb = (now - last_input_tick) >= pdMS_TO_TICKS(GAME_DEBOUNCE_MS);

    bool moved = false;

    /* ── HOME ── */
    if (current_screen == SCREEN_HOME)
    {
        if (!deb)
            return;
        if (up)
        {
            menu_cursor = (menu_cursor - 1 + MENU_ITEMS) % MENU_ITEMS;
            moved = true;
        }
        if (down)
        {
            menu_cursor = (menu_cursor + 1) % MENU_ITEMS;
            moved = true;
        }
        if (btn && !moved)
        {
            splash_cursor = 0;
            switch (menu_cursor)
            {
            case 0:
                current_screen = SCREEN_FLAPPY_SPLASH;
                break;
            case 1:
                current_screen = SCREEN_SNAKE_SPLASH;
                break;
            case 2:
                current_screen = SCREEN_TETRIS_SPLASH;
                break;
            }
            moved = true;
        }
    }

    /* ── SPLASHES (shared) ── */
    else if (current_screen == SCREEN_FLAPPY_SPLASH ||
             current_screen == SCREEN_SNAKE_SPLASH ||
             current_screen == SCREEN_TETRIS_SPLASH)
    {
        if (!deb)
            return;
        if (left)
        {
            splash_cursor = 0;
            moved = true;
        }
        if (right)
        {
            splash_cursor = 1;
            moved = true;
        }
        if (btn && !moved)
        {
            if (splash_cursor == 1)
            {
                current_screen = SCREEN_HOME;
            }
            else
            {
                if (current_screen == SCREEN_FLAPPY_SPLASH)
                {
                    fb_start();
                    current_screen = SCREEN_FLAPPY_GAME;
                }
                else if (current_screen == SCREEN_SNAKE_SPLASH)
                {
                    sn_start();
                    current_screen = SCREEN_SNAKE_GAME;
                }
                else if (current_screen == SCREEN_TETRIS_SPLASH)
                {
                    tt_start();
                    current_screen = SCREEN_TETRIS_GAME;
                }
            }
            moved = true;
        }
    }

    /* ── GAME OVER (all three games share this handler) ── */
    else if (current_screen == SCREEN_FLAPPY_OVER ||
             current_screen == SCREEN_SNAKE_OVER ||
             current_screen == SCREEN_TETRIS_OVER)
    {
        if (!deb)
            return;
        if (left)
        {
            over_cursor = 0;
            moved = true;
        } /* highlight AGAIN */
        if (right)
        {
            over_cursor = 1;
            moved = true;
        } /* highlight EXIT  */
        if (btn && !moved)
        {
            if (over_cursor == 1)
            {
                /* EXIT → home menu */
                current_screen = SCREEN_HOME;
            }
            else
            {
                /* AGAIN → restart the same game */
                if (current_screen == SCREEN_FLAPPY_OVER)
                {
                    fb_start();
                    current_screen = SCREEN_FLAPPY_GAME;
                }
                else if (current_screen == SCREEN_SNAKE_OVER)
                {
                    sn_start();
                    current_screen = SCREEN_SNAKE_GAME;
                }
                else if (current_screen == SCREEN_TETRIS_OVER)
                {
                    tt_start();
                    current_screen = SCREEN_TETRIS_GAME;
                }
            }
            over_cursor = 0; /* reset for next time */
            moved = true;
        }
        /* redraw over screen whenever cursor moves */
        if (moved && (current_screen == SCREEN_FLAPPY_OVER ||
                      current_screen == SCREEN_SNAKE_OVER ||
                      current_screen == SCREEN_TETRIS_OVER))
        {
            /* re-draw with updated cursor highlight */
            /* score is already on screen — redraw the button area only by
             * calling draw_game_over with stored score; we re-use the score
             * saved in each game's struct */
            int s = (current_screen == SCREEN_FLAPPY_OVER) ? fb.score : (current_screen == SCREEN_SNAKE_OVER) ? sn.score
                                                                                                              : tt.score;
            draw_game_over(s);
            last_input_tick = now;
            redraw_needed = false;
            return;
        }
    }

    /* ── FLAPPY BIRD ── */
    else if (current_screen == SCREEN_FLAPPY_GAME)
    {
        if (btn && gdeb)
        {
            fb.bird_vy = FB_FLAP;
            moved = true;
        }
        fb_tick();
        if (!fb.alive)
        {
            over_cursor = 0;
            current_screen = SCREEN_FLAPPY_OVER;
            draw_game_over(fb.score);
        }
        redraw_needed = true;
        if (moved)
            last_input_tick = now;
        return;
    }

    /* ── SNAKE ── */
    else if (current_screen == SCREEN_SNAKE_GAME)
    {
        if (gdeb)
        {
            if (up && sn.dir.y == 0)
            {
                sn.dir = (sn_pt_t){0, -1};
                moved = true;
            }
            if (down && sn.dir.y == 0)
            {
                sn.dir = (sn_pt_t){0, 1};
                moved = true;
            }
            if (left && sn.dir.x == 0)
            {
                sn.dir = (sn_pt_t){-1, 0};
                moved = true;
            }
            if (right && sn.dir.x == 0)
            {
                sn.dir = (sn_pt_t){1, 0};
                moved = true;
            }
        }
        sn_tick();
        if (!sn.alive)
        {
            over_cursor = 0;
            current_screen = SCREEN_SNAKE_OVER;
            draw_game_over(sn.score);
        }
        redraw_needed = true;
        if (moved)
            last_input_tick = now;
        return;
    }

    /* ── TETRIS ── */
    else if (current_screen == SCREEN_TETRIS_GAME)
    {
        if (gdeb)
        {
            if (left && tt_valid(tt.px - 1, tt.py, tt.piece))
            {
                tt.px--;
                moved = true;
            }
            if (right && tt_valid(tt.px + 1, tt.py, tt.piece))
            {
                tt.px++;
                moved = true;
            }
            if (down && tt_valid(tt.px, tt.py + 1, tt.piece))
            {
                tt.py++;
                moved = true;
            }
            if (btn)
            {
                tt_rotate();
                moved = true;
            }
        }
        TickType_t now2 = xTaskGetTickCount();
        if ((now2 - tt.last_grav) >= pdMS_TO_TICKS(TT_TICK_MS))
        {
            tt_gravity();
            tt.last_grav = now2;
        }
        if (!tt.alive)
        {
            over_cursor = 0;
            current_screen = SCREEN_TETRIS_OVER;
            draw_game_over(tt.score);
        }
        redraw_needed = true;
        if (moved)
            last_input_tick = now;
        return;
    }

    if (moved)
    {
        last_input_tick = now;
        redraw_needed = true;
    }
}

//  APP MAIN
void app_main(void)
{
    ESP_LOGI(TAG, "Game Console booting");
    rng_state ^= (uint32_t)xTaskGetTickCount();

    oled_init();
    joystick_init();

    redraw_needed = true;
    current_screen = SCREEN_HOME;

    while (1)
    {
        read_input();

        if (redraw_needed)
        {
            switch (current_screen)
            {
            case SCREEN_HOME:
                draw_home();
                break;
            case SCREEN_FLAPPY_SPLASH:
                fb_draw_splash();
                break;
            case SCREEN_FLAPPY_GAME:
                fb_draw_game();
                break;
            case SCREEN_SNAKE_SPLASH:
                sn_draw_splash();
                break;
            case SCREEN_SNAKE_GAME:
                sn_draw_game();
                break;
            case SCREEN_TETRIS_SPLASH:
                tt_draw_splash();
                break;
            case SCREEN_TETRIS_GAME:
                tt_draw_game();
                break;
            /* OVER screens draw themselves on transition and on cursor move */
            default:
                break;
            }
            redraw_needed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_PERIOD_MS));
    }
}