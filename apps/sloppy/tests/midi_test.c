/* The MIDI codec (lib/rbcodec/codecs/midi.c) and its metadata parser
 * (lib/rbcodec/metadata/midi.c) on files gen_midi.py makes:
 *   1. lengths: tempo changes, SMPTE time, RIFF RMID, the track name;
 *   2. the pitch: an A is 440 Hz, bent a semitone up 466 Hz;
 *   3. every file plays to its length and rings out, then ends;
 *   4. drums sound, a note-off ends a note, a seek lands where it is asked;
 *   5. damaged files (bytes changed, cut short) never read out of bounds,
 *      hang or play for ever;
 *   6. speed on a dense file (sixteen channels, every family, drums).
 * usage: midi_test <dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "codecs.h"
#include "metadata.h"

struct codec_api *ci;
static struct codec_api api;
static struct mp3entry id3;
static unsigned char *file_data;
static size_t file_size, file_pos;
static int32_t *out;
static size_t out_frames, out_cap, out_limit = 44100 * 60;
static unsigned long elapsed, rate_set, seek_elapsed;
static long seek_at_frame = -1, seek_ms;
static size_t seek_frames;
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
static bool f_seek(size_t pos) { if (pos > file_size) return false; file_pos = pos; api.curpos = pos; return true; }
static void f_seek_complete(void) { seek_elapsed = elapsed; }
static void f_elapsed(unsigned long v) { elapsed = v; }
static void f_configure(int s, intptr_t v) { if (s == DSP_SET_FREQUENCY) rate_set = v; }
static long f_command(intptr_t *param)
{
    if (out_frames >= out_limit) return CODEC_ACTION_HALT;   /* played on and on: stop it */
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

/* codeclib and metadata helpers */
int codec_init(void) { return 0; }
void codec_set_replaygain(const struct mp3entry *e) { (void)e; }
size_t codec_strlen(const char *s) { return strlen(s); }
void *codec_malloc(size_t n) { static void *last; free(last); last = malloc(n); return last; }   /* one file a run */
uint32_t get_long_le(void *buf) { unsigned char *p = buf; return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint32_t get_long_be(void *buf) { unsigned char *p = buf; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
off_t sim_filesize(int fd) { struct stat st; return fstat(fd, &st) ? -1 : st.st_size; }
bool get_midi_metadata(int fd, struct mp3entry *id3);
enum codec_status codec_main(enum codec_entry_call_reason reason);
enum codec_status codec_run(void);

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL can't open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) exit(1);
    fclose(f);
    return b;
}
static int meta_open(const char *p);
static bool meta(const char *path)
{
    memset(&id3, 0, sizeof id3);
    int fd = meta_open(path);
    bool ok = get_midi_metadata(fd, &id3);
    close(fd);
    return ok;
}
static enum codec_status run(const char *path, unsigned char *d, size_t n)
{
    memset(&id3, 0, sizeof id3);
    strcpy(id3.path, path);
    file_data = d; file_size = n; file_pos = 0;
    api.filesize = n; api.curpos = 0;
    out_frames = 0;
    codec_main(CODEC_LOAD);
    return codec_run();
}
/* the frequency of the left channel over [a, b) frames, by its rising zero crossings */
static double freq(size_t a, size_t b)
{
    int n = 0; size_t first = 0, last = 0;
    for (size_t i = a + 1; i < b; i++)
        if (out[(i - 1) * 2] < 0 && out[i * 2] >= 0) { if (!n) first = i; last = i; n++; }
    return n > 1 ? (n - 1) * 44100.0 / (last - first) : 0;
}
static double rms(size_t a, size_t b)
{
    double s = 0;
    for (size_t i = a; i < b && i < out_frames; i++) s += (double)out[i * 2] * out[i * 2] + (double)out[i * 2 + 1] * out[i * 2 + 1];
    return b > a ? sqrt(s / (2.0 * (b - a))) : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    ci = &api;
    api.id3 = &id3;
    api.read_filebuf = f_read; api.seek_buffer = f_seek; api.seek_complete = f_seek_complete;
    api.set_elapsed = f_elapsed; api.configure = f_configure; api.get_command = f_command;
    api.pcmbuf_insert = f_insert; api.memset = f_memset; api.memcpy = f_memcpy;
    api.memmove = f_memmove; api.memcmp = f_memcmp;
    char p[1024];
    static const struct { const char *name; unsigned long ms; } want[] = {
        { "tone440.mid", 1000 }, { "tone440.rmi", 1000 }, { "bend.mid", 1000 }, { "tempo.mid", 1500 },
        { "smpte.mid", 500 }, { "drums.mid", 4000 }, { "arp.mid", 2950 }, { "dense.mid", 9875 },
    };
    printf("1. lengths\n");
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
    {
        snprintf(p, sizeof p, "%s/%s", argv[1], want[i].name);
        bool ok = meta(p);
        CHECK(ok && id3.length == want[i].ms && id3.frequency == 44100, "%s: %d, %lu ms, want %lu", want[i].name, ok, id3.length, want[i].ms);
        printf("  %-12s %5lu ms%s%s\n", want[i].name, id3.length, id3.title ? "  title: " : "", id3.title ? id3.title : "");
        if (i < 2) CHECK(id3.title && !strcmp(id3.title, "Test tone"), "%s: title", want[i].name);
    }

    printf("2-3. playing each to its end\n");
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
    {
        size_t n; snprintf(p, sizeof p, "%s/%s", argv[1], want[i].name);
        unsigned char *d = load(p, &n);
        enum codec_status st = run(p, d, n);
        size_t len = want[i].ms * 441 / 10;
        CHECK(st == CODEC_OK && rate_set == 44100 && out_frames >= len && out_frames <= len + 44100 * 3 + 512,
              "%s: status %d, %zu frames for %zu", want[i].name, st, out_frames, len);
        double r = rms(0, len);
        CHECK(r > 300, "%s: silent (rms %.0f)", want[i].name, r);
        if (!strcmp(want[i].name, "tone440.mid") || !strcmp(want[i].name, "tone440.rmi"))
        {
            double f = freq(4410, 40000);
            CHECK(fabs(f - 440) < 1, "%s: %.2f Hz", want[i].name, f);
            /* the note-off at a second: a flute's release (1/e in 0.12 s) has it
             * below 5 % by half a second on, and the voice gone within 1.2 s */
            CHECK(rms(44100 + 22050, 44100 + 24255) < r / 20 && out_frames < len + 52920, "%s: still sounding after its note-off", want[i].name);
            if (i == 0) printf("  tone440.mid  %.2f Hz, rms %.0f, %zu frames (%zu after the note-off)\n", f, r, out_frames, out_frames - len);
        }
        if (!strcmp(want[i].name, "bend.mid"))
        {
            double f = freq(4410, 40000), w = 440 * pow(2, 1 / 12.0);
            CHECK(fabs(f - w) < 1.5, "bend: %.2f Hz, want %.2f", f, w);
            printf("  bend.mid     %.2f Hz (a semitone up: %.2f)\n", f, w);
        }
        if (!strcmp(want[i].name, "drums.mid"))
        {
            /* a kick or a snare every half second: each louder than the moment before it */
            int beats = 0;
            for (int b = 2; b < 16; b += 2) beats += rms(b * 11025, b * 11025 + 600) > 2 * rms(b * 11025 - 1500, b * 11025 - 900);
            CHECK(beats == 7, "drums: %d of 7 beats stand out", beats);
            printf("  drums.mid    %d of 7 kicks and snares stand out, rms %.0f\n", beats, r);
        }
        free(d);
    }

    printf("4. seeking\n");
    {
        size_t n; snprintf(p, sizeof p, "%s/arp.mid", argv[1]);
        unsigned char *d = load(p, &n);
        seek_at_frame = 2000; seek_ms = 1550;
        run(p, d, n);
        CHECK(seek_elapsed == 1550, "seek reported %lu ms", seek_elapsed);
        /* the next note (1.6 s) sounds 50 ms after the seek, nothing before it */
        size_t at = seek_frames;
        double before = rms(at, at + 2000), after = rms(at + 2400, at + 4400);
        CHECK(before < 50 && after > 300, "after the seek: %.0f before the next note, %.0f with it", before, after);
        size_t rest = out_frames - at, want_rest = (2950 - 1550) * 441 / 10;
        CHECK(rest >= want_rest && rest <= want_rest + 44100 * 3 + 512, "after the seek: %zu frames, want %zu+", rest, want_rest);
        printf("  arp.mid      to 1550 ms: quiet %.0f, then the 1.6 s note %.0f; %zu frames on\n", before, after, rest);
        /* resume at 1 s */
        memset(&id3, 0, sizeof id3); strcpy(id3.path, p); id3.elapsed = 1000;
        file_data = d; file_size = n; file_pos = 0; api.filesize = n; out_frames = 0;
        codec_main(CODEC_LOAD); codec_run();
        size_t want_r = (2950 - 1000) * 441 / 10;
        CHECK(out_frames >= want_r && out_frames <= want_r + 44100 * 3 + 512, "resume at 1 s: %zu frames, want %zu+", out_frames, want_r);
        free(d);
    }

    printf("5. damaged files\n");
    {
        static const char *const names[] = { "tone440.mid", "tempo.mid", "drums.mid", "dense.mid", "tone440.rmi", "smpte.mid" };
        unsigned seed = 7; int runs = 0, short_files = 0, long_files = 0;
        out_limit = 44100 * 40;
        for (unsigned f = 0; f < 6; f++)
        {
            size_t n; snprintf(p, sizeof p, "%s/%s", argv[1], names[f]);
            unsigned char *d = load(p, &n), *b = malloc(n);
            for (int k = 0; k < 120; k++)
            {
                memcpy(b, d, n);
                size_t len = n;
                for (int j = 0; j < 1 + k % 9; j++)
                {
                    seed = seed * 1103515245 + 12345;
                    size_t at = (seed >> 8) % n;
                    b[at] = k % 3 ? (unsigned char)(seed >> 16) : b[at] ^ 0x80;
                }
                if (k % 7 == 0) len = (seed >> 4) % n;
                /* the metadata parser first, on the same bytes: the library's length */
                char t[64]; snprintf(t, sizeof t, "/tmp/midi-test-%d.mid", getpid());
                FILE *o = fopen(t, "wb"); fwrite(b, 1, len, o); fclose(o);
                bool ok = meta(t); remove(t);
                unsigned long ms = id3.length;
                if (!ok) { short_files++; }
                if (ms > 60000) { long_files++; runs++; continue; }   /* a delta of hours: valid, too slow to play here */
                memset(&id3, 0, sizeof id3); strcpy(id3.path, p); id3.length = ms;
                file_data = b; file_size = len; file_pos = 0; api.filesize = len; out_frames = 0;
                codec_main(CODEC_LOAD); codec_run();
                CHECK(!ok || out_frames <= (ms + 3000) * 441 / 10 + 1024, "%s damage %d: %zu frames for %lu ms", names[f], k, out_frames, ms);
                runs++;
            }
            free(b); free(d);
        }
        printf("  %d damaged files: no fault, none past its length (%d refused by the parser, %d hours long)\n", runs, short_files, long_files);
        out_limit = 44100 * 60;
    }

    printf("6. speed (this Mac, one core)\n");
    {
        size_t n; snprintf(p, sizeof p, "%s/dense.mid", argv[1]);
        unsigned char *d = load(p, &n);
        clock_t t0 = clock(); double audio = 0;
        while ((double)(clock() - t0) / CLOCKS_PER_SEC < 0.5) { run(p, d, n); audio += out_frames / 44100.0; }
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  dense.mid    %.0fx real time (%.2f%% of a core)\n", audio / secs, 100 * secs / audio);
        free(d);
    }
    printf(fails ? "midi: %d FAILED\n" : "midi: all passed\n", fails);
    return fails != 0;
}

/* the simulator's file layer, straight to the host's */
#undef open
#undef read
#undef lseek
#undef close
#undef remove
int sim_remove(const char *p) { return remove(p); }
static int meta_open(const char *p) { return open(p, O_RDONLY); }
int sim_open(const char *name, int flags, ...) { return open(name, flags); }
ssize_t sim_read(int fd, void *buf, size_t n) { return read(fd, buf, n); }
off_t sim_lseek(int fd, off_t at, int whence) { return lseek(fd, at, whence); }
int sim_close(int fd) { return close(fd); }
