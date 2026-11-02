/* Fake libraries for the tests: a song's path kept as the player keeps it
 * (sloppy_lib.c), its folder once in th_dirs, which every fake library
 * shares as sl_lib.dirs, and its file's name, which points into path (so
 * path must outlive the song). GPLv2+. */
#ifndef SLOPPY_TEST_PATHS_H
#define SLOPPY_TEST_PATHS_H
#include <stdlib.h>
#include <string.h>
#include "sloppy_int.h"
static const char *th_dirs[8192]; static int th_n_dirs;
static inline void tpath(struct sl_track *t, const char *path)
{
    if (!path) { t->file = NULL; t->dir = 0; return; }
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) + 1 : 0;
    int d = 0;
    while (d < th_n_dirs && (strncmp(th_dirs[d], path, n) || th_dirs[d][n])) d++;
    if (d == th_n_dirs)
    {
        char *c = th_n_dirs < (int)(sizeof th_dirs / sizeof th_dirs[0]) ? malloc(n + 1) : NULL;
        if (!c) abort();                 /* more folders than a test needs */
        memcpy(c, path, n);
        c[n] = '\0';
        th_dirs[th_n_dirs++] = c;
    }
    t->dir = (unsigned)d; t->file = path + n;
    sl_lib.dirs = th_dirs; sl_lib.n_dirs = th_n_dirs;
}
/* a fake song whose initializer put its whole path in file: split as above */
static inline void tsplit(struct sl_track *t) { tpath(t, t->file); }
#endif
