/* The explicit queue against a copy of Rockbox's playlist index bookkeeping
 * (apps/playlist.c: add_track_to_playlist_unlocked, remove_track_unlocked,
 * playlist_move, get_next_index, playlist_next, randomise, sort). Only the
 * index arithmetic is copied; file names are "t<n>" and "q<n>". */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH 260
#define PLAYLIST_ATTR_QUEUED 0x01
enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE, REPEAT_SHUFFLE };
enum { PLAYLIST_PREPEND = -1, PLAYLIST_INSERT = -2, PLAYLIST_INSERT_LAST = -3,
       PLAYLIST_INSERT_FIRST = -4, PLAYLIST_INSERT_LAST_ROTATED = -8 };
struct playlist_info { int index, first_index, amount, last_insert_pos; bool started; };
struct playlist_track_info { char filename[MAX_PATH]; int attr, index, display_index; };
static struct { int repeat_mode; } global_settings;

#define CAP 400
static struct { int id, seq; bool queued; } ent[CAP];
static struct playlist_info pl;
static int seq_counter;

static int rotate_index(int index) { index -= pl.first_index; return index < 0 ? index + pl.amount : index; }
static int name_id(const char *f) { return (f[0] == 'q' ? 1000 : 0) + atoi(f + 1); }

static int add_track(int id, int position, bool queue)
{
    int insert_position, orig_position = position;
    insert_position = position;
    switch (position)
    {
        case PLAYLIST_PREPEND: position = insert_position = pl.first_index; break;
        case PLAYLIST_INSERT:
            if (pl.last_insert_pos >= 0 && pl.last_insert_pos < pl.amount)
                position = insert_position = pl.last_insert_pos + 1;
            else if (pl.amount > 0) position = insert_position = pl.index + 1;
            else position = insert_position = 0;
            pl.last_insert_pos = position;
            break;
        case PLAYLIST_INSERT_FIRST:
            position = insert_position = pl.amount > 0 ? pl.index + 1 : 0;
            pl.last_insert_pos = position;
            break;
        case PLAYLIST_INSERT_LAST:
            if (pl.first_index <= 0)
            {
                position = insert_position = pl.amount;
                pl.last_insert_pos = position;
                break;
            }
            /* fallthrough */
        case PLAYLIST_INSERT_LAST_ROTATED:
            position = insert_position = pl.first_index;
            pl.last_insert_pos = position;
            break;
    }
    for (int i = pl.amount; i > insert_position; i--) ent[i] = ent[i - 1];
    if (orig_position < 0)
    {
        if (pl.amount > 0 && insert_position <= pl.index && pl.started &&
            orig_position != PLAYLIST_INSERT_LAST_ROTATED)
            pl.index++;
        if (pl.amount > 0 && insert_position <= pl.first_index &&
            orig_position != PLAYLIST_PREPEND && pl.started)
        {
            if (insert_position == pl.first_index) position = PLAYLIST_INSERT_LAST_ROTATED;
            pl.first_index++;
        }
    }
    else if (pl.amount > 0 && insert_position < pl.first_index && pl.started)
        pl.first_index++;
    if (insert_position < pl.last_insert_pos ||
        (insert_position == pl.last_insert_pos && position < 0 && position != PLAYLIST_INSERT_LAST_ROTATED))
        pl.last_insert_pos++;
    ent[insert_position].id = id;
    ent[insert_position].seq = seq_counter++;
    ent[insert_position].queued = queue;
    pl.amount++;
    return insert_position;
}
static int remove_track(int position)
{
    if (pl.amount <= 0) return -1;
    for (int i = position; i < pl.amount; i++) ent[i] = ent[i + 1];
    pl.amount--;
    if (position < pl.index) pl.index--;
    if (position < pl.first_index) pl.first_index--;
    if (position <= pl.last_insert_pos) pl.last_insert_pos--;
    return 0;
}

/* ---- the playlist API sloppy_queue.h uses */
static int moves;
struct playlist_info *playlist_get_current(void) { return &pl; }
int playlist_amount(void) { return pl.amount; }
int playlist_get_resume_info(int *i) { *i = pl.index; return 0; }
int playlist_get_first_index(const struct playlist_info *p) { (void)p; return pl.first_index; }
int playlist_get_track_attr(struct playlist_info *p, int i)
{ (void)p; return i < 0 || i >= pl.amount ? -1 : ent[i].queued ? PLAYLIST_ATTR_QUEUED : 0; }
int playlist_get_track_info(struct playlist_info *p, int i, struct playlist_track_info *info)
{
    (void)p;
    if (i < 0 || i >= pl.amount) return -1;
    snprintf(info->filename, sizeof info->filename, "%c%d", ent[i].id >= 1000 ? 'q' : 't', ent[i].id % 1000);
    info->attr = ent[i].queued ? PLAYLIST_ATTR_QUEUED : 0;
    info->index = i;
    info->display_index = rotate_index(i) + 1;
    return 0;
}
int playlist_insert_track(struct playlist_info *p, const char *f, int pos, bool queue, bool sync)
{ (void)p; (void)sync; return pl.amount >= CAP ? -1 : add_track(name_id(f), pos, queue); }
int playlist_delete(struct playlist_info *p, int i) { (void)p; return remove_track(i); }
void playlist_sync(struct playlist_info *p) { (void)p; }
int playlist_move(struct playlist_info *p, int index, int new_index)
{
    (void)p;
    bool current = false, displace_current = false;
    if (index == new_index) return -1;
    moves++;
    if (index == pl.index) current = true;
    else
    {
        int idx_cur = rotate_index(pl.index) + 1, idx_to = rotate_index(new_index) + 1,
            idx_from = rotate_index(index) + 1;
        if (((idx_from > idx_cur) && (idx_to <= idx_cur)) || ((idx_from < idx_cur) && (idx_to < idx_cur)))
            displace_current = true;
    }
    int id = ent[index].id;
    bool queue = ent[index].queued;
    int r = rotate_index(new_index);
    remove_track(index);
    if (r == 0) new_index = PLAYLIST_PREPEND;
    else if (r == pl.amount) new_index = PLAYLIST_INSERT_LAST;
    else
    {
        new_index = (r + pl.first_index) % pl.amount;
        if ((new_index < pl.first_index) && (new_index <= pl.index)) displace_current = true;
        else if ((new_index >= pl.first_index) && (pl.index < pl.first_index)) displace_current = false;
    }
    add_track(id, new_index, queue);
    if (current)
    {
        if (new_index == PLAYLIST_PREPEND) pl.index = pl.first_index;
        else if (new_index == PLAYLIST_INSERT_LAST)
        { pl.index = pl.first_index - 1; if (pl.index < 0) pl.index += pl.amount; }
        else pl.index = new_index;
    }
    else if (displace_current && new_index != PLAYLIST_PREPEND)
        pl.index++;
    return 0;
}
#define REPEAT_ALL_ REPEAT_ALL
static int get_next_index(int steps)
{
    if (pl.amount <= 0) return -1;
    if (global_settings.repeat_mode == REPEAT_ALL)
    {
        int n = (pl.index + steps) % pl.amount;
        while (n < 0) n += pl.amount;
        return n;
    }
    int n = rotate_index(pl.index) + steps;
    return n < 0 || n >= pl.amount ? -1 : (n + pl.first_index) % pl.amount;
}
/* playlist_next(): what a skip or the end of a song does */
static int playlist_next(int steps)
{
    if (steps > 0)
        for (int i = 0, j = steps; i < j; i++)
        {
            int index = get_next_index(i);
            if (index >= 0 && ent[index].queued) { remove_track(index); steps--; }
        }
    int index = get_next_index(steps);
    if (index < 0) return -1;
    pl.index = index;
    if (pl.last_insert_pos >= 0 && steps > 0 && rotate_index(index) > rotate_index(pl.last_insert_pos))
        pl.last_insert_pos = -1;
    return index;
}
static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static void randomise(void)
{
    int cur_seq = ent[pl.index].seq;
    for (int c = pl.amount - 1; c >= 0; c--)
    { int k = rnd() % (c + 1); __typeof__(ent[0]) t = ent[k]; ent[k] = ent[c]; ent[c] = t; }
    for (int i = 0; i < pl.amount; i++) if (ent[i].seq == cur_seq) pl.index = pl.first_index = i;
    pl.last_insert_pos = -1;
}
static void sort_list(void)
{
    int cur_seq = ent[pl.index].seq;
    for (int i = 1; i < pl.amount; i++)
        for (int j = i; j > 0 && ent[j - 1].seq > ent[j].seq; j--)
        { __typeof__(ent[0]) t = ent[j]; ent[j] = ent[j - 1]; ent[j - 1] = t; }
    for (int i = 0; i < pl.amount; i++) if (ent[i].seq == cur_seq) pl.index = i;
    pl.last_insert_pos = -1;
    pl.first_index = 0;
}

#include "../sloppy_queue.h"

/* ---- helpers */
static void new_list(int n, int start)
{
    memset(&pl, 0, sizeof pl);
    pl.last_insert_pos = -1;
    char f[16];
    for (int i = 0; i < n; i++) { snprintf(f, sizeof f, "t%d", i); playlist_insert_track(NULL, f, PLAYLIST_INSERT_LAST, false, false); }
    pl.index = start;
    pl.started = true;
}
static int cur_id(void) { return ent[pl.index].id; }
static void queue(int q, bool front) { char f[16]; snprintf(f, sizeof f, "q%d", q); assert(q_insert(f, front, true) >= 0); }
/* expected queue, in order */
static int want[CAP], nwant;
static void want_push(int q, bool front)
{
    if (front) { memmove(want + 1, want, nwant * sizeof *want); want[0] = 1000 + q; }
    else want[nwant] = 1000 + q;
    nwant++;
}
static void check_queue(void)
{
    int run = q_run(), total = 0;
    for (int i = 0; i < pl.amount; i++) if (i != pl.index && ent[i].queued) total++;
    if (run != total || run != nwant)
    { fprintf(stderr, "run %d, queued %d, expected %d\n", run, total, nwant); assert(0); }
    for (int k = 1; k <= run; k++) assert(ent[q_at(k)].id == want[k - 1]);
}
/* a skip or the end of a song, then GabagoolOS's track-change tidy */
static int next(void)
{
    if (playlist_next(1) < 0) return -1;
    q_tidy();
    if (nwant && cur_id() == want[0]) { memmove(want, want + 1, --nwant * sizeof *want); }
    return cur_id();
}

int main(void)
{
    /* 1. The reported bug: an album in order, then "Add to queue". Plain
     * PLAYLIST_INSERT (the old code) sends the song to the end. */
    new_list(10, 2);
    assert(playlist_insert_track(NULL, "q1", PLAYLIST_INSERT, true, true) == 10);
    new_list(10, 2);
    nwant = 0;
    queue(1, false); want_push(1, false);
    queue(2, false); want_push(2, false);
    queue(3, true);  want_push(3, true);    /* Play next goes in front */
    check_queue();
    assert(next() == 1003 && next() == 1001 && next() == 1002 && next() == 3);
    check_queue();

    /* 2. Queue while a queued song plays: it lines up behind the rest. */
    new_list(6, 0); nwant = 0;
    queue(1, false); want_push(1, false);
    queue(2, false); want_push(2, false);
    assert(next() == 1001);
    queue(4, false); want_push(4, false);
    check_queue();
    assert(next() == 1002 && next() == 1004 && next() == 1);

    /* 3. Skipping back leaves the queue next. */
    new_list(8, 3); nwant = 0;
    queue(1, false); want_push(1, false);
    queue(2, false); want_push(2, false);
    assert(playlist_next(-1) >= 0 && cur_id() == 2);
    q_tidy(); check_queue();
    assert(next() == 1001 && next() == 1002 && next() == 3 && next() == 4);

    /* 4. Shuffle on and off keep the queue, in order, straight after. */
    new_list(40, 5); nwant = 0;
    for (int q = 1; q <= 5; q++) { queue(q, false); want_push(q, false); }
    int playing = cur_id();
    q_keep(); q_drop(); randomise(); q_restore();
    assert(cur_id() == playing); check_queue();
    q_keep(); q_drop(); sort_list(); q_restore();
    assert(cur_id() == playing); check_queue();
    assert(next() == 1001);

    /* 5. A new list carries the queue over. */
    new_list(12, 4); nwant = 0;
    queue(7, false); want_push(7, false);
    queue(8, false); want_push(8, false);
    q_keep(); new_list(9, 6); q_restore();
    check_queue();
    assert(next() == 1007 && next() == 1008 && next() == 7);
    /* ... however long it is: 300 queued songs (128 were the most kept) */
    new_list(20, 3); nwant = 0;
    for (int q = 1; q <= 300; q++) { queue(q, false); want_push(q, false); }
    q_keep(); assert(q_kept_n == 300); new_list(9, 2); q_restore();
    check_queue();

    /* 6. Tapping a Next Up song plays it and keeps the queue next;
     * tapping a queued song drops the ones before it. */
    new_list(10, 0); nwant = 0;
    queue(1, false); want_push(1, false);
    queue(2, false); want_push(2, false);
    int steps = q_jump_steps(2 + 4);          /* the 4th Next Up song, t4 */
    assert(steps == 4 && playlist_next(steps) >= 0 && cur_id() == 4);
    check_queue();
    assert(next() == 1001 && next() == 1002 && next() == 5);
    new_list(10, 0); nwant = 0;
    for (int q = 1; q <= 3; q++) { queue(q, false); want_push(q, false); }
    assert(q_jump_steps(2) == 2 && playlist_next(2) >= 0 && cur_id() == 1002);
    nwant = 1; want[0] = 1003;
    check_queue();

    /* 7. A rotated list (after shuffle, first_index > 0) near its end. */
    new_list(6, 5); pl.first_index = 3; nwant = 0;
    for (int q = 1; q <= 3; q++) { queue(q, false); want_push(q, false); }
    check_queue();
    assert(next() == 1001 && next() == 1002 && next() == 1003 && next() == 0);

    /* 8. Everything at once, checking the invariant after every step. */
    for (int round = 0; round < 300; round++)
    {
        global_settings.repeat_mode = round % 3 == 0 ? REPEAT_ALL : REPEAT_OFF;
        new_list(5 + rnd() % 30, 0);
        pl.index = rnd() % pl.amount;
        nwant = 0;
        int qn = 1;
        for (int step = 0; step < 60; step++)
        {
            switch (rnd() % 9)
            {
                case 0: case 1: queue(qn, false); want_push(qn++, false); break;
                case 2: queue(qn, true); want_push(qn++, true); break;
                case 3: if (next() < 0) step = 60; break;
                case 4:
                    if (playlist_next(-1) >= 0)
                    {
                        /* a queued song that was playing is queued again, first */
                        int was = ent[q_at(1)].id;
                        if (was >= 1000 && (!nwant || want[0] != was))
                        { memmove(want + 1, want, nwant * sizeof *want); want[0] = was; nwant++; }
                        q_tidy();
                    }
                    break;
                case 5:
                {
                    int was = cur_id();
                    q_keep(); q_drop();
                    if (rnd() & 1) randomise(); else sort_list();
                    q_restore();
                    assert(cur_id() == was);
                    break;
                }
                case 6:
                {
                    int ahead = q_ahead(), run = q_run();
                    if (ahead > run)
                    {
                        int k = run + 1 + rnd() % (ahead - run);
                        int target = ent[q_at(k)].id, s = q_jump_steps(k);
                        assert(s == k - run && playlist_next(s) >= 0 && cur_id() == target);
                    }
                    break;
                }
                case 7:
                    if (nwant)
                    {
                        int k = 1 + rnd() % nwant;
                        playlist_delete(NULL, q_at(k));
                        memmove(want + k - 1, want + k, (nwant - k) * sizeof *want);
                        nwant--;
                    }
                    break;
                case 8:
                    if (nwant > 1)       /* Up Next drag inside the queue */
                    {
                        int a = 1 + rnd() % nwant, b = 1 + rnd() % nwant;
                        if (a != b)
                        {
                            assert(q_move(q_at(a), q_at(b)) == 0);
                            int m = want[a - 1];
                            if (a < b) memmove(want + a - 1, want + a, (b - a) * sizeof *want);
                            else memmove(want + b, want + b - 1, (a - b) * sizeof *want);
                            want[b - 1] = m;
                        }
                    }
                    break;
            }
            if (ent[pl.index].queued && nwant && want[0] == cur_id())
            { memmove(want, want + 1, --nwant * sizeof *want); }
            check_queue();
        }
    }
    printf("queue: priority, play next, skip back, shuffle, new list (300 queued carried over), jumps, rotated list and 300 random runs pass\n");
    return 0;
}
