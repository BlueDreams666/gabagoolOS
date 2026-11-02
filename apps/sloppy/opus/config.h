/* The Opus decoder's build settings in the GabagoolOS interface: Rockbox's
 * libopus/config.h without the codec interface (plain malloc, no IRAM).
 * Every file of the copy includes this one (sloppy-dev/port-opus.py). */
#ifndef SLOPPY_OPUS_CONFIG_H
#define SLOPPY_OPUS_CONFIG_H

#define OPUS_BUILD

/* temporaries on the stack, as Rockbox builds it, and its few big ones
 * (ALLOCH) from the C library */
#include <stdlib.h>
#define _ogg_malloc malloc
#define _ogg_free free
#define VAR_ARRAYS
#define NORM_ALIASING_HACK

#define HAVE_LRINTF 0
#define HAVE_LRINT  0

/* Rockbox's fixed-point decoder */
#define FIXED_POINT
#define DISABLE_FLOAT_API
#define EMBEDDED_ARM 1

/* Rockbox's memory placement, which the copy does not use */
#define ICONST_ATTR
#define IBSS_ATTR
#define MEM_ALIGN_ATTR

#endif
