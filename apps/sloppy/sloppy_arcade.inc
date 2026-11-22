/* ---- Arcade: Game Boy and Game Boy Color cartridges from the card, played
 * inside GabagoolOS. GPLv2+.
 *
 * The cartridges live where Sonix keeps them: Games/GB and Games/GBC at
 * the card's top (and Games itself). The page lists each by its file's
 * name under its system, with its box art beside it when a picture of the
 * same name sits next to it (or in Games/Art), else the last moment it was
 * played. Their saves sit beside them as every other player's do (.sav);
 * where each was left, and the one state saved by hand, go in
 * Games/States.
 *
 * The game itself runs here, on the core in gb/ (gnuboy, by way of the
 * rockboy plugin): the page asks it for each frame, scales the 160 x 144
 * picture three times to the screen's width, and plays its sound on the
 * mixer's voice channel while the music waits. The controls are drawn under
 * the picture: one finger at a time on this panel, so the Play key is A
 * and Next is B for jumping while the thumb holds the pad. Tap the
 * picture for the menu. Leaving, locking or a USB cable keeps the place.
 *
 * Held sideways (the menu's switch) the four side keys are along the top,
 * as a console's, and nothing is drawn on the screen but the game. From
 * the power key's end they are Left, B, A and Right (Volume up, Volume
 * down, Play and Next). Volume up, Volume down and
 * Play are read by one ADC on the R1 (button-r1.c: "jz adc keyboard"),
 * which hears one of them at a time; Next has a line of its own. So Right
 * goes with A or B, and Left with neither: the screen right of the
 * picture is A, for a jump while Left is held. Left of the picture is Up
 * (the half nearer the keys) and Down. Start and Select are in the menu
 * (a tap on the picture), pressed once from there. The picture is turned a
 * quarter, the same size in the middle. The menu stays upright, and with
 * it open the volume keys are the volume's again.
 *
 * Doom is still a classic Rockbox program; it runs outside this interface
 * and GabagoolOS comes back when it ends. */
#include "gb/gb.h"

/* a whole object fading (set_opa fades only its fill): the games' cards and notes */
static void gm_opa_cb(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }

/* the card's top, where Games is: "" on the player (the harness keeps its
 * own sandboxed card) */
static const char *arc_root = "";
#define ARC_CFG     ROCKBOX_DIR "/sloppy/arcade.cfg"
#define ARC_MAX     256
#define ARC_SCALE   3
#define ARC_PIC_W   (GB_W * ARC_SCALE)    /* 480: the screen's width */
#define ARC_PIC_H   (GB_H * ARC_SCALE)    /* 432 */
#define ARC_PIC_Y   SB_H
#define ARC_PAD_Y   (ARC_PIC_Y + ARC_PIC_H)
#define ARC_THUMB_W 64                    /* a row's picture: the list rows' art height, the Game Boy's shape */
#define ARC_THUMB_H 58
#define ARC_SNAP_W  80                    /* the moment a game was left: half size, on the card */
#define ARC_SNAP_H  72
#define ARC_DOOM_WAD ROCKBOX_DIR "/doom/doom1.wad"

struct arc_rom {
    char path[MAX_PATH];
    char name[64];
    bool color;
    lv_image_dsc_t art;                   /* data NULL: none read */
};
static struct arc_rom *arc_roms;
static int arc_n;
static lv_obj_t *arc_page_obj;
static char arc_last[MAX_PATH];           /* the cartridge played last */
static struct { int pal, fx, cc, sound, side; } arc_opt = { 0, 0, 1, 1, 0 };

/* GabagoolOS's six, then the Game Boy Color's twelve (gb/palette-presets.h) */
#define ARC_OWN_PALETTES 6
static const char *const arc_pal_names[GB_PALETTES] = { "Classic", "Pocket", "Backlit", "Paper", "Berry", "Ice",
    "Brown", "Red", "Dark brown", "Blue", "Dark blue", "Grey", "Pastel", "Orange", "Yellow", "Green", "Dark green", "Reversed" };
/* the screen's look over the picture: none, the LCD's pixel grid, a CRT's lines */
enum { ARC_FX_NONE, ARC_FX_GRID, ARC_FX_SCAN, ARC_FX_N };
static const char *const arc_fx_names[ARC_FX_N] = { "Off", "Pixel grid", "Scanlines" };

/* ---- settings and paths ---- */
static void arc_cfg_load(void)
{
    static bool loaded;
    if (loaded) return;
    loaded = true;
    int fd = open(ARC_CFG, O_RDONLY);
    if (fd < 0) return;
    char b[MAX_PATH + 96];
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return;
    b[n] = 0;
    int pal, fx, cc, sound, side = 0;
    if (sscanf(b, "ARCADE1 %d %d %d %d %d", &pal, &fx, &cc, &sound, &side) >= 4)   /* the fifth came with sideways */
    {
        arc_opt.side = !!side;
        arc_opt.pal = pal >= 0 && pal < GB_PALETTES ? pal : 0;
        arc_opt.fx = fx >= 0 && fx < ARC_FX_N ? fx : 0;   /* 1 was the pixel grid's On before scanlines */
        arc_opt.cc = !!cc; arc_opt.sound = !!sound;
    }
    char *last = strchr(b, '\n');
    if (last && last[1] == '/')
    {
        strlcpy(arc_last, last + 1, sizeof arc_last);
        char *end = strchr(arc_last, '\n');
        if (end) *end = 0;
    }
}
static void arc_cfg_save(void)
{
    char b[MAX_PATH + 96];
    int n = snprintf(b, sizeof b, "ARCADE1 %d %d %d %d %d\n%s\n", arc_opt.pal, arc_opt.fx, arc_opt.cc, arc_opt.sound, arc_opt.side, arc_last);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(ARC_CFG ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, n) == n;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(ARC_CFG ".tmp", ARC_CFG) < 0) remove(ARC_CFG ".tmp");
}
static const char *arc_file(const char *path) { const char *s = strrchr(path, '/'); return s ? s + 1 : path; }
/* where a cartridge's place is kept: Games/States/<file><ending> */
static void arc_state_path(const char *rom, const char *ending, char *out, size_t n)
{
    snprintf(out, n, "%s/Games/States/%s%s", arc_root, arc_file(rom), ending);
}
static void arc_mkdirs(void)
{
    char p[64];
    snprintf(p, sizeof p, "%s/Games", arc_root);
    mkdir(p);
    snprintf(p, sizeof p, "%s/Games/States", arc_root);
    mkdir(p);
}
/* the battery save: beside the cartridge, its ending swapped for .sav */
static void arc_save_base(const char *rom, char *out, size_t n)
{
    strlcpy(out, rom, n);
    char *dot = strrchr(out, '.');
    if (dot && dot > strrchr(out, '/')) *dot = 0;
}

/* ---- the list ---- */
static bool arc_is_rom(const char *f)
{
    size_t n = strlen(f);
    if (f[0] == '.') return false;       /* a Mac's ._ shadow of the file is 4 KB of nothing */
    return (n > 4 && !strcasecmp(f + n - 4, ".gbc")) || (n > 3 && !strcasecmp(f + n - 3, ".gb"));
}
/* the name people gave the file, less its ending and the dump tags that
 * follow a title in brackets: "Tetris (World) (Rev 1).gb" is Tetris */
static void arc_name(const char *file, char *out, size_t cap)
{
    strlcpy(out, file, cap);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    for (char *c = out + 1; *c; c++)
        if ((*c == '(' || *c == '[') && c[-1] == ' ') { c[-1] = 0; break; }
    for (char *c = out; *c; c++) if (*c == '_') *c = ' ';
    if (!out[0]) strlcpy(out, file, cap);
}
static bool arc_header_color(const char *path)
{
    unsigned char c = 0;
    int fd = open(path, O_RDONLY);
    bool ok = fd >= 0 && lseek(fd, 0x143, SEEK_SET) == 0x143 && read(fd, &c, 1) == 1;
    if (fd >= 0) close(fd);
    return ok && (c == 0x80 || c == 0xC0);
}
static int arc_cmp(const void *a, const void *b)
{
    const struct arc_rom *x = a, *y = b;
    if (x->color != y->color) return x->color ? -1 : 1;     /* Color first: the newer, brighter shelf */
    return sl_natcmp(x->name, y->name);
}
static void arc_free_art(void)
{
    for (int k = 0; k < arc_n; k++)
    {
        free((void *)arc_roms[k].art.data);
        arc_roms[k].art.data = NULL;
    }
}
static void arc_scan(void)
{
    static const char *const subs[] = { "/Games/GBC", "/Games/GB", "/Games" };
    arc_free_art();
    if (!arc_roms) arc_roms = calloc(ARC_MAX, sizeof *arc_roms);
    arc_n = 0;
    if (!arc_roms) return;
    for (unsigned k = 0; k < sizeof subs / sizeof subs[0]; k++)
    {
        char dir[64];
        snprintf(dir, sizeof dir, "%s%s", arc_root, subs[k]);
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)) && arc_n < ARC_MAX)
        {
            if (!arc_is_rom(e->d_name)) continue;
            struct arc_rom *r = &arc_roms[arc_n];
            memset(r, 0, sizeof *r);
            snprintf(r->path, sizeof r->path, "%s/%s", dir, e->d_name);
            arc_name(e->d_name, r->name, sizeof r->name);
            size_t n = strlen(e->d_name);
            r->color = !strcasecmp(e->d_name + n - 4, ".gbc") || arc_header_color(r->path);
            arc_n++;
        }
        if (d) closedir(d);
    }
    qsort(arc_roms, arc_n, sizeof *arc_roms, arc_cmp);
}
/* a picture for a row: box art of the same name, else where it was left */
static void arc_load_art(struct arc_rom *r)
{
    if (r->art.data) return;
    char p[MAX_PATH], base[MAX_PATH];
    uint16_t *px = NULL;
    int w = 0, h = 0;
    arc_save_base(r->path, base, sizeof base);
    snprintf(p, sizeof p, "%s.png", base);
    bool got = file_exists(p) && sl_png_load565(p, ARC_THUMB_W, ARC_THUMB_H, &px, &w, &h);
    if (!got)
    {
        char nm[MAX_PATH];
        arc_save_base(arc_file(r->path), nm, sizeof nm);
        snprintf(p, sizeof p, "%s/Games/Art/%s.png", arc_root, nm);
        got = file_exists(p) && sl_png_load565(p, ARC_THUMB_W, ARC_THUMB_H, &px, &w, &h);
    }
    if (!got)
    {
        /* the moment it was left: 80 x 72 on the card, 64 x 58 here (box filtered) */
        arc_state_path(r->path, ".thumb", p, sizeof p);
        int fd = open(p, O_RDONLY);
        static uint16_t snap[ARC_SNAP_W * ARC_SNAP_H];
        if (fd >= 0)
        {
            got = read(fd, snap, sizeof snap) == (ssize_t)sizeof snap;
            close(fd);
        }
        if (got && (px = malloc(ARC_THUMB_W * ARC_THUMB_H * 2)))
        {
            w = ARC_THUMB_W; h = ARC_THUMB_H;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    int sx = x * ARC_SNAP_W / w, sy = y * ARC_SNAP_H / h, rr = 0, gg = 0, bb = 0;
                    for (int k = 0; k < 4; k++)
                    {
                        uint16_t c = snap[LV_MIN(sy + k / 2, ARC_SNAP_H - 1) * ARC_SNAP_W + LV_MIN(sx + k % 2, ARC_SNAP_W - 1)];
                        rr += c >> 11; gg += (c >> 5) & 63; bb += c & 31;
                    }
                    px[y * w + x] = (uint16_t)((rr / 4) << 11 | (gg / 4) << 5 | (bb / 4));
                }
        }
        else got = false;
    }
    if (!got) return;
    r->art.header.magic = LV_IMAGE_HEADER_MAGIC;
    r->art.header.cf = LV_COLOR_FORMAT_RGB565;
    r->art.header.w = w;
    r->art.header.h = h;
    r->art.header.stride = w * 2;
    r->art.data_size = w * h * 2;
    r->art.data = (const uint8_t *)px;
}
static bool arc_has_state(const char *rom, const char *ending)
{
    char p[MAX_PATH];
    arc_state_path(rom, ending, p, sizeof p);
    return file_exists(p);
}

static void arc_play(int k);
static void arc_row_cb(lv_event_t *e) { arc_play((int)(intptr_t)lv_event_get_user_data(e)); }
/* a cartridge's picture, or a drawn one: the pad on its system's colour */
static lv_obj_t *arc_art(lv_obj_t *parent, struct arc_rom *r, int w, int h)
{
    lv_obj_t *t = box(parent);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_radius(t, SL_R_SM, 0);
    lv_obj_set_style_clip_corner(t, true, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    uint32_t tint = r->color ? 0x7a5bd6 : 0x6d7a6a;     /* the Color's grape, the original's grey-green */
    lv_obj_set_style_bg_color(t, lv_color_hex(tint), 0);
    lv_obj_set_style_bg_grad_color(t, lv_color_mix(lv_color_hex(tint), lv_color_black(), 170), 0);
    lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
    if (r->art.data)
    {
        lv_obj_t *im = lv_image_create(t);
        lv_image_set_src(im, &r->art);
        lv_obj_center(im);
        if (r->art.header.w != w)
            lv_image_set_scale(im, (uint32_t)(256 * w / r->art.header.w));
    }
    else lv_obj_center(sl_icon(t, IC_CARTRIDGE, h * 2 / 3, 0xffffff, tint));
    return t;
}
static lv_obj_t *arc_row(lv_obj_t *g, int k)
{
    struct arc_rom *r = &arc_roms[k];
    arc_load_art(r);
    int tx = SL_INSET + ARC_THUMB_W + SL_GUTTER;
    int lw = SET_INNER - tx - SL_INSET - set_chevron_w() - SL_SP_S;
    int lh1 = lv_font_get_line_height(&ss_semibold_21), lh2 = lv_font_get_line_height(&ss_regular_16);
    int deep = sl_line_h(&ss_regular_16) - lh2;
    int h = LV_MAX(ARC_THUMB_H + 2 * SL_SP_S + 2, lh1 + lh2 + deep + 2 * SL_GUTTER), y = (h - lh1 - lh2 - deep) / 2;
    lv_obj_t *row = set_row_box(g, h);
    lv_obj_t *a = text(row, r->name, &ss_semibold_21, C_TEXT);
    lv_obj_set_size(a, lw, lh1);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    set_align(a);
    sl_label_deep(a);
    set_place(a, tx, y, lw);
    char sub[96];
    snprintf(sub, sizeof sub, "%s \xc2\xb7 %s", sl_tr(r->color ? "Game Boy Color" : "Game Boy"),
             sl_tr(arc_has_state(r->path, ".resume") ? "Saved where you left it" : "New"));
    lv_obj_t *b = text(row, sub, &ss_regular_16, C_SUB);
    lv_obj_set_size(b, lw, lh2);
    lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
    set_align(b);
    sl_label_deep(b);
    set_place(b, tx, y + lh1, lw);
    set_place(arc_art(row, r, ARC_THUMB_W, ARC_THUMB_H), SL_INSET, (h - ARC_THUMB_H) / 2, ARC_THUMB_W);
    lv_obj_t *ch = text(row, set_rtl() ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, &sl_sym_16, C_DIM);
    lv_obj_align(ch, set_rtl() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, set_rtl() ? SL_INSET : -SL_INSET, 0);
    set_sep(g, row, tx);
    on_click(row, arc_row_cb, (void *)(intptr_t)k);
    return row;
}
/* the game played last, on a card of its own with Resume */
static void arc_continue(lv_obj_t *body, int k)
{
    struct arc_rom *r = &arc_roms[k];
    set_header(body, "CONTINUE PLAYING");
    lv_obj_t *c = box(body);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    uint32_t tint = r->color ? 0x7a5bd6 : 0x6d7a6a;
    lv_obj_set_style_bg_color(c, lv_color_mix(lv_color_hex(tint), lv_color_hex(C_CARD), sl_theme() == 1 ? 40 : 70), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    hairline(c);
    sl_elevate(c, SL_E1, 0);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, SL_GUTTER, 0);
    lv_obj_t *top = box(c);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    int aw = 2 * ARC_THUMB_W - 8, ah = aw * GB_H / GB_W;   /* 120 x 108 */
    lv_obj_t *art = box(top);
    lv_obj_set_size(art, aw, ah);
    lv_obj_t *im = arc_art(art, r, aw, ah);
    lv_obj_set_pos(im, 0, 0);
    sl_elevate(im, SL_E1, 0);
    sl_align(art, LV_ALIGN_TOP_LEFT, 0, 0);
    int tw = W - 2 * SL_GUTTER - 36 - aw - SL_GUTTER;
    lv_obj_t *t = text(top, r->name, &ss_semibold_26, C_TEXT);
    lv_obj_set_width(t, tw);
    const char *ts = lv_label_get_text(t);
    label_lines(t, lv_text_get_width(ts, strlen(ts), &ss_semibold_26, 0) > tw ? 2 : 1);
    set_align(t);
    sl_align(t, LV_ALIGN_TOP_LEFT, aw + SL_GUTTER, 4);
    lv_obj_t *s = text(top, r->color ? "Game Boy Color" : "Game Boy", &ss_regular_18, C_SUB);
    lv_obj_set_width(s, tw);
    label_lines(s, 1);
    set_align(s);
    lv_obj_update_layout(t);
    sl_align(s, LV_ALIGN_TOP_LEFT, aw + SL_GUTTER, 4 + lv_obj_get_height(t) + SL_SP_XS);
    lv_obj_t *go = mn_pill(c, LV_SYMBOL_PLAY "  Resume", W - 2 * SL_GUTTER - 36, SL_PILL_H, true, arc_row_cb, (void *)(intptr_t)k);
    lv_obj_set_style_transform_pivot_x(go, lv_pct(50), 0);
}

static void arc_scan_cb(lv_event_t *e);
static void doom_open(void);
static void arc_doom_cb(lv_event_t *e) { (void)e; doom_open(); }
static void arc_transfer_cb(lv_event_t *e) { wf_share_cb(e); }
static void arc_page_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != arc_page_obj) return;
    arc_page_obj = NULL;
    arc_free_art();
}
static void arcade_page_at(bool replace)
{
    arc_cfg_load();
    lv_obj_t *p, *body = page_new("Arcade", &p);
    if (!replace) arc_page_obj = NULL;
    lv_obj_add_event_cb(p, arc_page_deleted, LV_EVENT_DELETE, NULL);
    header_button(p, LV_SYMBOL_REFRESH, arc_scan_cb, NULL);
    arc_scan();
    if (!arc_n)
    {
        /* nothing to play: where they go, which kinds, and two ways to get them there */
        lv_obj_t *c = sl_empty(body, IC_CARTRIDGE, 96, "No games on the card yet.",
                               "Put Game Boy cartridges (.gb) in Games/GB and Game Boy Color ones (.gbc) in Games/GBC "
                               "at the top of the card. GabagoolOS doesn't come with any.", 40);
        lv_obj_set_style_pad_bottom(c, SL_SP_XS, 0);
        lv_obj_t *g = set_group(body, "HOW TO ADD THEM");
        set_action_sub(g, LV_SYMBOL_WIFI, 0x3a8ee6, "Over Wi-Fi", "Transfer in Wi-Fi, then open the Games folder",
                       arc_transfer_cb, NULL);
        /* said, not done: no chevron, no press */
        lv_obj_t *usb = set_action_sub(g, LV_SYMBOL_USB, 0x6c7a89, "With a cable",
                                       "In USB mode, copy them from a computer", NULL, NULL);
        lv_obj_t *chev = sl_zune() ? NULL : lv_obj_get_child(usb, 3);   /* Zule's rows draw none */
        if (chev) lv_obj_add_flag(chev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(usb, LV_OBJ_FLAG_CLICKABLE);
    }
    else
    {
        int last = -1;
        for (int k = 0; k < arc_n && arc_last[0]; k++)
            if (!strcmp(arc_roms[k].path, arc_last)) last = k;
        if (last >= 0) arc_continue(body, last);
        for (int sys = 1; sys >= 0; sys--)
        {
            int count = 0;
            for (int k = 0; k < arc_n; k++) count += arc_roms[k].color == sys;
            if (!count) continue;
            lv_obj_t *g = set_group(body, sys ? "GAME BOY COLOR" : "GAME BOY");
            for (int k = 0; k < arc_n; k++)
                if (arc_roms[k].color == sys) arc_row(g, k);
        }
    }
    lv_obj_t *g = set_group(body, "MORE GAMES");
    set_action_sub(g, NULL, 0, "Doom", "The 1993 shooter, as a classic Rockbox program", arc_doom_cb, NULL);
    opt_note(body, arc_n ? "More cartridges go in Games/GB and Games/GBC. Saves sit beside each one." :
                           "Games you add appear here by their file names, newest systems first.");
    arc_page_obj = p;
    if (replace) page_replace(p, true); else page_push(p);
}
static void arcade_page(void) { arcade_page_at(false); }
static void arc_scan_cb(lv_event_t *e)
{
    (void)e;
    if (arc_page_obj && npages && pages[npages - 1] == arc_page_obj) arcade_page_at(true);
}

/* ---- playing ---------------------------------------------------------------- */
enum { ARC_K_DPAD = 0x0f };
static struct {
    lv_obj_t *page, *screen, *pad, *arm[4], *arrow[4], *hub, *btn_a, *btn_b, *btn_sel, *btn_start, *btn_menu, *note;
    lv_obj_t *side;                        /* sideways: the screen either side of the picture, read, not drawn */
    bool held;                             /* paused by a lock or a panel, not by the menu */
    unsigned pulse, pulse_frames;          /* a button pressed once from the menu (Start, Select) */
    uint16_t *px;                          /* the picture at three times */
    lv_timer_t *timer;
    int rom;
    bool paused, sheet, hidden_saved, music_was_on;
    uint32_t t0, frames;
    unsigned touch, keys, shown;
    uint32_t stat_at, stat_frames, stat_cost_us, stat_worst_us;
} arc;

/* sound: the frames the core made, resampled to the mixer's rate, wait in a
 * ring the mixer's thread drains */
#define ARC_RING 8192                      /* stereo frames, a power of two */
#define ARC_CHUNK 256
static int16_t arc_ring[ARC_RING * 2];
static volatile uint32_t arc_ring_w, arc_ring_r;
static int16_t arc_chunk[ARC_CHUNK * 2];
static int16_t arc_tail[2];
static bool arc_sound_on;
static float arc_rs_pos;                   /* the resampler's place between two source frames */
static int16_t arc_rs_last[2];
static unsigned arc_fs = 44100;

static void arc_get_more(const void **start, size_t *size)
{
    uint32_t r = arc_ring_r, have = arc_ring_w - r;
    for (int i = 0; i < ARC_CHUNK; i++)
    {
        if (have) { arc_tail[0] = arc_ring[(r & (ARC_RING - 1)) * 2]; arc_tail[1] = arc_ring[(r & (ARC_RING - 1)) * 2 + 1]; r++; have--; }
        else { arc_tail[0] = (int16_t)(arc_tail[0] * 7 / 8); arc_tail[1] = (int16_t)(arc_tail[1] * 7 / 8); }   /* run dry softly, no click */
        arc_chunk[i * 2] = arc_tail[0];
        arc_chunk[i * 2 + 1] = arc_tail[1];
    }
    arc_ring_r = r;
    *start = arc_chunk;
    *size = sizeof arc_chunk;
}
static void arc_sound_stop(void)
{
    if (!arc_sound_on) return;
    arc_sound_on = false;
    mixer_channel_stop(PCM_MIXER_CHAN_VOICE);
}
static void arc_sound_start(void)
{
    if (arc_sound_on || !arc_opt.sound) return;
    arc_fs = mixer_get_frequency();
    if (arc_fs < 8000) arc_fs = 44100;
    arc_ring_w = arc_ring_r = 0;
    arc_tail[0] = arc_tail[1] = 0;
    arc_rs_pos = 0;
    /* a little silence first: the game's frames come in steps of about 17 ms */
    uint32_t pre = arc_fs * 30 / 1000;
    memset(arc_ring, 0, pre * 4);
    arc_ring_w = pre;
    arc_sound_on = true;
    const void *start;
    size_t size;
    arc_get_more(&start, &size);
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_VOICE, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_VOICE, arc_get_more, start, size);
}
/* the frame's sound into the ring, at the mixer's rate. The ring is kept
 * about 45 ms full: a little fuller and the step through the source grows a
 * hair (0.5 % at most, not heard), a little emptier and it shrinks, so the
 * game's clock and the sound card's never drift apart. */
static void arc_sound_push(void)
{
    static int16_t src[2048 * 2];
    int n = gb_take_sound(src, 2048);
    if (!arc_sound_on || n <= 0) return;
    uint32_t fill = arc_ring_w - arc_ring_r;
    float target = arc_fs * 0.045f, err = ((float)fill - target) / target;
    if (err > 1) err = 1;
    if (err < -1) err = -1;
    float step = (float)gb_sample_hz() / arc_fs * (1.0f + 0.005f * err);
    uint32_t w = arc_ring_w;
    while (arc_rs_pos < n)
    {
        if (w - arc_ring_r >= ARC_RING - 1) break;   /* the mixer stopped taking: drop the rest */
        int i = (int)arc_rs_pos;
        float f = arc_rs_pos - i;
        const int16_t *a = i ? src + (i - 1) * 2 : arc_rs_last, *b = src + i * 2;
        /* position i lies between source frame i - 1 and i */
        int l = (int)(a[0] + (b[0] - a[0]) * f), r = (int)(a[1] + (b[1] - a[1]) * f);
        arc_ring[(w & (ARC_RING - 1)) * 2] = (int16_t)l;
        arc_ring[(w & (ARC_RING - 1)) * 2 + 1] = (int16_t)r;
        w++;
        arc_rs_pos += step;
    }
    arc_rs_pos -= n;
    if (arc_rs_pos < 0) arc_rs_pos = 0;
    arc_rs_last[0] = src[(n - 1) * 2];
    arc_rs_last[1] = src[(n - 1) * 2 + 1];
    arc_ring_w = w;
}

/* ---- the picture ---- */
static inline uint16_t arc_dim(uint16_t c) { return (uint16_t)(((c >> 1) & 0x7bef) + ((c >> 2) & 0x39e7)); }   /* 3/4 */
static inline uint16_t arc_half(uint16_t c) { return (uint16_t)((c >> 1) & 0x7bef); }
/* Sideways the picture is turned a quarter in the same buffer, 432 wide
 * and 480 tall on the upright panel. The player's right side (its keys)
 * is the top: a Game Boy pixel (gx, gy) is at panel row 3 gx, column
 * 431 - 3 gy; with the screen set upside down (Settings > Buttons) the
 * keys are on the other side and it turns the other way. Each panel row
 * is one Game Boy column, read down the picture. The screen's lines (the
 * effects' third line of each pixel) are columns here. */
static bool arc_side_cw(void) { return sl_flipped; }
static void arc_blit_side(void)
{
    const uint16_t *pic = gb_picture();
    bool cw = arc_side_cw();
    int fx = arc_opt.fx;
    for (int r = 0; r < GB_W; r++)
    {
        int gx = cw ? GB_W - 1 - r : r;
        uint16_t *d = arc.px + r * ARC_SCALE * ARC_PIC_H;
        const uint16_t *s = pic + gx + (cw ? 0 : (GB_H - 1) * GB_W);
        int step = cw ? GB_W : -GB_W;
        /* a pixel's third line across the turned picture: its first column
         * here turned one way, its third the other */
        int low = cw ? 2 : 0;
        for (int k = 0; k < GB_H; k++, s += step)
        {
            uint16_t c = *s;
            d[3 * k] = d[3 * k + 1] = d[3 * k + 2] = c;
            if (fx == ARC_FX_SCAN) d[3 * k + low] = arc_half(c);
            else if (fx == ARC_FX_GRID) d[3 * k + low] = arc_dim(c);
        }
        memcpy(d + ARC_PIC_H, d, ARC_PIC_H * 2);
        memcpy(d + 2 * ARC_PIC_H, d, ARC_PIC_H * 2);
        if (fx == ARC_FX_GRID)             /* and its third column: a whole row here */
        {
            uint16_t *g = d + (cw ? 0 : 2) * ARC_PIC_H;
            for (int x = 0; x < ARC_PIC_H; x++) g[x] = arc_dim(d[ARC_PIC_H + x]);
        }
    }
    lv_obj_invalidate(arc.screen);
}
static void arc_blit(void)
{
    if (arc_opt.side) { arc_blit_side(); return; }
    const uint16_t *pic = gb_picture();
    for (int y = 0; y < GB_H; y++)
    {
        const uint16_t *s = pic + y * GB_W;
        uint16_t *d = arc.px + y * ARC_SCALE * ARC_PIC_W;
        if (arc_opt.fx == ARC_FX_SCAN)
        {
            /* a CRT's lines: each pixel's third line at half */
            for (int x = 0; x < GB_W; x++) { uint16_t c = s[x]; d[3 * x] = d[3 * x + 1] = d[3 * x + 2] = c; }
            memcpy(d + ARC_PIC_W, d, ARC_PIC_W * 2);
            for (int x = 0; x < ARC_PIC_W; x++) d[2 * ARC_PIC_W + x] = arc_half(d[x]);
        }
        else if (arc_opt.fx == ARC_FX_GRID)
        {
            /* the LCD's pixel grid: the third line and column of each pixel a shade darker */
            for (int x = 0; x < GB_W; x++) { uint16_t c = s[x]; d[3 * x] = d[3 * x + 1] = c; d[3 * x + 2] = arc_dim(c); }
            memcpy(d + ARC_PIC_W, d, ARC_PIC_W * 2);
            for (int x = 0; x < ARC_PIC_W; x++) d[2 * ARC_PIC_W + x] = arc_dim(d[x]);
        }
        else
        {
            for (int x = 0; x < GB_W; x++) { uint16_t c = s[x]; d[3 * x] = d[3 * x + 1] = d[3 * x + 2] = c; }
            memcpy(d + ARC_PIC_W, d, ARC_PIC_W * 2);
            memcpy(d + 2 * ARC_PIC_W, d, ARC_PIC_W * 2);
        }
    }
    lv_obj_invalidate(arc.screen);
}
/* the moment it was left, half size, for the list */
static void arc_write_thumb(const char *rom)
{
    static uint16_t t[ARC_SNAP_W * ARC_SNAP_H];
    const uint16_t *pic = gb_picture();
    for (int y = 0; y < ARC_SNAP_H; y++)
        for (int x = 0; x < ARC_SNAP_W; x++)
        {
            int rr = 0, gg = 0, bb = 0;
            for (int k = 0; k < 4; k++)
            {
                uint16_t c = pic[(y * 2 + k / 2) * GB_W + x * 2 + k % 2];
                rr += c >> 11; gg += (c >> 5) & 63; bb += c & 31;
            }
            t[y * ARC_SNAP_W + x] = (uint16_t)((rr / 4) << 11 | (gg / 4) << 5 | (bb / 4));
        }
    char p[MAX_PATH];
    arc_state_path(rom, ".thumb", p, sizeof p);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    if (write(fd, t, sizeof t) != (ssize_t)sizeof t) {}
    close(fd);
}
/* where it is now, so it comes back here: the state, the picture, the battery save */
static void arc_keep_place(void)
{
    if (!gb_running() || arc.rom < 0) return;
    char p[MAX_PATH];
    arc_mkdirs();
    arc_state_path(arc_roms[arc.rom].path, ".resume", p, sizeof p);
    if (gb_state_save(p) == 0) arc_write_thumb(arc_roms[arc.rom].path);
    gb_battery_save();
}

/* ---- keys: the panel under the finger, and the Play and Next keys ---- */
static bool arc_on_top(void) { return arc.page && npages && pages[npages - 1] == arc.page; }
static bool arc_playing(void) { return arc_on_top() && !arc.paused && gb_running(); }
static bool arc_side_playing(void) { return arc_opt.side && arc_playing() && !overlay_visible(); }
#define ARC_DPAD_X 116
#define ARC_DPAD_Y (ARC_PAD_Y + 150)
#define ARC_DPAD_R 86                       /* an arm's reach from the middle */
#define ARC_DPAD_ARM 60                     /* an arm's width */
#define ARC_A_X 404
#define ARC_A_Y (ARC_PAD_Y + 120)
#define ARC_B_X 308
#define ARC_B_Y (ARC_PAD_Y + 176)
#define ARC_AB_R 42
#define ARC_SS_Y (ARC_PAD_Y + 282)          /* Select and Start, small pills low in the middle */
#define ARC_SS_W 96
#define ARC_SS_H 40
#define ARC_SEL_X 186
#define ARC_START_X 294
#define ARC_MENU_Y (ARC_PAD_Y + 34)
static unsigned arc_keys_at(int x, int y)
{
    int dx = x - ARC_DPAD_X, dy = y - ARC_DPAD_Y;
    if (x < W / 2 - 10 && dx * dx + dy * dy <= (ARC_DPAD_R + 34) * (ARC_DPAD_R + 34))
    {
        if (dx * dx + dy * dy < 12 * 12) return 0;            /* the middle: nothing, as on a real pad */
        unsigned k = 0;
        /* each way owns 63 degrees around its axis; the corners between are diagonals */
        if (2 * abs(dx) > abs(dy)) k |= dx > 0 ? GB_RIGHT : GB_LEFT;
        if (2 * abs(dy) > abs(dx)) k |= dy > 0 ? GB_DOWN : GB_UP;
        return k;
    }
    if (abs(y - ARC_SS_Y) <= ARC_SS_H / 2 + 12)
    {
        if (abs(x - ARC_SEL_X) <= ARC_SS_W / 2 + 6) return GB_SELECT;
        if (abs(x - ARC_START_X) <= ARC_SS_W / 2 + 6) return GB_START;
    }
    if (x >= W / 2 - 10)
    {
        int da = (x - ARC_A_X) * (x - ARC_A_X) + (y - ARC_A_Y) * (y - ARC_A_Y);
        int db = (x - ARC_B_X) * (x - ARC_B_X) + (y - ARC_B_Y) * (y - ARC_B_Y);
        int reach = (ARC_AB_R + 30) * (ARC_AB_R + 30);
        if (da > reach && db > reach) return 0;
        /* between the two: both, for the games that want A and B together */
        int ra = (int)sqrtf((float)da), rb = (int)sqrtf((float)db);
        if (abs(ra - rb) < 12 && ra < ARC_AB_R + 22 && rb < ARC_AB_R + 22) return GB_A | GB_B;
        return da <= db ? GB_A : GB_B;
    }
    return 0;
}
/* Sideways: the long way of the screen is across, u from the left, and v
 * down from the keys' edge. Left of the picture: Up (the half nearer the
 * keys), Down; right of it: A. */
#define ARC_SIDE_BAR ((H - ARC_PIC_W) / 2)   /* 160: the screen either side of the picture */
static void arc_side_uv(int x, int y, int *u, int *v)
{
    if (arc_side_cw()) { *u = H - 1 - y; *v = x; }
    else { *u = y; *v = W - 1 - x; }
}
static unsigned arc_side_keys_at(int x, int y)
{
    int u, v;
    arc_side_uv(x, y, &u, &v);
    if (u < ARC_SIDE_BAR) return v < W / 2 ? GB_UP : GB_DOWN;
    if (u >= H - ARC_SIDE_BAR) return GB_A;
    return 0;
}
static void arc_show_keys(void)
{
    unsigned k = arc.touch | arc.keys;
    if (k == arc.shown) return;
    unsigned changed = k ^ arc.shown;
    arc.shown = k;
    static const unsigned arm_key[4] = { GB_UP, GB_RIGHT, GB_DOWN, GB_LEFT };
    uint32_t acc = sl_zune() ? ZN_PINK : C_ACCENT_UI, ink = sl_theme() == 1 && !sl_zune() ? 0x43576a : 0xb3bfcb;
    for (int i = 0; i < 4; i++)
        if (changed & arm_key[i])
        {
            bool on = k & arm_key[i];
            lv_obj_set_style_bg_opa(arc.arm[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(arc.arrow[i], lv_color_hex(on ? sl_ink_on(acc) : ink), 0);
        }
    lv_obj_t *b[4] = { arc.btn_a, arc.btn_b, arc.btn_sel, arc.btn_start };
    static const unsigned bk[4] = { GB_A, GB_B, GB_SELECT, GB_START };
    for (int i = 0; i < 4; i++)
        if (changed & bk[i])
        {
            if (k & bk[i]) lv_obj_add_state(b[i], LV_STATE_PRESSED);
            else lv_obj_remove_state(b[i], LV_STATE_PRESSED);
        }
}
static void arc_menu_open(void);
static void arc_pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)
    {
        lv_point_t pt;
        lv_indev_get_point(lv_indev_active(), &pt);
        if (code == LV_EVENT_PRESSED && abs(pt.x - W / 2) <= 40 && abs(pt.y - ARC_MENU_Y) <= 26)
        {
            lv_indev_wait_release(lv_indev_active());
            arc_menu_open();
            return;
        }
        arc.touch = arc_keys_at(pt.x, pt.y);
    }
    else arc.touch = 0;                    /* released, or the press was taken */
    arc_show_keys();
}
static void arc_side_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)
    {
        lv_point_t pt;
        lv_indev_get_point(lv_indev_active(), &pt);
        arc.touch = arc_side_keys_at(pt.x, pt.y);
    }
    else arc.touch = 0;
    arc_show_keys();
}
/* Sideways, a side key by where it is: down the R1's side from the power
 * key they are Volume up, Volume down, Play and Next; along the top from
 * the power key's end that is Left, B, A, Right. Turned the other way (the
 * screen set upside down) the power key is at the right end, so the same
 * places read the other way round. b comes after sl_flip_key, which swaps
 * the volume keys when the screen is upside down. */
static unsigned arc_side_key_of(int b)
{
    bool vol_up = sl_flipped ? b == BUTTON_DOWN : b == BUTTON_UP;
    bool vol_down = sl_flipped ? b == BUTTON_UP : b == BUTTON_DOWN;
    int from_power = vol_up ? 0 : vol_down ? 1 : b == BUTTON_LEFT ? 2 : b == BUTTON_RIGHT ? 3 : -1;
    if (from_power < 0) return 0;
    static const unsigned along_top[4] = { GB_LEFT, GB_B, GB_A, GB_RIGHT };
    return along_top[arc_side_cw() ? 3 - from_power : from_power];
}
/* the R1's keys: Play is A, Next is B (their press and their release);
 * sideways, all four are the game's while it runs (arc_side_key_of).
 * Paused, the menu open, the volume keys are the volume's. */
static bool arcade_key(int b, bool rel)
{
    if (!arc_on_top()) return false;
    unsigned k = b == BUTTON_LEFT ? GB_A : b == BUTTON_RIGHT ? GB_B : 0;
    if (arc_opt.side)
    {
        if (arc.paused) return b == BUTTON_LEFT || b == BUTTON_RIGHT || b == BUTTON_PLAY || b == BUTTON_NEXT || b == BUTTON_PREV;
        k = arc_side_key_of(b);
    }
    if (!k) return b == BUTTON_PLAY || b == BUTTON_NEXT || b == BUTTON_PREV;   /* a headset's keys do nothing here */
    if (rel) arc.keys &= ~k; else arc.keys |= k;
    arc_show_keys();
    return true;
}

/* ---- running ---- */
static void arc_pause(bool on)
{
    if (on == arc.paused) return;
    arc.paused = on;
    if (on) { arc_sound_stop(); arc.touch = arc.keys = 0; arc.pulse_frames = 0; arc_show_keys(); }
    else
    {
        arc.t0 = lv_tick_get();
        arc.frames = 0;
        arc.hidden_saved = false;
        arc_sound_start();
    }
}
static void arc_tick(lv_timer_t *t)
{
    (void)t;
    if (!arc.page) return;
    bool hidden = sl_power_hides_ui() || !arc_on_top();
    if (hidden || (overlay_visible() && !arc.sheet))
    {
        arc_pause(true);
        arc.held = true;
        if (sl_power_hides_ui() && !arc.hidden_saved) { arc.hidden_saved = true; arc_keep_place(); }   /* locked: a flat battery keeps it */
        return;
    }
    if (arc.sheet)
    {
        /* the menu put away, and gone: while it slides off it is still an
         * overlay, and taken for another one it paused the game for good
         * (a test R1, Nov 13: no frame after the menu closed) */
        if (acts_open_now || (acts && !lv_obj_has_flag(acts, LV_OBJ_FLAG_HIDDEN))) return;
        arc.sheet = false;                 /* carry on */
        arc.held = false;
        arc_pause(false);
    }
    else if (arc.held)
    {
        /* back from a lock or a panel: paused, and the menu says so, its
         * Resume one tap away (keys alone did nothing on a paused game) */
        arc.held = false;
        if (gb_running()) { arc_menu_open(); return; }
    }
    if (arc.paused || !gb_running()) return;
    uint32_t now = lv_tick_get();          /* the interface's clock (sl_ms on the player) */
    uint32_t due = (uint32_t)((uint64_t)(now - arc.t0) * GB_FPS_X100 / 100000);
    int n = (int)(due - arc.frames);
    if (n <= 0) return;
    if (n > 4) { arc.t0 = now; arc.frames = 0; n = 1; }      /* far behind (a slow card write): start the clock again */
    uint64_t c0 = sl_perf_now_us();
    unsigned keys = arc.touch | arc.keys;
    for (int i = 0; i < n; i++)
    {
        gb_frame(keys | (arc.pulse_frames ? arc.pulse : 0), i == n - 1);
        if (arc.pulse_frames) arc.pulse_frames--;
        arc_sound_push();
    }
    arc.frames += n;
    arc_blit();
    uint32_t cost = (uint32_t)(sl_perf_now_us() - c0);
    arc.stat_frames += n;
    arc.stat_cost_us += cost;
    if (cost > arc.stat_worst_us) arc.stat_worst_us = cost;
    if (now - arc.stat_at >= 10000)
    {
        /* for the device check: what the game gets, and what a frame costs here */
        if (arc.stat_at)
        {
            unsigned fps10 = arc.stat_frames * 10000u / (now - arc.stat_at);
            sl_log("arcade: %u.%u frames a second, %u us of work a frame (worst %u us)", fps10 / 10, fps10 % 10,
                   arc.stat_frames ? arc.stat_cost_us / arc.stat_frames : 0, arc.stat_worst_us);
        }
        arc.stat_at = now;
        arc.stat_frames = arc.stat_cost_us = arc.stat_worst_us = 0;
    }
    if (!gb_running())
    {
        sl_log("arcade: stopped: %s", gb_error());
        toast_warn("The game stopped");
        arc_pause(true);
    }
}

/* ---- the menu ---- */
static void arc_menu_close(void) { acts_close(); }
static void arc_resume_cb(lv_event_t *e) { (void)e; arc_menu_close(); }
static void arc_save_cb(lv_event_t *e)
{
    (void)e;
    char p[MAX_PATH];
    arc_mkdirs();
    arc_state_path(arc_roms[arc.rom].path, ".state", p, sizeof p);
    bool ok = gb_state_save(p) == 0;
    gb_battery_save();
    arc_menu_close();
    ok ? toast("Game saved") : toast_warn("Could not save the game");
}
static void arc_load_cb(lv_event_t *e)
{
    (void)e;
    char p[MAX_PATH];
    arc_state_path(arc_roms[arc.rom].path, ".state", p, sizeof p);
    bool ok = gb_state_load(p) == 0;
    arc_menu_close();
    if (ok) { gb_frame(0, true); arc_blit(); toast("Saved game loaded"); }
    else toast_warn("Could not load the saved game");
}
static void arc_restart_cb(lv_event_t *e)
{
    (void)e;
    char base[MAX_PATH];
    arc_save_base(arc_roms[arc.rom].path, base, sizeof base);
    gb_battery_save();
    if (gb_open(arc_roms[arc.rom].path, base)) toast_warn("Could not open the game");
    gb_palette(arc_opt.pal);
    arc_menu_close();
}
static void arc_quit_cb(lv_event_t *e) { (void)e; arc_menu_close(); page_pop(); }
/* a row that says what it is set to and changes it in place */
static void arc_opt_label(lv_obj_t *r, const char *what, const char *value)
{
    char b[96];
    snprintf(b, sizeof b, "%s: %s", sl_tr(what), sl_tr(value));
    lv_obj_t *l = lv_obj_get_child(r, 1);
    char low[96];
    lv_label_set_text(l, sl_zune() ? sl_lower(b, low, sizeof low) : b);
}
/* Colours: every palette as a swatch, four to a row, ours then the Game
 * Boy Color's; a tap shows it at once behind the sheet, Done goes back */
#define ARC_SW_COLS 4
#define ARC_SW_W    ((W - 2 * SL_FLOAT - 2 * 10 - (ARC_SW_COLS - 1) * SL_SP_S) / ARC_SW_COLS)   /* 104 */
#define ARC_SW_H    50                    /* the ring's: the swatch is 5 in from it */
static lv_obj_t *arc_sw[GB_PALETTES];
static void arc_menu_fill(void);
static void arc_sw_mark(void)
{
    for (int k = 0; k < GB_PALETTES; k++)
        if (arc_sw[k]) lv_obj_set_style_border_opa(arc_sw[k], k == arc_opt.pal ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}
static void arc_sw_cb(lv_event_t *e)
{
    arc_opt.pal = (int)(intptr_t)lv_event_get_user_data(e);
    gb_palette(arc_opt.pal);
    gb_frame(arc.touch | arc.keys, true);
    arc_blit();
    arc_sw_mark();
    arc_cfg_save();
}
static void arc_sw_done_cb(lv_event_t *e) { (void)e; arc_menu_fill(); }
static void arc_sw_grid(int from, int to)
{
    lv_obj_t *g = box(acts);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_base_dir(g, sl_ui_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);   /* the first at the reading edge */
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, SL_SP_S, 0);
    lv_obj_set_style_pad_row(g, SL_SP_S + SL_SP_XS, 0);
    lv_obj_set_style_pad_bottom(g, SL_SP_S, 0);
    for (int k = from; k < to; k++)
    {
        lv_obj_t *t = box(g);
        lv_obj_set_size(t, ARC_SW_W, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(t, SL_SP_XS, 0);
        lv_obj_set_style_pad_bottom(t, 6, 0);   /* room under the name for Arabic's deepest letters */
        press_motion(t);
        /* the one in use ringed in the accent, a little way out from its swatch */
        lv_obj_t *ring = box(t);
        arc_sw[k] = ring;
        lv_obj_set_size(ring, ARC_SW_W, ARC_SW_H);
        lv_obj_set_style_radius(ring, sl_zune() ? 0 : SL_R_SM + 5, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_pad_all(ring, 2, 0);
        /* the four shades, lightest to darkest; the sprites' own two over
         * them when the Game Boy Color colours them apart */
        lv_obj_t *sw = box(ring);
        lv_obj_set_size(sw, lv_pct(100), lv_pct(100));
        lv_obj_set_style_radius(sw, sl_zune() ? 0 : SL_R_SM, 0);
        lv_obj_set_style_clip_corner(sw, true, 0);
        /* the hairline of every panel, so a white shade reads on a white sheet */
        lv_obj_set_style_border_width(sw, 1, 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(sl_zune() ? 0x2a2a2a : C_SUB), 0);
        lv_obj_set_style_border_opa(sw, sl_zune() ? LV_OPA_COVER : 36, 0);
        lv_obj_set_style_border_post(sw, true, 0);
        lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW);
        bool apart = gb_palette_rgb(k, 2, 1) != gb_palette_rgb(k, 0, 1) || gb_palette_rgb(k, 3, 1) != gb_palette_rgb(k, 0, 1);
        for (int c = 0; c < 4; c++)
        {
            lv_obj_t *band = box(sw);
            lv_obj_set_size(band, 0, lv_pct(100));
            lv_obj_set_flex_grow(band, 1);
            lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(band, lv_color_hex(gb_palette_rgb(k, 0, c)), 0);
        }
        if (apart)
            for (int c = 0; c < 2; c++)
            {
                lv_obj_t *dot = box(sw);
                lv_obj_add_flag(dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
                lv_obj_set_size(dot, 12, 12);
                lv_obj_set_style_radius(dot, SL_R_FULL, 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(dot, lv_color_hex(gb_palette_rgb(k, 2 + c, 1)), 0);
                lv_obj_set_style_border_width(dot, 2, 0);
                lv_obj_set_style_border_color(dot, lv_color_hex(gb_palette_rgb(k, 2 + c, 3)), 0);
                sl_align(dot, LV_ALIGN_BOTTOM_RIGHT, -6 - c * 16, -6);
            }
        lv_obj_t *name = text(t, arc_pal_names[k], &ss_regular_16, C_SUB);
        lv_obj_set_width(name, ARC_SW_W);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        sl_label_deep(name);
        on_click(t, arc_sw_cb, (void *)(intptr_t)k);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}
static void arc_sw_head(const char *s)
{
    /* a page's section head, on the sheet's own grey */
    lv_obj_t *l = text(acts, s, &ss_semibold_18, C_SUB);
    lv_obj_set_style_pad_top(l, SL_SP_S, 0);
    lv_obj_set_style_pad_left(l, 6, 0);
}
static void arc_pal_cb(lv_event_t *e)
{
    (void)e;
    acts_begin();
    memset(arc_sw, 0, sizeof arc_sw);
    lv_obj_t *t = text(acts, "Colours", &ss_semibold_23, 0xffffff);
    lv_obj_set_style_pad_all(t, SL_SP_S, 0);
    arc_sw_head("GABAGOOLOS");
    arc_sw_grid(0, ARC_OWN_PALETTES);
    arc_sw_head("GAME BOY COLOR");
    arc_sw_grid(ARC_OWN_PALETTES, GB_PALETTES);
    acts_row_cb(LV_SYMBOL_OK, "Done", arc_sw_done_cb, 0);
    arc_sw_mark();
    acts_show();
}
static void arc_fx_cb(lv_event_t *e)
{
    arc_opt.fx = (arc_opt.fx + 1) % ARC_FX_N;
    arc_blit();
    arc_opt_label(lv_event_get_current_target(e), "Screen effect", arc_fx_names[arc_opt.fx]);
    arc_cfg_save();
}
static void arc_sound_cb(lv_event_t *e)
{
    arc_opt.sound = !arc_opt.sound;
    arc_opt_label(lv_event_get_current_target(e), "Game sound", arc_opt.sound ? "On" : "Off");
    arc_cfg_save();
}
static void arc_layout(void);
static void arc_menu_refill_async(void *ud) { (void)ud; if (arc.page && arc.sheet && acts_open_now) arc_menu_fill(); }
static void arc_side_opt_cb(lv_event_t *e)
{
    arc_opt.side = !arc_opt.side;
    arc_opt_label(lv_event_get_current_target(e), "Hold sideways", arc_opt.side ? "On" : "Off");
    arc_cfg_save();
    arc_layout();
    lv_async_call(arc_menu_refill_async, NULL);   /* Start and Select come and go with it: after this tap, not under it */
    if (arc_opt.side) toast("Along the top from the power key: Left, B, A, Right. Tap right of the game for A.");
}
/* Start or Select, pressed once (8 frames, about 130 ms) as the game goes on */
static void arc_press_once(unsigned k)
{
    arc.pulse = k;
    arc.pulse_frames = 8;
    arc_menu_close();
}
static void arc_start_cb(lv_event_t *e) { (void)e; arc_press_once(GB_START); }
static void arc_select_cb(lv_event_t *e) { (void)e; arc_press_once(GB_SELECT); }
static void arc_menu_open(void)
{
    if (!arc.page || arc.sheet) return;
    arc_pause(true);
    arc.sheet = true;
    arc_menu_fill();
}
static void arc_menu_fill(void)
{
    struct arc_rom *r = &arc_roms[arc.rom];
    acts_begin();
    lv_obj_t *hd = box(acts);
    lv_obj_set_size(hd, lv_pct(100), 84);
    lv_obj_t *t1 = text(hd, r->name, &ss_semibold_23, 0xffffff);
    lv_obj_set_width(t1, W - 20 - 2 * 10 - 32);
    label_lines(t1, 1);
    sl_place(t1, 16, 12);
    lv_obj_t *t2 = text(hd, "Paused", &ss_regular_16, C_SUB);
    sl_place(t2, 16, 12 + lv_font_get_line_height(&ss_semibold_23) + 4);
    lv_obj_set_height(hd, LV_MAX(84, 12 + lv_font_get_line_height(&ss_semibold_23) + 4 + lv_font_get_line_height(&ss_regular_16) + 12));
    acts_row_cb(LV_SYMBOL_PLAY, "Resume", arc_resume_cb, 0);
    if (arc_opt.side)
    {
        /* sideways the screen has no room for them: here, each pressed once */
        acts_row_cb(LV_SYMBOL_RIGHT, "START", arc_start_cb, 0);
        acts_row_cb(LV_SYMBOL_RIGHT, "SELECT", arc_select_cb, 0);
    }
    acts_row_cb(LV_SYMBOL_DOWNLOAD, "Save game here", arc_save_cb, 0);
    if (arc_has_state(r->path, ".state")) acts_row_cb(LV_SYMBOL_UPLOAD, "Load saved game", arc_load_cb, 0);
    lv_obj_t *row;
    if (!gb_color())
    {
        acts_row_cb(LV_SYMBOL_IMAGE, "Colours", arc_pal_cb, 0);
        row = lv_obj_get_child(acts, -1);
        arc_opt_label(row, "Colours", arc_pal_names[arc_opt.pal]);
    }
    acts_row_cb(LV_SYMBOL_EYE_OPEN, "Screen effect", arc_fx_cb, 0);
    arc_opt_label(lv_obj_get_child(acts, -1), "Screen effect", arc_fx_names[arc_opt.fx]);
    acts_row_cb(LV_SYMBOL_VOLUME_MAX, "Game sound", arc_sound_cb, 0);
    arc_opt_label(lv_obj_get_child(acts, -1), "Game sound", arc_opt.sound ? "On" : "Off");
    acts_row_cb(LV_SYMBOL_LOOP, "Hold sideways", arc_side_opt_cb, 0);
    arc_opt_label(lv_obj_get_child(acts, -1), "Hold sideways", arc_opt.side ? "On" : "Off");
    acts_row_cb(LV_SYMBOL_REFRESH, "Start over", arc_restart_cb, 0);
    acts_row_cb(LV_SYMBOL_CLOSE, "Quit game", arc_quit_cb, 0);
    acts_show();
}
static void arc_screen_cb(lv_event_t *e) { (void)e; arc_menu_open(); }

/* ---- the page ---- */
static uint32_t arc_bg(void) { return sl_zune() || sl_theme() == 2 ? 0x000000 : sl_theme() == 1 ? 0xe6e9ed : 0x0f1318; }
static uint32_t arc_surface(void) { return sl_zune() || sl_theme() == 2 ? 0x1c1c1e : sl_theme() == 1 ? 0xffffff : 0x232a33; }
static uint32_t arc_surface_p(void) { return sl_zune() || sl_theme() == 2 ? 0x2c2c2e : sl_theme() == 1 ? 0xd5dae0 : 0x323b47; }
static lv_obj_t *arc_round(lv_obj_t *p, int cx, int cy, int w, int h, uint32_t bg, uint32_t bg_p, const char *label,
                           const lv_font_t *f, uint32_t ink)
{
    lv_obj_t *b = box(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, cx - w / 2, cy - h / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg_p), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(b, SL_PRESSED - 10, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(b, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, h / 2, 0);
    sl_elevate(b, SL_E1, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);          /* the panel under them reads the finger */
    if (label)
    {
        lv_obj_t *l = text(b, label, f, ink);
        lv_obj_center(l);
    }
    return b;
}
/* upright or sideways: where the picture is, which controls show */
static void arc_layout(void)
{
    if (!arc.page) return;
    bool side = arc_opt.side;
    memset(arc.px, 0, ARC_PIC_W * ARC_PIC_H * 2);
    lv_canvas_set_buffer(arc.screen, arc.px, side ? ARC_PIC_H : ARC_PIC_W, side ? ARC_PIC_W : ARC_PIC_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(arc.screen, side ? (W - ARC_PIC_H) / 2 : 0, side ? ARC_SIDE_BAR : ARC_PIC_Y);
    lv_obj_set_style_bg_color(arc.page, lv_color_hex(side ? 0x000000 : arc_bg()), 0);
    if (side) { lv_obj_add_flag(arc.pad, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(arc.side, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_remove_flag(arc.pad, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(arc.side, LV_OBJ_FLAG_HIDDEN); }
    if (arc.note && side) lv_obj_add_flag(arc.note, LV_OBJ_FLAG_HIDDEN);   /* its words would lie on their side */
    statusbar_show(!side);
    arc.touch = arc.keys = 0;
    arc_show_keys();
    arc_blit();
}
/* Sideways' screen: one panel under the picture that reads the finger
 * (arc_side_keys_at); nothing drawn on it, the game is the whole screen. */
static void arc_side_build(lv_obj_t *pg)
{
    arc.side = box(pg);
    lv_obj_set_size(arc.side, W, H);
    lv_obj_set_pos(arc.side, 0, 0);
    lv_obj_add_flag(arc.side, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_remove_flag(arc.side, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(arc.side, arc_side_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(arc.side, arc_side_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(arc.side, arc_side_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(arc.side, arc_side_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_flag(arc.side, LV_OBJ_FLAG_HIDDEN);
}
static void arc_rescan_async(void *ud) { (void)ud; arc_scan_cb(NULL); }
static void arc_game_deleted(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != arc.page) return;
    if (arc.sheet) acts_close();
    arc_sound_stop();
    arc_keep_place();
    gb_close();
    if (arc.timer) lv_timer_delete(arc.timer);
    free(arc.px);
    memset(&arc, 0, sizeof arc);
    arc.rom = -1;
    statusbar_show(true);                  /* sideways hid it */
    statusbar_retheme();
    /* the list says where each was left, with its last picture */
    if (arc_page_obj) lv_async_call(arc_rescan_async, NULL);
}
static void arc_play(int k)
{
    if (k < 0 || k >= arc_n || arc.page) return;
    if (sl_btrx_on) { toast_warn("Stop receiving from your phone first"); return; }
    struct arc_rom *r = &arc_roms[k];
    {   /* the cartridge is read whole, and the screen and state come with it */
        struct stat st;
        long rom_kb = stat(r->path, &st) == 0 ? (long)(st.st_size / 1024) : 0;
        if (!sl_app_room(rom_kb + 600, "Arcade")) return;
    }
    char base[MAX_PATH];
    arc_save_base(r->path, base, sizeof base);
    arc_cfg_load();
    uint16_t *px = malloc(ARC_PIC_W * ARC_PIC_H * 2);
    if (!px || gb_open(r->path, base))
    {
        free(px);
        sl_log("arcade: %s: %s", r->path, gb_error());
        toast_warn("This cartridge couldn't be opened");
        return;
    }
    sl_log("arcade: %s (%s)", r->path, gb_color() ? "Game Boy Color" : "Game Boy");
    gb_palette(arc_opt.pal);
    char p[MAX_PATH];
    arc_state_path(r->path, ".resume", p, sizeof p);
    bool resumed = file_exists(p) && gb_state_load(p) == 0;
    if (np_open) np_hide(0);
    /* one sound at a time: the song waits, a station stops */
    if (st_on()) st_end(NULL);
    if (audio_status() & AUDIO_STATUS_PLAY && !(audio_status() & AUDIO_STATUS_PAUSE)) audio_pause();
    strlcpy(arc_last, r->path, sizeof arc_last);
    arc_cfg_save();

    memset(&arc, 0, sizeof arc);
    arc.rom = k;
    arc.px = px;
    memset(px, 0, ARC_PIC_W * ARC_PIC_H * 2);
    lv_obj_t *pg, *body = page_new(r->name, &pg);
    arc.page = pg;
    lv_obj_add_event_cb(pg, arc_game_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_flag(lv_obj_get_child(pg, 0), LV_OBJ_FLAG_HIDDEN);   /* the picture is the header */
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(pg, lv_color_hex(arc_bg()), 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, 0);
    statusbar_ink(sl_theme() == 1 && !sl_zune() ? 0x182735 : 0xffffff);

    arc.screen = lv_canvas_create(pg);
    lv_canvas_set_buffer(arc.screen, px, ARC_PIC_W, ARC_PIC_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(arc.screen, 0, ARC_PIC_Y);
    lv_obj_add_flag(arc.screen, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);   /* no back-swipe over the game */
    on_click(arc.screen, arc_screen_cb, NULL);

    /* the controls: drawn pieces on one panel that reads the finger */
    arc.pad = box(pg);
    lv_obj_set_size(arc.pad, W, H - ARC_PAD_Y);
    lv_obj_set_pos(arc.pad, 0, ARC_PAD_Y);
    lv_obj_add_flag(arc.pad, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_remove_flag(arc.pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(arc.pad, arc_pad_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(arc.pad, arc_pad_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(arc.pad, arc_pad_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(arc.pad, arc_pad_cb, LV_EVENT_PRESS_LOST, NULL);
    int py = -ARC_PAD_Y;                   /* the panel's own coordinates */
    uint32_t sf = arc_surface(), sp = arc_surface_p(), ink = sl_theme() == 1 && !sl_zune() ? 0x43576a : 0xb3bfcb;
    uint32_t acc = sl_zune() ? ZN_PINK : C_ACCENT_UI;
    /* the d-pad: a cross, each arm lit in the accent while it is held */
    lv_obj_t *h = box(arc.pad), *v = box(arc.pad);
    lv_obj_set_size(h, 2 * ARC_DPAD_R, ARC_DPAD_ARM);
    lv_obj_set_pos(h, ARC_DPAD_X - ARC_DPAD_R, ARC_DPAD_Y + py - ARC_DPAD_ARM / 2);
    lv_obj_set_size(v, ARC_DPAD_ARM, 2 * ARC_DPAD_R);
    lv_obj_set_pos(v, ARC_DPAD_X - ARC_DPAD_ARM / 2, ARC_DPAD_Y + py - ARC_DPAD_R);
    lv_obj_t *cross[2] = { h, v };
    for (int i = 0; i < 2; i++)
    {
        lv_obj_set_style_radius(cross[i], SL_R_MD, 0);
        lv_obj_set_style_bg_color(cross[i], lv_color_hex(sf), 0);
        lv_obj_set_style_bg_opa(cross[i], LV_OPA_COVER, 0);
        sl_elevate(cross[i], SL_E1, 0);
    }
    static const int ax[4] = { 0, 1, 0, -1 }, ay[4] = { -1, 0, 1, 0 };
    static const char *const sym[4] = { LV_SYMBOL_UP, LV_SYMBOL_RIGHT, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT };
    int arm_l = ARC_DPAD_R - ARC_DPAD_ARM / 2;
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *a = box(arc.pad);
        int w = ax[i] ? arm_l : ARC_DPAD_ARM, hh = ay[i] ? arm_l : ARC_DPAD_ARM;
        int cx = ARC_DPAD_X + ax[i] * (ARC_DPAD_ARM / 2 + arm_l / 2), cy = ARC_DPAD_Y + py + ay[i] * (ARC_DPAD_ARM / 2 + arm_l / 2);
        lv_obj_set_size(a, w, hh);
        lv_obj_set_pos(a, cx - w / 2, cy - hh / 2);
        lv_obj_set_style_radius(a, SL_R_MD, 0);
        lv_obj_set_style_bg_color(a, lv_color_hex(acc), 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
        arc.arm[i] = a;
        lv_obj_t *s = text(arc.pad, sym[i], &sl_sym_18, ink);
        arc.arrow[i] = s;
        lv_obj_update_layout(s);
        lv_obj_set_pos(s, cx - lv_obj_get_width(s) / 2 + ax[i] * 6, cy - lv_obj_get_height(s) / 2 + ay[i] * 6);
    }
    arc.hub = box(arc.pad);
    lv_obj_set_size(arc.hub, 22, 22);
    lv_obj_set_pos(arc.hub, ARC_DPAD_X - 11, ARC_DPAD_Y + py - 11);
    lv_obj_set_style_radius(arc.hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(arc.hub, lv_color_hex(sp), 0);
    lv_obj_set_style_bg_opa(arc.hub, LV_OPA_COVER, 0);
    /* A and B, on the slant the Game Boy has them */
    arc.btn_b = arc_round(arc.pad, ARC_B_X, ARC_B_Y + py, 2 * ARC_AB_R, 2 * ARC_AB_R, sf, sp, "B", &ss_bold_28, C_TEXT);
    arc.btn_a = arc_round(arc.pad, ARC_A_X, ARC_A_Y + py, 2 * ARC_AB_R, 2 * ARC_AB_R, acc,
                          lv_color_to_u32(lv_color_mix(lv_color_hex(acc), lv_color_black(), 200)) & 0xffffff, "A", &ss_bold_28,
                          sl_ink_on(acc));
    arc.btn_sel = arc_round(arc.pad, ARC_SEL_X, ARC_SS_Y + py, ARC_SS_W, ARC_SS_H, sf, sp, "SELECT", &ss_semibold_18, ink);
    arc.btn_start = arc_round(arc.pad, ARC_START_X, ARC_SS_Y + py, ARC_SS_W, ARC_SS_H, sf, sp, "START", &ss_semibold_18, ink);
    arc.btn_menu = arc_round(arc.pad, W / 2, ARC_MENU_Y + py, 64, 36, sf, sp, LV_SYMBOL_PAUSE, &sl_sym_16, ink);
    for (int i = 0; i < 2; i++)
        lv_obj_set_style_text_letter_space(lv_obj_get_child(i ? arc.btn_start : arc.btn_sel, 0), 1, 0);

    arc_side_build(pg);
    lv_obj_move_foreground(arc.screen);    /* over the sideways panel: a tap on the picture is still the menu */

    arc.timer = lv_timer_create(arc_tick, 8, NULL);
    arc.t0 = lv_tick_get();
    arc.stat_at = 0;
    gb_frame(0, true);
    if (arc_opt.side) arc_layout();
    else arc_blit();
    page_push(pg);
    arc_sound_start();
    if (resumed && !arc_opt.side)       /* sideways its words would lie on their side */
    {
        /* said on the picture, where the eyes are, and gone in a moment: a
         * toast would sit over the controls */
        arc.note = box(pg);
        lv_obj_set_size(arc.note, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(arc.note, SL_GUTTER, 0);
        lv_obj_set_style_pad_ver(arc.note, SL_SP_S, 0);
        lv_obj_set_style_radius(arc.note, sl_zune() ? 0 : SL_R_PILL, 0);
        lv_obj_set_style_bg_color(arc.note, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(arc.note, LV_OPA_70, 0);
        text(arc.note, "Back where you left off", &ss_semibold_18, 0xfffffe);
        lv_obj_align(arc.note, LV_ALIGN_TOP_MID, 0, ARC_PIC_Y + SL_GUTTER);
        lv_obj_remove_flag(arc.note, LV_OBJ_FLAG_CLICKABLE);
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, arc.note);
        lv_anim_set_exec_cb(&an, gm_opa_cb);
        lv_anim_set_values(&an, 255, 0);
        lv_anim_set_duration(&an, SL_T_SLOW);
        lv_anim_set_delay(&an, 1600);
        lv_anim_set_completed_cb(&an, hide_cb);
        lv_anim_start(&an);
    }
}

/* ---- Doom: a classic Rockbox program, run once the interface's loop ends;
 * GabagoolOS starts again when it quits (sloppy_main). ---- */
static const char *arcade_path, *arcade_arg;   /* chosen, not yet confirmed */
static void arcade_cancel_cb(lv_event_t *e) { (void)e; page_pop(); }
static void arcade_go_cb(lv_event_t *e)
{
    (void)e;
#ifndef SLOPPY_STRESS
    want_plugin = arcade_path;
    want_plugin_arg = arcade_arg;
    want_classic = true;
#endif
}
static void arcade_confirm_page(const char *name, const char *path, const char *arg)
{
    static char title[96], arg_buf[MAX_PATH];
    arcade_path = path;
    if (arg) { strlcpy(arg_buf, arg, sizeof arg_buf); arcade_arg = arg_buf; }
    else arcade_arg = NULL;
    snprintf(title, sizeof title, sl_tr("Play %s?"), sl_tr(name));
    lv_obj_t *p, *body = page_new(title, &p);
    section(body, "A CLASSIC ROCKBOX PROGRAM");
    lv_obj_t *info = text(body,
        "It runs as a classic Rockbox program: touch the screen as a 3 x 3 pad, Power opens its menu. "
        "When you quit it, GabagoolOS comes back.",
        &ss_regular_18, C_SUB);
    lv_obj_set_width(info, lv_pct(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_t *cancel = card_row(body, "Stay in GabagoolOS", "Keep using this interface", ACT_SOON);
    lv_obj_remove_event_cb_with_user_data(cancel, card_row_cb, (void *)(intptr_t)ACT_SOON);
    on_click(cancel, arcade_cancel_cb, NULL);
    lv_obj_t *go = card_row(body, name, "Start it now", ACT_SOON);
    lv_obj_remove_event_cb_with_user_data(go, card_row_cb, (void *)(intptr_t)ACT_SOON);
    on_click(go, arcade_go_cb, NULL);
    page_push(p);
}
static void arcade_missing_page(const char *name, const char *how, const char *where)
{
    lv_obj_t *p, *body = page_new(name, &p);
    section(body, "ONE FILE MISSING");
    lv_obj_t *info = text(body, how, &ss_regular_18, C_SUB);
    lv_obj_set_width(info, lv_pct(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_t *path = text(body, where, &ss_regular_18, C_TEXT);
    lv_obj_set_width(path, lv_pct(100));
    lv_label_set_long_mode(path, LV_LABEL_LONG_WRAP);
    page_push(p);
}
static void doom_open(void)
{
    if (!file_exists(ARC_DOOM_WAD))
    {
        arcade_missing_page("Doom", "Doom needs its game data. Put the shareware doom1.wad on the card at this path, then come back.",
                            "Card: " ARC_DOOM_WAD);
        return;
    }
    arcade_confirm_page("Doom", PLUGIN_GAMES_DIR "/doom.rock", NULL);
}
