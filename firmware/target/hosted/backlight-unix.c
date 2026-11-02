/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2019 by Roman Stolyarov
 * Copyright (C) 2025 by Melissa Autumn
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include "config.h"
#include "backlight-target.h"
#include "sysfs.h"
#include "panic.h"
#include "lcd.h"

#if defined(BACKLIGHT_RG_NANO)
static const char * const sysfs_bl_brightness =
    "/sys/class/backlight/backlight/brightness";

static const char * const sysfs_bl_power =
    "/sys/class/backlight/backlight/bl_power";
#elif defined(BACKLIGHT_HIBY)
static const char * const sysfs_bl_brightness =
    "/sys/class/backlight/backlight_pwm0/brightness";

static const char * const sysfs_bl_power =
#ifdef HAVE_TOUCHSCREEN
    /* Framebuffer powers off both touch and screen */
    "/sys/class/graphics/fb0/blank";
#else
    "/sys/class/backlight/backlight_pwm0/bl_power";
#endif

#else
static const char * const sysfs_bl_brightness =
    "/sys/class/backlight/pwm-backlight.0/brightness";

static const char * const sysfs_bl_power =
    "/sys/class/backlight/pwm-backlight.0/bl_power";
#endif

bool backlight_hw_init(void)
{
    backlight_hw_on();
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
#ifdef HAVE_BUTTON_LIGHT
    buttonlight_hw_on();
#ifdef HAVE_BUTTONLIGHT_BRIGHTNESS
    buttonlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
#endif
#endif
    return true;
}

static int last_bl = -1;

#if defined(BACKLIGHT_HIBY) && defined(HAVE_TOUCHSCREEN) && defined(HAVE_BACKLIGHT_FINE_BRIGHTNESS)
/* Blanking fb0 turns the panel off under a lit backlight, so the picture
 * fades out slowly (about a second) instead of going black. The PWM goes
 * to 0 first, which is black at once; the level comes back on unblank. */
#define BL_DARK_FIRST
static int bl_raw = -1;                  /* the level last asked for, raw */
#endif

/* Ref: https://www.kernel.org/doc/html/latest/gpu/backlight.html#c.backlight_properties */
#define BACKLIGHT_POWER_ON 0
#define BACKLIGHT_POWER_REDUCED 1
#define BACKLIGHT_POWER_OFF 4

void backlight_hw_on(void)
{
    if (last_bl != BACKLIGHT_POWER_ON) {
#ifdef HAVE_LCD_ENABLE
        lcd_enable(true);
#endif
        last_bl = BACKLIGHT_POWER_ON;
        sysfs_set_int(sysfs_bl_power, last_bl);
#ifdef BL_DARK_FIRST
        if (bl_raw > 0)
            sysfs_set_int(sysfs_bl_brightness, bl_raw);
#endif
    }
}

void backlight_hw_off(void)
{
    if (last_bl != BACKLIGHT_POWER_REDUCED) {
#ifdef BL_DARK_FIRST
        sysfs_set_int(sysfs_bl_brightness, 0);
#endif
        last_bl = BACKLIGHT_POWER_REDUCED;
        sysfs_set_int(sysfs_bl_power, last_bl);
#ifdef HAVE_LCD_ENABLE
        lcd_enable(false);
#endif
    }
}

#ifdef HAVE_BACKLIGHT_FINE_BRIGHTNESS
/* The driver's own range. The setting has always been written straight
 * through as 1..100, which is only right if the driver's range is 100;
 * scaling through max_brightness keeps that exactly when it is, and is
 * where resolution below 1 % comes from when it is larger. */
static int hw_max_brightness(void)
{
    static int hw_max = -1;
    if (hw_max < 0)
    {
        char path[64];
        const char *slash = strrchr(sysfs_bl_brightness, '/');
        size_t dir = slash ? (size_t)(slash - sysfs_bl_brightness) : 0;
        snprintf(path, sizeof(path), "%.*s/max_brightness",
                 (int)dir, sysfs_bl_brightness);
        if (!sysfs_get_int(path, &hw_max) || hw_max <= 0)
            hw_max = MAX_BRIGHTNESS_SETTING;
    }
    return hw_max;
}

void backlight_hw_brightness_fine(int permille)
{
    int max = hw_max_brightness();
    int raw;

    if (permille > 1000)
        permille = 1000;
    raw = (permille * max + 500) / 1000;
    /* The R1's PWM backlight goes dark (and flickers) at a full duty
     * cycle: the top of the range stays a few percent short of it. */
    if (max > 1 && raw > max * 97 / 100)
        raw = max * 97 / 100;
    if (raw < 1)
        raw = 1;
#ifdef BL_DARK_FIRST
    bl_raw = raw;
    if (last_bl != BACKLIGHT_POWER_ON && last_bl != -1)
        return;                          /* dark: set on unblank */
#endif
    sysfs_set_int(sysfs_bl_brightness, raw);
}
#endif

void backlight_hw_brightness(int brightness)
{
    /* cap range, just in case */
    if (brightness > MAX_BRIGHTNESS_SETTING)
        brightness = MAX_BRIGHTNESS_SETTING;
    if (brightness < MIN_BRIGHTNESS_SETTING)
        brightness = MIN_BRIGHTNESS_SETTING;

#ifdef HAVE_BACKLIGHT_FINE_BRIGHTNESS
    backlight_hw_brightness_fine(brightness * 1000 / MAX_BRIGHTNESS_SETTING);
#else
    sysfs_set_int(sysfs_bl_brightness, brightness);
#endif
}

#ifdef HAVE_LCD_SLEEP
void lcd_awake(void)
{
    /* Nothing to do */
}

void lcd_sleep(void)
{
    if (last_bl != BACKLIGHT_POWER_OFF) {
        last_bl = BACKLIGHT_POWER_OFF;
        sysfs_set_int(sysfs_bl_power, last_bl);
#ifdef HAVE_LCD_ENABLE
        lcd_enable(false);
#endif
    }
}
#endif
