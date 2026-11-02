/***************************************************************************
 * Standard MIDI files: the length, from every track's last event and the
 * tempo changes along the way, and a title from the first track's name.
 * Each track is read once from start to end, a few KB at a time.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "platform.h"

#include "metadata.h"
#include "metadata_common.h"
#include "metadata_parsers.h"
#include "logf.h"

#define MIDI_TRACKS  64
#define MIDI_TEMPOS  1024

struct midi_rd {
    int fd;
    uint32_t left;                       /* bytes of the track not yet read */
    int pos, len;
    unsigned char buf[1024];
};

static int rd_byte(struct midi_rd *r)
{
    if (r->pos == r->len) {
        if (!r->left)
            return -1;
        int n = read(r->fd, r->buf, r->left < sizeof r->buf ? r->left : sizeof r->buf);
        if (n <= 0)
            return -1;
        r->left -= n;
        r->pos = 0;
        r->len = n;
    }
    return r->buf[r->pos++];
}

static int64_t rd_var(struct midi_rd *r)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int c = rd_byte(r);
        if (c < 0)
            return -1;
        v = v << 7 | (c & 0x7f);
        if (!(c & 0x80))
            return v;
    }
    return v;
}

static bool rd_skip(struct midi_rd *r, uint32_t n)
{
    while (n--)
        if (rd_byte(r) < 0)
            return false;
    return true;
}

static struct { uint32_t tick, us; } tempos[MIDI_TEMPOS];
static int ntempos;

static int tempo_cmp(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* a track from start to end: its last tick, its tempo changes kept, and
 * its name when it is the first (into title, up to n bytes) */
static uint32_t scan_track(struct midi_rd *r, char *title, int n)
{
    uint32_t tick = 0;
    int status = 0;
    while (1) {
        int64_t d = rd_var(r);
        if (d < 0 || tick + (uint32_t)d < tick)
            break;
        tick += (uint32_t)d;
        int c = rd_byte(r);
        if (c < 0)
            break;
        if (c & 0x80) {
            status = c < 0xf0 ? c : 0;
            if (c == 0xff) {
                int type = rd_byte(r);
                int64_t len = rd_var(r);
                if (type < 0 || len < 0)
                    break;
                if (type == 0x2f)
                    break;
                if (type == 0x51 && len >= 3 && ntempos < MIDI_TEMPOS) {
                    int a = rd_byte(r), b = rd_byte(r), e = rd_byte(r);
                    if (a < 0 || b < 0 || e < 0)
                        break;
                    tempos[ntempos].tick = tick;
                    tempos[ntempos].us = (uint32_t)a << 16 | b << 8 | e;
                    ntempos++;
                    len -= 3;
                }
                else if (type == 0x03 && title && !title[0]) {
                    int k = 0;
                    for (; len > 0; len--) {
                        int ch = rd_byte(r);
                        if (ch < 0)
                            break;
                        if (k < n - 1 && ch >= 0x20 && ch < 0x7f)
                            title[k++] = ch;
                    }
                    title[k] = 0;
                }
                if (!rd_skip(r, (uint32_t)len))
                    break;
                continue;
            }
            if (c == 0xf0 || c == 0xf7) {
                int64_t len = rd_var(r);
                if (len < 0 || !rd_skip(r, (uint32_t)len))
                    break;
                continue;
            }
            if (c > 0xf7)
                break;
        }
        else {
            if (!status)
                break;
            r->pos--;                    /* running status: c was its first data byte */
        }
        int k = (status & 0xe0) == 0xc0 ? 1 : 2;
        if (!rd_skip(r, k))
            break;
    }
    return tick;
}

bool get_midi_metadata(int fd, struct mp3entry* id3)
{
    unsigned char h[14];
    uint32_t base = 0, end = 0;
    if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, h, 12) != 12)
        return false;
    id3->filesize = filesize(fd);
    end = id3->filesize;
    /* RIFF RMID: the file is in the "data" chunk */
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "RMID", 4)) {
        uint32_t at = 12;
        while (1) {
            unsigned char c[8];
            if (lseek(fd, at, SEEK_SET) < 0 || read(fd, c, 8) != 8)
                return false;
            uint32_t n = get_long_le(c + 4);
            if (!memcmp(c, "data", 4)) {
                base = at + 8;
                if (n < end - base)
                    end = base + n;
                break;
            }
            if (n > end - at - 8)
                return false;
            at += 8 + n + (n & 1);
        }
    }
    if (lseek(fd, base, SEEK_SET) < 0 || read(fd, h, 14) != 14 || memcmp(h, "MThd", 4))
        return false;
    uint32_t hlen = get_long_be(h + 4);
    unsigned div = h[12] << 8 | h[13];
    if (hlen < 6 || hlen > end - base - 8)
        return false;

    static struct midi_rd r;
    char *title = (char *)id3->id3v2buf;
    title[0] = 0;
    ntempos = 0;
    uint32_t at = base + 8 + hlen, last = 0;
    int tracks = 0;
    while (tracks < MIDI_TRACKS && end - at >= 8) {
        unsigned char c[8];
        if (lseek(fd, at, SEEK_SET) < 0 || read(fd, c, 8) != 8)
            break;
        uint32_t n = get_long_be(c + 4);
        if (n > end - at - 8)
            n = end - at - 8;
        if (!memcmp(c, "MTrk", 4)) {
            r.fd = fd;
            r.left = n;
            r.pos = r.len = 0;
            uint32_t t = scan_track(&r, tracks ? NULL : title, sizeof id3->id3v2buf);
            if (t > last)
                last = t;
            tracks++;
        }
        at += 8 + n;
    }
    if (!tracks)
        return false;

    /* ticks to time, through the tempo changes in order */
    uint64_t us = 0;
    if (div & 0x8000) {
        int fps = -(int8_t)(div >> 8);
        unsigned tps = (unsigned)(fps == 29 ? 30 : fps) * (div & 0xff);
        if (!tps)
            return false;
        us = (uint64_t)last * 1000000 / tps;
    }
    else {
        unsigned tpq = div ? div : 96;
        qsort(tempos, ntempos, sizeof tempos[0], tempo_cmp);
        uint32_t tick = 0, tempo = 500000;
        for (int i = 0; i < ntempos && tempos[i].tick < last; i++) {
            us += (uint64_t)(tempos[i].tick - tick) * tempo / tpq;
            tick = tempos[i].tick;
            tempo = tempos[i].us ? tempos[i].us : 500000;
        }
        us += (uint64_t)(last - tick) * tempo / tpq;
    }

    id3->vbr = false;
    id3->frequency = 44100;
    id3->length = (unsigned long)(us / 1000);
    id3->bitrate = id3->length ? (unsigned)((uint64_t)id3->filesize * 8 / id3->length) : 0;
    if (title[0])
        id3->title = title;
    return id3->length > 0;
}
