/* Build like usb_probe_test: the ui-test compile flags and this file only
 * (it includes thread-common.c and crashlog-linux.c itself).
 *
 * Plugging the R1 into a computer crashed it with a SIGFPE (2026-10-24):
 * the USB screen acknowledges USB mode on the main thread, usb_acknowledge()
 * asks thread_get_debug_info() for that thread's name for the USB trace,
 * and the main thread of a hosted build runs on the process stack, so its
 * stack_size is 0 and the stack percentage divided by it. arm64 divides by
 * zero quietly, so UBSan is made to stop on it here, the way MIPS traps.
 *
 * And the crash log: nothing goes onto a card USB mode gave the computer;
 * a spool off the card keeps it and the next start moves it on. */
#include "config.h"
/* the R1's threads, not the sim's SDL ones: they have stacks to measure */
#undef HAVE_SDL_THREADS
#define HAVE_SIGALTSTACK_THREADS
#include "../../../firmware/kernel/thread-common.c"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* UBSan reports a division by zero and goes on; MIPS traps. Stop here too. */
const char *__ubsan_default_options(void) { return "halt_on_error=1:print_stacktrace=1"; }

void __assert(const char *file, int line, const char *expr) { fprintf(stderr, "%s:%d: %s\n", file, line, expr); abort(); }

/* the bits of the kernel thread-common.c leans on */
volatile long current_tick;
void lcd_transition_idle(void) {}
int set_irq_level(int level) { return level; }
void sleep_thread(int ticks) { (void)ticks; }
void switch_thread(void) {}
unsigned int wakeup_thread_(struct thread_entry *thread IF_PRIO(, enum wakeup_thread_protocol proto))
{ (void)thread; IF_PRIO((void)proto;) return THREAD_NONE; }
int find_first_set_bit(uint32_t v) { return v ? __builtin_ctz(v) : 32; }

/* system-hosted.c: "fake stack, to make thread-internal.h happy" */
uintptr_t *stackbegin;
uintptr_t *stackend;

/* crashlog-linux.c wants the code's bounds (GNU ld) and the version */
#define __executable_start test_text_start
#define etext test_text_end
char test_text_start[1], test_text_end[1];
const char rbversion[] = "test";
#include "../../../firmware/target/hosted/crashlog-linux.c"

static struct thread_entry *thread_like(const char *name, void *stack, size_t size, uintptr_t sp)
{
    struct thread_entry *t = thread_alloc();
    assert(t);
    t->name = name;
    t->stack = stack;
    t->stack_size = size;
    t->context.sp = sp;
    t->state = STATE_RUNNING;
    return t;
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static bool file_has(const char *p, const char *what)
{
    char b[8192];
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    b[n > 0 ? n : 0] = '\0';
    return strstr(b, what) != NULL;
}

int main(void)
{
    thread_alloc_init();

    /* the main thread as init_threads() makes it on a hosted build */
    int s;
    stackbegin = stackend = (uintptr_t *)&s;
    size_t main_size;
    void *main_stack = __get_main_stack(&main_size);
    assert(main_size == 0);
    struct thread_entry *main_thread = thread_like("main", main_stack, main_size, (uintptr_t)&s - 64);
    __cores[0].running = main_thread;

    /* what usb_acknowledge(SYS_USB_CONNECTED_ACK) does on the USB screen */
    struct thread_debug_info ti;
    assert(thread_get_debug_info(thread_self(), &ti) == 1);
    assert(!strcmp(ti.name, "main"));
    assert(ti.stack_usage == 0 && ti.stack_usage_cur == 0);

    /* a thread with a stack of its own still measures: a quarter used */
    static uintptr_t stack[1024];
    for (size_t i = 0; i < 1024; i++)
        stack[i] = i < 768 ? DEADBEEF : 0;
    struct thread_entry *usb = thread_like("usb", stack, sizeof stack, (uintptr_t)&stack[768]);
    assert(thread_get_debug_info(usb->id, &ti) == 1);
    assert(!strcmp(ti.name, "usb") && ti.stack_usage == 25 && ti.stack_usage_cur == 25);
    /* a saved pointer off its stack reads 0%, not a wrapped size */
    usb->context.sp = (uintptr_t)stack - 4096;
    assert(thread_get_debug_info(usb->id, &ti) == 1 && ti.stack_usage_cur == 0);
    printf("PASS: the main thread's stack (none of its own) no longer divides by zero when it answers USB mode\n");

    /* the crash log: a card folder and a spool off it */
    char dir[PATH_MAX], card[PATH_MAX + 32], spool[PATH_MAX + 32];
    snprintf(dir, sizeof dir, "%s/thread_info_test.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(dir));
    snprintf(card, sizeof card, "%s/crash.log", dir);
    snprintf(spool, sizeof spool, "%s/spool.log", dir);

    crashlog_init(card, spool);
    assert(crashlog_write(SIGFPE, 0x662c24, 0x644bf0, (unsigned long)stack, (void *)0x662c24) == CRASHLOG_CARD);
    long on_card = file_size(card);
    assert(on_card > 0 && file_size(spool) < 0 && file_has(card, "pc 0x662c24"));

    /* USB mode: the card is the computer's, so the spool takes it */
    crashlog_card_away(1);
    assert(crashlog_write(SIGFPE, 0x1234, 0, (unsigned long)stack, NULL) == CRASHLOG_SPOOL);
    assert(file_size(card) == on_card);
    assert(file_has(spool, "pc 0x1234") && file_has(spool, "USB mode"));

    /* the next start, card back: it moves on and the spool goes */
    crashlog_card_away(0);
    crashlog_init(card, spool);
    assert(file_size(spool) < 0 && file_size(card) > on_card && file_has(card, "pc 0x1234"));

    /* a card that will not take it: kept in the spool, with why */
    snprintf(card, sizeof card, "%s/gone/crash.log", dir);
    crashlog_init(card, spool);
    assert(crashlog_write(SIGSEGV, 0x5678, 0, (unsigned long)stack, NULL) == CRASHLOG_SPOOL);
    assert(file_has(spool, "pc 0x5678") && file_has(spool, "errno"));
    crashlog_init(card, spool);          /* still no card: the spool stays */
    assert(file_size(spool) > 0);
    printf("PASS: a crash in USB mode is kept off the card and reaches it at the next start\n");

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    return system(cmd) == 0 ? 0 : 1;
}
