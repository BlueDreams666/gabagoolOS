/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2006-2007 Thom Johansen
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
#ifndef _EQ_H
#define _EQ_H

/* => support from 3 to 32 bands */
#define EQ_NUM_BANDS 10

/* A band's filter. EQ_TYPE_AUTO (0) is the fixed layout every saved EQ
 * before typed bands has: the first band a low shelf, the last a high
 * shelf, the rest peaks. Any other type is that filter in any band. Every
 * band is the same biquad (filter_bq_coefs), Q up to 20. Values past the
 * list read as EQ_TYPE_AUTO. */
enum eq_band_type
{
    EQ_TYPE_AUTO = 0,
    EQ_TYPE_PEAK,
    EQ_TYPE_LOW_SHELF,
    EQ_TYPE_HIGH_SHELF,
    EQ_TYPE_LOW_PASS,
    EQ_TYPE_HIGH_PASS,
    EQ_TYPE_COUNT
};

#define EQ_Q_TYPED_MAX 200  /* Q 20 */

struct eq_band_setting
{
    int cutoff; /* Hz */
    int q;
    int gain;   /* +/- dB */
    int type;   /* enum eq_band_type; 0 for the fixed layout */
};

/* the filter a band applies: its own type, or its place's */
static inline int eq_band_kind(int band, const struct eq_band_setting *s)
{
    if (s->type > EQ_TYPE_AUTO && s->type < EQ_TYPE_COUNT)
        return s->type;
    return band == 0 ? EQ_TYPE_LOW_SHELF :
           band == EQ_NUM_BANDS - 1 ? EQ_TYPE_HIGH_SHELF : EQ_TYPE_PEAK;
}

static inline bool eq_kind_is_pass(int kind)
{
    return kind == EQ_TYPE_LOW_PASS || kind == EQ_TYPE_HIGH_PASS;
}

/* a band that changes the sound: a gain, or a pass filter (which has none) */
static inline bool eq_band_active(int band, const struct eq_band_setting *s)
{
    return s->gain != 0 || eq_kind_is_pass(eq_band_kind(band, s));
}

/** DSP interface **/
void dsp_set_eq_precut(int precut);
void dsp_set_eq_coefs(int band, const struct eq_band_setting *setting);
void dsp_eq_enable(bool enable);

#endif /* _EQ_H */
