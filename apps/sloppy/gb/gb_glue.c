/* The Game Boy core's front end inside GabagoolOS: what rockboy.c,
 * sys_rockbox.c, rbsound.c and emu.c were to the plugin, cut to what the
 * Arcade page needs. GPLv2+.
 *
 * The plugin ran the game in a loop of its own and waited on the sound
 * card between frames. Here the page asks for one frame at a time from its
 * timer, so the interface keeps running around the game; the sound goes
 * into a buffer the page's mixer channel drains. Memory comes from malloc,
 * every block noted so gb_close() can hand it all back (the plugin's
 * allocator never freed: the plugin's buffer went away with it). */
#include "rockmacros.h"
#include <stdarg.h>
#include "defs.h"
#include "regs.h"
#include "hw.h"
#include "cpu-gb.h"
#include "mem.h"
#include "lcd-gb.h"
#include "sound.h"
#include "rtc-gb.h"
#include "pcm.h"
#include "fb.h"
#include "save.h"
#include "loader.h"
#include "gb.h"

int shut, cleanshut;
struct options options;
struct fb fb;
struct pcm pcm;
extern fb_data gb_screen[160 * 144];

static char gb_err[96];
static bool gb_on;

/* ---- memory: noted, so all of it goes back at the end ---- */
#define GB_BLOCKS 16
static void *gb_block[GB_BLOCKS];
void *my_malloc(size_t size)
{
    for (int i = 0; i < GB_BLOCKS; i++)
        if (!gb_block[i])
        {
            gb_block[i] = calloc(1, size + 4);
            return gb_block[i];
        }
    return NULL;
}
static void gb_free_all(void)
{
    for (int i = 0; i < GB_BLOCKS; i++)
    {
        free(gb_block[i]);
        gb_block[i] = NULL;
    }
}

void die(char *message, ...)
{
    va_list ap;
    va_start(ap, message);
    vsnprintf(gb_err, sizeof gb_err, message, ap);
    va_end(ap);
    shut = 1;
}

void vid_begin(void) { }

/* ---- sound: what the frame made, kept until the page takes it ---- */
#define GB_PCM_LEN 1024                   /* the core's chunk, in samples (stereo pairs x 2) */
#define GB_SOUND_FRAMES 4096              /* about 90 ms at 44.1 kHz: two frames and room */
static short gb_pcm_chunk[GB_PCM_LEN];
static int16_t gb_sound[GB_SOUND_FRAMES * 2];
static int gb_sound_n;                    /* stereo frames waiting */

int rockboy_pcm_submit(void)
{
    int frames = pcm.pos / 2;
    if (frames > GB_SOUND_FRAMES - gb_sound_n)    /* nobody listening: keep the newest */
    {
        int drop = frames - (GB_SOUND_FRAMES - gb_sound_n);
        if (drop > gb_sound_n) drop = gb_sound_n;
        memmove(gb_sound, gb_sound + drop * 2, (gb_sound_n - drop) * 2 * sizeof *gb_sound);
        gb_sound_n -= drop;
    }
    if (frames > GB_SOUND_FRAMES - gb_sound_n) frames = GB_SOUND_FRAMES - gb_sound_n;
    memcpy(gb_sound + gb_sound_n * 2, pcm.buf, frames * 2 * sizeof *gb_sound);
    gb_sound_n += frames;
    pcm.pos = 0;
    return 1;
}
int gb_take_sound(int16_t *out, int max_frames)
{
    int n = gb_sound_n < max_frames ? gb_sound_n : max_frames;
    memcpy(out, gb_sound, n * 2 * sizeof *out);
    memmove(gb_sound, gb_sound + n * 2, (gb_sound_n - n) * 2 * sizeof *gb_sound);
    gb_sound_n -= n;
    return n;
}
/* the core makes a sample every snd.rate cycles of its 2^21 Hz sound clock */
int gb_sample_hz(void) { return snd.rate ? (1 << 21) / snd.rate : 44100; }

/* ---- a frame ---- */
static bool gb_drawn;
void gb_frame_done(void) { gb_drawn = true; }

static void emu_reset(void)
{
    hw_reset();
    lcd_reset();
    cpu_reset();
    mbc_reset();
    sound_reset();
}

int gb_open(const char *rom, const char *save)
{
    gb_close();
    shut = cleanshut = 0;
    gb_err[0] = 0;
    memset(&options, 0, sizeof options);
    options.sound = 1;
    options.maxskip = 4;
    plugbuf = false;
    memset(&pcm, 0, sizeof pcm);
    pcm.hz = 44100;
    pcm.stereo = 1;
    pcm.len = GB_PCM_LEN;
    pcm.buf = gb_pcm_chunk;
    gb_sound_n = 0;
    fb.enabled = 1;
    if (loader_init(rom, save) || shut)
    {
        if (!gb_err[0]) snprintf(gb_err, sizeof gb_err, "cannot read the cartridge");
        gb_free_all();
        memset(&pcm, 0, sizeof pcm);
        return -1;
    }
    emu_reset();
    lcd_begin();
    set_pal();
    gb_on = true;
    return 0;
}

void gb_close(void)
{
    if (!gb_on) return;
    cleanup();                            /* the battery save and the clock */
    gb_on = false;
    gb_free_all();
    memset(&pcm, 0, sizeof pcm);
    gb_sound_n = 0;
}

bool gb_running(void) { return gb_on && !shut; }
const char *gb_error(void) { return gb_err; }
bool gb_color(void) { return gb_on && hw.cgb; }
const uint16_t *gb_picture(void) { return (const uint16_t *)gb_screen; }

static unsigned gb_keys_held;
static void gb_keys(unsigned keys)
{
    unsigned changed = keys ^ gb_keys_held;
    for (unsigned k = 1; k <= 0x80; k <<= 1)
        if (changed & k) pad_set((byte)k, (keys & k) != 0);
    gb_keys_held = keys;
}

/* emu_run's loop, one turn: the visible lines, vertical blank, sound */
void gb_frame(unsigned keys, bool draw)
{
    if (!gb_running()) return;
    gb_keys(keys);
    fb.enabled = draw;
    gb_drawn = false;
    cpu_emulate(2280);
    while (R_LY > 0 && R_LY < 144 && !shut)
        cpu_emulate(cpu.lcdc);
    rtc_tick();
    sound_mix();
    rockboy_pcm_submit();
    lcd_begin();
    if (!(R_LCDC & 0x80))
        cpu_emulate(32832);
    while (R_LY > 0 && !shut)
        cpu_emulate(cpu.lcdc);
}

int gb_state_save(const char *path) { return gb_on ? sn_save(path) : -1; }
int gb_state_load(const char *path)
{
    if (!gb_on || sn_load(path)) return -1;
    gb_keys_held = 0;
    return 0;
}
int gb_battery_save(void) { return gb_on ? sram_save() : -1; }

void gb_palette(int k)
{
    options.pal = k >= 0 && k < GB_PALETTES ? k : 0;
    if (gb_on) set_pal();
}
extern int palettes[GB_PALETTES][4][4];   /* palette-presets.h, in lcd.c */
uint32_t gb_palette_rgb(int k, int set, int shade)
{
    if (k < 0 || k >= GB_PALETTES || set < 0 || set > 3 || shade < 0 || shade > 3) return 0;
    uint32_t c = (uint32_t)palettes[k][set][shade];   /* 0xBBGGRR */
    return (c & 0xff) << 16 | (c & 0xff00) | (c >> 16 & 0xff);
}
