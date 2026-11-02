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
#ifndef __CRASHLOG_LINUX_H__
#define __CRASHLOG_LINUX_H__

/* ospath is a real filesystem path, already translated. spool, if not
 * NULL, is off the card and survives a reboot: a crash while the card
 * cannot be written goes there, and the next crashlog_init() moves it
 * onto the card. */
void crashlog_init(const char *ospath, const char *spool);
/* The card is the computer's (USB mode): nothing goes onto it. */
void crashlog_card_away(int away);
/* Async-signal-safe. Appends one entry and fsyncs. Returns where. */
enum { CRASHLOG_NOWHERE, CRASHLOG_CARD, CRASHLOG_SPOOL };
int crashlog_write(int sig, unsigned long pc, unsigned long ra,
                   unsigned long sp, void *addr);
/* Words on the stack above sp that point into our code: most are return
 * addresses of the calls that led to the crash, newest first. glibc's
 * backtrace() cannot unwind through the signal frame on MIPS, so this is
 * the only call chain a stripped binary can give. Look the addresses up
 * in the build's rockbox.map (tools/crashlog-symbols.py). */
int crashlog_callers(unsigned long sp, unsigned long *out, int max);

#endif
