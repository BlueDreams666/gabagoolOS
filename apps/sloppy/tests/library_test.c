/* Exercise the real stepped loader against a fake tag database, actual
 * cache files, and an allocator that can fail every individual allocation. */
#define _DEFAULT_SOURCE /* truncate(), strlcpy() under -std=c99 on glibc */
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static int allocation, fail_at = -1, outstanding, search_open, searches;
static int rows = 64, cursor, unique_albums;
static bool compilation;   /* every album by Various Artists, every song by its own */
static bool loose_comp;    /* one album title in one folder, four song artists, no album artist tag */
static bool usable = true, search_ok = true, long_tags, blanked;   /* blanked: every fifth name taken out by an update */
static char cue_root[512];   /* cue tests: songs are real files under it, 40 minutes long */
static void *test_malloc(size_t n)
{
    if (++allocation == fail_at) return NULL;
    void *p = malloc(n); if (p) outstanding++; return p;
}
static void *test_calloc(size_t n, size_t s)
{
    if (++allocation == fail_at) return NULL;
    void *p = calloc(n, s); if (p) outstanding++; return p;
}
static void *test_realloc(void *p, size_t n)
{
    if (++allocation == fail_at) return NULL;
    bool fresh = !p;
    void *q = realloc(p, n); if (q && fresh) outstanding++; return q;
}
static void test_free(void *p) { if (p) outstanding--; free(p); }
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#include "../sloppy_lib.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

static struct tagcache_stat test_stat = { ROCKBOX_DIR, true };
bool sl_sort_as_written;
void sl_log(const char *fmt, ...) { (void)fmt; }
bool tagcache_is_usable(void) { return usable; }
struct tagcache_stat *tagcache_get_stat(void) { return &test_stat; }
bool tagcache_search(struct tagcache_search *s, int tag)
{
    (void)tag; assert(!search_open); searches++;
    cursor = 0; s->entry_count = rows;
    search_open = search_ok; return search_ok;
}
bool tagcache_get_next(struct tagcache_search *s, char *b, long size)
{
    if (!usable || cursor >= rows) return false;
    s->idx_id = cursor++;
    if (blanked && s->idx_id % 5 == 4) { b[0] = 0; return true; }
    if (cue_root[0]) { snprintf(b, size, "%s/Music/%05d.%s", cue_root, s->idx_id, s->idx_id == 2 ? "wv" : "flac"); return true; }
    snprintf(b, size, "/Music/%05d.flac", s->idx_id); return true;
}
bool tagcache_retrieve(struct tagcache_search *s, int idx, int tag, char *b, long size)
{
    (void)s;
    if (tag == tag_title)
    {
        if (idx == 0) { b[0] = 0; return false; }
        snprintf(b, size, "Track %05d", rows - idx);
    }
    else if (tag == tag_album)
    {
        if (long_tags)
        {
            char name[400]; memset(name, 'x', 300);
            snprintf(name + 300, sizeof(name) - 300, "%d", idx);
            strlcpy(b, name, size);
        }
        else if (loose_comp) strlcpy(b, "Mixtape", size);
        else snprintf(b, size, "Album %d", unique_albums ? idx : idx / 8);
    }
    else if (tag == tag_albumartist) { if (!compilation) { b[0] = 0; return false; } strlcpy(b, "Various Artists", size); }
    else if (tag == tag_genre) { if (idx % 3 == 2) return false; snprintf(b, size, "Genre %d", idx / 48); }
    else snprintf(b, size, "Artist %d", loose_comp ? idx % 4 : idx / 16);
    return true;
}
long tagcache_get_numeric(const struct tagcache_search *s, int tag)
{
    if (tag == tag_tracknumber)
        return s->idx_id % 8 == 0 ? INT_MAX : s->idx_id % 8 == 1 ? INT_MIN : 8 - s->idx_id % 8;
    if (tag == tag_discnumber) return 1;
    if (tag == tag_length) return cue_root[0] ? 2400000 : 180000;
    return 2026;
}
void tagcache_search_finish(struct tagcache_search *s)
{ (void)s; assert(search_open); search_open = 0; }

static int finish_load(int state)
{
    int steps = 0;
    while (state == SL_LIB_LOADING)
    {
        assert(++steps < 2 * rows + 8);   /* library.bin with one unit a step: a few per 512 songs */
        state = sl_lib_step(17);
    }
    return state;
}
static void assert_empty(void)
{
    assert(!sl_lib.n_tracks && !sl_lib.tracks && !sl_lib.albums);
    assert(!outstanding && !search_open && !arena && !rt && !ra && !ahash);
    assert(cl_fd < 0 && cl_phase == CL_OFF && !ord_songs && !ord_seen);
}
/* the library's lists in order: songs by title, albums by title and by artist */
static void assert_sorted(void)
{
    for (int i = 1; i < sl_lib.n_tracks; i++) assert(cmp_song_title(&sl_lib.songs_by_title[i - 1], &sl_lib.songs_by_title[i]) < 0);
    for (int i = 1; i < sl_lib.n_albums; i++)
        assert(cmp_album_title(&sl_lib.albums_by_title[i - 1], &sl_lib.albums_by_title[i]) <= 0
               && cmp_artist(&sl_lib.albums_by_artist[i - 1], &sl_lib.albums_by_artist[i]) <= 0);
}
static char *cache_bytes(long *size)
{
    FILE *f = fopen(CACHE_FILE, "rb"); assert(f);
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(*size); assert(b && fread(b, 1, *size, f) == (size_t)*size); fclose(f);
    return b;
}
static void clear_cache(void) { remove(CACHE_FILE); remove(CACHE_FILE ".tmp"); }
static void create_key(void)
{
    int fd = open(ROCKBOX_DIR "/database_idx.tcd", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int32_t key[6] = { 1, 2, 3, 4, 5, 6 };
    assert(fd >= 0 && write(fd, key, sizeof key) == sizeof key); close(fd);
}
static void expect_cache_rebuild(void)
{
    int before = searches;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before + 1);
    assert(sl_lib.n_tracks == rows);
    sl_lib_free(); assert_empty();
}
/* library.bin's orders do not hold (as after a sort change): sorted again
 * with no database read, written back once, then read as they are */
static void expect_cache_resort(void)
{
    long was_n, now_n, again_n;
    char *was = cache_bytes(&was_n);
    int before = searches;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before && sl_lib.n_tracks == rows);
    assert_sorted(); sl_lib_free(); assert_empty();
    char *now = cache_bytes(&now_n);
    assert(now_n == was_n && memcmp(was, now, was_n));
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before);
    assert_sorted(); sl_lib_free(); assert_empty();
    char *again = cache_bytes(&again_n);
    assert(again_n == now_n && !memcmp(now, again, now_n));
    free(was); free(now); free(again);
}
static void mutate_cache(int mutation)
{
    int fd = open(CACHE_FILE, O_RDWR); assert(fd >= 0);
    int32_t hdr[CACHE_HDR], key[6];
    assert(read(fd, hdr, sizeof hdr) == sizeof hdr);
    assert(read(fd, key, sizeof key) == sizeof key);
    size_t nt = hdr[1], na = hdr[2], ns = hdr[3], nd = hdr[6];
    struct rec_track *tracks = malloc(nt * sizeof(*tracks));
    struct rec_album *albums = malloc(na * sizeof(*albums));
    int32_t *dirs = malloc(nd * sizeof(*dirs));
    int *songs = malloc(nt * sizeof(int)), *by_title = malloc(na * sizeof(int)), *by_artist = malloc(na * sizeof(int));
    char *strings = malloc(ns);
    assert(tracks && albums && dirs && songs && by_title && by_artist && strings);
    assert(read(fd, tracks, nt * sizeof(*tracks)) == (ssize_t)(nt * sizeof(*tracks)));
    assert(read(fd, albums, na * sizeof(*albums)) == (ssize_t)(na * sizeof(*albums)));
    assert(read(fd, dirs, nd * sizeof(*dirs)) == (ssize_t)(nd * sizeof(*dirs)));
    assert(read(fd, songs, nt * sizeof(int)) == (ssize_t)(nt * sizeof(int)));
    assert(read(fd, by_title, na * sizeof(int)) == (ssize_t)(na * sizeof(int)));
    assert(read(fd, by_artist, na * sizeof(int)) == (ssize_t)(na * sizeof(int)));
    assert(read(fd, strings, ns) == (ssize_t)ns);
    switch (mutation)
    {
        case 0: tracks[0].album = na; break;
        case 1: tracks[0].file = ns; break;
        case 2: tracks[0].title++; break; /* middle of a string */
        case 3: strings[ns - 1] = 'x'; break;
        case 4: tracks[0].len = -1; break;
        case 5: tracks[0].lossless = 2; break;
        case 6: albums[0].artist = -1; break;
        case 7: albums[0].year = INT_MAX; break;
        case 8: strings[0] ^= 1; break; /* checksum mismatch */
        case 9: hdr[1] = INT_MAX; break;
        case 10: hdr[2] = hdr[1] + 1; break;
        case 11: hdr[3] = INT_MAX; break;
        case 12: hdr[0] = 0x534c4f32; break; /* old buggy cache */
        case 13: tracks[0].artist = ns; break;   /* a song's own artist past the strings */
        case 14: hdr[5] = !hdr[5]; break;        /* read with the other Join compilations */
        case 15: tracks[0].dir = nd; break;      /* a song's folder past the folders */
        case 16: tracks[0].cue = 256; break;     /* a cue number past a byte */
        case 17: hdr[0] = 0x534c4f39; break;     /* the cache before folders were kept once (whole paths) */
        case 18: dirs[0] = tracks[0].file; break;   /* a folder that is a file's name (no '/' around it) */
        case 19: tracks[1].file = dirs[0]; break;   /* a file's name with '/' in it */
        case 20: hdr[0] = 0x534c4f41; break;     /* the cache before the orders were kept */
        /* orders that do not hold: sorted again, not read again */
        case 21: { int t = songs[0]; songs[0] = songs[1]; songs[1] = t; break; }
        case 22: by_title[0] = na; break;        /* past the albums */
        case 23: by_artist[1] = by_artist[0]; break;   /* one album twice */
        case 24: songs[nt - 1] = -1; break;
        default: assert(false);
    }
    if (mutation != 8)
    {
        unsigned h = hash_bytes(2166136261u, tracks, nt * sizeof(*tracks));
        h = hash_bytes(h, albums, na * sizeof(*albums));
        h = hash_bytes(h, dirs, nd * sizeof(*dirs));
        h = hash_bytes(h, songs, nt * sizeof(int));
        h = hash_bytes(h, by_title, na * sizeof(int));
        h = hash_bytes(h, by_artist, na * sizeof(int));
        hdr[4] = hash_bytes(h, strings, ns);
    }
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(write(fd, hdr, sizeof hdr) == sizeof hdr);
    assert(write(fd, key, sizeof key) == sizeof key);
    assert(write(fd, tracks, nt * sizeof(*tracks)) == (ssize_t)(nt * sizeof(*tracks)));
    assert(write(fd, albums, na * sizeof(*albums)) == (ssize_t)(na * sizeof(*albums)));
    assert(write(fd, dirs, nd * sizeof(*dirs)) == (ssize_t)(nd * sizeof(*dirs)));
    assert(write(fd, songs, nt * sizeof(int)) == (ssize_t)(nt * sizeof(int)));
    assert(write(fd, by_title, na * sizeof(int)) == (ssize_t)(na * sizeof(int)));
    assert(write(fd, by_artist, na * sizeof(int)) == (ssize_t)(na * sizeof(int)));
    assert(write(fd, strings, ns) == (ssize_t)ns);
    close(fd); free(tracks); free(albums); free(dirs); free(songs); free(by_title); free(by_artist); free(strings);
}
int main(void)
{
    /* list order: accents fold, case and articles and leading quotes go, other scripts after Z */
    assert(sl_sort_cmp("\xc3\x89" "dith Piaf", "Fleetwood Mac") < 0 && sl_sort_cmp("Eagles", "\xc3\x89" "dith Piaf") < 0);
    assert(sl_sort_cmp("The Beatles", "Bjork") < 0 && sl_sort_cmp("\"Heroes\"", "Hey") < 0);
    assert(sl_sort_cmp("\xc2\xbfQui\xc3\xa9n?", "Queen") > 0 && sl_sort_cmp("zz", "\xd7\x90") < 0);
    assert(!sl_sort_cmp("abba", "ABBA") && sl_sort_cmp("Ab", "Abc") < 0 && sl_sort_cmp("\xc2\xa9 2020", "\xc2\xa9 2021") < 0);
    mkdir(ROCKBOX_DIR); create_key();
    slice_us = 0;   /* library.bin read a unit a step: every slice boundary is crossed */
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY);
    assert(sl_lib.n_tracks == rows && sl_lib.n_albums == 8 && sl_lib.n_artists == 4);
    assert(!strcmp(sl_lib.tracks[0].title, "00000"));
    char tp[MAX_PATH];
    for (int i = 0; i < rows; i++) assert(sl_lib_find_path(sl_track_path(i, tp, sizeof tp)) == i && sl_track_path_is(i, tp));
    assert(sl_lib.n_dirs == 1 && !strcmp(sl_lib.dirs[0], "/Music/") && !strcmp(sl_lib.tracks[3].file, "00003.flac"));   /* the folder once */
    assert(!strcmp(sl_track_path(3, tp, 8), "/Music/") && !strcmp(sl_track_path(3, tp, 10), "/Music/00"));   /* cut to fit, never past */
    assert(!sl_track_path_is(3, "/Music/") && !sl_track_path_is(3, "/Music/00003.flacx") && !sl_track_path_is(3, "/Musi"));
    assert(sl_lib_find_path("/missing") == -1);
    for (int a = 0; a < sl_lib.n_albums; a++)
    {
        struct sl_album *al = &sl_lib.albums[a]; assert(al->n_tracks == 8);
        assert(al->genre && !strncmp(al->genre, "Genre ", 6));   /* a track's genre, untagged ones skipped */
        for (int i = 1; i < al->n_tracks; i++)
            assert(cmp_track_in_album(&al->tracks[i - 1], &al->tracks[i]) <= 0);
    }
    int cold_points = allocation;
    sl_lib_free(); assert_empty();
    int before = searches;
    allocation = 0;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before);
    assert_sorted();
    for (int a = 0; a < sl_lib.n_albums; a++) assert(sl_lib.albums[a].genre && !strncmp(sl_lib.albums[a].genre, "Genre ", 6));
    int cache_points = allocation;
    sl_lib_free(); assert_empty();
    for (int i = 1; i <= cache_points; i++)
    {
        allocation = 0; fail_at = i;
        assert(finish_load(sl_lib_begin()) == SL_LIB_READY);
        sl_lib_free(); assert_empty();
    }
    int spared = 0;   /* the arena's trim (sloppy_lib.c arena_trim): it failing leaves the arena as it was */
    for (int i = 1; i <= cold_points; i++)
    {
        clear_cache(); allocation = 0; fail_at = i;
        int r = finish_load(sl_lib_begin());
        if (r == SL_LIB_READY) { spared++; assert(sl_lib.n_tracks == rows && sl_lib.n_albums == 8); sl_lib_free(); assert_empty(); continue; }
        assert(r == SL_LIB_FAILED);
        assert(access(CACHE_FILE, F_OK) != 0); assert_empty();
    }
    assert(spared == 1);   /* every other allocation failing still fails the read cleanly */
    fail_at = -1;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY); sl_lib_free();
    for (int i = 0; i < 21; i++) { mutate_cache(i); expect_cache_rebuild(); }
    for (int i = 21; i < 25; i++) { mutate_cache(i); expect_cache_resort(); }
    /* library.bin stopped mid-read (USB mode): let go of, then read whole */
    before = searches;
    assert(sl_lib_begin() == SL_LIB_LOADING && cl_phase == CL_READ && cl_fd >= 0);
    sl_lib_free(); assert_empty();
    assert(sl_lib_begin() == SL_LIB_LOADING && sl_lib_step(17) == SL_LIB_LOADING && cl_phase == CL_CHECK);
    sl_lib_free(); assert_empty();
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before); sl_lib_free(); assert_empty();
    assert(truncate(CACHE_FILE, 100) == 0); expect_cache_rebuild();
    int fd = open(CACHE_FILE, O_WRONLY | O_APPEND); assert(fd >= 0);
    assert(write(fd, "x", 1) == 1); close(fd); expect_cache_rebuild();
    clear_cache(); assert(sl_lib_begin() == SL_LIB_LOADING);
    /* stopped mid-read (USB mode): the tag database is closed, and a read
     * started over runs to the end */
    assert(sl_lib_step(4) == SL_LIB_LOADING && search_open); sl_lib_free(); assert_empty(); assert(!search_open);
    assert(sl_lib_begin() == SL_LIB_LOADING); assert(finish_load(SL_LIB_LOADING) == SL_LIB_READY && !search_open);
    clear_cache(); sl_lib_free();
    assert(sl_lib_begin() == SL_LIB_LOADING); usable = false;
    assert(sl_lib_step(4) == SL_LIB_FAILED); assert_empty();
    assert(sl_lib_begin() == SL_LIB_NO_DB); assert_empty(); usable = true;
    search_ok = false; assert(sl_lib_begin() == SL_LIB_FAILED); assert_empty(); search_ok = true;
    rows = 0; assert(finish_load(sl_lib_begin()) == SL_LIB_FAILED); assert_empty();
    rows = 9000; unique_albums = 1;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY);
    assert(sl_lib.n_tracks == rows && sl_lib.n_albums == rows);
    sl_lib_free(); assert_empty(); before = searches;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before && sl_lib.n_tracks == rows);   /* in a few hundred slices */
    assert_sorted(); sl_lib_free(); assert_empty(); clear_cache();
    /* a compilation: the album is Various Artists, each song keeps its own, cold and from the cache */
    rows = 32; compilation = true; clear_cache();
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY);
    assert(!strcmp(sl_lib.albums[sl_lib.tracks[0].album].artist, "Various Artists"));
    assert(sl_lib.tracks[0].artist && !strncmp(sl_track_artist(0), "Artist ", 7));
    sl_lib_free(); assert_empty();
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY);
    assert(sl_lib.tracks[0].artist && !strncmp(sl_track_artist(0), "Artist ", 7));
    sl_lib_free(); assert_empty(); clear_cache(); compilation = false;
    /* untagged: four song artists, one album title, one folder: one album, each song keeping its artist */
    rows = 32; loose_comp = true; clear_cache();
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY);
    assert(sl_lib.n_albums == 1 && sl_lib.albums[0].n_tracks == 32 && !strcmp(sl_lib.albums[0].artist, "Various Artists"));
    for (int i = 0; i < 32; i++) assert(!strncmp(sl_track_artist(i), "Artist ", 7) && sl_lib.tracks[i].album == 0);
    sl_lib_free(); assert_empty();
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && sl_lib.n_albums == 1);   /* from the cache too */
    sl_lib_free(); assert_empty(); clear_cache(); loose_comp = false;
    /* Join compilations off: the same songs stay an album per song artist,
     * and a cache read joined is read again */
    rows = 32; loose_comp = true; clear_cache();
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && sl_lib.n_albums == 1); sl_lib_free();
    sl_lib_keep_apart = true; before = searches;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before + 1 && sl_lib.n_albums == 4);
    sl_lib_free(); before = searches;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before && sl_lib.n_albums == 4);   /* its own cache */
    sl_lib_free(); assert_empty(); sl_lib_keep_apart = false; clear_cache(); loose_comp = false;
    /* Ignore The, A and An off: names sort as written */
    sl_sort_as_written = true; assert(sl_sort_cmp("The Beatles", "Bjork") > 0 && sl_sort_cmp("\"Heroes\"", "Hey") < 0);
    sl_sort_as_written = false; assert(sl_sort_cmp("The Beatles", "Bjork") < 0);
    /* songs an update took out (their names blanked) are not songs, and
     * the cache read with them left out is good the next time */
    rows = 40; blanked = true; clear_cache();
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && sl_lib.n_tracks == 32);
    for (int i = 0; i < sl_lib.n_tracks; i++) assert(sl_track_path(i, tp, sizeof tp)[0] == '/');
    sl_lib_free(); before = searches;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before && sl_lib.n_tracks == 32);
    sl_lib_free(); assert_empty(); clear_cache(); blanked = false;
    /* albums in one file: a sheet beside one, a FLAC's own CUESHEET, a
     * WavPack's APE tag; each sheet's tracks are songs, the album its own */
    {
        assert(getcwd(cue_root, sizeof cue_root)); mkdir("Music");
        static const char sheet[] = "PERFORMER \"S\xc3\xa9verine\"\nTITLE \"Live \xc3\xa0 Paris\"\nFILE \"00000.flac\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"Ouverture\"\n    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n    TITLE \"Deux\"\n    PERFORMER \"Guest\"\n    INDEX 00 04:58:00\n    INDEX 01 05:00:00\n"
            "  TRACK 03 AUDIO\n    TITLE \"Trois\"\n    INDEX 01 12:30:37\n";
        FILE *f = fopen("Music/00000.cue", "w"); fputs(sheet, f); fclose(f);
        f = fopen("Music/00000.flac", "wb"); fputs("fLaC", f); fclose(f);
        /* a FLAC: STREAMINFO, then a VORBIS_COMMENT with a vendor, a title and the sheet (Latin-1 here) */
        static const char latin[] = "TITLE \"Caf\xe9\"\nFILE \"x.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"A\"\n    INDEX 01 00:00:00\n"
                                    "  TRACK 02 AUDIO\n    TITLE \"B\"\n    INDEX 01 20:00:00\n";
        unsigned char blk[1024]; int n = 0;
        #define LE32(v) do { uint32_t v_ = (v); blk[n++] = v_; blk[n++] = v_ >> 8; blk[n++] = v_ >> 16; blk[n++] = v_ >> 24; } while (0)
        LE32(3); memcpy(blk + n, "abc", 3); n += 3; LE32(2);
        LE32(7); memcpy(blk + n, "TITLE=x", 7); n += 7;
        LE32(9 + sizeof latin - 1); memcpy(blk + n, "cuesheet=", 9); n += 9; memcpy(blk + n, latin, sizeof latin - 1); n += sizeof latin - 1;
        f = fopen("Music/00001.flac", "wb"); fputs("fLaC", f);
        unsigned char si[4 + 34] = { 0, 0, 0, 34 }; fwrite(si, 1, sizeof si, f);
        unsigned char vh[4] = { 0x84, (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n }; fwrite(vh, 1, 4, f);
        fwrite(blk, 1, n, f); fclose(f);
        /* a WavPack with an APEv2 tag at its end */
        static const char wv[] = "FILE \"y.wv\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 10:00:00\n";
        n = 0; LE32(sizeof wv - 1); LE32(0); memcpy(blk + n, "Cuesheet", 9); n += 9; memcpy(blk + n, wv, sizeof wv - 1); n += sizeof wv - 1;
        int items = n; n = items;
        unsigned char foot[32] = "APETAGEX"; uint32_t sz = (uint32_t)items + 32;
        foot[8] = 0xd0; foot[9] = 7; foot[12] = sz; foot[13] = sz >> 8; foot[16] = 1;
        f = fopen("Music/00002.wv", "wb"); fwrite("wvpk", 1, 4, f); fwrite(blk, 1, items, f); fwrite(foot, 1, 32, f); fclose(f);
        #undef LE32
        rows = 4; clear_cache();
        assert(finish_load(sl_lib_begin()) == SL_LIB_READY);
        assert(sl_lib.n_tracks == 3 + 2 + 2 + 1);           /* the fourth file has no sheet: a song */
        sl_track_path(0, tp, sizeof tp);
        int t0 = sl_lib_find_path(tp);
        assert(sl_lib.tracks[1].file == sl_lib.tracks[0].file && sl_track_same_file(0, 2) && !sl_track_same_file(2, 3));   /* one name for the sheet's tracks */
        assert(t0 == 0 && sl_lib.tracks[0].cue == 1 && sl_lib.tracks[2].cue == 3 && !strcmp(sl_lib.tracks[1].title, "Deux"));
        assert(sl_lib.tracks[1].start == 300000 && sl_lib.tracks[1].len == 450493 && !strcmp(sl_track_artist(1), "Guest"));
        assert(sl_lib.tracks[2].start == 750493 && sl_lib.tracks[2].len == 2400000 - 750493);
        const struct sl_album *al = &sl_lib.albums[sl_lib.tracks[0].album];
        assert(!strcmp(al->title, "Live \xc3\xa0 Paris") && !strcmp(al->artist, "S\xc3\xa9verine") && al->n_tracks == 3);
        assert(sl_lib_find_path_at(tp, 0) == 0 && sl_lib_find_path_at(tp, 299999) == 0
               && sl_lib_find_path_at(tp, 300000) == 1 && sl_lib_find_path_at(tp, 2000000) == 2);
        assert(!strcmp(sl_lib.albums[sl_lib.tracks[3].album].title, "Caf\xc3\xa9") && sl_lib.tracks[4].start == 1200000);   /* Latin-1 read as such */
        assert(sl_lib.tracks[5].cue == 1 && !strcmp(sl_lib.tracks[6].title, "Track 2") && sl_lib.tracks[6].start == 600000);
        assert(!sl_lib.tracks[7].cue && sl_lib.tracks[7].len == 2400000);
        sl_lib_free(); before = searches;
        assert(finish_load(sl_lib_begin()) == SL_LIB_READY && searches == before && sl_lib.n_tracks == 8 && sl_lib.tracks[1].start == 300000);   /* from the cache */
        sl_lib_free(); assert_empty(); clear_cache(); cue_root[0] = 0;
    }
    rows = 2; long_tags = true;
    assert(finish_load(sl_lib_begin()) == SL_LIB_READY);
    assert(sl_lib.n_albums == 2 && strlen(sl_lib.albums[0].title) == 301);
    sl_lib_free(); assert_empty();
    printf("PASS: %d cold-load allocation failures, %d cache-load allocation failures; "
           "23 corrupt/truncated caches, 4 orders sorted again, a cache read stopped and read again; sort extremes; compilations kept apart; articles; names an update took out; cue sheets beside, in FLAC and in APE tags; path index; metadata fallback; "
           "9000 albums; long tags; interrupted/unavailable/empty database; cleanup.\n",
           cold_points, cache_points);
}
