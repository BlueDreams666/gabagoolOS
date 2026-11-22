/***************************************************************************
 * GabagoolOS - album art thumbnails.
 *
 * Covers are decoded once, in idle moments, from the embedded picture or a
 * cover/folder image next to the music, then stored on the card as ready-
 * to-draw LVGL images (/.rockbox/sloppy/thumbs). LVGL reads them through a
 * small file-system driver and keeps recently used ones in its image cache,
 * so scrolling a grid of covers never decodes a JPEG.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "string-extra.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "rbpaths.h"
#include "metadata.h"
#include "lcd.h"
#include "recorder/bmp.h"
#include "recorder/jpeg_load.h"
#include "recorder/png_load.h"
#include "recorder/albumart.h"
#include "lvgl.h"
#include "sloppy_int.h"
#include "sloppy_net.h"
#include <pthread.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

/* ------------------------------------------- covers Rockbox cannot read */
/* Rockbox's decoders take baseline JPEG, PNG and BMP. Progressive JPEG
 * (common for art from streaming services and the web) and some PNGs
 * fail, and those albums had no cover on any player. stb_image decodes
 * them, on a low-priority thread so music and the UI keep their time,
 * and only when the kernel says there is memory for it: it decodes at
 * full size (a progressive JPEG keeps every coefficient), up to 9 bytes
 * a pixel. The card is read on the cover thread; this one only computes. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#define STBI_MAX_DIMENSIONS 8192
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "../../lib/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define STB_FILE_MAX   (8 * 1024 * 1024)   /* bigger embedded art is not a cover */
#define STB_HEADROOM_KB 6144               /* left free while a cover decodes */

static long art_mem_available_kb(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char line[96];
    long kb = -1;
    while (fgets(line, sizeof line, fp))
        if (sscanf(line, "MemAvailable: %ld", &kb) == 1) break;
    fclose(fp);
    return kb;
}

/* One job at a time. state: 0 idle, 1 decoding, 2 done (ok says how);
 * only ever read and written atomically, as the thread sets it. */
static struct stb_job {
    int state;
    unsigned epoch;
    int album;
    unsigned char *in;
    int in_len;
    uint16_t *px;                    /* RGB565, out_w x out_h, fitted to the grid size */
    int out_w, out_h;
    bool ok;
    char source[MAX_PATH];
    pthread_t thread;
} sj;
static unsigned art_epoch;           /* a library change discards a job in flight */
static int sj_state(void) { return __atomic_load_n(&sj.state, __ATOMIC_ACQUIRE); }
static void sj_set(int v) { __atomic_store_n(&sj.state, v, __ATOMIC_RELEASE); }
/* a breath for the UI on the cover maker's thread (none without one) */
static unsigned int art_thread_id;
static void art_pause(void) { if (art_thread_id) yield(); }

/* RGB888 to RGB565, area-averaged so the shorter side is fit (the
 * thumbnails are a centre square of it), keeping aspect */
static uint16_t *stb_fit565(const unsigned char *rgb, int w, int h, int fit, int *ow, int *oh)
{
    int short_side = w < h ? w : h;
    int dw = short_side > fit ? (int)((long)w * fit / short_side) : w;
    int dh = short_side > fit ? (int)((long)h * fit / short_side) : h;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    uint16_t *out = malloc((size_t)dw * dh * 2);
    if (!out) return NULL;
    for (int y = 0; y < dh; y++)
    {
        int y0 = (int)((long)y * h / dh), y1 = (int)((long)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++)
        {
            int x0 = (int)((long)x * w / dw), x1 = (int)((long)(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
            {
                const unsigned char *p = rgb + ((size_t)yy * w + x0) * 3;
                for (int xx = x0; xx < x1; xx++, p += 3) { r += p[0]; g += p[1]; b += p[2]; n++; }
            }
            r /= n; g /= n; b /= n;
            out[y * dw + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
    *ow = dw; *oh = dh;
    return out;
}

static void *stb_worker(void *arg)
{
    (void)arg;
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15);   /* after audio and the UI */
#endif
    int w = 0, h = 0, comp = 0;
    unsigned char *rgb = stbi_load_from_memory(sj.in, sj.in_len, &w, &h, &comp, 3);
    free(sj.in); sj.in = NULL;
    sj.ok = false;
    if (rgb && w > 0 && h > 0)
    {
        sj.px = stb_fit565(rgb, w, h, SL_ART_GRID, &sj.out_w, &sj.out_h);
        sj.ok = sj.px != NULL;
    }
    stbi_image_free(rgb);
    sj_set(2);
    return NULL;
}

/* Hand a cover to the thread: false when it cannot be tried at all (not an
 * image stb reads, too big for the memory there is, no thread). */
static bool stb_start(int album, const char *path, long off, long size)
{
    if (sj_state() || size <= 0 || size > STB_FILE_MAX) return false;
    unsigned char *in = malloc((size_t)size);
    if (!in) return false;
    int fd = open(path, O_RDONLY);
    bool ok = fd >= 0 && lseek(fd, off, SEEK_SET) == off;
    /* a read blocks every Rockbox thread on the R1: 64 KiB at a time */
    for (long got = 0; ok && got < size; art_pause())
    {
        ssize_t r = read(fd, in + got, (size_t)(size - got < 65536 ? size - got : 65536));
        if (r <= 0) ok = false;
        else got += r;
    }
    if (fd >= 0) close(fd);
    int w, h, comp;
    if (ok) ok = stbi_info_from_memory(in, (int)size, &w, &h, &comp) && w > 0 && h > 0;
    if (ok)
    {
        long need_kb = (long)((uint64_t)w * h * 9 / 1024) + size / 1024;
        long avail = art_mem_available_kb();
        ok = avail < 0 ? (uint64_t)w * h <= 4000000u : need_kb + STB_HEADROOM_KB <= avail;
        if (!ok) sl_log("art: %dx%d cover for album %d needs %ld KiB, %ld KiB free: skipped",
                        w, h, album, need_kb, avail);
    }
    if (!ok) { free(in); return false; }
    sj.album = album;
    sj.epoch = art_epoch;
    sj.in = in;
    sj.in_len = (int)size;
    sj.px = NULL;
    strlcpy(sj.source, path, sizeof sj.source);
    sj_set(1);
    if (pthread_create(&sj.thread, NULL, stb_worker, NULL) != 0)
    {
        sj_set(0);
        free(sj.in); sj.in = NULL;
        return false;
    }
    sl_log("art: decoding a %dx%d cover for album %d in the background", w, h, album);
    return true;
}

/* ------------------------------------------- a podcast's picture */
/* The podcast worker (sloppy_podjob.inc) fetches a show's picture into
 * memory and makes its squares itself; the decode is this one's, under
 * the covers' rule for memory. Called on that worker's thread. */
#include "sloppy_podcast.h"
unsigned char *sl_art_rgb_load(const unsigned char *bytes, int len, int *w, int *h)
{
    int comp = 0;
    if (!bytes || len <= 0 || !stbi_info_from_memory(bytes, len, w, h, &comp) || *w <= 0 || *h <= 0) return NULL;
    long need_kb = (long)((uint64_t)*w * *h * 9 / 1024);   /* the covers' rule: a progressive JPEG's coefficients too */
    long avail = art_mem_available_kb();
    if (avail < 0 ? (uint64_t)*w * *h > 4000000u : need_kb + STB_HEADROOM_KB > avail)
    {
        sl_log("podcasts: a %dx%d picture needs %ld KiB, %ld KiB free: not opened", *w, *h, need_kb, avail);
        return NULL;
    }
    return stbi_load_from_memory(bytes, len, w, h, &comp, 3);
}
void sl_art_rgb_free(unsigned char *rgb) { stbi_image_free(rgb); }

/* ------------------------------------------- a picture, for the Files viewer */
/* The viewer reads the file on the UI thread, a slice a frame; the decode
 * and the fit run here, on a thread of their own at the same low priority
 * as the covers', and only when the kernel says there is memory for a
 * full-size decode. One picture at a time; a picture the viewer left
 * before it was done is freed when the thread ends. */
static struct pic_job {
    int state;                       /* 0 idle, 1 decoding, 2 done */
    bool ok, abandoned;
    unsigned char *in;
    int in_len, max_w, max_h, w, h, out_w, out_h;
    uint16_t *px;
    pthread_t thread;
} pj;
static int pj_state(void) { return __atomic_load_n(&pj.state, __ATOMIC_ACQUIRE); }
static void *pic_worker(void *arg)
{
    (void)arg;
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15);
#endif
    int w = 0, h = 0, comp = 0;
    unsigned char *rgb = stbi_load_from_memory(pj.in, pj.in_len, &w, &h, &comp, 3);
    free(pj.in); pj.in = NULL;
    pj.ok = false;
    if (rgb && w > 0 && h > 0)
    {
        /* the largest whole box that fits, never scaled up: fit the longer
         * side's ratio, then the stb fit (shorter side) at that size */
        int ow = w, oh = h;
        if (ow > pj.max_w || oh > pj.max_h)
        {
            if ((long)w * pj.max_h > (long)h * pj.max_w) { ow = pj.max_w; oh = (int)((long)h * pj.max_w / w); }
            else { oh = pj.max_h; ow = (int)((long)w * pj.max_h / h); }
        }
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        pj.px = stb_fit565(rgb, w, h, ow < oh ? ow : oh, &pj.out_w, &pj.out_h);
        pj.w = w; pj.h = h;
        pj.ok = pj.px != NULL;
    }
    stbi_image_free(rgb);
    __atomic_store_n(&pj.state, 2, __ATOMIC_RELEASE);
    return NULL;
}
int sl_pic_start(unsigned char *bytes, int len, int max_w, int max_h)
{
    if (pj_state() == 1) { free(bytes); return SL_PIC_BUSY; }
    if (pj_state() == 2) { free(pj.px); pj.px = NULL; pj.state = 0; }
    int w, h, comp;
    if (!bytes || len <= 0 || !stbi_info_from_memory(bytes, len, &w, &h, &comp) || w <= 0 || h <= 0)
    { free(bytes); return SL_PIC_UNREADABLE; }
    long need_kb = (long)((uint64_t)w * h * 9 / 1024) + len / 1024;
    long avail = art_mem_available_kb();
    if (avail < 0 ? (uint64_t)w * h > 12000000u : need_kb + STB_HEADROOM_KB > avail)
    {
        sl_log("picture: %dx%d needs %ld KiB, %ld KiB free: not opened", w, h, need_kb, avail);
        free(bytes);
        return SL_PIC_TOO_BIG;
    }
    pj.in = bytes; pj.in_len = len; pj.max_w = max_w; pj.max_h = max_h;
    pj.px = NULL; pj.ok = pj.abandoned = false;
    __atomic_store_n(&pj.state, 1, __ATOMIC_RELEASE);
    if (pthread_create(&pj.thread, NULL, pic_worker, NULL) != 0)
    {
        pj.state = 0;
        free(pj.in); pj.in = NULL;
        return SL_PIC_UNREADABLE;
    }
    return SL_PIC_STARTED;
}
int sl_pic_poll(uint16_t **px, int *w, int *h, int *iw, int *ih)
{
    if (pj_state() != 2) return pj_state() == 1 ? 0 : -1;
    pthread_join(pj.thread, NULL);
    pj.state = 0;
    if (!pj.ok) return -1;
    *px = pj.px; pj.px = NULL;
    *w = pj.out_w; *h = pj.out_h; *iw = pj.w; *ih = pj.h;
    return 1;
}

/* ---------------------------------------- a picture made a wallpaper */
/* on its own thread, so the plain POSIX calls on the card's real path,
 * not Rockbox's file layer */
#include <fcntl.h>
#pragma push_macro("open")
#pragma push_macro("read")
#pragma push_macro("write")
#pragma push_macro("close")
#pragma push_macro("rename")
#pragma push_macro("fsync")
#undef open
#undef read
#undef write
#undef close
#undef rename
#undef fsync
static struct wall_job {
    int state;                       /* 0 idle, 1 working, 2 done */
    bool ok;
    char in[MAX_PATH], out[MAX_PATH];
    int tw, th;
    bool fit;                        /* a boot logo: all of it, no bigger than it is, undimmed */
    pthread_t thread;
} wj;
static int wj_state(void) { return __atomic_load_n(&wj.state, __ATOMIC_ACQUIRE); }
static bool wall_real(char *out, size_t n, const char *rb)
{
    const char *root = sl_net_card_root();
    return root && snprintf(out, n, "%s%s", root, rb) < (int)n;
}
static bool wall_make(void)
{
    char real[MAX_PATH + 64];
    struct stat st;
    if (!wall_real(real, sizeof real, wj.in) || stat(real, &st) || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024)
        return false;
    unsigned char *in = malloc((size_t)st.st_size);
    int fd = in ? open(real, O_RDONLY) : -1;
    long got = 0;
    while (fd >= 0 && got < st.st_size)
    {
        ssize_t r = read(fd, in + got, (size_t)(st.st_size - got));
        if (r <= 0) break;
        got += r;
    }
    if (fd >= 0) close(fd);
    int w = 0, h = 0, comp = 0;
    if (got != st.st_size || !stbi_info_from_memory(in, (int)got, &w, &h, &comp) || w <= 0 || h <= 0)
    { free(in); return false; }
    long need_kb = (long)((uint64_t)w * h * 9 / 1024) + got / 1024 + (long)wj.tw * wj.th * 2 / 1024;   /* 9: as the covers' */
    long avail = art_mem_available_kb();
    if (avail < 0 ? (uint64_t)w * h > 12000000u : need_kb + STB_HEADROOM_KB > avail)
    {
        sl_log("wallpaper: %dx%d needs %ld KiB, %ld KiB free", w, h, need_kb, avail);
        free(in);
        return false;
    }
    unsigned char *rgb = stbi_load_from_memory(in, (int)got, &w, &h, &comp, 3);
    free(in);
    int cx = 0, cy = 0, cw = w, ch = h;
    if (wj.fit && rgb)
    {
        /* a boot logo: the whole picture, shrunk to the screen if it is
         * bigger, never grown (a small logo stays crisp, in the middle) */
        if ((long)w * wj.th > (long)h * wj.tw) { if (w > wj.tw) { wj.th = (int)((long)h * wj.tw / w); } else { wj.tw = w; wj.th = h; } }
        else if (h > wj.th) wj.tw = (int)((long)w * wj.th / h);
        else { wj.tw = w; wj.th = h; }
        if (wj.tw < 1) wj.tw = 1;
        if (wj.th < 1) wj.th = 1;
    }
    uint16_t *px = rgb ? malloc((size_t)wj.tw * wj.th * 2) : NULL;
    if (!px) { stbi_image_free(rgb); return false; }
    /* the middle of it, in the screen's shape */
    if (!wj.fit && (long)w * wj.th > (long)h * wj.tw) { cw = (int)((long)h * wj.tw / wj.th); cx = (w - cw) / 2; }
    else if (!wj.fit) { ch = (int)((long)w * wj.th / wj.tw); cy = (h - ch) / 2; }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    /* area-averaged (a picture smaller than the screen: its pixels repeat) */
    uint64_t luma = 0;
    for (int y = 0; y < wj.th; y++)
    {
        int y0 = cy + (int)((long)y * ch / wj.th), y1 = cy + (int)((long)(y + 1) * ch / wj.th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < wj.tw; x++)
        {
            int x0 = cx + (int)((long)x * cw / wj.tw), x1 = cx + (int)((long)(x + 1) * cw / wj.tw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
            {
                const unsigned char *p = rgb + ((size_t)yy * w + x0) * 3;
                for (int xx = x0; xx < x1; xx++, p += 3) { r += p[0]; g += p[1]; b += p[2]; n++; }
            }
            r /= n; g /= n; b /= n;
            luma += (r * 54 + g * 183 + b * 19) >> 8;
            px[y * wj.tw + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
    stbi_image_free(rgb);
    /* white words and the status bar sit on it: a bright picture is dimmed
     * until its average is about the built-in photos', never below 55 % */
    unsigned mean = (unsigned)(luma / ((uint64_t)wj.tw * wj.th));
    unsigned k = mean > 96 ? 96 * 256 / mean : 256;
    if (k < 141) k = 141;
    if (wj.fit) k = 256;                 /* nothing sits on a boot logo */
    if (k < 256)
        for (int i = 0; i < wj.tw * wj.th; i++)
        {
            unsigned c = px[i];
            px[i] = (uint16_t)((((c >> 11) * k) >> 8) << 11 | ((((c >> 5) & 63) * k) >> 8) << 5 | (((c & 31) * k) >> 8));
        }
    /* written aside, then put in place: a cut-off write never shows */
    char tmp[MAX_PATH + 64];
    bool ok = wall_real(real, sizeof real, wj.out) && snprintf(tmp, sizeof tmp, "%s.new", real) < (int)sizeof tmp;
    fd = ok ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    size_t size = (size_t)wj.tw * wj.th * 2, put = 0;
    if (wj.fit && fd >= 0)
    {
        /* its size first: "SLBL", width and height, little-endian */
        unsigned char hd[8] = { 'S', 'L', 'B', 'L', (unsigned char)wj.tw, (unsigned char)(wj.tw >> 8),
                                (unsigned char)wj.th, (unsigned char)(wj.th >> 8) };
        if (write(fd, hd, sizeof hd) != (ssize_t)sizeof hd) { close(fd); fd = -1; }
    }
    while (fd >= 0 && put < size)
    {
        ssize_t r = write(fd, (const char *)px + put, size - put);
        if (r <= 0) break;
        put += (size_t)r;
    }
    if (fd >= 0) { fsync(fd); close(fd); }
    free(px);
    ok = fd >= 0 && put == size && !rename(tmp, real);
    if (!ok) unlink(tmp);
    sl_log("%s: %dx%d %s -> %dx%d, mean %u, kept %u/256: %s", wj.fit ? "boot logo" : "wallpaper", w, h, wj.in, wj.tw, wj.th,
           mean, k, ok ? "saved" : "not saved");
    return ok;
}
static void *wall_worker(void *arg)
{
    (void)arg;
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15);
#endif
    wj.ok = wall_make();
    __atomic_store_n(&wj.state, 2, __ATOMIC_RELEASE);
    return NULL;
}
static int wall_start(const char *path, const char *out, int tw, int th, bool fit)
{
    if (wj_state() == 1) return SL_PIC_BUSY;
    if (wj_state() == 2) { pthread_join(wj.thread, NULL); wj.state = 0; }
    if (!path || !out || tw <= 0 || th <= 0) return SL_PIC_UNREADABLE;
    strlcpy(wj.in, path, sizeof wj.in);
    strlcpy(wj.out, out, sizeof wj.out);
    wj.tw = tw; wj.th = th; wj.ok = false; wj.fit = fit;
    __atomic_store_n(&wj.state, 1, __ATOMIC_RELEASE);
    if (pthread_create(&wj.thread, NULL, wall_worker, NULL) != 0) { wj.state = 0; return SL_PIC_UNREADABLE; }
    return SL_PIC_STARTED;
}
int sl_wall_start(const char *path, const char *out, int tw, int th) { return wall_start(path, out, tw, th, false); }
int sl_bootlogo_start(const char *path, const char *out, int tw, int th) { return wall_start(path, out, tw, th, true); }
int sl_wall_poll(void)
{
    if (wj_state() != 2) return wj_state() == 1 ? 0 : -1;
    pthread_join(wj.thread, NULL);
    wj.state = 0;
    return wj.ok ? 1 : -1;
}
#pragma pop_macro("open")
#pragma pop_macro("read")
#pragma pop_macro("write")
#pragma pop_macro("close")
#pragma pop_macro("rename")
#pragma pop_macro("fsync")

#define THUMB_DIR ROCKBOX_DIR "/sloppy/thumbs"
#define THUMB_GENERATION THUMB_DIR "/generation"
/* the generation whose older files are all gone: without it every boot
 * checked all albums and read the whole folder again, for nothing */
#define THUMB_PRUNED THUMB_DIR "/pruned"
static uint32_t thumb_generation;
static bool thumb_prev_sharded;      /* the generation before is in folders too */
static bool thumb_prune_pending;

/* A cover is kept while the image it came from is the same file, the same
 * size and the same time: a song with the art in it, or a picture in the
 * album's folder. Nothing else counts. Version 1 also held the album's
 * songs and the folder's time, so a song added to an album, or a computer
 * that left a .DS_Store in its folder, made the cover again. */
#define ART_STAMP_MAGIC 0x53524132u  /* SRA2 */
struct art_stamp {
    uint32_t magic;
    int64_t source_size, source_mtime;
    char source[MAX_PATH];
};
static uint64_t stamp_bytes(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n--) h = (h ^ *p++) * UINT64_C(1099511628211);
    return h;
}
static uint64_t album_tracks_stamp(int album)
{
    const struct sl_album *al = &sl_lib.albums[album];
    uint64_t h = UINT64_C(14695981039346656037);
    h = stamp_bytes(h, &al->n_tracks, sizeof al->n_tracks);
    for (int i = 0; i < al->n_tracks; i++)
    {
        const struct sl_track *t = &sl_lib.tracks[al->tracks[i]];
        char path[MAX_PATH];                 /* the bytes of its whole path, as stamps made before have them */
        sl_track_path(al->tracks[i], path, sizeof path);
        h = stamp_bytes(h, path, strlen(path) + 1);
        h = stamp_bytes(h, &t->added, sizeof t->added);
    }
    return h;
}
/* stat() of a library path, through Rockbox's open(). The C library's
 * stat() takes the path as it is, and on the R1 the card is under
 * PIVOT_ROOT ("/Music/a.flac" is /data/mnt/sd_0/Music/a.flac), so it never
 * found a song: every cover was stamped as having no source, and every
 * library update threw all 1700 away and made them again (17 minutes).
 * Rockbox's open() adds the prefix, as the simulator's adds its own. */
static int card_stat(const char *path, struct stat *st)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fstat(fd, st);
    close(fd);
    return rc;
}
static int64_t source_dir_mtime(const char *path)
{
    char dir[MAX_PATH];
    strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    if (slash == dir) slash[1] = '\0';
    else *slash = '\0';
    struct stat st;
    return card_stat(dir, &st) ? -1 : (int64_t)st.st_mtime;
}
static bool stamp_source(struct art_stamp *stamp, const char *source)
{
    struct stat st;
    memset(stamp, 0, sizeof *stamp);
    if (!source || strlen(source) >= sizeof stamp->source || card_stat(source, &st)) return false;
    stamp->magic = ART_STAMP_MAGIC;
    stamp->source_size = st.st_size;
    stamp->source_mtime = st.st_mtime;
    strlcpy(stamp->source, source, sizeof stamp->source);
    return true;
}
/* The source is still this album's: one of its songs, or a picture in the
 * folder of one of them (or a folder above, where folder art is looked
 * for too). */
static bool source_in_album(const char *source, int album)
{
    const char *slash = strrchr(source, '/');
    if (!slash) return false;
    size_t dir = (size_t)(slash - source);
    const struct sl_album *al = &sl_lib.albums[album];
    for (int i = 0; i < al->n_tracks; i++)
    {
        /* its folder, as the file's name has no '/' in it */
        const struct sl_track *tr = &sl_lib.tracks[al->tracks[i]];
        const char *t = tr->file ? sl_lib.dirs[tr->dir] : "";
        if (!strncmp(t, source, dir) && t[dir] == '/') return true;
    }
    return false;
}
static bool stamp_matches(const struct art_stamp *stamp, int album)
{
    struct stat st;
    return stamp->magic == ART_STAMP_MAGIC &&
           stamp->source[0] &&
           memchr(stamp->source, '\0', sizeof stamp->source) &&
           source_in_album(stamp->source, album) &&
           !card_stat(stamp->source, &st) &&
           stamp->source_size == st.st_size && stamp->source_mtime == st.st_mtime;
}

/* Retire old generations a few files at a time during idle art work:
 * flat files left in the folder by older builds, and every generation's
 * folder but the current one, emptied from the bottom and then removed. */
static DIR *prune_dir[3];
static char prune_path[3][48];
static int prune_depth = -1;
static bool art_over(void);
static bool prune_thumbs(void)
{
    if (prune_depth < 0 && thumb_prune_pending)
    {
        strlcpy(prune_path[0], THUMB_DIR, sizeof prune_path[0]);
        prune_dir[0] = opendir(THUMB_DIR);
        if (prune_dir[0]) prune_depth = 0;
    }
    if (prune_depth < 0) return false;
    char current[12];
    snprintf(current, sizeof current, "g%08x", thumb_generation);
    /* four files, in the step's time: names skipped cost a read each too */
    for (int n = 0, seen = 0; n < 4 && (!seen || !art_over()); seen++)
    {
        struct dirent *entry = readdir(prune_dir[prune_depth]);
        if (!entry)
        {
            closedir(prune_dir[prune_depth]);
            if (prune_depth > 0) { rmdir(prune_path[prune_depth]); prune_depth--; n++; continue; }
            prune_depth = -1; thumb_prune_pending = false;
            int fd = open(THUMB_PRUNED, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0)
            {
                if (write(fd, &thumb_generation, sizeof thumb_generation) != (ssize_t)sizeof thumb_generation)
                    remove(THUMB_PRUNED);      /* unsure: check again next boot */
                close(fd);
            }
            return false;
        }
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (name[0] == '.') continue;
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s/%s", prune_path[prune_depth], name);
        if (prune_depth == 0)
        {
            bool flat = len > 4 && (!strcmp(name + len - 4, ".bin") || !strcmp(name + len - 4, ".tmp"));
            if (flat) { remove(path); n++; continue; }
            /* an old generation's folder: g and eight hex digits */
            if (len != 9 || name[0] != 'g' || !strcmp(name, current)) continue;
        }
        else if (prune_depth == 2) { remove(path); n++; continue; }
        DIR *sub = strlen(path) < sizeof prune_path[0] ? opendir(path) : NULL;
        if (!sub) { if (prune_depth) { remove(path); n++; } continue; }
        prune_depth++;
        prune_dir[prune_depth] = sub;
        strlcpy(prune_path[prune_depth], path, sizeof prune_path[0]);
    }
    return true;
}

/* ------------------------------------------------ LVGL file driver 'R' */
static void *fs_open(lv_fs_drv_t *d, const char *path, lv_fs_mode_t mode)
{
    (void)d;
    int fd = open(path, mode == LV_FS_MODE_WR ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0666);
    return fd < 0 ? NULL : (void *)(intptr_t)(fd + 1);
}
static lv_fs_res_t fs_close(lv_fs_drv_t *d, void *f)
{ (void)d; close((int)(intptr_t)f - 1); return LV_FS_RES_OK; }
static lv_fs_res_t fs_read(lv_fs_drv_t *d, void *f, void *buf, uint32_t n, uint32_t *br)
{
    (void)d;
    ssize_t r = read((int)(intptr_t)f - 1, buf, n);
    if (r < 0) { *br = 0; return LV_FS_RES_FS_ERR; }
    *br = (uint32_t)r;
    return LV_FS_RES_OK;
}
static lv_fs_res_t fs_seek(lv_fs_drv_t *d, void *f, uint32_t pos, lv_fs_whence_t w)
{
    (void)d;
    int wh = w == LV_FS_SEEK_CUR ? SEEK_CUR : w == LV_FS_SEEK_END ? SEEK_END : SEEK_SET;
    return lseek((int)(intptr_t)f - 1, pos, wh) < 0 ? LV_FS_RES_FS_ERR : LV_FS_RES_OK;
}
static lv_fs_res_t fs_tell(lv_fs_drv_t *d, void *f, uint32_t *pos)
{
    (void)d;
    off_t p = lseek((int)(intptr_t)f - 1, 0, SEEK_CUR);
    if (p < 0) return LV_FS_RES_FS_ERR;
    *pos = (uint32_t)p;
    return LV_FS_RES_OK;
}

static void art_thread_start(void);
void sl_art_init(void)
{
    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter = 'R';
    drv.open_cb = fs_open;
    drv.close_cb = fs_close;
    drv.read_cb = fs_read;
    drv.seek_cb = fs_seek;
    drv.tell_cb = fs_tell;
    lv_fs_drv_register(&drv);
    art_thread_start();
    mkdir(ROCKBOX_DIR "/sloppy");
    mkdir(THUMB_DIR);
    int fd = open(THUMB_GENERATION, O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, &thumb_generation, sizeof thumb_generation) !=
            (ssize_t)sizeof thumb_generation) thumb_generation = 0;
        close(fd);
    }
    uint32_t pruned = 0;
    fd = open(THUMB_PRUNED, O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, &pruned, sizeof pruned) != (ssize_t)sizeof pruned) pruned = 0;
        close(fd);
    }
    thumb_prune_pending = thumb_generation != 0 && pruned != thumb_generation;
    /* Each generation used to be flat in the thumbnail folder. With a big
     * library that folder grew past 4000 files, and on the R1's exFAT the
     * first lookup of a name after a boot scans the whole folder: 36 ms a
     * cover, 1.4 s to draw the tiles. A generation now has a folder of its
     * own in 16 parts; a flat one moves into a new generation (its files
     * are kept, not made again: see promote_thumb). */
    char dir[48], prev[48];
    snprintf(dir, sizeof dir, THUMB_DIR "/g%08x", thumb_generation);
    snprintf(prev, sizeof prev, THUMB_DIR "/g%08x", thumb_generation - 1);
    if (!thumb_generation || !dir_exists(dir)) sl_art_invalidate_cache();
    else thumb_prev_sharded = thumb_generation > 1 && dir_exists(prev);
}

void sl_art_invalidate_cache(void)
{
    char dir[48];
    snprintf(dir, sizeof dir, THUMB_DIR "/g%08x", thumb_generation);
    bool sharded = thumb_generation && dir_exists(dir);
    uint32_t next = thumb_generation + 1;
    if (!next) next = 1;
    int fd = open(THUMB_GENERATION ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool saved = fd >= 0;
    if (saved)
    {
        saved = write(fd, &next, sizeof next) == (ssize_t)sizeof next && !fsync(fd);
        if (close(fd)) saved = false;
        if (saved) saved = rename(THUMB_GENERATION ".tmp", THUMB_GENERATION) == 0;
    }
    if (!saved)
    {
        remove(THUMB_GENERATION ".tmp");
        sl_log("art: could not advance thumbnail cache generation");
        return;
    }
    thumb_generation = next;
    thumb_prev_sharded = sharded;
    snprintf(dir, sizeof dir, THUMB_DIR "/g%08x", next);
    mkdir(dir);                              /* also marks the generation as in folders */
    while (prune_depth >= 0) closedir(prune_dir[prune_depth--]);
    thumb_prune_pending = true;
}

/* USB mode: the card is unmounted for the computer, and an open folder
 * would keep it mounted. The prune starts over from the top afterwards. */
static void art_job_settle(void);
static void sc_settle(void);
void sl_art_release(void)
{
    art_job_settle();
    sc_settle();                         /* a live tile's cover being read */
    if (prune_depth >= 0) thumb_prune_pending = true;
    while (prune_depth >= 0) closedir(prune_dir[prune_depth--]);
}

/* ------------------------------------------------------------- paths */
static unsigned album_key(int album)
{
    const struct sl_album *al = &sl_lib.albums[album];
    unsigned h = 2166136261u;
    for (const char *p = al->artist; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ '|') * 16777619u;
    for (const char *p = al->title; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return h;
}
static void thumb_path_key(char *buf, int n, uint32_t generation, bool sharded, unsigned key, int size)
{
    if (!generation)
        snprintf(buf, n, THUMB_DIR "/%08x_%d.bin", key, size);
    else if (!sharded)
        snprintf(buf, n, THUMB_DIR "/g%08x_%08x_%d.bin", generation, key, size);
    else
        snprintf(buf, n, THUMB_DIR "/g%08x/%x/%08x_%d.bin", generation, key >> 28, key, size);
}
static void thumb_path_at(char *buf, int n, uint32_t generation, bool sharded, int album, int size)
{
    thumb_path_key(buf, n, generation, sharded, album_key(album), size);
}
static void thumb_path(char *buf, int n, int album, int size)
{
    thumb_path_at(buf, n, thumb_generation, true, album, size);
}

/* The folders a cover of this generation goes in: mkdir on one that is
 * there costs a lookup the system has already cached. */
static void thumb_mkdirs_key(uint32_t generation, unsigned key)
{
    char dir[48];
    snprintf(dir, sizeof dir, THUMB_DIR "/g%08x", generation);
    mkdir(dir);
    snprintf(dir, sizeof dir, THUMB_DIR "/g%08x/%x", generation, key >> 28);
    mkdir(dir);
}
static void thumb_mkdirs(int album)
{
    thumb_mkdirs_key(thumb_generation, album_key(album));
}

/* A library update starts a new generation, and an album whose art source
 * is unchanged moves its files into it (a rename, no decode). Each file
 * carries the stamp of its source. One with no stamp, or an older kind,
 * is made again once; the prune removes what nobody moved (gone albums). */
static bool promote_thumb(int album, int size, const char *current)
{
    /* once pruned there is no older generation: looking costs a lookup */
    if (!thumb_generation || !thumb_prune_pending) return false;
    char old[72];
    thumb_path_at(old, sizeof old, thumb_generation - 1, thumb_prev_sharded, album, size);
    int fd = open(old, O_RDONLY);
    if (fd < 0) return false;
    lv_image_header_t hdr;
    struct art_stamp stamp;
    bool valid = read(fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr &&
                 hdr.magic == LV_IMAGE_HEADER_MAGIC && hdr.cf == LV_COLOR_FORMAT_RGB565 &&
                 hdr.w > 0 && hdr.w <= size && hdr.h > 0 && hdr.h <= size &&
                 hdr.stride == hdr.w * 2 &&
                 lseek(fd, 0, SEEK_END) == (off_t)(sizeof hdr + (size_t)hdr.stride * hdr.h + sizeof stamp) &&
                 lseek(fd, -(off_t)sizeof stamp, SEEK_END) >= 0 &&
                 read(fd, &stamp, sizeof stamp) == (ssize_t)sizeof stamp &&
                 stamp_matches(&stamp, album);
    close(fd);
    if (valid) thumb_mkdirs(album);
    if (valid && rename(old, current) == 0) return true;
    if (!valid) remove(old);
    return false;
}
/* The Zune grid's small covers come from the album's 219 px thumbnail: a
 * downscale, no second decode of the art, and only for albums someone has
 * looked at in that grid. The big one's stamp goes with it, so a library
 * refresh treats both alike. */
static void scale565(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh);
static bool derive_small(int album, const char *path)
{
    char big[72];
    thumb_path(big, sizeof big, album, SL_ART_GRID);
    int fd = open(big, O_RDONLY);
    if (fd < 0) return false;
    lv_image_header_t hdr;
    struct art_stamp stamp;
    bool ok = read(fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr &&
              hdr.magic == LV_IMAGE_HEADER_MAGIC && hdr.cf == LV_COLOR_FORMAT_RGB565 &&
              hdr.w > 0 && hdr.w <= SL_ART_GRID && hdr.h == hdr.w && hdr.stride == hdr.w * 2;
    size_t bytes = ok ? (size_t)hdr.w * hdr.h * 2 : 0;
    uint16_t *px = ok ? malloc(bytes) : NULL;
    ok = px && read(fd, px, bytes) == (ssize_t)bytes && read(fd, &stamp, sizeof stamp) == (ssize_t)sizeof stamp;
    close(fd);
    int out = hdr.w < SL_ART_SMALL ? hdr.w : SL_ART_SMALL;
    uint16_t *dst = ok ? malloc((size_t)out * out * 2) : NULL;
    if (dst)
    {
        scale565(px, hdr.w, hdr.h, dst, out, out);
        lv_image_header_t oh = hdr;
        oh.w = oh.h = out;
        oh.stride = out * 2;
        char tmp[80];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        ok = o >= 0 && write(o, &oh, sizeof oh) == (ssize_t)sizeof oh &&
             write(o, dst, (size_t)out * out * 2) == (ssize_t)((size_t)out * out * 2) &&
             write(o, &stamp, sizeof stamp) == (ssize_t)sizeof stamp;
        if (o >= 0 && close(o) < 0) ok = false;
        if (ok) ok = rename(tmp, path) == 0;
        if (!ok) remove(tmp);
    }
    else ok = false;
    free(px); free(dst);
    return ok;
}

const char *sl_art_src(int album, int size)
{
    static char src[8][80];
    static int slot;
    char path[72];
    if (album < 0 || album >= sl_lib.n_albums || size <= 0) return NULL;
    thumb_path(path, sizeof path, album, size);
    if (!file_exists(path) && !promote_thumb(album, size, path) &&
        !(size == SL_ART_SMALL && derive_small(album, path)))
        return NULL;
    char *s = src[slot];
    slot = (slot + 1) & 7;
    snprintf(s, 80, "R:%s", path);
    return s;
}
/* ------------------------------------------------------ making thumbs */
/* the first cover Rockbox could not decode, for stb_image to try */
static struct { bool have; char path[MAX_PATH]; long off, size; } stb_cand;

/* Rockbox threads on the R1 take turns on one OS thread (sigaltstack
 * threads): a switch happens only at a yield or a wait. The decoders yield
 * every row, and when the UI loop made a cover itself, every 17th yield
 * gave the background threads a turn they kept until they yielded. With
 * the tag database scanning 27k songs after USB mode, one cover held the
 * UI loop 88 s (R1 log 2026-10-23). Covers are made on a thread of their
 * own below the UI: the loop hands over one album and later collects it,
 * and the decoders' yields go back to the UI. What the thread needs of the
 * album is copied here: the library can be reloaded meanwhile. */
#define JOB_TRIES 4
enum { AJ_IDLE, AJ_QUEUED, AJ_RUNNING, AJ_STB, AJ_DONE };
static struct art_job {
    int state;
    unsigned epoch;
    int album;
    bool ok, again;                  /* again: not tried after all, pick it later */
    bool missed;                     /* no art last time, and nothing changed: not tried */
    unsigned key;
    uint32_t generation;
    uint64_t tracks;                 /* album_tracks_stamp */
    int n_tracks, tries;
    char title[96];
    char track[JOB_TRIES][MAX_PATH];
    struct art_stamp stamp;
} aj;

static bool decode_art(struct art_job *j, struct bitmap *bm, int size, unsigned char *work, size_t work_size)
{
    static struct mp3entry id3;
    stb_cand.have = false;
    static char artpath[MAX_PATH];
    if (j->tries <= 0) return false;
    const int format = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT;
    /* An album's first track often has no APIC even when later tracks do.
     * Try a few tracks, not all: each costs an open and a tag parse, and a
     * 30-track album with no art stalled the UI. Folder art is searched
     * once. */
    bool searched = false;
    for (int song = 0; song < j->tries; ++song, art_pause())
    {
        const char *track = j->track[song];
        const char *source = NULL;
        int fd = open(track, O_RDONLY);
        if (fd < 0) continue;
        memset(&id3, 0, sizeof id3);
        bool meta = get_metadata(&id3, fd, track);
        int rc = -1;
        bm->width = size; bm->height = size;
        bm->data = work;
#if LCD_DEPTH > 1
        bm->maskdata = NULL;
#endif
        if (meta && id3.has_embedded_albumart)
        {
            struct mp3_albumart *aa = &id3.albumart;
            if ((aa->type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_PNG)
                rc = clip_png_fd(fd, aa->pos, aa->size, bm, (int)work_size, format);
            else if ((aa->type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_JPG)
            {
                lseek(fd, aa->pos, SEEK_SET);
                rc = clip_jpeg_fd(fd, aa->type, aa->size, bm, (int)work_size, format, NULL, NULL);
            }
            if (rc > 0) source = track;
            else if (!stb_cand.have && aa->size > 0)
            {
                stb_cand.have = true;
                strlcpy(stb_cand.path, track, sizeof stb_cand.path);
                stb_cand.off = aa->pos;
                stb_cand.size = aa->size;
            }
        }
        close(fd);
        art_pause();

        bool search = rc <= 0 && meta && !searched;
        if (search) searched = true;
        if (search && search_albumart_files(&id3, "", artpath, sizeof artpath))
        {
            fd = open(artpath, O_RDONLY);
            if (fd >= 0)
            {
                bm->width = size; bm->height = size;
                size_t n = strlen(artpath);
                if (n > 4 && !strcasecmp(artpath + n - 4, ".png"))
                    rc = read_png_fd(fd, bm, (int)work_size, format);
                else if (n > 4 && !strcasecmp(artpath + n - 4, ".bmp"))
                    rc = read_bmp_fd(fd, bm, (int)work_size, format, NULL);
                else
                    rc = read_jpeg_fd(fd, 0, bm, (int)work_size, format, NULL, NULL);
                if (rc > 0) source = artpath;
                else if (!stb_cand.have && strcasecmp(artpath + (n > 4 ? n - 4 : 0), ".bmp"))
                {
                    /* the size from the open file: stat() bypasses the
                     * file layer's path mapping on some builds */
                    off_t end = lseek(fd, 0, SEEK_END);
                    if (end > 0)
                    {
                        stb_cand.have = true;
                        strlcpy(stb_cand.path, artpath, sizeof stb_cand.path);
                        stb_cand.off = 0;
                        stb_cand.size = (long)end;
                    }
                }
                close(fd);
            }
        }
        if (rc > 0 && bm->width > 0 && bm->height > 0 &&
            bm->width <= size && bm->height <= size)
        {
            stamp_source(&j->stamp, source);
            return true;
        }
    }
    sl_log("art: no decodable cover for album %d \"%s\" (%d tracks, first %s)", j->album,
           j->title, j->n_tracks, j->track[0]);
    return false;
}

/* Area-average downscale of an RGB565 image. */
static void scale565(const uint16_t *src, int sw, int sh, uint16_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++)
    {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++)
        {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++)
                {
                    uint16_t p = src[yy * sw + xx];
                    r += p >> 11; g += (p >> 5) & 63; b += p & 31; n++;
                }
            dst[y * dw + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
}

/* A saved thumbnail can fill Now Playing while Rockbox is still decoding
 * the full-size art. This avoids showing a placeholder for a visited album. */
bool sl_art_load_square(int album, int size, uint16_t *dst)
{
    if (!dst || size <= 0 || size > 480 || album < 0 || album >= sl_lib.n_albums)
        return false;
    const char *path = sl_art_src(album, SL_ART_GRID);
    int fd = path ? open(path + 2, O_RDONLY) : -1;
    if (fd < 0) return false;
    lv_image_header_t hdr;
    bool ok = read(fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr &&
              hdr.magic == LV_IMAGE_HEADER_MAGIC && hdr.cf == LV_COLOR_FORMAT_RGB565 &&
              hdr.w > 0 && hdr.w <= SL_ART_GRID && hdr.h == hdr.w && hdr.stride == hdr.w * 2;
    uint16_t *src = ok ? malloc((size_t)hdr.w * hdr.h * 2) : NULL;
    ok = src && read(fd, src, (size_t)hdr.w * hdr.h * 2) ==
                (ssize_t)((size_t)hdr.w * hdr.h * 2);
    close(fd);
    if (ok) scale565(src, hdr.w, hdr.h, dst, size, size);
    free(src);
    return ok;
}

/* Crop to a centred square (covers look right in square cells), then
 * write the image LVGL can load straight from the card. */
static bool write_thumb(int size, const uint16_t *px, int w, int h)
{
    int s = w < h ? w : h;
    int ox = (w - s) / 2, oy = (h - s) / 2;
    int out = size < s ? size : s;
    uint16_t *sq = malloc((size_t)s * s * 2);
    uint16_t *dst = malloc((size_t)out * out * 2);
    if (!sq || !dst) { free(sq); free(dst); return false; }
    for (int y = 0; y < s; y++)
        memcpy(sq + y * s, px + (y + oy) * w + ox, (size_t)s * 2);
    if (out == s) memcpy(dst, sq, (size_t)s * s * 2);
    else scale565(sq, s, s, dst, out, out);

    lv_image_header_t hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = LV_IMAGE_HEADER_MAGIC;
    hdr.cf = LV_COLOR_FORMAT_RGB565;
    hdr.w = out; hdr.h = out;
    hdr.stride = out * 2;

    char path[72], tmp[80];
    thumb_path_key(path, sizeof path, aj.generation, true, aj.key, size);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    thumb_mkdirs_key(aj.generation, aj.key);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok = fd >= 0;
    if (ok)
    {
        ok = write(fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr
          && write(fd, dst, (size_t)out * out * 2) == (ssize_t)((size_t)out * out * 2)
          && write(fd, &aj.stamp, sizeof aj.stamp) == (ssize_t)sizeof aj.stamp;
        if (close(fd) < 0) ok = false;
        if (ok) ok = rename(tmp, path) == 0;
        if (!ok) remove(tmp);
    }
    free(sq); free(dst);
    return ok;
}

/* An album whose art could not be decoded. Without a record, every boot
 * opened and parsed up to four of its songs and searched its folder again:
 * on a big card, hundreds of reads for nothing. The record holds a
 * fingerprint of everything that could bring art: the album's songs, the
 * files tried (size and time, so newly embedded art counts) and their
 * folders (a cover.jpg dropped in). Any change and the album is tried
 * again, so replacing music or covers never leaves art missing. */
/* Bumped whenever the decoders learn something (2: m4a covers that are
 * not JFIF), so albums noted under an older one are tried once more. */
#define ART_MISS_MAGIC 0x534d4932u  /* SMI2 */
struct art_miss { uint32_t magic; uint64_t print; };
static void miss_path(char *buf, int n, unsigned key)
{
    snprintf(buf, n, THUMB_DIR "/%08x.miss", key);
}
static uint64_t miss_print_track(uint64_t h, const char *track)
{
    struct stat st;
    int64_t v[3] = { -1, -1, source_dir_mtime(track) };
    if (!card_stat(track, &st)) { v[0] = st.st_size; v[1] = st.st_mtime; }
    return stamp_bytes(h, v, sizeof v);
}
/* The job's album, from its copies. Checked on the cover thread, not in
 * the pick: two card lookups a song, eight an album, and the UI loop's
 * step is 16 ms. */
static uint64_t miss_print(void)
{
    uint64_t h = aj.tracks;
    for (int i = 0; i < aj.tries; i++, art_pause())
        h = miss_print_track(h, aj.track[i]);
    return h;
}
static bool miss_known(void)
{
    char path[72];
    struct art_miss m;
    miss_path(path, sizeof path, aj.key);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = read(fd, &m, sizeof m) == (ssize_t)sizeof m && m.magic == ART_MISS_MAGIC;
    close(fd);
    if (ok && m.print == miss_print()) return true;
    remove(path);                       /* something changed: try again */
    return false;
}
static void miss_note(void)
{
    char path[72];
    struct art_miss m = { ART_MISS_MAGIC, miss_print() };
    miss_path(path, sizeof path, aj.key);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, &m, sizeof m) == (ssize_t)sizeof m;
    if (close(fd) < 0 || !ok) remove(path);
}

/* Make every size for one album. A failed decode is noted with the
 * fingerprint above and retried when the album's files change. */
static const int sizes[] = { SL_ART_GRID, SL_ART_PAGE, SL_ART_ROW };
static bool write_all_thumbs(const uint16_t *px, int w, int h)
{
    bool ok = true;
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++, art_pause())
        if (!write_thumb(sizes[i], px, w, h)) ok = false;
    return ok;
}

/* The job, from AJ_RUNNING to AJ_DONE, or to AJ_STB while stb_image has it.
 * A library reloaded meanwhile (another epoch or generation) gets nothing
 * written: its album is picked again if it still needs art. */
static bool job_stale(void) { return aj.epoch != art_epoch || aj.generation != thumb_generation; }
static void make_album(void)
{
    if (miss_known()) { aj.missed = true; aj.state = AJ_DONE; return; }
    struct bitmap bm;
    memset(&bm, 0, sizeof bm);
    size_t work_size = (size_t)SL_ART_GRID * SL_ART_GRID * 2 + JPEG_DECODE_OVERHEAD + 256 * 1024;
    unsigned char *work = malloc(work_size);
    if (!work) { aj.again = true; aj.state = AJ_DONE; return; }
    bool decoded = decode_art(&aj, &bm, SL_ART_GRID, work, work_size);
    uint16_t *px = NULL;
    int w = bm.width, h = bm.height;
    if (decoded)
    {
        /* the decoder may pad rows; copy out tightly packed */
        px = malloc((size_t)w * h * 2);
        if (px) memcpy(px, bm.data, (size_t)w * h * 2);
    }
    free(work);
    if (job_stale()) aj.again = true;
    else if (!decoded)
    {
        if (stb_cand.have && stb_start(aj.album, stb_cand.path, stb_cand.off, stb_cand.size))
        { aj.state = AJ_STB; return; }
        miss_note();
    }
    else aj.ok = px && write_all_thumbs(px, w, h);
    free(px);
    aj.state = AJ_DONE;
}

/* A finished background decode (sj_state() == 2): its thumbnails, or a miss. */
static void stb_finish(void)
{
    pthread_join(sj.thread, NULL);
    if (job_stale()) aj.again = true;
    else
    {
        /* the stamp lets a later library refresh keep these; without one
         * (the source cannot be stat'ed) they are still worth showing */
        if (sj.ok)
        {
            stamp_source(&aj.stamp, sj.source);
            aj.ok = write_all_thumbs(sj.px, sj.out_w, sj.out_h);
        }
        if (!aj.ok) miss_note();
    }
    free(sj.px); sj.px = NULL;
    sj_set(0);
    aj.state = AJ_DONE;
}

/* The album page's Big cover: one album's art bigger than any thumbnail,
 * decoded on this thread when the page asks, held until the page takes it
 * (or leaves). Only Rockbox's decoders: a cover only stb_image reads stays
 * the thumbnail, scaled up. */
enum { BJ_IDLE, BJ_QUEUED, BJ_RUNNING, BJ_DONE, BJ_FAILED };
static struct semaphore art_sem;
static struct art_job bj;
static volatile int bj_state;
static int bj_size, bj_w, bj_h;
static uint16_t *bj_px;
static void make_big(void)
{
    struct bitmap bm;
    memset(&bm, 0, sizeof bm);
    size_t work_size = (size_t)bj_size * bj_size * 2 + JPEG_DECODE_OVERHEAD + 256 * 1024;
    unsigned char *work = malloc(work_size);
    bool ok = work && decode_art(&bj, &bm, bj_size, work, work_size);
    uint16_t *px = ok ? malloc((size_t)bm.width * bm.height * 2) : NULL;
    if (px) { memcpy(px, bm.data, (size_t)bm.width * bm.height * 2); bj_w = bm.width; bj_h = bm.height; }
    free(work);
    bj_px = px;
    __atomic_store_n(&bj_state, px ? BJ_DONE : BJ_FAILED, __ATOMIC_RELEASE);
}
void sl_art_big_start(int album, int size)
{
    if (album < 0 || album >= sl_lib.n_albums || !art_thread_id || size <= SL_ART_GRID || size > 480) return;
    int st = __atomic_load_n(&bj_state, __ATOMIC_ACQUIRE);
    if (st == BJ_QUEUED || st == BJ_RUNNING) return;   /* one at a time; the page asks again */
    free(bj_px); bj_px = NULL;
    const struct sl_album *al = &sl_lib.albums[album];
    memset(&bj, 0, sizeof bj);
    bj.album = album;
    bj.n_tracks = al->n_tracks;
    bj.tries = al->n_tracks < JOB_TRIES ? al->n_tracks : JOB_TRIES;
    strlcpy(bj.title, al->title ? al->title : "", sizeof bj.title);
    for (int i = 0; i < bj.tries; i++) sl_track_path(al->tracks[i], bj.track[i], sizeof bj.track[i]);
    bj_size = size;
    __atomic_store_n(&bj_state, BJ_QUEUED, __ATOMIC_RELEASE);
    semaphore_release(&art_sem);
}
/* 0 working (or none asked), 1 made: *px (taken over, w x h), -1 none */
int sl_art_big_poll(int album, uint16_t **px, int *w, int *h)
{
    int st = __atomic_load_n(&bj_state, __ATOMIC_ACQUIRE);
    if (st == BJ_FAILED) { bj_state = BJ_IDLE; return -1; }
    if (st != BJ_DONE) return 0;
    bj_state = BJ_IDLE;
    if (bj.album != album) { free(bj_px); bj_px = NULL; return -1; }
    *px = bj_px; *w = bj_w; *h = bj_h;
    bj_px = NULL;
    return 1;
}
static struct semaphore art_sem;
static long art_stack[(DEFAULT_STACK_SIZE + 0x8000) / sizeof(long)];
static void art_thread(void)
{
    for (;;)
    {
        semaphore_wait(&art_sem, TIMEOUT_BLOCK);
        if (__atomic_load_n(&bj_state, __ATOMIC_ACQUIRE) == BJ_QUEUED)
        {
            bj_state = BJ_RUNNING;
            make_big();
        }
        if (aj.state != AJ_QUEUED) continue;
        aj.state = AJ_RUNNING;
        make_album();
        while (aj.state == AJ_STB)
        {
            if (sj_state() == 2) stb_finish();
            else sleep(HZ / 20);
        }
    }
}
static void art_thread_start(void)
{
    static bool started;
    if (started) return;
    /* without the thread (a test, or none to be had) the UI makes them */
    started = true;
    semaphore_init(&art_sem, 1, 0);
    art_thread_id = create_thread(art_thread, art_stack, sizeof art_stack, 0, "covers"
                                  IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
}

/* ------------------------------------------------ background worker */
static int next_album;
static int verify_album;
static bool verify_done;
static unsigned char *attempted;
static int attempted_count;
static struct watch { lv_obj_t *box; int album, size; } watches[512];
static int n_watches;

static void attach(lv_obj_t *boxo, const char *src)
{
    lv_obj_t *img = lv_image_create(boxo);
    lv_image_set_src(img, src);
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(img, lv_obj_get_style_radius(boxo, 0), 0);
    lv_obj_set_style_clip_corner(img, true, 0);
}

static void watch_deleted(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    for (int i = 0; i < n_watches; i++)
        if (watches[i].box == o) { watches[i] = watches[--n_watches]; i--; }
}

/* A cover at exactly size x size, scaled once into RAM from the nearest
 * thumbnail. Showing a thumbnail with lv_image_set_scale instead makes
 * LVGL transform every pixel again on each redraw, which the live tiles
 * do all the time. The pixels are freed with the image. */
static void scaled_deleted(lv_event_t *e)
{
    lv_image_dsc_t *dsc = lv_event_get_user_data(e);
    lv_image_cache_drop(dsc);            /* the next malloc may reuse this address */
    free(dsc);
}
bool sl_art_apply_scaled(lv_obj_t *boxo, int album, int size)
{
    if (album < 0 || album >= sl_lib.n_albums || size <= 0 || size > 480) return false;
    int thumb = sizes[0];                /* the smallest thumbnail at least this big */
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        if (sizes[i] >= size && sizes[i] < thumb) thumb = sizes[i];
    const char *path = sl_art_src(album, thumb);
    int fd = path ? open(path + 2, O_RDONLY) : -1;
    if (fd < 0) return false;
    lv_image_header_t hdr;
    bool ok = read(fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr &&
              hdr.magic == LV_IMAGE_HEADER_MAGIC && hdr.cf == LV_COLOR_FORMAT_RGB565 &&
              hdr.w > 0 && hdr.w <= SL_ART_GRID && hdr.h > 0 && hdr.h <= SL_ART_GRID && hdr.stride == hdr.w * 2;
    uint16_t *src = ok ? malloc((size_t)hdr.w * hdr.h * 2) : NULL;
    lv_image_dsc_t *dsc = src ? malloc(sizeof *dsc + (size_t)size * size * 2) : NULL;
    ok = dsc && read(fd, src, (size_t)hdr.w * hdr.h * 2) == (ssize_t)((size_t)hdr.w * hdr.h * 2);
    close(fd);
    if (!ok) { free(src); free(dsc); return false; }
    uint16_t *px = (uint16_t *)(dsc + 1);
    scale565(src, hdr.w, hdr.h, px, size, size);
    free(src);
    memset(dsc, 0, sizeof *dsc);
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = size;
    dsc->header.h = size;
    dsc->header.stride = size * 2;
    dsc->data = (const uint8_t *)px;
    dsc->data_size = (uint32_t)size * size * 2;
    lv_obj_t *img = lv_image_create(boxo);
    lv_image_set_src(img, dsc);
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(img, lv_obj_get_style_radius(boxo, 0), 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    lv_obj_add_event_cb(img, scaled_deleted, LV_EVENT_DELETE, dsc);
    return true;
}

/* ---- scaled covers made beside the UI (the home's live tiles) ----
 * A live tile takes a new cover at each peek and a mosaic at each swap.
 * Read from the card and scaled on the UI loop, that held it 60-240 ms
 * every two seconds on an idle home (test R1 logs, Nov 12-13): the read is
 * a system call, and Rockbox's own threads take turns with the UI on one
 * OS thread, so one of them would hold the loop as long. Here the loop finds
 * the file (sl_art_src may make a missing size), and a pthread of its own
 * reads it with the plain POSIX calls on the card's real path, as the
 * wallpaper maker does, and scales it; the loop puts the finished picture
 * in its box (sc_poll) or drops it when the box has gone. USB mode waits
 * for any read under way (sl_art_release). */
#pragma push_macro("open")
#pragma push_macro("read")
#pragma push_macro("close")
#undef open
#undef read
#undef close
enum { SC_FREE, SC_WORKING, SC_MADE, SC_FAILED };
#define SC_JOBS 6
static struct sc_job {
    int state;                     /* SC_*, atomic */
    lv_obj_t *box;                 /* NULL once the box has gone */
    int size;
    char real[MAX_PATH + 96];
    lv_image_dsc_t *dsc;
} sc_jobs[SC_JOBS];
static int sc_working;             /* jobs a thread holds a card file for, atomic */
static lv_timer_t *sc_timer;
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
int sl_art_scaled_jobs_made;       /* the tests: pictures that came from a thread */
#endif
/* the thread's part: the thumbnail read whole, then scaled to size x size */
static lv_image_dsc_t *sc_make(const char *real, int size)
{
    int fd = open(real, O_RDONLY);
    if (fd < 0) return NULL;
    lv_image_header_t hdr;
    bool ok = read(fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr &&
              hdr.magic == LV_IMAGE_HEADER_MAGIC && hdr.cf == LV_COLOR_FORMAT_RGB565 &&
              hdr.w > 0 && hdr.w <= SL_ART_GRID && hdr.h > 0 && hdr.h <= SL_ART_GRID && hdr.stride == hdr.w * 2;
    uint16_t *src = ok ? malloc((size_t)hdr.w * hdr.h * 2) : NULL;
    lv_image_dsc_t *dsc = src ? malloc(sizeof *dsc + (size_t)size * size * 2) : NULL;
    ok = dsc && read(fd, src, (size_t)hdr.w * hdr.h * 2) == (ssize_t)((size_t)hdr.w * hdr.h * 2);
    close(fd);
    if (!ok) { free(src); free(dsc); return NULL; }
    uint16_t *px = (uint16_t *)(dsc + 1);
    scale565(src, hdr.w, hdr.h, px, size, size);
    free(src);
    memset(dsc, 0, sizeof *dsc);
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = size;
    dsc->header.h = size;
    dsc->header.stride = size * 2;
    dsc->data = (const uint8_t *)px;
    dsc->data_size = (uint32_t)size * size * 2;
    return dsc;
}
#pragma pop_macro("open")
#pragma pop_macro("read")
#pragma pop_macro("close")
static void *sc_worker(void *arg)
{
    struct sc_job *j = arg;
    lv_image_dsc_t *dsc = sc_make(j->real, j->size);
    j->dsc = dsc;
    __atomic_store_n(&j->state, dsc ? SC_MADE : SC_FAILED, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&sc_working, 1, __ATOMIC_ACQ_REL);
    return NULL;
}
static void sc_box_deleted(lv_event_t *e)
{
    struct sc_job *j = lv_event_get_user_data(e);
    j->box = NULL;                       /* the picture, when it comes, is dropped */
}
static void sc_attach(lv_obj_t *boxo, lv_image_dsc_t *dsc)
{
    lv_obj_t *img = lv_image_create(boxo);
    lv_image_set_src(img, dsc);
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(img, lv_obj_get_style_radius(boxo, 0), 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    lv_obj_add_event_cb(img, scaled_deleted, LV_EVENT_DELETE, dsc);
}
/* finished pictures into their boxes; a gone box's picture freed */
static void sc_poll(lv_timer_t *t)
{
    (void)t;
    for (int i = 0; i < SC_JOBS; i++)
    {
        struct sc_job *j = &sc_jobs[i];
        int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
        if (st != SC_MADE && st != SC_FAILED) continue;
        if (j->box)
        {
            lv_obj_remove_event_cb_with_user_data(j->box, sc_box_deleted, j);
            if (st == SC_MADE) { sc_attach(j->box, j->dsc); j->dsc = NULL;
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
                sl_art_scaled_jobs_made++;
#endif
            }
        }
        free(j->dsc);
        j->dsc = NULL;
        j->box = NULL;
        __atomic_store_n(&j->state, SC_FREE, __ATOMIC_RELEASE);
    }
}
bool sl_art_apply_scaled_async(lv_obj_t *boxo, int album, int size)
{
    if (album < 0 || album >= sl_lib.n_albums || size <= 0 || size > 480) return false;
    int thumb = sizes[0];                /* the smallest thumbnail at least this big */
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        if (sizes[i] >= size && sizes[i] < thumb) thumb = sizes[i];
    const char *path = sl_art_src(album, thumb);
    const char *root = sl_net_card_root();
    if (!path || !root) return false;
    struct sc_job *j = NULL;
    for (int i = 0; i < SC_JOBS && !j; i++)
        if (__atomic_load_n(&sc_jobs[i].state, __ATOMIC_ACQUIRE) == SC_FREE) j = &sc_jobs[i];
    if (!j || snprintf(j->real, sizeof j->real, "%s%s", root, path + 2) >= (int)sizeof j->real)
        return sl_art_apply_scaled(boxo, album, size);   /* all busy: as before, here */
    if (!sc_timer) sc_timer = lv_timer_create(sc_poll, 20, NULL);
    j->box = boxo;
    j->size = size;
    j->dsc = NULL;
    __atomic_store_n(&j->state, SC_WORKING, __ATOMIC_RELEASE);
    __atomic_add_fetch(&sc_working, 1, __ATOMIC_ACQ_REL);
    pthread_attr_t a;
    pthread_t th;
    bool started = !pthread_attr_init(&a);
    if (started)
    {
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        started = !pthread_create(&th, &a, sc_worker, j);
        pthread_attr_destroy(&a);
    }
    if (!started)
    {
        __atomic_sub_fetch(&sc_working, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&j->state, SC_FREE, __ATOMIC_RELEASE);
        j->box = NULL;
        return sl_art_apply_scaled(boxo, album, size);
    }
    lv_obj_add_event_cb(boxo, sc_box_deleted, LV_EVENT_DELETE, j);
    return true;
}
/* USB mode: no thread may hold a card file when the card goes (3 s at most) */
static void sc_settle(void)
{
    for (int waited = 0; __atomic_load_n(&sc_working, __ATOMIC_ACQUIRE) > 0 && waited < 3000; waited += 2) usleep(2000);
}

/* The colour a cover reads as, from its smallest thumbnail: the hue the
 * most colourful pixels share (a histogram weighted by how coloured each
 * is, so a red sleeve on a white border reads red), and how coloured that
 * is overall, so a grey sleeve gets a grey wash. */
bool sl_art_hue(int album, int *hue, int *sat)
{
    const char *path = sl_art_src(album, SL_ART_ROW);
    int fd = path ? open(path + 2, O_RDONLY) : -1;
    if (fd < 0) return false;
    lv_image_header_t hdr;
    static uint16_t px[SL_ART_ROW * SL_ART_ROW];
    bool ok = read(fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr && hdr.magic == LV_IMAGE_HEADER_MAGIC &&
              hdr.cf == LV_COLOR_FORMAT_RGB565 && hdr.w > 0 && hdr.h > 0 &&
              (size_t)hdr.w * hdr.h <= sizeof px / sizeof px[0] && hdr.stride == hdr.w * 2;
    size_t n = ok ? (size_t)hdr.w * hdr.h : 0;
    ok = ok && read(fd, px, n * 2) == (ssize_t)(n * 2);
    close(fd);
    if (!ok) return false;
    uint32_t bins[36] = { 0 };
    uint64_t chroma = 0;
    for (size_t i = 0; i < n; i++)
    {
        int r = (px[i] >> 11) << 3, g = ((px[i] >> 5) & 63) << 2, b = (px[i] & 31) << 3;
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        int c = mx - mn;
        chroma += (uint64_t)c;
        if (c < 24) continue;            /* greys, whites and blacks say nothing of the hue */
        int h = mx == r ? 60 * (g - b) / c : mx == g ? 120 + 60 * (b - r) / c : 240 + 60 * (r - g) / c;
        if (h < 0) h += 360;
        bins[(h / 10) % 36] += (uint32_t)(c * c);
    }
    int best = 0;
    for (int k = 1; k < 36; k++) if (bins[k] > bins[best]) best = k;
    /* the peak and its two neighbours, averaged by weight */
    uint64_t wl = bins[(best + 35) % 36], wm = bins[best], wr = bins[(best + 1) % 36], w = wl + wm + wr;
    *hue = w ? (int)(((int64_t)(best * 10 + 5) * (int64_t)w + ((int64_t)wr - (int64_t)wl) * 10) / (int64_t)w + 360) % 360 : 0;
    int avg = (int)(chroma / n);         /* 0..255 */
    *sat = !w ? 0 : avg > 120 ? 100 : avg * 100 / 120;
    return true;
}

/* Put art on a cover box now, or as soon as it has been made. */
void sl_art_apply(lv_obj_t *boxo, int album, int size)
{
    if (album < 0 || album >= sl_lib.n_albums) return;
    const char *src = sl_art_src(album, size);
    if (src) { attach(boxo, src); return; }
    /* 4: being made now, so worth a watch */
    if ((album < attempted_count && attempted[album] && attempted[album] != 4)
        || n_watches >= (int)(sizeof watches / sizeof watches[0]))
        return;
    watches[n_watches].box = boxo;
    watches[n_watches].album = album;
    watches[n_watches].size = size;
    n_watches++;
    lv_obj_add_event_cb(boxo, watch_deleted, LV_EVENT_DELETE, NULL);
}

/* A step's share of the UI loop. Card lookups cost 4 ms or more each on
 * the R1, so a count of albums is no limit: 24 of them, after a verify
 * pass, held the loop 404 ms. One share for the whole step (pick, check,
 * prune), and past it only the unit under way: 16 ms and one cover moved
 * (six lookups) stays near 30 ms on a 2 ms card. */
static struct timespec art_t0;
static bool art_over(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - art_t0.tv_sec) * 1000L + (t.tv_nsec - art_t0.tv_nsec) / 1000000L >= 16;
}

/* Albums on screen first: anything being watched goes before the rest.
 * 1: a size is missing, make it (whether the album has art at all is the
 * cover thread's to check: miss_known). -1: the step's time ran out
 * between sizes (after a library update each one is moved). */
unsigned sl_art_made;
/* A lookup slower than the step's share (a test R1: 100-485 ms each, the
 * card busy and memory short) ran the time out after the first size, every
 * step, and the same album was begun again and again, never got past. The
 * sizes found are kept: the next step goes on from the one it stopped at.
 * whole: every size whatever the time (Cover Flow's first neighbour). */
static bool art_whole;
static int needs_album = -1;
static unsigned needs_size;
static int needs_art(int album)
{
    if (album < 0 || album >= sl_lib.n_albums || attempted[album]) return 0;
    unsigned i = album == needs_album ? needs_size : 0;
    needs_album = -1;
    for (; i < sizeof sizes / sizeof sizes[0]; i++)
    {
        if (i && !art_whole && art_over()) { needs_album = album; needs_size = i; return -1; }
        if (!sl_art_src(album, sizes[i])) return 1;
    }
    /* every size is on the card: never look again (three card lookups each
     * time, and Cover Flow asked about the same few dozen every step) */
    attempted[album] = 3;
    return 0;
}

/* Albums someone is about to show (Cover Flow's neighbours), newest first:
 * they go before the watches and the walk through the library. */
#define PREFER_MAX 24
static int prefer[PREFER_MAX], n_prefer;
void sl_art_prefer(int album)
{
    if (album < 0 || album >= sl_lib.n_albums) return;
    for (int i = 0; i < n_prefer; i++)
        if (prefer[i] == album) return;
    if (n_prefer == PREFER_MAX) n_prefer--;           /* drop the oldest */
    memmove(prefer + 1, prefer, (size_t)n_prefer * sizeof prefer[0]);
    prefer[0] = album;
    n_prefer++;
}

/* The walk's card lookups, on a thread of their own. Each album's check is
 * up to three lookups on the card; on a test R1 they held the UI loop
 * 100-485 ms a step "in covers", and a whole walk that finds nothing to
 * make is thousands of them. Rockbox's threads take turns on one OS thread,
 * so the cover thread would only move the wait: this is a pthread, with
 * plain POSIX on the card's real path, no LVGL and no library. The UI
 * hands it a batch (the watched albums, then the walk's next), each as the
 * key its thumbnails are named by, and later collects the first one with a
 * size missing; that album alone goes through needs_art() as before (a
 * cover moved from the last generation, the small one made from the big).
 * A library reloaded meanwhile (another epoch or generation) gets nothing. */
#define FIND_BATCH 64
static struct find_job {
    int state;                       /* 0 idle, 1 looking, 2 done */
    bool cancel;
    unsigned epoch;
    uint32_t generation;
    int n, found;                    /* found: the first with a size missing, n for none */
    int album[FIND_BATCH], walk[FIND_BATCH];   /* walk: its place in the walk, -1 a watch */
    unsigned key[FIND_BATCH];
    char root[MAX_PATH];
    pthread_t thread;
} fj;
/* No thread to be had (on the R1 memory is committed so tightly that
 * fork() fails): the UI looks itself, as before, and the thread is tried
 * again 30 s later. */
#define FIND_RETRY_MS 30000
static bool find_failed;
static long find_failed_ms, find_logged_ms;
static int find_pend = -1, find_pend_walk;
static long find_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static bool find_retry_due(void)
{
    if (find_failed && find_now_ms() - find_failed_ms >= FIND_RETRY_MS) find_failed = false;
    return !find_failed;
}
static int fj_state(void) { return __atomic_load_n(&fj.state, __ATOMIC_ACQUIRE); }
#if defined(HIBY_R1) && !defined(SIMULATOR)
static bool find_exists(const char *path)
{
    char real[MAX_PATH + 96];
    snprintf(real, sizeof real, "%s%s", fj.root, path);
    return access(real, F_OK) == 0;
}
static bool find_on_thread(void) { return find_retry_due(); }
static int (*find_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = pthread_create;
#else
static bool (*art_find_exists)(const char *path);   /* the tests' card, off the UI; none: the UI looks */
static bool find_exists(const char *path) { return art_find_exists(path); }
static bool find_on_thread(void) { return art_find_exists && find_retry_due(); }
static int (*find_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = pthread_create;   /* the tests make it fail */
#endif
static void *find_worker(void *arg)
{
    (void)arg;
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15);   /* after audio and the UI */
#endif
    int i = 0;
    for (; i < fj.n && !__atomic_load_n(&fj.cancel, __ATOMIC_ACQUIRE); i++)
    {
        bool all = true;
        for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0] && all; k++)
        {
            char path[72];
            thumb_path_key(path, sizeof path, fj.generation, true, fj.key[i], sizes[k]);
            all = find_exists(path);
        }
        if (!all) break;
    }
    fj.found = i;
    __atomic_store_n(&fj.state, 2, __ATOMIC_RELEASE);
    return NULL;
}
/* USB mode or a reload: a batch under way stops at its next lookup */
static void find_settle(void)
{
    if (!fj_state()) return;
    __atomic_store_n(&fj.cancel, true, __ATOMIC_RELEASE);
    pthread_join(fj.thread, NULL);
    fj.state = 0;
}
static int find_pick(void)
{
    if (fj_state() == 1) return -2;          /* still looking */
    if (fj_state() == 2)
    {
        pthread_join(fj.thread, NULL);
        fj.state = 0;
        if (fj.epoch == art_epoch && fj.generation == thumb_generation && !fj.cancel)
        {
            for (int i = 0; i < fj.found; i++)
            {
                int a = fj.album[i];
                if (a >= 0 && a < attempted_count && !attempted[a]) attempted[a] = 3;   /* all made: never again */
                if (fj.walk[i] >= next_album) next_album = fj.walk[i] + 1;
            }
            if (fj.found < fj.n)
            {
                find_pend = fj.album[fj.found];
                find_pend_walk = fj.walk[fj.found];
                if (find_pend_walk >= 0) next_album = find_pend_walk;
            }
        }
    }
    /* the album found with a size missing: looked at here, over as many
     * steps as its lookups take (a cover moved from the last generation, the
     * small one made from the big) */
    if (find_pend >= 0)
    {
        int a = find_pend;
        int need = a < attempted_count ? needs_art(a) : 0;
        if (need < 0) return -2;
        find_pend = -1;
        if (need) return a;
        if (find_pend_walk >= 0 && find_pend_walk >= next_album) next_album = find_pend_walk + 1;   /* there after all */
        return -2;
    }
    /* the next batch: albums on screen first, then the walk */
    int n = 0;
    for (int i = 0; i < n_watches && n < FIND_BATCH / 4; i++)
    {
        int a = watches[i].album, k = 0;
        if (a < 0 || a >= attempted_count || attempted[a]) continue;
        while (k < n && fj.album[k] != a) k++;
        if (k < n) continue;
        fj.album[n] = a; fj.walk[n] = -1; fj.key[n] = album_key(a); n++;
    }
    int w = next_album;
    for (; w < sl_lib.n_albums && n < FIND_BATCH; w++)
    {
        int a = sl_lib.albums_by_title[w];
        if (a < 0 || a >= attempted_count || attempted[a]) continue;
        fj.album[n] = a; fj.walk[n] = w; fj.key[n] = album_key(a); n++;
    }
    if (!n) { next_album = sl_lib.n_albums; return -1; }
    fj.n = n;
    fj.found = n;
    fj.epoch = art_epoch;
    fj.generation = thumb_generation;
    fj.cancel = false;
#if defined(HIBY_R1) && !defined(SIMULATOR)
    strlcpy(fj.root, sl_net_card_root(), sizeof fj.root);
#endif
    __atomic_store_n(&fj.state, 1, __ATOMIC_RELEASE);
    /* a small stack: the default reserves 8 MB (RLIMIT_STACK) of a memory
     * so tight on the R1 that fork() fails. The worker's frame is a path or
     * two (under 1 KiB) and access(). */
    pthread_attr_t attr;
    bool made = pthread_attr_init(&attr) == 0;
    if (made)
    {
        pthread_attr_setstacksize(&attr, 64 * 1024);
        made = find_create(&fj.thread, &attr, find_worker, NULL) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!made)
    {
        fj.state = 0;
        find_failed = true;
        find_failed_ms = find_now_ms();
        if (!find_logged_ms || find_failed_ms - find_logged_ms >= 60000)
        {
            find_logged_ms = find_failed_ms;
            sl_log("art: no thread for the walk: the UI looks itself, the thread tried again in 30 s");
        }
    }
    return -2;
}

static int pick_album(void)
{
    /* the step's time, as the walk below: on a full card under memory
     * pressure these lookups held the loop 1.3 s (the R1 log, 2026-10-13).
     * The first of Cover Flow's is looked at whatever the time, and whole:
     * never shown as a placeholder. */
    for (bool first = true; n_prefer; first = false)
    {
        if (!first && art_over()) return -2;
        int a = prefer[0];
        art_whole = first;
        int need = attempted_count == sl_lib.n_albums ? needs_art(a) : 0;
        art_whole = false;
        if (need < 0) return -2;             /* still first next step */
        memmove(prefer, prefer + 1, (size_t)--n_prefer * sizeof prefer[0]);
        if (need) return a;
    }
    if (find_on_thread()) return find_pick();
    for (int i = 0; i < n_watches; i++)
    {
        if (art_over()) return -2;
        int need = needs_art(watches[i].album);
        if (need < 0) return -2;
        if (need) return watches[i].album;
    }
    /* Each check is up to three file lookups; with every thumbnail cached,
     * walking thousands of albums in one step froze the UI. Resume later. */
    while (next_album < sl_lib.n_albums)
    {
        if (art_over()) return -2;
        int a = sl_lib.albums_by_title[next_album];
        int need = needs_art(a);
        if (need < 0) return -2;
        if (need) return a;
        next_album++;
    }
    return -1;
}

/* The album the thread is to make, copied out of the library. */
static void job_fill(int a)
{
    const struct sl_album *al = &sl_lib.albums[a];
    aj.album = a;
    aj.epoch = art_epoch;
    aj.key = album_key(a);
    aj.generation = thumb_generation;
    aj.tracks = album_tracks_stamp(a);
    aj.n_tracks = al->n_tracks;
    aj.tries = al->n_tracks < JOB_TRIES ? (al->n_tracks > 0 ? al->n_tracks : 0) : JOB_TRIES;
    strlcpy(aj.title, al->title ? al->title : "", sizeof aj.title);
    for (int i = 0; i < aj.tries; i++)
        sl_track_path(al->tracks[i], aj.track[i], sizeof aj.track[i]);
    aj.ok = aj.again = aj.missed = false;
}

/* A made album onto the screen. False while one is still being made. */
static void art_made(int a, bool ok);
static bool art_collect(void)
{
    if (!art_thread_id && aj.state == AJ_STB && sj_state() == 2) stb_finish();
    if (aj.state == AJ_IDLE) return true;
    if (aj.state != AJ_DONE) return false;
    aj.state = AJ_IDLE;
    int a = aj.album;
    if (aj.epoch != art_epoch || a < 0 || a >= attempted_count) return true;
    if (aj.again) { attempted[a] = 0; return true; }
    if (aj.missed) { attempted[a] = 2; return true; }
    attempted[a] = 1;
    art_made(a, aj.ok);
    return true;
}

/* Do one album. Returns false when there is nothing left to do.
 * Runs on the UI loop: a step that holds it long is noted in the log. */
static bool art_step(int *album, const char **phase);
bool sl_art_step(void)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    art_t0 = t0;
    int album = -1;
    const char *phase = "idle";
    bool more = art_step(&album, &phase);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    if (ms >= 400)
        sl_log("art: slow step, %ld ms in %s (album %d \"%s\")", ms, phase, album,
               album >= 0 && album < sl_lib.n_albums && sl_lib.albums[album].title ?
               sl_lib.albums[album].title : "");
    return more;
}
/* The next album to make, handed to the cover thread. */
static bool art_pick(int *album, const char **phase)
{
    if (attempted_count != sl_lib.n_albums)
    {
        free(attempted);
        attempted_count = 0;
        attempted = calloc(sl_lib.n_albums, 1);
        if (!attempted) return false;
        attempted_count = sl_lib.n_albums;
        next_album = 0;
    }
    *phase = "pick";
    *album = -1;
    SL_AT("art pick");
    int a = pick_album();
    if (a == -2) return true;   /* more albums to check on the next step */
    if (a < 0) return false;
    /* An unwritable/full card or a decode failure must not keep selecting
     * this same album every frame and prevent later albums from loading. */
    attempted[a] = 4;
    *phase = "make";
    SL_AT("art make");
    *album = a;
    job_fill(a);
    if (art_thread_id)
    {
        aj.state = AJ_QUEUED;
        semaphore_release(&art_sem);
        return true;
    }
    aj.state = AJ_RUNNING;
    make_album();
    art_collect();
    return true;
}

static int verify_size;
static bool art_step(int *album, const char **phase)
{
    /* A made album onto the screen, and its line in the log. At 123a36bb27
     * that line's fsync, on a card busy with the covers just written, held
     * the loop 2-3 s, logged as the pick (R1, 2026-10-23): named apart. */
    *phase = "collect";
    SL_AT("art collect");
    if (aj.state == AJ_DONE) *album = aj.album;
    bool more = !art_collect();             /* one album at a time */
    /* The pick first, whatever the time: Cover Flow's neighbours (prefer)
     * must be made at once, never shown as placeholders. */
    if (!more && sl_lib.n_albums) more = art_pick(album, phase);
    /* In what is left of the step: every surviving album checked (its
     * covers moved into the new generation) before the old one is deleted,
     * a size at a time. This continues even if decoding runs out of memory. */
    if (thumb_prune_pending && !verify_done)
    {
        if (!art_over()) { *phase = "verify"; *album = verify_album; SL_AT("art verify"); }
        while (verify_album < sl_lib.n_albums && !art_over())
        {
            /* one album held the R1 ~400 ms here: name it, apart from the pick */
            struct timespec v0, v1;
            clock_gettime(CLOCK_MONOTONIC, &v0);
            sl_art_src(verify_album, sizes[verify_size]);
            clock_gettime(CLOCK_MONOTONIC, &v1);
            long ms = (v1.tv_sec - v0.tv_sec) * 1000L + (v1.tv_nsec - v0.tv_nsec) / 1000000L;
            if (ms >= 100)
                sl_log("art: verify took %ld ms on album %d \"%s\"", ms, verify_album,
                       sl_lib.albums[verify_album].title ? sl_lib.albums[verify_album].title : "");
            if (++verify_size == (int)(sizeof sizes / sizeof sizes[0])) { verify_size = 0; verify_album++; }
        }
        verify_done = verify_album >= sl_lib.n_albums;
        if (!verify_done) return true;
    }
    if (!verify_done || !thumb_prune_pending) return more;
    if (art_over()) return true;
    *phase = "prune";
    SL_AT("art prune");
    return prune_thumbs() || more;
}

bool sl_art_busy(void)
{
    return aj.state != AJ_IDLE;
}

/* USB mode: the thread may be in the middle of a song, its file open,
 * which would keep the card mounted. One not begun is put back; one being
 * made is let finish (the UI sleeping gives it the time), 5 s at most. */
static void art_job_settle(void)
{
    find_settle();
    if (aj.state == AJ_QUEUED)
    {
        aj.state = AJ_IDLE;
        if (aj.epoch == art_epoch && aj.album >= 0 && aj.album < attempted_count)
            attempted[aj.album] = 0;
    }
    for (int i = 0; aj.state == AJ_RUNNING && i < 100; i++)
        sleep(HZ / 20);
}

bool sl_art_no_cover(int album)
{
    return album >= 0 && album < attempted_count && attempted[album] == 2;
}
/* 1 the album has a cover made, 0 it was looked at and has none, -1 not
 * looked at yet (the cover fetcher asks) */
int sl_art_state(int album)
{
    if (album < 0 || album >= attempted_count) return -1;
    int v = attempted[album];
    return v == 1 || v == 3 ? 1 : v == 2 ? 0 : -1;
}
/* a cover put in the album's folder: looked for again, soon */
void sl_art_again(int album)
{
    if (album >= 0 && album < attempted_count) attempted[album] = 0;
    sl_art_prefer(album);
}

static void art_made(int a, bool ok)
{
    if (ok) sl_art_made++;
    else if (a >= 0 && a < attempted_count) attempted[a] = 2;
    for (int i = 0; i < n_watches; i++)
    {
        if (watches[i].album != a) continue;
        const char *src = sl_art_src(a, watches[i].size);
        if (src) attach(watches[i].box, src);
        lv_obj_remove_event_cb(watches[i].box, watch_deleted);
        watches[i] = watches[--n_watches];
        i--;
    }
    sl_log("art: album %d %s", a, ok ? "done" : "no usable cover or cache write failed");
}

void sl_art_reset(void)
{
    find_settle();                           /* its albums are the old library's */
    find_pend = needs_album = -1;
    n_prefer = 0;
    art_epoch++;                             /* a decode in flight is for the old library */
    for (int i = 0; i < n_watches; i++)
        lv_obj_remove_event_cb(watches[i].box, watch_deleted);
    next_album = 0;
    verify_album = verify_size = 0;
    verify_done = false;
    n_watches = 0;
    free(attempted); attempted = NULL; attempted_count = 0;
    if (aj.state == AJ_QUEUED) aj.state = AJ_IDLE;   /* not begun: for the old library */
}
