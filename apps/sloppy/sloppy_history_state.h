/* GabagoolOS listening history: which songs were heard, when and for how
 * long. No LVGL or Rockbox. GPLv2+.
 *
 * Rockbox only keeps play counts when "gather runtime data" is on, and its
 * "last played" is a counter, not a date. GabagoolOS keeps its own log instead:
 * one 12-byte record per play in /.rockbox/sloppy/plays.dat. A play counts
 * after 30 s of listening, or half of a short song. Seeking does not add
 * time, and a song that repeats counts again. */
#ifndef SLOPPY_HISTORY_STATE_H
#define SLOPPY_HISTORY_STATE_H
#include <stdbool.h>
#include <stdint.h>

#define SL_HIST_MAGIC 0x31484c53u     /* "SLH1" */
#define SL_HIST_MAX   40000           /* records kept; older ones are dropped */
#define SL_HIST_VALID_CLOCK 1577836800u   /* 2020-01-01: earlier means unset */

struct sl_play {
    uint32_t when;         /* seconds since 1970, local time */
    uint32_t path_hash;    /* sl_hist_hash of the file path */
    uint32_t ms;           /* time actually listened */
};

struct sl_hist_tracker {
    uint32_t hash, listened, len, last_elapsed, started;
};

static inline uint32_t sl_hist_hash(const char *path)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) h = (h ^ *p) * 16777619u;
    return h ? h : 1;      /* 0 means "nothing playing" */
}

static inline bool sl_hist_counts(uint32_t listened, uint32_t len)
{
    return listened >= 30000 || (len >= 6000 && listened >= len / 2);
}

/* Close the current song; true (and *out) when it counted as a play. */
static inline bool sl_hist_finish(struct sl_hist_tracker *t, struct sl_play *out)
{
    bool counted = t->hash && sl_hist_counts(t->listened, t->len);
    if (counted) { out->when = t->started; out->path_hash = t->hash; out->ms = t->listened; }
    t->hash = t->listened = t->len = t->last_elapsed = 0;
    return counted;
}

/* True when this step starts a new play (a new song, or the same one
 * starting over): only then does sl_hist_step need the clock. */
static inline bool sl_hist_starts(const struct sl_hist_tracker *t, uint32_t hash, uint32_t elapsed)
{
    return hash != t->hash ||
           (hash && elapsed < 3000 && t->last_elapsed > elapsed + 5000);
}

/* Call a few times a second. hash is 0 when nothing plays (or for sounds
 * that are not music, like quiz clips); playing is false while paused.
 * Returns true with *out filled when a finished play should be logged. */
static inline bool sl_hist_step(struct sl_hist_tracker *t, uint32_t hash, bool playing,
                                uint32_t elapsed, uint32_t len, uint32_t now, struct sl_play *out)
{
    bool counted = false;
    /* a new song, or the same one starting over (repeat one, skip back) */
    if (sl_hist_starts(t, hash, elapsed))
    {
        counted = sl_hist_finish(t, out);
        if (hash) { t->hash = hash; t->len = len; t->last_elapsed = elapsed; t->started = now; }
        return counted;
    }
    if (!hash) return false;
    if (len) t->len = len;
    if (playing && elapsed > t->last_elapsed && elapsed - t->last_elapsed <= 2000)
        t->listened += elapsed - t->last_elapsed;    /* a bigger jump is a seek */
    t->last_elapsed = elapsed;
    return false;
}

/* Days since 1970-01-01 to a calendar date (Howard Hinnant's algorithm). */
static inline void sl_hist_date(uint32_t when, int *year, int *month, int *day, int *hour)
{
    int32_t z = (int32_t)(when / 86400u) + 719468;
    int32_t era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    if (year) *year = y + (m <= 2);
    if (month) *month = (int)m;
    if (day) *day = (int)d;
    if (hour) *hour = (int)(when % 86400u / 3600u);
}
#endif
