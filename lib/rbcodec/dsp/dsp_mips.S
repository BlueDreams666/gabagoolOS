/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
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

#if defined(__mips_isa_rev) && __mips_isa_rev >= 2

/****************************************************************************
 * void filter_process_ef(struct dsp_filter *f, struct dsp_filter_ef *ef,
 *                        int32_t * const buf[], int count,
 *                        unsigned int channels)
 *
 * dsp_filter.c's C version, bit for bit, with every sum in HI/LO and every
 * coefficient and state word in a register. Each sum is taken out of HI/LO
 * once and shifted there: the error feedback's (>> 30, fits 32 bits) goes
 * back in with the rounding as the first product of the fine sum, scaled
 * by 2^12, so the fine sum's >> 12 carries it; that goes back into HI/LO
 * under the coarse sum. Saturation and the output shift's overload branch
 * out of line.
 *
 * struct dsp_filter:    coefs[5] 0, history[c][4] 20 + 16c
 * struct dsp_filter_ef: fine[5] 0, a30[2] 20, err[c][2] 28 + 8c,
 *                       post 44, fine_on 45
 */

/* the loop's registers */
#define P   $4      /* a0: sample */
#define END $5      /* a1: past the last */
#define K27 $6      /* a2: 2^27, the rounding */
#define POST $2     /* v0: ef->post */
#define K1  $3      /* v1: 2^12 for the fine sum, else 1 */
#define X   $8      /* t0..t6: x, x-1, x-2, y-1, y-2, e-1, e-2 */
#define X1  $9
#define X2  $10
#define Y1  $11
#define Y2  $12
#define E1  $13
#define E2  $14
#define T7  $15     /* t7..t9: scratch */
#define T8  $24
#define T9  $25
#define C0  $16     /* s0..s4: coefs[] */
#define C1  $17
#define C2  $18
#define C3  $19
#define C4  $20
#define D0  $21     /* s5..s7, fp, gp: fine[] */
#define D1  $22
#define D2  $23
#define D3  $30
#define D4  $28
#define A3  $31     /* ra, at: a30[] */
#define A4  $1

/* frame: 0 hist, 4 err, 8 buf, 12 count in bytes, 16 channels left,
   20 which loop; 24.. saved */
#define FRAME 72

/* One channel's samples, P to END. FINE: the fine[] sum too; POSTED: the
   output shifted left by post, saturating. */
.macro EFLOOP name, fine, posted
\name:
    lw      X, 0(P)
    mult    E1, A3                  /* error feedback */
    madd    E2, A4
    mfhi    T7
    mflo    T8
    srl     T8, T8, 30
    ins     T8, T7, 2, 30           /* >> 30: fits 32 bits */
    addu    T8, T8, K27             /* and the rounding */
    mult    T8, K1                  /* in at 2^12 under the fine sum, or 1 */
.if \fine
    madd    X, D0
    madd    X1, D1
    madd    X2, D2
    madd    Y1, D3
    madd    Y2, D4
    mfhi    T7
    mflo    T8
    srl     T8, T8, 12
    ins     T8, T7, 20, 12          /* >> 12 */
    sra     T7, T7, 12
    mthi    T7
    mtlo    T8
.endif
    madd    X, C0
    madd    X1, C1
    madd    X2, C2
    madd    Y1, C3
    madd    Y2, C4
    addiu   P, P, 4
    mfhi    T7
    mflo    T8                      /* v + 2^27 */
    addu    T9, T7, K27
    srl     T9, T9, 28
    bnez    T9, \name\()_sat        /* v >> 28 past 32 bits */
    move    Y2, Y1
    srl     Y1, T8, 28
    ins     Y1, T7, 4, 28           /* y */
    move    E2, E1
    ext     E1, T8, 0, 28
    subu    E1, E1, K27             /* e = v - (y << 28) */
.if \posted
    sllv    T9, Y1, POST
    srav    T7, T9, POST
    bne     T7, Y1, \name\()_psat
    sw      T9, -4(P)
.else
    sw      Y1, -4(P)
.endif
\name\()_next:
    move    X2, X1
    bne     P, END, \name
    move    X1, X
    b       .Lchannel_done
    nop
\name\()_sat:
    sra     T7, T7, 31
    lui     T9, 0x7fff
    ori     T9, T9, 0xffff
    xor     Y1, T7, T9              /* INT32_MAX or INT32_MIN */
    move    E2, E1
    move    E1, $0
    b       \name\()_next
    sw      Y1, -4(P)
.if \posted
\name\()_psat:
    sra     T7, Y1, 31
    lui     T9, 0x7fff
    ori     T9, T9, 0xffff
    xor     T9, T7, T9
    b       \name\()_next
    sw      T9, -4(P)
.endif
.endm

    .text
    .set    noreorder
    .set    noat
    .set    nomacro
    .align  2
    .global filter_process_ef
    .type   filter_process_ef, @function
    .ent    filter_process_ef
filter_process_ef:
    lw      $8, 16($sp)             /* channels */
    beqz    $8, 9f
    nop
    blez    $7, 9f                  /* no samples */
    nop
    addiu   $sp, $sp, -FRAME
    sw      $16, 24($sp)
    sw      $17, 28($sp)
    sw      $18, 32($sp)
    sw      $19, 36($sp)
    sw      $20, 40($sp)
    sw      $21, 44($sp)
    sw      $22, 48($sp)
    sw      $23, 52($sp)
    sw      $30, 56($sp)
    sw      $28, 60($sp)
    sw      $31, 64($sp)
    addiu   $9, $4, 20
    sw      $9, 0($sp)              /* &f->history[0] */
    addiu   $9, $5, 28
    sw      $9, 4($sp)              /* &ef->err[0] */
    sw      $6, 8($sp)
    sll     $7, $7, 2
    sw      $7, 12($sp)
    sw      $8, 16($sp)
    lbu     $9, 45($5)              /* fine_on */
    lbu     POST, 44($5)
    sltu    $10, $0, POST
    sll     $9, $9, 1
    or      $9, $9, $10
    sw      $9, 20($sp)             /* 2 fine + 1 posted */

    lw      C0, 0($4)
    lw      C1, 4($4)
    lw      C2, 8($4)
    lw      C3, 12($4)
    lw      C4, 16($4)
    lw      D0, 0($5)
    lw      D1, 4($5)
    lw      D2, 8($5)
    lw      D3, 12($5)
    lw      D4, 16($5)
    lw      A3, 20($5)
    lw      A4, 24($5)
    lui     K27, 0x0800

.Lchannel:
    lw      T7, 0($sp)
    lw      T8, 4($sp)
    lw      T9, 8($sp)
    lw      X1, 0(T7)
    lw      X2, 4(T7)
    lw      Y1, 8(T7)
    lw      Y2, 12(T7)
    lw      E1, 0(T8)
    lw      E2, 4(T8)
    lw      P, 0(T9)
    lw      END, 12($sp)
    lw      T7, 20($sp)
    addu    END, END, P
    andi    T8, T7, 2
    bnez    T8, 1f
    andi    T8, T7, 1
    bnez    T8, .Lcoarse_post
    addiu   K1, $0, 1
    b       .Lcoarse
    nop
1:  bnez    T8, .Lfine_post
    addiu   K1, $0, 1 << 12
    b       .Lfine
    nop

    EFLOOP  .Lcoarse, 0, 0
    EFLOOP  .Lcoarse_post, 0, 1
    EFLOOP  .Lfine, 1, 0
    EFLOOP  .Lfine_post, 1, 1

.Lchannel_done:
    lw      T7, 0($sp)
    lw      T8, 4($sp)
    lw      T9, 8($sp)
    sw      X1, 0(T7)
    sw      X2, 4(T7)
    sw      Y1, 8(T7)
    sw      Y2, 12(T7)
    sw      E1, 0(T8)
    sw      E2, 4(T8)
    addiu   T7, T7, 16
    addiu   T8, T8, 8
    addiu   T9, T9, 4
    sw      T7, 0($sp)
    sw      T8, 4($sp)
    sw      T9, 8($sp)
    lw      T7, 16($sp)
    addiu   T7, T7, -1
    bnez    T7, .Lchannel
    sw      T7, 16($sp)

    lw      $16, 24($sp)
    lw      $17, 28($sp)
    lw      $18, 32($sp)
    lw      $19, 36($sp)
    lw      $20, 40($sp)
    lw      $21, 44($sp)
    lw      $22, 48($sp)
    lw      $23, 52($sp)
    lw      $30, 56($sp)
    lw      $28, 60($sp)
    lw      $31, 64($sp)
    addiu   $sp, $sp, FRAME
9:  jr      $31
    nop
    .end    filter_process_ef
    .size   filter_process_ef, .-filter_process_ef

#endif /* __mips_isa_rev >= 2 */
