/* GabagoolOS Skip Graveyard tests: cc -fsanitize=address,undefined grave_test.c */
#include <assert.h>
#include <stdio.h>
#include "../sloppy_grave_state.h"

static struct sl_hist_tracker t;
static struct sl_graves g;
static uint32_t now = 1790000000u;
static int skips;

/* play hash for ms of wall time from elapsed at, in 250 ms ticks, the way
 * hist_poll does: note a skip before the step that forgets the old song */
static uint32_t play(uint32_t hash, uint32_t at, uint32_t ms, uint32_t len)
{
    struct sl_play rec;
    for (uint32_t w = 0; w <= ms; w += 250, now++)
    {
        if (sl_hist_is_skip(&t, hash)) { sl_grave_skip(&g, t.hash); skips++; }
        if (sl_hist_step(&t, hash, hash != 0, at, len, now, &rec)) sl_grave_played(&g, rec.path_hash);
        if (hash) at += 250;
    }
    return at;
}

int main(void)
{
    uint32_t a = sl_hist_hash("/Music/a.mp3"), b = sl_hist_hash("/Music/b.mp3"),
             c = sl_hist_hash("/Music/c.mp3");
    sl_graves_init(&g);
    assert(sl_graves_valid(&g) && g.n == 0);

    /* 10 s of a then on to b: one skip of a */
    play(a, 0, 10000, 240000);
    play(b, 0, 500, 240000);
    assert(skips == 1 && g.n == 1 && g.g[0].hash == a && g.g[0].skips == 1);
    /* half a second of b is not hearing it: flicking past is no skip */
    play(c, 0, 250, 240000);
    assert(skips == 1);
    /* stopping is not a skip, nor is starting the same song over */
    play(c, 250, 5000, 240000);
    play(0, 0, 0, 0);
    play(c, 0, 5000, 240000);
    play(c, 0, 500, 240000);
    assert(skips == 1);
    /* the end of a song (a seek near its end, then the next one) is no skip */
    play(c, 236000, 3000, 240000);
    play(a, 0, 500, 240000);
    assert(skips == 1);

    /* three skips bury it */
    for (int i = 0; i < 2; i++) { play(a, 0, 3000, 240000); play(b, 0, 300, 240000); }
    int i = sl_grave_find(&g, a);
    assert(skips == 3 && i >= 0 && g.g[i].skips == 3 && sl_grave_buried(&g.g[i]));
    /* a real play halves it and digs it back out */
    play(a, 0, 40000, 240000);
    play(b, 0, 500, 240000);
    i = sl_grave_find(&g, a);
    assert(i >= 0 && g.g[i].skips == 1 && !sl_grave_buried(&g.g[i]));
    play(a, 0, 40000, 240000);
    play(b, 0, 500, 240000);
    assert(sl_grave_find(&g, a) < 0);

    /* haunting keeps it buried through a play; resurrecting forgets it */
    sl_grave_skip(&g, c);
    sl_grave_haunt(&g, c, true);
    i = sl_grave_find(&g, c);
    assert(sl_grave_haunted(&g, c) && g.g[i].skips == SL_GRAVE_BURY && sl_grave_buried(&g.g[i]));
    for (int k = 0; k < 4; k++) sl_grave_played(&g, c);
    assert(sl_grave_haunted(&g, c) && sl_grave_buried(&g.g[sl_grave_find(&g, c)]));
    assert(sl_grave_resurrect(&g, c) && sl_grave_find(&g, c) < 0 && !sl_grave_haunted(&g, c));
    assert(!sl_grave_resurrect(&g, c) && !sl_grave_haunted(&g, 0));

    /* a full table evicts the least skipped, never a haunted song */
    sl_graves_init(&g);
    for (uint32_t h = 1; h <= SL_GRAVE_MAX; h++) { sl_grave_skip(&g, h); sl_grave_skip(&g, h); }
    sl_grave_haunt(&g, 7, true);
    g.g[sl_grave_find(&g, 7)].skips = 0;       /* least skipped, but haunted */
    g.checksum = sl_grave_checksum(&g);
    g.g[sl_grave_find(&g, 9)].skips = 1;
    g.checksum = sl_grave_checksum(&g);
    sl_grave_skip(&g, 999);
    assert(g.n == SL_GRAVE_MAX && sl_grave_find(&g, 9) < 0 && sl_grave_find(&g, 7) >= 0 &&
           sl_grave_find(&g, 999) >= 0 && sl_graves_valid(&g));
    /* all haunted: a new skip has nowhere to go */
    for (int k = 0; k < g.n; k++) g.g[k].haunted = 1;
    g.checksum = sl_grave_checksum(&g);
    sl_grave_skip(&g, 12345);
    assert(sl_grave_find(&g, 12345) < 0 && g.n == SL_GRAVE_MAX);
    /* skips saturate */
    sl_graves_init(&g);
    for (int k = 0; k < 70000; k++) sl_grave_skip(&g, a);
    assert(g.g[0].skips == 0xffff);

    /* loading: damage is caught */
    assert(sl_graves_valid(&g));
    g.n = SL_GRAVE_MAX + 1;
    assert(!sl_graves_valid(&g));
    sl_graves_init(&g);
    sl_grave_skip(&g, a);
    g.g[0].skips = 9;                          /* no checksum update */
    assert(!sl_graves_valid(&g));
    g.checksum = sl_grave_checksum(&g);
    g.g[0].haunted = 2;
    g.checksum = sl_grave_checksum(&g);
    assert(!sl_graves_valid(&g));
    puts("PASS: skips, flicks, stops, repeats, song ends, burial, plays, haunting, resurrection, eviction, loading");
    return 0;
}
