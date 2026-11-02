/* DSD in WavPack: libwavpack/wvdsd.c and dsd.c's WavPack path, against
 * the DSF files they were made from (gen_dsd.py; WavPack 5.6.0 made the
 * .wv files in tests/wvdsd/).
 *   1. every mode (raw, fast, high), stereo, false stereo, mono, DSD64
 *      and DSD128 unpacks bit for bit to the source's DSD and each
 *      block's CRC matches;
 *   2. the metadata parser hands DSD WavPack to the DSD decoder at the
 *      right rate and length;
 *   3. dsd.c's PCM from the .wv is sample for sample what it makes from
 *      the source .dsf (the same CIC decimation);
 *   4. a seek lands on the block holding the target and decodes on from
 *      there exactly;
 *   5. damaged files (bytes flipped, cut short, garbage) never read out of
 *      bounds or hang, and keep their length;
 *   6. decode speed per mode.
 * usage: wvdsd_test <vector-dir> <dsf-dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "codecs.h"
#include "metadata.h"
#include "libwavpack/wvdsd.h"

struct codec_api *ci;
static struct codec_api api;
static struct mp3entry id3;

static unsigned char *file_data;
static size_t file_size, file_pos;
static int32_t *out;
static size_t out_frames, out_cap;
static unsigned long elapsed, rate_set;
static long seek_at_frame = -1, seek_ms;
static size_t seek_frames;   /* frames out before the seek */
static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static size_t f_read(void *p, size_t n)
{
    if (n > file_size - file_pos) n = file_size - file_pos;
    memcpy(p, file_data + file_pos, n);
    file_pos += n;
    api.curpos = file_pos;
    return n;
}
static bool f_seek(size_t pos)
{
    if (pos > file_size) return false;
    file_pos = pos;
    api.curpos = pos;
    return true;
}
static unsigned long seek_elapsed;
static void f_seek_complete(void) { seek_elapsed = elapsed; }
static void f_elapsed(unsigned long v) { elapsed = v; }
static void f_configure(int s, intptr_t v) { if (s == DSP_SET_FREQUENCY) rate_set = v; }
static long f_command(intptr_t *param)
{
    if (seek_at_frame >= 0 && (long)out_frames >= seek_at_frame)
    {
        seek_at_frame = -1;
        seek_frames = out_frames;
        *param = seek_ms;
        return CODEC_ACTION_SEEK_TIME;
    }
    return CODEC_ACTION_NULL;
}
static void f_insert(const void *ch1, const void *ch2, int count)
{
    (void)ch2;
    if (out_frames + count > out_cap)
    {
        out_cap = (out_frames + count) * 2;
        out = realloc(out, out_cap * 2 * sizeof *out);
    }
    memcpy(out + out_frames * 2, ch1, count * 2 * sizeof *out);
    out_frames += count;
}
static void *f_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *f_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *f_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static int f_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }

/* codeclib and metadata helpers the two files use */
int codec_init(void) { return 0; }
void codec_set_replaygain(const struct mp3entry *e) { (void)e; }
size_t codec_strlen(const char *s) { return strlen(s); }
uint32_t get_long_le(void *buf)
{
    unsigned char *p = buf;
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
bool read_ape_tags(int fd, struct mp3entry *e) { (void)fd; (void)e; return true; }
off_t sim_filesize(int fd) { struct stat st; return fstat(fd, &st) ? -1 : st.st_size; }
bool get_wavpack_metadata(int fd, struct mp3entry *id3);
enum codec_status codec_main(enum codec_entry_call_reason reason);
enum codec_status codec_run(void);

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL can't open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) { printf("FAIL short read %s\n", path); exit(1); }
    fclose(f);
    return b;
}

/* the source's DSD, MSB first, a channel at a time */
struct ref { unsigned char *ch[2]; size_t n; int chans; unsigned rate; };
static struct ref load_dsf(const char *path)
{
    size_t n;
    unsigned char *b = load(path, &n);
    struct ref r = { { NULL, NULL }, 0, b[52] | b[53] << 8, b[56] | b[57] << 8 | b[58] << 16 | (unsigned)b[59] << 24 };
    unsigned long long bits = 0;
    memcpy(&bits, b + 64, 8);
    r.n = bits / 8;
    for (int c = 0; c < r.chans; c++) r.ch[c] = malloc(r.n);
    const unsigned char *d = b + 92;
    for (size_t i = 0; i < r.n; i++)
        for (int c = 0; c < r.chans; c++)
        {
            unsigned char v = d[(i / 4096) * 4096 * r.chans + c * 4096 + i % 4096], o = 0;
            for (int k = 0; k < 8; k++) if (v & 1 << k) o |= 0x80 >> k;
            r.ch[c][i] = o;
        }
    if (r.chans == 1) r.ch[1] = r.ch[0];
    free(b);
    return r;
}

static int32_t mem_read(void *p, int32_t n) { return (int32_t)f_read(p, n); }

static void set_file(unsigned char *d, size_t n)
{
    file_data = d; file_size = n; file_pos = 0;
    api.filesize = n; api.curpos = 0;
}

static WavpackDSD dec;

static void unpack_check(const char *name, unsigned char *wv, size_t wvn, struct ref *r)
{
    static unsigned char L[1 << 20], R[1 << 20];
    set_file(wv, wvn);
    CHECK(wvdsd_open(&dec, mem_read), "%s: open", name);
    CHECK(wvdsd_byte_rate(&dec) * 8 == r->rate, "%s: rate %u, want %u", name, wvdsd_byte_rate(&dec) * 8, r->rate);
    size_t got = 0;
    int n;
    while ((n = wvdsd_unpack(&dec, L + got, R + got, 1000)) > 0 && got < sizeof L - 1000) got += n;
    CHECK(got == r->n, "%s: %zu bytes a channel, want %zu", name, got, r->n);
    CHECK(!dec.crc_errors, "%s: %u CRC errors", name, dec.crc_errors);
    size_t bad = 0;
    for (size_t i = 0; i < got && i < r->n; i++) bad += L[i] != r->ch[0][i] || R[i] != r->ch[1][i];
    CHECK(!bad, "%s: %zu bytes differ from the source", name, bad);
    printf("  %-16s mode %d, %s, %zu bytes a channel, bit exact%s\n", name, dec.mode,
           dec.mono ? (dec.flags & 4 ? "mono" : "false stereo") : "stereo", got, bad ? " NO" : "");
}

static void run_codec(const char *path, unsigned char *d, size_t n)
{
    memset(&id3, 0, sizeof id3);
    strcpy(id3.path, path);
    id3.frequency = 44100;
    set_file(d, n);
    out_frames = 0;
    codec_main(CODEC_LOAD);
    codec_run();
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: wvdsd_test <vector-dir> <dsf-dir>\n"); return 2; }
    ci = &api;
    api.id3 = &id3;
    api.read_filebuf = f_read;
    api.seek_buffer = f_seek;
    api.seek_complete = f_seek_complete;
    api.set_elapsed = f_elapsed;
    api.configure = f_configure;
    api.get_command = f_command;
    api.pcmbuf_insert = f_insert;
    api.memset = f_memset;
    api.memcpy = f_memcpy;
    api.memmove = f_memmove;
    api.memcmp = f_memcmp;

    static const char *const vec[][2] = { { "tone64-fast", "tone64" }, { "tone64-high", "tone64" },
        { "noise64-raw", "noise64" }, { "same128-high", "same128" }, { "mono64-fast", "mono64" } };
    char p[1024], q[1024];
    printf("1. unpack against the source DSD\n");
    for (unsigned v = 0; v < sizeof vec / sizeof vec[0]; v++)
    {
        size_t n;
        snprintf(p, sizeof p, "%s/%s.wv", argv[1], vec[v][0]);
        snprintf(q, sizeof q, "%s/%s.dsf", argv[2], vec[v][1]);
        unsigned char *wv = load(p, &n);
        struct ref r = load_dsf(q);
        unpack_check(vec[v][0], wv, n, &r);

        /* 2. metadata */
        memset(&id3, 0, sizeof id3);
        id3.codectype = AFMT_WAVPACK;
        int fd = open(p, O_RDONLY);
        bool ok = get_wavpack_metadata(fd, &id3);
        close(fd);
        unsigned long want_ms = (unsigned long long)r.n * 8 * 1000 / r.rate;
        CHECK(ok && id3.codectype == AFMT_DSD && id3.frequency == r.rate / 64 && id3.length == want_ms,
              "%s: metadata ok %d codec %u, %lu Hz %lu ms; want DSD %u Hz %lu ms", vec[v][0], ok,
              id3.codectype, id3.frequency, id3.length, r.rate / 64, want_ms);
        if (v == 0) printf("2. metadata: DSD decoder, %lu Hz PCM, %lu ms (each file)\n", id3.frequency, id3.length);

        /* 3. PCM: the .wv through dsd.c against the .dsf through dsd.c (stereo
         * DSF only; dsd.c's DSF reader takes two channels) */
        run_codec(p, wv, n);
        size_t wv_frames = out_frames;
        int32_t *wv_pcm = malloc(wv_frames * 2 * sizeof *wv_pcm + 1);
        memcpy(wv_pcm, out, wv_frames * 2 * sizeof *out);
        CHECK(rate_set == r.rate / 64, "%s: DSP rate %lu", vec[v][0], rate_set);
        CHECK(wv_frames == r.n / 8, "%s: %zu PCM frames, want %zu", vec[v][0], wv_frames, r.n / 8);
        if (r.chans == 2)
        {
            size_t dn;
            unsigned char *dsf = load(q, &dn);
            run_codec(q, dsf, dn);
            size_t frames = out_frames < wv_frames ? out_frames : wv_frames, bad = 0;
            for (size_t i = 0; i < frames * 2; i++) bad += out[i] != wv_pcm[i];
            CHECK(!bad && frames >= r.n / 8 - 1, "%s: %zu of %zu PCM samples differ from the DSF's", vec[v][0], bad, frames * 2);
            if (v == 0 || v == 3) printf("3. %s: %zu PCM frames, sample for sample the DSF's\n", vec[v][0], frames);
            free(dsf);
        }
        else
        {
            size_t bad = 0;
            for (size_t i = 0; i < wv_frames; i++) bad += wv_pcm[i * 2] != wv_pcm[i * 2 + 1];
            CHECK(!bad, "%s: mono came out with L != R in %zu frames", vec[v][0], bad);
        }

        /* 4. seek to 130 ms (DSD64: the block from 125 ms) after 64 frames */
        if (v == 0 || v == 1)
        {
            seek_at_frame = 64;
            seek_ms = 130;
            run_codec(p, wv, n);
            CHECK(seek_elapsed >= 129 && seek_elapsed <= 130, "%s: seek to 130 ms reported %lu ms", vec[v][0], seek_elapsed);
            CHECK(out_frames > 64, "%s: nothing after the seek", vec[v][0]);
            /* after the CIC's three frames to settle, the PCM is the whole
               decode's from 130 ms on */
            size_t from = 130 * (r.rate / 64) / 1000, bad = 0, m = 0, at = seek_frames;
            for (size_t i = at + 3; i < out_frames && from + i - at < wv_frames; i++, m++)
                bad += out[i * 2] != wv_pcm[(from + i - at) * 2] || out[i * 2 + 1] != wv_pcm[(from + i - at) * 2 + 1];
            CHECK(!bad && m > 500 && out_frames - at == wv_frames - from,
                  "%s: after the seek %zu of %zu frames differ (%zu frames, want %zu)", vec[v][0], bad, m,
                  out_frames - at, wv_frames - from);
            printf("4. %s: seek to 130 ms lands at %lu ms, frame %zu; the %zu frames after match the whole decode\n",
                   vec[v][0], seek_elapsed, from, m);
        }
        free(wv_pcm);
        free(wv);
    }

    {
        size_t n;
        snprintf(p, sizeof p, "%s/tone64-fast.wv", argv[1]);
        unsigned char *wv = load(p, &n);

        /* 4b. resume at 70 ms (id3 elapsed): the same frame as the seek */
        memset(&id3, 0, sizeof id3);
        strcpy(id3.path, p);
        id3.elapsed = 70;
        set_file(wv, n);
        out_frames = 0;
        codec_main(CODEC_LOAD);
        codec_run();
        CHECK(out_frames == 6615 - 70 * 44100 / 1000, "resume at 70 ms: %zu frames", out_frames);
        printf("4b. resume at 70 ms: %zu frames from frame %d\n", out_frames, 70 * 44100 / 1000);

        /* 5. damage: flip bytes, cut short, garbage; under ASan/UBSan */
        unsigned char *bad_wv = malloc(n);
        unsigned seed = 1;
        int runs = 0;
        for (int k = 0; k < 300; k++)
        {
            memcpy(bad_wv, wv, n);
            size_t len = n;
            for (int j = 0; j < 1 + k % 7; j++)
            {
                seed = seed * 1103515245 + 12345;
                size_t at2 = (seed >> 8) % n;
                bad_wv[at2] ^= 1 << (seed >> 4 & 7);
                if (k % 5 == 0) bad_wv[at2] = seed >> 16;
            }
            if (k % 11 == 0) len = (seed >> 3) % n;
            run_codec(p, bad_wv, len);
            CHECK(out_frames <= (size_t)(132300 / 8 + 600), "damage %d: %zu frames", k, out_frames);
            runs++;
        }
        for (int k = 0; k < 50; k++)
        {
            for (size_t i = 0; i < n; i++) { seed = seed * 1103515245 + 12345; bad_wv[i] = seed >> 16; }
            memcpy(bad_wv, wv, 32 + k * 3);   /* a real header, then garbage */
            run_codec(p, bad_wv, n);
            runs++;
        }
        printf("5. %d damaged files decoded without a fault\n", runs);
        free(bad_wv);
        free(wv);
    }

    /* 6. speed: each mode, repeated, against real time */
    printf("6. decode speed (this Mac, one core; PCM out included)\n");
    for (unsigned v = 0; v < 3; v++)
    {
        size_t n;
        snprintf(p, sizeof p, "%s/%s.wv", argv[1], vec[v][0]);
        unsigned char *wv = load(p, &n);
        double audio = 0;
        clock_t t0 = clock();
        while ((double)(clock() - t0) / CLOCKS_PER_SEC < 0.5)
        {
            run_codec(p, wv, n);
            audio += (double)out_frames / rate_set;
        }
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  %-16s %6.1fx real time (%.2f%% of a core per DSD64 stream)\n", vec[v][0], audio / secs, 100 * secs / audio);
        free(wv);
    }

    printf(fails ? "wvdsd: %d FAILED\n" : "wvdsd: all passed\n", fails);
    return fails != 0;
}

/* the simulator's file layer, straight to the host's */
const uint8_t bs_log2_tab[256];   /* DST only, not run here */
#undef open
#undef read
#undef lseek
#undef close
int sim_open(const char *name, int flags, ...) { return open(name, flags); }
ssize_t sim_read(int fd, void *buf, size_t n) { return read(fd, buf, n); }
off_t sim_lseek(int fd, off_t at, int whence) { return lseek(fd, at, whence); }
int sim_close(int fd) { return close(fd); }
