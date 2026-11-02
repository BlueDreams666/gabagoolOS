/* GabagoolOS software updates, option A: no hosting. An update file
 * (*.gbupdate) is dropped at the card's top or sent from the phone remote;
 * it is checked (an Ed25519 signature over its manifest, the version, then
 * the SHA-256 of the zip and of every file), staged under
 * /.rockbox/update/stage, and swapped in by a journal that a power cut at
 * any step leaves rolled forward on the next start. The files it replaces
 * wait in /.rockbox/update/prev for Previous version, and a plain shell
 * script at the card's top puts them back from the boot menu (Tools > Run
 * script) if the new build never starts. Only .rockbox program files the
 * signed manifest names are written: never config.cfg, sloppy/, the
 * database, thumbnails or playlists.
 *
 * The file:  "GBUPDATE" 01 00 00 00, u32 LE manifest length, u32 LE
 *            signature length (64), the manifest (JSON), the signature,
 *            then the zip to the end. See sloppy-dev/ota/make-update.py.
 * Plain POSIX, like sloppy_net.c: the checks run on their own thread; the
 * interface reads sl_ota_status() and never waits, except for the swap
 * itself (renames only) on the way to a restart. GPLv2+. */
#ifndef SLOPPY_OTA_H
#define SLOPPY_OTA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* This build's version: what an update must be newer than. Raise it with
 * every release (make-update.py's --version is the next one). */
#define SL_VERSION "1.1"

#define SL_OTA_EXT ".gbupdate"
#define SL_OTA_DIR "/.rockbox/update"          /* under the card's root */
#define SL_OTA_UNDO "GabagoolOS - undo update.sh"   /* at the card's top while a swap can be undone */
#define SL_OTA_HEALTHY_MS 20000u               /* a new build up this long is kept */

enum {
    SL_OTA_NONE,         /* nothing found (or not looked yet) */
    SL_OTA_LOOKING,      /* reading the card's top */
    SL_OTA_FOUND,        /* a newer, signed update: version, size, notes */
    SL_OTA_REJECTED,     /* why says what; nothing on the card changed */
    SL_OTA_WORKING,      /* checking and staging: phase, done of total */
    SL_OTA_READY,        /* staged and checked: restart to finish */
    SL_OTA_RECEIVING,    /* coming in from the phone remote */
};
enum {
    SL_OTA_OK,
    SL_OTA_E_NOKEY,      /* this build has no release key: it installs nothing */
    SL_OTA_E_NOCRYPTO,   /* the player's OpenSSL can't check Ed25519 */
    SL_OTA_E_NOTOURS,    /* not a GabagoolOS update, or the signature is wrong */
    SL_OTA_E_OLDER,      /* not newer than this build */
    SL_OTA_E_MINFROM,    /* needs min_from first */
    SL_OTA_E_DAMAGED,    /* a hash, a size or the zip is wrong */
    SL_OTA_E_SPACE,      /* need bytes more free */
    SL_OTA_E_USB,        /* USB mode stopped it */
    SL_OTA_E_CARD,       /* a write failed */
    SL_OTA_E_PATH,       /* the manifest names a file it may not write */
    SL_OTA_E_STOPPED,    /* cancelled */
};
enum { SL_OTA_PH_ZIP = 1, SL_OTA_PH_COMPARE, SL_OTA_PH_UNPACK };

struct sl_ota_status {
    unsigned seq;                /* moves with every change */
    int state, why;
    bool key, crypto;            /* a release key in this build; Ed25519 there */
    char version[16];            /* the update's */
    char min_from[16], build_date[16];
    char file[96];               /* its name at the card's top, "" when from the remote */
    uint64_t size;               /* the update file */
    uint64_t need;               /* E_SPACE: how much more */
    int files, changed;          /* in the manifest; differing from what is on the card */
    bool boot_menu;              /* needs_boot_menu: the page says it is not part of this */
    int phase;                   /* SL_OTA_PH_* while WORKING */
    uint64_t done, total;        /* bytes */
    uint64_t rx, rx_total;       /* RECEIVING */
    char notes[1200];            /* What's new, in the language asked for (English when it has none) */
    /* what is installed */
    bool prev;                   /* Previous version can put prev_version back */
    char prev_version[16];
    bool trial;                  /* this start is the first on a new version, not kept yet */
    char updated_to[16];         /* set once a swap is kept: the banner's version */
    bool restart;                /* the start found a swap half done: restart to finish */
};

/* At start, before anything else of ours reads the card: a journal left by
 * a swap is finished (rolled forward, back only when a staged file is
 * gone) and the undo script removed when nothing waits to be undone. */
void sl_ota_boot(void);
/* the interface: sl_ota_lang picks the notes' language ("en", "he", ...) */
void sl_ota_lang(const char *code);
void sl_ota_scan(void);                 /* look at the card's top again (quiet: on the worker) */
bool sl_ota_install(void);              /* check and stage what was found */
void sl_ota_cancel(void);
void sl_ota_status(struct sl_ota_status *s);
void sl_ota_hold(bool hold);            /* USB mode: a check stops, its staged half removed; an upload ends */
bool sl_ota_busy(void);                 /* the worker is running */
void sl_ota_healthy(void);              /* SL_OTA_HEALTHY_MS up on a trial: kept, the undo script goes */
void sl_ota_seen(void);                 /* the banner was shown: updated_to clears */
/* The swap, on the way to a restart: staged files in, theirs to prev, the
 * player's program last. 0 when done (restart now); else SL_OTA_E_*. */
int  sl_ota_swap(void);
/* Previous version: prev back over what the last update put in. */
int  sl_ota_revert(void);
/* the phone remote's upload, from the server thread: begin answers 0 or
 * an HTTP status (403 no key, 409 busy, 503 USB mode, 507 card full) */
int  sl_ota_upload_begin(uint64_t len);
bool sl_ota_upload_write(const void *b, size_t n);   /* false: stop (USB mode, write failed) */
void sl_ota_upload_end(bool complete);  /* complete: checked like a file at the top */

#if defined(SIMULATOR) || defined(SLOPPY_NET_TEST)
/* the tests: a release key and the library to load, the version running,
 * free space, and a power cut after n journal steps (0: none) */
extern unsigned char sl_ota_test_key[32];
extern bool sl_ota_test_key_set;
extern const char *sl_ota_crypto_lib;
extern char sl_ota_running[16];
extern long long sl_ota_free_fake;
extern int sl_ota_cut_after;
extern int sl_ota_steps;                /* journal steps the last swap or recovery took */
extern int sl_ota_slow_ms;              /* a pause per piece checked, so USB mode can land mid-way */
extern bool sl_ota_test_frozen;         /* the audit's screens: the status stays as forced */
void sl_ota_test_force(const struct sl_ota_status *s);
size_t sl_ota_test_static_bytes(void);
void sl_ota_test_reset(void);           /* the worker stopped, the state and OpenSSL forgotten (not the card) */
#endif
#endif
