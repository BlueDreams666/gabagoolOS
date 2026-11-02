/* GabagoolOS Skip Graveyard: songs you keep skipping get buried. No LVGL or
 * Rockbox. GPLv2+.
 *
 * A skip is moving on to a different song after hearing at least a second
 * of this one, but not enough for it to count as a play, and not at its end.
 * Three skips dig a grave. Every real play halves the count, so a song you
 * start enjoying again climbs back out on its own. A haunted song stays
 * buried whatever its count, and the player skips it for you.
 *
 * The whole table is one small file, /.rockbox/sloppy/graves.dat. */
#ifndef SLOPPY_GRAVE_STATE_H
#define SLOPPY_GRAVE_STATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "sloppy_history_state.h"

#define SL_GRAVE_MAGIC 0x31475253u    /* "SRG1" */
#define SL_GRAVE_MAX   256
#define SL_GRAVE_BURY  3              /* skips that dig a grave */

struct sl_grave {
    uint32_t hash;         /* sl_hist_hash of the path */
    uint16_t skips;
    uint8_t haunted, pad;
};

struct sl_graves {
    uint32_t magic;
    uint16_t n, pad;
    struct sl_grave g[SL_GRAVE_MAX];
    uint32_t checksum;
};

static inline uint32_t sl_grave_checksum(const struct sl_graves *s)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(struct sl_graves, checksum); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static inline void sl_graves_init(struct sl_graves *s)
{
    memset(s, 0, sizeof *s);
    s->magic = SL_GRAVE_MAGIC;
    s->checksum = sl_grave_checksum(s);
}

static inline bool sl_graves_valid(const struct sl_graves *s)
{
    if (s->magic != SL_GRAVE_MAGIC || s->n > SL_GRAVE_MAX || s->checksum != sl_grave_checksum(s))
        return false;
    for (int i = 0; i < s->n; i++)
        if (!s->g[i].hash || s->g[i].haunted > 1) return false;
    return true;
}

static inline int sl_grave_find(const struct sl_graves *s, uint32_t hash)
{
    for (int i = 0; i < s->n; i++) if (s->g[i].hash == hash) return i;
    return -1;
}

static inline bool sl_grave_buried(const struct sl_grave *g)
{
    return g->haunted || g->skips >= SL_GRAVE_BURY;
}

static inline void sl_grave_remove(struct sl_graves *s, int i)
{
    s->g[i] = s->g[--s->n];
    s->checksum = sl_grave_checksum(s);
}

/* The tracker is about to move to (hash, elapsed): was the song it had a
 * skip? Call before sl_hist_step, which forgets it. */
static inline bool sl_hist_is_skip(const struct sl_hist_tracker *t, uint32_t hash)
{
    return t->hash && hash && hash != t->hash &&
           t->listened >= 1000 && !sl_hist_counts(t->listened, t->len) &&
           (!t->len || t->last_elapsed + 5000 < t->len);
}

/* One more skip. A full table gives up its least-skipped unhaunted song. */
static inline void sl_grave_skip(struct sl_graves *s, uint32_t hash)
{
    if (!hash) return;
    int i = sl_grave_find(s, hash);
    if (i < 0)
    {
        if (s->n == SL_GRAVE_MAX)
        {
            int low = -1;
            for (int k = 0; k < s->n; k++)
                if (!s->g[k].haunted && (low < 0 || s->g[k].skips < s->g[low].skips)) low = k;
            if (low < 0) return;                  /* all haunted: nothing to evict */
            sl_grave_remove(s, low);
        }
        i = s->n++;
        memset(&s->g[i], 0, sizeof s->g[i]);
        s->g[i].hash = hash;
    }
    if (s->g[i].skips < 0xffff) s->g[i].skips++;
    s->checksum = sl_grave_checksum(s);
}

/* A real play: the song earns half its skips back. */
static inline bool sl_grave_played(struct sl_graves *s, uint32_t hash)
{
    int i = sl_grave_find(s, hash);
    if (i < 0) return false;
    s->g[i].skips /= 2;
    if (!s->g[i].skips && !s->g[i].haunted) sl_grave_remove(s, i);
    else s->checksum = sl_grave_checksum(s);
    return true;
}

/* Bring it back: the grave is filled in and its skips forgiven. */
static inline bool sl_grave_resurrect(struct sl_graves *s, uint32_t hash)
{
    int i = sl_grave_find(s, hash);
    if (i < 0) return false;
    sl_grave_remove(s, i);
    return true;
}

static inline void sl_grave_haunt(struct sl_graves *s, uint32_t hash, bool on)
{
    int i = sl_grave_find(s, hash);
    if (i < 0) return;
    s->g[i].haunted = on;
    if (s->g[i].skips < SL_GRAVE_BURY) s->g[i].skips = SL_GRAVE_BURY;
    s->checksum = sl_grave_checksum(s);
}

static inline bool sl_grave_haunted(const struct sl_graves *s, uint32_t hash)
{
    int i = hash ? sl_grave_find(s, hash) : -1;
    return i >= 0 && s->g[i].haunted;
}
#endif
