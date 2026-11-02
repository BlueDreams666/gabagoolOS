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
#include "rbcodecconfig.h"
#include "fixedpoint.h"
#include "fracmul.h"
#include "dsp_filter.h"
#include "replaygain.h"
#include <string.h>

enum filter_shift
{
    FILTER_BISHELF_SHIFT = 5, /* For bishelf (bass/treble) */
    FILTER_PEAK_SHIFT = 4,    /* Each peaking filter */
};

/** 
 * Calculate first order shelving filter. Filter is not directly usable by the
 * filter_process() function.
 * @param cutoff shelf midpoint frequency. See eq_pk_coefs for format.
 * @param A decibel value multiplied by ten, describing gain/attenuation of
 * shelf. Max value is 24 dB.
 * @param low true for low-shelf filter, false for high-shelf filter.
 * @param c pointer to coefficient storage. Coefficients are s4.27 format.
 */
void filter_shelf_coefs(unsigned long cutoff, long A, bool low, int32_t *c)
{
    long sin, cos;
    int32_t b0, b1, a0, a1; /* s3.28 */
    const long g = get_replaygain_int(A*5) << 4; /* 10^(db/40), s3.28 */

    sin = fp_sincos(cutoff/2, &cos);
    if (low) {
        const int32_t sin_div_g = fp_div(sin, g, 25);
        const int32_t sin_g = FRACMUL(sin, g);
        cos >>= 3;
        b0 = sin_g + cos;             /* 0.25 .. 4.10 */
        b1 = sin_g - cos;             /* -1 .. 3.98 */
        a0 = sin_div_g + cos;         /* 0.25 .. 4.10 */
        a1 = sin_div_g - cos;         /* -1 .. 3.98 */
    } else {
        const int32_t cos_div_g = fp_div(cos, g, 25);
        const int32_t cos_g = FRACMUL(cos, g);
        sin >>= 3;
        b0 = sin + cos_g;             /* 0.25 .. 4.10 */
        b1 = sin - cos_g;             /* -3.98 .. 1 */
        a0 = sin + cos_div_g;         /* 0.25 .. 4.10 */
        a1 = sin - cos_div_g;         /* -3.98 .. 1 */
    }

    const int32_t rcp_a0 = fp_div(1, a0, 57); /* 0.24 .. 3.98, s2.29 */
    *c++ = FRACMUL_SHL(b0, rcp_a0, 1);       /* 0.063 .. 15.85 */
    *c++ = FRACMUL_SHL(b1, rcp_a0, 1);       /* -15.85 .. 15.85 */
    *c++ = -FRACMUL_SHL(a1, rcp_a0, 1);      /* -1 .. 1 */
}


/** 
 * Calculate second order section filter consisting of one low-shelf and one
 * high-shelf section.
 * @param cutoff_low low-shelf midpoint frequency. See filter_pk_coefs for format.
 * @param cutoff_high high-shelf midpoint frequency.
 * @param A_low decibel value multiplied by ten, describing gain/attenuation of
 * low-shelf part. Max value is 24 dB.
 * @param A_high decibel value multiplied by ten, describing gain/attenuation of
 * high-shelf part. Max value is 24 dB.
 * @param A decibel value multiplied by ten, describing additional overall gain.
 * @param c pointer to coefficient storage. Coefficients are s4.27 format.
 */
void filter_bishelf_coefs(unsigned long cutoff_low, unsigned long cutoff_high,
                          long A_low, long A_high, long A,
                          struct dsp_filter *f)
{
    const long g = get_replaygain_int(A*10) << 7; /* 10^(db/20), s0.31 */
    int32_t c_ls[3], c_hs[3];

    filter_shelf_coefs(cutoff_low, A_low, true, c_ls);
    filter_shelf_coefs(cutoff_high, A_high, false, c_hs);
    c_ls[0] = FRACMUL(g, c_ls[0]);
    c_ls[1] = FRACMUL(g, c_ls[1]);

    /* now we cascade the two first order filters to one second order filter
     * which can be used by filter_process(). these resulting coefficients have a
     * really wide numerical range, so we use a fixed point format which will
     * work for the selected cutoff frequencies (in tone_controls.c) only.
     */
    const int32_t b0 = c_ls[0], b1 = c_ls[1], b2 = c_hs[0], b3 = c_hs[1];
    const int32_t a0 = c_ls[2], a1 = c_hs[2];

    int32_t *c = f->coefs;
    *c++ = FRACMUL_SHL(b0, b2, 4);
    *c++ = FRACMUL_SHL(b0, b3, 4) + FRACMUL_SHL(b1, b2, 4);
    *c++ = FRACMUL_SHL(b1, b3, 4);
    *c++ = a0 + a1;
    *c   = -FRACMUL_SHL(a0, a1, 4);

    f->shift = FILTER_BISHELF_SHIFT;
}


/* Coef calculation taken from Audio-EQ-Cookbook.txt by Robert Bristow-Johnson.
 * Slightly faster calculation can be done by deriving forms which use tan()
 * instead of cos() and sin(), but the latter are far easier to use when doing
 * fixed point math, and performance is not a big point in the calculation part.
 * All the 'a' filter coefficients are negated so we can use only additions
 * in the filtering equation.
 */

/** 
 * Calculate second order section peaking filter coefficients.
 * @param cutoff a value from 0 to 0x80000000, where 0 represents 0 Hz and
 * 0x80000000 represents the Nyquist frequency (samplerate/2).
 * @param Q Q factor value multiplied by ten. Lower bound is artificially set
 * at 0.5.
 * @param db decibel value multiplied by ten, describing gain/attenuation at
 * peak freq. Max value is 24 dB.
 * @param c pointer to coefficient storage. Coefficients are s3.28 format.
 */
void filter_pk_coefs(unsigned long cutoff, unsigned long Q, long db,
                     struct dsp_filter *f)
{
    long cs;
    const long one = 1 << 28; /* s3.28 */
    const long A = get_replaygain_int(db*5) << 5; /* 10^(db/40), s2.29 */
    const long alpha = fp_sincos(cutoff, &cs)/(2*Q)*10 >> 1; /* s1.30 */
    int32_t a0, a1, a2; /* these are all s3.28 format */
    int32_t b0, b1, b2;
    const long alphadivA = fp_div(alpha, A, 27);
    const long alphaA = FRACMUL(alpha, A);

    /* possible numerical ranges are in comments by each coef */
    b0 = one + alphaA;                /* [1 .. 5] */
    b1 = a1 = -2*(cs >> 3);           /* [-2 .. 2] */
    b2 = one - alphaA;                /* [-3 .. 1] */
    a0 = one + alphadivA;             /* [1 .. 5] */
    a2 = one - alphadivA;             /* [-3 .. 1] */

    /* range of this is roughly [0.2 .. 1], but we'll never hit 1 completely */
    int32_t *c = f->coefs;
    const long rcp_a0 = fp_div(1, a0, 59); /* s0.31 */
    *c++ = FRACMUL(b0, rcp_a0);         /* [0.25 .. 4] */
    *c++ = FRACMUL(b1, rcp_a0);         /* [-2 .. 2] */
    *c++ = FRACMUL(b2, rcp_a0);         /* [-2.4 .. 1] */
    *c++ = FRACMUL(-a1, rcp_a0);        /* [-2 .. 2] */
    *c   = FRACMUL(-a2, rcp_a0);        /* [-0.6 .. 1] */

    f->shift = FILTER_PEAK_SHIFT;
}

/** Typed biquads (filter_bq_coefs). The design runs in 64-bit Q40, so
 * the small differences that place a low, sharp filter (1 - cos w is
 * 4e-7 for 20 Hz at 192 kHz) keep their precision until the coefficients
 * are rounded. Only integer arithmetic, as the rest of this file. */
#define BQ_FRAC 40
#ifndef BQ_FINE_RATIO
#define BQ_FINE_RATIO 1000  /* filters below fs / 1000 keep two words a coefficient */
#endif
#define BQ_ONE ((int64_t)1 << BQ_FRAC)

/* the 128-bit product of two magnitudes, as hi:lo */
static void bq_umul(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    *lo = (uint32_t)ll | (mid << 32);
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

/* (a * b) >> 40, rounded */
static int64_t bq_mul(int64_t a, int64_t b)
{
    bool neg = (a < 0) != (b < 0);
    uint64_t hi, lo;
    bq_umul(a < 0 ? -(uint64_t)a : (uint64_t)a, b < 0 ? -(uint64_t)b : (uint64_t)b, &hi, &lo);
    uint64_t r = (hi << (64 - BQ_FRAC)) | (lo >> BQ_FRAC);
    r += (lo >> (BQ_FRAC - 1)) & 1;
    return neg ? -(int64_t)r : (int64_t)r;
}

/* (a << 40) / b, rounded. |a| and |b| stay below 2^62 here. */
static int64_t bq_div(int64_t a, int64_t b)
{
    bool neg = (a < 0) != (b < 0);
    uint64_t n = a < 0 ? -(uint64_t)a : (uint64_t)a, d = b < 0 ? -(uint64_t)b : (uint64_t)b;
    uint64_t q = 0, r = 0;
    if (d == 0)
        return neg ? -INT64_MAX : INT64_MAX;
    for (int i = 63 + BQ_FRAC; i >= 0; i--)
    {
        r = (r << 1) | (i >= BQ_FRAC ? (n >> (i - BQ_FRAC)) & 1 : 0);
        q <<= 1;
        if (r >= d) { r -= d; q |= 1; }
    }
    if (2 * r >= d)
        q++;
    return neg ? -(int64_t)q : (int64_t)q;
}

static uint64_t bq_isqrt(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    for (; bit; bit >>= 2)
    {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
    }
    return r;
}

/* sin and cos of half the angle 'phase' (2^32 a full turn, so the half
   angle is 0..pi/2), Q40, by their Taylor series: CORDIC's sine is only
   good to about 1e-8, a hundredth of 1 - cos w at 20 Hz and 192 kHz */
static void bq_sincos_half(unsigned long phase, int64_t *s, int64_t *c)
{
    const int64_t x = bq_mul((int64_t)phase << 8, INT64_C(3454217652358)); /* pi */
    const int64_t x2 = bq_mul(x, x);
    int64_t ts = BQ_ONE, tc = BQ_ONE;
    for (int k = 16; k >= 2; k -= 2)
    {
        ts = BQ_ONE - bq_mul(x2, ts) / ((k + 1) * k);   /* sin: 1 - x^2/(k(k+1)) (...) */
        tc = BQ_ONE - bq_mul(x2, tc) / (k * (k - 1));   /* cos: 1 - x^2/(k(k-1)) (...) */
    }
    *s = bq_mul(x, ts);
    *c = tc;
}

/* 10^(db / 400), db in tenths of a dB: the A of a gain, Q40, by e^y's
   series (|y| <= 1.4 here); replaygain's table is good to only 2.4e-4,
   which moves a sharp low notch's bandwidth visibly */
static int64_t bq_gain_a(long db)
{
    const int64_t y = INT64_C(2531719083691) * db / 400; /* ln 10 */
    int64_t t = BQ_ONE;
    for (int k = 24; k >= 1; k--)
        t = BQ_ONE + bq_mul(y, t) / k;
    return t;
}

/* a Q40 value as a 32-bit coefficient with 'frac' fraction bits, rounded */
static int32_t bq_coef(int64_t v, int frac)
{
    int sh = BQ_FRAC - frac;
    int64_t c = sh > 0 ? (v + ((int64_t)1 << (sh - 1))) >> sh : v;
    return c > INT32_MAX ? INT32_MAX : c < INT32_MIN ? INT32_MIN : (int32_t)c;
}

/**
 * Calculate any RBJ cookbook biquad.
 * @param type a filter_bq_type.
 * @param cutoff as filter_pk_coefs: 0x80000000 is the Nyquist frequency.
 * @param Q Q factor multiplied by ten, 1 (0.1) and up; a shelf's Q as RBJ's.
 * @param db gain in tenths of a dB, +-24 dB; the pass filters ignore it.
 * @param ef its error feedback and output shift, for filter_process_ef.
 */
void filter_bq_coefs(int type, unsigned long cutoff, unsigned long Q, long db,
                     struct dsp_filter *f, struct dsp_filter_ef *ef)
{
    const int64_t one = BQ_ONE;
    if (Q < 1)
        Q = 1;
    db = db > 240 ? 240 : db < -240 ? -240 : db;
    /* past Nyquist a filter turns over on itself; at 0 Hz its poles sit
       on the unit circle. Keep it inside: about 0.7 Hz to 0.49 fs */
    if (cutoff > 0x7d70a3d7ul)
        cutoff = 0x7d70a3d7ul;
    if (cutoff < (1ul << 14))
        cutoff = 1ul << 14;

    /* the half angle: its sine and cosine keep 1 - cos w and 1 + cos w
       precise where cos w comes close to +-1 */
    int64_t sh, c2;
    bq_sincos_half(cutoff, &sh, &c2);
    const int64_t omc = 2 * bq_mul(sh, sh);         /* 1 - cos w */
    const int64_t opc = 2 * bq_mul(c2, c2);         /* 1 + cos w */
    const int64_t cw = omc < opc ? one - omc : opc - one;
    const int64_t sw = 2 * bq_mul(sh, c2);
    const int64_t alpha = sw * 5 / (int64_t)Q;      /* sin w / 2Q */
    const int64_t A = bq_gain_a(db);                /* 10^(dB/40) */
    const int64_t sqA = (int64_t)bq_isqrt((uint64_t)A << 22) << 9;
    int64_t b0, b1, b2, a0, a1, a2;

    switch (type)
    {
    case FILTER_BQ_LOW_PASS:
        b0 = b2 = omc / 2; b1 = omc;
        a0 = one + alpha; a1 = -2 * cw; a2 = one - alpha;
        break;
    case FILTER_BQ_HIGH_PASS:
        b0 = b2 = opc / 2; b1 = -opc;
        a0 = one + alpha; a1 = -2 * cw; a2 = one - alpha;
        break;
    case FILTER_BQ_LOW_SHELF:
    case FILTER_BQ_HIGH_SHELF:
    {
        const int64_t ap1 = A + one, am1 = A - one, t = 2 * bq_mul(sqA, alpha);
        const int64_t ap1c = bq_mul(ap1, cw), am1c = bq_mul(am1, cw);
        if (type == FILTER_BQ_LOW_SHELF)
        {
            b0 = bq_mul(A, ap1 - am1c + t);
            b1 = 2 * bq_mul(A, am1 - ap1c);
            b2 = bq_mul(A, ap1 - am1c - t);
            a0 = ap1 + am1c + t;
            a1 = -2 * (am1 + ap1c);
            a2 = ap1 + am1c - t;
        }
        else
        {
            b0 = bq_mul(A, ap1 + am1c + t);
            b1 = -2 * bq_mul(A, am1 + ap1c);
            b2 = bq_mul(A, ap1 + am1c - t);
            a0 = ap1 - am1c + t;
            a1 = 2 * (am1 - ap1c);
            a2 = ap1 - am1c - t;
        }
        break;
    }
    default: /* FILTER_BQ_PEAK */
    {
        const int64_t aA = bq_mul(alpha, A), adA = bq_div(alpha, A);
        b0 = one + aA; b1 = -2 * cw; b2 = one - aA;
        a0 = one + adA; a1 = -2 * cw; a2 = one - adA;
        break;
    }
    }

    const int64_t n0 = bq_div(b0, a0), n1 = bq_div(b1, a0), n2 = bq_div(b2, a0);
    const int64_t n3 = -bq_div(a1, a0), n4 = -bq_div(a2, a0);

    /* A gain past what s1.30 holds comes off the b's here and back onto
       the output (post). Then each coefficient is held to 2^-40, as a
       coarse part in steps of 2^-10 (coefs[], Q28) and the rest, below
       2^-11 (fine[], Q40): a 20 Hz pole or zero at 192 kHz sits 1.3e-4
       from z = 1, where one 32-bit word would move it a part in a
       thousand. The error feedback takes the a's to 2^-30 (a30[]). */
    int64_t n[5] = { n0, n1, n2, n3, n4 };
    int64_t m = 0;
    for (int i = 0; i < 3; i++)
        if ((n[i] < 0 ? -n[i] : n[i]) > m)
            m = n[i] < 0 ? -n[i] : n[i];
    int post = 0;
    while (post < 8 && m >= ((int64_t)2 << (BQ_FRAC + post)) - ((int64_t)1 << (BQ_FRAC - 31 + post)))
        post++;
    for (int i = 0; i < 3; i++)
        n[i] = (n[i] + ((int64_t)1 << post >> 1)) >> post;

    /* Only a filter low against the rate needs the second word: above
       fs / BQ_FINE_RATIO one Q28 word a coefficient keeps every response
       in eq_dsp_test within its tolerance, at half the work */
    ef->fine_on = cutoff < (unsigned long)(UINT32_MAX / BQ_FINE_RATIO);
    for (int i = 0; i < 5; i++)
    {
        int64_t k = (n[i] + ((int64_t)1 << 29)) >> 30;     /* steps of 2^-10 */
        if (!ef->fine_on)
            k = (n[i] + ((int64_t)1 << 11)) >> 12;          /* or all of it, Q28 */
        f->coefs[i] = (int32_t)(ef->fine_on ? k * (1 << 18) : k);   /* |k| <= 2^11: Q28 */
        ef->fine[i] = ef->fine_on ? (int32_t)(n[i] - k * ((int64_t)1 << 30)) : 0;   /* |.| <= 2^29 */
    }
    ef->a30[0] = bq_coef(n[3], 30);
    ef->a30[1] = bq_coef(n[4], 30);
    f->shift = 4;
    ef->post = post;
}

void filter_flush_ef(struct dsp_filter_ef *ef)
{
    memset(ef->err, 0, sizeof (ef->err));
}

/**
 * Direct form 1, as filter_process, for filter_bq_coefs' filters: the
 * output rounded, and the rounding error fed back through the feedback
 * coefficients themselves, so the recursion runs on the output with 28
 * more bits ("double precision" DF1). The error then reaches the output
 * once, white, instead of through the resonance's 1 / A(z); and with no
 * deadband the filter decays to true silence (no limit cycles). Saturates
 * rather than wraps. dsp_mips.S has it for MIPS32r2, bit for bit.
 */
#if !defined(CPU_MIPS) || !defined(__mips_isa_rev) || __mips_isa_rev < 2
void filter_process_ef(struct dsp_filter *f, struct dsp_filter_ef *ef,
                       int32_t * const buf[], int count, unsigned int channels)
{
    /* The sum is kept in units of 2^-28 output LSB. Each of its three
       parts stays below 2^62.2 for any input (|b| < 2 each after post,
       |a1| < 2, |a2| < 1, remainders below 2^29), so it never wraps and
       saturation sees every overload. Every shift is a constant: no
       64-bit shift helpers on a 32-bit CPU. */
    const int32_t c0 = f->coefs[0], c1 = f->coefs[1], c2 = f->coefs[2];
    const int32_t c3 = f->coefs[3], c4 = f->coefs[4];
    const int32_t d0 = ef->fine[0], d1 = ef->fine[1], d2 = ef->fine[2];
    const int32_t d3 = ef->fine[3], d4 = ef->fine[4];
    const int32_t a3 = ef->a30[0], a4 = ef->a30[1];
    const unsigned int post = ef->post;
    const bool fine = ef->fine_on;
    const int32_t hi = INT32_MAX >> post, lo = INT32_MIN >> post;

    for (unsigned int c = 0; c < channels; c++)
    {
        int32_t *p = buf[c];
        int32_t x1 = f->history[c][0], x2 = f->history[c][1];
        int32_t y1 = f->history[c][2], y2 = f->history[c][3];
        int32_t e1 = ef->err[c][0], e2 = ef->err[c][1];

        for (int i = 0; i < count; i++)
        {
            const int32_t x = p[i];
            int64_t v = (int64_t)x * c0 + (int64_t)x1 * c1 + (int64_t)x2 * c2
                      + (int64_t)y1 * c3 + (int64_t)y2 * c4;
            if (fine)
                v += ((int64_t)x * d0 + (int64_t)x1 * d1 + (int64_t)x2 * d2
                      + (int64_t)y1 * d3 + (int64_t)y2 * d4) >> 12;
            v += ((int64_t)e1 * a3 + (int64_t)e2 * a4) >> 30;
            int64_t yq = (v + (1 << 27)) >> 28;
            int32_t y, e;
            if (yq > INT32_MAX)      { y = INT32_MAX; e = 0; }
            else if (yq < INT32_MIN) { y = INT32_MIN; e = 0; }
            else { y = (int32_t)yq; e = (int32_t)(v - yq * (1 << 28)); }
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            e2 = e1; e1 = e;
            p[i] = y > hi ? INT32_MAX : y < lo ? INT32_MIN : (int32_t)((uint32_t)y << post);
        }

        f->history[c][0] = x1; f->history[c][1] = x2;
        f->history[c][2] = y1; f->history[c][3] = y2;
        ef->err[c][0] = e1; ef->err[c][1] = e2;
    }
}
#endif /* CPU */

/**
 * Copy filter definition without destroying dst's history
 */
void filter_copy(struct dsp_filter *dst, const struct dsp_filter *src)
{
    memcpy(dst->coefs, src->coefs, sizeof (src->coefs));
    dst->shift = src->shift;
}

/**
 * Clear filter sample history
 */
void filter_flush(struct dsp_filter *f)
{
    memset(f->history, 0, sizeof (f->history));
}

/**
 * We realise the filters as a second order direct form 1 structure. Direct
 * form 1 was chosen because of better numerical properties for fixed point
 * implementations.
 */
#if (!defined(CPU_COLDFIRE) && !defined(CPU_ARM)) || defined(CPU_ARM_MICRO)
void filter_process(struct dsp_filter *f, int32_t * const buf[], int count,
                    unsigned int channels)
{
    /* Direct form 1 filtering code.
       y[n] = b0*x[i] + b1*x[i - 1] + b2*x[i - 2] + a1*y[i - 1] + a2*y[i - 2],
       where y[] is output and x[] is input.
     */
    unsigned int shift = f->shift;

    for (unsigned int c = 0; c < channels; c++) {
        for (int i = 0; i < count; i++) {
            long long acc = (long long) buf[c][i] * f->coefs[0];
            acc += (long long) f->history[c][0] * f->coefs[1];
            acc += (long long) f->history[c][1] * f->coefs[2];
            acc += (long long) f->history[c][2] * f->coefs[3];
            acc += (long long) f->history[c][3] * f->coefs[4];
            f->history[c][1] = f->history[c][0];
            f->history[c][0] = buf[c][i];
            f->history[c][3] = f->history[c][2];
            buf[c][i] = (acc << shift) >> 32;
            f->history[c][2] = buf[c][i];
        }
    }
}
#endif /* CPU */

/* ring buffer */
int32_t dequeue(int32_t* buffer, int *head, int boundary)
{
    int32_t var = buffer[*head];
    if (*head +1 >= boundary)
        *head = 0;
    else
        *head += 1;
    return var;
}

void enqueue(int32_t var, int32_t* buffer, int *head, int boundary)
{
    buffer[*head] = var;
    if (*head +1 >= boundary)
        *head = 0;
    else
        *head += 1;
}
