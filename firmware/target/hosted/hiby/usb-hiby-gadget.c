/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2018 by Marcin Bukat
 * Copyright (C) 2025 by Melissa Autumn
 * Copyright (C) 2026 by Michael McAllister
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

#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "kernel.h"
#include "dir.h"
#include "disk.h"
#include "usb.h"
#include "sysfs.h"
#include "power.h"
#include "usb-hiby-gadget.h"
#include "../crashlog-linux.h"

#include "audio.h"
#include "pcm.h"
#include "pcm_sampr.h"
#include "usb_ch9.h"
#include "usb-dac-hiby.h"

#ifdef HAVE_GENERAL_PURPOSE_LED
#include "led-general-purpose.h"
#endif

#define LOGF_ENABLE
#include "logf.h"
#include <stdio.h>

/* Configfs root of the gadget created by usb_init_device() */
#define USB_GADGET_PATH   "/sys/kernel/config/usb_gadget/adb_demo"

static int _usb_mode = -1;
static bool _usb_init = false;
static bool is_adb_running = false;
static int _usb_audio = 0;   /* usb_audio setting: 0 never .. 3 while mass-storage */
static const char *usb_mode_name(int mode);

void enable_adb(void);
void enable_mass_storage(void);
void enable_charging(void);
bool enable_usb_audio(void);

void disable_adb(void);
void disable_mass_storage(void);
void disable_usb_audio(void);

#ifdef HAVE_MULTIDRIVE
void cleanup_rbhome(void);
void startup_rbhome(void);
#endif

/* Bring up the gadget for a base USB mode (mass storage / charge / adb). */
static void configure_usb_mode(int mode)
{
    switch(mode) {
    case USB_MODE_MASS_STORAGE:
        logf("Enabling Mass Storage\n");
        enable_mass_storage();
        break;
    case USB_MODE_CHARGE:
        logf("Enabling Charge\n");
        enable_charging();
        break;
    case USB_MODE_ADB:
        logf("Enabling ADB\n");
        enable_adb();
        break;
    default:
        break;
    }
}

/* Whether the USB-DAC function should be running, given the usb_audio
 * setting (never/always/while_charge_only/while_mass_storage) and the
 * current base USB mode. */
static bool dac_should_run(void)
{
    switch (_usb_audio) {
    case 1: return true;                                /* always */
    case 2: return _usb_mode == USB_MODE_CHARGE;        /* while charge-only */
    case 3: return _usb_mode == USB_MODE_MASS_STORAGE;  /* while mass-storage */
    default: return false;                              /* never */
    }
}

/* Reconcile the DAC gadget with the setting + current mode.
 *
 * In adb mode the DAC rides alongside adb as a composite function, so we
 * rebuild the adb gadget (enable_adb() composes uac_sa when dac_should_run()).
 * In charge/mass-storage mode the DAC is a standalone self-bound gadget. */
static void apply_usb_audio(void)
{
    if (_usb_mode == USB_MODE_ADB) {
        enable_adb();
        return;
    }

    bool want = dac_should_run();
    bool active = usb_audio_get_active();

    if (want && !active) {
        /* enable_usb_audio() returns false if applied before audio_init()
         * (PCM mixer not up yet -- e.g. at boot) or if uac_sa never comes
         * up. Either way restore the base mode so we never leave a dead
         * gadget behind. */
        if (!enable_usb_audio())
            configure_usb_mode(_usb_mode);
    } else if (!want && active) {
        disable_usb_audio();
        configure_usb_mode(_usb_mode);
    }
}

/* usb_set_audio() is the target's implementation of the usb_audio setting
 * callback declared in usb.h (HAVE_HOST_USB_AUDIO); it drives the DAC gadget
 * up or down. */
void usb_set_audio(int value)
{
    _usb_audio = value;
    apply_usb_audio();
}

/* Compose uac_sa into the adb config, linked FIRST so the audio interfaces
 * are 0-1 (the vendor uac_sa function hard-codes them), with the DAC VID/PID
 * + IAD descriptor. Returns true if it composed the DAC. Skipped before
 * audio_init() (no PCM mixer yet); re-applied once it is up. */
static bool adb_compose_dac(void)
{
    if (!dac_should_run() || !pcm_is_initialized())
        return false;

    sysfs_set_string(USB_GADGET_PATH "/idVendor", "0x" USB_VID_STR);
    sysfs_set_string(USB_GADGET_PATH "/idProduct", "0x0004");
    sysfs_set_int(USB_GADGET_PATH "/bDeviceClass", USB_CLASS_MISC);
    sysfs_set_int(USB_GADGET_PATH "/bDeviceSubClass", 2);
    sysfs_set_int(USB_GADGET_PATH "/bDeviceProtocol", 1);

    system("mkdir -p " USB_GADGET_PATH "/functions/uac_sa.a");
    sysfs_set_int(USB_GADGET_PATH "/functions/uac_sa.a/c_chmask", 3);
    sysfs_set_int(USB_GADGET_PATH "/functions/uac_sa.a/c_ssize", 2);
    sysfs_set_int(USB_GADGET_PATH "/functions/uac_sa.a/c_srate", USB_DAC_SAMPLE_RATE);
    system("ln -sf " USB_GADGET_PATH "/functions/uac_sa.a " USB_GADGET_PATH "/configs/c.1/");
    sysfs_set_string(USB_GADGET_PATH "/configs/c.1/strings/0x409/configuration", "uac,adb");
    return true;
}

void hiby_set_usb_mode(int mode) {
    logf(">>>>>>>>>>>>>>>>> hiby_set_usb_mode(%d)\n", mode);
    if (!_usb_init) {
        logf("Need to init usb!\n");
        usb_init_device();
    }

    if (_usb_mode == mode)
        return;
    usb_trace("mode: %s -> %s", usb_mode_name(_usb_mode), usb_mode_name(mode));

    /* Drop any active DAC before reconfiguring the base gadget. */
    if (usb_audio_get_active())
        disable_usb_audio();

    /* Set the mode first so enable_adb()/dac_should_run() see it. */
    _usb_mode = mode;
    configure_usb_mode(mode);

    /* adb composes the DAC itself in enable_adb(); charge/mass-storage get
     * the standalone self-bound DAC here. */
    if (mode != USB_MODE_ADB)
        apply_usb_audio();
}

/* USB power alone does not mean a computer: a wall charger gives power
 * too, and treating it as a connection put the player into the USB screen
 * (and exported the card) until it was unplugged. In transfer-files mode a
 * computer is looked for first: the gadget is bound with no medium in its
 * LUN, and a host resets and addresses the bus within a moment, which the
 * UDC reports in its state file; a charger never does. The probe runs from
 * the usb tick, one small step per call, and is decided once per plug.
 * If the state file cannot be read, it falls back to trusting power. */
#define USB_UDC_NAME       "13500000.otg_new"
#define USB_UDC_STATE      "/sys/class/udc/" USB_UDC_NAME "/state"
#define USB_HOST_WAIT      (3 * HZ)
enum { PROBE_IDLE, PROBE_RUNNING, PROBE_HOST, PROBE_CHARGER };
static int usb_probe = PROBE_IDLE;
static const char *udc_state_now(void);
static long usb_probe_until;

static void usb_probe_bind(bool on)
{
    if (on)
        sysfs_set_string(USB_GADGET_PATH "/functions/mass_storage.0/lun.0/file", "\n");
    sysfs_set_string(USB_GADGET_PATH "/UDC", on ? USB_UDC_NAME : "\n");
}

/* 1: a host has reset or addressed the bus, 0: not yet, -1: unknown */
static int usb_probe_host(void)
{
    char state[32] = "";
    if (!sysfs_get_string(USB_UDC_STATE, state, sizeof(state)))
        return -1;
    return !strncmp(state, "default", 7) || !strncmp(state, "addressed", 9) ||
           !strncmp(state, "configured", 10);
}

static const char *usb_mode_name(int mode)
{
    switch (mode)
    {
    case USB_MODE_MASS_STORAGE: return "transfer files";
    case USB_MODE_CHARGE:       return "charge only";
    case USB_MODE_ADB:          return "adb";
    default:                    return "not set yet";
    }
}

static int usb_traced_wait;              /* this plug's reason for no USB mode is noted: 1 no mode yet, 2 another mode */
int usb_detect(void)
{
#ifdef HAVE_GENERAL_PURPOSE_LED
    led_hw_on();
#endif
    static int power_was = -1;
    int power = power_input_status() == POWER_INPUT_USB_CHARGER;
    if (power != power_was)
    {
        if (power_was >= 0 || power)
            usb_trace("cable: USB power %s (UDC %s, mode %s)", power ? "in" : "out",
                      udc_state_now(), usb_mode_name(_usb_mode));
        power_was = power;
        usb_traced_wait = 0;
    }
    if (!power)
    {
        if (usb_probe == PROBE_RUNNING)
            usb_probe_bind(false);
        usb_probe = PROBE_IDLE;
        return USB_EXTRACTED;
    }
    /* No mode yet (early boot): decide once the setting is applied. */
    if (_usb_mode < 0)
    {
        if (!usb_traced_wait)
            usb_trace("detect: plugged in before the USB setting was applied: waiting for it");
        usb_traced_wait = 1;
        return USB_EXTRACTED;
    }
    /* Other modes keep their own handling: charge only, adb, the DAC. */
    if (_usb_mode != USB_MODE_MASS_STORAGE || usb_audio_get_active())
    {
        if (usb_traced_wait != 2)
            usb_trace("detect: mode %s%s: no card for the computer", usb_mode_name(_usb_mode),
                      usb_audio_get_active() ? ", USB DAC on" : "");
        usb_traced_wait = 2;
        return USB_INSERTED;
    }

    switch (usb_probe)
    {
    case PROBE_IDLE:
        if (access(USB_UDC_STATE, R_OK) != 0 ||
            access(USB_GADGET_PATH "/functions/mass_storage.0/lun.0/file", W_OK) != 0)
        {
            usb_trace("probe: no UDC state or LUN file (%s): taken for a computer", udc_state_now());
            usb_probe = PROBE_HOST;         /* cannot tell: behave as before */
            return USB_INSERTED;
        }
        usb_probe_bind(true);
        usb_probe_until = current_tick + USB_HOST_WAIT;
        usb_probe = PROBE_RUNNING;
        usb_trace("probe: gadget bound with no medium, waiting up to %d s for a computer (UDC %s)",
                  USB_HOST_WAIT / HZ, udc_state_now());
        return USB_EXTRACTED;
    case PROBE_RUNNING:
    {
        int host = usb_probe_host();
        if (host != 0 || TIME_AFTER(current_tick, usb_probe_until))
        {
            const char *state = udc_state_now();
            long waited = (current_tick - (usb_probe_until - USB_HOST_WAIT)) * 1000 / HZ;
            /* unbind either way: a host is bound again, with the card in
             * the LUN, by usb_enable() once the usb thread takes over */
            usb_probe_bind(false);
            usb_probe = host != 0 ? PROBE_HOST : PROBE_CHARGER;
            logf("usb probe: %s", usb_probe == PROBE_HOST ? "computer" : "charger");
            if (usb_probe == PROBE_HOST)
                usb_trace("probe: a computer after %ld ms (UDC %s): asking for USB mode", waited, state);
            else
                usb_trace("probe: no computer in %ld ms (UDC %s): a charger, so NO USB mode until unplugged",
                          waited, state);
            return usb_probe == PROBE_HOST ? USB_INSERTED : USB_EXTRACTED;
        }
        return USB_EXTRACTED;
    }
    case PROBE_HOST:
        return USB_INSERTED;
    default:                                /* a charger: stay usable */
        return USB_EXTRACTED;
    }
}

/* ---- the card during USB mode ----
 * Mass storage hands the card's block device to the computer. For as long
 * as the player keeps the card mounted too, two exFAT drivers write one
 * volume: on 2026-10-21 the player wrote a font's glyph cache after the
 * handover, the directory ended up with an entry pointing at cluster 0,
 * the kernel remounted the card read-only and every file create failed
 * ("Error accessing playlist control file"). So the card is unmounted
 * before it is put in the LUN, and mounted again the same way (type and
 * options read from /proc/mounts) once the computer lets go. A card that
 * will not come off (a file still open somewhere: EBUSY) is NOT exported:
 * the computer sees a drive with no medium, and usb_card_handover_error()
 * tells the UI why, for sloppy.log. */
#define CARD_LUN_FILE  USB_GADGET_PATH "/functions/mass_storage.0/lun.0/file"
#ifndef MS_RELATIME                      /* an older libc's <sys/mount.h> */
#define MS_RELATIME    (1 << 21)
#endif
#define CARD_TRIES     30                /* x HZ/10: 3 s for the gadget to let go of the card */
#define CARD_OFF_TRIES 12                /* x 0.4 s: ~5 s for other threads to close files */

static bool card_off;                    /* unmounted, the computer's for now */
/* From the moment the handover starts until the card is mounted again
 * nothing may write to it: the file layer (filesystem-app.c) refuses card
 * writes and our own workers see no card (sl_net_card_root). Written here
 * only, read from any thread. */
static volatile bool card_leaving;
bool usb_card_away(void) { return card_leaving || card_off; }
static int card_errno = -1;              /* the last handover: 0 clean, else umount's errno, -1 never tried */
static char card_busy[512];              /* what was still on the card when it would not come off */
static char card_dev[40] = "";
static char card_fs[16] = "";
static char card_opts[200] = "";
static unsigned long card_flags;
static bool card_retrying;               /* this connect's handover failed: usb_card_retry() tries again */

/* ---- the USB trace ----
 * On 2026-10-23 the R1 showed the USB screen and the Mac saw no device at
 * all, and nothing said why: sloppy.log stops at "usb: USB mode" (the card
 * goes away) and usb-busy.txt only speaks when umount fails. So every step
 * is noted here, timed by uptime (which survives the player restarting
 * after USB mode) and UTC, in a RAM file first: a step taken while the card
 * is the computer's is kept there and copied onto the card the next time
 * it is the player's. Raw calls only: usb_detect() runs from the tick. */
#ifndef USB_TRACE_RAM                    /* the test has its own */
#define USB_TRACE_RAM       "/tmp/usb-trace.txt"
#define USB_TRACE_OFF       "/tmp/usb-trace.off"     /* how much of it is on the card already */
#define USB_TRACE_CARD      PIVOT_ROOT "/.rockbox/sloppy/usb-trace.txt"
#define USB_TRACE_CARD_PREV PIVOT_ROOT "/.rockbox/sloppy/usb-trace.prev.txt"
#endif
#define USB_TRACE_MAX       (128 * 1024)              /* the card's copy, before it starts over */
static volatile bool usb_trace_dirty = true;          /* a run before this one may have left lines */

void usb_trace(const char *fmt, ...)
{
    char b[400];
    struct timespec up, now;
    struct tm tm;
    clock_gettime(CLOCK_MONOTONIC, &up);
    clock_gettime(CLOCK_REALTIME, &now);
    time_t t = now.tv_sec;
    gmtime_r(&t, &tm);
    int n = snprintf(b, sizeof b, "[up %5ld.%03ld %02d:%02d:%02d UTC] ", (long)up.tv_sec,
                     up.tv_nsec / 1000000, tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2)
        n = sizeof b - 2;
    b[n++] = '\n';
    int fd = openat(AT_FDCWD, USB_TRACE_RAM, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write(fd, b, n) == n)
        usb_trace_dirty = true;
    close(fd);
}

/* The RAM lines not on the card yet go onto it. Only while the card is the
 * player's; Rockbox threads take turns on one OS thread, so this never
 * runs in the middle of a handover. */
void usb_trace_flush(void)
{
    if (!usb_trace_dirty || card_off)
        return;
    usb_trace_dirty = false;
    int in = openat(AT_FDCWD, USB_TRACE_RAM, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return;
    char b[4096];
    long off = 0;
    int o = openat(AT_FDCWD, USB_TRACE_OFF, O_RDONLY | O_CLOEXEC);
    if (o >= 0)
    {
        ssize_t got = read(o, b, 31);
        b[got > 0 ? got : 0] = '\0';
        off = atol(b);
        close(o);
    }
    struct stat st, cs;
    if (fstat(in, &st) != 0)
    {
        close(in);
        return;
    }
    if (st.st_size < off)                /* a fresh RAM file: a reboot, or it was cut short */
        off = 0;
    if (st.st_size == off)
    {
        close(in);
        return;
    }
    if (fstatat(AT_FDCWD, USB_TRACE_CARD, &cs, 0) == 0 && cs.st_size > USB_TRACE_MAX)
        renameat(AT_FDCWD, USB_TRACE_CARD, AT_FDCWD, USB_TRACE_CARD_PREV);
    int out = openat(AT_FDCWD, USB_TRACE_CARD, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (out < 0)
    {
        close(in);
        usb_trace_dirty = true;          /* try again next time */
        return;
    }
    ssize_t got;
    while ((got = pread(in, b, sizeof b, off)) > 0 && write(out, b, got) == got)
        off += got;
    close(out);
    close(in);
    if (off > 4 * USB_TRACE_MAX)         /* RAM is small: start the RAM copy over, it is all on the card */
    {
        int cut = openat(AT_FDCWD, USB_TRACE_RAM, O_WRONLY | O_TRUNC | O_CLOEXEC);
        if (cut >= 0) close(cut);
        off = 0;
    }
    o = openat(AT_FDCWD, USB_TRACE_OFF, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (o >= 0)
    {
        int n = snprintf(b, sizeof b, "%ld\n", off);
        if (write(o, b, n) != n) usb_trace_dirty = true;
        close(o);
    }
}

/* The UDC as the kernel has it: "not attached", "default", "addressed",
 * "configured" (the computer took it), "suspended" ... */
static const char *udc_state_now(void)
{
    static char state[32];
    if (!sysfs_get_string(USB_UDC_STATE, state, sizeof state))
        strcpy(state, "unreadable");
    return state;
}

/* Bind the gadget to the UDC, or unbind it, and check that it took: a
 * refused write only shows when the file is closed, which
 * sysfs_set_string() does not look at. Tried three times; false (and a
 * line in the trace) when the UDC file never reads back what was asked. */
static bool udc_bind(bool on, const char *why)
{
    char now[64] = "";
    for (int tries = 0; tries < 3; tries++)
    {
        sysfs_set_string(USB_GADGET_PATH "/UDC", on ? USB_UDC_NAME : "\n");
        if (!sysfs_get_string(USB_GADGET_PATH "/UDC", now, sizeof now))
            return true;                 /* cannot tell */
        if (on ? !strcmp(now, USB_UDC_NAME) : !now[0])
            return true;
        usb_trace("%s: the UDC file reads '%s' after %s, try %d", why, now, on ? "binding" : "unbinding", tries + 1);
        if (on)
            sysfs_set_string(USB_GADGET_PATH "/UDC", "\n");   /* bound elsewhere or half way: start over */
    }
    usb_trace("%s: the gadget could NOT be %s", why, on ? "bound: the computer sees no device" : "unbound");
    return false;
}

/* While the USB screen is up (usb_card_retry, every half second): whether
 * the computer took the device, as the UDC's state says. One that has not
 * been configured some seconds after binding gets the gadget bound again
 * once, as a cable pulled and put back would. */
#define UDC_REBIND_AFTER (8 * HZ)
static long udc_bound_at;                /* 0: not bound for USB mode */
static bool udc_rebound;
static char udc_seen[32];
static void udc_follow(void)
{
    if (!udc_bound_at)
        return;
    const char *state = udc_state_now();
    long ms = (current_tick - udc_bound_at) * 1000 / HZ;
    if (strcmp(state, udc_seen))
    {
        usb_trace("host: UDC %s, %ld ms after binding", state, ms);
        snprintf(udc_seen, sizeof udc_seen, "%s", state);
    }
    if (!udc_rebound && strcmp(state, "configured") && TIME_AFTER(current_tick, udc_bound_at + UDC_REBIND_AFTER))
    {
        udc_rebound = true;
        usb_trace("host: not configured %ld ms after binding: binding the gadget again", ms);
        udc_bind(false, "rebind");
        udc_bind(true, "rebind");
        udc_bound_at = current_tick;
        udc_seen[0] = '\0';
    }
}

/* Where the card is, as the kernel writes it: /data is a link to usr/data
 * on the HiBy system, and /proc/mounts, /proc/<pid>/fd and getcwd() all
 * give the resolved path, so PIVOT_ROOT itself matches none of them. */
static const char *card_root(void)
{
    static char real[PATH_MAX];
    if (!real[0] && !realpath(PIVOT_ROOT, real))
        strcpy(real, PIVOT_ROOT);
    return real;
}
/* path is the card, or something on it (by either name) */
static bool on_card(const char *path)
{
    const char *roots[2] = { card_root(), PIVOT_ROOT };
    for (int i = 0; i < 2; i++)
    {
        size_t n = strlen(roots[i]);
        if (!strncmp(path, roots[i], n) && (!path[n] || path[n] == '/'))
            return true;
    }
    return false;
}

/* The card's device, filesystem type and options, from /proc/mounts: the
 * last line for PIVOT_ROOT is the mount on top. False when none is found. */
static bool card_mount_read(void)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return false;
    bool found = false;
    char line[400];
    while (fgets(line, sizeof line, fp))
    {
        char dev[40], mnt[120], fs[16], opts[200];
        if (sscanf(line, "%39s %119s %15s %199s", dev, mnt, fs, opts) != 4)
            continue;
        if (strcmp(mnt, PIVOT_ROOT) && strcmp(mnt, card_root()))
            continue;
        strcpy(card_dev, dev);
        strcpy(card_fs, fs);
        /* flag words become mount(2) flags; the rest is the driver's data */
        static const struct { const char *name; unsigned long flag; } words[] = {
            { "ro", MS_RDONLY }, { "nosuid", MS_NOSUID }, { "nodev", MS_NODEV },
            { "noexec", MS_NOEXEC }, { "sync", MS_SYNCHRONOUS }, { "dirsync", MS_DIRSYNC },
            { "noatime", MS_NOATIME }, { "nodiratime", MS_NODIRATIME }, { "relatime", MS_RELATIME },
            { "rw", 0 },
        };
        card_flags = 0;
        card_opts[0] = '\0';
        for (char *tok = strtok(opts, ","); tok; tok = strtok(NULL, ","))
        {
            bool word = false;
            for (unsigned i = 0; i < sizeof words / sizeof words[0]; i++)
                if (!strcmp(tok, words[i].name)) { card_flags |= words[i].flag; word = true; break; }
            if (word)
                continue;
            size_t have = strlen(card_opts);
            if (have + strlen(tok) + 2 > sizeof card_opts)
                continue;
            if (have) card_opts[have++] = ',';
            strcpy(card_opts + have, tok);
        }
        found = true;
    }
    fclose(fp);
    return found;
}

/* Mount the card back where it was. 0 on success, else errno. */
static int card_mount(void)
{
    /* read-write first, even if the kernel had turned it read-only after
     * errors: the computer may just have repaired it */
    if (card_fs[0] && mount(card_dev, PIVOT_ROOT, card_fs, card_flags & ~MS_RDONLY, card_opts[0] ? card_opts : NULL) == 0)
        return 0;
    if (card_fs[0] && (card_flags & MS_RDONLY) &&
        mount(card_dev, PIVOT_ROOT, card_fs, card_flags, card_opts[0] ? card_opts : NULL) == 0)
        return 0;
    int err = card_fs[0] ? errno : 0;     /* no mount tried: errno is someone else's */
    if (err == EBUSY)                     /* the gadget still holds the device */
        return err;
    /* not as before: the kinds a HiBy card can be, with the driver's defaults */
    static const char *const fs[] = { "exfat", "vfat" };
    for (unsigned i = 0; i < sizeof fs / sizeof fs[0]; i++)
        if (mount(card_dev, PIVOT_ROOT, fs[i], 0, NULL) == 0)
            return 0;
    return err ? err : errno;
}

int usb_card_handover_error(void)
{
    return card_errno;
}
/* What one of our descriptors is open on, as the kernel names it */
static bool card_fd_path(int fd, char *out, size_t size)
{
#ifdef F_GETPATH                         /* the Mac test */
    char buf[PATH_MAX];
    if (fcntl(fd, F_GETPATH, buf) == -1 || strlen(buf) >= size)
        return false;
    strcpy(out, buf);
    return true;
#else
    char link[40];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, size - 1);
    if (n <= 0)
        return false;
    out[n] = '\0';
    return true;
#endif
}

/* An empty folder off the card, opened once: a folder of ours left open on
 * the card is pointed here for the handover */
#ifndef CARD_AWAY_DIR                    /* the test has its own */
#define CARD_AWAY_DIR  "/tmp/.card-away"
#endif
static int card_away(void)
{
    static int away = -1;
    char path[PATH_MAX];
    if (away >= 0)
        return away;
    mkdirat(AT_FDCWD, CARD_AWAY_DIR, 0700);  /* openat/mkdirat: Rockbox's own open and mkdir would put it on the card */
    away = openat(AT_FDCWD, CARD_AWAY_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (away >= 0 && (!card_fd_path(away, path, sizeof path) || on_card(path)))
    {
        logf("%s is on the card", CARD_AWAY_DIR);
        close(away);                     /* it would pin the card itself */
        away = -1;
    }
    return away;
}

/* Folders of ours still open on the card. On 2026-10-23 a handover failed
 * twice with only a folder of ours on it (/.rockbox, then the card's root;
 * every thread listed it because threads share descriptors and working
 * directory), for the whole session and not after it. A folder pins the card as surely as a file, but unlike a file
 * nothing is lost by letting it go: each is pointed (dup2) at an empty folder
 * off the card, so whoever holds it reads no more entries and closes it as
 * usual, and the card can come off. Named in card_let_go so the holder can
 * be found. */
static char card_let_go[200];
static int card_let_go_dirs(void)
{
    int n = 0;
    card_let_go[0] = '\0';
    for (int fd = 0; fd < 1024; fd++)
    {
        struct stat st;
        char path[PATH_MAX];
        if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || !card_fd_path(fd, path, sizeof path) || !on_card(path))
            continue;
        int away = card_away();
        if (away < 0 || fd == away)
            break;
        int flags = fcntl(fd, F_GETFD);
        if (dup2(away, fd) < 0)
            continue;
        if (flags >= 0)
            fcntl(fd, F_SETFD, flags);
        n++;
        size_t have = strlen(card_let_go);
        size_t root = strlen(!strncmp(path, PIVOT_ROOT, strlen(PIVOT_ROOT)) ? PIVOT_ROOT : card_root());
        snprintf(card_let_go + have, sizeof card_let_go - have, "%sfd %d:%s", have ? " " : "", fd,
                 path[root] ? path + root : "/");
    }
    return n;
}

/* A line in .rockbox/sloppy/usb-busy.txt, written while the card is still
 * the player's: the player restarts after USB mode, before the interface
 * could log anything. */
static void card_note(const char *say, const char *what)
{
    FILE *note = fopen(PIVOT_ROOT "/.rockbox/sloppy/usb-busy.txt", "a");
    if (!note)
        return;
    struct timespec up;
    clock_gettime(CLOCK_MONOTONIC, &up);
    fprintf(note, "up %lds: %s%s\n", (long)up.tv_sec, say, what);
    fclose(note);
}

/* One try at taking the card off: 0, or umount's errno. Every try, the
 * first and the USB screen's retries alike, leaves the card as working
 * directory and lets go of our folders on it first. */
static int card_umount(void)
{
    /* our working directory on the card holds it as surely as an open
     * file; one whose path getcwd() cannot give may be on it too */
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd) || on_card(cwd))
    {
        logf("leaving the working directory for the handover");
        if (chdir("/") != 0)
            logf("chdir / failed: %d", errno);
    }
    if (card_let_go_dirs())
    {
        card_note("let go of folders still open on the card: ", card_let_go);
        usb_trace("handover: let go of folders on the card: %s", card_let_go);
    }
    sync();
    return umount(PIVOT_ROOT) == 0 ? 0 : errno;
}
const char *usb_card_busy_files(void)
{
    return card_busy;
}
/* Only reads /proc: what any process (ours or HiBy's) holds on the card,
 * as an open file, a working or root directory or something mapped, and
 * any mount inside it, so the log names what kept it busy. Each item is
 * who:path[how], how being "cwd", "root", "fd N", "map" or "mount". An item
 * is listed once, and only a whole item counts as a repeat: "x:/" is not
 * one of "x:/.rockbox" (on 2026-10-23 that hid the card's root). */
static void card_busy_add(const char *who, const char *path, const char *how)
{
    size_t have = strlen(card_busy);
    if (!on_card(path))
        return;
    size_t root = strlen(!strncmp(path, PIVOT_ROOT, strlen(PIVOT_ROOT)) ? PIVOT_ROOT : card_root());
    char item[200];
    int len = snprintf(item, sizeof item, "%s:%s[%s]", who, path[root] ? path + root : "/", how);
    if (len < 0 || len >= (int)sizeof item)
        len = (int)strlen(item);
    for (const char *at = card_busy; (at = strstr(at, item)) != NULL; at++)
        if ((at == card_busy || at[-1] == ' ') && (at[len] == ' ' || !at[len]))
            return;
    snprintf(card_busy + have, sizeof card_busy - have, "%s%s", have ? " " : "", item);
}
static void card_busy_link(const char *who, const char *link, const char *how)
{
    char target[200];
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n <= 0)
        return;
    target[n] = '\0';
    card_busy_add(who, target, how);
}
static void card_busy_read(void)
{
    card_busy[0] = '\0';
    /* every process by number: Rockbox's dir.h makes opendir its own,
     * which does not reach /proc. A number reaches a thread too, and every
     * thread shows its process's files: processes only ("us/" is ours). */
    pid_t self = getpid();
    for (int pid = 1; pid < 32768; pid++)
    {
        char path[64], who[40], comm[20] = "?", line[400];
        snprintf(path, sizeof path, "/proc/%d/status", pid);
        FILE *c = fopen(path, "r");
        if (!c)
            continue;
        int tgid = pid;
        while (fgets(line, sizeof line, c))
            if (sscanf(line, "Tgid: %d", &tgid) == 1)
                break;
        fclose(c);
        if (tgid != pid)
            continue;
        snprintf(path, sizeof path, "/proc/%d/comm", pid);
        c = fopen(path, "r");
        if (c)
        {
            if (fgets(comm, sizeof comm, c)) comm[strcspn(comm, "\n")] = '\0';
            fclose(c);
        }
        snprintf(who, sizeof who, "%s%s(%d)", pid == self ? "us/" : "", comm, pid);
        snprintf(path, sizeof path, "/proc/%d/cwd", pid);
        card_busy_link(who, path, "cwd");
        snprintf(path, sizeof path, "/proc/%d/root", pid);
        card_busy_link(who, path, "root");
        for (int fd = 0; fd < 1024; fd++)
        {
            char how[12];
            snprintf(path, sizeof path, "/proc/%d/fd/%d", pid, fd);
            snprintf(how, sizeof how, "fd %d", fd);
            card_busy_link(who, path, how);
        }
        snprintf(path, sizeof path, "/proc/%d/maps", pid);
        FILE *fp = fopen(path, "r");
        while (fp && fgets(line, sizeof line, fp))
        {
            char *at = strchr(line, '/');
            if (!at)
                continue;
            at[strcspn(at, "\n")] = '\0';
            card_busy_add(who, at, "map");
        }
        if (fp)
            fclose(fp);
    }
    /* a filesystem mounted inside the card's own folder pins it too */
    FILE *m = fopen("/proc/mounts", "r");
    char line[400];
    while (m && fgets(line, sizeof line, m))
    {
        char dev[40], mnt[200];
        if (sscanf(line, "%39s %199s", dev, mnt) == 2 && strcmp(mnt, PIVOT_ROOT) && strcmp(mnt, card_root()))
            card_busy_add("mount", mnt, "mount");
    }
    if (m)
        fclose(m);
}

static void set_mass_storage_lun(void)
{
    /* Only a card that came off cleanly goes to the computer; otherwise the
     * drive has no medium, which beats the two of us writing one exFAT. */
    if (!card_off)
    {
        logf("card not unmounted (%d): LUN left empty", card_errno);
        sysfs_set_string(CARD_LUN_FILE, "\n");   /* a bare "" writes nothing: the medium would stay */
        usb_trace("LUN: left empty, the card is still mounted (umount errno %d)", card_errno);
        return;
    }
    sysfs_set_string(CARD_LUN_FILE, card_dev);
    char lun[64] = "?";
    sysfs_get_string(CARD_LUN_FILE, lun, sizeof lun);
    usb_trace("LUN: set to %s, reads '%s'", card_dev, lun);
}

void usb_enable(bool on)
{
    logf(">>>>>>>>>>>>>>>>> usb_enable(%d)\n", on);
    logf("usb enable %d %d\n", on, _usb_mode);

    /* ADB binds the UDC itself, and so does the USB-DAC while it is active,
     * so leave the UDC alone in those cases. */
    if (_usb_mode == USB_MODE_ADB || usb_audio_get_active())
    {
        if (on)
            usb_trace("enable: gadget left alone (mode %s%s)", usb_mode_name(_usb_mode),
                      usb_audio_get_active() ? ", USB DAC on" : "");
        return;
    }

    // Re-arm the LUN on each connect, otherwise the disk is only exported the first time.
    if (on && _usb_mode == USB_MODE_MASS_STORAGE)
        set_mass_storage_lun();

    bool bound = udc_bind(on, on ? "enable" : "disable");
    udc_bound_at = on && bound ? current_tick : 0;
    udc_rebound = false;
    udc_seen[0] = '\0';
    if (on)
        usb_trace("enable: gadget %s (UDC %s)", bound ? "bound" : "NOT bound", udc_state_now());
}

/* This is called by usb thread after usb extract in order to return
 * regular FS access
 *
 * returns the # of successful mounts
*/
int disk_mount_all(void)
{
    logf(">>>>>>>>>>>>>>>>> disk_mount_all()\n");

    int rc = 0;
    if (card_retrying && !card_off)
    {
        /* unplugged with the card still busy all along: what holds it now,
         * to tell a holder that stayed from one that came and went */
        card_busy_read();
        card_note("unplugged with the card still busy; still open: ", card_busy[0] ? card_busy : "nothing found");
    }
    card_retrying = false;               /* the computer is gone: the card stays ours */
    usb_trace("unplugged: USB mode over (UDC %s), card %s", udc_state_now(), card_off ? "comes back" : "was never handed over");
    udc_bound_at = 0;
    if (card_off)
    {
        /* take the card back from the gadget first: a mount wants the
         * device to itself, and the host may still be letting go */
        sysfs_set_string(CARD_LUN_FILE, "\n");   /* a bare "" writes nothing: the medium would stay */
        for (int tries = 0; tries < CARD_TRIES; tries++)
        {
            rc = card_mount();
            if (rc == 0)
                break;
            sleep(HZ / 10);
        }
        if (rc)
            logf("mount %s on %s failed: %d", card_dev, PIVOT_ROOT, rc);
        else
        {
            card_off = false;
            crashlog_card_away(0);
        }
        usb_trace("remount: %s (errno %d)", rc ? "FAILED" : "the card is the player's again", rc);
    }
    usb_trace_flush();

#ifdef HAVE_MULTIDRIVE
    startup_rbhome();
#endif
    return card_off ? 0 : 1;
}

/* This is called by usb thread after all threads ACKs usb inserted message
 *
 * returns the # of successful unmounts
 */
/* At power off: the card unmounted, or else read-only, before the power
 * goes. /sbin/poweroff, or the kernel's reboot() when it is slow, left it
 * mounted: the exFAT volume stayed dirty, and a Mac's check found its
 * allocation bitmap needing repair, three times in two weeks on a test R1
 * (3 Nov). A file still open for writing keeps it mounted; read-only
 * then, which flushes it and takes no more writes. */
void usb_card_park(void)
{
    if (card_off || card_leaving)
        return;                          /* the computer has it, or it is going */
    if (!card_mount_read())
    {
        /* not in /proc/mounts: as the HiBy init mounts it (disk_unmount_all) */
        strcpy(card_dev, access("/dev/mmcblk0p1", F_OK) == 0 ? "/dev/mmcblk0p1" : "/dev/mmcblk0");
        card_fs[0] = card_opts[0] = '\0';
        card_flags = 0;
    }
    usb_trace("power off: unmounting the card");
    usb_trace_flush();                   /* the card's last line */
    card_leaving = true;                 /* nothing more onto it */
    crashlog_card_away(1);
    if (card_umount() == 0)
        return;
    mount(card_dev, PIVOT_ROOT, card_fs, card_flags | MS_REMOUNT | MS_RDONLY,
          card_opts[0] ? card_opts : NULL);
}

int disk_unmount_all(void)
{
    logf(">>>>>>>>>>>>>>>>> disk_unmount_all()\n");

    /* Charge-only, adb and the USB DAC do not export the card: it stays
     * the player's. Rockbox lives on it, so this is only for the handover. */
    if (_usb_mode != USB_MODE_MASS_STORAGE || usb_audio_get_active() || card_off)
    {
        usb_trace("handover: not needed (mode %s%s%s)", usb_mode_name(_usb_mode),
                  usb_audio_get_active() ? ", USB DAC on" : "", card_off ? ", card already off" : "");
        return 1;
    }

    if (!card_mount_read())
    {
        /* not in /proc/mounts: as the HiBy init mounts it */
        strcpy(card_dev, access("/dev/mmcblk0p1", F_OK) == 0 ? "/dev/mmcblk0p1" : "/dev/mmcblk0");
        card_fs[0] = card_opts[0] = '\0';
        card_flags = 0;
    }

#ifdef HAVE_MULTIDRIVE
    /* its descriptor of the home directory would keep the card busy; it
     * is opened again by disk_mount_all() once the computer is gone */
    cleanup_rbhome();
#endif
    usb_trace("handover: unmounting %s (%s)", card_dev, card_fs[0] ? card_fs : "not in /proc/mounts");
    usb_trace_flush();                   /* the last lines the card gets before it goes */
    card_leaving = true;                 /* from now no writes: a late one would keep it busy, or land under the mount point */
    crashlog_card_away(1);
    long from = current_tick;
    int err = 0, tries;
    for (tries = 0; tries < CARD_OFF_TRIES; tries++)
    {
        err = card_umount();
        if (err != EBUSY)                /* only an open file is worth waiting out */
            break;
        sleep(HZ * 4 / 10);
    }
    usb_trace("handover: umount %s after %d tr%s, %ld ms", err ? strerror(err) : "done", tries + (tries < CARD_OFF_TRIES),
              tries ? "ies" : "y", (current_tick - from) * 1000 / HZ);
    card_errno = err;
    card_busy[0] = '\0';
    if (err == EBUSY)
        card_busy_read();
    if (err)
    {
        card_leaving = false;            /* it stays the player's */
        crashlog_card_away(0);
        logf("umount %s failed: %d", PIVOT_ROOT, err);
        /* the card is still ours, so say why on it now */
        char why[80];
        snprintf(why, sizeof why, "umount errno %d (%s); still open: ", err, strerror(err));
        card_note(why, card_busy[0] ? card_busy : "nothing found");
        usb_trace("handover: the card stays the player's; still open: %.300s", card_busy[0] ? card_busy : "nothing found");
        usb_trace_flush();
        /* the USB screen says so and keeps trying (usb_card_retry) */
        card_retrying = err == EBUSY;
        return 0;
    }
    card_off = true;
    card_leaving = false;                /* card_off holds it from here */
    return 1;
}

/* The USB screen calls this while it is up: a handover that failed on a
 * busy card is tried again, and a card that came off goes to the computer
 * (the gadget is bound again so it sees the new medium at once). True
 * while the card is still not the computer's. */
bool usb_card_retry(void)
{
    udc_follow();
    if (!card_retrying)
        return false;
    if (_usb_mode != USB_MODE_MASS_STORAGE || usb_audio_get_active())
    {
        card_retrying = false;
        return false;
    }
    card_leaving = true;
    crashlog_card_away(1);
    int err = card_umount();
    if (err)
    {
        card_leaving = false;            /* still the player's */
        crashlog_card_away(0);
    }
    if (err == EBUSY)
        return true;
    card_retrying = false;
    card_errno = err;
    if (err)
    {
        logf("umount %s failed: %d", PIVOT_ROOT, err);
        usb_trace("late handover: umount failed: %s", strerror(err));
        return false;
    }
    card_off = true;
    card_leaving = false;
    usb_trace("late handover: the card came off; gadget bound again with it");
    udc_bind(false, "late handover");
    sysfs_set_string(CARD_LUN_FILE, card_dev);
    udc_bound_at = udc_bind(true, "late handover") ? current_tick : 0;
    udc_rebound = false;
    udc_seen[0] = '\0';
    return false;
}

void enable_charging(void) {
    logf(">>>>>>>>>>>>>>>>> enable_charging()\n");

    disable_mass_storage();
    disable_adb();
    disable_usb_audio();
}

void enable_adb(void) {
    logf(">>>>>>>>>>>>>>>>> set_adb()\n");

    // Stop the respawner FIRST so it can't rebind adbd under us.
    system("killall adbserver.sh adbd 2>/dev/null");

    /* Disable mass storage and USB DAC if they were running (unbinds the UDC
     * and clears any uac_sa link/descriptor). */
    disable_mass_storage();
    disable_usb_audio();

    /* Also drop the adb function link so we control the link order below
     * (uac_sa must be linked before adb -- see adb_compose_dac()). */
    if (access(USB_GADGET_PATH "/configs/c.1/ffs.adb", F_OK) == 0)
        system("rm " USB_GADGET_PATH "/configs/c.1/ffs.adb");

    system("mkdir -p " USB_GADGET_PATH "/configs/c.1/strings/0x409");
    system("mkdir -p " USB_GADGET_PATH "/functions/ffs.adb");

    /* Compose the USB-DAC (uac_sa, linked first) alongside adb when the
     * usb_audio setting wants it; otherwise present plain adb. */
    bool with_dac = adb_compose_dac();
    if (!with_dac) {
        // Use the adb VID/PID so the host recognises the device
        sysfs_set_string(USB_GADGET_PATH "/idVendor", "0x18d1");
        sysfs_set_string(USB_GADGET_PATH "/idProduct", "0xd002");
        sysfs_set_string(USB_GADGET_PATH "/configs/c.1/strings/0x409/configuration", "adb");
    }

    sysfs_set_int(USB_GADGET_PATH "/configs/c.1/MaxPower", 120);

    // Link adb into the config (after uac_sa, so adb is the later interface)
    system("ln -sf " USB_GADGET_PATH "/functions/ffs.adb " USB_GADGET_PATH "/configs/c.1/");

    system("mkdir -p /dev/usb-ffs/adb");

    // Let the vendor respawner mount functionfs and run adbd, which binds the UDC.
    system("/sbin/adbserver.sh 440 &");

    /* uac_sa is bound by the same auto-bind; start pumping /dev/uac_sa. */
    if (with_dac)
        usb_dac_start();
}


void disable_adb(void) {
    // Remove any lingering adb daemon and its respawner
    system("killall adbserver.sh adbd 2>/dev/null");

    // Unbind the UDC so the gadget can be reconfigured
    if (access("/sys/kernel/config/usb_gadget/adb_demo/UDC", F_OK) == 0) {
        sysfs_set_string("/sys/kernel/config/usb_gadget/adb_demo/UDC", "\n");
    }

    // Unlink the adb function from config (the function is left in place and
    // reused, so re-creating it in enable_adb can't block)
    if (access("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/ffs.adb", F_OK) == 0) {
        system("rm /sys/kernel/config/usb_gadget/adb_demo/configs/c.1/ffs.adb");
    }

    // Reset the MaxPower to its default value
    if (access("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/MaxPower", F_OK) == 0) {
        sysfs_set_int("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/MaxPower", 120);
    }

    // Disable storage or adb configs
    if (access("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409/configuration", F_OK) == 0) {
        sysfs_set_string("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409/configuration", "");
    }

    // Unmount adb
    if (!system("mountpoint -q /dev/usb-ffs/adb")) {
        system("umount -l /dev/usb-ffs/adb");
    }
}

void enable_mass_storage(void) {
    logf(">>>>>>>>>>>>>>>>> set_mass_storage()\n");

    /* Disable adb and USB DAC if they're running */
    disable_adb();
    disable_usb_audio();

    system("mkdir -p /sys/kernel/config/usb_gadget/adb_demo/functions/mass_storage.0/lun.0");
    system("mkdir -p /sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409");

    if (is_adb_running) {
        sysfs_set_string("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409/configuration", "adb,storage");
    } else {
        sysfs_set_string("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409/configuration", "storage");
    }
    /* Ask the computer for the full USB 2.0 budget: a player that is
     * charging while it serves the card needs more than 120 mA. */
    sysfs_set_int("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/MaxPower", 500);

    system("ln -s /sys/kernel/config/usb_gadget/adb_demo/functions/mass_storage.0 /sys/kernel/config/usb_gadget/adb_demo/configs/c.1/");

    /* no medium yet: usb_enable(true) puts the card in once it is unmounted */
    sysfs_set_string(CARD_LUN_FILE, "\n");   /* a bare "" writes nothing: the medium would stay */
}

void disable_mass_storage(void) {
    // Remove the mass_storage.0 link to config
    if (access("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/mass_storage.0", F_OK) == 0) {
        system("rm /sys/kernel/config/usb_gadget/adb_demo/configs/c.1/mass_storage.0");
    }

    // Remove the mass_storage.0 function
    if (access("/sys/kernel/config/usb_gadget/adb_demo/functions/mass_storage.0", F_OK) == 0) {
        system("rm -rf /sys/kernel/config/usb_gadget/adb_demo/functions/mass_storage.0");
    }

    // Reset the MaxPower to its default value
    if (access("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/MaxPower", F_OK) == 0) {
        sysfs_set_int("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/MaxPower", 120);
    }

    // Disable storage or adb configs
    if (access("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409/configuration", F_OK) == 0) {
        sysfs_set_string("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409/configuration", "");
    }
}

/* USB DAC (USB Audio Class gadget).
 *
 * The vendor kernel provides a UAC function "uac_sa"; building it into the
 * gadget and binding the UDC creates /dev/uac_sa, which usb-dac-hiby.c pumps
 * into the PCM mixer. enable_usb_audio() is the standalone path used in
 * charge/mass-storage mode; adb mode composes uac_sa alongside adb in
 * enable_adb() instead. */

bool enable_usb_audio(void)
{
    logf(">>>>>>>>>>>>>>>>> enable_usb_audio()\n");

    /* The DAC pump feeds the PCM mixer, which isn't up until audio_init(), so
     * a DAC setting applied at boot lands too early. Bail before touching
     * anything so the caller can restore the base mode. */
    if (!pcm_is_initialized())
        return false;

    /* The DAC hands the output path to the host's PCM; stop local playback
     * first (it can't restart while the DAC is active). */
    audio_stop();

    /* Standalone DAC: tear down the other gadget functions first. */
    disable_mass_storage();
    disable_adb();

    /* Build the "uac_sa" function: stereo, 16-bit, capture (host->device).
     * Set all three explicitly: the kernel's defaults are 3/2/64000, so at
     * least the 64 kHz capture rate must always be overridden. */
    system("mkdir -p " USB_GADGET_PATH "/functions/uac_sa.a");
    sysfs_set_int(USB_GADGET_PATH "/functions/uac_sa.a/c_chmask", 3);
    sysfs_set_int(USB_GADGET_PATH "/functions/uac_sa.a/c_ssize", 2); 
    sysfs_set_int(USB_GADGET_PATH "/functions/uac_sa.a/c_srate", USB_DAC_SAMPLE_RATE);

    /* Present an IAD audio device with the HiBy DAC VID/PID so the host
     * recognises it as a USB DAC. */
    sysfs_set_int(USB_GADGET_PATH "/bDeviceClass", USB_CLASS_MISC);
    sysfs_set_int(USB_GADGET_PATH "/bDeviceSubClass", 2);
    sysfs_set_int(USB_GADGET_PATH "/bDeviceProtocol", 1);
    sysfs_set_string(USB_GADGET_PATH "/idVendor", "0x" USB_VID_STR);
    sysfs_set_string(USB_GADGET_PATH "/idProduct", "0x0004");

    system("mkdir -p " USB_GADGET_PATH "/configs/c.1/strings/0x409");
    sysfs_set_string(USB_GADGET_PATH
                     "/configs/c.1/strings/0x409/configuration", "uac");
    sysfs_set_int(USB_GADGET_PATH "/configs/c.1/bmAttributes",
                  USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER);
    sysfs_set_int(USB_GADGET_PATH "/configs/c.1/MaxPower", 120);

    system("ln -sf " USB_GADGET_PATH "/functions/uac_sa.a "
           USB_GADGET_PATH "/configs/c.1/");

    /* Bind the UDC ourselves -- in charge mode the usb thread is power-only,
     * and usb_enable() bails while the DAC is active, so nothing else binds
     * it. This creates /dev/uac_sa. */
    sysfs_set_string(USB_GADGET_PATH "/UDC", "13500000.otg_new");

    return usb_dac_start();
}

void disable_usb_audio(void)
{
    logf(">>>>>>>>>>>>>>>>> disable_usb_audio()\n");

    usb_dac_stop();

    /* Unbind the UDC and tear the function down so the next mode starts
     * from a clean gadget. */
    if (access(USB_GADGET_PATH "/UDC", F_OK) == 0)
        sysfs_set_string(USB_GADGET_PATH "/UDC", "\n");

    if (access(USB_GADGET_PATH "/configs/c.1/uac_sa.a", F_OK) == 0)
        system("rm " USB_GADGET_PATH "/configs/c.1/uac_sa.a");

    if (access(USB_GADGET_PATH "/functions/uac_sa.a", F_OK) == 0)
        system("rmdir " USB_GADGET_PATH "/functions/uac_sa.a");

    /* Restore a plain (non-IAD) device descriptor and the target's own
     * VID/PID for the other modes. */
    sysfs_set_int(USB_GADGET_PATH "/bDeviceClass", USB_CLASS_PER_INTERFACE);
    sysfs_set_int(USB_GADGET_PATH "/bDeviceSubClass", 0);
    sysfs_set_int(USB_GADGET_PATH "/bDeviceProtocol", 0);
    sysfs_set_string(USB_GADGET_PATH "/idVendor", "0x" USB_VID_STR);
    sysfs_set_string(USB_GADGET_PATH "/idProduct", "0x" USB_PID_STR);

    if (access(USB_GADGET_PATH
               "/configs/c.1/strings/0x409/configuration", F_OK) == 0)
        sysfs_set_string(USB_GADGET_PATH
                         "/configs/c.1/strings/0x409/configuration", "");
}

void usb_init_device(void)
{
    logf(">>>>>>>>>>>>>>>>> usb_init_device()\n");
    if (_usb_init) {
        logf("usb is already init, skipping!\n");
        return;
    }

    char functions[128] = {0};

    /* Before we can do anything here we need to mount configfs */
    int is_mounted = !system("mountpoint -q /sys/kernel/config");

    if (!is_mounted && system("mount -t configfs none /sys/kernel/config")) {
        logf("mount configfs failed, can't do usb functionality! ErrNo: %d\n", errno);
        return;
    }

    _usb_init = true;

    system("ls -la /sys/kernel/config/usb_gadget");

    /* os_mkdir doesn't seem to work here for whatever reason */
    system("mkdir -p /sys/kernel/config/usb_gadget/adb_demo");
    system("mkdir -p /sys/kernel/config/usb_gadget/adb_demo/strings/0x409");

    system("ls -la /sys/kernel/config/usb_gadget/adb_demo");
    system("ls -la /sys/kernel/config/usb_gadget/adb_demo/configs/");

    system("mkdir -p /sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409");

    /* Check if ADB was activated in bootloader */
    logf("checking if adb is already on\n");

    if (access("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409/configuration", F_OK) == 0) {
        logf("found usb config string!\n");

        sysfs_get_string("/sys/kernel/config/usb_gadget/adb_demo/configs/c.1/strings/0x409/configuration", functions, sizeof(functions));
        is_adb_running = (strstr(functions, "adb") == NULL) ? false : true;
    }

    sysfs_set_string("/sys/kernel/config/usb_gadget/adb_demo/strings/0x409/manufacturer", "Rockbox.org");
    sysfs_set_string("/sys/kernel/config/usb_gadget/adb_demo/strings/0x409/product", "Rockbox media player");
    sysfs_set_string("/sys/kernel/config/usb_gadget/adb_demo/strings/0x409/serialnumber", "0123456789ABCDEF");
    char udc[64] = "";
    sysfs_get_string(USB_GADGET_PATH "/UDC", udc, sizeof udc);
    usb_trace("start: the player is up; UDC %s, gadget bound to '%s', config '%s'", udc_state_now(), udc, functions);
    usb_trace_flush();
}
