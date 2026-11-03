/* Build with the sim include flags plus -I firmware/target/hosted and no
 * other sources: it includes hiby/usb-hiby-gadget.c itself. */
/* usb-hiby-gadget.c's host probe against a pretend kernel */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
void usb_init_device(void);
#define sysfs_get_string fake_get
#define sysfs_set_string fake_set
#define sysfs_set_int fake_set_int
#define sysfs_get_int fake_get_int
#define access fake_access
#define system fake_system
/* the card: Linux's mount(2), and an umount that is busy a while */
#include <errno.h>
#include <unistd.h>
#include <sys/mount.h>
#ifndef MS_RDONLY
#define MS_RDONLY 1
#endif
#ifndef MS_NOSUID
#define MS_NOSUID 2
#define MS_NODEV 4
#define MS_NOEXEC 8
#define MS_SYNCHRONOUS 16
#endif
#ifndef MS_DIRSYNC
#define MS_DIRSYNC 128
#endif
#ifndef MS_NOATIME
#define MS_NOATIME 1024
#endif
#ifndef MS_NODIRATIME
#define MS_NODIRATIME 2048
#endif
#ifndef MS_REMOUNT
#define MS_REMOUNT 32
#endif
#define mount fake_mount
#define umount fake_umount
#define sync fake_sync
#define sleep fake_sleep
/* The card is a real folder here (card_dir, standing in for PIVOT_ROOT), so
 * a working directory or a descriptor of the test's own on it keeps it busy
 * the way the kernel would. */
#include <fcntl.h>
#include <sys/stat.h>
static char card_dir[PATH_MAX], away_path[PATH_MAX];
#define CARD_AWAY_DIR away_path
static char *fake_realpath(const char *p, char *out);
#define realpath fake_realpath
static bool under_card(const char *p) { size_t n = strlen(card_dir); return !strncmp(p, card_dir, n) && (!p[n] || p[n] == '/'); }
static bool fd_on_card(int fd) { char p[PATH_MAX]; return fcntl(fd, F_GETPATH, p) != -1 && under_card(p); }
static bool card_held(void)
{
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) && under_card(cwd)) return true;
    for (int fd = 0; fd < 256; fd++) if (fd_on_card(fd)) return true;
    return false;
}
static int busy_for, umounts, slept;
static bool mounted = true;
int fake_umount(const char *t) { (void)t; umounts++; if (busy_for > 0 || (card_dir[0] && card_held())) { if (busy_for > 0) busy_for--; errno = EBUSY; return -1; } mounted = false; return 0; }
static unsigned long mount_flags;       /* the last mount(2)'s flags */
int fake_mount(const char *d, const char *t, const char *fs, unsigned long f, const void *o) { (void)d; (void)t; (void)fs; (void)o; mount_flags = f; mounted = true; return 0; }
void fake_sync(void) {}
unsigned fake_sleep(unsigned ticks) { slept += ticks; return 0; }
static char udc_state[32] = "not attached", lun[128] = "/dev/mmcblk0p1", udc_bound[64] = "";
static bool power = false, state_readable = true;
static int binds_refused, binds;         /* the kernel turning a bind down (EBUSY at close) */
bool fake_get(const char *p, char *v, int n)
{
    if (strstr(p, "/state")) { if (!state_readable) return false; snprintf(v, n, "%s", udc_state); return true; }
    if (strstr(p, "/UDC")) { snprintf(v, n, "%s", udc_bound); return true; }
    if (strstr(p, "lun.0/file")) { snprintf(v, n, "%s", strcmp(lun, "\n") ? lun : ""); return true; }
    return false;
}
bool fake_set(const char *p, char *v)
{
    if (strstr(p, "lun.0/file")) snprintf(lun, sizeof lun, "%s", v);
    else if (strstr(p, "/UDC") && strcmp(v, "\n") && binds_refused > 0) binds_refused--;
    else if (strstr(p, "/UDC")) { snprintf(udc_bound, sizeof udc_bound, "%s", strcmp(v, "\n") ? v : ""); if (strcmp(v, "\n")) binds++; }
    return true;
}
/* the trace: a RAM file and the card's copy, in a folder of the test's own */
static char trace_ram[PATH_MAX], trace_off[PATH_MAX], trace_card[PATH_MAX], trace_prev[PATH_MAX];
#define USB_TRACE_RAM       trace_ram
#define USB_TRACE_OFF       trace_off
#define USB_TRACE_CARD      trace_card
#define USB_TRACE_CARD_PREV trace_prev
bool fake_set_int(const char *p, int v) { (void)p; (void)v; return true; }
bool fake_get_int(const char *p, int *v) { (void)p; *v = 0; return true; }
int fake_access(const char *p, int m) { (void)m; if (strstr(p, "/state")) return state_readable ? 0 : -1; return 0; }
int fake_system(const char *c) { (void)c; return 0; }
/* where a crash would be logged: never onto the card while it is the computer's */
static int crash_card_away;
void crashlog_card_away(int away) { crash_card_away = away; }
#include "hiby/usb-hiby-gadget.c"
#undef realpath
static char *fake_realpath(const char *p, char *out)
{
    if (!strcmp(p, PIVOT_ROOT)) { strcpy(out, card_dir); return out; }
    return realpath(p, out);
}
volatile long current_tick;
unsigned int power_input_status(void) { return power ? POWER_INPUT_USB_CHARGER : POWER_INPUT_NONE; }
void led_hw_on(void) {}
bool usb_audio_get_active(void) { return false; }
bool usb_dac_start(void) { return true; }
void usb_dac_stop(void) {}
const int USB_DAC_SAMPLE_RATE = 44100;
bool pcm_is_initialized(void) { return true; }
void audio_stop(void) {}
void debugf(const char *f, ...) { (void)f; }
static int rbhome_open = 1;              /* the home directory's descriptor, which pins the card */
void cleanup_rbhome(void) { if (rbhome_open) rbhome_open--; }
void startup_rbhome(void) { rbhome_open++; }
#undef assert
#define assert(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static int count_in(const char *path, const char *needle)
{
    static char b[1 << 16];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = '\0';
    int c = 0;
    for (const char *at = b; (at = strstr(at, needle)) != NULL; at++) c++;
    return c;
}
static int run(int ticks) { int r = 0; for (int i = 0; i < ticks; i++) { current_tick++; r = usb_detect(); } return r; }
int main(void)
{
    char tdir[] = "/tmp/usbtrace.XXXXXX";
    assert(mkdtemp(tdir));
    snprintf(trace_ram, sizeof trace_ram, "%s/ram.txt", tdir);
    snprintf(trace_off, sizeof trace_off, "%s/ram.off", tdir);
    snprintf(trace_card, sizeof trace_card, "%s/card.txt", tdir);
    snprintf(trace_prev, sizeof trace_prev, "%s/card.prev.txt", tdir);
    _usb_mode = -1; power = true;
    assert(run(5) == USB_EXTRACTED && !udc_bound[0]);          /* no mode yet: wait, touch nothing */
    _usb_mode = USB_MODE_MASS_STORAGE;
    /* a wall charger: probed with the card withheld, then left alone */
    assert(run(1) == USB_EXTRACTED && !strcmp(udc_bound, USB_UDC_NAME) && !strcmp(lun, "\n"));   /* no medium */
    strcpy(udc_state, "suspended");                             /* an idle bus is not a host */
    assert(run(2 * HZ) == USB_EXTRACTED);
    assert(run(2 * HZ) == USB_EXTRACTED && !udc_bound[0]);      /* gave up: unbound */
    assert(run(10 * HZ) == USB_EXTRACTED && !udc_bound[0]);     /* and stays usable */
    power = false; assert(run(1) == USB_EXTRACTED);
    /* a computer: the host resets the bus, the probe unbinds, USB is reported */
    strcpy(udc_state, "not attached"); power = true;
    assert(run(1) == USB_EXTRACTED && udc_bound[0]);
    strcpy(udc_state, "default");
    assert(run(1) == USB_INSERTED && !udc_bound[0]);
    assert(run(HZ) == USB_INSERTED);
    /* unplugged mid-probe: unbound at once */
    power = false; run(1); strcpy(udc_state, "not attached"); power = true; run(1);
    assert(udc_bound[0]); power = false; assert(run(1) == USB_EXTRACTED && !udc_bound[0]);
    /* no state file on this kernel: behave as before */
    state_readable = false; power = true; assert(run(1) == USB_INSERTED);
    power = false; run(1); state_readable = true;
    /* other modes untouched */
    _usb_mode = USB_MODE_CHARGE; power = true; assert(run(1) == USB_INSERTED);
    printf("PASS: USB mode only for a computer; a charger leaves the player usable\n");

    /* the handover: a file open a moment is waited out, ~0.4 s a try */
    _usb_mode = USB_MODE_MASS_STORAGE; power = true;
    busy_for = 3; umounts = slept = 0;
    assert(disk_unmount_all() == 1 && !mounted && umounts == 4 && slept == 3 * (HZ * 4 / 10));
    assert(usb_card_handover_error() == 0 && !usb_card_retry() && crash_card_away);
    usb_enable(true); assert(strcmp(lun, "\n") && lun[0] && mounted == false);
    assert(disk_mount_all() == 1 && mounted && !strcmp(lun, "\n") && rbhome_open == 1 && !crash_card_away);
    /* one that stays open: no medium, a few seconds of tries, then the USB
     * screen's retries; once it closes the card goes over and the gadget
     * is bound again so the computer sees it */
    busy_for = 1000; umounts = 0;
    assert(disk_unmount_all() == 0 && mounted && umounts == CARD_OFF_TRIES && !crash_card_away);
    assert(usb_card_handover_error() == EBUSY && rbhome_open == 0);   /* kept closed for the retries */
    usb_enable(true); assert(!strcmp(lun, "\n"));
    assert(usb_card_retry() && usb_card_retry() && umounts == CARD_OFF_TRIES + 2 && mounted);
    busy_for = 0;
    assert(!usb_card_retry() && !mounted && usb_card_handover_error() == 0 && crash_card_away);
    assert(strcmp(lun, "\n") && lun[0] && !strcmp(udc_bound, USB_UDC_NAME));
    umounts = 0; assert(!usb_card_retry() && umounts == 0);    /* done: no more tries */
    assert(disk_mount_all() == 1 && mounted && rbhome_open == 1);
    /* unplugged while still busy: the card simply stays the player's */
    busy_for = 1000;
    assert(disk_unmount_all() == 0 && usb_card_retry());
    assert(disk_mount_all() == 0 || mounted);
    umounts = 0; assert(!usb_card_retry() && umounts == 0 && mounted && rbhome_open == 1);
    assert(!crash_card_away);
    printf("PASS: the card goes over once a file lets go, and the USB screen keeps trying (a crash meanwhile is logged off it)\n");

    /* 2026-10-23: the card would not come off with only folders open on it
     * (/.rockbox, then its root), as working directory or descriptor */
    char base[] = "/tmp/usbcard.XXXXXX", sub[PATH_MAX + 16], file[PATH_MAX + 16], cwd[PATH_MAX], at[PATH_MAX];
    assert(mkdtemp(base) && realpath(base, at));
    snprintf(card_dir, sizeof card_dir, "%s/card", at);
    snprintf(away_path, sizeof away_path, "%s/away", at);
    snprintf(sub, sizeof sub, "%s/.rockbox", card_dir);
    snprintf(file, sizeof file, "%s/song.flac", card_dir);
    assert(mkdirat(AT_FDCWD, card_dir, 0700) == 0 && mkdirat(AT_FDCWD, sub, 0700) == 0);
    int f = openat(AT_FDCWD, file, O_WRONLY | O_CREAT, 0600);
    assert(f >= 0 && write(f, "x", 1) == 1 && close(f) == 0);
    char home[PATH_MAX]; assert(getcwd(home, sizeof home));
    /* the busy list: each item once, a folder and the one inside it both */
    card_busy[0] = '\0';
    card_busy_add("us/rockbox.r1(1012)", sub, "fd 7");
    card_busy_add("us/rockbox.r1(1012)", card_dir, "fd 9");
    card_busy_add("us/rockbox.r1(1012)", sub, "fd 7");
    card_busy_add("us/rockbox.r1(1012)", "/usr/lib/libc.so", "map");      /* not the card */
    assert(!strcmp(card_busy, "us/rockbox.r1(1012):/.rockbox[fd 7] us/rockbox.r1(1012):/[fd 9]"));
    /* working directory and two folders on the card: all let go at the first try */
    assert(chdir(sub) == 0);
    int d1 = openat(AT_FDCWD, sub, O_RDONLY | O_DIRECTORY), d2 = openat(AT_FDCWD, card_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(d1 >= 0 && d2 >= 0 && fd_on_card(d1) && card_held());
    mounted = true; busy_for = 0; umounts = 0;
    assert(disk_unmount_all() == 1 && !mounted && umounts == 1);
    assert(getcwd(cwd, sizeof cwd) && !under_card(cwd));
    struct stat st;
    assert(fstat(d1, &st) == 0 && S_ISDIR(st.st_mode) && !fd_on_card(d1) && !fd_on_card(d2));   /* still open: an empty folder */
    assert((fcntl(d2, F_GETFD) & FD_CLOEXEC) && !(fcntl(d1, F_GETFD) & FD_CLOEXEC));            /* flags kept */
    char want[64]; snprintf(want, sizeof want, "fd %d:/.rockbox", d1); assert(strstr(card_let_go, want));
    snprintf(want, sizeof want, "fd %d:/", d2); assert(strstr(card_let_go, want));
    close(d1); close(d2);                /* the holders close them as usual */
    assert(disk_mount_all() == 1 && mounted && rbhome_open == 1);
    /* a file is never let go: the card stays busy, and the USB screen's
     * retries leave the card as working directory and let go of a folder
     * opened since, every try */
    f = openat(AT_FDCWD, file, O_RDONLY);
    assert(f >= 0 && chdir(home) == 0);
    umounts = 0;
    assert(disk_unmount_all() == 0 && mounted && umounts == CARD_OFF_TRIES && fd_on_card(f));
    assert(chdir(card_dir) == 0);
    int d3 = openat(AT_FDCWD, sub, O_RDONLY | O_DIRECTORY);
    assert(d3 >= 0 && usb_card_retry() && mounted && fd_on_card(f));
    assert(getcwd(cwd, sizeof cwd) && !under_card(cwd) && !fd_on_card(d3));
    close(f);
    assert(!usb_card_retry() && !mounted && usb_card_handover_error() == 0);
    assert(disk_mount_all() == 1 && mounted && rbhome_open == 1);
    close(d3);
    unlinkat(AT_FDCWD, file, 0);
    unlinkat(AT_FDCWD, sub, AT_REMOVEDIR); unlinkat(AT_FDCWD, card_dir, AT_REMOVEDIR);
    unlinkat(AT_FDCWD, away_path, AT_REMOVEDIR); unlinkat(AT_FDCWD, at, AT_REMOVEDIR);
    card_dir[0] = '\0';
    printf("PASS: folders left open on the card and a working directory there no longer keep it from the computer\n");

    /* 2026-10-23: the USB screen was up and the Mac saw no device, with no
     * word why. Every step is traced: in RAM at once, on the card only
     * while the card is the player's. */
    assert(count_in(trace_ram, "probe: no computer in") && count_in(trace_ram, "probe: a computer after"));
    assert(count_in(trace_ram, "handover: umount done") && count_in(trace_ram, "LUN: set to /dev/mmcblk0p1"));
    assert(count_in(trace_ram, "unplugged: USB mode over") && count_in(trace_ram, "LUN: left empty"));
    usb_trace_flush();
    assert(count_in(trace_card, "LUN: set to /dev/mmcblk0p1") == count_in(trace_ram, "LUN: set to /dev/mmcblk0p1"));
    mounted = true; busy_for = 0;
    assert(disk_unmount_all() == 1 && !mounted);
    usb_trace("a step while the card is the computer's");
    usb_trace_flush();
    assert(!count_in(trace_card, "while the card is the computer's"));
    /* a bind the kernel turns down is tried again, and said when it never takes */
    usb_enable(false); assert(!udc_bound[0]);
    binds_refused = 2;
    usb_enable(true);
    assert(!strcmp(udc_bound, USB_UDC_NAME) && count_in(trace_ram, "after binding, try 2"));
    usb_enable(false); assert(!udc_bound[0]);
    binds_refused = 100;
    usb_enable(true);
    assert(!udc_bound[0] && count_in(trace_ram, "could NOT be bound") && count_in(trace_ram, "enable: gadget NOT bound"));
    binds_refused = 0;
    /* a computer that never takes the device: bound again once, 8 s on */
    usb_enable(false); usb_enable(true);
    strcpy(udc_state, "default"); binds = 0;
    current_tick += 2 * HZ; assert(!usb_card_retry() && binds == 0 && count_in(trace_ram, "host: UDC default"));
    current_tick += 7 * HZ; usb_card_retry(); assert(binds == 1 && count_in(trace_ram, "binding the gadget again"));
    current_tick += 20 * HZ; usb_card_retry(); assert(binds == 1);
    strcpy(udc_state, "configured"); usb_card_retry();
    assert(count_in(trace_ram, "host: UDC configured"));
    /* back from the computer: the lines from meanwhile reach the card, once */
    assert(disk_mount_all() == 1 && mounted);
    assert(count_in(trace_card, "while the card is the computer's") == 1 && count_in(trace_card, "host: UDC configured") == 1);
    usb_trace_flush();
    assert(count_in(trace_card, "while the card is the computer's") == 1);
    /* the player restarts after USB mode: the RAM copy is kept, nothing goes twice */
    usb_trace_dirty = true; usb_trace_flush();
    assert(count_in(trace_card, "host: UDC configured") == 1);
    printf("PASS: the USB trace notes every step, reaches the card only while it is the player's, and a refused bind is retried\n");
    /* power off: the card unmounted before the power goes, or read-only
     * when a file still holds it (switched off mounted, its exFAT was left
     * dirty: a Mac found the bitmap to repair, Tai's R1, 3 Nov) */
    card_off = card_leaving = false; mounted = true; busy_for = 0; umounts = 0;
    usb_card_park();
    assert(!mounted && umounts == 1 && card_leaving);
    usb_card_park(); assert(umounts == 1);                     /* once */
    card_off = card_leaving = false; mounted = true; busy_for = 1; umounts = 0; mount_flags = 0;
    usb_card_park();
    assert(umounts == 1 && mounted && (mount_flags & MS_REMOUNT) && (mount_flags & MS_RDONLY));
    card_off = true; card_leaving = false; umounts = 0;
    usb_card_park(); assert(umounts == 0);                    /* the computer has it: left alone */
    card_off = false;
    printf("PASS: at power off the card is unmounted, or made read-only when a file holds it\n");
    unlink(trace_ram); unlink(trace_off); unlink(trace_card); unlink(trace_prev); unlinkat(AT_FDCWD, tdir, AT_REMOVEDIR);
    return 0;
}
