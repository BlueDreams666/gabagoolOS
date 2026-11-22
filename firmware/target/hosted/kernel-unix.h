/***************************************************************************
 *             __________               __   ___.                  
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___  
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /  
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <   
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \  
 *                     \/            \/     \/    \/            \/ 
 * $Id$
 *
 * Copyright (C) 2010 by Thomas Martitz
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

#ifndef __KERNEL_UNIX_H__
#define __KERNEL_UNIX_H__

/* interrupt emulation incomplete, doesn't seem needed so far */
#define disable_irq()
#define enable_irq()
#define disable_irq_save() 0
#define restore_irq(level) (void)level

void wait_for_interrupt(void);
void interrupt(void);

/* The tick tasks run on the timer's own OS thread, alongside ours, and
 * disable_irq_save() stops nothing: the tick and timeout lists are kept
 * under this lock instead (recursive, as a task may take itself off) */
#define HAVE_TICK_LOCK
void tick_lock(void);
void tick_unlock(void);

/* A post from another OS thread (a tick task, mostly) stores its event, but
 * waking the thread that waits on the queue is left to the scheduler's own
 * thread: wakeup_thread edits the scheduler's lists, which that thread edits
 * unlocked. Done from the tick's thread, a key posted as the UI went to
 * sleep on the button queue could lose the UI for good: it stayed blocked,
 * every other thread ran on, and no key woke it (a test R1, 19 Nov). */
struct event_queue;
bool kernel_off_thread(void);
void kernel_wake_later(struct event_queue *q);
struct event_queue *kernel_wake_take(void);
void queue_wake_deferred(void);

/* A sleep, block or switch asked for on another OS thread would mark the
 * Rockbox thread running there as waiting and switch contexts on the wrong
 * OS thread: the UI was left asleep for good, its saved stack another
 * thread's (a test R1, 22 Nov). It is refused, and the first caller's
 * stack kept for the log. */
void kernel_misuse(const char *what);
extern const char *volatile kernel_misuse_what;
extern volatile unsigned kernel_misuse_n;
extern unsigned long kernel_misuse_stack[8];

#endif /* __KERNEL_UNIX_H__ */
