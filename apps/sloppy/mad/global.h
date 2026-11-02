/*
 * libmad - MPEG audio decoder library
 * Copyright (C) 2000-2004 Underbit Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id$
 */

/* GabagoolOS's copy of Rockbox's libmad, for internet radio (sloppy_net.c):
 * the same sources, built into the interface with the C library's own
 * malloc and memcpy instead of a codec's (codeclib.h is not used). */
#include <limits.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

# ifndef LIBMAD_GLOBAL_H
# define LIBMAD_GLOBAL_H

#include "mad_iram.h"

/* as Rockbox's system.h has it on this player; sloppy_net.c does not
 * include system.h, and both sides must lay the structures out alike */
#ifndef MEM_ALIGN_ATTR
#define MEM_ALIGN_ATTR __attribute__((aligned(16)))
#endif
#ifndef betoh32                          /* big-endian 32 bits, as system.h's */
# if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define betoh32(x) (x)
# else
#  define betoh32(x) __builtin_bswap32(x)
# endif
#endif

#if defined(__mips__)                  /* the player; the Mac test build gets plain C */
#define FPM_MIPS
#else
#define FPM_64BIT
#endif

/* conditional debugging */

# if defined(DEBUG) && defined(NDEBUG)
#  error "cannot define both DEBUG and NDEBUG"
# endif

# if defined(DEBUG)
#  include <stdio.h>
# endif

/* conditional features */

# if defined(OPT_SPEED) && defined(OPT_ACCURACY)
#  error "cannot optimize for both speed and accuracy"
# endif

# if defined(OPT_SPEED) && !defined(OPT_SSO)
#  define OPT_SSO
# endif

# if defined(HAVE_UNISTD_H) && defined(HAVE_WAITPID) &&  \
    defined(HAVE_FCNTL) && defined(HAVE_PIPE) && defined(HAVE_FORK)
#  define USE_ASYNC
# endif

# if !defined(HAVE_ASSERT_H)
#  if defined(NDEBUG)
#   define assert(x)    /* nothing */
#  else
#   define assert(x)    do { if (!(x)) abort(); } while (0)
#  endif
# endif

# endif
