/* The Game Boy core (gnuboy, by way of Rockbox's rockboy plugin) built into
 * GabagoolOS itself. GPLv2+.
 *
 * The plugin reached Rockbox through rb->; here the core calls the C library
 * and Rockbox's own file layer directly, draws one 160 x 144 picture for the
 * Arcade page to scale, and hands its sound to the page's mixer channel. The
 * page (sloppy_arcade.inc) drives it one frame at a time and owns the
 * glue: gb_glue.c. */
#ifndef __ROCKMACROS_H__
#define __ROCKMACROS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "lcd.h"

#ifndef ICODE_ATTR
#define ICODE_ATTR
#endif
#ifndef IDATA_ATTR
#define IDATA_ATTR
#endif
#ifndef IBSS_ATTR
#define IBSS_ATTR
#endif
#if !defined(ROCKBOX_LITTLE_ENDIAN) && !defined(ROCKBOX_BIG_ENDIAN)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ROCKBOX_LITTLE_ENDIAN 1
#endif
#endif

/* every global of the core, out of the way of Rockbox's and ours */
#include "gb_names.h"

#define malloc(a) my_malloc(a)
void *my_malloc(size_t size);

extern int shut, cleanshut;
void die(char *message, ...);
void vid_begin(void);
void setvidmode(void);
void set_pal(void);
void gb_frame_done(void);            /* the last line of a picture is drawn */

struct options {
    int A, B, START, SELECT, MENU;
    int UP, DOWN, LEFT, RIGHT;
    int frameskip, fps, maxskip;
    int sound, scaling, showstats;
    int autosave;
    int rotate;
    int pal;
    int dirty;
};

extern bool plugbuf;
extern struct options options;

#endif
