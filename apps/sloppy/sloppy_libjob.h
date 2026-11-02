/* GabagoolOS library jobs over Wi-Fi: lyrics for every song (LRCLIB) and
 * the covers albums lack (MusicBrainz and the Cover Art Archive). One job
 * at a time, on its own thread; the interface hands it songs or albums a
 * few at a time (sl_libjob_want / sl_libjob_give) and reads how it goes.
 * Nothing here blocks: start, stop and the rest only set what the thread
 * reads. What is on the card already is skipped, and what the web did not
 * have is remembered, so a job stopped (or the player turned off) carries
 * on where it was. Plain POSIX, like sloppy_net.c. */
#ifndef SLOPPY_LIBJOB_H
#define SLOPPY_LIBJOB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SL_LIBJOB_LYRICS, SL_LIBJOB_COVERS, SL_LIBJOB_KINDS };
enum { SL_LIBJOB_IDLE, SL_LIBJOB_RUNNING, SL_LIBJOB_WAITING, SL_LIBJOB_DONE, SL_LIBJOB_STOPPED, SL_LIBJOB_FAILED };

/* the User-Agent MusicBrainz asks for: who we are and how to reach us */
#define SL_LIBJOB_UA "GabagoolOS/1.1 ( https://github.com/BlueDreams666/gabagoolOS )"
#define SL_LIBJOB_LRCLIB_MS 1000u   /* LRCLIB: a request a second at most */
#define SL_LIBJOB_MB_MS 1100u       /* MusicBrainz: one a second, and a little over */
extern unsigned sl_libjob_lrclib_ms, sl_libjob_mb_ms;   /* the paces above (the tests may shorten them) */

struct sl_libjob_item {
    int index;                  /* the song's or album's, for the interface */
    char path[300];             /* lyrics: the song, as Rockbox names it; covers: a song of the album */
    char artist[128], title[128], album[128];
    unsigned secs;              /* lyrics: the song's length */
    bool had;                   /* covers: the album has one already (counted, nothing asked) */
};
struct sl_libjob_status {
    int kind, state;            /* SL_LIBJOB_* */
    int fed, total;             /* handed over so far, of all */
    int found, had, missing, failed;   /* fetched, there already, not on the web, did not come */
    char current[96];           /* what it is looking for now */
    char why[72];               /* WAITING or FAILED: what for, in fixed words */
    unsigned seq;               /* moves with each one done */
    int last_found;             /* covers: the album index fetched last, -1 none */
    unsigned found_seq;         /* moves with each cover fetched */
};

bool sl_libjob_start(int kind, int total, int fed);   /* fed: where a resumed one was */
void sl_libjob_stop(void);              /* stops after the request in flight */
bool sl_libjob_want(void);              /* room for another item */
void sl_libjob_give(const struct sl_libjob_item *it);
void sl_libjob_fed_all(void);           /* no more items: done when these are */
void sl_libjob_online(bool up);         /* Wi-Fi: it waits without */
void sl_libjob_hold(bool hold);         /* USB mode: nothing written, the job waits */
void sl_libjob_status(struct sl_libjob_status *s);
bool sl_libjob_busy(void);              /* its thread is still running */
int sl_libjob_saved(int kind);          /* where the last one stopped, 0 from the start (a small file read) */

/* pure helpers, for the tests */
bool sl_libjob_lrc_path(const char *song, char *out, size_t n, const char *ext);   /* "x.flac" -> "x<ext>" */
bool sl_libjob_json_str(const char *json, const char *key, char *out, size_t cap);  /* first "key":"..." */

/* Audiobook covers: a book's picture (src, as Rockbox names it: a picture
 * file when size is 0, else size bytes at off in its audio) made into two
 * LVGL squares under SL_BOOK_ART_DIR on a thread, one book at a time; one
 * that cannot be read leaves "<key>.none" (sl_book_art_name with px 0) */
#define SL_BOOK_ART_DIR "/.rockbox/sloppy/books/art"
#define SL_BOOK_ART_BIG 219         /* the Library's covers (SL_ART_GRID), and the book being heard, scaled */
#define SL_BOOK_ART_ROW 58          /* rows (SL_ART_ROW) */
enum { SL_BOOK_ART_UNSYNC = 1, SL_BOOK_ART_BASE64 = 2 };   /* the picture as the tags keep it */
void sl_book_art_name(char *out, size_t n, uint32_t key, int px);
bool sl_book_art_start(uint32_t key, const char *src, uint32_t off, uint32_t size, int flags);   /* false: busy */
int  sl_book_art_poll(uint32_t *key);   /* 1 made, -1 could not be, 0 working, -2 idle */
int  sl_book_art_state(uint32_t key);  /* 1 its squares are made, -1 it could not be, 0 not yet */
uint32_t sl_book_art_tint(uint32_t key);   /* its mean colour (0xRRGGBB), 0 when not made */
#endif
