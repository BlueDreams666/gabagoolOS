/* Build like art_miss_test.c (the sim include flags, this file in place of
 * ui_test.c, no other sources). Run: ./art-loop-test
 *
 * The R1 freeze of 2026-10-23: a database update after USB mode, a card of
 * 1700 albums whose covers are being made, and the UI loop stuck 88 s, then
 * for good, in 'art make'. Rockbox threads on the R1 take turns on one OS
 * thread, so this runs them the same way: the UI loop here, the cover
 * thread as a coroutine that runs only when the UI waits or the cover
 * thread is scheduled. The tag database scanner is a turn of TC_TURN_MS
 * that it takes when the UI waits and on every 17th yield the UI makes
 * (thread.c's aging: priority 20 against the UI's 16). Rockbox's JPEG
 * decoder yields once per row; ROWS of them for a cover.
 *
 * Checked: the UI loop never goes LOOP_MAX_MS without coming round (the
 * watchdog says "stuck" at 1000), every cover is made, a library reload
 * mid-cover is safe, and USB mode finds no file of the thread open.
 *
 * And the R1 stalls of 2026-10-23 (build 123a36bb27): 2-3 s in the art
 * step, four times in the one-time rebuild of 1700 covers. A card where
 * every lookup the UI makes costs CARD_MS, 2000 albums: the art step
 * never holds a pass over STEP_MAX_MS, through a full rebuild, a library
 * update that changes every cover (the check of the old ones, the prune,
 * the remake), Cover Flow's neighbours asked for meanwhile, and a boot
 * that walks them all. */
#define _XOPEN_SOURCE 600
#include <ucontext.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* ucontext: still works on macOS */
#include <time.h>
#include <stdbool.h>
#include <errno.h>
/* the slow card's clock: the time its lookups take, counted, not slept,
 * so the step's 20 ms share is measured as the R1 would see it */
static bool card_clock;
static long long card_us;
static int test_clock_gettime(clockid_t c, struct timespec *t)
{
    if (!card_clock) return clock_gettime(c, t);
    t->tv_sec = (time_t)(card_us / 1000000); t->tv_nsec = (long)(card_us % 1000000) * 1000;
    return 0;
}
#define clock_gettime test_clock_gettime
#include "sloppy_art.c"
#include "test_paths.h"
const char *volatile sl_at_where;   /* the hang watchdog's breadcrumb, in sloppy.c */
#include <sys/time.h>
#include <stdio.h>
#undef open
#undef close
#undef read
#undef write
#undef mkdir
#undef remove
#undef rename
#undef lseek
#undef fsync
#undef assert
#define assert(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define TC_TURN_MS 20
#define LOOP_MAX_MS 200
#define CARD_MS 2
#define STEP_MAX_MS 30
static int rows = 1000;   /* a cover's rows; few in the big library, where the card is the point */

static long now_ms(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec * 1000L + t.tv_usec / 1000; }

/* ---- the cooperative scheduler */
static ucontext_t ui_ctx, art_ctx;
static bool in_art, art_made_thread, threads_on = true, scanner = true;
static int sem_count, ui_yields, tc_turns;
static void tc_turn(void) { if (!scanner) return; tc_turns++; usleep(TC_TURN_MS * 1000); }   /* the scanner's read()s */
static void to_ui(void) { in_art = false; swapcontext(&art_ctx, &ui_ctx); }
static void art_entry(void) { art_thread(); }
unsigned int create_thread(void (*fn)(void), void *st, size_t sz, unsigned fl, const char *nm IF_PRIO(, int pr) IF_COP(, unsigned int co))
{
    (void)fn; (void)st; (void)sz; (void)fl; (void)nm; IF_PRIO((void)pr;) IF_COP((void)co;)
    if (!threads_on) return 0;
    static char stack[512 * 1024];
    getcontext(&art_ctx);
    art_ctx.uc_stack.ss_sp = stack; art_ctx.uc_stack.ss_size = sizeof stack; art_ctx.uc_link = NULL;
    makecontext(&art_ctx, art_entry, 0);
    art_made_thread = true;
    return 1;
}
void semaphore_init(struct semaphore *s, int m, int st) { (void)s; (void)m; sem_count = st; }
void semaphore_release(struct semaphore *s) { (void)s; if (sem_count < 1) sem_count++; }
int semaphore_wait(struct semaphore *s, int t) { (void)s; (void)t; while (!sem_count) to_ui(); sem_count--; return 1; }
void yield(void)
{
    if (in_art) { to_ui(); return; }
    if (++ui_yields % 17 == 0) tc_turn();
}
static void art_slice(void) { if (!art_made_thread) return; in_art = true; swapcontext(&ui_ctx, &art_ctx); }
/* the UI waits for its next frame: the scanner and the cover thread share it */
static void ui_wait(void)
{
    tc_turn();
    long t0 = now_ms();
    do art_slice(); while (now_ms() - t0 < 16 && aj.state != AJ_IDLE && aj.state != AJ_DONE);
}
unsigned sleep(unsigned t) { (void)t; if (in_art) to_ui(); else ui_wait(); return 0; }

/* ---- the card: a lookup made on the UI loop costs card_ms on the card's
 * clock (the cover thread's own are not counted: it yields between them) */
static char root[256];
const char *sl_net_card_root(void) { return root; }   /* "Set as wallpaper" reads the card by its real path */
static int open_fds, card_ms;
/* a busy card under memory pressure: now and then one lookup takes this long (a test R1: 100-485 ms "in covers") */
static int spike_every, spike_ms, card_lookups;
static void card(void)
{
    if (!card_ms || in_art) return;
    card_lookups++;
    card_us += (spike_every && card_lookups % spike_every == 0 ? spike_ms : card_ms) * 1000LL;
}
static const char *at(const char *p, char *b) { if (!strncmp(p, "/.rockbox", 9)) { snprintf(b, 512, "%s%s", root, p); return b; } return p; }
int sim_open(const char *p, int f, ...) { char b[512]; card(); int fd = open(at(p,b), f, 0666); if (fd >= 0) open_fds++; return fd; }
int sim_close(int fd) { open_fds--; return close(fd); }
ssize_t sim_read(int fd, void *b, size_t n) { return read(fd, b, n); }
ssize_t sim_write(int fd, const void *b, size_t n) { return write(fd, b, n); }
off_t sim_lseek(int fd, off_t o, int w) { return lseek(fd, o, w); }
int sim_fsync(int fd) { return fsync(fd); }
int sim_remove(const char *p) { char b[512]; card(); return remove(at(p,b)); }
int sim_rename(const char *a, const char *c) { char b[512], d[512]; card(); return rename(at(a,b), at(c,d)); }
int sim_mkdir(const char *p) { char b[512]; card(); return mkdir(at(p,b), 0777); }
bool sim_file_exists(const char *p) { char b[512]; struct stat st; card(); return !stat(at(p,b), &st); }
bool sim_dir_exists(const char *p) { char b[512]; struct stat st; card(); return !stat(at(p,b), &st) && S_ISDIR(st.st_mode); }
#undef opendir
#undef readdir
#undef closedir
#undef rmdir
#undef dirent
DIR *sim_opendir(const char *p) { char b[512]; card(); return opendir(at(p,b)); }
struct sim_dirent *sim_readdir(DIR *d)
{
    static struct sim_dirent e; struct dirent *h = readdir(d);
    if (!h) return NULL;
    memset(&e, 0, sizeof e); strlcpy(e.d_name, h->d_name, sizeof e.d_name); return &e;
}
int sim_closedir(DIR *d) { return closedir(d); }
int sim_rmdir(const char *p) { char b[512]; card(); return rmdir(at(p,b)); }

/* ---- every song has a baseline JPEG that takes ROWS rows */
struct sl_lib sl_lib;
void sl_log(const char *f, ...) { (void)f; }
bool get_metadata(struct mp3entry *id3, int fd, const char *path)
{ (void)fd; (void)path; id3->has_embedded_albumart = true; id3->albumart.type = AA_TYPE_JPG; id3->albumart.pos = 0; id3->albumart.size = 1; return true; }
bool search_albumart_files(const struct mp3entry *id3, const char *s, char *b, int n) { (void)id3; (void)s; (void)b; (void)n; return false; }
int clip_jpeg_fd(int fd, int t, unsigned long s, struct bitmap *bm, int m, int f, const struct custom_format *c, bool (*x)(int, int))
{
    (void)fd;(void)t;(void)s;(void)f;(void)c;(void)x;
    for (int row = 0; row < rows; row++) yield();   /* jpeg_load.c: once per MCU row */
    bm->width = bm->height = SL_ART_GRID;
    assert(m >= SL_ART_GRID * SL_ART_GRID * 2);
    memset(bm->data, 0x5a, SL_ART_GRID * SL_ART_GRID * 2);
    return 1;
}
int clip_png_fd(int fd, int p, int s, struct bitmap *bm, int m, int f) { (void)fd;(void)p;(void)s;(void)bm;(void)m;(void)f; return -1; }
int read_png_fd(int fd, struct bitmap *bm, int m, int f) { (void)fd;(void)bm;(void)m;(void)f; return -1; }
int read_bmp_fd(int fd, struct bitmap *bm, int m, int f, const struct custom_format *c) { (void)fd;(void)bm;(void)m;(void)f;(void)c; return -1; }
int read_jpeg_fd(int fd, int t, struct bitmap *bm, int m, int f, const struct custom_format *c, bool (*x)(int, int)) { (void)fd;(void)t;(void)bm;(void)m;(void)f;(void)c;(void)x; return -1; }
void *lv_event_get_target(lv_event_t *e) { (void)e; return NULL; }
void *lv_event_get_user_data(lv_event_t *e) { (void)e; return NULL; }
void lv_fs_drv_init(lv_fs_drv_t *d) { memset(d, 0, sizeof *d); }
void lv_fs_drv_register(lv_fs_drv_t *d) { (void)d; }
void lv_image_cache_drop(const void *s) { (void)s; }
lv_obj_t *lv_image_create(lv_obj_t *p) { (void)p; return NULL; }
void lv_image_set_src(lv_obj_t *o, const void *s) { (void)o; (void)s; }
lv_event_dsc_t *lv_obj_add_event_cb(lv_obj_t *o, lv_event_cb_t c, lv_event_code_t f, void *u) { (void)o;(void)c;(void)f;(void)u; return NULL; }
void lv_obj_add_flag(lv_obj_t *o, lv_obj_flag_t f) { (void)o; (void)f; }
void lv_obj_center(lv_obj_t *o) { (void)o; }
lv_style_value_t lv_obj_get_style_prop(const lv_obj_t *o, lv_part_t p, lv_style_prop_t q) { (void)o;(void)p;(void)q; lv_style_value_t v = {0}; return v; }
bool lv_obj_remove_event_cb(lv_obj_t *o, lv_event_cb_t c) { (void)o; (void)c; return true; }
void lv_obj_set_style_clip_corner(lv_obj_t *o, bool v, lv_style_selector_t s) { (void)o;(void)v;(void)s; }
void lv_obj_set_style_radius(lv_obj_t *o, int32_t v, lv_style_selector_t s) { (void)o;(void)v;(void)s; }
const size_t JPEG_DECODE_OVERHEAD = 0;

enum { NMAX = 2000 };
static int N = 24;
static struct sl_track tracks[NMAX];
static struct sl_album albums[NMAX];
static int idx[NMAX], order[NMAX];
static char paths[NMAX][600], titles[NMAX][16];
static void library(void)
{
    for (int a = 0; a < N; a++)
    {
        snprintf(paths[a], sizeof paths[a], "%s/music/%04d.flac", root, a);
        FILE *f = fopen(paths[a], "w"); fputs("x", f); fclose(f);
        snprintf(titles[a], sizeof titles[a], "Album %04d", a);
        tpath(&tracks[a], paths[a]); tracks[a].title = "t"; idx[a] = a; order[a] = a;
        albums[a].title = titles[a]; albums[a].artist = "Artist"; albums[a].n_tracks = 1; albums[a].tracks = &idx[a];
    }
    sl_lib.tracks = tracks; sl_lib.n_tracks = N; sl_lib.albums = albums; sl_lib.n_albums = N; sl_lib.albums_by_title = order;
}
/* no covers on the card at all: every album to be made */
static void fresh(void)
{
    char b[600]; snprintf(b, sizeof b, "rm -rf %s/.rockbox/sloppy/thumbs", root); system(b);
    thumb_generation = 0; sl_art_init(); sl_art_reset();
}
static int made(void) { int n = 0; for (int a = 0; a < N; a++) n += sl_art_src(a, SL_ART_GRID) != NULL; return n; }
/* The UI loop: an art step, then the wait for the next frame. The worst
 * time between two passes is what the watchdog would see. */
static long run_loop(int passes)
{
    long worst = 0, t0 = now_ms();
    bool more = true;
    for (int i = 0; i < passes && (more || sl_art_busy()); i++)
    {
        more = sl_art_step();
        ui_wait();
        long t1 = now_ms();
        if (t1 - t0 > worst) worst = t1 - t0;
        t0 = t1;
    }
    return worst;
}

/* The big library on the slow card. One pass: the art step, then the
 * cover thread's turn up to its next yield (fast here: only the UI's
 * lookups are timed). The worst art step is what the loop is held. */
static long worst_step;
static void big_pass(void)
{
    long long t0 = card_us;
    sl_art_step();
    long ms = (long)((card_us - t0) / 1000);
    if (ms > worst_step) worst_step = ms;
    art_slice();
}
static long big_run(int max_passes)
{
    worst_step = 0;
    int i = 0;
    for (bool more = true; i < max_passes && (more || sl_art_busy()); i++)
    {
        long long t0 = card_us;
        more = sl_art_step();
        long ms = (long)((card_us - t0) / 1000);
        if (ms > worst_step) worst_step = ms;
        art_slice();
    }
    return i;
}
static int made_all(void) { int was = card_ms; card_ms = 0; int n = made(); card_ms = was; return n; }
static void big_library(void)
{
    N = NMAX; rows = 4; scanner = false;
    library();
    fresh();
    card_ms = CARD_MS; card_clock = true;
    /* the one-time rebuild: every cover made, no step past STEP_MAX_MS */
    int passes = big_run(400000);
    assert(made_all() == N);
    printf("PASS: a full rebuild of %d covers on a %d ms card: %d passes, the art step at most %ld ms\n",
           N, CARD_MS, passes, worst_step);
    assert(worst_step <= STEP_MAX_MS);

    /* new art in every song and a library update: the old covers are
     * checked and pruned, every cover made again; Cover Flow's neighbours
     * meanwhile come next, before the walk */
    char b[700];
    struct timeval tv[2] = {{1500000000, 0}, {1500000000, 0}};
    for (int a = 0; a < N; a++) utimes(paths[a], tv);
    sl_art_invalidate_cache(); sl_art_reset();
    worst_step = 0;
    for (int i = 0; i < 300; i++) big_pass();
    long before = worst_step;
    int want[3] = { N - 1, N / 2, N - 7 };
    for (int k = 0; k < 3; k++) sl_art_prefer(want[k]);
    for (int i = 0; i < 40; i++) big_pass();
    card_ms = 0;
    for (int k = 0; k < 3; k++) assert(sl_art_src(want[k], SL_ART_GRID));
    card_ms = CARD_MS;
    if (before > worst_step) worst_step = before;
    long upd = worst_step;
    passes = big_run(400000);
    if (upd > worst_step) worst_step = upd;
    assert(made_all() == N);
    snprintf(b, sizeof b, "%s/.rockbox/sloppy/thumbs/g%08x", root, thumb_generation - 1);
    struct stat st;
    assert(stat(b, &st) != 0);             /* the old generation is gone */
    printf("PASS: a library update that changes every cover: checked, pruned, remade; "
           "Cover Flow's 3 first; the art step at most %ld ms\n", worst_step);
    assert(worst_step <= STEP_MAX_MS);

    /* a boot with every cover made: the walk looks at all of them */
    sl_art_reset();
    passes = big_run(400000);
    assert(!sl_art_busy() && next_album == N);
    printf("PASS: a boot walk past %d made covers: %d passes, the art step at most %ld ms\n",
           N, passes, worst_step);
    assert(worst_step <= STEP_MAX_MS);
    card_ms = 0; card_clock = false;
}

/* The walk's lookups on a slow card: a boot with 1,724 covers all made (a
 * test R1's library), every lookup 20 ms, then one in 50 of the UI's taking
 * 400 ms. What the UI loop is held per pass, as the R1's lag lines count it:
 * the walk's lookups are on the finder's thread (slept there for real, not
 * counted), so only the UI's own are. */
static int find_ms, find_lookups;
static bool test_find(const char *p)
{
    char b[512]; struct stat st;
    __sync_fetch_and_add(&find_lookups, 1);
    if (find_ms) usleep((useconds_t)find_ms * 1000);
    return !stat(at(p, b), &st);
}
static int creates;
static int test_create_fails_once(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg)
{
    size_t st = 0;
    assert(a && pthread_attr_getstacksize(a, &st) == 0 && st == 64 * 1024);   /* the small stack asked for */
    if (++creates == 1) return EAGAIN;
    return pthread_create(t, a, f, arg);
}
static void slow_card_walk(int lookup_ms, int every, int every_ms)
{
    N = 1724; rows = 4; scanner = false;
    library();
    fresh();
    card_ms = 0;
    big_run(400000);                       /* every cover made, on a quick card */
    assert(made_all() == N);
    sl_art_reset();
    art_find_exists = test_find; find_ms = lookup_ms; find_lookups = 0;
    card_ms = lookup_ms; spike_every = every; spike_ms = every_ms; card_lookups = 0; card_clock = true;
    long long sum0 = card_us;
    worst_step = 0;
    long w0 = now_ms();
    int passes = 0;
    bool prefer_checked = false, reload_checked = false;
    for (bool more = true; passes < 400000 && (more || sl_art_busy()); passes++)
    {
        long long t0 = card_us;
        more = sl_art_step();
        long ms = (long)((card_us - t0) / 1000);
        if (ms > worst_step) worst_step = ms;
        art_slice();
        usleep(2000);                      /* the rest of a UI pass */
        /* mid-walk, Cover Flow's neighbour with a cover gone: made at once, never a placeholder */
        if (!prefer_checked && next_album > N / 3 && fj_state() == 1)
        {
            prefer_checked = true;
            int x = N - 3;
            char path[600]; card_ms = 0;
            thumb_path(path, sizeof path, x, SL_ART_GRID);
            char real[700]; snprintf(real, sizeof real, "%s%s", root, path); remove(real);
            sl_art_again(x);
            card_ms = lookup_ms;
            sl_art_step();
            assert(aj.album == x && aj.state != AJ_IDLE);
            while (sl_art_busy()) { art_slice(); sl_art_step(); }
            card_ms = 0; assert(sl_art_src(x, SL_ART_GRID)); card_ms = lookup_ms;
        }
    }
    long long held = card_us - sum0;
    printf("slow card walk: %d made covers, %d ms a lookup%s: %d passes, %d of %d albums walked, the UI loop held %ld ms at worst, %.2f ms a pass on average, %d lookups on the UI loop, %d on the finder's thread (walk %ld s)\n",
           N, lookup_ms, every ? ", one in 50 at 400 ms" : "", passes, next_album, N, worst_step,
           passes ? held / 1000.0 / passes : 0.0, card_lookups, find_lookups, (now_ms() - w0) / 1000);
    assert(!sl_art_busy() && next_album == N && prefer_checked);
    for (int a = 0; a < N; a++) assert(sl_art_state(a) == 1);   /* every album known made */
    assert(worst_step <= STEP_MAX_MS);
    /* a library reload with a batch under way: it stops, nothing of it lands on the new library */
    if (every)
    {
        sl_art_reset();
        sl_art_step();
        assert(fj_state() == 1);
        N = 10; library();
        sl_art_invalidate_cache(); sl_art_reset();
        assert(fj_state() == 0 && next_album == 0 && !attempted);
        for (int i = 0; i < 2000 && (sl_art_step() || sl_art_busy()); i++) { art_slice(); usleep(2000); }
        assert(next_album == N && attempted_count == N);
        reload_checked = true;
        printf("PASS: a library reload with the finder's batch under way: stopped, nothing stale kept\n");
    }
    (void)reload_checked;
    /* no thread to be had once: the UI walks itself meanwhile, and 30 s on the thread is tried again and used */
    if (every)
    {
        find_ms = 0;
        N = 1724; sl_lib.n_tracks = N; sl_lib.n_albums = N;   /* back to the big library */
        art_find_exists = NULL; card_ms = 0;                     /* its covers moved into the reload's generation */
        sl_art_reset(); big_run(400000);
        /* and without the finder, on the 20 ms card: one size a step, never the same one again */
        sl_art_reset(); card_ms = 20; worst_step = 0;
        int sync_passes = big_run(400000);
        assert(next_album == N);
        printf("PASS: without the finder, a 20 ms card: the UI walked all %d in %d passes, at most %ld ms each (it never got past the first before)\n",
               N, sync_passes, worst_step);
        assert(worst_step <= every_ms);   /* one lookup at most: a spike itself, never two */
        art_find_exists = test_find; card_ms = 2;
        sl_art_reset();
        find_create = test_create_fails_once; creates = 0;
        sl_art_step();
        assert(creates == 1 && find_failed && fj_state() == 0);
        int ui0 = card_lookups;
        for (int i = 0; i < 20; i++) { sl_art_step(); art_slice(); }
        assert(card_lookups > ui0 && creates == 1 && next_album > 0);   /* the UI looked itself, no new try yet */
        card_us += 31000LL * 1000;                                       /* 30 s on */
        int walked = next_album;
        sl_art_step();
        assert(creates == 2 && !find_failed && fj_state() != 0);        /* the thread again */
        int ui1 = card_lookups;
        for (int i = 0; i < 400000 && (sl_art_step() || sl_art_busy()); i++) { art_slice(); usleep(200); }
        assert(next_album == N && walked < N && card_lookups - ui1 == 0);
        printf("PASS: no thread for the walk once: the UI looked itself (%d albums), and 30 s later the thread walked the other %d with %d lookups on the UI\n",
               walked, N - walked, card_lookups - ui1);
        find_create = pthread_create;
    }
    art_find_exists = NULL; find_ms = 0;
    card_ms = 0; spike_every = 0; card_clock = false;
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/art-loop-%d", (int)getpid());
    char b[600]; mkdir(root, 0777); snprintf(b, sizeof b, "%s/.rockbox", root); mkdir(b, 0777);
    snprintf(b, sizeof b, "%s/music", root); mkdir(b, 0777);
    library();
    sl_art_init();
    assert(art_thread_id);

    /* the old way, the UI making the cover itself: one cover is a stall */
    unsigned id = art_thread_id; art_thread_id = 0;
    sl_art_reset();
    sl_art_prefer(0);
    long t0 = now_ms(); sl_art_step(); long inline_ms = now_ms() - t0;
    assert(sl_art_src(0, SL_ART_GRID));
    assert(inline_ms >= 1000);              /* past the watchdog: the R1's freeze */
    art_thread_id = id;

    /* with the cover thread: every cover made, the loop never held */
    fresh();
    int turns0 = tc_turns;
    long worst = run_loop(5000);
    assert(made() == N);
    assert(sl_art_made >= N);
    assert(worst < LOOP_MAX_MS);
    assert(tc_turns > turns0);             /* the scanner had its turns all along */
    assert(open_fds == 0);

    /* the library reloaded while a cover is half made: nothing of the old
     * library is touched, and the album is made again for the new one */
    fresh();
    sl_art_step();
    assert(aj.state == AJ_QUEUED);
    art_slice(); art_slice();
    assert(aj.state == AJ_RUNNING && open_fds == 1);   /* mid-decode, the song open */
    static struct sl_album moved[NMAX]; memcpy(moved, albums, sizeof moved);
    memset(albums, 0xff, sizeof albums);    /* the old library freed */
    sl_lib.albums = moved;
    sl_art_invalidate_cache(); sl_art_reset();
    while (aj.state == AJ_RUNNING) art_slice();
    assert(aj.state == AJ_DONE && aj.again && open_fds == 0);
    worst = run_loop(5000);
    assert(made() == N && worst < LOOP_MAX_MS);

    /* USB mode during a cover: the thread lets go of the card first */
    fresh();
    sl_art_step(); art_slice(); art_slice();
    assert(aj.state == AJ_RUNNING && open_fds == 1);
    sl_art_release();
    assert(aj.state != AJ_RUNNING && open_fds == 0);
    run_loop(5000);
    assert(made() == N);

    printf("PASS: a cover made by the UI held its loop %ld ms behind the tag scanner; "
           "on the cover thread %d covers, the loop at most %ld ms apart\n", inline_ms, N, worst);
    printf("PASS: a library reload and USB mode mid-cover: nothing stale written, no file left open\n");
    big_library();
    slow_card_walk(20, 0, 0);
    slow_card_walk(20, 50, 400);
    snprintf(b, sizeof b, "rm -rf %s", root); system(b);
    return 0;
}
