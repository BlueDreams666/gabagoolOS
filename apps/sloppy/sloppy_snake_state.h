/* Deterministic Snake rules for GabagoolOS. GPLv2+. */
#ifndef SLOPPY_SNAKE_STATE_H
#define SLOPPY_SNAKE_STATE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define SL_SNAKE_GRID 16
#define SL_SNAKE_MAX 128
#define SL_SNAKE_OBSTACLE_MAX 48
#define SL_SNAKE_FOOD_PER_LEVEL 10
#define SL_SNAKE_QUEUE 3                    /* turns kept waiting: a fast zig-zag is never lost */
#define SL_SNAKE_GOLD_EVERY 5               /* every fifth apple is golden */
#define SL_SNAKE_GOLD_POINTS 3
enum sl_snake_result { SL_SNAKE_MOVED, SL_SNAKE_ATE, SL_SNAKE_DEAD, SL_SNAKE_WON };
enum { SL_SNAKE_NORMAL, SL_SNAKE_RELAXED, SL_SNAKE_FAST };   /* Normal is 0: a state made empty plays at the usual pace */
struct sl_snake_state {
    int8_t x[SL_SNAKE_MAX], y[SL_SNAKE_MAX];
    int8_t obstacle_x[SL_SNAKE_OBSTACLE_MAX], obstacle_y[SL_SNAKE_OBSTACLE_MAX];
    int8_t dx, dy, next_dx, next_dy, queued_dx, queued_dy, food_x, food_y;
    int8_t queue_dx[SL_SNAKE_QUEUE], queue_dy[SL_SNAKE_QUEUE];   /* queued_dx/dy is the first of these */
    int len, score, level, obstacle_count, queued, eaten, speed;
    uint32_t rng;
    bool levels, alive, won, has_queued_turn, food_gold;
};

static uint32_t sl_snake_rand(struct sl_snake_state *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return s->rng;
}
static bool sl_snake_occupied(const struct sl_snake_state *s, int x, int y, int count)
{
    for (int i = 0; i < count; i++) if (s->x[i] == x && s->y[i] == y) return true;
    return false;
}
static bool sl_snake_obstacle(const struct sl_snake_state *s, int x, int y)
{
    for (int i = 0; i < s->obstacle_count; i++)
        if (s->obstacle_x[i] == x && s->obstacle_y[i] == y) return true;
    return false;
}
/* Obstacles can split the arena. Food must stay in the head's connected area. */
static void sl_snake_reachable(const struct sl_snake_state *s, bool cells[SL_SNAKE_GRID * SL_SNAKE_GRID])
{
    int queue[SL_SNAKE_GRID * SL_SNAKE_GRID], front = 0, back = 0;
    int head = s->y[0] * SL_SNAKE_GRID + s->x[0];
    cells[head] = true;
    queue[back++] = head;
    while (front < back)
    {
        int cell = queue[front++], x = cell % SL_SNAKE_GRID, y = cell / SL_SNAKE_GRID;
        static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
        for (int dir = 0; dir < 4; dir++)
        {
            int nx = x + dx[dir], ny = y + dy[dir];
            if (nx < 0 || ny < 0 || nx >= SL_SNAKE_GRID || ny >= SL_SNAKE_GRID) continue;
            int next = ny * SL_SNAKE_GRID + nx;
            if (cells[next] || sl_snake_obstacle(s, nx, ny)) continue;
            cells[next] = true;
            queue[back++] = next;
        }
    }
}
static int sl_snake_period(const struct sl_snake_state *s)
{
    /* Relaxed a third slower, Fast about a quarter quicker, both all the way up */
    static const int pct[3] = { 100, 135, 72 };
    int k = pct[s->speed >= 0 && s->speed <= 2 ? s->speed : 1];
    if (!s->levels)
    {
        /* Endless play gains tempo gradually without stage transitions. */
        int ms = 190 - s->score * 2;
        return (ms < 100 ? 100 : ms) * k / 100;
    }
    int ms = 190 - (s->level - 1) * 15;
    return (ms < 70 ? 70 : ms) * k / 100;
}
static void sl_snake_add_obstacles(struct sl_snake_state *s)
{
    /* Two short walls per stage read like a maze, rather than loose pixels. */
    for (int added = 0; added < 2 && s->obstacle_count + 1 < SL_SNAKE_OBSTACLE_MAX; added++)
    {
        int start = sl_snake_rand(s) % (SL_SNAKE_GRID * SL_SNAKE_GRID);
        int step_x = s->level % 2 ? 1 : 0, step_y = step_x ? 0 : 1;
        bool placed = false;
        for (int i = 0; i < SL_SNAKE_GRID * SL_SNAKE_GRID; i++)
        {
            int cell = (start + i) % (SL_SNAKE_GRID * SL_SNAKE_GRID);
            int x = cell % SL_SNAKE_GRID, y = cell / SL_SNAKE_GRID;
            if (x + step_x >= SL_SNAKE_GRID || y + step_y >= SL_SNAKE_GRID) continue;
            bool clear = true;
            for (int part = 0; part < 2; part++)
            {
                int px = x + part * step_x, py = y + part * step_y;
                if (sl_snake_occupied(s, px, py, s->len) ||
                    sl_snake_obstacle(s, px, py) ||
                    (px == s->food_x && py == s->food_y) ||
                    (abs(px - s->x[0]) + abs(py - s->y[0]) <= 2))
                { clear = false; break; }
            }
            if (!clear) continue;
            for (int part = 0; part < 2; part++)
            {
                s->obstacle_x[s->obstacle_count] = x + part * step_x;
                s->obstacle_y[s->obstacle_count++] = y + part * step_y;
            }
            placed = true;
            break;
        }
        if (!placed) break;
    }
}
static void sl_snake_food(struct sl_snake_state *s)
{
    bool reachable[SL_SNAKE_GRID * SL_SNAKE_GRID] = { false };
    if (s->levels) sl_snake_reachable(s, reachable);
    int start = sl_snake_rand(s) % (SL_SNAKE_GRID * SL_SNAKE_GRID);
    for (int i = 0; i < SL_SNAKE_GRID * SL_SNAKE_GRID; i++)
    {
        int cell = (start + i) % (SL_SNAKE_GRID * SL_SNAKE_GRID);
        int x = cell % SL_SNAKE_GRID, y = cell / SL_SNAKE_GRID;
        if ((!s->levels || reachable[cell]) &&
            !sl_snake_occupied(s, x, y, s->len) && !sl_snake_obstacle(s, x, y))
        {
            s->food_x = x; s->food_y = y;
            s->food_gold = s->eaten % SL_SNAKE_GOLD_EVERY == SL_SNAKE_GOLD_EVERY - 1;
            return;
        }
    }
    s->food_x = s->food_y = -1;
    s->won = true; s->alive = false;
}
static void sl_snake_init_speed(struct sl_snake_state *s, bool levels, uint32_t seed, int speed)
{
    *s = (struct sl_snake_state){0};
    s->speed = speed;
    s->levels = levels;
    s->rng = seed ? seed : 1;
    s->alive = true;
    s->level = 1;
    s->len = 3;
    s->dx = s->next_dx = 1;
    s->x[0] = 8; s->y[0] = 8;
    s->x[1] = 7; s->y[1] = 8;
    s->x[2] = 6; s->y[2] = 8;
    sl_snake_food(s);
}
static void sl_snake_init(struct sl_snake_state *s, bool levels, uint32_t seed)
{
    sl_snake_init_speed(s, levels, seed, SL_SNAKE_NORMAL);
}
static void sl_snake_queue_sync(struct sl_snake_state *s)
{
    s->has_queued_turn = s->queued > 0;
    s->queued_dx = s->queued ? s->queue_dx[0] : 0;
    s->queued_dy = s->queued ? s->queue_dy[0] : 0;
}
/* A turn is checked against the way the snake will be going once the
 * turns already waiting are made: the same way again, or straight back
 * onto itself, is no turn. Up to SL_SNAKE_QUEUE wait their steps. */
static void sl_snake_turn(struct sl_snake_state *s, int dx, int dy)
{
    if (!s->alive || (dx == 0) == (dy == 0)) return;
    bool pending = s->next_dx != s->dx || s->next_dy != s->dy || s->queued;
    int ldx = s->queued ? s->queue_dx[s->queued - 1] : s->next_dx;
    int ldy = s->queued ? s->queue_dy[s->queued - 1] : s->next_dy;
    if (!pending)
    {
        if (dx != -s->dx || dy != -s->dy) { s->next_dx = dx; s->next_dy = dy; }
        return;
    }
    if ((dx == ldx && dy == ldy) || (dx == -ldx && dy == -ldy) || s->queued >= SL_SNAKE_QUEUE) return;
    s->queue_dx[s->queued] = (int8_t)dx; s->queue_dy[s->queued] = (int8_t)dy;
    s->queued++;
    sl_snake_queue_sync(s);
}
static enum sl_snake_result sl_snake_step(struct sl_snake_state *s)
{
    if (!s->alive) return s->won ? SL_SNAKE_WON : SL_SNAKE_DEAD;
    s->dx = s->next_dx; s->dy = s->next_dy;
    if (s->queued)
    {
        s->next_dx = s->queue_dx[0]; s->next_dy = s->queue_dy[0];
        for (int i = 1; i < s->queued; i++) { s->queue_dx[i - 1] = s->queue_dx[i]; s->queue_dy[i - 1] = s->queue_dy[i]; }
        s->queued--;
        sl_snake_queue_sync(s);
    }
    else { s->next_dx = s->dx; s->next_dy = s->dy; }
    int x = s->x[0] + s->dx, y = s->y[0] + s->dy;
    if (x < 0 || y < 0 || x >= SL_SNAKE_GRID || y >= SL_SNAKE_GRID)
    { s->alive = false; return SL_SNAKE_DEAD; }
    bool eat = x == s->food_x && y == s->food_y;
    int occupied = s->len - (eat ? 0 : 1); /* stepping into the moving tail is legal */
    if (sl_snake_obstacle(s, x, y) || sl_snake_occupied(s, x, y, occupied))
    { s->alive = false; return SL_SNAKE_DEAD; }
    int new_len = s->len + (eat && s->len < SL_SNAKE_MAX);
    for (int i = new_len - 1; i > 0; i--)
    { s->x[i] = s->x[i-1]; s->y[i] = s->y[i-1]; }
    s->x[0] = x; s->y[0] = y; s->len = new_len;
    if (eat)
    {
        int gained = s->food_gold ? SL_SNAKE_GOLD_POINTS : 1;
        s->score += gained;
        s->eaten++;
        if (s->levels && s->len >= SL_SNAKE_MAX)
        { s->alive = false; s->won = true; return SL_SNAKE_WON; }
        /* a level each ten points: a golden apple can carry it over one */
        if (s->levels && s->score / SL_SNAKE_FOOD_PER_LEVEL > (s->score - gained) / SL_SNAKE_FOOD_PER_LEVEL)
        {
            s->level++;
            sl_snake_add_obstacles(s);
        }
        sl_snake_food(s);
        return s->won ? SL_SNAKE_WON : SL_SNAKE_ATE;
    }
    return SL_SNAKE_MOVED;
}
#endif
