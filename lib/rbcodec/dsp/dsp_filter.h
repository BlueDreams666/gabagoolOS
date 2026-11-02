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
#ifndef DSP_FILTER_H
#define DSP_FILTER_H

/** Basic filter implementations which may be used independently **/

/* Used by: EQ, tone controls and crossfeed */

/* These depend on the fixed point formats used by the different filter types
   and need to be changed when they change.
 */
struct dsp_filter
{
    int32_t coefs[5];      /* 00h: Order is b0, b1, b2, a1, a2 */
    int32_t history[2][4]; /* 14h: Order is x-1, x-2, y-1, y-2, per channel */
    uint8_t shift;         /* 34h: Final shift after computation */
                           /* 38h */
};

void filter_shelf_coefs(unsigned long cutoff, long A, bool low, int32_t *c);
void filter_bishelf_coefs(unsigned long cutoff_low,
                          unsigned long cutoff_high,
                          long A_low, long A_high, long A,
                          struct dsp_filter *f);
void filter_pk_coefs(unsigned long cutoff, unsigned long Q, long db,
                     struct dsp_filter *f);
/* Any RBJ cookbook biquad, for the EQ's typed bands, designed in 64-bit
 * fixed point. A filter low against the rate keeps each coefficient to
 * 2^-40 in two words, so poles a hair from the unit circle stay where they
 * were designed; the rest to 2^-28 in one. The output is rounded and its
 * rounding error fed back, so low, sharp resonances (20 Hz at Q 20,
 * 192 kHz) neither hiss, drift nor ring on in silence; it saturates rather
 * than wraps. Process with filter_process_ef. */
enum filter_bq_type
{
    FILTER_BQ_PEAK = 1,
    FILTER_BQ_LOW_SHELF,
    FILTER_BQ_HIGH_SHELF,
    FILTER_BQ_LOW_PASS,
    FILTER_BQ_HIGH_PASS,
};
struct dsp_filter_ef
{
    int32_t fine[5];       /* b0 b1 b2 -a1 -a2 past coefs[]' Q28: Q40 */
    int32_t a30[2];        /* -a1 -a2 at Q30, for the error feedback */
    int32_t err[2][2];     /* each channel's last two rounding errors */
    uint8_t post;          /* the b's divided by 2^post, the output multiplied back */
    bool fine_on;          /* fine[] in use (a filter low against the rate) */
};
void filter_bq_coefs(int type, unsigned long cutoff, unsigned long Q, long db,
                     struct dsp_filter *f, struct dsp_filter_ef *ef);
void filter_flush_ef(struct dsp_filter_ef *ef);
void filter_process_ef(struct dsp_filter *f, struct dsp_filter_ef *ef,
                       int32_t * const buf[], int count, unsigned int channels);
void filter_copy(struct dsp_filter *dst, const struct dsp_filter *src);
void filter_flush(struct dsp_filter *f);
void filter_process(struct dsp_filter *f, int32_t * const buf[], int count,
                    unsigned int channels);
/* ring buffer */
void enqueue(int32_t var, int32_t* buffer, int *head, int boundary);
int32_t dequeue(int32_t* buffer, int *head, int boundary);
#endif /* DSP_FILTER_H */
