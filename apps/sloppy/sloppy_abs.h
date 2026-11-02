/* GabagoolOS and an Audiobookshelf server: its books and podcasts, brought
 * to the card to play as our own audiobooks, and where you are in each
 * kept the same on both sides. Plain POSIX, like sloppy_subsonic.h (whose
 * unit, sloppy_libjob.c, compiles it: sloppy_absjob.inc). The interface
 * asks and never waits: every request, download and picture is the
 * worker's, one thread of its own, a request at a time with timeouts.
 *
 * Audiobookshelf's REST API (2.x): POST /login (or an API token), then a
 * Bearer token on every request; /api/libraries, their items, an item
 * with its chapters and audio files, /api/me/items-in-progress, covers,
 * the files themselves, and PATCH /api/me/progress/<item>[/<episode>].
 *
 * On the card (paths as Rockbox names them, under sl_net_card_root()):
 *   <audiobook folder>/<author>/<title>/<nn file>   a book, metadata.json and cover.jpg beside
 *   <audiobook folder>/<podcast>/<episode>/<file>   an episode, a book of its own
 *   /.rockbox/sloppy/abs/server.tsv                 the server, its token and password (0600, never logged or shown)
 *   /.rockbox/sloppy/abs/books.tsv                  the books on the card: which item, where, what the server last said
 *   /.rockbox/sloppy/abs/queue.tsv                  files still to come down
 *   /.rockbox/sloppy/abs/progress.tsv               places not yet told to the server (the newest of each)
 *   /.rockbox/sloppy/abs/ch/<key>.txt               a book's chapters, for the place sync
 *   /.rockbox/sloppy/abs/art/<key>-<px>.bin         covers, LVGL RGB565 squares */
#ifndef SLOPPY_ABS_H
#define SLOPPY_ABS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_ABS_STATE     "/.rockbox/sloppy/abs"
#define SL_ABS_LIST_MAX  1500             /* items in one answer */
#define SL_ABS_PAGE_N    40               /* a library's items a page */
#define SL_ABS_ART_BIG   219              /* a book's page, the grid (SL_ART_GRID) */
#define SL_ABS_ART_ROW   58               /* rows (SL_ART_ROW) */
#define SL_ABS_BOOKS     256              /* books on the card */
#define SL_ABS_CH_MAX    600              /* chapters of a book */
#define SL_ABS_BIG_MB    300              /* more than this: a word first */
#define SL_ABS_UA "GabagoolOS/1.1 ( https://github.com/BlueDreams666/gabagoolOS )"
/* the audiobook folder, as Rockbox names it (the setting's; the tests' sandbox moves it) */
extern char sl_abs_folder[64];

enum { SL_ABS_OK, SL_ABS_E_NET, SL_ABS_E_AUTH, SL_ABS_E_NOTABS, SL_ABS_E_SERVER, SL_ABS_E_NOTFOUND, SL_ABS_E_NOMEM,
       SL_ABS_E_OFFLINE, SL_ABS_E_HOLD, SL_ABS_E_NOSERVER, SL_ABS_E_FULL, SL_ABS_E_ADDRESS, SL_ABS_E_HTTPS, SL_ABS_E_ELSEWHERE,
       SL_ABS_E_NOFILES };

struct sl_abs_server {
    char name[64], url[192], user[64], version[16];
    bool api_token;                       /* signed in with a pasted API token, no password kept */
};

enum { SL_ABS_LIBRARY = 1, SL_ABS_BOOK, SL_ABS_PODCAST, SL_ABS_EPISODE, SL_ABS_CHAPTER, SL_ABS_FILE, SL_ABS_AUTHOR, SL_ABS_SERIES };
struct sl_abs_item {
    uint8_t kind, finished, local, podcast;   /* local: on the card; podcast: a podcast library or show */
    uint16_t count, permille;                 /* count: files, chapters, episodes or books; permille: how far */
    uint32_t dur_ms, cur_ms, start_ms;        /* its length, where it is, a chapter's start */
    long long size, updated;                  /* bytes; when the place last moved (ms, the server's clock) */
    /* offsets into the list's pool ("" is 0) */
    uint32_t id, title, author, narrator, series, desc, url, ext, episode, lib;
};
struct sl_abs_list {
    unsigned req;
    int what, err;
    bool offline;                         /* from the card: the server could not be reached */
    int total, page;                      /* a paged list: all there are, this page */
    struct sl_abs_item head;              /* an item's own (its chapters, files or episodes in it) */
    int n, cap;
    struct sl_abs_item *it;
    char *pool;
    size_t pool_n, pool_cap;
    char why[96];                         /* the server's own words, for SL_ABS_E_SERVER */
};
static inline const char *sl_abs_s(const struct sl_abs_list *l, uint32_t off) { return l && l->pool ? l->pool + off : ""; }

/* the requests: an answer comes as a list, taken once */
enum { SL_ABS_Q_LIBRARIES = 1, SL_ABS_Q_ITEMS, SL_ABS_Q_ITEM, SL_ABS_Q_CONTINUE, SL_ABS_Q_AUTHORS, SL_ABS_Q_SERIES, SL_ABS_Q_LOCAL };
/* arg: a library (ITEMS, AUTHORS, SERIES) or an item (ITEM); filter: "authors.<id>" or "series.<id>" (ITEMS);
 * page: from 0. 0 when the worker is full. */
unsigned sl_abs_ask(int what, const char *arg, const char *filter, int page);
bool sl_abs_take(unsigned req, struct sl_abs_list *out);    /* false until it has come */
void sl_abs_forget_req(unsigned req);
void sl_abs_list_free(struct sl_abs_list *l);

enum { SL_ABS_JOB_NONE, SL_ABS_JOB_SIGNIN, SL_ABS_JOB_LIST, SL_ABS_JOB_ART, SL_ABS_JOB_DOWNLOAD, SL_ABS_JOB_SYNC, SL_ABS_JOB_PULL,
       SL_ABS_JOB_REMOVE };
struct sl_abs_status {
    int job, jobs;
    unsigned seq;                         /* moves whenever anything below does */
    unsigned list_seq, server_seq;
    /* a sign-in */
    unsigned test_seq;
    int test_err;
    bool test_saved;
    char test_version[16], test_why[96];
    /* the card */
    unsigned books_seq;                   /* a book came onto the card or left it */
    unsigned whole_seq;                   /* a book came whole, or a whole one left */
    unsigned art_seq;
    int queued;                           /* files waiting, not counting the one coming */
    char dl_item[48], dl_episode[48], dl_title[96];
    int dl_file, dl_files;                /* the item's file coming, of how many */
    long long dl_got, dl_total;           /* the item's bytes so far, of all */
    unsigned dl_seq;                      /* a download ended: dl_err how */
    int dl_err;
    /* the place */
    int pending;                          /* places waiting to be told */
    unsigned sync_seq;                    /* one was told (or refused) */
    unsigned pick_seq;                    /* the server had a newer place: sl_abs_take_pick */
    bool online, hold;
};
void sl_abs_online(bool up);              /* Wi-Fi connected: what waits is told */
void sl_abs_hold(bool hold);              /* USB mode: a download stops at once, its part kept; nothing written */
void sl_abs_status(struct sl_abs_status *s);
bool sl_abs_busy(void);
void sl_abs_poll(void);

/* the server (one) */
bool sl_abs_server(struct sl_abs_server *out);              /* false: none (out may be NULL) */
bool sl_abs_remove_server(void);          /* its token and password gone; the books stay on the card */
/* try an address and a user with a password, or "" user and an API token (NULL or "" secret: the saved one);
 * save: kept when it works. The answer: status test_seq. */
bool sl_abs_signin(const char *url, const char *user, const char *secret, bool save);
bool sl_abs_url_fix(const char *in, char *out, size_t n);   /* "abs.local:13378" -> "http://abs.local:13378" */

/* the books on the card */
struct sl_abs_book {
    char item[48], episode[48], title[128], author[96];
    char dir[256];                        /* its folder, as Rockbox names it */
    uint32_t dur_ms;
    long long size, seen;                 /* bytes; the server's lastUpdate we last knew (ms) */
    bool whole;                           /* every file is there */
    int files;
};
int  sl_abs_books(struct sl_abs_book *out, int max);
bool sl_abs_book(const char *item, const char *episode, struct sl_abs_book *out);
bool sl_abs_book_at(const char *path, struct sl_abs_book *out);   /* the book a file (Rockbox path) is in */
bool sl_abs_download(const char *item, const char *episode);       /* its files queued; status books_seq when whole */
bool sl_abs_remove(const char *item, const char *episode);         /* off the card (a download coming stops) */
int  sl_abs_chapters(const char *item, const char *episode, uint32_t *start_ms, int max);
bool sl_abs_card_free(long long *bytes);  /* room left on the card */

/* the place, both ways: ours goes on pause, chapter, a few minutes, finished and Wi-Fi back;
 * the server's newer one comes as a pick (where, from which device when it says) */
bool sl_abs_push(const char *item, const char *episode, uint32_t cur_ms, uint32_t dur_ms, bool finished);
bool sl_abs_pull(const char *item, const char *episode);
struct sl_abs_pick {
    char item[48], episode[48], device[64];
    uint32_t cur_ms, dur_ms;
    bool finished;
    long long updated;                    /* ms, Unix, as the server's clock had it */
};
bool sl_abs_take_pick(struct sl_abs_pick *out);
void sl_abs_art_name(char *out, size_t n, const char *item, int px);
bool sl_abs_art_want(const char *item);
bool sl_abs_art_have(const char *item);

#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
extern unsigned sl_abs_idle_ms;
extern unsigned sl_abs_retry_ms;          /* after a failed tell (60 s) */
void sl_abs_test_forget(void);            /* memory cleared, the files read again */
size_t sl_abs_test_static_bytes(void);
long long sl_abs_test_skew(void);         /* the server's clock less ours, seconds */
#endif
#endif
