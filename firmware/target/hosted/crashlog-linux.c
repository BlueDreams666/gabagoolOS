/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* A crash log that survives the reboot.
 *
 * The on-screen backtrace in system-hosted.c is gone the moment the device
 * restarts, and on a player in a pocket nobody reads it. This writes the
 * same information to a file on the card and fsyncs it.
 *
 * Only libc here, and only calls that are safe inside a signal handler:
 * open/write/fsync/close and backtrace_symbols_fd(). No Rockbox file API
 * (its open() is a macro over a mutex-protected layer), and no settings
 * save - a crash must never write config.cfg. The path is resolved once at
 * startup, while it is still safe to ask where the card is. */

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "crashlog-linux.h"

extern const char rbversion[];
/* from the default GNU ld script: the executable's code */
extern char __executable_start[], etext[];

static char crashlog_path[256], crashlog_dir[256], crashlog_spool[256];
static volatile int crashlog_card_gone;

static void copy_path(char *to, const char *from)
{
    strncpy(to, from ? from : "", 255);
    to[255] = '\0';
}

/* The card's crash log takes in what the spool kept while the card could
 * not be written: USB mode had handed it to the computer, or it refused. */
static void spool_to_card(void)
{
    int in = open(crashlog_spool, O_RDONLY);
    if (in < 0)
        return;
    int out = open(crashlog_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (out < 0)
    {
        close(in);
        return;                          /* kept for the next start */
    }
    char b[1024];
    ssize_t got;
    int ok = 1;
    while ((got = read(in, b, sizeof b)) > 0)
        if (write(out, b, got) != got)
            ok = 0;
    if (got < 0 || fsync(out) != 0)
        ok = 0;
    close(out);
    close(in);
    if (ok)
        unlink(crashlog_spool);
}

void crashlog_init(const char *ospath, const char *spool)
{
    if (!ospath)
        return;
    copy_path(crashlog_path, ospath);
    copy_path(crashlog_dir, ospath);
    char *slash = strrchr(crashlog_dir, '/');
    if (slash)
        *slash = '\0';
    copy_path(crashlog_spool, spool);
    if (crashlog_spool[0])
        spool_to_card();
}

void crashlog_card_away(int away)
{
    crashlog_card_gone = away;
}

static void put_str(int fd, const char *s)
{
    ssize_t n = write(fd, s, strlen(s));
    (void)n;
}

static void put_hex(int fd, unsigned long v)
{
    char buf[2 + 2 * sizeof(v) + 1];
    int i = sizeof(buf) - 1;
    buf[i] = '\0';
    do {
        buf[--i] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    } while (v && i > 2);
    buf[--i] = 'x';
    buf[--i] = '0';
    put_str(fd, buf + i);
}

static void put_dec(int fd, long v)
{
    char buf[24];
    int i = sizeof(buf) - 1;
    int neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    buf[i] = '\0';
    do {
        buf[--i] = '0' + (u % 10);
        u /= 10;
    } while (u && i > 1);
    if (neg)
        buf[--i] = '-';
    put_str(fd, buf + i);
}

int crashlog_callers(unsigned long sp, unsigned long *out, int max)
{
    /* 128 words stays within the frames of the calls that crashed */
    const unsigned long *p = (const unsigned long *)sp;
    int n = 0;
    for (int i = 0; i < 128 && n < max && p; i++)
        if (p[i] >= (unsigned long)__executable_start && p[i] < (unsigned long)etext)
            out[n++] = p[i];
    return n;
}

int crashlog_write(int sig, unsigned long pc, unsigned long ra,
                   unsigned long sp, void *addr)
{
    if (!crashlog_path[0])
        return CRASHLOG_NOWHERE;

    /* Append: a crash loop leaves every crash, not only the last one.
     * Never onto a card USB mode gave the computer: both writing it is
     * how an exFAT gets corrupted. The spool takes it then. */
    int card_err = 0, where = CRASHLOG_CARD;
    int fd = -1;
    if (!crashlog_card_gone)
    {
        fd = open(crashlog_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0)
            card_err = errno;
    }
    if (fd < 0)
    {
        if (!crashlog_spool[0])
            return CRASHLOG_NOWHERE;
        fd = open(crashlog_spool, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0)
            return CRASHLOG_NOWHERE;
        where = CRASHLOG_SPOOL;
    }

    put_str(fd, "=== crash: signal ");
    put_dec(fd, sig);
    put_str(fd, " (");
    put_str(fd, strsignal(sig));
    put_str(fd, ") pc ");
    put_hex(fd, pc);
    put_str(fd, " ra ");
    put_hex(fd, ra);
    put_str(fd, " addr ");
    put_hex(fd, (unsigned long)addr);
    put_str(fd, "\nversion ");
    put_str(fd, rbversion);
    put_str(fd, "\n");
    if (where == CRASHLOG_SPOOL)
    {
        if (crashlog_card_gone)
            put_str(fd, "card: the computer's (USB mode), kept off it\n");
        else
        {
            put_str(fd, "card: crash.log would not open, errno ");
            put_dec(fd, card_err);
            put_str(fd, "\n");
        }
    }
    /* The header is on the card before the stack is read. */
    fsync(fd);
    if (where == CRASHLOG_CARD && crashlog_dir[0])
    {
        /* a new file's directory entry too: fsync(fd) alone may not
         * write it on the R1's exFAT */
        int dir = open(crashlog_dir, O_RDONLY);
        if (dir >= 0)
        {
            fsync(dir);
            close(dir);
        }
    }

    unsigned long callers[24];
    int ncallers = crashlog_callers(sp, callers, 24);
    put_str(fd, "stack:");
    for (int i = 0; i < ncallers; i++)
    {
        put_str(fd, " ");
        put_hex(fd, callers[i]);
    }
    put_str(fd, "\nbacktrace:\n");

    void *frames[64];
    int count = backtrace(frames, sizeof(frames) / sizeof(frames[0]));
    backtrace_symbols_fd(frames, count, fd);
    put_str(fd, "\n");

    fsync(fd);
    close(fd);
    return where;
}
