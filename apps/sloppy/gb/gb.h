/* The Game Boy core as the Arcade page sees it. GPLv2+.
 * One cartridge at a time; every call from the UI thread. */
#ifndef SLOPPY_GB_H
#define SLOPPY_GB_H
#include <stdbool.h>
#include <stdint.h>

#define GB_W 160
#define GB_H 144
#define GB_FPS_X100 5973                 /* 4194304 / 70224 frames a second, x 100 */

/* the pad, as the cartridge reads it */
enum { GB_RIGHT = 0x01, GB_LEFT = 0x02, GB_UP = 0x04, GB_DOWN = 0x08,
       GB_A = 0x10, GB_B = 0x20, GB_SELECT = 0x40, GB_START = 0x80 };

/* rom: the cartridge; save: its battery save's path less ".sav". 0 when it
 * runs, else -1 and gb_error() says why. */
int gb_open(const char *rom, const char *save);
void gb_close(void);                     /* writes the battery save, frees it all */
bool gb_running(void);
const char *gb_error(void);
bool gb_color(void);                     /* a Game Boy Color cartridge */

/* one frame of the game with these keys held; draw: make its picture (a
 * frame skipped to catch up need not) */
void gb_frame(unsigned keys, bool draw);
const uint16_t *gb_picture(void);        /* GB_W x GB_H, RGB565 */
/* the sound the last frames made, stereo, at gb_sample_hz(); taken, it is gone */
int gb_take_sound(int16_t *out, int max_frames);
int gb_sample_hz(void);

int gb_state_save(const char *path);     /* 0 when written */
int gb_state_load(const char *path);     /* 0 when read */
int gb_battery_save(void);               /* the cartridge's own save, now */
void gb_palette(int k);                  /* an original Game Boy's colours: 0 .. GB_PALETTES - 1 */
#define GB_PALETTES 18                   /* our six, then the Game Boy Color's twelve */
/* a palette's colour as 0xRRGGBB: set 0 the background, 1 the window, 2 and
 * 3 the two sprite sets; shade 0 the lightest .. 3 the darkest */
uint32_t gb_palette_rgb(int k, int set, int shade);

#endif
