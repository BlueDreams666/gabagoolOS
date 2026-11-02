/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2016 Amaury Pouly
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
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <pthread.h>
#include <time.h>
#include <sys/reboot.h>

#include "system.h"
#include "mv.h"
#include "font.h"
#include "power.h"
#include "button.h"
#include "button-devinput.h"
#include "backlight-target.h"
#include "lcd.h"
#include "filesystem-hosted.h"
#include "logf.h"
#include "rbpaths.h"
#include "crashlog-linux.h"

const char *handle_special_dirs(const char *dir, unsigned flags,
                                char *buf, const size_t bufsize);

#ifdef HIBY_R1
/* A crash in USB mode, when the card is the computer's, is kept on the
 * internal flash (UBIFS, survives the reboot; /tmp does not) and goes
 * onto the card at the next start. */
#define CRASHLOG_SPOOL_PATH "/usr/data/rockbox-crash.log"
#else
#define CRASHLOG_SPOOL_PATH NULL
#endif

/* to make thread-internal.h happy */
uintptr_t *stackbegin;
uintptr_t *stackend;

/* forward-declare */
bool os_file_exists(const char *ospath);
extern const char rbversion[];

static void sig_handler(int sig, siginfo_t *siginfo, void *context)
{
    /* safe guard variable - we call backtrace() only on first
     * UIE call. This prevent endless loop if backtrace() touches
     * memory regions which cause abort
     */
    static bool triggered = false;

    /* get context info */
    ucontext_t *uc = (ucontext_t *)context;
    unsigned long pc = uc->uc_mcontext.pc;
    unsigned long sp = uc->uc_mcontext.gregs[29];
    unsigned long ra = uc->uc_mcontext.gregs[31];

    /* The file first: the screen is gone after the reboot, the card is not.
       Only once, for the same reason the backtrace is only taken once. */
    int saved = CRASHLOG_NOWHERE;
    if (!triggered && sig != SIGTERM && sig != SIGPIPE)
        saved = crashlog_write(sig, pc, ra, sp, siginfo->si_addr);

    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
    unsigned line = 0;

    lcd_setfont(FONT_SYSFIXED);
    lcd_set_viewport(NULL);
    lcd_clear_display();

    /* Short lines: the system font fits about 28 characters across. */
    lcd_putsf(0, line++, "Rockbox crashed");
    lcd_putsf(0, line++, "%s", strsignal(sig));
    line++;
    lcd_putsf(0, line++, "pc    %08lx", pc);
    lcd_putsf(0, line++, "ra    %08lx", ra);

    if(sig == SIGILL || sig == SIGFPE || sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP) {
        lcd_putsf(0, line++, "addr  %08lx", (unsigned long)siginfo->si_addr);
    }
    lcd_putsf(0, line++, "%.28s", rbversion);

    if(!triggered)
    {
        triggered = true;
        /* glibc's backtrace() stops at the signal frame here; show the
         * return addresses on the stack instead, two to a line. */
        unsigned long callers[24];
        int n = crashlog_callers(sp, callers, 24);
        line++;
        lcd_putsf(0, line++, "called from:");
        for (int i = 0; i < n && line + 3 < (unsigned)(LCD_HEIGHT / font_get(FONT_SYSFIXED)->height); i += 2)
        {
            if (i + 1 < n)
                lcd_putsf(0, line++, " %08lx %08lx", callers[i], callers[i + 1]);
            else
                lcd_putsf(0, line++, " %08lx", callers[i]);
        }
    }

    line++;
    if (saved == CRASHLOG_CARD)
        lcd_putsf(0, line++, "Saved to .rockbox/crash.log");
    else if (saved == CRASHLOG_SPOOL)
        lcd_putsf(0, line++, "Log goes on the card next start");
    else
        lcd_putsf(0, line++, "No crash log saved");
    lcd_putsf(0, line++, "Press a button to restart");

#ifdef ROCKBOX_HAS_LOGF
    lcd_putsf(0, line++, "logf:");
    logf_panic_dump(&line);
#endif

    lcd_update();

    system_exception_wait(); /* If this returns, try to reboot */
    system_reboot();
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

#ifdef HAVE_SHUTDOWN_WATCHDOG
/* A clean shutdown can stall anywhere between the "Shutting Down..."
 * splash and the kernel cutting power: a driver that blocks on close,
 * /sbin/poweroff waiting on an init that never answers, or a poweroff
 * that returns having done nothing. Any of those left the splash lit
 * until the power button was held. The watchdog is a plain OS thread,
 * so it fires whatever the Rockbox threads are stuck in, and it asks
 * the kernel directly. */
#define SHUTDOWN_WATCHDOG_SECS 20

static void *shutdown_watchdog_fn(void *arg)
{
    int cmd = (int)(intptr_t)arg;
    struct timespec ts = { SHUTDOWN_WATCHDOG_SECS, 0 };
    while (nanosleep(&ts, &ts) != 0)
        ;
    sync();
    reboot(cmd);
    return NULL;
}

void shutdown_watchdog_arm(bool reboot_instead)
{
    static bool armed = false;
    if (armed)
        return;

    pthread_t t;
    pthread_attr_t attr;
    intptr_t cmd = reboot_instead ? RB_AUTOBOOT : RB_POWER_OFF;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    armed = pthread_create(&t, &attr, shutdown_watchdog_fn,
                           (void *)cmd) == 0;
    pthread_attr_destroy(&attr);
}

/* init only gets signalled by poweroff/reboot; if it has not acted
 * within a few seconds, go to the kernel ourselves */
static void kernel_reboot_fallback(int cmd)
{
    sleep(5);
    sync();
    reboot(cmd);
}
#endif

void power_off(void)
{
#ifdef HAVE_SHUTDOWN_WATCHDOG
    shutdown_watchdog_arm(false);
#endif
    backlight_hw_on();
    button_close_device();
    sync();
    system("/sbin/poweroff");
#ifdef HAVE_SHUTDOWN_WATCHDOG
    kernel_reboot_fallback(RB_POWER_OFF);
#endif
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

void system_init(void)
{
    int *s;
    /* fake stack, to make thread-internal.h happy */
    stackbegin = stackend = (uintptr_t*)&s;

    /* Resolve the crash log's real path now; a signal handler cannot. */
    char crashlog_buf[256];
    crashlog_init(handle_special_dirs(ROCKBOX_DIR "/crash.log", 0,
                                      crashlog_buf, sizeof(crashlog_buf)),
                  CRASHLOG_SPOOL_PATH);

   /* catch some signals for easier debugging */
    struct sigaction sa;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = &sig_handler;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void system_reboot(void)
{
#ifdef HAVE_SHUTDOWN_WATCHDOG
    shutdown_watchdog_arm(true);
#endif
    backlight_hw_off();
    system("/sbin/reboot");
#ifdef HAVE_SHUTDOWN_WATCHDOG
    kernel_reboot_fallback(RB_AUTOBOOT);
#endif
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

void system_exception_wait(void)
{
    backlight_hw_on();
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
    /* wait until button press and release */
#ifdef HAVE_BUTTON_DATA
    int bdata;
#define BDATA &bdata
#else
#define BDATA
#endif
    while(button_read_device(BDATA) != 0) {}
    while(button_read_device(BDATA) == 0) {}
    while(button_read_device(BDATA) != 0) {}
    while(button_read_device(BDATA) == 0) {}
}

bool hostfs_removable(IF_MD_NONVOID(int drive))
{
#ifdef HAVE_MULTIDRIVE
    if (drive > 0)
        return true;
    else
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        return true;
#else
        return false; /* internal: always present */
#endif
}

bool hostfs_present(IF_MD_NONVOID(int drive))
{
#ifdef HAVE_MULTIDRIVE
    if (drive > 0)
#if defined(MULTIDRIVE_DEV)
        return os_file_exists(MULTIDRIVE_DEV);
#else
        return true; // FIXME?
#endif
    else
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        return os_file_exists(ROOTDRIVE_DEV);
#else
        return true; /* internal: always present */
#endif
}

#ifdef HAVE_MULTIDRIVE
int volume_drive(int drive)
{
    return drive;
}
#endif /* HAVE_MULTIDRIVE */

#ifdef CONFIG_STORAGE_MULTI
int hostfs_driver_type(int drive)
{
#if (CONFIG_STORAGE & STORAGE_USB)
    return drive > 0 ? STORAGE_USB_NUM : STORAGE_HOSTFS_NUM;
#else
    return drive > 0 ? STORAGE_SD_NUM : STORAGE_HOSTFS_NUM;
#endif
}
#endif /* CONFIG_STORAGE_MULTI */

int hostfs_init(void)
{
    return 0;
}

int hostfs_flush(void)
{
    sync();
    return 0;
}

#ifdef HAVE_HOTSWAP
bool volume_removable(int volume)
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_removable(volume);
}

bool volume_present(int volume)
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_present(volume);
}
#endif

int volume_partition(int volume)
{
    (void)volume;
    /* Hosted only implement a single parition per "drive" */
    return 0;
}
