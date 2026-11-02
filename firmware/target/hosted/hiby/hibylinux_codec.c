/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 *
 * Copyright (c) 2018 Marcin Bukat
 * Copyright (c) 2025 Solomon Peachy
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

//#define LOGF_ENABLE

#include "config.h"
#include "audio.h"
#include "audiohw.h"
#include "button.h"
#include "system.h"
#include "kernel.h"
#include "panic.h"
#include "sysfs.h"
#include "alsa-controls.h"
#include "pcm-alsa-hiby.h"
#include "sound.h"
#include "settings.h"

#include "logf.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

int hiby_has_valid_output(void);

#ifndef HIBY_SYS_SERVER_SOCKET          /* a host test points it elsewhere */
#define HIBY_SYS_SERVER_SOCKET "/var/run/sys_server"
#endif
#define HIBY_ABSVOL_MAX 127

static int hw_init = 0;

static long int vol_l_hw = 255;
static long int vol_r_hw = 255;
static long int last_ps = -1;

static int muted = -1;
/* The step the headphones were last asked for (queued, being sent or
 * taken), and where. A change is compared with this, not with the last
 * step sys_server answered OK: up then down while the up was still being
 * sent looked like "no change" and the down was lost, leaving the
 * headphones a step louder than the screen said. */
static int bt_absvol_last_step = -1;
static char bt_absvol_last_mac[18];
static unsigned bt_absvol_gen;          /* bumped when the route changes */

/* Straight from the volume to AVRCP's 0-127, rounded once. Going through a
 * whole percentage first rounded twice: the 120 half-dB steps landed on 101
 * percentages, so some presses never reached the headphones and others
 * moved them two steps. */
static int hiby_volume_to_absvol_step(int volume_cb)
{
    int min_vol = sound_min(SOUND_VOLUME);
    int max_vol = sound_max(SOUND_VOLUME);
    int span = max_vol - min_vol;
    int step;

    if (span <= 0)
        return 0;
    step = ((volume_cb - min_vol) * HIBY_ABSVOL_MAX + span / 2) / span;
    if (step < 0)
        step = 0;
    if (step > HIBY_ABSVOL_MAX)
        step = HIBY_ABSVOL_MAX;

    return step;
}

static int hiby_sys_server_command(const char *command, char *reply, size_t reply_size)
{
    struct sockaddr_un addr;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    int fd = -1;
    int rc = -1;
    ssize_t n;

    if (reply && reply_size > 0)
        reply[0] = '\0';

    if (!command || !*command)
        return -1;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", HIBY_SYS_SERVER_SOCKET);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto out;
    if (send(fd, command, strlen(command), 0) < 0)
        goto out;

    if (reply && reply_size > 1)
    {
        n = recv(fd, reply, reply_size - 1, 0);
        if (n < 0)
            goto out;
        reply[n] = '\0';
    }

    rc = 0;

out:
    if (fd >= 0)
        close(fd);
    return rc;
}

static bool hiby_send_bt_absvol(const char *mac_u, int step)
{
    char cmd[96];
    char reply[128];

    snprintf(cmd, sizeof(cmd), "BT:ABSVOL:%s %d", mac_u, step);
    if (hiby_sys_server_command(cmd, reply, sizeof(reply)) < 0)
    {
        logf("bt absvol send fail: %s", cmd);
        return false;
    }

    if (!strstr(reply, "OK"))
    {
        logf("bt absvol not-ok: %s", reply);
        return false;
    }
    return true;
}

/* sys_server takes tens of ms to answer ABSVOL, or 300 ms when it does not
 * answer at all, and every volume step used to wait for it on the Rockbox
 * thread: dragging the volume over Bluetooth stuttered the UI and the
 * decoder, and a step that never got its OK was sent again on every move.
 * A raw pthread (no Rockbox kernel calls) sends it now. Callers only leave
 * the newest step for it, so a drag sends where the finger is, not a
 * backlog. A failed send is retried twice, unless a newer step comes. */
void hiby_debug_log(const char *format, ...);
static pthread_mutex_t bt_absvol_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bt_absvol_cond = PTHREAD_COND_INITIALIZER;
static bool bt_absvol_started, bt_absvol_failing;
static int bt_absvol_want = -1;
static char bt_absvol_want_mac[18];

static void *hiby_bt_absvol_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&bt_absvol_lock);
    for (;;)
    {
        while (bt_absvol_want < 0)
            pthread_cond_wait(&bt_absvol_cond, &bt_absvol_lock);
        int step = bt_absvol_want;
        char mac[18];
        snprintf(mac, sizeof(mac), "%s", bt_absvol_want_mac);
        unsigned gen = bt_absvol_gen;
        bt_absvol_want = -1;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok && bt_absvol_want < 0 && gen == bt_absvol_gen; attempt++)
        {
            if (attempt)
            {
                struct timespec until;
                clock_gettime(CLOCK_REALTIME, &until);
                until.tv_nsec += attempt * 200000000L;
                if (until.tv_nsec >= 1000000000L)
                {
                    until.tv_sec++;
                    until.tv_nsec -= 1000000000L;
                }
                while (bt_absvol_want < 0 &&
                       pthread_cond_timedwait(&bt_absvol_cond, &bt_absvol_lock, &until) == 0);
                if (bt_absvol_want >= 0 || gen != bt_absvol_gen)
                    break;              /* a newer step or route replaces this one */
            }
            pthread_mutex_unlock(&bt_absvol_lock);
            ok = hiby_send_bt_absvol(mac, step);
            pthread_mutex_lock(&bt_absvol_lock);
        }
        /* Not taken: the next change sends again, even the same step -
         * unless a newer step or another route came while this was out. */
        if (!ok && bt_absvol_want < 0 && gen == bt_absvol_gen)
            bt_absvol_last_step = -1;
        if (ok && bt_absvol_want < 0)   /* the step a drag or presses settled on */
            hiby_debug_log("volume: sent Bluetooth step %d", step);
        if (ok == bt_absvol_failing)
        {
            bt_absvol_failing = !ok;
            hiby_debug_log(ok ? "volume: Bluetooth volume reaches the device again"
                              : "volume: sys_server did not take Bluetooth volume %d", step);
        }
    }
    return NULL;
}

static void hiby_notify_bt_absvol(int volume_cb)
{
    const char *mac_u = hiby_pcm_get_bt_mac();
    char mac[18];
    int step;

    pthread_mutex_lock(&bt_absvol_lock);
    if (!mac_u || !mac_u[0])
    {
        bt_absvol_last_step = -1;
        bt_absvol_last_mac[0] = '\0';
        bt_absvol_want = -1;
        bt_absvol_gen++;
        pthread_mutex_unlock(&bt_absvol_lock);
        return;
    }
    snprintf(mac, sizeof(mac), "%s", mac_u);

    step = hiby_volume_to_absvol_step(volume_cb);
    if (step != bt_absvol_last_step || strcmp(mac, bt_absvol_last_mac))
    {
        if (!bt_absvol_started)
        {
            pthread_t thread;
            bt_absvol_started = pthread_create(&thread, NULL, hiby_bt_absvol_thread, NULL) == 0;
            if (bt_absvol_started)
                pthread_detach(thread);
        }
        bt_absvol_want = step;
        snprintf(bt_absvol_want_mac, sizeof(bt_absvol_want_mac), "%s", mac);
        bt_absvol_last_step = step;
        snprintf(bt_absvol_last_mac, sizeof(bt_absvol_last_mac), "%s", mac);
        pthread_cond_signal(&bt_absvol_cond);
    }
    pthread_mutex_unlock(&bt_absvol_lock);
}

/* A device that has just been routed to still plays at its own level: send
 * it ours, so the slider and what is heard agree from the first note. Safe
 * from any thread. */
void hiby_bt_absvol_resync(void)
{
    pthread_mutex_lock(&bt_absvol_lock);
    bt_absvol_last_step = -1;
    bt_absvol_gen++;
    pthread_mutex_unlock(&bt_absvol_lock);
    if (hw_init)
        hiby_notify_bt_absvol((vol_l_hw + vol_r_hw) / 2);
}

void audiohw_mute(int mute)
{
    if (hw_init < 0 || muted == mute)
        return;

    muted = mute;

    alsa_controls_set_bool("Mute Output", !!mute);
}

int hiby_get_outputs(void){
    long int ps = hiby_has_valid_output();

    hiby_set_output(ps);

    return ps;
}

void hiby_set_output(int ps)
{
    if (!hw_init || muted) return;

    // Default to headset if nothing was ever inserted; otherwise, R3 Pro II crashes on playback
    if (ps == 0)
    {
        ps = last_ps > 0 ? last_ps : 2;
    }

    if (last_ps != ps)
    {
        logf("set out %d/%d", ps, last_ps);
        /* Output port switch */
        last_ps = ps;
        alsa_controls_set_ints("Output Port Switch", 1, &last_ps);
        audiohw_set_volume(vol_l_hw, vol_r_hw);
        // Enable/disable inline controls(OF also uses ps = 5 for inline controls)
        // if (ps == 2)
        // {
        // sysfs_set_string("/sys/devices/platform/earpods_adc/earpods_adc/earpods_adc_sw", "on"); 
        // } else if (last_ps == 2)
        // {
        //     sysfs_set_string("/sys/devices/platform/earpods_adc/earpods_adc/earpods_adc_sw", "off"); 
        // }
    }
}

void audiohw_preinit(void)
{
    logf("hw preinit");
    alsa_controls_init("default");
    hw_init = 1;

    audiohw_mute(false);  /* No need ? */
    alsa_controls_set_bool("DOP_EN", 0); //isDSD
}

void audiohw_postinit(void)
{
    logf("hw postinit");
}

void audiohw_close(void)
{
    logf("hw close");
    /* Powering off: the DAC mutes before its supply goes, so the jack does
     * not pop (the music has already faded out). */
    if (hw_init > 0)
        audiohw_mute(true);
    hw_init = 0;
    alsa_controls_close();
}

void audiohw_set_frequency(int fsel)
{
    (void)fsel;
}
void bt_bluealsa_change_volume(int l, int r, char* mac);

void audiohw_set_volume(int vol_l, int vol_r)
{
    logf("hw vol %d %d", vol_l, vol_r);

    long l,r;

    vol_l_hw = vol_l;
    vol_r_hw = vol_r;

    l = -vol_l/5;
    r = -vol_r/5;
#ifdef HAVE_SLOPPY_UI
    /* Low gain: the whole volume range sits 6 dB lower (12 half-dB steps),
     * for sensitive earphones. */
    if (!global_settings.sloppy_gain)
    {
        l = MIN(l + 12, 255);
        r = MIN(r + 12, 255);
    }
    /* Maximum digital volume: the scale tops out at -10 dB; this lifts the
     * whole range 10 dB (20 steps) so 100% is the full digital level. */
    if (global_settings.sloppy_digital_max)
    {
        l = MAX(l - 20, 0);
        r = MAX(r - 20, 0);
    }
#endif

    if (!hw_init)
        return;

    alsa_controls_set_ints("Left Playback Volume", 1, &l);
    alsa_controls_set_ints("Right Playback Volume", 1, &r);

    int vol_avg = (vol_l + vol_r) / 2;
    hiby_notify_bt_absvol(vol_avg);

    // It won't work as is, too many system calls will cause lag and audio dropouts
    //int vol_percents = hwvolume_to_percent(vol_avg);
    //bt_bluealsa_change_volume(vol_percents, vol_percents, hiby_pcm_get_bt_mac());
}

/* How many filter responses the DAC's "Digital Filter" control accepts
 * (the CS43131 on the R1 has five), or 0 when the driver has none. Asked
 * once: the value comes from the kernel driver and does not change. */
int hiby_dac_filter_count(void)
{
    static int count = -1;
    long min, max;
    if (count >= 0)
        return count;
    if (!hw_init)
        return 0;
    count = alsa_controls_get_int_range("Digital Filter", &min, &max) &&
            min == 0 && max >= 0 ? (int)max + 1 : 0;
    if (count > 5)
        count = 5;
    logf("dac filters %d", count);
    return count;
}

/* The CS43131's non-oversampling mode, the "NOS_EN" control of HiBy's 2.0
 * driver (its 1.6 driver has none: then nothing is offered or written).
 * On, the driver writes its own setting to the DAC's filter register; off,
 * it puts back the digital filter last chosen. */
static bool nos_on;
bool hiby_dac_has_nos(void)
{
    static int has = -1;
    if (has < 0 && hw_init)
        has = alsa_has_control("NOS_EN");
    return has > 0;
}
static void nos_write(bool on)
{
    long min, max, v = on;
    /* a switch to ALSA (boolean) or a 0..1 integer: either is taken */
    if (alsa_controls_get_int_range("NOS_EN", &min, &max))
        alsa_controls_set_ints("NOS_EN", 1, &v);
    else
        alsa_controls_set_bool("NOS_EN", on);
}
void hiby_dac_set_nos(bool on)
{
    logf("nos %d", on);
    nos_on = on;
    if (hiby_dac_has_nos())
        nos_write(on);
}

void audiohw_set_filter_roll_off(int value)
{
    logf("rolloff %d", value);
    /* 0 = Sharp;
     *       1 = Slow;
     *       2 = Short Sharp
     *       3 = Short Slow
     *       4 = Super Slow */
    int count = hiby_dac_filter_count();
    if (count > 0 && value >= count)
        value = count - 1;          /* never write outside the driver's range */
    if (value < 0)
        value = 0;
    if (count == 0 && !alsa_has_control("Digital Filter"))
        return;
    long int value_hw = value;
    alsa_controls_set_ints("Digital Filter", 1, &value_hw);
    /* the filter just written replaced NOS in the register: NOS again */
    if (nos_on && hiby_dac_has_nos())
        nos_write(true);

}
