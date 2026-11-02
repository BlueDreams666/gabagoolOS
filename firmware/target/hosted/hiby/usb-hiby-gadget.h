/***************************************************************************
 *             __________               __   ___
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
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
#ifndef __USB_HIBY_H__
#define __USB_HIBY_H__

#include <stdbool.h>

/**
 * Handle the setting and unsetting of adb usb mode
 */
void hiby_set_usb_mode(int mode);

/* The last handover of the card to a computer: 0 it was unmounted and
 * exported, -1 none yet, else the errno umount gave (EBUSY: a file was
 * still open), in which case the computer saw a drive with no medium. */
int usb_card_handover_error(void);
const char *usb_card_busy_files(void);   /* when it would not come off: what of ours was on it */
bool usb_card_retry(void);                /* USB screen: try a failed handover again; true while still busy */
bool usb_card_away(void);                 /* from the handover's start until the card is mounted again: write nothing to it */

/* The USB trace: each step from plug to unplug, timed, kept in RAM
 * (/tmp/usb-trace.txt) and copied to .rockbox/sloppy/usb-trace.txt on the
 * card whenever the card is the player's. usb_trace_flush() does the copy
 * and does nothing while the card is the computer's. */
void usb_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void usb_trace_flush(void);

#endif /* __USB_HIBY_H__ */
