/* GabagoolOS internals. GPLv2+. */
#ifndef SLOPPY_INT_H
#define SLOPPY_INT_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sloppy_fold.h"                 /* sl_q_fold_char: the sort folds as search does */

struct sl_track {
    const char *file;    /* its file's name in its folder, sl_lib.dirs[dir]; NULL: none. Its path: sl_track_path */
    const char *title;
    int album, num, disc;
    long len;            /* ms */
    uint32_t added;      /* file time from the database: larger is newer */
    bool lossless;
    const char *artist;  /* its own artist when not the album's (compilations, guests); else NULL */
    long start;          /* a cue sheet's track: where it starts in its file, ms */
    unsigned dir : 24;   /* its folder, an index into sl_lib.dirs (one word with cue: a song is 44 bytes on the R1) */
    unsigned cue : 8;    /* a cue sheet's track: its number in the sheet (1..99), the file shared; else 0 */
};
struct sl_album {
    const char *title, *artist;
    int year, n_tracks, hue;
    int *tracks;         /* indexes into sl_lib.tracks, in play order */
    const char *genre;   /* the first track's that has one; NULL when none */
};
struct sl_lib {
    struct sl_track *tracks; int n_tracks, cap_tracks;
    struct sl_album *albums; int n_albums, cap_albums;
    int *album_hash; int album_hash_cap;
    int *albums_by_title, *albums_by_artist, *songs_by_title;
    int n_artists;
    int scan_done, scan_total;   /* progress of a full database read */
    int *path_hash; int path_hash_cap;
    const char **dirs; int n_dirs;   /* each folder once, ending in '/': a song's path is its folder, then its file */
};
extern struct sl_lib sl_lib;
/* Song t's path ("/Music/A/01 x.flac"), made in buf (MAX_PATH always
 * fits) and returned; "" when it has none. The library keeps no whole
 * paths, so this is the only way to one: buf is the caller's, which keeps
 * it safe on any thread and for as long as the caller needs it. */
static inline const char *sl_track_path(int t, char *buf, size_t size)
{
    const struct sl_track *tr = &sl_lib.tracks[t];
    if (!size) return buf;
    buf[0] = '\0';
    if (!tr->file) return buf;
    const char *d = sl_lib.dirs[tr->dir];
    size_t ld = strlen(d), lf = strlen(tr->file);
    if (ld > size - 1) ld = size - 1;
    if (lf > size - 1 - ld) lf = size - 1 - ld;
    memcpy(buf, d, ld);
    memcpy(buf + ld, tr->file, lf);
    buf[ld + lf] = '\0';
    return buf;
}
/* Song t's path is path (no copy made) */
static inline bool sl_track_path_is(int t, const char *path)
{
    const struct sl_track *tr = &sl_lib.tracks[t];
    if (!tr->file || !path) return false;
    for (const char *d = sl_lib.dirs[tr->dir]; *d; d++, path++)
        if (*d != *path) return false;
    return !strcmp(tr->file, path);
}
/* Songs a and b are one file (a cue sheet's tracks share theirs) */
static inline bool sl_track_same_file(int a, int b)
{
    const struct sl_track *x = &sl_lib.tracks[a], *y = &sl_lib.tracks[b];
    if (!x->file || !y->file) return false;
    return x->dir == y->dir && (x->file == y->file || !strcmp(x->file, y->file));   /* a folder is there once */
}
/* FNV-1a of song t's path, as hashing the whole path string gives */
static inline uint32_t sl_track_path_fnv(int t)
{
    const struct sl_track *tr = &sl_lib.tracks[t];
    uint32_t h = 2166136261u;
    if (!tr->file) return h;
    for (const unsigned char *p = (const unsigned char *)sl_lib.dirs[tr->dir]; *p; p++) h = (h ^ *p) * 16777619u;
    for (const unsigned char *p = (const unsigned char *)tr->file; *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}
/* sl_hist_hash (sloppy_history_state.h) of song t's path, with no copy made */
static inline uint32_t sl_track_hist_hash(int t)
{
    uint32_t h = sl_track_path_fnv(t);
    return h ? h : 1;
}
/* Who a song is by: its own artist, or its album's */
static inline const char *sl_track_artist(int t)
{
    const struct sl_track *tr = &sl_lib.tracks[t];
    return tr->artist ? tr->artist : sl_lib.albums[tr->album].artist;
}
/* Library options > Ignore The, A and An off: names sort as written
 * (sloppy.c keeps it in step with the setting) */
extern bool sl_sort_as_written;
/* A name as it sorts: past a leading "The ", "A " or "An ", so The Beatles
 * file under B (a name that is only the article keeps it) */
static inline const char *sl_sort_key(const char *s)
{
    static const char *const art[] = { "the ", "a ", "an " };
    /* quotes, brackets and dots in front say nothing of where it goes */
    const char *t = s;
    for (;;)
    {
        if (t && *t && strchr(" \"'([{.-_*#", *t)) t++;
        else if (t && (unsigned char)t[0] == 0xc2 && ((unsigned char)t[1] == 0xbf || (unsigned char)t[1] == 0xa1)) t += 2;   /* ¿ ¡ */
        else break;
    }
    if (t && *t) s = t;
    for (unsigned i = 0; s && !sl_sort_as_written && i < sizeof art / sizeof art[0]; i++)
    {
        unsigned k = 0;
        while (art[i][k] && (s[k] | 32) == (art[i][k] | 32) && (art[i][k] != ' ' || s[k] == ' ')) k++;
        if (!art[i][k] && s[k]) return s + k;
    }
    return s;
}
/* Two names in list order: case and the common accents fold away (Édith
 * Piaf under E), the sort key's article and punctuation are skipped,
 * other scripts, whose bytes are all past ASCII's, come after Z, and a
 * run of digits counts as its number, so Track 2 comes before Track 10
 * (with the number equal, fewer leading zeros first). */
static inline int sl_sort_cmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)sl_sort_key(a), *q = (const unsigned char *)sl_sort_key(b);
    while (*p && *q)
    {
        if (*p >= '0' && *p <= '9' && *q >= '0' && *q <= '9')
        {
            const unsigned char *p0 = p, *q0 = q;
            while (*p == '0') p++;
            while (*q == '0') q++;
            const unsigned char *ps = p, *qs = q;
            while (*p >= '0' && *p <= '9') p++;
            while (*q >= '0' && *q <= '9') q++;
            if (p - ps != q - qs) return (int)(p - ps) - (int)(q - qs);   /* more digits: a bigger number */
            int c = memcmp(ps, qs, (size_t)(p - ps));
            if (c) return c;
            if (ps - p0 != qs - q0) return (int)(ps - p0) - (int)(qs - q0);
            continue;
        }
        unsigned char fa[4], fb[4];
        int la, lb;
        p += sl_q_fold_char(p, fa, &la);
        q += sl_q_fold_char(q, fb, &lb);
        int c = memcmp(fa, fb, (size_t)(la < lb ? la : lb));
        if (!c) c = la - lb;
        if (c) return c;
    }
    return (*p != 0) - (*q != 0);
}
enum { SL_LIB_READY, SL_LIB_LOADING, SL_LIB_NO_DB, SL_LIB_FAILED };
int  sl_lib_begin(void);          /* LOADING (library.bin or the database, in steps), NO_DB or FAILED */
int  sl_lib_step(int budget);     /* library.bin: a few ms of it; the database: up to budget tracks */
bool sl_lib_scanning(void);       /* the database is being read (long: the banner shows it), not library.bin */
void sl_lib_free(void);
int  sl_lib_find_path(const char *path);   /* track index or -1 */
int  sl_lib_find_path_at(const char *path, long ms);   /* the same, the cue sheet's track playing at ms */
extern bool sl_lib_keep_apart;      /* set before sl_lib_begin: untagged compilations not joined */
bool sl_lib_db_key(int32_t key[6]);  /* the tag database's commit key (database_idx.tcd's header) */
void sl_lib_cache_drop(void);         /* the next sl_lib_begin reads the database, not library.bin */

/* album art thumbnails (sloppy_art.c) */
/* The update's name: the Settings hero ("HiBy R1 · <name>") and its
 * footer ("GABAGOOLOS · <NAME> UPDATE", capitals made at run time). The
 * next rename is this one line. */
#define SL_UPDATE_NAME "Game on then"

#define SL_ART_GRID 219
#define SL_ART_PAGE 170
#define SL_ART_ROW  58
#define SL_ART_SMALL 140          /* the Zune theme's three-across album grid; made from the 219 */
/* ------------------------------------------------------------ tokens */
/* The shapes and timings the pages share. A rounded thing inside another
 * keeps the outer radius less its inset, so the corners stay concentric. */
#define SL_R_SM    10            /* thumbnails, small controls */
#define SL_R_MD    14            /* rows, keys, buttons in a bar */
#define SL_R_LG    20            /* cards, tiles, panels, anything that floats */
#define SL_R_PILL  28            /* 56 px buttons and pills */
#define SL_R_XS    6             /* badges and chips; a miniature's cards */
#define SL_R_MINI  3             /* a miniature's small parts (Settings' pictures of a screen) */
#define SL_R_XL    26            /* sheets over everything, the control centre's glass */
#define SL_R_SCREEN 24           /* rounded screen corners (P62): the audit's corner rule keeps words and taps out of them */
#define SL_R_FULL  LV_RADIUS_CIRCLE   /* round: a circle, or a bar or pill with round ends */
#define SL_R_ICON(s_) ((s_) * 3 / 10)   /* an icon's square, rounded in proportion */
#define SL_PILL_H  56            /* a pill button's height: twice SL_R_PILL */
/* space: the page's gutter is also the gap between its cards, so a grid's
 * edges line up with the lists above and below it */
#define SL_SP_XS   4             /* label to label, row to row */
#define SL_SP_S    8             /* inside a control */
#define SL_GUTTER  14            /* page edge to card, card to card */
#define SL_INSET   20            /* card edge to its words */
#define SL_FLOAT   10            /* screen edge to a floating surface */
#define SL_TAP     44            /* the smallest thing a finger is asked to hit, each way */
/* motion: every eased change uses one of three; a press lands within a
 * frame or two (SL_T_TOUCH) so it reads as the finger, not as motion */
#define SL_T_FAST  120           /* a knob, a hint, something leaving */
#define SL_T_PAGE  220           /* a page, sheet or banner arriving; a press settling back */
#define SL_T_SLOW  320           /* grow, settle */
#define SL_T_TOUCH 40            /* press-in: under the finger at once */
#define SL_T_ENTER 480           /* a screen's tiles or title arriving, staggered: the one long entrance */
#define SL_T_LOOP  900           /* a pulse that repeats: a live dot, a busy bar */
#define SL_T_SPAN(n_, d_) (SL_T_FAST + (SL_T_PAGE - SL_T_FAST) * (n_) / (d_))   /* a move as long as its distance (n of d), never longer than a page arriving */
#define SL_PRESSED 246           /* 96 %: a pressed tile sinks a little */
/* a song row: its height, and one row to the next; the cover's lead, the
 * words' lead beside a cover or a track number, and the ⋮ at the end,
 * before which the words stop. One set for every list of songs (A10). */
#define SL_ROW_H       78
#define SL_ROW_PITCH   (SL_ROW_H + SL_SP_XS)
#define SL_ROW_ART_X   6
#define SL_ROW_TXT_X   80
#define SL_ROW_NUM_X   52
#define SL_ROW_MORE_W  48
#define SL_ROW_MORE_IN 2
#define SL_ROW_END     (SL_ROW_MORE_IN + SL_ROW_MORE_W + 10)
#define BACK_X     6             /* a page's back button: its left edge, */
#define BACK_D     56            /* and its size */
#define SL_HDR_H   66            /* a page's header, under the status bar */
/* sliders: one knob, swelling under the finger; the track is a bar with
 * round ends. Large is the control centre's. */
#define SL_TRACK_H     8
#define SL_KNOB        24
#define SL_KNOB_PR     32
#define SL_KNOB_LG     28
#define SL_KNOB_LG_PR  36
#define SL_E2_LIFT     18        /* a knob under the finger casts a wider shadow than SL_E2's */

/* symbols Montserrat lacks, drawn by sl_sym_<px> (sloppy_icons.inc) */
#define SL_SYM_EQ    "\xEE\x80\x80"   /* U+E000 three faders: the equalizer */
#define SL_SYM_XFADE "\xEE\x80\x81"   /* U+E001 one sound ramping down as another ramps up */
#define SL_SYM_GLOBE "\xEE\x80\x82"   /* U+E002 a globe: languages and places */
#include "lvgl.h"
extern void *lv_timer_slowest_cb;      /* lib/lvgl lv_timer.c: the slowest timer callback since cleared */
extern uint32_t lv_timer_slowest_ms;
void sl_art_init(void);
void sl_art_apply(lv_obj_t *cover_box, int album, int size);
bool sl_art_apply_scaled(lv_obj_t *cover_box, int album, int size);   /* false: no thumbnail yet */
/* the same, read and scaled on a thread of its own: true when it is coming (the box gets it on a later UI pass) */
bool sl_art_apply_scaled_async(lv_obj_t *cover_box, int album, int size);
void sl_art_prefer(int album);   /* make this album's art next (Cover Flow's neighbours) */
const char *sl_art_src(int album, int size);   /* "R:<path>" of a made thumbnail, or NULL */
bool sl_art_load_square(int album, int size, uint16_t *dst); /* cached art for Now Playing */
bool sl_art_step(void);      /* make one album's thumbnails; false when done */
bool sl_art_busy(void);      /* an album is with the cover thread: collect it soon */
extern unsigned sl_art_made; /* counts thumbnails made, so caches can look again */
bool sl_art_no_cover(int album); /* tried this boot: no decodable art */
int sl_art_state(int album);     /* 1 has a cover, 0 has none, -1 not looked at yet */
void sl_art_again(int album);    /* a cover was put in its folder: look again */
bool sl_art_hue(int album, int *hue, int *sat);   /* the cover's own colour, 0-359 and 0-100; false: no thumbnail */
void sl_art_reset(void);
void sl_art_invalidate_cache(void); /* a completed library refresh */
void sl_art_release(void);       /* USB mode: close every folder held open on the card */
/* the album page's Big cover: one album's art up to 480 px, made on the cover thread; poll:
 * 0 working, 1 made (*px RGB565 w x h, the caller frees it), -1 none */
void sl_art_big_start(int album, int size);
int  sl_art_big_poll(int album, uint16_t **px, int *w, int *h);
/* A picture for the Files viewer (JPEG or PNG): its bytes, taken over and
 * freed, decoded on a thread and fitted inside max_w x max_h. Then poll:
 * 0 still decoding, 1 done (*px RGB565 *w x *h, the caller frees it; *iw
 * x *ih the picture's own size), -1 it could not be read. */
enum { SL_PIC_STARTED, SL_PIC_BUSY, SL_PIC_UNREADABLE, SL_PIC_TOO_BIG };
int  sl_pic_start(unsigned char *bytes, int len, int max_w, int max_h);
int  sl_pic_poll(uint16_t **px, int *w, int *h, int *iw, int *ih);
/* A picture on the card (JPEG or PNG, a Rockbox path) made a wallpaper,
 * all on a thread: read, decoded, cut to tw x th's shape from its middle,
 * area-averaged to exactly that, dimmed if bright (white words go on it),
 * and written raw RGB565 to out (a Rockbox path). Then poll: 0 working,
 * 1 written, -1 it could not be read or written. Answers as sl_pic_start. */
int  sl_wall_start(const char *path, const char *out, int tw, int th);
/* a boot logo: the whole picture fitted inside tw x th (never enlarged),
 * undimmed, written with an 8-byte head ("SLBL", width, height); polled
 * with sl_wall_poll, as the two share one worker */
int  sl_bootlogo_start(const char *path, const char *out, int tw, int th);
int  sl_wall_poll(void);

/* breadcrumb log for field debugging: /.rockbox/sloppy/sloppy.log */
void sl_log(const char *fmt, ...);
/* where the UI loop is, for the watchdog's log line (sloppy.c) */
extern const char *volatile sl_at_where;
#define SL_AT(where) (sl_at_where = (where))
#endif
