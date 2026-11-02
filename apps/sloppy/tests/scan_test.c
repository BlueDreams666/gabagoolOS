/* The library scan, end to end, on the Mac: Rockbox's own tag database
 * (apps/tagcache.c, built as the database tool builds it) scans a card the
 * generator filled (gen_library.py), then GabagoolOS's loader
 * (sloppy_lib.c) reads that database as it does on the player. Every
 * audio file on the card must come out as a song, none twice. Then the
 * card changes (songs added, songs deleted, a song retagged) and an
 * update, not a rebuild, must find exactly that. run-scan-test.sh builds
 * and runs it; it prints the times and the memory the loader holds.
 *
 *   scan-test <card dir> [--update]   (run from inside the card dir) */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ftw.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include "config.h"
#include "tagcache.h"
#include "dir.h"

/* the loader, whole, so its arena and records can be measured */
bool sl_sort_as_written;
#include "../sloppy_lib.c"
void sl_log(const char *fmt, ...) { (void)fmt; }

/* the database tool's: no settings, no threads */
const char *sim_root_dir = ".";
#include "kernel.h"
void mutex_init(struct mutex *m) { (void)m; }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
void sim_thread_lock(void *me) { (void)me; }
void *sim_thread_unlock(void) { return (void *)1; }
/* tagcache.c leaves this out of the tool; the loader asks for the path */
static struct tagcache_stat scan_stat;
struct tagcache_stat *tagcache_get_stat(void)
{
    strlcpy(scan_stat.db_path, ROCKBOX_DIR, sizeof scan_stat.db_path);
    scan_stat.ready = true;
    return &scan_stat;
}

/* sloppy_history_state.h's sl_hist_hash, which sl_track_hist_hash must match */
static uint32_t sl_hist_hash_of(const char *path)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) h = (h ^ *p) * 16777619u;
    return h ? h : 1;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static long peak_kb(void) { struct rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_maxrss / 1024; }   /* bytes on macOS */

/* the audio files on the card, by walking it */
static char **disk; static int n_disk, cap_disk;
static int walk_cb(const char *p, const struct stat *s, int type, struct FTW *f)
{
    (void)s; (void)f;
    if (type != FTW_F) return 0;
    const char *e = strrchr(p, '.');
    if (!e || (strcasecmp(e, ".mp3") && strcasecmp(e, ".iso") && strcasecmp(e, ".flac")) || strstr(p, "/.rockbox/")) return 0;
    if (n_disk == cap_disk) { cap_disk = cap_disk ? cap_disk * 2 : 4096; disk = realloc(disk, cap_disk * sizeof *disk); }
    disk[n_disk++] = strdup(p + 1);          /* "./Music/..." -> "/Music/..." */
    return 0;
}
/* a load as the player steps it: its time, its steps and the longest
 * one (what the screen waits on), the last (the library made) */
static int load_steps;
static double load_longest, load_last;
static int load(double *ms)
{
    double t0 = now_ms(), at = t0;
    int state = sl_lib_begin();
    load_steps = 0; load_longest = load_last = now_ms() - at;
    while (state == SL_LIB_LOADING)
    {
        at = now_ms();
        state = sl_lib_step(12);
        load_last = now_ms() - at;
        if (load_last > load_longest) load_longest = load_last;
        load_steps++;
    }
    *ms = now_ms() - t0;
    return state;
}
/* every file on the card is a song, once */
static int check_all(const char *when)
{
    n_disk = 0;
    nftw(".", walk_cb, 64, FTW_PHYS);
    int walked = n_disk;
    /* a path longer than Rockbox's MAX_PATH can't be in its database: counted apart */
    int missing = 0, too_long = 0;
    for (int i = 0; i < n_disk; i++)
        if (strlen(disk[i]) >= MAX_PATH) too_long += sl_lib_find_path(disk[i]) < 0;
        else if (sl_lib_find_path(disk[i]) < 0) { if (missing++ < 10) fprintf(stderr, "%s: not in the library: %s\n", when, disk[i]); }
    if (too_long) printf("%s: %d files skipped, their paths past Rockbox's %d bytes\n", when, too_long, MAX_PATH - 1);
    n_disk -= too_long;
    /* an album in one file is a song a track of its sheet */
    int cue_extra = 0;
    for (int t = 0; t < sl_lib.n_tracks; t++) if (sl_lib.tracks[t].cue > 1) cue_extra++;
    n_disk += cue_extra;
    /* and nothing that isn't there, nothing twice */
    int stale = 0, twice = 0;
    for (int t = 0; t < sl_lib.n_tracks; t++)
    {
        struct stat st;
        char p[MAX_PATH + 2], tp[MAX_PATH];
        sl_track_path(t, tp, sizeof tp);
        snprintf(p, sizeof p, ".%s", tp);
        if (stat(p, &st)) { if (stale++ < 10) fprintf(stderr, "%s: not on the card: %s\n", when, tp); }
        else if (sl_lib.tracks[t].cue <= 1 && sl_lib_find_path(tp) != t) { if (twice++ < 10) fprintf(stderr, "%s: twice: %s\n", when, tp); }
        else if (!sl_track_path_is(t, tp) || sl_track_hist_hash(t) != sl_hist_hash_of(tp)) { if (twice++ < 10) fprintf(stderr, "%s: path read back wrong: %s\n", when, tp); }
    }
    for (int i = 0; i < walked; i++) free(disk[i]);
    printf("%s: %d files on the card, %d songs in the library, %d missing, %d gone from the card, %d twice\n",
           when, n_disk, sl_lib.n_tracks, missing, stale, twice);
    return missing || stale || twice || n_disk != sl_lib.n_tracks;
}

int main(int argc, char **argv)
{
    const char *paths[] = { "/", NULL };
    if (argc < 2 || chdir(argv[1])) { fprintf(stderr, "scan-test <card dir> [--update|--same]\n"); return 2; }
    bool same = argc > 2 && !strcmp(argv[2], "--same");
    bool update = same || (argc > 2 && !strcmp(argv[2], "--update"));
    int bad = 0;
    double t0 = now_ms();
    tagcache_init();
    int32_t key0[6], key1[6];
    bool had = sl_lib_db_key(key0);
    do_tagcache_build(paths);
    tagcache_reverse_scan();
    double scan = now_ms() - t0;
    if (same)
    {
        /* the start-up check on a card nothing happened to: the database
         * stays as it was, so the player keeps its library.bin */
        bool kept = had && sl_lib_db_key(key1) && !memcmp(key0, key1, sizeof key0);
        printf("update with no changes: %.1f s, the database %s\n", scan / 1000, kept ? "untouched" : "WRITTEN AGAIN");
        printf("%s\n", kept ? "PASS" : "FAIL");
        return !kept;
    }
    long kb_scan = peak_kb();
    double cold, warm;
    if (!update)
    {
        remove(CACHE_FILE);
        bad |= load(&cold) != SL_LIB_READY;
        size_t held = arena_len + (size_t)sl_lib.n_tracks * (sizeof(struct sl_track) + 2 * sizeof(int))
                    + (size_t)sl_lib.n_albums * (sizeof(struct sl_album) + 2 * sizeof(int)) + sl_lib.path_hash_cap * sizeof(int)
                    + (size_t)sl_lib.n_dirs * sizeof(char *);
        /* the same on the R1, where pointers are 4 bytes: a song 44 bytes, an album 28, a folder 4 */
        size_t r1 = arena_cap + (size_t)sl_lib.n_tracks * (44 + 3 * 4) + (size_t)sl_lib.n_albums * (28 + 2 * 4)
                  + sl_lib.path_hash_cap * 4 + (size_t)sl_lib.n_dirs * 4;
        printf("scan: tag database built in %.1f s (peak %ld MB of process memory)\n", scan / 1000, kb_scan / 1024);
        printf("load: %d folders\n", sl_lib.n_dirs);
        printf("load: %d songs, %d albums, %d album artists read from the database in %.1f s; the library holds %.1f MB here, %.1f MB on the R1 (%.1f MB of it names and paths)\n",
               sl_lib.n_tracks, sl_lib.n_albums, sl_lib.n_artists, cold / 1000, held / 1048576.0, r1 / 1048576.0, arena_len / 1048576.0);
        bad |= check_all("first scan");
        /* the two albums in one file: three songs each, named by their sheets */
        for (int k = 0; k < 2; k++)
        {
            const char *img = k ? "/Music/Cue/Beside.flac" : "/Music/Cue/Inside.flac";
            int t = sl_lib_find_path(img);
            bool ok = t >= 0 && t + 2 < sl_lib.n_tracks && sl_lib.tracks[t].cue == 1 && sl_lib.tracks[t + 2].cue == 3
                   && !strcmp(sl_lib.tracks[t + 1].title, "Two") && sl_lib.tracks[t + 1].start == 720000
                   && sl_lib.tracks[t + 2].len == 2400000 - 1530000 && !strcmp(sl_track_artist(t + 2), "Cue Guest")
                   && !strcmp(sl_lib.albums[sl_lib.tracks[t].album].title, k ? "Sheet beside" : "Sheet inside");
            if (!ok) { fprintf(stderr, "cue sheet %s: not read as its tracks\n", img); bad = 1; }
            else printf("cue sheet %s: %s, its three tracks\n", k ? "beside" : "inside", sl_lib.albums[sl_lib.tracks[t].album].title);
        }
        int iso = sl_lib_find_path("/Music/SACD/Test disc.iso");
        if (access("Music/SACD", F_OK) == 0)
        {
            if (iso < 0 || sl_lib.tracks[iso].len < 9000) { fprintf(stderr, "the SACD image isn't a song\n"); bad = 1; }
            else printf("SACD image: a song of %ld s, \"%s\"\n", sl_lib.tracks[iso].len / 1000, sl_lib.tracks[iso].title);
        }
        int *order = malloc((sl_lib.n_tracks ? sl_lib.n_tracks : 1) * sizeof(int)), n_order = sl_lib.n_tracks;
        memcpy(order, sl_lib.songs_by_title, n_order * sizeof(int));
        sl_lib_free();
        bad |= load(&warm) != SL_LIB_READY;
        printf("load from library.bin: %.0f ms in %d steps, the longest %.1f ms, the last (the library made) %.1f ms\n",
               warm, load_steps, load_longest, load_last);
        /* the kept order is the sorted one */
        if (sl_lib.n_tracks != n_order || memcmp(order, sl_lib.songs_by_title, n_order * sizeof(int)))
        { fprintf(stderr, "library.bin's song order is not the sorted one\n"); bad = 1; }
        free(order);
        sl_lib_free();
    }
    else
    {
        /* the update after the card changed: as the player's Q_UPDATE does */
        printf("update: tag database updated in %.1f s, %d entries taken out\n", scan / 1000, tagcache_entries_deleted);
        sl_lib_cache_drop();
        bad |= load(&cold) != SL_LIB_READY;
        printf("load after the update: %d songs in %.1f s\n", sl_lib.n_tracks, cold / 1000);
        bad |= check_all("after the update");
        /* the retagged song carries its new title */
        int t = sl_lib_find_path("/Music/Artist 00000/Album 000000/01 Song 000000.mp3");
        if (t < 0 || strcmp(sl_lib.tracks[t].title, "Retagged")) { fprintf(stderr, "the retagged song kept its old title\n"); bad = 1; }
        else printf("retagged: the new title is read\n");
        sl_lib_free();
    }
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}
