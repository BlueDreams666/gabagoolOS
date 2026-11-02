/* The database scan must see USB mode asked for (tagcache.c's
 * check_event_queue) even when another event is ahead of it in the
 * thread's queue. On the R1 a plugged-in cable broadcasts
 * SYS_CHARGER_CONNECTED just before SYS_USB_CONNECTED: a scan that only
 * looked at the head ran on through USB mode, kept album folders and
 * database_tmp.tcd open, and the card could not be unmounted (EBUSY).
 * Real queue.c, and check_event_queue's own body, cut from tagcache.c by
 * run-tagcache-usb-test.sh. */
#include <stdio.h>
#include <stdlib.h>
#include "../../../firmware/kernel/queue.c"

/* what queue.c needs from the rest of the kernel; no thread ever waits here */
volatile long current_tick;
struct core_entry __cores[NUM_CORES];
int set_irq_level(int level) { return level; }
void switch_thread(void) {}
unsigned int wakeup_thread_(struct thread_entry *thread IF_PRIO(, enum wakeup_thread_protocol proto))
{ (void)thread; IF_PRIO((void)proto;) return 0; }
unsigned int wait_queue_wake(struct __wait_queue *wqp) { (void)wqp; return 0; }
void block_thread_(struct thread_entry *current, int timeout) { (void)current; (void)timeout; }
void panicf(const char *fmt, ...) { (void)fmt; abort(); }
void debugf(const char *fmt, ...) { (void)fmt; }
void **find_array_ptr(void **arr, void *ptr) { (void)arr; (void)ptr; return NULL; }
int remove_array_ptr(void **arr, void *ptr) { (void)arr; (void)ptr; return -1; }
void lld_remove(struct lld_head *list, struct lld_node *node) { (void)list; (void)node; }

enum tagcache_queue { Q_STOP_SCAN = 0, Q_START_SCAN, Q_IMPORT_CHANGELOG, Q_UPDATE, Q_REBUILD };
static struct event_queue tagcache_queue;
#include "tagcache_check_event_queue.inc"

static int fails;
static void expect(bool got, bool want, const char *what)
{
    printf("%s: %s\n", got == want ? "PASS" : "FAIL", what);
    if (got != want) fails++;
}

int main(void)
{
    queue_init(&tagcache_queue, false);
    expect(check_event_queue(), false, "an empty queue: the scan goes on");
    queue_post(&tagcache_queue, SYS_CHARGER_CONNECTED, 0);
    expect(check_event_queue(), false, "a charger alone: the scan goes on");
    queue_post(&tagcache_queue, SYS_USB_CONNECTED, 0);
    expect(check_event_queue(), true, "USB mode behind the charger event: the scan stops");
    queue_clear(&tagcache_queue);
    queue_post(&tagcache_queue, SYS_BATTERY_UPDATE, 0);
    queue_post(&tagcache_queue, Q_STOP_SCAN, 0);
    expect(check_event_queue(), true, "a stop behind another event: the scan stops");
    queue_clear(&tagcache_queue);
    queue_post(&tagcache_queue, SYS_USB_CONNECTED, 0);
    expect(check_event_queue(), true, "USB mode at the head: the scan stops");
    struct queue_event ev;
    queue_wait_w_tmo(&tagcache_queue, &ev, 0);
    expect(ev.id == SYS_USB_CONNECTED, true, "looking leaves the event for the thread to answer");
    return fails ? 1 : 0;
}
