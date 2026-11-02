/* The glide decoder as it was before the keyboards batch (23 Oct 2026),
 * renamed sto_: the accuracy test measures it beside the new one. GPLv2+.
 *
 * The way glide keyboards do it (Kristensson and Zhai's SHARK2, in its
 * simplest useful form): every word in the vocabulary has an ideal path,
 * the line through the centres of its letters' keys. The finger's path and
 * each candidate's ideal path are both resampled to the same number of
 * evenly spaced points, and the word whose path lies closest, point by
 * point, wins. Two cheap checks first throw out almost every word: it has
 * to start near where the finger went down and end near where it lifted.
 * A word heard more often in the library breaks near ties.
 *
 * Letters are Unicode code points, so Hebrew glides as English does. */
#ifndef SLOPPY_SWIPE_TYPE_OLD_H
#define SLOPPY_SWIPE_TYPE_OLD_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STO_N        24          /* points each path is resampled to */
#define STO_KEYS     40
#define STO_WORD_MAX 24          /* code points */
#define STO_BEST     4

struct sto_key { uint32_t cp; float x, y; };      /* a letter and its key's centre */
struct sto_layout { struct sto_key k[STO_KEYS]; int n; float key_w; };

/* A vocabulary is one arena of NUL-ended UTF-8 words and a small index:
 * 16 bytes a word plus its text (a 12000-word library is about 280 KB). */
struct sto_word {
    uint32_t off;               /* into the arena */
    uint32_t first, last;       /* code points, for the cheap checks */
    uint16_t count;             /* how often the library uses it */
    uint8_t len;                /* code points */
};
struct sto_vocab {
    struct sto_word *w; int n, max;
    char *arena; int used, size;
    int *hash; int cap;         /* open hash for repeats while building */
};

struct sto_match { int word; float score; };

static inline int sto_key_of(const struct sto_layout *l, uint32_t cp)
{
    for (int i = 0; i < l->n; i++) if (l->k[i].cp == cp) return i;
    return -1;
}

/* One UTF-8 code point from s, or 0 at the end; *s moves past it. */
static inline uint32_t sto_utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = p[0];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (!c) return 0;
    if (n > 1)
    {
        c &= (0xff >> (n + 1));
        for (int i = 1; i < n; i++)
        {
            if ((p[i] & 0xc0) != 0x80) { *s += 1; return 0xfffd; }
            c = (c << 6) | (p[i] & 0x3f);
        }
    }
    *s += n;
    return c;
}

static inline uint32_t sto_lower(uint32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Evenly spaced points along a polyline of n points. */
static inline void sto_resample(const float *xs, const float *ys, int n, float *ox, float *oy)
{
    if (n <= 0) { for (int i = 0; i < STO_N; i++) ox[i] = oy[i] = 0; return; }
    if (n == 1) { for (int i = 0; i < STO_N; i++) { ox[i] = xs[0]; oy[i] = ys[0]; } return; }
    float total = 0;
    for (int i = 1; i < n; i++) total += hypotf(xs[i] - xs[i - 1], ys[i] - ys[i - 1]);
    if (total <= 0) { for (int i = 0; i < STO_N; i++) { ox[i] = xs[0]; oy[i] = ys[0]; } return; }
    float step = total / (STO_N - 1), acc = 0;
    int seg = 1;
    float px = xs[0], py = ys[0];
    ox[0] = px; oy[0] = py;
    for (int k = 1; k < STO_N; k++)
    {
        float want = step;
        while (seg < n)
        {
            float d = hypotf(xs[seg] - px, ys[seg] - py);
            if (acc + d >= want && d > 0)
            {
                float t = (want - acc) / d;
                px += (xs[seg] - px) * t;
                py += (ys[seg] - py) * t;
                acc = 0;
                break;
            }
            acc += d;
            px = xs[seg]; py = ys[seg];
            seg++;
        }
        ox[k] = px; oy[k] = py;
    }
    ox[STO_N - 1] = xs[n - 1]; oy[STO_N - 1] = ys[n - 1];
}

/* The ideal path of a word on this layout; false if a letter has no key. */
static inline bool sto_word_path(const struct sto_layout *l, const char *utf8, float *ox, float *oy)
{
    float xs[STO_WORD_MAX], ys[STO_WORD_MAX];
    int n = 0;
    uint32_t c;
    while ((c = sto_utf8_next(&utf8)) && n < STO_WORD_MAX)
    {
        int k = sto_key_of(l, c);
        if (k < 0) return false;
        if (n && xs[n - 1] == l->k[k].x && ys[n - 1] == l->k[k].y) continue;   /* "ll": one key */
        xs[n] = l->k[k].x; ys[n] = l->k[k].y; n++;
    }
    sto_resample(xs, ys, n, ox, oy);
    return true;
}

/* The best few words for a finger path of n points; returns how many. */
static inline int sto_decode(const struct sto_layout *l, const struct sto_vocab *v,
                            const float *px, const float *py, int n, struct sto_match *best)
{
    float gx[STO_N], gy[STO_N];
    int found = 0;
    if (n < 2 || !l->n) return 0;
    sto_resample(px, py, n, gx, gy);
    float reach = l->key_w * 1.6f, reach2 = reach * reach;
    for (int w = 0; w < v->n; w++)
    {
        const struct sto_word *wd = &v->w[w];
        if (wd->len < 2) continue;
        int k0 = sto_key_of(l, wd->first), k1 = sto_key_of(l, wd->last);
        if (k0 < 0 || k1 < 0) continue;
        float dx = l->k[k0].x - px[0], dy = l->k[k0].y - py[0];
        if (dx * dx + dy * dy > reach2) continue;              /* starts somewhere else */
        dx = l->k[k1].x - px[n - 1]; dy = l->k[k1].y - py[n - 1];
        if (dx * dx + dy * dy > reach2) continue;              /* ends somewhere else */
        float wx[STO_N], wy[STO_N];
        if (!sto_word_path(l, v->arena + wd->off, wx, wy)) continue;
        float d = 0;
        for (int i = 0; i < STO_N; i++) d += hypotf(wx[i] - gx[i], wy[i] - gy[i]);
        d /= STO_N;
        /* a little help for the words the library uses most */
        /* double log: logf needs a newer glibc than the R1 has */
        float score = d / l->key_w - 0.08f * (float)log(1.0 + wd->count);
        int at = found < STO_BEST ? found++ : STO_BEST;
        if (at == STO_BEST && score >= best[STO_BEST - 1].score) continue;
        if (at == STO_BEST) at = STO_BEST - 1;
        while (at > 0 && best[at - 1].score > score) { best[at] = best[at - 1]; at--; }
        best[at].word = w; best[at].score = score;
    }
    return found;
}

/* A path counts as a glide, not a tap, once it has gone this far. */
static inline bool sto_is_swipe(const float *px, const float *py, int n, float key_w)
{
    float len = 0;
    for (int i = 1; i < n; i++) len += hypotf(px[i] - px[i - 1], py[i] - py[i - 1]);
    return len > key_w * 1.3f;
}

/* Add the words of s (letters of the layout only) to a vocabulary,
 * counting repeats; words with a letter outside the layout, or too long,
 * are skipped. False when it is full. */
static inline bool sto_vocab_add(const struct sto_layout *l, struct sto_vocab *v, const char *s)
{
    while (s && *s)
    {
        uint32_t cp[STO_WORD_MAX];
        int len = 0;
        bool ok = true;
        uint32_t c;
        while (*s)                       /* to the next letter */
        {
            const char *before = s;
            c = sto_lower(sto_utf8_next(&s));
            if (sto_key_of(l, c) >= 0) { s = before; break; }
        }
        while (*s)
        {
            const char *before = s;
            c = sto_lower(sto_utf8_next(&s));
            if (sto_key_of(l, c) < 0)
            {
                if (c == '\'' || c == 0x2019) continue;      /* "don't": dont */
                if (c > 0x7f) ok = false;    /* an accent inside the word: all of it is out */
                s = before;
                if (c) sto_utf8_next(&s);
                break;
            }
            if (len < STO_WORD_MAX) cp[len++] = c; else ok = false;
        }
        if (!ok || len < 2) continue;
        uint32_t h = 2166136261u;
        for (int i = 0; i < len; i++) h = (h ^ cp[i]) * 16777619u;
        int slot = (int)(h % (uint32_t)v->cap);
        for (;;)
        {
            int at = v->hash[slot];
            if (at < 0)
            {
                char tmp[STO_WORD_MAX * 3 + 1], *o = tmp;
                for (int i = 0; i < len; i++)
                {
                    uint32_t q = cp[i];
                    if (q < 0x80) *o++ = (char)q;
                    else if (q < 0x800) { *o++ = (char)(0xc0 | q >> 6); *o++ = (char)(0x80 | (q & 0x3f)); }
                    else { *o++ = (char)(0xe0 | q >> 12); *o++ = (char)(0x80 | ((q >> 6) & 0x3f)); *o++ = (char)(0x80 | (q & 0x3f)); }
                }
                *o++ = '\0';
                int need = (int)(o - tmp);
                if (v->n >= v->max || v->used + need > v->size) return false;
                memcpy(v->arena + v->used, tmp, need);
                v->w[v->n] = (struct sto_word){ (uint32_t)v->used, cp[0], cp[len - 1], 1, (uint8_t)len };
                v->used += need;
                v->hash[slot] = v->n++;
                break;
            }
            const struct sto_word *e = &v->w[at];
            if (e->len == len && e->first == cp[0] && e->last == cp[len - 1])
            {
                const char *q = v->arena + e->off;
                bool same = true;
                for (int i = 0; i < len && same; i++) same = sto_utf8_next(&q) == cp[i];
                if (same) { if (v->w[at].count < 0xffff) v->w[at].count++; break; }
            }
            slot = (slot + 1) % v->cap;
        }
    }
    return true;
}
#endif
