/* GabagoolOS lyrics carried inside a song, no LVGL or Rockbox. GPLv2+.
 *
 * Taggers like Picard and Mp3tag write lyrics into the file: an ID3v2
 * USLT frame (ULT in v2.2) in MP3s, a Vorbis comment LYRICS= or
 * UNSYNCEDLYRICS= in FLAC. Only that frame is read: the rest are seeked
 * past, the cover picture often among them. The text comes out as the
 * .lrc converter takes it (sl_lrc_to_utf8): UTF-16 with its byte order
 * mark, UTF-8, or Latin-1. It may itself be timed LRC. */
#ifndef SLOPPY_TAGLYRICS_H
#define SLOPPY_TAGLYRICS_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SL_TAG_LYRICS_MAX (256 * 1024)   /* a frame bigger than this is not lyrics */

static inline uint32_t sl_tag_be(const unsigned char *p, int n, bool syncsafe)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = syncsafe ? v << 7 | (p[i] & 0x7f) : v << 8 | p[i];
    return v;
}
static inline bool sl_tag_at(int fd, uint32_t at, unsigned char *buf, size_t n)
{
    return lseek(fd, (off_t)at, SEEK_SET) == (off_t)at && read(fd, buf, n) == (ssize_t)n;
}
/* the key of a "KEY=value" comment, any case */
static inline bool sl_tag_key(const unsigned char *s, size_t len, const char *key)
{
    size_t k = strlen(key);
    if (len <= k) return false;
    for (size_t i = 0; i < k; i++)
        if ((s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]) != (unsigned char)key[i]) return false;
    return true;
}

static inline size_t sl_tag_id3(int fd, const unsigned char *h, unsigned char *out, size_t cap)
{
    int ver = h[3], idn = ver == 2 ? 3 : 4, hn = ver == 2 ? 6 : 10;
    uint32_t end = 10 + sl_tag_be(h + 6, 4, true), at = 10;
    if ((h[5] & 0x40) && ver >= 3)       /* an extended header first */
    {
        unsigned char e[4];
        if (!sl_tag_at(fd, 10, e, 4)) return 0;
        at += ver == 4 ? sl_tag_be(e, 4, true) : 4 + sl_tag_be(e, 4, false);
    }
    while (at + (uint32_t)hn <= end)
    {
        unsigned char f[10];
        if (!sl_tag_at(fd, at, f, (size_t)hn) || !f[0]) return 0;   /* padding: no more frames */
        uint32_t size = sl_tag_be(f + idn, ver == 2 ? 3 : 4, ver == 4);
        at += (uint32_t)hn;
        if (size > end - at) return 0;
        if (!memcmp(f, ver == 2 ? "ULT" : "USLT", (size_t)idn) && size > 5 && size <= SL_TAG_LYRICS_MAX)
        {
            unsigned char *b = malloc(size);
            size_t n = 0;
            if (b && sl_tag_at(fd, at, b, size))
            {
                /* encoding, language, a description ended by a zero, then the words */
                int enc = b[0];
                bool wide = enc == 1 || enc == 2;
                uint32_t p = 4;
                if (wide) { while (p + 1 < size && (b[p] || b[p + 1])) p += 2; p += 2; }
                else { while (p < size && b[p]) p++; p++; }
                if (enc == 2 && cap > 2) { out[0] = 0xfe; out[1] = 0xff; n = 2; }   /* big-endian, no mark */
                if (p < size)
                {
                    size_t take = size - p < cap - 1 - n ? size - p : cap - 1 - n;
                    if (wide) take &= ~(size_t)1;
                    memcpy(out + n, b + p, take);
                    n += take;
                }
            }
            free(b);
            if (n > 2 || (n && !(out[0] == 0xfe && out[1] == 0xff))) return n;
            return 0;
        }
        at += size;
    }
    return 0;
}

static inline size_t sl_tag_flac(int fd, unsigned char *out, size_t cap)
{
    uint32_t at = 4;
    for (int blocks = 0; blocks < 64; blocks++)
    {
        unsigned char bh[4];
        if (!sl_tag_at(fd, at, bh, 4)) return 0;
        uint32_t len = sl_tag_be(bh + 1, 3, false);
        at += 4;
        if ((bh[0] & 0x7f) == 4 && len >= 8 && len <= 4 * SL_TAG_LYRICS_MAX)   /* the Vorbis comments */
        {
            unsigned char *b = malloc(len);
            size_t n = 0;
            if (b && sl_tag_at(fd, at, b, len))
            {
                #define LE32(q) ((uint32_t)(q)[0] | (uint32_t)(q)[1] << 8 | (uint32_t)(q)[2] << 16 | (uint32_t)(q)[3] << 24)
                if (LE32(b) > len - 8) { free(b); return 0; }   /* a vendor length past the block: not a comment block we trust */
                uint32_t p = 4 + LE32(b);              /* past the vendor string */
                uint32_t count = p + 4 <= len ? LE32(b + p) : 0;
                for (p += 4; count-- && p + 4 <= len; )
                {
                    uint32_t l = LE32(b + p);
                    p += 4;
                    if (l > len - p) break;
                    size_t skip = sl_tag_key(b + p, l, "LYRICS=") ? 7 : sl_tag_key(b + p, l, "UNSYNCEDLYRICS=") ? 15 : 0;
                    if (skip)
                    {
                        n = l - skip < cap - 1 ? l - skip : cap - 1;
                        memcpy(out, b + p + skip, n);
                        break;
                    }
                    p += l;
                }
                #undef LE32
            }
            free(b);
            return n;
        }
        if (bh[0] & 0x80) return 0;          /* the last block */
        at += len;
    }
    return 0;
}

/* The lyrics in the song open on fd, into out; their length, 0 for none. */
static inline size_t sl_tag_lyrics(int fd, unsigned char *out, size_t cap)
{
    unsigned char h[10];
    if (cap < 8 || !sl_tag_at(fd, 0, h, 10)) return 0;
    if (!memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4) return sl_tag_id3(fd, h, out, cap);
    if (!memcmp(h, "fLaC", 4)) return sl_tag_flac(fd, out, cap);
    return 0;
}
#endif
