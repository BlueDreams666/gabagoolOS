/* "Set as wallpaper"'s maker (sl_wall_start in sloppy_art.c) on the Mac,
 * with the real stb_image: pictures of other shapes are cut from their
 * middle to exactly 480 x 800, a small one is enlarged, a bright one is
 * dimmed for the white words over it, a file that is no picture fails,
 * and the result lands in place whole. Build like art_miss_test.c (sim
 * include flags, no other sources), or run run-art-wall-test.sh. Run with
 * a folder of test pictures: wide.png tall.png small.png white.png
 * photo.jpg (the script makes them). */
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
static char root[256], cover[512];
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

static uint16_t out[480 * 800];
static int make(const char *name, const char *dest)
{
    char in[600]; snprintf(in, sizeof in, "/pics/%s", name);
    assert(sl_wall_start(in, dest, 480, 800) == SL_PIC_STARTED);
    int r, polls = 0;
    while (!(r = sl_wall_poll())) { usleep(2000); polls++; }
    if (r < 0) return r;
    char real[600]; snprintf(real, sizeof real, "%s%s", root, dest);
    FILE *f = fopen(real, "rb"); assert(f);
    size_t n = fread(out, 2, 480 * 800, f); int extra = fgetc(f); fclose(f);
    assert(n == 480 * 800 && extra == EOF);   /* exactly the screen, nothing after */
    return 1;
}
/* a boot logo made from /pics/name: its width and height, its pixels in out */
static int logo(const char *name, int *w, int *h)
{
    char in[600]; snprintf(in, sizeof in, "/pics/%s", name);
    const char *dest = "/.rockbox/sloppy/bootlogo.rgb565";
    assert(sl_bootlogo_start(in, dest, 480, 800) == SL_PIC_STARTED);
    int r;
    while (!(r = sl_wall_poll())) usleep(2000);
    if (r < 0) return r;
    char real[600]; snprintf(real, sizeof real, "%s%s", root, dest);
    FILE *f = fopen(real, "rb"); assert(f);
    unsigned char hd[8]; assert(fread(hd, 1, 8, f) == 8 && !memcmp(hd, "SLBL", 4));
    *w = hd[4] | hd[5] << 8; *h = hd[6] | hd[7] << 8;
    size_t n = fread(out, 2, (size_t)*w * *h, f); int extra = fgetc(f); fclose(f);
    assert(n == (size_t)*w * *h && extra == EOF);
    return 1;
}
static int count(unsigned (*ch)(uint16_t), unsigned lo)
{
    int n = 0; for (int i = 0; i < 480 * 800; i++) n += ch(out[i]) >= lo; return n;
}
static unsigned red(uint16_t c) { return c >> 11; }
static unsigned green(uint16_t c) { return (c >> 5) & 63; }
static unsigned blue(uint16_t c) { return c & 31; }
int main(int argc, char **argv)
{
    snprintf(root, sizeof root, "%s", argc > 1 ? argv[1] : ".");
    char b[700]; snprintf(b, sizeof b, "%s/.rockbox/sloppy", root); mkdir(b, 0777);
    const char *dest = "/.rockbox/sloppy/wallpaper.rgb565";
    /* 1600 x 900, red in the middle 540 columns (the screen's shape at that
     * height), blue outside: only red may survive the cut */
    assert(make("wide.png", dest) == 1);
    assert(count(red, 20) == 480 * 800 && count(blue, 8) == 0);
    /* 900 x 3000, green in the middle 1500 rows, blue above and below */
    assert(make("tall.png", dest) == 1);
    assert(count(green, 24) == 480 * 800 && count(blue, 8) == 0);   /* dimmed a little: bright */
    /* 120 x 90: enlarged to fill, its left half dark, right half light, so the
     * middle 54 columns of it (the cut) split down the screen's middle */
    assert(make("small.png", dest) == 1);
    assert(green(out[400 * 480 + 100]) < 20 && green(out[400 * 480 + 380]) > 40);
    /* a white picture: dimmed to 55 %, so white words still read */
    assert(make("white.png", dest) == 1);
    unsigned g = green(out[400 * 480 + 240]);
    printf("white picture kept at %u/63 green\n", g);
    assert(g >= 32 && g <= 36);
    /* a photo, as a JPEG */
    assert(make("photo.jpg", dest) == 1);
    long sum = 0; for (int i = 0; i < 480 * 800; i++) sum += green(out[i]);
    assert(sum > 480 * 800);
    /* not a picture: fails, and the wallpaper there stays as it was */
    uint16_t keep = out[1234];
    assert(make("notes.txt", dest) == -1);
    char real[600]; snprintf(real, sizeof real, "%s%s", root, dest);
    FILE *f = fopen(real, "rb"); assert(f && fread(out, 2, 480 * 800, f) == 480 * 800); fclose(f);
    assert(out[1234] == keep);
    snprintf(real, sizeof real, "%s%s.new", root, dest); assert(access(real, F_OK));   /* nothing half-made left */
    printf("PASS: Set as wallpaper: wide, tall, small, white and a JPEG cut to 480 x 800 from the middle; not a picture refused\n");
    /* boot logos: the whole picture, shrunk to fit and never enlarged, undimmed */
    int w, h;
    assert(logo("wide.png", &w, &h) == 1 && w == 480 && h == 270);   /* 1600 x 900: all of it, blue edges kept */
    assert(blue(out[135 * 480 + 5]) > 24 && red(out[135 * 480 + 240]) > 24);
    assert(logo("tall.png", &w, &h) == 1 && w == 240 && h == 800);   /* 900 x 3000 */
    assert(logo("small.png", &w, &h) == 1 && w == 120 && h == 90);    /* as it is */
    assert(green(out[45 * 120 + 10]) < 10 && green(out[45 * 120 + 110]) > 50);
    assert(logo("white.png", &w, &h) == 1 && w == 400 && h == 300);
    for (int i = 0; i < w * h; i++) assert(out[i] == 0xffff);           /* not dimmed */
    assert(logo("notes.txt", &w, &h) == -1);
    printf("PASS: boot logo: 1600 x 900 to 480 x 270, 900 x 3000 to 240 x 800, 120 x 90 kept as it is, white kept white, not a picture refused\n");
    return 0;
}
