/* The live tiles' covers, made beside the UI loop (sl_art_apply_scaled_async
 * in sloppy_art.c), with the real reader thread against a real folder:
 * - a cover read and scaled on the thread lands in its box at the next poll,
 *   the size asked for, its pixels the thumbnail's;
 * - a box deleted before its cover comes gets nothing, and the picture is
 *   freed (run under AddressSanitizer to see no leak);
 * - with every job slot held, the next cover is made at once, as before;
 * - a thumbnail that cannot be read fills no box and frees its slot;
 * - USB mode (sc_settle) waits for a read under way, and no longer.
 * Build and run with run-art-scaled-test.sh (the sim include flags, no other
 * sources: it includes sloppy_art.c itself). */
#include "sloppy_art.c"
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
static char root[256];
const char *sl_net_card_root(void) { return root; }
static const char *at(const char *p, char *b) { if (!strncmp(p, "/.rockbox", 9)) { snprintf(b, 512, "%s%s", root, p); return b; } return p; }
int sim_open(const char *p, int f, ...) { char b[512]; return open(at(p,b), f, 0666); }
int sim_close(int fd) { return close(fd); }
ssize_t sim_read(int fd, void *b, size_t n) { return read(fd, b, n); }
ssize_t sim_write(int fd, const void *b, size_t n) { return write(fd, b, n); }
off_t sim_lseek(int fd, off_t o, int w) { return lseek(fd, o, w); }
int sim_fsync(int fd) { return fsync(fd); }
int sim_remove(const char *p) { char b[512]; return remove(at(p,b)); }
int sim_rename(const char *a, const char *c) { char b[512], d[512]; return rename(at(a,b), at(c,d)); }
int sim_mkdir(const char *p) { char b[512]; return mkdir(at(p,b), 0777); }
bool sim_file_exists(const char *p) { char b[512]; struct stat st; return !stat(at(p,b), &st); }
bool sim_dir_exists(const char *p) { char b[512]; struct stat st; return !stat(at(p,b), &st) && S_ISDIR(st.st_mode); }
int sim_rmdir(const char *p) { (void)p; return -1; }
DIR *sim_opendir(const char *p) { (void)p; return NULL; }
struct sim_dirent *sim_readdir(DIR *d) { (void)d; return NULL; }
int sim_closedir(DIR *d) { (void)d; return 0; }
struct sl_lib sl_lib;
void sl_log(const char *f, ...) { (void)f; }
bool get_metadata(struct mp3entry *id3, int fd, const char *path) { (void)id3; (void)fd; (void)path; return true; }
bool search_albumart_files(const struct mp3entry *id3, const char *s, char *b, int n) { (void)id3; (void)s; (void)b; (void)n; return false; }
int clip_jpeg_fd(int fd, int t, unsigned long s, struct bitmap *bm, int m, int f, const struct custom_format *c, bool (*x)(int, int)) { (void)fd;(void)t;(void)s;(void)bm;(void)m;(void)f;(void)c;(void)x; return -1; }
int clip_png_fd(int fd, int p, int s, struct bitmap *bm, int m, int f) { (void)fd;(void)p;(void)s;(void)bm;(void)m;(void)f; return -1; }
int read_png_fd(int fd, struct bitmap *bm, int m, int f) { (void)fd;(void)bm;(void)m;(void)f; return -1; }
int read_bmp_fd(int fd, struct bitmap *bm, int m, int f, const struct custom_format *c) { (void)fd;(void)bm;(void)m;(void)f;(void)c; return -1; }
int read_jpeg_fd(int fd, int t, struct bitmap *bm, int m, int f, const struct custom_format *c, bool (*x)(int, int)) { (void)fd;(void)t;(void)bm;(void)m;(void)f;(void)c;(void)x; return -1; }
const size_t JPEG_DECODE_OVERHEAD = 0;
unsigned int create_thread(void (*fn)(void), void *st, size_t sz, unsigned fl, const char *nm IF_PRIO(, int pr) IF_COP(, unsigned int co))
{ (void)fn; (void)st; (void)sz; (void)fl; (void)nm; IF_PRIO((void)pr;) IF_COP((void)co;) return 0; }
void semaphore_init(struct semaphore *s, int m, int st) { (void)s; (void)m; (void)st; }
int semaphore_wait(struct semaphore *s, int t) { (void)s; (void)t; return 0; }
void semaphore_release(struct semaphore *s) { (void)s; }
void yield(void) {}

/* ---- just enough LVGL: boxes, images, delete events and the poll timer */
static char boxes[16], imgs[16];          /* their addresses are the objects */
static int n_imgs;
static struct { lv_obj_t *parent; const lv_image_dsc_t *src; } img_of[16];
static struct { lv_obj_t *o; lv_event_cb_t cb; void *user; } evs[64];
static int n_evs;
static lv_timer_cb_t poll_cb;
static void *cur_user;
#define BOX(i) ((lv_obj_t *)&boxes[i])
void *lv_event_get_target(lv_event_t *e) { (void)e; return NULL; }
void *lv_event_get_user_data(lv_event_t *e) { (void)e; return cur_user; }
void lv_fs_drv_init(lv_fs_drv_t *d) { memset(d, 0, sizeof *d); }
void lv_fs_drv_register(lv_fs_drv_t *d) { (void)d; }
void lv_image_cache_drop(const void *s) { (void)s; }
lv_obj_t *lv_image_create(lv_obj_t *p) { assert(n_imgs < 16); img_of[n_imgs].parent = p; return (lv_obj_t *)&imgs[n_imgs++]; }
void lv_image_set_src(lv_obj_t *o, const void *s) { img_of[(char *)o - imgs].src = s; }
lv_event_dsc_t *lv_obj_add_event_cb(lv_obj_t *o, lv_event_cb_t c, lv_event_code_t f, void *u)
{ (void)f; assert(n_evs < 64); evs[n_evs].o = o; evs[n_evs].cb = c; evs[n_evs].user = u; n_evs++; return NULL; }
bool lv_obj_remove_event_cb(lv_obj_t *o, lv_event_cb_t c) { (void)o; (void)c; return true; }
uint32_t lv_obj_remove_event_cb_with_user_data(lv_obj_t *o, lv_event_cb_t c, void *u)
{
    uint32_t n = 0;
    for (int i = 0; i < n_evs; i++)
        if (evs[i].o == o && evs[i].cb == c && evs[i].user == u) { evs[i] = evs[--n_evs]; i--; n++; }
    return n;
}
void lv_obj_add_flag(lv_obj_t *o, lv_obj_flag_t f) { (void)o; (void)f; }
void lv_obj_center(lv_obj_t *o) { (void)o; }
lv_style_value_t lv_obj_get_style_prop(const lv_obj_t *o, lv_part_t p, lv_style_prop_t q) { (void)o;(void)p;(void)q; lv_style_value_t v = {0}; return v; }
void lv_obj_set_style_clip_corner(lv_obj_t *o, bool v, lv_style_selector_t s) { (void)o;(void)v;(void)s; }
void lv_obj_set_style_radius(lv_obj_t *o, int32_t v, lv_style_selector_t s) { (void)o;(void)v;(void)s; }
lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *u) { (void)period; (void)u; poll_cb = cb; return (lv_timer_t *)&poll_cb; }
/* LVGL deleting an object: its delete callbacks run, then they are gone */
static void delete_obj(lv_obj_t *o)
{
    for (int i = 0; i < n_evs; i++)
        if (evs[i].o == o) { cur_user = evs[i].user; evs[i].cb(NULL); evs[i] = evs[--n_evs]; i--; }
}
static int images_in(lv_obj_t *box) { int n = 0; for (int i = 0; i < n_imgs; i++) n += img_of[i].parent == box; return n; }
static int watching(lv_obj_t *o) { int n = 0; for (int i = 0; i < n_evs; i++) n += evs[i].o == o; return n; }

/* ---- the card: album 0's 58-pixel thumbnail is a FIFO (a read that waits
 * until the test lets it), album 1's a real file of one colour */
static struct sl_album albums[2];
static int by_title[2];
static char real0[600], real1[600];
static void real_path(int album, char *b)
{
    char p[72]; thumb_path(p, sizeof p, album, SL_ART_ROW); snprintf(b, 600, "%s%s", root, p);
    for (char *s = b + strlen(root) + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(b, 0777); *s = '/'; }
}
static void put_thumb(const char *real, int side, uint16_t colour)
{
    lv_image_header_t h; memset(&h, 0, sizeof h);
    h.magic = LV_IMAGE_HEADER_MAGIC; h.cf = LV_COLOR_FORMAT_RGB565; h.w = side; h.h = side; h.stride = side * 2;
    FILE *f = fopen(real, "wb"); assert(f);
    fwrite(&h, sizeof h, 1, f);
    for (int i = 0; i < side * side; i++) fwrite(&colour, 2, 1, f);
    fclose(f);
}
static int jobs_in(int state) { int n = 0; for (int i = 0; i < SC_JOBS; i++) n += __atomic_load_n(&sc_jobs[i].state, __ATOMIC_ACQUIRE) == state; return n; }
/* the poll, once every job a thread has is back */
static void poll_when_done(void)
{
    for (int w = 0; jobs_in(SC_WORKING) && w < 2500; w++) usleep(2000);
    assert(!jobs_in(SC_WORKING));
    assert(poll_cb);
    poll_cb(NULL);
}
static long now_ms(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec * 1000L + t.tv_usec / 1000; }
static void *let_fifo_go(void *arg)
{
    usleep((useconds_t)(intptr_t)arg * 1000);
    /* every reader waiting on it opens and reads nothing (no picture); a
     * reader woken late may miss one writer, so open again until none waits */
    for (int i = 0; i < 1000 && __atomic_load_n(&sc_working, __ATOMIC_ACQUIRE) > 0; i++)
    {
        int fd = open(real0, O_WRONLY | O_NONBLOCK);
        usleep(5000);
        if (fd >= 0) close(fd);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    snprintf(root, sizeof root, "%s", argc > 1 ? argv[1] : ".");
    albums[0].title = "Waiting"; albums[0].artist = "Fifo";
    albums[1].title = "Ready"; albums[1].artist = "File";
    sl_lib.albums = albums; sl_lib.n_albums = 2; sl_lib.albums_by_title = by_title;
    real_path(0, real0); real_path(1, real1);
    unlink(real0); assert(!mkfifo(real0, 0666));
    put_thumb(real1, SL_ART_ROW, 0x1234);

    /* 1: made on the thread, in its box at the next poll */
    assert(sl_art_apply_scaled_async(BOX(0), 1, SL_ART_ROW));
    assert(images_in(BOX(0)) == 0 && watching(BOX(0)) == 1);   /* not yet: the thread has it */
    poll_when_done();
    assert(images_in(BOX(0)) == 1 && watching(BOX(0)) == 0);
    const lv_image_dsc_t *d = img_of[n_imgs - 1].src;
    assert(d && d->header.w == SL_ART_ROW && d->header.h == SL_ART_ROW && d->data_size == SL_ART_ROW * SL_ART_ROW * 2);
    for (int i = 0; i < SL_ART_ROW * SL_ART_ROW; i++) assert(((const uint16_t *)d->data)[i] == 0x1234);
    assert(sl_art_scaled_jobs_made == 1 && jobs_in(SC_FREE) == SC_JOBS);
    delete_obj((lv_obj_t *)&imgs[n_imgs - 1]);                  /* the tile goes: scaled_deleted frees it */
    printf("PASS: a live tile's cover is read and scaled on a thread and lands in its box at the next poll\n");

    /* 2: the box goes first: nothing lands, the picture is freed */
    assert(sl_art_apply_scaled_async(BOX(1), 1, SL_ART_ROW));
    delete_obj(BOX(1));
    poll_when_done();
    assert(images_in(BOX(1)) == 0 && sl_art_scaled_jobs_made == 1 && jobs_in(SC_FREE) == SC_JOBS);
    printf("PASS: a box deleted before its cover comes gets nothing, and the picture is freed\n");

    /* 3: every slot held by a read that waits; the next is made at once */
    for (int i = 0; i < SC_JOBS; i++) assert(sl_art_apply_scaled_async(BOX(2 + i), 0, SL_ART_ROW));
    usleep(50000);
    assert(jobs_in(SC_WORKING) == SC_JOBS && __atomic_load_n(&sc_working, __ATOMIC_ACQUIRE) == SC_JOBS);
    int before = n_imgs;
    assert(sl_art_apply_scaled_async(BOX(8), 1, SL_ART_ROW));
    assert(n_imgs == before + 1 && images_in(BOX(8)) == 1 && watching(BOX(8)) == 0);   /* here, now */
    delete_obj((lv_obj_t *)&imgs[n_imgs - 1]);
    printf("PASS: with every slot busy, the next cover is made at once as before\n");

    /* 4: USB mode waits for the reads under way (here 300 ms), and 5: those
     * reads get nothing, so their boxes stay empty and the slots come free */
    pthread_t th;
    assert(!pthread_create(&th, NULL, let_fifo_go, (void *)(intptr_t)300));
    long t0 = now_ms();
    sc_settle();
    long waited = now_ms() - t0;
    pthread_join(th, NULL);
    printf("USB mode waited %ld ms for %d reads\n", waited, SC_JOBS);
    assert(waited >= 250 && waited < 2000 && __atomic_load_n(&sc_working, __ATOMIC_ACQUIRE) == 0);
    assert(jobs_in(SC_FAILED) == SC_JOBS);
    poll_cb(NULL);
    for (int i = 0; i < SC_JOBS; i++) assert(images_in(BOX(2 + i)) == 0 && watching(BOX(2 + i)) == 0);
    assert(jobs_in(SC_FREE) == SC_JOBS && sl_art_scaled_jobs_made == 1);
    t0 = now_ms(); sc_settle(); assert(now_ms() - t0 < 50);   /* nothing under way: no wait */
    printf("PASS: USB mode waits for a cover read under way; an unreadable thumbnail fills no box and frees its slot\n");

    /* 6: a missing album or a silly size: refused, no thread */
    assert(!sl_art_apply_scaled_async(BOX(9), 2, SL_ART_ROW) && !sl_art_apply_scaled_async(BOX(9), 1, 0)
           && !sl_art_apply_scaled_async(BOX(9), -1, SL_ART_ROW) && jobs_in(SC_FREE) == SC_JOBS && watching(BOX(9)) == 0);
    unlink(real0);
    printf("PASS: live-tile covers off the UI loop: all cases\n");
    return 0;
}
