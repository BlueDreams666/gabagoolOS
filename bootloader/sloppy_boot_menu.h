/* GabagoolOS boot menu for the HiBy R1 bootloader.
 *
 * The whole screen is drawn into one RGB565 frame here, in plain C, and the
 * caller blits it: anti-aliased cards and text without a font engine, and the
 * same code renders a preview off the device (bootloader/boot-menu-preview.c).
 * Text and icons are 4-bit alpha masks from sloppy_boot_art.h. */
#include "sloppy_boot_art.h"

#define SB_W 480
#define SB_H 800

struct sb_rgb { unsigned char r, g, b; };
static fb_data sb_frame[SB_W * SB_H];

static const struct sb_rgb sb_bg      = { 0x0c, 0x0c, 0x10 };
static const struct sb_rgb sb_card    = { 0x1a, 0x1a, 0x21 };
static const struct sb_rgb sb_tile    = { 0x27, 0x27, 0x31 };
static const struct sb_rgb sb_white   = { 0xf4, 0xf4, 0xf6 };
static const struct sb_rgb sb_sub     = { 0x9c, 0x9c, 0xa6 };
static const struct sb_rgb sb_muted   = { 0x6f, 0x6f, 0x7a };
static const struct sb_rgb sb_track   = { 0x24, 0x24, 0x2c };
static const struct sb_rgb sb_ink     = { 0x12, 0x12, 0x16 };
static struct sb_rgb sb_accent        = { 0x2b, 0xb3, 0xa3 };   /* GabagoolOS teal */

static struct sb_rgb sb_mix(struct sb_rgb a, struct sb_rgb b, int t)   /* t: 0..255 of b */
{
    struct sb_rgb c = { (unsigned char)(a.r + (b.r - a.r) * t / 255),
                        (unsigned char)(a.g + (b.g - a.g) * t / 255),
                        (unsigned char)(a.b + (b.b - a.b) * t / 255) };
    return c;
}

static fb_data sb_pack(struct sb_rgb c) { return FB_RGBPACK(c.r, c.g, c.b); }

static void sb_fill(int x, int y, int w, int h, struct sb_rgb c)
{
    fb_data p = sb_pack(c);
    for (int j = y; j < y + h; j++)
        if (j >= 0 && j < SB_H)
            for (int i = x; i < x + w; i++)
                if (i >= 0 && i < SB_W) sb_frame[j * SB_W + i] = p;
}

/* Rounded rectangle; corner pixels are 4x4 supersampled against bg. */
static void sb_round(int x, int y, int w, int h, int r, struct sb_rgb c, struct sb_rgb bg)
{
    sb_fill(x + r, y, w - 2 * r, h, c);
    sb_fill(x, y + r, r, h - 2 * r, c);
    sb_fill(x + w - r, y + r, r, h - 2 * r, c);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++)
        {
            int in = 0;                       /* subsamples inside the arc */
            for (int sj = 0; sj < 4; sj++)
                for (int si = 0; si < 4; si++)
                {
                    int dx = (r * 8) - (i * 8 + si * 2 + 1);
                    int dy = (r * 8) - (j * 8 + sj * 2 + 1);
                    in += dx * dx + dy * dy <= r * r * 64;
                }
            fb_data p = sb_pack(sb_mix(bg, c, in * 255 / 16));
            sb_frame[(y + j) * SB_W + x + i] = p;
            sb_frame[(y + j) * SB_W + x + w - 1 - i] = p;
            sb_frame[(y + h - 1 - j) * SB_W + x + i] = p;
            sb_frame[(y + h - 1 - j) * SB_W + x + w - 1 - i] = p;
        }
}

/* A mask in fg over a solid bg. */
static void sb_mask(const struct sb_mask *m, int x, int y, struct sb_rgb fg, struct sb_rgb bg)
{
    int stride = (m->w + 1) / 2;
    for (int j = 0; j < m->h; j++)
    {
        if (y + j < 0 || y + j >= SB_H) continue;
        for (int i = 0; i < m->w; i++)
        {
            unsigned char b = m->a[j * stride + i / 2];
            int a = i % 2 ? b & 15 : b >> 4;
            if (a && x + i >= 0 && x + i < SB_W)
                sb_frame[(y + j) * SB_W + x + i] = sb_pack(sb_mix(bg, fg, a * 17));
        }
    }
}

/* The player's own accent ("sloppy accent color" in config.cfg, 0xRRGGBB),
 * so the menu looks like the GabagoolOS it starts. */
static void sb_load_accent(const char *cfg)
{
    static char buf[8192];
    int fd = open(cfg, O_RDONLY);
    if (fd < 0) return;
    int n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    const char *key = "sloppy accent color:";
    char *p = strstr(buf, key);
    if (!p) return;
    long v = strtol(p + strlen(key), NULL, 10);
    if (v <= 0 || v > 0xffffff) return;
    struct sb_rgb c = { (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
    sb_accent = c;
}

/* rows top to bottom; index into sb_rows by the bootloader's boot_mode */
enum { SB_SLOPPY, SB_TOOLS, SB_HIBY };
struct sb_row { const struct sb_mask *icon, *title, *sub, *starting; };
static const struct sb_row sb_rows[3] = {
    [SB_SLOPPY] = { &sb_i_sloppy, &sb_t_sloppy, &sb_s_sloppy, &sb_st_sloppy },
    [SB_TOOLS]  = { &sb_i_tools,  &sb_t_tools,  &sb_s_tools,  &sb_st_tools },
    [SB_HIBY]   = { &sb_i_hiby,   &sb_t_hiby,   &sb_s_hiby,   &sb_st_hiby },
};
/* screen order: GabagoolOS, HiBy Player, Tools (Volume Down walks down it) */
static const int sb_order[3] = { SB_SLOPPY, SB_HIBY, SB_TOOLS };

#define SB_CARD_X 24
#define SB_CARD_W (SB_W - 2 * SB_CARD_X)
#define SB_CARD_H 128
#define SB_CARD_Y0 236
#define SB_CARD_GAP 16

/* sel: boot_mode index; left/total: ticks of the countdown */
static void sb_draw(int sel, int left, int total)
{
    int luma = sb_accent.r * 299 + sb_accent.g * 587 + sb_accent.b * 114;
    struct sb_rgb on_fg = luma > 160000 ? sb_ink : sb_white;
    struct sb_rgb on_sub = sb_mix(sb_accent, on_fg, 205);
    struct sb_rgb on_tile = sb_mix(sb_accent, sb_ink, 56);

    sb_fill(0, 0, SB_W, SB_H, sb_bg);
    sb_mask(&sb_wordmark, (SB_W - sb_wordmark.w) / 2, 66, sb_white, sb_bg);
    sb_round((SB_W - 36) / 2, 178, 36, 6, 3, sb_accent, sb_bg);
    sb_mask(&sb_tagline, (SB_W - sb_tagline.w) / 2, 190, sb_sub, sb_bg);

    for (int k = 0; k < 3; k++)
    {
        int m = sb_order[k];
        const struct sb_row *r = &sb_rows[m];
        bool on = m == sel;
        int y = SB_CARD_Y0 + k * (SB_CARD_H + SB_CARD_GAP);
        struct sb_rgb fill = on ? sb_accent : sb_card, tile = on ? on_tile : sb_tile;
        sb_round(SB_CARD_X, y, SB_CARD_W, SB_CARD_H, 26, fill, sb_bg);
        sb_round(SB_CARD_X + 24, y + 24, 80, 80, 20, tile, fill);
        sb_mask(r->icon, SB_CARD_X + 36, y + 36, on ? sb_white : sb_sub, tile);
        sb_mask(r->title, SB_CARD_X + 128, y + 20, on ? on_fg : sb_white, fill);
        sb_mask(r->sub, SB_CARD_X + 129, y + 70, on ? on_sub : sb_sub, fill);
    }

    /* "Starting GabagoolOS in 3" and a bar that drains to the start */
    int secs = (left + HZ - 1) / HZ;
    if (secs < 0) secs = 0;
    if (secs > 9) secs = 9;
    const struct sb_mask *st = sb_rows[sel].starting, *d = sb_digits[secs];
    int tw = st->w + 6 + d->w, tx = (SB_W - tw) / 2;
    sb_mask(st, tx, 680, sb_sub, sb_bg);
    sb_mask(d, tx + st->w + 6, 680, sb_white, sb_bg);
    sb_round(SB_CARD_X, 724, SB_CARD_W, 6, 3, sb_track, sb_bg);
    int fill_w = total > 0 ? SB_CARD_W * (left > 0 ? left : 0) / total : 0;
    if (fill_w > SB_CARD_W) fill_w = SB_CARD_W;
    if (fill_w >= 6) sb_round(SB_CARD_X, 724, fill_w, 6, 3, sb_accent, sb_track);
    sb_mask(&sb_hint, (SB_W - sb_hint.w) / 2, 748, sb_muted, sb_bg);
}
