/* GabagoolOS screenshots: the screen as it was, made a PNG and written to
 * the card off the interface thread, and PNGs read back for the viewer.
 * Plain POSIX and lodepng (lib/lvgl/src/libs/lodepng), no Rockbox headers:
 * the encoder and the file run on a low-priority pthread of their own. */
#ifndef SLOPPY_PNG_H
#define SLOPPY_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_SHOT_DIR "/Screenshots"           /* at the card's top, where a computer sees it */

enum { SL_SHOT_IDLE, SL_SHOT_BUSY, SL_SHOT_SAVED, SL_SHOT_FULL, SL_SHOT_FAILED };

/* Encode px (w x h RGB565, taken over and freed) and write it as
 * SL_SHOT_DIR/<name>.png, or "<name> 2.png" and on when that is taken.
 * False when one is still being written (px is freed then too). */
bool sl_shot_start(uint16_t *px, int w, int h, const char *name);
/* Where the last one got to. A finished one (SAVED, FULL or FAILED) is
 * reported once, with its path as Rockbox names it ("/Screenshots/..."). */
int  sl_shot_poll(char *path, size_t n);
bool sl_shot_busy(void);
void sl_shot_wait(void);                     /* USB mode: let the write finish first */
/* The last finished PNG, for the phone remote: its number (0 none) and a
 * copy of its bytes in out (its size; 0 when it did not fit or none). */
unsigned sl_shot_seq(void);
size_t sl_shot_png(void *out, size_t cap, unsigned *seq);
size_t sl_shot_png_size(void);
/* A PNG on the card (a Rockbox path) as RGB565 at most mw x mh, its
 * aspect kept (box filtered down, never up). *w, *h get the result's
 * size; the caller frees *px. Blocking: the interface thread, once. */
bool sl_png_load565(const char *path, int mw, int mh, uint16_t **px, int *w, int *h);
bool sl_shot_delete(const char *path);
/* Raw deflate (a zip member) into a new buffer of at most cap bytes (the
 * caller frees *out): 0 all of it, 1 cut at cap or where the input ended
 * or broke, -1 nothing came. Any thread. */
int sl_inflate(const unsigned char *in, size_t n, size_t cap, unsigned char **out, size_t *outn);
/* w x h RGB, 8 bits each, as a PNG in a new buffer (the caller frees *out):
 * its size, 0 when it could not be made. Any thread. */
size_t sl_png_encode_rgb(const unsigned char *rgb, int w, int h, unsigned char **out);

#endif
