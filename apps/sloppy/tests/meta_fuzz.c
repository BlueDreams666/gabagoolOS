/* Mutation fuzz of Rockbox's tag readers (get_metadata), as the library scan and
 * a song's start read them; run-meta-fuzz.sh builds it with ASan and UBSan.
 * Each seed is mutated N times (the same mutants every run), written to the
 * card dir and read; a hang past 5 s is a failure. N < 0: each file as it is. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "config.h"
#include "metadata.h"
#include "kernel.h"
#include "file.h"
const char *sim_root_dir = ".";
void mutex_init(struct mutex *m) { (void)m; }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
void sim_thread_lock(void *me) { (void)me; }
void *sim_thread_unlock(void) { return (void *)1; }

static unsigned rs = 1;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return rs >> 8; }
static unsigned char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n); if (fread(b, 1, *n, f) != *n) { free(b); fclose(f); return NULL; }
    fclose(f); return b;
}
static char cur[64]; static size_t cur_seed;
static void on_alarm(int s) { (void)s; fprintf(stderr, "HANG in %s (seed iteration %zu)\n", cur, cur_seed); _exit(3); }
int main(int argc, char **argv)
{
    /* fuzz-meta <iters> <seedfile>...  (run inside a scratch dir) */
    int iters = atoi(argv[1]);
    signal(SIGALRM, on_alarm);
    for (int a = 2; a < argc; a++)
    {
        size_t n; unsigned char *seed = slurp(argv[a], &n);
        if (!seed) continue;
        const char *ext = strrchr(argv[a], '.');
        snprintf(cur, sizeof cur, "fz%s", ext ? ext : "");
        unsigned char *m = malloc(n + 64);
        for (int it = 0; it < (iters < 0 ? 1 : iters); it++)
        {
            rs = (unsigned)(a * 1000003 + it); cur_seed = it;
            size_t len = n; memcpy(m, seed, n);
            int k = iters < 0 ? 0 : 1 + rnd() % 8;   /* < 0: the file as it is, once */
            for (int j = 0; j < k; j++)
            {
                size_t at = rnd() % len;
                /* most changes in the first 4 KB, where the tags are */
                if (rnd() % 4) at = rnd() % (len < 4096 ? len : 4096);
                switch (rnd() % 6)
                {
                case 0: m[at] ^= 1u << (rnd() % 8); break;
                case 1: m[at] = (unsigned char[]){ 0, 0xff, 0x7f, 0x80, 0xfe, 1 }[rnd() % 6]; break;
                case 2: if (at + 4 <= len) { uint32_t v = (uint32_t[]){ 0xffffffffu, 0x7fffffffu, 0x80000000u, 0, 0xfffffff0u, 0x10000 }[rnd() % 6]; memcpy(m + at, &v, 4); } break;
                case 3: if (at + 4 <= len) { m[at] = 0xff; m[at+1] = 0xff; m[at+2] = 0xff; m[at+3] = 0xf0; } break;
                case 4: len = at + 1; break;                     /* cut short */
                case 5: m[at] = (unsigned char)rnd(); break;
                }
            }
            FILE *f = fopen(cur, "wb"); fwrite(m, 1, len, f); fclose(f);
            char path[80]; snprintf(path, sizeof path, "/%s", cur);
            int fd = open(path, O_RDONLY);
            if (fd < 0) { fprintf(stderr, "cannot open %s\n", path); return 2; }
            static struct mp3entry id3;
            memset(&id3, 0, sizeof id3);
            alarm(5);
            get_metadata(&id3, fd, path);
            alarm(0);
            close(fd);
        }
        printf("%s: %d mutants read\n", argv[a], iters);
        free(m); free(seed);
    }
    return 0;
}
