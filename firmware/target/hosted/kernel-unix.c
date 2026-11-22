/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (c) 2010 Thomas Martitz
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


#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "audio.h"
#include "panic.h"
#include "timer.h"

/* Forward-declaration */
void interrupt(void);

static pthread_mutex_t tick_mtx;
static pthread_once_t tick_mtx_once = PTHREAD_ONCE_INIT;
static void tick_mtx_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&tick_mtx, &attr);
    pthread_mutexattr_destroy(&attr);
}
/* A task added while the timer thread took another off could land past
 * the list's end and never run again: a lost fade kept audio_stop()
 * waiting for good (the R1 froze, music and all) */
void tick_lock(void)
{
    pthread_once(&tick_mtx_once, tick_mtx_init);
    pthread_mutex_lock(&tick_mtx);
}
void tick_unlock(void)
{
    pthread_mutex_unlock(&tick_mtx);
}

struct event_queue;
static pthread_t kernel_thread;
static volatile bool kernel_thread_known;
bool kernel_off_thread(void)
{
    return kernel_thread_known && !pthread_equal(pthread_self(), kernel_thread);
}
/* the queues whose waiters the scheduler's thread is to wake */
static pthread_mutex_t wake_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct event_queue *wake_list[16];
static int wake_n;
void kernel_wake_later(struct event_queue *q)
{
    pthread_mutex_lock(&wake_mtx);
    int i;
    for (i = 0; i < wake_n && wake_list[i] != q; i++)
        ;
    if (i == wake_n && wake_n < (int)(sizeof wake_list / sizeof wake_list[0]))
        wake_list[wake_n++] = q;
    pthread_mutex_unlock(&wake_mtx);
    interrupt();
}
struct event_queue *kernel_wake_take(void)
{
    struct event_queue *q = NULL;
    if (!wake_n)                         /* unlocked look: a miss is taken on the next pass */
        return NULL;
    pthread_mutex_lock(&wake_mtx);
    if (wake_n)
        q = wake_list[--wake_n];
    pthread_mutex_unlock(&wake_mtx);
    return q;
}

const char *volatile kernel_misuse_what;
volatile unsigned kernel_misuse_n;
unsigned long kernel_misuse_stack[8];
extern char __executable_start[], etext[];
void kernel_misuse(const char *what)
{
    if (__atomic_add_fetch(&kernel_misuse_n, 1, __ATOMIC_RELAXED) != 1)
        return;                          /* the first is kept */
    const unsigned long *w = __builtin_frame_address(0);
    int k = 0;
    for (int i = 0; i < 256 && k < 8; i++)
        if (w[i] >= (unsigned long)__executable_start && w[i] < (unsigned long)etext)
            kernel_misuse_stack[k++] = w[i];
    kernel_misuse_what = what;
}

static pthread_cond_t wfi_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t wfi_mtx = PTHREAD_MUTEX_INITIALIZER;
/*
 * call tick tasks and wake the scheduler up */
/* The tick task running now, and the ticks whose tasks were put off: the
 * UI's watchdog names them when the main loop is stuck */
void (*volatile tick_task_now)(void);
volatile unsigned int tick_skipped;
void timer_signal(union sigval arg)
{
    (void)arg;
    /* Never wait for the lock: one kept by a thread stuck holding it (or by
     * a tick task that never came back) stopped the clock for good, and
     * every sleep, timeout and fade with it (the R1 froze in Your taste,
     * 3 Nov, its log's time standing still). The tick still counts and
     * wakes the scheduler; its tasks run on the next one. */
    pthread_once(&tick_mtx_once, tick_mtx_init);
    if (pthread_mutex_trylock(&tick_mtx) == 0)
    {
        extern void (*tick_funcs[MAX_NUM_TICK_TASKS+1])(void);
        current_tick++;
        for (void (**p)(void) = tick_funcs; *p; p++)
        {
            tick_task_now = *p;
            (*p)();
        }
        tick_task_now = NULL;
        tick_unlock();
    }
    else
    {
        __atomic_add_fetch(&current_tick, 1, __ATOMIC_RELAXED);
        tick_skipped++;
    }
    interrupt();
}

/*
 * wait on the sem which the signal handler posts to save cpu time (aka sleep)
 *
 * other mechanisms could use them as well */
void wait_for_interrupt(void)
{
    pthread_cond_wait(&wfi_cond, &wfi_mtx);
}

/*
 * Wakeup the kernel, if sleeping (shall not be called from a signal handler) */
void interrupt(void)
{
    pthread_cond_signal(&wfi_cond);
}

/*
 * setup a hrtimer to send a signal to our process every tick
 *
 * WARNING for Android: JNI calls are not permitted from tick tasks, as the
 * underlying thread is not attached to the Java VM
 *
 * Can be possibly be attached if it really needs to be. but let's
 * keep this leightweight
 */

void tick_start(unsigned int interval_in_ms)
{
    int ret = 0;
    kernel_thread = pthread_self();      /* init_tick, on the scheduler's thread */
    kernel_thread_known = true;
    timer_t timerid;
    struct itimerspec ts;
    sigevent_t sigev;

    /* initializing in the declaration causes some weird warnings */
    memset(&sigev, 0, sizeof(sigevent_t));
    sigev.sigev_notify = SIGEV_THREAD,
    sigev.sigev_notify_function = timer_signal;

    ts.it_value.tv_sec = ts.it_interval.tv_sec = 0;
    ts.it_value.tv_nsec = ts.it_interval.tv_nsec = interval_in_ms*1000*1000;

    /* add the timer */
    ret |= timer_create(CLOCK_REALTIME, &sigev, &timerid);
    ret |= timer_settime(timerid, 0, &ts, NULL);

    /* Grab the mutex already now and leave it to this thread. We don't
     * care about race conditions when signaling the condition (because
     * they are not critical), but a mutex is necessary due to the API */
    pthread_mutex_lock(&wfi_mtx);

    if (ret != 0)
        panicf("%s(): %s\n", __func__, strerror(errno));
}

#define cycles_to_microseconds(cycles) \
    ((int)((10000*cycles)/(TIMER_FREQ / 100)))


static timer_t timer_tid;
static int timer_prio = -1;
void (*global_unreg_callback)(void);
void (*global_timer_callback)(void);

static void timer_cb(union sigval arg)
{
    /* check for spurious callbacks [arg.sival_ptr] */
    if (global_timer_callback && global_timer_callback == arg.sival_ptr)
        global_timer_callback();
}

bool timer_register(int reg_prio, void (*unregister_callback)(void),
                    long cycles, void (*timer_callback)(void))
{
    int ret = 0;
    struct itimerspec ts;
    sigevent_t sigev;
    long in_us = cycles_to_microseconds(cycles);

    if (reg_prio <= timer_prio || in_us <= 0)
        return false;

    if(timer_prio >= 0)
    {
        if (global_unreg_callback) /* timer has callback user needs to unreg */
            global_unreg_callback();
        else /* no callback -- delete timer */
            timer_delete(timer_tid);
    }

    memset(&sigev, 0, sizeof(sigevent_t));
    sigev.sigev_notify = SIGEV_THREAD,
    sigev.sigev_notify_function = timer_cb;
    sigev.sigev_value.sival_ptr = timer_callback; /* store cb to check later */

    div_t q = div(in_us, 1000000);
    ts.it_value.tv_sec = ts.it_interval.tv_sec = q.quot;
    ts.it_value.tv_nsec = ts.it_interval.tv_nsec = q.rem*1000;

    /* add the timer */
    ret |= timer_create(CLOCK_REALTIME, &sigev, &timer_tid);
    ret |= timer_settime(timer_tid, 0, &ts, NULL);

    global_timer_callback = timer_callback;
    global_unreg_callback = unregister_callback;
    timer_prio = reg_prio;

    return ret == 0;
}

bool timer_set_period(long cycles)
{
    struct itimerspec ts;
    long in_us = cycles_to_microseconds(cycles);
    div_t q = div(in_us, 1000000);
    ts.it_value.tv_sec = ts.it_interval.tv_sec = q.quot;
    ts.it_value.tv_nsec = ts.it_interval.tv_nsec = q.rem*1000;

    return timer_settime(timer_tid, 0, &ts, NULL) == 0;
}

void timer_unregister(void)
{
    timer_delete(timer_tid);
    timer_prio = -1;
    global_unreg_callback = NULL;
    global_timer_callback = NULL;
}
