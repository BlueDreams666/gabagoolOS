/* Your taste's swipe deck under stress (sloppy_taste.inc). Included from
 * ui_test.c after the taste shots, whose helpers it uses.
 *   SLOPPY_TASTE_FUZZ=<rounds> [SLOPPY_FUZZ_SEED=<n>] [SLOPPY_FUZZ_LOG=1]
 * Each round opens the deck on one of three libraries (the test one, the
 * audit's, none) and throws random events at it: slow drags and fast
 * flicks every way, the four buttons, More and Done at the end of a deck,
 * leaving mid-throw, the library read again under it, a taste.txt full to
 * its cap. Run the ASan build. */
static void tsf_drag(int cx, int cy, int dx, int dy, int steps, bool lift)
{
    t_x = cx; t_y = cy; t_down = true; frame(16);
    for (int s = 1; s <= steps; s++)
    {
        int x = cx + dx * s / steps, y = cy + dy * s / steps;
        t_x = x < 0 ? 0 : x >= W ? W - 1 : x;
        t_y = y < 0 ? 0 : y >= H ? H - 1 : y;
        frame(16);
    }
    if (lift) { t_down = false; frame(16); }
}
static void tsf_fill(int n)              /* taste.txt as full as it gets, songs that are not on the card */
{
    ts_load();
    for (int i = ts.n_rated; i < n && i < TS_RATED_MAX; i++)
        snprintf(ts.rated[ts.n_rated++], MAX_PATH, "%c/Gone/Song %d.mp3", i % 3 ? '+' : '-', i);
    ts.gen++;
}
static void taste_fuzz(int rounds)
{
    srand(getenv("SLOPPY_FUZZ_SEED") ? (unsigned)atoi(getenv("SLOPPY_FUZZ_SEED")) : 4242);
    bool log = getenv("SLOPPY_FUZZ_LOG") != NULL;
    ts_load();
    int rated0 = ts.n_rated;
    static char (*keep)[MAX_PATH];
    keep = malloc(TS_RATED_MAX * sizeof *keep);
    memcpy(keep, ts.rated, rated0 * sizeof *keep);
    while (npages > 1) { page_pop(); frame(300); }
    for (int r = 0; r < rounds; r++)
    {
        int lib = rand() % 6;            /* 0-3 the test library, 4 the audit's, 5 none */
        if (lib == 4) au_library();
        if (lib == 5) au_empty(true);
        if (rand() % 4 == 0) tsf_fill(TS_RATED_MAX - rand() % 3);
        test_status = rand() % 2 ? AUDIO_STATUS_PLAY : 0;
        if (log) fprintf(stderr, "taste fuzz round %d lib %d rated %d\n", r, lib, ts.n_rated);
        taste_page(rand() % 2);
        frame(rand() % 600);
        for (int e = 0; e < 40 && npages > 1; e++)
        {
            int kind = rand() % 16;
            struct td_slot *s = td.page ? td_slot_of(td.at) : NULL;
            lv_area_t a = { W / 2 - 100, TD_TOP, W / 2 + 100, TD_TOP + 300 };
            if (s && s->img) lv_obj_get_coords(s->img, &a);
            int cx = (a.x1 + a.x2) / 2 + rand() % 120 - 60, cy = (a.y1 + a.y2) / 2 + rand() % 160 - 80;
            if (log) fprintf(stderr, "  ev %d kind %d at %d of %d (dealt %d) flying %d\n", e, kind, td.at, td.n, td.dealt, td.flying);
            if (kind < 4)                /* a slow drag any way, let go anywhere */
                tsf_drag(cx, cy, rand() % 500 - 250, rand() % 500 - 250, 6 + rand() % 30, true);
            else if (kind < 7)           /* a flick: far in two or three frames */
            {
                int way = rand() % 3;
                tsf_drag(cx, cy, way == 0 ? 300 : way == 1 ? -300 : rand() % 60 - 30, way == 2 ? -320 : rand() % 80 - 40, 2 + rand() % 2, true);
            }
            else if (kind < 10 && td.page) key_tap(td.btn[rand() % 4]);
            else if (kind == 10 && td.page)   /* two buttons at once, before the first throw lands */
            {
                key_tap(td.btn[rand() % 4]);
                key_tap(td.btn[rand() % 4]);
            }
            else if (kind == 11 && td.page && td.hint && !lv_obj_has_flag(td.hint, LV_OBJ_FLAG_HIDDEN))
            {
                int k = rand() % 4;
                if (k == 0) td_done_cb(NULL);
                else if (k == 1) td_mix_cb(NULL);
                else td_more_cb(NULL);
            }
            else if (kind == 12) frame(rand() % 900);
            else if (kind == 13)         /* away mid-throw or mid-drag, and maybe straight back */
            {
                if (rand() % 2) tsf_drag(cx, cy, 200, 0, 3, false);
                if (td.page) key_tap(td.btn[rand() % 2]);
                frame(rand() % 60);
                t_down = false;
                if (npages > 1) page_pop();
                frame(rand() % 300);
                if (rand() % 2) { taste_page(false); frame(rand() % 300); }
            }
            else if (kind == 14 && rand() % 4 == 0)   /* the library read again under it */
            {
                db_update_reload();
                for (int i = 0; i < 400 && lib_state == SL_LIB_LOADING; i++) frame(20);
                if (rand() % 2) { taste_page(false); frame(200); }
            }
            else if (kind == 15 && test_status) { if (np_open) np_hide(0); else np_show(); frame(400); }
            assert(npages >= 1 && npages <= MAX_PAGES);
            for (int i = 0; i < npages; i++) assert(lv_obj_is_valid(pages[i]));
            if (td.page) assert(td.at >= 0 && td.at <= td.n && td.n <= TS_DECK && td.dealt <= td.n);
        }
        t_down = false; frame(16);
        if (np_open) { np_hide(0); frame(400); }
        while (npages > 1) { page_pop(); frame(300); }
        if (lib == 4) au_library_done();
        if (lib == 5) au_empty(false);
        if (r % 25 == 24) printf("taste fuzz: %d rounds\n", r + 1);
    }
    memcpy(ts.rated, keep, rated0 * sizeof *keep);
    ts.n_rated = rated0;
    ts.gen++;
    free(keep);
    printf("PASS: taste fuzz, %d rounds\n", rounds);
}

/* ---- what Your taste learns (SLOPPY_TASTE_LEARN=1 alone; in the suite too) */
/* A made-up library: albums of per songs each, album a by artist a % artists
 * and in genre a % genres, a folder an artist; what it replaces is put back
 * by tl_done(). */
static struct sl_lib tl_keep;
static struct sl_track *tl_tr;
static struct sl_album *tl_al;
static int *tl_idx;
static char *tl_str;
static void tl_make(int albums, int per, int artists, int genres)
{
    tl_keep = sl_lib;
    int n = albums * per;
    tl_tr = calloc(n, sizeof *tl_tr);
    tl_al = calloc(albums, sizeof *tl_al);
    tl_idx = malloc(n * sizeof *tl_idx);
    tl_str = malloc((size_t)n * 40 + (size_t)albums * 16 + (size_t)artists * 16 + (size_t)genres * 16);
    assert(tl_tr && tl_al && tl_idx && tl_str);
    char *s = tl_str, **art = malloc(artists * sizeof *art), **gen = malloc(genres * sizeof *gen);
    for (int i = 0; i < artists; i++) { art[i] = s; s += sprintf(s, "Artist %d", i) + 1; }
    for (int i = 0; i < genres; i++) { gen[i] = s; s += sprintf(s, "Genre %d", i) + 1; }
    for (int a = 0; a < albums; a++)
    {
        struct sl_album *al = &tl_al[a];
        al->title = s; s += sprintf(s, "Album %d", a) + 1;
        al->artist = art[a % artists];
        al->genre = gen[a % genres];
        al->year = 1960 + a % 60;
        al->n_tracks = per;
        al->tracks = tl_idx + a * per;
        for (int k = 0; k < per; k++)
        {
            int t = a * per + k;
            tl_idx[t] = t;
            char *p = s;
            s += sprintf(s, "/T/%d/%d-%d.mp3", a % artists, a, k) + 1;
            tpath(&tl_tr[t], p);
            tl_tr[t].title = tl_tr[t].file;
            tl_tr[t].album = a;
            tl_tr[t].num = k + 1;
            tl_tr[t].len = 200000;
        }
    }
    free(art); free(gen);
    sl_lib.tracks = tl_tr; sl_lib.n_tracks = n;
    sl_lib.albums = tl_al; sl_lib.n_albums = albums;
    sl_lib.path_hash_cap = 1024;
    while (sl_lib.path_hash_cap < n + n / 3) sl_lib.path_hash_cap *= 2;
    sl_lib.path_hash = malloc(sl_lib.path_hash_cap * sizeof(int));
    for (int i = 0; i < sl_lib.path_hash_cap; i++) sl_lib.path_hash[i] = -1;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < sl_lib.path_hash_cap; j++)
        {
            int slot = (sl_track_path_fnv(i) + j) & (sl_lib.path_hash_cap - 1);
            if (sl_lib.path_hash[slot] < 0) { sl_lib.path_hash[slot] = i; break; }
        }
}
static void tl_done(void)
{
    free(sl_lib.path_hash);
    sl_lib = tl_keep;
    free(tl_tr); free(tl_al); free(tl_idx); free(tl_str);
    ts.gen++;
}
/* taste.txt's swipe lines as the card has them */
static int tl_card_lines(char first[MAX_PATH], char last[MAX_PATH])
{
    char line[MAX_PATH + 4];
    int n = 0, fd = open(TS_TASTE, O_RDONLY);
    if (fd < 0) return -1;
    while (read_line(fd, line, sizeof line) > 0)
        if (line[0] == '+' || line[0] == '-' || line[0] == '*')
        {
            if (!n && first) strlcpy(first, line, MAX_PATH);
            if (last) strlcpy(last, line, MAX_PATH);
            n++;
        }
    close(fd);
    return n;
}
/* the player off and on again: taste.txt read afresh */
static void tl_reload(void)
{
    free(ts.rated);
    ts.rated = NULL;
    ts.n_rated = ts.n_hidden = 0;
    ts.loaded = false;
    ts_load();
    ts.gen++;
}
/* the deck picks with a made-up listener behind it: a run of n cards, each
 * swiped as likes() says; the hits in the first and the last stretch */
static void tl_run(bool (*likes)(int), int n, int head, int tail, int *hits_head, int *hits_tail)
{
    *hits_head = *hits_tail = 0;
    for (int i = 0; i < n; i++)
    {
        uint8_t why;
        uint32_t artist;
        td.dealt = 0;                    /* each card its own deck: nothing held back for variety's sake */
        td.n = TS_DECK;
        int t = td_pick(&why, &artist);
        assert(t >= 0);
        bool yes = likes(t);
        if (i < head) *hits_head += yes;
        if (i >= n - tail) *hits_tail += yes;
        assert(ts_swipe(t, yes ? '+' : '-'));
    }
}
static bool tl_jazz_or_7(int t)          /* Genre 0, and Artist 7 whatever the genre: about one album in ten */
{
    int a = sl_lib.tracks[t].album;
    return a % 12 == 0 || a % 100 == 7;
}
static void taste_learn_tests(void)
{
    ts_load();
    char (*keep)[MAX_PATH] = malloc(TS_RATED_MAX * sizeof *keep);
    int rated0 = ts.n_rated;
    memcpy(keep, ts.rated, rated0 * sizeof *keep);
    int st0 = test_status, q0 = queued, s0 = started;
    test_status = 0; queued = 1; started = 0; queue_tracks[0] = 0;

    /* 1. every swipe is on the card as soon as the deck is still: leaving half way (or the
     *    player going off, or a crash) keeps them, and an undo takes one back off the card */
    tl_make(40, 3, 40, 6);
    ts.n_rated = 0; ts.gen++;
    assert(ts_save_file(TS_TASTE, true) && tl_card_lines(NULL, NULL) == 0);
    taste_page(false); tsw_settle();
    /* the cards already in order: putting them in order again redraws nothing
     * (each move in front redrew the whole page, every card that landed) */
    frame(16);
    assert(disp->inv_p == 0);
    td_restack();
    assert(disp->inv_p == 0);
    int c0 = td.deck[0], c1 = td.deck[1], c2 = td.deck[2];
    key_tap(td.btn[1]); tsw_settle();    /* like */
    key_tap(td.btn[0]); tsw_settle();    /* pass */
    key_tap(td.btn[3]); tsw_settle();    /* love */
    char first[MAX_PATH], last[MAX_PATH], p[MAX_PATH];
    assert(td.page && td.at == 3 && tl_card_lines(first, last) == 3);
    assert(first[0] == '+' && !strcmp(first + 1, sl_track_path(c0, p, sizeof p)));
    assert(last[0] == '*' && !strcmp(last + 1, sl_track_path(c2, p, sizeof p)));
    key_tap(td.btn[2]); tsw_settle();    /* undo the love: off the card too */
    assert(tl_card_lines(NULL, last) == 2 && last[0] == '-' && !strcmp(last + 1, sl_track_path(c1, p, sizeof p)));
    tl_reload();                         /* off and on again, the page never closed */
    assert(ts.n_rated == 2 && ts_rating(c0) == 1 && ts_rating(c1) == -1 && ts_rating(c2) == 0);
    /* the end of the deck: Play your mix first, then More and Done */
    for (int i = 0; i < TS_DECK + 2 && lv_obj_has_flag(td.hint, LV_OBJ_FLAG_HIDDEN); i++) { key_tap(td.btn[1]); tsw_settle(); }
    assert(td.at == td.n && !lv_obj_has_flag(td.hint, LV_OBJ_FLAG_HIDDEN));
    assert(find_label(td.hint, LV_SYMBOL_PLAY "  Play your mix"));
    shot("taste-end.ppm");
    if (quiz_clip_active()) { quiz_hush_clip(); quiz_restore_clip(); }
    test_status = 0; started = -1;
    td_mix_cb(NULL); frame(300);
    assert(started >= 0 && test_status == AUDIO_STATUS_PLAY && !quiz_clip_active());   /* the mix plays; the snippet is gone, nothing to put back over it */
    page_pop(); frame(500);
    assert(tl_card_lines(NULL, NULL) == ts.n_rated);
    /* a file that grew past the cap by appending: the newest kept */
    ts.n_rated = 0; ts.gen++; ts_save_file(TS_TASTE, true);
    {
        int fd = open(TS_TASTE, O_WRONLY | O_APPEND);
        for (int i = 0; i < TS_RATED_MAX + 20; i++) { char l[64]; int n = snprintf(l, sizeof l, "+/Gone/%d.mp3\n", i); assert(write(fd, l, n) == n); }
        close(fd);
    }
    tl_reload();
    assert(ts.n_rated <= TS_RATED_MAX && ts.n_rated > TS_RATED_MAX - 60 && ts.on_card == -1);
    char want[32]; snprintf(want, sizeof want, "+/Gone/%d.mp3", TS_RATED_MAX + 19);
    assert(!strcmp(ts.rated[ts.n_rated - 1], want));
    ts_flush();
    assert(ts.on_card == ts.n_rated && tl_card_lines(NULL, NULL) == ts.n_rated);
    /* a power cut mid-append: the last line has no end; the next swipe is
     * not glued onto it (the file is written again whole) */
    ts.n_rated = 0; ts.gen++; ts_save_file(TS_TASTE, true);
    { int fd = open(TS_TASTE, O_WRONLY | O_APPEND); assert(write(fd, "+/Gone/a.mp3\n+/Gone/b", 21) == 21); close(fd); }
    tl_reload();
    assert(ts.n_rated == 2 && ts.on_card == -1);
    strlcpy(ts.rated[ts.n_rated++], "-/Gone/c.mp3", MAX_PATH); ts_flush();
    { char last[MAX_PATH]; assert(ts.on_card == 3 && tl_card_lines(NULL, last) == 3 && !strcmp(last, "-/Gone/c.mp3")); }
    tl_done();

    /* 2. it learns: a listener who likes one genre and one artist (about one song in ten);
     *    the deck's first cards are mostly something else, its later ones mostly theirs */
    tl_make(300, 10, 100, 12);
    ts.n_rated = 0; ts.gen++;
    memset(&td, 0, sizeof td);
    td.rng = 12345;
    int h0, h1;
    tl_run(tl_jazz_or_7, 200, 10, 50, &h0, &h1);
    printf("taste: a listener who likes one song in ten: %d of the first 10 cards, %d of the last 50 theirs\n", h0, h1);
    assert(h0 <= 5 && h1 >= 40);
    /* and keeps it after a restart: the same model from taste.txt alone */
    ts_flush();
    tl_reload();
    { int g0, g1; tl_run(tl_jazz_or_7, 30, 30, 30, &g0, &g1); printf("taste: after a restart, %d of the next 30\n", g1); assert(g1 >= 20); }
    memset(&td, 0, sizeof td);
    tl_done();

    /* 3. at 70,000 songs: the model built once, then each card picked in a fraction of a frame */
    tl_make(7000, 10, 700, 30);
    ts.n_rated = 0; ts.gen++;
    memset(&td, 0, sizeof td);
    td.rng = 777;
    uint64_t t0 = sl_perf_now_us();
    assert(tm_ready());
    uint64_t build = sl_perf_now_us() - t0;
    tl_run(tl_jazz_or_7, 100, 0, 0, &h0, &h1);
    t0 = sl_perf_now_us();
    tl_run(tl_jazz_or_7, 100, 0, 50, &h0, &h1);
    double pick = (sl_perf_now_us() - t0) / 100.0;
    printf("taste at 70k songs: model %.1f ms, a card %.3f ms on this Mac (est. R1 %.0f ms and %.1f ms); %d of the last 50 theirs\n",
           build / 1000.0, pick / 1000.0, build / 1000.0 * PF_K, pick / 1000.0 * PF_K, h1);
    assert(pick / 1000.0 * PF_K < 40 && h1 >= 30);
    /* Play your mix: what was liked, and twice as many picks never swiped, nearly all theirs */
    {
        uint64_t m0 = sl_perf_now_us();
        int *mix = NULL, n = ts_mix(&mix), mine = 0, swiped = 0;
        double ms = (sl_perf_now_us() - m0) / 1000.0;
        for (int i = 0; i < n; i++) { mine += tl_jazz_or_7(mix[i]); swiped += ts_rating(mix[i]) != 0; }
        printf("taste: your mix at 70k songs, %d songs, %d theirs, %d never swiped, made in %.1f ms (est. R1 %.0f ms)\n", n, mine, n - swiped, ms, ms * PF_K);
        assert(n >= 60 && mine * 10 >= n * 8 && n - swiped >= 40);
        free(mix);
    }
    memset(&td, 0, sizeof td);
    tl_done();

    memcpy(ts.rated, keep, rated0 * sizeof *keep);
    ts.n_rated = rated0; ts.gen++;
    ts_save_file(TS_TASTE, true);
    free(keep);
    test_status = st0; queued = q0; started = s0;
    printf("PASS: Your taste learns: swipes on the card as made, a liked genre and artist found, kept after a restart, your mix, 70k songs.\n");
}
