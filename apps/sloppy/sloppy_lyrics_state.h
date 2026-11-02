/* GabagoolOS synced lyrics: .lrc parsing, no LVGL or Rockbox. GPLv2+.
 *
 * An .lrc file holds lines such as "[01:02.50]Some words". A line may carry
 * several timestamps ("[00:10][01:40]Chorus"), "[offset:+250]" shifts every
 * time, and tags like "[ar:Artist]" are metadata. Enhanced LRC word times
 * ("<01:02.60>") are removed. A file without any timestamp is plain lyrics
 * and is shown unsynced. Text is converted to UTF-8 first: UTF-16 with a
 * BOM, and legacy single-byte files as Windows-1255 when they look Hebrew
 * or as Latin-1 otherwise. */
#ifndef SLOPPY_LYRICS_STATE_H
#define SLOPPY_LYRICS_STATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SL_LRC_MAX_LINES 400
#define SL_LRC_MAX_BYTES (96 * 1024)

struct sl_lrc_line {
    uint32_t ms;           /* start time; 0 for unsynced lyrics */
    const char *text;      /* NUL-terminated, inside the parsed buffer */
};

struct sl_lrc {
    struct sl_lrc_line lines[SL_LRC_MAX_LINES];
    int n;
    bool synced;
};

static inline bool sl_lrc_utf8_valid(const unsigned char *s, size_t len)
{
    for (size_t i = 0; i < len; )
    {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 :
                (c & 0xf8) == 0xf0 ? 3 : -1;
        if (n < 0 || (n == 1 && c < 0xc2) || i + (size_t)n >= len) return false;
        for (int k = 1; k <= n; k++)
            if ((s[i + k] & 0xc0) != 0x80) return false;
        i += (size_t)n + 1;
    }
    return true;
}

static inline size_t sl_lrc_put(unsigned char *out, size_t at, size_t cap, uint32_t cp)
{
    unsigned char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xc0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3f); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xe0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3f);
                             b[2] = 0x80 | (cp & 0x3f); n = 3; }
    else { b[0] = 0xf0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3f);
           b[2] = 0x80 | ((cp >> 6) & 0x3f); b[3] = 0x80 | (cp & 0x3f); n = 4; }
    if (at + n >= cap) return at;          /* keep room for the final NUL */
    memcpy(out + at, b, n);
    return at + n;
}

/* Windows-1255 (Hebrew) letters and the few punctuation marks rips use. */
static inline uint32_t sl_lrc_cp1255(unsigned char c)
{
    if (c >= 0xe0 && c <= 0xfa) return 0x5d0 + (c - 0xe0);
    if (c >= 0xc0 && c <= 0xd2) return 0x5b0 + (c - 0xc0);   /* points */
    switch (c)
    {
        case 0x96: return 0x2013;
        case 0x97: return 0x2014;
        case 0x91: return 0x2018;
        case 0x92: return 0x2019;
        case 0x93: return 0x201c;
        case 0x94: return 0x201d;
        case 0x85: return 0x2026;
    }
    return c;   /* Latin-1 for the rest */
}

/* Convert raw file bytes to UTF-8 in out (cap bytes). Returns the length. */
static inline size_t sl_lrc_to_utf8(const unsigned char *in, size_t len, unsigned char *out, size_t cap)
{
    size_t at = 0;
    if (!cap) return 0;
    if (len >= 2 && ((in[0] == 0xff && in[1] == 0xfe) || (in[0] == 0xfe && in[1] == 0xff)))
    {
        bool le = in[0] == 0xff;
        for (size_t i = 2; i + 1 < len; i += 2)
        {
            uint32_t u = le ? (uint32_t)(in[i] | in[i + 1] << 8) : (uint32_t)(in[i] << 8 | in[i + 1]);
            if (u >= 0xd800 && u < 0xdc00 && i + 3 < len)
            {
                uint32_t lo = le ? (uint32_t)(in[i + 2] | in[i + 3] << 8) : (uint32_t)(in[i + 2] << 8 | in[i + 3]);
                if (lo >= 0xdc00 && lo < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00); i += 2; }
                else u = 0xfffd;
            }
            else if (u >= 0xd800 && u < 0xe000) u = 0xfffd;
            at = sl_lrc_put(out, at, cap, u);
        }
        out[at] = 0;
        return at;
    }
    if (len >= 3 && in[0] == 0xef && in[1] == 0xbb && in[2] == 0xbf) { in += 3; len -= 3; }
    if (sl_lrc_utf8_valid(in, len))
    {
        size_t n = len < cap - 1 ? len : cap - 1;
        while (n && n < len && (in[n] & 0xc0) == 0x80) n--;   /* whole characters only */
        memcpy(out, in, n);
        out[n] = 0;
        return n;
    }
    /* Those bytes are Hebrew letters in Windows-1255 but accented Latin
     * letters (é, à, ñ) in Latin-1; French or Spanish lyrics are mostly
     * plain ASCII letters with a few of them. */
    int hebrew = 0, ascii = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (in[i] >= 0xe0 && in[i] <= 0xfa) hebrew++;
        else if ((in[i] | 0x20) >= 'a' && (in[i] | 0x20) <= 'z') ascii++;
    }
    bool cp1255 = hebrew >= 3 && hebrew * 2 > ascii;
    for (size_t i = 0; i < len; i++)
        at = sl_lrc_put(out, at, cap, cp1255 ? sl_lrc_cp1255(in[i]) : in[i]);
    out[at] = 0;
    return at;
}

static inline bool sl_lrc_digits(const char **p, int max, int *value, int *count)
{
    int v = 0, n = 0;
    while (n < max && **p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; n++; }
    *value = v;
    if (count) *count = n;
    return n > 0;
}

/* "[mm:ss]", "[mm:ss.f]", "[mm:ss.ff]", "[mm:ss.fff]" or "[mm:ss:ff]" at p.
 * On success returns the time and moves p past the "]". */
static inline bool sl_lrc_time(const char **pp, uint32_t *ms)
{
    const char *p = *pp;
    int m, s, f = 0, fn = 0;
    if (*p++ != '[' || !sl_lrc_digits(&p, 4, &m, NULL) || *p++ != ':' ||
        !sl_lrc_digits(&p, 2, &s, NULL) || s > 59) return false;
    if (*p == '.' || *p == ':')
    {
        p++;
        if (!sl_lrc_digits(&p, 3, &f, &fn)) return false;
        while (*p >= '0' && *p <= '9') p++;          /* extra precision */
        f = fn == 1 ? f * 100 : fn == 2 ? f * 10 : f;
    }
    if (*p++ != ']') return false;
    *ms = (uint32_t)m * 60000u + (uint32_t)s * 1000u + (uint32_t)f;
    *pp = p;
    return true;
}

/* Remove enhanced-LRC word times "<mm:ss.xx>" and trim spaces, in place. */
static inline char *sl_lrc_clean(char *s)
{
    char *w = s;
    for (const char *r = s; *r; )
    {
        if (*r == '<')
        {
            const char *q = r + 1;
            int v;
            if (sl_lrc_digits(&q, 4, &v, NULL) && *q == ':')
            {
                const char *e = strchr(q, '>');
                if (e) { r = e + 1; continue; }
            }
        }
        *w++ = *r++;
    }
    *w = 0;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

static inline bool sl_lrc_meta(const char *p, int32_t *offset)
{
    /* "[xx:...]" with letters before the colon is a tag, not a time */
    if (*p != '[') return false;
    const char *q = p + 1;
    while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) q++;
    if (q == p + 1 || *q != ':' || !strchr(q, ']')) return false;
    if (q - p - 1 == 6 && !strncmp(p + 1, "offset", 6))
    {
        const char *v = q + 1;
        while (*v == ' ') v++;
        bool neg = *v == '-';
        if (*v == '-' || *v == '+') v++;
        int n;
        if (sl_lrc_digits(&v, 7, &n, NULL)) *offset = neg ? -n : n;
    }
    return true;
}

/* Parse a NUL-terminated UTF-8 buffer in place. Line texts point into buf. */
static inline void sl_lrc_parse(char *buf, struct sl_lrc *out)
{
    int32_t offset = 0;
    out->n = 0;
    out->synced = false;
    /* Pass 1: is any line timed? Plain lyrics keep every line. */
    for (const char *p = buf; *p; )
    {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        uint32_t t;
        if (sl_lrc_time(&q, &t)) { out->synced = true; break; }
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    char *p = buf;
    while (*p && out->n < SL_LRC_MAX_LINES)
    {
        char *nl = strchr(p, '\n');
        char *next = nl ? nl + 1 : p + strlen(p);
        if (nl) *nl = 0;
        size_t len = strlen(p);
        if (len && p[len - 1] == '\r') p[len - 1] = 0;
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        uint32_t times[16];
        int nt = 0;
        uint32_t t;
        while (sl_lrc_time(&q, &t))
            if (nt < 16) times[nt++] = t;
        if (!nt && sl_lrc_meta(q, &offset)) { p = next; continue; }
        char *words = sl_lrc_clean((char *)q);
        if (out->synced)
            for (int i = 0; i < nt && out->n < SL_LRC_MAX_LINES; i++)
            {
                out->lines[out->n].ms = times[i];
                out->lines[out->n].text = words;
                out->n++;
            }
        else if (*words || (out->n && *out->lines[out->n - 1].text))   /* one blank between verses */
        {
            out->lines[out->n].ms = 0;
            out->lines[out->n].text = words;
            out->n++;
        }
        p = next;
    }
    if (!out->synced)
    {
        while (out->n && !*out->lines[out->n - 1].text) out->n--;
        return;
    }
    /* A positive offset shows lyrics sooner. */
    for (int i = 0; i < out->n; i++)
    {
        int64_t v = (int64_t)out->lines[i].ms - offset;
        out->lines[i].ms = v < 0 ? 0 : (uint32_t)v;
    }
    /* stable insertion sort: repeated choruses arrive out of order */
    for (int i = 1; i < out->n; i++)
    {
        struct sl_lrc_line l = out->lines[i];
        int j = i - 1;
        while (j >= 0 && out->lines[j].ms > l.ms) { out->lines[j + 1] = out->lines[j]; j--; }
        out->lines[j + 1] = l;
    }
}

/* The line being sung at ms: the last line starting at or before it, or -1
 * before the first line. */
static inline int sl_lrc_find(const struct sl_lrc *l, uint32_t ms)
{
    if (!l->synced || !l->n || ms < l->lines[0].ms) return -1;
    int lo = 0, hi = l->n - 1;
    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;
        if (l->lines[mid].ms <= ms) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* "/Music/a/song.mp3" -> "/Music/a/song.lrc" (upper case when upper). */
static inline bool sl_lrc_path(const char *track, char *out, size_t size, bool upper)
{
    const char *slash = strrchr(track, '/'), *dot = strrchr(track, '.');
    size_t stem = dot && (!slash || dot > slash) ? (size_t)(dot - track) : strlen(track);
    if (stem + 5 > size) return false;
    memcpy(out, track, stem);
    memcpy(out + stem, upper ? ".LRC" : ".lrc", 5);
    return true;
}
#endif
