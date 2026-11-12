/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2020 Solomon Peachy

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
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include "kernel.h"
#include "sysfs.h"
#include "button.h"
#include "panic.h"

#ifdef HAVE_SCROLLWHEEL
#include "powermgmt.h"
#if defined(HAVE_BACKLIGHT) || defined(HAVE_BUTTON_LIGHT)
#include "backlight.h"
#endif
#endif /* HAVE_SCROLLWHEEL */
#ifdef BUTTON_TOUCH_WAKES
#include "backlight.h"
#endif

/* TODO:  HAVE_SCROLLWHEEL is a hack.  Instead of posting the exact number
   of clicks, instead do it similar to the ipod clickwheel and post
   the wheel angular velocity (degrees per second)

   * Track the relative position  (ie based on click events)
   * Use WHEELCLICKS_PER_ROTATION to convert clicks to angular distance
   * Compute to angular velocity (degrees per second)

 */
#define NR_POLL_DESC    5

static int num_devices = 0;
static struct pollfd poll_fds[NR_POLL_DESC];

bool button_add_input_device(int i)
{
    int fd = poll_fds[i].fd;
    if (fd >= 0)
        close(fd);

    char path[32];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    poll_fds[i].fd = fd >= 0 ? fd : -1;
    if(fd >= 0)
    {
        poll_fds[i].events = POLLIN;
        poll_fds[i].revents = 0;
        if (num_devices <= i)
            num_devices = i + 1;
    }
    return fd >= 0;
}

bool button_input_device_open(int i)
{
    return poll_fds[i].fd >= 0;
}

void button_init_device(void)
{
    for(int i = 0; i < NR_POLL_DESC; i++)
    {
        poll_fds[i].fd = -1;
        button_add_input_device(i);
    }
}

#if defined(HAVE_HIBY_BLUETOOTH) && !defined(BOOTLOADER) && !defined(SIMULATOR)
void hiby_debug_log(const char *format, ...);
#define INPUT_LOG(...) hiby_debug_log(__VA_ARGS__)
#else
#define INPUT_LOG(...) do { } while (0)
#endif
/* A built-in device (keys, touchscreen) closed on an error is opened
 * again: closed for good, it left the player with no touch or keys until a
 * reboot, the screen going dark under a game nobody could steer any more
 * (a test R1, Nov 10). The Bluetooth remote's own slot has its own tries. */
#define BUILTIN_INPUTS 4
static unsigned input_lost;              /* built-in slots closed on an error */
static long input_retry_at;

void button_remove_input_device(int i)
{
    int fd = poll_fds[i].fd;
    if (fd < 0)
        return;

    if (i == num_devices - 1)
        num_devices = i;

    close(fd);
    poll_fds[i].fd = -1;
}

void button_close_device(void)
{
    num_devices = 0;
    /* close descriptors */
    for(int i = 0; i < NR_POLL_DESC; i++)
    {
        button_remove_input_device(i);
    }
}

#ifdef BUTTON_DELAY_RELEASE
static int button_delay_release = 0;
static int delay_tick = 0;
#endif

#ifdef HAVE_TOUCHSCREEN
/* Last known touchscreen coordinates. */
static int _last_x = 0;
static int _last_y = 0;

/* Last known touchscreen state. */
static enum
{
    TOUCHSCREEN_STATE_UNKNOWN = 0,
    TOUCHSCREEN_STATE_UP,
    TOUCHSCREEN_STATE_DOWN
} _last_touch_state = TOUCHSCREEN_STATE_UNKNOWN;

// XXX ... figure out what is standard.
#define EVENT_VALUE_TOUCHSCREEN_PRESS    1
#define EVENT_VALUE_TOUCHSCREEN_RELEASE -1

static int ts_enabled = 1;

void touchscreen_enable_device(bool en)
{
    ts_enabled = en;
}

/*
 * Previously ABS_MT_TRACKING_ID events were used for tracking _last_touch_state, however this doesn't work great on some touchscreens
 * _last_touch_state probably shouldn't be updated here tbh, but I don't want to refactor this code anymore.
 *
 * So we're using a fake event that is called on BUTTON_TOUCH. This will break any touchscreens which don't have this enabled. (Sorry!)
 */
#define ABS_FAKE_PRESSED 0x1337

static void trail_push(bool down);
static bool handle_touchscreen_event(__u16 code, __s32 value)
{
    bool read_more = false;

    switch(code)
    {
        case ABS_X:
        case ABS_MT_POSITION_X:
        {
            _last_x = value;

            /* x -> next will be y. */
            read_more = true;

            break;
        }

        case ABS_Y:
        case ABS_MT_POSITION_Y:
        {
            _last_y = value;
            break;
        }

        case ABS_FAKE_PRESSED:
        {
            if(value == EVENT_VALUE_TOUCHSCREEN_PRESS)
            {
                _last_touch_state = TOUCHSCREEN_STATE_DOWN;

                /* Press -> next will be x. */
                read_more = true;
            }
            else
            {
                if (_last_touch_state == TOUCHSCREEN_STATE_DOWN)
                    trail_push(false);
                _last_touch_state = TOUCHSCREEN_STATE_UP;
            }
            break;
        }
    }

    return read_more;
}

/* Every sample of a contact in order, not only where it ended up: the
 * loop below drains all waiting events at once, and a quick flick that
 * went down and up between two reads (a game frame on the R1 takes
 * 50-90 ms) left nothing at all; Snake missed swipes (a test R1, Nov 12).
 * One sample per SYN_REPORT while down, and one at the lift. */
#define TOUCH_TRAIL 64
static int trail_pos[TOUCH_TRAIL];
static bool trail_down[TOUCH_TRAIL];
static volatile unsigned trail_n;
static void trail_push(bool down)
{
    int pos = 0;
    if (!touchscreen_to_pixels(_last_x, _last_y, &pos))
        return;
    trail_pos[trail_n % TOUCH_TRAIL] = pos;
    trail_down[trail_n % TOUCH_TRAIL] = down;
    trail_n++;
}
unsigned button_touch_trail_end(void)
{
    return trail_n;
}
/* the next sample after *at: 1 down, 0 the lift, -1 none left */
int button_touch_trail(unsigned *at, int *pos)
{
    unsigned n = trail_n;
    if (n - *at > TOUCH_TRAIL)
        *at = n - TOUCH_TRAIL;
    if (*at == n)
        return -1;
    unsigned i = (*at)++ % TOUCH_TRAIL;
    *pos = trail_pos[i];
    return trail_down[i];
}
#endif

#ifdef HAVE_BUTTON_DATA
#define BDATA int *data
#else
#define BDATA void
#endif
int button_read_device(BDATA)
{
    static int button_bitmap = 0;
#ifdef BUTTON_TOUCH_WAKES
    /* the last tap was eaten to wake the screen */
    static bool touch_wake_swallowed = false;
#endif
    struct input_event event;

#if defined(HAVE_BUTTON_DATA) && !defined(HAVE_TOUCHSCREEN)
    (void)data;
#endif

#ifdef HAVE_SCROLLWHEEL
    int wheel_ticks = 0;
#endif

#ifdef BUTTON_DELAY_RELEASE
    /* First de-assert delayed-release buttons */
    if (button_delay_release && current_tick >= delay_tick)
    {
        button_bitmap &= ~button_delay_release;
        button_delay_release = 0;
    }
#endif

    /* check if there are any events pending and process them */
    while(poll(poll_fds, num_devices, 0)) {
        for(int i = 0; i < num_devices; i++) {
            /* read only if non-blocking */
            if(poll_fds[i].revents & POLLIN) {
                int size = read(poll_fds[i].fd, &event, sizeof(event));
                if(size == (int)sizeof(event)) {
                    switch(event.type) {
                    case EV_KEY: {
#ifdef BUTTON_TOUCH_WAKES
                        /* A tap on a dimmed or dark screen wakes it and goes
                         * no further. The release finds it lit and only
                         * marks the touch as up.
                         *
                         * Only ever one tap in a row: if the screen still
                         * reads dark on the next tap, something is keeping
                         * it from coming back and swallowing again would
                         * leave the touchscreen dead until a reboot. Let the
                         * tap through instead. */
                        if (event.code == BTN_TOUCH && event.value)
                        {
                            bool dark = !is_backlight_lit();
                            /* Input locked: the screen stays dark and the
                             * tap goes nowhere. Getting back in is a key
                             * combo, which the physical path still sees. */
                            if (dark && !button_touch_wake_enabled())
                                break;
                            if (dark && !touch_wake_swallowed)
                            {
                                touch_wake_swallowed = true;
                                backlight_wake();
                                break;
                            }
                            touch_wake_swallowed = false;
                        }
#endif
#ifdef BUTTON_NEED_DEV_INPUT_ID
                        int bmap = button_map_with_id(event.code, i);
#else
                        /* map linux event code to rockbox button bitmap */
                        int bmap = button_map(event.code);
#endif
                        
                        /* event.value == 0x10000 means press
                         * event.value == 0 means release
                         */
                        if(event.value) {
#ifdef HAVE_SCROLLWHEEL
                            /* Filter out wheel ticks */
                            if (bmap & BUTTON_SCROLL_BACK)
                                wheel_ticks--;
                            else if (bmap & BUTTON_SCROLL_FWD)
                                wheel_ticks++;
                            bmap &= ~(BUTTON_SCROLL_BACK|BUTTON_SCROLL_FWD);
#endif
#ifdef BUTTON_DELAY_RELEASE
                            bmap &= ~BUTTON_DELAY_RELEASE;
#endif
#if defined(HAVE_TOUCHSCREEN) && defined(BUTTON_TOUCH)
                            /* Some touchscreens give us actual touch/untouch as a "key" */
                            if (bmap & BUTTON_TOUCH) {
                                handle_touchscreen_event(ABS_FAKE_PRESSED, EVENT_VALUE_TOUCHSCREEN_PRESS);
                                bmap &= ~BUTTON_TOUCH;
                            }
                            else
                            {
                                reset_last_touch();
                                _last_touch_state = TOUCHSCREEN_STATE_UNKNOWN;
                            }
#endif
                            button_bitmap |= bmap;
                        } else {
#if defined(HAVE_TOUCHSCREEN) && defined(BUTTON_TOUCH)
                            /* Some touchscreens give us actual touch/untouch as a "key" */
                            if (bmap & BUTTON_TOUCH) {
                                handle_touchscreen_event(ABS_FAKE_PRESSED, EVENT_VALUE_TOUCHSCREEN_RELEASE);
                                bmap &= ~BUTTON_TOUCH;
                            }
                            else
                            {
                                reset_last_touch();
                                _last_touch_state = TOUCHSCREEN_STATE_UNKNOWN;
                            }
#endif
#ifdef BUTTON_DELAY_RELEASE
                            /* Delay the release of any requested buttons */
                            if (bmap & BUTTON_DELAY_RELEASE) {
                                button_delay_release |= bmap & ~BUTTON_DELAY_RELEASE;
                                delay_tick = current_tick + HZ/20;
                                bmap = 0;
                            }
#endif
#ifdef HAVE_SCROLLWHEEL
                            /* Wheel gives us press+release back to back; ignore the release */
                            bmap &= ~(BUTTON_SCROLL_BACK|BUTTON_SCROLL_FWD);
#endif
                            button_bitmap &= ~bmap;
                        }
                        break;
                    }
#ifdef HAVE_TOUCHSCREEN
                    case EV_ABS: {
                        if (ts_enabled) {
                            handle_touchscreen_event(event.code, event.value);
                        } else {
                             /* If disabled... ignore */
                            _last_touch_state = TOUCHSCREEN_STATE_UNKNOWN;
                        }
                        break;
                    }
                    case EV_SYN:
                        if (event.code == SYN_REPORT && ts_enabled &&
                            _last_touch_state == TOUCHSCREEN_STATE_DOWN)
                            trail_push(true);
                        break;
#endif
                    default:
                        /* Ignore other event types */
                        break;
                    }
                }
            }
            /* device was removed/disconnected — close it to stop poll returning POLLHUP forever */
            else if (poll_fds[i].revents & (POLLERR | POLLHUP)) {
                INPUT_LOG("input: event%d closed on an error (%s)", i,
                          poll_fds[i].revents & POLLHUP ? "hang-up" : "error");
                button_remove_input_device(i);
                if (i < BUILTIN_INPUTS) {
                    input_lost |= 1u << i;
                    input_retry_at = current_tick + HZ;
                }
            }
        }
    }

#ifdef HAVE_SCROLLWHEEL
    /* Reset backlight and poweroff timers */
    if (wheel_ticks) {
#ifdef HAVE_BACKLIGHT
        backlight_on();
#endif
#ifdef HAVE_BUTTON_LIGHT
        buttonlight_on();
#endif
        reset_poweroff_timer();
    }

    if (wheel_ticks > 0)
    {
        while (wheel_ticks-- > 0)
        {
            button_queue_post(BUTTON_SCROLL_FWD, 0);
        }
    }
    else if (wheel_ticks < 0)
    {
        while (wheel_ticks++ < 0)
        {
            button_queue_post(BUTTON_SCROLL_BACK, 0);
        }
    }
#endif /* HAVE_SCROLLWHEEL */

    if (input_lost && TIME_AFTER(current_tick, input_retry_at))
    {
        input_retry_at = current_tick + HZ;
        for (int i = 0; i < BUILTIN_INPUTS; i++)
            if ((input_lost & (1u << i)) && button_add_input_device(i))
            {
                input_lost &= ~(1u << i);
                INPUT_LOG("input: event%d open again", i);
            }
    }

#if defined(HIBY_R1) && !defined(SIMULATOR)
    /* Power held in our bitmap but not in the kernel's: its release was
     * lost (events dropped while we were behind, or the device went), and
     * the button driver's held-power backstop switched a test R1 off while
     * it was being used by touch (Nov 10). Asked once a second while it
     * reads held; a real hold is untouched. */
    if (button_bitmap & BUTTON_POWER)
    {
        static long next_check;
        if (TIME_AFTER(current_tick, next_check))
        {
            next_check = current_tick + HZ;
            bool down = false, asked = false;
            for (int i = 0; i < num_devices && !down; i++)
            {
                unsigned char keys[KEY_MAX / 8 + 1];
                if (poll_fds[i].fd < 0 ||
                    ioctl(poll_fds[i].fd, EVIOCGKEY(sizeof keys), keys) < 0)
                    continue;
                asked = true;
                down = keys[KEY_POWER / 8] & (1 << (KEY_POWER % 8));
            }
            if (asked && !down)
                button_bitmap &= ~BUTTON_POWER;
        }
    }
#endif

#ifdef HAVE_TOUCHSCREEN
    int touch = touchscreen_to_pixels(_last_x, _last_y, data);

    if(_last_touch_state == TOUCHSCREEN_STATE_DOWN)
    {
        return button_bitmap | touch;
    }
#endif

    return button_bitmap;
}
