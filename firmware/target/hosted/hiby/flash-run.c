/* The player started from the R1's own flash, not from RAM. GPLv2+.
 *
 * The boot menu copies the player (13 MB) to /tmp and starts it there, so
 * the card can be handed to a computer in USB mode. /tmp is RAM, and RAM
 * that is never given back: a quarter of the R1's 57 MB, all of it held
 * whether the player uses those pages or not. With Bluetooth on that left
 * 5 to 6 MB, where the card's cache runs cold and the stalls and dropouts
 * came from (test R1 logs, November). Started from a file on the flash
 * (/usr/data: ubifs, 34 MB free on both test R1s), the kernel keeps only
 * the pages in use and reads the others back when they are wanted.
 *
 * Nothing here may ever keep the player from starting:
 *  - the copy is made later, on a thread of its own, while the player
 *    runs from /tmp as before, and is read back and compared before it is
 *    trusted; the next start is the first to use it
 *  - a start from the flash leaves a mark that the player takes away once
 *    its screen is up. A mark still there at the next start means that
 *    start never got that far: the copy goes, that build is not tried
 *    from the flash again, and the player starts from /tmp
 *  - whatever cannot be read, written or run leaves things as they were
 *
 * The paths are arguments so the test can run it all in a folder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "flash-run.h"

#ifndef ST_NOEXEC
#define ST_NOEXEC 0                  /* not everywhere the test is built */
#endif
#define FR_ENV      "GABAGOOL_FROM_FLASH"
#define FR_KEEP_KB  8192                 /* left free on the flash for the stock player's own files */

static int fr_state = FLASH_RUN_OFF;
int flash_run_state(void) { return fr_state; }
const char *flash_run_says(void)
{
    switch (fr_state)
    {
    case FLASH_RUN_FLASH:      return "from the flash copy (the copy in /tmp is gone, its memory free)";
    case FLASH_RUN_NO_COPY:    return "from /tmp: no flash copy of this build yet, one is made for the next start";
    case FLASH_RUN_GAVE_UP:    return "from /tmp: the last start from the flash did not come up, so this build stays here";
    case FLASH_RUN_BAD:        return "from /tmp: this build's flash copy failed before";
    case FLASH_RUN_EXEC:       return "from /tmp: the flash copy would not run";
    default:                   return "from /tmp";
    }
}

static void fr_path(char *out, size_t n, const char *dir, const char *name) { snprintf(out, n, "%s/%s", dir, name); }

/* a file's size and a sum of its bytes; false if it cannot be read whole */
static bool fr_sum(const char *path, unsigned long *size, uint32_t *sum)
{
    static unsigned char buf[32768];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    uint32_t h = 2166136261u;
    unsigned long total = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
    {
        for (ssize_t i = 0; i < n; i++) h = (h ^ buf[i]) * 16777619u;
        total += (unsigned long)n;
    }
    close(fd);
    if (n < 0) return false;
    *size = total; *sum = h;
    return true;
}
/* "size sum" in a small file, written whole or not at all */
static bool fr_note_is(const char *dir, const char *name, unsigned long size, uint32_t sum)
{
    char p[256], b[48] = "", want[48];
    fr_path(p, sizeof p, dir, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return false;
    b[n] = '\0';
    snprintf(want, sizeof want, "%lu %08lx\n", size, (unsigned long)sum);
    return !strcmp(b, want);
}
static bool fr_note_write(const char *dir, const char *name, unsigned long size, uint32_t sum)
{
    char p[256], t[260], b[48];
    fr_path(p, sizeof p, dir, name);
    snprintf(t, sizeof t, "%s.new", p);
    int n = snprintf(b, sizeof b, "%lu %08lx\n", size, (unsigned long)sum);
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, b, (size_t)n) == n;
    if (fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (!ok || rename(t, p)) { unlink(t); return false; }
    return true;
}
static void fr_forget(const char *dir)
{
    char p[256];
    fr_path(p, sizeof p, dir, FLASH_RUN_FILE); unlink(p);
    fr_path(p, sizeof p, dir, FLASH_RUN_FILE ".new"); unlink(p);
    fr_path(p, sizeof p, dir, "sum"); unlink(p);
}

/* At the very start, before anything is opened. Returns only when the
 * player is to carry on where it is. */
void flash_run_boot(const char *src, const char *dir)
{
    char dst[256], mark[256];
    fr_path(dst, sizeof dst, dir, FLASH_RUN_FILE);
    fr_path(mark, sizeof mark, dir, "trying");
    if (getenv(FR_ENV)) { fr_state = FLASH_RUN_FLASH; return; }
    unsigned long size = 0;
    uint32_t sum = 0;
    fr_state = FLASH_RUN_OFF;
    if (!fr_sum(src, &size, &sum) || !size) return;
    if (!access(mark, F_OK))
    {
        /* the start before this one went to the flash copy and never said
         * its screen was up */
        fr_forget(dir);
        fr_note_write(dir, "bad", size, sum);
        unlink(mark);
        fr_state = FLASH_RUN_GAVE_UP;
        return;
    }
    if (fr_note_is(dir, "bad", size, sum)) { fr_state = FLASH_RUN_BAD; return; }
    struct stat st;
    if (!fr_note_is(dir, "sum", size, sum) || stat(dst, &st) || (unsigned long)st.st_size != size)
    {
        fr_state = FLASH_RUN_NO_COPY;
        return;
    }
    int fd = open(mark, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fr_state = FLASH_RUN_NO_COPY; return; }
    fsync(fd);
    close(fd);
    setenv(FR_ENV, "1", 1);
    char *argv[] = { dst, NULL };
    chmod(dst, 0755);
    execv(dst, argv);
    /* still here: it would not run. Never again for this build. */
    unsetenv(FR_ENV);
    unlink(mark);
    fr_forget(dir);
    fr_note_write(dir, "bad", size, sum);
    fr_state = FLASH_RUN_EXEC;
}

/* the screen is up: the start from the flash worked */
void flash_run_ok(const char *src, const char *dir)
{
    char mark[256];
    if (fr_state != FLASH_RUN_FLASH) return;
    fr_path(mark, sizeof mark, dir, "trying");
    unlink(mark);
    unlink(src);                         /* the copy in RAM: nothing runs from it now */
}

/* Make the flash copy of the build that is running from src: for the next
 * start. Slow (the flash is written through, a few seconds), so on a thread
 * of its own. 0: made, or there already; otherwise why not. */
int flash_run_copy(const char *src, const char *dir)
{
    char dst[256], tmp[260];
    unsigned long size = 0, back = 0;
    uint32_t sum = 0, sum_back = 0;
    if (fr_state != FLASH_RUN_NO_COPY) return 1;
    if (!fr_sum(src, &size, &sum) || !size) return 2;
    mkdir(dir, 0755);
    fr_path(dst, sizeof dst, dir, FLASH_RUN_FILE);
    snprintf(tmp, sizeof tmp, "%s.new", dst);
    fr_forget(dir);                      /* another build's: its room is ours */
    struct statvfs v;
    if (statvfs(dir, &v) || (v.f_flag & (ST_RDONLY | ST_NOEXEC))) return 3;
    if ((unsigned long long)v.f_bavail * v.f_frsize < (unsigned long long)size + FR_KEEP_KB * 1024ull) return 4;
    int in = open(src, O_RDONLY), out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    bool ok = in >= 0 && out >= 0;
    static unsigned char buf[32768];
    ssize_t n = 0;
    while (ok && (n = read(in, buf, sizeof buf)) > 0)
    {
        ok = write(out, buf, (size_t)n) == n;
        usleep(2000);                    /* the music and the screen first */
    }
    if (n < 0) ok = false;
    if (in >= 0) close(in);
    if (out >= 0) { if (fsync(out)) ok = false; if (close(out)) ok = false; }
    /* read back from the flash and compared: only a whole, true copy is kept */
    if (ok) ok = fr_sum(tmp, &back, &sum_back) && back == size && sum_back == sum;
    if (!ok || rename(tmp, dst)) { unlink(tmp); return 5; }
    if (!fr_note_write(dir, "sum", size, sum)) { fr_forget(dir); return 6; }
    return 0;
}
