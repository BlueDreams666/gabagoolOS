/* Torture: the R1 plays with itself for a while, to shake out freezes,
 * glitches and leaks that a person finds only after days.
 *
 * On: a file .rockbox/sloppy/torture.txt at start, holding the minutes to
 * run (30 if it says nothing). It becomes torture.running while it runs and
 * torture.done when it ends; a start that finds torture.running says the
 * last run was cut short (a freeze, a crash, the battery), which is the
 * finding. Once it runs, a touch or a key stops it at once (not in the 20 s
 * before it starts: the screen may be woken then).
 *
 * It drives the player only through its own safe doors: skips and skip
 * storms, seeks, pause and play, the library's lists (scrolled, as a
 * fling would: never a finger of its own, which a slow frame could turn
 * into a hold, and a hold opens a sheet of actions), an album, Cover Flow, Now Playing and the queue, the quick
 * panel, the visualizer, For you, the games' page, a round of the music
 * quiz, and the screen locked and woken. It never taps a page's controls,
 * so nothing is deleted, reset or changed. Each step is in sloppy.log
 * ("torture 12: next"), next to the lag and watchdog lines it causes. */

#define TT_FILE    ROCKBOX_DIR "/sloppy/torture.txt"
#define TT_RUNNING ROCKBOX_DIR "/sloppy/torture.running"
#define TT_DONE    ROCKBOX_DIR "/sloppy/torture.done"
#define TT_CUT     ROCKBOX_DIR "/sloppy/torture.cut"

static struct {
    bool armed, on;
    uint32_t start_at, end_at, next_at, steps;
    uint32_t rng;
    int base;                       /* pages open when it began: it goes back there */
    int burst;                      /* skips left in a storm */
    int pending;                    /* an action's second half: what */
    uint32_t pending_at;
    uint32_t silent_since;          /* the music stopped, unasked */
    long mem_low;                   /* least memory seen, KiB */
    uint32_t report_at;
} tt;

enum { TT_NONE, TT_RESUME, TT_WAKE, TT_HOME, TT_CF_CLOSE, TT_SPA_CLOSE, TT_NP_HIDE, TT_QP_CLOSE };

static uint32_t tt_rand(uint32_t n)
{
    tt.rng ^= tt.rng << 13; tt.rng ^= tt.rng >> 17; tt.rng ^= tt.rng << 5;
    return n ? tt.rng % n : 0;
}

/* At start: a run asked for, or one cut short. */
static void sl_torture_boot(void)
{
    int fd = open(TT_RUNNING, O_RDONLY);
    if (fd >= 0)
    {
        close(fd);
        sl_log("torture: THE LAST RUN WAS CUT SHORT (a freeze, a crash or the battery): see the log before this start");
        remove(TT_CUT);
        rename(TT_RUNNING, TT_CUT);
    }
    fd = open(TT_FILE, O_RDONLY);
    if (fd < 0) return;
    char b[16] = "";
    read(fd, b, sizeof b - 1);
    close(fd);
    int minutes = atoi(b);
    if (minutes <= 0) minutes = 30;
    if (minutes > 24 * 60) minutes = 24 * 60;
    remove(TT_RUNNING);
    rename(TT_FILE, TT_RUNNING);
    tt.armed = true;
    tt.start_at = sl_ms() + 20000;           /* the library and Bluetooth settle first */
    tt.end_at = tt.start_at + (uint32_t)minutes * 60000u;
    tt.rng = (sl_ms() ^ (uint32_t)current_tick * 2654435761u) | 1;
    sl_log("torture: %d minutes, starting in 20 s; once it runs, a touch or a key stops it", minutes);
    notify_ex(NS_SYSTEM, NOTE_INFO, 0, NA_NONE, "torture", "Stress test starts in 20 s. Then a touch stops it.");
}

static void tt_home(void)
{
    if (acts_open_now) acts_close();
    if (qp_open) qp_close();
    if (uq_open) uq_close();
    if (np_open) np_hide(0);
    if (spa.active) spa_close();
    if (cf.active) cf_close();
    while (npages > tt.base && npages > 1) { int n = npages; page_pop(); if (npages == n) break; }   /* a page that will not go (the pet mid-care) */
}

static void sl_torture_stop(const char *why)
{
    if (!tt.on) return;                      /* the 20 s before: a touch to unlock is not a no */
    tt.on = tt.armed = false;
    tt_home();
    sl_log("torture: stopped (%s) after %u steps, %u s; least memory %ld KiB", why, (unsigned)tt.steps,
           (unsigned)((sl_ms() - tt.start_at) / 1000), tt.mem_low);
    remove(TT_DONE);
    rename(TT_RUNNING, TT_DONE);
    char m[64];
    snprintf(m, sizeof m, "Stress test %s", strcmp(why, "its time is up") ? "stopped" : "done");
    notify_ex(NS_SYSTEM, NOTE_INFO, 0, NA_NONE, "torture", m);
}

/* the list on top, scrolled by dy (up: negative) */
static lv_obj_t *tt_scroller(lv_obj_t *o, int depth)
{
    if (!o || depth > 6) return NULL;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_bottom(o) > 0) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
    {
        lv_obj_t *f = tt_scroller(lv_obj_get_child(o, (int32_t)i), depth + 1);
        if (f) return f;
    }
    return NULL;
}
/* the label reading s, under o */
static lv_obj_t *tt_labelled(lv_obj_t *o, const char *s, int depth)
{
    if (!o || depth > 8) return NULL;
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), sl_tr(s))) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
    {
        lv_obj_t *f = tt_labelled(lv_obj_get_child(o, (int32_t)i), s, depth + 1);
        if (f) return f;
    }
    return NULL;
}
static void tt_fling(int dy)
{
    lv_obj_t *l = npages ? tt_scroller(pages[npages - 1], 0) : NULL;
    if (l) lv_obj_scroll_by(l, 0, dy, LV_ANIM_ON);
}

static void tt_later(int what, uint32_t ms) { tt.pending = what; tt.pending_at = sl_ms() + ms; }

static void tt_step(void)
{
    static const char *const names[] = {
        "next", "previous", "skip storm", "seek", "pause", "albums", "an album", "songs", "artists",
        "Cover Flow", "Now Playing and the queue", "the visualizer", "the quick panel", "For you",
        "games", "the music quiz", "lock and wake" };
    enum { N = sizeof names / sizeof names[0] };
    static const uint8_t weight[N] = { 10, 4, 3, 5, 3, 4, 4, 3, 3, 4, 4, 2, 3, 2, 2, 3, 3 };
    int total = 0;
    for (int i = 0; i < N; i++) total += weight[i];
    int r = (int)tt_rand((uint32_t)total), a = 0;
    while (r >= weight[a]) r -= weight[a++];
    tt.steps++;
    sl_log("torture %u: %s", (unsigned)tt.steps, names[a]);
    uint32_t gap = 400 + tt_rand(2600);
    if (a >= 5) tt_home();                   /* each page from home */
    switch (a)
    {
    case 0: audio_next(); break;
    case 1: audio_prev(); break;
    case 2: tt.burst = 3 + (int)tt_rand(6); gap = 0; break;
    case 3:
    {
        struct mp3entry *id3 = audio_current_track();
        if (id3 && id3->length > 4000) audio_ff_rewind((long)tt_rand((uint32_t)(id3->length - 3000)));
        break;
    }
    case 4: toggle_play(); tt_later(TT_RESUME, 500 + tt_rand(3000)); break;
    case 5: albums_page(); tt_fling(-(int)(200 + tt_rand(1500))); break;
    case 6: if (sl_lib.n_albums) album_page((int)tt_rand((uint32_t)sl_lib.n_albums)); break;
    case 7: songs_page(); tt_fling(-(int)(200 + tt_rand(3000))); break;
    case 8: artists_page(); tt_fling(-(int)(200 + tt_rand(1500))); break;
    case 9:
        cf_open_on(sl_lib.n_albums ? (int)tt_rand((uint32_t)sl_lib.n_albums) : 0);
        tt_later(TT_CF_CLOSE, 1500 + tt_rand(3000)); gap = 0; break;
    case 10: np_show(); uq_show(); tt_later(TT_NP_HIDE, 800 + tt_rand(2000)); gap = 0; break;
    case 11: spa_open(); tt_later(TT_SPA_CLOSE, 1500 + tt_rand(3000)); gap = 0; break;
    case 12: qp_drag_to(650); qp_release(650, 900); tt_later(TT_QP_CLOSE, 600 + tt_rand(1500)); gap = 0; break;
    case 13: foryou_page(); break;
    case 14: sl_games_page(); break;
    case 15:
    {
        /* the Music Quiz (clips from the library): leaving puts the song
         * back, its steps timed in the log. Found by its name: the second
         * tile was Movie Trivia, which plays nothing (23 Nov's run) */
        quiz_page();
        lv_obj_t *t = npages ? tt_labelled(pages[npages - 1], "Music Quiz", 0) : NULL;
        while (t && !lv_obj_has_flag(t, LV_OBJ_FLAG_CLICKABLE)) t = lv_obj_get_parent(t);
        if (t) lv_obj_send_event(t, LV_EVENT_CLICKED, NULL);
        tt_later(TT_HOME, 3000 + tt_rand(6000)); gap = 0;
        break;
    }
    case 16: sl_power_lock("torture"); tt_later(TT_WAKE, 1500 + tt_rand(6000)); gap = 0; break;
    }
    tt.next_at = sl_ms() + gap;
}

static void tt_finish_pending(void)
{
    int what = tt.pending;
    tt.pending = TT_NONE;
    switch (what)
    {
    case TT_RESUME: if (!(audio_status() & AUDIO_STATUS_PLAY) || (audio_status() & AUDIO_STATUS_PAUSE)) toggle_play(); break;
    case TT_WAKE:
        sl_power_by = "torture";
        sl_power_state_set(&sl_power, SL_POWER_ACTIVE, sl_ms());
        sl_power_apply();
        break;
    case TT_CF_CLOSE: if (cf.active) cf_close(); break;
    case TT_SPA_CLOSE: if (spa.active) spa_close(); break;
    case TT_NP_HIDE: uq_close(); np_hide(0); break;
    case TT_QP_CLOSE: qp_close(); break;
    case TT_HOME: tt_home(); break;
    }
    tt.next_at = sl_ms() + 300 + tt_rand(1500);
}

/* periodic(): a step when one is due */
static void sl_torture_poll(void)
{
    if (!tt.armed) return;
    uint32_t now = sl_ms();
    if (!tt.on)
    {
        if ((int32_t)(now - tt.start_at) < 0) return;
        tt.on = true;
        tt.base = npages;
        tt.next_at = now;
        tt.report_at = now + 300000;
        tt.mem_low = sl_mem_available_kb();
        sl_log("torture: begins, %ld KiB free", tt.mem_low);
        notify_ex(NS_SYSTEM, NOTE_INFO, 0, NA_NONE, "torture", "Stress test running. A touch stops it.");
        if (!(audio_status() & AUDIO_STATUS_PLAY)) toggle_play();
    }
    if ((int32_t)(now - tt.end_at) >= 0) { sl_torture_stop("its time is up"); return; }
    /* awake and seen, as a person would keep it, except while it locks */
    if (tt.pending != TT_WAKE && sl_power.mode != SL_POWER_DARK) sl_power.activity_at = now;
    long kb = sl_mem_available_kb();
    if (kb >= 0 && (tt.mem_low < 0 || kb < tt.mem_low)) tt.mem_low = kb;
    /* the music should play throughout, apart from a pause it asked for */
    bool playing = (audio_status() & AUDIO_STATUS_PLAY) && !(audio_status() & AUDIO_STATUS_PAUSE);
    if (playing || tt.pending == TT_RESUME) tt.silent_since = 0;
    else if (!tt.silent_since) tt.silent_since = now | 1;
    else if (now - tt.silent_since > 6000)
    {
        sl_log("torture: the music stopped unasked for 6 s; playing again");
        tt.silent_since = 0;
        toggle_play();
    }
    if ((int32_t)(now - tt.report_at) >= 0)
    {
        tt.report_at = now + 300000;
        sl_log("torture: %u steps in %u min, %ld KiB free now, least %ld", (unsigned)tt.steps,
               (unsigned)((now - tt.start_at) / 60000), kb, tt.mem_low);
    }
    if (acts_open_now)                       /* a sheet of actions: never left up (nothing taps it) */
    {
        sl_log("torture: a sheet opened; closed");
        acts_close();
    }
    if (tt.pending && (int32_t)(now - tt.pending_at) >= 0) { tt_finish_pending(); return; }
    if (tt.pending) return;
    if (tt.burst > 0 && (int32_t)(now - tt.next_at) >= 0)
    {
        audio_next();
        tt.next_at = now + 100 + tt_rand(250);
        if (--tt.burst == 0) tt.next_at = now + 1500 + tt_rand(2500);
        return;
    }
    if ((int32_t)(now - tt.next_at) >= 0) tt_step();
}
