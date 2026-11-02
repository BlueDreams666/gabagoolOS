/***************************************************************************
 * GabagoolOS - music library view built on top of the Rockbox tag database.
 *
 * The library lives in RAM as two arrays (tracks, albums) whose strings all
 * sit in one arena. After a full read of the tag database the arena and the
 * arrays are written to /.rockbox/sloppy/library.bin together with the
 * database's commit id and the list orders; the next boot loads that file
 * instead of doing thousands of small database lookups. Both run in small
 * steps between frames so the interface never freezes: the file a slice
 * at a time, its orders checked rather than sorted again.
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
#include <time.h>
#include "string-extra.h"
#include "file.h"
#include "dir.h"
#include "rbpaths.h"
#include "tagcache.h"
#include "sloppy_int.h"

struct sl_lib sl_lib;
/* Library options > Join compilations off: an untagged compilation stays
 * an album per song artist. A cache read the other way is read again. */
bool sl_lib_keep_apart;

#define CACHE_DIR   ROCKBOX_DIR "/sloppy"
#define CACHE_FILE  CACHE_DIR "/library.bin"
#define CACHE_MAGIC 0x534c4f42   /* "SLOB": the list orders kept too (SLOA: folders once; SLO9: whole paths) */
#define CACHE_HDR   7            /* magic, songs, albums, string bytes, checksum, options, folders */
#define MAX_TRACKS 500000        /* and folders: sl_track's dir has 24 bits */
#define MAX_ALBUMS 200000
#define MAX_ARENA (256u * 1024 * 1024)
#define CUE_MAX 99               /* tracks in a cue sheet: a song's cue number fits its byte */

static struct tagcache_search tcs;
static bool scanning;
/* library.bin read in slices (sl_lib_step): the file, its records
 * checked, its kept orders checked, then the library made in one go */
enum { CL_OFF, CL_READ, CL_CHECK, CL_ORDER };
static int cl_phase, cl_fd = -1, cl_at;
static int32_t cl_hdr[CACHE_HDR], cl_key[6];
static unsigned cl_hash;
static size_t cl_read;                           /* bytes of the file after its key */
static int *ord_songs, *ord_title, *ord_artist;  /* its orders; build_views takes them when they hold */
static bool ord_ok;
static unsigned char *ord_seen;                  /* an order's entries met so far, a bit each */
static int64_t slice_us = 8000;                  /* a step's work: half the 16 ms between frames */
typedef char int_is_int32[sizeof(int) == sizeof(int32_t) ? 1 : -1];   /* the orders are read into int arrays */
static int64_t lib_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* ------------------------------------------------------------ arena */
static char *arena;
static size_t arena_len, arena_cap;

static int32_t arena_add(const char *s)
{
    size_t n = strlen(s) + 1;
    if (n > MAX_ARENA - arena_len) return -1;
    if (arena_len + n > arena_cap)
    {
        size_t nc = arena_cap ? arena_cap * 2 : 64 * 1024;
        while (nc < arena_len + n) nc *= 2;
        char *na = realloc(arena, nc);
        if (!na) return -1;
        arena = na;
        arena_cap = nc;
    }
    memcpy(arena + arena_len, s, n);
    arena_len += n;
    return (int32_t)(arena_len - n);
}
#define STR(off) (arena + (off))
/* The arena grows by doubling while a scan reads, so up to half of it is
 * spare at the end (3.4 MB of 8 for a 70k-song card). Records hold offsets
 * until build_views makes pointers, so it can shrink to fit just before. */
static bool arena_trim(void)
{
    if (arena && arena_len && arena_len < arena_cap)
    {
        char *na = realloc(arena, arena_len);
        if (na) { arena = na; arena_cap = arena_len; }
    }
    return true;
}

/* Structures as stored (offsets instead of pointers). */
struct rec_track { int32_t dir, file, title, album, num, disc, len, lossless; uint32_t added; int32_t artist, start, cue; };   /* dir: rd's; artist -1: the album's; cue 0: not a cue sheet's */
struct rec_album { int32_t title, artist, year, genre; };   /* genre -1: none */

static struct rec_track *rt;  static int n_rt, cap_rt;
static struct rec_album *ra;  static int n_ra, cap_ra;
static int *ahash;            static int ahash_cap;
/* the folders, each once (arena offsets of "/Music/A/", ending in '/'),
 * and during a scan a hash of them; a song keeps its folder's index and
 * its file's name, not its whole path (2 MB of 4.6 for 70k songs) */
static int32_t *rd;           static int n_rd, cap_rd;
static int *dhash;            static int dhash_cap, dlast = -1;
/* during a scan: albums named by their songs' own artist, for want of an
 * album artist tag (a compilation tagged that way splits into one album
 * per artist until join_compilations puts it back together) */
static unsigned char *ra_loose; static int cap_loose;
static bool scan_loose;

/* -------------------------------------------------------- helpers */
static unsigned album_hash(const char *artist, const char *album)
{
    unsigned h = 2166136261u;
    for (const char *p = artist; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ '|') * 16777619u;
    for (const char *p = album; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return h;
}

static bool grow_album_hash(void)
{
    int nc = ahash_cap ? ahash_cap * 2 : 1024;
    int *nh = malloc((size_t)nc * sizeof(*nh));
    if (!nh) return false;
    for (int i = 0; i < nc; i++) nh[i] = -1;
    for (int a = 0; a < n_ra; a++)
    {
        unsigned slot = album_hash(STR(ra[a].artist), STR(ra[a].title)) & (nc - 1);
        while (nh[slot] >= 0) slot = (slot + 1) & (nc - 1);
        nh[slot] = a;
    }
    free(ahash);
    ahash = nh;
    ahash_cap = nc;
    return true;
}

static int album_find_or_add(const char *artist, const char *album, int year, const char *genre)
{
    if (n_ra >= ahash_cap / 2 && !grow_album_hash()) return -1;
    unsigned h = album_hash(artist, album);
    for (int i = 0; i < ahash_cap; i++)
    {
        int slot = (h + i) & (ahash_cap - 1);
        int a = ahash[slot];
        if (a < 0)
        {
            if (n_ra >= MAX_ALBUMS) return -1;
            if (n_ra == cap_ra)
            {
                int nc = cap_ra ? cap_ra * 2 : 256;
                struct rec_album *n = realloc(ra, nc * sizeof(*n));
                if (!n) return -1;
                ra = n; cap_ra = nc;
            }
            int32_t t = arena_add(album), ar = arena_add(artist);
            if (t < 0 || ar < 0) return -1;
            ra[n_ra].title = t;
            ra[n_ra].artist = ar;
            ra[n_ra].year = year;
            ra[n_ra].genre = genre[0] ? arena_add(genre) : -1;
            if (n_ra >= cap_loose)
            {
                int nc = cap_ra > n_ra ? cap_ra : n_ra + 256;
                unsigned char *nl = realloc(ra_loose, (size_t)nc);
                if (!nl) return -1;
                memset(nl + cap_loose, 0, (size_t)(nc - cap_loose));
                ra_loose = nl;
                cap_loose = nc;
            }
            ra_loose[n_ra] = scan_loose;
            ahash[slot] = n_ra;
            return n_ra++;
        }
        if (!strcmp(STR(ra[a].artist), artist) && !strcmp(STR(ra[a].title), album))
        {
            if (!ra[a].year && year) ra[a].year = year;
            if (ra[a].genre < 0 && genre[0]) ra[a].genre = arena_add(genre);
            return a;
        }
    }
    return -1;
}

static unsigned dir_hash(const char *s, size_t n)
{
    unsigned h = 2166136261u;
    while (n--) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}
static bool dir_is(int d, const char *s, size_t n)
{
    const char *x = STR(rd[d]);
    return !strncmp(x, s, n) && !x[n];
}
static bool grow_dir_hash(void)
{
    int nc = dhash_cap ? dhash_cap * 2 : 1024;
    int *nh = malloc((size_t)nc * sizeof(*nh));
    if (!nh) return false;
    for (int i = 0; i < nc; i++) nh[i] = -1;
    for (int d = 0; d < n_rd; d++)
    {
        const char *x = STR(rd[d]);
        unsigned slot = dir_hash(x, strlen(x)) & (nc - 1);
        while (nh[slot] >= 0) slot = (slot + 1) & (nc - 1);
        nh[slot] = d;
    }
    free(dhash);
    dhash = nh;
    dhash_cap = nc;
    return true;
}
/* path's folder ("/Music/A/" of "/Music/A/x.flac"), its index; -1 when
 * memory runs out. A scan reads a folder's songs together, so the last
 * one found is tried first. */
static int dir_find_or_add(const char *path, size_t n)
{
    if (dlast >= 0 && dlast < n_rd && dir_is(dlast, path, n)) return dlast;
    if (n_rd >= dhash_cap / 2 && !grow_dir_hash()) return -1;
    unsigned h = dir_hash(path, n);
    for (int i = 0; i < dhash_cap; i++)
    {
        int slot = (h + i) & (dhash_cap - 1);
        int d = dhash[slot];
        if (d >= 0)
        {
            if (dir_is(d, path, n)) return dlast = d;
            continue;
        }
        if (n_rd >= MAX_TRACKS) return -1;
        if (n_rd == cap_rd)
        {
            int nc = cap_rd ? cap_rd * 2 : 256;
            int32_t *nr = realloc(rd, (size_t)nc * sizeof(*nr));
            if (!nr) return -1;
            rd = nr; cap_rd = nc;
        }
        char buf[MAX_PATH];
        if (n >= sizeof buf) return -1;
        memcpy(buf, path, n);
        buf[n] = '\0';
        int32_t off = arena_add(buf);
        if (off < 0) return -1;
        rd[n_rd] = off;
        dhash[slot] = n_rd;
        return dlast = n_rd++;
    }
    return -1;
}
/* path into its folder's index and its file's name in the arena */
static bool path_add(const char *path, int32_t *dir, int32_t *file)
{
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) + 1 : 0;
    *dir = dir_find_or_add(path, n);
    *file = *dir >= 0 ? arena_add(path + n) : -1;
    return *dir >= 0 && *file >= 0;
}

static int cmp_track_in_album(const void *a, const void *b)
{
    const struct sl_track *x = &sl_lib.tracks[*(const int *)a];
    const struct sl_track *y = &sl_lib.tracks[*(const int *)b];
    if (x->disc != y->disc) return (x->disc > y->disc) - (x->disc < y->disc);
    if (x->num != y->num) return (x->num > y->num) - (x->num < y->num);
    return strcasecmp(x->title, y->title);
}
static int cmp_album_title(const void *a, const void *b)
{
    return sl_sort_cmp(sl_lib.albums[*(const int *)a].title, sl_lib.albums[*(const int *)b].title);
}
/* Titles in list order; songs of one title by their artist, then as the
 * database has them, so the order never changes between scans */
static int song_order(int x, const char *tx, const char *ax, int y, const char *ty, const char *ay)
{
    int c = sl_sort_cmp(tx, ty);
    if (!c) c = sl_sort_cmp(ax ? ax : "", ay ? ay : "");
    return c ? c : (x > y) - (x < y);
}
static int cmp_song_title(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return song_order(x, sl_lib.tracks[x].title, sl_track_artist(x), y, sl_lib.tracks[y].title, sl_track_artist(y));
}
static int artist_order(const char *xa, int xy, const char *ya, int yy)
{
    int c = sl_sort_cmp(xa, ya);
    if (!c) c = strcasecmp(xa, ya);   /* "Beatles" and "The Beatles" stay apart */
    return c ? c : (xy > yy) - (xy < yy);
}
static int cmp_artist(const void *a, const void *b)
{
    const struct sl_album *x = &sl_lib.albums[*(const int *)a];
    const struct sl_album *y = &sl_lib.albums[*(const int *)b];
    return artist_order(x->artist, x->year, y->artist, y->year);
}

/* Albums of one title in one folder, each named after a different song
 * artist because none had an album artist tag, are one compilation: they
 * become a single album by Various Artists, every song keeping its own
 * artist. The emptied albums are removed and the rest renumbered. */
static int jc_n;
static const int *jc_first;              /* an album's first song, for its folder */
static int jc_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    int c = strcasecmp(STR(ra[x].title), STR(ra[y].title));
    if (c) return c;
    int dx = rt[jc_first[x]].dir, dy = rt[jc_first[y]].dir;   /* a folder is there once: its index says which */
    if (dx != dy) return dx < dy ? -1 : 1;
    return (x > y) - (x < y);
}
static bool jc_same(int x, int y)
{
    if (strcasecmp(STR(ra[x].title), STR(ra[y].title))) return false;
    return rt[jc_first[x]].dir == rt[jc_first[y]].dir;
}
static bool join_compilations(void)
{
    if (!ra_loose || !n_ra || sl_lib_keep_apart) return true;
    int *first = malloc((size_t)n_ra * sizeof *first), *loose = malloc((size_t)n_ra * sizeof *loose);
    int *map = malloc((size_t)n_ra * sizeof *map), *count = calloc((size_t)n_ra, sizeof *count);
    if (!first || !loose || !map || !count) { free(first); free(loose); free(map); free(count); return false; }
    for (int a = 0; a < n_ra; a++) first[a] = -1;
    for (int i = 0; i < n_rt; i++)
        if (rt[i].album >= 0 && rt[i].album < n_ra && first[rt[i].album] < 0) first[rt[i].album] = i;
    int n = 0;
    for (int a = 0; a < n_ra && a < cap_loose; a++)   /* "Singles" is ours, for songs with no album at all */
        if (ra_loose[a] && first[a] >= 0 && strcmp(STR(ra[a].title), "Singles")) loose[n++] = a;
    jc_first = first;
    jc_n = n;
    if (n > 1) qsort(loose, n, sizeof *loose, jc_cmp);
    for (int a = 0; a < n_ra; a++) map[a] = a;
    int32_t various = -1;
    bool any = false;
    for (int i = 0; i < n; )
    {
        int j = i + 1;
        while (j < n && jc_same(loose[i], loose[j])) j++;
        if (j - i > 1)
        {
            if (various < 0) various = arena_add("Various Artists");
            if (various < 0) { free(first); free(loose); free(map); free(count); return false; }
            int to = loose[i];
            for (int k = i; k < j; k++) map[loose[k]] = to;
            for (int t = 0; t < n_rt; t++)
            {
                int a = rt[t].album;
                if (a < 0 || a >= n_ra || map[a] != to || (a != to && !jc_same(a, to))) continue;
                if (rt[t].artist < 0) rt[t].artist = ra[a].artist;   /* the song keeps who it is by */
                rt[t].album = to;
            }
            ra[to].artist = various;
            any = true;
        }
        i = j;
    }
    if (any)
    {
        /* the albums now empty go, the rest move up */
        for (int t = 0; t < n_rt; t++) if (rt[t].album >= 0 && rt[t].album < n_ra) count[rt[t].album]++;
        int m = 0;
        for (int a = 0; a < n_ra; a++)
        {
            map[a] = count[a] ? m : -1;
            if (count[a]) ra[m++] = ra[a];
        }
        for (int t = 0; t < n_rt; t++) rt[t].album = map[rt[t].album];
        n_ra = m;
    }
    free(first); free(loose); free(map); free(count);
    return true;
}

static void free_records(void)
{
    free(rt); rt = NULL; n_rt = cap_rt = 0;
    free(ra); ra = NULL; n_ra = cap_ra = 0;
    free(ahash); ahash = NULL; ahash_cap = 0;
    free(ra_loose); ra_loose = NULL; cap_loose = 0;
    free(rd); rd = NULL; n_rd = cap_rd = 0;
    free(dhash); dhash = NULL; dhash_cap = 0; dlast = -1;
    free(ord_songs); free(ord_title); free(ord_artist); free(ord_seen);
    ord_songs = ord_title = ord_artist = NULL; ord_seen = NULL; ord_ok = false;
}

static void free_views(void)
{
    for (int i = 0; i < sl_lib.n_albums; i++)
        free(sl_lib.albums[i].tracks);
    free(sl_lib.tracks);
    free(sl_lib.albums);
    free(sl_lib.albums_by_title);
    free(sl_lib.albums_by_artist);
    free(sl_lib.songs_by_title);
    free(sl_lib.path_hash);
    free(sl_lib.dirs);
    memset(&sl_lib, 0, sizeof(sl_lib));
}

void sl_lib_free(void)
{
    if (scanning) { tagcache_search_finish(&tcs); scanning = false; }
    if (cl_fd >= 0) { close(cl_fd); cl_fd = -1; }
    cl_phase = CL_OFF;
    free_views();
    free_records();
    free(arena); arena = NULL; arena_len = arena_cap = 0;
}

/* Turn the stored records into the in-memory library with indexes. */
static bool build_views(void)
{
    free_views();
    sl_lib.tracks = malloc((n_rt ? n_rt : 1) * sizeof(struct sl_track));
    sl_lib.albums = calloc(n_ra ? n_ra : 1, sizeof(struct sl_album));
    sl_lib.dirs = malloc((n_rd ? n_rd : 1) * sizeof(*sl_lib.dirs));
    if (!sl_lib.tracks || !sl_lib.albums || !sl_lib.dirs) goto fail;
    for (int d = 0; d < n_rd; d++) sl_lib.dirs[d] = STR(rd[d]);
    sl_lib.n_dirs = n_rd;
    for (int a = 0; a < n_ra; a++)
    {
        struct sl_album *al = &sl_lib.albums[a];
        al->title = STR(ra[a].title);
        al->artist = STR(ra[a].artist);
        al->year = ra[a].year;
        al->genre = ra[a].genre >= 0 ? STR(ra[a].genre) : NULL;
        unsigned h = 2166136261u;
        for (const char *p = al->artist; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
        for (const char *p = al->title; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
        al->hue = h % 360;
    }
    int n = 0;
    for (int i = 0; i < n_rt; i++)
    {
        if (rt[i].album < 0 || rt[i].album >= n_ra || rt[i].dir < 0 || rt[i].dir >= n_rd) goto fail;
        struct sl_track *t = &sl_lib.tracks[n++];
        t->dir = (unsigned)rt[i].dir;
        t->file = STR(rt[i].file);
        t->title = STR(rt[i].title);
        t->album = rt[i].album;
        t->num = rt[i].num;
        t->disc = rt[i].disc;
        t->len = rt[i].len;
        t->lossless = rt[i].lossless;
        t->added = rt[i].added;
        t->artist = rt[i].artist >= 0 ? STR(rt[i].artist) : NULL;
        t->start = rt[i].start;
        t->cue = (unsigned)rt[i].cue;
        sl_lib.albums[t->album].n_tracks++;
    }
    sl_lib.n_tracks = n;
    sl_lib.n_albums = n_ra;

    for (int a = 0; a < n_ra; a++)
    {
        sl_lib.albums[a].tracks = malloc((sl_lib.albums[a].n_tracks ? sl_lib.albums[a].n_tracks : 1) * sizeof(int));
        if (!sl_lib.albums[a].tracks) goto fail;
        sl_lib.albums[a].n_tracks = 0;
    }
    for (int i = 0; i < n; i++)
    {
        struct sl_album *al = &sl_lib.albums[sl_lib.tracks[i].album];
        al->tracks[al->n_tracks++] = i;
    }
    for (int a = 0; a < n_ra; a++)
        qsort(sl_lib.albums[a].tracks, sl_lib.albums[a].n_tracks, sizeof(int), cmp_track_in_album);

    if (ord_ok)   /* library.bin's own, checked (cache_step): no sort */
    {
        sl_lib.albums_by_title = ord_title;
        sl_lib.albums_by_artist = ord_artist;
        sl_lib.songs_by_title = ord_songs;
        ord_title = ord_artist = ord_songs = NULL;
        ord_ok = false;
    }
    else
    {
        sl_lib.albums_by_title = malloc((n_ra ? n_ra : 1) * sizeof(int));
        sl_lib.albums_by_artist = malloc((n_ra ? n_ra : 1) * sizeof(int));
        sl_lib.songs_by_title = malloc((n ? n : 1) * sizeof(int));
        if (!sl_lib.albums_by_title || !sl_lib.albums_by_artist || !sl_lib.songs_by_title) goto fail;
        for (int a = 0; a < n_ra; a++) sl_lib.albums_by_title[a] = sl_lib.albums_by_artist[a] = a;
        for (int i = 0; i < n; i++) sl_lib.songs_by_title[i] = i;
        qsort(sl_lib.albums_by_title, n_ra, sizeof(int), cmp_album_title);
        qsort(sl_lib.albums_by_artist, n_ra, sizeof(int), cmp_artist);
        qsort(sl_lib.songs_by_title, n, sizeof(int), cmp_song_title);
    }

    /* path -> track, for showing the playlist Rockbox is playing */
    sl_lib.path_hash_cap = 1024;
    while (sl_lib.path_hash_cap < n + n / 3) sl_lib.path_hash_cap *= 2;   /* at most three quarters full */
    sl_lib.path_hash = malloc(sl_lib.path_hash_cap * sizeof(int));
    if (!sl_lib.path_hash) goto fail;
    for (int i = 0; i < sl_lib.path_hash_cap; i++) sl_lib.path_hash[i] = -1;
    for (int i = 0; i < n; i++)
    {
        unsigned h = sl_track_path_fnv(i);
        for (int j = 0; j < sl_lib.path_hash_cap; j++)
        {
            int slot = (h + j) & (sl_lib.path_hash_cap - 1);
            if (sl_lib.path_hash[slot] < 0) { sl_lib.path_hash[slot] = i; break; }
        }
    }

    sl_lib.n_artists = 0;
    for (int k = 0; k < n_ra; k++)
        if (k == 0 || strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[k]].artist,
                                 sl_lib.albums[sl_lib.albums_by_artist[k - 1]].artist))
            sl_lib.n_artists++;
    if (n > 0) return true;
fail:
    free_views();
    return false;
}

/* The master index header changes on every database commit. */
static bool db_key(int32_t key[6])
{
    char path[MAX_PATH];
    struct tagcache_stat *st = tagcache_get_stat();
    snprintf(path, sizeof path, "%s/database_idx.tcd", st->db_path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = read(fd, key, 6 * sizeof(int32_t)) == 6 * sizeof(int32_t);
    close(fd);
    return ok;
}

static unsigned hash_bytes(unsigned h, const void *data, size_t size)
{
    const unsigned char *p = data;
    while (size--) h = (h ^ *p++) * 16777619u;
    return h;
}

/* the records, then the library's orders (build_views made them), then the strings */
static unsigned cache_checksum(void)
{
    unsigned h = hash_bytes(2166136261u, rt, (size_t)n_rt * sizeof(*rt));
    h = hash_bytes(h, ra, (size_t)n_ra * sizeof(*ra));
    h = hash_bytes(h, rd, (size_t)n_rd * sizeof(*rd));
    h = hash_bytes(h, sl_lib.songs_by_title, (size_t)n_rt * sizeof(int));
    h = hash_bytes(h, sl_lib.albums_by_title, (size_t)n_ra * sizeof(int));
    h = hash_bytes(h, sl_lib.albums_by_artist, (size_t)n_ra * sizeof(int));
    return hash_bytes(h, arena, arena_len);
}

/* key: the database commit the records are of */
static bool cache_save(const int32_t key[6])
{
    mkdir(CACHE_DIR);
    int fd = open(CACHE_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    int32_t hdr[CACHE_HDR] = { CACHE_MAGIC, n_rt, n_ra, (int32_t)arena_len,
                               (int32_t)cache_checksum(), sl_lib_keep_apart, n_rd };
    bool ok = write(fd, hdr, sizeof hdr) == sizeof hdr
           && write(fd, key, 6 * sizeof(*key)) == 6 * sizeof(*key)
           && write(fd, rt, n_rt * sizeof(*rt)) == (ssize_t)(n_rt * sizeof(*rt))
           && write(fd, ra, n_ra * sizeof(*ra)) == (ssize_t)(n_ra * sizeof(*ra))
           && write(fd, rd, n_rd * sizeof(*rd)) == (ssize_t)(n_rd * sizeof(*rd))
           && write(fd, sl_lib.songs_by_title, n_rt * sizeof(int)) == (ssize_t)(n_rt * sizeof(int))
           && write(fd, sl_lib.albums_by_title, n_ra * sizeof(int)) == (ssize_t)(n_ra * sizeof(int))
           && write(fd, sl_lib.albums_by_artist, n_ra * sizeof(int)) == (ssize_t)(n_ra * sizeof(int))
           && write(fd, arena, arena_len) == (ssize_t)arena_len;
    if (close(fd) < 0) ok = false;
    if (ok) ok = rename(CACHE_FILE ".tmp", CACHE_FILE) == 0;
    if (!ok) remove(CACHE_FILE ".tmp");
    return ok;
}

static bool valid_string(int32_t offset)
{
    /* The final byte is NUL, so every accepted string is bounded. Also
     * reject offsets into the middle of a different string. */
    return offset >= 0 && (size_t)offset < arena_len
        && (offset == 0 || arena[offset - 1] == '\0');
}

/* library.bin's header and size against the database now, and room
 * for it all: the read itself is cache_step's, in slices */
static bool cache_open(void)
{
    int32_t fkey[6], *hdr = cl_hdr;
    if (!db_key(cl_key)) return false;
    cl_fd = open(CACHE_FILE, O_RDONLY);
    if (cl_fd < 0) return false;
    bool ok = read(cl_fd, hdr, sizeof cl_hdr) == sizeof cl_hdr && hdr[0] == CACHE_MAGIC
           && read(cl_fd, fkey, sizeof fkey) == sizeof fkey && !memcmp(cl_key, fkey, sizeof fkey)
           && hdr[1] > 0 && hdr[1] <= MAX_TRACKS && hdr[2] > 0 && hdr[2] <= MAX_ALBUMS
           && hdr[2] <= hdr[1] && hdr[3] > 0 && (unsigned)hdr[3] <= MAX_ARENA
           && hdr[5] == (int32_t)sl_lib_keep_apart && hdr[6] > 0 && hdr[6] <= hdr[1];
    if (ok)
    {
        /* Reject truncation/implausible lengths before allocating large
         * buffers from untrusted on-card counts. All terms are bounded. */
        size_t expected = sizeof cl_hdr + sizeof fkey + (size_t)hdr[1] * (sizeof(*rt) + sizeof(int))
                        + (size_t)hdr[2] * (sizeof(*ra) + 2 * sizeof(int)) + (size_t)hdr[6] * sizeof(*rd) + (size_t)hdr[3];
        ok = lseek(cl_fd, 0, SEEK_END) == (off_t)expected
          && lseek(cl_fd, sizeof cl_hdr + sizeof fkey, SEEK_SET) >= 0;
    }
    if (ok)
    {
        free_records();
        free(arena);
        n_rt = cap_rt = hdr[1];
        n_ra = cap_ra = hdr[2];
        n_rd = cap_rd = hdr[6];
        arena_len = arena_cap = hdr[3];
        rt = malloc(n_rt * sizeof(*rt));
        ra = malloc(n_ra * sizeof(*ra));
        rd = malloc(n_rd * sizeof(*rd));
        ord_songs = malloc(n_rt * sizeof(int));
        ord_title = malloc(n_ra * sizeof(int));
        ord_artist = malloc(n_ra * sizeof(int));
        arena = malloc(arena_len);
        ok = rt && ra && rd && ord_songs && ord_title && ord_artist && arena;
    }
    if (!ok)
    {
        close(cl_fd); cl_fd = -1;
        free_records();
        free(arena); arena = NULL; arena_len = arena_cap = 0;
        return false;
    }
    cl_phase = CL_READ;
    cl_read = 0;
    cl_hash = 2166136261u;
    return true;
}
/* the file's parts in its order, the checksum's too */
static size_t cache_part(int i, void **p)
{
    switch (i)
    {
        case 0: *p = rt; return n_rt * sizeof(*rt);
        case 1: *p = ra; return n_ra * sizeof(*ra);
        case 2: *p = rd; return n_rd * sizeof(*rd);
        case 3: *p = ord_songs; return n_rt * sizeof(int);
        case 4: *p = ord_title; return n_ra * sizeof(int);
        case 5: *p = ord_artist; return n_ra * sizeof(int);
        case 6: *p = arena; return arena_len;
    }
    return 0;
}
/* up to max more bytes, hashed as they come; true at the file's end */
static bool cache_read_some(size_t max, bool *broken)
{
    size_t at = cl_read;
    void *p;
    for (int i = 0; i < 7; i++)
    {
        size_t n = cache_part(i, &p);
        if (at >= n) { at -= n; continue; }
        if (!max) return false;
        size_t take = n - at < max ? n - at : max;
        char *to = (char *)p + at;
        if (read(cl_fd, to, take) != (ssize_t)take) { *broken = true; return false; }
        cl_hash = hash_bytes(cl_hash, to, take);
        cl_read += take; max -= take;
        if (take < n - at) return false;   /* the rest of this part: the next call */
        at = 0;
    }
    return true;
}
/* record k: the folders, then the songs, then the albums */
static bool cache_record_ok(int k)
{
    if (k < n_rd)   /* a folder starts and ends with '/'; a file's name has none */
        return valid_string(rd[k]) && STR(rd[k])[0] == '/' && STR(rd[k])[strlen(STR(rd[k])) - 1] == '/';
    if ((k -= n_rd) < n_rt)
        return rt[k].dir >= 0 && rt[k].dir < n_rd && valid_string(rt[k].file) && STR(rt[k].file)[0] && !strchr(STR(rt[k].file), '/')
            && valid_string(rt[k].title) && (rt[k].artist == -1 || valid_string(rt[k].artist))
            && rt[k].album >= 0 && rt[k].album < n_ra
            && rt[k].num >= 0 && rt[k].disc >= 0 && rt[k].len >= 0
            && (rt[k].lossless == 0 || rt[k].lossless == 1) && rt[k].start >= 0 && rt[k].cue >= 0 && rt[k].cue <= CUE_MAX;
    k -= n_rt;
    return valid_string(ra[k].title) && valid_string(ra[k].artist)
        && ra[k].year >= 0 && ra[k].year < 3000
        && (ra[k].genre == -1 || valid_string(ra[k].genre));
}
/* a song's artist from the records, as sl_track_artist has it */
static const char *rec_artist(int t) { return STR(rt[t].artist >= 0 ? rt[t].artist : ra[rt[t].album].artist); }
/* entry k of the kept orders (songs, albums by title, by artist): an
 * index met once, in order after the one before it. Another sort
 * setting or sort code finds them out of order: they are made again. */
static bool cache_order_ok(int k)
{
    int *o = ord_songs, n = n_rt;
    if (k >= n_rt) { k -= n_rt; o = ord_title; n = n_ra; }
    if (o == ord_title && k >= n_ra) { k -= n_ra; o = ord_artist; }
    if (!k) memset(ord_seen, 0, ((size_t)n + 7) / 8);
    int v = o[k], u = k ? o[k - 1] : 0;
    if (v < 0 || v >= n || ord_seen[v >> 3] & 1 << (v & 7)) return false;
    ord_seen[v >> 3] |= 1 << (v & 7);
    if (!k) return true;
    if (o == ord_songs) return song_order(u, STR(rt[u].title), rec_artist(u), v, STR(rt[v].title), rec_artist(v)) <= 0;
    if (o == ord_title) return sl_sort_cmp(STR(ra[u].title), STR(ra[v].title)) <= 0;
    return artist_order(STR(ra[u].artist), ra[u].year, STR(ra[v].artist), ra[v].year) <= 0;
}

/* ------------------------------------------------------- cue sheets */
/* A long file with a cue sheet (beside it, "<name>.cue", or inside it: a
 * FLAC's CUESHEET comment, an APE tag's Cuesheet item, as in .ape and .wv
 * images) is an album in one file: its tracks become songs, each the file
 * from its INDEX 01, the album named by the sheet. A sheet naming more
 * than one file is left alone (those files are songs of their own). */
#define CUE_MIN_MS (8 * 60 * 1000)      /* shorter files are songs, sheet or not */
#define CUE_TEXT (64 * 1024)
struct sl_cue {
    char title[128], perf[128];
    int n, files;
    struct { char title[128], perf[128]; long ms; } t[CUE_MAX];
};
static struct sl_cue cue_sheet;
static char cue_buf[CUE_TEXT + 1];

static uint32_t cue_le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
/* text as UTF-8: kept when it already is, else read as Latin-1 */
static void cue_utf8(char *s, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    bool valid = true;
    while (*p && valid)
    {
        int n = *p < 0x80 ? 0 : (*p & 0xe0) == 0xc0 ? 1 : (*p & 0xf0) == 0xe0 ? 2 : (*p & 0xf8) == 0xf0 ? 3 : -1;
        if (n < 0) { valid = false; break; }
        p++;
        while (n-- > 0) { if ((*p & 0xc0) != 0x80) { valid = false; break; } p++; }
    }
    if (valid) return;
    size_t len = strlen(s), out = 0;
    for (size_t i = 0; i < len; i++) out += (unsigned char)s[i] >= 0x80 ? 2 : 1;
    if (out >= cap) return;
    s[out] = '\0';
    for (size_t i = len; i-- > 0; )
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) s[--out] = (char)c;
        else { s[--out] = (char)(0x80 | (c & 0x3f)); s[--out] = (char)(0xc0 | (c >> 6)); }
    }
}
static void cue_value(char *out, size_t n, const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    const char *e;
    if (*s == '"') { s++; e = strrchr(s, '"'); if (!e || e < s) e = s + strlen(s); }
    else { e = s + strlen(s); while (e > s && (e[-1] == ' ' || e[-1] == '\r')) e--; }
    size_t len = (size_t)(e - s);
    if (len >= n) len = n - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}
/* the sheet in cue_buf, parsed into cue_sheet: true when it has tracks */
static bool cue_parse_text(void)
{
    memset(&cue_sheet, 0, sizeof cue_sheet);
    char *s = cue_buf;
    if ((unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb && (unsigned char)s[2] == 0xbf) s += 3;
    cue_utf8(s, CUE_TEXT + 1 - (size_t)(s - cue_buf));
    for (char *line = s; line && *line; )
    {
        char *next = strpbrk(line, "\r\n");
        if (next) { *next++ = '\0'; while (*next == '\r' || *next == '\n') next++; }
        while (*line == ' ' || *line == '\t') line++;
        int k = cue_sheet.n;
        if (!strncasecmp(line, "FILE ", 5)) cue_sheet.files++;
        else if (!strncasecmp(line, "TRACK ", 6))
        {
            if (k >= CUE_MAX) break;
            cue_sheet.t[k].ms = -1;
            cue_sheet.n++;
        }
        else if (!strncasecmp(line, "TITLE ", 6)) cue_value(k ? cue_sheet.t[k - 1].title : cue_sheet.title, 128, line + 6);
        else if (!strncasecmp(line, "PERFORMER ", 10)) cue_value(k ? cue_sheet.t[k - 1].perf : cue_sheet.perf, 128, line + 10);
        else if (!strncasecmp(line, "INDEX 01 ", 9) && k)
        {
            int m = 0, sec = 0, f = 0;
            if (sscanf(line + 9, "%d:%d:%d", &m, &sec, &f) == 3) cue_sheet.t[k - 1].ms = m * 60000L + sec * 1000L + f * 1000L / 75;
        }
        line = next;
    }
    /* every track placed, in order, inside one file */
    if (cue_sheet.n < 2 || cue_sheet.files != 1) return false;
    for (int i = 0; i < cue_sheet.n; i++)
        if (cue_sheet.t[i].ms < 0 || (i && cue_sheet.t[i].ms <= cue_sheet.t[i - 1].ms)) return false;
    return true;
}
static int cue_read(int fd, size_t n)
{
    if (n > CUE_TEXT) n = CUE_TEXT;
    ssize_t got = read(fd, cue_buf, n);
    if (got < 0) got = 0;
    cue_buf[got] = '\0';
    return (int)got;
}
/* a FLAC's CUESHEET=... comment */
static bool cue_flac(int fd)
{
    unsigned char h[4];
    if (read(fd, h, 4) != 4 || memcmp(h, "fLaC", 4)) return false;
    for (int blocks = 0; blocks < 64; blocks++)
    {
        if (read(fd, h, 4) != 4) return false;
        uint32_t len = (uint32_t)h[1] << 16 | h[2] << 8 | h[3];
        if ((h[0] & 0x7f) == 4)               /* VORBIS_COMMENT, little endian inside */
        {
            unsigned char b[4];
            if (read(fd, b, 4) != 4) return false;
            uint32_t vendor = cue_le32(b);
            if (vendor > len || lseek(fd, vendor, SEEK_CUR) < 0 || read(fd, b, 4) != 4) return false;
            for (uint32_t c = 0, count = cue_le32(b); c < count && c < 4096; c++)
            {
                if (read(fd, b, 4) != 4) return false;
                uint32_t clen = cue_le32(b);
                char key[9];
                if (clen > len) return false;
                if (clen > 9 && read(fd, key, 9) == 9)
                {
                    if (!strncasecmp(key, "CUESHEET=", 9)) return cue_read(fd, clen - 9) > 0;
                    if (lseek(fd, clen - 9, SEEK_CUR) < 0) return false;
                }
                else if (clen <= 9 && lseek(fd, clen, SEEK_CUR) < 0) return false;
            }
            return false;
        }
        if (h[0] & 0x80) return false;            /* the last block */
        if (lseek(fd, len, SEEK_CUR) < 0) return false;
    }
    return false;
}
/* an APEv2 tag's Cuesheet item, at the file's end (before an ID3v1 tag) */
static bool cue_ape(int fd)
{
    off_t end = lseek(fd, 0, SEEK_END);
    unsigned char f[32];
    for (int id3 = 0; id3 < 2; id3++)
    {
        off_t at = end - 32 - (id3 ? 128 : 0);
        if (at < 0 || lseek(fd, at, SEEK_SET) < 0 || read(fd, f, 32) != 32) return false;
        if (memcmp(f, "APETAGEX", 8)) continue;
        uint32_t size = cue_le32(f + 12), items = cue_le32(f + 16);
        if (size < 32 || (off_t)size > at + 32 || lseek(fd, at + 32 - size, SEEK_SET) < 0) return false;
        for (uint32_t i = 0; i < items && i < 256; i++)
        {
            unsigned char ih[8];
            char key[32];
            if (read(fd, ih, 8) != 8) return false;
            uint32_t vlen = cue_le32(ih);
            int k = 0;
            while (k < (int)sizeof key - 1 && read(fd, key + k, 1) == 1 && key[k]) k++;
            key[k] = '\0';
            if (vlen > size) return false;
            if (!strcasecmp(key, "Cuesheet")) return cue_read(fd, vlen) > 0;
            if (lseek(fd, vlen, SEEK_CUR) < 0) return false;
        }
        return false;
    }
    return false;
}
/* path's sheet, found and parsed into cue_sheet */
static bool cue_find(const char *path)
{
    char side[MAX_PATH + 8];
    strlcpy(side, path, sizeof side);
    char *dot = strrchr(side, '.'), *slash = strrchr(side, '/');
    if (!dot || (slash && dot < slash)) return false;
    static const char *const ext[] = { ".cue", ".CUE", ".Cue" };
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++)
    {
        strlcpy(dot, ext[i], sizeof side - (size_t)(dot - side));
        int fd = open(side, O_RDONLY);
        if (fd < 0) continue;
        bool got = cue_read(fd, CUE_TEXT) > 0;
        close(fd);
        if (got && cue_parse_text()) return true;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool got = cue_flac(fd) || cue_ape(fd);
    close(fd);
    return got && cue_parse_text();
}

/* --------------------------------------------------- stepped loader */
static const char *basename_noext(const char *path, char *buf, int size)
{
    const char *b = strrchr(path, '/');
    strlcpy(buf, b ? b + 1 : path, size);
    char *dot = strrchr(buf, '.');
    if (dot) *dot = '\0';
    return buf;
}

static int scan_begin(void)
{
    free_records();
    free(arena); arena = NULL; arena_len = arena_cap = 0;
    if (!grow_album_hash()) return SL_LIB_FAILED;
    if (!tagcache_search(&tcs, tag_filename))
    {
        sl_lib_free();
        return SL_LIB_FAILED;
    }
    scanning = true;
    sl_lib.scan_done = 0;
    sl_lib.scan_total = tagcache_get_stat()->ready ? tcs.entry_count : 0;
    return SL_LIB_LOADING;
}

int sl_lib_begin(void)
{
    sl_lib_free();
    if (!tagcache_is_usable())
        return SL_LIB_NO_DB;
    return cache_open() ? SL_LIB_LOADING : scan_begin();
}
bool sl_lib_scanning(void) { return scanning; }

/* library.bin, slice_us of it: read and hashed, its records checked, its
 * orders checked, then the library made (no sort when they hold). Anything
 * wrong with it and the database is read instead. */
static int cache_step(void)
{
    int64_t until = lib_us() + slice_us;
    int total = n_rd + n_rt + n_ra;
    do
    {
        if (cl_phase == CL_READ)
        {
            bool broken = false;
            if (!cache_read_some(64 * 1024, &broken)) { if (broken) goto bad; continue; }
            close(cl_fd); cl_fd = -1;
            if (arena[arena_len - 1] != '\0' || cl_hash != (unsigned)cl_hdr[4]) goto bad;
            cl_phase = CL_CHECK; cl_at = 0;
        }
        else if (cl_phase == CL_CHECK)
        {
            for (int end = cl_at + 512; cl_at < end && cl_at < total; cl_at++)
                if (!cache_record_ok(cl_at)) goto bad;
            if (cl_at < total) continue;
            cl_phase = CL_ORDER; cl_at = 0;
            ord_seen = malloc(((size_t)n_rt + 7) / 8);
            ord_ok = ord_seen != NULL;   /* no room to check them: sorted again */
        }
        else
        {
            int orders = n_rt + 2 * n_ra;
            for (int end = cl_at + 512; ord_ok && cl_at < end && cl_at < orders; cl_at++)
                ord_ok = cache_order_ok(cl_at);
            if (ord_ok && cl_at < orders) continue;
            cl_phase = CL_OFF;
            bool kept = ord_ok;
            if (!build_views()) goto bad;
            if (!kept) cache_save(cl_key);   /* sorted the new way: kept for the next start */
            free_records();
            return SL_LIB_READY;
        }
    } while (lib_us() < until);
    return SL_LIB_LOADING;
bad:
    sl_log("library: library.bin did not hold; reading the database");
    if (cl_fd >= 0) { close(cl_fd); cl_fd = -1; }
    cl_phase = CL_OFF;
    return scan_begin();
}

int sl_lib_step(int budget)
{
    static char path[MAX_PATH], buf[TAGCACHE_BUFSZ], album[TAGCACHE_BUFSZ], artist[TAGCACHE_BUFSZ];
    if (cl_phase) return cache_step();
    if (!scanning) return SL_LIB_FAILED;

    while (budget-- > 0)
    {
        if (!tagcache_get_next(&tcs, path, sizeof(path)))
        {
            /* Keep the database's search lock until the cache is written:
             * its key and records must describe the same commit. */
            int32_t key[6];
            bool ok = join_compilations() && tagcache_is_usable() && arena_trim() && build_views();
            if (ok && db_key(key)) cache_save(key);
            tagcache_search_finish(&tcs);
            scanning = false;
            free_records();
            if (!ok) sl_lib_free();
            return ok ? SL_LIB_READY : SL_LIB_FAILED;
        }
        /* a song an update took out keeps its place in the file name list,
         * its name blanked: not a song */
        if (path[0] != '/') continue;
        if (n_rt >= MAX_TRACKS) goto fail;
        int idx = tcs.idx_id;
        if (n_rt == cap_rt)
        {
            int nc = cap_rt ? cap_rt * 2 : 1024;
            struct rec_track *n = realloc(rt, nc * sizeof(*n));
            if (!n) goto fail;
            rt = n; cap_rt = nc;
        }
        if (!tagcache_retrieve(&tcs, idx, tag_title, buf, sizeof(buf)) || !buf[0] || !strcmp(buf, "<Untagged>"))
            basename_noext(path, buf, sizeof(buf));
        scan_loose = false;
        if (!tagcache_retrieve(&tcs, idx, tag_albumartist, artist, sizeof(artist))
            || !artist[0] || !strcmp(artist, "<Untagged>"))
        {
            scan_loose = true;
            if (!tagcache_retrieve(&tcs, idx, tag_artist, artist, sizeof(artist))
                || !artist[0] || !strcmp(artist, "<Untagged>"))
                strlcpy(artist, "Unknown Artist", sizeof(artist));
        }
        /* the song's own artist, kept only when it is not the album's */
        static char own[TAGCACHE_BUFSZ];
        if (!tagcache_retrieve(&tcs, idx, tag_artist, own, sizeof(own)) || !strcmp(own, "<Untagged>")
            || !strcmp(own, artist))
            own[0] = '\0';
        if (!tagcache_retrieve(&tcs, idx, tag_album, album, sizeof(album))
            || !album[0] || !strcmp(album, "<Untagged>"))
            strlcpy(album, "Singles", sizeof(album));

        char genre[48];
        if (!tagcache_retrieve(&tcs, idx, tag_genre, genre, sizeof(genre)) || !strcmp(genre, "<Untagged>"))
            genre[0] = '\0';
        int year = tagcache_get_numeric(&tcs, tag_year);
        struct rec_track *t = &rt[n_rt];
        t->num = tagcache_get_numeric(&tcs, tag_tracknumber);
        t->disc = tagcache_get_numeric(&tcs, tag_discnumber);
        t->len = tagcache_get_numeric(&tcs, tag_length);
        long mtime = tagcache_get_numeric(&tcs, tag_mtime);
        t->added = mtime > 0 ? (uint32_t)mtime : 0;
        if (t->num < 0) t->num = 0;
        if (t->disc < 0) t->disc = 0;
        if (t->len < 0) t->len = 0;
        const char *ext = strrchr(path, '.');
        t->lossless = ext && (!strcasecmp(ext, ".flac") || !strcasecmp(ext, ".wav") || !strcasecmp(ext, ".aiff")
                              || !strcasecmp(ext, ".aif") || !strcasecmp(ext, ".ape") || !strcasecmp(ext, ".wv")
                              || !strcasecmp(ext, ".dsf") || !strcasecmp(ext, ".dff"));
        t->start = t->cue = 0;
        if (t->len >= CUE_MIN_MS && cue_find(path))
        {
            /* an album in one file: a song a track, the album the sheet's */
            struct rec_track base = *t;
            const char *by = cue_sheet.perf[0] ? cue_sheet.perf : artist, *name = cue_sheet.title[0] ? cue_sheet.title : album;
            scan_loose = scan_loose && !cue_sheet.perf[0];
            int a = album_find_or_add(by, name, year > 0 && year < 3000 ? year : 0, genre);
            int32_t dir, shared;                    /* one copy of the file's name for all its tracks */
            if (a < 0 || !path_add(path, &dir, &shared)) goto fail;
            for (int i = 0; i < cue_sheet.n; i++)
            {
                if (n_rt >= MAX_TRACKS) goto fail;
                if (n_rt == cap_rt)
                {
                    int nc = cap_rt * 2;
                    struct rec_track *n = realloc(rt, nc * sizeof(*n));
                    if (!n) goto fail;
                    rt = n; cap_rt = nc;
                }
                struct rec_track *c = &rt[n_rt];
                *c = base;
                long from = cue_sheet.t[i].ms, to = i + 1 < cue_sheet.n ? cue_sheet.t[i + 1].ms : base.len;
                c->num = i + 1;
                c->start = (int32_t)from;
                c->len = to > from ? (int32_t)(to - from) : 0;
                c->cue = i + 1;
                c->album = a;
                c->dir = dir;
                c->file = shared;
                char fallback[24];
                snprintf(fallback, sizeof fallback, "Track %d", i + 1);
                c->title = arena_add(cue_sheet.t[i].title[0] ? cue_sheet.t[i].title : fallback);
                const char *perf = cue_sheet.t[i].perf;
                c->artist = perf[0] && strcmp(perf, by) ? arena_add(perf) : -1;
                if (c->title < 0 || (perf[0] && strcmp(perf, by) && c->artist < 0)) goto fail;
                n_rt++;
            }
            sl_lib.scan_done++;
            continue;
        }
        t->album = album_find_or_add(artist, album, year > 0 && year < 3000 ? year : 0, genre);
        bool placed = path_add(path, &t->dir, &t->file);
        t->title = arena_add(buf);
        t->artist = own[0] ? arena_add(own) : -1;
        if (t->album < 0 || !placed || t->title < 0 || (own[0] && t->artist < 0))
            goto fail;
        n_rt++;
        sl_lib.scan_done++;
    }
    return SL_LIB_LOADING;
fail:
    sl_log("library: scan failed (memory or library size limit); cache not saved");
    sl_lib_free();
    return SL_LIB_FAILED;
}

/* The tag database's key now (it moves on with every commit), and the
 * cache dropped, for an update that only took songs out: a removal leaves
 * the key as it was, so the next read must not trust the cache. */
bool sl_lib_db_key(int32_t key[6]) { return db_key(key); }
void sl_lib_cache_drop(void) { remove(CACHE_FILE); }

/* The song at ms into path: a cue sheet's track, the one playing then */
int sl_lib_find_path_at(const char *path, long ms)
{
    int t = sl_lib_find_path(path);
    if (t < 0 || !sl_lib.tracks[t].cue) return t;
    while (t + 1 < sl_lib.n_tracks && sl_lib.tracks[t + 1].cue == sl_lib.tracks[t].cue + 1
           && sl_lib.tracks[t + 1].start <= ms && sl_track_same_file(t + 1, t)) t++;
    return t;
}

int sl_lib_find_path(const char *path)
{
    if (!sl_lib.path_hash || !path) return -1;
    unsigned h = 2166136261u;
    for (const char *p = path; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    for (int j = 0; j < sl_lib.path_hash_cap; j++)
    {
        int slot = (h + j) & (sl_lib.path_hash_cap - 1);
        int t = sl_lib.path_hash[slot];
        if (t < 0) return -1;
        if (sl_track_path_is(t, path)) return t;
    }
    return -1;
}
