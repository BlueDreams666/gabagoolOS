/* Build with the sim include flags plus firmware/common/inflate.c, adler32.c
 * and crc32.c: it includes recorder/png_load.c itself.
 *
 * A cover is decoded on the music's threads (bufopen of the album art for
 * each track as it buffers). Rockbox's threads take turns, so a decode
 * that never yields holds the UI and the music the whole time: a test R1
 * logged "the music's threads held on for 117-137 ms" again and again
 * after a 4,249-song shuffle started, one new album per track. This times
 * the longest stretch between yields for a large PNG cover scaled to the
 * player's art slot. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../recorder/png_load.c"
#undef open
#undef close
#undef read
#undef lseek
/* the file layer and the scaler's one helper, as the player has them */
int sim_open(const char *p, int f, ...) { return open(p, f, 0666); }
int sim_close(int fd) { return close(fd); }
ssize_t sim_read(int fd, void *b, size_t n) { return read(fd, b, n); }
off_t sim_lseek(int fd, off_t o, int w) { return lseek(fd, o, w); }
int recalc_dimension(struct dim *dst, struct dim *src)
{
    if (src->width * dst->height > src->height * dst->width) dst->height = src->height * dst->width / src->width;
    else dst->width = src->width * dst->height / src->height;
    return 0;
}
void __assert(const char *file, int line, const char *expr) { fprintf(stderr, "%s:%d: %s\n", file, line, expr); abort(); }

static long long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}
static long long last, longest;
static int yields;
void yield(void)
{
    long long n = now_us();
    if (n - last > longest) longest = n - last;
    last = n;
    yields++;
}

static void put32(unsigned char *b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }
static uint32_t crc(uint32_t c, const unsigned char *b, size_t n)
{
    c = ~c;
    while (n--) { c ^= *b++; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; }
    return ~c;
}
static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char b[4];
    put32(b, len); fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc(0, (const unsigned char *)type, 4);
    if (len) c = crc(c, data, len);
    put32(b, (uint32_t)c); fwrite(b, 1, 4, f);
}

int main(void)
{
    const int side = 1600;               /* a big scan, as covers on a card often are */
    const char *path = "/tmp/sloppy-png-yield-test.png";
    size_t raw_n = (size_t)side * (side * 3 + 1);
    unsigned char *raw = malloc(raw_n);
    for (int y = 0; y < side; y++)
    {
        unsigned char *r = raw + (size_t)y * (side * 3 + 1);
        r[0] = y % 5;                    /* every filter type */
        for (int x = 0; x < side * 3; x++) r[1 + x] = (unsigned char)(x * 7 + y * 13 + (x * y >> 5));
    }
    /* zlib with stored blocks: no compressor needed, and inflate still runs */
    size_t z_n = 0, blocks = (raw_n + 65534) / 65535;
    unsigned char *z = malloc(raw_n + blocks * 5 + 6);
    z[z_n++] = 0x78; z[z_n++] = 0x01;
    for (size_t at = 0; at < raw_n; at += 65535)
    {
        size_t n = raw_n - at < 65535 ? raw_n - at : 65535;
        z[z_n++] = at + n >= raw_n;
        z[z_n++] = n & 0xff; z[z_n++] = n >> 8; z[z_n++] = ~n & 0xff; z[z_n++] = (~n >> 8) & 0xff;
        memcpy(z + z_n, raw + at, n); z_n += n;
    }
    uint32_t a1 = 1, a2 = 0;
    for (size_t i = 0; i < raw_n; i++) { a1 = (a1 + raw[i]) % 65521; a2 = (a2 + a1) % 65521; }
    put32(z + z_n, a2 << 16 | a1); z_n += 4;
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    unsigned char ihdr[13] = { 0 };
    put32(ihdr, side); put32(ihdr + 4, side); ihdr[8] = 8; ihdr[9] = 2;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)z_n);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);

    const int slot = 480;                /* the player's album art slot, W x W */
    size_t max = (size_t)slot * slot * 4 + 65536;
    struct bitmap bm = { .width = slot, .height = slot };
#if (LCD_DEPTH > 1)
    bm.format = FORMAT_NATIVE;
#endif
    bm.data = malloc(max);
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    long long t0 = now_us();
    last = t0;
    int rc = read_png_fd(fd, &bm, (int)max, FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT);
    long long total = now_us() - t0;
    if (total - (last - t0) > longest) longest = total - (last - t0);   /* the stretch after the last yield */
    close(fd);
    unlink(path);
    assert(rc > 0);
    printf("png cover %dx%d to %dx%d: %lld us in all, %d yields, longest stretch without one %lld us (this machine)\n",
           side, side, bm.width, bm.height, total, yields, longest);
    assert(yields >= side / 16 && longest * 8 <= total);   /* split into many short turns */
    printf("PASS: a PNG cover decodes in short turns, the music's threads and the UI get theirs\n");
    return 0;
}
