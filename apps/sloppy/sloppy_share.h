/* GabagoolOS: music in over Wi-Fi. Two ways in, one session at a time:
 *  - the transfer page: a browser on the same Wi-Fi opens the player's
 *    address (port 4399) and drops songs or whole folders on it;
 *  - LocalSend (localsend.org, protocol v2 over plain HTTP): the player
 *    shows up in the LocalSend app on a phone or computer, asks before it
 *    takes anything, and receives on port 53317.
 * Everything lands under Music/Received on the card, never over a file
 * that is there (a second copy gets " (2)"), songs only unless other files
 * are allowed. Plain POSIX, like sloppy_net.c: the servers run on their own
 * threads with timeouts; nothing here blocks the interface, which reads
 * the state with sl_share_status() and answers a LocalSend ask with
 * sl_share_answer(). */
#ifndef SLOPPY_SHARE_H
#define SLOPPY_SHARE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_SHARE_DIR "/Music/Received"      /* under the card's root */

enum { SL_RX_IDLE, SL_RX_ASK, SL_RX_RECEIVING, SL_RX_DONE, SL_RX_ENDED };
enum { SL_RX_WEB, SL_RX_LOCALSEND };
struct sl_rx_status {
    int state;                 /* SL_RX_*: ASK waits for sl_share_answer() */
    int kind;                  /* SL_RX_WEB or SL_RX_LOCALSEND */
    unsigned seq;              /* moves when a session starts, asks or ends */
    char from[64];             /* LocalSend: the sender's name */
    int files;                 /* in the session (ASK: offered and takeable) */
    int songs;                 /* of them, songs */
    int refused;               /* offered but not taken (not songs) */
    int done, failed;          /* files landed, files that did not */
    uint64_t bytes, total;     /* received, expected */
    char current[96];          /* the file coming in now */
    char why[72];              /* ENDED: what stopped it, in fixed words */
    bool web_on, ls_on;        /* the servers that are up */
};

/* Start and stop: quick, but they join threads, so call them off the
 * interface thread (the Wi-Fi worker). alias: how LocalSend names us. */
bool sl_share_web_start(void);
void sl_share_web_stop(void);
bool sl_share_ls_start(const char *alias);
void sl_share_ls_stop(void);
void sl_share_ls_announce(void);            /* say we are here again (never blocks) */

void sl_share_status(struct sl_rx_status *s);
void sl_share_answer(bool accept);          /* to the ASK */
void sl_share_seen(void);                   /* DONE or ENDED read: back to IDLE */
void sl_share_allow_any(bool any);          /* take files that are not songs too */
void sl_share_hold(bool hold);              /* USB mode: what is coming in stops and is removed */

/* a file this name could be a song or what goes with one (cover, cue, lyrics) */
bool sl_share_is_music(const char *name);
/* rel made safe as a path under the receive folder: no "..", no
 * characters the card's file system refuses; "" when nothing is left */
void sl_share_clean_path(char *out, size_t n, const char *rel);

/* the ports; the tests set 0 for any free one and read back what was bound */
extern int sl_share_web_port, sl_share_ls_port;
extern int sl_share_io_s;                  /* seconds a silent client is waited for */
int sl_share_bound_port(bool localsend);
#endif
