/* GabagoolOS - touch-first interface for the HiBy R1, built on Rockbox. GPLv2+. */
#ifndef SLOPPY_H
#define SLOPPY_H
#include <stdbool.h>
/* Runs the GabagoolOS interface. Returns when the user opens the classic
 * Rockbox menus; the caller then continues into root_menu(). */
void sloppy_main(void);
/* early boot: draw the GabagoolOS splash, and record how long each stage takes */
void sloppy_boot_splash(void);
void sloppy_boot_mark(const char *stage);
/* clean_shutdown(): draw the goodbye screen in place of Rockbox's text.
 * why: 0 power off, 1 battery low, 2 battery empty. False when the classic
 * UI owns the screen. */
bool sloppy_shutdown_screen(int why);
/* a line in the breadcrumb log on the card, /.rockbox/sloppy/sloppy.log */
void sl_log(const char *fmt, ...);
#endif
