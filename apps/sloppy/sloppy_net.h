/* GabagoolOS networking: Wi-Fi, the clock, file transfer, AirPlay and DLNA.
 * Plain POSIX, no Rockbox headers: every call here that blocks runs on a
 * raw pthread, and the Rockbox side only uses the non-blocking ones. */
#ifndef SLOPPY_NET_H
#define SLOPPY_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_WIFI_MAX 24

struct sl_wifi_status {
    bool on;                 /* the radio is up */
    bool connected;          /* associated and has an address */
    char ssid[33];
    char ip[16];
};

struct sl_wifi_ap {
    char ssid[33];
    int signal;              /* dBm */
    bool secure;
    bool saved;
};

/* Wi-Fi (blocking) */
bool sl_net_wifi_power(bool on);
void sl_net_wifi_status(struct sl_wifi_status *s);
int  sl_net_wifi_scan(struct sl_wifi_ap *aps, int max);
const char *sl_net_wifi_last_error(void);
const char *sl_net_wifi_detail(void);      /* kernel detail for the last failure, or "" */
bool sl_net_wifi_radio_missing(void);      /* no wlan0: detached at boot (never blocks) */
bool sl_net_wifi_connect(const char *ssid, const char *psk);
/* the same, for a network typed in by hand: hidden ones answer only a
 * probe that names them (wpa_supplicant's scan_ssid) */
bool sl_net_wifi_connect_ex(const char *ssid, const char *psk, bool hidden);
bool sl_net_wifi_forget(const char *ssid);

/* Which wireless chip this R1 carries. Newer units (SoC X1600EN 0x4a00
 * and X1600HN 0x4900) have an AIC8800D40 in place of the Broadcom chip.
 * The boot menu built on HiBy 2.0 carries HiBy's AIC driver and scripts,
 * and Wi-Fi and Bluetooth use them; without them (a 1.6-based boot menu)
 * or when the driver failed to load, both stay off and say why.
 * Never blocks: one small sysfs read, kept. -1 when the kernel does not say. */
int  sl_net_soc_id(void);
const char *sl_net_soc_name(int id);       /* "X1600E", "X1600EN", "X1600HN" or "unknown" */
bool sl_net_wireless_aic(void);            /* an AIC8800 unit, driven or not */
int  sl_net_aic_state(void);               /* 1 HiBy 2.0's AIC driver is ready, 0 not on this system, -1 failed to load */
bool sl_net_wireless_unsupported(void);    /* an AIC8800 unit this system cannot drive */
void sl_net_wireless_probe(char *out, size_t n);   /* for sloppy.log: the id, the chip, the SDIO drivers */
int  sl_net_soc_parse(const char *s);       /* the file's text: "0x4a00", "4a00" or 18944 */
extern int sl_net_soc_fake;                /* the harness's stand-in id; -1 reads the kernel */
extern int sl_net_aic_fake;                /* the harness's stand-in sl_net_aic_state(); -2 looks */

/* The card's root as the C library sees it ("/" to Rockbox) */
const char *sl_net_card_root(void);

/* Set the clock from the internet; true when it was set (blocking) */
bool sl_net_ntp(void);

/* A web address's text into out (blocking): its length, or -1 when it
 * did not come; longer answers are cut at cap - 1 */
int sl_net_get(const char *url, char *out, size_t cap);

/* Lyrics for one song from LRCLIB, as .lrc text in out (blocking); false
 * when it has none or the network did not answer */

/* Music transfer: the player's own web page on port 4399 (blocking) */
bool sl_net_share_start(void);
void sl_net_share_stop(void);

/* AirPlay receiver. Start/stop block; the rest never does. The stream is
 * 44.1 kHz 16-bit stereo. */
bool sl_net_airplay_start(const char *name);
void sl_net_airplay_stop(void);
size_t sl_net_airplay_read(int16_t *dst, size_t frames);
uint32_t sl_net_airplay_idle_ms(void);   /* since the last audio arrived */

/* DLNA renderer. Commands are for the Rockbox thread to carry out; the
 * Rockbox thread reports back what is playing. */
enum sl_dlna_cmd_type { SL_DLNA_NONE, SL_DLNA_LOAD, SL_DLNA_PLAY, SL_DLNA_PAUSE,
                        SL_DLNA_STOP, SL_DLNA_SEEK, SL_DLNA_VOLUME };
struct sl_dlna_cmd {
    int type;
    long value;              /* SEEK: ms, VOLUME: percent, LOAD: 1 to start playing */
    char path[128];          /* LOAD: the downloaded file, as Rockbox names it */
    char title[96];
};
struct sl_dlna_state {
    int playing;             /* 0 stopped, 1 playing, 2 paused */
    long elapsed_ms, length_ms;
    int volume;              /* percent */
};
bool sl_net_dlna_start(const char *name);
void sl_net_dlna_stop(void);
bool sl_net_dlna_poll(struct sl_dlna_cmd *cmd);
void sl_net_dlna_report(const struct sl_dlna_state *st);
bool sl_net_dlna_busy(void);          /* downloading what a phone sent */

/* Scrobbling: ListenBrainz and the Audioscrobbler log. The Rockbox side
 * hands over listens and settings and never waits: one worker thread does
 * every file write and every request. None of these block, except
 * sl_net_scrobble_flush (up to ms) and sl_net_scrobble_hold (a file write
 * in progress). */
#include "sloppy_scrobble_state.h"
enum { SL_SCROB_UNTESTED, SL_SCROB_TESTING, SL_SCROB_GOOD, SL_SCROB_BAD, SL_SCROB_NO_ANSWER };
struct sl_scrobble_status {
    int queued;              /* listens waiting on the card for ListenBrainz */
    int sent;                /* listens it accepted since start-up */
    uint32_t last_sent;      /* UNIX time of the last accepted request; 0 none yet */
    bool has_token, token_bad;
    int test;                /* SL_SCROB_*: the last token check */
    char user[48];           /* the ListenBrainz name the token belongs to */
    char error[72];          /* the last problem in fixed words; "" none */
    unsigned test_seq;       /* moves when a token check ends */
    unsigned fail_seq;       /* moves on a problem worth telling the user */
    /* Last.fm */
    int lfm;                 /* SL_LFM_OUT, SL_LFM_IN or SL_LFM_LOST (Last.fm ended the session) */
    int lfm_queued, lfm_sent;
    uint32_t lfm_last_sent;
    bool lfm_keyless;        /* this build carries no API key */
    char lfm_user[48];       /* the account signed in */
    char lfm_error[72];      /* the last problem, in fixed words */
    int lfm_auth;            /* SL_LFM_AUTH_*: a sign-in under way */
    char lfm_url[160];       /* SL_LFM_AUTH_WAIT: the address the phone opens */
    unsigned lfm_auth_seq, lfm_fail_seq;
};
enum { SL_LFM_OUT, SL_LFM_IN, SL_LFM_LOST };
enum { SL_LFM_AUTH_IDLE, SL_LFM_AUTH_TOKEN, SL_LFM_AUTH_WAIT, SL_LFM_AUTH_CHECKING, SL_LFM_AUTH_OK, SL_LFM_AUTH_FAILED };
void sl_net_scrobble_start(void);
void sl_net_scrobble_config(bool listenbrainz, bool log, bool lastfm);
/* Last.fm: a sign-in with a phone (a QR code of the address in lfm_url) or
 * with a name and password, a sign-in given up, signing out */
void sl_net_lastfm_signin_phone(void);
void sl_net_lastfm_signin_password(const char *user, const char *pass);
void sl_net_lastfm_signin_cancel(void);
void sl_net_lastfm_signout(void);
extern const char *sl_lastfm_key, *sl_lastfm_secret;   /* "" in a build without the key file */
extern unsigned sl_lastfm_poll_ms;
void sl_net_scrobble_token(const char *token);     /* "" forgets it */
void sl_net_scrobble_online(bool up);              /* Wi-Fi connected */
void sl_net_scrobble_hold(bool hold);              /* USB mode: hands off the card (a DLNA download too) */
bool sl_net_scrobble_add(const struct sl_listen *l);
void sl_net_scrobble_now(const struct sl_listen *l);
void sl_net_scrobble_test(void);
void sl_net_scrobble_status(struct sl_scrobble_status *s);
bool sl_net_scrobble_message(char *out, size_t n); /* a line for the log */
void sl_net_scrobble_poll(void);                   /* the simulator's worker runs here */
bool sl_net_scrobble_flush(int ms);                /* listens handed over are on the card */
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
/* the tests answer requests: returns the HTTP status, -1 for no answer */
extern int (*sl_net_scrobble_fake)(const char *url, const char *auth, const char *body, char *out, size_t cap);
#endif

/* A GET for the library jobs: text into out, or bytes into the file at
 * path; the HTTP status, -1 with no answer. Blocking: a worker thread's. */
int sl_net_http_get(const char *url, const char *ua, char *out, size_t cap, const char *path, int timeout_ms);
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
extern int (*sl_net_http_fake)(const char *url, char *out, size_t cap, const char *path);   /* the tests' internet */
#endif
/* A GET read as it comes: sink gets the body a slice at a time (false
 * from it ends the request, as do *stop and timeout_ms of silence). The
 * HTTP status, -1 with no answer. Blocking: a worker thread's. */
int sl_net_http_stream(const char *url, const char *ua, int timeout_ms, bool (*sink)(void *ctx, const char *data, size_t n),
                       void *ctx, volatile bool *stop);
/* A download that runs by itself (wget -c: a part on the card is carried
 * on). Start and poll never block; poll says 0 while it comes, then the
 * HTTP status (200/206 whole, -1 failed). Stop kills it, the part kept. */
struct sl_net_dl { int pid, fd, status; size_t n; char report[1024], url[512], path[400]; };
bool sl_net_dl_start(struct sl_net_dl *d, const char *url, const char *ua, const char *path);
int  sl_net_dl_poll(struct sl_net_dl *d);
void sl_net_dl_stop(struct sl_net_dl *d);
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
extern size_t sl_net_http_fake_len;      /* a fake's answer of bytes: their count (0: text, strlen) */
/* the tests' downloads: called each poll with the part's size so far; 0 more to come
 * (it adds a slice to path), else the status it ends with */
extern int (*sl_net_dl_fake)(const char *url, const char *path, long long have);
#endif

/* One request of any method (GET, POST, PATCH...), over our own socket:
 * HTTP, or HTTPS through the system's OpenSSL (the certificate not
 * checked, as wget's). headers: more lines, each "Name: value\r\n";
 * body: JSON (or ctype) sent with its length. The answer's body goes to
 * sink a slice at a time (false: enough), chunked or not; redirects are
 * followed to the same host only (310 when one leads elsewhere). The HTTP
 * status, -1 with no answer (https_failed: TLS could not start). date:
 * the answer's Date, Unix seconds (0 none). Blocking, timeout_ms of
 * silence at most, *stop heard within a second: a worker thread's. */
struct sl_net_req {
    const char *method, *url, *ua, *headers, *body, *ctype;
    int timeout_ms;
    volatile bool *stop;
    bool (*sink)(void *ctx, const char *data, size_t n);
    void *ctx;
    long long date;
    bool https_failed;
};
int sl_net_http_req(struct sl_net_req *r);
#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
extern int (*sl_net_req_fake)(struct sl_net_req *r);   /* the tests' own answers (NULL: the socket) */
#endif

/* where downloads land, relative to the Rockbox root */
#define SL_CAST_DIR "/.rockbox/sloppy"

/* Internet radio: an MP3 stream read by wget, decoded here (libmad, the
 * copy in mad/) into 16-bit stereo for the mixer. Start and stop do not
 * block; the state says how it is going. */
enum { SL_RADIO_OFF, SL_RADIO_CONNECTING, SL_RADIO_PLAYING, SL_RADIO_FAILED, SL_RADIO_ENDED };
bool sl_net_radio_start(const char *url);
void sl_net_radio_stop(void);
int  sl_net_radio_state(void);
unsigned sl_net_radio_rate(void);          /* Hz of what is decoded; 0 before the first frame */
size_t sl_net_radio_buffered(void);        /* frames waiting */
size_t sl_net_radio_read(int16_t *dst, size_t frames);
void sl_net_radio_why(char *out, size_t cap);
void sl_net_radio_title(char *out, size_t cap);   /* the song on air, as the station names it; "" when it does not */   /* after a failure: what came and what wget said */

/* Remote: a page any phone's browser on the same Wi-Fi opens at this port
 * to see what plays, control it and pick songs; no app to install. The
 * server thread hands each /api request to the interface (take), which
 * answers it with JSON (answer) on its own thread. */
#define SL_REMOTE_PORT 4400
bool sl_net_remote_start(void);
void sl_net_remote_stop(void);
bool sl_net_remote_on(void);
bool sl_net_remote_take(char *path, size_t pn, char *query, size_t qn);
void sl_net_remote_answer(const char *json);
/* The cover playing now, as the page fetches it (/cover?k=key): bytes
 * off..off+size of a Rockbox path (size < 0: to the end), a JPEG or PNG,
 * maybe unsynchronised as an ID3 tag keeps it; key 0 none. The server
 * sends them as they are, so the phone scales them, not the player. */
#define SL_REMOTE_COVER_JPEG   1
#define SL_REMOTE_COVER_PNG    2
#define SL_REMOTE_COVER_UNSYNC 16
#define SL_REMOTE_COVER_MAX    (6L << 20)   /* bigger is not sent */
void sl_net_remote_cover(const char *path, long off, long size, int kind, unsigned key);
/* the app icon's accent and the ink on it; the icon as a PNG (the tests) */
void sl_net_remote_tint(uint32_t accent, uint32_t ink);
size_t sl_net_remote_icon(int size, unsigned char **png);
extern unsigned sl_net_remote_served;
/* Pairing: only a phone that typed the code shown on the player may use
 * the remote. pair_code: the code to show now and its serial (a new one
 * each time a phone asks); false when none is waiting. paired: how many
 * phones may; forget: none may any more. */
bool sl_net_remote_pair_code(char *code, size_t n, unsigned *serial);
void sl_net_remote_pair_cancel(void);
int sl_net_remote_paired(void);
void sl_net_remote_forget(void);
/* the value of key in a URL query, %XX and + undone; false when missing */
bool sl_net_query_param(const char *query, const char *key, char *out, size_t n);

/* Erase GabagoolOS data: delete .rockbox/sloppy-erased0..9 (set aside at the
 * start) on a thread of its own, at the lowest priority */
bool sl_net_erase_start(void);
bool sl_net_erase_busy(void);
int  sl_net_erase_count(void);             /* files deleted so far */

#endif
