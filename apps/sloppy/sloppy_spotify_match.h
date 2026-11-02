/* Matching a Spotify playlist's songs to songs on the card. No LVGL or
 * Rockbox: tests/spotify_test.c runs it on the desk. GPLv2+.
 *
 * Only metadata is compared (the card's library has no ISRCs): the title,
 * the artist and, to choose between copies, the album and the length. It
 * leans towards "not on this player" rather than the wrong song:
 *  - remaster, deluxe, "feat." and similar notes are ignored, but a version
 *    (live, acoustic, remix, demo, instrumental, edit...) has to be on both
 *    titles, so a live take never stands in for the studio one;
 *  - the artist must match one of Spotify's artists (a leading "The" and
 *    punctuation aside); a compilation's "Various Artists" can't, so there
 *    the album title has to match instead;
 *  - lengths more than 7 s apart are different recordings. */
#ifndef SLOPPY_SPOTIFY_MATCH_H
#define SLOPPY_SPOTIFY_MATCH_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SP_FIELD 160

struct sp_song {                 /* one line of an exported playlist */
    char title[SP_FIELD], artists[SP_FIELD], album[SP_FIELD];   /* artists: "A;B" */
    long ms;                     /* 0 when unknown */
};

struct sp_local {                /* one song on the card */
    const char *title, *artist, *album;
    long ms;
    uint32_t key;                /* sp_title_key(title) */
};

static const char *const sp_version_words[] = {
    "live", "acoustic", "remix", "mix", "edit", "demo", "instrumental", "unplugged",
    "karaoke", "cover", "rework", "rerecorded", "session", "orchestral", "piano",
    "radio", "extended", "dub", "reprise", "slowed", "spedup", "sped", NULL };
static const char *const sp_version_only[] = { "version", NULL };
static const char *const sp_album_words[] = { "album", "original", NULL };
static const char *const sp_neutral_words[] = {
    "remaster", "remastered", "deluxe", "expanded", "anniversary", "bonus", "explicit",
    "feat", "ft", "featuring", "with", "mono", "stereo", NULL };

/* Lowercase ASCII letters and digits, and every byte of a non-ASCII
 * character as it is; everything else (spaces, punctuation) goes. */
static inline void sp_squash(const char *in, size_t len, char *out, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < len && in[i] && k + 1 < n; i++)
    {
        unsigned char c = (unsigned char)in[i];
        if (c == '&' && k + 4 < n) { memcpy(out + k, "and", 3); k += 3; continue; }
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) out[k++] = (char)c;
    }
    out[k] = '\0';
}

static inline bool sp_word_in(const char *seg, size_t len, const char *const *words)
{
    char low[SP_FIELD];
    size_t k = 0;
    for (size_t i = 0; i < len && k + 1 < sizeof low; i++)
    {
        unsigned char c = (unsigned char)seg[i];
        low[k++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c == '-' || c == '\'' ? ' ' : c);
    }
    low[k] = '\0';
    for (const char *const *w = words; *w; w++)
    {
        size_t wl = strlen(*w);
        for (const char *p = low; (p = strstr(p, *w)); p++)
        {
            bool start = p == low || !((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= '0' && p[-1] <= '9'));
            char e = p[wl];
            bool end = !((e >= 'a' && e <= 'z') || (e >= '0' && e <= '9'));
            if (start && end) return true;
        }
    }
    return false;
}

/* A title without its notes: the parts in brackets and after " - " are
 * dropped when they only say remaster, deluxe, feat. and the like, kept
 * (squashed in) when they name a version. */
static inline void sp_title_norm(const char *in, char *out, size_t n)
{
    char keep[SP_FIELD * 2];
    size_t k = 0, len = strlen(in);
    size_t i = 0;
    while (i < len && k + 1 < sizeof keep)
    {
        char open = in[i];
        size_t seg_end = 0, skip = 0;
        if (open == '(' || open == '[')
        {
            const char *close = strchr(in + i, open == '(' ? ')' : ']');
            seg_end = close ? (size_t)(close - in) : len;
            skip = 1;
        }
        else if (open == ' ' && in[i + 1] == '-' && in[i + 2] == ' ')
        {
            seg_end = len;               /* " - Live at ..." runs to the end */
            const char *paren = strchr(in + i + 3, '(');
            if (paren) seg_end = (size_t)(paren - in);
            skip = 3;
        }
        else if ((i == 0 || in[i - 1] == ' ') && (!strncasecmp(in + i, "feat.", 5) || !strncasecmp(in + i, "ft. ", 4)))
        {
            break;                       /* "Song feat. X": the rest is credits */
        }
        if (seg_end)
        {
            const char *seg = in + i + skip;
            size_t sl = seg_end > i + skip ? seg_end - i - skip : 0;
            bool neutral = sp_word_in(seg, sl, sp_neutral_words);
            /* "Live Version" is a version; "2011 Remastered Version" and
             * "Album Version" are the song as it was */
            bool version = sp_word_in(seg, sl, sp_version_words) ||
                           (sp_word_in(seg, sl, sp_version_only) && !neutral &&
                            !sp_word_in(seg, sl, sp_album_words));
            if (version || !neutral)
                for (size_t j = 0; j < sl && k + 1 < sizeof keep; j++) keep[k++] = seg[j];
            i = seg_end + (seg_end < len && (in[seg_end] == ')' || in[seg_end] == ']'));
            continue;
        }
        keep[k++] = in[i++];
    }
    keep[k] = '\0';
    sp_squash(keep, k, out, n);
    if (!out[0]) sp_squash(in, len, out, n);   /* all notes: keep what there was */
}

static inline void sp_artist_norm(const char *in, size_t len, char *out, size_t n)
{
    while (len && *in == ' ') { in++; len--; }
    if (len > 4 && !strncasecmp(in, "the ", 4)) { in += 4; len -= 4; }
    sp_squash(in, len, out, n);
}

static inline uint32_t sp_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}
static inline uint32_t sp_title_key(const char *title)
{
    char t[SP_FIELD];
    sp_title_norm(title ? title : "", t, sizeof t);
    return sp_hash(t);
}

static inline bool sp_various(const char *a)
{
    return !a || !a[0] || !strcasecmp(a, "Various Artists") || !strcasecmp(a, "Various") ||
           !strcasecmp(a, "VA") || !strncasecmp(a, "<Untagged", 9);
}

/* The card's artist is one of Spotify's, or all of them together with the
 * first one first ("Mira Sol & Kite" for Mira Sol;Kite). */
static inline bool sp_artist_ok(const char *local, const char *artists)
{
    char l[SP_FIELD], one[SP_FIELD];
    sp_artist_norm(local, strlen(local), l, sizeof l);
    if (!l[0]) return false;
    bool all_in = true, first = true, first_leads = false;
    for (const char *p = artists; *p; first = false)
    {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        sp_artist_norm(p, len, one, sizeof one);
        if (one[0] && !strcmp(one, l)) return true;
        if (first) first_leads = one[0] && !strncmp(l, one, strlen(one));
        if (!one[0] || !strstr(l, one)) all_in = false;
        p += len + (semi ? 1 : 0);
    }
    return first_leads && all_in && strchr(artists, ';');
}

/* The best song on the card for s, or -1. */
static inline int sp_match(const struct sp_song *s, const struct sp_local *lib, int n)
{
    char want[SP_FIELD], got[SP_FIELD], sa[SP_FIELD], la[SP_FIELD];
    sp_title_norm(s->title, want, sizeof want);
    if (!want[0]) return -1;
    uint32_t key = sp_hash(want);
    sp_squash(s->album, strlen(s->album), sa, sizeof sa);
    int best = -1, best_score = -1;
    for (int i = 0; i < n; i++)
    {
        const struct sp_local *l = &lib[i];
        if (l->key != key || !l->title) continue;
        sp_title_norm(l->title, got, sizeof got);
        if (strcmp(got, want)) continue;
        sp_squash(l->album ? l->album : "", l->album ? strlen(l->album) : 0, la, sizeof la);
        bool same_album = sa[0] && !strcmp(sa, la);
        if (sp_various(l->artist) ? !same_album : !sp_artist_ok(l->artist, s->artists)) continue;
        long d = s->ms && l->ms ? s->ms - l->ms : 0;
        if (d < 0) d = -d;
        if (d > 7000) continue;
        int score = (same_album ? 4 : 0) + (s->ms && l->ms && d <= 2000 ? 2 : 0) + (s->ms && l->ms ? 1 : 0);
        if (score > best_score) { best = i; best_score = score; }
    }
    return best;
}

/* One exported line: title, artists, album, length in ms, ISRC (unused),
 * tab separated. False for a header or a line too broken to use. */
static inline bool sp_parse_line(char *line, struct sp_song *s)
{
    if (line[0] == '#' || !line[0]) return false;
    char *f[5] = { 0 };
    int k = 0;
    f[k++] = line;
    for (char *p = line; *p && k < 5; p++)
        if (*p == '\t') { *p = '\0'; f[k++] = p + 1; }
    for (char *p = f[k - 1]; *p; p++) if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
    if (k < 2 || !f[0][0]) return false;
    strncpy(s->title, f[0], SP_FIELD - 1); s->title[SP_FIELD - 1] = '\0';
    strncpy(s->artists, f[1], SP_FIELD - 1); s->artists[SP_FIELD - 1] = '\0';
    strncpy(s->album, k > 2 ? f[2] : "", SP_FIELD - 1); s->album[SP_FIELD - 1] = '\0';
    s->ms = k > 3 ? strtol(f[3], NULL, 10) : 0;
    if (s->ms < 0) s->ms = 0;
    return true;
}
#endif
