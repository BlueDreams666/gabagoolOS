/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/_ \ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026
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

#include "config.h"
#include "lang.h"
#include "settings.h"
#include "hiby_bluetooth.h"

#if defined(HIBY_LINUX) && !defined(SIMULATOR)


#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kernel.h"
#include "dir.h"      /* Rockbox mkdir(path): <sys/stat.h> alone gives the 2-argument POSIX one */
#include "audio.h"
#include "action.h"
#include "button.h"
#include "button-devinput.h"
#include "menu.h"
#include "misc.h"
#include "splash.h"
#include "gui/list.h"
#include "pcm-alsa.h"
#include "yesno.h"

/* HiBy hosted build provides dynamic output routing helper in its
 * target-specific PCM implementation. */
int pcm_alsa_switch_playback_device(const char *device);
void pcm_alsa_close_device(const char *device);
void hiby_pcm_set_bt_mac(const char *mac);
const char *hiby_pcm_get_bt_mac(void);
void hiby_pcm_set_bt_rate(unsigned rate);
void hiby_bt_absvol_resync(void);
volatile bool hiby_bt_volume_by_ui;
static bool bt_ctl_run(const char *arg1, const char *arg2, const char *success_str);
static bool bt_get_active_mac(char *mac_out, size_t mac_out_len);
static void bt_get_device_name(const char *mac, char *name_out, size_t name_len);
static bool bt_is_enabled(void);

#define BT_MAX_DEVICES 32
#define BT_NAME_LEN 80
#define BT_LOCAL_PLAYBACK_DEVICE "plughw:0,0"
#define BT_DEVICE_PICK_CANCEL (-1)
#define BT_DEVICE_PICK_SCAN (-2)
#define BT_MAX_CODECS 8
#define BT_CODEC_NAME_LEN 16
#define BOOT_SETTING_FILE ROCKBOX_DIR"/rb_bt_on.txt"
#define BT_SYS_PATH "/sys/class/bluetooth"
#define BT_DEBUG_LOG_FILE "/data/mnt/sd_0/rockbox-bt-debug.log"
#define BT_FALLBACK_LOG_FILE ROCKBOX_DIR "/sloppy/bluetooth.log"

const int BT_REMOTE_INPUT_IDX = 4;


struct bt_device
{
    char mac[18];
    char name[BT_NAME_LEN];
    bool paired;
};

struct bt_device_menu_data
{
    struct bt_device *devices;
    int count;
};

struct bt_strlist_data
{
    char (*items)[BT_CODEC_NAME_LEN];
    int count;
};

static char bt_selected_mac[18];
static char bt_selected_name[BT_NAME_LEN];
static const char *bt_playback_dev = BT_LOCAL_PLAYBACK_DEVICE;
static char bt_bt_playback_dev[2][96];
static unsigned int bt_bt_playback_dev_next = 0;
static char bt_active_codec[16];

static bool bt_wait_for_bluealsa_pcm(const char *mac, int timeout_ticks);
static void bt_set_active_codec(const char *mac);
static bool bt_bluealsa_pcm_ready(const char *mac);
static bool bt_is_connected(const char *mac);
static bool bt_prepare_stack(void);
static void bt_connect_device(const struct bt_device *device);
static void bt_disconnect(void);
static bool is_busy = false;
static int bt_last_err;                 /* HIBY_BT_ERR_*: why the last connect or pairing failed */
static bool bt_pair_session(const char *mac, const char *name);
static void bt_apply_names(void);
static void bt_local_names(struct bt_device *devices, int count);
static void bt_cfg_set_last(const char *mac);
static void bt_cfg_forget(const char *mac);
static const char *bt_source_daemon(void);
static bool bt_source_matches(void);
static bool bt_source_restart(void);
static int btrx_bluealsa(const char *daemon);
static void bt_aap_maybe_start(const char *mac);
static bool btrx_daemon_said(off_t at, const char *what);
static void btrx_clean(const char *in, char *out, size_t size);
static pid_t btrx_spawn(const char *cmd, int *to_child, int *from_child);
static void btrx_end_child(pid_t *pid, int wait_ms);
static __thread bool bt_off_kernel;     /* a GabagoolOS worker: never enters the Rockbox kernel */
/* A device the user just disconnected is not routed to again on its own
 * for a while, even if it links straight back. */
static char bt_dropped_mac[18];
static long bt_dropped_at;
static bool bt_embedded_ui;
static FILE *bt_embedded_scan;

void hiby_debug_log(const char *format, ...)
{
    char line[512];
    va_list ap;
    int fd;
    int len;

    va_start(ap, format);
    len = vsnprintf(line, sizeof(line) - 1, format, ap);
    va_end(ap);

    if (len < 0)
        return;
    if (len >= (int)sizeof(line) - 1)
        len = sizeof(line) - 2;
    line[len++] = '\n';

    const char *path = BT_DEBUG_LOG_FILE;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
    {
        mkdir(ROCKBOX_DIR "/sloppy");
        path = BT_FALLBACK_LOG_FILE;
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0) return;
    }
    /* The log is always on now: start it over rather than let it fill
     * the card. */
    if (lseek(fd, 0, SEEK_END) > 256 * 1024)
    {
        close(fd);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) return;
    }

    write(fd, line, len);
    /* Synced on the Bluetooth workers only: this is also called on
     * Rockbox's own threads (the PCM's pause from the audio thread, the
     * classic menus), which all share one OS thread, and a card fsync
     * there stalls the music and the screen with it. */
    if (bt_off_kernel)
        fsync(fd);
    close(fd);
}

/* popen() and system() fork, and fork has to reserve a copy of the whole
 * Rockbox process. On the R1, with playback buffers and the library loaded,
 * that fails with ENOMEM, so no helper could start: bt_resume returned -1,
 * bluetoothctl never ran and power on failed ("Bluetooth radio did not
 * start") until memory happened to free up. vfork() borrows our memory
 * until the child execs. The pipes are close-on-exec, so the child makes no
 * call but dup2() and exec, and other children never inherit them. */
static pid_t bt_child_pid[256];         /* by the parent's pipe fd */

/* Rockbox threads on the R1 share one OS thread, and some of these helpers
 * run on them (SYS_BT_PLUGGED routing in misc.c, the classic menus). A
 * bluealsa-cli or bluetoothctl that never answers - bluetoothd is busy for
 * a while after boot, while bt_init reconnects headphones - froze the
 * whole player until a reboot. Every child now has a deadline; past it,
 * its process group is killed. */
#define BT_READ_TIMEOUT_MS   5000
#define BT_SYSTEM_TIMEOUT_MS 20000

static long bt_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int bt_wait_child_for(pid_t pid, int timeout_ms)
{
    int status;
    long deadline = bt_ms() + timeout_ms;
    for (;;)
    {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return status;
        if (r < 0 && errno != EINTR) return -1;
        if (bt_ms() >= deadline) break;
        usleep(20000);
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
    hiby_debug_log("spawn: child %d killed after %d ms", (int)pid, timeout_ms);
    return -1;
}

static int bt_wait_child(pid_t pid)
{
    return bt_wait_child_for(pid, BT_SYSTEM_TIMEOUT_MS);
}

/* Reading: the whole reply is collected here within BT_READ_TIMEOUT_MS and
 * handed back already complete, so callers' fgets() loops cannot block. */
static struct bt_reply { FILE *fp; int status; } bt_replies[4];
static pthread_mutex_t bt_reply_lock = PTHREAD_MUTEX_INITIALIZER;

static FILE *bt_popen(const char *cmd, const char *mode)
{
    int fds[2];
    bool reading = mode[0] == 'r';
    if (pipe2(fds, O_CLOEXEC) != 0)
    {
        hiby_debug_log("spawn: pipe for '%s': %s", cmd, strerror(errno));
        return NULL;
    }
    int ours = reading ? fds[0] : fds[1], theirs = reading ? fds[1] : fds[0];
    pid_t pid = ours < 256 ? vfork() : -1;
    if (pid == 0)
    {
        setpgid(0, 0);
        dup2(theirs, reading ? STDOUT_FILENO : STDIN_FILENO);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int err = errno;
    close(theirs);
    if (reading && pid > 0)
    {
        struct bt_reply *r = NULL;
        pthread_mutex_lock(&bt_reply_lock);
        for (int i = 0; i < 4 && !r; i++)
            if (!bt_replies[i].fp) { r = &bt_replies[i]; r->fp = (FILE *)r; }   /* claimed */
        pthread_mutex_unlock(&bt_reply_lock);
        size_t cap = 4096, len = 0;
        char *buf = r ? malloc(cap) : NULL;
        long deadline = bt_ms() + BT_READ_TIMEOUT_MS;
        bool timed_out = false;
        while (buf)
        {
            int left = (int)(deadline - bt_ms());
            if (left <= 0) { timed_out = true; break; }
            struct pollfd pfd = { ours, POLLIN | POLLHUP, 0 };
            int ready = poll(&pfd, 1, left);
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) { timed_out = ready == 0; break; }
            if (len + 1024 > cap)
            {
                char *more = cap < 32 * 1024 ? realloc(buf, cap * 2) : NULL;
                if (!more) break;           /* enough: drop the rest */
                buf = more; cap *= 2;
            }
            ssize_t n = read(ours, buf + len, cap - len - 1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            len += (size_t)n;
        }
        close(ours);
        int status = bt_wait_child_for(pid, timed_out ? 0 : BT_READ_TIMEOUT_MS);
        if (timed_out) hiby_debug_log("spawn: '%s' gave no answer in %d ms", cmd, BT_READ_TIMEOUT_MS);
        /* hand the reply back through a fresh pipe (fmemopen needs glibc
         * 2.22, newer than the R1's); replies are small, far under a pipe */
        int back[2];
        FILE *fp = NULL;
        if (buf && pipe2(back, O_CLOEXEC) == 0)
        {
            if (write(back[1], buf, len) == (ssize_t)len) fp = fdopen(back[0], "r");
            close(back[1]);
            if (!fp) close(back[0]);
        }
        free(buf);
        if (!fp)
        {
            if (r) r->fp = NULL;
            hiby_debug_log("spawn: '%s': no reply buffer", cmd);
            return NULL;
        }
        r->status = status; r->fp = fp;
        return fp;
    }
    FILE *fp = pid > 0 ? fdopen(ours, reading ? "r" : "w") : NULL;
    if (!fp)
    {
        hiby_debug_log("spawn: '%s': %s", cmd, strerror(pid < 0 ? err : errno));
        close(ours);
        if (pid > 0) bt_wait_child(pid);
        return NULL;
    }
    bt_child_pid[ours] = pid;
    return fp;
}

static int bt_pclose(FILE *fp)
{
    for (int i = 0; i < 4; i++)
        if (bt_replies[i].fp == fp)
        {
            int status = bt_replies[i].status;
            fclose(fp);
            pthread_mutex_lock(&bt_reply_lock);
            bt_replies[i].fp = NULL;
            pthread_mutex_unlock(&bt_reply_lock);
            return status;
        }
    int fd = fileno(fp);
    pid_t pid = fd >= 0 && fd < 256 ? bt_child_pid[fd] : 0;
    if (pid > 0) bt_child_pid[fd] = 0;
    fclose(fp);
    return pid > 0 ? bt_wait_child(pid) : -1;
}

static int bt_system_for(const char *cmd, int timeout_ms)
{
    pid_t pid = vfork();
    if (pid == 0)
    {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
    {
        hiby_debug_log("spawn: '%s': %s", cmd, strerror(errno));
        return -1;
    }
    return bt_wait_child_for(pid, timeout_ms);
}

static int bt_system(const char *cmd)
{
    return bt_system_for(cmd, BT_SYSTEM_TIMEOUT_MS);
}

bool bt_is_suspended_fast(void)
{
    return access(BT_SYS_PATH"/hci0", F_OK) != 0 
    && access(PIVOT_ROOT BOOT_SETTING_FILE, F_OK) != 0;
}

/* sysfs lists controllers, not A2DP peers, so the answer comes from
 * bluealsa-cli. That forks a process, and bt_is_connected_fast() is called
 * from button_tick() on the kernel tick thread, which must never block. A
 * raw pthread (no Rockbox kernel calls) runs the query and caches the
 * answer; the tick reads the cache and asks for a refresh. */
static volatile bool bt_connected_cache;
static unsigned bt_watch_gen;           /* bumped by a route: older answers are stale */
static pthread_mutex_t bt_watch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bt_watch_cond = PTHREAD_COND_INITIALIZER;
/* A route that just took is a connected device. The cache still said what
 * BlueALSA answered before the connect, and the button tick, seeing
 * "routed but not connected", routed straight back to the jack (pausing
 * the music) until the next answer came and routed to it again: every
 * connect bounced once, and a connect's success check landed in the gap. */
static void bt_watch_routed(bool connected)
{
    pthread_mutex_lock(&bt_watch_lock);
    bt_watch_gen++;
    bt_connected_cache = connected;
    pthread_mutex_unlock(&bt_watch_lock);
}
static bool bt_watch_wanted, bt_watch_started;

static void *bt_watch_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&bt_watch_lock);
    for (;;)
    {
        while (!bt_watch_wanted)
            pthread_cond_wait(&bt_watch_cond, &bt_watch_lock);
        bt_watch_wanted = false;
        unsigned gen = bt_watch_gen;
        pthread_mutex_unlock(&bt_watch_lock);
        char mac[18];
        bool connected = bt_get_active_mac(mac, sizeof(mac));
        pthread_mutex_lock(&bt_watch_lock);
        /* a route made while BlueALSA was being asked knows better */
        if (gen == bt_watch_gen)
            bt_connected_cache = connected;
    }
    return NULL;
}

bool bt_is_connected_fast(void)
{
    if (pthread_mutex_trylock(&bt_watch_lock) == 0)
    {
        if (!bt_watch_started)
        {
            pthread_t thread;
            bt_watch_started = pthread_create(&thread, NULL, bt_watch_thread, NULL) == 0;
            if (bt_watch_started)
                pthread_detach(thread);
        }
        bt_watch_wanted = true;
        pthread_cond_signal(&bt_watch_cond);
        pthread_mutex_unlock(&bt_watch_lock);
    }
    return bt_connected_cache;
}

bool bt_is_routed_fast(void)
{
    return hiby_pcm_get_bt_mac() != NULL;
}

static void bt_suspend(void)
{
    bt_system("/usr/bin/bt_suspend");
}

static const char *bt_make_bt_playback_dev(const char *mac, const char *codec)
{
    char *route = bt_bt_playback_dev[bt_bt_playback_dev_next];

    bt_bt_playback_dev_next ^= 1;

    char* codec_str = codec ? ",CODEC=": "";
    codec = codec ? codec : "";
    snprintf(route, sizeof(bt_bt_playback_dev[0]),
             "bluealsa:DEV=%s,PROFILE=a2dp%s%s", mac, codec_str, codec);
    return route;
}

static int bt_simplelist_ok_cancel(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    return action;
}

static int bt_simplelist_ok_cancel_return_action(int action, struct gui_synclist *lists)
{
    if (lists->data)
        *((int*)lists->data) = action;

    if (action == ACTION_STD_CONTEXT)
        return ACTION_STD_OK;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;

    return action;
}

static int bt_devicelist_callback(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    if (action == ACTION_STD_CONTEXT)
    {
        struct bt_device_menu_data* ctx = lists->data;
        if (lists->selected_item <= 0 || lists->selected_item > ctx->count)
            return action;
        struct bt_device* bt_device = &ctx->devices[lists->selected_item - 1];
        if (bt_device->paired && confirm_delete_yesno(bt_device->name, NULL) == YESNO_YES)
        {
            bt_enable();
            if (strcmp(bt_playback_dev, BT_LOCAL_PLAYBACK_DEVICE) != 0 &&
                strcmp(bt_selected_mac, bt_device->mac) == 0)
                bt_disconnect();

            if (bt_ctl_run("remove", bt_device->mac, "has been removed"))
                bt_device->paired = false;
            return ACTION_REDRAW;
        }
    }

    return action;
}

static const char *bt_action_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    const char **items = data;
    if (selected_item < 0 || selected_item >= 3)
    {
        buffer[0] = '\0';
        return buffer;
    }
    snprintf(buffer, buffer_len, "%s", items[selected_item]);
    return buffer;
}

static const char *bt_device_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    struct bt_device_menu_data *ctx = data;
    if (selected_item == 0)
    {
        snprintf(buffer, buffer_len, "%s", (const char *)str(LANG_BT_SCAN_FOR_NEW));
        return buffer;
    }
    selected_item--;
    if (selected_item < 0 || selected_item >= ctx->count)
    {
        buffer[0] = '\0';
        return buffer;
    }

    snprintf(buffer, buffer_len, "%s%s (%s)",
    ctx->devices[selected_item].paired ? " [P]" : "",
        ctx->devices[selected_item].name,
        ctx->devices[selected_item].mac
        );
    return buffer;
}

static const char *bt_strlist_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    struct bt_strlist_data *ctx = data;
    if (selected_item < 0 || selected_item >= ctx->count)
    {
        buffer[0] = '\0';
        return buffer;
    }
    snprintf(buffer, buffer_len, "%s", ctx->items[selected_item]);
    return buffer;
}

static void bt_trim(char *s)
{
    size_t len;

    if (!s)
        return;

    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || isspace((unsigned char)s[len - 1])))
        s[--len] = '\0';
}

static bool bt_has_mac_pattern(const char *p, char sep)
{
    int i;

    for (i = 0; i < 17; i++)
    {
        if ((i % 3) == 2)
        {
            if (p[i] != sep)
                return false;
        }
        else if (!isxdigit((unsigned char)p[i]))
        {
            return false;
        }
    }

    return true;
}

static bool bt_extract_mac_from_line(const char *line, char *mac_out, size_t mac_out_len)
{
    size_t i, len;

    if (!line || mac_out_len < 18)
        return false;

    len = strlen(line);
    if (len < 17)
        return false;

    for (i = 0; i + 17 <= len; i++)
    {
        char sep = line[i + 2];
        if (sep != ':' && sep != '_')
            continue;
        if (!bt_has_mac_pattern(&line[i], sep))
            continue;

        snprintf(mac_out, mac_out_len, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
            toupper((unsigned char)line[i + 0]), toupper((unsigned char)line[i + 1]),
            toupper((unsigned char)line[i + 3]), toupper((unsigned char)line[i + 4]),
            toupper((unsigned char)line[i + 6]), toupper((unsigned char)line[i + 7]),
            toupper((unsigned char)line[i + 9]), toupper((unsigned char)line[i + 10]),
            toupper((unsigned char)line[i + 12]), toupper((unsigned char)line[i + 13]),
            toupper((unsigned char)line[i + 15]), toupper((unsigned char)line[i + 16]));
        return true;
    }

    return false;
}

/* BlueZ names a device it has no name for after its address, written
 * AA:BB:.. or AA-BB-.. depending on the version. */
static bool bt_name_is_address(const char *name)
{
    return !name[0] || (strlen(name) == 17 &&
           (bt_has_mac_pattern(name, ':') || bt_has_mac_pattern(name, '-')));
}

static void bt_mac_to_underscore(const char *mac, char *out, size_t out_len)
{
    size_t i, n = 0;

    for (i = 0; mac[i] != '\0' && n + 1 < out_len; i++)
    {
        out[n++] = (mac[i] == ':') ? '_' : mac[i];
    }
    out[n] = '\0';
}

/* hiby_pcm_get_bt_mac() spells the address as BlueALSA does, AA_BB_..;
 * BlueZ and the device lists use AA:BB:... Comparing the two with strcmp
 * never matched, so a working route was reported as none: every connect
 * ended in "Could not pair or route audio" and the output card said "This
 * player" with headphones playing. */
static bool bt_same_mac(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    for (; *a && *b; a++, b++)
    {
        bool sep = (*a == ':' || *a == '_') && (*b == ':' || *b == '_');
        if (!sep && toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return false;
    }
    return !*a && !*b;
}

static int bt_add_device_unique_ex(struct bt_device *devices, int count, int max_devices,
    const char *mac, const char *name, bool paired)
{
    int i;

    if (!mac || !mac[0] || count >= max_devices)
        return count;

    for (i = 0; i < count; i++)
    {
        if (!strcasecmp(devices[i].mac, mac))
        {
            if (paired)
                devices[i].paired = true;
            if (name && name[0] &&
                (!devices[i].name[0] || !strcasecmp(devices[i].name, devices[i].mac)))
            {
                snprintf(devices[i].name, sizeof(devices[i].name), "%s", name);
            }
            return count;
        }
    }

    snprintf(devices[count].mac, sizeof(devices[count].mac), "%s", mac);
    if (name && name[0])
        snprintf(devices[count].name, sizeof(devices[count].name), "%s", name);
    else
        snprintf(devices[count].name, sizeof(devices[count].name), "%s", mac);
    devices[count].paired = paired;

    return count + 1;
}

static int bt_device_sort_cmp(const void *a, const void *b)
{
    const struct bt_device *da = a;
    const struct bt_device *db = b;

    if (da->paired != db->paired)
        return da->paired ? -1 : 1;

    return strcasecmp(da->name, db->name);
}

/* bluetoothctl sometimes waits indefinitely for a powered controller or a
 * pairing agent. Bound the child and drain its output without a shell. */
/* the last one-shot bluetoothctl's words: why a connect failed */
static char bt_ctl_reply[512];
static bool bt_ctl_run_for(const char *arg1, const char *arg2, const char *success_str, int timeout_ms);
static bool bt_ctl_run(const char *arg1, const char *arg2, const char *success_str)
{
    return bt_ctl_run_for(arg1, arg2, success_str,
                          !strcmp(arg1, "pair") || !strcmp(arg1, "connect") ? 25000 : 8000);
}
static bool bt_ctl_run_for(const char *arg1, const char *arg2, const char *success_str, int timeout_ms)
{
    int fds[2], status = -1;
    pid_t pid;
    char output[512] = "";
    bool matched = success_str == NULL, reaped = false;
    struct timespec started, now;
    /* vfork and close-on-exec pipes, as in bt_popen() */
    if (pipe2(fds, O_CLOEXEC) != 0)
    {
        hiby_debug_log("bluetoothctl %s: pipe: %s", arg1, strerror(errno));
        return false;
    }
    pid = vfork();
    if (pid < 0)
    {
        hiby_debug_log("bluetoothctl %s: %s", arg1, strerror(errno));
        close(fds[0]); close(fds[1]);
        return false;
    }
    if (pid == 0)
    {
        setpgid(0, 0);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        if (arg2) execlp("bluetoothctl", "bluetoothctl", arg1, arg2, (char *)NULL);
        else execlp("bluetoothctl", "bluetoothctl", arg1, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    clock_gettime(CLOCK_MONOTONIC, &started);
    for (;;)
    {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int elapsed = (int)((now.tv_sec - started.tv_sec) * 1000 +
                            (now.tv_nsec - started.tv_nsec) / 1000000);
        int remaining = timeout_ms - elapsed;
        if (remaining <= 0) break;
        struct pollfd pfd = { fds[0], POLLIN | POLLHUP, 0 };
        int ready = poll(&pfd, 1, remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (ready == 0) break;
        char chunk[256];
        ssize_t n = read(fds[0], chunk, sizeof(chunk) - 1);
        if (n <= 0)
        {
            /* its output closed; its exit is waited for within the time
             * left too (a child that hangs on is killed there) */
            status = bt_wait_child_for(pid, remaining);
            reaped = true;
            break;
        }
        chunk[n] = '\0';
        size_t old = strlen(output);
        if (old + (size_t)n >= sizeof(output))
        {
            size_t drop = old + (size_t)n - sizeof(output) + 1;
            memmove(output, output + drop, old - drop + 1);
            old -= drop;
        }
        memcpy(output + old, chunk, (size_t)n + 1);
        if (success_str && strstr(output, success_str)) matched = true;
    }
    close(fds[0]);
    snprintf(bt_ctl_reply, sizeof(bt_ctl_reply), "%s", output);
    if (!reaped)
    {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR);
        status = -1;
    }
    if (status < 0)
    {
        hiby_debug_log("bluetoothctl %s %s: timed out", arg1, arg2 ? arg2 : "");
        snprintf(bt_ctl_reply, sizeof(bt_ctl_reply), "timed out");
        return false;
    }
    hiby_debug_log("bluetoothctl %s %s: exit=%d reply=%d", arg1,
                   arg2 ? arg2 : "", status, matched);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && matched;
}

/* Parse "Device XX:XX:XX:XX:XX:XX Name" lines from a bluetoothctl command. */
static int  bt_parse_ctl_devices(const char *ctl_cmd, struct bt_device *devices,
                                 int count, int max_devices, bool paired)
{
    FILE *fp;
    char line[256];

    fp = bt_popen(ctl_cmd, "r");
    if (!fp)
        return count;

    while (fgets(line, sizeof(line), fp))
    {
        const char *p = line;
        char mac[18] = "";
        char name[BT_NAME_LEN] = "";

        /* Strip leading junk (bluetoothctl can emit color codes / prompts) */
        while (*p && *p != 'D') p++;
        if (strncmp(p, "Device ", 7) != 0)
            continue;

        p += 7;
        if (strlen(p) < 17 || !bt_has_mac_pattern(p, ':'))
            continue;

        memcpy(mac, p, 17);
        mac[17] = '\0';
        p += 17;
        while (*p == ' ') p++;
        snprintf(name, sizeof(name), "%s", p);
        bt_trim(name);

        count = bt_add_device_unique_ex(devices, count, max_devices, mac, name, paired);
    }

    bt_pclose(fp);
    return count;
}

/* Load paired devices instantly via bluetoothctl paired-devices. */
static int bt_load_devices_via_bluetoothctl(struct bt_device *devices, int max_devices)
{
    int count = bt_parse_ctl_devices("bluetoothctl paired-devices 2>/dev/null",
                                     devices, 0, max_devices, true);
    count = bt_parse_ctl_devices("bluetoothctl devices 2>/dev/null",
                             devices, count, max_devices, false);
    /* BlueZ also reports anonymous LE advertisements as "Device <MAC>".
     * Resolve the advertised name, then leave anonymous unpaired devices
     * out of the connect list. */
    int visible = 0;
    for (int i = 0; i < count; i++)
    {
        if (bt_name_is_address(devices[i].name))
        {
            char name[BT_NAME_LEN];
            bt_get_device_name(devices[i].mac, name, sizeof(name));
            if (name[0]) snprintf(devices[i].name, sizeof(devices[i].name), "%s", name);
        }
        if (!devices[i].paired && bt_name_is_address(devices[i].name))
            continue;
        if (visible != i) devices[visible] = devices[i];
        visible++;
    }
    count = visible;
    bt_local_names(devices, count);
    if (count > 1)
        qsort(devices, count, sizeof(devices[0]), bt_device_sort_cmp);
    return count;
}

static int bt_scan_devices(struct bt_device *devices, int count, int max_devices)
{
    (void)count;
    int waited = 0;
    int action;
    FILE *fp;

    fp = bt_popen("bluetoothctl", "w");
    if (!fp)
    {
        hiby_debug_log("scan: could not start bluetoothctl");
        return count;
    }
    hiby_debug_log("scan: started");

    fprintf(fp, "scan on\n");
    fflush(fp);

    const int timeout = 15;
    while (waited < timeout)
    {
        splashf(0, ID2P(LANG_BT_SCANNING_PROGRESS), waited, timeout);
        action = get_action(CONTEXT_STD, HZ);
        if (action != ACTION_NONE)
            break;
        waited++;
    }

    fprintf(fp, "scan off\n");
    fflush(fp);
    fprintf(fp, "exit\n");
    fflush(fp);
    bt_pclose(fp);

    count = bt_load_devices_via_bluetoothctl(devices, max_devices);
    hiby_debug_log("scan: %d devices", count);
    return count;
}

static int bt_choose_device(const char *title, struct bt_device *devices, int count, int selection)
{
    struct bt_device_menu_data data;
    struct simplelist_info info;
    int total_count = count + 1;

    if (total_count <= 0)
    {
        splash(HZ, ID2P(LANG_BT_NO_DEVICES));
        return BT_DEVICE_PICK_CANCEL;
    }

    data.devices = devices;
    data.count = count;

    simplelist_info_init(&info, (char *)title, total_count, &data);
    info.get_name = bt_device_name_cb;
    info.action_callback = bt_devicelist_callback;
    info.selection = selection + 1; /* +1 for the "Scan" entry */
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);
    if (info.selection < 0 || info.selection >= total_count)
        return BT_DEVICE_PICK_CANCEL;

    if (info.selection == 0)
        return BT_DEVICE_PICK_SCAN;

    return info.selection - 1;
}

static void bt_set_selected(const struct bt_device *device)
{
    if (device)
    {
        snprintf(bt_selected_mac, sizeof(bt_selected_mac), "%s", device->mac);
        snprintf(bt_selected_name, sizeof(bt_selected_name), "%s", device->name);
    }
    else
    {
        bt_selected_mac[0] = '\0';
        bt_selected_name[0] = '\0';
        bt_active_codec[0] = '\0';
    }
}

/* SloppyOS runs Bluetooth jobs on a plain pthread so the UI keeps moving.
 * Rockbox threads on the R1 are cooperative (sigaltstack) and share one OS
 * thread, so that worker must never enter the Rockbox kernel: it sleeps
 * with usleep and leaves restarting the audio to the UI thread. */
static volatile bool bt_kick_pending, bt_pause_pending;

void hiby_bt_worker_thread(void)
{
    bt_off_kernel = true;
}

bool hiby_bt_take_audio_kick(void)
{
    bool kick = bt_kick_pending;
    bt_kick_pending = false;
    return kick;
}

bool hiby_bt_take_audio_pause(void)
{
    bool pause = bt_pause_pending;
    bt_pause_pending = false;
    return pause;
}

static void bt_sleep(int ticks)
{
    if (bt_off_kernel) usleep(ticks * (1000000 / HZ));
    else sleep(ticks);
}

static void bt_kick_audio_if_playing(void)
{
    if (bt_off_kernel) { bt_kick_pending = true; return; }
    int status = audio_status();
    if ((status & AUDIO_STATUS_PLAY) && !(status & AUDIO_STATUS_PAUSE))
    {
        audio_pause();
        sleep(HZ / 4);
        audio_resume();
    }
}

void bt_route_to_local(void)
{
    bool from_bt = hiby_pcm_get_bt_mac() != NULL;
    bool jack = headphones_inserted();
    hiby_debug_log("route: local output%s", from_bt && !jack ? ", nothing in the jack: pausing" : "");
    bt_playback_dev = BT_LOCAL_PLAYBACK_DEVICE;
    bt_active_codec[0] = '\0';
    hiby_pcm_set_bt_mac(NULL);
    pcm_alsa_switch_playback_device(bt_playback_dev);
    /* Off headphones with nothing in the jack, the song would play on to
     * nobody: pause, as unplugging wired headphones does. With wired ones
     * in, carry on through them. */
    if (from_bt && !jack)
    {
        bt_kick_pending = false;
        if (bt_off_kernel) bt_pause_pending = true;
        else if (audio_status() & AUDIO_STATUS_PLAY) audio_pause();
    }
    else
        bt_kick_audio_if_playing();
}

static bool bt_route_to_bluetooth(const char *mac, const char* codec)
{
    int rc;

    if (!mac || !mac[0])
        return false;

    hiby_debug_log("route: preparing A2DP %s", mac);

    bt_playback_dev = bt_make_bt_playback_dev(mac, codec);

    if (!bt_wait_for_bluealsa_pcm(mac, HZ * 6))
    {
        hiby_debug_log("route: no BlueALSA A2DP PCM for %s", mac);
        bt_route_to_local();
        return false;
    }

    if (!codec)
        bt_set_active_codec(mac);
    else
    {
        //bt_set_active_codec hangs if called after codec switching. So just believe...
        strcpy(bt_active_codec, codec);
    }

    /* Codec labels are informational. Older bluealsa-cli versions may not
     * print one even when the A2DP PCM is ready and can carry audio. */
    rc = pcm_alsa_switch_playback_device(bt_playback_dev);
    if (rc == 0)
    {
        hiby_debug_log("route: ALSA output ready for %s", mac);
        hiby_pcm_set_bt_mac(mac);
        bt_cfg_set_last(mac);           /* the headphones to come back to */
        bt_aap_maybe_start(mac);        /* AirPods: battery and noise control */
        bt_watch_routed(true);
        bt_pause_pending = false;
        bt_kick_audio_if_playing();
        /* SloppyOS gives Bluetooth its own level and sends it from the UI
         * thread; sending the jack's level first would blip it loud */
        if (!hiby_bt_volume_by_ui)
            hiby_bt_absvol_resync();
        /* the remote's input device is opened by the button tick, which
         * also waits for it: BlueZ adds it a moment after the audio */
        return true;
    }

    hiby_debug_log("route: ALSA switch failed for %s, rc=%d", mac, rc);
    bt_route_to_local();
    return false;
}

static bool bt_is_connected(const char *mac)
{
    if (!mac || !*mac)
        return false;

    return bt_bluealsa_pcm_ready(mac);
}

static bool bt_bluealsa_pcm_ready(const char *mac)
{
    char line[256];
    char mac_u[18];
    FILE *fp;

    if (!mac || !*mac)
        return false;

    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    fp = bt_popen("bluealsa-cli list-pcms 2>/dev/null", "r");
    if (!fp)
        return false;

    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, mac_u) && strstr(line, "/a2dpsrc/sink"))
        {
            bt_pclose(fp);
            return true;
        }
    }

    bt_pclose(fp);
    return false;
}

static void bt_get_device_name(const char *mac, char *name_out, size_t name_len)
{
    char cmd[128];
    char line[256];
    char actual_name[BT_NAME_LEN] = "";
    char alias[BT_NAME_LEN] = "";
    FILE *fp;

    if (!mac || !name_out || !name_len)
        return;

    name_out[0] = '\0';
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", mac);
    fp = bt_popen(cmd, "r");
    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp))
    {
        char *p = line;

        while (*p == ' ' || *p == '\t')
            p++;

        bool is_alias = strncmp(p, "Alias:", 6) == 0;
        if (!is_alias && strncmp(p, "Name:", 5) != 0) continue;
        p += is_alias ? 6 : 5;
        while (*p == ' ' || *p == '\t')
            p++;

        bt_trim(p);
        if (is_alias) snprintf(alias, sizeof(alias), "%s", p);
        else snprintf(actual_name, sizeof(actual_name), "%s", p);
    }

    bt_pclose(fp);
    /* An unmodified Alias often equals the MAC address. */
    const char *best = !bt_name_is_address(alias) ? alias : actual_name;
    if (!bt_name_is_address(best)) snprintf(name_out, name_len, "%s", best);
}

static bool bt_get_active_mac(char *mac_out, size_t mac_out_len)
{
    FILE *fp;
    char line[256];

    if (!mac_out || mac_out_len < 18)
        return false;

    mac_out[0] = '\0';
    fp = bt_popen("bluealsa-cli list-pcms 2>/dev/null", "r");
    if (fp)
    {
        while (fgets(line, sizeof(line), fp))
        {
            if (!strstr(line, "/a2dpsrc/sink"))
                continue;
            if (bt_extract_mac_from_line(line, mac_out, mac_out_len))
            {
                bt_pclose(fp);
                return true;
            }
        }
        bt_pclose(fp);
    }

    return false;
}

static bool bt_wait_for_bluealsa_pcm(const char *mac, int timeout_ticks)
{
    int ticks = 0;

    if (timeout_ticks < HZ / 2)
        timeout_ticks = HZ / 2;

    while (ticks < timeout_ticks)
    {
        if (bt_bluealsa_pcm_ready(mac))
            return true;

        bt_sleep(HZ / 5);
        ticks += HZ / 5;
    }

    return false;
}

static bool bt_try_set_codec(const char *pcm_path, const char *codec)
{
    char cmd[256];
    int rc;

    snprintf(cmd, sizeof(cmd),
             "bluealsa-cli codec '%s' %s >/dev/null 2>&1",
             pcm_path, codec);
    rc = bt_system(cmd);
    return (rc == 0);
}

static void bt_build_pcm_path(const char *mac, char *path, size_t path_len)
{
    char mac_u[18];
    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    snprintf(path, path_len, "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", mac_u);
}

void bt_bluealsa_change_volume(int l, int r, const char* mac)
{
    if (!mac || !mac[0])
        return;

    //With hardware volume, smaller value is used as volume, so keep that in mind
    char pcm_path[96];
    bt_build_pcm_path(mac, pcm_path, sizeof(pcm_path));
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "bluealsa-cli volume '%s' %d %d >/dev/null 2>&1", pcm_path, l, r);
    bt_system(cmd);
}

static void bt_set_active_codec(const char *mac)
{
    char pcm_path[96];
    char cmd[256];
    char line[256];
    FILE *fp;

    if (!mac || !*mac)
        return;

    bt_active_codec[0] = '\0';
    int rate = 0;
    char delay[24] = "";
    bt_build_pcm_path(mac, pcm_path, sizeof(pcm_path));
    //snprintf(cmd, sizeof(cmd), "bluealsa-cli codec '%s' 2>/dev/null", pcm_path);
    snprintf(cmd, sizeof(cmd), "bluealsa-cli info '%s' 2>/dev/null", pcm_path);
    fp = bt_popen(cmd, "r");
    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp))
    {
        if (strncmp(line, "Selected codec:", 15) == 0)
        {
            const char *p = line + 15;
            while (*p == ' ' || *p == '\t') p++;
            snprintf(bt_active_codec, sizeof(bt_active_codec), "%s", p);
            bt_trim(bt_active_codec);
        }
        else if (strncmp(line, "Sampling:", 9) == 0)
            rate = atoi(line + 9);
        else if (strncmp(line, "Delay:", 6) == 0)
        {
            snprintf(delay, sizeof(delay), "%s", line + 6);
            bt_trim(delay);
        }
    }

    bt_pclose(fp);
    /* the link's rate keeps the mixer from reopening between tracks */
    hiby_pcm_set_bt_rate((unsigned)rate);
    hiby_debug_log("route: %s at %d Hz, BlueALSA delay %s", bt_active_codec[0] ? bt_active_codec : "?",
                   rate, delay[0] ? delay : "?");
}

bool bt_disable(void)
{
    return bt_ctl_run("power", "off", "power off succeeded");
}

/* The stock bt_init brings the controller up in the background after boot.
 * '[b]t_init' so pgrep does not match the sh -c running it, whose command
 * line holds the pattern too: 'bt_init' always matched, so this was always
 * "still running". */
static bool bt_init_running(void)
{
    return bt_system("pgrep -f '[b]t_init' > /dev/null 2>&1") == 0;
}

void wait_for_bt_init(void)
{
    static bool initialized = false;
    if (initialized)
        return;

    long deadline = current_tick + 10 * HZ;

    while (bt_init_running())
    {
        if (TIME_AFTER(current_tick, deadline))
        {
            /* Don't make every later Bluetooth action wait another 10 s. */
            hiby_debug_log("init: bt_init still running after 10 s, going on");
            break;
        }
        splash(HZ/4, ID2P(LANG_BT_INITIALIZING));
    }
    initialized = true;
}

bool bt_enable(void)
{
    return bt_is_enabled() || bt_ctl_run("power", "on", "power on succeeded") ||
           bt_is_enabled();
}

static void bt_remember_boot_on(void)
{
    int fd = open(BOOT_SETTING_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        if (fsync(fd)) hiby_debug_log("stack: could not sync Bluetooth boot setting");
        close(fd);
    }
    else hiby_debug_log("stack: could not save Bluetooth boot setting");
}

/* An R1 with the AIC8800D40 chip on HiBy 2.0's system: its Bluetooth is
 * powered on demand through HiBy's "power gate". bt_init (at boot) starts
 * bluetoothd, the agent and bluealsa with the radio off; bt_enable_aic.sh
 * powers the chip, attaches its UART with hciattach and powers the adapter;
 * bt_disable (the script, which 2.0's bt_suspend runs too) undoes it.
 * bt_resume does nothing there, so the Broadcom path cannot start it. */
static bool bt_aic_gate(void)
{
    return access("/run/aic_combo_power_gate", F_OK) == 0 ||
           access("/run/aic_bt_power_gate", F_OK) == 0;
}

static bool bt_aic_start(void)
{
    if (access("/usr/bin/bt_enable_aic.sh", X_OK) != 0)
    {
        hiby_debug_log("stack: AIC gate but no bt_enable_aic.sh");
        return false;
    }
    hiby_debug_log("stack: powering the AIC8800 controller");
    /* it may wait 10 s for the boot-time driver, then for the firmware,
     * the UART and BlueZ: more than bt_system's 20 s, and a kill midway
     * would skip its own clean-up */
    int rc = bt_system_for("/usr/bin/bt_enable_aic.sh > /tmp/sloppy-bt-aic.log 2>&1", 40000);
    if (rc != 0)
        hiby_debug_log("stack: bt_enable_aic.sh returned %d (see /tmp/sloppy-bt-aic.log)", rc);
    /* bt_init starts bluealsa at boot; something may have stopped it since */
    if (bt_system("pgrep -x bluealsa > /dev/null 2>&1") != 0)
    {
        hiby_debug_log("stack: starting bluealsa");
        char cmd[160];
        snprintf(cmd, sizeof(cmd), "/usr/bin/%s > /dev/null 2>&1 &", bt_source_daemon());
        bt_system(cmd);
    }
    return rc == 0;
}

/* once per power-on: the names kept here back to BlueZ, and BlueALSA
 * with the SBC quality asked for (while nothing plays through it) */
static bool bt_ready_done;
static void bt_stack_ready(void)
{
    if (bt_ready_done) return;
    bt_ready_done = true;
    bt_apply_names();
    char active[18];
    if (!bt_source_matches() && !hiby_pcm_get_bt_mac() && !bt_get_active_mac(active, sizeof(active)))
        bt_source_restart();
}

static bool bt_prepare_stack(void)
{
    /* Powering on while bt_init still loads the chip's firmware fails, and
     * bt_resume over it races the init. Let it finish; no splash, as this
     * runs on SloppyOS's worker thread too. */
    for (int i = 0; i < 30 && bt_init_running(); i++)
    {
        if (i == 0) hiby_debug_log("stack: waiting for bt_init");
        bt_sleep(HZ / 2);
    }
    if (bt_enable())
    {
        bt_remember_boot_on();
        hiby_debug_log("stack: controller powered on");
        bt_stack_ready();
        return true;
    }

    if (!bt_embedded_ui) splash(0, ID2P(LANG_BT_SUSPENDED_RESUMING));
    if (bt_aic_gate())
        bt_aic_start();   /* synchronous: firmware download, UART attach, adapter on */
    else
    {
        hiby_debug_log("stack: resuming controller");
        int resume_status = bt_system("/usr/bin/bt_resume");
        if (resume_status != 0)
            hiby_debug_log("stack: bt_resume returned %d; checking controller", resume_status);
    }
    /* bt_resume starts the controller asynchronously. Wait for it instead
     * of treating the first not-ready reply as a permanent failure. */
    for (int attempt = 0; attempt < 30; attempt++)   /* about 15 s: firmware load */
    {
        if (bt_enable())
        {
            hiby_debug_log("stack: controller ready after %d retries", attempt);
            bt_remember_boot_on();
            bt_stack_ready();
            if (!bt_embedded_ui) splash(0, ID2P(LANG_BT_DONE));
            return true;
        }
        bt_sleep(HZ / 2);
    }
    hiby_debug_log("stack: controller not ready after resume");
    return false;
}

static void bt_show_status(void);

static void bt_show_devices(void)
{
    wait_for_bt_init();

    static struct bt_device devices[BT_MAX_DEVICES];
    int count;

    if (!bt_enable())
    {
        const char *lines[] = {(const char *)str(LANG_BT_IS_SUSPENDED),
                              (const char *)str(LANG_BT_ENABLE_IT)};
        const struct text_message message = {lines, 2};

        if (gui_syncyesno_run(&message, NULL, NULL) != YESNO_YES)
            return;

        if (!bt_prepare_stack())
        {
            //try to suspend again to avoid leaving it in a weird state
            bt_suspend();
            splash(HZ * 2, ID2P(LANG_BT_UNAVAILABLE));
            return;
        }
    }

    count = bt_load_devices_via_bluetoothctl(devices, BT_MAX_DEVICES);
    int idx = -1;

    while (1)
    {
        idx = bt_choose_device(str(LANG_BT_DEVICES), devices, count, idx);
        if (idx == BT_DEVICE_PICK_SCAN)
        {
            count = bt_scan_devices(devices, count, BT_MAX_DEVICES);
            if (count <= 0)
                splash(HZ, ID2P(LANG_BT_NO_DEVICES_FOUND));
            continue;
        }

        if (idx >= 0 && idx < count)
        {
            bt_connect_device(&devices[idx]);
            if (strcmp(bt_playback_dev, BT_LOCAL_PLAYBACK_DEVICE) != 0)
                bt_show_status();
            else
                continue;
        }
        return;
    }
}

static void bt_connect_device(const struct bt_device *device)
{
    const char *mac;

    if (!device || !device->mac[0] || is_busy)
        return;

    mac = device->mac;
    hiby_debug_log("connect: %s paired=%d", mac, device->paired);
    is_busy = true;

    // disallow for now multiple connections.
    if (strcmp(bt_playback_dev, BT_LOCAL_PLAYBACK_DEVICE) != 0 &&
        bt_selected_mac[0] && strcmp(bt_selected_mac, mac) != 0)
    {
        bt_disconnect();
        is_busy = true;
    }

    if (!bt_embedded_ui) splash(0, ID2P(LANG_BT_CONNECTING));

    bt_last_err = HIBY_BT_ERR_NONE;
    if (!bt_prepare_stack())
    {
        bt_last_err = HIBY_BT_ERR_RADIO;
        if (!bt_embedded_ui) splash(HZ * 2, ID2P(LANG_BT_UNAVAILABLE));
        is_busy = false;
        return;
    }

    if (!device->paired)
    {
        /* an agent of our own for this pairing: passkeys and PINs go to
         * the screen (GabagoolOS), or are accepted (the classic menus) */
        if (!bt_pair_session(mac, device->name))
        {
            hiby_debug_log("connect: pairing failed for %s (error %d)", mac, bt_last_err);
            if (!bt_embedded_ui) splash(HZ * 2, ID2P(LANG_BT_PAIR_FAILED));
            is_busy = false;
            return;
        }
        bt_sleep(HZ / 2);               /* earbuds want a moment between pairing and audio */
    }

    /* Headphones just switched on often turn the first page away; one
     * more try a second later, unless the first already used its time. */
    long connect_from = bt_ms();
    bool linked = bt_ctl_run("connect", mac, "Connection successful");
    if (!linked && bt_ms() - connect_from < 10000)
    {
        hiby_debug_log("connect: retrying %s", mac);
        bt_sleep(HZ);
        linked = bt_ctl_run("connect", mac, "Connection successful");
    }
    if (!linked)
    {
        char active_mac[18];
        // Make sure it's not a race with firmware auto-connection
        bt_sleep(HZ/2);
        bt_get_active_mac(active_mac, sizeof(active_mac));
        if (strcmp(active_mac, mac) != 0)
        {
            bt_last_err = HIBY_BT_ERR_NOT_FOUND;
            hiby_debug_log("connect: connection failed for %s: %.100s", mac, bt_ctl_reply);
            if (!bt_embedded_ui) splash(HZ * 2, ID2P(LANG_BT_CONNECT_FAILED));
            is_busy = false;
            return;
        }
    }

    bt_set_selected(device);
    bt_dropped_mac[0] = '\0';          /* asked for again */

    if (bt_route_to_bluetooth(mac, NULL))
    { if (!bt_embedded_ui) splash(HZ, ID2P(LANG_BT_CONNECTED)); }
    else
    {
        bt_last_err = HIBY_BT_ERR_NO_AUDIO;
        if (!bt_embedded_ui) splash(HZ * 2, ID2P(LANG_BT_CONNECTED_NO_ROUTE));
    }

    is_busy = false;
}

static void bt_disconnect(void)
{
    hiby_debug_log("disconnect: routing to local output");
    is_busy = true;
    button_remove_input_device(BT_REMOTE_INPUT_IDX);

    //bt_set_selected(NULL);
    bt_active_codec[0] = 0;
    bt_route_to_local();

    char mac[18];
    char cmd[96];

    if (bt_get_active_mac(mac, sizeof(mac)))
    {
        snprintf(bt_dropped_mac, sizeof(bt_dropped_mac), "%s", mac);
        bt_dropped_at = bt_ms();
        if (!bt_embedded_ui) splash(0, ID2P(LANG_BT_DISCONNECTING));
        snprintf(cmd, sizeof(cmd), "bluetoothctl disconnect %s >/dev/null 2>&1", mac);
        bt_system(cmd);
    }

    is_busy = false;
    if (!bt_embedded_ui) splash(HZ/4, ID2P(LANG_BT_DISCONNECTED));
}

static bool bt_is_enabled(void)
{
    FILE *fp;

    fp = bt_popen("bluetoothctl show 2>/dev/null | grep -q 'Powered: yes'", "r");
    //fp = bt_popen("bt-adapter -i 2>/dev/null | grep 'Powered: 1'", "r");

    if (fp && bt_pclose(fp) == 0)
    {
        return true;
    }
    //splash(0, ID2P(LANG_BT_NOT_ENABLED));
    return false;
}

static int bt_get_available_codecs(const char *mac,
                                   char codecs[][BT_CODEC_NAME_LEN],
                                   int max_codecs)
{
    char pcm_path[96];
    char cmd[256];
    char line[256];
    FILE *fp;
    int count = 0;

    if (!mac || !*mac)
        return 0;

    bt_build_pcm_path(mac, pcm_path, sizeof(pcm_path));
    snprintf(cmd, sizeof(cmd), "bluealsa-cli codec '%s' 2>/dev/null", pcm_path);
    fp = bt_popen(cmd, "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp))
    {
        if (strncmp(line, "Available codecs:", 17) == 0)
        {
            char *p = line + 17;
            while (*p && count < max_codecs)
            {
                int i = 0;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '\0' || *p == '\n' || *p == '\r') break;
                while (*p && *p != ' ' && *p != '\t' &&
                       *p != '\n' && *p != '\r' &&
                       i < BT_CODEC_NAME_LEN - 1)
                    codecs[count][i++] = *p++;
                codecs[count][i] = '\0';
                if (i > 0)
                    count++;
            }
            break;
        }
    }

    bt_pclose(fp);
    return count;
}

static void bt_show_codec_picker(const char *mac)
{
    static char codecs[BT_MAX_CODECS][BT_CODEC_NAME_LEN];
    struct bt_strlist_data data;
    struct simplelist_info info;
    int count;

    count = bt_get_available_codecs(mac, codecs, BT_MAX_CODECS);
    if (count <= 0)
    {
        splash(HZ, ID2P(LANG_BT_NO_CODECS));
        return;
    }

    data.items = codecs;
    data.count = count;

    simplelist_info_init(&info, ID2P(LANG_BT_SELECT_CODEC), count, &data);
    info.get_name = bt_strlist_name_cb;
    info.action_callback = bt_simplelist_ok_cancel;
    info.selection = -1;
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);

    if (info.selection >= 0 && info.selection < count)
    {
        //pcm_alsa_close_device(bt_playback_dev);
        bt_route_to_local();
        char pcm_path[96];
        bt_build_pcm_path(mac, pcm_path, sizeof(pcm_path));
        //This seems to work more reliable with active playback, but setting seems doesn't stick through reboots (?)
        //if (bt_route_to_bluetooth(mac, codecs[info.selection]))

        if (bt_try_set_codec(pcm_path, codecs[info.selection]) && bt_route_to_bluetooth(mac, NULL))
        {
            //bt_set_active_codec(mac);
            splashf(HZ, "%s: %s", ID2P(LANG_BT_CODEC), bt_active_codec );
        }
        else
            splash(HZ, ID2P(LANG_BT_CODEC_CHANGE_FAILED));
    }
}

bool bt_can_autoconnect(void)
{
    return !is_busy && pcm_is_initialized();
}

bool bt_autoconnection_route_to_bluetooth(char* active_mac, bool bt_on)
{
    if (is_busy)
        return false;

    is_busy = true;
    bool bt_connected = bt_on && bt_get_active_mac(active_mac, 18);
    bool active_mac_changed = bt_connected && (strcmp(bt_selected_mac, active_mac) != 0);
    if (bt_connected && !strcasecmp(active_mac, bt_dropped_mac) && bt_ms() - bt_dropped_at < 15000)
    {
        is_busy = false;
        return bt_connected;            /* the user just let it go */
    }
    /* Auto-route to BT if headphone is connected but output is still local (connected by Hiby OS) */
    if (bt_connected && 
        (active_mac_changed || strcmp(bt_playback_dev, BT_LOCAL_PLAYBACK_DEVICE) == 0 ))
    {
       //splash(0, "Bluetooth connection detected.\nRouting audio to Bluetooth...");
        //TODO: Auto connection seems to ignore codec preference
        // Should we do full reconnection?
        if (active_mac_changed)
        {
            bt_get_device_name(active_mac, bt_selected_name, sizeof(bt_selected_name));
            strcpy(bt_selected_mac, active_mac);
        }
        bt_route_to_bluetooth(active_mac, NULL);
        //splash(HZ/5, "Done.");
    }
    is_busy = false;
    return bt_connected;
}

static void bt_show_status(void)
{
    struct simplelist_info info;
    char active_mac[18];
    bool bt_on = false;
    int sel;

    bool suspended = bt_is_suspended_fast();
    if (!suspended)
    {
        wait_for_bt_init();
        bt_on = bt_is_enabled();
    }
    while (1)
    {
        bool bt_connected = bt_autoconnection_route_to_bluetooth(active_mac, bt_on);

        int line_idx = 0;
        int bt_toggle_line;
        int codec_line = -1;
        int device_line = -1;
        int executed_action = -1;

        simplelist_info_init(&info, (char *)str(LANG_BT_STATUS), 0, &executed_action);
        info.action_callback = bt_simplelist_ok_cancel_return_action;
        info.selection = -1;
        simplelist_reset_lines();

        simplelist_addline("%s: %s", (const char *)str(LANG_BT_BLUETOOTH),
                         bt_on ? (const char *)str(LANG_BT_ENABLED) :
                         (suspended ? (const char *)str(LANG_BT_SUSPENDED) : (const char *)str(LANG_BT_DISABLED)));
        bt_toggle_line = line_idx++;

        if (bt_selected_mac[0])
        {
            simplelist_addline("%s: %s", (const char *)str(LANG_BT_DEVICE),
                             bt_selected_name[0] ? bt_selected_name : (const char *)str(LANG_BT_BLUETOOTH));
            device_line = line_idx++;

            if (bt_connected)
            {
                simplelist_addline("%s: %s", (const char *)str(LANG_BT_CODEC),
                       bt_active_codec[0] ? bt_active_codec : (const char *)str(LANG_UNKNOWN));
                codec_line = line_idx++;
            }
            simplelist_addline("%s: %s", str(LANG_BT_MAC), bt_selected_mac);
            line_idx++;
            simplelist_addline("%s: %s", (const char *)str(LANG_BT_CONNECTED),
                               bt_connected ? (const char *)str(LANG_SET_BOOL_YES) : (const char *)str(LANG_SET_BOOL_NO));
            line_idx++;

        }
        else
        {
            simplelist_addline("%s: %s", (const char *)str(LANG_BT_DEVICE), (const char *)str(LANG_BT_LOCAL));
            line_idx++;
        }

        simplelist_addline("%s: %s", (const char *)str(LANG_BT_OUTPUT), bt_playback_dev);
        line_idx++;

        info.count = simplelist_get_line_count();
        simplelist_show_list(&info);

        sel = info.selection;
        if (sel < 0)
            break;

        if (sel == bt_toggle_line)
        {
            const char *toggle_items[] = {(const char *)str(LANG_ON),
                                        (const char *)str(LANG_OFF),
                                        (const char *)str(LANG_BT_SUSPEND)};
            struct simplelist_info t_info;
            simplelist_info_init(&t_info, (char *)str(LANG_BT_BLUETOOTH), 3, (void *)toggle_items);
            t_info.get_name = bt_action_name_cb;
            t_info.action_callback = bt_simplelist_ok_cancel;
            t_info.selection = -1;
            simplelist_show_list(&t_info);
            bool suspend = t_info.selection == 2; 

            if (t_info.selection == 0)
            {
                bt_on = bt_prepare_stack();
            }
            else if (t_info.selection > 0)
            {
                button_remove_input_device(BT_REMOTE_INPUT_IDX);
                bt_route_to_local();
                if (suspend)
                {
                    bt_suspend();
                    remove(BOOT_SETTING_FILE);
                }
                else
                {
                    bt_disable();
                }
                
                bt_on = false;
            }
            suspended = !bt_on && suspend;
        }
        else if (sel == codec_line && active_mac[0])
        {
            bt_show_codec_picker(active_mac);
        }
        else if (sel == device_line && bt_on && bt_selected_mac[0])
        {
            if (bt_connected && executed_action == ACTION_STD_CONTEXT)
            {
                bt_disconnect();
                continue;
            }
            struct bt_device device;
            strcpy(device.mac, bt_selected_mac);
            strcpy(device.name, bt_selected_name);
            device.paired = true;
            bt_connect_device(&device);
        }
        /* other lines: just re-show status */
    }
}

/* SloppyOS uses these operations without opening Rockbox lists or splashes. */
/* The answer comes from every source there is, most direct first: audio
 * routed to a device (our own state), then an A2DP sink in BlueALSA, then
 * bluetoothctl. bluetoothctl alone used to decide "on", and it fails or
 * times out while bluetoothd is busy streaming: the page then said
 * Bluetooth was off with headphones playing, and turning it "on" ran
 * bt_resume over a live stack. A live link is never reported as off. */
void hiby_bt_status(struct hiby_bt_status *status)
{
    memset(status, 0, sizeof(*status));
    const char *routed = hiby_pcm_get_bt_mac();
    status->connected = bt_get_active_mac(status->mac, sizeof(status->mac));
    if (!status->connected && routed)
    {
        /* routed but BlueALSA did not answer: trust the route */
        snprintf(status->mac, sizeof(status->mac), "%s", routed);
        for (char *c = status->mac; *c; c++)
            if (*c == '_') *c = ':';
        status->connected = true;
    }
    status->suspended = !status->connected && bt_is_suspended_fast();
    status->enabled = status->connected || (!status->suspended && bt_is_enabled());
    if (!status->connected) return;
    bt_get_device_name(status->mac, status->name, sizeof(status->name));
    {
        char own[HIBY_BT_ALIAS_LEN];
        hiby_bt_local_name(status->mac, own, sizeof(own));
        if (own[0]) snprintf(status->name, sizeof(status->name), "%s", own);
    }
    /* bluetoothctl info often has no name for a device right after it
     * connects (or while another bluetoothctl is busy). Keep the last name
     * known for it, from the device list or an earlier lookup. */
    bool same = !strcasecmp(status->mac, bt_selected_mac);
    if (status->name[0])
    {
        snprintf(bt_selected_mac, sizeof(bt_selected_mac), "%s", status->mac);
        snprintf(bt_selected_name, sizeof(bt_selected_name), "%s", status->name);
    }
    else if (same && !bt_name_is_address(bt_selected_name))
        snprintf(status->name, sizeof(status->name), "%s", bt_selected_name);
    if (!status->name[0]) snprintf(status->name, sizeof(status->name), "%s", status->mac);
    status->routed = bt_same_mac(hiby_pcm_get_bt_mac(), status->mac);
    snprintf(status->codec, sizeof(status->codec), "%s", bt_active_codec);
    /* Diagnostics: what BlueALSA says the headphones' volume is, logged
     * when it changes. The player never reads it back yet; this shows
     * whether the headphones' own buttons are reported here at all. */
    if (status->routed)
    {
        static char last[128];
        char path[96], cmd[160], line[128] = "";
        bt_build_pcm_path(status->mac, path, sizeof(path));
        snprintf(cmd, sizeof(cmd), "bluealsa-cli volume '%s' 2>&1", path);
        FILE *fp = bt_popen(cmd, "r");
        if (fp)
        {
            if (!fgets(line, sizeof(line), fp)) line[0] = '\0';
            bt_pclose(fp);
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, last))
        {
            snprintf(last, sizeof(last), "%s", line);
            hiby_debug_log("volume: BlueALSA reports \"%s\"", line);
        }
    }
}

int hiby_bt_devices(struct hiby_bt_device *devices, int max_devices)
{
    struct bt_device found[BT_MAX_DEVICES];
    if (!devices || max_devices <= 0) return 0;
    if (max_devices > BT_MAX_DEVICES) max_devices = BT_MAX_DEVICES;
    int count = bt_load_devices_via_bluetoothctl(found, max_devices);
    for (int i = 0; i < count; i++)
    {
        snprintf(devices[i].mac, sizeof(devices[i].mac), "%s", found[i].mac);
        snprintf(devices[i].name, sizeof(devices[i].name), "%s", found[i].name);
        devices[i].paired = found[i].paired;
    }
    return count;
}

bool hiby_bt_power(bool on)
{
    bt_embedded_ui = true;
    is_busy = true;
    bool ok;
    char active[18];
    /* already up with a device on it: nothing to start (and bt_resume
     * over a live stack drops the link) */
    if (on && (hiby_pcm_get_bt_mac() || bt_get_active_mac(active, sizeof(active))))
    {
        bt_remember_boot_on();
        bt_stack_ready();
        ok = true;
    }
    else if (on) ok = bt_prepare_stack();
    else
    {
        if (bt_embedded_scan) hiby_bt_scan_finish(NULL, 0);
        button_remove_input_device(BT_REMOTE_INPUT_IDX);
        bt_route_to_local();
        ok = bt_is_suspended_fast() || bt_disable();
        bt_ready_done = false;
        /* off stays off: the boot menu leaves the radio asleep next time */
        if (ok) remove(BOOT_SETTING_FILE);
        /* AIC: power the chip down too, as HiBy's own switch does; on the
         * Broadcom chip "power off" is all the stock player does as well */
        if (ok && bt_aic_gate() && bt_system("/usr/bin/bt_disable > /dev/null 2>&1") != 0)
            hiby_debug_log("stack: AIC bt_disable failed; the adapter is off, the chip may stay powered");
    }
    is_busy = false;
    bt_embedded_ui = false;
    return ok;
}

bool hiby_bt_auto_route(void)
{
    char active[18];
    bt_embedded_ui = true;
    bt_autoconnection_route_to_bluetooth(active, true);
    bt_embedded_ui = false;
    return hiby_pcm_get_bt_mac() != NULL;
}

bool hiby_bt_connect(const struct hiby_bt_device *device)
{
    if (!device || strlen(device->mac) != 17 || !bt_has_mac_pattern(device->mac, ':'))
        return false;
    struct bt_device selected;
    snprintf(selected.mac, sizeof(selected.mac), "%s", device->mac);
    snprintf(selected.name, sizeof(selected.name), "%s", device->name);
    selected.paired = device->paired;
    bt_embedded_ui = true;
    bt_connect_device(&selected);
    bt_embedded_ui = false;
    /* Success is the audio going to it. Asking BlueALSA's PCM list again
     * here said "Could not pair or route audio" over headphones that were
     * playing: the list lags and comes back empty now and then. And a
     * connect that found the headphones' own auto-connect already routing
     * returned at once; give that route a few seconds to land. */
    for (int waited = 0; !bt_same_mac(hiby_pcm_get_bt_mac(), device->mac) && waited < 4000; waited += 200)
    {
        if (!is_busy && waited >= 1000)
            break;                      /* nothing under way: it did fail */
        bt_sleep(HZ / 5);
    }
    return bt_same_mac(hiby_pcm_get_bt_mac(), device->mac);
}

bool hiby_bt_disconnect(void)
{
    bt_embedded_ui = true;
    bt_disconnect();
    bt_embedded_ui = false;
    char active[18];
    return !bt_get_active_mac(active, sizeof(active));
}

bool hiby_bt_forget(const char *mac)
{
    if (!mac || strlen(mac) != 17 || !bt_has_mac_pattern(mac, ':')) return false;
    char active[18];
    if (bt_get_active_mac(active, sizeof(active)) && !strcasecmp(active, mac))
        hiby_bt_disconnect();
    bt_cfg_forget(mac);
    return bt_ctl_run("remove", mac, "has been removed");
}

bool hiby_bt_scan_start(void)
{
    if (bt_embedded_scan) return true;
    if (!hiby_bt_power(true)) return false;
    bt_embedded_scan = bt_popen("bluetoothctl", "w");
    if (!bt_embedded_scan) return false;
    /* audio devices are BR/EDR: earbuds that also advertise over LE (Sony,
     * Fast Pair) were found, and paired, as LE devices with no audio */
    fprintf(bt_embedded_scan, "menu scan\ntransport bredr\nback\nscan on\n");
    fflush(bt_embedded_scan);
    return true;
}

int hiby_bt_scan_finish(struct hiby_bt_device *devices, int max_devices)
{
    if (bt_embedded_scan)
    {
        fprintf(bt_embedded_scan, "scan off\nexit\n");
        fflush(bt_embedded_scan);
        bt_pclose(bt_embedded_scan);
        bt_embedded_scan = NULL;
    }
    return devices ? hiby_bt_devices(devices, max_devices) : 0;
}

int hiby_bt_codecs(const char *mac, char codecs[][HIBY_BT_CODEC_NAME_LEN], int max_codecs)
{
    if (!mac || strlen(mac) != 17 || !bt_has_mac_pattern(mac, ':')) return 0;
    if (max_codecs > BT_MAX_CODECS) max_codecs = BT_MAX_CODECS;
    return max_codecs > 0 ? bt_get_available_codecs(mac, codecs, max_codecs) : 0;
}

bool hiby_bt_codec(const char *mac, const char *codec)
{
    char choices[BT_MAX_CODECS][BT_CODEC_NAME_LEN];
    int count = hiby_bt_codecs(mac, choices, BT_MAX_CODECS);
    bool valid = false;
    for (int i = 0; i < count; i++)
        if (!strcmp(codec, choices[i])) { valid = true; break; }
    if (!valid) return false;
    char pcm_path[96];
    bt_build_pcm_path(mac, pcm_path, sizeof(pcm_path));
    bt_route_to_local();
    return bt_try_set_codec(pcm_path, codec) && bt_route_to_bluetooth(mac, codec);
}

int hiby_bluetooth_menu(void)
{
    const char *action_items[] =
    {
        (const char *)str(LANG_BT_STATUS),
        (const char *)str(LANG_BT_DEVICES),
        (const char *)str(LANG_BT_DISCONNECT),
    };

    int action = -1;

    while (true)
    {
        struct simplelist_info info;

        simplelist_info_init(&info, (char *)str(LANG_BT_BLUETOOTH),
            (int)(sizeof(action_items) / sizeof(action_items[0])),
            (void *)action_items);
        info.get_name = bt_action_name_cb;
        info.action_callback = bt_simplelist_ok_cancel;
        info.selection = -1;
        info.title_icon = Icon_Submenu;

        simplelist_show_list(&info);
        action = info.selection;
        if (action < 0)
            break;

        switch (action)
        {
            case 0:
                bt_show_status();
                break;
            case 1:
                bt_show_devices();
                break;
            case 2:
                bt_disconnect();
                break;
            default:
                break;
        }
    }
    return 0;
}

/* ------------------------------------------------- Receive from phone */
/* The player as Bluetooth headphones for a phone. BlueALSA is restarted as
 * an A2DP sink, as HiBy's own bluealsa_profile does it; a bluetoothctl
 * kept open is the pairing agent (it answers yes) and makes the player
 * visible as GabagoolOS R1; the phone's decoded audio comes out of
 * "bluealsa-cli open" into a ring that SloppyOS's mixer channel drains.
 * One thread runs the whole session and, however it ends, puts back
 * today's source mode: the bluealsa bt_init starts, the old name, the
 * old visibility. Each step is logged here and queued for the field log. */
#define BTRX_ALIAS "GabagoolOS R1"
#define BTRX_SINK "bluealsa -p a2dp-sink --a2dp-volume"
/* Every decoder the stock BlueALSA has (libldacdec, libopenaptx, fdk-aac),
 * offered to the phone; the phone picks among them. Plain BTRX_SINK if
 * this one does not answer. */
#define BTRX_SINK_ALL "bluealsa -p a2dp-sink --a2dp-volume -c SBC -c AAC -c aptX -c aptX-HD -c LDAC"
#define BTRX_SOURCE "bluealsa -p a2dp-source --a2dp-volume"   /* as bt_init */
#define BTRX_DAEMON_LOG "/tmp/sloppy-bluealsa.log"
#define BTRX_HELPER_LOG "/tmp/sloppy-btrx.err"
#define BTRX_RING (1 << 15)              /* frames, 0.68 s at 48 kHz */
#define BTRX_EVENTS 24
#define BTRX_RESTARTS 6                  /* audio readers per phone connection */

static int btrx_wait_ms = 180000;       /* visible this long for a phone */
static pthread_mutex_t btrx_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hiby_btrx_status btrx_st;  /* under btrx_lock */
static char btrx_events[BTRX_EVENTS][128];
static unsigned btrx_ev_head, btrx_ev_tail;
static char btrx_ctl_want[12];
static char btrx_codec_want[HIBY_BT_CODEC_NAME_LEN];
static volatile bool btrx_on, btrx_stop_wanted;

static void btrx_log(const char *format, ...)
{
    char line[128];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    hiby_debug_log("rx: %s", line);
    pthread_mutex_lock(&btrx_lock);
    if (btrx_ev_head - btrx_ev_tail >= BTRX_EVENTS) btrx_ev_tail++;   /* the oldest goes */
    snprintf(btrx_events[btrx_ev_head++ % BTRX_EVENTS], sizeof(btrx_events[0]), "%s", line);
    pthread_mutex_unlock(&btrx_lock);
}

static void btrx_set_state(int state)
{
    pthread_mutex_lock(&btrx_lock);
    btrx_st.state = state;
    pthread_mutex_unlock(&btrx_lock);
}

/* what a helper wrote to its error log last, into the log */
static void btrx_log_tail(const char *path)
{
    char tail[200] = "";
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    off_t end = lseek(fd, 0, SEEK_END);
    lseek(fd, end > (off_t)sizeof(tail) - 1 ? end - (off_t)sizeof(tail) + 1 : 0, SEEK_SET);
    ssize_t n = read(fd, tail, sizeof(tail) - 1);
    close(fd);
    if (n <= 0) return;
    tail[n] = '\0';
    while (n > 0 && (tail[n - 1] == '\n' || tail[n - 1] == '\r')) tail[--n] = '\0';
    char *last = strrchr(tail, '\n');
    btrx_log("%s says: %.90s", path, last ? last + 1 : tail);
}

/* Rockbox's own files (the card's logs, the sound card) are not all
 * close-on-exec. A helper, or a BlueALSA started here that outlives the
 * session, must not hold them: the card could not be unmounted for USB. */
static void btrx_close_inherited(void)
{
    for (int fd = 3; fd < 256; fd++) close(fd);
}

/* sh -c cmd with its stdout (and stdin, if asked) on pipes; vfork and
 * close-on-exec, as bt_popen() */
static pid_t btrx_spawn(const char *cmd, int *to_child, int *from_child)
{
    int in[2] = { -1, -1 }, out[2];
    if (pipe2(out, O_CLOEXEC) != 0) return -1;
    if (to_child && pipe2(in, O_CLOEXEC) != 0)
    {
        close(out[0]); close(out[1]);
        return -1;
    }
    pid_t pid = vfork();
    if (pid == 0)
    {
        setpgid(0, 0);
        dup2(out[1], STDOUT_FILENO);
        if (to_child) dup2(in[0], STDIN_FILENO);
        btrx_close_inherited();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(out[1]);
    if (to_child) close(in[0]);
    if (pid < 0)
    {
        hiby_debug_log("rx: spawn '%s': %s", cmd, strerror(errno));
        close(out[0]);
        if (to_child) close(in[1]);
        return -1;
    }
    *from_child = out[0];
    if (to_child) *to_child = in[1];
    return pid;
}

static void btrx_end_child(pid_t *pid, int wait_ms)
{
    if (*pid <= 0) return;
    if (wait_ms) kill(-*pid, SIGTERM);
    bt_wait_child_for(*pid, wait_ms);   /* SIGKILL past the wait */
    *pid = 0;
}

/* -------------------------------------------------------------- audio */
static int16_t btrx_ring[BTRX_RING * 2];
static volatile uint32_t btrx_head, btrx_tail;   /* the reader / the mixer */
static volatile long btrx_last_ms;
static volatile unsigned long btrx_bytes;        /* from this reader */
static volatile bool btrx_reader_done;
static int btrx_frame_bytes = 4, btrx_sample_bytes = 2, btrx_hi = 0, btrx_in_channels = 2;
static int btrx_audio_fd = -1;
static pid_t btrx_audio_pid;
static pthread_t btrx_audio_thread;
static bool btrx_audio_up;

static int16_t btrx_sample(const uint8_t *p)
{
    /* the top 16 bits: S16_LE, S24_3LE, S24_LE (low 3 of 4 bytes), S32_LE */
    return (int16_t)(p[btrx_hi] | (p[btrx_hi + 1] << 8));
}

static void *btrx_reader(void *unused)
{
    (void)unused;
    static uint8_t buf[4096];
    size_t have = 0;
    int fb = btrx_frame_bytes;
    for (;;)
    {
        ssize_t r = read(btrx_audio_fd, buf + have, sizeof(buf) - have);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        have += (size_t)r;
        btrx_bytes += (unsigned long)r;
        size_t frames = have / fb;
        /* Nobody playing it (local playback has the output): what does
         * not fit is dropped, so the phone's stream, and its clock, go on */
        uint32_t space = BTRX_RING - (btrx_head - btrx_tail);
        uint32_t n = (uint32_t)frames < space ? (uint32_t)frames : space;
        for (uint32_t k = 0; k < n; k++)
        {
            const uint8_t *f = buf + k * fb;
            uint32_t at = (btrx_head + k) & (BTRX_RING - 1);
            btrx_ring[at * 2] = btrx_sample(f);
            btrx_ring[at * 2 + 1] = btrx_sample(btrx_in_channels > 1 ? f + btrx_sample_bytes : f);
        }
        __sync_synchronize();
        btrx_head += n;
        memmove(buf, buf + frames * fb, have - frames * fb);
        have -= frames * fb;
        btrx_last_ms = bt_ms();
    }
    btrx_reader_done = true;
    return NULL;
}

static void btrx_audio_stop(void)
{
    if (!btrx_audio_up) return;
    btrx_end_child(&btrx_audio_pid, 1000);   /* its pipe ends: the reader sees EOF */
    pthread_join(btrx_audio_thread, NULL);
    close(btrx_audio_fd);
    btrx_audio_fd = -1;
    btrx_audio_up = false;
}

static bool btrx_audio_start(const char *path, const char *mac, const char *format,
                             int channels, int rate, bool arecord)
{
    char cmd[256];
    if (arecord)
        snprintf(cmd, sizeof(cmd), "exec arecord -q -D bluealsa:DEV=%s,PROFILE=a2dp -t raw -f %s -c %d -r %d 2>>"
                 BTRX_HELPER_LOG, mac, format, channels, rate);
    else
        snprintf(cmd, sizeof(cmd), "exec bluealsa-cli open '%s' 2>>" BTRX_HELPER_LOG, path);
    btrx_sample_bytes = !strcmp(format, "S16_LE") ? 2 : !strcmp(format, "S24_3LE") ? 3 : 4;
    btrx_hi = btrx_sample_bytes == 2 ? 0 : !strcmp(format, "S32_LE") ? 2 : 1;
    btrx_in_channels = channels;
    btrx_frame_bytes = btrx_sample_bytes * channels;
    btrx_bytes = 0;
    btrx_reader_done = false;
    btrx_audio_pid = btrx_spawn(cmd, NULL, &btrx_audio_fd);
    if (btrx_audio_pid <= 0) return false;
    if (pthread_create(&btrx_audio_thread, NULL, btrx_reader, NULL) != 0)
    {
        btrx_end_child(&btrx_audio_pid, 0);
        close(btrx_audio_fd);
        btrx_audio_fd = -1;
        return false;
    }
    btrx_audio_up = true;
    btrx_log("audio: reading with %s (%s, %d ch, %d Hz)", arecord ? "arecord" : "bluealsa-cli open",
             format, channels, rate);
    return true;
}

int hiby_btrx_read(int16_t *dst, int frames)
{
    uint32_t have = btrx_head - btrx_tail;
    /* the phone's clock runs a little ahead of the DAC's: never let the
     * delay grow past half a second */
    if (have > BTRX_RING / 4 * 3)
    {
        btrx_tail = btrx_head - BTRX_RING / 4;
        have = BTRX_RING / 4;
    }
    if ((uint32_t)frames > have) frames = (int)have;
    for (int k = 0; k < frames; k++)
    {
        uint32_t at = (btrx_tail + (uint32_t)k) & (BTRX_RING - 1);
        dst[k * 2] = btrx_ring[at * 2];
        dst[k * 2 + 1] = btrx_ring[at * 2 + 1];
    }
    __sync_synchronize();
    btrx_tail += (uint32_t)frames;
    return frames;
}

int hiby_btrx_buffered(void) { return (int)(btrx_head - btrx_tail); }
unsigned hiby_btrx_idle_ms(void) { return (unsigned)(bt_ms() - btrx_last_ms); }

/* -------------------------------------------------------------- agent */
static pid_t btrx_agent_pid;
static int btrx_agent_in = -1, btrx_agent_out = -1;
static char btrx_agent_buf[1024];
static size_t btrx_agent_len;
static int btrx_agent_lines;            /* logged this session, bounded */
static bool btrx_discoverable_ok;

static void btrx_agent_say(const char *line)
{
    if (btrx_agent_in < 0) return;
    char l[128];
    int n = snprintf(l, sizeof(l), "%s\n", line);
    if (write(btrx_agent_in, l, n) != n)
        hiby_debug_log("rx: agent did not take '%s': %s", line, strerror(errno));
}

/* bluetoothctl's line without colours, prompts or carriage returns */
static void btrx_clean(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (; *in && n + 1 < size; in++)
    {
        if (*in == '\033') { while (in[1] && !isalpha((unsigned char)in[1])) in++; if (in[1]) in++; continue; }
        if (*in == '\001' || *in == '\002' || *in == '\r') continue;
        out[n++] = *in;
    }
    out[n] = '\0';
    char *p = strstr(out, "]# ");                 /* "[bluetooth]# " before the text */
    if (p) memmove(out, p + 3, strlen(p + 3) + 1);
}

static void btrx_agent_line(char *raw)
{
    char line[256];
    btrx_clean(raw, line, sizeof(line));
    if (!line[0]) return;
    if (strstr(line, "discoverable on succeeded")) btrx_discoverable_ok = true;
    bool wanted = strstr(line, "Paired:") || strstr(line, "Connected:") || strstr(line, "Trusted:") ||
                  strstr(line, "[NEW] Device") || strstr(line, "[DEL] Device") ||
                  strstr(line, "gent") || strstr(line, "Changing") || strstr(line, "Failed") ||
                  strstr(line, "rror") || strstr(line, "not available") || strstr(line, "Request");
    if (wanted && btrx_agent_lines < 120)
    {
        btrx_agent_lines++;
        btrx_log("agent: %.100s", line);
    }
}

/* Read what bluetoothctl said, up to wait_ms. A question it asks has no
 * line end: it is answered as it comes. */
static void btrx_agent_pump(int wait_ms)
{
    if (btrx_agent_out < 0) { usleep(wait_ms * 1000); return; }
    struct pollfd pfd = { btrx_agent_out, POLLIN | POLLHUP, 0 };
    int ready = poll(&pfd, 1, wait_ms);
    if (ready <= 0) return;
    ssize_t n = read(btrx_agent_out, btrx_agent_buf + btrx_agent_len,
                     sizeof(btrx_agent_buf) - 1 - btrx_agent_len);
    if (n <= 0)
    {
        btrx_log("agent: bluetoothctl ended");
        close(btrx_agent_out);
        btrx_agent_out = -1;
        return;
    }
    btrx_agent_len += (size_t)n;
    btrx_agent_buf[btrx_agent_len] = '\0';
    char *nl;
    while ((nl = strchr(btrx_agent_buf, '\n')))
    {
        *nl = '\0';
        btrx_agent_line(btrx_agent_buf);
        btrx_agent_len -= (size_t)(nl + 1 - btrx_agent_buf);
        memmove(btrx_agent_buf, nl + 1, btrx_agent_len + 1);
    }
    char q[256];
    btrx_clean(btrx_agent_buf, q, sizeof(q));
    if (strstr(q, "(yes/no)") || strstr(q, "Enter PIN code"))
    {
        bool pin = strstr(q, "Enter PIN code") != NULL;
        btrx_log("pairing request: \"%.80s\", answered %s", q, pin ? "0000" : "yes");
        btrx_agent_say(pin ? "0000" : "yes");
        btrx_agent_len = 0;
        btrx_agent_buf[0] = '\0';
    }
    else if (btrx_agent_len >= sizeof(btrx_agent_buf) - 1)
        btrx_agent_len = 0;             /* a line too long to be ours */
}

static bool btrx_agent_start(void)
{
    btrx_agent_pid = btrx_spawn("exec bluetoothctl 2>&1", &btrx_agent_in, &btrx_agent_out);
    if (btrx_agent_pid <= 0) { btrx_log("agent: bluetoothctl did not start"); return false; }
    btrx_agent_len = 0;
    btrx_agent_lines = 0;
    btrx_discoverable_ok = false;
    btrx_agent_say("agent NoInputNoOutput");
    btrx_agent_say("default-agent");
    char alias[HIBY_BT_ALIAS_LEN + 16], name[HIBY_BT_ALIAS_LEN];
    hiby_bt_player_name(name, sizeof(name));   /* the name given in Settings, else ours */
    snprintf(alias, sizeof(alias), "system-alias %s", name[0] ? name : BTRX_ALIAS);
    btrx_agent_say(alias);
    btrx_agent_say("pairable on");
    btrx_agent_say("discoverable on");
    for (long until = bt_ms() + 5000; !btrx_discoverable_ok && btrx_agent_out >= 0 && bt_ms() < until; )
        btrx_agent_pump(200);
    return btrx_discoverable_ok;
}

/* The agent goes, then the old visibility and name are put back with
 * one-shot commands: they work even when the agent died on the way. */
static void btrx_agent_stop(const char *alias, bool pairable)
{
    if (btrx_agent_pid > 0)
    {
        btrx_agent_say("quit");
        for (long until = bt_ms() + 1500; btrx_agent_out >= 0 && bt_ms() < until; )
            btrx_agent_pump(100);
        btrx_end_child(&btrx_agent_pid, 1000);
    }
    if (btrx_agent_in >= 0) close(btrx_agent_in);
    if (btrx_agent_out >= 0) close(btrx_agent_out);
    btrx_agent_in = btrx_agent_out = -1;
    bool hidden = bt_ctl_run("discoverable", "off", "succeeded");
    bool pair = bt_ctl_run("pairable", pairable ? "on" : "off", "succeeded");
    bool named = !alias[0] || bt_ctl_run("system-alias", alias, "succeeded");
    btrx_log("restore: discoverable off %s, pairable %s %s, alias '%s' %s", hidden ? "ok" : "FAILED",
             pairable ? "on" : "off", pair ? "ok" : "FAILED", alias, named ? "ok" : "FAILED");
}

/* ------------------------------------------------------------ session */
/* BlueALSA restarted with these profiles; how long until it answered, or -1 */
static int btrx_bluealsa(const char *daemon)
{
    char cmd[200];
    bt_system("killall bluealsa >/dev/null 2>&1");
    for (int i = 0; i < 20 && bt_system("pgrep -x bluealsa >/dev/null 2>&1") == 0; i++)
    {
        if (i == 10) bt_system("killall -9 bluealsa >/dev/null 2>&1");
        usleep(100000);
    }
    long from = bt_ms();
    snprintf(cmd, sizeof(cmd), "%s >>" BTRX_DAEMON_LOG " 2>&1 &", daemon);
    pid_t pid = vfork();
    if (pid == 0)
    {
        setpgid(0, 0);
        btrx_close_inherited();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) bt_wait_child(pid);    /* the shell, gone at once; the daemon stays */
    else btrx_log("bluealsa: could not start a shell: %s", strerror(errno));
    while (bt_ms() - from < 6000)
    {
        FILE *fp = bt_popen("bluealsa-cli status 2>&1", "r");
        char line[160], profiles[120] = "";
        while (fp && fgets(line, sizeof(line), fp))
            if (strstr(line, "A2DP"))
            {
                bt_trim(line);
                snprintf(profiles + strlen(profiles), sizeof(profiles) - strlen(profiles), "%s%s",
                         profiles[0] ? " " : "", line + strspn(line, " \t"));
            }
        if (fp && bt_pclose(fp) == 0)
        {
            btrx_log("bluealsa: '%s' answering after %ld ms, profiles: %s", daemon + 9,
                     bt_ms() - from, profiles[0] ? profiles : "?");
            return (int)(bt_ms() - from);
        }
        usleep(250000);
    }
    btrx_log("bluealsa: '%s' did not answer in 6 s", daemon + 9);
    btrx_log_tail(BTRX_DAEMON_LOG);
    return -1;
}

/* the phone: an A2DP sink PCM in BlueALSA */
static bool btrx_find_phone(char *mac, char *path, size_t path_len)
{
    char line[256];
    FILE *fp = bt_popen("bluealsa-cli list-pcms 2>/dev/null", "r");
    bool found = false;
    while (fp && fgets(line, sizeof(line), fp))
    {
        if (found || !strstr(line, "/a2dpsnk/source") || !bt_extract_mac_from_line(line, mac, 18))
            continue;
        bt_trim(line);
        snprintf(path, path_len, "%s", line);
        found = true;
    }
    if (fp) bt_pclose(fp);
    return found;
}

/* "bluealsa-cli info": codec, format, channels and rate of the phone's PCM */
static void btrx_pcm_info(const char *path, char *format, size_t format_len)
{
    char cmd[160], line[160];
    snprintf(cmd, sizeof(cmd), "bluealsa-cli info '%s' 2>/dev/null", path);
    snprintf(format, format_len, "S16_LE");
    FILE *fp = bt_popen(cmd, "r");
    pthread_mutex_lock(&btrx_lock);
    btrx_st.rate = 44100;
    btrx_st.channels = 2;
    pthread_mutex_unlock(&btrx_lock);
    while (fp && fgets(line, sizeof(line), fp))
    {
        bt_trim(line);
        const char *p = line + strspn(line, " \t");
        pthread_mutex_lock(&btrx_lock);
        if (!strncmp(p, "Sampling:", 9) && atoi(p + 9) >= 8000) btrx_st.rate = atoi(p + 9);
        else if (!strncmp(p, "Channels:", 9) && atoi(p + 9) >= 1) btrx_st.channels = atoi(p + 9) > 1 ? 2 : 1;
        else if (!strncmp(p, "Selected codec:", 15))
            snprintf(btrx_st.codec, sizeof(btrx_st.codec), "%s", p + 15 + strspn(p + 15, " "));
        else if (!strncmp(p, "Codec:", 6) && !btrx_st.codec[0])
            snprintf(btrx_st.codec, sizeof(btrx_st.codec), "%s", p + 6 + strspn(p + 6, " "));
        else if (!strncmp(p, "Format:", 7))
            snprintf(format, format_len, "%s", p + 7 + strspn(p + 7, " "));
        pthread_mutex_unlock(&btrx_lock);
    }
    if (fp) bt_pclose(fp);
    if (strcmp(format, "S16_LE") && strcmp(format, "S24_3LE") && strcmp(format, "S24_LE") &&
        strcmp(format, "S32_LE"))
    {
        btrx_log("audio: format '%s' unknown, reading it as S16_LE", format);
        snprintf(format, format_len, "S16_LE");
    }
}

/* BlueALSA's own log, since at: does it mention this? (a codec name it
 * did not take: then it is started again without the extra codecs) */
static bool btrx_daemon_said(off_t at, const char *what)
{
    char buf[2048];
    int fd = open(BTRX_DAEMON_LOG, O_RDONLY);
    if (fd < 0) return false;
    lseek(fd, at, SEEK_SET);
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    return strstr(buf, what) != NULL;
}

/* the codecs the phone and this player share, for the codec picker (#7) */
static void btrx_codecs(const char *path)
{
    char cmd[200], line[200];
    snprintf(cmd, sizeof(cmd), "bluealsa-cli codec '%s' 2>/dev/null", path);
    FILE *fp = bt_popen(cmd, "r");
    pthread_mutex_lock(&btrx_lock);
    btrx_st.codec_count = 0;
    pthread_mutex_unlock(&btrx_lock);
    while (fp && fgets(line, sizeof(line), fp))
    {
        if (strncmp(line, "Available codecs:", 17)) continue;
        char *save = NULL;
        pthread_mutex_lock(&btrx_lock);
        for (char *t = strtok_r(line + 17, " \t\r\n", &save); t && btrx_st.codec_count < 6;
             t = strtok_r(NULL, " \t\r\n", &save))
            snprintf(btrx_st.codecs[btrx_st.codec_count++], sizeof(btrx_st.codecs[0]), "%s", t);
        pthread_mutex_unlock(&btrx_lock);
    }
    if (fp) bt_pclose(fp);
    btrx_log("codecs: %d the phone and the player share", btrx_st.codec_count);
}

/* A sink can only ask: BlueALSA sends the phone a new configuration for
 * its stream (A2DP reconfigure). Many phones take it; some keep their
 * own choice, and that is said on the page. The audio then restarts with
 * whatever format came. */
static void btrx_codec_switch(const char *path, const char *codec, char *format, size_t format_len, bool use_arecord)
{
    char cmd[220], was[HIBY_BT_CODEC_NAME_LEN];
    pthread_mutex_lock(&btrx_lock);
    snprintf(was, sizeof(was), "%s", btrx_st.codec);
    btrx_st.codec_note[0] = '\0';
    pthread_mutex_unlock(&btrx_lock);
    snprintf(cmd, sizeof(cmd), "bluealsa-cli codec '%s' '%s' >/dev/null 2>&1", path, codec);
    int rc = bt_system_for(cmd, 8000);
    btrx_audio_stop();
    usleep(300000);
    btrx_pcm_info(path, format, format_len);
    pthread_mutex_lock(&btrx_lock);
    bool took = !strcasecmp(btrx_st.codec, codec);
    if (!took)
        snprintf(btrx_st.codec_note, sizeof(btrx_st.codec_note), "%s", rc == 0 ? "kept" : "refused");
    char now[HIBY_BT_CODEC_NAME_LEN];
    snprintf(now, sizeof(now), "%s", btrx_st.codec);
    int ch = btrx_st.channels, rate = btrx_st.rate;
    pthread_mutex_unlock(&btrx_lock);
    btrx_log("codec: asked for %s (was %s): %s, now %s", codec, was, rc == 0 ? "sent" : "refused", now);
    btrx_head = btrx_tail = 0;
    char mac[18] = "";
    pthread_mutex_lock(&btrx_lock);
    snprintf(mac, sizeof(mac), "%s", btrx_st.mac);
    pthread_mutex_unlock(&btrx_lock);
    btrx_audio_start(path, mac, format, ch, rate, use_arecord);
}

/* the phone's player over AVRCP (BlueZ MediaPlayer1): track and state */
static bool btrx_player(const char *mac)
{
    char mac_u[18], cmd[256], line[256], key[16] = "";
    char title[96] = "", artist[96] = "", status[16] = "";
    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    snprintf(cmd, sizeof(cmd), "dbus-send --system --print-reply --dest=org.bluez "
             "/org/bluez/hci0/dev_%s/player0 org.freedesktop.DBus.Properties.GetAll "
             "string:org.bluez.MediaPlayer1 2>/dev/null", mac_u);
    FILE *fp = bt_popen(cmd, "r");
    if (!fp) return false;
    while (fgets(line, sizeof(line), fp))
    {
        /* string "Title" then, on the next line, variant string "..." */
        char *q = strstr(line, "string \"");
        if (!q) continue;
        q += 8;
        char *end = strrchr(q, '"');
        if (!end) continue;
        *end = '\0';
        if (!strstr(line, "variant")) { snprintf(key, sizeof(key), "%s", q); continue; }
        if (!strcmp(key, "Title")) snprintf(title, sizeof(title), "%s", q);
        else if (!strcmp(key, "Artist")) snprintf(artist, sizeof(artist), "%s", q);
        else if (!strcmp(key, "Status")) snprintf(status, sizeof(status), "%s", q);
        key[0] = '\0';
    }
    bool ok = bt_pclose(fp) == 0;
    if (!ok) return false;
    pthread_mutex_lock(&btrx_lock);
    bool changed = strcmp(title, btrx_st.title) || strcmp(artist, btrx_st.artist) ||
                   strcmp(status, btrx_st.player);
    snprintf(btrx_st.title, sizeof(btrx_st.title), "%s", title);
    snprintf(btrx_st.artist, sizeof(btrx_st.artist), "%s", artist);
    snprintf(btrx_st.player, sizeof(btrx_st.player), "%s", status);
    pthread_mutex_unlock(&btrx_lock);
    if (changed) btrx_log("player: '%.40s' by '%.30s', %s", title, artist, status[0] ? status : "?");
    return true;
}

static void btrx_volume(const char *path)
{
    char cmd[160], line[96] = "";
    snprintf(cmd, sizeof(cmd), "bluealsa-cli volume '%s' 2>/dev/null", path);
    FILE *fp = bt_popen(cmd, "r");
    if (!fp) return;
    if (!fgets(line, sizeof(line), fp)) line[0] = '\0';
    bt_pclose(fp);
    const char *p = strstr(line, "Volume:");
    if (!p) return;
    p += 7;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return;
    int v = atoi(p);
    if (v < 0 || v > 127) return;
    pthread_mutex_lock(&btrx_lock);
    bool changed = btrx_st.volume != v;
    btrx_st.volume = v;
    pthread_mutex_unlock(&btrx_lock);
    if (changed) btrx_log("volume: the phone set %d/127", v);
}

static void btrx_control_send(const char *mac, const char *method)
{
    char mac_u[18], cmd[256];
    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    snprintf(cmd, sizeof(cmd), "dbus-send --system --print-reply --dest=org.bluez "
             "/org/bluez/hci0/dev_%s/player0 org.bluez.MediaPlayer1.%s >/dev/null 2>&1", mac_u, method);
    int rc = bt_system(cmd);
    btrx_log("control: %s %s", method, rc == 0 ? "sent" : "FAILED");
}

static void btrx_session(char *why, size_t why_len)
{
    char alias[HIBY_BT_NAME_LEN] = "", line[160], active[18];
    char mac[18] = "", path[128] = "", format[12] = "S16_LE";
    bool pairable = true, discoverable = false, profile_switched = false;

    btrx_log("start: receive from phone");
    if (!hiby_bt_power(true))
    {
        btrx_log("start: controller did not power on");
        snprintf(why, why_len, "Bluetooth radio did not start");
        return;
    }
    if (hiby_pcm_get_bt_mac() || bt_get_active_mac(active, sizeof(active)))
    {
        btrx_log("start: headphones %s let go for now (they stay paired)",
                 hiby_pcm_get_bt_mac() ? hiby_pcm_get_bt_mac() : active);
        hiby_bt_disconnect();
    }
    FILE *fp = bt_popen("bluetoothctl show 2>/dev/null", "r");
    while (fp && fgets(line, sizeof(line), fp))
    {
        char *p = line + strspn(line, " \t");
        bt_trim(p);
        if (!strncmp(p, "Alias:", 6)) snprintf(alias, sizeof(alias), "%s", p + 6 + strspn(p + 6, " "));
        else if (!strncmp(p, "Pairable:", 9)) pairable = strstr(p, "yes") != NULL;
        else if (!strncmp(p, "Discoverable:", 13)) discoverable = strstr(p, "yes") != NULL;
    }
    if (fp) bt_pclose(fp);
    btrx_log("start: adapter alias '%s', pairable %d, discoverable %d", alias, pairable, discoverable);

    profile_switched = true;
    struct stat before;
    off_t log_at = stat(BTRX_DAEMON_LOG, &before) == 0 ? before.st_size : 0;
    if (btrx_bluealsa(BTRX_SINK_ALL) < 0 &&
        (!btrx_daemon_said(log_at, "codec") || btrx_bluealsa(BTRX_SINK) < 0))
    {
        snprintf(why, why_len, "Bluetooth could not switch to receiving");
        goto restore;
    }
    if (!btrx_agent_start())
    {
        btrx_log("start: discoverable on did not succeed");
        snprintf(why, why_len, "The player could not be made visible");
        goto restore;
    }
    {
        char shown[HIBY_BT_ALIAS_LEN];
        hiby_bt_player_name(shown, sizeof(shown));
        btrx_log("waiting: visible as '%s' for %d s", shown[0] ? shown : BTRX_ALIAS, btrx_wait_ms / 1000);
    }
    btrx_set_state(HIBY_BTRX_WAITING);

    long started = bt_ms(), next_pcm = 0, next_player = 0, connected_at = 0;
    int missing = 0, restarts = 0;
    bool use_arecord = false, logged_first = false, quiet = false, has_player = true;
    while (!btrx_stop_wanted)
    {
        btrx_agent_pump(250);
        long now = bt_ms();
        if (now >= next_pcm)
        {
            next_pcm = now + 1000;
            char found_mac[18], found_path[128];
            bool found = btrx_find_phone(found_mac, found_path, sizeof(found_path));
            if (found && !mac[0])
            {
                char name[HIBY_BT_NAME_LEN];
                snprintf(mac, sizeof(mac), "%s", found_mac);
                snprintf(path, sizeof(path), "%s", found_path);
                bt_get_device_name(mac, name, sizeof(name));
                pthread_mutex_lock(&btrx_lock);
                snprintf(btrx_st.mac, sizeof(btrx_st.mac), "%s", mac);
                snprintf(btrx_st.name, sizeof(btrx_st.name), "%s", name[0] ? name : mac);
                btrx_st.codec[0] = btrx_st.title[0] = btrx_st.artist[0] = btrx_st.player[0] = '\0';
                btrx_st.volume = -1;
                pthread_mutex_unlock(&btrx_lock);
                btrx_pcm_info(path, format, sizeof(format));
                btrx_codecs(path);
                btrx_log("connected: %s (%s), codec %s, %s, %d ch, %d Hz", mac, name[0] ? name : "no name",
                         btrx_st.codec[0] ? btrx_st.codec : "?", format, btrx_st.channels, btrx_st.rate);
                snprintf(line, sizeof(line), "trust %s", mac);
                btrx_agent_say(line);         /* it comes back without asking */
                btrx_head = btrx_tail = 0;
                btrx_last_ms = now - 60000;
                connected_at = now;
                restarts = 0;
                use_arecord = logged_first = quiet = false;
                has_player = true;
                next_player = now;
                btrx_audio_start(path, mac, format, btrx_st.channels, btrx_st.rate, use_arecord);
                pthread_mutex_lock(&btrx_lock);
                btrx_st.state = HIBY_BTRX_CONNECTED;
                btrx_st.connects++;
                pthread_mutex_unlock(&btrx_lock);
            }
            else if (!found && mac[0] && ++missing >= 2)   /* the list can come back empty once */
            {
                btrx_log("disconnected: %s, its A2DP PCM is gone", mac);
                snprintf(why, why_len, "Phone disconnected");
                break;
            }
            if (found) missing = 0;
        }
        if (!mac[0])
        {
            if (now - started > btrx_wait_ms)
            {
                btrx_log("waiting: no phone in %d s, giving up", btrx_wait_ms / 1000);
                snprintf(why, why_len, "No phone connected");
                break;
            }
            continue;
        }
        if (!logged_first && btrx_bytes > 0)
        {
            logged_first = true;
            btrx_log("audio: first PCM, %lu bytes, %ld ms after connecting", btrx_bytes, now - connected_at);
        }
        bool idle = now - btrx_last_ms > 3000;
        if (logged_first && idle != quiet)
        {
            quiet = idle;
            btrx_log(idle ? "audio: none for 3 s (phone paused)" : "audio: flowing again");
        }
        if (btrx_reader_done)
        {
            unsigned long got = btrx_bytes;
            char gone_mac[18], gone_path[128];
            btrx_audio_stop();
            btrx_log("audio: reader ended after %lu bytes", got);
            btrx_log_tail(BTRX_HELPER_LOG);
            if (!btrx_find_phone(gone_mac, gone_path, sizeof(gone_path)))
            {
                btrx_log("disconnected: %s, its A2DP PCM went with the audio", mac);
                snprintf(why, why_len, "Phone disconnected");
                break;
            }
            if (++restarts > BTRX_RESTARTS)
            {
                snprintf(why, why_len, "Could not read the phone's audio");
                break;
            }
            if (!got) use_arecord = !use_arecord;   /* the other way in */
            usleep(500000);
            btrx_audio_start(path, mac, format, btrx_st.channels, btrx_st.rate, use_arecord);
        }
        char want[12];
        pthread_mutex_lock(&btrx_lock);
        snprintf(want, sizeof(want), "%s", btrx_ctl_want);
        btrx_ctl_want[0] = '\0';
        pthread_mutex_unlock(&btrx_lock);
        if (want[0]) { btrx_control_send(mac, want); next_player = now + 300; }
        char codec[HIBY_BT_CODEC_NAME_LEN];
        pthread_mutex_lock(&btrx_lock);
        snprintf(codec, sizeof(codec), "%s", btrx_codec_want);
        btrx_codec_want[0] = '\0';
        pthread_mutex_unlock(&btrx_lock);
        if (codec[0])
        {
            btrx_codec_switch(path, codec, format, sizeof(format), use_arecord);
            next_pcm = bt_ms() + 2000;  /* the PCM comes back under the same path */
        }
        if (now >= next_player)
        {
            bool had = has_player;
            has_player = btrx_player(mac);
            if (had && !has_player) btrx_log("player: none over AVRCP, showing the phone's name");
            btrx_volume(path);
            next_player = now + (has_player ? 2000 : 10000);
        }
    }
    if (!why[0] && btrx_stop_wanted) btrx_log("stop: asked to");

restore:
    btrx_set_state(HIBY_BTRX_STOPPING);
    btrx_audio_stop();
    btrx_agent_stop(alias, pairable);
    if (mac[0] && strcmp(why, "Phone disconnected"))
        btrx_log("restore: phone %s let go: %s", mac,
                 bt_ctl_run("disconnect", mac, "Successful disconnected") ? "ok" : "FAILED");
    if (profile_switched)
    {
        int ms = btrx_bluealsa(bt_source_daemon());
        btrx_log(ms >= 0 ? "restore: source mode back" : "restore: FAILED, bluealsa a2dp-source did not answer");
        if (ms < 0 && !why[0]) snprintf(why, why_len, "Bluetooth did not come back. Restart the player");
    }
}

static void *btrx_thread(void *unused)
{
    (void)unused;
    char why[80] = "";
    sigset_t pipe_set;
    hiby_bt_worker_thread();            /* no Rockbox kernel calls from here */
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);      /* a helper that died: EPIPE, not the end of us */
    pthread_sigmask(SIG_BLOCK, &pipe_set, NULL);
    btrx_session(why, sizeof(why));
    btrx_log("ended: %s", why[0] ? why : "stopped");
    pthread_mutex_lock(&btrx_lock);
    btrx_st.state = HIBY_BTRX_OFF;
    snprintf(btrx_st.error, sizeof(btrx_st.error), "%s", why);
    pthread_mutex_unlock(&btrx_lock);
    btrx_on = false;
    return NULL;
}

bool hiby_btrx_start(void)
{
    if (btrx_on) return true;
    pthread_mutex_lock(&btrx_lock);
    memset(&btrx_st, 0, sizeof(btrx_st));
    btrx_st.state = HIBY_BTRX_STARTING;
    btrx_st.volume = -1;
    btrx_ctl_want[0] = '\0';
    pthread_mutex_unlock(&btrx_lock);
    btrx_stop_wanted = false;
    btrx_on = true;
    pthread_t thread;
    if (pthread_create(&thread, NULL, btrx_thread, NULL) != 0)
    {
        btrx_on = false;
        btrx_set_state(HIBY_BTRX_OFF);
        return false;
    }
    pthread_detach(thread);
    return true;
}

void hiby_btrx_stop(void) { btrx_stop_wanted = true; }
bool hiby_btrx_running(void) { return btrx_on; }

void hiby_btrx_status(struct hiby_btrx_status *status)
{
    pthread_mutex_lock(&btrx_lock);
    *status = btrx_st;
    pthread_mutex_unlock(&btrx_lock);
}

void hiby_btrx_control(const char *method)
{
    pthread_mutex_lock(&btrx_lock);
    snprintf(btrx_ctl_want, sizeof(btrx_ctl_want), "%s", method);
    pthread_mutex_unlock(&btrx_lock);
}

bool hiby_btrx_take_event(char *line, int size)
{
    pthread_mutex_lock(&btrx_lock);
    bool got = btrx_ev_tail != btrx_ev_head;
    if (got) snprintf(line, size, "%s", btrx_events[btrx_ev_tail++ % BTRX_EVENTS]);
    pthread_mutex_unlock(&btrx_lock);
    return got;
}

/* ================================================== names, pairing, reconnect */
/* What GabagoolOS keeps about Bluetooth itself, on the card: the player's
 * name, the names given to paired devices, the headphones to reconnect,
 * the SBC quality. BlueZ's own store sits in a path the R1's rootfs points
 * nowhere useful (/var/lib/bluetooth is a dangling link in 1.6 and 2.0),
 * so its aliases are given back to it from here whenever the stack comes up. */
#define BT_CFG_FILE ROCKBOX_DIR "/sloppy/bluetooth.cfg"
#define BT_CFG_NAMES 16
static pthread_mutex_t bt_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct
{
    bool loaded;
    char name[HIBY_BT_ALIAS_LEN];
    char last[18];
    int sbc;
    struct { char mac[18]; char name[HIBY_BT_ALIAS_LEN]; } dev[BT_CFG_NAMES];
} bt_cfg;

static void bt_cfg_load_locked(void)
{
    if (bt_cfg.loaded) return;
    bt_cfg.loaded = true;
    int fd = open(BT_CFG_FILE, O_RDONLY);
    if (fd < 0) return;
    char buf[3072];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    int devs = 0;
    char *save = NULL;
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
    {
        if (!strncmp(l, "name=", 5)) snprintf(bt_cfg.name, sizeof(bt_cfg.name), "%s", l + 5);
        else if (!strncmp(l, "last=", 5) && strlen(l + 5) == 17) snprintf(bt_cfg.last, sizeof(bt_cfg.last), "%s", l + 5);
        else if (!strncmp(l, "sbc=", 4)) bt_cfg.sbc = atoi(l + 4) >= 0 && atoi(l + 4) <= 2 ? atoi(l + 4) : 0;
        else if (!strncmp(l, "dev ", 4) && strlen(l) > 22 && l[21] == '=' && devs < BT_CFG_NAMES)
        {
            memcpy(bt_cfg.dev[devs].mac, l + 4, 17);
            bt_cfg.dev[devs].mac[17] = '\0';
            snprintf(bt_cfg.dev[devs].name, sizeof(bt_cfg.dev[devs].name), "%s", l + 22);
            devs++;
        }
    }
}

static void bt_cfg_save_locked(void)
{
    char buf[3072];
    int n = snprintf(buf, sizeof(buf), "name=%s\nlast=%s\nsbc=%d\n", bt_cfg.name, bt_cfg.last, bt_cfg.sbc);
    for (int i = 0; i < BT_CFG_NAMES && n < (int)sizeof(buf); i++)
        if (bt_cfg.dev[i].mac[0])
            n += snprintf(buf + n, sizeof(buf) - n, "dev %s=%s\n", bt_cfg.dev[i].mac, bt_cfg.dev[i].name);
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    int fd = open(BT_CFG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        mkdir(ROCKBOX_DIR "/sloppy");
        fd = open(BT_CFG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    if (fd < 0) { hiby_debug_log("cfg: could not save %s", BT_CFG_FILE); return; }
    if (write(fd, buf, n) != n) hiby_debug_log("cfg: short write");
    if (bt_off_kernel) fsync(fd);
    close(fd);
}

/* a name as BlueZ and the file can hold it: one line, trimmed, short */
static void bt_clean_name(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (; in && *in && n + 1 < size; in++)
        out[n++] = (unsigned char)*in < 0x20 || *in == 0x7f ? ' ' : *in;
    out[n] = '\0';
    bt_trim(out);
    char *p = out;
    while (*p == ' ') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
}

/* argv, no shell (a name can hold anything), its output dropped */
static int bt_run_argv(char *const argv[], int timeout_ms)
{
    int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    pid_t pid = vfork();
    if (pid == 0)
    {
        setpgid(0, 0);
        if (null >= 0) { dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO); }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (null >= 0) close(null);
    if (pid < 0) return -1;
    int status = bt_wait_child_for(pid, timeout_ms);
    return status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* BlueZ's Alias of a device ("" gives it its own name back) */
static bool bt_set_alias(const char *mac, const char *name)
{
    char path[64], variant[HIBY_BT_ALIAS_LEN + 16], mac_u[18];
    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    snprintf(path, sizeof(path), "/org/bluez/hci0/dev_%s", mac_u);
    snprintf(variant, sizeof(variant), "variant:string:%s", name);
    char *argv[] = { "dbus-send", "--system", "--print-reply", "--dest=org.bluez", path,
                     "org.freedesktop.DBus.Properties.Set", "string:org.bluez.Device1",
                     "string:Alias", variant, NULL };
    return bt_run_argv(argv, 5000) == 0;
}

void hiby_bt_player_name(char *out, int size)
{
    pthread_mutex_lock(&bt_cfg_lock);
    bt_cfg_load_locked();
    snprintf(out, size, "%s", bt_cfg.name);
    pthread_mutex_unlock(&bt_cfg_lock);
}

void hiby_bt_local_name(const char *mac, char *out, int size)
{
    out[0] = '\0';
    if (!mac) return;
    pthread_mutex_lock(&bt_cfg_lock);
    bt_cfg_load_locked();
    for (int i = 0; i < BT_CFG_NAMES; i++)
        if (bt_cfg.dev[i].mac[0] && !strcasecmp(bt_cfg.dev[i].mac, mac))
            snprintf(out, size, "%s", bt_cfg.dev[i].name);
    pthread_mutex_unlock(&bt_cfg_lock);
}

/* the stack is up: the names kept here go back to BlueZ */
static void bt_apply_names(void)
{
    char name[HIBY_BT_ALIAS_LEN];
    hiby_bt_player_name(name, sizeof(name));
    if (name[0])
        hiby_debug_log("names: player '%s' %s", name,
                       bt_ctl_run("system-alias", name, "succeeded") ? "set" : "NOT set");
}

bool hiby_bt_set_player_name(const char *name)
{
    char clean[HIBY_BT_ALIAS_LEN];
    bt_clean_name(name, clean, sizeof(clean));
    pthread_mutex_lock(&bt_cfg_lock);
    bt_cfg_load_locked();
    snprintf(bt_cfg.name, sizeof(bt_cfg.name), "%s", clean);
    bt_cfg_save_locked();
    pthread_mutex_unlock(&bt_cfg_lock);
    if (!bt_is_enabled())
    {
        hiby_debug_log("names: player '%s' kept, given to BlueZ when Bluetooth comes on", clean);
        return true;
    }
    bool ok = clean[0] ? bt_ctl_run("system-alias", clean, "succeeded") : bt_ctl_run("reset-alias", NULL, "succeeded");
    hiby_debug_log("names: player '%s' %s", clean[0] ? clean : "(BlueZ's own)", ok ? "set" : "NOT set");
    return ok;
}

bool hiby_bt_rename(const char *mac, const char *name)
{
    if (!mac || strlen(mac) != 17 || !bt_has_mac_pattern(mac, ':')) return false;
    char clean[HIBY_BT_ALIAS_LEN];
    bt_clean_name(name, clean, sizeof(clean));
    pthread_mutex_lock(&bt_cfg_lock);
    bt_cfg_load_locked();
    int slot = -1, free_slot = -1;
    for (int i = 0; i < BT_CFG_NAMES; i++)
    {
        if (bt_cfg.dev[i].mac[0] && !strcasecmp(bt_cfg.dev[i].mac, mac)) slot = i;
        else if (!bt_cfg.dev[i].mac[0] && free_slot < 0) free_slot = i;
    }
    if (slot < 0) slot = free_slot >= 0 ? free_slot : BT_CFG_NAMES - 1;   /* full: the last one goes */
    if (clean[0])
    {
        snprintf(bt_cfg.dev[slot].mac, sizeof(bt_cfg.dev[slot].mac), "%s", mac);
        snprintf(bt_cfg.dev[slot].name, sizeof(bt_cfg.dev[slot].name), "%s", clean);
    }
    else
        bt_cfg.dev[slot].mac[0] = bt_cfg.dev[slot].name[0] = '\0';
    bt_cfg_save_locked();
    pthread_mutex_unlock(&bt_cfg_lock);
    bool told = bt_is_enabled() && bt_set_alias(mac, clean);
    hiby_debug_log("names: %s is '%s' here; BlueZ alias %s", mac, clean[0] ? clean : "(its own)", told ? "set" : "not set");
    return true;
}

/* names given here win over what BlueZ remembers */
static void bt_local_names(struct bt_device *devices, int count)
{
    for (int i = 0; i < count; i++)
    {
        char name[HIBY_BT_ALIAS_LEN];
        hiby_bt_local_name(devices[i].mac, name, sizeof(name));
        if (name[0]) snprintf(devices[i].name, sizeof(devices[i].name), "%s", name);
    }
}

static void bt_cfg_set_last(const char *mac)
{
    pthread_mutex_lock(&bt_cfg_lock);
    bt_cfg_load_locked();
    if (strcasecmp(bt_cfg.last, mac ? mac : ""))
    {
        snprintf(bt_cfg.last, sizeof(bt_cfg.last), "%s", mac ? mac : "");
        bt_cfg_save_locked();
    }
    pthread_mutex_unlock(&bt_cfg_lock);
}

static void bt_cfg_forget(const char *mac)
{
    pthread_mutex_lock(&bt_cfg_lock);
    bt_cfg_load_locked();
    bool changed = false;
    if (!strcasecmp(bt_cfg.last, mac)) { bt_cfg.last[0] = '\0'; changed = true; }
    for (int i = 0; i < BT_CFG_NAMES; i++)
        if (bt_cfg.dev[i].mac[0] && !strcasecmp(bt_cfg.dev[i].mac, mac))
        {
            bt_cfg.dev[i].mac[0] = bt_cfg.dev[i].name[0] = '\0';
            changed = true;
        }
    if (changed) bt_cfg_save_locked();
    pthread_mutex_unlock(&bt_cfg_lock);
}

bool hiby_bt_last_device(char mac[18])
{
    pthread_mutex_lock(&bt_cfg_lock);
    bt_cfg_load_locked();
    snprintf(mac, 18, "%s", bt_cfg.last);
    pthread_mutex_unlock(&bt_cfg_lock);
    return mac[0] != '\0';
}

int hiby_bt_last_error(void) { return bt_last_err; }

/* ------------------------------------------------------------ pairing */
/* Pairing used to be "bluetoothctl pair" on its own: its agent asked a
 * question on a terminal nobody read, and the stock bt-agent answers as
 * NoInputNoOutput with PIN 0000 - car head units that show a passkey or
 * want a PIN typed (#168) never paired. Now a bluetoothctl kept open is
 * the agent for this pairing, as KeyboardDisplay: a passkey to compare,
 * a PIN or passkey to type, or one to type on the other side, each goes
 * to the screen and the answer comes back. Headphones (NoInputNoOutput)
 * still pair with no question (Just Works, accepted here as asked for).
 * When the agent goes, BlueZ falls back to bt-agent, as before. */
static pthread_mutex_t bt_ask_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hiby_bt_ask bt_ask;
static unsigned bt_ask_serial, bt_answered;
static bool bt_answer_yes;
static char bt_answer_code[20];

bool hiby_bt_pair_question(struct hiby_bt_ask *ask)
{
    pthread_mutex_lock(&bt_ask_lock);
    *ask = bt_ask;
    pthread_mutex_unlock(&bt_ask_lock);
    return ask->kind != HIBY_BT_ASK_NONE;
}

void hiby_bt_pair_answer(unsigned serial, bool accept, const char *code)
{
    pthread_mutex_lock(&bt_ask_lock);
    if (serial == bt_ask.serial && bt_ask.kind != HIBY_BT_ASK_NONE)
    {
        bt_answered = serial;
        bt_answer_yes = accept;
        snprintf(bt_answer_code, sizeof(bt_answer_code), "%s", code ? code : "");
    }
    pthread_mutex_unlock(&bt_ask_lock);
}

static unsigned bt_ask_set(int kind, const char *mac, const char *name, const char *code)
{
    pthread_mutex_lock(&bt_ask_lock);
    bt_ask.kind = kind;
    bt_ask.serial = ++bt_ask_serial;
    snprintf(bt_ask.mac, sizeof(bt_ask.mac), "%s", mac);
    snprintf(bt_ask.name, sizeof(bt_ask.name), "%s", name);
    snprintf(bt_ask.code, sizeof(bt_ask.code), "%s", code ? code : "");
    unsigned serial = bt_ask.serial;
    pthread_mutex_unlock(&bt_ask_lock);
    hiby_debug_log("pair: asking (kind %d)%s%s", kind, code && code[0] ? ", code " : "", code ? code : "");
    return serial;
}

static void bt_ask_clear(void)
{
    pthread_mutex_lock(&bt_ask_lock);
    bt_ask.kind = HIBY_BT_ASK_NONE;
    pthread_mutex_unlock(&bt_ask_lock);
}

struct bt_sess { pid_t pid; int in, out; char buf[1024]; size_t len; int logged; };

static void bt_sess_say(struct bt_sess *s, const char *line)
{
    char l[160];
    int n = snprintf(l, sizeof(l), "%s\n", line);
    if (s->in >= 0 && write(s->in, l, n) != n)
        hiby_debug_log("pair: bluetoothctl did not take '%s'", line);
}

static bool bt_sess_prompt(const char *q)
{
    return strstr(q, "(yes/no)") || strstr(q, "Enter PIN code") || strstr(q, "Enter passkey");
}

/* the next line bluetoothctl said, or the question it is asking (which
 * has no line end), within wait_ms; cleaned of colours and prompts */
static bool bt_sess_next(struct bt_sess *s, char *line, size_t size, int wait_ms)
{
    for (int pass = 0; pass < 2; pass++)
    {
        char *nl = memchr(s->buf, '\n', s->len);
        if (nl)
        {
            *nl = '\0';
            btrx_clean(s->buf, line, size);
            s->len -= (size_t)(nl + 1 - s->buf);
            memmove(s->buf, nl + 1, s->len);
            s->buf[s->len] = '\0';
            return true;
        }
        s->buf[s->len] = '\0';
        btrx_clean(s->buf, line, size);
        if (bt_sess_prompt(line))
        {
            s->len = 0;
            s->buf[0] = '\0';
            return true;
        }
        if (pass || s->out < 0) break;
        struct pollfd pfd = { s->out, POLLIN | POLLHUP, 0 };
        if (poll(&pfd, 1, wait_ms) <= 0) break;
        if (s->len >= sizeof(s->buf) - 1) s->len = 0;   /* a line too long to be ours */
        ssize_t n = read(s->out, s->buf + s->len, sizeof(s->buf) - 1 - s->len);
        if (n <= 0) { close(s->out); s->out = -1; break; }
        s->len += (size_t)n;
    }
    line[0] = '\0';
    return false;
}

static int bt_pair_error(const char *line)
{
    if (strstr(line, "AlreadyExists")) return HIBY_BT_ERR_NONE;
    if (strstr(line, "AuthenticationFailed")) return HIBY_BT_ERR_PAIR_FAILED;
    if (strstr(line, "AuthenticationRejected") || strstr(line, "AuthenticationCanceled")) return HIBY_BT_ERR_PAIR_REJECTED;
    if (strstr(line, "AuthenticationTimeout")) return HIBY_BT_ERR_PAIR_TIMEOUT;
    if (strstr(line, "ConnectionAttemptFailed") || strstr(line, "Page Timeout") || strstr(line, "Host is down") ||
        strstr(line, "not available") || strstr(line, "timed out") || strstr(line, "No route"))
        return HIBY_BT_ERR_NOT_FOUND;
    return HIBY_BT_ERR_PAIR_FAILED;
}

/* the digits in a line ("Confirm passkey 012345 (yes/no)") */
static void bt_digits_after(const char *p, char *out, size_t size)
{
    size_t n = 0;
    while (*p && !isdigit((unsigned char)*p)) p++;
    while (*p && isdigit((unsigned char)*p) && n + 1 < size) out[n++] = *p++;
    out[n] = '\0';
}

/* wait for the user while bluetoothctl goes on talking: 1 yes, 0 no,
 * -1 nobody answered, -2 the other side gave up (its line in why) */
static int bt_wait_answer(struct bt_sess *s, unsigned serial, int max_ms, char *code, size_t code_size,
                          char *why, size_t why_size)
{
    for (long until = bt_ms() + max_ms; bt_ms() < until; )
    {
        pthread_mutex_lock(&bt_ask_lock);
        bool got = bt_answered == serial;
        bool yes = bt_answer_yes;
        if (got && code) snprintf(code, code_size, "%s", bt_answer_code);
        pthread_mutex_unlock(&bt_ask_lock);
        if (got) return yes ? 1 : 0;
        char line[256];
        if (bt_sess_next(s, line, sizeof(line), 100) &&
            (strstr(line, "Failed to pair") || strstr(line, "Request canceled") || strstr(line, "Paired: no")))
        {
            snprintf(why, why_size, "%s", line);
            return -2;
        }
        if (s->out < 0) return -2;
    }
    return -1;
}

static bool bt_pair_once(const char *mac, const char *name, int *err, bool *user_no)
{
    struct bt_sess s = { 0 };
    s.in = s.out = -1;
    s.pid = btrx_spawn("exec bluetoothctl 2>&1", &s.in, &s.out);
    if (s.pid <= 0) { *err = HIBY_BT_ERR_HUNG; return false; }
    char line[256], cmd[48];
    bt_sess_say(&s, "agent KeyboardDisplay");
    bt_sess_say(&s, "default-agent");
    for (long until = bt_ms() + 3000; bt_ms() < until; )
        if (bt_sess_next(&s, line, sizeof(line), 200) && strstr(line, "efault agent")) break;
    snprintf(cmd, sizeof(cmd), "pair %s", mac);
    bt_sess_say(&s, cmd);
    bool ok = false, done = false;
    *err = HIBY_BT_ERR_PAIR_TIMEOUT;
    for (long until = bt_ms() + 45000; !done && bt_ms() < until; )
    {
        if (!bt_sess_next(&s, line, sizeof(line), 250))
        {
            if (s.out < 0) { *err = HIBY_BT_ERR_HUNG; break; }
            continue;
        }
        if (!line[0]) continue;
        if (s.logged < 40 && (strstr(line, "air") || strstr(line, "asskey") || strstr(line, "PIN") ||
                              strstr(line, "gent") || strstr(line, "rror") || strstr(line, "Failed") ||
                              strstr(line, "Request") || strstr(line, "Bonded")))
        {
            s.logged++;
            hiby_debug_log("pair: %.120s", line);
        }
        char code[20], why[160] = "";
        const char *p;
        if (strstr(line, "Pairing successful")) { ok = done = true; *err = HIBY_BT_ERR_NONE; }
        else if ((p = strstr(line, "Failed to pair")))
        {
            *err = bt_pair_error(p);
            ok = *err == HIBY_BT_ERR_NONE;
            done = true;
        }
        else if ((p = strstr(line, "Confirm passkey")) || (p = strstr(line, "Enter PIN code")) ||
                 (p = strstr(line, "Enter passkey")))
        {
            int kind = strstr(line, "Confirm passkey") ? HIBY_BT_ASK_CONFIRM :
                       strstr(line, "Enter PIN code") ? HIBY_BT_ASK_PIN : HIBY_BT_ASK_PASSKEY;
            if (kind == HIBY_BT_ASK_CONFIRM) bt_digits_after(p + 15, code, sizeof(code));
            else code[0] = '\0';
            unsigned serial = bt_ask_set(kind, mac, name, code);
            char typed[20] = "";
            int a = bt_wait_answer(&s, serial, 40000, typed, sizeof(typed), why, sizeof(why));
            bt_ask_clear();
            if (a == 1 && kind == HIBY_BT_ASK_CONFIRM) bt_sess_say(&s, "yes");
            else if (a == 1 && typed[0]) bt_sess_say(&s, typed);
            else if (a == -2)
            {
                *err = bt_pair_error(why);
                done = true;
            }
            else
            {
                /* no, or nobody: the request is turned down and the
                 * session ends (bluetoothctl has no way to cancel a PIN) */
                if (kind == HIBY_BT_ASK_CONFIRM) bt_sess_say(&s, "no");
                *user_no = a == 0;
                *err = a == 0 ? HIBY_BT_ERR_CANCELED : HIBY_BT_ERR_PAIR_TIMEOUT;
                done = true;
            }
        }
        else if (strstr(line, "Accept pairing") || strstr(line, "Authorize service"))
            bt_sess_say(&s, "yes");             /* asked for here: Just Works headphones */
        else if ((p = strstr(line, "Passkey:")) || (p = strstr(line, "PIN code:")))
        {
            bt_digits_after(p, code, sizeof(code));
            if (code[0]) bt_ask_set(strstr(line, "Passkey:") ? HIBY_BT_ASK_SHOW_PASSKEY : HIBY_BT_ASK_SHOW_PIN,
                                    mac, name, code);   /* typed on the other side: no answer */
        }
        else if (strstr(line, "Request canceled")) bt_ask_clear();
    }
    bt_ask_clear();
    if (ok)
    {
        snprintf(cmd, sizeof(cmd), "trust %s", mac);
        bt_sess_say(&s, cmd);
        for (long until = bt_ms() + 1500; bt_ms() < until; )
            if (bt_sess_next(&s, line, sizeof(line), 100) && strstr(line, "trust succeeded")) break;
    }
    bt_sess_say(&s, "quit");
    for (long until = bt_ms() + 800; s.out >= 0 && bt_ms() < until; )
        bt_sess_next(&s, line, sizeof(line), 100);
    btrx_end_child(&s.pid, 1000);
    if (s.in >= 0) close(s.in);
    if (s.out >= 0) close(s.out);
    hiby_debug_log("pair: %s %s (error %d)", mac, ok ? "paired" : "NOT paired", *err);
    return ok;
}

static bool bt_pair_session(const char *mac, const char *name)
{
    int err = HIBY_BT_ERR_NONE;
    bool user_no = false;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (bt_pair_once(mac, name, &err, &user_no)) { bt_last_err = HIBY_BT_ERR_NONE; return true; }
        /* earbuds that kept an old pairing turn a new one down, or fail
         * its keys: forget ours and ask once more */
        if (user_no || attempt || (err != HIBY_BT_ERR_PAIR_FAILED && err != HIBY_BT_ERR_PAIR_REJECTED))
            break;
        hiby_debug_log("pair: %s turned it down; removing old keys and trying once more", mac);
        bt_ctl_run("remove", mac, NULL);
        bt_sleep(HZ);
    }
    bt_last_err = err;
    return false;
}

/* ---------------------------------------------------------- reconnect */
int hiby_bt_reconnect(void)
{
    char mac[18], active[18];
    if (hiby_pcm_get_bt_mac()) return 1;
    if (!hiby_bt_last_device(mac)) return -1;
    if (!bt_is_enabled()) return -1;
    if (is_busy) return 0;
    bt_embedded_ui = true;
    int result;
    if (bt_get_active_mac(active, sizeof(active)))
    {
        /* something linked by itself: route to it, as the tick does */
        bt_autoconnection_route_to_bluetooth(active, true);
        result = hiby_pcm_get_bt_mac() ? 1 : 0;
    }
    else
    {
        is_busy = true;
        bool linked = bt_ctl_run_for("connect", mac, "Connection successful", 12000);
        if (linked)
        {
            struct bt_device d = { "", "", true };
            snprintf(d.mac, sizeof(d.mac), "%s", mac);
            bt_get_device_name(mac, d.name, sizeof(d.name));
            bt_set_selected(&d);
            result = bt_route_to_bluetooth(mac, NULL) ? 1 : 0;
            bt_last_err = result ? HIBY_BT_ERR_NONE : HIBY_BT_ERR_NO_AUDIO;
        }
        else
        {
            result = 0;
            bt_last_err = HIBY_BT_ERR_NOT_FOUND;
        }
        is_busy = false;
    }
    bt_embedded_ui = false;
    hiby_debug_log("reconnect: %s %s", mac, result ? "routed" : "not there");
    return result;
}

/* -------------------------------------------------------- SBC quality */
/* BlueALSA picks SBC's quality when it starts: XQ is Dual Channel at
 * bitpool 38 (452 kb/s at 44.1 kHz), XQ+ bitpool 47 (551 kb/s). The stock
 * 4.1.1 binary takes --sbc-quality=xq|xq+ and falls back to high quality
 * by itself with headphones that do not offer Dual Channel or the bitpool. */
int hiby_bt_sbc_quality(void)
{
    pthread_mutex_lock(&bt_cfg_lock);
    bt_cfg_load_locked();
    int q = bt_cfg.sbc;
    pthread_mutex_unlock(&bt_cfg_lock);
    return q;
}

static const char *bt_source_daemon(void)
{
    static char cmd[96];
    int q = hiby_bt_sbc_quality();
    snprintf(cmd, sizeof(cmd), "%s%s", BTRX_SOURCE, q == 2 ? " --sbc-quality=xq+" : q == 1 ? " --sbc-quality=xq" : "");
    return cmd;
}

/* the running BlueALSA started with the quality asked for? */
static bool bt_source_matches(void)
{
    char line[256] = "";
    FILE *fp = bt_popen("pgrep -a -x bluealsa 2>/dev/null", "r");   /* busybox: "PID bluealsa -p ..." */
    if (fp)
    {
        if (!fgets(line, sizeof(line), fp)) line[0] = '\0';
        bt_pclose(fp);
    }
    if (!line[0] || strstr(line, "a2dp-sink")) return true;   /* none, or receiving: not ours to touch now */
    int q = hiby_bt_sbc_quality();
    const char *want = q == 2 ? "--sbc-quality=xq+" : q == 1 ? "--sbc-quality=xq" : NULL;
    if (!want) return !strstr(line, "--sbc-quality=");
    const char *at = strstr(line, want);
    return at && (at[strlen(want)] == ' ' || at[strlen(want)] == '\0' || at[strlen(want)] == '\n');
}

static bool bt_source_restart(void)
{
    char routed[18] = "";
    const char *r = hiby_pcm_get_bt_mac();
    if (r)
    {
        snprintf(routed, sizeof(routed), "%s", r);
        for (char *c = routed; *c; c++) if (*c == '_') *c = ':';
        bt_route_to_local();
    }
    bool ok = btrx_bluealsa(bt_source_daemon()) >= 0;
    hiby_debug_log("sbc: BlueALSA restarted as '%s': %s", bt_source_daemon(), ok ? "ok" : "FAILED");
    if (ok && routed[0] && bt_ctl_run("connect", routed, "Connection successful"))
        bt_route_to_bluetooth(routed, NULL);
    return ok;
}

bool hiby_bt_set_sbc_quality(int quality)
{
    if (quality < 0 || quality > 2) return false;
    pthread_mutex_lock(&bt_cfg_lock);
    bt_cfg_load_locked();
    bt_cfg.sbc = quality;
    bt_cfg_save_locked();
    pthread_mutex_unlock(&bt_cfg_lock);
    if (btrx_on || !bt_is_enabled()) return true;   /* taken when source mode next starts */
    return bt_source_matches() || bt_source_restart();
}

/* --------------------------------------------- receive: the codec asked */
void hiby_btrx_codec(const char *codec)
{
    pthread_mutex_lock(&btrx_lock);
    snprintf(btrx_codec_want, sizeof(btrx_codec_want), "%s", codec);
    pthread_mutex_unlock(&btrx_lock);
}

/* ============================================ AirPods: battery and noise */
/* Apple's accessory protocol (AAP) runs on L2CAP PSM 0x1001 beside the
 * audio. Its packets are documented by the LibrePods project (AAP
 * Definitions, AirPods Pro 2 firmware 7A305): a handshake, a request for
 * notifications, then battery (04 00 04 00 04 00 ...), noise control
 * (04 00 04 00 09 00 0D mode) and ear packets come in, and the same noise
 * packet sent changes the mode. It is the kernel's L2CAP socket, not
 * BlueZ, so it holds on BlueZ 5.54; what it needs from the R1 is the
 * kernel's Bluetooth sockets (bluetoothd uses them) and AirPods that
 * answer a non-Apple host, which LibrePods reports they do on Linux. */
#define AAP_PSM 0x1001
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0                   /* the Mac the tests run on */
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
static pthread_mutex_t aap_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hiby_bt_aap aap_st = { .left = -1, .right = -1, .box = -1 };
static char aap_mac[18];
static int aap_fd = -1;
static volatile bool aap_running, aap_stop;

/* one packet from the AirPods; any thread (the test calls it straight) */
void hiby_bt_aap_parse(const uint8_t *p, int n)
{
    if (n < 7 || p[0] != 0x04 || p[1] != 0x00 || p[2] != 0x04 || p[3] != 0x00) return;
    pthread_mutex_lock(&aap_lock);
    if (p[4] == 0x04 && p[5] == 0x00)               /* battery: count, then 5 bytes each */
    {
        int count = p[6];
        for (int i = 0; i < count && 7 + i * 5 + 4 < n; i++)
        {
            const uint8_t *c = p + 7 + i * 5;
            int level = c[2] <= 100 ? c[2] : -1;
            bool charging = c[3] == 0x01, gone = c[3] == 0x04;
            if (gone) level = -1;
            if (c[0] == 0x04) { aap_st.left = level; aap_st.left_charging = charging; }
            else if (c[0] == 0x02) { aap_st.right = level; aap_st.right_charging = charging; }
            else if (c[0] == 0x08) { aap_st.box = level; aap_st.box_charging = charging; }
        }
        aap_st.up = true;
    }
    else if (p[4] == 0x09 && p[5] == 0x00 && p[6] == 0x0d && n >= 8)   /* noise control */
    {
        if (p[7] >= 1 && p[7] <= 4) aap_st.noise = p[7];
        aap_st.up = true;
    }
    else if (p[4] == 0x06 && p[5] == 0x00 && n >= 8)   /* ear: 0 in, 1 out, 2 in the case */
    {
        aap_st.in_ear = p[6] == 0x00 || p[7] == 0x00;
    }
    pthread_mutex_unlock(&aap_lock);
}

void hiby_bt_aap_status(struct hiby_bt_aap *out)
{
    pthread_mutex_lock(&aap_lock);
    *out = aap_st;
    pthread_mutex_unlock(&aap_lock);
}

bool hiby_bt_aap_noise(int mode)
{
    if (mode < 1 || mode > 4) return false;
    uint8_t pkt[] = { 0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0d, (uint8_t)mode, 0x00, 0x00, 0x00 };
    pthread_mutex_lock(&aap_lock);
    bool ok = aap_fd >= 0 && send(aap_fd, pkt, sizeof(pkt), MSG_NOSIGNAL) == (ssize_t)sizeof(pkt);
    pthread_mutex_unlock(&aap_lock);
    hiby_debug_log("aap: noise control %d %s", mode, ok ? "sent" : "NOT sent (no AAP link)");
    return ok;
}

/* the kernel's Bluetooth L2CAP socket address (bluetooth/l2cap.h) */
struct aap_sockaddr_l2
{
    unsigned short l2_family;
    unsigned short l2_psm;
    uint8_t l2_bdaddr[6];                /* least significant byte first */
    unsigned short l2_cid;
    uint8_t l2_bdaddr_type;
};
#define AAP_AF_BLUETOOTH 31
#define AAP_BTPROTO_L2CAP 0

static int aap_open(const char *mac)
{
    int fd = socket(AAP_AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, AAP_BTPROTO_L2CAP);
    if (fd < 0) { hiby_debug_log("aap: no L2CAP socket: %s", strerror(errno)); return -1; }
    struct aap_sockaddr_l2 a;
    memset(&a, 0, sizeof(a));
    a.l2_family = AAP_AF_BLUETOOTH;
    a.l2_psm = AAP_PSM;                  /* little-endian, as the R1 is */
    unsigned b[6];
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) { close(fd); return -1; }
    for (int i = 0; i < 6; i++) a.l2_bdaddr[i] = (uint8_t)b[5 - i];
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0 && errno != EINPROGRESS)
    {
        hiby_debug_log("aap: connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    struct pollfd pfd = { fd, POLLOUT, 0 };
    int err = 0;
    socklen_t len = sizeof(err);
    if (poll(&pfd, 1, 6000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err)
    {
        hiby_debug_log("aap: no answer on PSM 0x1001: %s", err ? strerror(err) : "timed out");
        close(fd);
        return -1;
    }
    static const uint8_t hello[] = { 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t features[] = { 0x04, 0x00, 0x04, 0x00, 0x4d, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t notify[] = { 0x04, 0x00, 0x04, 0x00, 0x0f, 0x00, 0xff, 0xff, 0xff, 0xff };
    if (send(fd, hello, sizeof(hello), MSG_NOSIGNAL) < 0 || send(fd, features, sizeof(features), MSG_NOSIGNAL) < 0 ||
        send(fd, notify, sizeof(notify), MSG_NOSIGNAL) < 0)
    {
        hiby_debug_log("aap: handshake not taken: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void *aap_thread(void *arg)
{
    (void)arg;
    hiby_bt_worker_thread();
    char mac[18];
    pthread_mutex_lock(&aap_lock);
    snprintf(mac, sizeof(mac), "%s", aap_mac);
    pthread_mutex_unlock(&aap_lock);
    hiby_debug_log("aap: %s is Apple's: asking for battery and noise control", mac);
    for (int tries = 0; tries < 3 && !aap_stop; tries++)
    {
        int fd = aap_open(mac);
        if (fd < 0) { for (int i = 0; i < 30 && !aap_stop; i++) usleep(100000); continue; }
        pthread_mutex_lock(&aap_lock);
        aap_fd = fd;
        pthread_mutex_unlock(&aap_lock);
        hiby_debug_log("aap: connected");
        tries = 0;
        uint8_t buf[512];
        while (!aap_stop && bt_same_mac(hiby_pcm_get_bt_mac(), mac))
        {
            struct pollfd pfd = { fd, POLLIN, 0 };
            int r = poll(&pfd, 1, 1000);
            if (r < 0 && errno != EINTR) break;
            if (r <= 0) continue;
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) { hiby_debug_log("aap: link closed"); break; }
            hiby_bt_aap_parse(buf, (int)n);
        }
        pthread_mutex_lock(&aap_lock);
        aap_fd = -1;
        pthread_mutex_unlock(&aap_lock);
        close(fd);
        if (!bt_same_mac(hiby_pcm_get_bt_mac(), mac)) break;
    }
    pthread_mutex_lock(&aap_lock);
    memset(&aap_st, 0, sizeof(aap_st));
    aap_st.left = aap_st.right = aap_st.box = -1;
    pthread_mutex_unlock(&aap_lock);
    aap_running = false;
    return NULL;
}

/* routed to Apple's headphones (its vendor in the Device ID, or the name) */
static void bt_aap_maybe_start(const char *mac)
{
    if (aap_running) return;
    char cmd[64], line[160];
    bool apple = false;
    snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", mac);
    FILE *fp = bt_popen(cmd, "r");
    while (fp && fgets(line, sizeof(line), fp))
        if (strstr(line, "Modalias: bluetooth:v004C") || strstr(line, "AirPods") || strstr(line, "Beats"))
            apple = true;
    if (fp) bt_pclose(fp);
    if (!apple) return;
    pthread_mutex_lock(&aap_lock);
    snprintf(aap_mac, sizeof(aap_mac), "%s", mac);
    pthread_mutex_unlock(&aap_lock);
    aap_stop = false;
    aap_running = true;
    pthread_t t;
    if (pthread_create(&t, NULL, aap_thread, NULL) == 0) pthread_detach(t);
    else aap_running = false;
}

#endif /* HIBY_LINUX */
