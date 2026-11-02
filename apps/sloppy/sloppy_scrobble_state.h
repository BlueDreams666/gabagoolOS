/* GabagoolOS scrobbling: when a play counts, and the text it becomes.
 * No LVGL, no Rockbox, no I/O. GPLv2+.
 *
 * Listening history (sloppy_history_state.h) counts a play after 30 s.
 * Last.fm and ListenBrainz want more: a song of 30 s or longer, heard for
 * half its length or for four minutes. A play that passes becomes a
 * listen: a line in the ListenBrainz queue, a line in the Audioscrobbler
 * log, and JSON on its way to api.listenbrainz.org. */
#ifndef SLOPPY_SCROBBLE_STATE_H
#define SLOPPY_SCROBBLE_STATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SL_SCROB_TEXT 128               /* bytes kept of a tag */

struct sl_listen {
    uint32_t when;                      /* UNIX time the song started */
    uint32_t secs;                      /* its length */
    int track;                          /* number on the album; 0 unknown */
    char artist[SL_SCROB_TEXT], title[SL_SCROB_TEXT], album[SL_SCROB_TEXT];
    char mbid[40];                      /* MusicBrainz recording id, or "" */
};

/* The Audioscrobbler log's first lines, as Rockbox's own scrobbler plugin
 * writes them (apps/plugins/lastfm_scrobbler.c). The times are true UNIX
 * times, so the zone is UTC. */
#define SL_SCROB_LOG_HEADER "#AUDIOSCROBBLER/1.1\n#TZ/UTC\n#CLIENT/GabagoolOS HiBy R1\n" \
    "#ARTIST\t#ALBUM\t#TITLE\t#TRACKNUM\t#LENGTH\t#RATING\t#TIMESTAMP\t#MUSICBRAINZ_TRACKID\n"

static inline bool sl_scrob_counts(uint32_t listened_ms, uint32_t len_ms)
{
    return len_ms >= 30000 && (listened_ms >= len_ms / 2 || listened_ms >= 240000);
}

/* src as one field of a line: tabs, line breaks and other control
 * characters become spaces, bytes that are not UTF-8 become '?', the ends
 * are trimmed and a character is never cut in half. */
static inline void sl_scrob_clean(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    const unsigned char *s = (const unsigned char *)(src ? src : "");
    while (*s == ' ') s++;
    while (*s && n)
    {
        unsigned c = *s;
        size_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        for (size_t i = 1; i < len; i++)
            if ((s[i] & 0xc0) != 0x80) { len = 0; break; }
        if (len == 2 && c < 0xc2) len = 0;                  /* overlong */
        if (!len) { if (o + 1 >= n) break; dst[o++] = '?'; s++; continue; }
        if (o + len >= n) break;
        if (len == 1) dst[o++] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
        else { memcpy(dst + o, s, len); o += len; }
        s += len;
    }
    while (o && dst[o - 1] == ' ') o--;
    if (n) dst[o] = '\0';
}

/* 8-4-4-4-12 hex digits: anything else would have ListenBrainz refuse the
 * whole request */
static inline bool sl_scrob_uuid(const char *s)
{
    for (int i = 0; i < 36; i++)
    {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return s[36] == '\0';
}

/* A queue line: when, length, track, artist, title, album, MBID. Returns
 * its length, 0 when it did not fit. */
static inline int sl_scrob_queue_line(char *b, size_t n, const struct sl_listen *l)
{
    int len = snprintf(b, n, "%lu\t%lu\t%d\t%s\t%s\t%s\t%s\n", (unsigned long)l->when, (unsigned long)l->secs,
                       l->track, l->artist, l->title, l->album, l->mbid);
    return len > 0 && (size_t)len < n ? len : 0;
}

static inline const char *sl_scrob_field(const char *s, char *out, size_t n)
{
    const char *end = strchr(s, '\t');
    size_t len = end ? (size_t)(end - s) : strcspn(s, "\n");
    size_t take = len < n - 1 ? len : n - 1;
    memcpy(out, s, take);
    out[take] = '\0';
    return end ? end + 1 : NULL;
}

/* A queue line back into a listen; false for a line that is not one. */
static inline bool sl_scrob_parse_line(const char *s, struct sl_listen *l)
{
    char num[16];
    memset(l, 0, sizeof *l);
    s = sl_scrob_field(s, num, sizeof num); l->when = (uint32_t)strtoul(num, NULL, 10);
    if (!s) return false;
    s = sl_scrob_field(s, num, sizeof num); l->secs = (uint32_t)strtoul(num, NULL, 10);
    if (!s) return false;
    s = sl_scrob_field(s, num, sizeof num); l->track = atoi(num);
    if (!s) return false;
    s = sl_scrob_field(s, l->artist, sizeof l->artist);
    if (!s) return false;
    s = sl_scrob_field(s, l->title, sizeof l->title);
    if (!s) return false;
    s = sl_scrob_field(s, l->album, sizeof l->album);
    if (!s) return false;
    sl_scrob_field(s, l->mbid, sizeof l->mbid);
    if (l->mbid[0] && !sl_scrob_uuid(l->mbid)) l->mbid[0] = '\0';
    return l->when && l->artist[0] && l->title[0];
}

/* An Audioscrobbler log line: artist, album, title, track (blank when
 * unknown), length, L (listened), time, MBID. */
static inline int sl_scrob_log_line(char *b, size_t n, const struct sl_listen *l)
{
    char track[12] = "";
    if (l->track > 0) snprintf(track, sizeof track, "%d", l->track);
    int len = snprintf(b, n, "%s\t%s\t%s\t%s\t%lu\tL\t%lu\t%s\n", l->artist, l->album, l->title, track,
                       (unsigned long)l->secs, (unsigned long)l->when, l->mbid);
    return len > 0 && (size_t)len < n ? len : 0;
}

/* ------------------------------------------------------------ JSON */
struct sl_jbuf { char *b; size_t n, len; bool full; };

static inline void sl_jput(struct sl_jbuf *j, const char *s)
{
    size_t len = strlen(s);
    if (j->full || j->len + len >= j->n) { j->full = true; return; }
    memcpy(j->b + j->len, s, len + 1);
    j->len += len;
}

static inline void sl_jstr(struct sl_jbuf *j, const char *s)
{
    sl_jput(j, "\"");
    for (; *s && !j->full; s++)
    {
        char e[8] = { *s, 0 };
        if (*s == '"' || *s == '\\') { e[0] = '\\'; e[1] = *s; e[2] = 0; }
        else if ((unsigned char)*s < 0x20) snprintf(e, sizeof e, "\\u%04x", (unsigned)*s);
        sl_jput(j, e);
    }
    sl_jput(j, "\"");
}

static inline void sl_jnum(struct sl_jbuf *j, unsigned long v)
{
    char d[16];
    snprintf(d, sizeof d, "%lu", v);
    sl_jput(j, d);
}

/* The body of a submit-listens request: listen_type "playing_now" (no
 * time), "single" or "import". Returns its length, 0 when it does not fit. */
static inline size_t sl_scrob_json(char *b, size_t n, const char *type, const struct sl_listen *l, int count)
{
    struct sl_jbuf j = { b, n, 0, false };
    if (n) b[0] = '\0';
    sl_jput(&j, "{\"listen_type\":\"");
    sl_jput(&j, type);
    sl_jput(&j, "\",\"payload\":[");
    for (int i = 0; i < count; i++)
    {
        sl_jput(&j, i ? ",{" : "{");
        if (strcmp(type, "playing_now")) { sl_jput(&j, "\"listened_at\":"); sl_jnum(&j, l[i].when); sl_jput(&j, ","); }
        sl_jput(&j, "\"track_metadata\":{\"artist_name\":");
        sl_jstr(&j, l[i].artist);
        sl_jput(&j, ",\"track_name\":");
        sl_jstr(&j, l[i].title);
        if (l[i].album[0]) { sl_jput(&j, ",\"release_name\":"); sl_jstr(&j, l[i].album); }
        sl_jput(&j, ",\"additional_info\":{\"media_player\":\"GabagoolOS\",\"submission_client\":\"GabagoolOS\","
                    "\"submission_client_version\":\"1.0\"");
        if (l[i].secs) { sl_jput(&j, ",\"duration_ms\":"); sl_jnum(&j, (unsigned long)l[i].secs * 1000ul); }
        if (l[i].track > 0) { sl_jput(&j, ",\"tracknumber\":"); sl_jnum(&j, (unsigned long)l[i].track); }
        if (l[i].mbid[0]) { sl_jput(&j, ",\"recording_mbid\":"); sl_jstr(&j, l[i].mbid); }
        sl_jput(&j, "}}}");
    }
    sl_jput(&j, "]}");
    return j.full ? 0 : j.len;
}

/* "key": true in a flat JSON answer */
static inline bool sl_scrob_json_true(const char *json, const char *key)
{
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(json, k);
    if (!p) return false;
    p += strlen(k);
    while (*p == ' ' || *p == ':') p++;
    return !strncmp(p, "true", 4);
}

/* The text of "key": "..." in a flat JSON answer; escapes other than \"
 * and \\ are left as they are. False when it is not there. */
static inline bool sl_scrob_json_text(const char *json, const char *key, char *out, size_t n)
{
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(json, k);
    if (!p || !n) return false;
    p += strlen(k);
    while (*p == ' ' || *p == ':') p++;
    if (*p++ != '"') return false;
    size_t o = 0;
    for (; *p && *p != '"'; p++)
    {
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
        if (o + 1 < n) out[o++] = *p;
    }
    out[o] = '\0';
    return *p == '"';
}

/* Seconds to wait after the nth failure in a row: 30 s doubling to 30 min */
static inline uint32_t sl_scrob_backoff(int failures)
{
    uint32_t s = 30;
    for (int i = 1; i < failures && s < 1800; i++) s *= 2;
    return s < 1800 ? s : 1800;
}
#endif
