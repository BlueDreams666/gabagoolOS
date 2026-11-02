/* GabagoolOS and a music server: Subsonic, OpenSubsonic, Navidrome and the
 * rest that speak the Subsonic API. Plain POSIX, like sloppy_podcast.h
 * (whose unit, sloppy_libjob.c, compiles it: sloppy_subjob.inc). The
 * interface asks and never waits: every request, download and picture is
 * the worker's, one thread of its own, a request at a time with timeouts.
 *
 * Rockbox plays files, so a server's song plays from the card: Play
 * downloads the first (as the server sends it, or made smaller: MP3 320 or
 * Opus 128), starts it and fetches the next few while it plays. What comes
 * down stays as a cache with a size limit; the least recently played go
 * first, except albums and playlists kept offline.
 *
 * On the card (paths as Rockbox names them, under sl_net_card_root()):
 *   /Subsonic/<artist>/<album>/<nn title>.<ext>     the songs, tags as the server sent them, cover.jpg beside
 *   /.rockbox/sloppy/subsonic/servers.tsv           the servers; their passwords (0600, never logged or shown)
 *   /.rockbox/sloppy/subsonic/prefs.txt             quality and the cache's size
 *   /.rockbox/sloppy/subsonic/cache.tsv             every song on the card: which server's, when played, kept or not
 *   /.rockbox/sloppy/subsonic/pins.tsv              the albums and playlists kept offline
 *   /.rockbox/sloppy/subsonic/queue.tsv             kept songs still to come
 *   /.rockbox/sloppy/subsonic/scrobbles.tsv         plays not yet told to the server
 *   /.rockbox/sloppy/subsonic/art/<key>-<px>.bin    covers, LVGL RGB565 squares */
#ifndef SLOPPY_SUBSONIC_H
#define SLOPPY_SUBSONIC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_SUB_STATE     "/.rockbox/sloppy/subsonic"
#define SL_SUB_SERVERS   4
#define SL_SUB_LIST_MAX  1500             /* items in one answer (a playlist of more stops there) */
#define SL_SUB_PAGE_N    40               /* albums a page */
#define SL_SUB_ART_BIG   219              /* the grid's squares (SL_ART_GRID) */
#define SL_SUB_ART_ROW   58               /* rows (SL_ART_ROW) */
#define SL_SUB_AHEAD     3                /* songs fetched ahead of the one playing */
#define SL_SUB_UA "GabagoolOS/1.1 ( https://github.com/BlueDreams666/gabagoolOS )"
#define SL_SUB_API "1.16.1"
/* the songs' folder, as Rockbox names it ("/Subsonic"; the tests' sandbox moves it) */
extern char sl_sub_folder[64];

/* what went wrong, in a word the pages turn into a sentence */
enum { SL_SUB_OK, SL_SUB_E_NET, SL_SUB_E_AUTH, SL_SUB_E_NOTSUB, SL_SUB_E_TOKEN, SL_SUB_E_SERVER,
       SL_SUB_E_NOTFOUND, SL_SUB_E_NOMEM, SL_SUB_E_OFFLINE, SL_SUB_E_HOLD, SL_SUB_E_NOSERVER, SL_SUB_E_FULL,
       SL_SUB_E_ADDRESS };

struct sl_sub_server {
    uint32_t id;                          /* of its address and user */
    char name[64], url[192], user[64];
    char kind[24], version[16];           /* "navidrome", "0.53.3", as its ping said */
    bool api_key;                         /* signs in with an OpenSubsonic API key */
    bool plain;                           /* takes no tokens: the password goes (HTTPS only) */
    bool open;                            /* OpenSubsonic */
};

enum { SL_SUB_ARTIST = 1, SL_SUB_ALBUM, SL_SUB_SONG, SL_SUB_PLAYLIST };
struct sl_sub_item {
    uint8_t kind, starred, cached, pinned;    /* cached: songs on the card (albums: some of them) */
    uint16_t track, disc, year, count;        /* count: an artist's albums, an album's or playlist's songs */
    uint32_t dur, size;                       /* seconds; bytes, as the server has it */
    /* offsets into the list's pool ("" is 0) */
    uint32_t id, album_id, artist_id, cover, title, artist, album, suffix;
};
struct sl_sub_list {
    unsigned req;
    int what, err;
    bool offline;                         /* from the card: the server could not be reached */
    bool more;                            /* a paged list: there is another page */
    int offset;                           /* its first item's place */
    uint32_t server;
    struct sl_sub_item head;              /* the album's, artist's or playlist's own */
    int n, cap;
    struct sl_sub_item *it;
    char *pool;
    size_t pool_n, pool_cap;
    char why[96];                         /* the server's own words, for SL_SUB_E_SERVER */
};
static inline const char *sl_sub_s(const struct sl_sub_list *l, uint32_t off) { return l && l->pool ? l->pool + off : ""; }

/* the requests: an answer comes as a list, taken once */
enum { SL_SUB_Q_ARTISTS = 1, SL_SUB_Q_ARTIST, SL_SUB_Q_ALBUMS, SL_SUB_Q_ALBUM, SL_SUB_Q_PLAYLISTS, SL_SUB_Q_PLAYLIST,
       SL_SUB_Q_SEARCH, SL_SUB_Q_STARRED, SL_SUB_Q_OFFLINE };
/* arg: an id (artist, album, playlist), the albums' order ("newest", "recent", "random", "starred",
 * "alphabeticalByName") or the words searched for; offset: a page of albums. 0 when the worker is full. */
unsigned sl_sub_ask(int what, const char *arg, int offset);
bool sl_sub_take(unsigned req, struct sl_sub_list *out);    /* false until it has come */
void sl_sub_forget_req(unsigned req);                       /* a page that asked has gone */
void sl_sub_list_free(struct sl_sub_list *l);
bool sl_sub_list_dup(const struct sl_sub_list *l, struct sl_sub_list *out);

/* the worker */
enum { SL_SUB_JOB_NONE, SL_SUB_JOB_SIGNIN, SL_SUB_JOB_LIST, SL_SUB_JOB_ART, SL_SUB_JOB_STAR, SL_SUB_JOB_SCROBBLE,
       SL_SUB_JOB_FORGET, SL_SUB_JOB_PIN };
struct sl_sub_status {
    int job, jobs;
    unsigned seq;                         /* moves whenever anything below does */
    unsigned list_seq;                    /* an answer is ready: sl_sub_take */
    unsigned servers_seq;                 /* the servers changed */
    /* a sign-in */
    unsigned test_seq;
    int test_err;
    bool test_api_key, test_open, test_plain;
    char test_kind[24], test_version[16], test_why[96];
    uint32_t test_saved;                  /* the server it saved; 0 tested only */
    /* the card */
    unsigned cache_seq;                   /* a song came onto the card or left it */
    unsigned art_seq;                     /* a cover was made */
    int queued;                           /* waiting, not counting the one coming down */
    uint32_t dl_server;
    char dl_id[48], dl_title[96];
    long long dl_got, dl_total;           /* bytes so far, of all (0 not known) */
    unsigned dl_seq;                      /* a download ended: dl_err how */
    int dl_err;
    unsigned star_seq;                    /* a star or unstar ended */
    int star_err;
    int scrobbles;                        /* waiting to be told */
    bool online, hold;
};
void sl_sub_online(bool up);              /* Wi-Fi connected */
void sl_sub_hold(bool hold);              /* USB mode: a download stops at once, its part kept; nothing written */
void sl_sub_status(struct sl_sub_status *s);
bool sl_sub_busy(void);                   /* its thread is running */
void sl_sub_poll(void);                   /* the interface's tick */

/* servers: the one in use is active (0 none) */
int  sl_sub_servers(struct sl_sub_server *out, int max);
uint32_t sl_sub_active(void);
bool sl_sub_server(uint32_t id, struct sl_sub_server *out);   /* 0: the active one */
bool sl_sub_use(uint32_t id);
bool sl_sub_remove(uint32_t id);          /* its password gone; its songs stay until the cache wants room */
/* try an address, a user and a secret (password or API key; NULL or "": the saved one of id);
 * save: kept (and made active) when it works. The answer: status test_seq. */
bool sl_sub_signin(const char *url, const char *user, const char *secret, uint32_t id, bool save);
bool sl_sub_url_fix(const char *in, char *out, size_t n);    /* "host:4533" -> "http://host:4533" */
bool sl_sub_url_https(uint32_t id);

/* settings */
enum { SL_SUB_Q_ORIGINAL, SL_SUB_Q_MP3, SL_SUB_Q_OPUS };
struct sl_sub_prefs { int quality, limit_gb; };
void sl_sub_prefs(struct sl_sub_prefs *p);
bool sl_sub_prefs_set(const struct sl_sub_prefs *p);

/* the cache */
unsigned sl_sub_session_new(void);        /* a new play: what an old one was fetching is dropped */
int  sl_sub_fetch(const struct sl_sub_list *l, const int *idx, int n, unsigned session);   /* queued in that order */
bool sl_sub_cached(uint32_t server, const char *id, char *path, size_t n);   /* its Rockbox path */
bool sl_sub_which(const char *path, uint32_t *server, char *id, size_t n);
bool sl_sub_is_cached_path(const char *path);                /* under the songs' folder */
void sl_sub_touch(const char *path);      /* played: the last the cache gives up */
bool sl_sub_pin(const struct sl_sub_list *l, bool on);       /* an album or playlist (its head) and its songs */
bool sl_sub_pinned(int kind, const char *id);
bool sl_sub_forget(const char *album_id); /* an album's songs off the card (kept offline: no longer) */
bool sl_sub_clear(void);                  /* every song not kept offline */
void sl_sub_usage(long long *used, long long *limit, int *songs, int *albums);
void sl_sub_art_name(char *out, size_t n, uint32_t server, const char *cover, int px);
bool sl_sub_art_want(const char *cover);  /* made at both sizes; status art_seq */
bool sl_sub_art_have(const char *cover);

/* back to the server */
bool sl_sub_scrobble(const char *path, uint32_t when, bool submission);
bool sl_sub_star(int kind, const char *id, bool on);

/* pure helpers, for the tests */
void sl_sub_token(const char *secret, const char *salt, char out[33]);
void sl_sub_file_name(char *out, size_t n, int disc, int track, const char *title, const char *ext);
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
extern unsigned sl_sub_idle_ms;
extern unsigned sl_sub_test_limit_kb;    /* the cache's room in KB instead of the setting's GB (0: the setting) */
void sl_sub_test_forget(void);            /* memory cleared, the files read again */
size_t sl_sub_test_static_bytes(void);
int  sl_sub_test_parse(const char *json, size_t slice, int what, struct sl_sub_list *out);
#endif
#endif
