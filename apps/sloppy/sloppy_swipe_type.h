/* Swipe typing: a finger's path across the keys, read as a word. No LVGL
 * or Rockbox: tests/swipetype_test.c runs it on the desk. GPLv2+.
 *
 * The way glide keyboards do it (Kristensson and Zhai's SHARK2): every word
 * in the vocabulary has an ideal path, the line through the centres of its
 * letters' keys. The finger's path and each candidate's ideal path are both
 * resampled to the same number of evenly spaced points. A word scores on
 * four things: how far its path lies from the finger's, point by point
 * (shape and place); how close the finger came to each of its keys (a word
 * whose letters the finger never went near is out, whatever its shape); how
 * long its path is beside the finger's; and how common it is. Two cheap
 * checks first throw out almost every word: it has to start near where the
 * finger went down and end near where it lifted.
 *
 * Letters are Unicode code points, so Hebrew, Arabic and Cyrillic glide as
 * English does. A letter with no key of its own is typed on its plain
 * letter's key (é on e, ё on е, أ on ا), so "café" glides as c-a-f-e and
 * comes out with its accent. */
#ifndef SLOPPY_SWIPE_TYPE_H
#define SLOPPY_SWIPE_TYPE_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ST_N        32          /* points each path is resampled to */
#define ST_KEYS     48
#define ST_WORD_MAX 24          /* code points */
#define ST_BEST     6

struct st_key { uint32_t cp; float x, y; };      /* a letter and its key's centre */
struct st_layout { struct st_key k[ST_KEYS]; int n; float key_w, key_h; };

/* A vocabulary is one arena of NUL-ended UTF-8 words and a small index:
 * 12 bytes a word plus its text (a 12000-word library is about 230 KB).
 * Keys are the layout's, by index: a vocabulary belongs to one layout. */
struct st_word {
    uint32_t off;               /* into the arena */
    uint16_t count;             /* weight: how common the word is */
    uint8_t k0, k1;             /* its first and last letters' keys, for the cheap checks */
    uint8_t len;                /* code points */
};
struct st_vocab {
    struct st_word *w; int n, max;
    char *arena; int used, size;
    int *hash; int cap;         /* open hash for repeats while building */
};

struct st_match { int word; float score; };

/* Lower case for the scripts the keyboards type: Latin with its accents,
 * Greek and Cyrillic. Other scripts have no case. */
static inline uint32_t st_lower(uint32_t c)
{
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;
    if (c >= 0x100 && c <= 0x17f)
    {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) ? c + 1 : c;
        if (c == 0x178) return 0xff;
        if (c == 0x138 || c == 0x149 || c == 0x17f) return c;
        return (c & 1) ? c : c + 1;
    }
    if (c >= 0x410 && c <= 0x42f) return c + 32;
    if (c >= 0x400 && c <= 0x40f) return c + 80;
    if (c >= 0x391 && c <= 0x3a9) return c + 32;
    return c;
}
static inline uint32_t st_upper(uint32_t c)
{
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c == 0xdf || c == 0xff) return c;
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 32;
    if (c >= 0x100 && c <= 0x17f)
    {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) ? c : c - 1;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x430 && c <= 0x44f) return c - 32;
    if (c >= 0x450 && c <= 0x45f) return c - 80;
    if (c >= 0x3b1 && c <= 0x3c9) return c - 32;
    return c;
}

/* The plain letter a lower-case letter is typed on when it has no key of
 * its own; 0 when it has none. */
static inline uint32_t st_fold(uint32_t c)
{
    static const char latin1[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";   /* U+00E0..U+00FF */
    static const char exta[] =                                          /* U+0100..U+017F, lower case at odd or even as the block has it */
        "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkkllllllllllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
    if (c >= 0xe0 && c <= 0xff) { char f = latin1[c - 0xe0]; return f == '/' ? 0 : (uint32_t)f; }
    if (c == 0xdf) return 's';                       /* ß */
    if (c >= 0x100 && c <= 0x17f) return (uint32_t)exta[c - 0x100];
    if (c == 0x451) return 0x435;                    /* ё on е */
    if (c == 0x44a) return 0x44c;                    /* ъ on ь */
    if (c == 0x623 || c == 0x625 || c == 0x622 || c == 0x671) return 0x627;   /* أ إ آ ٱ on ا */
    if (c == 0x624) return 0x648;                    /* ؤ on و */
    if (c == 0x626) return 0x64a;                    /* ئ on ي */
    return 0;
}

static inline int st_key_exact(const struct st_layout *l, uint32_t cp)
{
    for (int i = 0; i < l->n; i++) if (l->k[i].cp == cp) return i;
    return -1;
}
/* the key a letter is typed on: its own, or its plain letter's */
static inline int st_key_of(const struct st_layout *l, uint32_t cp)
{
    int k = st_key_exact(l, cp);
    if (k >= 0) return k;
    uint32_t f = st_fold(cp);
    return f ? st_key_exact(l, f) : -1;
}

/* One UTF-8 code point from s, or 0 at the end; *s moves past it. */
static inline uint32_t st_utf8_next(const char **s)
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
static inline int st_utf8_put(char *o, uint32_t q)
{
    if (q < 0x80) { o[0] = (char)q; return 1; }
    if (q < 0x800) { o[0] = (char)(0xc0 | q >> 6); o[1] = (char)(0x80 | (q & 0x3f)); return 2; }
    o[0] = (char)(0xe0 | q >> 12); o[1] = (char)(0x80 | ((q >> 6) & 0x3f)); o[2] = (char)(0x80 | (q & 0x3f));
    return 3;
}

/* Evenly spaced points along a polyline of n points; returns its length. */
static inline float st_resample(const float *xs, const float *ys, int n, float *ox, float *oy)
{
    if (n <= 0) { for (int i = 0; i < ST_N; i++) ox[i] = oy[i] = 0; return 0; }
    if (n == 1) { for (int i = 0; i < ST_N; i++) { ox[i] = xs[0]; oy[i] = ys[0]; } return 0; }
    float total = 0;
    for (int i = 1; i < n; i++) total += hypotf(xs[i] - xs[i - 1], ys[i] - ys[i - 1]);
    if (total <= 0) { for (int i = 0; i < ST_N; i++) { ox[i] = xs[0]; oy[i] = ys[0]; } return 0; }
    float step = total / (ST_N - 1), acc = 0;
    int seg = 1;
    float px = xs[0], py = ys[0];
    ox[0] = px; oy[0] = py;
    for (int k = 1; k < ST_N; k++)
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
    ox[ST_N - 1] = xs[n - 1]; oy[ST_N - 1] = ys[n - 1];
    return total;
}

/* The keys of a word, repeats merged ("ll": one key); how many, or -1 if a
 * letter has no key. */
static inline int st_word_keys(const struct st_layout *l, const char *utf8, int *keys)
{
    int n = 0;
    uint32_t c;
    while ((c = st_utf8_next(&utf8)) && n < ST_WORD_MAX)
    {
        if (c == '\'' || c == 0x5f3 || c == 0x5f4) continue;   /* "don't", ג'ז: the mark has no key */
        int k = st_key_of(l, c);
        if (k < 0) return -1;
        if (n && keys[n - 1] == k) continue;
        keys[n++] = k;
    }
    return n;
}

/* The ideal path of a word on this layout; its length, or -1 if a letter has no key. */
static inline float st_word_path(const struct st_layout *l, const char *utf8, float *ox, float *oy)
{
    int keys[ST_WORD_MAX];
    float xs[ST_WORD_MAX], ys[ST_WORD_MAX];
    int n = st_word_keys(l, utf8, keys);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) { xs[i] = l->k[keys[i]].x; ys[i] = l->k[keys[i]].y; }
    return st_resample(xs, ys, n, ox, oy);
}

/* The weights of the score, in key widths: tuned on the harness's 1,800
 * glides in 12 languages (glide_accuracy, ui_audit.inc) */
#define ST_KEY_TOL 0.25f        /* a letter's key passed this near counts as hit */
#define ST_W_KEYS  0.3f         /* each letter's key the finger missed by */
#define ST_W_LEN   0.9f         /* paths much longer or shorter than the finger's */
#define ST_W_FREQ  0.05f        /* a common word, against a rare one */

/* The best few words for a finger path of n points; returns how many. */
static inline int st_decode(const struct st_layout *l, const struct st_vocab *v,
                            const float *px, const float *py, int n, struct st_match *best)
{
    float gx[ST_N], gy[ST_N];
    int found = 0;
    if (n < 2 || !l->n || l->key_w <= 0) return 0;
    float glen = st_resample(px, py, n, gx, gy);
    float kw = l->key_w, kh = l->key_h > 0 ? l->key_h : kw;
    /* where it went down and lifted: within about a key and a half across, a key up and down */
    float rx = kw * 1.5f, ry = kh * 1.05f;
    for (int w = 0; w < v->n; w++)
    {
        const struct st_word *wd = &v->w[w];
        if (wd->len < 2) continue;
        float dx = (l->k[wd->k0].x - px[0]) / rx, dy = (l->k[wd->k0].y - py[0]) / ry;
        if (dx * dx + dy * dy > 1) continue;                   /* starts somewhere else */
        dx = (l->k[wd->k1].x - px[n - 1]) / rx; dy = (l->k[wd->k1].y - py[n - 1]) / ry;
        if (dx * dx + dy * dy > 1) continue;                   /* ends somewhere else */
        const char *word = v->arena + wd->off;
        int keys[ST_WORD_MAX];
        int nk = st_word_keys(l, word, keys);
        if (nk < 1) continue;
        float wx[ST_N], wy[ST_N];
        float xs[ST_WORD_MAX], ys[ST_WORD_MAX];
        for (int i = 0; i < nk; i++) { xs[i] = l->k[keys[i]].x; ys[i] = l->k[keys[i]].y; }
        float wlen = st_resample(xs, ys, nk, wx, wy);
        float d = 0;
        for (int i = 0; i < ST_N; i++) d += hypotf(wx[i] - gx[i], wy[i] - gy[i]);
        d /= ST_N * kw;
        /* every letter's key: how near the finger came, in order along the path */
        float miss = 0;
        int from = 0;
        for (int i = 0; i < nk; i++)
        {
            float kx = l->k[keys[i]].x, ky = l->k[keys[i]].y, m = 1e9f;
            int at = from;
            for (int j = from; j < n; j++)
            {
                float e = hypotf((px[j] - kx) / kw, (py[j] - ky) / kh);
                if (e < m) { m = e; at = j; }
            }
            from = at;
            if (m > ST_KEY_TOL) miss += m - ST_KEY_TOL;
        }
        miss /= nk;
        float lr = fabsf(glen - wlen) / (wlen + kw);
        /* double log: logf needs a newer glibc than the R1 has */
        float score = d + ST_W_KEYS * miss + ST_W_LEN * lr - ST_W_FREQ * (float)log(1.0 + wd->count);
        int at = found < ST_BEST ? found++ : ST_BEST;
        if (at == ST_BEST && score >= best[ST_BEST - 1].score) continue;
        if (at == ST_BEST) at = ST_BEST - 1;
        while (at > 0 && best[at - 1].score > score) { best[at] = best[at - 1]; at--; }
        best[at].word = w; best[at].score = score;
    }
    return found;
}

/* A path counts as a glide, not a tap, once it has gone this far. */
static inline bool st_is_swipe(const float *px, const float *py, int n, float key_w)
{
    float len = 0;
    for (int i = 1; i < n; i++) len += hypotf(px[i] - px[i - 1], py[i] - py[i - 1]);
    return len > key_w * 1.3f;
}

/* Add the words of s (letters of the layout, or letters typed on one) to a
 * vocabulary, each weighing 'weight' more; a word with a letter that has no
 * key, or too long, is skipped. False when it is full. */
static inline bool st_vocab_add_w(const struct st_layout *l, struct st_vocab *v, const char *s, int weight)
{
    while (s && *s)
    {
        uint32_t cp[ST_WORD_MAX];
        int len = 0;
        bool ok = true;
        uint32_t c;
        while (*s)                       /* to the next letter */
        {
            const char *before = s;
            c = st_lower(st_utf8_next(&s));
            if (st_key_of(l, c) >= 0) { s = before; break; }
        }
        while (*s)
        {
            const char *before = s;
            c = st_lower(st_utf8_next(&s));
            if (st_key_of(l, c) < 0)
            {
                if (c == '\'' || c == 0x2019 || c == 0x5f3 || c == 0x5f4) { if (len < ST_WORD_MAX) cp[len++] = c == 0x2019 ? '\'' : c; continue; }
                if (c == 0x640 || (c >= 0x64b && c <= 0x652) || (c >= 0x591 && c <= 0x5c7)) continue;   /* tatweel, harakat, niqqud */
                if (c > 0x7f && c != 0xa0 && c != 0x2014 && c != 0x2013 && c != 0x5be) ok = false;   /* another script inside the word */
                s = before;
                if (c) st_utf8_next(&s);
                break;
            }
            if (len < ST_WORD_MAX) cp[len++] = c; else ok = false;
        }
        while (len && (cp[len - 1] == '\'' || cp[len - 1] == 0x5f3)) len--;   /* "rock'": rock; a trailing geresh stays with gimel only */
        if (!ok || len < 2) continue;
        int k0 = st_key_of(l, cp[0]), k1 = st_key_of(l, cp[len - 1]);
        if (k0 < 0 || k1 < 0) continue;
        uint32_t h = 2166136261u;
        for (int i = 0; i < len; i++) h = (h ^ cp[i]) * 16777619u;
        int slot = (int)(h % (uint32_t)v->cap);
        for (;;)
        {
            int at = v->hash[slot];
            if (at < 0)
            {
                char tmp[ST_WORD_MAX * 3 + 1], *o = tmp;
                for (int i = 0; i < len; i++) o += st_utf8_put(o, cp[i]);
                *o++ = '\0';
                int need = (int)(o - tmp);
                if (v->n >= v->max || v->used + need > v->size) return false;
                memcpy(v->arena + v->used, tmp, need);
                v->w[v->n] = (struct st_word){ (uint32_t)v->used, (uint16_t)(weight > 0xffff ? 0xffff : weight),
                                               (uint8_t)k0, (uint8_t)k1, (uint8_t)len };
                v->used += need;
                v->hash[slot] = v->n++;
                break;
            }
            const struct st_word *e = &v->w[at];
            if (e->len == len && e->k0 == k0 && e->k1 == k1)
            {
                const char *q = v->arena + e->off;
                bool same = true;
                for (int i = 0; i < len && same; i++) same = st_utf8_next(&q) == cp[i];
                if (same) { int c2 = v->w[at].count + weight; v->w[at].count = (uint16_t)(c2 > 0xffff ? 0xffff : c2); break; }
            }
            slot = (slot + 1) % v->cap;
        }
    }
    return true;
}
static inline bool st_vocab_add(const struct st_layout *l, struct st_vocab *v, const char *s)
{
    return st_vocab_add_w(l, v, s, 1);
}

/* A dictionary: words separated by spaces, the commonest first. A word's
 * weight falls with its place, as word frequencies do (Zipf). Stops when
 * the vocabulary is full; returns how many words were read. */
static inline int st_vocab_add_ranked(const struct st_layout *l, struct st_vocab *v, const char *list, int top)
{
    int rank = 0;
    char w[ST_WORD_MAX * 4 + 1];
    while (*list)
    {
        while (*list == ' ' || *list == '\n') list++;
        if (!*list) break;
        int n = 0;
        while (*list && *list != ' ' && *list != '\n') { if (n < (int)sizeof w - 1) w[n++] = *list; list++; }
        w[n] = '\0';
        int weight = top / (rank / 8 + 1);
        if (!st_vocab_add_w(l, v, w, weight < 1 ? 1 : weight)) break;
        rank++;
    }
    return rank;
}

/* Is a lower-case letter of 'word' the same key-letter as typed 'c'? A
 * typed plain letter matches its accented forms ("cafe" finds "café"). */
static inline bool st_same_letter(uint32_t typed, uint32_t c)
{
    typed = st_lower(typed);
    return c == typed || (st_fold(c) && st_fold(c) == typed);
}
/* Up to 'max' words that start with 'prefix' (as typed: case and accents
 * loose), most common first; words equal to the prefix come last. */
static inline int st_complete(const struct st_vocab *v, const char *prefix, int *out, int max)
{
    uint32_t p[ST_WORD_MAX];
    int pn = 0;
    uint32_t c;
    while ((c = st_utf8_next(&prefix)) && pn < ST_WORD_MAX) p[pn++] = c;
    if (!pn) return 0;
    int found = 0;
    for (int w = 0; w < v->n; w++)
    {
        const struct st_word *wd = &v->w[w];
        if (wd->len < pn) continue;
        const char *q = v->arena + wd->off;
        bool ok = true;
        for (int i = 0; i < pn && ok; i++) ok = st_same_letter(p[i], st_utf8_next(&q));
        if (!ok) continue;
        int score = wd->len == pn ? -1 : wd->count;
        int at = found < max ? found++ : max;
        if (at == max && score <= (v->w[out[max - 1]].len == pn ? -1 : v->w[out[max - 1]].count)) continue;
        if (at == max) at = max - 1;
        while (at > 0)
        {
            const struct st_word *o = &v->w[out[at - 1]];
            if ((o->len == pn ? -1 : o->count) >= score) break;
            out[at] = out[at - 1]; at--;
        }
        out[at] = w;
    }
    return found;
}
#endif
