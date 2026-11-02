/* GabagoolOS screenshots, the plain-POSIX half: see sloppy_png.h. The PNG
 * encoder and decoder are lodepng, the copy already in LVGL's tree, built
 * here with its own allocators (LV_USE_LODEPNG stays 0: LVGL does not
 * decode PNGs anywhere else, and its image cache never sees these). */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#include "lvgl.h"
#undef LV_USE_LODEPNG
#define LV_USE_LODEPNG 1
#define LODEPNG_NO_COMPILE_DISK
#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#define LODEPNG_NO_COMPILE_ERROR_TEXT
#define LODEPNG_NO_COMPILE_CPP
#define LODEPNG_NO_COMPILE_ALLOCATORS
#include "../../lib/lvgl/src/libs/lodepng/lodepng.c"
#include "sloppy_net.h"
#include "sloppy_png.h"

void *lodepng_malloc(size_t size) { return malloc(size); }
void *lodepng_realloc(void *ptr, size_t new_size) { return realloc(ptr, new_size); }
void lodepng_free(void *ptr) { free(ptr); }

static pthread_mutex_t shot_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t shot_thread;
static bool shot_running, shot_joinable;
static int shot_state = SL_SHOT_IDLE;
static char shot_path[256];                  /* as Rockbox names it */
static uint16_t *shot_px;
static int shot_w, shot_h;
static char shot_name[160];
static unsigned char *last_png;              /* the last one written, for the remote */
static size_t last_size;
static unsigned last_seq;

static bool shot_real(char *out, size_t n, const char *rb)
{
    const char *root = sl_net_card_root();
    return root && snprintf(out, n, "%s%s", root, rb) < (int)n;
}

static void *shot_worker(void *unused)
{
    (void)unused;
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   /* music and touch come first */
#endif
    int w = shot_w, h = shot_h, result = SL_SHOT_FAILED;
    char rb[256] = "", real[400];
    unsigned char *png = NULL;
    size_t size = 0;
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (rgb)
    {
        for (int i = 0; i < w * h; i++)
        {
            unsigned p = shot_px[i];
            rgb[3 * i] = (unsigned char)(((p >> 11) & 31) * 255 / 31);
            rgb[3 * i + 1] = (unsigned char)(((p >> 5) & 63) * 255 / 63);
            rgb[3 * i + 2] = (unsigned char)((p & 31) * 255 / 31);
        }
        free(shot_px);
        shot_px = NULL;
        if (lodepng_encode24(&png, &size, rgb, (unsigned)w, (unsigned)h)) { free(png); png = NULL; }
        free(rgb);
    }
    if (png && shot_real(real, sizeof real, SL_SHOT_DIR))
    {
        mkdir(real, 0777);
        int fd = -1;
        for (int k = 1; k < 100 && fd < 0; k++)
        {
            if (k == 1) snprintf(rb, sizeof rb, SL_SHOT_DIR "/%s.png", shot_name);
            else snprintf(rb, sizeof rb, SL_SHOT_DIR "/%s %d.png", shot_name, k);
            if (!shot_real(real, sizeof real, rb)) break;
            fd = open(real, O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (fd < 0 && errno != EEXIST) break;
        }
        if (fd >= 0)
        {
            size_t done = 0;
            while (done < size)
            {
                ssize_t r = write(fd, png + done, size - done);
                if (r <= 0) break;
                done += (size_t)r;
            }
            int err = done < size ? (errno ? errno : ENOSPC) : 0;
            if (!err && fsync(fd)) err = errno;
            close(fd);
            if (err) { unlink(real); result = err == ENOSPC ? SL_SHOT_FULL : SL_SHOT_FAILED; }
            else result = SL_SHOT_SAVED;
        }
        else if (errno == ENOSPC) result = SL_SHOT_FULL;
    }
    free(shot_px);
    shot_px = NULL;
    pthread_mutex_lock(&shot_lock);
    if (result == SL_SHOT_SAVED)
    {
        free(last_png);
        last_png = png;
        last_size = size;
        last_seq++;
        png = NULL;
    }
    snprintf(shot_path, sizeof shot_path, "%s", result == SL_SHOT_SAVED ? rb : "");
    shot_state = result;
    shot_running = false;
    pthread_mutex_unlock(&shot_lock);
    free(png);
    return NULL;
}

bool sl_shot_start(uint16_t *px, int w, int h, const char *name)
{
    pthread_mutex_lock(&shot_lock);
    bool busy = shot_running;
    pthread_mutex_unlock(&shot_lock);
    if (busy) { free(px); return false; }
    if (shot_joinable) { pthread_join(shot_thread, NULL); shot_joinable = false; }
    shot_px = px;
    shot_w = w;
    shot_h = h;
    snprintf(shot_name, sizeof shot_name, "%s", name);
    shot_state = SL_SHOT_BUSY;
    shot_running = true;
    if (pthread_create(&shot_thread, NULL, shot_worker, NULL))
    {
        shot_running = false;
        shot_state = SL_SHOT_IDLE;
        free(px);
        shot_px = NULL;
        return false;
    }
    shot_joinable = true;
    return true;
}

int sl_shot_poll(char *path, size_t n)
{
    pthread_mutex_lock(&shot_lock);
    int s = shot_state;
    if (s != SL_SHOT_IDLE && s != SL_SHOT_BUSY)
    {
        if (path && n) snprintf(path, n, "%s", shot_path);
        shot_state = SL_SHOT_IDLE;
    }
    pthread_mutex_unlock(&shot_lock);
    return s;
}

bool sl_shot_busy(void)
{
    pthread_mutex_lock(&shot_lock);
    bool b = shot_running;
    pthread_mutex_unlock(&shot_lock);
    return b;
}

void sl_shot_wait(void)
{
    if (shot_joinable) { pthread_join(shot_thread, NULL); shot_joinable = false; }
}

unsigned sl_shot_seq(void)
{
    pthread_mutex_lock(&shot_lock);
    unsigned s = last_seq;
    pthread_mutex_unlock(&shot_lock);
    return s;
}

size_t sl_shot_png_size(void)
{
    pthread_mutex_lock(&shot_lock);
    size_t n = last_png ? last_size : 0;
    pthread_mutex_unlock(&shot_lock);
    return n;
}

size_t sl_shot_png(void *out, size_t cap, unsigned *seq)
{
    size_t n = 0;
    pthread_mutex_lock(&shot_lock);
    if (seq) *seq = last_seq;
    if (last_png && last_size <= cap) { memcpy(out, last_png, last_size); n = last_size; }
    pthread_mutex_unlock(&shot_lock);
    return n;
}

bool sl_png_load565(const char *path, int mw, int mh, uint16_t **px, int *w, int *h)
{
    char real[400];
    *px = NULL;
    if (!shot_real(real, sizeof real, path)) return false;
    int fd = open(real, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    unsigned char *buf = NULL;
    size_t size = 0;
    if (!fstat(fd, &st) && st.st_size > 0 && st.st_size < 16 * 1024 * 1024 && (buf = malloc((size_t)st.st_size)))
    {
        while (size < (size_t)st.st_size)
        {
            ssize_t r = read(fd, buf + size, (size_t)st.st_size - size);
            if (r <= 0) break;
            size += (size_t)r;
        }
    }
    close(fd);
    if (!buf || size != (size_t)st.st_size) { free(buf); return false; }
    /* lodepng as LVGL ships it hands back a draw buffer holding R, G, B, A */
    unsigned char *out = NULL;
    unsigned iw = 0, ih = 0;
    /* the size its header claims, before any of it is made: a small file can
     * say 30000 x 30000, and decoding that took the player's memory (a few
     * MB here at most: a screenshot is 480 x 720, box art far less) */
    if (size < 24 || memcmp(buf + 12, "IHDR", 4)) { free(buf); return false; }
    iw = (unsigned)buf[16] << 24 | (unsigned)buf[17] << 16 | (unsigned)buf[18] << 8 | buf[19];
    ih = (unsigned)buf[20] << 24 | (unsigned)buf[21] << 16 | (unsigned)buf[22] << 8 | buf[23];
    if (!iw || !ih || iw > 4096 || ih > 4096 || (unsigned long long)iw * ih > 2u << 20) { free(buf); return false; }
    unsigned err = lodepng_decode32(&out, &iw, &ih, buf, size);
    free(buf);
    if (err || !out) { if (out) lv_draw_buf_destroy((lv_draw_buf_t *)out); return false; }
    lv_draw_buf_t *db = (lv_draw_buf_t *)out;
    const uint8_t *src = db->data;
    uint32_t stride = db->header.stride;
    /* the largest whole-pixel box that fits: area-averaged, so text stays crisp */
    int ow = (int)iw, oh = (int)ih;
    if (ow > mw || oh > mh)
    {
        if ((long)iw * mh > (long)ih * mw) { ow = mw; oh = (int)((long)ih * mw / iw); }
        else { oh = mh; ow = (int)((long)iw * mh / ih); }
    }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    uint16_t *dst = malloc((size_t)ow * oh * 2);
    if (!dst) { lv_draw_buf_destroy(db); return false; }
    for (int y = 0; y < oh; y++)
    {
        unsigned y0 = (unsigned)((long)y * ih / oh), y1 = (unsigned)((long)(y + 1) * ih / oh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < ow; x++)
        {
            unsigned x0 = (unsigned)((long)x * iw / ow), x1 = (unsigned)((long)(x + 1) * iw / ow);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (unsigned yy = y0; yy < y1; yy++)
            {
                const uint8_t *p = src + yy * stride + x0 * 4;
                for (unsigned xx = x0; xx < x1; xx++, p += 4) { r += p[0]; g += p[1]; b += p[2]; n++; }
            }
            r /= n; g /= n; b /= n;
            dst[y * ow + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    lv_draw_buf_destroy(db);
    *px = dst;
    *w = ow;
    *h = oh;
    return true;
}

bool sl_shot_delete(const char *path)
{
    char real[400];
    return shot_real(real, sizeof real, path) && !unlink(real);
}

/* ------------------------------------------- a zip member, for the e-book reader */
int sl_inflate(const unsigned char *in, size_t n, size_t cap, unsigned char **out, size_t *outn)
{
    LodePNGDecompressSettings s;
    lodepng_decompress_settings_init(&s);
    s.max_output_size = cap;
    unsigned char *o = NULL;
    size_t on = 0;
    unsigned err = lodepng_inflate(&o, &on, in, n, &s);
    if (err && !on) { free(o); *out = NULL; *outn = 0; return -1; }
    if (on > cap) on = cap;              /* the last block may run a little past */
    *out = o;
    *outn = on;
    return err ? 1 : 0;
}

/* ------------------------------------------- a PNG of any picture (the remote's icons) */
size_t sl_png_encode_rgb(const unsigned char *rgb, int w, int h, unsigned char **out)
{
    unsigned char *png = NULL;
    size_t size = 0;
    if (lodepng_encode24(&png, &size, rgb, (unsigned)w, (unsigned)h)) { free(png); png = NULL; size = 0; }
    *out = png;
    return size;
}
