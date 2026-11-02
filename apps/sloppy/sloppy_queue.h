/* The explicit queue ("In your queue") on top of Rockbox's playlist. GPLv2+.
 *
 * Rockbox marks queued tracks (PLAYLIST_ATTR_QUEUED) and drops each one once
 * it has played, but it leaves where a queued track lands to last_insert_pos,
 * and several things carried queued tracks away from the playing one:
 *  - play_list() builds lists with PLAYLIST_INSERT_LAST, which leaves
 *    last_insert_pos on the list's last track, so "Add to queue"
 *    (PLAYLIST_INSERT) put the song after everything in Next Up;
 *  - shuffle and unshuffle reorder queued tracks with the rest;
 *  - skipping back puts the song that was playing in front of the queue;
 *  - tapping a Next Up song skipped over the queue, which deletes it.
 * The rule here: queued tracks are one run straight after the playing track,
 * in the order they were queued. They play before anything else, and Up Next
 * shows exactly what will play.
 *
 * Only the playlist calls below are used, so tests/queue_test.c runs this
 * against a copy of Rockbox's own index bookkeeping. */


static int q_cur(void) { int i = 0; playlist_get_resume_info(&i); return i; }
static int q_at(int k) { int n = playlist_amount(); return n > 0 ? (q_cur() + k) % n : 0; }
static bool q_is_queued(int idx)
{
    int attr = playlist_get_track_attr(NULL, idx);
    return attr >= 0 && (attr & PLAYLIST_ATTR_QUEUED);
}
/* Place in play order, counted from the list's first track. */
static int q_rot(int idx)
{
    int n = playlist_amount(), r = idx - playlist_get_first_index(NULL);
    return r < 0 ? r + n : r;
}
/* How many tracks will still play after this one. */
static int q_ahead(void)
{
    int n = playlist_amount();
    if (n <= 0) return 0;
    if (global_settings.repeat_mode == REPEAT_ALL) return n - 1;
    return n - 1 - q_rot(q_cur());
}
/* Length of the queued run straight after the playing track. */
static int q_run(void)
{
    int ahead = q_ahead(), run = 0;
    while (run < ahead && q_is_queued(q_at(run + 1))) run++;
    return run;
}

/* Put track x straight after track y. playlist_move() puts a track that
 * moves later in the list after the one at new_index, and one that moves
 * earlier in its place. */
static int q_place_after(int x, int y)
{
    int n = playlist_amount();
    if (x == y || x == (y + 1) % n) return 0;
    return playlist_move(NULL, x, q_rot(x) < q_rot(y) ? y : (y + 1) % n);
}

/* Drop track x where track y is, as an Up Next drag does: after y when x
 * came from before it, else in front of it. Counted in play order from the
 * playing track, which playlist_move() alone gets wrong once the order
 * wraps past the list's first track (repeat all, shuffle). */
static int q_move(int x, int y)
{
    int n = playlist_amount(), c = q_cur();
    if ((x - c + n) % n < (y - c + n) % n) return q_place_after(x, y);
    return q_place_after(x, (y - 1 + n) % n);
}

/* Queue a song at the end of the queue, or at its front for Play next. */
static int q_insert(const char *path, bool front, bool sync)
{
    int run = front ? 0 : q_run();
    if (run == 0)
        return playlist_insert_track(NULL, path, PLAYLIST_INSERT_FIRST, true, sync);
    /* PLAYLIST_INSERT goes after last_insert_pos: point it at the last
     * queued song, whatever inserted last. Rockbox then keeps the
     * playing index and first_index right, even when the run wraps. */
    playlist_get_current()->last_insert_pos = q_at(run);
    return playlist_insert_track(NULL, path, PLAYLIST_INSERT, true, sync);
}

/* Gather queued tracks that are not in the run into it, keeping their
 * order. Returns how many moved. */
static int q_tidy(void)
{
    int moved = 0;
    for (int guard = playlist_amount(); guard > 0; guard--)
    {
        int n = playlist_amount(), run = q_run(), k = run + 2;
        while (k < n && !q_is_queued(q_at(k))) k++;
        if (k >= n) break;
        if (q_place_after(q_at(k), q_at(run)) < 0) break;
        moved++;
    }
    return moved;
}

/* Play the song k places ahead now. Queued songs before it are skipped and
 * leave the queue, as in Rockbox. From Next Up, the queue first moves
 * behind the song so it still plays next. Returns the steps to skip. */
static int q_jump_steps(int k)
{
    int run = q_run();
    if (k <= run) return k;
    for (int m = 0; m < run; m++)
        if (q_place_after(q_at(run - m), q_at(k - m)) < 0) return -1;
    return k - run;
}

/* Carrying the queue over a list rebuild (a new list, shuffle, sort):
 * q_keep() notes the queued songs in play order, q_drop() takes them out
 * of the list, and q_restore() queues them again after the playing track. */
static char *q_kept_pool;                /* the queued songs' paths, end to end, kept for the next list */
static size_t q_kept_len, q_kept_cap;
static int *q_kept_off, *q_kept_idx, q_kept_n, q_kept_room;
static bool q_kept_add(const char *path, int idx)
{
    size_t n = strlen(path) + 1;
    if (q_kept_len + n > q_kept_cap)
    {
        size_t cap = q_kept_cap ? q_kept_cap * 2 : 4096;
        while (cap < q_kept_len + n) cap *= 2;
        char *p = realloc(q_kept_pool, cap);
        if (!p) return false;
        q_kept_pool = p;
        q_kept_cap = cap;
    }
    if (q_kept_n == q_kept_room)
    {
        int room = q_kept_room ? q_kept_room * 2 : 64;
        int *o = realloc(q_kept_off, (size_t)room * sizeof *o);
        if (o) q_kept_off = o;
        int *i = o ? realloc(q_kept_idx, (size_t)room * sizeof *i) : NULL;
        if (!i) return false;
        q_kept_idx = i;
        q_kept_room = room;
    }
    memcpy(q_kept_pool + q_kept_len, path, n);
    q_kept_off[q_kept_n] = (int)q_kept_len;
    q_kept_idx[q_kept_n++] = idx;
    q_kept_len += n;
    return true;
}
static int q_keep(void)
{
    q_kept_n = 0;
    q_kept_len = 0;
    int n = playlist_amount();
    struct playlist_track_info info;
    for (int k = 1; k < n; k++)              /* every queued song, however many */
    {
        int idx = q_at(k);
        if (!q_is_queued(idx) || playlist_get_track_info(NULL, idx, &info) < 0) continue;
        if (!q_kept_add(info.filename, idx)) break;
    }
    return q_kept_n;
}
static void q_drop(void)
{
    /* highest index first, so the others keep their places */
    for (int left = q_kept_n; left > 0; left--)
    {
        int best = -1;
        for (int i = 0; i < q_kept_n; i++)
            if (q_kept_idx[i] >= 0 && (best < 0 || q_kept_idx[i] > q_kept_idx[best])) best = i;
        playlist_delete(NULL, q_kept_idx[best]);
        q_kept_idx[best] = -1;
    }
}
static int q_restore(void)
{
    int put = 0;
    for (int i = 0; i < q_kept_n; i++)
        if (q_insert(q_kept_pool + q_kept_off[i], false, false) >= 0) put++;
    if (q_kept_n) playlist_sync(NULL);
    q_kept_n = 0;
    return put;
}
