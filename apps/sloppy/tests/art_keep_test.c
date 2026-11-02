/* Build like art_miss_test.c (the sim include flags, this file in place of
 * ui_test.c, no other sources). Run: ./art-keep-test
 *
 * Covers survive a library update. Every database update (the one after
 * USB mode too) starts a new thumbnail generation, and each album's covers
 * move into it when their art source is unchanged. On the R1 none did:
 * the source was checked with the C library's stat(), which does not know
 * the card is under PIVOT_ROOT, so no cover had a stamp and all ~1700 were
 * made again after each update (17 minutes of work and battery).
 *
 * Here the library's paths are the card's ("/music/..."), found only
 * through the file layer (sim_open), as on the R1. Checked: a library of
 * covers, an update that changes nothing remakes none; new art in one
 * song remakes that album alone; a gone album's covers are pruned; a song
 * added to an album keeps its cover. */
#include "sloppy_art.c"
#include "test_paths.h"
const char *volatile sl_at_where;
#undef assert
#define assert(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include <stdio.h>
#include <sys/time.h>
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
/* the card: our folder and the music, under root; nothing else is there */
static const char *at(const char *p, char *b)
{
    if (!strncmp(p, "/.rockbox", 9) || !strncmp(p, "/music", 6)) { snprintf(b, 512, "%s%s", root, p); return b; }
    return p;
}
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
bool sim_dir_exists(const char *p) { char b[512]; struct stat st; return !stat(at(p,b), &st) && S_ISDIR(st.st_mode); }
static int count_files(const char *p)
{
    char b[512], c[1024]; DIR *d = opendir(at(p,b)); int n = 0; struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        DIR *sub = opendir(at(c,b)); if (sub) { closedir(sub); n += count_files(c); } else n++; }
    closedir(d); return n;
}

/* Every song has a JPEG in it; "decoding" one is counted, per song */
struct sl_lib sl_lib;
void sl_log(const char *f, ...) { (void)f; }
static int decodes;
bool get_metadata(struct mp3entry *id3, int fd, const char *path)
{
    (void)fd; (void)path;
    id3->has_embedded_albumart = true;
    id3->albumart.type = AA_TYPE_JPG; id3->albumart.pos = 0; id3->albumart.size = 1;
    return true;
}
bool search_albumart_files(const struct mp3entry *id3, const char *s, char *b, int n) { (void)id3; (void)s; (void)b; (void)n; return false; }
int clip_jpeg_fd(int fd, int t, unsigned long s, struct bitmap *bm, int m, int f, const struct custom_format *c, bool (*x)(int, int))
{
    (void)fd;(void)t;(void)s;(void)m;(void)f;(void)c;(void)x;
    decodes++;
    bm->width = bm->height = 8;
    memset(bm->data, 0x5a, 8 * 8 * 2);
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
/* no cover thread here: the UI makes them (create_thread fails) */
unsigned int create_thread(void (*fn)(void), void *st, size_t sz, unsigned fl, const char *nm IF_PRIO(, int pr) IF_COP(, unsigned int co))
{ (void)fn; (void)st; (void)sz; (void)fl; (void)nm; IF_PRIO((void)pr;) IF_COP((void)co;) return 0; }
void semaphore_init(struct semaphore *s, int m, int st) { (void)s; (void)m; (void)st; }
int semaphore_wait(struct semaphore *s, int t) { (void)s; (void)t; return 0; }
void semaphore_release(struct semaphore *s) { (void)s; }
void yield(void) {}
unsigned sleep(unsigned t) { (void)t; return 0; }

/* A library as sl_lib builds it: album a has songs /music/<a>/<n>.mp3.
 * Each build lists the albums in another order (indices are not kept
 * across a database update; only artist and title are). */
enum { N = 40, MAX_SONGS = 3 };
static struct sl_track tracks[N * MAX_SONGS];
static struct sl_album albums[N];
static int songs_of[N][MAX_SONGS], by_title[N], n_songs[N];
static char paths[N * MAX_SONGS][64], titles[N][16];
static bool gone[N];
static void song_file(int a, int n, char *b)
{
    char p[64]; snprintf(p, sizeof p, "/music/%02d/%d.mp3", a, n); at(p, b);
}
static void build_library(int rotate)
{
    int na = 0, nt = 0;
    for (int k = 0; k < N; k++)
    {
        int a = (k + rotate) % N;
        if (gone[a]) continue;
        struct sl_album *al = &albums[na];
        memset(al, 0, sizeof *al);
        snprintf(titles[a], sizeof titles[a], "Album %02d", a);
        al->title = titles[a]; al->artist = "Artist"; al->tracks = songs_of[na];
        for (int n = 0; n < n_songs[a]; n++)
        {
            struct sl_track *t = &tracks[nt];
            memset(t, 0, sizeof *t);
            snprintf(paths[nt], sizeof paths[nt], "/music/%02d/%d.mp3", a, n);
            tpath(t, paths[nt]); t->title = "song"; t->album = na; t->num = n + 1;
            t->added = (uint32_t)(1000 + rotate);   /* the database's own numbers change too */
            al->tracks[al->n_tracks++] = nt++;
        }
        by_title[na] = na;
        na++;
    }
    sl_lib.tracks = tracks; sl_lib.n_tracks = nt;
    sl_lib.albums = albums; sl_lib.n_albums = na; sl_lib.albums_by_title = by_title;
}
/* the art step the UI loop runs, until it has nothing left to do */
static void settle(void)
{
    sl_art_reset();
    int steps = 0;
    while (sl_art_step() && steps < 20000) steps++;
    assert(steps < 20000 && !thumb_prune_pending);
}
/* db_update_reload: a new generation, the library read again */
static int update(int rotate)
{
    int before = decodes;
    sl_art_invalidate_cache();
    build_library(rotate);
    settle();
    return decodes - before;
}
static bool all_covers(void)
{
    for (int a = 0; a < sl_lib.n_albums; a++)
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        { char p[72]; thumb_path(p, sizeof p, a, sizes[i]); if (!sim_file_exists(p)) return false; }
    return true;
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/art-keep-%d", (int)getpid());
    char b[512];
    mkdir(root, 0777);
    snprintf(b, sizeof b, "%s/.rockbox", root); mkdir(b, 0777);
    snprintf(b, sizeof b, "%s/music", root); mkdir(b, 0777);
    for (int a = 0; a < N; a++)
    {
        snprintf(b, sizeof b, "%s/music/%02d", root, a); mkdir(b, 0777);
        n_songs[a] = 2;
        for (int n = 0; n < n_songs[a]; n++)
        { song_file(a, n, b); FILE *f = fopen(b, "w"); fputs("song", f); fclose(f); }
    }
    /* the library's paths are the card's: the C library cannot find them */
    struct stat st;
    assert(stat("/music/00/0.mp3", &st) != 0 && card_stat("/music/00/0.mp3", &st) == 0);

    sl_art_init();
    build_library(0);
    settle();
    assert(decodes == N && all_covers());           /* first time: every album made */
    const int per_album = (int)(sizeof sizes / sizeof sizes[0]);
    assert(count_files(THUMB_DIR) == N * per_album + 2);   /* + generation, pruned */

    /* a database update that changed nothing: no cover made again */
    assert(update(7) == 0);
    assert(all_covers() && count_files(THUMB_DIR) == N * per_album + 2);
    assert(update(13) == 0);                        /* and the one after */
    sl_art_init();                                  /* a boot: nothing to do */
    assert(!thumb_prune_pending);
    build_library(13); settle();
    assert(decodes == N);

    /* new art in one song (tags rewritten): that album alone is made again */
    song_file(5, 0, b);
    { FILE *f = fopen(b, "a"); fputs("new art", f); fclose(f); }
    struct timeval tv[2] = {{1500000000, 0}, {1500000000, 0}}; utimes(b, tv);
    assert(update(21) == 1);
    assert(all_covers());
    assert(update(22) == 0);

    /* a song added to an album, a .DS_Store left in another's folder: kept */
    n_songs[9] = 3;
    song_file(9, 2, b); { FILE *f = fopen(b, "w"); fputs("song", f); fclose(f); }
    snprintf(b, sizeof b, "%s/music/11/.DS_Store", root); { FILE *f = fopen(b, "w"); fputs("x", f); fclose(f); }
    assert(update(23) == 0);

    /* a gone album's covers go with the prune */
    gone[3] = true;
    assert(update(24) == 0 && all_covers());
    assert(count_files(THUMB_DIR) == (N - 1) * per_album + 2);

    printf("PASS: art keep: a library update remakes no unchanged cover (%d albums)\n", N);
    printf("PASS: art keep: new art in one song remakes that album alone; gone albums pruned\n");
    snprintf(b, sizeof b, "rm -rf %s", root); system(b);
    return 0;
}
