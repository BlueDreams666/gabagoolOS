#include <assert.h>
#include <stdio.h>
#include "../sloppy_snake_state.h"

int main(void)
{
    struct sl_snake_state s;
    sl_snake_init(&s, true, 42);
    assert(s.len == 3 && s.alive && !sl_snake_occupied(&s, s.food_x, s.food_y, s.len));
    sl_snake_turn(&s, -1, 0);
    assert(s.next_dx == 1); /* reverse steering is ignored */
    for (int i = 0; i < 7; i++) assert(sl_snake_step(&s) != SL_SNAKE_DEAD);
    assert(sl_snake_step(&s) == SL_SNAKE_DEAD && !s.alive);

    sl_snake_init(&s, false, 42);
    for (int i = 0; i < 7; i++) assert(sl_snake_step(&s) != SL_SNAKE_DEAD);
    assert(sl_snake_step(&s) == SL_SNAKE_DEAD); /* Classic has solid walls too */

    sl_snake_init(&s, true, 42);
    s.food_x = 9; s.food_y = 8;
    assert(sl_snake_step(&s) == SL_SNAKE_ATE && s.len == 4 && s.score == 1);
    assert(!sl_snake_occupied(&s, s.food_x, s.food_y, s.len));
    s.x[1] = 10; s.y[1] = 8;
    assert(sl_snake_step(&s) == SL_SNAKE_DEAD); /* collide with body */

    sl_snake_init(&s, false, 42);
    s.len = 4;
    s.x[0] = 5; s.y[0] = 5;
    s.x[1] = 5; s.y[1] = 6;
    s.x[2] = 6; s.y[2] = 6;
    s.x[3] = 6; s.y[3] = 5;
    s.food_x = 0; s.food_y = 0;
    assert(sl_snake_step(&s) == SL_SNAKE_MOVED && s.alive); /* vacating tail is safe */

    sl_snake_init(&s, true, 42);
    s.score = 9;
    s.food_x = 9; s.food_y = 8;
    assert(sl_snake_step(&s) == SL_SNAKE_ATE);
    assert(s.score == 10 && s.level == 2 && s.obstacle_count == 4);
    assert(sl_snake_period(&s) < sl_snake_period(&(struct sl_snake_state){.levels = true, .level = 1}));
    for (int i = 0; i < s.obstacle_count; i++)
    {
        assert(!sl_snake_occupied(&s, s.obstacle_x[i], s.obstacle_y[i], s.len));
        assert(s.obstacle_x[i] != s.food_x || s.obstacle_y[i] != s.food_y);
    }
    assert(!sl_snake_obstacle(&s, s.food_x, s.food_y));
    assert(!sl_snake_occupied(&s, s.food_x, s.food_y, s.len));
    s.obstacle_x[0] = 10; s.obstacle_y[0] = 8;
    assert(sl_snake_step(&s) == SL_SNAKE_DEAD); /* collide with obstacle */

    sl_snake_init(&s, true, 42);
    s.x[0] = 5; s.y[0] = 5;
    s.x[1] = 5; s.y[1] = 6;
    s.x[2] = 6; s.y[2] = 6;
    s.dx = s.next_dx = 1;
    s.food_x = 0; s.food_y = 0;
    s.obstacle_count = 1; s.obstacle_x[0] = 0; s.obstacle_y[0] = 0;
    sl_snake_food(&s);
    assert(s.food_x != 0 || s.food_y != 0);

    sl_snake_init(&s, false, 42);
    s.score = 9;
    s.food_x = 9; s.food_y = 8;
    assert(sl_snake_step(&s) == SL_SNAKE_ATE);
    assert(s.score == 10 && s.level == 1 && s.obstacle_count == 0);
    assert(sl_snake_period(&s) == 170);
    s.len = SL_SNAKE_MAX;
    s.food_x = 10; s.food_y = 8;
    assert(sl_snake_step(&s) == SL_SNAKE_ATE);
    assert(s.score == 11 && s.alive && !s.won && s.len == SL_SNAKE_MAX);

    sl_snake_init(&s, false, 42);
    sl_snake_turn(&s, 0, -1);
    sl_snake_turn(&s, -1, 0); /* rapid input queues the second corner */
    assert(s.has_queued_turn && s.next_dy == -1);
    assert(sl_snake_step(&s) == SL_SNAKE_MOVED && s.dx == 0 && s.dy == -1);
    assert(sl_snake_step(&s) == SL_SNAKE_MOVED && s.dx == -1 && s.dy == 0);

    sl_snake_init(&s, true, 42);
    for (int i = 0; i < s.len; i++) { s.x[i] = 4 - i; s.y[i] = 8; }
    s.obstacle_count = SL_SNAKE_GRID;
    for (int y = 0; y < SL_SNAKE_GRID; y++)
    { s.obstacle_x[y] = 8; s.obstacle_y[y] = y; }
    sl_snake_food(&s);
    assert(s.food_x >= 0 && s.food_x < 8); /* unreachable pocket is excluded */
    puts("PASS: Snake solid walls, endless Classic scoring, Levels progression, food, obstacles, collisions");
    return 0;
}
