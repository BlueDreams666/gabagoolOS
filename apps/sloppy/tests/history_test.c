/* GabagoolOS listening history tests: cc -fsanitize=address,undefined history_test.c */
#include <assert.h>
#include <stdio.h>
#include "../sloppy_history_state.h"

static struct sl_hist_tracker t;
static struct sl_play rec;
static int logged;
static uint32_t now = 1790000000u;

/* play hash for ms milliseconds of wall time from elapsed at, 250 ms ticks */
static uint32_t play(uint32_t hash, uint32_t at, uint32_t ms, uint32_t len, bool playing)
{
    for (uint32_t w = 0; w <= ms; w += 250, now++)
    {
        if (sl_hist_step(&t, hash, playing, at, len, now, &rec)) logged++;
        if (playing) at += 250;
    }
    return at;
}

int main(void)
{
    uint32_t a = sl_hist_hash("/Music/a.mp3"), b = sl_hist_hash("/Music/b.mp3");
    assert(a && b && a != b && sl_hist_hash("") != 0);
    /* 40 s of a long song counts once when the next song starts */
    play(a, 0, 40000, 240000, true);
    assert(!logged);
    play(b, 0, 1000, 240000, true);
    assert(logged == 1 && rec.path_hash == a && rec.ms >= 39000 && rec.ms <= 40500);
    /* a skip after 10 s does not count */
    logged = 0;
    play(b, 1000, 9000, 240000, true);
    play(a, 0, 500, 240000, true);
    assert(!logged);
    /* half of a short song counts */
    play(a, 500, 6000, 12000, true);
    play(0, 0, 0, 0, false);
    assert(logged == 1 && rec.path_hash == a);
    /* pause time and seeks add nothing */
    logged = 0;
    uint32_t at = play(b, 0, 10000, 300000, true);
    play(b, at, 60000, 300000, false);
    at = play(b, at + 90000, 5000, 300000, true);
    play(0, 0, 0, 0, false);
    assert(!logged);
    /* repeat one: the same song starting over is a second play */
    at = play(a, 0, 35000, 36000, true);
    play(a, 0, 35000, 36000, true);
    play(0, 0, 0, 0, false);
    assert(logged == 2);
    /* dates: 2026-09-25 14:30 */
    int y, m, d, h;
    sl_hist_date(1790346600u, &y, &m, &d, &h);
    assert(y == 2026 && m == 9 && d == 25 && h == 14);
    sl_hist_date(0, &y, &m, &d, &h);
    assert(y == 1970 && m == 1 && d == 1 && h == 0);
    sl_hist_date(951782400u, &y, &m, &d, NULL);        /* 2000-02-29 */
    assert(y == 2000 && m == 2 && d == 29);
    sl_hist_date(1704067199u, &y, &m, &d, &h);         /* 2023-12-31 23:59:59 */
    assert(y == 2023 && m == 12 && d == 31 && h == 23);
    printf("PASS: play counting, skips, pauses, seeks, repeats, calendar dates\n");
    return 0;
}
