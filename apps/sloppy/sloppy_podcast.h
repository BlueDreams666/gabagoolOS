/* GabagoolOS podcasts: the shows followed, their episodes, what has been
 * heard and how far, and the downloads. Plain POSIX, like sloppy_libjob.c
 * (whose unit compiles it, sloppy_podjob.inc): the interface calls these
 * and never waits on the network. Feeds, the directory and the shows'
 * pictures are fetched on one worker thread of its own, a request at a
 * time with timeouts; a download is a wget of its own the worker watches,
 * continued where it stopped (wget -c into a ".part" file), so a job (a
 * search, a refresh) runs while an episode comes down.
 *
 * On the card (paths as Rockbox names them, under sl_net_card_root()):
 *   /Podcasts/<show>/<date> <title>.<ext>     the downloaded episodes
 *   /.rockbox/sloppy/podcasts/shows.tsv       the shows followed
 *   /.rockbox/sloppy/podcasts/<id>.eps        each show's episodes, heard or not, downloaded or not
 *   /.rockbox/sloppy/podcasts/progress.tsv    where each episode being listened to is (Continue listening)
 *   /.rockbox/sloppy/podcasts/queue.tsv       downloads still to come
 *   /.rockbox/sloppy/podcasts/art/<id>-<px>.bin   the shows' pictures, LVGL RGB565 squares
 * Nothing secret is kept: feeds and the directory need no account. */
#ifndef SLOPPY_PODCAST_H
#define SLOPPY_PODCAST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_POD_STATE   "/.rockbox/sloppy/podcasts"
#define SL_POD_MAX_SHOWS 200
#define SL_POD_MAX_EPS 300                /* a show's newest; older downloaded ones stay too */
#define SL_POD_MAX_FOUND 25               /* directory answers */
#define SL_POD_PROGRESS 40                /* episodes remembered part-heard */
#define SL_POD_ART_BIG 219                /* the grid's squares (SL_ART_GRID) */
#define SL_POD_ART_ROW 58                 /* rows (SL_ART_ROW) */
#define SL_POD_UA "GabagoolOS/1.1 ( https://github.com/BlueDreams666/gabagoolOS )"
/* the folder the episodes go in, as Rockbox names it ("/Podcasts"; the tests' sandbox moves it) */
extern char sl_pod_folder[64];

enum { SL_POD_HEARD = 1 };
enum { SL_POD_DL_NONE, SL_POD_DL_QUEUED, SL_POD_DL_ACTIVE, SL_POD_DL_DONE };

struct sl_pod_show {
    uint32_t id;                          /* of its feed's address */
    uint32_t added, refreshed, newest;    /* UNIX times; newest: its latest episode */
    uint16_t speed;                       /* percent; 0 never set (100) */
    uint16_t n_eps, n_unheard, n_dl;      /* as its episode file was last written */
    bool art;                             /* its picture is on the card */
    uint32_t tint;                        /* its picture's own colour, 0xRRGGBB; 0 none yet */
    char feed[384], title[160], author[96], image[384], folder[96];
};
struct sl_pod_ep {
    uint32_t key;                         /* of its guid (or its audio's address) */
    uint32_t date, dur, bytes;            /* UNIX time, seconds, the feed's size */
    uint8_t flags;                        /* SL_POD_HEARD */
    uint8_t dl;                           /* SL_POD_DL_*, filled in by sl_pod_eps_load */
    char *title, *url, *file;             /* file: its name in the show's folder, "" not downloaded */
};
struct sl_pod_eps {                        /* one show's, a copy the interface owns */
    uint32_t show;
    int n;
    struct sl_pod_ep *ep;
    char *pool;
};
struct sl_pod_found {                     /* a directory answer */
    char title[160], author[96], feed[384], art[384], genre[48];
    int count;                            /* episodes, as the directory counts them */
};
struct sl_pod_progress {                  /* an episode part-heard */
    uint32_t show, key, pos_ms, dur_ms, when;
    char path[300], title[160];
};

/* what the worker is doing, for the pages */
enum { SL_POD_JOB_NONE, SL_POD_JOB_REFRESH, SL_POD_JOB_FOLLOW, SL_POD_JOB_SEARCH, SL_POD_JOB_PREVIEW,
       SL_POD_JOB_OPML, SL_POD_JOB_DELETE, SL_POD_JOB_ART };
struct sl_pod_status {
    int job;                              /* running now */
    int jobs;                             /* waiting */
    unsigned seq;                         /* moves whenever anything below changes */
    unsigned shows_seq;                   /* the list of shows changed */
    unsigned eps_seq;                     /* an episode file was written: eps_show's */
    uint32_t eps_show;
    unsigned found_seq;                   /* a search ended: sl_pod_found_take */
    int found_n;                          /* -1 the directory did not answer */
    unsigned preview_seq;                 /* a feed looked at before following ended */
    int preview_ok;                       /* 1 read, 0 not a podcast feed, -1 no answer */
    unsigned done_seq;                    /* a refresh, a follow or an import ended */
    int done_job, done_ok, done_count;    /* what, how, how many (shows imported, episodes new) */
    char done_title[160];                 /* the show it was about */
    /* downloads */
    int queued;                           /* waiting, not counting the one coming down */
    uint32_t dl_show, dl_key;             /* coming down now; 0 none */
    long long dl_got, dl_total;           /* bytes so far, of all (0 not known) */
    unsigned dl_seq;                      /* a download ended (dl_ok how) */
    bool dl_ok;
    bool online, hold;
    char why[72];                         /* the last problem, in fixed words */
};

/* the worker: started on first use, ends when it has nothing to do */
void sl_pod_online(bool up);              /* Wi-Fi connected */
void sl_pod_hold(bool hold);              /* USB mode: a download stops at once, its part kept; nothing written */
void sl_pod_status(struct sl_pod_status *s);
bool sl_pod_busy(void);                   /* its thread is running */
void sl_pod_poll(void);                   /* the interface's tick: starts downloads that were waiting */

/* the shows: a copy, under the module's lock (a small file read the first time) */
int  sl_pod_shows(struct sl_pod_show *out, int max);
struct sl_pod_show *sl_pod_shows_copy(int *n);   /* all of them, newest first, in a block the caller frees */
int  sl_pod_show_count(void);
bool sl_pod_show(uint32_t id, struct sl_pod_show *out);
uint32_t sl_pod_feed_id(const char *feed);
bool sl_pod_following(const char *feed);
/* follow by a feed's address (title, author and picture from the directory when known: "" or NULL) */
bool sl_pod_follow(const char *feed, const char *title, const char *author, const char *art);
bool sl_pod_unfollow(uint32_t id, bool delete_files);
bool sl_pod_set_speed(uint32_t id, int percent);
bool sl_pod_refresh(uint32_t id);         /* 0: every show */
bool sl_pod_search(const char *term);
bool sl_pod_preview(const char *feed);    /* read a feed before following it */
int  sl_pod_found_take(struct sl_pod_found *out, int max);
/* the previewed feed: its show (title, author, picture) and episodes */
bool sl_pod_preview_take(struct sl_pod_show *show, struct sl_pod_eps *eps);
void sl_pod_preview_forget(void);         /* the page that showed it has gone */

/* a show's episodes, newest first: a copy the caller frees (a file read) */
bool sl_pod_eps_load(uint32_t show, struct sl_pod_eps *out);
void sl_pod_eps_free(struct sl_pod_eps *e);
/* heard or not, several at once (a file write) */
bool sl_pod_mark(uint32_t show, const uint32_t *keys, int n, bool heard);
/* downloads: queued in order; the ones on the card already are skipped */
int  sl_pod_download(uint32_t show, const uint32_t *keys, int n);
bool sl_pod_cancel(uint32_t show, uint32_t key);     /* a queued or running one; its part deleted */
bool sl_pod_delete(uint32_t show, const uint32_t *keys, int n);   /* the files, on the worker */
/* where an episode's file is, as Rockbox names it ("" none) */
bool sl_pod_ep_path(uint32_t show, const struct sl_pod_ep *ep, char *out, size_t n);
bool sl_pod_is_episode(const char *path);            /* in the podcasts folder */
/* a show's picture, px square (SL_POD_ART_*), as Rockbox names it; there when sl_pod_show's art is */
void sl_pod_art_name(char *out, size_t n, uint32_t id, int px);
/* a Rockbox path of an episode back to its show and key (a file name search in the show's file) */
bool sl_pod_which(const char *path, uint32_t *show, uint32_t *key, char *title, size_t tn);   /* title: the feed's for it */

/* listening: where an episode is, and the list of ones part-heard, latest first */
void sl_pod_progress_set(uint32_t show, uint32_t key, uint32_t pos_ms, uint32_t dur_ms, const char *path, const char *title);
bool sl_pod_progress_get(uint32_t show, uint32_t key, struct sl_pod_progress *out);
int  sl_pod_progress_list(struct sl_pod_progress *out, int max);
void sl_pod_progress_flush(void);         /* written now (pause, USB mode, power off) */
void sl_pod_finished(uint32_t show, uint32_t key);   /* heard to the end: marked heard, its place forgotten */

/* OPML: every .opml at the card's top and in the podcasts folder; an import follows each feed once */
int  sl_pod_opml_list(char names[][96], int max);
bool sl_pod_opml_import(const char *path);           /* a Rockbox path */
bool sl_pod_opml_export(char *path_out, size_t n);   /* written to <folder>/Podcasts.opml (a file write) */

/* pure helpers, for the tests */
uint32_t sl_pod_rfc822(const char *s);    /* a feed's date, or ISO 8601; 0 when unreadable */
uint32_t sl_pod_duration(const char *s);  /* "1:02:03", "62:03", "3723" seconds */
void sl_pod_file_name(char *out, size_t n, uint32_t date, const char *title, const char *url);
void sl_pod_clean_name(char *out, size_t n, const char *s);   /* fit for a FAT folder or file name */
const char *sl_pod_clean_ext(const char *e, const char *fallback);   /* a server's extension: 1-8 letters or digits, else fallback */
struct sl_pod_feed_cb {
    void (*show)(void *ctx, const char *title, const char *author, const char *image);
    void (*item)(void *ctx, const char *title, const char *guid, const char *url, const char *date, const char *dur, long long bytes);
    void *ctx;
};
/* feed a parser bytes as they come; end once */
struct sl_pod_parser;
struct sl_pod_parser *sl_pod_parse_begin(const struct sl_pod_feed_cb *cb);
void sl_pod_parse_feed(struct sl_pod_parser *p, const char *data, size_t n);
bool sl_pod_parse_end(struct sl_pod_parser *p);      /* true when it was an RSS or Atom feed */
/* the decoder sloppy_art.c already links (stb_image): RGB of a JPEG or PNG, NULL when it
 * cannot or memory is short; freed with sl_art_rgb_free */
unsigned char *sl_art_rgb_load(const unsigned char *bytes, int len, int *w, int *h);
void sl_art_rgb_free(unsigned char *rgb);
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
extern unsigned sl_pod_idle_ms;           /* how long the worker waits with nothing to do before it ends */
void sl_pod_test_forget(void);           /* memory cleared, the files read again */
int  sl_pod_test_parse(const char *xml, size_t slice, struct sl_pod_show *show, struct sl_pod_eps *eps);
int  sl_pod_test_search(const char *json, struct sl_pod_found *out, int max);
void sl_pod_test_art_smaller(char *url);
uint32_t sl_pod_test_key(const char *guid);
size_t sl_pod_test_static_bytes(void);   /* the worker's own state, always there */
#endif
#endif
