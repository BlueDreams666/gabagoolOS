/* Run with run-tick-stall-test.sh (Linux: timer_create; on a Mac it runs
 * in the R1's build VM). The real firmware/target/hosted/kernel-unix.c,
 * its Rockbox headers stood in for by empty ones.
 *
 * Tai's R1 froze in Your taste (3 Nov): its log's time stood at 113 s while
 * the watchdog counted 20 s stuck, every Rockbox thread asleep waiting for a
 * tick. The timer thread waited for the tick lock, and while anything kept
 * that lock the clock stopped, and every sleep, timeout and fade with it.
 * Now the tick counts and wakes the scheduler whoever holds the lock; its
 * tasks wait for the next one. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NUM_TICK_TASKS 8
#define TIMER_FREQ 1000000
volatile long current_tick;
void (*tick_funcs[MAX_NUM_TICK_TASKS + 1])(void);
void tick_lock(void);
void tick_unlock(void);
void panicf(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); abort(); }

#include "../../../firmware/target/hosted/kernel-unix.c"

static int runs;
static void task(void)
{
    runs++;
    assert(tick_task_now == task);   /* the watchdog can name it */
}

static pthread_mutex_t go_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t go_cond = PTHREAD_COND_INITIALIZER;
static int holding, release;
static void *holder(void *arg)
{
    (void)arg;
    tick_lock();                        /* a thread that keeps the tick lock */
    pthread_mutex_lock(&go_mtx);
    holding = 1;
    pthread_cond_broadcast(&go_cond);
    while (!release) pthread_cond_wait(&go_cond, &go_mtx);
    pthread_mutex_unlock(&go_mtx);
    tick_unlock();
    return NULL;
}

int main(void)
{
    union sigval none = { 0 };
    tick_funcs[0] = task;

    for (int i = 0; i < 3; i++) timer_signal(none);
    assert(current_tick == 3 && runs == 3 && tick_skipped == 0 && tick_task_now == NULL);

    pthread_t t;
    pthread_create(&t, NULL, holder, NULL);
    pthread_mutex_lock(&go_mtx);
    while (!holding) pthread_cond_wait(&go_cond, &go_mtx);
    pthread_mutex_unlock(&go_mtx);

    /* the lock kept: each tick still counts, its task waits */
    for (int i = 0; i < 100; i++) timer_signal(none);
    assert(current_tick == 103 && runs == 3 && tick_skipped == 100);

    /* and the real timer, a tick every 10 ms on its own thread, goes on too */
    tick_start(10);
    struct timespec nap = { 0, 300 * 1000 * 1000 };
    nanosleep(&nap, NULL);
    long counted = current_tick - 103;
    assert(counted >= 10 && runs == 3);

    pthread_mutex_lock(&go_mtx);
    release = 1;
    pthread_cond_broadcast(&go_cond);
    pthread_mutex_unlock(&go_mtx);
    pthread_join(t, NULL);
    nanosleep(&nap, NULL);
    assert(runs > 3);                   /* the lock back: the tasks run again */
    printf("tick: %ld ticks counted while the lock was kept, %u put off, tasks ran again after\n",
           counted, tick_skipped);
    return 0;
}
