/* Build like art_miss_test.c (sim include flags, no other sources). Run with
 * a progressive JPEG: ./art-stb-test cover.jpg; STB_SURVEY=1 prints made/skipped. */
/* sloppy_art.c with Rockbox's decoders failing: a progressive cover.jpg
 * must end as thumbnails through stb_image, not as a miss */
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
static char root[256], cover[512];
const char *sl_net_card_root(void) { return root; }   /* "Set as wallpaper" reads the card by its real path */
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
void sl_log(const char *f, ...) { va_list ap; va_start(ap, f); vfprintf(stderr, f, ap); va_end(ap); fputc('\n', stderr); }
bool get_metadata(struct mp3entry *id3, int fd, const char *path) { (void)id3; (void)fd; (void)path; return true; }
bool search_albumart_files(const struct mp3entry *id3, const char *s, char *b, int n) { (void)id3; (void)s; snprintf(b, n, "%s", cover); return true; }
int clip_jpeg_fd(int fd, int t, unsigned long s, struct bitmap *bm, int m, int f, const struct custom_format *c, bool (*x)(int, int)) { (void)fd;(void)t;(void)s;(void)bm;(void)m;(void)f;(void)c;(void)x; return -1; }
int clip_png_fd(int fd, int p, int s, struct bitmap *bm, int m, int f) { (void)fd;(void)p;(void)s;(void)bm;(void)m;(void)f; return -1; }
int read_png_fd(int fd, struct bitmap *bm, int m, int f) { (void)fd;(void)bm;(void)m;(void)f; return -1; }
int read_bmp_fd(int fd, struct bitmap *bm, int m, int f, const struct custom_format *c) { (void)fd;(void)bm;(void)m;(void)f;(void)c; return -1; }
int read_jpeg_fd(int fd, int t, struct bitmap *bm, int m, int f, const struct custom_format *c, bool (*x)(int, int)) { (void)fd;(void)t;(void)bm;(void)m;(void)f;(void)c;(void)x; return -4; }   /* progressive: not implemented */
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
int main(int argc, char **argv)
{
    snprintf(root, sizeof root, "/tmp/art-stb-%d", (int)getpid());
    char b[512]; mkdir(root, 0777); snprintf(b, sizeof b, "%s/.rockbox", root); mkdir(b, 0777);
    snprintf(cover, sizeof cover, "%s", argv[1]);
    static char track[600]; snprintf(track, sizeof track, "%s/a.flac", root); FILE *f = fopen(track, "w"); fputs("x", f); fclose(f);
    static struct sl_track t; static struct sl_album al; static int idx[1] = {0}, by_title[1] = {0};
    tpath(&t, track); t.title = "a"; al.title = "Album"; al.artist = "Artist"; al.n_tracks = 1; al.tracks = idx;
    sl_lib.tracks = &t; sl_lib.n_tracks = 1; sl_lib.albums = &al; sl_lib.n_albums = 1; sl_lib.albums_by_title = by_title;
    sl_art_init(); sl_art_reset();
    struct timeval a0, a1; gettimeofday(&a0, NULL);
    int steps = 0; bool more = true;
    while (more && steps < 2000) { more = sl_art_step(); steps++; if (sj_state() == 1) usleep(2000); }
    gettimeofday(&a1, NULL);
    const char *src = sl_art_src(0, SL_ART_GRID);
    if (getenv("STB_SURVEY")) { printf("%s\n", src ? "made" : "skipped"); snprintf(b, sizeof b, "rm -rf %s", root); system(b); return 0; }
    assert(src);
    int fd = sim_open(src + 2, O_RDONLY); lv_image_header_t hdr; assert(read(fd, &hdr, sizeof hdr) == sizeof hdr);
    static uint16_t px[SL_ART_GRID * SL_ART_GRID]; assert(read(fd, px, (size_t)hdr.w * hdr.h * 2) == (ssize_t)((size_t)hdr.w * hdr.h * 2)); close(fd);
    long sum = 0; for (int i = 0; i < hdr.w * hdr.h; i++) sum += px[i] >> 11;
    assert(hdr.w > 100 && hdr.h > 100 && sum > hdr.w * hdr.h);   /* a picture, not a black square */
    char miss[600]; snprintf(miss, sizeof miss, "%s/.rockbox/sloppy/thumbs", root);
    printf("PASS: progressive cover decoded in the background: %ux%u thumbnail, %ld ms, %d steps\n",
           (unsigned)hdr.w, (unsigned)hdr.h, (a1.tv_sec - a0.tv_sec) * 1000L + (a1.tv_usec - a0.tv_usec) / 1000, steps);
    snprintf(b, sizeof b, "rm -rf %s", root); system(b);
    return 0;
}
