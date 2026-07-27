/* snake.c -- Snake, played directly on the VGA text buffer
 *
 * Runs synchronously in the shell's own task (task 0). Movement is
 * paced by the PIT timer rather than tied to raw loop speed, and
 * input is polled non-blockingly during the wait between steps so
 * controls stay responsive regardless of step rate.
 */

#include "snake.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"

#define FIELD_TOP    2
#define FIELD_BOTTOM 22
#define FIELD_LEFT   1
#define FIELD_RIGHT  78

#define MAX_SNAKE_LEN 400
#define TICKS_PER_STEP 12 /* ~120ms per move at 100Hz */

typedef struct { int row, col; } point_t;

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

static unsigned int rng_state;

static void rng_seed(unsigned int seed) {
    rng_state = seed ? seed : 12345;
}

static unsigned int rng_next(void) {
    /* simple LCG -- fine for placing food, not for anything security-sensitive */
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFF;
}

static void draw_border(void) {
    for (int c = FIELD_LEFT - 1; c <= FIELD_RIGHT + 1; c++) {
        vga_put_at(FIELD_TOP - 1, c, '#', VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_put_at(FIELD_BOTTOM + 1, c, '#', VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    }
    for (int r = FIELD_TOP - 1; r <= FIELD_BOTTOM + 1; r++) {
        vga_put_at(r, FIELD_LEFT - 1, '#', VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_put_at(r, FIELD_RIGHT + 1, '#', VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    }
}

static void draw_score(unsigned int score, int alive) {
    for (int c = 0; c < 80; c++) vga_put_at(0, c, ' ', VGA_COLOR(VGA_WHITE, VGA_BLACK));
    const char *label = alive ? "SNAKE -- score: " : "GAME OVER -- final score: ";
    int col = 0;
    while (*label) vga_put_at(0, col++, *label++, VGA_COLOR(VGA_YELLOW, VGA_BLACK));

    char buf[11]; int i = 10; buf[10] = '\0';
    unsigned int n = score;
    if (n == 0) { vga_put_at(0, col++, '0', VGA_COLOR(VGA_YELLOW, VGA_BLACK)); }
    else {
        while (n > 0 && i > 0) { buf[--i] = (char)('0' + (n % 10)); n /= 10; }
        while (buf[i]) vga_put_at(0, col++, buf[i++], VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    }

    if (!alive) {
        const char *msg = "  (press any key to return to shell)";
        while (*msg) vga_put_at(0, col++, *msg++, VGA_COLOR(VGA_WHITE, VGA_BLACK));
    }
}

static void place_food(point_t *food, const point_t *body, int len) {
    for (;;) {
        int r = FIELD_TOP + (int)(rng_next() % (FIELD_BOTTOM - FIELD_TOP + 1));
        int c = FIELD_LEFT + (int)(rng_next() % (FIELD_RIGHT - FIELD_LEFT + 1));
        int collides = 0;
        for (int i = 0; i < len; i++) if (body[i].row == r && body[i].col == c) { collides = 1; break; }
        if (!collides) { food->row = r; food->col = c; return; }
    }
}

void snake_run(void) {
    while (keyboard_has_char()) keyboard_getchar(); /* drain any stale input */

    point_t body[MAX_SNAKE_LEN];
    int len = 3;
    int mid_row = (FIELD_TOP + FIELD_BOTTOM) / 2;
    int mid_col = (FIELD_LEFT + FIELD_RIGHT) / 2;
    body[0] = (point_t){mid_row, mid_col};
    body[1] = (point_t){mid_row, mid_col - 1};
    body[2] = (point_t){mid_row, mid_col - 2};

    int direction = DIR_RIGHT;
    unsigned int score = 0;
    int alive = 1;

    rng_seed(timer_ticks() * 2654435761u + 1);

    vga_clear();
    draw_border();
    for (int i = 0; i < len; i++)
        vga_put_at(body[i].row, body[i].col, i == 0 ? '@' : 'o', VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));

    point_t food;
    place_food(&food, body, len);
    vga_put_at(food.row, food.col, '*', VGA_COLOR(VGA_YELLOW, VGA_BLACK));

    draw_score(score, 1);
    vga_set_cursor_pos(24, 0);
    vga_write("Controls: w a s d to move, q to quit.");

    while (alive) {
        unsigned int target = timer_ticks() + TICKS_PER_STEP;
        int requested_dir = direction;
        int quit = 0;

        while (timer_ticks() < target) {
            if (keyboard_has_char()) {
                char c = keyboard_getchar();
                if (c == 'w' && direction != DIR_DOWN)  requested_dir = DIR_UP;
                else if (c == 's' && direction != DIR_UP)    requested_dir = DIR_DOWN;
                else if (c == 'a' && direction != DIR_RIGHT) requested_dir = DIR_LEFT;
                else if (c == 'd' && direction != DIR_LEFT)  requested_dir = DIR_RIGHT;
                else if (c == 'q') { quit = 1; }
            } else {
                __asm__ volatile ("hlt");
            }
        }
        if (quit) break;
        direction = requested_dir;

        point_t new_head = body[0];
        if (direction == DIR_UP) new_head.row--;
        else if (direction == DIR_DOWN) new_head.row++;
        else if (direction == DIR_LEFT) new_head.col--;
        else new_head.col++;

        if (new_head.row < FIELD_TOP || new_head.row > FIELD_BOTTOM ||
            new_head.col < FIELD_LEFT || new_head.col > FIELD_RIGHT) {
            alive = 0;
            break;
        }
        for (int i = 0; i < len; i++) {
            if (body[i].row == new_head.row && body[i].col == new_head.col) { alive = 0; break; }
        }
        if (!alive) break;

        int ate = (new_head.row == food.row && new_head.col == food.col);

        if (ate && len < MAX_SNAKE_LEN) {
            for (int i = len; i > 0; i--) body[i] = body[i - 1];
            body[0] = new_head;
            len++;
            score += 10;
            place_food(&food, body, len);
            vga_put_at(food.row, food.col, '*', VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        } else {
            point_t old_tail = body[len - 1];
            for (int i = len - 1; i > 0; i--) body[i] = body[i - 1];
            body[0] = new_head;
            vga_put_at(old_tail.row, old_tail.col, ' ', VGA_COLOR(VGA_BLACK, VGA_BLACK));
        }

        vga_put_at(body[0].row, body[0].col, '@', VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        if (len > 1) vga_put_at(body[1].row, body[1].col, 'o', VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));

        draw_score(score, 1);
    }

    draw_score(score, 0);
    vga_set_cursor_pos(24, 0);
    while (keyboard_has_char()) keyboard_getchar();
    keyboard_getchar(); /* wait for a keypress before returning to the shell */

    vga_clear();
}
