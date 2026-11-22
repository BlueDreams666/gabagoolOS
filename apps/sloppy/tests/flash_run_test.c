/* cc -I firmware/target/hosted/hiby apps/sloppy/tests/flash_run_test.c -o /tmp/flash_run_test
 * The player started from the R1's flash (firmware/target/hosted/hiby/flash-run.c), run in a
 * folder: a start is never held up or lost, only a whole and true copy is ever run, and a start
 * from the flash that does not come up is not tried again for that build. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static jmp_buf ran;
static char ran_path[256];
static int exec_fails;
static int fake_execv(const char *path, char *const argv[])
{
    (void)argv;
    if (exec_fails) return -1;
    snprintf(ran_path, sizeof ran_path, "%s", path);
    longjmp(ran, 1);                     /* the real one does not come back */
}
static unsigned long long fake_free = ~0ull;
static int fake_statvfs(const char *path, struct statvfs *v)
{
    int r = statvfs(path, v);
    if (!r && fake_free != ~0ull) { v->f_frsize = 1024; v->f_bavail = (fsblkcnt_t)(fake_free / 1024); }
    return r;
}
#define execv fake_execv
#define statvfs(p, v) fake_statvfs(p, v)
#include "flash-run.c"
#undef statvfs
#undef execv

static char root[200], src[256], dir[256], dst[300], mark[300];
static void put(const char *path, const char *fill, size_t n)
{
    FILE *f = fopen(path, "wb"); assert(f);
    for (size_t i = 0; i < n; i++) fputc(fill[i % strlen(fill)] + (int)(i / 4096 % 7), f);
    fclose(f);
}
static long size_of(const char *path) { struct stat st; return stat(path, &st) ? -1 : (long)st.st_size; }
static bool same(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb"); if (!x || !y) { if (x) fclose(x); if (y) fclose(y); return false; }
    int c, d; do { c = fgetc(x); d = fgetc(y); } while (c == d && c != EOF);
    fclose(x); fclose(y); return c == d;
}
/* one start of the player from /tmp: true when it went on to run the flash copy */
static bool start(void)
{
    unsetenv(FR_ENV);
    ran_path[0] = 0;
    if (setjmp(ran)) return true;
    flash_run_boot(src, dir);
    return false;
}
/* the flash copy's own start, and its screen coming up or not */
static void flash_start(bool screen_up)
{
    setenv(FR_ENV, "1", 1);
    flash_run_boot(src, dir);
    assert(flash_run_state() == FLASH_RUN_FLASH);
    if (screen_up) flash_run_ok(src, dir);
}

int main(void)
{
    snprintf(root, sizeof root, "%s/flash-run-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    mkdir(root, 0755);
    snprintf(src, sizeof src, "%s/rockbox.r1", root);
    snprintf(dir, sizeof dir, "%s/flash", root);
    snprintf(dst, sizeof dst, "%s/" FLASH_RUN_FILE, dir);
    snprintf(mark, sizeof mark, "%s/trying", dir);
    const size_t big = 300000;

    /* no player in /tmp at all: nothing happens */
    assert(!start() && flash_run_state() == FLASH_RUN_OFF && flash_run_copy(src, dir) == 1);

    /* a new build's first start: from /tmp as ever, and the copy is made for the next one */
    put(src, "build-one", big);
    assert(!start() && flash_run_state() == FLASH_RUN_NO_COPY && access(dst, F_OK));
    assert(flash_run_copy(src, dir) == 0 && same(src, dst) && access(mark, F_OK));
    /* its second start runs the copy, marked as being tried */
    assert(start() && !strcmp(ran_path, dst) && !access(mark, F_OK) && !access(src, F_OK));
    /* the copy comes up: the mark goes, and so does the one in RAM */
    flash_start(true);
    assert(access(mark, F_OK) && access(src, F_OK) && flash_run_copy(src, dir) == 1);
    /* the next start (the boot menu has copied it to /tmp again) runs it again */
    put(src, "build-one", big);
    assert(start() && !access(mark, F_OK));
    /* this time its screen never comes up (a crash, a bad page): the start after that stays in
     * /tmp, the copy is gone and that build is not tried from the flash again */
    flash_start(false);
    assert(!start() && flash_run_state() == FLASH_RUN_GAVE_UP && access(dst, F_OK) && access(mark, F_OK) && !access(src, F_OK));
    assert(flash_run_copy(src, dir) == 1);
    assert(!start() && flash_run_state() == FLASH_RUN_BAD && flash_run_copy(src, dir) == 1 && access(dst, F_OK));

    /* another build is a fresh try */
    put(src, "build-two", big + 777);
    assert(!start() && flash_run_state() == FLASH_RUN_NO_COPY && flash_run_copy(src, dir) == 0 && same(src, dst));
    assert(start());
    flash_start(true);
    /* a copy that is not this build's (an update put a new player in /tmp) is not run: it is replaced */
    put(src, "build-three", big);
    assert(!start() && flash_run_state() == FLASH_RUN_NO_COPY && !same(src, dst));
    assert(flash_run_copy(src, dir) == 0 && same(src, dst));
    /* a copy cut short on the flash (its size is wrong) is not run either */
    assert(truncate(dst, 1000) == 0);
    assert(!start() && flash_run_state() == FLASH_RUN_NO_COPY && flash_run_copy(src, dir) == 0 && same(src, dst));

    /* the flash copy will not run at all: carry on in /tmp, and never again for this build */
    exec_fails = 1;
    assert(!start() && flash_run_state() == FLASH_RUN_EXEC && access(mark, F_OK) && access(dst, F_OK) && !access(src, F_OK) && !getenv(FR_ENV));
    exec_fails = 0;
    assert(!start() && flash_run_state() == FLASH_RUN_BAD);

    /* too little room on the flash: nothing is written, the stock player's space is left alone */
    put(src, "build-four", big);
    assert(!start() && flash_run_state() == FLASH_RUN_NO_COPY);
    fake_free = big + 4096;
    assert(flash_run_copy(src, dir) == 4 && access(dst, F_OK) && size_of(dst) < 0);
    fake_free = ~0ull;
    assert(flash_run_copy(src, dir) == 0 && same(src, dst));

    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); if (system(cmd)) {}
    printf("PASS: the player runs from the flash only as a whole, true copy of the build in /tmp; a start that fails is not tried again.\n");
    return 0;
}
