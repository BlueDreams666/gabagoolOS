/* For run-scan-test.sh (macOS, DYLD_INSERT_LIBRARIES): once a commit has
 * opened database_tmp.tcd to read it, every read-only open of the master
 * index fails, as when the card goes away under a commit (a USB hand-over).
 * commit() opens the master for writing first, then each build_index()
 * opens it read-only, which is where the database used to be deleted. */
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static int committing;

static int fail_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        va_list a;
        va_start(a, flags);
        mode = (mode_t)va_arg(a, int);
        va_end(a);
    }
    size_t n = strlen(path);
    if (n >= 16 && !strcmp(path + n - 16, "database_tmp.tcd") && (flags & O_ACCMODE) == O_RDONLY)
        committing = 1;
    if (committing && n >= 16 && !strcmp(path + n - 16, "database_idx.tcd") && (flags & O_ACCMODE) == O_RDONLY)
    {
        errno = EIO;
        return -1;
    }
    return open(path, flags, mode);
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *with, *instead; } interpose_open = { (const void *)fail_open, (const void *)open };
