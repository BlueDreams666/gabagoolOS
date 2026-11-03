/***************************************************************************
 * GabagoolOS - touch-first interface for the HiBy R1, built on Rockbox.
 *
 * Rockbox keeps doing what it is good at (decoding, gapless playback,
 * the tag database, Bluetooth, power). Everything the user sees and touches
 * is drawn by LVGL into the Rockbox framebuffer.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "string-extra.h"
#include <time.h>
#include <math.h>
#include <pthread.h>                    /* sl_log's lock: worker threads log too, in the harness as on the R1 */
#include <unistd.h>
#include <errno.h>                      /* the card's errors (sloppy_card_error) */
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
#include <sys/syscall.h>
#include <sys/mman.h>                   /* sl_give_back */
#include <unistd.h>
#endif
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "button.h"
#include "touchscreen.h"
#include "backlight.h"
#include "powermgmt.h"
#include "power.h"
#include "power.h"
#include "audio.h"
#include "playback.h"
#include "playlist.h"
#include "buffering.h"
#include "settings.h"
#include "sound.h"
#include "eq.h"
#include "crossfeed.h"
#include "tdspeed.h"
#include "usb.h"
#ifdef HAVE_GENERAL_PURPOSE_LED
#include "led-general-purpose.h"
#endif
#include "hiby_bluetooth.h"
#include "misc.h"
#include "tagcache.h"
#include "viewport.h"
#include "timefuncs.h"
#include "metadata.h"
#include "file.h"
#include "dir.h"
#include "rbpaths.h"
#include <stdarg.h>
#include "lvgl.h"
#include "sloppy.h"
#include "sloppy_int.h"
#include "sloppy_net.h"
#include "sloppy_ota.h"
#include "sloppy_share.h"
#include "sloppy_libjob.h"
#include "sloppy_podcast.h"
#include "sloppy_subsonic.h"
#include "sloppy_abs.h"
#include "sloppy_epub.h"
#include "sloppy_png.h"
#if defined(HIBY_R1) && !defined(SIMULATOR)
#include "usb-hiby-gadget.h"            /* usb_card_handover_error() */
#endif

/* ---------------------------------------------------------------- look */
#define W LCD_WIDTH
#define H LCD_HEIGHT
#define SB_H 40                       /* status bar */
/* Zune (theme 3) is True dark underneath: every black-surface branch
 * (sl_theme() == 2) holds for it. sl_zune() adds its own type and shapes. */
static bool sl_zune(void) { return global_settings.sloppy_theme == 3; }
static int sl_theme(void)
{
    if (sl_zune()) return 2;
    return global_settings.sloppy_theme >= 0 && global_settings.sloppy_theme <= 2 ?
           global_settings.sloppy_theme : 0;
}
#define ZN_PINK  0xf2287a                /* the Zune's hot pink */
static uint32_t sl_accent(void)
{
    uint32_t c = (uint32_t)global_settings.sloppy_accent_color;
    if (sl_zune()) return ZN_PINK;
    return c <= 0xffffff ? c : 0x72d8cd;
}
static uint32_t sl_accent_ink(void)
{
    uint32_t c = sl_accent();
    int light = ((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114;
    return light < 145000 ? 0xffffff : 0x182735;
}
/* WCAG relative luminance, x 10000. The panel is small and often seen
 * outdoors, so controls keep 3:1 against their card and text 4.5:1. */
static int sl_lum(uint32_t c)
{
    double v = 0;
    static const double w[3] = { 0.2126, 0.7152, 0.0722 };
    for (int i = 0; i < 3; i++)
    {
        double x = ((c >> (16 - 8 * i)) & 255) / 255.0;
        x = x <= 0.03928 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
        v += w[i] * x;
    }
    return (int)(v * 10000 + 0.5);
}
static int sl_contrast_x10(uint32_t a, uint32_t b)
{
    int la = sl_lum(a), lb = sl_lum(b);
    if (la < lb) { int t = la; la = lb; lb = t; }
    return (la + 500) * 10 / (lb + 500);
}
/* HSV value v (0..1) and saturation sat applied to hue h (0..6) */
static uint32_t sl_hsv(double h, double sat, double v)
{
    int i = (int)h % 6;
    double f = h - (int)h, p = v * (1 - sat), q = v * (1 - sat * f), t = v * (1 - sat * (1 - f));
    double r = i == 0 ? v : i == 1 ? q : i == 2 ? p : i == 3 ? p : i == 4 ? t : v;
    double g = i == 0 ? t : i == 1 ? v : i == 2 ? v : i == 3 ? q : i == 4 ? p : p;
    double b = i == 0 ? p : i == 1 ? p : i == 2 ? t : i == 3 ? v : i == 4 ? v : q;
    return (uint32_t)(r * 255 + 0.5) << 16 | (uint32_t)(g * 255 + 0.5) << 8 | (uint32_t)(b * 255 + 0.5);
}
/* The accent where it marks a control on a surface: the same hue, lifted
 * toward white (or deepened toward black on a light surface) just until
 * it reaches the contrast asked for. A deep red accent on the dark theme
 * left switches that were on looking off. */
static uint32_t sl_accent_on(uint32_t surface, int min_x10)
{
    static struct { uint32_t in, surface, out; int min; } memo[4];
    static int next;
    uint32_t c = sl_accent();
    for (int i = 0; i < 4; i++)
        if (memo[i].min && memo[i].in == c && memo[i].surface == surface && memo[i].min == min_x10)
            return memo[i].out;
    /* brighter on a dark surface, deeper on a light one; once full
     * brightness is reached, saturation gives way */
    double r = (c >> 16 & 255) / 255.0, g = (c >> 8 & 255) / 255.0, b = (c & 255) / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double d = mx - mn, h = 0, sat = mx > 0 ? d / mx : 0;
    if (d > 0)
    {
        h = mx == r ? (g - b) / d : mx == g ? 2 + (b - r) / d : 4 + (r - g) / d;
        if (h < 0) h += 6;
    }
    bool dark = sl_lum(surface) < 1800;
    uint32_t out = c;
    for (int step = 1; step <= 40 && sl_contrast_x10(out, surface) < min_x10; step++)
    {
        double v = dark ? mx + step * 0.025 : mx - step * 0.025, s2 = sat;
        if (v > 1) { s2 = sat - (v - 1); v = 1; }
        if (v < 0) v = 0;
        if (s2 < 0) s2 = 0;
        out = sl_hsv(h, s2, v);
    }
    memo[next].in = c; memo[next].surface = surface; memo[next].min = min_x10; memo[next].out = out;
    next = (next + 1) & 3;
    return out;
}
/* text on a filled control: whichever of white and ink reads better */
static uint32_t sl_ink_on(uint32_t c)
{
    return sl_contrast_x10(0xffffff, c) >= sl_contrast_x10(0x182735, c) ? 0xffffff : 0x182735;
}
/* Dark: a deep blue-black with surfaces one step up, their hairline edges
 * doing the separating, and neutral cool greys for secondary text. */
#define C_BG      (sl_theme() == 1 ? 0xf1f4f7 : sl_theme() == 2 ? 0x000000 : 0x0d1219)
#define C_CARD    (sl_theme() == 1 ? 0xffffff : sl_theme() == 2 ? 0x000000 : 0x18202b)
#define C_CARD_P  (sl_theme() == 1 ? 0xdfe6ed : sl_theme() == 2 ? 0x111111 : 0x242f3d)
#define C_ACCENT  sl_accent()
#define C_TEXT    (sl_theme() == 1 ? 0x182735 : 0xffffff)
#define C_SUB     (sl_theme() == 1 ? 0x43576a : sl_theme() == 2 ? 0xb8b8b8 : 0xb3bfcb)
#define C_DIM     (sl_theme() == 1 ? 0x5a7082 : sl_theme() == 2 ? 0x888888 : 0x7d8a98)
/* the accent on a card: filled controls, and text */
#define C_ACCENT_UI  sl_accent_on(C_CARD, 30)
#define C_ACCENT_TXT sl_accent_on(C_CARD, 45)
/* a sheet over everything (actions, sort, playlists): white in light, a
 * step above the cards in dark, black in Zule; its words are C_TEXT */
#define C_SHEET   (sl_zune() ? 0x0c0c0c : sl_theme() == 1 ? 0xffffff : 0x1d2d3e)

static void sl_wallpaper_draw(lv_event_t *e);

LV_FONT_DECLARE(ss_regular_16)
LV_FONT_DECLARE(ss_regular_18)
LV_FONT_DECLARE(ss_regular_34)
LV_FONT_DECLARE(ss_regular_60)
LV_FONT_DECLARE(ss_semibold_18)
LV_FONT_DECLARE(ss_semibold_21)
LV_FONT_DECLARE(ss_semibold_23)
LV_FONT_DECLARE(ss_semibold_26)
LV_FONT_DECLARE(ss_semibold_30)
LV_FONT_DECLARE(ss_bold_28)
LV_FONT_DECLARE(ss_bold_56)
LV_FONT_DECLARE(sl_brand_50)
LV_FONT_DECLARE(sl_brand_34)
LV_FONT_DECLARE(ss_semibold_104)
LV_FONT_DECLARE(ss_regular_104)
LV_FONT_DECLARE(ss_regular_21)
LV_FONT_DECLARE(ss_regular_23)

/* Settings > Customization > Text size: four steps, each moving body and
 * list text up a size; titles, clocks and game art keep theirs. The names
 * below stand for whichever size is in use, so pages pick it up when they
 * are built. */
LV_FONT_DECLARE(ss_regular_26)
LV_FONT_DECLARE(ss_semibold_34)
LV_FONT_DECLARE(ss_semibold_38)
#define SL_TEXT_STEPS 4
static int sl_text_step;
static const lv_font_t *const sl_fonts[SL_TEXT_STEPS][6] = {
    { &ss_regular_16, &ss_regular_18, &ss_semibold_18, &ss_semibold_21, &ss_semibold_23, &ss_semibold_26 },
    { &ss_regular_18, &ss_regular_21, &ss_semibold_21, &ss_semibold_23, &ss_semibold_26, &ss_semibold_30 },
    { &ss_regular_21, &ss_regular_23, &ss_semibold_23, &ss_semibold_26, &ss_semibold_30, &ss_semibold_34 },
    { &ss_regular_23, &ss_regular_26, &ss_semibold_26, &ss_semibold_30, &ss_semibold_34, &ss_semibold_38 },
};
#define SL_FONT(i) (*sl_fonts[sl_text_step][i])
static const char *const text_size_names[SL_TEXT_STEPS] = { "Standard", "Large", "Larger", "Largest" };
#define ss_regular_16  SL_FONT(0)
#define ss_regular_18  SL_FONT(1)
#define ss_semibold_18 SL_FONT(2)
#define ss_semibold_21 SL_FONT(3)
#define ss_semibold_23 SL_FONT(4)
#define ss_semibold_26 SL_FONT(5)

/* Settings > Language: interface text goes through sl_tr() (text(), toast()
 * and the label helpers). Anything not in the table stays as written. */
static const char *sl_tr(const char *s);
#include "sloppy_lang.inc"

/* ---------------------------------------------------------------- time */
static uint32_t sl_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static lv_obj_t *wallpaper_lock_obj;
static bool wallpaper_screen_dark;
#include "sloppy_sfx.inc"
static void radio_flush(void);
static void hist_flush(void);
static void clicker_flush(void);
static void rd_flush(void);          /* sloppy_reader.inc: where a book was left */
static void rd_open(const char *path);
static bool search_hides_mini(void);
static bool sl_reading_lyrics(void);       /* sloppy_lyrics.inc */
static void sl_screenshot(void);
static void shot_card_hide(void);   /* sloppy_shot.inc: a toast takes its place */
#include "sloppy_power.inc"
/* our symbols (sloppy_icons.inc): every font's chain ends at one */
extern const lv_font_t sl_sym_14, sl_sym_16, sl_sym_18, sl_sym_20, sl_sym_22, sl_sym_24, sl_sym_28, sl_sym_32, sl_sym_40, sl_sym_48;
#include "sloppy_xfont.inc"

/* --------------------------------------------------------------- log */
/* Breadcrumbs for field debugging. Each line is synced to the card, so the
 * last lines before a crash survive the reboot. The previous session's log
 * is kept as sloppy.prev.log. */
static int log_fd = -1;
static bool log_started;            /* the previous log was rotated out this boot */
#define DB_USB_NOTE "/tmp/sloppy-usb-was-on"   /* USB mode was on: update the library (sloppy_dbupdate.inc) */
static volatile bool log_held;      /* USB mode: the card is being handed to the computer */
static unsigned log_dropped;        /* lines that came while it was held, past log_away */
/* ...the first 8 KB of them, written once the card is back */
static char log_away[8192];
static unsigned log_away_n;         /* bytes in it */
static bool log_sync_thread;        /* the watchdog thread syncs the log, not the writer */
static volatile bool log_unsynced;  /* lines written since its last sync */
/* sl_log's file, its held lines and its counts, one thread at a time:
 * USB mode's hold must not land between a worker's look at log_held and
 * its open (the log reopened on the card as it is handed over), nor a
 * release between a line's room taken and its copy. The lock, or after
 * 200 ms without it (a writer stuck on the card) the line goes anyway:
 * the watchdog's own report must get out. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static bool log_lock_take(void)
{
    for (int i = 0; i < 200; i++)
    {
        if (!pthread_mutex_trylock(&log_lock)) return true;
        usleep(1000);
    }
    return false;
}
/* a line as the log has it: the time, the words, a newline */
static int log_line(char *b, size_t cap, const char *fmt, va_list ap)
{
    int n = snprintf(b, cap, "[%7ld.%02ld] ", current_tick / HZ, current_tick % HZ);
    n += vsnprintf(b + n, cap - n - 1, fmt, ap);
    if (n > (int)cap - 2) n = cap - 2;
    b[n++] = '\n';
    return n;
}
static int log_linef(char *b, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = log_line(b, cap, fmt, ap);
    va_end(ap);
    return n;
}
/* the line onto the card, or into log_away while it is held; log_lock held */
static void log_put(const char *b, int n)
{
    if (log_held)
    {
        if (log_away_n + (unsigned)n <= sizeof log_away) { memcpy(log_away + log_away_n, b, (size_t)n); log_away_n += (unsigned)n; }
        else log_dropped++;
        return;
    }
    if (log_fd < 0)
    {
        /* opened again after USB mode: carry on below the lines before it.
         * Until the first open succeeds (the card may still be coming up
         * when the player restarts) every try rotates again. */
        int how = log_started ? O_APPEND : O_TRUNC;
        if (!log_started)
        {
            mkdir(ROCKBOX_DIR "/sloppy");
            remove(ROCKBOX_DIR "/sloppy/sloppy.prev.log");
            rename(ROCKBOX_DIR "/sloppy/sloppy.log", ROCKBOX_DIR "/sloppy/sloppy.prev.log");
        }
        log_fd = open(ROCKBOX_DIR "/sloppy/sloppy.log", O_WRONLY | O_CREAT | how, 0666);
        if (log_fd < 0) return;
        log_started = true;
    }
    /* A crash keeps written lines (they are in the kernel's cache); only a
     * power cut loses them. Syncing every line stalled the UI on the card,
     * so sync at most every 2 s - except for the first two minutes, when
     * there are few lines and a hang that needs a forced reboot (as good
     * as a power cut) would otherwise take its last steps with it. On the
     * R1 the watchdog thread does the syncing (sl_log_sync): an fsync there
     * took 250 ms a line while the card was busy at boot (2026-10-23: the
     * home screen came 5.4 s after the splash instead of 1.3 s, and USB
     * mode came that much later). */
    static uint32_t synced_at;
    if (write(log_fd, b, n) == n)
    {
        if (log_sync_thread)
            log_unsynced = true;
        else if (current_tick < 120 * HZ || sl_ms() - synced_at >= 2000)
        {
            fsync(log_fd);
            synced_at = sl_ms();
        }
    }
}
void sl_log(const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    int n = log_line(b, sizeof b, fmt, ap);
    va_end(ap);
    bool locked = log_lock_take();
    log_put(b, n);
    if (locked) pthread_mutex_unlock(&log_lock);
}

static void sl_log_flush(void)
{
    if (log_fd >= 0) fsync(log_fd);
}

/* USB mode unmounts the card for the computer, and an open file on it
 * keeps it mounted (umount: EBUSY) - the player would then write the same
 * exFAT the computer does. So the log is closed for the handover, lines
 * that come meanwhile are counted, and it is opened again (appending) once
 * the card is back. */
static void sl_log_hold(bool on)
{
    bool locked = log_lock_take();
    if (on)
    {
        log_held = true;                /* from here a line from any thread waits in log_away */
        if (log_fd >= 0) { fsync(log_fd); close(log_fd); log_fd = -1; }
    }
    else
    {
        log_held = false;
        char b[200];
        if (log_away_n)
        {
            log_put(b, log_linef(b, sizeof b, "log: %u bytes written while the card was the computer's:", log_away_n));
            if (log_fd >= 0 && write(log_fd, log_away, log_away_n) == (ssize_t)log_away_n) log_unsynced = true;
        }
        if (log_dropped)
            log_put(b, log_linef(b, sizeof b, "log: %u line%s dropped while the card was the computer's", log_dropped, log_dropped == 1 ? "" : "s"));
        log_away_n = 0;
        log_dropped = 0;
    }
    if (locked) pthread_mutex_unlock(&log_lock);
}

void sl_lv_assert(const char *file, int line)
{
    const char *base = strrchr(file, '/');
    sl_log("lvgl assert: %s:%d", base ? base + 1 : file, line);
    sl_log_flush();
    for (;;) sleep(HZ);                 /* halt as before; the watchdog keeps reporting */
}

/* Sample UI work only while a panel or Cover Flow is moving. Logging each
 * frame would itself stall the card, so write one summary on exit. */
static struct {
    const char *name;
    uint32_t calls, over_16ms, gaps_over_25ms;
    uint32_t worst_us, worst_gap_us;
    uint64_t total_us, previous_us;
} sl_perf;

static uint64_t sl_perf_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}

static void sl_perf_begin(const char *name)
{
    memset(&sl_perf, 0, sizeof sl_perf);
    sl_perf.name = name;
}

static void sl_perf_sample(uint64_t started, uint32_t elapsed_us)
{
    if (!sl_perf.name) return;
    if (sl_perf.previous_us && started - sl_perf.previous_us < 200000u)
    {
        uint32_t gap = (uint32_t)(started - sl_perf.previous_us);
        if (gap > 25000u) sl_perf.gaps_over_25ms++;
        if (gap > sl_perf.worst_gap_us) sl_perf.worst_gap_us = gap;
    }
    sl_perf.previous_us = started;
    sl_perf.calls++;
    sl_perf.total_us += elapsed_us;
    if (elapsed_us > 16667u) sl_perf.over_16ms++;
    if (elapsed_us > sl_perf.worst_us) sl_perf.worst_us = elapsed_us;
}

static void sl_perf_end(void)
{
    if (!sl_perf.name) return;
    sl_log("perf %s: %lu handlers, %lu us avg, %lu us worst, %lu >16ms; "
           "%lu gaps >25ms, %lu us worst gap", sl_perf.name,
           (unsigned long)sl_perf.calls,
           (unsigned long)(sl_perf.calls ? sl_perf.total_us / sl_perf.calls : 0),
           (unsigned long)sl_perf.worst_us, (unsigned long)sl_perf.over_16ms,
           (unsigned long)sl_perf.gaps_over_25ms, (unsigned long)sl_perf.worst_gap_us);
    sl_perf.name = NULL;
}

/* RSS includes Rockbox and decoder memory, while the component counts below
 * show which UI allocations are responsible for a change. */
static unsigned long sl_perf_rss_kb(unsigned long *peak_kb)
{
    *peak_kb = 0;
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return 0;
    unsigned long kb = 0;
    char line[128];
    while (fgets(line, sizeof line, fp))
    {
        sscanf(line, "VmRSS: %lu", &kb);
        sscanf(line, "VmHWM: %lu", peak_kb);
    }
    fclose(fp);
    return kb;
}

/* An app's big static buffer, handed back to the system when its page
 * closes: once drawn its pages stayed the player's for good, and Snake,
 * Spoon and the coin each kept hundreds of KB after closing until the R1
 * ran out of memory and froze (a device log, 2 Nov). The whole pages inside
 * it read as 0 afterwards, so the app redraws it on its next open; the Mac
 * clears them too, so the harness sees what the R1 does. */
static void sl_give_back(void *p, size_t n)
{
    uintptr_t a = ((uintptr_t)p + 4095) & ~(uintptr_t)4095, e = ((uintptr_t)p + n) & ~(uintptr_t)4095;
    if (e <= a) return;
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    madvise((void *)a, e - a, MADV_DONTNEED);
#else
    memset((void *)a, 0, e - a);
#endif
}

static void sl_perf_memory(const char *where);
#include "sloppy_cfmeter.inc"

/* ------------------------------------------------------------- display */
static lv_display_t *disp;
static lv_indev_t *touch;
static int dirty_x1, dirty_y1, dirty_x2, dirty_y2;

/* What is on the panel, for the control centre's frosted glass (it blurs
 * whatever the screen showed when it was pulled down). lcd_bitmap already
 * copies every flushed strip into Rockbox's framebuffer, so read it there
 * rather than keeping a second 768 KB copy updated on every frame. */
#ifndef SL_SCREEN_PIXELS
#define SL_SCREEN_PIXELS() (lcd_set_viewport(NULL), (const uint16_t *)FBADDR(0, 0))
#define SL_SCREEN_STRIDE LCD_FBWIDTH
#endif

/* Cover Flow turns the display on its side. LVGL then renders landscape
 * strips; turn each one back to the panel's portrait orientation. */
static uint8_t *sl_rot_buf;
static size_t sl_rot_size;
static const uint8_t *sl_flush_portrait(lv_display_t *d, lv_area_t *a, const uint8_t *px)
{
    lv_display_rotation_t r = lv_display_get_rotation(d);
    if (r == LV_DISPLAY_ROTATION_0) return px;
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    size_t need = (size_t)w * h * 2;
    if (need > sl_rot_size)
    {
        free(sl_rot_buf);
        sl_rot_buf = malloc(need);
        sl_rot_size = sl_rot_buf ? need : 0;
        if (!sl_rot_buf) return px;
    }
    lv_draw_sw_rotate(px, sl_rot_buf, w, h, lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565),
                      lv_draw_buf_width_to_stride(r == LV_DISPLAY_ROTATION_180 ? w : h, LV_COLOR_FORMAT_RGB565),
                      r, LV_COLOR_FORMAT_RGB565);
    lv_display_rotate_area(d, a);
    return sl_rot_buf;
}

/* pixels put on the screen so far: how much a still copy of it (the
 * Control Center's glass, taken at touch-down) has fallen behind */
static unsigned long sl_flushed_px;
/* Drawing straight into Rockbox's framebuffer: LVGL's strips used to be
 * copied into it (lcd_bitmap, 3.6 ms a full screen on the R1) before the
 * driver copied it on to the panel. Upright screens draw in place; a
 * turned one (Cover Flow, Spectrum) needs the strips to rotate, so it
 * goes back to them. An empty /.rockbox/sloppy/partial-flush keeps the
 * strips everywhere, should the panel ever disagree. */
static bool sl_direct, sl_direct_ok;
static bool mini_core_ok;   /* the mini player's middle on screen is its own (mini_cut_cb) */
static void sl_display_buffers(bool upright)
{
    static uint8_t draw_bufs[2][W * 100 * 2] __attribute__((aligned(64)));
    static bool set;
    bool direct = upright && sl_direct_ok;
    if (set && direct == sl_direct) return;
    set = true;
    if (direct)
        lv_display_set_buffers(disp, (void *)SL_SCREEN_PIXELS(), NULL, (uint32_t)W * H * 2,
                               LV_DISPLAY_RENDER_MODE_DIRECT);
    else
        lv_display_set_buffers(disp, draw_bufs[0], draw_bufs[1], sizeof draw_bufs[0],
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
    sl_direct = direct;
    mini_core_ok = false;
    lv_obj_invalidate(lv_screen_active());
}
static void sl_set_rotation(lv_display_rotation_t r)
{
    /* The buffers take their row length from the display as it is turned
     * then: upright again, turn first, or the screen buffer keeps 800 px
     * rows, the frame no longer fits and LVGL asserts (a freeze on Done). */
    mini_core_ok = false;
    if (r != LV_DISPLAY_ROTATION_0) sl_display_buffers(false);
    lv_display_set_rotation(disp, r);
    if (r == LV_DISPLAY_ROTATION_0) sl_display_buffers(true);
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *map)
{
    lv_area_t rotated = *area;
    const lv_area_t *a = &rotated;
    uint64_t m0 = cfm.on ? sl_perf_now_us() : 0;
    const uint8_t *px = sl_flush_portrait(d, &rotated, map);
    uint64_t m1 = cfm.on ? sl_perf_now_us() : 0;
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    sl_flushed_px += (unsigned long)w * h;
#ifdef SIMULATOR
    /* SLOPPY_FLUSH_STATS=1: log how much of the panel is redrawn, to find
     * what keeps an idle screen busy */
    static int stats = -1;
    static long stat_px, stat_n;
    static uint32_t since;
    if (stats < 0) stats = getenv("SLOPPY_FLUSH_STATS") != NULL;
    if (stats)
    {
        stat_px += (long)w * h; stat_n++;
        if (sl_ms() - since >= 1000)
        {
            sl_log("flush: %ld px in %ld strips, last %d,%d %dx%d", stat_px, stat_n, a->x1, a->y1, w, h);
            stat_px = stat_n = 0; since = sl_ms();
        }
    }
#endif
    /* a splash from a background thread can leave another viewport current */
    lcd_set_viewport(NULL);
    if (!sl_direct) lcd_bitmap((const fb_data *)px, a->x1, a->y1, w, h);   /* drawn in place otherwise */
    uint64_t m2 = cfm.on ? sl_perf_now_us() : 0;
    if (a->x1 < dirty_x1) dirty_x1 = a->x1;
    if (a->y1 < dirty_y1) dirty_y1 = a->y1;
    if (a->x2 > dirty_x2) dirty_x2 = a->x2;
    if (a->y2 > dirty_y2) dirty_y2 = a->y2;
    if (lv_display_flush_is_last(d))
    {
        lcd_update_rect(dirty_x1, dirty_y1, dirty_x2 - dirty_x1 + 1, dirty_y2 - dirty_y1 + 1);
        dirty_x1 = dirty_y1 = 0x7fff;
        dirty_x2 = dirty_y2 = -1;
    }
    if (cfm.on)
    {
        cfm.refr_rot += (uint32_t)(m1 - m0);
        cfm.refr_blit += (uint32_t)(m2 - m1);
        cfm.refr_present += (uint32_t)(sl_perf_now_us() - m2);
    }
    lv_display_flush_ready(d);
}

/* An ARGB8888 picture (a snapshot) as RGB565 plus an alpha plane: on this
 * RGB565 screen LVGL blends and turns that format with one multiply a pixel
 * where ARGB8888 takes three. NULL without memory. */
static lv_draw_buf_t *sl_rgb565a8_from(const lv_draw_buf_t *argb)
{
    int w = argb->header.w, h = argb->header.h;
    lv_draw_buf_t *out = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565A8, 0);
    if (!out) return NULL;
    uint8_t *a = out->data + out->header.stride * h;   /* the alpha plane follows the colours */
    for (int y = 0; y < h; y++)
    {
        const lv_color32_t *p = (const lv_color32_t *)(argb->data + y * argb->header.stride);
        uint16_t *c = (uint16_t *)(out->data + y * out->header.stride);
        for (int x = 0; x < w; x++)
        {
            c[x] = lv_color_to_u16(lv_color_make(p[x].red, p[x].green, p[x].blue));
            a[y * w + x] = p[x].alpha;
        }
    }
    return out;
}

static void periodic_ui(void);
static void sl_card_error_note(void);
static void sl_power_redraw(lv_timer_t *t)
{
    (void)t;
    if (wallpaper_screen_dark) return;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
}
#ifdef HAVE_HIBY_BLUETOOTH
static void sl_bt_on_wake(void);
#endif
static void sl_power_finish_wake(void)
{
    if (!sl_power_wake_pending) return;
    periodic_ui();                      /* labels froze while the panel was dark */
    /* The panel can lose its framebuffer while the light is off.
     * Draw the lock screen before exposing it to the user. The panel must
     * be enabled first: while it is off lcd_update_rect() drops the frame,
     * and the backlight thread would later unblank an empty screen. */
#ifdef HAVE_LCD_ENABLE
    lcd_enable(true);
#endif
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(disp);
    sl_power_wake_pending = false;
    backlight_wake();
#ifdef HAVE_HIBY_BLUETOOTH
    sl_bt_on_wake();                    /* headphones switched on meanwhile */
#endif
    /* The backlight thread powers the R1's panel up a moment later, and
     * the frame drawn before that can be lost: the lock screen stayed black
     * until a swipe redrew it. Draw it all again once the light is on. */
    lv_timer_t *t = lv_timer_create(sl_power_redraw, 90, NULL);
    lv_timer_set_repeat_count(t, 3);
}

/* ------------------------------------------------ system gestures state */
/* A contact is first offered to the system: a pull from the top edge opens
 * the quick panel, a rightward drag on a pushed page goes back. Once the
 * system claims a contact LVGL never sees the rest of it, so a swipe can
 * never also trigger the row it started on. */
enum { G_IDLE, G_UNDECIDED, G_LVGL, G_BACK, G_PULL, G_MINI, G_SHEET, G_GAME, G_PANEL, G_GROW, G_CCPAGE };
static int g_state = G_IDLE;
static int g_x0, g_y0, g_lx, g_ly;
static uint32_t g_t0, g_tl;
static int g_vy, g_vx;              /* recent vertical and sideways speed, px/s */
/* A tap that woke the paused touch reader but lifted before it was read:
 * 1 = press it at tap_x, tap_y on the next read, 2 = then let go. */
static int tap_replay, tap_x, tap_y;
static bool g_on_sheet;             /* may pull the Up Next sheet down */
static bool g_on_acts;              /* or a song's sheet (acts) */
static bool g_on_game;              /* a game steered by swipes (Snake) */
static bool nav_contact_blocked;
static bool g_on_mini;
static bool g_no_back;              /* contact started on a slider etc */
static bool g_mid_back;             /* may go back from mid screen, as newer iOS: nothing under it swipes sideways */
static bool g_dragged;              /* a scroll must never also click a tile */
static bool quiz_active;             /* hide track titles while a music question is visible */

static void back_drag(int dx);
static void back_release(int dx, int vx);
static bool cf_is_open(void);
static int  page_depth(void);
static bool overlay_visible(void);
static lv_obj_t *ui_screen(void);
static bool kb_on_top(void);
static bool mini_held_off(void);
static void mini_update(void);
static bool qp_is_visible(void);
static void qp_drag_to(int dy);
static void qp_glass_early(void);
static void qp_release(int dy, int vy);
static bool qp_edge_grab(int y);
static void qp_edge_drag(int dy);
static void qp_edge_release(int dy, int vy);
static void qp_pull(int dy);
static void qp_pull_release(int dy, int vy, int vy_now);
static bool qx_grab(int x, int y, int dx, int dy);
static void qx_drag(int dy);
static void qx_release(int dy, int vy);
static bool qx_page_grab(int x, int y, int dx, int dy);
static void qx_page_drag(int dx);
static void qx_page_release(int dx, int vx);
static void uq_show(void);
static bool something_playing(void);
static void kebab_cb(lv_event_t *e);
static void acts_open_np(int t);
static void sl_prev(void);
static void sl_next(void);
static void sl_prev_song(void);
static void row_long_cb(lv_event_t *e);
static bool sel_on(void);
static bool sel_picking(lv_obj_t *host);
static void sel_mark(lv_obj_t *row, int track);
static void sel_tap_cb(lv_event_t *e);
static void sel_forget(void *view);
/* albums and artists: their sheets, picking them, the play menu (sloppy_playback.inc) */
enum { SEL_SONGS, SEL_ALBUMS, SEL_ARTISTS, SEL_FILES, SEL_PLAYLISTS, SEL_EPISODES };
static bool sel_kind_on(int kind);
static bool sel_has(int t);
static void sel_cell_cb(lv_event_t *e);
static void sel_cell_mark(lv_obj_t *art, int a);
static void sel_artist_cb(lv_event_t *e);
static void sel_artist_mark(lv_obj_t *row, int k0);
static void album_long_cb(lv_event_t *e);
static void artist_long_cb(lv_event_t *e);
static void play_menu_button(lv_obj_t *page, int kind);
static bool st_on(void);
static void eq_page(void);
static void play_settings_page(void);
/* Settings home, its vocabulary and its pages: sloppy_settings.inc */
static lv_obj_t *set_group(lv_obj_t *body, const char *label);
static lv_obj_t *set_row(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2, intptr_t ud);
static lv_obj_t *set_switch(lv_obj_t *g, const char *label, const char *note, bool on, void (*apply)(int value));
static lv_obj_t *set_switch_ex(lv_obj_t *g, const char *sym, uint32_t tint, const char *label, const char *note,
                               bool on, void (*apply)(int value));
static lv_obj_t *set_action(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                            lv_event_cb_t cb, void *ud);
static lv_obj_t *set_action_sub(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                                lv_event_cb_t cb, void *ud);
static lv_obj_t *set_info(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2, bool sub);
static lv_obj_t *set_link(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *t2,
                          lv_event_cb_t cb, void *ud);
static lv_obj_t *set_link_sub(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, const char *line,
                              const char *value, lv_event_cb_t cb, void *ud);
static void set_link_text(lv_obj_t *r, const char *line, const char *value);
static lv_obj_t *set_check(lv_obj_t *g, const char *sym, uint32_t tint, const char *t1, bool on,
                           lv_event_cb_t cb, void *ud);
static void set_check_mark(lv_obj_t *chosen);
static lv_obj_t *set_danger(lv_obj_t *g, const char *sym, const char *t1, lv_event_cb_t cb, void *ud);
static lv_obj_t *set_row_button(lv_obj_t *r, int chevron, const char *sym, lv_event_cb_t cb, void *ud);
static lv_obj_t *set_slider_row(lv_obj_t *c, const char *label, const char *value, uint32_t value_color);
static lv_obj_t *set_text(lv_obj_t *g, const char *s, uint32_t color);
static void opt_note(lv_obj_t *body, const char *s);
static void opt_note_centred(lv_obj_t *body, const char *s);
static lv_obj_t *set_header(lv_obj_t *body, const char *label);
static void set_join_next(lv_obj_t *g);
static void set_sep(lv_obj_t *g, lv_obj_t *r, int x);
static bool set_rtl(void);
static lv_obj_t *set_field(lv_obj_t *g);
#define SET_PIC_W 72                         /* set_pick's picture */
#define SET_PIC_H 104
static lv_obj_t *set_pick(lv_obj_t *g, const char *t1, const char *t2, bool on, lv_event_cb_t cb, void *ud);
static lv_obj_t *set_pic_screen(lv_obj_t *pic, int w, int h, uint32_t bg);
static void set_confirm(lv_obj_t *body, const char *header, const char *what, const char *keep, const char *keep_sub,
                        lv_event_cb_t keep_cb, const char *go, const char *go_sub, lv_event_cb_t go_cb, bool danger);
static void qp_refresh(void);
static void label_if_changed(lv_obj_t *o, const char *value);
static lv_obj_t *icon_btn(lv_obj_t *, const char *, int, lv_event_cb_t, void *);
static bool sl_rtl(const char *s);
static void hairline(lv_obj_t *o);
static int ts_rating(int t);
static void ts_love(int t, bool love);
static void hidden_if_changed(lv_obj_t *o, bool hidden);
static uint32_t np_icon_color(void);
static void sw_attach(lv_obj_t *row, int track);
static bool sw_busy(void);
static bool pl_visible(void);
static uint32_t last_touch_ms;
static lv_obj_t *mini;
static void mini_swipe_drag(int dx);
static void mini_swipe_release(int dx, int vx);
static bool uq_sheet_grab(lv_obj_t *hit);
static bool acts_sheet_grab(int x, int y);
static bool snake_takes_swipe(int y);
static bool arcade_key(int b, bool rel);   /* sloppy_arcade.inc */
static bool arc_on_top(void);
static bool t48_on_top(void);             /* the new games: sloppy_2048.inc and on */
static bool t48_key(int b, bool rel);
static bool fm_key(int b, bool rel, bool repeat);   /* sloppy_files.inc */
static bool rd_key(int b, bool rel, bool repeat);   /* sloppy_reader.inc */
static bool sdk_on_top(void);
static bool pt_on_top(void);
static bool fl_on_top(void);
static bool fl_key(int btn);
static void snake_swipe_begin(int x, int y);
static void snake_swipe_move(int x, int y);
static void uq_sheet_drag(int dy);
static void uq_sheet_release(int dy, int vy);
static void acts_sheet_drag(int dy);
static void acts_sheet_release(int dy, int vy);

static void nav_guard_contact(void)
{
    int pos;
    if (button_status_wdata(&pos) & BUTTON_TOUCHSCREEN)
        nav_contact_blocked = true;
}

static bool hud_hit(int x, int y);
/* Upside down (Settings > Buttons, P65): the panel draws the picture
 * turned round (lcd_set_flip, in the copy the framebuffer driver already
 * makes), the touches are turned back to match, and the volume keys swap
 * so the upper one is still louder. Everything that reads the screen's
 * pixels (screenshots, the control centre's glass) sees it upright. */
static bool sl_flipped;
static void sl_flip_apply(bool on)
{
    sl_flipped = on;
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    lcd_set_flip(on);
#endif
    if (lv_screen_active()) lv_obj_invalidate(lv_screen_active());
}
static long sl_flip_key(long btn)
{
    if (!sl_flipped || btn == BUTTON_NONE || (btn & BUTTON_TOUCHSCREEN)) return btn;
    long ud = btn & (BUTTON_UP | BUTTON_DOWN);
    if (ud == BUTTON_UP || ud == BUTTON_DOWN) btn ^= BUTTON_UP | BUTTON_DOWN;
    return btn;
}
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    int pos = 0;
    int btn = button_status_wdata(&pos);
    bool down = (btn & BUTTON_TOUCHSCREEN) != 0;
    int x = pos >> 16, y = pos & 0xffff;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= W) x = W - 1;
    if (y >= H) y = H - 1;
    cfm_touch_read(down, x, y);
    if (tap_replay == 1)
    {
        if (down) tap_replay = 0;          /* still down: read it as it is */
        else { down = true; x = tap_x; y = tap_y; tap_replay = 2; }
    }
    else if (tap_replay == 2) tap_replay = 0;
    if (sl_flipped) { x = W - 1 - x; y = H - 1 - y; }   /* the panel's corner is the picture's opposite one */
    /* before the lock screen takes the contact: otherwise the reader paused
     * the moment a lock-screen swipe flickered, and came back on a stale
     * queued position (swipes logged as going down, stuck halfway) */
    if (down) last_touch_ms = sl_ms();

    if (sl_power_filter_touch(down, x, y))
    {
        g_state = G_IDLE;
        lv_indev_reset(indev, NULL);
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = x; data->point.y = y;
        return;
    }
    if (nav_contact_blocked)
    {
        nav_contact_blocked = down;
        g_state = G_IDLE;
        lv_indev_reset(indev, NULL);
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = x; data->point.y = y;
        return;
    }
    if (down)
    {
        /* Speed from the last few samples, not the whole contact: a flick
         * at the end of a slow drag is still a flick. */
        if (g_state != G_IDLE)
        {
            int dt = (int)(lv_tick_get() - g_tl);
            if (dt > 0)
            {
                int v = (y - g_ly) * 1000 / dt, vx = (x - g_lx) * 1000 / dt;
                g_vy = dt > 100 ? v : (g_vy + v) / 2;
                g_vx = dt > 100 ? vx : (g_vx + vx) / 2;
                g_tl = lv_tick_get();
            }
        }
        if (g_state == G_IDLE && (lv_display_get_rotation(disp) != LV_DISPLAY_ROTATION_0 || cf_is_open() ||
                                  po_is_open()))
        {
            /* Cover Flow (either way up): no system gestures; the power off
             * slider: none at all (its knob starts in the back-swipe edge) */
            g_state = G_LVGL;
            g_dragged = false;
        }
        else if (g_state == G_IDLE && qp_edge_grab(y))
        {
            /* the Notification Center's bottom edge: the whole contact puts
             * it away, the list under the finger never scrolls first */
            g_state = G_PANEL;
            g_dragged = false;
            g_x0 = g_lx = x; g_y0 = g_ly = y; g_t0 = sl_ms();
            g_tl = lv_tick_get();
            g_vy = g_vx = 0;
            lv_indev_reset(indev, NULL);
        }
        else if (g_state == G_IDLE && hud_hit(x, y))
        {
            g_state = G_LVGL;                /* the volume bar: no system gestures */
            g_dragged = false;
            g_x0 = g_lx = x; g_y0 = g_ly = y; g_t0 = sl_ms();
        }
        else if (g_state == G_IDLE)
        {
            g_state = G_UNDECIDED;
            g_x0 = g_lx = x; g_y0 = g_ly = y; g_t0 = sl_ms();
            g_tl = lv_tick_get();
            g_vy = g_vx = 0;
            g_dragged = false;
            lv_obj_t *hit = lv_indev_search_obj(lv_screen_active(), &(lv_point_t){x, y});
            g_on_acts = acts_sheet_grab(x, y);
            g_on_sheet = g_on_acts || uq_sheet_grab(hit);
            g_on_game = snake_takes_swipe(y);
            g_no_back = overlay_visible();
            if (y < SB_H && !g_no_back) qp_glass_early();   /* maybe a pull: blur now, not on its first frame */
            g_on_mini = false;
            g_mid_back = true;
            for (lv_obj_t *o = hit; o; o = lv_obj_get_parent(o))
            {
                if (o == mini) g_on_mini = true;
                if (lv_obj_check_type(o, &lv_slider_class) || lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1))
                    g_no_back = true;
                /* what swipes sideways itself keeps its swipe away from the
                 * edge: song rows (USER_3), a sideways list, a canvas, keys */
                if (lv_obj_has_flag(o, LV_OBJ_FLAG_USER_3) || lv_obj_check_type(o, &lv_canvas_class) ||
                    lv_obj_check_type(o, &lv_buttonmatrix_class) || lv_obj_check_type(o, &lv_roller_class) ||
                    ((lv_obj_get_scroll_dir(o) & LV_DIR_HOR) && lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) &&
                     (lv_obj_get_scroll_left(o) > 0 || lv_obj_get_scroll_right(o) > 0)))
                    g_mid_back = false;
            }
        }
        else if (g_state == G_UNDECIDED)
        {
            int dx = x - g_x0, dy = y - g_y0;
            if (dx * dx + dy * dy > 12 * 12)
            {
                g_dragged = true;
                if (g_on_game)
                {
                    /* the game has the whole contact: no back-swipe, no press */
                    g_state = G_GAME;
                    lv_indev_reset(indev, NULL);
                    snake_swipe_begin(g_x0, g_y0);
                }
                else if (g_on_mini && abs(dx) > abs(dy))
                {
                    g_state = G_MINI;
                    lv_indev_reset(indev, NULL);
                }
                else if (g_on_sheet && dy > 0 && dy > abs(dx) * 6 / 5)
                {
                    g_state = G_SHEET;
                    lv_indev_reset(indev, NULL);      /* no row press, no scroll */
                }
                else if (g_y0 < SB_H && dy > 10 && dy > abs(dx) && !overlay_visible())
                {
                    g_state = G_PULL;
                    lv_indev_reset(indev, NULL);
                }
                else if (qx_grab(g_x0, g_y0, dx, dy))
                {
                    /* the open control centre, pulled further or folded:
                     * no tile press, no scroll */
                    g_state = G_GROW;
                    lv_indev_reset(indev, NULL);
                }
                else if (qx_page_grab(g_x0, g_y0, dx, dy))
                {
                    /* the grown control centre, sideways on its shortcuts:
                     * its pages turn, no tile press */
                    g_state = G_CCPAGE;
                    lv_indev_reset(indev, NULL);
                }
                else if (dx > 0 && !g_no_back && page_depth() > 1 &&
                         (g_x0 < 48 ? dx > (dy < 0 ? -dy : dy) * 6 / 5
                                    : g_mid_back && dx > (dy < 0 ? -dy : dy) * 2))   /* mid screen: clearly sideways, not a scroll */
                {
                    g_state = G_BACK;
                    lv_indev_reset(indev, NULL);      /* cancel press, no click */
                }
                else
                    g_state = G_LVGL;
            }
        }
        if (g_state == G_PANEL)
        {
            qp_edge_drag(y - g_y0);
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = x; data->point.y = y;
            g_lx = x; g_ly = y;
            return;
        }
        if (g_state == G_GROW)
        {
            qx_drag(y - g_y0);
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = x; data->point.y = y;
            g_lx = x; g_ly = y;
            return;
        }
        if (g_state == G_CCPAGE)
        {
            qx_page_drag(x - g_x0);
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = x; data->point.y = y;
            g_lx = x; g_ly = y;
            return;
        }
        if (g_state == G_PULL)
        {
            qp_pull(y - g_y0);
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = x; data->point.y = y;
            g_lx = x; g_ly = y;
            return;
        }
        if (g_state == G_BACK)
        {
            back_drag(x - g_x0);
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = x; data->point.y = y;
            g_lx = x; g_ly = y;
            return;
        }
        if (g_state == G_GAME)
        {
            snake_swipe_move(x, y);
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = x; data->point.y = y;
            g_lx = x; g_ly = y;
            return;
        }
        if (g_state == G_SHEET)
        {
            if (g_on_acts) acts_sheet_drag(y - g_y0); else uq_sheet_drag(y - g_y0);
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = x; data->point.y = y;
            g_lx = x; g_ly = y;
            return;
        }
        if (g_state == G_MINI)
        {
            mini_swipe_drag(x - g_x0);
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = x; data->point.y = y;
            g_lx = x; g_ly = y;
            return;
        }
        g_lx = x; g_ly = y;
        data->point.x = x; data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }

    if (g_state == G_PULL)
    {
        uint32_t dt = sl_ms() - g_t0;
        qp_pull_release(g_ly - g_y0, dt ? (g_ly - g_y0) * 1000 / (int)dt : 0,
                        lv_tick_get() - g_tl > 80 ? 0 : g_vy);
    }
    if (g_state == G_BACK)
    {
        uint32_t dt = sl_ms() - g_t0;
        back_release(g_lx - g_x0, dt ? (g_lx - g_x0) * 1000 / (int)dt : 0);
    }
    if (g_state == G_MINI)   /* a finger that stopped before lifting has no flick */
        mini_swipe_release(g_lx - g_x0, lv_tick_get() - g_tl > 80 ? 0 : g_vx);
    if (g_state == G_SHEET)   /* a finger that stopped before lifting has no flick */
    {
        if (g_on_acts) acts_sheet_release(g_ly - g_y0, lv_tick_get() - g_tl > 80 ? 0 : g_vy);
        else uq_sheet_release(g_ly - g_y0, lv_tick_get() - g_tl > 80 ? 0 : g_vy);
    }
    if (g_state == G_PANEL)   /* the same: a held finger lets the panel spring back */
        qp_edge_release(g_ly - g_y0, lv_tick_get() - g_tl > 80 ? 0 : g_vy);
    if (g_state == G_GROW)    /* and a held one lets it settle where it is nearer */
        qx_release(g_ly - g_y0, lv_tick_get() - g_tl > 80 ? 0 : g_vy);
    if (g_state == G_CCPAGE)  /* sideways, the same */
        qx_page_release(g_lx - g_x0, lv_tick_get() - g_tl > 80 ? 0 : g_vx);
    g_state = G_IDLE;
    data->point.x = g_lx; data->point.y = g_ly;
    data->state = LV_INDEV_STATE_RELEASED;
}

/* ------------------------------------------------------------ tokens */
/* The shapes, spaces and timings every page shares are in sloppy_int.h. */
/* elevation: 0 flat; 1 a hairline panel; 2 a knob; 3 something floating
 * over the page (mini player, banner, bubble); 4 a sheet over everything */
enum { SL_E0, SL_E1, SL_E2, SL_E3, SL_E4 };
static void sl_elevate(lv_obj_t *o, int level, lv_style_selector_t sel)
{
    static const uint8_t w[] = { 0, 0, 10, 28, 40 }, opa[] = { 0, 0, 127, 110, 153 }, dy[] = { 0, 0, 0, 6, 0 };
    lv_obj_set_style_shadow_width(o, w[level], sel);
    lv_obj_set_style_shadow_opa(o, opa[level], sel);
    lv_obj_set_style_shadow_offset_y(o, dy[level], sel);
    lv_obj_set_style_shadow_color(o, lv_color_black(), sel);
    if (level == SL_E1)
    {
        lv_obj_set_style_border_color(o, lv_color_hex(C_SUB), sel);
        lv_obj_set_style_border_opa(o, 36, sel);
        lv_obj_set_style_border_width(o, sl_zune() ? 0 : 1, sel);   /* Zule: words on black, no panel */
    }
}
/* Every slider's knob and caps: round ends on the track and the fill, a
 * white knob of one size centred on a track h high, swelling under the
 * finger and settling back on release. */
static void sl_slider_knob(lv_obj_t *s, int h, bool large)
{
    static lv_style_transition_dsc_t tr;
    static const lv_style_prop_t props[] = { LV_STYLE_PAD_TOP, LV_STYLE_PAD_BOTTOM, LV_STYLE_PAD_LEFT,
                                             LV_STYLE_PAD_RIGHT, LV_STYLE_SHADOW_WIDTH, 0 };
    if (!tr.time) lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, SL_T_FAST, 0, NULL);
    int rest = large ? SL_KNOB_LG : SL_KNOB, pr = large ? SL_KNOB_LG_PR : SL_KNOB_PR;
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, (rest - h) / 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, (pr - h) / 2, LV_PART_KNOB | LV_STATE_PRESSED);
    sl_elevate(s, SL_E2, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, SL_E2_LIFT, LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_transition(s, &tr, LV_PART_KNOB);
    lv_obj_set_style_transition(s, &tr, LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(s, 24);
}

/* --------------------------------------------------------- style bits */
static lv_style_t st_page, st_card, st_card_pr, st_title, st_sub, st_row, st_row_pr;

/* Press feedback eases in fast and settles back a little slower, so a tap
 * reads as a physical press rather than a snap between two frames. The
 * pressed state's transition runs on press, the default state's on release. */
static const lv_style_prop_t press_props[] = {
    LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_TRANSLATE_X, LV_STYLE_TEXT_OPA, 0
};
static lv_style_transition_dsc_t press_in, press_out;
static void press_motion(lv_obj_t *o)
{
    lv_obj_set_style_transition(o, &press_out, 0);
    lv_obj_set_style_transition(o, &press_in, LV_STATE_PRESSED);
}

static void styles_init(void)
{
    /* in: one frame or two, so the press shows under the finger at once */
    lv_style_transition_dsc_init(&press_in, press_props, lv_anim_path_ease_out, SL_T_TOUCH, 0, NULL);
    lv_style_transition_dsc_init(&press_out, press_props, lv_anim_path_ease_out, SL_T_PAGE, 0, NULL);
    lv_style_init(&st_page);
    lv_style_set_bg_color(&st_page, lv_color_hex(C_BG));
    lv_style_set_bg_opa(&st_page, LV_OPA_COVER);
    lv_style_set_text_color(&st_page, lv_color_hex(C_TEXT));
    lv_style_set_text_font(&st_page, &ss_semibold_21);

    lv_style_init(&st_card);
    lv_style_set_bg_color(&st_card, lv_color_hex(C_CARD));
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_radius(&st_card, SL_R_LG);
    lv_style_set_transition(&st_card, &press_out);
    lv_style_init(&st_card_pr);
    lv_style_set_bg_color(&st_card_pr, lv_color_hex(C_CARD_P));
    lv_style_set_transition(&st_card_pr, &press_in);

    lv_style_init(&st_row);
    lv_style_set_radius(&st_row, SL_R_MD);
    lv_style_set_bg_opa(&st_row, LV_OPA_TRANSP);
    lv_style_set_bg_color(&st_row, lv_color_hex(C_CARD_P));   /* only the opacity eases: no flash of another colour */
    lv_style_set_transition(&st_row, &press_out);
    lv_style_init(&st_row_pr);
    lv_style_set_bg_opa(&st_row_pr, LV_OPA_COVER);
    lv_style_set_bg_color(&st_row_pr, lv_color_hex(C_CARD_P));
    lv_style_set_transition(&st_row_pr, &press_in);

    lv_style_init(&st_title);
    lv_style_set_text_font(&st_title, &ss_semibold_23);
    lv_style_set_text_color(&st_title, lv_color_hex(C_TEXT));
    lv_style_init(&st_sub);
    lv_style_set_text_font(&st_sub, &ss_regular_18);
    lv_style_set_text_color(&st_sub, lv_color_hex(C_SUB));
}

/* The shared styles in the current theme. styles_init runs at the splash,
 * before Rockbox has read the settings, so they start out dark. */
static void styles_retheme(void)
{
    lv_style_set_radius(&st_card, sl_zune() ? 0 : SL_R_LG);   /* Zune: flat, type does the work */
    lv_style_set_bg_color(&st_page, lv_color_hex(C_BG));
    lv_style_set_text_color(&st_page, lv_color_hex(C_TEXT));
    lv_style_set_bg_color(&st_card, lv_color_hex(C_CARD));
    lv_style_set_bg_color(&st_card_pr, lv_color_hex(C_CARD_P));
    lv_style_set_bg_color(&st_row, lv_color_hex(C_CARD_P));
    lv_style_set_bg_color(&st_row_pr, lv_color_hex(C_CARD_P));
    lv_style_set_text_color(&st_title, lv_color_hex(C_TEXT));
    lv_style_set_text_color(&st_sub, lv_color_hex(C_SUB));
    lv_obj_report_style_change(&st_page);
    lv_obj_report_style_change(&st_card);
    lv_obj_report_style_change(&st_card_pr);
    lv_obj_report_style_change(&st_row);
    lv_obj_report_style_change(&st_row_pr);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(C_BG), 0);
}

/* Lowercase, as the Zune wrote it, and capitals for a header: ASCII, the
 * Latin-1 and Latin Extended-A letters (É to é, Ł to ł, so "Álbumes" reads
 * "álbumes" and never "Álbumes") and Cyrillic; each pair is two bytes in
 * UTF-8 both ways, so the words keep their length. Any other script passes
 * through untouched. */
static uint32_t sl_recase_cp(uint32_t c, bool up)
{
    if (c >= 0xc0 && c <= 0xfe && c != 0xd7 && c != 0xf7 && c != 0xdf)   /* not the multiplication and division signs, nor sharp s */
        return up ? (c >= 0xe0 ? c - 0x20 : c) : (c < 0xe0 ? c + 0x20 : c);
    if (c >= 0x100 && c <= 0x17f && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x178 && c != 0x17f)
    {
        bool odd_up = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e);   /* the runs that start on an odd capital */
        bool is_up = odd_up ? (c & 1) : !(c & 1);
        return up == is_up ? c : up ? c - 1 : c + 1;   /* each capital's small letter follows it */
    }
    if (c >= 0x400 && c <= 0x40f) return up ? c : c + 0x50;
    if (c >= 0x410 && c <= 0x42f) return up ? c : c + 0x20;
    if (c >= 0x430 && c <= 0x44f) return up ? c - 0x20 : c;
    if (c >= 0x450 && c <= 0x45f) return up ? c - 0x50 : c;
    return c;
}
static const char *sl_recase(const char *s, char *buf, size_t n, bool up)
{
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++)
    {
        unsigned char c = (unsigned char)s[i];
        buf[i] = up ? (c >= 'a' && c <= 'z' ? (char)(c - 32) : (char)c) : (c >= 'A' && c <= 'Z' ? (char)(c + 32) : (char)c);
        unsigned char d = (unsigned char)s[i + 1];
        if (c >= 0xc3 && c <= 0xd1 && (d & 0xc0) == 0x80 && i + 2 < n)
        {
            uint32_t cp = sl_recase_cp(((uint32_t)(c & 0x1f) << 6) | (d & 0x3f), up);
            buf[i] = (char)(0xc0 | (cp >> 6));
            buf[++i] = (char)(0x80 | (cp & 0x3f));
        }
    }
    buf[i] = '\0';
    return buf;
}
static const char *sl_lower(const char *s, char *buf, size_t n) { return sl_recase(s, buf, n, false); }
/* A label's words as the theme writes them: translated, and lowercase
 * under Zune. Four buffers in turn, for the few labels of one build. */
static const char *sl_case(const char *s)
{
    static char buf[4][96];
    static int next;
    if (!sl_zune()) return sl_tr(s);
    next = (next + 1) & 3;
    return sl_lower(sl_tr(s), buf[next], sizeof buf[0]);
}
/* Hebrew or Arabic chosen in Settings > Language: right-to-left words run
 * from the right edge as the interface's own do */
static bool sl_ui_rtl(void) { return global_settings.sloppy_language == 4 || global_settings.sloppy_language == 5; }
/* A row laid out from its leading edge: the left in English, the right in
 * Hebrew and Arabic. Aligned, never set, as Settings' set_place: an
 * explicit alignment is not mirrored again by LVGL. */
static lv_align_t sl_lead(lv_align_t a)
{
    if (!sl_ui_rtl()) return a;
    switch (a)
    {
    case LV_ALIGN_TOP_LEFT: return LV_ALIGN_TOP_RIGHT;
    case LV_ALIGN_TOP_RIGHT: return LV_ALIGN_TOP_LEFT;
    case LV_ALIGN_LEFT_MID: return LV_ALIGN_RIGHT_MID;
    case LV_ALIGN_RIGHT_MID: return LV_ALIGN_LEFT_MID;
    case LV_ALIGN_BOTTOM_LEFT: return LV_ALIGN_BOTTOM_RIGHT;
    case LV_ALIGN_BOTTOM_RIGHT: return LV_ALIGN_BOTTOM_LEFT;
    default: return a;
    }
}
static void sl_align(lv_obj_t *o, lv_align_t a, int x, int y) { lv_obj_align(o, sl_lead(a), sl_ui_rtl() ? -x : x, y); }
/* words at x from the leading edge; they run from it (right-aligned in RTL,
 * a Latin name in a Hebrew list too) */
static void sl_place(lv_obj_t *l, int x, int y)
{
    sl_align(l, LV_ALIGN_TOP_LEFT, x, y);
    if (sl_ui_rtl() && lv_obj_check_type(l, &lv_label_class)) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
}
/* a row's chevron points the way the page reads */
static const char *sl_chevron(void) { return sl_ui_rtl() ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT; }
/* A count as the language writes it: 10,000 in English, 10.000 in
 * Spanish, German, Italian, Dutch and Portuguese, 10 000 in French, Polish,
 * Czech and Russian */
static const char *sl_fmt_count(char *buf, size_t n, int v)
{
    static const char sep[SL_LANG_LAST + 1] = { ',', '.', ' ', '.', ',', ',', ',', '.', '.', '.', ' ', ' ', ' ', ',' };
    int lang = global_settings.sloppy_language;
    char d[16];
    int len = snprintf(d, sizeof d, "%d", v < 0 ? 0 : v), o = 0;
    for (int i = 0; i < len && o + 2 < (int)n; i++)
    {
        if (i && (len - i) % 3 == 0) buf[o++] = sep[lang > 0 && lang <= SL_LANG_LAST ? lang : 0];
        buf[o++] = d[i];
    }
    buf[o] = '\0';
    return buf;
}
static const char *sl_upper(const char *s, char *buf, size_t n) { return sl_recase(s, buf, n, true); }
/* "80 songs" as the language says it: the count grouped its way inside
 * the singular or plural phrase from the table ("%s song", "%s songs") */
static const char *sl_count(char *buf, size_t n, int v, const char *one, const char *many)
{
    char num[16];
    snprintf(buf, n, sl_tr(v == 1 ? one : many), sl_fmt_count(num, sizeof num, v));
    return buf;
}
/* The big Zune type has ASCII only: words in another script or with
 * accents are set in the largest face that has them, not left with gaps. */
static const lv_font_t *sl_zune_font(const char *s, const lv_font_t *big)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0x80) return &ss_semibold_30;
    return big;
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}
static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *f, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_base_dir(l, LV_BASE_DIR_AUTO, 0);
    lv_label_set_text(l, sl_tr(s));
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color == 0xffffff ? C_TEXT : color), 0);
    return l;
}
/* words on a surface that is dark in every theme (a hero, glass): white,
 * where text() would take the light theme's dark ink */
static lv_obj_t *text_light(lv_obj_t *parent, const char *s, const lv_font_t *f)
{
    lv_obj_t *l = text(parent, s, f, 0xffffff);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    return l;
}
#define MAX_PAGES 12
static lv_obj_t *pages[MAX_PAGES];
static int npages;
/* A tap belongs to the page on top. While a new page slides in (220 ms)
 * the one it covers is still under the finger: a quick second tap opened
 * the same page twice, and pages keeping their objects in globals then
 * crashed (found by the fuzzer, a double tap on a quiz mode). */
static bool on_covered_page(lv_obj_t *o)
{
    if (npages < 2) return false;
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_USER_2))
        {
            for (int i = 0; i < npages - 1; i++)
                if (pages[i] == o) return true;
            return false;
        }
    return false;
}
static void click_drag_guard(lv_event_t *e)
{
    if ((g_dragged && lv_indev_active() == touch) ||
        (lv_indev_active() == touch && on_covered_page(lv_event_get_current_target(e))))
        lv_event_stop_processing(e);
}
/* LV_LABEL_LONG_DOT only truncates when the label has a fixed height; with
 * content height it silently wraps and overlaps whatever sits below. */
/* Arabic letters hang further below the line than the Latin font they
 * fall back from, past the quarter line LVGL lets a label draw beyond its
 * box: let it draw them. No layout moves. */
static void sl_label_deep_cb(lv_event_t *e)
{
    const lv_font_t *f = lv_obj_get_style_text_font(lv_event_get_target(e), LV_PART_MAIN);
    int below = 0;
    for (const lv_font_t *b = f->fallback; b; b = b->fallback)
        if (b->base_line - f->base_line > below) below = b->base_line - f->base_line;
    lv_event_set_ext_draw_size(e, lv_font_get_line_height(f) / 4 + below);
}
/* one line of f, with room for the deepest letters of the fonts it falls
 * back to: a fixed row that clips its words needs this much */
static int sl_line_h(const lv_font_t *f)
{
    int below = f->base_line;
    for (const lv_font_t *b = f->fallback; b; b = b->fallback)
        if (b->base_line > below) below = b->base_line;
    return lv_font_get_line_height(f) + below - f->base_line;
}
/* Arabic in s: its letters hang lower than the Latin line (sl_line_h) */
static bool sl_has_arabic(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++)
        if (*p >= 0xd8 && *p <= 0xdb) return true;
    return false;
}
static void sl_label_deep(lv_obj_t *l)
{
    lv_obj_add_event_cb(l, sl_label_deep_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(l);
}
static void label_lines(lv_obj_t *l, int lines)
{
    sl_label_deep(l);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    const lv_font_t *font = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    int line = lv_font_get_line_height(font) + lv_obj_get_style_text_line_space(l, LV_PART_MAIN);
    lv_obj_set_height(l, line * lines - lv_obj_get_style_text_line_space(l, LV_PART_MAIN));
}
/* Words in a button of fixed size: the text size in use, or the largest
 * smaller step whose words fit; failing even Standard, an ellipsis. A long
 * word in a long language ran out of its key, chip or tab. */
static void sl_fit_now(lv_obj_t *l, int room)
{
    const lv_font_t *f = lv_obj_get_style_text_font(l, LV_PART_MAIN);
    int k = 0;
    while (k < 6 && sl_fonts[sl_text_step][k] != f) k++;
    if (room <= 0 || k == 6) return;
    for (int s = sl_text_step; s >= 0; s--)
    {
        lv_point_t z;
        lv_text_get_size(&z, lv_label_get_text(l), sl_fonts[s][k], lv_obj_get_style_text_letter_space(l, LV_PART_MAIN), 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_style_text_font(l, sl_fonts[s][k], 0);
        if (z.x <= room) return;
    }
    lv_obj_set_width(l, room);
    label_lines(l, 1);
}
static void sl_fit_cb(lv_event_t *e)
{
    sl_fit_now(lv_event_get_user_data(e), lv_obj_get_content_width(lv_event_get_target(e)) - 2 * SL_SP_S);
}
/* fit l to its button, now and whenever the button is sized */
static void sl_fit(lv_obj_t *l)
{
    lv_obj_t *b = lv_obj_get_parent(l);
    lv_obj_add_event_cb(b, sl_fit_cb, LV_EVENT_SIZE_CHANGED, l);
    if (lv_obj_get_style_width(b, LV_PART_MAIN) > 0 && !LV_COORD_IS_SPEC(lv_obj_get_style_width(b, LV_PART_MAIN)))
        sl_fit_now(l, lv_obj_get_style_width(b, LV_PART_MAIN) - lv_obj_get_style_pad_left(b, LV_PART_MAIN) -
                      lv_obj_get_style_pad_right(b, LV_PART_MAIN) - 2 * SL_SP_S);
}
static void on_click(lv_obj_t *o, lv_event_cb_t cb, void *ud)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, click_drag_guard, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, ud);
}
#include "sloppy_icons.inc"

/* A generated cover: an album-coloured gradient, until (or unless) the
 * album's art comes. With mark, a note in the album's own light tint sits
 * in it: the title is printed under these covers, so it isn't repeated in
 * the art (B2). */
static lv_obj_t *cover(lv_obj_t *parent, int album, int size, bool mark)
{
    const struct sl_album *al = &sl_lib.albums[album];
    lv_obj_t *c = box(parent);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : size > 100 ? SL_R_MD : SL_R_SM, 0);   /* Zune: square art */
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hsv_to_rgb(al->hue, 62, 62), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hsv_to_rgb((al->hue + 70) % 360, 70, 22), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_clip_corner(c, false, 0);
    lv_obj_t *n = NULL;
    if (mark)
    {
        int s = size * 3 / 8;
        n = sl_icon(c, IC_SONGS, s, lv_color_to_u32(lv_color_hsv_to_rgb(al->hue, 28, 88)) & 0xffffff,
                    lv_color_to_u32(lv_color_hsv_to_rgb(al->hue, 62, 62)) & 0xffffff);
        lv_obj_center(n);
        lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(n, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    sl_art_apply(c, album, size);
    if (n && lv_obj_get_child_count(c) > 1) lv_obj_delete(n);   /* the art came at once: nothing to draw under it */
    return c;
}

static void fmt_time(char *b, int n, long ms)
{
    long s = ms < 0 ? 0 : ms / 1000;
    if (s >= 3600) snprintf(b, n, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(b, n, "%ld:%02ld", s / 60, s % 60);   /* 3:05, as every player writes it */
}

/* How far the ears are behind Rockbox's position: what is queued past the
 * mixer for a Bluetooth output (our ring, BlueALSA, the headphones' own
 * delay when they report it); about 0.6-1.7 s on the R1 (2026-10-23). The
 * jack's few tens of ms are left alone. */
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
unsigned hiby_out_delay_ms(void);
const char *hiby_pcm_get_bt_mac(void);
static unsigned sl_out_latency_ms(void) { return hiby_pcm_get_bt_mac() ? hiby_out_delay_ms() : 0; }
static bool sl_out_is_bt(void) { return hiby_pcm_get_bt_mac() != NULL; }
#else
static unsigned sl_test_out_latency_ms;  /* the harness's pretend Bluetooth delay */
static bool sl_test_out_bt;
static unsigned sl_out_latency_ms(void) { return sl_test_out_latency_ms; }
static bool sl_out_is_bt(void) { return sl_test_out_bt; }
#endif
/* Music going out over Bluetooth: BlueALSA's encoder (fdk-aac, LDAC) and
 * its daemons share the CPU and the 64 MB with us. a second test R1 (2026-10-23)
 * drew 0.5-2.3 s passes with 4-5 MB free while a speaker played. The
 * extras that cost most (the visualizer's full frame rate, the cover's
 * neighbour reflection, the glass kept in memory) give way. */
static bool sl_bt_streaming(void)
{
    int st = audio_status();
    return sl_out_is_bt() && (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
}

/* A skip can briefly expose an invalid codec elapsed value. Keep the seek
 * controls bounded until the new track reports a usable position. The
 * position shown (progress bars, lyrics, the turntable) is the one heard. */
static unsigned long visible_elapsed(const struct mp3entry *id3)
{
    if (!id3->length ||
        (id3->elapsed > id3->length && id3->elapsed - id3->length > 2000UL))
        return 0;
    unsigned long at = id3->elapsed > id3->length ? id3->length : id3->elapsed, lag = sl_out_latency_ms();
    return at > lag ? at - lag : 0;
}

/* ------------------------------------------------------- library state */
static int lib_state = SL_LIB_NO_DB;
static bool lib_usb_stopped;            /* USB mode stopped a library read: it starts over after */
bool sl_sort_as_written;                /* sloppy_int.h: Library options' Ignore The, A and An, off */
/* Library options the library is read and sorted with, before each read */
static void lib_options_apply(void)
{
    sl_sort_as_written = global_settings.sloppy_sort_articles != 0;
    sl_lib_keep_apart = global_settings.sloppy_albums_apart != 0;
}
/* a library read, timed for the log: all of it, and its longest step (the
 * longest the screen waited on it) */
static uint32_t lib_read_at, lib_step_max_us;
static bool lib_read_db;
static int lib_begin(void)
{
    uint64_t at = sl_perf_now_us();
    lib_read_at = sl_ms();
    int state = sl_lib_begin();
    lib_step_max_us = (uint32_t)(sl_perf_now_us() - at);
    lib_read_db = sl_lib_scanning();
    return state;
}
static lv_obj_t *lib_banner, *lib_banner_txt, *lib_banner_fill;
static void db_update_start(void);
static void db_update_begin(bool show);
static bool db_check_quiet;              /* sloppy_dbupdate.inc */
static bool dbu_showing(void);
static bool lib_ready(void)
{
    if (lib_state == SL_LIB_READY)
        return true;
    return false;
}

/* Notifications (sloppy_notify.inc): an event, a failure, and one whose
 * words can name something of yours. The file saying it is its source. */
static void notify_src_file(const char *file, int kind, unsigned flags, const char *msg);
static void notify_ex(int src, int kind, unsigned flags, int action, const char *key, const char *msg);
#define toast(m)         notify_src_file(__FILE__, 0, 0, (m))
#define toast_warn(m)    notify_src_file(__FILE__, 1, 0, (m))
#define toast_private(m) notify_src_file(__FILE__, 0, 1, (m))

/* ------------------------------------------------------------ playback */
#include "sloppy_queue.h"
/* Starting something the user just picked cuts the old song cleanly. The
 * core treats a new playlist over a playing one as a manual track change,
 * and with crossfade on skip the old song carried on under the new one for
 * the whole fade (9 s on a test player). Crossfade stays for songs flowing
 * into each other inside a queue. */
static int album_chain = -1;          /* last album queued by play-through, as its albums_by_title spot */
/* A new list keeps what the user queued: note it before playlist_create()
 * wipes the old list, and queue it again once the new one plays. */
static int sl_new_list(const char *dir)
{
    if (audio_status() & AUDIO_STATUS_PLAY) q_keep();
    else q_kept_n = 0;
    int result = playlist_create(dir, NULL);
    if (result < 0)
    {
        q_kept_n = 0;
        audio_stop();                 /* Rockbox has already replaced the old list */
    }
    return result;
}
static void sl_abort_list(void)
{
    q_kept_n = 0;
    audio_stop();                     /* a partial list cannot back the current decoder */
}
static void sr_stop(void);
/* from ms into the song at index (a cue sheet's track) */
static void sl_start_list_at(int index, unsigned long ms)
{
    album_chain = -1;                 /* a new list: play_album re-arms it after */
    sr_stop();                        /* and radio, which sets itself going after */
    if (audio_status() & AUDIO_STATUS_PLAY) audio_stop();
    playlist_start(index, ms, 0);
    if (q_kept_n) sl_log("queue carried over: %d", q_restore());
}
static void sl_start_list(int index) { sl_start_list_at(index, 0); }
/* Every shuffle caller passes 0, and playlist_shuffle keeps the start
 * song first, so the list's first song (by title: "!!!!!!!") always
 * opened it. Start on a random song instead. The clock adds entropy
 * so a shuffle a fixed time after boot differs day to day. */
static unsigned sl_shuffle_seed(void)
{
    struct tm *tm = get_time();
    unsigned seed = (unsigned)current_tick * 2654435761u
                  ^ (unsigned)(tm->tm_yday * 86400 + tm->tm_hour * 3600
                               + tm->tm_min * 60 + tm->tm_sec);
    return seed ? seed : 1;
}
/* Shuffle and unshuffle reorder the whole list, queued songs too: lift the
 * queue out first and put it back after the playing song. */
static void sl_set_shuffle(bool on)
{
    global_settings.playlist_shuffle = on;
    bool playing = (audio_status() & AUDIO_STATUS_PLAY) != 0;
    if (playing && q_keep()) q_drop();
    if (on) playlist_randomise(NULL, current_tick, true);
    else playlist_sort(NULL, true);
    if (playing) q_restore();
}
static bool play_list(const int *tracks, int n, int start, bool shuffle)
{
    if (!tracks || n <= 0 || start < 0 || start >= n) return false;
    album_chain = -1;
    for (int i = 0; i < n; i++)
        if (tracks[i] < 0 || tracks[i] >= sl_lib.n_tracks) return false;
    char path[MAX_PATH];
    sl_log("play %d tracks from %d%s: %s", n, start, shuffle ? " shuffled" : "", sl_track_path(tracks[start], path, sizeof path));
    if (sl_new_list(NULL) < 0)
    {
        toast_warn("Could not create playlist");
        return false;
    }
    /* a cue sheet's tracks one after another are their file once: it plays
     * on from the track picked through the rest of the sheet */
    int entries = 0, start_entry = 0;
    long from = 0;
    for (int i = 0; i < n; i++)
    {
        const struct sl_track *tr = &sl_lib.tracks[tracks[i]];
        bool same_file = i && tr->cue && sl_lib.tracks[tracks[i - 1]].cue
                         && sl_track_same_file(tracks[i], tracks[i - 1]);
        if (i == start) { start_entry = same_file ? entries - 1 : entries; from = tr->cue && !same_file ? tr->start : 0; }
        if (same_file) continue;
        if (playlist_insert_track(NULL, sl_track_path(tracks[i], path, sizeof path), PLAYLIST_INSERT_LAST, false, false) < 0)   /* it copies the path */
        {
            sl_log("playlist: stopped adding at track %d", i);
            sl_abort_list();
            toast_warn("Could not load all songs. Check card space.");
            return false;
        }
        entries++;
        if ((i & 15) == 15) yield();
    }
    /* started inside a run: the run's file from that track */
    if (sl_lib.tracks[tracks[start]].cue) from = sl_lib.tracks[tracks[start]].start;
    playlist_sync(NULL);
    if (shuffle)
    {
        unsigned seed = sl_shuffle_seed();
        srand(seed);
        start_entry = playlist_shuffle(seed, rand() % entries);
        from = 0;
    }
    sl_start_list_at(start_entry, (unsigned long)from);
    return true;
}
/* An album started in order; with play-through on, the next album in the
 * Albums list is queued while its last song plays. */
static bool play_album(int album, int start, bool shuffle)
{
    const struct sl_album *al = &sl_lib.albums[album];
    if (!play_list(al->tracks, al->n_tracks, start, shuffle)) return false;
    if (!shuffle && global_settings.sloppy_album_through)
        for (int i = 0; i < sl_lib.n_albums; i++)
            if (sl_lib.albums_by_title[i] == album) album_chain = i;
    return true;
}
static bool quiz_clip_active(void);
static void album_chain_poll(void)
{
    if (album_chain < 0 || quiz_clip_active()) return;   /* a clip is not the album */
    if (!global_settings.sloppy_album_through || !(audio_status() & AUDIO_STATUS_PLAY))
    { album_chain = -1; return; }
    if (playlist_get_display_index() < playlist_amount()) return;
    if (++album_chain >= sl_lib.n_albums) { album_chain = -1; return; }
    const struct sl_album *al = &sl_lib.albums[sl_lib.albums_by_title[album_chain]];
    char path[MAX_PATH];
    for (int i = 0; i < al->n_tracks; i++)
        if (playlist_insert_track(NULL, sl_track_path(al->tracks[i], path, sizeof path), PLAYLIST_INSERT_LAST, false, false) < 0)
        { album_chain = -1; break; }
    playlist_sync(NULL);
    sl_log("play through: queued %s", al->title);
}
static bool is_playing(void) { return (audio_status() & AUDIO_STATUS_PLAY) && !(audio_status() & AUDIO_STATUS_PAUSE); }
static bool sl_bt_resume_due;              /* a route change paused, to resume shortly */
/* When playback last paused, by any hand (0: playing). A book picks up a
 * little before where it stopped, more the longer it waited: 3 seconds
 * after a short pause, 10 after a minute, 30 after an hour. */
static uint32_t sl_paused_at;
static bool sl_book_now(void);
static void sl_paused_poll(void)
{
    int st = audio_status();
    if (!(st & AUDIO_STATUS_PLAY) || !(st & AUDIO_STATUS_PAUSE)) sl_paused_at = 0;
    else if (!sl_paused_at) sl_paused_at = sl_ms();
}
static long sl_book_rewind_ms(uint32_t away)
{
    return away < 3000 ? 0 : away < 60000 ? 3000 : away < 3600000 ? 10000 : 30000;
}
static void sl_resume(void)
{
    if (global_settings.sloppy_book_rewind && sl_paused_at && sl_book_now())
    {
        struct mp3entry *id3 = audio_current_track();
        long back = sl_book_rewind_ms(sl_ms() - sl_paused_at);
        if (id3 && back)
        {
            long to = (long)id3->elapsed - back;
            audio_ff_rewind(to < 0 ? 0 : to);
        }
    }
    sl_paused_at = 0;
    audio_resume();
}
static void toggle_play(void)
{
    int st = audio_status();
    sl_bt_resume_due = false;                 /* the user decides now */
    if (!(st & AUDIO_STATUS_PLAY))
    {
        if (playlist_resume() != -1)
        {
            /* Resume by the audible time when it is known. The saved byte
             * offset is the decoder's read position, which runs seconds
             * ahead of what was heard (more with crossfade); the MP3 codec
             * would seek there but show the older time. */
            unsigned long offset = global_status.resume_elapsed ? 0 : global_status.resume_offset;
            playlist_resume_track(global_status.resume_index, global_status.resume_crc32,
                                  global_status.resume_elapsed, offset);
        }
    }
    else if (st & AUDIO_STATUS_PAUSE)
        sl_resume();
    else
    {
        audio_pause();
        sl_paused_at = sl_ms();
    }
}

/* --------------------------------------------------------- navigation */
static bool sb_on_wall;                   /* status bar inked for the home wallpaper */
static void statusbar_retheme(void);
static lv_obj_t *sb_ink_page;            /* the page that chose the status bar's ink (statusbar_ink) */
static uint32_t sb_ink_color;
static bool sb_ink_pending;              /* chosen while the page is built: it is the next one pushed */
static void sb_ink_forget_cb(lv_event_t *e) { if (lv_event_get_target(e) == sb_ink_page) sb_ink_page = NULL; }
static bool pet_care_locked;
/* mini player is declared above touch_read for its swipe gesture */

static int page_depth(void) { return npages; }

static void anim_x(lv_obj_t *o, int from, int to, int ms, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&a, lv_anim_path_custom_bezier3);
    a.parameter.bezier3 = (lv_anim_bezier3_para_t){ LV_BEZIER_VAL_FLOAT(0.32), LV_BEZIER_VAL_FLOAT(0.72),
                                                    LV_BEZIER_VAL_FLOAT(0.0), LV_BEZIER_VAL_FLOAT(1.0) };
    if (done)
        lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void page_hide_cb(lv_anim_t *a)
{
    lv_obj_t *p = a->var;
    if (lv_obj_get_x(p) < 0)
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);   /* covered: skip drawing it */
}
static void page_delete_cb(lv_anim_t *a) { lv_obj_delete(a->var); }

/* Give the first visible menu items a quick, staggered entrance. Keeping the
 * reveal bounded avoids animating an entire long song or album list. */
static void ui_reveal_y(void *obj, int32_t value)
{ lv_obj_set_style_translate_y(obj, value, 0); }
static void ui_reveal_item(lv_obj_t *obj, unsigned delay)
{
    lv_obj_set_style_translate_y(obj, 12, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 12, 0);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, ui_reveal_y);
    lv_anim_start(&a);
}
static void ui_reveal_page(lv_obj_t *p)
{
    if (!lv_obj_has_flag(p, LV_OBJ_FLAG_USER_2)) return;
    lv_obj_t *header = lv_obj_get_child(p, 0);
    lv_obj_t *body = lv_obj_get_child(p, 1);
    if (lv_obj_get_child_count(header))
        ui_reveal_item(lv_obj_get_child(header, lv_obj_get_child_count(header) - 1), 20);
    uint32_t count = lv_obj_get_child_count(body);
    if (count > 5) count = 5;
    for (uint32_t i = 0; i < count; i++)
        ui_reveal_item(lv_obj_get_child(body, i), 45 + i * 30);
}

/* A push slides two pictures, not two pages: each page is drawn once into
 * a snapshot and the frames between only move the pictures. A live slide
 * redrew both pages' every row, shadow and corner each frame, about 50 ms
 * a frame on the R1; a picture is a copy. (The Zune theme keeps the live
 * slide: its big titles trail the page.) */
static struct { lv_obj_t *img[2], *page, *below; lv_draw_buf_t *buf[2]; } ps;
static void ps_gone_cb(lv_event_t *e);
static long sl_mem_available_kb(void);
/* The pictures borrow 1.5 MB for a moment. With the player short of memory
 * that borrowing pushed out the code and the song being read, and every
 * page then took seconds (the R1 log, 5 MB free with Bluetooth on): only
 * with room to spare, else the live slide. -1 is no /proc (the desk). */
#define PS_MIN_FREE_KB (24 * 1024)
static bool ps_room(void)
{
    long kb = sl_mem_available_kb();
    return kb < 0 || kb >= PS_MIN_FREE_KB;
}
static bool ps_back;                     /* the slide is a pop: ps.page goes at the end */
static void ps_end(void)
{
    if (!ps.page) return;
    if (ps_back)
    {
        ps_back = false;
        lv_obj_t *gone = ps.page;
        for (int i = 0; i < 2; i++) lv_anim_delete(ps.img[i], NULL);
        lv_obj_remove_event_cb(ps.page, ps_gone_cb);
        lv_obj_remove_event_cb(ps.below, ps_gone_cb);
        lv_obj_set_x(ps.below, 0);
        ps.page = ps.below = NULL;
        for (int i = 0; i < 2; i++)
        {
            lv_obj_delete(ps.img[i]);
            lv_draw_buf_destroy(ps.buf[i]);
            ps.img[i] = NULL; ps.buf[i] = NULL;
        }
        lv_obj_delete(gone);
        return;
    }
    for (int i = 0; i < 2; i++)
    {
        if (ps.img[i]) { lv_anim_delete(ps.img[i], NULL); lv_obj_delete(ps.img[i]); }
        if (ps.buf[i]) lv_draw_buf_destroy(ps.buf[i]);
        ps.img[i] = NULL; ps.buf[i] = NULL;
    }
    lv_obj_set_x(ps.page, 0);
    lv_obj_set_x(ps.below, -W * 3 / 10);
    lv_obj_add_flag(ps.below, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_event_cb(ps.page, ps_gone_cb);
    lv_obj_remove_event_cb(ps.below, ps_gone_cb);
    ps.page = ps.below = NULL;
}
static void ps_done_cb(lv_anim_t *a) { (void)a; ps_end(); }
/* a page deleted while it slides (page_bring_back, a theme change): the
 * pictures go with it */
static void ps_gone_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    if (o != ps.page && o != ps.below) return;
    lv_obj_t *other = o == ps.page ? ps.below : ps.page;
    bool back = ps_back;
    ps.page = ps.below = NULL;
    ps_back = false;
    for (int i = 0; i < 2; i++)
    {
        if (ps.img[i]) { lv_anim_delete(ps.img[i], NULL); lv_obj_delete(ps.img[i]); }
        if (ps.buf[i]) lv_draw_buf_destroy(ps.buf[i]);
        ps.img[i] = NULL; ps.buf[i] = NULL;
    }
    if (!other) return;
    lv_obj_remove_event_cb(other, ps_gone_cb);
    /* going back, the page above is out of the stack already: it goes too,
     * or it stays alive with nothing to delete it (the fuzzer found it) */
    if (back && other != pages[npages - 1]) { lv_obj_delete(other); return; }
    lv_obj_set_x(other, other == pages[npages - 1] ? 0 : -W * 3 / 10);
}
static bool ps_slide(lv_obj_t *p, lv_obj_t *below)
{
    if (sl_zune() || ps.page || !ps_room()) return false;
    lv_obj_set_x(p, 0);
    lv_obj_update_layout(p);
    ps.buf[0] = lv_snapshot_take(below, LV_COLOR_FORMAT_RGB565);
    ps.buf[1] = ps.buf[0] ? lv_snapshot_take(p, LV_COLOR_FORMAT_RGB565) : NULL;
    if (!ps.buf[1])
    {
        if (ps.buf[0]) lv_draw_buf_destroy(ps.buf[0]);
        ps.buf[0] = NULL;
        return false;                    /* no memory: the live slide */
    }
    ps.page = p; ps.below = below;
    lv_obj_add_event_cb(p, ps_gone_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(below, ps_gone_cb, LV_EVENT_DELETE, NULL);
    lv_obj_t *parent = lv_obj_get_parent(p);
    for (int i = 0; i < 2; i++)
    {
        ps.img[i] = lv_image_create(parent);
        lv_image_set_src(ps.img[i], ps.buf[i]);
        lv_obj_remove_flag(ps.img[i], LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_move_foreground(ps.img[0]);
    lv_obj_move_foreground(ps.img[1]);
    if (mini) lv_obj_move_foreground(mini);
    /* the pages wait just off the screen, not hidden: nothing draws them,
     * and a game on the new page runs from the first frame as before */
    lv_obj_set_x(p, W);
    lv_obj_set_x(below, -W);
    anim_x(ps.img[1], W, 0, 220, ps_done_cb);
    anim_x(ps.img[0], 0, -W * 3 / 10, 220, NULL);
    return true;
}

/* Back: the same, the other way; the page above is deleted at the end. */
static void ps_pop_done_cb(lv_anim_t *a) { (void)a; ps_end(); }
static bool ps_slide_back(lv_obj_t *p, lv_obj_t *below, int x, int ms)
{
    if (sl_zune() || ps.page || !ps_room()) return false;
    lv_obj_set_x(below, 0);
    lv_obj_update_layout(below);
    ps.buf[0] = lv_snapshot_take(below, LV_COLOR_FORMAT_RGB565);
    ps.buf[1] = ps.buf[0] ? lv_snapshot_take(p, LV_COLOR_FORMAT_RGB565) : NULL;
    if (!ps.buf[1])
    {
        if (ps.buf[0]) lv_draw_buf_destroy(ps.buf[0]);
        ps.buf[0] = NULL;
        return false;
    }
    ps.page = p; ps.below = below;
    lv_obj_add_event_cb(p, ps_gone_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(below, ps_gone_cb, LV_EVENT_DELETE, NULL);
    lv_obj_t *parent = lv_obj_get_parent(p);
    for (int i = 0; i < 2; i++)
    {
        ps.img[i] = lv_image_create(parent);
        lv_image_set_src(ps.img[i], ps.buf[i]);
        lv_obj_remove_flag(ps.img[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(ps.img[i]);
    }
    if (mini) lv_obj_move_foreground(mini);
    lv_obj_set_x(p, W);
    lv_obj_set_x(below, -W);
    ps_back = true;
    anim_x(ps.img[1], x, W, ms, ps_pop_done_cb);
    anim_x(ps.img[0], -W * 3 / 10 + x * 3 / 10, 0, ms, NULL);
    return true;
}

static bool page_push_quiet;            /* the next push appears in place */
/* Settings tiles: the colour the page a tile opens wears, on it before it
 * slides in (the slide is a snapshot taken here), so it never pops in after */
static uint32_t page_wash_next;          /* 0: none; else the tint | 1 << 24 */
static void set_wash(lv_obj_t *p, uint32_t tint);
static void page_title_tx(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void page_push(lv_obj_t *p)
{
    if (npages >= MAX_PAGES)
    {
        lv_obj_delete(p);
        return;
    }
    ps_end();                            /* one still sliding: settled first */
    lv_obj_t *below = npages ? pages[npages - 1] : NULL;
    pages[npages++] = p;
    if (page_wash_next) set_wash(p, page_wash_next & 0xffffff);
    page_wash_next = 0;
    if (sb_ink_pending)
    {
        sb_ink_page = p;
        sb_ink_pending = false;
        lv_obj_add_event_cb(p, sb_ink_forget_cb, LV_EVENT_DELETE, NULL);   /* its address may come back as another page */
    }
    statusbar_retheme();                 /* off the wallpaper; ink for the page's own background */
    lv_obj_move_foreground(p);
    if (mini)
        lv_obj_move_foreground(mini);
    if (mini && mini_held_off()) lv_obj_add_flag(mini, LV_OBJ_FLAG_HIDDEN);   /* at once: a keyboard or a game's controls hold the bottom edge */
    if (!below || page_push_quiet) ui_reveal_page(p);
    if (below && page_push_quiet)
    {
        /* opened by its own animation (the Settings search bubble): no slide */
        page_push_quiet = false;
        lv_anim_delete(below, NULL);
        lv_obj_set_x(p, 0);
        lv_obj_set_x(below, -W * 3 / 10);
        lv_obj_add_flag(below, LV_OBJ_FLAG_HIDDEN);
    }
    else if (below)
    {
        sfx_play(SFX_UI_OPEN);
        lv_anim_delete(below, NULL);
        nav_guard_contact();
        if (ps_slide(p, below)) return;
        ui_reveal_page(p);
        anim_x(p, W, 0, 220, NULL);
        anim_x(below, lv_obj_get_x(below), -W * 3 / 10, 220, page_hide_cb);
        if (sl_zune())
        {
            /* the Zune's parallax: the big title trails the page in and
             * settles last (a translation: no redraw of anything else) */
            lv_obj_t *hdr = lv_obj_get_child(p, 0);
            lv_obj_t *title = hdr ? lv_obj_get_child(hdr, -1) : NULL;
            if (title && lv_obj_check_type(title, &lv_label_class))
            {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, title);
                lv_anim_set_values(&a, 120, 0);
                lv_anim_set_duration(&a, SL_T_ENTER);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                lv_anim_set_exec_cb(&a, page_title_tx);
                lv_obj_set_style_translate_x(title, 120, 0);
                lv_anim_start(&a);
            }
        }
    }
}
static void page_replace(lv_obj_t *p, bool forward)
{
    if (!npages) { page_push(p); return; }
    lv_obj_t *old = pages[npages - 1];
    pages[npages - 1] = p;
    ui_reveal_page(p);
    lv_obj_move_foreground(p);
    if (mini) lv_obj_move_foreground(mini);
    nav_guard_contact();
    lv_anim_delete(old, NULL);
    anim_x(p, forward ? W : -W, 0, 200, NULL);
    anim_x(old, lv_obj_get_x(old), forward ? -W : W, 200, page_delete_cb);
}
static void page_pop_from(int x)
{
    if (pet_care_locked) return;
    if (npages < 2)
        return;
    ps_end();
    lv_obj_t *p = pages[--npages];
    lv_obj_t *below = pages[npages - 1];
    statusbar_retheme();                 /* back on the wallpaper, or the page under it */
    sfx_play(SFX_UI_BACK);
    if (x < 0) x = 0;
    if (x > W) x = W;
    int ms = 80 + 140 * (W - x) / W;
    nav_guard_contact();
    lv_anim_delete(p, NULL);
    lv_anim_delete(below, NULL);
    lv_obj_remove_flag(below, LV_OBJ_FLAG_HIDDEN);
    if (ps_slide_back(p, below, x, ms)) return;
    anim_x(p, x, W, ms, page_delete_cb);
    anim_x(below, -W * 3 / 10 + x * 3 / 10, 0, ms, NULL);
}
static void page_pop(void) { page_pop_from(0); }

/* Pages that keep their objects in globals (the games, the transfer
 * page) must exist once. Opened again while one is further down the
 * stack, go back to it: a second copy shared the globals, and closing it
 * left the first one's controls on freed or cleared objects (a crash on
 * the next tap, found by the fuzzer). Returns true when p was open. */
static bool page_bring_back(lv_obj_t *p)
{
    if (!p) return false;
    int i = npages - 1;
    while (i >= 0 && pages[i] != p) i--;
    if (i < 0) return false;
    if (i == npages - 1 || pet_care_locked) return true;
    while (npages - 1 > i + 1)
    {
        lv_obj_t *top = pages[--npages];
        lv_anim_delete(top, NULL);
        lv_obj_delete(top);
    }
    page_pop();                 /* the last page above it slides away */
    return true;
}

/* An album's or an artist's page, marked as such (its user data): "Go to
 * album" from a song of the album on show closes the sheet, rather than
 * opening the page again over itself. */
#define PG_ALBUM   (1 << 24)
#define PG_ARTIST  (2 << 24)
#define PG_TARTIST (3 << 24)
static bool page_top_is(intptr_t key)
{
    return npages && (intptr_t)lv_obj_get_user_data(pages[npages - 1]) == key;
}
/* Artists, Genres and a genre's albums: their rows are places in the
 * artists and genres one by one (sloppy_tags.inc) */
#define PG_TAGS    (4 << 24)
/* Split on or Keep together changed: those lists are made again (a moment
 * later, or when next asked for), so the pages built on the old ones, an
 * artist's too, go; they are under Settings, never home or on top */
static void tag_pages_drop(void)
{
    int k = 0;
    for (int i = 0; i < npages; i++)
    {
        intptr_t u = (intptr_t)lv_obj_get_user_data(pages[i]);
        if (i && i < npages - 1 && (u == PG_TAGS || (u & ~0xffffff) == PG_TARTIST))
        {
            lv_anim_delete(pages[i], NULL);
            lv_obj_delete(pages[i]);
        }
        else pages[k++] = pages[i];
    }
    npages = k;
}

static void back_drag(int dx)
{
    if (pet_care_locked) return;
    if (npages < 2) return;
    if (dx < 0) dx = 0;
    if (dx > W) dx = W;
    lv_obj_t *p = pages[npages - 1], *below = pages[npages - 2];
    lv_anim_delete(p, NULL);
    lv_anim_delete(below, NULL);
    lv_obj_remove_flag(below, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(p, dx);
    lv_obj_set_x(below, -W * 3 / 10 + dx * 3 / 10);
}
static void back_release(int dx, int vx)
{
    if (pet_care_locked) return;
    if (npages < 2) return;
    if (dx < 0) dx = 0;
    if (dx > W) dx = W;
    if (dx > W * 3 / 10 || (vx > 450 && dx > 30))
        page_pop_from(dx);
    else
    {
        lv_obj_t *p = pages[npages - 1], *below = pages[npages - 2];
        nav_guard_contact();
        anim_x(p, dx, 0, 180, NULL);
        anim_x(below, lv_obj_get_x(below), -W * 3 / 10, 180, page_hide_cb);
    }
}

static void back_cb(lv_event_t *e) { (void)e; page_pop(); }

static bool cf_is_open(void);
static void cf_close(void);
/* A new full-screen page with a header; returns the scrolling body.
 * Cover Flow runs on a screen of its own and deletes it on the way out:
 * a page opened over it (the quick panel sits on the top layer) would be
 * built there, freed with it, and left dangling in pages[]. */
static lv_obj_t *page_new(const char *title, lv_obj_t **page_out)
{
    if (cf_is_open()) cf_close();
    lv_obj_t *p = box(lv_screen_active());
    lv_obj_add_flag(p, LV_OBJ_FLAG_USER_2);
    lv_obj_add_style(p, &st_page, 0);
    lv_obj_set_size(p, W, H);

    lv_obj_t *hdr = box(p);
    lv_obj_set_size(hdr, W, SL_HDR_H);
    lv_obj_set_pos(hdr, 0, SB_H);
    lv_obj_t *b = NULL;
    if (npages > 0)
    {
        b = box(hdr);
        lv_obj_set_size(b, BACK_D, BACK_D);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, BACK_X, 0);
        lv_obj_set_style_radius(b, SL_R_PILL, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_t *ic = text(b, LV_SYMBOL_LEFT, &sl_sym_28, 0xffffff);
        lv_obj_center(ic);
        on_click(b, back_cb, NULL);
    }
    if (sl_zune())
    {
        /* the Zune's pivot title: big, light, lowercase, running off the
         * edge. Same 66 px header (games are laid out under it): the
         * face's top bearing reaches up into the clear status bar strip. */
        char low[96];
        const char *ls = sl_lower(sl_tr(title), low, sizeof low);
        const lv_font_t *f = sl_zune_font(ls, &ss_regular_104);
        lv_obj_t *t = lv_label_create(hdr);
        lv_obj_set_style_base_dir(t, LV_BASE_DIR_AUTO, 0);
        lv_label_set_text(t, ls);
        lv_obj_set_style_text_font(t, f, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_CLIP);
        int x = npages > 0 ? 58 : 14;
        lv_obj_set_width(t, W - x);
        lv_obj_set_height(t, lv_font_get_line_height(f));
        /* the Zune HD's pivot title: so big it is cut off, the bodies of
         * the letters in the header and their tops lost over it */
        if (f == &ss_regular_104)
        {
            lv_obj_set_pos(t, x - 6, 62 - (lv_font_get_line_height(f) - f->base_line));
            /* the back chevron as the title's own mark: drawn as tall as
             * its x-height, standing on its baseline (62), in a stroke
             * lighter than the letters so the word stays the voice */
            if (b && ls[0])
            {
                static const lv_point_precise_t chev[] = { { 20, 3 }, { 3, 22 }, { 20, 41 } };
                lv_obj_clean(b);
                lv_obj_align(b, LV_ALIGN_TOP_LEFT, BACK_X, SL_HDR_H - BACK_D);
                lv_obj_t *ln = lv_line_create(b);
                lv_line_set_points(ln, chev, 3);
                lv_obj_set_style_line_width(ln, 6, 0);
                lv_obj_set_style_line_rounded(ln, true, 0);
                lv_obj_set_style_line_color(ln, lv_color_hex(C_TEXT), 0);
                lv_obj_align(ln, LV_ALIGN_TOP_MID, 0, 62 - (SL_HDR_H - BACK_D) - 44);
            }
        }
        else
        {
            /* letters the big face lacks: the regular title, on one line,
             * ending in an ellipsis when long (a file's name in a question) */
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_align(t, LV_ALIGN_LEFT_MID, x, 0);
        }
    }
    else
    {
        lv_obj_t *t = text(hdr, title, &ss_semibold_30, 0xffffff);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, npages > 0 ? 66 : 20, 0);
        lv_obj_set_width(t, W - 90);
        label_lines(t, 1);
        if (sl_ui_rtl()) lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0);   /* a Latin name too, from the reading edge */
    }

    lv_obj_t *body = lv_obj_create(p);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, SB_H + SL_HDR_H);
    lv_obj_set_size(body, W, H - SB_H - SL_HDR_H);
    lv_obj_set_style_pad_hor(body, SL_GUTTER, 0);
    lv_obj_set_style_pad_top(body, SL_SP_XS, 0);
    lv_obj_set_style_pad_bottom(body, 110, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 2, 0);
    if (page_out)
        *page_out = p;
    return body;
}
/* A long list's place in it: a slim bar down the right edge in the
 * theme's quiet grey, stopping above the mini player */
static void sl_scroll_indicator(lv_obj_t *body)
{
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(body, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(body, SL_R_FULL, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(body, lv_color_hex(C_DIM), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(body, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(body, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(body, SL_SP_S, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(body, 82 + 2 * SL_FLOAT, LV_PART_SCROLLBAR);
}
/* A round button at the right of a page's header, the size of its back
 * button; a second one sits to the left of the first. */
/* A page's title stops short of what sits at the right of its header (x):
 * a long one, or one set right to left (it hugs the right edge), ran under
 * the buttons. Zule's 104 px pivot title runs off the edge on purpose. */
static void header_title_clear(lv_obj_t *page, int x)
{
    lv_obj_t *hdr = lv_obj_get_child(page, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(hdr); i++)
    {
        lv_obj_t *t = lv_obj_get_child(hdr, i);
        if (!lv_obj_check_type(t, &lv_label_class) || lv_obj_get_style_text_font(t, LV_PART_MAIN) == &ss_regular_104) continue;
        lv_obj_update_layout(hdr);
        int room = x - lv_obj_get_x(t) - SL_SP_S;
        if (room > 0 && lv_obj_get_width(t) > room) lv_obj_set_width(t, room);
    }
}
static lv_obj_t *header_button(lv_obj_t *page, const char *sym, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *hdr = lv_obj_get_child(page, 0);
    int n = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(hdr); i++)
        if (lv_obj_has_flag(lv_obj_get_child(hdr, i), LV_OBJ_FLAG_USER_4)) n++;
    lv_obj_t *b = box(hdr);
    lv_obj_add_flag(b, LV_OBJ_FLAG_USER_4);   /* marks a header button */
    lv_obj_set_size(b, 56, 56);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -6 - n * 58, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_transition(b, &press_out, 0);
    lv_obj_set_style_transition(b, &press_in, LV_STATE_PRESSED);
    lv_obj_center(text(b, sym, &sl_sym_24, 0xffffff));
    on_click(b, cb, ud);
    lv_obj_update_layout(hdr);
    header_title_clear(page, lv_obj_get_x(b));
    return b;
}

/* ------------------------------------------------------------- toast */
#include "sloppy_notify.inc"
static void sl_keys_note(const char *msg) { notify_ex(NS_SYSTEM, NOTE_INFO, 0, NA_NONE, "keylock", msg); }

#include "sloppy_appearance.inc"
#include "sloppy_glass.inc"

/* What a song is called wherever the player shows it: its title, or its
 * file name (no folder, no extension) when it has no title tag. Untagged
 * songs used to show a blank line in some places and "Unknown song" in
 * others. */
/* A cue sheet's track playing inside its file (an album in one file):
 * the song, and where in the file it starts and how long it is. -1 (the
 * whole file) for any other. */
static int cue_now(const struct mp3entry *id3, long *start, long *len)
{
    int t = id3 && id3->path[0] ? sl_lib_find_path_at(id3->path, (long)id3->elapsed) : -1;
    if (t < 0 || !sl_lib.tracks[t].cue)
    {
        if (start) *start = 0;
        if (len) *len = id3 ? (long)id3->length : 0;
        return -1;
    }
    if (start) *start = sl_lib.tracks[t].start;
    if (len) *len = sl_lib.tracks[t].len;
    return t;
}
static const char *track_title(const struct mp3entry *id3, char *buf, size_t n)
{
    int cue = cue_now(id3, NULL, NULL);
    if (cue >= 0) return sl_lib.tracks[cue].title;   /* the sheet's track, not the file's tag */
    if (id3->title && id3->title[0]) return id3->title;
    const char *name = strrchr(id3->path, '/');
    strlcpy(buf, name ? name + 1 : id3->path, n);
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf) *dot = '\0';
    return buf[0] ? buf : "Unknown song";
}
/* Who sings it: the song's own artist first (a compilation's tracks are
 * not all "Various Artists"), its album's when it has none */
static const char *track_artist(const struct mp3entry *id3)
{
    int cue = cue_now(id3, NULL, NULL);
    if (cue >= 0) return sl_track_artist(cue);
    if (id3->artist && id3->artist[0]) return id3->artist;
    if (id3->albumartist && id3->albumartist[0]) return id3->albumartist;
    return "Unknown artist";
}

/* Which album: a cue sheet's, the tag's, or the library's for the file;
 * nothing when none says */
static const char *track_album(const struct mp3entry *id3)
{
    int cue = cue_now(id3, NULL, NULL);
    int t = cue >= 0 ? cue : -1;
    if (t < 0 && id3->album && id3->album[0]) return id3->album;
    if (t < 0) t = sl_lib_find_path(id3->path);
    if (t >= 0 && t < sl_lib.n_tracks && sl_lib.tracks[t].album >= 0 && sl_lib.tracks[t].album < sl_lib.n_albums)
        return sl_lib.albums[sl_lib.tracks[t].album].title;
    return "";
}

/* --------------------------------------------------------- now playing */
static lv_obj_t *np_zn;                    /* Zune: the artist, huge and faint, behind the song */
static lv_obj_t *np_scrim;                 /* the shade under the status bar, when it shows */
static bool np_status_on(void) { return global_settings.sloppy_np_status == 1; }   /* #112 */
static lv_obj_t *np_album;                 /* the album, under the artist (Customization > Now Playing, R2) */
static bool np_album_on(void) { return global_settings.sloppy_np_album == 1; }
static lv_obj_t *np, *np_art, *np_art_img, *np_art_none, *np_title, *np_artist, *np_fmt, *np_seek,
                *np_t0, *np_t1, *np_pp, *np_shuffle, *np_repeat, *np_queue, *np_vol, *np_vol_lo, *np_vol_hi, *np_love, *np_more;
static int np_love_track = -1;             /* the song the heart shows, or none */
static bool np_open;
static int aa_slot = -1;
static lv_image_dsc_t np_img;
static uint16_t *np_img_buf;
static int np_img_album = -1;
static bool np_img_provisional;
static char np_last_path[MAX_PATH];
/* sloppy_npwave.inc: the waveform seek bar (P19) */
#define WV_H 28                          /* its tallest bar, and the control's box */
static bool np_wave(void);
static void wv_apply(void);
static void wv_progress(int prog);
static void wv_draw_cb(lv_event_t *e);
static void wv_poll(void);
#include "sloppy_npglass.inc"

static void np_anim_y(int from, int to, int ms, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, np);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a, lv_anim_path_custom_bezier3);
    a.parameter.bezier3 = (lv_anim_bezier3_para_t){ LV_BEZIER_VAL_FLOAT(0.32), LV_BEZIER_VAL_FLOAT(0.72),
                                                    LV_BEZIER_VAL_FLOAT(0.0), LV_BEZIER_VAL_FLOAT(1.0) };
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
static void np_hidden_cb(lv_anim_t *a) { (void)a; lv_obj_add_flag(np, LV_OBJ_FLAG_HIDDEN); }
static void statusbar_retheme(void);
static void np_shown_cb(lv_anim_t *a) { (void)a; statusbar_retheme(); }
static void np_skip(int side);
static bool nps_drag(lv_event_code_t c, lv_point_t p);
static void nps_release(void);
static void nps_build(lv_obj_t *parent);
static void nps_track_changed(void);
static bool nps_pending(void);
static void statusbar_show(bool on);
static void np_update(bool force);
static void song_lists_mark(int was, int now);
static uint32_t np_ink(void);
static lv_obj_t *vnl;
static bool vnl_on(void);
static void vnl_apply(lv_obj_t *np_parent, bool drop);
static void vnl_wake(void);
static void vz_build_button(lv_obj_t *parent);
static void np_show(void)
{
    if (np_open) return;
    sl_log("now playing open");
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return;
    np_open = true;
    if (vnl_on() && !vnl) vnl_apply(np, false);   /* built on first use: it costs 0.5 MB */
    vnl_wake();
    np_update(true);
    if (!np_glow_buf || (np_centred() && !nc_bd_px)) np_glow_refresh(true);   /* given back while it was shut */
    lv_obj_remove_flag(np, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(np);
    lv_obj_add_flag(mini, LV_OBJ_FLAG_HIDDEN);
    statusbar_show(np_status_on());       /* over the cover when asked (#112), white once it is up */
    np_anim_y(H, 0, 460, np_shown_cb);
}
/* after a song is picked: Now Playing, unless the user stays in the list */
static void np_show_auto(void)
{
    if (global_settings.sloppy_auto_np) np_show();
}
static void np_hide(int from)
{
    np_open = false;
    statusbar_retheme();                 /* the page's ink again, not the cover's white */
    nps_release();
    np_anim_y(from, H, 380, np_hidden_cb);
    statusbar_show(true);
    mini_update();   /* back, unless the page under it keeps the bottom (a keyboard, picking) */
}

/* The heart: the accent when the song is loved, the icon grey otherwise. */
static void np_love_paint(void)
{
    bool loved = np_love_track >= 0 && ts_rating(np_love_track) == 1;
    lv_obj_t *heart = lv_obj_get_child(np_love, 0);
    lv_obj_set_style_text_color(heart, lv_color_hex(loved ? np_ink() : np_icon_color()), 0);
    lv_obj_set_style_opa(heart, loved ? LV_OPA_COVER : LV_OPA_60, 0);
    hidden_if_changed(np_love, np_love_track < 0);
    for (uint32_t i = 0; np_more && i < lv_obj_get_child_count(np_more); i++)   /* its dots in the heart's grey */
    {
        lv_obj_t *d = lv_obj_get_child(np_more, (int32_t)i);
        lv_obj_set_style_bg_color(d, lv_color_hex(np_icon_color()), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_60, 0);
    }
    if (np_more) hidden_if_changed(np_more, np_love_track < 0);
}
/* An audiobook playing: back and ahead are 30 seconds, not a chapter */
bool audiobooks_is_book(const char *path);
static lv_obj_t *np_prev_btn, *np_next_btn;
static bool np_book;
static void np_book_buttons(bool book)
{
    if (!np_prev_btn || book == np_book) return;
    np_book = book;
    lv_obj_t *p = lv_obj_get_child(np_prev_btn, 0), *n = lv_obj_get_child(np_next_btn, 0);
    lv_label_set_text(p, book ? "-30" : LV_SYMBOL_PREV);
    lv_label_set_text(n, book ? "+30" : LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(p, book ? &ss_semibold_26 : &sl_sym_40, 0);
    lv_obj_set_style_text_font(n, book ? &ss_semibold_26 : &sl_sym_40, 0);
}
static void np_book_skip(int side)
{
    struct mp3entry *id3 = audio_current_track();
    if (!id3) return;
    long to = (long)id3->elapsed + side * 30000L;
    if (to < 0) to = 0;
    if (id3->length && to > (long)id3->length - 1000) { audio_next(); return; }   /* past the chapter's end: the next */
    if (!(audio_status() & AUDIO_STATUS_PAUSE)) audio_pre_ff_rewind();
    audio_ff_rewind(to);
}
/* Now Playing's repeat and shuffle buttons, and the phone remote's */
static void np_cycle_repeat(void)
{
    global_settings.repeat_mode = global_settings.repeat_mode == REPEAT_OFF ? REPEAT_ALL :
                                  global_settings.repeat_mode == REPEAT_ALL ? REPEAT_ONE : REPEAT_OFF;
    audio_flush_and_reload_tracks();
    settings_save();
    notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "repeat", global_settings.repeat_mode == REPEAT_ONE ? "Repeat one" :
              global_settings.repeat_mode == REPEAT_ALL ? "Repeat all" : "Repeat off");
    np_update(false);
}
static void np_toggle_shuffle(void)
{
    sl_set_shuffle(!global_settings.playlist_shuffle);
    settings_save();
    notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "shuffle", global_settings.playlist_shuffle ? "Shuffle on" : "Shuffle off");
    np_update(false);
}
static void np_btn_cb(lv_event_t *e)
{
    intptr_t k = (intptr_t)lv_event_get_user_data(e);
    switch (k)
    {
        case 0: np_hide(0); break;
        case 1: if (np_book) np_book_skip(-1); else np_skip(-1); break;
        case 2: toggle_play(); break;
        case 3: if (np_book) np_book_skip(1); else np_skip(1); break;
        case 5: uq_show(); break;
        case 8: if (np_love_track >= 0) acts_open_np(np_love_track); break;
        case 6:
            if (np_love_track >= 0)
            {
                bool love = ts_rating(np_love_track) != 1;
                ts_love(np_love_track, love);
                sfx_play(love ? SFX_UI_ON : SFX_UI_OFF);
                np_love_paint();
            }
            break;
        case 7: np_cycle_repeat(); break;
        case 4: np_toggle_shuffle(); break;
    }
}
static char np_seek_path[MAX_PATH];
static int np_seek_index;
static void np_seek_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    struct mp3entry *id3 = audio_current_track();
    if (c == LV_EVENT_PRESSED)
    {
        np_seek_path[0] = '\0';
        if (id3 && id3->length)
        {
            strlcpy(np_seek_path, id3->path, sizeof np_seek_path);
            np_seek_index = q_cur();
        }
    }
    else if (c == LV_EVENT_RELEASED)
    {
        if (id3 && np_seek_path[0] && np_seek_index == q_cur() &&
            !strcmp(np_seek_path, id3->path) && id3->length)
        {
            long cs, cl;
            cue_now(id3, &cs, &cl);
            long ms = cs + (long)((uint64_t)lv_slider_get_value(np_seek) * cl / 1000);
            if (!(audio_status() & AUDIO_STATUS_PAUSE))
                audio_pre_ff_rewind();
            audio_ff_rewind(ms);
        }
        np_seek_path[0] = '\0';
    }
    else if (c == LV_EVENT_PRESS_LOST)
        np_seek_path[0] = '\0';
    else if (c == LV_EVENT_VALUE_CHANGED && id3 && np_seek_path[0] &&
             !strcmp(np_seek_path, id3->path))
    {
        char b[16];
        long cl;
        cue_now(id3, NULL, &cl);
        fmt_time(b, sizeof b, (long)((int64_t)lv_slider_get_value(np_seek) * cl / 1000));
        lv_label_set_text(np_t0, b);
    }
}

/* swipe the Now Playing sheet down to close it */
static int np_drag_y0;
static void np_drag_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p;
    if (!in) return;
    lv_indev_get_point(in, &p);
    if (nps_drag(c, p)) return;          /* a swipe of the cover, not a pull */
    if (c == LV_EVENT_PRESSED)
        np_drag_y0 = p.y;
    else if (c == LV_EVENT_PRESSING)
    {
        int dy = p.y - np_drag_y0;
        if (dy > 0) { lv_anim_delete(np, NULL); lv_obj_set_y(np, dy); }
    }
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST)
    {
        int dy = p.y - np_drag_y0;
        if (dy < -80) uq_show();
        else if (dy > 140) np_hide(dy);
        else if (dy > 0) np_anim_y(dy, 0, 300, NULL);
    }
}

/* the small round buttons' icons: pale on the dark themes, ink on the light
 * one, white on the glass */
static uint32_t np_icon_color(void) { return np_glass() || np_centred() || sl_zune() ? 0xffffff : sl_theme() == 1 ? 0x182735 : 0xbfe3ff; }
/* A Now Playing control: size is what a finger may hit, glyph how big it
 * is drawn. Only play and pause has a disc, as on the stock player; the
 * others are bare glyphs, a soft disc showing under the finger. */
static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, int size, const lv_font_t *glyph,
                           bool primary, intptr_t k)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, SL_R_FULL, 0);
    if (primary)
    {
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xdfe8f2), LV_STATE_PRESSED);
    }
    else
    {
        lv_obj_set_style_bg_color(b, lv_color_hex(0xffffff), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    }
    press_motion(b);
    lv_obj_set_style_transform_scale(b, 230, LV_STATE_PRESSED);   /* gives a little under the finger */
    lv_obj_set_style_transform_pivot_x(b, size / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, size / 2, 0);
    lv_obj_t *l = text(b, sym, glyph, primary ? sl_accent_on(0xffffff, 30) : np_icon_color());
    lv_obj_center(l);
    on_click(b, np_btn_cb, (void *)k);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(b, 10);    /* a near miss still lands */
    return b;
}

static void now_playing_retheme(void);
#include "sloppy_vinyl.inc"

/* Now Playing's sliders: a thin rounded line h thick, a small knob that
 * swells by grow under the finger; a generous strip to touch. The
 * colours are np_paint's. */
/* Now Playing's sliders: thin lines over the art, with the family's knob */
#define NP_TRACK_H 6
static void np_slider_style(lv_obj_t *s, int w)
{
    lv_obj_set_size(s, w, NP_TRACK_H);
    sl_slider_knob(s, NP_TRACK_H, false);
}

static int volume_pct(void);
static int volume_from_pct(int pct);
static void statusbar_update(void);
static int np_vol_seen = -100000;         /* the volume the slider last showed or set; none yet */
static void np_vol_cb(lv_event_t *e)
{
    (void)e;
    global_status.volume = volume_from_pct(lv_slider_get_value(np_vol));
    np_vol_seen = global_status.volume;   /* its own drag: no snapping to the rounded step after */
    setvol();
    statusbar_update();
}

static bool sr_on(void);
static void lyr_build(lv_obj_t *parent);
static void lyr_track(const char *path);
static void np_paint(void);
static void np_build(void)
{
    np = box(ui_screen());
    lv_obj_set_size(np, W, H);
    lv_obj_set_style_bg_color(np, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(np, LV_OPA_COVER, 0);
    lv_obj_add_flag(np, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(np, np_drag_cb, LV_EVENT_ALL, NULL);

    np_art = box(np);
    lv_obj_set_size(np_art, W, W);
    lv_obj_set_style_bg_opa(np_art, LV_OPA_COVER, 0);
    np_art_img = lv_image_create(np_art);
    lv_obj_center(np_art_img);
    lv_obj_add_flag(np_art_img, LV_OBJ_FLAG_HIDDEN);
    /* no cover: a record on the album's own colour, not an empty square */
    np_art_none = sl_icon(np_art, IC_ALBUMS, 176, 0xffffff, 0x000000);
    lv_obj_center(np_art_none);
    lv_obj_add_flag(np_art_none, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(np_art, LV_OBJ_FLAG_EVENT_BUBBLE);
    nps_build(np);                           /* the neighbour's cover, beside it */
    np_glow = lv_image_create(np);           /* the glass: under everything but the cover */
    lv_obj_set_pos(np_glow, 0, NG_TOP);
    lv_obj_add_flag(np_glow, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* the status bar over the cover (#112): a shade under it, so it and
     * the buttons read on a pale cover */
    np_scrim = box(np);
    lv_obj_set_size(np_scrim, W, 120);
    lv_obj_set_style_bg_color(np_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(np_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(np_scrim, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(np_scrim, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(np_scrim, 150, 0);
    lv_obj_set_style_bg_grad_opa(np_scrim, 0, 0);
    lv_obj_add_flag(np_scrim, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(np_scrim, LV_OBJ_FLAG_CLICKABLE);
    lyr_build(np);

    lv_obj_t *down = np_down = box(np);
    lv_obj_set_size(down, 56, 56);
    lv_obj_set_pos(down, 8, 10);
    lv_obj_set_style_radius(down, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(down, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(down, LV_OPA_30, 0);
    lv_obj_t *chev = text(down, LV_SYMBOL_DOWN, &sl_sym_24, 0xffffff);
    lv_obj_set_style_text_color(chev, lv_color_white(), 0);   /* on the art: white in every theme */
    lv_obj_center(chev);
    lv_obj_set_style_bg_opa(down, LV_OPA_50, LV_STATE_PRESSED);
    on_click(down, np_btn_cb, (void *)0);
    lv_obj_remove_flag(down, LV_OBJ_FLAG_EVENT_BUBBLE);
    vnl_build_button(np);
    vz_build_button(np);
    /* the queue: a view, as the lyrics are, beside their button (np_queue_place),
     * so the row below is playback alone. Plain as the Lyrics pill: no lens
     * (a fourth cost every cover change 10 % more) */
    np_queue = box(np);
    lv_obj_set_size(np_queue, 56, 56);
    lv_obj_set_pos(np_queue, W - 8 - 56, 10);
    lv_obj_set_style_radius(np_queue, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(np_queue, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(np_queue, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(np_queue, LV_OPA_50, LV_STATE_PRESSED);
    /* its own glyph (N1): lines with the next one coming, as the Up next
     * tile; the list glyph means Songs and Add to queue elsewhere */
    lv_obj_center(sl_icon(np_queue, IC_UPNEXT, 28, 0xffffff, 0x000000));
    on_click(np_queue, np_btn_cb, (void *)5);
    lv_obj_remove_flag(np_queue, LV_OBJ_FLAG_EVENT_BUBBLE);

    int y = W + 14;
    /* Zune HD's Now Playing set the artist across the screen, far bigger
     * than anything else and barely there; the song reads over it. Still,
     * so it costs nothing per frame. */
    np_zn = lv_label_create(np);
    lv_obj_set_style_text_font(np_zn, &ss_regular_60, 0);
    lv_obj_set_style_text_color(np_zn, lv_color_white(), 0);
    lv_obj_set_style_text_opa(np_zn, 34, 0);
    lv_label_set_long_mode(np_zn, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(np_zn, W + 8, lv_font_get_line_height(&ss_regular_60));
    lv_obj_set_pos(np_zn, -4, y - 10);   /* all of it under the cover, the song over it */
    lv_label_set_text(np_zn, "");
    lv_obj_add_flag(np_zn, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
    np_title = text(np, "", &ss_semibold_30, 0xffffff);
    lv_obj_set_pos(np_title, 24, y);
    lv_obj_set_width(np_title, W - 48 - 94);   /* up to the heart and the dots */
    lv_label_set_long_mode(np_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    /* love it: a heart at the end of the title line, lit when the song is
     * in Loved (the Taste swipes' yes pile); a tap puts it there or takes
     * it out again */
    np_love = box(np);
    lv_obj_set_size(np_love, 48, 48);
    lv_obj_set_pos(np_love, W - 62 - 48, y - 4);
    lv_obj_set_style_radius(np_love, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(np_love, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(np_love, LV_OPA_20, LV_STATE_PRESSED);
    press_motion(np_love);
    lv_obj_set_style_transform_scale(np_love, 230, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(np_love, 24, 0);
    lv_obj_set_style_transform_pivot_y(np_love, 24, 0);
    lv_obj_center(sl_icon(np_love, IC_HEART, 26, 0xffffff, 0x000000));
    lv_obj_set_ext_click_area(np_love, 8);
    on_click(np_love, np_btn_cb, (void *)6);
    lv_obj_remove_flag(np_love, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* more (P22): the song's sheet, as its row's dots open it, with
     * Playback's settings at its end */
    np_more = icon_btn(np, NULL, 48, np_btn_cb, (void *)8);
    lv_obj_set_pos(np_more, W - 14 - 48, y - 4);
    np_artist = text(np, "", &ss_semibold_23, 0xeef1f7);
    lv_obj_set_pos(np_artist, 24, y + 38);
    lv_obj_set_width(np_artist, W - 48);   /* the whole line: long and right-to-left names */
    label_lines(np_artist, 1);
    /* the album, when asked for: a quieter line under the artist; the
     * song's lines close up by 2 and what is under them moves down 12 */
    np_album = text(np, "", &ss_regular_18, 0xeef1f7);
    lv_obj_set_pos(np_album, 24, y + 64);
    lv_obj_set_width(np_album, W - 48);
    label_lines(np_album, 1);

    /* where in the song: a thick rounded track, its knob swelling under
     * the finger; a release seeks, so dragging never jumps the audio */
    np_seek = lv_slider_create(np);
    np_slider_style(np_seek, W - 48);
    lv_obj_set_pos(np_seek, 24, y + 86);
    lv_slider_set_range(np_seek, 0, 1000);
    lv_obj_add_event_cb(np_seek, np_seek_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(np_seek, np_seek_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(np_seek, np_seek_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(np_seek, np_seek_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(np_seek, wv_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    np_t0 = text(np, "0:00", &ss_regular_18, 0xeef1f7);
    lv_obj_set_pos(np_t0, 24, y + 104);
    /* between the times: the format, the bit rate and where in the list,
     * each only when known */
    np_fmt = text(np, "", &ss_regular_18, 0xeef1f7);
    lv_obj_set_width(np_fmt, W - 210);
    lv_obj_set_style_text_align(np_fmt, LV_TEXT_ALIGN_CENTER, 0);
    label_lines(np_fmt, 1);
    lv_obj_align(np_fmt, LV_ALIGN_TOP_MID, 0, y + 104);
    np_t1 = text(np, "0:00", &ss_regular_18, 0xeef1f7);
    lv_obj_align(np_t1, LV_ALIGN_TOP_RIGHT, -24, y + 104);

    /* the controls: prev and next big and bare either side of the one
     * disc, play and pause; shuffle and the queue small at the ends */
    lv_obj_t *row = box(np);
    lv_obj_set_size(row, W - 40, 96);
    lv_obj_set_pos(row, 20, y + 136);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    np_shuffle = round_btn(row, LV_SYMBOL_SHUFFLE, 56, &sl_sym_24, false, 4);
    np_prev_btn = round_btn(row, LV_SYMBOL_PREV, 76, &sl_sym_40, false, 1);
    np_pp = round_btn(row, LV_SYMBOL_PLAY, 84, &sl_sym_40, true, 2);
    np_next_btn = round_btn(row, LV_SYMBOL_NEXT, 76, &sl_sym_40, false, 3);
    /* repeat mirrors shuffle across the transport (#94): off, all, one;
     * one wears a small "1" on the glyph */
    np_repeat = round_btn(row, LV_SYMBOL_LOOP, 56, &sl_sym_24, false, 7);
    lv_obj_t *one = box(np_repeat);
    lv_obj_set_size(one, 20, 20);
    lv_obj_set_pos(one, 33, 5);
    lv_obj_set_style_radius(one, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(one, LV_OPA_COVER, 0);
    lv_obj_t *one_l = text(one, "1", &ss_semibold_18, 0xffffff);
    lv_obj_center(one_l);
    lv_obj_add_flag(one, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);

    /* loudness, between a quiet and a loud speaker */
    /* each in a box of one width, set in from the edges as the seek bar is,
     * the same gap to the track either side (N2: they sat at 24 and 20) */
    np_vol_lo = text(np, LV_SYMBOL_VOLUME_MID, &sl_sym_20, 0xffffff);
    np_vol_hi = text(np, LV_SYMBOL_VOLUME_MAX, &sl_sym_20, 0xffffff);
    for (int k = 0; k < 2; k++)
    {
        lv_obj_t *v = k ? np_vol_hi : np_vol_lo;
        lv_obj_set_width(v, 28);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(v, k ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, k ? -24 : 24, y + 240);
    }
    np_vol = lv_slider_create(np);
    np_slider_style(np_vol, W - 128);
    lv_obj_set_pos(np_vol, 64, y + 248);
    lv_slider_set_range(np_vol, 0, 100);
    lv_obj_add_event_cb(np_vol, np_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    np_paint();   /* a light or true-dark theme from boot, not only after a change */
}

static void hidden_if_changed(lv_obj_t *o, bool hidden);
/* Where things sit for the layout chosen: the square cover with the song
 * under it, or Centred's smaller cover with all of that a little higher;
 * the buttons over the cover lower while the status bar shows (#112). Each
 * one's own place is read once, at the first call, and moved from there. */
static lv_obj_t *lyr_btn;
static bool lyr_visible(void);
#define LYR_TOP0 70                      /* the lyric lines' top, under the buttons */
static void lyr_layout(int top, int bottom);
static void np_layout(void)
{
    lv_obj_t *below[] = { np_zn, np_title, np_love, np_artist, np_album, np_seek, np_t0, np_fmt, np_t1,
                          lv_obj_get_parent(np_pp), np_vol_lo, np_vol_hi, np_vol, np_more };
    lv_obj_t *over[] = { np_down, vnl_btn, vz_btn, np_queue, lyr_btn };
    static int32_t below_y[14], over_y[5];
    static bool read;
    if (!read)
    {
        for (int i = 0; i < 14; i++) below_y[i] = lv_obj_get_style_y(below[i], 0);
        for (int i = 0; i < 5; i++) over_y[i] = over[i] ? lv_obj_get_style_y(over[i], 0) : 0;
        read = true;
    }
    /* the song's words from the leading edge, whatever their script: in
     * Hebrew and Arabic the title and artist run from the right and the
     * heart sits at the left (the transport and times stay as they are) */
    sl_align(np_title, LV_ALIGN_TOP_LEFT, 24, lv_obj_get_style_y(np_title, 0));
    sl_align(np_artist, LV_ALIGN_TOP_LEFT, 24, lv_obj_get_style_y(np_artist, 0));
    sl_align(np_album, LV_ALIGN_TOP_LEFT, 24, lv_obj_get_style_y(np_album, 0));
    sl_align(np_love, LV_ALIGN_TOP_RIGHT, -62, lv_obj_get_style_y(np_love, 0));
    sl_align(np_more, LV_ALIGN_TOP_RIGHT, -14, lv_obj_get_style_y(np_more, 0));
    lv_text_align_t ta = sl_ui_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
    lv_obj_set_style_text_align(np_title, ta, 0);
    lv_obj_set_style_text_align(np_artist, ta, 0);
    lv_obj_set_style_text_align(np_album, ta, 0);
    bool c = np_centred(), alb = np_album_on();
    hidden_if_changed(np_album, !alb);
    int dy = c ? NC_DY : 0, top = np_status_on() ? SB_H - 4 : 0;
    for (int i = 0; i < 14; i++)
    {
        /* with the album: the artist 2 higher, the seek bar and all under it 12 lower */
        int extra = !alb || i < 3 || below[i] == np_album || below[i] == np_more ? 0 : below[i] == np_artist ? -2 : 12;
        if (below[i] == np_seek && np_wave()) extra -= (WV_H - NP_TRACK_H) / 2;   /* the waveform's box on the track's middle */
        if (lv_obj_get_style_y(below[i], 0) != below_y[i] + dy + extra) lv_obj_set_y(below[i], below_y[i] + dy + extra);
    }
    for (int i = 0; i < 5; i++)
        if (over[i] && lv_obj_get_style_y(over[i], 0) != over_y[i] + top) lv_obj_set_y(over[i], over_y[i] + top);
    lv_obj_set_pos(np_art, c ? (W - NC_S) / 2 : 0, c ? NC_Y : 0);
    lv_obj_set_size(np_art, c ? NC_S : W, c ? NC_S : W);
    lv_obj_set_style_radius(np_art, c ? SL_R_LG : 0, 0);
    lv_obj_set_style_clip_corner(np_art, c, 0);
    if (!c && lv_image_get_src(np_art_img) == &nc_cov_dsc) lv_image_set_src(np_art_img, &np_img);
    hidden_if_changed(np_scrim, !np_status_on());
    if (!vnl_on()) hidden_if_changed(np_art, c && lyr_visible());   /* Centred puts the cover away under the lyrics */
    lyr_layout(LYR_TOP0 + top, c ? W + NC_DY : W);
}
/* Now Playing's ink: the accent, or (Customization > Now Playing) the
 * cover's own colour, brightened enough to read, when it has one */
static uint32_t np_cover_ink;            /* 0: no cover, or a grey one */
static uint32_t np_ink(void) { return global_settings.sloppy_np_cover_ink && np_cover_ink ? np_cover_ink : C_ACCENT; }
static uint32_t cover_ink(const uint16_t *px, int w, int h, int stride)
{
    uint32_t bins[36] = { 0 };
    uint64_t chroma = 0;
    int n = 0;
    for (int y = h / 32; y < h; y += h / 16 ? h / 16 : 1)
        for (int x = w / 32; x < w; x += w / 16 ? w / 16 : 1)
        {
            uint16_t p = px[y * stride + x];
            int r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
            int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b), c = mx - mn;
            chroma += (uint64_t)c;
            n++;
            if (c < 28 || mx < 40) continue;
            int hu = mx == r ? 60 * (g - b) / c : mx == g ? 120 + 60 * (b - r) / c : 240 + 60 * (r - g) / c;
            if (hu < 0) hu += 360;
            bins[(hu / 10) % 36] += (uint32_t)(c * c);
        }
    int best = 0;
    for (int k = 1; k < 36; k++) if (bins[k] > bins[best]) best = k;
    if (!n || !bins[best] || chroma / (uint64_t)n < 18) return 0;   /* grey: the accent stays */
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)(best * 10 + 5), 62, 92);
    return lv_color_to_u32(c) & 0xffffff;
}
static void np_paint(void);
static int np_repeat_shown = -1;          /* 0 off, 1 all, 2 one, as last painted */
static void np_ink_set(uint32_t ink)
{
    if (ink == np_cover_ink) return;
    np_cover_ink = ink;
    if (global_settings.sloppy_np_cover_ink && np) np_paint();
}
/* the cover's colour (or the accent) deepened until it reads at 3:1 on
 * the white play button */
static uint32_t np_ink_on_white(void)
{
    if (!(global_settings.sloppy_np_cover_ink && np_cover_ink)) return sl_accent_on(0xffffff, 30);
    uint32_t c = np_cover_ink;
    for (int k = 1; k <= 12 && sl_contrast_x10(c, 0xffffff) < 30; k++)
        c = lv_color_to_u32(lv_color_mix(lv_color_black(), lv_color_hex(np_cover_ink), (uint8_t)(k * 18))) & 0xffffff;
    return c;
}
static void np_paint(void)
{
    np_layout();
    lv_obj_set_style_bg_color(np, lv_color_hex(C_BG), 0);
    hidden_if_changed(np_zn, !sl_zune());
    struct mp3entry *zid3 = audio_current_track();
    if (zid3)
    {
        char up[96];
        lv_label_set_text(np_artist, sl_zune() ? sl_upper(track_artist(zid3), up, sizeof up) : track_artist(zid3));
    }
    if (!sl_zune()) zid3 = NULL;
    if (zid3)
    {
        char low[96];
        const char *a = sl_lower(track_artist(zid3), low, sizeof low);
        lv_obj_set_style_text_font(np_zn, sl_zune_font(a, &ss_regular_60), 0);
        lv_label_set_text(np_zn, a);
    }
    np_love_paint();                         /* the heart in this theme's grey or accent */
    lv_obj_t *labels[] = { np_title, np_artist, np_fmt, np_t0, np_t1, np_album };
    for (unsigned i = 0; i < sizeof labels / sizeof labels[0]; i++)
        lv_obj_set_style_text_color(labels[i], lv_color_hex(i ? C_SUB : C_TEXT), 0);
    /* On the light page a white play button and pale icons all but
     * vanished, and so did the white seek track: ink and accent there. */
    bool light = sl_theme() == 1, black = sl_theme() == 2;
    /* true dark: an accent-tinted disc like a lit toggle; a black one on
     * black was no button at all, and pressing it flashed near-white */
    uint32_t ink = np_ink();
    lv_obj_set_style_bg_color(np_pp, lv_color_hex(light || black ? ink : 0xffffff), 0);
    lv_obj_set_style_bg_opa(np_pp, black ? LV_OPA_20 : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(np_pp, lv_color_hex(light || black ? ink : 0xdfe8f2), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(np_pp, light ? LV_OPA_80 : black ? LV_OPA_40 : LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(np_pp, 0), lv_color_hex(light ? 0xffffff : ink), 0);
    /* bare glyphs: a soft disc only while pressed; shuffle and the queue
     * a step quieter than the transport */
    lv_obj_t *row = lv_obj_get_parent(np_pp);
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++)
    {
        lv_obj_t *b = lv_obj_get_child(row, i);
        if (b == np_pp) continue;
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(np_icon_color()), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(light ? 0x182735 : 0xffffff), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, light ? 30 : 46, LV_STATE_PRESSED);
        lv_obj_set_style_text_opa(lv_obj_get_child(b, 0), lv_obj_get_style_width(b, 0) < 70 ? 190 : LV_OPA_COVER, 0);
    }
    /* Zune HD: every control a thin white ring, the play button too */
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++)
    {
        lv_obj_t *b = lv_obj_get_child(row, i);
        lv_obj_set_style_border_width(b, sl_zune() ? 2 : 0, 0);
        lv_obj_set_style_border_color(b, lv_color_white(), 0);
        lv_obj_set_style_border_opa(b, lv_obj_get_style_width(b, 0) < 70 ? 120 : 220, 0);
    }
    if (sl_zune())
    {
        lv_obj_set_style_bg_opa(np_pp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(np_pp, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(np_pp, 46, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(lv_obj_get_child(np_pp, 0), lv_color_white(), 0);
    }
    /* the sliders: the played part in the accent, the rest receding, a
     * white knob (ringed on the light page, where white on white vanished) */
    lv_obj_t *sliders[] = { np_seek, np_vol };
    for (int i = 0; i < 2; i++)
    {
        lv_obj_set_style_bg_color(sliders[i], lv_color_hex(ink), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sliders[i], lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sliders[i], light ? LV_OPA_20 : LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_border_width(sliders[i], light ? 2 : 0, LV_PART_KNOB);
        lv_obj_set_style_border_color(sliders[i], lv_color_hex(ink), LV_PART_KNOB);
    }
    lv_obj_set_style_text_color(np_vol_lo, lv_color_hex(np_icon_color()), 0);
    lv_obj_set_style_text_color(np_vol_hi, lv_color_hex(np_icon_color()), 0);
    lv_obj_remove_state(np_shuffle, LV_STATE_CHECKED);   /* np_update repaints it in the new accent */
    np_repeat_shown = -1;                                 /* and repeat */
    bool glass = np_glass();
    lv_obj_update_flag(np_glow, LV_OBJ_FLAG_HIDDEN, !glass);
    if (!glass && !np_centred()) { wv_apply(); np_glow_refresh(true); return; }   /* the lyrics' backdrop, the buttons */
    /* on the glass, and Centred's blur: white type and a white track in
     * every theme, a white play button, black under all of it; the played
     * part and the play glyph in the cover's colour when asked */
    lv_obj_set_style_bg_color(np, lv_color_black(), 0);
    for (unsigned i = 0; i < sizeof labels / sizeof labels[0]; i++)
        lv_obj_set_style_text_color(labels[i], lv_color_hex(i ? 0xdfe3ea : 0xffffff), 0);
    bool tint = global_settings.sloppy_np_cover_ink && np_cover_ink;
    for (int i = 0; i < 2; i++)
    {
        lv_obj_set_style_bg_color(sliders[i], tint ? lv_color_hex(np_cover_ink) : lv_color_white(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sliders[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sliders[i], 64, LV_PART_MAIN);
        lv_obj_set_style_border_width(sliders[i], 0, LV_PART_KNOB);
    }
    lv_obj_set_style_text_color(np_vol_lo, lv_color_white(), 0);
    lv_obj_set_style_text_color(np_vol_hi, lv_color_white(), 0);
    lv_obj_set_style_bg_color(np_pp, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(np_pp, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(np_pp, lv_color_hex(0xdfe8f2), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(np_pp, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(np_pp, 0), lv_color_hex(np_ink_on_white()), 0);   /* 3:1 on the white disc */
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++)
    {
        lv_obj_t *b = lv_obj_get_child(row, i);
        if (b == np_pp) continue;
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_white(), 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, 50, LV_STATE_PRESSED);
    }
    wv_apply();
    np_glow_refresh(true);
}

static void now_playing_retheme(void)
{
    if (!np) return;
    /* Now Playing is built once: its fixed words follow a change of language here */
    if (lyr_btn) lv_label_set_text(lv_obj_get_child(lyr_btn, 0), sl_tr("Lyrics"));
    np_paint();
    np_update(false);
}

/* Setting a style or flag invalidates the object even when nothing changes.
 * Songs without art retry every tick (4 times a second), so only touch
 * what differs, or the 480x480 art box repaints for nothing. */
static void bg_if_changed(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, 0), c))
        lv_obj_set_style_bg_color(o, c, 0);
}
static void hidden_if_changed(lv_obj_t *o, bool hidden)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hidden)
        lv_obj_update_flag(o, LV_OBJ_FLAG_HIDDEN, hidden);
}

static bool wf_online(void);
static bool wf_fetch(const char *url, size_t cap, void (*done)(bool ok, const char *text, int len));
#include "sloppy_lyrics.inc"

static uint32_t np_glow_at;              /* the glass is due for the cover shown since then */
static void np_glow_soon(void) { if (!np_glow_at) np_glow_at = lv_tick_get() ? lv_tick_get() : 1; }
static void np_set_art(struct mp3entry *id3)
{
    int track = sl_lib_find_path(id3->path);
    int album = track >= 0 ? sl_lib.tracks[track].album : -1;
    int hid = aa_slot >= 0 ? playback_current_aa_hid(aa_slot) : -1;
    struct bitmap *bmp = NULL;
    int w = 0, h = 0, stride = 0;
    if (np_img_buf && hid >= 0 && bufgetdata(hid, 0, (void **)&bmp) > 0 && bmp && bmp->data)
    {
        w = bmp->width; h = bmp->height;
        if (w > 0 && h > 0 && w <= W && h <= W)
        {
            memcpy(np_img_buf, bmp->data, (size_t)bmp->width * h * 2);
            stride = bmp->width * 2;
            np_img_provisional = false;
            goto show_art;
        }
    }
    /* Keep this album's existing picture during a track transition. The
     * full-size playback image can replace it on a later tick. */
    if (album >= 0 && album == np_img_album &&
        !lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN))
    { np_img_provisional = true; return; }
    if (np_img_buf && sl_art_load_square(album, W, np_img_buf))
    {
        w = h = W;
        stride = W * 2;
        np_img_provisional = true;
        goto show_art;
    }
    np_img_album = -1;
    np_img_provisional = false;
    hidden_if_changed(np_art_img, true);
    unsigned hue = 0;
    for (const char *p = id3->album ? id3->album : id3->path; *p; p++) hue = hue * 31 + (unsigned char)*p;
    lv_color_t bg = lv_color_hsv_to_rgb(hue % 360, 55, 45);
    bg_if_changed(np_art, bg);
    if (!lv_color_eq(lv_obj_get_style_bg_color(np_art_none, 0), bg))
    {
        lv_obj_set_style_bg_color(np_art_none, bg, 0);   /* the icon's cut-outs */
        lv_obj_set_style_text_color(np_art_none, lv_color_hsv_to_rgb(hue % 360, 30, 72), 0);
    }
    hidden_if_changed(np_art_none, false);
    np_ink_set(0);
    np_glow_refresh(false);
    return;
show_art:
    np_img_album = album;
    np_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    np_img.header.cf = LV_COLOR_FORMAT_RGB565;
    np_img.header.w = w;
    np_img.header.h = h;
    np_img.header.stride = stride;
    np_img.data = (const uint8_t *)np_img_buf;
    np_img.data_size = (uint32_t)stride * h;
    lv_image_cache_drop(&np_img);
    lv_image_set_src(np_art_img, &np_img);
    lv_obj_remove_flag(np_art_img, LV_OBJ_FLAG_HIDDEN);
    hidden_if_changed(np_art_none, true);
    lv_obj_set_style_bg_color(np_art, lv_color_black(), 0);
    if (np_centred() && nc_cov_px)        /* Centred: the small cover at once, its backdrop with the glass */
    {
        nc_scale(np_img_buf, w, h, stride / 2, 0);
        nc_show(np_art_img, &nc_cov_dsc, nc_cov_px, NC_S, NC_S);
    }
    np_ink_set(cover_ink((const uint16_t *)np_img_buf, w, h, stride / 2));
    /* The thumbnail stands in until the full-size cover comes, often a
     * moment later: the glass waits for that one, or rebuilding it twice
     * (~140 ms each on the R1) made every skip stutter. */
    if (np_img_provisional) np_glow_soon();
    else { np_glow_at = 0; np_glow_refresh(false); }
}

/* Bits per sample, which Rockbox's track info does not keep: from a FLAC
 * file's STREAMINFO, read once a song. 0 when not known. */
static int np_bits(const char *path)
{
    static char seen[MAX_PATH];
    static int bits;
    if (!strcmp(seen, path)) return bits;
    strlcpy(seen, path, sizeof seen);
    bits = 0;
    unsigned char h[26];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    /* "fLaC", the block header, then STREAMINFO: its bits per sample less
     * one sit across bytes 12 and 13 */
    if (read(fd, h, sizeof h) == (ssize_t)sizeof h && !memcmp(h, "fLaC", 4) && (h[4] & 0x7f) == 0)
        bits = ((h[8 + 12] & 1) << 4 | h[8 + 13] >> 4) + 1;
    close(fd);
    return bits;
}
/* Shuffle and repeat on: a lit accent disc, the glyph in the ink that
 * reads on it (an accent glyph on a faint accent wash fell under 3:1 on
 * many covers). Off: the bare glyph, as the transport. */
static void np_state_paint(lv_obj_t *b, bool on)
{
    lv_obj_set_state(b, LV_STATE_CHECKED, on);
    uint32_t ink = np_ink();             /* the cover's colour, when Now Playing takes it */
    lv_obj_set_style_bg_color(b, lv_color_hex(ink), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(on ? sl_ink_on(ink) : np_icon_color()), 0);
    lv_obj_set_style_text_opa(lv_obj_get_child(b, 0), on ? LV_OPA_COVER : 190, 0);
}
/* beside the Lyrics button, or in its place when a song has none */
static void np_queue_place(void)
{
    int x = lyr_btn && !lv_obj_has_flag(lyr_btn, LV_OBJ_FLAG_HIDDEN) ? lv_obj_get_x(lyr_btn) - 8 - 56 : W - 8 - 56;
    if (lv_obj_get_x(np_queue) != x) lv_obj_set_x(np_queue, x);
}
static void np_update(bool force)
{
    struct mp3entry *id3 = audio_current_track();
    if (!id3 || !np)
        return;
    bool path_changed = strcmp(np_last_path, id3->path) != 0;
    static int np_last_cue = -1;         /* a cue sheet's next track is a new song in the same file */
    int cue = cue_now(id3, NULL, NULL);
    bool changed = force || path_changed || cue != np_last_cue;
    np_last_cue = cue;
    if (changed)
    {
        strlcpy(np_last_path, id3->path, sizeof np_last_path);
        int was = np_love_track;
        np_love_track = cue >= 0 ? cue : sl_lib_find_path(id3->path);
        np_love_paint();
        if (was != np_love_track) song_lists_mark(was, np_love_track);
        char name[64];
        label_if_changed(np_title, track_title(id3, name, sizeof name));
        char up[96];   /* Zune: the artist in bold capitals over the song */
        label_if_changed(np_artist, sl_zune() ? sl_upper(track_artist(id3), up, sizeof up) : track_artist(id3));
        label_if_changed(np_album, track_album(id3));
        if (sl_zune())
        {
            char low[96];
            const char *a = sl_lower(track_artist(id3), low, sizeof low);
            lv_obj_set_style_text_font(np_zn, sl_zune_font(a, &ss_regular_60), 0);
            label_if_changed(np_zn, a);
        }
        if (path_changed || np_img_provisional ||
            lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN)) np_set_art(id3);
        lyr_track(id3->path);
        np_book_buttons(audiobooks_is_book(id3->path) || sl_pod_is_episode(id3->path));
        vnl_track_changed();
        if (path_changed) { nps_pending(); nps_track_changed(); }   /* the song a slide showed, checked */
    }
    else if (np_img_provisional || lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN))
    {
        np_set_art(id3);   /* art may arrive after the track starts */
        if (!lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN)) vnl_label_dirty = true;
    }
    if (np_glow_at && lv_tick_elaps(np_glow_at) >= 700) { np_glow_at = 0; np_glow_refresh(false); }

    char b[16];
    if (!lv_slider_is_dragged(np_seek))
    {
        unsigned long elapsed = visible_elapsed(id3), lag = vnl_lag_ms();
        elapsed = elapsed > lag ? elapsed - lag : 0;    /* the turntable's needle trails */
        long cs, cl;                                     /* a cue sheet's track: its own time */
        cue_now(id3, &cs, &cl);
        elapsed = elapsed > (unsigned long)cs ? elapsed - cs : 0;
        if (cl > 0 && elapsed > (unsigned long)cl) elapsed = cl;
        int progress = cl > 0 ? (int)((uint64_t)elapsed * 1000 / cl) : 0;
        if (np_wave()) wv_progress(progress);   /* only the strip it moved across */
        else if (lv_slider_get_value(np_seek) != progress)
            lv_slider_set_value(np_seek, progress, LV_ANIM_OFF);
        fmt_time(b, sizeof b, elapsed);
        label_if_changed(np_t0, b);
    }
    {
        long cl;
        cue_now(id3, NULL, &cl);
        fmt_time(b, sizeof b, cl);
        label_if_changed(np_t1, b);
    }
    if (!lv_slider_is_dragged(np_vol) && global_status.volume != np_vol_seen)
    {
        lv_slider_set_value(np_vol, volume_pct(), LV_ANIM_OFF);   /* the buttons on the side move it too */
        np_vol_seen = global_status.volume;
    }
    /* "FLAC 320 kbps · 3/6": an unknown bit rate or place is left out, not
     * shown as 0kbps or 0/0 */
    char f[48];
    int len = 0, i = playlist_get_display_index(), all = playlist_amount();
    const char *ext = strrchr(id3->path, '.');
    if (ext && ext[1] && strlen(ext + 1) <= 5)
        for (const char *q = ext + 1; *q; q++) f[len++] = *q >= 'a' && *q <= 'z' ? *q - 32 : *q;
    f[len] = '\0';
    /* lossless says its bits and sample rate ("24/96 kHz"), and Hi-Res past
     * CD's; lossy its bit rate */
    int track = sl_lib_find_path(id3->path);
    if (track >= 0 && sl_lib.tracks[track].lossless && id3->frequency)
    {
        int bits = np_bits(id3->path);
        unsigned long khz = id3->frequency / 1000, tenth = id3->frequency % 1000 / 100;
        char rate[16], depth[8] = "";
        if (tenth) snprintf(rate, sizeof rate, "%lu.%lu", khz, tenth); else snprintf(rate, sizeof rate, "%lu", khz);
        if (bits) snprintf(depth, sizeof depth, "%d/", bits);
        len += snprintf(f + len, sizeof f - len, "%s%s%s kHz%s", len ? " " : "", depth, rate,
                        id3->frequency > 48000 || bits > 16 ? " Hi-Res" : "");
    }
    else if (id3->bitrate) len += snprintf(f + len, sizeof f - len, "%s%u kbps", len ? " " : "", (unsigned)id3->bitrate);
    if (sr_on()) snprintf(f + len, sizeof f - len, "%sRadio", len ? "  \xc2\xb7  " : "");   /* no end to count to */
    else if (i > 0 && all > 0 && i <= all) snprintf(f + len, sizeof f - len, "%s%d/%d", len ? "  \xc2\xb7  " : "", i, all);
    label_if_changed(np_fmt, f);
    label_if_changed(lv_obj_get_child(np_pp, 0), is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    /* shuffle shows whether it is on, not only a toast when pressed */
    bool shuffle = global_settings.playlist_shuffle;
    if (lv_obj_has_state(np_shuffle, LV_STATE_CHECKED) != shuffle)
    {
        np_state_paint(np_shuffle, shuffle);
    }
    int rep = global_settings.repeat_mode == REPEAT_ONE ? 2 : global_settings.repeat_mode != REPEAT_OFF ? 1 : 0;
    if (rep != np_repeat_shown)
    {
        np_repeat_shown = rep;
        np_state_paint(np_repeat, rep > 0);
        lv_obj_t *one = lv_obj_get_child(np_repeat, 1);   /* the reverse of the disc: ink, the accent in it */
        lv_obj_set_style_bg_color(one, lv_color_hex(sl_ink_on(C_ACCENT)), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(one, 0), lv_color_hex(C_ACCENT), 0);
        lv_obj_update_flag(one, LV_OBJ_FLAG_HIDDEN, rep != 2);
    }
    np_queue_place();
}

#include "sloppy_npswipe.inc"

/* ---------------------------------------------------------- mini player */
static lv_obj_t *mini_title, *mini_artist, *mini_pp, *mini_nx, *mini_prog, *mini_txt;
static lv_obj_t *mini_peek, *mini_peek_title, *mini_peek_artist;
static bool mini_leaving;            /* sliding to the next song: keep the titles */
static int mini_peek_side;           /* 1: next song on the right, -1: previous on the left */
static bool mini_peek_ok;            /* there is a song on that side */
/* The title and artist ride under the finger with the neighbouring song
 * beside them, the next one on the right and the previous on the left.
 * Let go past a third of the way, or flick, and that song slides into
 * place; the buttons stay put. */
#define MINI_TXT_W (W - 170)
#define MINI_GAP 28
#define MINI_STEP (mini_txt_w + MINI_GAP)
/* The song's cover, at a list row's size and in from the card's edge as
 * far as it is from the top. It shows only once its thumbnail is on the
 * card (never a stand-in): until then the words keep the whole lane; a
 * cover that comes later fades in as the words make room. */
#define MINI_ART_IN 12
#define MINI_ART_X (MINI_ART_IN + SL_ART_ROW + SL_GUTTER)   /* the words' lane beside a cover */
static lv_obj_t *mini_art, *mini_lane;
static int mini_txt_w = MINI_TXT_W;  /* the lane: narrower beside a cover */
static int mini_art_album = -1;      /* whose cover is in mini_art, or -1 */
static int mini_art_want = -1;       /* the song's album, its cover not on the card yet */
static unsigned mini_art_made;       /* sl_art_made when last looked */
static char mini_art_path[MAX_PATH]; /* the song looked up */
#define MINI_PROG_W (W - 2 * SL_FLOAT - 2 * SL_R_LG)   /* the progress line, clear of the corners */
static int cur_index(void);
static void mini_swipe_x(void *o, int32_t x)
{
    (void)o;
    lv_obj_set_style_translate_x(mini_txt, x, 0);
    lv_obj_set_style_translate_x(mini_peek, x + (mini_peek_side ? mini_peek_side : 1) * MINI_STEP, 0);
}
/* Put the song on this side in the peek. Only on a change of side: the
 * previous song's name is read from the playlist on the card. */
static void mini_peek_fill(int side)
{
    if (side == mini_peek_side) return;
    mini_peek_side = side;
    const char *title = NULL, *artist = "";
    struct mp3entry *next = side > 0 ? audio_next_track() : NULL;
    struct playlist_track_info info;
    if (next)
    {
        static char name[64];
        title = track_title(next, name, sizeof name);
        artist = track_artist(next);
    }
    else if (side < 0 && cur_index() > 0 && playlist_get_track_info(NULL, cur_index() - 1, &info) >= 0)
    {
        int t = sl_lib_find_path(info.filename);
        const char *slash = strrchr(info.filename, '/');
        title = t >= 0 ? sl_lib.tracks[t].title : slash ? slash + 1 : info.filename;
        if (t >= 0) artist = sl_track_artist(t);
    }
    else if (side < 0)
    {
        /* the first song: back means from the top */
        title = lv_label_get_text(mini_title);
        artist = lv_label_get_text(mini_artist);
    }
    mini_peek_ok = title != NULL;
    label_if_changed(mini_peek_title, title ? title : "");
    label_if_changed(mini_peek_artist, artist ? artist : "");
}
static void mini_swipe_drag(int dx)
{
    lv_anim_delete(mini_txt, mini_swipe_x);
    if (mini_leaving) return;            /* still settling the last one */
    if (dx) mini_peek_fill(dx < 0 ? 1 : -1);
    /* one to one while there is a song to bring in; stiff when not */
    if (!mini_peek_ok) dx /= 4;
    else if (dx > MINI_STEP) dx = MINI_STEP;
    else if (dx < -MINI_STEP) dx = -MINI_STEP;
    mini_swipe_x(mini_txt, dx);
}
static void mini_slide(int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, mini_txt);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, SL_T_SPAN((int)abs(to - from), MINI_STEP));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, mini_swipe_x);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
static void mini_update(void);
static void mini_landed(lv_anim_t *a)
{
    (void)a;
    /* the peek is now where the title was: take its words and step back */
    label_if_changed(mini_title, lv_label_get_text(mini_peek_title));
    label_if_changed(mini_artist, lv_label_get_text(mini_peek_artist));
    mini_leaving = false;
    mini_peek_side = 0;                  /* the neighbours have changed */
    mini_swipe_x(mini_txt, 0);
    mini_update();
}
/* back where it was: look at the neighbours afresh next time */
static void mini_back(lv_anim_t *a) { (void)a; mini_peek_side = 0; }
static void mini_swipe_release(int dx, int vx)
{
    int from = lv_obj_get_style_translate_x(mini_txt, 0);
    bool go = mini_peek_ok && !mini_leaving &&
              (abs(dx) > mini_txt_w / 3 || (abs(dx) > 30 && abs(vx) > 450 && (vx < 0) == (dx < 0)));
    if (!go)
    {
        if (!mini_leaving) mini_slide(from, 0, mini_back);
        return;
    }
    if (mini_peek_side > 0) sl_next(); else sl_prev_song();
    mini_leaving = true;
    mini_slide(from, -mini_peek_side * MINI_STEP, mini_landed);
}
static void mini_retheme(void)
{
    if (!mini) return;
    lv_obj_set_style_bg_color(mini, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(mini, sl_zune() ? 0 : SL_R_LG, 0);   /* Zune: a square bar */
    lv_obj_set_style_radius(mini_art, sl_zune() ? 0 : SL_R_SM, 0);
    lv_obj_set_style_text_color(mini_title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_color(mini_artist, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_text_color(mini_peek_title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_color(mini_peek_artist, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_bg_color(mini_prog, lv_color_hex(C_ACCENT), 0);
    if (mini_pp && lv_obj_get_child_count(mini_pp))
        lv_obj_set_style_text_color(lv_obj_get_child(mini_pp, 0), lv_color_hex(C_TEXT), 0);
    if (mini_nx && lv_obj_get_child_count(mini_nx))   /* it was white on the light card */
        lv_obj_set_style_text_color(lv_obj_get_child(mini_nx, 0), lv_color_hex(C_TEXT), 0);
}
static void mini_cb(lv_event_t *e)
{
    intptr_t k = (intptr_t)lv_event_get_user_data(e);
    if (k == 0) np_show();
    else if (k == 1) toggle_play();
    else if (k == 2) sl_next();
}
/* Rows scrolling behind the floating player showed in the gap under it
 * and round its corners. It draws, under itself, the page's own colour
 * from its top edge to the foot of the screen (A12): nothing above it
 * changes (a fade would only band on the 16-bit panel). It comes and goes
 * with the player, and leaves home's wallpaper alone. */
static void mini_scrim_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) { lv_event_set_ext_draw_size(e, SL_FLOAT); return; }
    if (npages < 2) return;
    lv_obj_t *pg = pages[npages - 1];
    if (lv_obj_get_style_bg_opa(pg, 0) < LV_OPA_COVER || lv_obj_get_style_bg_grad_dir(pg, 0) != LV_GRAD_DIR_NONE) return;
    lv_area_t m;
    lv_obj_get_coords(lv_event_get_current_target(e), &m);   /* the player, or the picking bar in its place */
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_obj_get_style_bg_color(pg, 0);
    lv_area_t a = { 0, m.y1, W - 1, H - 1 };
    lv_draw_rect(lv_event_get_layer(e), &d, &a);
}
#include "src/misc/lv_area_private.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"
#include "src/core/lv_obj_draw_private.h"
static bool qp_bd_idle(lv_obj_t *o);   /* sloppy_playback.inc */
/* Something visible drawn over the player's middle, inside a: it can't be
 * left out of a redraw then. */
static bool mini_covered(lv_obj_t *parent, uint32_t from, const lv_area_t *a)
{
    for (uint32_t i = from; i < lv_obj_get_child_count(parent); i++)
    {
        lv_obj_t *o = lv_obj_get_child(parent, i);
        lv_area_t c, x;
        lv_obj_get_coords(o, &c);
        int32_t ext = lv_obj_get_ext_draw_size(o);   /* a shadow reaches past it */
        lv_area_increase(&c, ext, ext);
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) && !qp_bd_idle(o) && lv_area_intersect(&x, &c, a)) return true;
    }
    return false;
}
/* The player floats over the page, opaque but for its round corners. When
 * the page under it scrolls or slides, a redraw that runs under the whole
 * player leaves out its middle (all but the corners' columns): those pixels
 * stay as they were, so they are neither drawn nor sent to the panel again.
 * Only while the middle on the screen is known to be the player's own
 * (mini_core_ok, kept by mini_core_track) and it stays so: not while the
 * player moves, fades, is scaled or has anything over it. */
static bool mini_core(lv_area_t *core)
{
    if (!mini || lv_obj_has_flag(mini, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t m;
    lv_obj_get_coords(mini, &m);
    int r = lv_obj_get_style_radius(mini, 0);
    *core = (lv_area_t){ m.x1 + r, m.y1, m.x2 - r, m.y2 };
    return true;
}
static bool mini_on_top(const lv_area_t *core)
{
    return lv_obj_get_style_opa_recursive(mini, 0) >= LV_OPA_MAX && lv_obj_get_style_bg_opa(mini, 0) >= LV_OPA_MAX &&
           lv_obj_get_style_transform_scale_x(mini, 0) == LV_SCALE_NONE &&
           lv_obj_get_style_transform_scale_y(mini, 0) == LV_SCALE_NONE &&
           !lv_obj_get_style_transform_rotation(mini, 0) && !lv_anim_get(mini, NULL) &&
           !lv_obj_has_flag(lv_obj_get_parent(mini), LV_OBJ_FLAG_HIDDEN) &&
           !mini_covered(lv_obj_get_parent(mini), lv_obj_get_index(mini) + 1, core) &&
           !mini_covered(lv_layer_top(), 0, core) && !mini_covered(lv_layer_sys(), 0, core);
}
/* After each render: the middle redrawn with anything else over it, or the
 * player away, is no longer the player's; redrawn whole with the player on
 * top, it is again. */
static void mini_core_track(lv_event_t *e)
{
    lv_display_t *d = lv_event_get_target(e);
    lv_area_t core, x;
    if (!mini_core(&core)) { mini_core_ok = false; return; }
    bool top = mini_on_top(&core), whole = false;
    for (uint32_t i = 0; i < d->inv_p; i++)
    {
        if (d->inv_area_joined[i] || !lv_area_intersect(&x, &d->inv_areas[i], &core)) continue;
        if (!top) { mini_core_ok = false; return; }
        if (lv_area_is_in(&core, &d->inv_areas[i], 0)) whole = true;
    }
    if (whole) mini_core_ok = true;
}
static void mini_cut_cb(lv_event_t *e)
{
    static bool cutting;
    lv_area_t *a = lv_event_get_param(e), core;
    if (cutting) return;
    if (a->x1 <= 0 && a->y1 <= 0 && a->x2 >= lv_display_get_horizontal_resolution(disp) - 1 &&
        a->y2 >= lv_display_get_vertical_resolution(disp) - 1)
    {
        mini_core_ok = false;            /* the whole screen: drawn over by someone else (a turn, Rockbox) */
        return;
    }
    if (!mini_core_ok || !mini_core(&core)) return;
    if (a->x1 > core.x1 || a->x2 < core.x2 || a->y1 > core.y1 || a->y2 < core.y2) return;
    lv_area_t left = { a->x1, core.y1, core.x1 - 1, core.y2 }, right = { core.x2 + 1, core.y1, a->x2, core.y2 };
    if (a->y1 >= core.y1 && left.x2 < left.x1 && right.x2 < right.x1) return;   /* a must keep something */
    if (!mini_on_top(&core)) return;
    cutting = true;
    if (left.x2 >= left.x1) lv_inv_area(disp, &left);
    if (right.x2 >= right.x1) lv_inv_area(disp, &right);
    if (a->y2 > core.y2) lv_inv_area(disp, &(lv_area_t){ a->x1, core.y2 + 1, a->x2, a->y2 });
    cutting = false;
    if (a->y1 < core.y1) a->y2 = core.y1 - 1;
    else if (left.x2 >= left.x1) *a = left;   /* nothing above: one already kept, so it is dropped */
    else *a = right;
}
static void mini_build(void)
{
    mini = box(ui_screen());
    lv_obj_set_size(mini, W - 2 * SL_FLOAT, 82);
    lv_obj_align(mini, LV_ALIGN_BOTTOM_MID, 0, -SL_FLOAT);
    lv_obj_set_style_radius(mini, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(mini, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(mini, LV_OPA_COVER, 0);
    sl_elevate(mini, SL_E3, 0);
    /* no clip_corner: every child stays inside the rounded corners, and
     * clipping drew the corners again through a layer on every frame the
     * page under the player moved (1.3 ms of a 22 ms home scroll) */
    lv_obj_add_event_cb(mini, mini_scrim_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(mini, mini_scrim_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    on_click(mini, mini_cb, (void *)0);
    lv_obj_add_flag(mini, LV_OBJ_FLAG_USER_1);   /* no back-swipe from here */

    mini_art = box(mini);
    lv_obj_set_size(mini_art, SL_ART_ROW, SL_ART_ROW);
    lv_obj_align(mini_art, LV_ALIGN_LEFT_MID, MINI_ART_IN, 0);
    lv_obj_set_style_radius(mini_art, sl_zune() ? 0 : SL_R_SM, 0);
    lv_obj_set_style_clip_corner(mini_art, true, 0);
    lv_obj_add_flag(mini_art, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(mini_art, LV_OBJ_FLAG_CLICKABLE);
    mini_art_album = mini_art_want = -1;
    mini_art_path[0] = 0;
    mini_txt_w = MINI_TXT_W;

    lv_obj_t *lane = mini_lane = box(mini);   /* clips the sliding title */
    lv_obj_set_pos(lane, SL_INSET, 0);
    lv_obj_set_size(lane, MINI_TXT_W, 82);
    for (int k = 0; k < 2; k++)
    {
        lv_obj_t *t = box(lane), *l1, *l2;
        lv_obj_set_size(t, MINI_TXT_W, 82);
        l1 = text(t, "", &ss_semibold_21, 0xffffff);
        lv_obj_set_pos(l1, 0, 12);
        lv_obj_set_width(l1, MINI_TXT_W);
        label_lines(l1, 1);
        l2 = text(t, "", &ss_regular_16, C_SUB);
        lv_obj_set_pos(l2, 0, 42);
        lv_obj_set_width(l2, MINI_TXT_W);
        label_lines(l2, 1);
        if (k) { mini_peek = t; mini_peek_title = l1; mini_peek_artist = l2; }
        else { mini_txt = t; mini_title = l1; mini_artist = l2; }
    }
    lv_obj_set_style_translate_x(mini_peek, MINI_STEP, 0);   /* out of sight */

    mini_pp = box(mini);
    lv_obj_set_size(mini_pp, 56, 56);
    lv_obj_align(mini_pp, LV_ALIGN_RIGHT_MID, -70, 0);
    lv_obj_add_style(mini_pp, &st_row, 0);
    lv_obj_add_style(mini_pp, &st_row_pr, LV_STATE_PRESSED);
    lv_obj_set_style_radius(mini_pp, SL_R_PILL, 0);
    lv_obj_center(text(mini_pp, LV_SYMBOL_PLAY, &sl_sym_28, 0xffffff));
    on_click(mini_pp, mini_cb, (void *)1);
    lv_obj_remove_flag(mini_pp, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *nx = mini_nx = box(mini);
    lv_obj_set_size(nx, 56, 56);
    lv_obj_align(nx, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_style(nx, &st_row, 0);
    lv_obj_add_style(nx, &st_row_pr, LV_STATE_PRESSED);
    lv_obj_set_style_radius(nx, SL_R_PILL, 0);
    lv_obj_center(text(nx, LV_SYMBOL_NEXT, &sl_sym_28, 0xffffff));
    on_click(nx, mini_cb, (void *)2);
    lv_obj_remove_flag(nx, LV_OBJ_FLAG_EVENT_BUBBLE);

    mini_prog = box(mini);
    lv_obj_set_size(mini_prog, 0, 3);
    lv_obj_align(mini_prog, LV_ALIGN_BOTTOM_LEFT, SL_R_LG, -SL_SP_XS);
    lv_obj_set_style_radius(mini_prog, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mini_prog, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(mini_prog, LV_OPA_COVER, 0);
    lv_obj_add_flag(mini, LV_OBJ_FLAG_HIDDEN);
    lv_display_add_event_cb(disp, mini_cut_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, mini_core_track, LV_EVENT_RENDER_READY, NULL);
}
static bool quiz_menu_shown(void);
static bool spoon_on_top(void);
static bool chess_on_top(void);
static bool clicker_on_top(void);
static bool spacey_on_top(void);
static bool kb_on_top(void);
static bool coin_on_top(void);
static bool snake_on_top(void);
/* Freaky Spoon's arrows, the chess toolbar, the coin's Flip, the snake's
 * hint and a keyboard sit where the mini player would */
static bool fm_dest_on_top(void);
/* Where the mini player floats, and whether a page keeps something to tap
 * there. A long list may run under it (it scrolls clear); a control fixed
 * in place, or in a page that only just scrolls, may not: the player
 * steps aside for that page, as it does for a game's controls. */
#define MINI_H 82
#define MINI_CLEAR (MINI_H + SL_FLOAT)   /* a list scrolling this far takes anything out from under it */
static bool mini_area_has_control(lv_obj_t *o)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    if (a.y2 < H - SL_FLOAT - MINI_H || a.y1 >= H - SL_FLOAT) return false;   /* nothing of it down there */
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) &&
        lv_obj_get_scroll_top(o) + lv_obj_get_scroll_bottom(o) >= MINI_CLEAR) return false;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) &&
        lv_area_get_height(&a) < H / 2)   /* not a page's body or a catcher */
        return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        if (mini_area_has_control(lv_obj_get_child(o, i))) return true;
    return false;
}
static bool page_needs_mini_area(void)
{
    if (npages < 2) return false;        /* home is laid out around the player */
    lv_obj_t *p = pages[npages - 1];
    lv_obj_update_layout(p);             /* a page just made has not been laid out yet */
    for (uint32_t i = 0; i < lv_obj_get_child_count(p); i++)
        if (mini_area_has_control(lv_obj_get_child(p, i))) return true;
    return false;
}
static bool mini_held_off(void)
{
    return page_needs_mini_area() || fm_dest_on_top() || quiz_active || quiz_menu_shown() || search_hides_mini() || kb_on_top() || coin_on_top() || snake_on_top() || sel_on() ||
           st_on() || spoon_on_top() || chess_on_top() || clicker_on_top() || spacey_on_top() || arc_on_top() ||
           t48_on_top() || sdk_on_top() || pt_on_top() || fl_on_top();
}
/* the lane at x, w wide, and the titles in it */
static void mini_lane_set(int x)
{
    mini_txt_w = MINI_TXT_W - (x - SL_INSET);
    lv_obj_set_x(mini_lane, x);
    lv_obj_set_width(mini_lane, mini_txt_w);
    lv_obj_t *const ls[] = { mini_txt, mini_peek, mini_title, mini_artist, mini_peek_title, mini_peek_artist };
    for (unsigned i = 0; i < sizeof ls / sizeof ls[0]; i++) lv_obj_set_width(ls[i], mini_txt_w);
    if (!mini_peek_side) lv_obj_set_style_translate_x(mini_peek, MINI_STEP, 0);
}
static void mini_lane_anim(void *o, int32_t v)
{
    (void)o;
    mini_lane_set(SL_INSET + (MINI_ART_X - SL_INSET) * v / 256);
    lv_obj_set_style_opa(mini_art, (lv_opa_t)(v > 255 ? 255 : v), 0);
}
static void mini_art_gone(void)
{
    lv_obj_add_flag(mini_art, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(mini_art);              /* its pixels go with it */
}
static void mini_art_hidden(lv_anim_t *a) { (void)a; if (mini_art_album < 0) mini_art_gone(); }
/* album's cover in (when its thumbnail is on the card), or none (-1);
 * eased when the player is on screen, at once when not */
static void mini_art_show(int album, bool ease)
{
    if (album >= 0)
    {
        /* the thumbnail, copied once into memory (6.7 KB, freed with it) */
        lv_obj_t *old = lv_obj_get_child_count(mini_art) ? lv_obj_get_child(mini_art, 0) : NULL;
        if (!sl_art_apply_scaled(mini_art, album, SL_ART_ROW)) return;   /* not made yet: keep what shows */
        if (old) lv_obj_delete(old);
        lv_obj_remove_flag(mini_art, LV_OBJ_FLAG_HIDDEN);
    }
    bool was = mini_art_album >= 0, now = album >= 0;
    mini_art_album = album;
    if (was == now) return;
    lv_anim_delete(mini_art, mini_lane_anim);
    if (!ease)
    {
        mini_lane_anim(mini_art, now ? 256 : 0);
        if (!now) mini_art_gone();
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, mini_art);
    lv_anim_set_exec_cb(&a, mini_lane_anim);
    lv_anim_set_values(&a, now ? 0 : 256, now ? 256 : 0);
    lv_anim_set_duration(&a, SL_T_PAGE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (!now) lv_anim_set_completed_cb(&a, mini_art_hidden);
    lv_anim_start(&a);
}
/* a new song: its album, and its cover if the card has it */
static void mini_art_track(struct mp3entry *id3)
{
    bool shown = !lv_obj_has_flag(mini, LV_OBJ_FLAG_HIDDEN);
    if (strcmp(mini_art_path, id3->path))
    {
        strlcpy(mini_art_path, id3->path, sizeof mini_art_path);
        int t = sl_lib_find_path(id3->path);
        int album = t >= 0 ? sl_lib.tracks[t].album : -1;
        mini_art_want = album >= 0 && album != mini_art_album ? album : -1;
        mini_art_made = sl_art_made;
        if (album < 0 || sl_art_no_cover(album)) { mini_art_want = -1; mini_art_show(-1, shown); }
        else if (album != mini_art_album) { mini_art_show(album, shown); if (mini_art_album != album) mini_art_show(-1, shown); }
        if (mini_art_album == album) mini_art_want = -1;
    }
    else if (mini_art_want >= 0 && mini_art_made != sl_art_made)
    {
        /* thumbnails were made since: is this one among them? */
        mini_art_made = sl_art_made;
        mini_art_show(mini_art_want, shown);
        if (mini_art_album == mini_art_want || sl_art_no_cover(mini_art_want)) mini_art_want = -1;
    }
}
static void mini_update(void)
{
    if (mini_held_off()) { lv_obj_add_flag(mini, LV_OBJ_FLAG_HIDDEN); return; }
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    if (!id3)
    {
        lv_obj_add_flag(mini, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!np_open)
        lv_obj_remove_flag(mini, LV_OBJ_FLAG_HIDDEN);
    char name[64];
    if (!mini_leaving)
    {
        label_if_changed(mini_title, track_title(id3, name, sizeof name));
        label_if_changed(mini_artist, track_artist(id3));
        mini_art_track(id3);
    }
    label_if_changed(lv_obj_get_child(mini_pp, 0), is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    long cs, cl;
    cue_now(id3, &cs, &cl);
    unsigned long at = visible_elapsed(id3);
    at = at > (unsigned long)cs ? at - cs : 0;
    int32_t progress = cl > 0 ? (int32_t)LV_MIN((uint64_t)MINI_PROG_W, (uint64_t)MINI_PROG_W * at / cl) : 0;
    if (lv_obj_get_width(mini_prog) != progress)
        lv_obj_set_width(mini_prog, progress);
}

static int volume_pct(void);
static int volume_snap(int v);
static void statusbar_update(void);
#include "sloppy_lockplayer.inc"

/* ----------------------------------------------------------- status bar */
static lv_obj_t *sb, *sb_vol, *sb_vol_n, *sb_clock, *sb_batt, *sb_bolt, *sb_radio, *sb_bt;
static bool sb_bt_linked(void);
static const char *sb_bt_codec(void);
static uint32_t sb_ink = 0xffffff;          /* the bar's ink, for the codec's tint */
/* The codec Bluetooth plays with, as the colour of its glyph (P42): gold
 * LDAC, violet aptX HD and Adaptive, blue aptX, teal AAC; SBC keeps the
 * bar's own ink. Two tones of each: a light one over dark ground, a deep
 * one over light ground, so it reads as clearly as the words beside it.
 * The Bluetooth page's codec list wears the same colours. */
static bool sb_has(const char *s, const char *w)
{
    for (size_t n = strlen(w); s && *s; s++) if (!strncasecmp(s, w, n)) return true;
    return false;
}
static uint32_t bt_codec_tint(const char *codec, bool on_light)
{
    if (sb_has(codec, "ldac")) return on_light ? 0x8f6200 : 0xf2bd4b;
    if (sb_has(codec, "aptx") && (sb_has(codec, "hd") || sb_has(codec, "adaptive"))) return on_light ? 0x6a43c4 : 0xb9a0ff;
    if (sb_has(codec, "aptx")) return on_light ? 0x1b62bf : 0x7fbaff;
    if (sb_has(codec, "aac")) return on_light ? 0x0a7a6f : 0x62d8ca;
    return 0;
}
/* the glyph's colour: its codec's, or the ink */
static uint32_t sb_bt_ink(void)
{
    uint32_t t = bt_codec_tint(sb_bt_codec(), lv_color_luminance(lv_color_hex(sb_ink)) < 128);
    return t ? t : sb_ink;
}
static void sb_set_ink(uint32_t c)
{
    sb_ink = c;
    lv_obj_set_style_text_color(sb_vol, lv_color_hex(global_status.sloppy_volume_on_bt == 1 ? sb_bt_ink() : c), 0);
    lv_obj_set_style_text_color(sb_vol_n, lv_color_hex(c), 0);
    lv_obj_set_style_text_color(sb_clock, lv_color_hex(c), 0);
    lv_obj_set_style_text_color(sb_batt, lv_color_hex(c), 0);
    lv_obj_set_style_text_color(sb_radio, lv_color_hex(c), 0);
    lv_obj_set_style_text_color(sb_bt, lv_color_hex(sb_bt_ink()), 0);
}
/* On home the status bar sits on the wallpaper: the photos are all dark,
 * so it is white there even in the light theme, until a page covers home. */
static void statusbar_retheme(void)
{
    if (!sb) return;
    sb_on_wall = npages <= 1 && wallpaper_id(false) >= 5;
    uint32_t c = sb_on_wall || (np_open && np_status_on()) ? 0xffffff : C_TEXT;   /* over Now Playing's cover: white */
    /* a page with its own dark stage in the light theme (the games, the
     * quiz) or its own light one in a dark theme: ink for what is under it */
    if (np_open && np_status_on()) {}
    else if (!sb_on_wall && npages > 1 && pages[npages - 1] == sb_ink_page) c = sb_ink_color;   /* it chose its own */
    else if (!sb_on_wall && npages > 1 && lv_obj_get_style_bg_opa(pages[npages - 1], LV_PART_MAIN) >= LV_OPA_50)
        c = lv_color_luminance(lv_obj_get_style_bg_color(pages[npages - 1], LV_PART_MAIN)) < 128 ? 0xffffff : 0x182735;
    sb_set_ink(c);
}
/* a light page (Flip a Coin) darkens the status bar until it is deleted */
static void statusbar_ink(uint32_t color)
{
    if (!sb) return;
    sb_ink_color = color;
    sb_ink_pending = true;
    sb_on_wall = false;
    sb_set_ink(color);
}
static void statusbar_show(bool on)
{
    if (on) lv_obj_remove_flag(sb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(sb, LV_OBJ_FLAG_HIDDEN);
}
/* Percentages round both ways, and a volume set from one lands on the
 * DAC's own half-dB grid: a slider left at 33 plays what the 33 beside it
 * says, and reads 33 again. (120 steps share 101 percentages, so a press
 * now and then keeps the number and is still a step louder.) */
static int volume_pct(void)
{
    int mn = sound_min(SOUND_VOLUME), mx = sound_max(SOUND_VOLUME);
    int v = global_status.volume < mn ? mn : global_status.volume > mx ? mx : global_status.volume;
    return mx > mn ? ((v - mn) * 100 + (mx - mn) / 2) / (mx - mn) : 0;
}
static int volume_snap(int v)
{
    int mn = sound_min(SOUND_VOLUME), mx = sound_max(SOUND_VOLUME), st = sound_steps(SOUND_VOLUME);
    if (v <= mn) return mn;
    if (v >= mx) return mx;
    return mn + (v - mn + st / 2) / st * st;
}
static int volume_from_pct(int pct)
{
    int mn = sound_min(SOUND_VOLUME), mx = sound_max(SOUND_VOLUME);
    return volume_snap(mn + ((mx - mn) * pct + 50) / 100);
}
static void statusbar_build(void)
{
    sb = box(lv_layer_top());
    lv_obj_set_size(sb, W, SB_H);
    sb_vol = text(sb, "", &ss_semibold_21, 0xffffff);   /* the output's glyph */
    lv_obj_align(sb_vol, LV_ALIGN_LEFT_MID, 16, 0);
    sb_vol_n = text(sb, "", &ss_semibold_21, 0xffffff);   /* and its level, a space after it */
    sb_clock = text(sb, "", &ss_semibold_21, 0xffffff);
    sb_batt = text(sb, "", &ss_semibold_21, 0xffffff);
    sb_radio = text(sb, "", &ss_semibold_21, 0xffffff);   /* play state and Wi-Fi, while connected */
    sb_bt = text(sb, LV_SYMBOL_BLUETOOTH, &ss_semibold_21, 0xffffff);   /* Bluetooth, in its codec's colour */
    lv_obj_add_flag(sb_bt, LV_OBJ_FLAG_HIDDEN);
    /* charging: a lime bolt struck across the battery, on a dark stroke so
     * it reads over the battery's white fill */
    static const lv_point_precise_t bolt[] = { { 8, 1 }, { 3, 8 }, { 9, 8 }, { 4, 15 } };
    sb_bolt = box(sb);
    lv_obj_set_size(sb_bolt, 12, 16);
    lv_obj_add_flag(sb_bolt, LV_OBJ_FLAG_HIDDEN);
    for (int k = 0; k < 2; k++)
    {
        lv_obj_t *l = lv_line_create(sb_bolt);
        lv_line_set_points(l, bolt, 4);
        lv_obj_set_style_line_width(l, k ? 2 : 5, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_set_style_line_color(l, lv_color_hex(k ? 0x7cff4f : 0x0b0d10), 0);
    }
}
/* the time as Settings > Time and power says: 24 hour, or 12 hour */
static void clock_text(char *b, int n, const struct tm *tm)
{
    if (global_settings.timeformat)
        snprintf(b, n, "%d:%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min);
    else
        snprintf(b, n, "%02d:%02d", tm->tm_hour, tm->tm_min);
}
/* Where the clock sits (Time and power > Clock position, P61): after the
 * volume, in the middle, or at the far right with the battery before it.
 * The rest of the bar keeps its order around them. */
static int sb_clock_pos(void) { int c = global_settings.sloppy_clock_pos; return c >= 0 && c <= 2 ? c : 0; }
static void sb_place(void)
{
    int c = sb_clock_pos();
    /* only on a change: a style set again redraws the bar */
    static int placed = -1;
    static int32_t placed_w;
    const char *ct = lv_label_get_text(sb_clock), *bt = lv_label_get_text(sb_batt);
    /* at the right, the battery hangs off the clock: either's width moves it */
    int32_t w = c == 2 ? lv_text_get_width(ct, strlen(ct), &ss_semibold_21, 0) * 1000 +
                         lv_text_get_width(bt, strlen(bt), &ss_semibold_21, 0) : 0;
    if (c == placed && w == placed_w) return;
    placed = c;
    placed_w = w;
    if (c == 0) lv_obj_align(sb_clock, LV_ALIGN_LEFT_MID, 96, 0);
    else if (c == 1) lv_obj_align(sb_clock, LV_ALIGN_CENTER, 0, 0);
    else lv_obj_align(sb_clock, LV_ALIGN_RIGHT_MID, -16, 0);
    if (c == 2)
    {
        lv_obj_update_layout(sb);
        lv_obj_align_to(sb_batt, sb_clock, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    }
    else lv_obj_align(sb_batt, LV_ALIGN_RIGHT_MID, -16, 0);
}
static void statusbar_update(void)
{
    char b[32];
    /* the level is Bluetooth's own while headphones play (see sloppy_bluetooth.inc) */
    int out = global_status.sloppy_volume_on_bt;
    const lv_font_t *sf = &ss_semibold_21;
    int32_t space = lv_text_get_width(" ", 1, sf, 0);
    label_if_changed(sb_vol, out == 2 ? LV_SYMBOL_USB : out ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_VOLUME_MAX);
    snprintf(b, sizeof b, "%d", volume_pct());
    label_if_changed(sb_vol_n, b);
    const char *vt = lv_label_get_text(sb_vol);
    lv_obj_align(sb_vol_n, LV_ALIGN_LEFT_MID, 16 + lv_text_get_width(vt, strlen(vt), sf, 0) + space, 0);
    clock_text(b, sizeof b, get_time());
    label_if_changed(sb_clock, b);
    int level = battery_level();
    const char *icon = level > 80 ? LV_SYMBOL_BATTERY_FULL : level > 55 ? LV_SYMBOL_BATTERY_3 :
                       level > 30 ? LV_SYMBOL_BATTERY_2 : level > 10 ? LV_SYMBOL_BATTERY_1 :
                       LV_SYMBOL_BATTERY_EMPTY;
    if (global_settings.battery_display) snprintf(b, sizeof b, "%d%% %s", level, icon);
    else strlcpy(b, icon, sizeof b);
    label_if_changed(sb_batt, b);
    sb_place();
    /* what the player is linked to, left of the battery; Bluetooth only when
     * the volume beside the clock is not already saying so */
    bool bt = sb_bt_linked() && global_status.sloppy_volume_on_bt != 1;
    /* and whether a song plays or waits, first */
    const char *pp = !audio_current_track() ? "" : is_playing() ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE;
    snprintf(b, sizeof b, "%s%s%s", pp, pp[0] && (wf_online() || bt) ? "  " : "", wf_online() ? LV_SYMBOL_WIFI : "");
    if (strcmp(lv_label_get_text(sb_radio), b))
    {
        lv_label_set_text(sb_radio, b);
        lv_obj_update_layout(sb);
    }
    hidden_if_changed(sb_bt, !bt);
    /* the codec may change under the same glyph */
    uint32_t bi = sb_bt_ink();
    if (!lv_color_eq(lv_obj_get_style_text_color(sb_bt, 0), lv_color_hex(bi))) lv_obj_set_style_text_color(sb_bt, lv_color_hex(bi), 0);
    uint32_t vi = out == 1 ? bi : sb_ink;
    if (!lv_color_eq(lv_obj_get_style_text_color(sb_vol, 0), lv_color_hex(vi))) lv_obj_set_style_text_color(sb_vol, lv_color_hex(vi), 0);
    if (bt)
    {
        lv_obj_align_to(sb_bt, sb_batt, LV_ALIGN_OUT_LEFT_MID, -10, 0);
        lv_obj_align_to(sb_radio, sb_bt, LV_ALIGN_OUT_LEFT_MID, wf_online() ? -space : 0, 0);
    }
    else lv_obj_align_to(sb_radio, sb_batt, LV_ALIGN_OUT_LEFT_MID, -10, 0);
#if CONFIG_CHARGING
    bool charging = charger_inserted();
#else
    bool charging = false;
#endif
    if (charging)
    {
        /* over the middle of the battery glyph, the label's last character */
        const lv_font_t *f = lv_obj_get_style_text_font(sb_batt, 0);
        int32_t gw = lv_text_get_width(icon, strlen(icon), f, 0);
        lv_obj_update_layout(sb);
        lv_obj_align_to(sb_bolt, sb_batt, LV_ALIGN_RIGHT_MID, -(gw - 12) / 2 - 2, 0);
    }
    hidden_if_changed(sb_bolt, !charging);
}
#include "sloppy_devhud.inc"

/* ---------------------------------------------------------- library UI */
static void album_open_cb(lv_event_t *e);

/* All songs: user data is the position in songs_by_title */
static void song_all_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= sl_lib.n_tracks) return;
    if (play_list(sl_lib.songs_by_title, sl_lib.n_tracks, idx, false)) np_show_auto();
}
/* Album rows: user data is album << 16 | position, both non-negative */
static void song_album_cb(lv_event_t *e)
{
    uintptr_t v = (uintptr_t)lv_event_get_user_data(e);
    int a = (int)(v >> 16), idx = (int)(v & 0xffff);
    if (a < 0 || a >= sl_lib.n_albums || idx >= sl_lib.albums[a].n_tracks) return;
    if (play_album(a, idx, false)) np_show_auto();
}

/* The labels are laid out before the row knows its width (a list with
 * the A-Z rail is narrower): size them once it does, so a title ends
 * before the dots instead of under them. Their lead is from the leading
 * edge, either side (the number, at 0, keeps its width). */
static void song_row_sized(lv_event_t *e)
{
    lv_obj_t *r = lv_event_get_target(e);
    for (uint32_t i = 0; i < lv_obj_get_child_count(r); i++)
    {
        lv_obj_t *l = lv_obj_get_child(r, i);
        int lead = LV_ABS(lv_obj_get_style_x(l, 0));
        if (lv_obj_check_type(l, &lv_label_class) && lead >= SL_ROW_NUM_X)
            lv_obj_set_width(l, lv_obj_get_width(r) - lead - SL_ROW_END);
    }
}
/* The song playing, in any list it is in: its title in the accent and
 * three bars rising and falling where its number or cover was. */
static void now_bar_h(void *o, int32_t v) { lv_obj_set_height(o, v); }
static lv_obj_t *now_bars(lv_obj_t *parent, uint32_t ink)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, 3 * 5 + 2 * 3, 20);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    bool moving = is_playing();
    static const uint16_t ms[] = { 420, 330, 510 };
    static const uint8_t still[] = { 12, 20, 8 };
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *bar = box(b);
        lv_obj_set_size(bar, 5, still[i]);
        lv_obj_set_style_radius(bar, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(ink), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, i * 8, 0);
        if (!moving) continue;            /* paused: the bars hold still */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, bar);
        lv_anim_set_values(&a, 4, 20);
        lv_anim_set_duration(&a, ms[i]);
        lv_anim_set_playback_duration(&a, ms[i]);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, now_bar_h);
        lv_anim_start(&a);
    }
    return b;
}
/* "  ·  FLAC": a lossless song's own format, by its file */
static void lossless_badge(char *b, size_t n, const char *path)
{
    const char *ext = strrchr(path, '.'), *name = NULL;
    if (!ext) return;
    static const char *const map[][2] = { { ".flac", "FLAC" }, { ".wav", "WAV" }, { ".aiff", "AIFF" }, { ".aif", "AIFF" },
                                          { ".ape", "APE" }, { ".wv", "WavPack" }, { ".dsf", "DSD" }, { ".dff", "DSD" } };
    for (unsigned i = 0; i < sizeof map / sizeof map[0] && !name; i++)
        if (!strcasecmp(ext, map[i][0])) name = map[i][1];
    if (!name) return;
    strlcat(b, "  \xc2\xb7  ", n);
    strlcat(b, name, n);
}
static lv_obj_t *song_row(lv_obj_t *parent, int track, const char *left, intptr_t ud)
{
    const struct sl_track *t = &sl_lib.tracks[track];
    bool now = track == np_love_track;
    lv_obj_t *r = box(parent);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_add_style(r, &st_row_pr, LV_STATE_PRESSED);
    lv_obj_set_size(r, lv_pct(100), SL_ROW_H);
    if (left)
    {
        if (now) sl_align(now_bars(r, C_ACCENT), LV_ALIGN_LEFT_MID, 10, 0);
        else
        {
            lv_obj_t *n = text(r, left, &ss_regular_18, C_DIM);
            lv_obj_set_width(n, 40);
            lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
            sl_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        }
    }
    else
    {
        /* Hebrew and Arabic: the row mirrored, the cover at the right */
        lv_obj_t *c = cover(r, t->album, SL_ART_ROW, false);
        sl_align(c, LV_ALIGN_LEFT_MID, SL_ROW_ART_X, 0);
        if (now)
        {
            /* the cover dimmed under the bars */
            lv_obj_t *dim = box(r);
            lv_obj_set_size(dim, SL_ART_ROW, SL_ART_ROW);
            sl_align(dim, LV_ALIGN_LEFT_MID, SL_ROW_ART_X, 0);
            lv_obj_set_style_radius(dim, lv_obj_get_style_radius(c, 0), 0);
            lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(dim, LV_OPA_50, 0);
            lv_obj_remove_flag(dim, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(now_bars(dim, 0xffffff));
        }
    }
    /* the two lines stacked by their fonts' heights, so the bigger text
     * sizes still fit the row's height */
    int lh1 = lv_font_get_line_height(&ss_semibold_23), lh2 = lv_font_get_line_height(&ss_regular_18);
    int ty = (SL_ROW_H - lh1 - lh2) / 2;
    if (ty < 0) ty = 0;
    lv_obj_t *t1 = text(r, t->title, &ss_semibold_23, now ? C_ACCENT_TXT : 0xffffff);   /* the accent as words */
    /* the width song_row_sized keeps once the row has its own (a list
     * without the rail is this wide, so nothing moves) */
    int lead = left ? SL_ROW_NUM_X : SL_ROW_TXT_X, tw = W - 2 * SL_GUTTER - lead - SL_ROW_END;
    sl_place(t1, lead, ty);
    lv_obj_set_width(t1, tw);
    lv_obj_set_height(t1, lh1);
    lv_label_set_long_mode(t1, LV_LABEL_LONG_DOT);
    char b[160];
    if (left)
    {
        /* a compilation's song (or a guest's) says whose it is, before its length */
        int n = 0;
        if (t->artist && strcasecmp(t->artist, sl_lib.albums[t->album].artist))
            n = snprintf(b, sizeof b, "%s  \xc2\xb7  ", t->artist);
        fmt_time(b + LV_MIN(n, (int)sizeof b - 1), sizeof b - LV_MIN(n, (int)sizeof b - 1), t->len);
        if (t->lossless) lossless_badge(b, sizeof b, t->file);   /* by its name's extension */
    }
    else
    {
        strlcpy(b, sl_track_artist(track), sizeof b);
        if (t->lossless) lossless_badge(b, sizeof b, t->file);   /* by its name's extension */
    }
    lv_obj_t *t2 = text(r, b, &ss_regular_18, C_SUB);
    sl_place(t2, lead, ty + lh1);
    lv_obj_set_width(t2, tw);
    lv_obj_set_height(t2, lh2);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_DOT);
    /* every title and artist starts by the cover, whatever its script: in a
     * left-to-right interface a Hebrew one doesn't jump across to the dots,
     * and in Hebrew or Arabic (sl_place, from the right) a Latin one doesn't
     * either (the words still read their own way) */
    if (!sl_ui_rtl())
    {
        lv_obj_set_style_text_align(t1, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_LEFT, 0);
    }
    lv_obj_add_event_cb(r, song_row_sized, LV_EVENT_SIZE_CHANGED, NULL);
    lv_obj_add_event_cb(r, sel_tap_cb, LV_EVENT_CLICKED, NULL);   /* first: picking a song is not playing it */
    on_click(r, left ? song_album_cb : song_all_cb, (void *)ud);
    lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_user_data(r, (void *)(intptr_t)track);
    lv_obj_add_event_cb(r, row_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    sw_attach(r, track);    /* swipe left to queue, right to add to a playlist */
    if (sel_picking(parent)) sel_mark(r, track);   /* picking songs: a mark where the dots were */
    else
    {
        lv_obj_t *more = icon_btn(r, NULL, SL_ROW_MORE_W, kebab_cb, (void *)(intptr_t)track);   /* three dots: more */
        sl_align(more, LV_ALIGN_RIGHT_MID, -SL_ROW_MORE_IN, 0);
    }
    return r;
}

/* Keep long song lists bounded: only visible rows and a small overscan
 * are instantiated. Covers/events are released as rows leave the pool. */
struct song_list_view {
    lv_obj_t *body, *host, *rows[14];
    const int *tracks;
    int *owned;              /* SL_LIST_CUSTOM: the list, freed with the view */
    int count, album, indexes[14];
    int *heads;              /* an album of discs: the disc headings above each row, counted; else NULL */
    bool binding;
};
#define SL_DISC_H 48
/* where row k sits, below the disc headings before it */
static int song_row_y(const struct song_list_view *v, int k) { return k * SL_ROW_PITCH + (v->heads ? v->heads[k] * SL_DISC_H : 0); }
/* album value for a list of any songs (smart playlists, search results):
 * rows look like All songs, and a tap plays this list from that row */
#define SL_LIST_CUSTOM (-2)
static void song_custom_cb(lv_event_t *e)
{
    struct song_list_view *v = lv_event_get_user_data(e);
    int k = lv_obj_get_y(lv_event_get_current_target(e)) / SL_ROW_PITCH;
    if (k < 0 || k >= v->count) return;
    if (play_list(v->tracks, v->count, k, false)) np_show_auto();
}
static void song_list_bind(struct song_list_view *v)
{
    if (v->binding) return;
    v->binding = true;
    int y = lv_obj_get_scroll_y(v->body) - lv_obj_get_y(v->host);
    int first = y / SL_ROW_PITCH;
    while (first > 0 && song_row_y(v, first) > y) first--;
    first -= 2;
    if (first < 0) first = 0;
    if (first > v->count - 14) first = v->count > 14 ? v->count - 14 : 0;
    for (int k = first; k < v->count && k < first + 14; k++)
    {
        int slot = k % 14;
        if (v->indexes[slot] == k) continue;
        if (v->rows[slot]) lv_obj_delete(v->rows[slot]);
        char number[16];
        int track = v->tracks[k];
        snprintf(number, sizeof number, "%d", sl_lib.tracks[track].num > 0 ? sl_lib.tracks[track].num : k + 1);
        intptr_t data = v->album < 0 ? k : (intptr_t)(((uintptr_t)v->album << 16) | (unsigned)k);
        v->rows[slot] = song_row(v->host, track, v->album < 0 ? NULL : number, data);
        lv_obj_set_pos(v->rows[slot], 0, song_row_y(v, k));
        if (v->album == SL_LIST_CUSTOM)
        {
            lv_obj_remove_event_cb(v->rows[slot], song_all_cb);
            lv_obj_add_event_cb(v->rows[slot], song_custom_cb, LV_EVENT_CLICKED, v);
        }
        v->indexes[slot] = k;
    }
    v->binding = false;
}
static void song_list_scroll(lv_event_t *e) { song_list_bind(lv_event_get_user_data(e)); }
/* the lists on screen, so a new song can be marked in them */
static struct song_list_view *song_views[8];
static void song_lists_mark(int was, int now)
{
    for (int i = 0; i < 8; i++)
    {
        struct song_list_view *v = song_views[i];
        if (!v) continue;
        bool any = false;
        for (int slot = 0; slot < 14; slot++)
        {
            int k = v->indexes[slot];
            if (k < 0 || !v->rows[slot] || (v->tracks[k] != was && v->tracks[k] != now)) continue;
            lv_obj_delete(v->rows[slot]);
            v->rows[slot] = NULL;
            v->indexes[slot] = -1;
            any = true;
        }
        if (any) song_list_bind(v);
    }
}
static void song_list_delete(lv_event_t *e)
{
    struct song_list_view *v = lv_event_get_user_data(e);
    for (int i = 0; i < 8; i++) if (song_views[i] == v) song_views[i] = NULL;
    lv_obj_remove_event_cb_with_user_data(v->body, song_list_scroll, v);
    sel_forget(v);
    free(v->owned);
    free(v->heads);
    free(v);
}
static struct song_list_view *song_list(lv_obj_t *body, const int *tracks, int count, int album)
{
    if (count <= 0) return NULL;
    struct song_list_view *v = calloc(1, sizeof(*v));
    if (!v) { toast_warn("Not enough memory to show songs"); return NULL; }
    v->body = body; v->tracks = tracks; v->count = count; v->album = album;
    for (int i = 0; i < 14; i++) v->indexes[i] = -1;
    v->host = box(body);
    lv_obj_set_user_data(v->host, v);    /* a row finds its list (selecting songs) */
    /* an album on more than one disc: "Disc 2" over the first song of each */
    int discs = 0;
    for (int k = 1; album >= 0 && k < count; k++)
        if (sl_lib.tracks[tracks[k]].disc != sl_lib.tracks[tracks[k - 1]].disc) discs++;
    if (discs && (v->heads = malloc((size_t)count * sizeof *v->heads)))
        for (int k = 0, h = 0; k < count; k++)
        {
            if (!k || sl_lib.tracks[tracks[k]].disc != sl_lib.tracks[tracks[k - 1]].disc)
            {
                v->heads[k] = ++h;
                char b[24];
                int d = sl_lib.tracks[tracks[k]].disc;
                if (d > 0) snprintf(b, sizeof b, "%s %d", sl_tr("DISC"), d); else strlcpy(b, sl_tr("OTHER SONGS"), sizeof b);
                lv_obj_t *l = text(v->host, b, &ss_semibold_18, C_SUB);
                lv_obj_set_pos(l, 6, song_row_y(v, k) - SL_DISC_H + 16);
            }
            else v->heads[k] = h;
        }
    lv_obj_set_size(v->host, lv_pct(100), count * SL_ROW_PITCH + (v->heads ? v->heads[count - 1] * SL_DISC_H : 0));
    lv_obj_add_event_cb(v->host, song_list_delete, LV_EVENT_DELETE, v);
    lv_obj_add_event_cb(body, song_list_scroll, LV_EVENT_SCROLL, v);
    for (int i = 0; i < 8; i++) if (!song_views[i]) { song_views[i] = v; break; }
    lv_obj_update_layout(body);
    lv_obj_send_event(body, LV_EVENT_SCROLL, NULL);
    return v;
}

static void pill_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int album = v >> 1;
    if (play_album(album, 0, v & 1)) np_show_auto();
}
static lv_obj_t *pill(lv_obj_t *parent, const char *label, bool primary, intptr_t ud)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_height(b, 56);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, SL_R_PILL, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT : C_CARD), 0);
    /* pressed: the accent a shade deeper, whatever the accent is */
    lv_obj_set_style_bg_color(b, primary ? lv_color_darken(lv_color_hex(C_ACCENT), 50) : lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    press_motion(b);
    if (!primary) hairline(b);
    lv_obj_center(text(b, label, &ss_semibold_21, primary ? sl_accent_ink() : 0xffffff));
    on_click(b, pill_cb, (void *)ud);
    return b;
}

/* An album's quality, by its first song: Lossy, CD (16 bits, 48 kHz at
 * most), Hi-Res past either, DSD, or Lossless when the file does not say.
 * Read once an album (FLAC's STREAMINFO, a WAV's fmt chunk or WavPack's
 * flags, the first 36 bytes) and kept while the library is the same. */
enum { AQ_UNREAD, AQ_LOSSY, AQ_CD, AQ_HIRES, AQ_DSD, AQ_LOSSLESS };
static uint8_t *album_q;
static const struct sl_album *album_q_lib;
static int album_q_n;
static int album_quality(int a)
{
    if (album_q_lib != sl_lib.albums || album_q_n != sl_lib.n_albums)
    {
        free(album_q);
        album_q = calloc(sl_lib.n_albums ? sl_lib.n_albums : 1, 1);
        album_q_lib = sl_lib.albums;
        album_q_n = sl_lib.n_albums;
    }
    if (!album_q || a < 0 || a >= sl_lib.n_albums || !sl_lib.albums[a].n_tracks) return AQ_UNREAD;
    if (album_q[a]) return album_q[a];
    const struct sl_track *t = &sl_lib.tracks[sl_lib.albums[a].tracks[0]];
    const char *ext = t->file ? strrchr(t->file, '.') : NULL;
    int q = AQ_LOSSLESS;
    if (ext && (!strcasecmp(ext, ".dsf") || !strcasecmp(ext, ".dff"))) q = AQ_DSD;
    else if (!t->lossless) q = AQ_LOSSY;
    else
    {
        unsigned char h[36];
        unsigned long rate = 0;
        char path[MAX_PATH];
        int bits = 0, fd = open(sl_track_path(sl_lib.albums[a].tracks[0], path, sizeof path), O_RDONLY);
        if (fd >= 0)
        {
            if (read(fd, h, sizeof h) == (ssize_t)sizeof h)
            {
                if (!memcmp(h, "fLaC", 4) && (h[4] & 0x7f) == 0)
                {
                    rate = (unsigned long)h[18] << 12 | h[19] << 4 | h[20] >> 4;
                    bits = ((h[20] & 1) << 4 | h[21] >> 4) + 1;
                }
                else if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8))
                {
                    rate = h[24] | h[25] << 8 | (unsigned long)h[26] << 16;
                    bits = h[34] | h[35] << 8;
                }
                else if (!memcmp(h, "wvpk", 4) && (h[27] & 0x80))
                    q = AQ_DSD;   /* WavPack's DSD flag */
            }
            close(fd);
        }
        if (rate && bits) q = rate > 48000 || bits > 16 ? AQ_HIRES : AQ_CD;
    }
    return album_q[a] = (uint8_t)q;
}
static void pl_open_many(const int *tracks, int n);
static void album_add_cb(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (a >= 0 && a < sl_lib.n_albums) pl_open_many(sl_lib.albums[a].tracks, sl_lib.albums[a].n_tracks);
}
/* the quality as a small tag in the cover's top corner (Library options) */
static void album_badge(lv_obj_t *art, int a)
{
    if (!global_settings.sloppy_album_badges) return;
    static const char *const names[] = { NULL, "Lossy", "CD", "Hi-Res", "DSD", "Lossless" };
    int q = album_quality(a);
    if (!names[q]) return;
    lv_obj_t *b = box(art);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, sl_zune() ? 0 : SL_R_XS, 0);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, 150, 0);
    lv_obj_set_style_pad_hor(b, 7, 0);
    lv_obj_set_style_pad_ver(b, 1, 0);
    lv_obj_t *l = text_light(b, names[q], &ss_semibold_18);
    lv_obj_set_style_text_color(l, lv_color_hex(q == AQ_HIRES ? 0xffd479 : 0xffffff), 0);   /* Hi-Res in gold */
    /* inside the cover whatever the word and size: a long one ends in an ellipsis */
    int room = lv_obj_get_style_width(art, 0) - 16 - 14;
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(l), &ss_semibold_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > room)
    {
        lv_obj_set_size(l, room, lv_font_get_line_height(&ss_semibold_18));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8, 8);
}

/* Big cover: the thumbnail scaled up at once, then the cover itself as
 * the cover thread makes it (a timer asks a few times a second); its
 * pixels go with the page */
struct album_big { lv_obj_t *box; int album; lv_timer_t *timer; lv_image_dsc_t dsc; uint16_t *px; int tries; };
static void album_big_deleted(lv_event_t *e)
{
    struct album_big *ab = lv_event_get_user_data(e);
    if (ab->timer) lv_timer_delete(ab->timer);
    if (ab->px) { lv_image_cache_drop(&ab->dsc); free(ab->px); }
    free(ab);
}
static void album_big_fade(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void album_big_poll(lv_timer_t *t)
{
    struct album_big *ab = lv_timer_get_user_data(t);
    uint16_t *px;
    int w, h, got = sl_art_big_poll(ab->album, &px, &w, &h);
    if (got == 0 && ++ab->tries < 120) return;           /* still making it, for up to 30 s */
    lv_timer_delete(t);
    ab->timer = NULL;
    if (got != 1) return;                                 /* the scaled thumbnail stays */
    ab->px = px;
    memset(&ab->dsc, 0, sizeof ab->dsc);
    ab->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ab->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    ab->dsc.header.w = w;
    ab->dsc.header.h = h;
    ab->dsc.header.stride = w * 2;
    ab->dsc.data = (const uint8_t *)px;
    ab->dsc.data_size = (uint32_t)w * h * 2;
    /* in place of what was there, faded up over it */
    lv_obj_t *img = lv_image_create(ab->box);
    lv_image_set_src(img, &ab->dsc);
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(img, lv_obj_get_style_radius(ab->box, 0), 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, img);
    lv_anim_set_values(&an, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&an, SL_T_SLOW);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&an, album_big_fade);
    lv_anim_start(&an);
}
static void album_big_cover(lv_obj_t *box, int a)
{
    if (!sl_art_apply_scaled(box, a, lv_obj_get_style_width(box, 0))) return;   /* no cover: the placeholder, nothing to sharpen */
    for (uint32_t i = 0; i < lv_obj_get_child_count(box); i++)            /* the note under it goes */
        if (!lv_obj_check_type(lv_obj_get_child(box, i), &lv_image_class)) { lv_obj_delete(lv_obj_get_child(box, i)); i--; }
    struct album_big *ab = calloc(1, sizeof *ab);
    if (!ab) return;
    ab->box = box;
    ab->album = a;
    sl_art_big_start(a, lv_obj_get_style_width(box, 0));
    ab->timer = lv_timer_create(album_big_poll, 250, ab);
    lv_obj_add_event_cb(box, album_big_deleted, LV_EVENT_DELETE, ab);
}
static bool fav_has(int a);
static void fav_set(int a, bool on);
static void album_fav_paint(lv_obj_t *b, bool on)
{
    lv_obj_t *ic = lv_obj_get_child(b, 1);
    lv_obj_set_style_text_color(ic, lv_color_hex(on ? C_ACCENT_TXT : C_TEXT), 0);
    lv_obj_set_style_opa(ic, on ? LV_OPA_COVER : LV_OPA_60, 0);
    lv_obj_invalidate(ic);
}
static void album_fav_cb(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = !fav_has(a);
    fav_set(a, on);
    album_fav_paint(lv_event_get_current_target(e), fav_has(a));
    sfx_play(on ? SFX_UI_ON : SFX_UI_OFF);
    notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, NULL, on ? "Added to Favourite albums" : "Removed from Favourite albums");
}
static void album_page(int a)
{
    sl_log("album page %d", a);
    const struct sl_album *al = &sl_lib.albums[a];
    lv_obj_t *body = page_new("", NULL);
    lv_obj_t *p = lv_obj_get_parent(body);

    /* The album as a hero: its cover large in the middle on a wash of its
     * own colour that fades into the page, the words centred under it.
     * The wash spans the screen edge to edge (the body is inset 14). */
    lv_obj_t *head = box(body);
    lv_obj_set_size(head, W, LV_SIZE_CONTENT);
    lv_obj_set_style_translate_x(head, -14, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(head, 6, 0);
    lv_obj_set_style_pad_bottom(head, 18, 0);
    lv_obj_set_style_pad_hor(head, 28, 0);
    lv_obj_set_style_pad_row(head, 6, 0);
    bool big = global_settings.sloppy_album_layout == 1 && !sl_zune();
    if (!sl_zune() && !big)                  /* Zune: words on black, no wash; Big cover: the cover is the colour */
    {
        /* the cover's own colour once it has a thumbnail; a grey sleeve, a grey wash */
        int hue = al->hue, sat = 100;
        if (!sl_art_hue(a, &hue, &sat)) hue = al->hue;
        lv_color_t wash = sl_theme() == 1 ? lv_color_hsv_to_rgb(hue, 4 + sat * 26 / 100, 94)
                                          : lv_color_hsv_to_rgb(hue, 6 + sat * 52 / 100, sl_theme() == 2 ? 24 : 36);
        /* on the page itself, behind its header and the status bar, so it
         * runs up to the top edge with no seam, fading out under the words */
        lv_obj_set_style_bg_color(p, wash, 0);
        lv_obj_set_style_bg_grad_color(p, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_stop(p, 36, 0);
        lv_obj_set_style_bg_grad_stop(p, 180, 0);
        for (uint32_t i = 0; i < lv_obj_get_child_count(p); i++)
        {
            /* the header keeps covering what scrolls under it, in the wash */
            lv_obj_t *o = lv_obj_get_child(p, i);
            if (o == body) lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            else lv_obj_set_style_bg_color(o, wash, 0);
            lv_obj_set_style_border_width(o, 0, 0);
            sl_elevate(o, SL_E0, 0);
        }
    }
    /* Library options > Album page > Big cover: the cover across the page,
     * the words under it from its edge; the cover's own pixels come from
     * the cover thread, the thumbnail scaled up showing until they do */
    int cs = big ? W - 2 * SL_GUTTER : 220;
    if (big)
    {
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_hor(head, SL_GUTTER, 0);
        lv_obj_set_style_pad_top(head, 0, 0);
    }
    lv_obj_t *c = cover(head, a, cs, true);
    lv_obj_set_style_margin_bottom(c, 8, 0);
    sl_elevate(c, sl_zune() ? SL_E0 : SL_E3, 0);   /* the cover stands off the page */
    if (big) album_big_cover(c, a);
    lv_obj_t *t = text(head, al->title, sl_zune() ? sl_zune_font(al->title, &ss_regular_34) : &ss_bold_28, 0xffffff);
    lv_obj_set_width(t, big ? cs : W - 56);
    lv_obj_set_style_text_align(t, big ? LV_TEXT_ALIGN_AUTO : LV_TEXT_ALIGN_CENTER, 0);
    /* two lines at most, the second ending in an ellipsis: a third was cut
     * in half and ran into the artist */
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    note_lines(t, al->title, W - 56, 2);   /* the title itself: LONG_DOT had already put its dots in the label's copy */
    sl_label_deep(t);
    lv_obj_t *ar = text(head, al->artist, &ss_semibold_18, C_SUB);
    lv_obj_set_width(ar, big ? cs : W - 56);
    lv_obj_set_style_text_align(ar, big ? LV_TEXT_ALIGN_AUTO : LV_TEXT_ALIGN_CENTER, 0);
    label_lines(ar, 1);
    long long total = 0;
    for (int i = 0; i < al->n_tracks; i++) total += sl_lib.tracks[al->tracks[i]].len;
    char b[128], sg[40], mn[32];
    sl_count(sg, sizeof sg, al->n_tracks, "%s song", "%s songs");
    sl_count(mn, sizeof mn, (int)(total / 60000), "%s min", "%s min");
    int n = 0;
    if (al->genre && al->genre[0]) n = snprintf(b, sizeof b, "%.20s · ", al->genre);
    if (al->year) snprintf(b + n, sizeof b - n, "%d · %s · %s", al->year, sg, mn);
    else snprintf(b + n, sizeof b - n, "%s · %s", sg, mn);
    lv_obj_t *m = text(head, b, &ss_regular_16, C_SUB);   /* on the cover's wash C_DIM fell under 4.5:1 */
    lv_obj_set_style_text_align(m, big ? LV_TEXT_ALIGN_AUTO : LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *pl = box(body);
    lv_obj_set_size(pl, lv_pct(100), 72);
    lv_obj_set_flex_flow(pl, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pl, 12, 0);
    lv_obj_set_style_pad_hor(pl, 6, 0);
    lv_obj_set_style_pad_top(pl, 4, 0);
    pill(pl, LV_SYMBOL_PLAY "  Play", true, (intptr_t)(a << 1));
    pill(pl, LV_SYMBOL_SHUFFLE "  Shuffle", false, (intptr_t)(a << 1 | 1));

    song_list(body, al->tracks, al->n_tracks, a);
    /* the whole album into a playlist, from the header (#114) */
    header_button(p, LV_SYMBOL_PLUS, album_add_cb, (void *)(intptr_t)a);
    /* and a favourite (Favourite albums), the heart filled in the accent */
    lv_obj_t *fav = header_button(p, "", album_fav_cb, (void *)(intptr_t)a);
    lv_obj_center(sl_icon(fav, IC_HEART, 26, C_TEXT, C_BG));
    album_fav_paint(fav, fav_has(a));
    lv_obj_set_user_data(p, (void *)(intptr_t)(PG_ALBUM | a));
    page_push(p);
}
static void album_open_cb(lv_event_t *e) { album_page((int)(intptr_t)lv_event_get_user_data(e)); }
/* the words under a cover: the title, then the artist (unless Library
 * options hides it) and the year when asked for */
static void album_caption(char *b, size_t n, const struct sl_album *al, bool year)
{
    b[0] = '\0';
    if (!global_settings.sloppy_album_no_artist) strlcpy(b, al->artist, n);
    if (year && al->year) snprintf(b + strlen(b), n - strlen(b), b[0] ? " \xc2\xb7 %d" : "%d", al->year);
}

static void album_grid(lv_obj_t *body, const int *albums, int n)
{
    lv_obj_t *g = box(body);
    lv_obj_set_width(g, lv_pct(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, 14, 0);
    lv_obj_set_style_pad_row(g, 16, 0);
    int cw = (W - 28 - 14) / 2;
    for (int k = 0; k < n; k++)
    {
        int a = albums[k];
        const struct sl_album *al = &sl_lib.albums[a];
        lv_obj_t *cell = box(g);
        char b[80];
        album_caption(b, sizeof b, al, true);
        /* the captions by the fonts in use (cw + 58 at Standard) */
        int th = lv_font_get_line_height(&ss_semibold_18) + 2, sh = b[0] ? lv_font_get_line_height(&ss_regular_16) + 1 : 0;
        lv_obj_set_size(cell, cw, cw + 8 + th + sh + 6);
        album_badge(cover(cell, a, cw, true), a);
        lv_obj_t *t = text(cell, al->title, &ss_semibold_18, 0xffffff);
        lv_obj_set_width(t, cw);
        lv_obj_set_height(t, th);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        sl_label_deep(t);
        lv_obj_set_pos(t, 0, cw + 8);
        if (b[0])
        {
            lv_obj_t *s = text(cell, b, &ss_regular_16, C_SUB);   /* a fixed pale grey vanished on the light theme */
            lv_obj_set_width(s, cw);
            lv_obj_set_height(s, sh);
            lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
            sl_label_deep(s);
            lv_obj_set_pos(s, 0, cw + 8 + th);
        }
        on_click(cell, album_open_cb, (void *)(intptr_t)a);
        lv_obj_add_event_cb(cell, album_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(a << 1));   /* hold: its sheet */
    }
}

#define ALBUM_VSLOTS 24
/* Two covers across with their names, or (Zune) three small ones alone */
static int av_cols(void) { return sl_zune() ? 3 : 2; }
static int av_w(void) { return sl_zune() ? 138 : 199; }
/* a cover, its title and artist, by the fonts in use (246 at Standard) */
static int av_title_h(void) { return lv_font_get_line_height(&ss_semibold_18) + 2; }
static int av_artist_h(void) { return global_settings.sloppy_album_no_artist ? 0 : lv_font_get_line_height(&ss_regular_16) + 1; }
static int av_row(void) { return sl_zune() ? 146 : 179 + av_title_h() + (av_artist_h() ? 5 + av_artist_h() : 0) + 18; }
static int av_slots(void) { return sl_zune() ? 24 : 12; }
struct album_grid_view {
    lv_obj_t *body, *host;
    lv_obj_t *cells[ALBUM_VSLOTS];
    int indexes[ALBUM_VSLOTS];
    int count;
    bool binding;
};
static struct album_grid_view *album_view;
/* The order the Albums page shows: by title, by artist, newest year
 * first, or the albums last added first (an array of its own for the
 * last two, rebuilt when the page opens). */
#define albums_sort global_settings.sloppy_albums_sort   /* kept across restarts */
static int *albums_year_order;
static uint32_t *albums_added;    /* while sorting by it: each album's newest song */
static const int *albums_order(void)
{
    return albums_sort == 1 ? sl_lib.albums_by_artist : albums_sort >= 2 && albums_year_order ? albums_year_order
                                                                                             : sl_lib.albums_by_title;
}
static int cmp_album_added(const void *a, const void *b)
{
    uint32_t x = albums_added[*(const int *)a], y = albums_added[*(const int *)b];
    return x < y ? 1 : x > y ? -1 : strcasecmp(sl_lib.albums[*(const int *)a].title, sl_lib.albums[*(const int *)b].title);
}
static int cmp_album_year(const void *a, const void *b)
{
    int x = sl_lib.albums[*(const int *)a].year, y = sl_lib.albums[*(const int *)b].year;
    return x < y ? 1 : x > y ? -1 : strcasecmp(sl_lib.albums[*(const int *)a].title, sl_lib.albums[*(const int *)b].title);
}
static lv_obj_t *album_virtual_cell(lv_obj_t *host, int position)
{
    int a = albums_order()[position];
    const struct sl_album *al = &sl_lib.albums[a];
    lv_obj_t *cell = box(host);
    lv_obj_set_size(cell, av_w(), av_row() - 8);
    lv_obj_set_pos(cell, (position % av_cols()) * (av_w() + 8),
                   (position / av_cols()) * av_row());
    /* picking albums takes the tap first; a hold opens the album's sheet */
    lv_obj_add_event_cb(cell, sel_cell_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    lv_obj_add_event_cb(cell, album_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(a << 1 | 1));
    lv_obj_t *art = cover(cell, a, sl_zune() ? SL_ART_SMALL : SL_ART_PAGE, !sl_zune());
    lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 0);
    album_badge(art, a);
    if (sel_kind_on(SEL_ALBUMS)) sel_cell_mark(art, a);
    if (!sl_zune())
    {
        lv_obj_t *title = text(cell, al->title, &ss_semibold_18, 0xffffff);
        lv_obj_set_pos(title, 0, 179);
        lv_obj_set_size(title, av_w(), av_title_h());
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        sl_label_deep(title);
        if (av_artist_h())
        {
            lv_obj_t *artist = text(cell, al->artist, &ss_regular_16, C_SUB);
            lv_obj_set_pos(artist, 0, 179 + av_title_h() + 5);
            lv_obj_set_size(artist, av_w(), av_artist_h());
            lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
            sl_label_deep(artist);
        }
    }
    on_click(cell, album_open_cb, (void *)(intptr_t)a);
    return cell;
}
static void album_virtual_scroll(lv_event_t *e)
{
    struct album_grid_view *v = lv_event_get_user_data(e);
    if (v->binding) return;
    v->binding = true;
    int cols = av_cols(), slots = av_slots(), rows = slots / cols;
    int first_row = (lv_obj_get_scroll_y(v->body) - lv_obj_get_y(v->host)) / av_row() - 1;
    if (first_row < 0) first_row = 0;
    int last_row = (v->count + cols - 1) / cols;
    if (first_row > last_row - rows)
        first_row = last_row > rows ? last_row - rows : 0;
    for (int k = first_row * cols; k < v->count && k < first_row * cols + slots; k++)
    {
        int slot = k % slots;
        if (v->indexes[slot] == k) continue;
        if (v->cells[slot]) lv_obj_delete(v->cells[slot]);
        v->cells[slot] = album_virtual_cell(v->host, k);
        v->indexes[slot] = k;
    }
    v->binding = false;
}
static void album_virtual_deleted(lv_event_t *e)
{
    struct album_grid_view *v = lv_event_get_user_data(e);
    lv_obj_remove_event_cb_with_user_data(v->body, album_virtual_scroll, v);
    sel_forget(v);
    if (album_view == v) album_view = NULL;
    free(v);
}
/* ---------------------------------------------------- A-Z index rail
 * iOS style: slide a finger down the letters, a big letter follows it and
 * the list jumps as you go. Lists are sorted with strcasecmp, so digits
 * come first and Hebrew (high UTF-8 bytes) last; '#' goes to the Hebrew
 * block at the end when there is one, else to the first non-letter. */
struct az_state;
typedef const char *(*az_key_fn)(int i);
typedef void (*az_jump_fn)(struct az_state *r, int i, bool final);
static const char az_letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
#define AZ_W       22                 /* the rail is a slim gutter column */
#define AZ_X       (W - AZ_W - 3)
#define AZ_TOP     (SB_H + 72)
#define AZ_BOTTOM  (H - 96)            /* stops above the mini player */
#define AZ_PAD     (AZ_W + 6)          /* list padding that leaves room for it */
#define AZ_BUBBLE  92                  /* the letter being scrubbed to, on a card */
struct az_state { lv_obj_t *rail, *bubble, *bubble_txt; az_key_fn key; az_jump_fn jump; void *ctx; int n, letter;
                  lv_timer_t *hide;
                  int (*first)(struct az_state *r);   /* the first row in view, when the list can say; else NULL */
                  int here;                           /* the letter lit for it, -1 none */
                  bool asleep;                        /* picking: out of sight and out of reach */
                  /* a rail of groups instead of letters (decades, years added, Z to A):
                   * each group's short label, its name in the bubble, its first row */
                  int nb; char (*lab)[8], (*big)[12]; int *start; };
/* The rail rests dimmed and comes up while the list moves or the finger
 * is on it, then dims again a moment after. */
#define AZ_REST 220                       /* at rest the letters still read 4.5:1 (their ink is deepened for it) */
static void az_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void az_fade(lv_obj_t *rail, int to, int ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rail);
    lv_anim_set_values(&a, lv_obj_get_style_opa(rail, 0), to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, az_opa);
    lv_anim_start(&a);
}
static void az_rest_cb(lv_timer_t *t)
{
    struct az_state *r = lv_timer_get_user_data(t);
    r->hide = NULL;                      /* a one-shot timer deletes itself */
    az_fade(r->rail, AZ_REST, SL_T_SLOW);
}
static void az_wake(struct az_state *r)
{
    if (r->asleep) return;
    if (r->hide) { lv_timer_reset(r->hide); return; }
    r->hide = lv_timer_create(az_rest_cb, 1400, r);
    lv_timer_set_repeat_count(r->hide, 1);
    az_fade(r->rail, LV_OPA_COVER, SL_T_FAST);
}
static int az_initial(const char *s);
/* Where the list is: the letter of the first row in view stands out from
 * the others, so the rail is the scroll position as well as the index */
static void az_here(struct az_state *r)
{
    if (!r->first || !r->n) return;
    int i = r->first(r);
    i = i < 0 ? 0 : i >= r->n ? r->n - 1 : i;
    int k;
    if (r->nb) { k = 0; while (k + 1 < r->nb && r->start[k + 1] <= i) k++; }   /* the group it is in */
    else { int c = az_initial(r->key(i)); k = c == '#' ? 26 : c - 'A'; }
    if (k == r->here) return;
    if (r->here >= 0) lv_obj_set_style_text_color(lv_obj_get_child(r->rail, r->here), lv_color_hex(sl_accent_on(C_BG, 65)), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(r->rail, k), lv_color_hex(C_TEXT), 0);
    r->here = k;
}
static void az_scroll_cb(lv_event_t *e)
{
    struct az_state *r = lv_event_get_user_data(e);
    az_wake(r);
    az_here(r);
}

static int az_initial(const char *s)
{
    s = sl_sort_key(s);                 /* where the list put it: The Beatles under B, Édith under E */
    unsigned char f[4];
    int fl = 0;
    if (s && *s) sl_q_fold_char((const unsigned char *)s, f, &fl);
    int c = fl == 1 ? f[0] : 0;
    if (c >= 'a' && c <= 'z') c -= 32;
    return c >= 'A' && c <= 'Z' ? c : '#';
}
static int az_find(const struct az_state *r, int letter)
{
    if (letter == '#')
    {
        int i = r->n;
        while (i > 0 && az_initial(r->key(i - 1)) == '#') i--;
        if (i < r->n) return i;                      /* trailing block */
        for (i = 0; i < r->n; i++) if (az_initial(r->key(i)) == '#') return i;
        return -1;
    }
    for (int i = 0; i < r->n; i++)
    {
        int c = az_initial(r->key(i));
        if (c != '#' && c >= letter) return i;
    }
    return az_find(r, '#');
}
static void az_rail_cb(lv_event_t *e)
{
    struct az_state *r = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) { if (r->hide) lv_timer_delete(r->hide); free(r->lab); free(r->big); free(r->start); free(r); return; }
    if (!lv_indev_active()) return;
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) az_wake(r);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        lv_obj_add_flag(r->bubble, LV_OBJ_FLAG_HIDDEN);
        int i = r->letter ? (r->nb ? r->start[r->letter - 1] : az_find(r, r->letter)) : -1;
        r->letter = 0;
        if (i >= 0) r->jump(r, i, true);            /* may replace the page */
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(r->rail, &a);
    int slots = r->nb ? r->nb : 27;
    int k = (pt.y - a.y1) * slots / lv_area_get_height(&a);
    k = k < 0 ? 0 : k > slots - 1 ? slots - 1 : k;
    int letter = r->nb ? k + 1 : az_letters[k];
    if (letter == r->letter) return;
    r->letter = letter;
    sfx_play_ex(SFX_UI_TICK, 80 + k * 50 / (slots > 1 ? slots - 1 : 1), 75);   /* a comb: A low, # high */
    char b[2] = { (char)r->letter, 0 };
    lv_label_set_text(r->bubble_txt, r->nb ? r->big[k] : b);
    int by = pt.y - AZ_BUBBLE / 2;
    lv_obj_set_y(r->bubble, by < AZ_TOP ? AZ_TOP : by > AZ_BOTTOM - AZ_BUBBLE ? AZ_BOTTOM - AZ_BUBBLE : by);
    lv_obj_remove_flag(r->bubble, LV_OBJ_FLAG_HIDDEN);
    int i = r->nb ? r->start[k] : az_find(r, r->letter);
    if (i >= 0) r->jump(r, i, false);
}
/* Picking songs, albums or artists: the rail sleeps, so only the marks
 * answer beside the rows, and wakes when picking ends (A13) */
static void az_sleep(lv_obj_t *page, bool sleep)
{
    for (uint32_t i = 0; page && i < lv_obj_get_child_count(page); i++)
    {
        lv_obj_t *c = lv_obj_get_child(page, i);
        for (uint32_t k = 0; k < lv_obj_get_event_count(c); k++)
        {
            lv_event_dsc_t *d = lv_obj_get_event_dsc(c, k);
            if (lv_event_dsc_get_cb(d) != az_rail_cb) continue;
            struct az_state *r = lv_event_dsc_get_user_data(d);
            if (r->asleep == sleep) return;
            r->asleep = sleep;
            if (r->hide) { lv_timer_delete(r->hide); r->hide = NULL; }
            if (sleep) lv_obj_remove_flag(r->rail, LV_OBJ_FLAG_CLICKABLE);
            else lv_obj_add_flag(r->rail, LV_OBJ_FLAG_CLICKABLE);
            az_fade(r->rail, sleep ? LV_OPA_TRANSP : AZ_REST, SL_T_FAST);
            return;
        }
    }
}
static struct az_state *az_rail_groups(lv_obj_t *page, lv_obj_t *body, int n, az_key_fn key, az_jump_fn jump, void *ctx,
                                       int nb, char (*lab)[8], char (*big)[12], int *start)
{
    struct az_state *r = calloc(1, sizeof *r);
    if (!r) { free(lab); free(big); free(start); return NULL; }
    r->key = key; r->jump = jump; r->ctx = ctx; r->n = n; r->here = -1;
    r->nb = nb; r->lab = lab; r->big = big; r->start = start;
    lv_obj_add_event_cb(body, az_scroll_cb, LV_EVENT_SCROLL, r);
    /* a slim column of letters in the page's right gutter; a soft capsule
     * shows only while it is touched, and the touch area is wider than
     * the letters so it is easy to hit */
    lv_obj_t *rail = box(page);
    r->rail = rail;
    lv_obj_set_pos(rail, AZ_X, AZ_TOP);
    lv_obj_set_size(rail, AZ_W, AZ_BOTTOM - AZ_TOP);
    lv_obj_set_style_radius(rail, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(rail, lv_color_hex(C_CARD_P), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_opa(rail, AZ_REST, 0);
    lv_obj_set_ext_click_area(rail, 12);
    lv_obj_add_flag(rail, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* no back-swipe */
    lv_obj_remove_flag(rail, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(rail, az_rail_cb, LV_EVENT_ALL, r);
    int slots = nb ? nb : 27, step = (AZ_BOTTOM - AZ_TOP) / slots;
    for (int i = 0; i < slots; i++)
    {
        char value[2] = { az_letters[i % 27], 0 };
        lv_obj_t *l = text(rail, nb ? lab[i] : value, &ss_regular_16, sl_accent_on(C_BG, 65));   /* the accent, made to read on the page, dimmed at rest */
        lv_obj_set_width(l, AZ_W);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, 0, i * step + (step - 20) / 2);
    }
    /* the letter being scrubbed to, on a card beside the finger */
    r->bubble = box(page);
    lv_obj_set_size(r->bubble, AZ_BUBBLE, AZ_BUBBLE);
    /* left of the rows' ⋮ column, a gutter clear of it: over the words,
     * which the scrub is moving anyway, never over a button (A11) */
    lv_obj_set_x(r->bubble, W - SL_GUTTER - AZ_PAD - SL_ROW_MORE_IN - SL_ROW_MORE_W - SL_GUTTER - AZ_BUBBLE);
    lv_obj_set_style_radius(r->bubble, SL_R_LG, 0);
    lv_obj_set_style_bg_color(r->bubble, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(r->bubble, 240, 0);
    lv_obj_set_style_border_width(r->bubble, 2, 0);
    lv_obj_set_style_border_color(r->bubble, lv_color_hex(C_ACCENT), 0);
    sl_elevate(r->bubble, SL_E3, 0);
    r->bubble_txt = lv_label_create(r->bubble);
    lv_obj_set_style_text_font(r->bubble_txt, nb ? &ss_semibold_26 : &ss_regular_60, 0);   /* a year or decade: smaller, to fit */
    lv_obj_set_style_text_color(r->bubble_txt, lv_color_hex(C_TEXT), 0);
    lv_obj_center(r->bubble_txt);
    lv_obj_add_flag(r->bubble, LV_OBJ_FLAG_HIDDEN);
    return r;
}
static struct az_state *az_rail(lv_obj_t *page, lv_obj_t *body, int n, az_key_fn key, az_jump_fn jump, void *ctx)
{
    return az_rail_groups(page, body, n, key, jump, ctx, 0, NULL, NULL, NULL);
}
/* Groups along a list that isn't A to Z: grp(i) says row i's group as a
 * number that only grows or only shrinks down the list (a decade, a year
 * added, a letter), name() its short label and its bubble. At most 27. */
static struct az_state *az_rail_by(lv_obj_t *page, lv_obj_t *body, int n, az_jump_fn jump, void *ctx,
                                   int (*grp)(int i), void (*name)(int g, char *lab, char *big))
{
    char (*lab)[8] = malloc(27 * sizeof *lab), (*big)[12] = malloc(27 * sizeof *big);
    int *start = malloc(27 * sizeof *start), nb = 0, last = 0;
    if (!lab || !big || !start || n <= 0) { free(lab); free(big); free(start); return NULL; }
    for (int i = 0; i < n && nb < 27; i++)
    {
        int g = grp(i);
        if (nb && g == last) continue;
        name(g, lab[nb], big[nb]);
        start[nb++] = i;
        last = g;
    }
    if (nb < 2) { free(lab); free(big); free(start); return NULL; }   /* one group: nothing to jump between */
    return az_rail_groups(page, body, n, NULL, jump, ctx, nb, lab, big, start);
}

static const char *az_album_key(int i)
{
    const struct sl_album *al = &sl_lib.albums[albums_order()[i]];
    return albums_sort == 1 ? al->artist : al->title;
}
static void az_album_jump(struct az_state *r, int i, bool final)
{
    (void)r; (void)final;
    if (album_view)
        lv_obj_scroll_to_y(album_view->body, lv_obj_get_y(album_view->host) + (i / av_cols()) * av_row(), LV_ANIM_OFF);
}
static void album_virtual_grid(lv_obj_t *body)
{
    struct album_grid_view *v = calloc(1, sizeof *v);
    if (!v) { toast_warn("Not enough memory to show albums"); return; }
    v->body = body;
    v->count = sl_lib.n_albums;
    for (int i = 0; i < ALBUM_VSLOTS; i++) v->indexes[i] = -1;
    v->host = box(body);
    lv_obj_set_size(v->host, av_w() * av_cols() + 8 * (av_cols() - 1),
                    ((v->count + av_cols() - 1) / av_cols()) * av_row());
    lv_obj_add_event_cb(v->host, album_virtual_deleted, LV_EVENT_DELETE, v);
    lv_obj_add_event_cb(body, album_virtual_scroll, LV_EVENT_SCROLL, v);
    album_view = v;
    lv_obj_update_layout(body);
    lv_obj_send_event(body, LV_EVENT_SCROLL, NULL);
}

/* In a page's column a long heading wraps, never running off the screen;
 * placed by hand in a row (beside a chip or a button) it keeps its size */
static void section_wrap(lv_obj_t *body, lv_obj_t *l)
{
    if (lv_obj_get_style_layout(body, LV_PART_MAIN) != LV_LAYOUT_FLEX ||
        lv_obj_get_style_flex_flow(body, LV_PART_MAIN) != LV_FLEX_FLOW_COLUMN) return;
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
}
static lv_obj_t *section(lv_obj_t *body, const char *s)
{
    if (sl_zune())
    {
        /* the Zune HD's small capitals over a list, in grey */
        char up[96];
        lv_obj_t *l = text(body, sl_upper(sl_tr(s), up, sizeof up), &ss_semibold_18, C_SUB);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        lv_obj_set_style_pad_top(l, 18, 0);
        lv_obj_set_style_pad_bottom(l, 4, 0);
        lv_obj_set_style_pad_left(l, 4, 0);
        section_wrap(body, l);
        return l;
    }
    lv_obj_t *l = text(body, s, &ss_semibold_18, C_DIM);   /* the quietest words: under titles and subtitles */
    lv_obj_set_style_pad_top(l, 12, 0);
    lv_obj_set_style_pad_bottom(l, 8, 0);
    lv_obj_set_style_pad_left(l, 6, 0);
    section_wrap(body, l);
    return l;
}

static void sl_pager_button(lv_obj_t *bar, const char *label, bool right,
                            lv_event_cb_t cb, intptr_t value)
{
    lv_obj_t *button = box(bar);
    lv_obj_set_size(button, lv_pct(47), 54);
    sl_align(button, right ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);   /* More is ahead: on the left in RTL */
    lv_obj_set_style_radius(button, SL_R_MD, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_center(text(button, label, &ss_semibold_21, 0xffffff));
    on_click(button, cb, (void *)value);
}
static lv_obj_t *sl_pager(lv_obj_t *body)
{
    lv_obj_t *bar = box(body);
    lv_obj_set_size(bar, lv_pct(100), 64);
    return bar;
}
enum { ACT_MUSIC = 1, ACT_ALL, ACT_ALBUMS, ACT_ARTISTS, ACT_SOON, ACT_CLASSIC, ACT_SYSTEM, ACT_REBUILD, ACT_BLUETOOTH, ACT_PET, ACT_RAGDOLL, ACT_PLAYLISTS, ACT_FILES, ACT_GAMES, ACT_LIBRARY, ACT_SHUFFLE, ACT_NOW, ACT_SNAKE, ACT_RADIO, ACT_SPOON, ACT_QUIZ, ACT_COIN, ACT_MAGIC, ACT_CHESS, ACT_COVERFLOW, ACT_FORYOU, ACT_SEARCH, ACT_SPECTRUM, ACT_LOVED, ACT_VISUALIZER, ACT_CLICKER, ACT_ARCADE, ACT_DOOM, ACT_MARIO, ACT_GENRES, ACT_YEARS, ACT_METRONOME, ACT_BOOKS, ACT_WEBRADIO, ACT_SPACEY, ACT_2048, ACT_SUDOKU, ACT_POINTS, ACT_FLAPPY, ACT_ALBUMARTISTS, ACT_FAVALBUMS, ACT_RECENTALBUMS, ACT_PODCASTS, ACT_EBOOKS, ACT_SUBSONIC };
/* An empty library says what to do about it, and offers to do it */
static lv_obj_t *card_row(lv_obj_t *parent, const char *t1, const char *t2, intptr_t ud);
/* The one empty state: a quiet icon, what is missing in a line, and what
 * to do about it below, centred in the space it has. The caller may add
 * an action under it. top is the space above, so a sheet can sit it
 * higher than a page does. */
static lv_obj_t *sl_empty_last;   /* the empty state made last: a note under it centres with it (opt_note) */
static void sl_empty_gone(lv_event_t *e) { if (lv_event_get_target(e) == sl_empty_last) sl_empty_last = NULL; }
static lv_obj_t *sl_empty(lv_obj_t *parent, int icon, int size, const char *what, const char *how, int top)
{
    lv_obj_t *c = box(parent);
    sl_empty_last = c;
    lv_obj_add_event_cb(c, sl_empty_gone, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(c, top, 0);
    lv_obj_set_style_pad_hor(c, SL_INSET, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ic = sl_icon(c, icon, size, C_DIM, C_BG);
    lv_obj_set_style_margin_bottom(ic, SL_INSET, 0);
    lv_obj_t *l = text(c, what, &ss_semibold_23, C_TEXT);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_t *h = text(c, how, &ss_regular_18, C_SUB);
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_margin_top(h, SL_SP_S, 0);
    lv_obj_set_style_pad_bottom(c, SL_SP_XS, 0);   /* Arabic's marks below the last line stay inside */
    return c;
}
/* The empty state's one action: a pill centred under its words, as wide
 * as Audiobooks' Look for books always was */
static lv_obj_t *mn_pill(lv_obj_t *parent, const char *label, int w, int h, bool primary, lv_event_cb_t cb, void *ud);
static lv_obj_t *sl_empty_action(lv_obj_t *c, const char *label, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *go = mn_pill(c, label, W - 2 * SL_GUTTER - 80, SL_PILL_H, true, cb, ud);
    lv_obj_set_style_margin_top(go, SL_INSET, 0);
    return go;
}
/* An empty library says what to do about it, and offers to do it */
static void card_row_cb(lv_event_t *e);
static void lib_empty(lv_obj_t *body, const char *what)
{
    lv_obj_t *c = sl_empty(body, IC_ALBUMS, 96, what, "Copy music to the player, then update the database.", 56);
    sl_empty_action(c, LV_SYMBOL_REFRESH "  Update database", card_row_cb, (void *)(intptr_t)ACT_REBUILD);
}
/* Wi-Fi and Bluetooth on an R1 with the newer AIC8800 chip: the page says
 * so plainly in the one empty state, names the chip for a bug report, and
 * offers nothing that would only fail */
static void sl_wireless_gone(lv_obj_t *body, int icon, const char *what)
{
    /* no driver on this system (a boot menu built on 1.6), or HiBy 2.0's
     * failed to load at boot */
    lv_obj_t *c = sl_empty(body, icon, 96, what, sl_net_aic_state() == 0 ?
        "This R1 has a newer wireless chip. Its driver needs the HiBy 2.0 edition of the GabagoolOS boot menu. Everything else works." :
        "This R1 has a newer wireless chip, and its driver didn't start. Restart the player to try again. Everything else works.", 56);
    char b[96];
    snprintf(b, sizeof b, "%s: AIC8800D40 \xc2\xb7 %s", sl_tr("Chip"), sl_net_soc_name(sl_net_soc_id()));
    lv_obj_t *n = text(body, b, &ss_regular_16, C_DIM);
    lv_obj_set_width(n, lv_pct(100));
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(n, SL_INSET, 0);
}
/* The header button for a list's order: it says the order it is in */
static void albums_page_at(int offset, bool replace);
static int albums_current_offset;
static lv_obj_t *sort_chip(lv_obj_t *parent, const char *now, lv_event_cb_t cb);
static void count_row(lv_obj_t *body, const char *count, const char *now, lv_event_cb_t cb);
static void sort_sheet(const char *const *names, int n, int cur, void (*pick)(int));
static void albums_sort_pick(int k)
{
    if (k == albums_sort) return;
    albums_sort = k;
    settings_save();
    albums_page_at(0, true);
}
static void albums_sort_cb(lv_event_t *e)
{
    (void)e;
    static const char *const names[] = { "A to Z", "Artist", "Year", "Recently added" };
    sort_sheet(names, 4, albums_sort, albums_sort_pick);
}
/* the rail by Year: a decade a mark; by Recently added: the year each album last gained a song */
static int added_year(uint32_t added);
static void added_name(int g, char *lab, char *big);
static int albums_decade_group(int i)
{
    int y = sl_lib.albums[albums_order()[i]].year;
    return y > 0 ? -(y / 10) : 1;
}
static void decade_name(int g, char *lab, char *big)
{
    if (g > 0) { strlcpy(lab, "?", 8); strlcpy(big, "?", 12); return; }
    snprintf(lab, 8, "%02d", (-g * 10) % 100);   /* '90 */
    snprintf(big, 12, "%ds", -g * 10);
}
static int albums_added_group(int i)
{
    const struct sl_album *al = &sl_lib.albums[albums_order()[i]];
    uint32_t newest = 0;
    for (int j = 0; j < al->n_tracks; j++) newest = LV_MAX(newest, sl_lib.tracks[al->tracks[j]].added);
    int y = added_year(newest);
    return y ? -y : 1;
}
static void albums_page_at(int offset, bool replace)
{
    if (offset < 0 || (sl_lib.n_albums && offset >= sl_lib.n_albums)) return;
    lv_obj_t *p, *body = page_new("Albums", &p);
    char b[72];
    if (albums_sort < 0 || albums_sort > 3) albums_sort = 0;
    free(albums_year_order);
    albums_year_order = NULL;
    if (albums_sort >= 2 && sl_lib.n_albums)
    {
        albums_year_order = malloc(sl_lib.n_albums * sizeof *albums_year_order);
        if (albums_sort == 3) albums_added = calloc(sl_lib.n_albums, sizeof *albums_added);
        if (albums_year_order && (albums_sort == 2 || albums_added))
        {
            memcpy(albums_year_order, sl_lib.albums_by_title, sl_lib.n_albums * sizeof *albums_year_order);
            for (int i = 0; albums_sort == 3 && i < sl_lib.n_albums; i++)
                for (int j = 0; j < sl_lib.albums[i].n_tracks; j++)
                    albums_added[i] = LV_MAX(albums_added[i], sl_lib.tracks[sl_lib.albums[i].tracks[j]].added);
            qsort(albums_year_order, sl_lib.n_albums, sizeof *albums_year_order, albums_sort == 2 ? cmp_album_year : cmp_album_added);
        }
        else { free(albums_year_order); albums_year_order = NULL; }
        free(albums_added);
        albums_added = NULL;
    }
    if (!sl_lib.n_albums)
        lib_empty(body, "No albums in your library yet.");   /* no "0 ALBUMS" over it: it sits where Songs' does */
    else
    {
        /* the count, and the order beside it, as the All list has them */
        sl_count(b, sizeof b, sl_lib.n_albums, "%s ALBUM", "%s ALBUMS");
        static const char *const shown[] = { "A to Z", "Artist", "Year", "Recently added" };
        lv_obj_set_style_pad_right(body, AZ_PAD, 0);
        count_row(body, b, shown[albums_sort], albums_sort_cb);
        album_virtual_grid(body);
        struct az_state *r = albums_sort < 2 ? az_rail(p, body, sl_lib.n_albums, az_album_key, az_album_jump, NULL)
                           : az_rail_by(p, body, sl_lib.n_albums, az_album_jump, NULL,
                                        albums_sort == 2 ? albums_decade_group : albums_added_group,
                                        albums_sort == 2 ? decade_name : added_name);
        if (!r) sl_scroll_indicator(body);
        play_menu_button(p, SEL_ALBUMS);
    }
    if (replace) page_replace(p, offset >= albums_current_offset); else page_push(p);
    if (offset && album_view)
        lv_obj_scroll_to_y(body, lv_obj_get_y(album_view->host) + (offset / av_cols()) * av_row(), LV_ANIM_OFF);
    albums_current_offset = offset;
}
static void albums_page(void) { albums_page_at(0, false); }
static const char *az_song_key(int i) { return sl_lib.tracks[sl_lib.songs_by_title[i]].title; }
static void az_song_jump(struct az_state *r, int i, bool final)
{
    struct song_list_view *v = r->ctx;
    (void)final;
    lv_obj_scroll_to_y(v->body, lv_obj_get_y(v->host) + i * SL_ROW_PITCH, LV_ANIM_OFF);
}
static int az_song_first(struct az_state *r)
{
    struct song_list_view *v = r->ctx;
    return (lv_obj_get_scroll_y(v->body) - lv_obj_get_y(v->host) + SL_ROW_PITCH / 2) / SL_ROW_PITCH;
}
static void tile_cb(lv_event_t *e);
/* Every song: Play all and Shuffle in the header, the count and the order
 * over the list. */
#define songs_sort global_settings.sloppy_songs_sort   /* 0 title A-Z, 1 title Z-A, 2 newest first */
static int cmp_song_added(const void *a, const void *b)
{
    uint32_t x = sl_lib.tracks[*(const int *)a].added, y = sl_lib.tracks[*(const int *)b].added;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void songs_page_at(bool replace);
static void songs_sort_pick(int k)
{
    if (k == songs_sort) return;
    songs_sort = k;
    settings_save();
    songs_page_at(true);
}
static void songs_sort_cb(lv_event_t *e)
{
    (void)e;
    static const char *const names[] = { "A to Z", "Z to A", "Recently added" };
    sort_sheet(names, 3, songs_sort, songs_sort_pick);
}
static void songs_page(void) { songs_page_at(false); }
/* The order a list is in, as a button: lines that step down beside its
 * name, on a chip that presses like a card */
static lv_obj_t *sort_chip(lv_obj_t *parent, const char *now, lv_event_cb_t cb)
{
    lv_obj_t *c = box(parent);
    lv_obj_add_style(c, &st_card, 0);
    lv_obj_add_style(c, &st_card_pr, LV_STATE_PRESSED);
    lv_obj_set_size(c, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_pad_hor(c, SL_GUTTER, 0);
    lv_obj_set_style_pad_column(c, SL_SP_S, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *ic = box(c);
    lv_obj_set_size(ic, 16, 12);
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *bar = box(ic);
        lv_obj_set_size(bar, 16 - i * 5, 2);
        lv_obj_set_pos(bar, 0, i * 5);
        lv_obj_set_style_radius(bar, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT_TXT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
    text(c, now, &ss_semibold_18, C_TEXT);
    on_click(c, cb, NULL);
    lv_obj_set_ext_click_area(c, SL_SP_S);
    return c;
}
/* A list's count with its order beside it; a long order's name ends in
 * an ellipsis before it reaches the count */
static void count_row(lv_obj_t *body, const char *count, const char *now, lv_event_cb_t cb)
{
    lv_obj_t *cr = box(body);
    lv_obj_set_size(cr, lv_pct(100), 52);
    lv_obj_t *l = section(cr, count);
    sl_align(l, LV_ALIGN_LEFT_MID, 0, 0);   /* the count leads and the chip trails, either way round */
    /* Zule's heading sits lower; at Largest, with Arabic's deep letters, the row grows to hold it */
    int need = lv_obj_get_style_pad_top(l, 0) + lv_obj_get_style_pad_bottom(l, 0) + sl_line_h(&ss_semibold_18) + 2;
    if (need > 52) lv_obj_set_height(cr, need);
    lv_obj_t *c = sort_chip(cr, now, cb);
    sl_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_update_layout(cr);
    lv_obj_t *w = lv_obj_get_child(c, 1);
    int room = lv_obj_get_content_width(cr) - lv_obj_get_width(l) - SL_SP_S;   /* beside the count, on either side */
    int chrome = lv_obj_get_width(c) - lv_obj_get_width(w);    /* the chip less its words */
    if (lv_obj_get_width(c) <= room) return;
    lv_obj_set_size(w, LV_MAX(40, room - chrome), lv_font_get_line_height(&ss_semibold_18));
    lv_label_set_long_mode(w, LV_LABEL_LONG_DOT);
}
/* The rail for the other orders: Z to A by letter (other scripts first,
 * as the list has them, figures last), Recently added by the year each
 * song came, newest first */
static const int *songs_rail_list;
static int songs_rail_n;
static int songs_za_group(int i)
{
    int c = az_initial(sl_lib.tracks[songs_rail_list[i]].title);
    return c == '#' ? (i < songs_rail_n / 2 ? 0 : 27) : 'Z' - c + 1;
}
static void letter_name(int g, char *lab, char *big)
{
    lab[0] = g <= 0 || g >= 27 ? '#' : (char)('Z' - g + 1);
    lab[1] = '\0';
    strlcpy(big, lab, 12);
}
static int added_year(uint32_t added)
{
    if (!added) return 0;
    time_t t = (time_t)added;
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_year + 1900;
}
static int songs_added_group(int i)
{
    int y = added_year(sl_lib.tracks[songs_rail_list[i]].added);
    return y ? -y : 1;                   /* newest first; an unknown time last */
}
static void added_name(int g, char *lab, char *big)
{
    if (g > 0) { strlcpy(lab, "?", 8); strlcpy(big, "?", 12); return; }
    snprintf(lab, 8, "%02d", -g % 100);   /* '26 */
    snprintf(big, 12, "%d", -g);
}
static void songs_page_at(bool replace)
{
    lv_obj_t *p, *body = page_new("Songs", &p);
    int n = sl_lib.n_tracks;
    if (!n) lib_empty(body, "No songs in your library yet.");
    /* any order but A-Z is a list of its own, played from the row tapped */
    int *order = NULL;
    if (songs_sort && n && (order = malloc(n * sizeof *order)))
    {
        for (int i = 0; i < n; i++) order[i] = sl_lib.songs_by_title[songs_sort == 1 ? n - 1 - i : i];
        if (songs_sort == 2) qsort(order, n, sizeof *order, cmp_song_added);
    }
    if (n && !order) lv_obj_set_style_pad_right(body, AZ_PAD, 0);   /* the rail's gutter */
    if (n)
    {
        /* the count, and the order beside it */
        char num[16], b[64];
        snprintf(b, sizeof b, sl_tr(n == 1 ? "%s SONG" : "%s SONGS"), sl_fmt_count(num, sizeof num, n));
        static const char *const names[] = { "A to Z", "Z to A", "Recently added" };
        count_row(body, b, names[songs_sort % 3], songs_sort_cb);
    }
    struct song_list_view *v = song_list(body, order ? order : sl_lib.songs_by_title, n, order ? SL_LIST_CUSTOM : -1);
    if (v)
    {
        v->owned = order;
        struct az_state *r = !order ? az_rail(p, body, n, az_song_key, az_song_jump, v)
                           : (songs_rail_list = order, songs_rail_n = n,
                              az_rail_by(p, body, n, az_song_jump, v, songs_sort == 1 ? songs_za_group : songs_added_group,
                                         songs_sort == 1 ? letter_name : added_name));
        if (r) { r->first = az_song_first; az_here(r); lv_obj_set_style_pad_right(body, AZ_PAD, 0); }
        else sl_scroll_indicator(body);   /* one group (every song added the same year): a scroll bar */
        /* Play all, Shuffle and the play menu in the header, as on Albums and Artists */
        play_menu_button(p, SEL_SONGS);
    }
    else free(order);
    if (replace) page_replace(p, true); else page_push(p);
}

static void card_row_cb(lv_event_t *e);
static void tone_page(void);
static void wf_share_cb(lv_event_t *e);
static void libjob_page(void);
static void card_row_open(intptr_t v);
static void sp_page(void);
static void eq_profiles_page(void);
static void reset_all_settings_page(void);
static void er_page(void);
static void saver_page(void);
static void car_page(void);
static void settings_layout_page(void);
static void sound_page(void);
static void buttons_page(void);
static void shortcuts_page(void);
static void time_power_page(void);
static void lyrics_color_page(void);
static void sl_wifi_page(void);
static void language_page(void);
static void text_size_page(void);
static void keyboards_page(void);
static void library_options_page(void);
static void lo_seps_page(void);
static void lo_folders_page(void);
static void lo_keep_page(void);
static const char *wf_row_text(void);
/* Text is laid out before the row knows its width (lists with an index
 * rail are narrower), so size the labels once it does: a right-to-left
 * title then ends before the chevron instead of under it. */
static void card_row_sized(lv_event_t *e)
{
    lv_obj_t *r = lv_event_get_target(e);
    for (uint32_t i = 0; i < 2 && i < lv_obj_get_child_count(r); i++)
    {
        lv_obj_t *l = lv_obj_get_child(r, i);
        /* a row with no subtitle has the chevron second: leave it be; the
         * words start their lead (18, or 76 after an icon) from the leading
         * edge, whichever side that is */
        if (lv_obj_check_type(l, &lv_label_class) && strcmp(lv_label_get_text(l), LV_SYMBOL_RIGHT)
            && strcmp(lv_label_get_text(l), LV_SYMBOL_LEFT))
            lv_obj_set_width(l, lv_obj_get_width(r) - LV_ABS(lv_obj_get_style_x(l, 0)) - 58);
    }
}
static bool sl_rtl(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++)
    {
        if ((*p | 32) >= 'a' && (*p | 32) <= 'z') return false;
        if (*p >= 0xd6 && *p <= 0xdb) return true;         /* Hebrew, Arabic */
    }
    return false;
}
static lv_obj_t *card_row(lv_obj_t *parent, const char *t1, const char *t2, intptr_t ud)
{
    lv_obj_t *r = box(parent);
    lv_obj_add_style(r, &st_card, 0);
    lv_obj_add_style(r, &st_card_pr, LV_STATE_PRESSED);
    /* the lines' heights from the fonts in use, so every text size fits
     * (at Standard: a 31 px title at 14, a 23 px subtitle at 48, 86 tall) */
    int h1 = lv_font_get_line_height(&ss_semibold_23) + 3, h2 = lv_font_get_line_height(&ss_regular_18) + 1;
    int y2 = 14 + h1 + 3;
    lv_obj_set_size(r, lv_pct(100), t2 ? y2 + h2 + 15 : 20 + h1 + 19);
    lv_obj_set_style_margin_bottom(r, 8, 0);
    /* the hairline edge of the Settings panels (set_group()) */
    lv_obj_set_style_radius(r, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_border_opa(r, 36, 0);
    lv_obj_set_style_border_width(r, sl_zune() ? 0 : 1, 0);
    char up[96];
    /* Zune: its lists are words in bold capitals, nothing else on the row */
    lv_obj_t *a = text(r, sl_zune() ? sl_upper(sl_tr(t1), up, sizeof up) : t1, &ss_semibold_23, 0xffffff);
    sl_place(a, 18, t2 ? 14 : 20);
    lv_obj_set_width(a, W - 100);
    lv_obj_set_height(a, h1);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    sl_label_deep(a);
    if (t2)
    {
        lv_obj_t *b = text(r, t2, &ss_regular_18, C_SUB);
        sl_place(b, 18, y2);
        lv_obj_set_width(b, W - 100);
        lv_obj_set_height(b, h2);
        lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
        sl_label_deep(b);
        if (sl_rtl(t1)) lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_add_event_cb(r, card_row_sized, LV_EVENT_SIZE_CHANGED, NULL);
    if (!sl_zune())                     /* the Zune's words need no arrows */
    {
        lv_obj_t *ch = text(r, sl_chevron(), &sl_sym_18, C_DIM);
        sl_align(ch, LV_ALIGN_RIGHT_MID, -20, 0);
    }
    on_click(r, card_row_cb, (void *)ud);
    return r;
}

#include "sloppy_tags.inc"
static void q_add_many(const int *l, int n);
static void acts_head(int album, const char *title, const char *line);

/* --------------------------------------------- long lists, made as shown */
/* A list of artist rows or a grid of covers too long to make whole: only
 * what is in view and a little either side exists, made again as it
 * scrolls, so ten thousand albums scroll as smoothly as ten. */
#define RV_SLOTS 16
struct row_view {
    lv_obj_t *body, *host, *rows[RV_SLOTS];
    int indexes[RV_SLOTS], count, pitch;
    lv_obj_t *(*make)(struct row_view *v, int k);
    int *list;                           /* what the rows are of, freed with the view */
    bool binding;
};
/* a card row's height with a second line, by the fonts in use (card_row) */
static int card_row_h(void)
{
    int h1 = lv_font_get_line_height(&ss_semibold_23) + 3, h2 = lv_font_get_line_height(&ss_regular_18) + 1;
    return 14 + h1 + 3 + h2 + 15;
}
static void row_view_bind(struct row_view *v)
{
    if (v->binding) return;
    v->binding = true;
    int first = (lv_obj_get_scroll_y(v->body) - lv_obj_get_y(v->host)) / v->pitch - 3;
    if (first > v->count - RV_SLOTS) first = v->count - RV_SLOTS;
    if (first < 0) first = 0;
    for (int k = first; k < v->count && k < first + RV_SLOTS; k++)
    {
        int slot = k % RV_SLOTS;
        if (v->indexes[slot] == k && v->rows[slot]) continue;
        if (v->rows[slot]) lv_obj_delete(v->rows[slot]);
        v->rows[slot] = v->make(v, k);
        lv_obj_set_pos(v->rows[slot], 0, k * v->pitch);
        v->indexes[slot] = k;
    }
    v->binding = false;
}
static void row_view_scroll(lv_event_t *e) { row_view_bind(lv_event_get_user_data(e)); }
static struct row_view *artists_view;   /* Album artists' rows, for picking and Go to current song */
static void row_view_deleted(lv_event_t *e)
{
    struct row_view *v = lv_event_get_user_data(e);
    lv_obj_remove_event_cb_with_user_data(v->body, row_view_scroll, v);
    if (artists_view == v) artists_view = NULL;
    free(v->list);
    free(v);
}
static struct row_view *row_view_new(lv_obj_t *body, int count, lv_obj_t *(*make)(struct row_view *, int), int *list)
{
    struct row_view *v = calloc(1, sizeof *v);
    if (!v) { free(list); toast_warn("Not enough memory to show this list"); return NULL; }
    v->body = body; v->count = count; v->make = make; v->list = list;
    v->pitch = card_row_h() + 8;
    for (int i = 0; i < RV_SLOTS; i++) v->indexes[i] = -1;
    v->host = box(body);
    lv_obj_set_size(v->host, lv_pct(100), count * v->pitch);
    lv_obj_add_event_cb(v->host, row_view_deleted, LV_EVENT_DELETE, v);
    lv_obj_add_event_cb(body, row_view_scroll, LV_EVENT_SCROLL, v);
    lv_obj_update_layout(body);
    row_view_bind(v);
    return v;
}

/* The covers of an artist's, a genre's or a year's albums, two across,
 * made as they come into view. A second run of them (the albums an artist
 * is only on) can follow under a heading of its own. */
#define GV_SLOTS 16
struct grid_view {
    lv_obj_t *body, *host, *cells[GV_SLOTS];
    int indexes[GV_SLOTS];
    int *list, count, split;             /* list[0..split) then, under the heading, the rest */
    int cw, pitch, head_h;
    bool binding;
};
static int gv_y(const struct grid_view *v, int k)
{
    if (k < v->split) return (k / 2) * v->pitch;
    return ((v->split + 1) / 2) * v->pitch + v->head_h + ((k - v->split) / 2) * v->pitch;
}
static lv_obj_t *gv_cell(struct grid_view *v, int k)
{
    int a = v->list[k];
    const struct sl_album *al = &sl_lib.albums[a];
    lv_obj_t *cell = box(v->host);
    char b[80];
    album_caption(b, sizeof b, al, true);
    int th = lv_font_get_line_height(&ss_semibold_18) + 2, sh = lv_font_get_line_height(&ss_regular_16) + 1;
    lv_obj_set_size(cell, v->cw, v->pitch - 16);
    int col = (k < v->split ? k : k - v->split) % 2;
    lv_obj_set_pos(cell, col * (v->cw + 14), gv_y(v, k));
    album_badge(cover(cell, a, v->cw, true), a);
    lv_obj_t *t = text(cell, al->title, &ss_semibold_18, 0xffffff);
    lv_obj_set_size(t, v->cw, th);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    sl_label_deep(t);
    lv_obj_set_pos(t, 0, v->cw + 8);
    if (b[0])
    {
        lv_obj_t *s = text(cell, b, &ss_regular_16, C_SUB);
        lv_obj_set_size(s, v->cw, sh);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        sl_label_deep(s);
        lv_obj_set_pos(s, 0, v->cw + 8 + th);
    }
    on_click(cell, album_open_cb, (void *)(intptr_t)a);
    lv_obj_add_event_cb(cell, album_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(a << 1));   /* hold: its sheet */
    return cell;
}
static void grid_view_bind(struct grid_view *v)
{
    if (v->binding) return;
    v->binding = true;
    int y = lv_obj_get_scroll_y(v->body) - lv_obj_get_y(v->host) - v->pitch;
    int lo = 0, hi = v->count;
    while (lo < hi) { int mid = (lo + hi) / 2; if (gv_y(v, mid) + v->pitch <= y) lo = mid + 1; else hi = mid; }
    int first = lo & ~1;                 /* a row starts on an even place in either run */
    if (first > v->split && ((first - v->split) & 1)) first--;
    if (first > v->count - GV_SLOTS) first = v->count - GV_SLOTS;
    if (first < 0) first = 0;
    for (int k = first; k < v->count && k < first + GV_SLOTS; k++)
    {
        int slot = k % GV_SLOTS;
        if (v->indexes[slot] == k && v->cells[slot]) continue;
        if (v->cells[slot]) lv_obj_delete(v->cells[slot]);
        v->cells[slot] = gv_cell(v, k);
        v->indexes[slot] = k;
    }
    v->binding = false;
}
static void grid_view_scroll(lv_event_t *e) { grid_view_bind(lv_event_get_user_data(e)); }
static void grid_view_deleted(lv_event_t *e)
{
    struct grid_view *v = lv_event_get_user_data(e);
    lv_obj_remove_event_cb_with_user_data(v->body, grid_view_scroll, v);
    free(v->list);
    free(v);
}
/* list (malloc'd, taken over): count albums, the first split of them, then
 * the rest under heading (NULL: one run) */
static struct grid_view *grid_view_new(lv_obj_t *body, int *list, int count, int split, const char *heading)
{
    struct grid_view *v = calloc(1, sizeof *v);
    if (!v) { free(list); toast_warn("Not enough memory to show albums"); return NULL; }
    v->body = body; v->list = list; v->count = count;
    v->split = heading && split < count ? split : count;
    v->cw = (W - 28 - 14) / 2;
    v->pitch = v->cw + 8 + lv_font_get_line_height(&ss_semibold_18) + 2 + lv_font_get_line_height(&ss_regular_16) + 1 + 6 + 16;
    for (int i = 0; i < GV_SLOTS; i++) v->indexes[i] = -1;
    v->host = box(body);
    lv_obj_set_width(v->host, lv_pct(100));
    if (v->split < count)
    {
        lv_obj_t *l = section(v->host, heading);
        lv_obj_update_layout(l);
        v->head_h = lv_obj_get_height(l) + 4;
        lv_obj_set_pos(l, 0, ((v->split + 1) / 2) * v->pitch);
    }
    lv_obj_set_height(v->host, count ? gv_y(v, count - 1) + v->pitch - 16 : 0);
    lv_obj_add_event_cb(v->host, grid_view_deleted, LV_EVENT_DELETE, v);
    lv_obj_add_event_cb(body, grid_view_scroll, LV_EVENT_SCROLL, v);
    lv_obj_update_layout(body);
    grid_view_bind(v);
    return v;
}

/* Play and Shuffle under a page's title, for any list of songs */
static void play_pair(lv_obj_t *body, lv_event_cb_t cb, intptr_t v, int top)
{
    lv_obj_t *pl = box(body);
    lv_obj_set_size(pl, lv_pct(100), 72);
    lv_obj_set_flex_flow(pl, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pl, 12, 0);
    lv_obj_set_style_pad_hor(pl, 6, 0);
    lv_obj_set_style_pad_top(pl, top, 0);
    lv_obj_t *play = pill(pl, LV_SYMBOL_PLAY "  Play", true, 0), *shuf = pill(pl, LV_SYMBOL_SHUFFLE "  Shuffle", false, 0);
    lv_obj_remove_event_cb(play, pill_cb);
    lv_obj_remove_event_cb(shuf, pill_cb);
    on_click(play, cb, (void *)(v << 1));
    on_click(shuf, cb, (void *)(v << 1 | 1));
}
/* how much there is: "12 albums · 140 songs · 610 min" */
static void amount_line(lv_obj_t *body, int albums, const int *tracks, int n_tracks)
{
    long long ms = 0;   /* 32 bits on the R1: past 596 hours it went negative */
    for (int i = 0; i < n_tracks; i++) ms += sl_lib.tracks[tracks[i]].len;
    char label[128], a1[40], a2[40], a3[32];
    snprintf(label, sizeof label, "%s \xc2\xb7 %s \xc2\xb7 %s", sl_count(a1, sizeof a1, albums, "%s album", "%s albums"),
             sl_count(a2, sizeof a2, n_tracks, "%s song", "%s songs"), sl_count(a3, sizeof a3, (int)(ms / 60000), "%s min", "%s min"));
    lv_obj_t *m = text(body, label, &ss_regular_18, C_SUB);
    lv_obj_set_style_pad_left(m, 6, 0);
}

/* ------------------------------------------------------- Album artists */
/* An album artist's page: every album of theirs, oldest first */
/* how many albums the artist at k0 (of albums_by_artist) has */
static int artist_span(int k0)
{
    int n = 0;
    if (k0 < 0 || k0 >= sl_lib.n_albums) return 0;
    const char *name = sl_lib.albums[sl_lib.albums_by_artist[k0]].artist;
    while (k0 + n < sl_lib.n_albums && !strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[k0 + n]].artist, name)) n++;
    return n;
}
/* the songs of n albums, album by album; the caller frees the list */
static int albums_tracks(const int *albums, int n, int **out)
{
    int total = 0, at = 0;
    for (int i = 0; i < n; i++) total += sl_lib.albums[albums[i]].n_tracks;
    *out = total ? malloc((size_t)total * sizeof **out) : NULL;
    if (!*out) return 0;
    for (int i = 0; i < n; i++)
    {
        const struct sl_album *al = &sl_lib.albums[albums[i]];
        memcpy(*out + at, al->tracks, (size_t)al->n_tracks * sizeof **out);
        at += al->n_tracks;
    }
    return total;
}
static void artist_play_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int k0 = (int)(v >> 1), *tracks;
    int total = albums_tracks(sl_lib.albums_by_artist + k0, artist_span(k0), &tracks);
    if (total && play_list(tracks, total, 0, v & 1)) np_show_auto();
    free(tracks);
}
static void artist_page(int k0)
{
    int n = artist_span(k0);
    if (!n) return;
    const char *name = sl_lib.albums[sl_lib.albums_by_artist[k0]].artist;
    lv_obj_t *p, *body = page_new(name, &p);
    int *tracks, total = albums_tracks(sl_lib.albums_by_artist + k0, n, &tracks);
    amount_line(body, n, tracks, total);
    free(tracks);
    play_pair(body, artist_play_cb, k0, 10);
    section(body, "ALBUMS");
    int *list = malloc((size_t)n * sizeof *list);
    if (list)
    {
        memcpy(list, sl_lib.albums_by_artist + k0, (size_t)n * sizeof *list);
        grid_view_new(body, list, n, n, NULL);
    }
    lv_obj_set_user_data(p, (void *)(intptr_t)(PG_ARTIST | k0));
    page_push(p);
}

/* The album artists, A to Z: a row each, made as the list scrolls. The
 * list keeps where each one's albums start in albums_by_artist. */
static int *aa_starts;                  /* the shown list's, while it is up */
static int aa_n;
static const char *az_artist_key(int i) { return sl_lib.albums[sl_lib.albums_by_artist[aa_starts[i]]].artist; }
static void az_artist_jump(struct az_state *r, int i, bool final)
{
    (void)r; (void)final;
    if (artists_view) lv_obj_scroll_to_y(artists_view->body, lv_obj_get_y(artists_view->host) + i * artists_view->pitch, LV_ANIM_OFF);
}
static int az_artist_first(struct az_state *r)
{
    (void)r;
    if (!artists_view) return 0;
    return (lv_obj_get_scroll_y(artists_view->body) - lv_obj_get_y(artists_view->host) + artists_view->pitch / 2) / artists_view->pitch;
}
static lv_obj_t *aa_row(struct row_view *v, int i)
{
    int k = v->list[i], n = artist_span(k), songs = 0;
    for (int j = 0; j < n; j++) songs += sl_lib.albums[sl_lib.albums_by_artist[k + j]].n_tracks;
    char b[96], a1[40], a2[40];
    snprintf(b, sizeof b, "%s · %s", sl_count(a1, sizeof a1, n, "%s album", "%s albums"), sl_count(a2, sizeof a2, songs, "%s song", "%s songs"));
    lv_obj_t *r = card_row(v->host, sl_lib.albums[sl_lib.albums_by_artist[k]].artist, b, (intptr_t)(0x10000 | k));
    /* the row knows its artist (k + 1): picking, and a hold for its sheet */
    lv_obj_set_user_data(r, (void *)(intptr_t)(k + 1));
    lv_obj_remove_event_cb(r, card_row_cb);   /* picking takes the tap before the row opens */
    lv_obj_add_event_cb(r, sel_artist_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(r, card_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(0x10000 | k));
    lv_obj_add_event_cb(r, artist_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    if (sel_kind_on(SEL_ARTISTS)) sel_artist_mark(r, k);
    return r;
}
static void aa_list_deleted(lv_event_t *e)
{
    if (lv_event_get_user_data(e) != aa_starts) return;
    aa_starts = NULL;
    aa_n = 0;
}
static void artists_page(void)
{
    lv_obj_t *p, *body = page_new("Album artists", &p);
    int n = 0, *starts = malloc(((size_t)sl_lib.n_albums + 1) * sizeof *starts);
    for (int k = 0; starts && k < sl_lib.n_albums; k += artist_span(k)) starts[n++] = k;
    if (!n)
    {
        free(starts);
        lib_empty(body, "No artists in your library yet.");
    }
    else
    {
        char b[48];
        lv_obj_set_style_pad_right(body, AZ_PAD, 0);
        section(body, sl_count(b, sizeof b, n, "%s ALBUM ARTIST", "%s ALBUM ARTISTS"));
        struct row_view *v = row_view_new(body, n, aa_row, starts);
        if (v)
        {
            artists_view = v;
            aa_starts = starts; aa_n = n;
            lv_obj_add_event_cb(v->host, aa_list_deleted, LV_EVENT_DELETE, starts);
            struct az_state *r = az_rail(p, body, n, az_artist_key, az_artist_jump, NULL);
            if (r) { r->first = az_artist_first; az_here(r); }
            play_menu_button(p, SEL_ARTISTS);
        }
    }
    page_push(p);
}

/* ------------------------------------------------------------- Artists */
/* Everyone a song names, one by one when a tag names several: the albums
 * they're on, theirs first; Play plays only their songs. */
static void tartist_play_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int *l, n = tg_artist_tracks((int)(v >> 1), &l);
    if (n && play_list(l, n, 0, v & 1)) np_show_auto();
    free(l);
}
static void tartist_page(int id)
{
    if (!tg_ready() || id < 0 || id >= tgx.n) return;
    lv_obj_t *p, *body = page_new(TG_NAME(id), &p);
    int *l, n = tg_artist_tracks(id, &l), albums = tg_albums_of(id), mine = tgx.mine[id];
    amount_line(body, albums, l, n);
    free(l);
    play_pair(body, tartist_play_cb, id, 10);
    section(body, mine ? "ALBUMS" : "APPEARS ON");
    int *list = malloc((size_t)(albums ? albums : 1) * sizeof *list);
    if (list)
    {
        memcpy(list, tgx.alb + tgx.first[id], (size_t)albums * sizeof *list);
        grid_view_new(body, list, albums, mine ? mine : albums, "APPEARS ON");
    }
    lv_obj_set_user_data(p, (void *)(intptr_t)(PG_TARTIST | id));
    page_push(p);
}
static void tartist_open_cb(lv_event_t *e) { tartist_page((int)(intptr_t)lv_event_get_user_data(e)); }
/* hold an artist: play, shuffle, queue or keep their songs */
static void tartist_act_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int id = (int)(v >> 3), k = (int)(v & 7), *l, n;
    acts_close();
    n = tg_artist_tracks(id, &l);
    if (k <= 1) { if (n && play_list(l, n, 0, k == 1)) np_show_auto(); }
    else if (k == 2) q_add_many(l, n);
    else if (n) pl_open_many(l, n);
    free(l);
}
static void tartist_long_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_indev_wait_release(lv_indev_active());
    if (!tg_ready() || id < 0 || id >= tgx.n || tg_albums_of(id) < 1) return;
    acts_begin();
    char b[96], a1[40], a2[40];
    snprintf(b, sizeof b, "%s · %s", sl_count(a1, sizeof a1, tg_albums_of(id), "%s album", "%s albums"),
             sl_count(a2, sizeof a2, tgx.songs[id], "%s song", "%s songs"));
    acts_head(tgx.alb[tgx.first[id]], TG_NAME(id), b);
    intptr_t v = (intptr_t)id << 3;
    acts_row_cb(LV_SYMBOL_PLAY, "Play", tartist_act_cb, v | 0);
    acts_row_cb(LV_SYMBOL_SHUFFLE, "Shuffle", tartist_act_cb, v | 1);
    acts_row_cb(LV_SYMBOL_LIST, "Add to queue", tartist_act_cb, v | 2);
    acts_row_cb(LV_SYMBOL_PLUS, "Add to playlist", tartist_act_cb, v | 3);
    acts_show();
}
static struct row_view *tartists_view;
static lv_obj_t *tartist_row(struct row_view *v, int i)
{
    int id = tgx.order[i];
    char b[96], a1[40], a2[40];
    snprintf(b, sizeof b, "%s · %s", sl_count(a1, sizeof a1, tg_albums_of(id), "%s album", "%s albums"),
             sl_count(a2, sizeof a2, tgx.songs[id], "%s song", "%s songs"));
    lv_obj_t *r = card_row(v->host, TG_NAME(id), b, 0);
    lv_obj_remove_event_cb(r, card_row_cb);
    on_click(r, tartist_open_cb, (void *)(intptr_t)id);
    lv_obj_add_event_cb(r, tartist_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)id);
    return r;
}
static const char *az_tartist_key(int i) { return TG_NAME(tgx.order[i]); }
static void az_tartist_jump(struct az_state *r, int i, bool final)
{
    (void)r; (void)final;
    if (tartists_view) lv_obj_scroll_to_y(tartists_view->body, lv_obj_get_y(tartists_view->host) + i * tartists_view->pitch, LV_ANIM_OFF);
}
static int az_tartist_first(struct az_state *r)
{
    (void)r;
    if (!tartists_view) return 0;
    return (lv_obj_get_scroll_y(tartists_view->body) - lv_obj_get_y(tartists_view->host) + tartists_view->pitch / 2) / tartists_view->pitch;
}
static void tartists_deleted(lv_event_t *e) { if (lv_event_get_user_data(e) == tartists_view) tartists_view = NULL; }
/* Play in order, Shuffle, or the play menu: every artist's songs, A to Z */
static void tartists_play_cb(lv_event_t *e)
{
    bool shuffle = (intptr_t)lv_event_get_user_data(e) & 1;
    int *l = malloc(((size_t)sl_lib.n_tracks + 1) * sizeof *l), n = 0;
    uint8_t *seen = calloc((size_t)sl_lib.n_tracks + 1, 1);
    for (int i = 0; l && seen && tg_ready() && i < tgx.listed; i++)
    {
        int *t, m = tg_artist_tracks(tgx.order[i], &t);
        for (int j = 0; j < m; j++) if (!seen[t[j]]) { seen[t[j]] = 1; l[n++] = t[j]; }
        free(t);
    }
    if (n && play_list(l, n, 0, shuffle)) np_show_auto();
    free(l); free(seen);
}
static void tartists_page(void)
{
    lv_obj_t *p, *body = page_new("Artists", &p);
    lv_obj_set_user_data(p, (void *)(intptr_t)PG_TAGS);
    int n = tg_n_artists();
    if (!tg_ready() && sl_lib.n_albums) sl_empty(body, IC_ARTISTS, 96, "Not enough memory to list artists one by one.",
                                                 "Album artists still lists them by album.", 56);
    else if (!n) lib_empty(body, "No artists in your library yet.");
    else
    {
        char b[48];
        lv_obj_set_style_pad_right(body, AZ_PAD, 0);
        section(body, sl_count(b, sizeof b, n, "%s ARTIST", "%s ARTISTS"));
        struct row_view *v = row_view_new(body, n, tartist_row, NULL);
        if (v)
        {
            tartists_view = v;
            lv_obj_add_event_cb(v->host, tartists_deleted, LV_EVENT_DELETE, v);
            struct az_state *r = az_rail(p, body, n, az_tartist_key, az_tartist_jump, NULL);
            if (r) { r->first = az_tartist_first; az_here(r); }
            header_button(p, LV_SYMBOL_SHUFFLE, tartists_play_cb, (void *)1);   /* as Album artists has them, less picking */
            header_button(p, LV_SYMBOL_PLAY, tartists_play_cb, (void *)0);
        }
    }
    page_push(p);
}

/* ---------------------------------------------- genres, years, decades */
/* The albums of one genre, one year or one decade, as an artist's are.
 * The key: 0x30000 | the genre's place A to Z, 0x40000 | the year, or
 * 0x50000 | the decade's first year. */
static int fav_albums(int **out);
static int hist_recent_albums(int *albums, int max);
#define RECENT_ALBUMS 200
static int group_albums(intptr_t key, int **out)
{
    *out = NULL;
    int kind = (int)(key & 0xf0000), k = (int)(key & 0xffff), n = 0;
    if (kind == 0x60000) return fav_albums(out);
    if (kind == 0x70000)
    {
        if (!(*out = malloc(RECENT_ALBUMS * sizeof **out))) return 0;
        return hist_recent_albums(*out, RECENT_ALBUMS);
    }
    if (kind == 0x30000)
    {
        if (!tg_ready() || k >= tgx.ng) return 0;
        n = tgx.gfirst[k + 1] - tgx.gfirst[k];
        if (!(*out = malloc((size_t)(n ? n : 1) * sizeof **out))) return 0;
        memcpy(*out, tgx.galb + tgx.gfirst[k], (size_t)n * sizeof **out);
        return n;
    }
    if (!(*out = malloc(((size_t)sl_lib.n_albums + 1) * sizeof **out))) return 0;
    for (int i = 0; i < sl_lib.n_albums; i++)
    {
        int a = sl_lib.albums_by_title[i], y = sl_lib.albums[a].year;
        if (kind == 0x40000 ? y == k : y > 0 && y / 10 * 10 == k) (*out)[n++] = a;
    }
    return n;
}
/* Play or Shuffle a genre, a year or a decade: every song of its albums */
static void group_play_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int *list, n = group_albums(v >> 1, &list), *tracks;
    int total = albums_tracks(list, n, &tracks);
    if (total && play_list(tracks, total, 0, v & 1)) np_show_auto();
    free(list);
    free(tracks);
}
static void group_page(intptr_t key)
{
    int *list, n = group_albums(key, &list);
    int kind = (int)(key & 0xf0000), k = (int)(key & 0xffff);
    bool own = kind == 0x60000 || kind == 0x70000;   /* yours: a page even with nothing on it yet */
    if (!list) { toast_warn("Not enough memory to show albums"); return; }
    if (!n && !own) { free(list); return; }
    char title[24];
    const char *name = kind == 0x30000 ? TG_GNAME(k) : kind == 0x60000 ? "Favourite albums" : kind == 0x70000 ? "Recently played" : title;
    snprintf(title, sizeof title, kind == 0x50000 ? "%ds" : "%d", k);
    lv_obj_t *p, *body = page_new(name, &p);
    if (kind == 0x30000) lv_obj_set_user_data(p, (void *)(intptr_t)PG_TAGS);   /* its Play asks the genres again */
    if (!n)
    {
        free(list);
        if (kind == 0x60000) sl_empty(body, IC_HEART, 96, "No favourite albums yet",
                                      "Tap the heart on an album's page, or hold its cover and choose Add to favourites.", 56);
        else sl_empty(body, IC_ALBUMS, 96, "Nothing played yet", "The albums you play show up here, the latest first.", 56);
        page_push(p);
        return;
    }
    int songs = 0;
    for (int i = 0; i < n; i++) songs += sl_lib.albums[list[i]].n_tracks;
    if (songs && kind != 0x70000) play_pair(body, group_play_cb, key, 4);
    char label[48];
    section(body, sl_count(label, sizeof label, n, "%s ALBUM", "%s ALBUMS"));
    grid_view_new(body, list, n, n, NULL);
    page_push(p);
}
/* A list of genres or years: a row each, made as it scrolls */
static lv_obj_t *genre_row(struct row_view *v, int k)
{
    char b[40];
    sl_count(b, sizeof b, tgx.gfirst[k + 1] - tgx.gfirst[k], "%s album", "%s albums");
    (void)v;
    return card_row(v->host, TG_GNAME(k), b, (intptr_t)(0x30000 | k));
}
/* Every genre in the library, with how many albums carry it; "Rock; Pop"
 * counts for both (Library options) */
static void genres_page(void)
{
    lv_obj_t *p, *body = page_new("Genres", &p);
    lv_obj_set_user_data(p, (void *)(intptr_t)PG_TAGS);
    int n = tg_ready() ? tgx.ng : 0;
    if (!n) lib_empty(body, "No genres in your library yet.");
    else
    {
        char b[40];
        section(body, sl_count(b, sizeof b, n, "%s GENRE", "%s GENRES"));
        row_view_new(body, n, genre_row, NULL);
        sl_scroll_indicator(body);
    }
    page_push(p);
}
/* Years: a row a year, newest first, or a row a decade; the order chip
 * switches, and the choice is kept */
static int cmp_year_desc(const void *a, const void *b) { return *(const int *)b - *(const int *)a; }
static lv_obj_t *year_row(struct row_view *v, int k)
{
    int y = v->list[2 * k], c = v->list[2 * k + 1];
    char t[16], b[40];
    bool dec = global_settings.sloppy_years_decades;
    snprintf(t, sizeof t, dec ? "%ds" : "%d", y);
    sl_count(b, sizeof b, c, "%s album", "%s albums");
    return card_row(v->host, t, b, (intptr_t)((dec ? 0x50000 : 0x40000) | y));
}
static void years_page_at(bool replace);
static void years_view_pick(int k)
{
    if (k == global_settings.sloppy_years_decades) return;
    global_settings.sloppy_years_decades = k;
    settings_save();
    years_page_at(true);
}
static void years_view_cb(lv_event_t *e)
{
    (void)e;
    static const char *const names[] = { "By year", "By decade" };
    sort_sheet(names, 2, global_settings.sloppy_years_decades ? 1 : 0, years_view_pick);
}
static void years_page_at(bool replace)
{
    lv_obj_t *p, *body = page_new("Years", &p);
    bool dec = global_settings.sloppy_years_decades;
    /* each year (or decade) with an album, and how many: pairs in one list */
    int *years = malloc(((size_t)sl_lib.n_albums + 1) * sizeof *years), n = 0;
    if (years)
    {
        for (int i = 0; i < sl_lib.n_albums; i++)
            if (sl_lib.albums[i].year > 0) years[n++] = dec ? sl_lib.albums[i].year / 10 * 10 : sl_lib.albums[i].year;
        qsort(years, n, sizeof *years, cmp_year_desc);
    }
    int *pairs = years ? malloc(((size_t)n + 1) * 2 * sizeof *pairs) : NULL, m = 0;
    for (int i = 0; pairs && i < n; i++)
    {
        if (m && pairs[2 * (m - 1)] == years[i]) { pairs[2 * (m - 1) + 1]++; continue; }
        pairs[2 * m] = years[i]; pairs[2 * m + 1] = 1; m++;
    }
    free(years);
    if (!m) { free(pairs); lib_empty(body, "No years in your library yet."); }
    else
    {
        static const char *const names[] = { "By year", "By decade" };
        char b[48];
        sl_count(b, sizeof b, m, dec ? "%s DECADE" : "%s YEAR", dec ? "%s DECADES" : "%s YEARS");
        count_row(body, b, names[dec], years_view_cb);
        row_view_new(body, m, year_row, pairs);
        sl_scroll_indicator(body);
    }
    if (replace) page_replace(p, true); else page_push(p);
}
static void years_page(void) { years_page_at(false); }

/* ---------------------------------------- favourite and recent albums */
/* Favourite albums: a heart on the album's page, or Add to favourites on
 * its sheet. Kept on the card by name, "artist<TAB>title" a line, so they
 * outlast a rescan; newest first. */
#define FAV_FILE ROCKBOX_DIR "/sloppy/favourite_albums.txt"
#define FAV_MAX 4096
static char *fav_buf;
static const char *fav_artist[FAV_MAX], *fav_title[FAV_MAX];
static int fav_n = -1;                  /* -1: not read yet */
static void fav_load(void)
{
    if (fav_n >= 0) return;
    fav_n = 0;
    int fd = open(FAV_FILE, O_RDONLY);
    if (fd < 0) return;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size > 0 && size < 1024 * 1024 && lseek(fd, 0, SEEK_SET) == 0 && (fav_buf = malloc((size_t)size + 1)))
    {
        ssize_t got = read(fd, fav_buf, (size_t)size);
        fav_buf[got > 0 ? got : 0] = '\0';
        for (char *p = fav_buf; *p && fav_n < FAV_MAX; )
        {
            char *e = p + strcspn(p, "\n"), *tab = memchr(p, '\t', (size_t)(e - p));
            bool last = !*e;
            *e = '\0';
            if (tab) { *tab = '\0'; fav_artist[fav_n] = p; fav_title[fav_n++] = tab + 1; }
            if (last) break;
            p = e + 1;
        }
    }
    close(fd);
}
static int fav_find(int a)
{
    fav_load();
    if (a < 0 || a >= sl_lib.n_albums) return -1;
    const struct sl_album *al = &sl_lib.albums[a];
    for (int i = 0; i < fav_n; i++) if (!strcmp(fav_title[i], al->title) && !strcmp(fav_artist[i], al->artist)) return i;
    return -1;
}
static bool fav_has(int a) { return fav_find(a) >= 0; }
static unsigned fav_hash(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static void fav_set(int a, bool on)
{
    int at = fav_find(a);
    if ((at >= 0) == on || a < 0 || a >= sl_lib.n_albums) return;
    const struct sl_album *al = &sl_lib.albums[a];
    if (on && fav_n >= FAV_MAX) { toast("That's as many favourite albums as there can be"); return; }
    /* the file again, this one added at the end or left out; then read back */
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(FAV_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok = fd >= 0;
    for (int i = 0; ok && i <= fav_n; i++)
    {
        if (i == at || (i == fav_n && !on)) continue;
        const char *ar = i < fav_n ? fav_artist[i] : al->artist, *ti = i < fav_n ? fav_title[i] : al->title;
        ok = write(fd, ar, strlen(ar)) == (ssize_t)strlen(ar) && write(fd, "\t", 1) == 1
          && write(fd, ti, strlen(ti)) == (ssize_t)strlen(ti) && write(fd, "\n", 1) == 1;
    }
    if (ok) ok = fsync(fd) == 0;   /* on the card before the rename */
    if (fd >= 0 && close(fd) < 0) ok = false;
    if (!ok || rename(FAV_FILE ".tmp", FAV_FILE) < 0) { remove(FAV_FILE ".tmp"); toast_warn("Could not save that. Check the card."); return; }
    free(fav_buf);
    fav_buf = NULL;
    fav_n = -1;
    fav_load();
}
/* the favourites in the library now, newest first */
static int fav_albums(int **out)
{
    fav_load();
    *out = malloc(((size_t)fav_n + 1) * sizeof **out);
    if (!*out) return 0;
    /* the albums by name once, then each favourite looked up in it */
    int cap = 1024, n = 0;
    while (cap < 2 * sl_lib.n_albums) cap *= 2;
    int *h = malloc((size_t)cap * sizeof *h);
    if (!h) { free(*out); *out = NULL; return 0; }
    for (int i = 0; i < cap; i++) h[i] = -1;
    for (int a = 0; a < sl_lib.n_albums; a++)
    {
        unsigned slot = (fav_hash(sl_lib.albums[a].title) ^ fav_hash(sl_lib.albums[a].artist) * 31u) & (unsigned)(cap - 1);
        while (h[slot] >= 0) slot = (slot + 1) & (unsigned)(cap - 1);
        h[slot] = a;
    }
    for (int i = fav_n - 1; i >= 0; i--)
        for (unsigned slot = (fav_hash(fav_title[i]) ^ fav_hash(fav_artist[i]) * 31u) & (unsigned)(cap - 1);
             h[slot] >= 0; slot = (slot + 1) & (unsigned)(cap - 1))
        {
            const struct sl_album *al = &sl_lib.albums[h[slot]];
            if (!strcmp(fav_title[i], al->title) && !strcmp(fav_artist[i], al->artist)) { (*out)[n++] = h[slot]; break; }
        }
    free(h);
    return n;
}

/* ------------------------------------------------------ home & music */
static bool want_classic;
static const char *want_plugin, *want_plugin_arg;   /* Arcade: a classic Rockbox program to run once the loop ends */
int plugin_load(const char *plugin, const void *parameter);   /* apps/plugin.h, without its world */
static void cf_open(void);
static void spa_open(void);
static void vz_open(void);
/* sloppy_taste.inc, after the Spotify matcher it uses */
static bool ts_blocked_track(int t);
static bool ts_blocked_album(int a);
static bool ts_liked_album(int a);
static bool ts_passed_album(int a);
static bool ts_first_visit(void);
static void taste_page(bool first);
static void loved_page(void);
static void hidden_page(void);
static void ts_hide_artist(const char *artist);
static int *ts_unhidden(const int *tracks, int n, int *out_n);

static void music_page(void);
static void system_page(void);
static void advanced_page(void);
static void settings_search_pill(lv_obj_t *body);
static void sounds_page(void);
static void pet_page(void);
static void ragdoll_page(void);
static void snake_page(void);
static void radio_page(void);
static void radio_tile(lv_obj_t *t, int w, bool wide);
static void spoon_page(void);
static bool spoon_key(int button, bool rel);
static void quiz_page(void);
static void coin_page(void);
static void magic_page(void);
static void chess_page(void);
static void clicker_page(void);
static void metronome_page(void);
static void spacey_page(void);
static void t48_page(void);
static void sudoku_page(void);
static void points_page(void);
static void flappy_page(void);
static void books_page(void);
static void webradio_page(void);
static void podcasts_page(void);
static void ebooks_page(void);
static void subsonic_page(void);          /* sloppy_subsonic.inc: a music server */
static void sub_servers_page(void);
static void abs_page(void);               /* sloppy_abs.inc: Audiobookshelf */
static void abs_books_entry(lv_obj_t *body);
static void abs_srv_group(lv_obj_t *body);
static void sl_saved_playlists_page(int offset);
static void sl_files_page(const char *path, int offset);
static void fm_usb_release(void);
static void sl_games_page(void);
static void arcade_page(void);
static void doom_open(void);

static void sl_library_page(void);
static void scr_page(void);
static void classic_confirm_page(void);
static void dual_boot_confirm_page(void);
static void sl_bt_page(void);
static void foryou_page(void);
static void search_page(void);
static void action(int k)
{
    sl_log("action %d (pages %d)", k, npages);
    if ((k == ACT_ALL || k == ACT_ALBUMS || k == ACT_ARTISTS || k == ACT_ALBUMARTISTS || k == ACT_LOVED ||
         k == ACT_SHUFFLE || k == ACT_FORYOU || k == ACT_SEARCH) && !lib_ready())
    {
        toast("Your library is still loading");
        return;
    }
    switch (k)
    {
        case ACT_MUSIC:   music_page(); break;
        case ACT_ALL:     songs_page(); break;
        case ACT_ALBUMS:  albums_page(); break;
        case ACT_ARTISTS: tartists_page(); break;
        case ACT_ALBUMARTISTS: artists_page(); break;
        case ACT_FAVALBUMS: group_page(0x60000); break;
        case ACT_RECENTALBUMS: group_page(0x70000); break;
        case ACT_GENRES:  genres_page(); break;
        case ACT_METRONOME: metronome_page(); break;
        case ACT_SPACEY:  spacey_page(); break;
        case ACT_2048:    t48_page(); break;
        case ACT_SUDOKU:  sudoku_page(); break;
        case ACT_POINTS:  points_page(); break;
        case ACT_FLAPPY:  flappy_page(); break;
        case ACT_BOOKS:   books_page(); break;
        case ACT_WEBRADIO: webradio_page(); break;
        case ACT_PODCASTS: podcasts_page(); break;
        case ACT_EBOOKS: ebooks_page(); break;
        case ACT_SUBSONIC: subsonic_page(); break;
        case ACT_YEARS:   years_page(); break;
        case ACT_SYSTEM:  system_page(); break;
        case ACT_PET:     pet_page(); break;
        case ACT_RAGDOLL: ragdoll_page(); break;
        case ACT_SNAKE: snake_page(); break;
        case ACT_RADIO: radio_page(); break;
        case ACT_SPOON: spoon_page(); break;
        case ACT_QUIZ: quiz_page(); break;
        case ACT_COIN: coin_page(); break;
        case ACT_MAGIC: magic_page(); break;
        case ACT_CHESS: chess_page(); break;
        case ACT_CLICKER: clicker_page(); break;
        case ACT_COVERFLOW: cf_open(); break;
        case ACT_SPECTRUM: spa_open(); break;
        case ACT_VISUALIZER:
            if (something_playing()) vz_open();
            else toast("Play a song to see the visualizer");
            break;
        case ACT_FORYOU: foryou_page(); break;
        case ACT_LOVED: loved_page(); break;
        case ACT_SEARCH: search_page(); break;
        case ACT_PLAYLISTS: sl_saved_playlists_page(0); break;
        case ACT_FILES: sl_files_page("/", 0); break;
        case ACT_GAMES: sl_games_page(); break;
        case ACT_ARCADE: arcade_page(); break;
        case ACT_DOOM: doom_open(); break;
        case ACT_MARIO: arcade_page(); break;   /* the Game Boy row of old: the Arcade lists every cartridge now */
        case ACT_LIBRARY: sl_library_page(); break;
        case ACT_SHUFFLE:
            if (!sl_lib.n_tracks) toast("Your library is empty");
            else
            {
                int n, *l = ts_unhidden(sl_lib.songs_by_title, sl_lib.n_tracks, &n);   /* no hidden artists */
                if (l && n) play_list(l, n, 0, true);
                else if (!l) play_list(sl_lib.songs_by_title, sl_lib.n_tracks, 0, true);
                free(l);
            }
            break;
        case ACT_NOW:
            if (something_playing()) uq_show();
            else toast("Play a song to see Up next");
            break;
        case ACT_BLUETOOTH:
            sl_bt_page();
            break;
        case ACT_CLASSIC:
            classic_confirm_page();
            break;
        case ACT_REBUILD:
            db_update_start();
            break;
        default: toast("Coming in a later milestone"); break;
    }
}
/* what a row's code opens; also Settings search's way in */
static void qp_display_page(void);
static void update_page(void);
static void card_row_open(intptr_t v)
{
    if (v == 0x20001) eq_page();
    else if (v == 0x20002) play_settings_page();
    else if (v == 0x20003) appearance_page();
    else if (v == 0x20004) theme_page();
    else if (v == 0x20005) accent_page();
    else if (v == 0x20006) wallpaper_page(false);
    else if (v == 0x20007) wallpaper_page(true);
    else if (v == 0x20008) settings_icon_page();
    else if (v == 0x20009) home_style_page();
    else if (v == 0x2000a) sounds_page();
    else if (v == 0x2000b) lock_player_page();
    else if (v == 0x20016) np_style_page();
    else if (v == 0x2001a) cf_style_page();
    else if (v == 0x2000c) screen_sleep_page();
    else if (v == 0x2000d) sound_page();
    else if (v == 0x2000e) buttons_page();
    else if (v == 0x2000f) shortcuts_page();
    else if (v == 0x20010) time_power_page();
    else if (v == 0x20011) lyrics_color_page();
    else if (v == 0x20012) sl_wifi_page();
    else if (v == 0x20013) language_page();
    else if (v == 0x20014) text_size_page();
    else if (v == 0x20020) keyboards_page();
    else if (v == 0x20015) dual_boot_confirm_page();
    else if (v == 0x20017) advanced_page();
    else if (v == 0x20018) sp_page();
    else if (v == 0x20019) reset_all_settings_page();
    else if (v == 0x2001b) scr_page();
    else if (v == 0x2001c) eq_profiles_page();
    else if (v == 0x2001d) settings_layout_page();
    else if (v == 0x2001e) library_options_page();
    else if (v == 0x2001f) tone_page();
    else if (v == 0x20021) qp_display_page();   /* brightness, from Settings search */
    else if (v == 0x20022) lo_seps_page();
    else if (v == 0x20024) lo_folders_page();
    else if (v == 0x20023) lo_keep_page();
    else if (v == 0x20025) libjob_page();
    else if (v == 0x20026) wf_share_cb(NULL);
    else if (v == 0x20027) er_page();
    else if (v == 0x20028) saver_page();
    else if (v == 0x20029) car_page();
    else if (v == 0x2002a) sub_servers_page();
    else if (v == 0x2002b) update_page();
    else if ((v & 0xf0000) >= 0x30000 && (v & 0xf0000) <= 0x70000) group_page(v);
    else if ((v & 0xf0000) == 0x10000) artist_page(v & 0xffff);
    else action((int)v);
}
static void card_row_cb(lv_event_t *e) { card_row_open((intptr_t)lv_event_get_user_data(e)); }
#include "sloppy_kb.inc"
#include "sloppy_browse.inc"
#include "sloppy_bluetooth.inc"
static bool sb_bt_linked(void) { return sl_bt_state.connected; }
static const char *sb_bt_codec(void) { return sl_bt_state.connected ? sl_bt_state.codec : ""; }
static void tile_cb(lv_event_t *e) { action((int)(intptr_t)lv_event_get_user_data(e)); }

/* Home motion: the tiles arrive together, rushing in from the edges. Keep
 * each animation to opacity and translation so it stays inexpensive on the
 * R1's software renderer: a transform (scale) makes LVGL draw the object
 * into an off-screen layer on every frame. */
static unsigned home_intro_ms;   /* extra wait before the cascade (boot splash) */
static void home_tile_motion(void *obj, int32_t value)
{
    lv_obj_set_style_translate_y(obj, value, 0);
}
static void home_tile_fade(void *obj, int32_t value)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)value, 0);
}
/* A long, soft deceleration (easeOutQuint): motion lands without a visible stop. */
static void motion_decel(lv_anim_t *a)
{
    lv_anim_set_path_cb(a, lv_anim_path_custom_bezier3);
    a->parameter.bezier3 = (lv_anim_bezier3_para_t){ LV_BEZIER_VAL_FLOAT(0.22), LV_BEZIER_VAL_FLOAT(1.0),
                                                     LV_BEZIER_VAL_FLOAT(0.36), LV_BEZIER_VAL_FLOAT(1.0) };
}
/* iPhone-style arrival: each tile starts pushed out toward the screen edge,
 * away from the centre, and rushes in to its place as it fades up. The push
 * is measured when the tile starts moving, once the layout has placed it,
 * and kept in the animation: (dx << 16) | dy. Translation and opacity only. */
static void home_rush_step(lv_anim_t *a, int32_t v)
{
    intptr_t d = (intptr_t)lv_anim_get_user_data(a);
    int dx = (int16_t)(d >> 16), dy = (int16_t)(d & 0xffff);
    int opa = (1024 - v) * 255 / 640;
    lv_obj_set_style_translate_x(a->var, dx * v / 1024, 0);
    lv_obj_set_style_translate_y(a->var, dy * v / 1024, 0);
    lv_obj_set_style_opa(a->var, (lv_opa_t)(opa > 255 ? 255 : opa), 0);
}
static void home_rush_start(lv_anim_t *a)
{
    lv_area_t c;
    lv_obj_update_layout(a->var);        /* a page built this pass has no places yet */
    lv_obj_get_coords(a->var, &c);
    int dx = ((c.x1 + c.x2) / 2 - W / 2) * 2 / 5;   /* two fifths further out */
    int dy = ((c.y1 + c.y2) / 2 - H / 2) * 2 / 5;
    dx = LV_CLAMP(-W, dx, W);
    dy = LV_CLAMP(-H, dy, H);
    lv_anim_set_user_data(a, (void *)(intptr_t)((dx << 16) | (dy & 0xffff)));
}
static void home_rush_in(lv_obj_t *tile, unsigned delay)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tile);
    lv_anim_set_values(&a, 1024, 0);
    lv_anim_set_duration(&a, SL_T_ENTER);
    lv_anim_set_delay(&a, delay);
    motion_decel(&a);
    lv_anim_set_start_cb(&a, home_rush_start);
    lv_anim_set_custom_exec_cb(&a, home_rush_step);   /* early: hidden while it waits */
    lv_anim_start(&a);
}

/* The icon trails its tile a little and settles with a slight overshoot,
 * which reads as the old scale "pop" without rendering a scaled layer. */
static void home_icon_rise(lv_obj_t *ic, unsigned delay)
{
    lv_obj_set_style_translate_y(ic, 14, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ic);
    lv_anim_set_values(&a, 14, 0);
    lv_anim_set_duration(&a, SL_T_ENTER);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&a, home_tile_motion);
    lv_anim_start(&a);
}

/* The Cards home: one card system with Settings and the Music page (their
 * radius, hairline, gutter and lit icon squares), a wide Music card that
 * says what is in the library and shuffles it, then six destinations two
 * across, each an icon over its name. Every size comes from the tokens; the
 * names grow with the text size. */
#define HOME_CARD_W ((W - 3 * SL_GUTTER) / 2)
#define HOME_ICON   56                   /* a destination's lit square: Settings and the Music page use the same */
static lv_obj_t *lit_square(lv_obj_t *parent, uint32_t tint, int size);
static lv_obj_t *home_card(lv_obj_t *grid, int w)
{
    lv_obj_t *c = box(grid);
    lv_obj_set_size(c, w, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(c, SL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(c, w / 2, 0);
    sl_elevate(c, SL_E1, 0);
    lv_obj_set_style_pad_all(c, SL_INSET, 0);
    press_motion(c);
    return c;
}
static void home_cell(lv_obj_t *grid, const char *sym, int icon, const char *label, uint32_t icbg, int k, int col, int row)
{
    lv_obj_t *c = home_card(grid, HOME_CARD_W);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, SL_SP_S + SL_SP_XS, 0);
    lv_obj_t *ic = lit_square(c, icbg, HOME_ICON);   /* child 0: the icon rises after the card */
    if (k == ACT_SYSTEM)
    {
        home_settings_icon = ic;
        home_icon_small = true;
        settings_icon_fill(ic, global_settings.sloppy_skull_icon);
    }
    else if (icon) lv_obj_center(sl_icon(ic, icon, 34, 0xffffff, icbg));
    else lv_obj_center(text_light(ic, sym, &sl_sym_32));
    lv_obj_t *l = text(c, label, &ss_semibold_21, 0xffffff);
    lv_obj_set_width(l, lv_pct(100));
    label_lines(l, 1);
    if (sl_ui_rtl())   /* icon and name from the right edge */
    {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_margin_left(ic, HOME_CARD_W - 2 * SL_INSET - HOME_ICON, 0);
    }
    on_click(c, tile_cb, (void *)(intptr_t)k);
    unsigned delay = home_intro_ms + (unsigned)(row * 30 + col * 15);
    home_rush_in(c, delay);
    home_icon_rise(ic, delay + 60);
}
static void home_shuffle_cb(lv_event_t *e) { (void)e; action(ACT_SHUFFLE); }
/* Music, wide: its icon, what the library holds, and Shuffle all */
static void home_music_card(lv_obj_t *grid)
{
    lv_obj_t *c = home_card(grid, W - 2 * SL_GUTTER);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, SL_GUTTER + 2, 0);
    lv_obj_t *ic = lit_square(c, 0x4487a7, 64);   /* child 0, as every card */
    lv_obj_center(text_light(ic, LV_SYMBOL_AUDIO, &sl_sym_40));
    lv_obj_t *words = box(c);
    lv_obj_set_flex_grow(words, 1);
    lv_obj_set_height(words, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(words, 2, 0);
    lv_obj_t *t = text(words, "Music", &ss_semibold_30, 0xffffff);
    lv_obj_set_width(t, lv_pct(100));
    label_lines(t, 1);
    char b[96], n1[16], n2[16];
    snprintf(b, sizeof b, "%s %s \xc2\xb7 %s %s", sl_fmt_count(n1, sizeof n1, sl_lib.n_tracks),
             sl_tr_count(sl_lib.n_tracks, "song", "songs"), sl_fmt_count(n2, sizeof n2, sl_lib.n_albums),
             sl_tr_count(sl_lib.n_albums, "album", "albums"));
    if (sl_ui_rtl() && sl_lib.n_tracks)
    {
        /* Hebrew and Arabic: LVGL's bidi puts the second count on the wrong
         * side of the dot when one line holds two, so each "count word" is
         * its own label in a right-to-left row */
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_t *row = box(words);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_base_dir(row, LV_BASE_DIR_RTL, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        snprintf(b, sizeof b, "%s %s", n1, sl_tr_count(sl_lib.n_tracks, "song", "songs"));
        text(row, b, &ss_regular_18, C_SUB);
        text(row, " \xc2\xb7 ", &ss_regular_18, C_SUB);
        snprintf(b, sizeof b, "%s %s", n2, sl_tr_count(sl_lib.n_albums, "album", "albums"));
        text(row, b, &ss_regular_18, C_SUB);
    }
    else
    {
        lv_obj_t *n = text(words, sl_lib.n_tracks ? b : "Tap to find your music", &ss_regular_18, C_SUB);
        lv_obj_set_width(n, lv_pct(100));
        label_lines(n, 1);
        if (sl_ui_rtl())
        {
            lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }
    if (sl_lib.n_tracks)
    {
        /* Shuffle all, a round accent button: the card's one shortcut */
        lv_obj_t *s = box(c);
        lv_obj_set_size(s, SL_PILL_H - 4, SL_PILL_H - 4);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s, lv_color_darken(lv_color_hex(C_ACCENT), 50), LV_STATE_PRESSED);
        lv_obj_set_style_transform_scale(s, SL_PRESSED, LV_STATE_PRESSED);
        press_motion(s);
        lv_obj_t *g = text(s, LV_SYMBOL_SHUFFLE, &sl_sym_24, sl_accent_ink());
        lv_obj_set_style_text_color(g, lv_color_hex(sl_accent_ink()), 0);
        lv_obj_center(g);
        on_click(s, home_shuffle_cb, NULL);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_EVENT_BUBBLE);   /* its own tap, not the card's */
    }
    on_click(c, tile_cb, (void *)(intptr_t)ACT_MUSIC);
    home_rush_in(c, home_intro_ms);
    home_icon_rise(ic, home_intro_ms + 60);
}
#include "sloppy_refresh.inc"
static int hist_recent_albums(int *albums, int max);
#include "sloppy_zune.inc"
#include "sloppy_metro.inc"
#include "sloppy_spectrum.inc"
#include "sloppy_visualizer.inc"
#include "sloppy_npwave.inc"
#include "sloppy_gabagool.inc"

static void reset_all_settings_cancel_cb(lv_event_t *e)
{
    (void)e;
    page_pop();
}

static void reset_all_settings_apply_cb(lv_event_t *e)
{
    (void)e;
    /* Rockbox owns the persisted preferences, including GabagoolOS theme and
     * playback choices. The tile arrangement is a separate preference file. */
    settings_reset();
    settings_save();
    settings_apply(true);
    settings_apply_skins();

    bool tiles_reset = true;
    int fd = open(MT_FILE, O_RDONLY);
    if (fd >= 0)
    {
        close(fd);
        tiles_reset = remove(MT_FILE) == 0;
    }

    /* The running GabagoolOS session keeps touch and display ownership after
     * Rockbox applies its defaults. */
    touchscreen_set_mode(TOUCHSCREEN_POINT);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    if (sl_power_running) { sl_power_take_display(); sl_power_apply(); }

    appearance_reload_soon(true); /* return to Settings */
    toast(tiles_reset ? "Settings restored" : "Settings restored; tiles could not reset");
}

static void reset_all_settings_page(void)
{
    lv_obj_t *p, *body = page_new("Reset all settings?", &p);
    set_confirm(body, "RESTORE DEFAULTS",
        "This restores system, playback, sound and appearance settings, plus your pinned tile layout. "
        "Music, playlists, pet and game progress stay on the player.",
        "Keep my settings", "Go back without changing anything", reset_all_settings_cancel_cb,
        "Reset all settings", "Restore the defaults now", reset_all_settings_apply_cb, true);
    page_push(p);
}

static void reset_all_settings_open_cb(lv_event_t *e)
{
    (void)e;
    reset_all_settings_page();
}

static void home_page(void)
{
    if (sl_home_tiles()) { metro_home_page(); return; }
    if (global_settings.sloppy_home_style == 2) { zune_home_page(); return; }
    lv_obj_t *p = box(ui_screen());
    lv_obj_add_style(p, &st_page, 0);
    lv_obj_set_size(p, W, H);
    lv_obj_add_event_cb(p, sl_wallpaper_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p, wallpaper_nexus_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_t *body = lv_obj_create(p);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, SB_H);
    /* Let the list reach the screen edge. The mini player floats above it;
     * scroll padding keeps the last app row reachable when playback is on. */
    lv_obj_set_size(body, W, H - SB_H);
    lv_obj_add_event_cb(body, wallpaper_nexus_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_hor(body, SL_GUTTER, 0);
    lv_obj_set_style_pad_top(body, SL_SP_S, 0);
    lv_obj_set_style_pad_bottom(body, 110, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *g = box(body);
    /* Music wide, then the six two across on the page's gutter */
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, SL_GUTTER, 0);
    lv_obj_set_style_pad_row(g, SL_GUTTER, 0);
    lv_obj_add_flag(g, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* the cards' press shrink and hairlines, whole */
    if (sl_ui_rtl()) lv_obj_set_style_base_dir(g, LV_BASE_DIR_RTL, 0);   /* Hebrew and Arabic: from the right */
    home_music_card(g);
    home_cell(g, NULL, IC_PLAYLISTS, "Playlists", 0x697da5, ACT_PLAYLISTS, 0, 0);
    home_cell(g, NULL, IC_UPNEXT, "Up next", 0x7aa689, ACT_NOW, 1, 0);
    home_cell(g, NULL, IC_COVERFLOW, "Cover Flow", 0xd0663c, ACT_COVERFLOW, 0, 1);
    home_cell(g, NULL, IC_PLAYROOM, "Playroom", 0xc47d70, ACT_GAMES, 1, 1);
    home_cell(g, LV_SYMBOL_BLUETOOTH, IC_NONE, "Bluetooth", 0x4e999f, ACT_BLUETOOTH, 0, 2);
    home_cell(g, LV_SYMBOL_SETTINGS, IC_NONE, "Settings", 0x657b93, ACT_SYSTEM, 1, 2);
    lv_obj_t *head = section(body, "APPS & GAMES");
    lv_obj_set_style_pad_top(head, SL_GUTTER + SL_SP_S, 0);
    /* it sits on the wallpaper, not a card: on the photos (all dark) it
     * goes white like the status bar, or the light theme's grey vanishes */
    if (wallpaper_id(false) >= 5) lv_obj_set_style_text_color(head, lv_color_hex(0xdde3ea), 0);
    sl_app_row(body, "Files", "Every folder on the card", ACT_FILES, IC_FILES, 0xa88a5c);
    sl_app_row(body, "Tamagotchi", "A little friend, always growing", ACT_PET, IC_PET, 0xb57c8f);
    sl_app_row(body, "Ragdoll", "Grab, toss and watch it tumble", ACT_RAGDOLL, IC_RAGDOLL, 0x6199a1);
    sl_app_row(body, "Snake", "Classic walls or level challenge", ACT_SNAKE, IC_SNAKE, 0x6aa87c);
    sl_app_row(body, "2048", "Slide the tiles, join the twins", ACT_2048, IC_2048, 0xd9893b);
    sl_app_row(body, "Sudoku", "Four levels, with notes and hints", ACT_SUDOKU, IC_SUDOKU, 0x4f7fe0);
    sl_app_row(body, "24 Points", "Make 24 from four cards", ACT_POINTS, IC_POINTS, 0xc2574a);
    sl_app_row(body, "Flappy Bird", "Tap to flap through the pipes", ACT_FLAPPY, IC_FLAPPY, 0x4fb8c4);
    sl_app_row(body, "Sloppy FM", "Tap to tune and grow your station", ACT_RADIO, IC_RADIO, 0x6b9eaa);
    sl_app_row(body, "Freaky Spoon", "Bounce up with a very strange spoon", ACT_SPOON, IC_SPOON, 0x947da7);
    sl_app_row(body, "Music Bip Bop Smarty", "Music, movies and TV trivia", ACT_QUIZ, IC_QUIZ, 0x738b7d);
    sl_app_row(body, "Flip a Coin", "A quick call for heads or tails", ACT_COIN, IC_COIN, 0xba8957);
    sl_app_row(body, "Magic 8 Ball", "Ask a question and tap the ball", ACT_MAGIC, IC_MAGIC, 0x6d83ba);
    sl_app_row(body, "Chess", "Play the computer or a friend", ACT_CHESS, IC_CHESS, 0x8a6a4a);
    sl_app_row(body, "Clicker", "Count anything, one tap at a time", ACT_CLICKER, IC_CLICKER, 0x4e9e8f);
    sl_app_row(body, "Metronome", "Keep time, 30 to 250 beats a minute", ACT_METRONOME, IC_METRONOME, 0xc4794a);
    sl_app_row(body, "Arcade", "Game Boy games from your card, and Doom", ACT_ARCADE, IC_CARTRIDGE, 0x7a5bd6);
    sl_app_row(body, "Spectrum", "Full screen, sideways", ACT_SPECTRUM, IC_SPECTRUM, 0x8a4fc9);
    sl_app_row(body, "Visualizer", "Your music, painting the screen", ACT_VISUALIZER, IC_VISUALIZER, 0x4b6fd6);
    sl_app_row(body, "Spacey Lines", "Draw into a spinning tunnel", ACT_SPACEY, IC_SPACEY, 0xb5568a);
    ptr_attach(p, body, wallpaper_id(false) >= 5 ? 0xffffff : C_TEXT);
    pages[npages++] = p;
}

/* sym, or a drawn icon (sl_icon) when sym is NULL */
/* A rounded square in a colour, lit from above: the icon tiles of
 * Settings and of the Music page. */
static lv_obj_t *lit_square(lv_obj_t *parent, uint32_t tint, int size)
{
    lv_obj_t *ic = box(parent);
    lv_obj_set_size(ic, size, size);
    lv_obj_set_style_radius(ic, SL_R_ICON(size), 0);
    lv_obj_set_style_bg_color(ic, lv_color_lighten(lv_color_hex(tint), 36), 0);
    lv_obj_set_style_bg_grad_color(ic, lv_color_hex(tint), 0);
    lv_obj_set_style_bg_grad_dir(ic, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    return ic;
}
/* The hairline edge the panels of Settings have (set_group()). */
static void hairline(lv_obj_t *o)
{
    lv_obj_set_style_border_color(o, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_border_opa(o, 36, 0);
    lv_obj_set_style_border_width(o, sl_zune() ? 0 : 1, 0);
}
/* A tile sinks under the finger: the card's colour change plus a small
 * scale about its centre, on the press transitions. */
static void press_sink(lv_obj_t *o, int w, int h)
{
    lv_obj_set_style_transform_scale(o, SL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, h / 2, 0);
}
/* A Music tile is a Cards home destination: the same card, its lit icon
 * at the leading top, its name at the leading foot, and what it holds (a
 * count in the language's own digits grouping) at the trailing top. h is
 * the height the page's fold asks for, never less than the words need. */
#define MU_TILE_W HOME_CARD_W
static int music_tile_h(void)
{
    return 2 * SL_INSET + HOME_ICON + SL_SP_S + SL_SP_XS + lv_font_get_line_height(&ss_semibold_21);
}
static int music_low_h(void)
{
    return LV_MAX(SL_PILL_H + SL_SP_S, lv_font_get_line_height(&ss_semibold_21) + 2 * SL_GUTTER);
}
static void music_tile(lv_obj_t *g, int icon, const char *label, uint32_t c1, int k, int count, int h)
{
    bool rtl = sl_ui_rtl();
    lv_obj_t *t = home_card(g, MU_TILE_W);
    lv_obj_set_height(t, h);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_set_style_transform_pivot_y(t, h / 2, 0);
    lv_obj_t *ic = lit_square(t, c1, HOME_ICON);
    lv_obj_align(ic, rtl ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, rtl ? -SL_INSET : SL_INSET, SL_INSET);
    lv_obj_center(sl_icon(ic, icon, 34, 0xffffff, c1));
    lv_obj_t *l = text(t, label, &ss_semibold_21, 0xffffff);
    lv_obj_set_width(l, MU_TILE_W - 2 * SL_INSET);
    lv_obj_set_style_text_align(l, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    label_lines(l, 1);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, SL_INSET, -(SL_INSET - SL_SP_XS));
    if (count >= 0)
    {
        char n[16];
        static const lv_font_t *const f[SL_TEXT_STEPS] = { &ss_regular_21, &ss_regular_23, &ss_regular_26, &ss_regular_26 };
        lv_obj_t *c = text(t, sl_fmt_count(n, sizeof n, count), f[sl_text_step], C_SUB);
        /* on the icon's middle, at the far edge of its card; Zule has no
         * card to end at, so the count sits a gutter after the icon */
        int cy = SL_INSET + (HOME_ICON - lv_font_get_line_height(f[sl_text_step])) / 2;
        if (sl_zune()) lv_obj_align(c, rtl ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT,
                                    rtl ? -(SL_INSET + HOME_ICON + SL_GUTTER) : SL_INSET + HOME_ICON + SL_GUTTER, cy);
        else lv_obj_align(c, rtl ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, rtl ? SL_INSET : -SL_INSET, cy);
    }
    on_click(t, tile_cb, (void *)(intptr_t)k);
}
/* More ways in, a low card each: the name, and a chevron the way the
 * page reads */
static void music_low(lv_obj_t *g, const char *label, int k, int h)
{
    bool rtl = sl_ui_rtl();
    lv_obj_t *c = home_card(g, MU_TILE_W);
    lv_obj_set_height(c, h);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_transform_pivot_y(c, h / 2, 0);
    int lw = MU_TILE_W - 2 * SL_INSET - (sl_zune() ? 0 : 24);
    const char *tr = sl_tr(label);   /* a size down where it would be cut ("Favourite albums") */
    lv_obj_t *l = text(c, label, lv_text_get_width(tr, strlen(tr), &ss_semibold_21, 0) > lw ? &ss_semibold_18 : &ss_semibold_21, 0xffffff);
    lv_obj_set_width(l, lw);
    lv_obj_set_style_text_align(l, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    label_lines(l, 1);
    lv_obj_align(l, rtl ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, rtl ? -SL_INSET : SL_INSET, 0);
    if (!sl_zune())
        lv_obj_align(text(c, rtl ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, &sl_sym_18, C_DIM),
                     rtl ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, rtl ? SL_INSET - 2 : -(SL_INSET - 2), 0);
    on_click(c, tile_cb, (void *)(intptr_t)k);
}
static lv_obj_t *sq_glass(lv_obj_t *parent, uint32_t color);
/* Music: a search bar across the top, then six tiles. Files and Cover Flow
 * live on the home screen; shuffle is the big button atop All songs. */
/* The Music page's ways in, in the order and with the ones you keep:
 * music.txt on the card, "SLM1" then a line each, "<id> <hidden>". The
 * first six are tiles, the rest low cards, wherever they are moved. */
#define MU_FILE ROCKBOX_DIR "/sloppy/music.txt"
#define MU_N 16
static const struct { const char *sym; int icon; const char *name; uint32_t tint; int act; bool tile; } mu_defs[MU_N] = {
    { NULL, IC_FORYOU, "For you", 0xdb2f9c, ACT_FORYOU, true },
    { NULL, IC_SONGS, "Songs", 0x2d8ff0, ACT_ALL, true },
    { NULL, IC_ALBUMS, "Albums", 0x34b84e, ACT_ALBUMS, true },
    { NULL, IC_ARTISTS, "Artists", 0xf0872e, ACT_ARTISTS, true },
    { NULL, IC_PLAYLISTS, "Playlists", 0x697da5, ACT_PLAYLISTS, true },
    { NULL, IC_HEART, "Loved", 0xd94a4a, ACT_LOVED, true },
    { NULL, IC_NONE, "Genres", 0, ACT_GENRES, false },
    { NULL, IC_NONE, "Years", 0, ACT_YEARS, false },
    { NULL, IC_NONE, "Audiobooks", 0, ACT_BOOKS, false },
    { NULL, IC_NONE, "Radio", 0, ACT_WEBRADIO, false },
    { NULL, IC_NONE, "Album artists", 0, ACT_ALBUMARTISTS, false },
    { NULL, IC_NONE, "Favourite albums", 0, ACT_FAVALBUMS, false },
    { NULL, IC_NONE, "Recently played", 0, ACT_RECENTALBUMS, false },
    { NULL, IC_NONE, "Podcasts", 0, ACT_PODCASTS, false },
    { NULL, IC_NONE, "Books", 0, ACT_EBOOKS, false },
    { NULL, IC_NONE, "Server", 0, ACT_SUBSONIC, false },
};
/* the order they come in until moved: the library's ways in first, then
 * Audiobooks, Books, Podcasts, a music server and Radio (ids are mu_defs' places, kept in music.txt) */
static const uint8_t mu_first_order[MU_N] = { 0, 1, 2, 3, 4, 5, 10, 6, 7, 11, 12, 8, 14, 13, 15, 9 };
static uint8_t mu_order[MU_N];
static bool mu_hidden[MU_N], mu_read;
static void mu_load(void)
{
    if (mu_read) return;
    mu_read = true;
    for (int i = 0; i < MU_N; i++) { mu_order[i] = mu_first_order[i]; mu_hidden[i] = false; }
    char b[16 + MU_N * 8];
    int fd = open(MU_FILE, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got < 5 || strncmp(b, "SLM1\n", 5)) return;
    b[got] = '\0';
    bool seen[MU_N] = { false };
    uint8_t order[MU_N];
    int n = 0;
    for (char *q = b + 5; *q && n < MU_N; )
    {
        char *end;
        long id = strtol(q, &end, 10), hid;
        if (end == q) break;
        hid = strtol(end, &q, 10);
        if (id < 0 || id >= MU_N || seen[id]) continue;
        seen[id] = true;
        order[n++] = (uint8_t)id;
        mu_hidden[id] = hid != 0;
    }
    for (int i = 0; i < MU_N; i++) if (!seen[mu_first_order[i]]) order[n++] = mu_first_order[i];   /* added since: at the end */
    memcpy(mu_order, order, sizeof mu_order);
}
static void mu_save(void)
{
    char b[16 + MU_N * 8];
    int len = snprintf(b, sizeof b, "SLM1\n");
    for (int i = 0; i < MU_N; i++) len += snprintf(b + len, sizeof b - len, "%d %d\n", mu_order[i], mu_hidden[mu_order[i]]);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(MU_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, len) == len;
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(MU_FILE ".tmp", MU_FILE) < 0) remove(MU_FILE ".tmp");
}
static void music_page(void);
static void mu_arrange_page(void);
static lv_obj_t *mu_page_obj;            /* the Music page, rebuilt when Arrange Music closes over it */
static bool mu_dirty;
static void music_page_at(bool replace);
static void mu_page_deleted(lv_event_t *e) { if (lv_event_get_target(e) == mu_page_obj) mu_page_obj = NULL; }
static void mu_refresh_async(void *unused)
{
    (void)unused;
    if (mu_dirty && mu_page_obj && npages && pages[npages - 1] == mu_page_obj) music_page_at(true);
    mu_dirty = false;
}
static void mu_arrange_cb(lv_event_t *e) { (void)e; mu_arrange_page(); }
static void music_page(void) { music_page_at(false); }
static void music_page_at(bool replace)
{
    mu_load();
    bool rtl = sl_ui_rtl();
    lv_obj_t *p, *body = page_new("Music", &p);
    mu_page_obj = p;
    lv_obj_add_event_cb(p, mu_page_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(body, SL_GUTTER, 0);
    lv_obj_set_style_pad_row(body, SL_GUTTER, 0);
    /* Hebrew and Arabic: the grid starts at the right, as the page reads */
    if (rtl) lv_obj_set_style_base_dir(body, LV_BASE_DIR_RTL, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* the cards' press shrink, whole */
    lv_obj_t *bar = box(body);
    lv_obj_set_size(bar, lv_pct(100), 2 * SL_R_PILL);
    lv_obj_set_style_radius(bar, SL_R_PILL, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    hairline(bar);
    press_motion(bar);
    lv_obj_align(sq_glass(bar, C_SUB), rtl ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, rtl ? -SL_INSET : SL_INSET, 0);
    /* the placeholder is the lightest words on the page: regular, a size
     * up from the body's and with the text size */
    static const lv_font_t *const ph_font[SL_TEXT_STEPS] = { &ss_regular_21, &ss_regular_23, &ss_regular_26, &ss_regular_26 };
    lv_obj_t *ph = text(bar, "Songs, albums, artists", ph_font[sl_text_step], C_SUB);
    lv_obj_set_width(ph, W - 2 * SL_GUTTER - 2 * SL_INSET - 24 - SL_GUTTER);
    lv_obj_set_style_text_align(ph, rtl ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    label_lines(ph, 1);
    lv_obj_align(ph, rtl ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, rtl ? -(SL_INSET + 24 + SL_GUTTER) : SL_INSET + 24 + SL_GUTTER, 0);
    on_click(bar, tile_cb, (void *)(intptr_t)ACT_SEARCH);
    /* A deliberate fold: with the mini player up, the last row on screen
     * ends a gutter above it and the next starts under it. The tiles take
     * the height that lands it, after the low cards' first row or after
     * the tiles themselves, and never less than their words need. */
    int nt = 0, nl = 0;
    for (int i = 0; i < MU_N; i++) if (!mu_hidden[i]) { if (mu_defs[i].tile) nt++; else nl++; }
    int rows = (nt + 1) / 2, hc = music_tile_h(), lh = music_low_h(), h = hc;
    int y0 = SB_H + SL_HDR_H + SL_SP_XS + 2 * SL_R_PILL + SL_GUTTER;     /* the tiles' top on screen */
    int fold = H - SL_FLOAT - 82 - SL_GUTTER;                            /* a gutter above the mini player */
    if (rows)
    {
        int a = nl ? (fold - y0 - rows * SL_GUTTER - lh) / rows : 0, b = (fold - y0 - (rows - 1) * SL_GUTTER) / rows;
        if (a >= hc && a <= hc + SL_SP_S + SL_SP_XS) h = a;
        else if (b >= hc && b <= hc + 2 * SL_GUTTER) h = b;
    }
    /* six colours a step apart round the wheel, no two neighbours alike;
     * then more ways in as low cards; in the order Arrange Music keeps */
    for (int pass = 0; pass < 2; pass++)   /* the tiles, then the cards */
    for (int i = 0; i < MU_N; i++)
    {
        int id = mu_order[i];
        if (mu_hidden[id] || mu_defs[id].tile != !pass) continue;
        if (mu_defs[id].tile)
        {
            int act = mu_defs[id].act;
            int count = act == ACT_ALL ? sl_lib.n_tracks : act == ACT_ALBUMS ? sl_lib.n_albums : act == ACT_ARTISTS ? tg_n_artists() : -1;
            music_tile(body, mu_defs[id].icon, mu_defs[id].name, mu_defs[id].tint, act, count, h);
        }
        else music_low(body, mu_defs[id].name, mu_defs[id].act, lh);
    }
    if (sl_lib.n_tracks) header_button(p, LV_SYMBOL_SHUFFLE, tile_cb, (void *)(intptr_t)ACT_SHUFFLE);
    header_button(p, LV_SYMBOL_EDIT, mu_arrange_cb, NULL);
    if (replace) page_replace(p, true); else page_push(p);
}

static void classic_accept_cb(lv_event_t *e)
{
    (void)e;
#ifndef SLOPPY_STRESS
    want_classic = true;
#endif
}
static void classic_cancel_cb(lv_event_t *e) { (void)e; page_pop(); }
static void dual_boot_accept_cb(lv_event_t *e)
{
    (void)e;
#ifdef HIBY_R1
    const char *path = ROCKBOX_DIR "/sloppy/boot-menu.once";
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool saved = fd >= 0;
    if (saved)
    {
        saved = write(fd, "1", 1) == 1 && fsync(fd) == 0;
        if (close(fd) != 0) saved = false;
    }
    if (!saved)
    {
        remove(path);
        toast_warn("Could not prepare the boot menu. Check the card.");
        return;
    }
    settings_save();
    hist_flush();
    notes_flush();
    sl_log_flush();
    system_reboot();
#else
    toast("The dual boot screen is available on the R1 only.");
#endif
}
static void dual_boot_confirm_page(void)
{
    lv_obj_t *p, *body = page_new("Dual boot screen", &p);
    set_confirm(body, "RESTART",
        "Restart to choose GabagoolOS, the HiBy player or Tools. The choice screen appears once; later starts go straight to GabagoolOS. Hold Volume Down at power-on to open it again.",
        "Stay in GabagoolOS", "Keep listening", classic_cancel_cb,
        "Restart to dual boot", "Show the player selection screen once", dual_boot_accept_cb, false);
    page_push(p);
}
static void classic_confirm_page(void)
{
    lv_obj_t *p, *body = page_new("Open Rockbox?", &p);
    set_confirm(body, "LEAVE GABAGOOLOS MENUS",
        "Classic Rockbox opens its original menus. Your music stays on the player. GabagoolOS returns after a restart.",
        "Stay in GabagoolOS", "Keep using this interface", classic_cancel_cb,
        "Open Classic Rockbox", "Confirm the switch", classic_accept_cb, false);
    page_push(p);
}

#define WF_KB_H KB_H                    /* the keyboard's height (sloppy_kb.inc), for pages that make room for it */
static void wf_kb_build(lv_obj_t *p, lv_obj_t *ta, int kind, lv_event_cb_t enter, const char *enter_label);
#include "sloppy_pet.inc"
#include "sloppy_ragdoll.inc"
#include "sloppy_snake.inc"
#include "sloppy_radio.inc"
#include "sloppy_spoon.inc"
#include "sloppy_chance.inc"
#include "sloppy_chess.inc"
#include "sloppy_clicker.inc"
#include "sloppy_metronome.inc"
#include "sloppy_spacey.inc"
#include "sloppy_books.inc"

/* empty or missing database: explain and offer to build it */
static lv_obj_t *nolib, *nolib_txt;
static void nolib_build(void)
{
    nolib = box(ui_screen());
    lv_obj_add_style(nolib, &st_page, 0);
    lv_obj_set_size(nolib, W, H);
    lv_obj_t *t = text(nolib, "Getting your music ready", &ss_bold_28, 0xffffff);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 220);
    lv_obj_t *s = text(nolib, "Rockbox is building its music database.\nThis happens once and takes a few minutes.", &ss_regular_18, C_SUB);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 270);
    lv_obj_t *sp = lv_spinner_create(nolib);
    lv_obj_set_size(sp, 80, 80);
    lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 360);
    lv_obj_set_style_arc_color(sp, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    nolib_txt = text(nolib, "", &ss_regular_16, C_DIM);
    lv_obj_set_width(nolib_txt, W - 60);
    lv_obj_set_style_text_align(nolib_txt, LV_TEXT_ALIGN_CENTER, 0);
    label_lines(nolib_txt, 2);
    lv_obj_align(nolib_txt, LV_ALIGN_TOP_MID, 0, 470);
}
/* what the database builder is doing right now */
static void nolib_update(void)
{
    if (!nolib_txt) return;
    struct tagcache_stat *st = tagcache_get_stat();
    char b[96];
    const char *cur = (const char *)st->curentry;
    if (st->commit_delayed)
        strlcpy(b, "Almost done. Restart the player to finish.", sizeof b);
    else if (st->commit_step > 0)
        snprintf(b, sizeof b, "Saving the library (step %d)", st->commit_step);
    else if (cur)
    {
        const char *leaf = strrchr(cur, '/');
        snprintf(b, sizeof b, "Looking in %s", leaf && leaf[1] ? leaf + 1 : cur);
    }
    else
        strlcpy(b, "Starting", sizeof b);
    if (strcmp(lv_label_get_text(nolib_txt), b))
        lv_label_set_text(nolib_txt, b);
}

/* The loading banner rises in, carries a progress track that glides between
 * the quarter-second updates (or sweeps while the total is unknown), and
 * fades away when the library lands instead of vanishing mid-frame. */
#define BANNER_TRACK (W - 88)
static void banner_set_y(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void banner_set_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void banner_set_w(void *o, int32_t v) { lv_obj_set_width(o, v); }
static void banner_set_x(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void banner_gone(lv_anim_t *a) { lv_obj_delete(a->var); }
static void banner_fade(lv_obj_t *o, int y0, int y1, int o0, int o1, int ms, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_values(&a, y0, y1);
    lv_anim_set_exec_cb(&a, banner_set_y);
    lv_anim_start(&a);
    lv_anim_set_values(&a, o0, o1);
    lv_anim_set_exec_cb(&a, banner_set_opa);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
static void banner_sweep(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lib_banner_fill);
    lv_anim_set_values(&a, 0, BANNER_TRACK * 7 / 10);
    lv_anim_set_duration(&a, SL_T_LOOP);
    lv_anim_set_playback_duration(&a, SL_T_LOOP);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, banner_set_x);
    lv_anim_start(&a);
}
static void banner_show(void)
{
    if (lib_banner || !sl_lib_scanning()) return;   /* library.bin is read in under a second: no banner */
    lib_banner = box(lv_layer_top());
    lv_obj_set_size(lib_banner, W - 40, 60);
    lv_obj_align(lib_banner, LV_ALIGN_BOTTOM_MID, 0, -110);
    lv_obj_set_style_radius(lib_banner, SL_R_LG, 0);
    /* a lifted surface, so it floats above the tiles rather than cutting them */
    lv_obj_set_style_bg_color(lib_banner, lv_color_hex(C_CARD_P), 0);
    lv_obj_set_style_bg_opa(lib_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(lib_banner, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_border_opa(lib_banner, LV_OPA_30, 0);
    lv_obj_set_style_border_width(lib_banner, 1, 0);
    lib_banner_txt = text(lib_banner, "Loading your library", &ss_semibold_18, C_TEXT);
    lv_obj_align(lib_banner_txt, LV_ALIGN_TOP_MID, 0, 9);
    lv_obj_t *track = box(lib_banner);
    lv_obj_set_size(track, BANNER_TRACK, 5);
    lv_obj_align(track, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(track, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lib_banner_fill = box(track);
    lv_obj_set_size(lib_banner_fill, BANNER_TRACK * 3 / 10, 5);
    lv_obj_set_style_radius(lib_banner_fill, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(lib_banner_fill, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(lib_banner_fill, LV_OPA_COVER, 0);
    banner_sweep();
    banner_fade(lib_banner, 20, 0, LV_OPA_TRANSP, LV_OPA_COVER, SL_T_PAGE, NULL);
}
static void banner_update(void)
{
    if (!lib_banner) return;
    char b[64];
    if (sl_lib.scan_total > 0)
    {
        int pct = sl_lib.scan_done * 100 / sl_lib.scan_total;
        snprintf(b, sizeof b, "Loading your library · %d%%", pct);
        int w = BANNER_TRACK * (pct < 3 ? 3 : pct > 100 ? 100 : pct) / 100;
        if (lv_anim_get(lib_banner_fill, banner_set_x))
        {
            lv_anim_delete(lib_banner_fill, banner_set_x);
            lv_obj_set_x(lib_banner_fill, 0);
        }
        /* glide to the new width over the next update interval */
        lv_anim_delete(lib_banner_fill, banner_set_w);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, lib_banner_fill);
        lv_anim_set_values(&a, lv_obj_get_width(lib_banner_fill), w);
        lv_anim_set_duration(&a, SL_T_PAGE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, banner_set_w);
        lv_anim_start(&a);
    }
    else
        snprintf(b, sizeof b, "Loading your library · %d songs", sl_lib.scan_done);
    lv_label_set_text(lib_banner_txt, b);
}
static void banner_hide(void)
{
    if (!lib_banner) return;
    lv_obj_t *o = lib_banner;
    lv_anim_delete(lib_banner_fill, NULL);
    lib_banner = lib_banner_txt = lib_banner_fill = NULL;
    banner_fade(o, 0, 16, lv_obj_get_style_opa(o, 0), LV_OPA_TRANSP, SL_T_FAST, banner_gone);
}

/* ---------------------------------------------------------- hardware keys */
static lv_obj_t *hud, *hud_lvl, *hud_fill, *hud_txt;
static lv_timer_t *hud_timer;
static bool hud_dragging;
/* Long enough to reach the screen after pressing a side key */
#define VOL_HUD_SHOW_MS 2600
#define VOL_HUD_AFTER_TOUCH_MS 1500
#define VOL_HUD_W 72
#define VOL_HUD_H 260
#define VOL_HUD_RIM 2
#define VOL_HUD_PAD 28                      /* extra touch margin around it */
#define VOL_HUD_OFF (VOL_HUD_W + 24)       /* slid out past the right edge */
static void hud_touch(lv_event_t *e);
/* It glides in from the edge like the charging drop: dark glass with a
 * rim of light, settling with a little overshoot, and glides back out. */
static void hud_slide(void *o, int32_t v)
{
    int opa = 255 - (v > 0 ? v : 0) * 255 / VOL_HUD_OFF;
    lv_obj_set_style_translate_x(o, v, 0);
    lv_obj_set_style_opa(o, (lv_opa_t)(opa < 0 ? 0 : opa > 255 ? 255 : opa), 0);
}
static void hud_gone(lv_anim_t *a)
{
    if (!hud_dragging) lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}
static void hud_glide(bool in)
{
    lv_anim_t a;
    lv_anim_delete(hud, hud_slide);
    lv_anim_init(&a);
    lv_anim_set_var(&a, hud);
    lv_anim_set_values(&a, lv_obj_get_style_translate_x(hud, 0), in ? 0 : VOL_HUD_OFF);
    lv_anim_set_duration(&a, in ? SL_T_SLOW : SL_T_PAGE);
    lv_anim_set_path_cb(&a, in ? lv_anim_path_overshoot : lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, hud_slide);
    if (!in) lv_anim_set_completed_cb(&a, hud_gone);
    lv_anim_start(&a);
}
static void hud_hide(lv_timer_t *t)
{
    if (!hud_dragging) hud_glide(false);
    lv_timer_pause(t);
}
static bool hud_hit(int x, int y)
{
    if (!hud || lv_obj_has_flag(hud, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords(hud, &a);
    return x >= a.x1 - VOL_HUD_PAD && x <= a.x2 + VOL_HUD_PAD &&
           y >= a.y1 - VOL_HUD_PAD && y <= a.y2 + VOL_HUD_PAD;
}
/* The level is a plain window as tall as the volume over a pill the
 * size of the bar, so its bottom keeps the bar's round end at any level,
 * even below one radius, and nothing pokes past the curve. The colour
 * still runs light to shade over the filled part only. */
static void hud_level(void *o, int32_t v)
{
    lv_obj_set_height(o, v);
    int stop = VOL_HUD_H > 0 ? 255 * (VOL_HUD_H - v) / VOL_HUD_H : 0;
    lv_obj_set_style_bg_main_stop(hud_fill, (int32_t)(stop < 0 ? 0 : stop > 255 ? 255 : stop), 0);
}
/* smooth: a key press eases the level to its new height; a finger on the
 * bar moves it directly */
static void hud_update(bool smooth)
{
    int v = volume_pct(), to = VOL_HUD_H * v / 100;
    char b[8];
    snprintf(b, sizeof b, "%d", v);
    lv_label_set_text(hud_txt, b);
    /* the number sits at the top: dark once the fill reaches it */
    lv_obj_set_style_text_color(hud_txt, lv_color_hex(to > VOL_HUD_H - 44 && !sl_zune() ? 0x16191f : 0xffffff), 0);
    lv_anim_delete(hud_lvl, hud_level);
    if (!smooth) { hud_level(hud_lvl, to); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, hud_lvl);
    lv_anim_set_values(&a, lv_obj_get_height(hud_lvl), to);
    lv_anim_set_duration(&a, SL_T_FAST);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, hud_level);
    lv_anim_start(&a);
}
static void hud_show(void)
{
    if (!hud)
    {
        hud = box(lv_layer_top());
        lv_obj_set_size(hud, VOL_HUD_W, VOL_HUD_H);
        lv_obj_align(hud, LV_ALIGN_TOP_RIGHT, -12, 110);
        lv_obj_set_style_radius(hud, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(hud, lv_color_hex(0x262a31), 0);
        lv_obj_set_style_bg_grad_color(hud, lv_color_hex(0x050506), 0);
        lv_obj_set_style_bg_grad_dir(hud, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(hud, 235, 0);
        lv_obj_set_style_border_width(hud, VOL_HUD_RIM, 0);
        lv_obj_set_style_border_color(hud, lv_color_hex(0xdfe9e2), 0);
        lv_obj_set_style_border_opa(hud, 70, 0);
        lv_obj_set_style_border_post(hud, true, 0);           /* the rim over the fill */
        sl_elevate(hud, SL_E3, 0);
        lv_obj_add_flag(hud, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(hud, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_ext_click_area(hud, VOL_HUD_PAD);
        lv_obj_add_event_cb(hud, hud_touch, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(hud, hud_touch, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(hud, hud_touch, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(hud, hud_touch, LV_EVENT_PRESS_LOST, NULL);
        hud_lvl = box(hud);
        lv_obj_set_width(hud_lvl, VOL_HUD_W);
        /* children sit inside the rim: this puts the level on the outline */
        lv_obj_align(hud_lvl, LV_ALIGN_BOTTOM_MID, 0, VOL_HUD_RIM);
        hud_fill = box(hud_lvl);
        lv_obj_set_size(hud_fill, VOL_HUD_W, VOL_HUD_H);
        lv_obj_align(hud_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_radius(hud_fill, SL_R_FULL, 0);
        lv_obj_set_style_bg_opa(hud_fill, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_dir(hud_fill, LV_GRAD_DIR_VER, 0);
        /* light from the window, across the top of the glass */
        lv_obj_t *hi = box(hud);
        lv_obj_set_size(hi, VOL_HUD_W - 18, 34);
        lv_obj_align(hi, LV_ALIGN_TOP_MID, 0, 5);
        lv_obj_set_style_radius(hi, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(hi, lv_color_white(), 0);
        lv_obj_set_style_bg_main_opa(hi, 60, 0);
        lv_obj_set_style_bg_grad_color(hi, lv_color_white(), 0);
        lv_obj_set_style_bg_grad_opa(hi, 0, 0);
        lv_obj_set_style_bg_grad_dir(hi, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(hi, LV_OPA_COVER, 0);
        hud_txt = text(hud, "", &ss_semibold_18, 0xffffff);
        lv_obj_align(hud_txt, LV_ALIGN_TOP_MID, 0, 12);
        hud_timer = lv_timer_create(hud_hide, VOL_HUD_SHOW_MS, NULL);
        hud_slide(hud, VOL_HUD_OFF);
    }
    lv_obj_set_style_bg_color(hud_fill, sl_zune() ? lv_color_hex(ZN_PINK) : lv_color_white(), 0);
    lv_obj_set_style_bg_grad_color(hud_fill, sl_zune() ? lv_color_hex(ZN_PINK) : lv_color_hex(0xc9d3dc), 0);
    bool hidden = lv_obj_has_flag(hud, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_style_translate_x(hud, 0) > 0;
    hud_update(!hidden);                  /* arriving, it shows the level at once */
    lv_obj_remove_flag(hud, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(hud);          /* over the lock screen too */
    if (hidden || lv_anim_get(hud, hud_slide)) hud_glide(true);
    lv_timer_set_period(hud_timer, VOL_HUD_SHOW_MS);
    lv_timer_reset(hud_timer);
    if (!hud_dragging) lv_timer_resume(hud_timer);
}
static void hud_set_pct(int pct)
{
    int volume = volume_from_pct(pct);
    if (volume != global_status.volume)
    {
        global_status.volume = volume;
        setvol();
        statusbar_update();
    }
    hud_update(false);
}
/* Slide anywhere on (or near) the bar and the level follows the finger
 * from where it was, rather than jumping to it; a tap that does not move
 * sets the level where it lands. */
static void hud_touch(lv_event_t *e)
{
    static int y0, pct0;
    static bool moved;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    lv_area_t area;
    lv_obj_get_coords(hud, &area);
    int h = area.y2 - area.y1;
    if (code == LV_EVENT_PRESSED)
    {
        hud_dragging = true;
        lv_timer_pause(hud_timer);
        y0 = p.y; pct0 = volume_pct(); moved = false;
        return;
    }
    if (code == LV_EVENT_PRESSING)
    {
        int dy = y0 - p.y;
        if (!moved && (dy > 6 || dy < -6)) { moved = true; y0 = p.y; pct0 = volume_pct(); dy = 0; }
        if (moved) hud_set_pct(pct0 + dy * 100 / h);
        return;
    }
    if (code == LV_EVENT_RELEASED && !moved && in)
        hud_set_pct((area.y2 - p.y) * 100 / h);
    hud_dragging = false;
    lv_timer_set_period(hud_timer, VOL_HUD_AFTER_TOUCH_MS);
    lv_timer_reset(hud_timer);
    lv_timer_resume(hud_timer);
}

/* A press moves the volume by Settings > Sound > Volume button step. */
static void volume_key(int dir)
{
    adjust_volume(dir * sl_vol_step());
    if (lp_volume_shown()) lp_refresh();   /* its own bar moves at once */
    else hud_show();
    statusbar_update();
    /* With music playing, the music getting louder is the answer.
     * Otherwise a tick that climbs with the level says where it is. */
    if (!is_playing())
        sfx_play_ex(SFX_UI_TICK, 80 + volume_pct() * 40 / 100, 100);
}

/* Previous, as on any player: back to the start of the song first, then
 * to the song before when pressed again near the start. */
/* Next and previous step through a cue sheet's tracks inside their file */
static void cue_seek(long ms)
{
    if (!(audio_status() & AUDIO_STATUS_PAUSE)) audio_pre_ff_rewind();
    audio_ff_rewind(ms);
}
static void sl_next(void)
{
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    int t = cue_now(id3, NULL, NULL);
    if (t >= 0 && t + 1 < sl_lib.n_tracks && sl_lib.tracks[t + 1].cue == sl_lib.tracks[t].cue + 1
        && sl_track_path_is(t + 1, id3->path))
    { cue_seek(sl_lib.tracks[t + 1].start); return; }
    audio_next();
}
/* the song before, never a restart (a swipe back): in a cue sheet, its track before */
static void sl_prev_song(void)
{
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    int t = cue_now(id3, NULL, NULL);
    if (t > 0 && sl_lib.tracks[t].cue > 1 && sl_track_path_is(t - 1, id3->path)) { cue_seek(sl_lib.tracks[t - 1].start); return; }
    audio_prev();
}
static void sl_prev(void)
{
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    long cs;
    int t = cue_now(id3, &cs, NULL);
    if (t >= 0)
    {
        if ((long)id3->elapsed - cs > 3000) { cue_seek(cs); return; }   /* back to its start */
        if (sl_lib.tracks[t].cue > 1 && t > 0 && sl_track_path_is(t - 1, id3->path))
        { cue_seek(sl_lib.tracks[t - 1].start); return; }
    }
    if (id3 && id3->elapsed > 3000)
    {
        if (!(audio_status() & AUDIO_STATUS_PAUSE))
            audio_pre_ff_rewind();
        audio_ff_rewind(0);
    }
    else
        audio_prev();
}

/* What a key can do (Settings > Buttons). A book playing, next and
 * previous are 30 seconds ahead and back rather than a chapter (#108). */
enum { KA_NONE, KA_PLAY, KA_NEXT, KA_PREV, KA_NP, KA_BACK, KA_AHEAD };
static bool sl_book_now(void)
{
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    return id3 && (audiobooks_is_book(id3->path) || sl_pod_is_episode(id3->path));   /* a podcast episode too */
}
static void np_book_skip(int side);
static void key_do(int act)
{
    bool book = (act == KA_NEXT || act == KA_PREV) && sl_book_now();
    switch (act)
    {
        case KA_PLAY: toggle_play(); break;
        case KA_NEXT: if (book) np_book_skip(1); else sl_next(); break;
        case KA_PREV: if (book) np_book_skip(-1); else sl_prev(); break;
        case KA_NP: if (np_open || !(audio_status() & AUDIO_STATUS_PLAY)) toggle_play(); else np_show(); break;
    }
}
/* Held, a seek key scrubs through the song in steps you can hear: small
 * at first, longer the longer it is held. It stops short of the end
 * rather than running into the next song. */
#define KEY_HOLD_MS     600
#define KEY_SEEK_EVERY  300
#define KEY_DOUBLE_MS   250
static void key_seek(int dir, uint32_t held_ms)
{
    static uint32_t last;
    uint32_t now = sl_ms();
    if (held_ms && now - last < KEY_SEEK_EVERY) return;
    last = now;
    struct mp3entry *id3 = (audio_status() & AUDIO_STATUS_PLAY) ? audio_current_track() : NULL;
    if (!id3 || !id3->length) return;
    long step = held_ms < 3000 ? 5000 : held_ms < 8000 ? 15000 : 30000;
    long to = (long)id3->elapsed + dir * step;
    if (to < 0) to = 0;
    if (to > (long)id3->length - 2000) to = (long)id3->length - 2000;
    if (to < 0 || to == (long)id3->elapsed) return;
    if (!(audio_status() & AUDIO_STATUS_PAUSE)) audio_pre_ff_rewind();
    audio_ff_rewind(to);
}
/* A key: its press, its hold (after KEY_HOLD_MS) and, when one is set, a
 * second press within KEY_DOUBLE_MS. Only a key with a double-press
 * action waits to see whether another press follows; the rest answer on
 * release as before. The release after a hold does nothing more. */
struct sl_key { uint32_t down_at, up_at; bool down, held, second; int pending; lv_timer_t *wait; };
static struct sl_key key_play, key_next;
static void key_wait_cb(lv_timer_t *t)
{
    struct sl_key *k = lv_timer_get_user_data(t);
    k->wait = NULL;
    int act = k->pending;
    k->pending = KA_NONE;
    key_do(act);
}
static void key_event(struct sl_key *k, long btn, int press, int hold, int twice)
{
    bool rel = btn & BUTTON_REL, repeat = btn & BUTTON_REPEAT;
    if (!rel && !repeat)
    {
        k->down = true; k->held = false; k->down_at = sl_ms();
        k->second = k->wait != NULL;      /* the second press of a double */
        if (k->second)
        {
            lv_timer_delete(k->wait);
            k->wait = NULL;
            k->pending = KA_NONE;
            key_do(twice);
        }
        return;
    }
    if (!rel)
    {
        if (!k->down || k->second || hold == KA_NONE) return;
        uint32_t held = sl_ms() - k->down_at;
        if (held < KEY_HOLD_MS) return;
        if (hold == KA_BACK || hold == KA_AHEAD)
        {
            key_seek(hold == KA_AHEAD ? 1 : -1, k->held ? held : 0);
            k->held = true;
        }
        else if (!k->held) { k->held = true; key_do(hold); }
        return;
    }
    bool held = k->held, second = k->second;
    k->down = k->held = k->second = false;
    if (held || second) return;
    if (twice == KA_NONE) { key_do(press); return; }
    k->pending = press;
    k->wait = lv_timer_create(key_wait_cb, KEY_DOUBLE_MS, k);
    lv_timer_set_repeat_count(k->wait, 1);
}
static const int key_play_press[] = { KA_PLAY, KA_NEXT, KA_PREV, KA_NP };
static const int key_play_twice[] = { KA_NONE, KA_NEXT, KA_PREV, KA_NP };
static const int key_next_hold[] = { KA_NONE, KA_PREV, KA_AHEAD };
static const int key_next_twice[] = { KA_NONE, KA_PREV, KA_PLAY };
static int key_pick(const int *acts, int n, int i) { return acts[i >= 0 && i < n ? i : 0]; }
/* the player's own play key, as set in Settings > Buttons */
static void play_key(long btn)
{
    key_event(&key_play, btn, key_pick(key_play_press, 4, global_settings.sloppy_play_key),
              global_settings.sloppy_play_hold == 1 ? KA_BACK : KA_NONE,
              key_pick(key_play_twice, 4, global_settings.sloppy_play_double));
}
/* The R1 has a next key and no previous key. A tap skips ahead (in a
 * book, 30 seconds ahead or back, as chosen); holding it goes back a
 * song or scrubs ahead; pressing it twice does what was chosen. */
static void next_key(long btn)
{
    int press = sl_book_now() && global_settings.sloppy_book_next == 1 ? KA_PREV : KA_NEXT;
    key_event(&key_next, btn, press, key_pick(key_next_hold, 3, global_settings.sloppy_next_hold),
              key_pick(key_next_twice, 3, global_settings.sloppy_next_double));
}
static void bye_cancel(void);
static volatile uint32_t sl_beat_ms;   /* the watchdog's heartbeat; 0 holds it */
/* Where the interface was when USB mode came, for the USB trace: on
 * 2026-10-23 USB mode reached the Mac from the home screen only. The top
 * page by its title (its first label that is not the back arrow). */
static const char *sl_label_in(lv_obj_t *o, int depth)
{
    if (lv_obj_check_type(o, &lv_label_class) && strcmp(lv_label_get_text(o), LV_SYMBOL_LEFT))
        return lv_label_get_text(o);
    if (depth <= 0) return NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
    {
        const char *t = sl_label_in(lv_obj_get_child(o, i), depth - 1);
        if (t) return t;
    }
    return NULL;
}
static void sl_usb_where(char *b, size_t n)
{
    const char *top = npages > 1 ? sl_label_in(pages[npages - 1], 4) : NULL;
    snprintf(b, n, "%s%s%s, %d page%s deep%s%s, screen %s",
             npages > 1 ? "page \"" : "home", npages > 1 ? (top ? top : "?") : "", npages > 1 ? "\"" : "",
             npages, npages == 1 ? "" : "s", np_open ? ", Now Playing open" : "", cf_is_open() ? ", Cover Flow open" : "",
             sl_power.mode == SL_POWER_ACTIVE ? "on" : sl_power.mode == SL_POWER_DIM ? "dim" :
             sl_power.mode == SL_POWER_LOCKED ? "locked" : "dark");
}
#if defined(HIBY_R1) && !defined(SIMULATOR)
#define SL_USB_TRACE(...) usb_trace(__VA_ARGS__)
#else
static char sl_usb_traced[1024];         /* the test reads them */
#define SL_USB_TRACE(...) do { size_t l_ = strlen(sl_usb_traced); \
    if (l_ < sizeof sl_usb_traced - 1) snprintf(sl_usb_traced + l_, sizeof sl_usb_traced - l_, __VA_ARGS__); } while (0)
#endif
static void handle_button(long btn)
{
    if (btn & SYS_EVENT)
    {
        /* the tick repeats a Bluetooth event until it is acted on */
        static long last_event;
        if (btn != last_event) sl_log("system event %lx", btn);
        last_event = btn;
#ifdef HAVE_HIBY_BLUETOOTH
        if (btn == SYS_BT_PLUGGED) { sl_bt_auto_request(); return; }
#endif
        /* Headphones in or out, a battery reading: Rockbox handles these
         * without drawing (unplugging pauses). Handing it the display
         * lit a dark player in a pocket to its lock screen every time. */
        if (((unsigned long)btn >> SYS_EVENT_CLS_SHIFT & 7) == SYS_EVENT_CLS_PLUG ||
            btn == SYS_BATTERY_UPDATE)
        {
            bool was_playing = is_playing();
            default_event_handler(btn);
            if (btn == SYS_PHONE_UNPLUGGED && was_playing && !is_playing())
                notify_ex(NS_MUSIC, NOTE_INFO, 0, NA_NONE, "headphones", "Paused. Headphones unplugged.");
#if defined(HAVE_HIBY_BLUETOOTH) && !defined(SIMULATOR)
            if (btn == SYS_BT_UNPLUGGED && !bt_is_routed_fast())
            {
                if (was_playing && !is_playing())
                    notify_ex(NS_BT, NOTE_INFO, 0, NA_BT, "bluetooth", "Paused. Bluetooth headphones disconnected.");
                sl_bt_known = false;          /* the page asks the stack again */
                if (sl_bt_page_obj && !sl_bt_busy) sl_bt_begin(SL_BT_JOB_REFRESH, "");
                sl_bt_on_lost();              /* out of range or flat: looked for again, backing off */
            }
#endif
            return;
        }
        /* USB screen, shutdown prompt etc draw with Rockbox's own UI; take
         * the screen back afterwards */
        radio_flush();
        hist_flush();
    notes_flush();
        clicker_flush();
        rd_flush();                     /* where a book was left */
        /* Hold the watchdog: USB mode hands the card to the computer,
         * and a log line written now would land on a disk the computer
         * has mounted (it left the card dirty, so the Mac refused it).
         * The loop's next pass starts the heartbeat again. */
        sl_beat_ms = 0;
        sl_power_suspend();
        /* The card belongs to the computer during USB mode, so nothing is
         * logged while it lasts: note the battery either side instead, to
         * tell whether the computer charged it. */
        bool usb = btn == SYS_USB_CONNECTED;
        uint32_t usb_from = sl_ms();
        if (usb)
        {
            char where[160];
            sl_usb_where(where, sizeof where);
            SL_USB_TRACE("ui: USB mode asked on %s", where);
            sl_log("usb: USB mode on %s, battery %d%% %d mV", where, battery_level(), battery_voltage());
            /* A library read keeps the tag database open between passes
             * (database_idx.tcd and a tag file): on 2026-10-23 that kept
             * the card from the Mac. It lets go and starts over after. */
            lib_usb_stopped = lib_state == SL_LIB_LOADING;
            if (lib_usb_stopped)
            {
                sl_lib_free();
                sl_log("library: read stopped for USB mode");
            }
            /* Everything of ours that holds a file on the card lets go
             * before Rockbox unmounts it for the computer (an open file
             * keeps it mounted, and then two drivers write one exFAT):
             * the log, the thumbnail pruner's folders, the scrobbler and
             * a DLNA download. Rockbox closes its own (fonts, the playlist
             * control file, the playing track) in default_event_handler. */
            sl_shot_wait();               /* a screenshot being written finishes first */
            sl_log_hold(true);
            { FILE *note = fopen(DB_USB_NOTE, "w"); if (note) fclose(note); }   /* the library updates once it is back (fopen: Rockbox's open() would put it on the card) */
            sl_art_release();
            fm_usb_release();             /* a copy in Files stops and closes its files */
            sl_net_scrobble_hold(true);
            sl_share_hold(true);          /* a transfer coming in stops, its half file removed */
            sl_libjob_hold(true);         /* lyrics and covers wait; nothing half-written */
            sl_pod_hold(true);            /* a podcast download stops, its part kept; where an episode was is saved first */
            sl_epub_hold(true);           /* a book being read or the shelf being looked over stops */
            sl_sub_hold(true);            /* a server's song coming down stops, its part kept */
            sl_abs_hold(true);            /* and an Audiobookshelf book's */
            sl_ota_hold(true);            /* an update being checked or sent stops; nothing half-staged stays */
            SL_USB_TRACE("ui: our files let go after %u ms: answering, the USB screen is up",
                         (unsigned)(sl_ms() - usb_from));
        }
        default_event_handler(btn);
        if (usb)
        {
            sl_net_scrobble_hold(false);
            sl_share_hold(false);
            sl_libjob_hold(false);
            sl_pod_hold(false);
            sl_epub_hold(false);
            sl_sub_hold(false);
            sl_abs_hold(false);
            sl_ota_hold(false);
            SL_USB_TRACE("ui: USB screen closed after %u s", (unsigned)((sl_ms() - usb_from) / 1000));
            sl_log_hold(false);
            sl_log("usb: back after %u s, battery %d%% %d mV", (unsigned)((sl_ms() - usb_from) / 1000),
                   battery_level(), battery_voltage());
#if defined(HIBY_R1) && !defined(SIMULATOR)
            int handover = usb_card_handover_error();
            if (handover > 0)
                sl_log("usb: the card was NOT handed over: umount failed, errno %d (%s); still open: %s", handover,
                       strerror(handover), usb_card_busy_files()[0] ? usb_card_busy_files() : "nothing of ours");
            else if (handover == 0)
                sl_log("usb: card handed over unmounted, and mounted again");
#endif
            /* still running (USB mode usually restarts us): the library now */
            if (lib_usb_stopped)
            {
                lib_epoch++;
                lib_state = lib_begin();
                sl_log("library: read again after USB mode");
                sl_art_reset();
                if (lib_state != SL_LIB_LOADING) banner_hide();
            }
            /* a read still going: the update waits for it (db_auto_poll) */
            if (lib_state != SL_LIB_LOADING && !unlink(DB_USB_NOTE)) { db_check_quiet = true; db_update_begin(false); }
        }
        bye_cancel();                   /* a power off that did not happen */
        sl_power_resume();
        touchscreen_set_mode(TOUCHSCREEN_POINT);
        lcd_set_viewport(NULL);
        lv_obj_invalidate(lv_screen_active());
        return;
    }
    if (btn & BUTTON_TOUCHSCREEN)
        return;
    int b = btn & ~(BUTTON_REL | BUTTON_REPEAT);
    /* headset and Bluetooth remote keys, when the user turned them off */
    if (!global_settings.sloppy_remote_keys && (b == BUTTON_PLAY || b == BUTTON_NEXT || b == BUTTON_PREV))
        return;
    if (clicker_key(btn)) return;           /* the volume keys count in Clicker, even locked */
    if (sl_power_filter_button(btn)) return;
    bool rel = btn & BUTTON_REL;

    if (spoon_on_top() && spoon_key(b, rel)) return;
    if (arcade_key(b, rel)) return;           /* a game: Play is A, Next is B */
    if (t48_key(b, rel) || fl_key(btn)) return;   /* 2048 slides by the keys; Flappy Bird flaps on Play and Next */
    if (sl_power.mode < SL_POWER_LOCKED && (fm_key(b, rel, btn & BUTTON_REPEAT) || rd_key(b, rel, btn & BUTTON_REPEAT))) return;   /* a picture, a text or a book open: the side keys turn it (not over the lock screen) */
    /* Simulator arrow keys map to these hardware buttons. Keep steering
     * local to Snake instead of changing the player's volume or track. */
    if (snake_page_obj && npages && pages[npages - 1] == snake_page_obj &&
        (b == BUTTON_UP || b == BUTTON_DOWN || b == BUTTON_LEFT || b == BUTTON_RIGHT))
    {
        if (!rel) snake_key(b);
        return;
    }

    if ((b == BUTTON_UP || b == BUTTON_DOWN) && !rel)
    {
        /* Held, a bigger step repeats more slowly, so holding climbs no
         * faster than it does one step at a time (a repeat every 50 ms at
         * full speed) and does not race to the top. A fresh press always
         * counts, and one step repeats as it always did. */
        static long next_repeat;
        int step = sl_vol_step();
        if (step > 1 && (btn & BUTTON_REPEAT) && TIME_BEFORE(current_tick, next_repeat)) return;
        next_repeat = current_tick + step * (HZ / 20);
        volume_key(b == BUTTON_UP ? 1 : -1);
    }
    else if (b == BUTTON_LEFT) play_key(btn);
    else if (b == BUTTON_PLAY && rel) toggle_play();
    else if (b == BUTTON_RIGHT) next_key(btn);
    else if (b == BUTTON_NEXT && rel) key_do(KA_NEXT);   /* headset and car: in a book, 30 seconds */
    else if (b == BUTTON_PREV && rel) key_do(KA_PREV);

}

#include "sloppy_playback.inc"
#include "sloppy_poweroff.inc"
#include "sloppy_settings.inc"
#include "sloppy_batlog.inc"
#include "sloppy_erase.inc"
#include "sloppy_saver.inc"
#include "sloppy_car.inc"
static void rm_set(int on);
static void rm_code_row(lv_obj_t *card, const char *url);
#include "sloppy_wifi.inc"
#include "sloppy_receive.inc"
#include "sloppy_scrobble.inc"
#include "sloppy_libjob.inc"
#include "sloppy_swipe.inc"
#include "sloppy_quiz.inc"
#include "sloppy_coverflow.inc"
#include "sloppy_history.inc"
#include "sloppy_spotify_match.h"
#include "sloppy_songradio.inc"
#include "sloppy_spotify.inc"
#include "sloppy_taste.inc"
#include "sloppy_grave.inc"
#include "sloppy_mixtape.inc"
#include "sloppy_search.inc"
#include "sloppy_update.inc"
#include "sloppy_remote.inc"
#include "sloppy_stations.inc"
#include "sloppy_podcast.inc"
#include "sloppy_subsonic.inc"
#include "sloppy_abs.inc"
#include "sloppy_reader.inc"

/* Back faces of the game tiles on the tiles home: a best score, a coin
 * tally, an answer from the ball. */
static bool mt_game_back(struct mt_slot *s)
{
    lv_obj_t *b = s->back;
    int w = mt_px(mt_span_w(s->size));
    char big[32], line[64];
    const char *kicker;
    lv_obj_clean(b);
    if (s->id == TL_COIN) coin_load();   /* the saved tally, before the page has been opened */
    if (s->size == SZ_SMALL)
    {
        /* one short thing, centred: an answer, the last flip, the best */
        const lv_font_t *font = &ss_semibold_23;
        line[0] = 0;
        if (s->id == TL_MAGIC)
        {
            int n = (int)(sizeof magic_answers / sizeof magic_answers[0]) - 1;
            strlcpy(big, magic_answers[mt_rand() % (unsigned)n], sizeof big);
            font = &ss_semibold_18;
        }
        else if (s->id == TL_COIN)
        {
            strlcpy(big, !coin_flips ? "flip?" : coin_heads ? "heads" : "tails", sizeof big);
            if (coin_flips)
                snprintf(line, sizeof line, "%lu - %lu", (unsigned long)coin_head_count,
                         (unsigned long)(coin_flips - coin_head_count));
        }
        else
        {
            quiz_stats_load();
            if (quiz_stats.games) quiz_money(big, sizeof big, quiz_stats.best_cash);
            else strlcpy(big, "play!", sizeof big);
            strlcpy(line, quiz_stats.games ? "best" : "name it", sizeof line);
        }
        lv_obj_t *l = mt_label(b, big, font, LV_OPA_COVER);
        lv_obj_set_width(l, w - 12);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, line[0] ? -10 : 0);
        if (line[0])
        {
            lv_obj_t *sub = mt_label(b, line, &ss_regular_16, 215);
            lv_obj_align(sub, LV_ALIGN_CENTER, 0, 22);
        }
        return true;
    }
    if (s->id == TL_MAGIC)
    {
        /* any answer but the rare one: that stays a surprise */
        int n = (int)(sizeof magic_answers / sizeof magic_answers[0]) - 1;
        lv_obj_set_pos(mt_label(b, "the ball says", &ss_regular_18, 215), 14, 14);
        lv_obj_t *l = mt_label(b, magic_answers[mt_rand() % (unsigned)n], &ss_regular_34, LV_OPA_COVER);
        lv_obj_set_pos(l, 12, 40);
        lv_obj_set_width(l, w - 24);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        label_lines(l, 3);
        mt_title(b, mt_defs[s->id].name);
        return true;
    }
    if (s->id == TL_COIN)
    {
        kicker = coin_flips ? "last flip" : "call it";
        strlcpy(big, !coin_flips ? "heads or tails?" : coin_heads ? "heads" : "tails", sizeof big);
        if (coin_flips)
            snprintf(line, sizeof line, "%lu heads  ·  %lu tails", (unsigned long)coin_head_count,
                     (unsigned long)(coin_flips - coin_head_count));
        else line[0] = 0;
    }
    else
    {
        quiz_stats_load();
        kicker = quiz_stats.games ? "best winnings" : "name that song";
        if (quiz_stats.games) quiz_money(big, sizeof big, quiz_stats.best_cash);
        else strlcpy(big, "5 rounds", sizeof big);
        if (quiz_stats.games)
            snprintf(line, sizeof line, "%d game%s  ·  best streak %d", quiz_stats.games,
                     quiz_stats.games == 1 ? "" : "s", quiz_stats.best_streak);
        else strlcpy(line, "3 seconds a clip", sizeof line);
    }
    lv_obj_set_pos(mt_label(b, kicker, &ss_regular_18, 215), 14, 14);
    lv_obj_t *l = mt_label(b, big, &ss_regular_34, LV_OPA_COVER);
    lv_obj_set_pos(l, 12, 40);
    lv_obj_set_width(l, w - 24);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    label_lines(l, 2);
    l = mt_label(b, line, &ss_regular_18, 215);
    lv_obj_set_pos(l, 14, 40 + 2 * lv_font_get_line_height(&ss_regular_34) + 2);
    lv_obj_set_width(l, w - 28);
    label_lines(l, 1);
    l = mt_title(b, mt_defs[s->id].name);
    lv_obj_set_width(l, w - 20);
    label_lines(l, 1);
    return true;
}

/* ---------------------------------------------------------------- boot */
static void lv_core_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    lv_init();
    lv_tick_set_cb(sl_ms);
    disp = lv_display_create(W, H);
#if defined(HIBY_R1) && !defined(SIMULATOR)
    {   /* the framebuffer is one upright 480 x 800 RGB565 panel, 4-aligned */
        const uint16_t *fb = SL_SCREEN_PIXELS();
        sl_direct_ok = LCD_FBWIDTH == W && fb && ((uintptr_t)fb & 3) == 0 &&
                       !file_exists(ROCKBOX_DIR "/sloppy/partial-flush");
        sl_log("display: %s", sl_direct_ok ? "drawing in place" : "drawing in strips");
    }
#endif
    sl_display_buffers(true);   /* static buffers: a failed allocation can't hand LVGL NULL */
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_add_event_cb(disp, cfm_refr_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, cfm_refr_cb, LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(disp, cfm_refr_cb, LV_EVENT_RENDER_READY, NULL);
    dirty_x1 = dirty_y1 = 0x7fff; dirty_x2 = dirty_y2 = -1;
    touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);
    lv_indev_set_scroll_throw(touch, 6);   /* long, iOS-like glide */
    styles_init();
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(lv_screen_active(), LV_SCROLLBAR_MODE_OFF);
}

/* The splash is drawn the moment the LCD exists, long before the rest of
 * Rockbox is up, and fades into the home screen. Boot stages are timed so
 * the log shows where startup time goes. */
static lv_obj_t *splash;
static struct { const char *name; uint32_t ms; } marks[16];
static int n_marks;
static uint32_t boot_ms0;

void sloppy_boot_mark(const char *stage)
{
    if (n_marks < (int)(sizeof marks / sizeof marks[0]))
    {
        marks[n_marks].name = stage;
        marks[n_marks].ms = sl_ms();
        n_marks++;
    }
}

/* The wordmark: Shrikhand, cream with "OS" in orange, on a hard red drop
 * shadow like the lettering on the boot logo, a shadow and a face for each
 * letter. */
#define BRAND_LETTERS 10
static lv_obj_t *brand_mark(lv_obj_t *parent, const lv_font_t *f, int drop)
{
    static const char *const glyph[BRAND_LETTERS] = { "G", "a", "b", "a", "g", "o", "o", "l", "O", "S" };
    lv_obj_t *row = box(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_left(row, drop * 2, 0);         /* balances the breath before OS, so it stays centred */
    lv_obj_set_style_pad_right(row, drop * 3, 0);        /* room for the shadow and that breath */
    lv_obj_set_style_pad_bottom(row, drop, 0);
    for (int i = 0; i < BRAND_LETTERS; i++)
    {
        lv_obj_t *l = box(row);
        lv_obj_set_size(l, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_add_flag(l, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        if (i == 8) lv_obj_set_style_margin_left(l, drop * 2, 0);   /* a breath before OS */
        for (int k = 0; k < 2; k++)
        {
            lv_obj_t *t = lv_label_create(l);
            lv_label_set_text_static(t, glyph[i]);
            lv_obj_set_style_text_font(t, f, 0);
            lv_obj_set_style_text_color(t, lv_color_hex(k == 0 ? 0xb3261e : i < 8 ? 0xfff1dc : 0xff6a1f), 0);
            if (k == 0)
            {
                lv_obj_add_flag(t, LV_OBJ_FLAG_IGNORE_LAYOUT);
                lv_obj_set_pos(t, drop, drop);
            }
        }
    }
    return row;
}

/* The splash is drawn once, on Rockbox's own thread, and stays still while
 * Rockbox starts. An entrance animated from a thread of its own (16 Oct)
 * raced Rockbox's startup over the framebuffer's two pages: the R1 drew
 * into the hidden page for the rest of the session, a black screen. */
static lv_obj_t *splash_word;
static void splash_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }

/* Your boot logo (P63): the first picture, by name, in the Bootlogo folder
 * at the card's top takes the wordmark's place on the splash. It is fitted
 * to the screen once, after a boot that finds it new or changed (never
 * enlarged, all of it), and kept as RGB565 with its source's name, size
 * and time beside it, so the splash only reads it: nothing is decoded while
 * the player starts. An empty folder brings the wordmark back. The boot
 * menu before GabagoolOS is the bootloader's, and keeps its own picture. */
static const char *bootlogo_root = "";   /* the card's top ("" on the player; the harness has its own) */
#define BOOTLOGO_CACHE ROCKBOX_DIR "/sloppy/bootlogo.rgb565"
#define BOOTLOGO_STAMP ROCKBOX_DIR "/sloppy/bootlogo.src"
static uint16_t *bootlogo_px;
static lv_image_dsc_t bootlogo_dsc;
/* the picture to show and what it is ("path size mtime"): false when none */
static bool bootlogo_source(char *path, size_t n, char *stamp, size_t sn)
{
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s/Bootlogo", bootlogo_root);
    DIR *d = opendir(dir);
    if (!d) return false;
    char best[MAX_PATH] = "";
    struct dirinfo bi = { 0 };
    struct dirent *e;
    while ((e = readdir(d)))
    {
        const char *x = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !x) continue;   /* hidden, and a Mac's ._ shadows */
        if (strcasecmp(x, ".jpg") && strcasecmp(x, ".jpeg") && strcasecmp(x, ".png") && strcasecmp(x, ".bmp")) continue;
        struct dirinfo info = dir_get_info(d, e);
        if ((info.attribute & ATTR_DIRECTORY) || info.size <= 0) continue;
        if (!best[0] || strcasecmp(e->d_name, best) < 0) { strlcpy(best, e->d_name, sizeof best); bi = info; }
    }
    closedir(d);
    if (!best[0]) return false;
    if (snprintf(path, n, "%s/%s", dir, best) >= (int)n) return false;   /* too long a name to open */
    return snprintf(stamp, sn, "%s %lld %lld", path, (long long)bi.size, (long long)bi.mtime) < (int)sn;
}
static bool bootlogo_stamp_is(const char *stamp)
{
    char b[MAX_PATH + 64];
    int fd = open(BOOTLOGO_STAMP, O_RDONLY);
    if (fd < 0) return false;
    ssize_t r = read(fd, b, sizeof b - 1);
    close(fd);
    if (r <= 0) return false;
    b[r] = 0;
    return !strcmp(b, stamp);
}
/* the fitted picture, when it is the one in the folder now */
static bool bootlogo_load(void)
{
    char path[MAX_PATH], stamp[MAX_PATH + 64];
    if (!bootlogo_source(path, sizeof path, stamp, sizeof stamp) || !bootlogo_stamp_is(stamp)) return false;
    int fd = open(BOOTLOGO_CACHE, O_RDONLY);
    unsigned char hd[8];
    if (fd < 0) return false;
    int w = 0, h = 0;
    if (read(fd, hd, 8) == 8 && !memcmp(hd, "SLBL", 4)) { w = hd[4] | hd[5] << 8; h = hd[6] | hd[7] << 8; }
    size_t size = (size_t)w * h * 2, got = 0;
    uint16_t *px = w > 0 && h > 0 && w <= W && h <= H ? malloc(size) : NULL;
    while (px && got < size)
    {
        ssize_t r = read(fd, (char *)px + got, size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!px || got != size) { free(px); return false; }
    bootlogo_px = px;
    bootlogo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    bootlogo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    bootlogo_dsc.header.w = w;
    bootlogo_dsc.header.h = h;
    bootlogo_dsc.header.stride = w * 2;
    bootlogo_dsc.data = (const uint8_t *)px;
    bootlogo_dsc.data_size = size;
    return true;
}
static void bootlogo_free(void)
{
    free(bootlogo_px);
    bootlogo_px = NULL;
    memset(&bootlogo_dsc, 0, sizeof bootlogo_dsc);
}
/* after the boot: a new picture fitted for the next one, a removed one forgotten */
static int bootlogo_job;                 /* 0 to check, 1 fitting */
static char bootlogo_making[MAX_PATH + 64];
static void bootlogo_tick(lv_timer_t *t)
{
    if (bootlogo_job == 1)
    {
        int r = sl_wall_poll();
        if (!r) return;
        if (r > 0)
        {
            int fd = open(BOOTLOGO_STAMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0) { if (write(fd, bootlogo_making, strlen(bootlogo_making)) < 0) {} close(fd); }
        }
        sl_log("boot logo: %s %s", bootlogo_making, r > 0 ? "fitted for the next start" : "could not be read");
        bootlogo_job = 0;
        lv_timer_delete(t);
        return;
    }
    char path[MAX_PATH], stamp[MAX_PATH + 64];
    if (!bootlogo_source(path, sizeof path, stamp, sizeof stamp))
    {
        int had = open(BOOTLOGO_STAMP, O_RDONLY);
        if (had >= 0)
        {
            close(had);
            remove(BOOTLOGO_CACHE);
            remove(BOOTLOGO_STAMP);
            sl_log("boot logo: the folder is empty, the wordmark is back");
        }
        lv_timer_delete(t);
        return;
    }
    if (bootlogo_stamp_is(stamp)) { lv_timer_delete(t); return; }
    remove(BOOTLOGO_STAMP);              /* the old picture is not this one */
    mkdir(ROCKBOX_DIR "/sloppy");
    if (sl_bootlogo_start(path, BOOTLOGO_CACHE, W, H) != SL_PIC_STARTED) return;   /* the worker is busy: again next tick */
    strlcpy(bootlogo_making, stamp, sizeof bootlogo_making);
    bootlogo_job = 1;
}

/* The logo poster: the wordmark and the pool photo on deep ink, a pool-blue
 * halftone rising under them (splash/make_splash.py). Already RGB565 at
 * the screen's size, so it is copied out, never decoded or scaled. */
#include "splash/splash.inc"
static const lv_image_dsc_t splash_art_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = W, .h = H, .stride = W * 2 },
    .data = (const uint8_t *)splash_art,
    .data_size = sizeof splash_art,
};
static void splash_build(void)
{
    splash = box(lv_layer_top());
    lv_obj_set_size(splash, W, H);
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x10161d), 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    /* your picture, in the middle; or the poster */
    bool own = bootlogo_load();
    splash_word = lv_image_create(splash);
    lv_image_set_src(splash_word, own ? &bootlogo_dsc : &splash_art_dsc);
    lv_obj_center(splash_word);
}

void sloppy_boot_splash(void)
{
    lcd_set_viewport(NULL);
    lv_core_init();
    if (!splash)
    {
        boot_ms0 = sl_ms();
        splash_build();
    }
    lv_refr_now(disp);
}

static void splash_gone(lv_anim_t *a) { lv_obj_delete(a->var); bootlogo_free(); }
static void splash_lift(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
/* Off it goes as the home screen rushes in: the word lifts away while the
 * backdrop dissolves. */
static void splash_fade_out(void)
{
    if (!splash) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, splash_word);
    /* a picture smaller than the screen lifts away; the poster only fades */
    lv_anim_set_values(&a, 0, lv_image_get_src(splash_word) == &splash_art_dsc ? 0 : -26);
    lv_anim_set_duration(&a, SL_T_SLOW);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, splash_lift);
    lv_anim_start(&a);
    lv_anim_init(&a);
    lv_anim_set_var(&a, splash);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, SL_T_SLOW);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, splash_opa);
    lv_anim_set_completed_cb(&a, splash_gone);
    lv_anim_start(&a);
    splash = splash_word = NULL;
}

/* The goodbye screen: the splash's logo on black, an accent line drawing in
 * under it while the shutdown chime plays. Rockbox's clean_shutdown() shows
 * it in place of its "Shutting down" text, and leaves it up while it saves. */
static lv_obj_t *bye, *bye_logo, *bye_line, *bye_note;
static void bye_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void bye_rise(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void bye_grow(void *o, int32_t v) { lv_obj_set_width(o, v); }
static void bye_anim(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, int ms, int delay)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_start(&a);
}
/* why: 0 a plain power off, 1 battery low, 2 battery empty */
static void bye_show(int why, bool animate)
{
    static const char *const notes[] = { "See you soon", "Battery low", "Battery empty" };
    if (!bye)
    {
        cf_close();                         /* the logo is portrait */
        bye = box(lv_layer_top());
        lv_obj_set_size(bye, W, H);
        lv_obj_set_style_bg_color(bye, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(bye, LV_OPA_COVER, 0);
        lv_obj_add_flag(bye, LV_OBJ_FLAG_CLICKABLE);   /* nothing below takes a touch */
        lv_obj_remove_flag(bye, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_t *mid = box(bye);
        lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(mid, 22, 0);
        lv_obj_center(mid);
        bye_logo = brand_mark(mid, &sl_brand_50, 5);
        bye_line = box(mid);
        lv_obj_set_size(bye_line, 180, 4);
        lv_obj_set_style_radius(bye_line, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(bye_line, lv_color_hex(0xff6a1f), 0);
        lv_obj_set_style_bg_opa(bye_line, LV_OPA_COVER, 0);
        bye_note = text(mid, notes[0], &ss_regular_23, 0x8b95a5);
    }
    if (why < 0 || why > 2) why = 0;
    lv_label_set_text(bye_note, sl_tr(notes[why]));
    lv_obj_set_style_text_color(bye_note, lv_color_hex(why ? 0xff6a1f : 0x8b95a5), 0);
    lv_obj_remove_flag(bye, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(bye, -1);
    lv_anim_delete(bye_logo, NULL);
    lv_anim_delete(bye_line, NULL);
    lv_anim_delete(bye_note, NULL);
    if (animate)
    {
        bye_anim(bye_logo, bye_opa, LV_OPA_TRANSP, LV_OPA_COVER, 260, 0);
        bye_anim(bye_logo, bye_rise, 18, 0, 420, 0);
        bye_anim(bye_line, bye_grow, 0, 180, 520, 120);
        bye_anim(bye_note, bye_opa, LV_OPA_TRANSP, LV_OPA_COVER, 300, 300);
        return;
    }
    bye_opa(bye_logo, LV_OPA_COVER);
    bye_rise(bye_logo, 0);
    bye_grow(bye_line, 180);
    bye_opa(bye_note, LV_OPA_COVER);
}
/* Light a dark or locked screen for it: shutting down, nothing else wants
 * the panel. The panel is enabled before drawing, as on wake. */
static void bye_light(void)
{
    sl_power_restore_display();
#ifdef HAVE_LCD_ENABLE
    lcd_enable(true);
#endif
}
/* Power off from GabagoolOS: play the goodbye while the chime sounds. */
static void bye_play(bool chime)
{
    bye_light();
    bye_show(0, true);
    uint32_t t0 = sl_ms();
    while (sl_ms() - t0 < (chime ? 700u : 560u))
    {
        lv_timer_handler();
        sleep(1);
    }
}
bool sloppy_shutdown_screen(int why)
{
    if (!sl_power_running) return false;    /* classic Rockbox owns the screen */
    bye_light();
    bye_show(why, false);
    lcd_set_viewport(NULL);
    lv_refr_now(disp);
    /* the log's file let go, so the card can be unmounted before the power
     * goes (usb_card_park): switched off mounted, it was left dirty */
    sl_log("power: off, the card let go");
    sl_log_hold(true);
    return true;
}
/* clean_shutdown() gave up (the database was busy): carry on */
static void bye_cancel(void)
{
    if (bye) lv_obj_add_flag(bye, LV_OBJ_FLAG_HIDDEN);
    sl_log_hold(false);                 /* the log again: the player goes on */
}

static void log_boot_times(void)
{
    sl_log("boot: GabagoolOS splash at %lu.%03lus after the device powered on", (unsigned long)boot_ms0 / 1000, (unsigned long)boot_ms0 % 1000);
    uint32_t prev = boot_ms0;
    for (int i = 0; i < n_marks; i++)
    {
        sl_log("boot: %-10s +%lums", marks[i].name, (unsigned long)(marks[i].ms - prev));
        prev = marks[i].ms;
    }
    sl_log("boot: home screen %lums after the splash", (unsigned long)(sl_ms() - boot_ms0));
}

#include "sloppy_charge.inc"

/* Charger and battery sounds, from the status tick. The first look only
 * learns whether a charger is in: plugging in before boot is not news.
 * Low battery speaks once at 15 % and once at 5 %, and re-arms after
 * charging or climbing back to 20 %, so a wavering reading stays quiet. */
static void car_power(bool charging, bool first);
static void power_sounds(int level, bool charging)
{
    static int known = -1;              /* charging, as last seen */
    static int warned = 100;            /* the lowest level announced */
    if (known != (int)charging) car_power(charging, known < 0);   /* car mode: the car's power came or went */
    if (known >= 0 && charging != known)
    {
        sfx_play(charging ? SFX_SYS_CHARGE : SFX_SYS_UNPLUG);
        if (charging) sl_power_cable_wake();   /* lit first, so the bubble rises on a screen that shows it */
        if (charging) charge_bubble_show(level);
        else charge_bubble_unplugged();
    }
    known = charging;
    if (level < 0) return;              /* not measured yet */
    if (charging || level >= 20) { warned = 100; return; }
    int at = level <= 5 ? 5 : level <= 15 ? 15 : 100;
    if (at >= warned) return;
    warned = at;
    char b[32];
    snprintf(b, sizeof b, "Battery low: %d%%", level);
    notify_ex(NS_SYSTEM, NOTE_WARN, 0, NA_NONE, "battery", b);   /* the words for those who can't hear it */
    sfx_play(SFX_SYS_BATT_LOW);         /* and it outranks the toast's chime */
}

#include "sloppy_dbupdate.inc"

static void sl_perf_memory(const char *where)
{
    unsigned slots = 0;
    unsigned long peak_kb;
    unsigned long rss_kb = sl_perf_rss_kb(&peak_kb);
    for (int i = 0; i < CF_SLOTS; i++)
        if (cf.art[i].px) slots++;
    sl_log("memory %s: rss %lu KiB, peak %lu KiB, LVGL images %lu KiB, cover %u/%d (%lu KiB), "
           "glass %lu KiB, sharp %lu KiB, symbols %lu KiB", where, rss_kb, peak_kb,
           (unsigned long)(lv_image_cache_get_used_size() / 1024), slots, CF_SLOTS,
           (unsigned long)((size_t)slots * CF_ART * CF_ART * 2 / 1024),
           (unsigned long)(qp_glass_px ? (size_t)W * H * 2 / 1024 : 0),
           (unsigned long)(qp_sharp_px ? (size_t)W * H * 2 / 1024 : 0), sym_bytes / 1024);
}

#include "sloppy_shot.inc"
#include "sloppy_files.inc"
#include "sloppy_arcade.inc"
#include "sloppy_2048.inc"
#include "sloppy_sudoku.inc"
#include "sloppy_points.inc"
#include "sloppy_flappy.inc"

/* ----------------------------------------------------------------- main */
static void settings_startup(void);
static void db_auto_poll(void);
/* Now Playing shut: the next song still comes, and the lists' mark and
 * "Go to current song" move to it */
static void np_follow_track(void)
{
    struct mp3entry *id3 = audio_current_track();
    if (!id3) return;
    int cue = cue_now(id3, NULL, NULL), t = cue >= 0 ? cue : sl_lib_find_path(id3->path);
    if (t == np_love_track) return;
    int was = np_love_track;
    np_love_track = t;
    song_lists_mark(was, t);
}
/* What is on screen: labels, progress, the open sheets. Nothing is seen
 * while the panel is dark, so this waits and runs once on wake. */
static void periodic_ui(void)
{
    sl_card_error_note();
    statusbar_update();
    mini_update();
    lp_refresh();
    if (np_open)
    {
        if (audio_status() & AUDIO_STATUS_PLAY) np_update(false);
        else np_hide(0);
    }
    else if (audio_status() & AUDIO_STATUS_PLAY) np_follow_track();
    if (uq_open && !(audio_status() & AUDIO_STATUS_PLAY)) uq_close();
    if (qp_open) qp_refresh();
    if (uq_open) uq_refresh_if_changed();
}

/* ------------------------------------------------------ memory budget */
/* 64 MB of RAM is shared with the stock system services, Rockbox's audio
 * buffer and the decoders. The UI keeps some big buffers for speed; when
 * the kernel reports little left, give back what can be rebuilt. */
#define SL_MEM_LOW_KB 6144
#define SL_MEM_BT_EXTRA_KB 4096
static void sl_mem_release(const char *why, bool images)
{
    size_t freed = 0;
    /* the control centre's frosted glass: rebuilt on the next pull */
    if (!qp_open && !qp_bd_on() && !cf_is_open() && !po_is_open() && (qp_glass_px || qp_sharp_px))
    {
        freed += (qp_glass_px ? 1 : 0) + (qp_sharp_px ? 1 : 0);
        free(qp_glass_px); free(qp_sharp_px);
        qp_glass_px = qp_sharp_px = NULL;
        qp_glass_ok = false;
    }
    /* Now Playing's glass, while it is shut: rebuilt when it opens */
    if (!np_open && np_glow_buf)
    {
        lv_image_set_src(np_glow, NULL);
        lv_image_cache_drop(&np_glow_img);
        free(np_glow_buf);
        np_glow_buf = NULL;
        free(nps.glow_buf);                 /* the neighbour's too: remade with the cover's */
        nps.glow_buf = NULL;
        nps.glow_ready = false;
        sl_log("memory: released Now Playing's glass (%s)", why);
    }
    if (!np_open && (nc_bd_px || lyr_bd_px))   /* Centred's backdrop and the lyrics': remade when it opens */
    {
        nc_free();
        lyr_bd_free();
        sl_log("memory: released Now Playing's backdrops (%s)", why);
    }
    if (images) lv_image_cache_drop(NULL);   /* decoded art reloads from the thumbnails */
    cf_art_trim();
    if (freed || images)
        sl_log("memory: released %lu KiB of glass%s (%s)",
               (unsigned long)(freed * W * H * 2 / 1024), images ? " and the image cache" : "", why);
}
static long sl_mem_available_kb(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char line[96];
    long kb = -1;
    while (fgets(line, sizeof line, fp))
        if (sscanf(line, "MemAvailable: %ld", &kb) == 1) break;
    fclose(fp);
    return kb;
}
static void sl_mem_watch(void)
{
    static uint32_t next_ms;
    static bool low;
    uint32_t now = sl_ms();
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + 5000;
    long kb = sl_mem_available_kb();
    if (kb < 0) return;
    bool was_low = low;
    /* over Bluetooth the encoder needs its room: give the glass back sooner */
    low = kb < SL_MEM_LOW_KB + (sl_bt_streaming() ? SL_MEM_BT_EXTRA_KB : 0);
    if (low && !was_low)
    {
        /* ours or the system's: the player's own size says which */
        unsigned long peak, rss = sl_perf_rss_kb(&peak);
        sl_log("memory: only %ld KiB available, the player %lu KiB (peak %lu)", kb, rss, peak);
        sl_mem_release("low memory", true);
    }
    /* the reflection, given back or never made: again once there is room */
    if (np_open && np_glass() && !np_glow_buf && kb >= NG_MIN_FREE_KB) np_glow_soon();
}
/* The panel went dark: the glass would be stale by the next pull anyway. */
static void sl_mem_on_dark(void)
{
    sl_mem_release("screen off", false);
}

/* Underruns are heard as clicks or gaps: note them on the card at most
 * once a minute, with the output and rate they happened on. */
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
unsigned int pcm_alsa_get_rate(void);
unsigned int pcm_alsa_get_xruns(void);
#endif
static void sl_audio_watch(void)
{
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    static unsigned seen;
    static uint32_t next_ms;
    uint32_t now = sl_ms();
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + 60000;
    unsigned x = pcm_alsa_get_xruns();
    if (x != seen)
        sl_log("audio: %u underruns in the last minute, %u Hz, %s", x - seen,
               pcm_alsa_get_rate(), bt_is_routed_fast() ? "Bluetooth" : "jack");
    seen = x;
#endif
}

static void periodic(lv_timer_t *t)
{
    /* dark, nobody sees the labels: once a second keeps play counts,
     * album chaining and the battery warning on time */
    if (t) lv_timer_set_period(t, wallpaper_screen_dark ? 1000 : 250);
    bl_tick();
#if defined(HIBY_R1) && !defined(SIMULATOR)
    usb_trace_flush();                   /* a plug that did not become USB mode, say: onto the card */
#endif
#if CONFIG_CHARGING
    power_sounds(battery_level(), charger_inserted());
#else
    power_sounds(battery_level(), false);
#endif
    if (!wallpaper_screen_dark)
    {
        periodic_ui();
        sl_mem_watch();
    }
    q_poll();
    wv_poll();                           /* the waveform hears the song with the screen dark too */
    sl_paused_poll();
    sleep_end_poll(wallpaper_screen_dark ? 1000 : 250);
    sl_audio_watch();
    if (db_update_state != DBU_IDLE) db_update_poll();
    pet_tick();
    hist_poll();
    scr_poll();
    rx_poll();
    lb_poll();
    pod_poll();
    sub_poll();
    abs_poll();
    up_poll();
    album_chain_poll();
    sr_poll();
    db_auto_poll();
    vnl_needle_poll();
    cf_art_trim();
}

/* The touch panel is read by an LVGL timer every frame. Idle, with no
 * finger down, no scroll still coasting and nothing moving for a second,
 * that timer is paused (touch_idle) and started again by any key or touch
 * event (touch_wake). While dark the panel keeps being read: a tap wakes it. */
static bool touch_paused;
static bool touch_idle(uint32_t now)
{
    int pos;
    lv_timer_t *read = lv_indev_get_read_timer(touch);
    if (touch_paused) return true;
    if (!read || now - last_touch_ms < 1000 || lv_indev_get_scroll_obj(touch) ||
        (button_status_wdata(&pos) & BUTTON_TOUCHSCREEN)) return false;
    lv_timer_pause(read);
    touch_paused = true;
    return true;
}
static void touch_wake(long btn, intptr_t pos)
{
    if (!touch_paused) return;
    touch_paused = false;
    if ((btn & BUTTON_TOUCHSCREEN) && !(btn & (BUTTON_REL | BUTTON_REPEAT)))
    {
        /* a quick tap can be over before the reader looks: keep where it was.
         * Only a press: replaying a queued release or repeat pressed again
         * where a finger had already been, a phantom contact. */
        tap_x = (int)(pos >> 16) & 0xffff;
        tap_y = (int)pos & 0xffff;
        if (tap_x < W && tap_y < H) tap_replay = 1;
    }
    lv_timer_t *read = lv_indev_get_read_timer(touch);
    lv_timer_resume(read);
    lv_timer_ready(read);                 /* read the finger on this pass */
}

/* Dark, LVGL's reader stays stopped: a tap on a dark R1 posts no button
 * event, but the driver still holds the contact. So the loop looks at it
 * itself, about 20 times a second (one call, no drawing), and a finger
 * down starts the reader with that tap, which wakes the lock screen.
 * Stopping the look altogether (to save power) let only a key wake it,
 * which is what Tap to wake off asks for. */
static void touch_dark_poll(void)
{
    int pos;
    if (!touch_paused)
    {
        lv_timer_t *read = lv_indev_get_read_timer(touch);
        if (read) { lv_timer_pause(read); touch_paused = true; }
    }
    else if (global_settings.sloppy_tap_wake &&
             (button_status_wdata(&pos) & BUTTON_TOUCHSCREEN))
        touch_wake(BUTTON_TOUCHSCREEN, pos);
}

/* Watchdog, while Cover Flow is measured: a raw thread that notes in the
 * log when the UI loop has not come round for a second, and where it was
 * (SL_AT). It only reads two words, and writes with raw syscalls: a raw
 * pthread must not call Rockbox's kernel or file layer. */
const char *volatile sl_at_where = "start";
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
#include <signal.h>
#include <ucontext.h>
#include "crashlog-linux.h"
/* 3 s into a hang (each hang, a few a session): where the UI thread is,
 * appended to crash.log like a crash (pc, ra, stack, backtrace). Nothing is
 * stopped: the handler returns, and a slow step carries on. Rockbox threads
 * share this OS thread, so the one running is named: a pc in another
 * thread's read() looked like the UI's own (2026-10-23). */
static pthread_t sl_ui_thread;
static void sl_stuck_snapshot(int sig, siginfo_t *si, void *context)
{
    (void)si;
    ucontext_t *uc = context;
    unsigned long pc = uc->uc_mcontext.pc, ra = uc->uc_mcontext.gregs[31], sp = uc->uc_mcontext.gregs[29];
    crashlog_write(sig, pc, ra, sp, NULL);
    /* and in sloppy.log, which is known to reach the card */
    unsigned long callers[12];
    int k = crashlog_callers(sp, callers, 12);
    char b[240];
    struct thread_debug_info ti;
    if (thread_get_debug_info(thread_self(), &ti) <= 0) strlcpy(ti.name, "?", sizeof ti.name);
    int n = snprintf(b, sizeof b, "watchdog: stuck in thread '%s' at pc %08lx ra %08lx, stack",
                     ti.name, pc, ra);
    for (int i = 0; i < k && n < (int)sizeof b - 10; i++)
        n += snprintf(b + n, sizeof b - n, " %08lx", callers[i]);
    if (n < (int)sizeof b - 1) b[n++] = '\n';
    if (log_fd >= 0)
    {
        syscall(SYS_write, log_fd, b, (size_t)n);
        syscall(SYS_fsync, log_fd);
    }
}
/* The log's fsync, off the UI thread (sl_log): as often as sl_log did it,
 * a quarter second later at most. Raw calls, like the rest of this thread. */
static void sl_log_sync(uint32_t now)
{
    static uint32_t synced_at;
    if (!log_unsynced || log_held || (current_tick >= 120 * HZ && now - synced_at < 2000)) return;
    log_unsynced = false;
    int fd = log_fd;
    if (fd >= 0) syscall(SYS_fsync, fd);
    synced_at = now;
}
static void *sl_watchdog(void *arg)
{
    (void)arg;
    uint32_t told = 0;
    for (;;)
    {
        /* in a pocket, look rarely: four wakes a second cost battery */
        usleep(wallpaper_screen_dark ? 2000000 : 250000);
        uint32_t beat = sl_beat_ms, now = sl_ms();
        sl_log_sync(now);
        static bool snapped;
        static int snaps;
        if (!beat || now - beat < 1000) { told = 0; snapped = false; continue; }
        if (told && now - told < 2000) continue;
        told = now;
        /* Rockbox's threads share one OS thread: name the one holding
         * it (a database scan or the buffering, not always our UI) */
        struct thread_debug_info ti;
        if (thread_get_debug_info(thread_self(), &ti) <= 0) strcpy(ti.name, "?");
        char b[256];
        /* and the clock's own thread: a stalled tick froze the R1 (3 Nov) */
        extern void (*volatile tick_task_now)(void);
        extern volatile unsigned int tick_skipped;
        int n = snprintf(b, sizeof b, "[%7ld.%02ld] watchdog: main loop stuck for %u ms in '%s', thread '%s' running, tick task %p, %u ticks put off\n",
                         current_tick / HZ, current_tick % HZ, (unsigned)(now - beat), sl_at_where, ti.name,
                         (void *)tick_task_now, tick_skipped);
        if (n <= 0 || log_fd < 0) continue;
        syscall(SYS_write, log_fd, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
        syscall(SYS_fsync, log_fd);
        if (!snapped && snaps < 8 && now - beat >= 3000)
        {
            snapped = true;
            snaps++;
            pthread_kill(sl_ui_thread, SIGUSR2);
        }
    }
    return NULL;
}
#endif

/* A computer that sees the card writes to it on its own: a Mac's Spotlight
 * index and file-event log. Unplugged before those writes reach it, the
 * exFAT is left needing a repair (a test R1, three times in two weeks, and
 * the card's .metadata_never_index lost with it). These two tell a Mac
 * to keep both off the card; made again at every start if gone. */
/* The card turned read-only: Linux does that to an exFAT it finds damaged,
 * and every save fails after ("Error accessing playlist control file").
 * Said once, in words, from any thread: the UI's periodic pass shows it. */
static volatile int sl_card_err;
static bool sl_card_err_told;
void sloppy_card_error(int err)
{
    if (err == EROFS && !sl_card_err) sl_card_err = err;
}
static void sl_card_error_note(void)
{
    if (!sl_card_err || sl_card_err_told) return;
    sl_card_err_told = true;
    sl_log("card: read-only (errno %d): asking for a check", sl_card_err);
    notify_ex(NS_SYSTEM, NOTE_WARN, 0, NA_NONE, "card",
              sl_tr("Your SD card needs a quick check. Connect it to a computer and run First Aid (Mac) or chkdsk (Windows): see \"Look after your card\" in the guide."));
}
#ifndef SL_CARD_ROOT
#define SL_CARD_ROOT ""   /* the card is the R1's / */
#endif
static void sl_card_guards(void)
{
    /* and the Games folder with its note: the release zip carries no Games
     * folder, so a "Replace" while copying an update over can't take the
     * user's cartridges with it */
    static const char note[] =
        "Super Mario Bros.\n\n"
        "GabagoolOS has a Game Boy Color player built in. Super Mario Bros. Deluxe is the\n"
        "Game Boy Color edition of Super Mario Bros. Put your own cartridge file here, named\n\n"
        "    mario.gbc\n\n"
        "and open Arcade > Super Mario Bros. on the player.\n";
    static const struct { const char *path; const char *text; } files[] = {
        { SL_CARD_ROOT "/.metadata_never_index", "" }, { SL_CARD_ROOT "/.fseventsd/no_log", "" },
        { SL_CARD_ROOT "/Games/Put mario.gbc here.txt", note },
    };
    mkdir(SL_CARD_ROOT "/.fseventsd");
    DIR *gd = opendir(SL_CARD_ROOT "/Games");
    bool games = gd != NULL;
    if (gd) closedir(gd);
    mkdir(SL_CARD_ROOT "/Games");
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++)
    {
        if (file_exists(files[i].path)) continue;
        if (files[i].text[0] && games) continue;   /* the user's own Games folder: the note only once, in a new one */
        int fd = open(files[i].path, O_WRONLY | O_CREAT, 0666);
        if (fd >= 0) { write(fd, files[i].text, strlen(files[i].text)); close(fd); }
        else { sl_log("card: %s not made, errno %d", files[i].path, errno); sloppy_card_error(errno); }
    }
}
static void sloppy_run(void)
{
    static bool inited;
    want_classic = false;
#if defined(HIBY_LINUX) && !defined(SIMULATOR)
    /* first: it syncs the log, so boot's lines cost the UI no fsync. It
     * holds its fire until the main loop beats (sl_beat_ms 0). */
    static bool watching;
    if (!watching)
    {
        watching = true;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sigfillset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sa.sa_sigaction = sl_stuck_snapshot;
        sigaction(SIGUSR2, &sa, NULL);
        sl_ui_thread = pthread_self();
        pthread_t wd;
        if (pthread_create(&wd, NULL, sl_watchdog, NULL) == 0) { pthread_detach(wd); log_sync_thread = true; }
    }
#endif
    static bool erase_checked;           /* first: an erase asked for happens before anything of ours is read */
    if (!erase_checked) { erase_checked = true; er_boot(); sl_ota_boot(); sl_card_guards(); }   /* then a swap cut short is finished */
    sl_log("GabagoolOS start, Rockbox up for %ld.%02lds", current_tick / HZ, current_tick % HZ);

    touchscreen_set_mode(TOUCHSCREEN_POINT);   /* we do our own gestures */
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    lcd_set_viewport(NULL);

    if (!inited)
    {
        inited = true;
        lv_core_init();

        struct dim d = { W, W };
        aa_slot = playback_claim_aa_slot(&d);
        np_img_buf = malloc(W * W * 2);

        sloppy_boot_mark("rockbox");
        sl_art_init();
        sloppy_boot_mark("covers");
        settings_startup();
        sloppy_boot_mark("startup");
        styles_retheme();                /* the settings are read by now */
        home_intro_ms = 60;              /* the tiles rush in as the splash dissolves */
        home_page();
        home_intro_ms = 0;
        sloppy_boot_mark("home");
        mini_build();
        np_build();
        statusbar_build();
        statusbar_update();
        devhud_show(global_settings.sloppy_dev_overlay);
        sloppy_boot_mark("players");
        lv_refr_now(disp);
        sloppy_boot_mark("drawn");
        splash_fade_out();
        lv_timer_create(bootlogo_tick, 3000, NULL);   /* once the start has settled */
        sfx_play(SFX_SYS_BOOT);
        log_boot_times();

        lib_options_apply();
        lib_epoch++;
        lib_state = lib_begin();
        sl_art_reset();
        sl_log("library: %s", lib_state == SL_LIB_LOADING ? (lib_read_db ? "reading database" : "reading library.bin") :
               lib_state == SL_LIB_NO_DB ? "no database yet" : "could not read");
        /* No usable database yet. On hosted builds the tagcache only
         * reports "ready" about a second after init, so wait for its
         * verdict before deciding the card needs a first scan. */
        if (lib_state == SL_LIB_LOADING)
            banner_show();
        lv_timer_create(periodic, 250, NULL);
        lv_timer_create(wallpaper_tick, 33, NULL);   /* ~30 fps live wallpaper */
        lv_timer_create(qp_warmup, 1200, NULL);
        lv_timer_create(coin_warm, 100, NULL);       /* Flip a Coin's faces, while idle */
        lv_timer_create(up_boot_scan, 8000, NULL);   /* an update at the card's top, once the start has settled */
    }
    else tone_apply_all();               /* back from Classic, whose menus set the EQ and tone their own way */
    lv_obj_invalidate(lv_screen_active());

    sl_power_begin();
    sl_flip_apply(global_settings.sloppy_flip == 1);   /* ours only: classic Rockbox gets the panel upright */
#if defined(HAVE_HIBY_BLUETOOTH) && !defined(SIMULATOR)
    hiby_bt_volume_by_ui = true;         /* sl_bt_volume_follow sends it; classic Rockbox's route does */
#endif
    bool art_pending = true;
    /* A loop pass that holds the UI for half a second, or a wait that
     * oversleeps, is noted with where the time went: touch, drawing and
     * USB all wait on this loop. */
    static const char *const lap_names[] = { "keys", "library", "power", "drawing", "covers" };
    static int stall_lines;
    while (!want_classic)
    {
        uint64_t lap_at = sl_perf_now_us(), lap_start = lap_at;
        sl_beat_ms = sl_ms();
        SL_AT("keys");
        uint32_t lap_us[5] = { 0 };
#define LAP(i) do { uint64_t t_ = sl_perf_now_us(); lap_us[i] += (uint32_t)(t_ - lap_at); lap_at = t_; } while (0)
        long btn;
        while ((btn = sl_flip_key(button_get(false))) != BUTTON_NONE)
        {
            touch_wake(btn, button_get_data());
            handle_button(btn);
        }
        sl_bt_volume_follow();
        LAP(0);
        SL_AT("library");

        static uint32_t next_lib_step_ms;
        if (lib_state == SL_LIB_LOADING &&
            (int32_t)(sl_ms() - next_lib_step_ms) >= 0)
        {
            static long next_banner;
            /* Bound database I/O to one small batch per display interval.
             * The main loop can otherwise run many batches between LVGL frames. */
            next_lib_step_ms = sl_ms() + LV_DEF_REFR_PERIOD;
            uint64_t step_at = sl_perf_now_us();
            lib_state = sl_lib_step(12);
            uint32_t step_us = (uint32_t)(sl_perf_now_us() - step_at);
            if (step_us > lib_step_max_us) lib_step_max_us = step_us;
            if (sl_lib_scanning()) lib_read_db = true;
            if (TIME_AFTER(current_tick, next_banner))
            {
                next_banner = current_tick + HZ / 4;
                /* library.bin did not hold, so the database is read: that takes a while */
                if (!lib_banner && sl_lib_scanning() && db_update_state != DBU_LOADING) banner_show();
                banner_update();
            }
            if (lib_state != SL_LIB_LOADING)
            {
                lib_epoch++;
                sl_log("library read finished: %d songs, %d albums in %u ms from %s, its longest step %u ms",
                       sl_lib.n_tracks, sl_lib.n_albums, (unsigned)(sl_ms() - lib_read_at),
                       lib_read_db ? "the database" : "library.bin", (unsigned)(lib_step_max_us / 1000));
                sl_art_reset();
                cf_art_flush();
                /* the library's one entry: "Updating your library" done, or failed */
                if (lib_state == SL_LIB_FAILED)
                    notify_ex(NS_LIBRARY, NOTE_WARN, 0, NA_NONE, "library", "Library could not load. Check card space and restart.");
                else if (db_update_tell)
                {
                    char b[48];
                    snprintf(b, sizeof b, "Library updated · %d songs", sl_lib.n_tracks);
                    notify_ex(NS_LIBRARY, NOTE_INFO, 0, NA_MUSIC, "library", b);
                }
                db_update_tell = false;
                banner_hide();
            }
        }
        else if (lib_state == SL_LIB_NO_DB)
        {
            static long next_lib_check;
            if (TIME_AFTER(current_tick, next_lib_check))
            {
                static bool scan_started;
                static long db_wait_until;
                next_lib_check = current_tick + HZ / 2;
                if (!db_wait_until) db_wait_until = current_tick + 6 * HZ;
                nolib_update();
                if (!scan_started && !tagcache_is_usable() &&
                    (tagcache_is_fully_initialized() || TIME_AFTER(current_tick, db_wait_until)))
                {
                    scan_started = true;
                    sl_log("no music database: starting the first scan");
                    if (!nolib) nolib_build();
                    tagcache_rebuild();
                }
                if (tagcache_is_usable())
                {
                    lib_epoch++;
                    lib_state = lib_begin();
                    sl_log("database ready: %s", lib_state == SL_LIB_NO_DB ? "still empty" : lib_read_db ? "reading it" : "reading library.bin");
                    if (lib_state != SL_LIB_NO_DB && nolib) { lv_obj_delete(nolib); nolib = nolib_txt = NULL; }
                    if (lib_state == SL_LIB_LOADING) banner_show();
                }
            }
        }

        LAP(1);
        SL_AT("power");
        sl_power_tick();
        LAP(2);
        SL_AT("lvgl");
        uint64_t handler_started = sl_perf.name || dev_hud.bar ? sl_perf_now_us() : 0;
        uint32_t wait = lv_timer_handler();
        if (handler_started)
        {
            uint32_t took = (uint32_t)(sl_perf_now_us() - handler_started);
            sl_perf_sample(handler_started, took);
            devhud_busy(took);
        }
        sl_power_finish_wake();
        LAP(3);
        static uint32_t next_art_ms;
        uint32_t now = sl_ms();
        /* the artists and genres one by one, made while nothing moves so
         * Music and Artists open without making them first */
        if (lib_state == SL_LIB_READY && g_state == G_IDLE && !lv_anim_count_running() && now - last_touch_ms > 1200
            && !tg_current() && !(is_playing() && pcmbuf_is_lowdata()))
        {
            SL_AT("artists and genres");
            tg_ready();
        }
        /* Covers are made while nothing moves: a second after a touch, or
         * in Cover Flow as soon as the row has come to rest */
        if (lib_state == SL_LIB_READY && g_state == G_IDLE &&
            !lv_anim_count_running() && (now - last_touch_ms > 1200 || cf_settled()) &&
            (int32_t)(now - next_art_ms) >= 0)
        {
            /* A cover can take 1.5 s, and the decoder only runs when this
             * loop waits. With music on, the decoder goes first: no step
             * while under a second is buffered, and a gap after each step
             * as long as the step, so the buffer refills between them. */
            bool playing = is_playing();
            if (playing && pcmbuf_is_lowdata())
                next_art_ms = now + 250;
            else
            {
                uint64_t art_from = cfm.on ? sl_perf_now_us() : 0;
                SL_AT("art step");
                art_pending = sl_art_step();
                if (art_from) cfm_add(&cfm.art_step, (uint32_t)(sl_perf_now_us() - art_from));
                uint32_t took = sl_ms() - now;
                /* an album with the cover thread is collected as soon as it is made */
                next_art_ms = sl_ms() + (sl_art_busy() ? 30 : playing && took > 150 ? took : 150);
            }
        }
        /* Until the next LVGL timer (a frame, an animation step, or the
         * touch read) there is nothing to do. Wait for it, or for a key,
         * in whole kernel ticks. While something moves, a timer due within
         * a tick is yielded for, to keep frames on time; when nothing
         * moves, being a few ms late is fine, so round the wait up.
         * Spinning on yield() between frames kept a core busy. */
        LAP(4);
#undef LAP
        if (cfm.on) cfm_add(&cfm.loop_other, lap_us[0] + lap_us[1] + lap_us[2]);
        SL_AT("waiting");
        if (lap_at - lap_start >= 500000 && stall_lines < 30)
        {
            int worst = 0;
            for (int i = 1; i < 5; i++) if (lap_us[i] > lap_us[worst]) worst = i;
            stall_lines++;
            sl_log("stall: loop pass %u ms, mostly %s (%u ms)", (unsigned)((lap_at - lap_start) / 1000),
                   lap_names[worst], (unsigned)(lap_us[worst] / 1000));
        }
        bool busy = lv_anim_count_running() > 0 || g_state != G_IDLE;
        /* Idle with the screen on and no finger down: stop reading the
         * touch panel 60 times a second. A touch posts a button event,
         * which wakes the wait below and the reading resumes at once. */
        bool deep = !busy && !wallpaper_screen_dark && lib_state == SL_LIB_READY && !art_pending &&
                    touch_idle(now);
        if (wallpaper_screen_dark) touch_dark_poll();
        /* With the touch reader stopped, a timer still due within two
         * frames is a page drawing its own animation (a canvas, a ticker):
         * keep its frames on time rather than rounded up to whole ticks. */
        if (deep && wait != LV_NO_TIMER_READY && wait < 34) busy = true;
        long cap = deep ? HZ / 10 : HZ / 20;
        if (wallpaper_screen_dark) cap = global_settings.sloppy_tap_wake ? HZ / 20 : HZ / 10;  /* nothing is drawn; the panel is looked at, unless Tap to wake is off */
        long ticks = wait == LV_NO_TIMER_READY ? cap :
                     busy ? (long)wait * HZ / 1000 : ((long)wait * HZ + 999) / 1000;
        if (ticks > cap) ticks = cap;
        if (wallpaper_screen_dark && ticks < HZ / 20) ticks = HZ / 20;   /* never busier than before */
        if (ticks < 1)
            yield();
        else
        {
            uint64_t wait_from = sl_perf_now_us();
            long btn = sl_flip_key(button_get_w_tmo((int)ticks));
            uint64_t waited = sl_perf_now_us() - wait_from;
            if (waited >= (uint64_t)ticks * 1000000u / HZ + 1000000u && stall_lines < 30)
            {
                stall_lines++;
                sl_log("stall: asked to wait %ld ms, slept %u ms", ticks * 1000 / HZ,
                       (unsigned)(waited / 1000));
            }
            if (btn != BUTTON_NONE) { touch_wake(btn, button_get_data()); handle_button(btn); }
        }
    }

    sl_power_end();
    sl_flip_apply(false);
    sl_btrx_shutdown();
#if defined(HAVE_HIBY_BLUETOOTH) && !defined(SIMULATOR)
    hiby_bt_volume_by_ui = false;
#endif
    hist_flush();
    notes_flush();
    sl_log("leaving for classic Rockbox");
    /* hand the screen back to Rockbox */
    touchscreen_set_mode(global_settings.touch_mode);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, true, NULL);
    sl_beat_ms = 0;                      /* the watchdog: nothing of ours runs now, nothing is stuck */
}
/* GabagoolOS, and a classic Rockbox program (Doom) when one is asked for:
 * once it quits, GabagoolOS again, where it was. Classic Rockbox itself
 * ends it (main.c goes on to the classic menus). */
void sloppy_main(void)
{
    for (;;)
    {
        sloppy_run();
        if (!want_plugin) return;
        sl_log("arcade: %s %s", want_plugin, want_plugin_arg ? want_plugin_arg : "");
        touchscreen_set_mode(TOUCHSCREEN_BUTTON);   /* Doom steers by the 3 x 3 grid */
        plugin_load(want_plugin, want_plugin_arg);
        want_plugin = want_plugin_arg = NULL;
        sl_log("arcade: back from the classic program");
    }
}
