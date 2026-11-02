/* Build like ui_test.c (the sim include flags) with this file in place of
 * ui_test.c and no other sources: it includes sloppy_art.c itself. */
/* sloppy_art.c's miss record against a real directory; decoders are stubbed */
#include "sloppy_art.c"
#include "test_paths.h"
const char *volatile sl_at_where;   /* the hang watchdog's breadcrumb, in sloppy.c */
#undef assert
#define assert(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
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
static char root[256];
const char *sl_net_card_root(void) { return root; }   /* "Set as wallpaper" reads the card by its real path */
static const char *at(const char *p, char *b) { if (!strncmp(p, "/.rockbox", 9)) { snprintf(b, 512, "%s%s", root, p); return b; } return p; }
static int slow_us;   /* each card lookup this slow, as on the R1 (about 4 ms) */
int sim_open(const char *p, int f, ...) { char b[512]; if (slow_us) usleep(slow_us); return open(at(p,b), f, 0666); }
int sim_close(int fd) { return close(fd); }
ssize_t sim_read(int fd, void *b, size_t n) { return read(fd, b, n); }
ssize_t sim_write(int fd, const void *b, size_t n) { return write(fd, b, n); }
off_t sim_lseek(int fd, off_t o, int w) { return lseek(fd, o, w); }
int sim_fsync(int fd) { return fsync(fd); }
int sim_remove(const char *p) { char b[512]; return remove(at(p,b)); }
int sim_rename(const char *a, const char *c) { char b[512], d[512]; return rename(at(a,b), at(c,d)); }
int sim_mkdir(const char *p) { char b[512]; return mkdir(at(p,b), 0777); }
bool sim_file_exists(const char *p) { char b[512]; struct stat st; if (slow_us) usleep(slow_us); return !stat(at(p,b), &st); }
#undef opendir
#undef readdir
#undef closedir
#undef rmdir
#undef dirent
DIR *sim_opendir(const char *p) { char b[512]; return opendir(at(p,b)); }
struct sim_dirent *sim_readdir(DIR *d)
{
    static struct sim_dirent e; struct dirent *h = readdir(d);
    if (!h) return NULL;
    memset(&e, 0, sizeof e); strlcpy(e.d_name, h->d_name, sizeof e.d_name); return &e;
}
int sim_closedir(DIR *d) { return closedir(d); }
int sim_rmdir(const char *p) { char b[512]; return rmdir(at(p,b)); }
bool sim_dir_exists(const char *p) { char b[512]; struct stat st; if (slow_us) usleep(slow_us); return !stat(at(p,b), &st) && S_ISDIR(st.st_mode); }
static int count_files(const char *p)   /* under p, recursively */
{
    char b[512], c[1024]; DIR *d = opendir(at(p,b)); int n = 0; struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        DIR *sub = opendir(at(c,b)); if (sub) { closedir(sub); n += count_files(c); } else n++; }
    closedir(d); return n;
}
static void put_thumb(const char *path, int size, const char *track)   /* a stamped cover, as write_thumb makes */
{
    lv_image_header_t hdr; memset(&hdr, 0, sizeof hdr); hdr.magic = LV_IMAGE_HEADER_MAGIC; hdr.cf = LV_COLOR_FORMAT_RGB565;
    hdr.w = hdr.h = size; hdr.stride = size * 2; static uint16_t px[SL_ART_GRID * SL_ART_GRID];
    struct art_stamp st; assert(stamp_source(&st, track));
    int fd = sim_open(path, O_WRONLY | O_CREAT | O_TRUNC); assert(fd >= 0);
    assert(write(fd, &hdr, sizeof hdr) == sizeof hdr && write(fd, px, (size_t)size * size * 2) == (ssize_t)((size_t)size * size * 2) &&
           write(fd, &st, sizeof st) == sizeof st);
    close(fd);
}
struct sl_lib sl_lib;
void sl_log(const char *f, ...) { (void)f; }
static int metadata_reads;
bool get_metadata(struct mp3entry *id3, int fd, const char *path) { (void)id3; (void)fd; (void)path; metadata_reads++; return false; }
bool search_albumart_files(const struct mp3entry *id3, const char *s, char *b, int n) { (void)id3; (void)s; (void)b; (void)n; return false; }
int clip_jpeg_fd(int fd, int t, unsigned long s, struct bitmap *bm, int m, int f, const struct custom_format *c, bool (*x)(int, int)) { (void)fd;(void)t;(void)s;(void)bm;(void)m;(void)f;(void)c;(void)x; return -1; }
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
/* no cover thread here: the UI makes them (create_thread fails) */
unsigned int create_thread(void (*fn)(void), void *st, size_t sz, unsigned fl, const char *nm IF_PRIO(, int pr) IF_COP(, unsigned int co))
{ (void)fn; (void)st; (void)sz; (void)fl; (void)nm; IF_PRIO((void)pr;) IF_COP((void)co;) return 0; }
void semaphore_init(struct semaphore *s, int m, int st) { (void)s; (void)m; (void)st; }
int semaphore_wait(struct semaphore *s, int t) { (void)s; (void)t; return 0; }
void semaphore_release(struct semaphore *s) { (void)s; }
void yield(void) {}
unsigned sleep(unsigned t) { (void)t; return 0; }

static int run(void) { int before = metadata_reads; sl_art_reset(); for (int i = 0; i < 20 && sl_art_step(); i++) {} return metadata_reads - before; }
int main(void)
{
    snprintf(root, sizeof root, "/tmp/art-miss-%d", (int)getpid());
    char b[512]; mkdir(root, 0777); snprintf(b, sizeof b, "%s/.rockbox", root); mkdir(b, 0777);
    char dir[512], track[600]; snprintf(dir, sizeof dir, "%s/music", root); mkdir(dir, 0777);
    snprintf(track, sizeof track, "%s/a.flac", dir); FILE *f = fopen(track, "w"); fputs("x", f); fclose(f);
    static struct sl_track t; static struct sl_album al; static int idx[1] = {0}, by_title[1] = {0};
    tpath(&t, track); t.title = "a"; al.title = "Album"; al.artist = "Artist"; al.n_tracks = 1; al.tracks = idx;
    sl_lib.tracks = &t; sl_lib.n_tracks = 1; sl_lib.albums = &al; sl_lib.n_albums = 1; sl_lib.albums_by_title = by_title;
    sl_art_init();
    assert(run() == 1);                 /* first session: the song is read */
    assert(run() == 0);                 /* nothing changed: not read again */
    assert(run() == 0);
    struct timeval tv[2] = {{1000000000, 0}, {1000000000, 0}}; utimes(dir, tv);   /* a cover dropped in the folder */
    assert(run() == 1);
    assert(run() == 0);
    f = fopen(track, "a"); fputs("art", f); fclose(f);   /* art embedded into the song */
    assert(run() == 1);
    assert(run() == 0);
    /* A library whose covers are all made, on a slow card: the walk past
     * them must give the UI loop back often. 24 albums a step held the
     * R1's loop 404 ms. */
    enum { N = 120 };
    static struct sl_album many[N]; static int order[N];
    for (int a = 0; a < N; a++) { many[a] = al; order[a] = a; }
    sl_lib.albums = many; sl_lib.n_albums = N; sl_lib.albums_by_title = order;
    for (int a = 0; a < N; a++)
    {
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        { char path[72], dir[72]; thumb_path(path, sizeof path, a, sizes[i]); strlcpy(dir, path, sizeof dir);
          *strrchr(dir, '/') = '\0'; sim_mkdir(dir); close(sim_open(path, O_WRONLY | O_CREAT)); }
    }
    sl_art_reset(); slow_us = 4000;
    long worst = 0; int steps = 0; bool more = true;
    while (more && steps < 1000)
    {
        struct timeval s0, s1; gettimeofday(&s0, NULL);
        more = sl_art_step(); steps++;
        gettimeofday(&s1, NULL);
        long ms = (s1.tv_sec - s0.tv_sec) * 1000L + (s1.tv_usec - s0.tv_usec) / 1000;
        if (ms > worst) worst = ms;
    }
    slow_us = 0;
    assert(!more);                      /* the walk finishes */
    assert(worst < 60);                 /* 20 ms a step, plus at most one album's lookups */
    /* Cover Flow asks for a cover in the middle of a verify pass that has
     * already used the step's time: it is still made in that step, never
     * left on screen as a placeholder */
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    { char path[72], b2[512]; thumb_path(path, sizeof path, 77, sizes[i]); remove(at(path, b2)); }
    sl_art_reset(); thumb_prune_pending = true; slow_us = 4000;
    sl_art_prefer(77);
    sl_art_step();
    slow_us = 0; thumb_prune_pending = false;
    assert(attempted_count == N && attempted[77]);
    /* A finished prune is remembered. Every boot used to check every album
     * and read the whole thumbnail folder again (402 ms on one album, each
     * boot, in the R1's log). */
    sl_art_invalidate_cache();              /* new covers: check and prune once */
    assert(thumb_prune_pending);
    sl_art_reset();
    for (int i = 0; i < 2000 && thumb_prune_pending; i++) sl_art_step();
    assert(!thumb_prune_pending);
    sl_art_init();                          /* the next boot: nothing to do */
    assert(!thumb_prune_pending);
    sl_art_invalidate_cache(); sl_art_init();   /* newer covers since: once more */
    assert(thumb_prune_pending);
    {   /* A flat generation from an older build: its covers move into the new
         * generation's folders, not made again; everything else flat goes */
        sl_lib.albums = &al; sl_lib.n_albums = 1; sl_lib.albums_by_title = by_title;
        snprintf(b, sizeof b, "rm -rf %s/.rockbox/sloppy/thumbs && mkdir -p %s/.rockbox/sloppy/thumbs", root, root); system(b);
        uint32_t g = 7; int fd = sim_open(THUMB_GENERATION, O_WRONLY | O_CREAT | O_TRUNC); assert(write(fd, &g, 4) == 4); close(fd);
        char p[72];
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        { thumb_path_at(p, sizeof p, 7, false, 0, sizes[i]); put_thumb(p, sizes[i], track); }
        thumb_path_at(p, sizeof p, 5, false, 0, SL_ART_GRID); put_thumb(p, SL_ART_GRID, track);   /* two generations old */
        strlcat(p, ".tmp", sizeof p); close(sim_open(p, O_WRONLY | O_CREAT));                   /* a write cut short */
        assert(count_files(THUMB_DIR) == 6);
        sl_art_init();
        assert(thumb_generation == 8 && !thumb_prev_sharded && thumb_prune_pending && sim_dir_exists(THUMB_DIR "/g00000008"));
        int reads = metadata_reads;
        sl_art_reset(); for (int i = 0; i < 200 && thumb_prune_pending; i++) sl_art_step();
        assert(!thumb_prune_pending && metadata_reads == reads);   /* moved, not decoded again */
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        { thumb_path(p, sizeof p, 0, sizes[i]); assert(sim_file_exists(p) && strstr(p, "/g00000008/")); }
        assert(count_files(THUMB_DIR) == 3 + 2);   /* the covers, generation, pruned */
        /* the next refresh: from folders into folders; the old one is removed */
        sl_art_invalidate_cache(); sl_art_init();
        assert(thumb_generation == 9 && thumb_prev_sharded && thumb_prune_pending);
        sl_art_reset(); for (int i = 0; i < 200 && thumb_prune_pending; i++) sl_art_step();
        assert(!thumb_prune_pending && metadata_reads == reads && !sim_dir_exists(THUMB_DIR "/g00000008"));
        thumb_path(p, sizeof p, 0, SL_ART_GRID); assert(sim_file_exists(p));
        sl_art_init(); assert(!thumb_prune_pending && thumb_generation == 9);   /* a boot after: nothing to do */
        /* nothing but legacy files and no generation: start in folders */
        snprintf(b, sizeof b, "rm -rf %s/.rockbox/sloppy/thumbs && mkdir -p %s/.rockbox/sloppy/thumbs", root, root); system(b);
        thumb_generation = 0; thumb_path_at(p, sizeof p, 0, false, 0, SL_ART_GRID); close(sim_open(p, O_WRONLY | O_CREAT));
        sl_art_init(); assert(thumb_generation == 1 && sim_dir_exists(THUMB_DIR "/g00000001"));
        sl_art_reset(); for (int i = 0; i < 200 && thumb_prune_pending; i++) sl_art_step();
        assert(!thumb_prune_pending && !sim_file_exists(p));
    }
    printf("PASS: flat thumbnails move into per-generation folders; old generations pruned\n");
    printf("PASS: art miss record: read once, again only after the folder or song changes\n");
    printf("PASS: a walk past %d made covers on a 4 ms card: %d steps, worst %ld ms\n", N, steps, worst);
    snprintf(b, sizeof b, "rm -rf %s", root); system(b);
    return 0;
}
