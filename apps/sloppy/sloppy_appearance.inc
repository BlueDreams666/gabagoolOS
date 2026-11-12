/* Persistent GabagoolOS colors, wallpapers, and Settings icon previews. */
#include <math.h>
#include "wallpapers/orion.inc"
#include "wallpapers/pillars.inc"
#include "wallpapers/red_roots.inc"
#include "wallpapers/nexus_background.inc"
static lv_obj_t *home_settings_icon;
static bool home_icon_small;              /* settings glyph on a small metro tile */
#define ACCENT_WHEEL_SIZE 224
static lv_color32_t accent_wheel_pixels[ACCENT_WHEEL_SIZE * ACCENT_WHEEL_SIZE];
static lv_obj_t *accent_marker, *accent_preview, *accent_preview_label, *accent_brightness;
static lv_obj_t *accent_shade, *accent_hex, *accent_was, *accent_was_label;
/* Teal is the GabagoolOS default; the rest are one tap away from the wheel. */
static const uint32_t accent_presets[] = { 0x72d8cd, 0x5aa9ff, 0xa78bfa, 0xff6fb5,
                                           0xf36f73, 0xff9f43, 0xf1ce62, 0x7bd88f };
static lv_obj_t *accent_swatches[ARRAYLEN(accent_presets)];
static lv_color_hsv_t accent_pick;
static bool appearance_reload_pending;
static bool appearance_reload_settings_only;
static lv_timer_t *appearance_reload_timer;
static lv_obj_t *wallpaper_nexus_preview;
static struct { int x, y; uint32_t start; } wallpaper_nexus_bursts[4];
static unsigned wallpaper_nexus_next_burst;
static void home_page(void);
static void system_page(void);
static void mini_retheme(void);
static void statusbar_retheme(void);
static void now_playing_retheme(void);
static void quick_panel_rebuild(void);
static void style_slider(lv_obj_t *s);
static void style_switch(lv_obj_t *sw);
static lv_obj_t *card_row(lv_obj_t *parent, const char *t1, const char *t2, intptr_t ud);
static lv_obj_t *set_group(lv_obj_t *body, const char *label);
static lv_obj_t *set_row(lv_obj_t *g, const char *sym, uint32_t tint,
                         const char *t1, const char *t2, intptr_t ud);
static void appearance_page(void);

/* Your picture (id 9): one made from a screenshot or a picture in Files
 * ("Set as wallpaper"), cut to the screen and kept on the card as raw
 * RGB565 (sl_wall_start), so it loads in one read, home and lock alike.
 * In memory only while one of them shows it. */
#define WALL_CUSTOM 9
#define WALL_CUSTOM_FILE ROCKBOX_DIR "/sloppy/wallpaper.rgb565"
static uint16_t *wall_custom_px;
static uint16_t wall_custom_small[60 * 100];
static lv_image_dsc_t wall_custom_dsc[2];
static bool wall_custom_failed;
static bool wallpaper_custom_load(void)
{
    if (wall_custom_px) return true;
    if (wall_custom_failed) return false;
    size_t size = (size_t)LCD_WIDTH * LCD_HEIGHT * 2, got = 0;
    int fd = open(WALL_CUSTOM_FILE, O_RDONLY);
    uint16_t *px = fd >= 0 ? malloc(size) : NULL;
    while (px && got < size)
    {
        ssize_t r = read(fd, (char *)px + got, size - got < 65536 ? size - got : 65536);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (fd >= 0) close(fd);
    if (!px || got != size)
    {
        free(px);
        wall_custom_failed = true;           /* until a new one is made */
        sl_log("wallpaper: your picture could not be read");
        return false;
    }
    wall_custom_px = px;
    /* the picker's sample: an 8 x 8 average of each block */
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 60; x++)
        {
            unsigned r = 0, g = 0, b = 0;
            for (int yy = 0; yy < 8; yy++)
                for (int xx = 0; xx < 8; xx++)
                {
                    unsigned c = px[(y * 8 + yy) * LCD_WIDTH + x * 8 + xx];
                    r += c >> 11; g += (c >> 5) & 63; b += c & 31;
                }
            wall_custom_small[y * 60 + x] = (uint16_t)((r / 64) << 11 | (g / 64) << 5 | (b / 64));
        }
    for (int i = 0; i < 2; i++)
    {
        lv_image_dsc_t *d = &wall_custom_dsc[i];
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = i ? 60 : LCD_WIDTH;
        d->header.h = i ? 100 : LCD_HEIGHT;
        d->header.stride = d->header.w * 2;
        d->data = (const uint8_t *)(i ? wall_custom_small : px);
        d->data_size = d->header.stride * d->header.h;
    }
    return true;
}
static void wallpaper_custom_free(void)
{
    free(wall_custom_px);
    wall_custom_px = NULL;
    memset(wall_custom_dsc, 0, sizeof wall_custom_dsc);
}
static int wallpaper_id(bool lock)
{
    int id = lock ? global_settings.sloppy_lock_wallpaper :
                    global_settings.sloppy_home_wallpaper;
    if (id == WALL_CUSTOM && !wallpaper_custom_load()) return 0;   /* gone from the card: plain */
    return id >= 0 && id <= WALL_CUSTOM ? id : 0;
}

static lv_image_dsc_t wallpaper_images[4][2];

static const lv_image_dsc_t *wallpaper_image(int id, bool preview)
{
    static const uint16_t *const pixels[4][2] = {
        { wallpaper_orion, wallpaper_orion_preview },
        { wallpaper_pillars, wallpaper_pillars_preview },
        { wallpaper_red_roots, wallpaper_red_roots_preview },
        { wallpaper_nexus_background, wallpaper_nexus_background_preview }
    };
    int index = id - 5, small = preview ? 1 : 0;
    lv_image_dsc_t *img = &wallpaper_images[index][small];
    if (!img->data)
    {
        img->header.magic = LV_IMAGE_HEADER_MAGIC;
        img->header.cf = LV_COLOR_FORMAT_RGB565;
        img->header.w = preview ? 60 : 480;
        img->header.h = preview ? 100 : 800;
        img->header.stride = img->header.w * 2;
        img->data = (const uint8_t *)pixels[index][small];
        img->data_size = (uint32_t)img->header.w * img->header.h * 2;
    }
    return img;
}

static void wallpaper_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                           uint32_t color, uint8_t opacity, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opacity;
    d.radius = radius;
    lv_area_t area = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &area);
}

static void wallpaper_line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                           uint32_t color, uint8_t opacity, int width)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1; d.p1.y = y1;
    d.p2.x = x2; d.p2.y = y2;
    d.color = lv_color_hex(color);
    d.opa = opacity;
    d.width = width;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

/* Drawing into a picture of our own, outside a screen redraw. */
static void wallpaper_layer(lv_layer_t *layer, lv_draw_buf_t *buf)
{
    lv_area_t all = { 0, 0, buf->header.w - 1, buf->header.h - 1 };
    lv_memzero(layer, sizeof *layer);
    layer->draw_buf = buf;
    layer->color_format = buf->header.cf;
    layer->buf_area = layer->_clip_area = layer->phy_clip_area = all;
}
static void wallpaper_layer_finish(lv_layer_t *layer)
{
    while (layer->draw_task_head)
    {
        lv_draw_dispatch_wait_for_request();
        if (!lv_draw_dispatch_layer(lv_display_get_default(), layer))
        {
            lv_draw_wait_for_finish();
            lv_draw_dispatch_request();
        }
    }
}

static uint32_t wallpaper_hash(uint32_t n)
{
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    return n ^ (n >> 16);
}

static void wallpaper_nexus_tap_at(int x, int y)
{
    unsigned slot = wallpaper_nexus_next_burst++ % ARRAYLEN(wallpaper_nexus_bursts);
    wallpaper_nexus_bursts[slot].x = x;
    wallpaper_nexus_bursts[slot].y = y;
    wallpaper_nexus_bursts[slot].start = lv_tick_get();
}

static void wallpaper_nexus_tap_cb(lv_event_t *e)
{
    if (wallpaper_id(false) != 8) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t point;
    lv_area_t area;
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(lv_event_get_current_target(e), &area);
    wallpaper_nexus_tap_at(point.x - area.x1, point.y - area.y1);
}

/* AOSP Nexus: four colored pixel streams crossing a dark pyramid grid. */
#define NEXUS_PULSES(preview)   ((preview) ? 8u : 12u)
#define NEXUS_TRAIL             19
#define NEXUS_BURST_MS          1900u
/* Every Nexus draw uses the frame time set by wallpaper_tick, so the tick
 * knows exactly which pixels the previous and next frames cover. */
static uint32_t wallpaper_nexus_now;
static const uint32_t wallpaper_nexus_colors[] = { 0xff3434, 0x30d568, 0x459dff, 0xffd63d };

struct nexus_pulse { int x, y, dir, cell, thick, glow; uint32_t color, cycle; };

static bool wallpaper_nexus_pulse(unsigned i, uint32_t now, int w, int h, bool preview,
                                  struct nexus_pulse *p)
{
    uint32_t clock = now + i * 617u;
    uint32_t phase = clock % 6500u;
    if (phase < 350u) return false;
    p->cycle = clock / 6500u;
    p->cell = preview ? 2 : 14;
    p->thick = preview ? 1 : 5;
    p->glow = preview ? 5 : 33;
    uint32_t seed = wallpaper_hash(i * 79u + p->cycle * 131u + 17u);
    p->dir = (int)(seed & 3u);
    /* A page not laid out yet (right after a reload) measures 0x0: no lanes,
     * and a modulo by zero would be SIGFPE on the R1. */
    int lanes = (p->dir & 1) ? w / p->cell : h / p->cell;
    if (lanes <= 0) return false;
    int lane = (int)((seed >> 4) % (unsigned)lanes) * p->cell;
    float speed = 0.16f + (float)((seed >> 15) & 7u) * 0.012f;
    int travel = (int)((phase - 350u) * speed * (preview ? 0.125f : 1.0f));
    p->x = (p->dir & 1) ? (p->dir == 1 ? travel : w - travel) : lane;
    p->y = (p->dir & 1) ? lane : (p->dir == 0 ? travel : h - travel);
    p->color = wallpaper_nexus_colors[(seed >> 11) & 3u];
    return true;
}

/* The pixels one pulse touches, relative to the wallpaper: its trail and its glow. */
static void wallpaper_nexus_pulse_areas(const struct nexus_pulse *p, lv_area_t *trail,
                                        lv_area_t *glow)
{
    int back = NEXUS_TRAIL * p->cell, pad = 2;
    if (p->dir & 1)
    {
        trail->x1 = p->dir == 1 ? p->x - back : p->x;
        trail->x2 = (p->dir == 1 ? p->x : p->x + back) + p->cell - 1;
        trail->y1 = p->y;
        trail->y2 = p->y + p->thick;
    }
    else
    {
        trail->y1 = p->dir == 0 ? p->y - back : p->y;
        trail->y2 = (p->dir == 0 ? p->y : p->y + back) + p->cell - 1;
        trail->x1 = p->x;
        trail->x2 = p->x + p->thick;
    }
    lv_area_increase(trail, pad, pad);
    *glow = (lv_area_t){ p->x - p->glow - pad, p->y - p->glow - pad,
                         p->x + p->glow + pad, p->y + p->glow + pad };
}

/* Each pulse on the home and lock screens is always the same few shapes:
 * a trail of 20 fading cells, then a soft glow, a halo and the head. Drawn
 * shape by shape that is 23 blends per pulse, every frame, which the tiles
 * scrolling over the wallpaper made the costliest part of the screen. So
 * each colour's trail (one per direction) and head are drawn once into
 * small see-through pictures, and a pulse is two of those: the same pixels
 * for a fraction of the work. About 180 KB, only while Nexus is in use. */
static lv_draw_buf_t *nexus_trail[4][4], *nexus_head[4];   /* [colour][dir], [colour] */
static bool nexus_sprites_ok;
#define NEXUS_CELL  14
#define NEXUS_THICK 5
#define NEXUS_GLOW  33

static void wallpaper_nexus_sprites_free(void)
{
    for (int k = 0; k < 4; k++)
    {
        for (int d = 0; d < 4; d++) { lv_draw_buf_destroy(nexus_trail[k][d]); nexus_trail[k][d] = NULL; }
        lv_draw_buf_destroy(nexus_head[k]); nexus_head[k] = NULL;
    }
    nexus_sprites_ok = false;
}

/* Where a trail picture's top-left sits from the pulse's head. */
static void wallpaper_nexus_trail_origin(int dir, int *x, int *y)
{
    *x = dir == 1 ? -NEXUS_TRAIL * NEXUS_CELL : 0;
    *y = dir == 0 ? -NEXUS_TRAIL * NEXUS_CELL : 0;
}

static void wallpaper_nexus_sprites(void)
{
    if (nexus_sprites_ok) return;
    const int len = (NEXUS_TRAIL + 1) * NEXUS_CELL;
    for (int k = 0; k < 4; k++)
    {
        uint32_t color = wallpaper_nexus_colors[k];
        for (int d = 0; d < 4; d++)
        {
            bool hor = d & 1;
            lv_draw_buf_t *b = lv_draw_buf_create(hor ? len : NEXUS_THICK + 1, hor ? NEXUS_THICK + 1 : len,
                                                  LV_COLOR_FORMAT_ARGB8888, 0);
            if (!b) { wallpaper_nexus_sprites_free(); return; }
            nexus_trail[k][d] = b;
            lv_draw_buf_clear(b, NULL);
            lv_layer_t layer;
            wallpaper_layer(&layer, b);
            int ox, oy;
            wallpaper_nexus_trail_origin(d, &ox, &oy);
            /* as wallpaper_nexus_pulses draws them, from the head at 0,0 */
            for (int trail = NEXUS_TRAIL; trail >= 0; trail--)
            {
                int px = hor ? (d == 1 ? -trail : trail) * NEXUS_CELL : 0;
                int py = hor ? 0 : (d == 0 ? -trail : trail) * NEXUS_CELL;
                wallpaper_rect(&layer, px - ox, py - oy,
                               px - ox + (hor ? NEXUS_CELL - 1 : NEXUS_THICK),
                               py - oy + (hor ? NEXUS_THICK : NEXUS_CELL - 1),
                               color, (uint8_t)(24 + (NEXUS_TRAIL - trail) * 8), 0);
            }
            wallpaper_layer_finish(&layer);
        }
        const int g = NEXUS_GLOW;
        lv_draw_buf_t *b = lv_draw_buf_create(2 * g + 1, 2 * g + 1, LV_COLOR_FORMAT_ARGB8888, 0);
        if (!b) { wallpaper_nexus_sprites_free(); return; }
        nexus_head[k] = b;
        lv_draw_buf_clear(b, NULL);
        lv_layer_t layer;
        wallpaper_layer(&layer, b);
        wallpaper_rect(&layer, 0, 0, 2 * g, 2 * g, color, LV_OPA_10, g);
        wallpaper_rect(&layer, g - NEXUS_CELL, g - NEXUS_CELL, g + NEXUS_CELL, g + NEXUS_CELL,
                       color, LV_OPA_30, NEXUS_CELL);
        wallpaper_rect(&layer, g, g, g + NEXUS_CELL - 1, g + NEXUS_CELL - 1, color, LV_OPA_COVER, 3);
        wallpaper_layer_finish(&layer);
    }
    nexus_sprites_ok = true;
}

static void wallpaper_sprite(lv_layer_t *layer, const lv_draw_buf_t *b, int x, int y)
{
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = b;
    lv_area_t at = { x, y, x + b->header.w - 1, y + b->header.h - 1 };
    lv_draw_image(layer, &d, &at);
}

static void wallpaper_nexus_pulses(lv_layer_t *layer, const lv_area_t *a, bool preview)
{
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    int cell = preview ? 2 : 14;
    uint32_t now = wallpaper_nexus_now;
    for (unsigned i = 0; i < NEXUS_PULSES(preview); i++)
    {
        struct nexus_pulse p;
        if (!wallpaper_nexus_pulse(i, now, w, h, preview, &p)) continue;
        if (!preview && nexus_sprites_ok)
        {
            int k = 0, ox, oy;
            while (wallpaper_nexus_colors[k] != p.color) k++;
            wallpaper_nexus_trail_origin(p.dir, &ox, &oy);
            wallpaper_sprite(layer, nexus_trail[k][p.dir], a->x1 + p.x + ox, a->y1 + p.y + oy);
            wallpaper_sprite(layer, nexus_head[k], a->x1 + p.x - NEXUS_GLOW, a->y1 + p.y - NEXUS_GLOW);
            continue;
        }
        for (int trail = NEXUS_TRAIL; trail >= 0; trail--)
        {
            int px = p.x + ((p.dir & 1) ? (p.dir == 1 ? -trail : trail) * cell : 0);
            int py = p.y + (!(p.dir & 1) ? (p.dir == 0 ? -trail : trail) * cell : 0);
            if (px < -cell || py < -cell || px >= w || py >= h) continue;
            uint8_t opa = (uint8_t)(24 + (NEXUS_TRAIL - trail) * 8);
            wallpaper_rect(layer, a->x1 + px, a->y1 + py,
                a->x1 + px + ((p.dir & 1) ? cell - 1 : p.thick),
                a->y1 + py + ((p.dir & 1) ? p.thick : cell - 1), p.color, opa, 0);
        }
        wallpaper_rect(layer, a->x1 + p.x - p.glow, a->y1 + p.y - p.glow,
                       a->x1 + p.x + p.glow, a->y1 + p.y + p.glow,
                       p.color, LV_OPA_10, p.glow);
        wallpaper_rect(layer, a->x1 + p.x - cell, a->y1 + p.y - cell,
                       a->x1 + p.x + cell, a->y1 + p.y + cell,
                       p.color, LV_OPA_30, cell);
        wallpaper_rect(layer, a->x1 + p.x, a->y1 + p.y,
                       a->x1 + p.x + cell - 1, a->y1 + p.y + cell - 1,
                       p.color, LV_OPA_COVER, preview ? 1 : 3);
    }
    if (preview) return;
    for (unsigned i = 0; i < ARRAYLEN(wallpaper_nexus_bursts); i++)
    {
        uint32_t age = now - wallpaper_nexus_bursts[i].start;
        if (!wallpaper_nexus_bursts[i].start || age > NEXUS_BURST_MS) continue;
        for (int dir = 0; dir < 4; dir++)
        {
            int distance = (int)((float)age * 0.29f);
            for (int trail = 11; trail >= 0; trail--)
            {
                int offset = distance - trail * cell;
                if (offset < 0) continue;
                int x = wallpaper_nexus_bursts[i].x +
                        (dir == 0 ? offset : dir == 1 ? -offset : 0);
                int y = wallpaper_nexus_bursts[i].y +
                        (dir == 2 ? offset : dir == 3 ? -offset : 0);
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                uint8_t opa = (uint8_t)(55 + (11 - trail) * 15);
                wallpaper_rect(layer, a->x1 + x, a->y1 + y,
                    a->x1 + x + (dir < 2 ? cell : 4),
                    a->y1 + y + (dir < 2 ? 4 : cell),
                    wallpaper_nexus_colors[dir], opa, 1);
            }
        }
    }
}

static void wallpaper_draw_id(lv_layer_t *layer, const lv_area_t *area, int id, bool preview)
{
    lv_area_t a = *area;
    if (preview)
        wallpaper_rect(layer, a.x1, a.y1, a.x2, a.y2, C_BG, LV_OPA_COVER, 0);
    if (!id || lv_area_get_width(&a) <= 0 || lv_area_get_height(&a) <= 0) return;
    if (id == WALL_CUSTOM)
    {
        if (!wallpaper_custom_load()) return;
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &wall_custom_dsc[preview ? 1 : 0];
        lv_draw_image(layer, &d, &a);
        return;
    }
    if (id >= 5 && id <= 7)
    {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = wallpaper_image(id, preview);
        lv_draw_image(layer, &d, &a);
        return;
    }
    if (id == 8)
    {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = wallpaper_image(id, preview);
        lv_draw_image(layer, &d, &a);
        wallpaper_nexus_pulses(layer, &a, preview);
        return;
    }
    uint32_t accent = C_ACCENT;
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    if (id == 1) /* soft glow */
    {
        for (int i = 12; i >= 0; i--)
        {
            int radius = w * (17 + i * 4) / 100;
            int cx = a.x1 + w * 34 / 100, cy = a.y1 + h * 31 / 100;
            wallpaper_rect(layer, cx - radius, cy - radius, cx + radius, cy + radius,
                           accent, i < 3 ? 12 : 7, radius);
            radius = w * (12 + i * 3) / 100;
            cx = a.x1 + w * 77 / 100; cy = a.y1 + h * 69 / 100;
            wallpaper_rect(layer, cx - radius, cy - radius, cx + radius, cy + radius,
                           accent, 6, radius);
        }
        wallpaper_line(layer, a.x1 + w / 10, a.y1 + h * 71 / 100,
                       a.x1 + w * 85 / 100, a.y1 + h * 25 / 100,
                       accent, 65, preview ? 1 : 2);
        for (int i = 0; i < 9; i++)
        {
            int x = a.x1 + (int)(wallpaper_hash((uint32_t)i * 23u) % (unsigned)w);
            int y = a.y1 + (int)(wallpaper_hash((uint32_t)i * 59u) % (unsigned)h);
            wallpaper_rect(layer, x, y, x + (preview ? 1 : 3), y + (preview ? 1 : 3),
                           accent, 120, 2);
        }
    }
    else if (id == 2) /* illuminated technical grid */
    {
        int step = preview ? 8 : 48;
        for (int x = 0; x <= w; x += step)
            wallpaper_line(layer, a.x1 + x, a.y1, a.x1 + x, a.y2,
                           accent, x % (step * 4) ? 32 : 92, 1);
        for (int y = 0; y <= h; y += step)
            wallpaper_line(layer, a.x1, a.y1 + y, a.x2, a.y1 + y,
                           accent, y % (step * 4) ? 32 : 92, 1);
        for (int i = 0; i < 7; i++)
        {
            int x = a.x1 + (int)(wallpaper_hash((uint32_t)i + 91u) % (unsigned)w);
            int y = a.y1 + (int)(wallpaper_hash((uint32_t)i + 43u) % (unsigned)h);
            int r = preview ? 1 : 5;
            wallpaper_rect(layer, x - r, y - r, x + r, y + r, accent, 155, r);
            wallpaper_line(layer, x - step / 2, y, x + step / 2, y, accent, 62, 1);
            wallpaper_line(layer, x, y - step / 2, x, y + step / 2, accent, 62, 1);
        }
        wallpaper_line(layer, a.x1 + w / 8, a.y1 + h * 7 / 8,
                       a.x1 + w * 7 / 8, a.y1 + h / 8,
                       accent, 92, preview ? 1 : 2);
    }
    else if (id == 3) /* connected constellations */
    {
        static const uint8_t points[][2] = {
            { 12, 13 }, { 25, 19 }, { 35, 12 }, { 42, 26 }, { 29, 33 },
            { 62, 35 }, { 77, 28 }, { 88, 39 }, { 76, 51 }, { 59, 48 },
            { 14, 61 }, { 29, 56 }, { 37, 69 }, { 23, 80 }, { 45, 84 }
        };
        static const uint8_t edges[][2] = {
            { 0, 1 }, { 1, 2 }, { 1, 4 }, { 2, 3 }, { 3, 4 },
            { 5, 6 }, { 6, 7 }, { 7, 8 }, { 8, 9 }, { 9, 5 },
            { 10, 11 }, { 11, 12 }, { 12, 13 }, { 13, 14 }
        };
        for (unsigned i = 0; i < 90; i++)
        {
            int x = (int)(wallpaper_hash(i * 31u + 3u) % (unsigned)w);
            int y = (int)(wallpaper_hash(i * 43u + 7u) % (unsigned)h);
            int size = preview ? 1 : (i % 9 == 0 ? 3 : 1);
            wallpaper_rect(layer, a.x1 + x, a.y1 + y, a.x1 + x + size,
                           a.y1 + y + size, accent, i % 9 == 0 ? 175 : 95, size);
        }
        for (unsigned i = 0; i < ARRAYLEN(edges); i++)
        {
            int from = edges[i][0], to = edges[i][1];
            wallpaper_line(layer,
                a.x1 + points[from][0] * w / 100, a.y1 + points[from][1] * h / 100,
                a.x1 + points[to][0] * w / 100, a.y1 + points[to][1] * h / 100,
                accent, 105, 1);
        }
        for (unsigned i = 0; i < ARRAYLEN(points); i++)
        {
            int x = a.x1 + points[i][0] * w / 100;
            int y = a.y1 + points[i][1] * h / 100;
            int size = preview ? 1 : 4;
            wallpaper_rect(layer, x - size, y - size, x + size, y + size,
                           accent, 52, size);
            wallpaper_rect(layer, x - size / 2, y - size / 2,
                           x + size / 2, y + size / 2, C_TEXT, LV_OPA_COVER, size);
        }
    }
    else if (id == 4) /* flowing light ribbons */
    {
        for (int band = 0; band < 6; band++)
        {
            int last_x = a.x1, last_y = a.y1 + h * (band + 2) / 10;
            for (int step = 1; step <= 24; step++)
            {
                int x = a.x1 + step * w / 24;
                float phase = (float)step * 0.43f + (float)band * 0.58f;
                int y = a.y1 + h * (band + 2) / 10 +
                        (int)(sinf(phase) * (float)h * 0.065f);
                wallpaper_line(layer, last_x, last_y, x, y, accent,
                               band % 2 ? 112 : 73, preview ? 1 : (band % 2 ? 3 : 2));
                last_x = x; last_y = y;
            }
        }
        wallpaper_rect(layer, a.x1 + w / 8, a.y1 + h / 3,
                       a.x1 + w * 7 / 8, a.y1 + h / 3 + (preview ? 2 : 10),
                       accent, 25, preview ? 1 : 5);
    }
}

/* The drawn wallpapers (1-4) are dozens of soft shapes: the glow alone
 * blends 26 large circles, ten times a photo's cost, and scrolling tiles
 * repaint the whole screen every frame. They never move, so each is drawn
 * once into a picture, home and lock screen one each, and then copied like
 * a photo. The picture is made outside a redraw, right after the first
 * frame that needs it, which draws the shapes itself. */
static struct wallpaper_cache
{
    lv_draw_buf_t *buf;
    int id, w, h;
    uint32_t accent, bg;
    bool pending;
} wallpaper_caches[2];

static bool wallpaper_cache_fits(const struct wallpaper_cache *c, int id, int w, int h, uint32_t bg)
{
    return c->buf && c->id == id && c->w == w && c->h == h && c->accent == C_ACCENT && c->bg == bg;
}

static void wallpaper_cache_build(void *which)
{
    struct wallpaper_cache *c = &wallpaper_caches[(intptr_t)which];
    c->pending = false;
    if (wallpaper_cache_fits(c, c->id, c->w, c->h, c->bg)) return;
    if (c->buf && (c->buf->header.w != c->w || c->buf->header.h != c->h))
    {
        lv_draw_buf_destroy(c->buf);
        c->buf = NULL;
    }
    if (!c->buf) c->buf = lv_draw_buf_create(c->w, c->h, LV_COLOR_FORMAT_RGB565, 0);
    if (!c->buf) return;                  /* no memory: keep drawing the shapes */
    lv_layer_t layer;
    lv_area_t all = { 0, 0, c->w - 1, c->h - 1 };
    wallpaper_layer(&layer, c->buf);
    wallpaper_rect(&layer, 0, 0, c->w - 1, c->h - 1, c->bg, LV_OPA_COVER, 0);
    wallpaper_draw_id(&layer, &all, c->id, false);
    wallpaper_layer_finish(&layer);
    c->accent = C_ACCENT;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
}

static void sl_wallpaper_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    bool lock = (intptr_t)lv_event_get_user_data(e) != 0;
    int id = wallpaper_id(lock);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    if (id >= 1 && id <= 4)
    {
        struct wallpaper_cache *c = &wallpaper_caches[lock];
        int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
        uint32_t bg = lv_color_to_u32(lv_obj_get_style_bg_color(o, 0)) & 0xffffff;
        if (wallpaper_cache_fits(c, id, w, h, bg))
        {
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = c->buf;
            lv_draw_image(lv_event_get_layer(e), &d, &a);
            return;
        }
        if (!c->pending && w > 0 && h > 0)
        {
            c->id = id; c->w = w; c->h = h; c->bg = bg;
            c->pending = true;
            lv_async_call(wallpaper_cache_build, (void *)(intptr_t)lock);
        }
    }
    wallpaper_draw_id(lv_event_get_layer(e), &a, id, false);
}

static void wallpaper_preview_draw(lv_event_t *e)
{
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target(e), &a);
    wallpaper_draw_id(lv_event_get_layer(e), &a,
                      (int)(intptr_t)lv_event_get_user_data(e), true);
}

static void wallpaper_nexus_preview_deleted(lv_event_t *e)
{
    if (wallpaper_nexus_preview == lv_event_get_current_target(e))
        wallpaper_nexus_preview = NULL;
}

static void wallpaper_area_join(lv_area_t *into, const lv_area_t *other)
{
    into->x1 = LV_MIN(into->x1, other->x1); into->y1 = LV_MIN(into->y1, other->y1);
    into->x2 = LV_MAX(into->x2, other->x2); into->y2 = LV_MAX(into->y2, other->y2);
}

static bool wallpaper_nexus_add(lv_area_t *areas, unsigned *n, unsigned max,
                                lv_area_t r, int w, int h)
{
    r.x1 = LV_MAX(r.x1, 0); r.y1 = LV_MAX(r.y1, 0);
    r.x2 = LV_MIN(r.x2, w - 1); r.y2 = LV_MIN(r.y2, h - 1);
    if (r.x1 > r.x2 || r.y1 > r.y2) return true;
    if (*n >= max) return false;
    areas[(*n)++] = r;
    return true;
}

/* Redraw only the strips the pulses leave and enter. Invalidating the whole
 * page would re-blit the 480x800 background and every tile on each frame. */
static void wallpaper_nexus_invalidate(lv_obj_t *obj, uint32_t before, uint32_t now)
{
    lv_area_t a, areas[24];
    unsigned n = 0, max = ARRAYLEN(areas);
    bool fits = true;
    lv_obj_get_coords(obj, &a);
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    for (unsigned i = 0; i < NEXUS_PULSES(false) && fits; i++)
    {
        struct nexus_pulse was, is;
        lv_area_t t0, g0, t1, g1;
        bool had = wallpaper_nexus_pulse(i, before, w, h, false, &was);
        bool has = wallpaper_nexus_pulse(i, now, w, h, false, &is);
        if (had) wallpaper_nexus_pulse_areas(&was, &t0, &g0);
        if (has) wallpaper_nexus_pulse_areas(&is, &t1, &g1);
        if (had && has && was.cycle == is.cycle)
        {
            wallpaper_area_join(&t1, &t0);
            wallpaper_area_join(&g1, &g0);
            had = false;
        }
        if (had)
            fits = wallpaper_nexus_add(areas, &n, max, t0, w, h) &&
                   wallpaper_nexus_add(areas, &n, max, g0, w, h);
        if (has && fits)
            fits = wallpaper_nexus_add(areas, &n, max, t1, w, h) &&
                   wallpaper_nexus_add(areas, &n, max, g1, w, h);
    }
    for (unsigned i = 0; i < ARRAYLEN(wallpaper_nexus_bursts) && fits; i++)
    {
        uint32_t start = wallpaper_nexus_bursts[i].start;
        if (!start || (before - start > NEXUS_BURST_MS && now - start > NEXUS_BURST_MS))
            continue;
        int x = wallpaper_nexus_bursts[i].x, y = wallpaper_nexus_bursts[i].y;
        fits = wallpaper_nexus_add(areas, &n, max, (lv_area_t){ 0, y - 2, w - 1, y + 6 }, w, h) &&
               wallpaper_nexus_add(areas, &n, max, (lv_area_t){ x - 2, 0, x + 6, h - 1 }, w, h);
    }
    if (!fits)
    {
        lv_obj_invalidate(obj);
        return;
    }
    for (unsigned i = 0; i < n; i++)
    {
        lv_area_move(&areas[i], a.x1, a.y1);
        lv_obj_invalidate_area(obj, &areas[i]);
    }
}

static void wallpaper_tick(lv_timer_t *timer)
{
    /* 30 fps only while the live wallpaper shows; otherwise look a few
     * times a second, rather than waking the player 30 times for nothing */
    bool live = false;
    if (wallpaper_screen_dark) { if (timer) lv_timer_set_period(timer, 1000); return; }
    uint32_t before = wallpaper_nexus_now;
    wallpaper_nexus_now = lv_tick_get();
    if (wallpaper_id(false) == 8 || wallpaper_id(true) == 8) wallpaper_nexus_sprites();
    else if (nexus_sprites_ok) wallpaper_nexus_sprites_free();
    if (!qp_is_visible() && wallpaper_id(false) == 8 && npages > 0 &&
        !lv_obj_has_flag(pages[0], LV_OBJ_FLAG_HIDDEN))
    { wallpaper_nexus_invalidate(pages[0], before, wallpaper_nexus_now); live = true; }
    if (wallpaper_id(true) == 8 && wallpaper_lock_obj &&
        !lv_obj_has_flag(wallpaper_lock_obj, LV_OBJ_FLAG_HIDDEN))
    { wallpaper_nexus_invalidate(wallpaper_lock_obj, before, wallpaper_nexus_now); live = true; }
    if (wallpaper_nexus_preview) live = true;
    if (timer) lv_timer_set_period(timer, live ? 33 : 250);
    if (wallpaper_nexus_preview)
    {
        lv_obj_t *parent = wallpaper_nexus_preview;
        while (parent && parent != lv_screen_active() &&
               !lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN))
            parent = lv_obj_get_parent(parent);
        if (parent == lv_screen_active())
            lv_obj_invalidate(wallpaper_nexus_preview);
    }
}

static bool cf_is_open(void);
static void cf_close(void);
static void appearance_reload(void *settings_only)
{
    sl_log("customization reload: begin (pages %d, style %d, wallpaper %d)",
           npages, global_settings.sloppy_home_style, global_settings.sloppy_home_wallpaper);
    sl_log_flush();
    appearance_reload_pending = false;
    if (cf_is_open()) cf_close();          /* the pages live on the main screen */
    for (int i = npages - 1; i >= 0; i--)
    {
        lv_anim_delete(pages[i], NULL);
        lv_obj_delete(pages[i]);
    }
    npages = 0;
    home_settings_icon = NULL;
    styles_retheme();
    mini_retheme();
    statusbar_retheme();
    now_playing_retheme();
    quick_panel_rebuild();
    home_page();
    if ((intptr_t)settings_only != 2) system_page();   /* 2: from the control centre, home alone */
    if (!settings_only) appearance_page();
    if (mini) lv_obj_move_foreground(mini);
    sl_log("customization reload: complete (pages %d)", npages);
    sl_log_flush();
}

static void appearance_reload_cb(lv_timer_t *timer)
{
    int pos;
    if (button_status_wdata(&pos) & BUTTON_TOUCHSCREEN) return;
    lv_timer_pause(timer);
    appearance_reload((void *)(intptr_t)appearance_reload_settings_only);
}

static void appearance_reload_soon(bool settings_only)
{
    appearance_reload_settings_only = settings_only;
    appearance_reload_pending = true;
    if (!appearance_reload_timer)
        appearance_reload_timer = lv_timer_create(appearance_reload_cb, 40, NULL);
    lv_timer_reset(appearance_reload_timer);
    lv_timer_resume(appearance_reload_timer);
}

static void appearance_changed(void)
{
    if (settings_save() != 0) toast_warn("Could not save customization settings");
    appearance_reload_soon(false);
}

static void skull_polygon(lv_layer_t *layer, const lv_area_t *a,
                          const int8_t (*points)[2], int n, uint32_t color)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = LV_OPA_COVER;
    for (int i = 1; i < n - 1; i++)
    {
        int indexes[] = { 0, i, i + 1 };
        for (int j = 0; j < 3; j++)
        {
            d.p[j].x = a->x1 + points[indexes[j]][0];
            d.p[j].y = a->y1 + points[indexes[j]][1];
        }
        lv_draw_triangle(layer, &d);
    }
}

static void skull_draw(lv_event_t *e)
{
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target(e), &a);
    lv_layer_t *layer = lv_event_get_layer(e);
    static const int8_t crown[][2] = {
        {16,0},{48,0},{61,13},{64,36},{54,47},{10,47},{0,36},{3,13}
    };
    static const int8_t jaw[][2] = {{13,38},{51,38},{47,62},{17,62}};
    static const int8_t left_eye[][2] = {{9,22},{28,27},{25,39},{12,36}};
    static const int8_t right_eye[][2] = {{55,22},{52,36},{39,39},{36,27}};
    static const int8_t nose[][2] = {{32,36},{25,47},{39,47}};
    static const int8_t rim_left[][2] = {{3,13},{16,0},{11,20},{0,36}};
    static const int8_t rim_right[][2] = {{48,0},{61,13},{64,36},{53,20}};
    skull_polygon(layer, &a, crown, 8, 0xffffff);
    skull_polygon(layer, &a, jaw, 4, 0xffffff);
    skull_polygon(layer, &a, rim_left, 4, 0xc5cfdd);
    skull_polygon(layer, &a, rim_right, 4, 0xc5cfdd);
    skull_polygon(layer, &a, left_eye, 4, 0x53617a);
    skull_polygon(layer, &a, right_eye, 4, 0x53617a);
    skull_polygon(layer, &a, nose, 3, 0x53617a);
    for (int x = 24; x <= 40; x += 8)
    {
        int8_t gap[][2] = {{x,52},{x+2,52},{x+2,64},{x,64}};
        skull_polygon(layer, &a, gap, 4, 0x53617a);
    }
}

static void home_settings_ink(lv_obj_t *glyph);   /* sloppy_metro.inc */
static void settings_icon_fill(lv_obj_t *parent, bool skull)
{
    lv_obj_clean(parent);
    if (!skull)
    {
        lv_obj_t *g = text(parent, LV_SYMBOL_SETTINGS, &sl_sym_48, 0xffffff);
        if (parent == home_settings_icon) home_settings_ink(g);   /* the home's own ink for its icons */
        lv_obj_center(g);
    }
    else
    {
        lv_obj_t *s = box(parent);
        lv_obj_set_size(s, 64, 64);
        lv_obj_center(s);
        lv_obj_add_event_cb(s, skull_draw, LV_EVENT_DRAW_MAIN, NULL);
    }
    if (parent == home_settings_icon && lv_obj_get_child_count(parent))
        lv_obj_set_style_transform_scale(lv_obj_get_child(parent, 0), home_icon_small ? 200 : 292, 0);
}

static void icon_choice_cb(lv_event_t *e)
{
    bool skull = (intptr_t)lv_event_get_user_data(e) != 0;
    global_settings.sloppy_skull_icon = skull;
    settings_save();
    if (home_settings_icon) settings_icon_fill(home_settings_icon, skull);
    /* the cards of the page that was tapped, found through it: a global
     * pointing at the last page built went stale when a second copy of the
     * page was opened and closed (a use after free) */
    set_check_mark(lv_event_get_current_target(e));
}

static void settings_icon_page(void)
{
    sl_log("settings icon loaded: %d", global_settings.sloppy_skull_icon);
    lv_obj_t *p, *body = page_new("Settings icon", &p);
    static const char *const names[] = { "Classic cog", "Angular skull" };
    static const char *const notes[] = { "The Settings gear, as it has always been", "Sharp lines and a grin" };
    lv_obj_t *g = set_group(body, "Icon");
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *r = set_pick(g, names[i], notes[i], global_settings.sloppy_skull_icon == (bool)i,
                               icon_choice_cb, (void *)(intptr_t)i);
        lv_obj_t *ic = box(lv_obj_get_child(r, 2));
        lv_obj_set_size(ic, SET_PIC_W, SET_PIC_W);
        lv_obj_center(ic);
        lv_obj_set_style_radius(ic, SL_R_ICON(SET_PIC_W), 0);   /* as lit_square rounds */
        lv_obj_set_style_bg_color(ic, lv_color_hex(0x53617a), 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        settings_icon_fill(ic, i != 0);
    }
    page_push(p);
}

static void theme_choice_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    if (choice == global_settings.sloppy_theme) return;   /* not sl_theme(): Zune reads as True dark */
    global_settings.sloppy_theme = choice;
    /* Zule is the whole look, so its list home comes with it, and leaving
     * it brings the tiles back; Home layout can still change either after */
    global_settings.sloppy_home_style = choice == 3 ? 2 : 1;
    appearance_changed();
}

/* A little screen in each theme's own colours: a card of rows on its
 * ground, the accent on the first; Zule's are big words on black. */
static void theme_sketch(lv_obj_t *pic, int th)
{
    static const uint32_t bg[] = { 0x0d1219, 0xf1f4f7, 0x000000, 0x000000 };
    static const uint32_t card[] = { 0x18202b, 0xffffff, 0x000000, 0x000000 };
    static const uint32_t ink[] = { 0xffffff, 0x182735, 0xffffff, 0xffffff };
    lv_obj_t *s = set_pic_screen(pic, SET_PIC_W, SET_PIC_H, bg[th]);
    uint32_t accent = th == 3 ? ZN_PINK : C_ACCENT;
    if (th == 3)
    {
        for (int i = 0; i < 4; i++)
        {
            lv_obj_t *w = box(s);
            lv_obj_set_size(w, i ? 40 - i * 6 : 52, i ? 7 : 12);
            lv_obj_set_pos(w, 8, i ? 26 + i * 16 : 10);
            lv_obj_set_style_radius(w, SL_R_MINI, 0);
            lv_obj_set_style_bg_color(w, lv_color_hex(i == 1 ? accent : ink[th]), 0);
            lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
        }
        return;
    }
    lv_obj_t *c = box(s);
    lv_obj_set_size(c, 58, 64);
    lv_obj_set_pos(c, 6, 20);
    lv_obj_set_style_radius(c, SL_R_XS, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(card[th]), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(th == 1 ? 0x182735 : 0xffffff), 0);
    lv_obj_set_style_border_opa(c, th == 2 ? 70 : 24, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *dot = box(c);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_pos(dot, 6, 6 + i * 19);
        lv_obj_set_style_radius(dot, SL_R_MINI, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(i ? 0x66738a : accent), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_t *ln = box(c);
        lv_obj_set_size(ln, i ? 26 : 32, 4);
        lv_obj_set_pos(ln, 21, 9 + i * 19);
        lv_obj_set_style_radius(ln, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(ln, lv_color_hex(ink[th]), 0);
        lv_obj_set_style_bg_opa(ln, i ? LV_OPA_60 : LV_OPA_COVER, 0);
    }
}

static void sl_corners_apply(void);
static lv_obj_t *opt_card(lv_obj_t *body, const char *title, const char *note);
static lv_obj_t *opt_switch(lv_obj_t *c, const char *label, bool on, void (*apply)(int value));
static void theme_corners_set(int on) { global_settings.sloppy_round_corners = on; sl_corners_apply(); }
static void theme_page(void)
{
    static const char *const names[] = { "Dark", "Light", "True dark", "Zule" };
    static const char *const notes[] = { "Deep blue GabagoolOS", "Bright cards and dark text",
                                         "True black surfaces for OLED",
                                         "Black, pink and big lowercase words" };
    lv_obj_t *p, *body = page_new("Theme", &p);
    lv_obj_t *g = set_group(body, "APPEARANCE");
    for (int i = 0; i < 4; i++)
        theme_sketch(lv_obj_get_child(set_pick(g, names[i], notes[i], i == global_settings.sloppy_theme,
                                               theme_choice_cb, (void *)(intptr_t)i), 2), i);
    lv_obj_t *c = opt_card(body, "Screen", "Rounds off the screen's square corners, as on a phone");
    opt_switch(c, "Rounded corners", global_settings.sloppy_round_corners == 1, theme_corners_set);
    page_push(p);
}

static void home_style_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    if (choice == global_settings.sloppy_home_style) return;
    global_settings.sloppy_home_style = choice;
    appearance_changed();
}

static lv_obj_t *opt_card(lv_obj_t *body, const char *title, const char *note);
static lv_obj_t *opt_switch(lv_obj_t *c, const char *label, bool on, void (*apply)(int value));
static lv_obj_t *opt_chips(lv_obj_t *c, const char *const *names, int n, int sel, void (*apply)(int value));
static void home_pull_refresh(int on) { global_settings.sloppy_pull_refresh = on; }

/* Each home in miniature: two rounded cards a row with their icons, live
 * tiles of three sizes, or big words down the screen. */
static void home_style_sketch(lv_obj_t *pic, int style)
{
    static const uint32_t tints[] = { 0x2d7ff0, 0xe8408a, 0x1fb39a, 0xe89a1c };
    lv_obj_t *s = set_pic_screen(pic, SET_PIC_W, SET_PIC_H, C_BG);
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *c = box(s);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        if (style == 0)
        {
            lv_obj_set_size(c, 28, 40);
            lv_obj_set_pos(c, 6 + i % 2 * 32, 8 + i / 2 * 46);
            lv_obj_set_style_radius(c, SL_R_XS, 0);
            lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
            lv_obj_set_style_border_color(c, lv_color_hex(C_SUB), 0);
            lv_obj_set_style_border_opa(c, 40, 0);
            lv_obj_set_style_border_width(c, 1, 0);
            lv_obj_t *ic = box(c);
            lv_obj_set_size(ic, 12, 12);
            lv_obj_set_pos(ic, 4, 4);
            lv_obj_set_style_radius(ic, SL_R_MINI, 0);
            lv_obj_set_style_bg_color(ic, lv_color_hex(tints[i]), 0);
            lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        }
        else if (style == 1)
        {
            static const uint8_t at[4][4] = { { 6, 8, 60, 28 }, { 6, 40, 28, 28 }, { 38, 40, 28, 28 }, { 6, 72, 60, 24 } };
            lv_obj_set_pos(c, at[i][0], at[i][1]);
            lv_obj_set_size(c, at[i][2], at[i][3]);
            lv_obj_set_style_bg_color(c, lv_color_hex(tints[i]), 0);
        }
        else
        {
            lv_obj_set_size(c, i ? 44 - i * 6 : 54, i ? 8 : 10);
            lv_obj_set_pos(c, 8, 12 + i * 22);
            lv_obj_set_style_radius(c, SL_R_MINI, 0);
            lv_obj_set_style_bg_color(c, lv_color_hex(i == 1 ? C_ACCENT : C_TEXT), 0);
        }
    }
}

static void home_style_page(void)
{
    static const char *const names[] = { "Cards", "Tiles", "List" };
    static const char *const notes[] = { "Rounded cards with big icons",
                                         "Live tiles; press and hold one to edit",
                                         "Big words in one long list" };
    lv_obj_t *p, *body = page_new("Home layout", &p);
    lv_obj_t *g = set_group(body, "Layout");
    for (int i = 0; i < 3; i++)
        home_style_sketch(lv_obj_get_child(set_pick(g, names[i], notes[i], i == global_settings.sloppy_home_style,
                                                    home_style_cb, (void *)(intptr_t)i), 2), i);
    /* off by default: a pull past the top only springs back */
    lv_obj_t *c = opt_card(body, "Pull to refresh", NULL);
    opt_switch(c, "Pull down to refresh", global_settings.sloppy_pull_refresh, home_pull_refresh);
    page_push(p);
}

static lv_obj_t *sounds_slider, *sounds_value;
static void sounds_value_show(int pct)
{
    char b[8];
    if (pct) snprintf(b, sizeof b, "%d%%", pct);
    lv_label_set_text(sounds_value, pct ? b : sl_tr("Off"));
}

/* Follow the finger; save and play a preview once it lets go. */
static void sounds_slider_cb(lv_event_t *e)
{
    int pct = (lv_slider_get_value(sounds_slider) + 2) / 5 * 5;   /* 5 % steps */
    lv_slider_set_value(sounds_slider, pct, LV_ANIM_OFF);
    global_settings.sloppy_sfx_volume = pct;
    sounds_value_show(pct);
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    sfx_play(SFX_WIN);                 /* a preview at the new level */
    if (settings_save() != 0) toast_warn("Could not save sound settings");
}

static lv_obj_t *sounds_ui_switch;
static void sounds_ui_cb(lv_event_t *e)
{
    global_settings.sloppy_ui_sounds = lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_CHECKED);
    if (global_settings.sloppy_ui_sounds) sfx_play(SFX_UI_ON);
    if (settings_save() != 0) toast_warn("Could not save sound settings");
}

static void lock_player_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    global_settings.sloppy_lock_player = choice;
    set_check_mark(lv_event_get_current_target(e));
    if (settings_save() != 0) toast_warn("Could not save lock screen settings");
}

/* A sketch of each layout: art, two lines of text and the controls. */
static void lock_player_sketch(lv_obj_t *pic, bool big)
{
    lv_obj_t *s = set_pic_screen(pic, SET_PIC_W, SET_PIC_H, C_BG);
    lv_obj_t *art = box(s);
    lv_obj_set_size(art, big ? 72 : 22, big ? 58 : 22);   /* big: edge to edge */
    lv_obj_set_pos(art, big ? 0 : 8, big ? 6 : 20);
    lv_obj_set_style_radius(art, big ? 0 : SL_R_MINI, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *line = box(s);
        int w = i == 2 ? 56 : big ? (i ? 30 : 44) : (i ? 22 : 32);
        lv_obj_set_size(line, w, i == 2 ? 3 : 4);
        lv_obj_set_pos(line, big ? (72 - w) / 2 : (i == 2 ? 8 : 36),
                       big ? 70 + i * 8 : (i == 2 ? 52 : 24 + i * 10));
        lv_obj_set_style_radius(line, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(i == 2 ? C_ACCENT : C_TEXT), 0);
        lv_obj_set_style_bg_opa(line, i == 1 ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *dot = box(s);
        int d = i == 1 ? 10 : 7;
        lv_obj_set_size(dot, d, d);
        lv_obj_set_style_radius(dot, SL_R_FULL, 0);
        lv_obj_set_pos(dot, 36 - d / 2 + (i - 1) * 16, (big ? 96 : 68) - d / 2);
        lv_obj_set_style_bg_color(dot, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }
}

/* A thumbnail of Now Playing in each style: the cover, then the lake it
 * is reflected in or the black panel, a thin line for the song, and the
 * controls, play the one disc. */
static void np_style_sketch(lv_obj_t *pic, int style)
{
    bool glass = style == 0 || style == 3, centred = style == 2, wave = style == 3;
    lv_obj_t *s = set_pic_screen(pic, SET_PIC_W, SET_PIC_H, 0x000000);
    lv_color_t accent = lv_color_hex(C_ACCENT);
    if (centred)
    {
        /* the whole screen in the cover's colour, softened; the cover smaller, in the middle */
        lv_obj_set_style_bg_color(s, lv_color_mix(accent, lv_color_black(), 120), 0);
        lv_obj_set_style_bg_grad_color(s, lv_color_mix(accent, lv_color_black(), 40), 0);
        lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
    }
    lv_obj_t *art = box(s);
    lv_obj_set_size(art, centred ? 46 : 72, centred ? 46 : 60);
    if (centred)
    {
        lv_obj_set_pos(art, 13, 9);
        lv_obj_set_style_radius(art, SL_R_MINI, 0);
    }
    lv_obj_set_style_bg_color(art, lv_color_mix(lv_color_white(), accent, 70), 0);
    lv_obj_set_style_bg_grad_color(art, accent, 0);
    lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    if (glass)
    {
        /* the reflection: the cover's colour again, sinking into the dark */
        lv_obj_t *g = box(s);
        lv_obj_set_size(g, 72, 44);
        lv_obj_set_pos(g, 0, 60);
        lv_obj_set_style_bg_color(g, lv_color_mix(accent, lv_color_black(), 170), 0);
        lv_obj_set_style_bg_grad_color(g, lv_color_mix(accent, lv_color_black(), 40), 0);
        lv_obj_set_style_bg_grad_dir(g, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    }
    lv_obj_t *line = box(s);
    lv_obj_set_size(line, 36, 3);
    lv_obj_set_pos(line, 8, 66);
    lv_obj_set_style_radius(line, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(line, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    if (wave)
    {
        /* the seek bar as the song's shape: played white, the rest receding */
        static const uint8_t hgt[] = { 4, 6, 10, 8, 12, 10, 6, 8, 12, 10, 8, 6, 4, 6 };
        for (int i = 0; i < 14; i++)
        {
            lv_obj_t *b = box(s);
            lv_obj_set_size(b, 2, hgt[i]);
            lv_obj_set_pos(b, 8 + i * 4, 77 - hgt[i] / 2);
            lv_obj_set_style_radius(b, SL_R_FULL, 0);
            lv_obj_set_style_bg_color(b, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(b, i < 6 ? LV_OPA_COVER : LV_OPA_40, 0);
        }
    }
    else
    {
        lv_obj_t *seek = box(s);
        lv_obj_set_size(seek, 56, 2);
        lv_obj_set_pos(seek, 8, 76);
        lv_obj_set_style_bg_color(seek, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(seek, LV_OPA_50, 0);
    }
    static const int8_t at[] = { -24, -12, 0, 12, 24 };
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *dot = box(s);
        int d = i == 2 ? 12 : i == 1 || i == 3 ? 6 : 4;
        lv_obj_set_size(dot, d, d);
        lv_obj_set_style_radius(dot, SL_R_FULL, 0);
        lv_obj_set_pos(dot, 36 - d / 2 + at[i], 91 - d / 2);
        lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(dot, i == 0 || i == 4 ? 170 : LV_OPA_COVER, 0);
    }
}

static void np_style_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    global_settings.sloppy_np_style = choice;
    set_check_mark(lv_event_get_current_target(e));
    now_playing_retheme();
    if (settings_save() != 0) toast_warn("Could not save Now Playing settings");
}

static void np_cover_ink_set(int on) { global_settings.sloppy_np_cover_ink = on; now_playing_retheme(); }
static void np_status_set(int on) { global_settings.sloppy_np_status = on; now_playing_retheme(); }
static void np_album_set(int on) { global_settings.sloppy_np_album = on; now_playing_retheme(); }
static void np_love_mark_set(int star)
{
    global_settings.sloppy_love_star = star;
    lv_obj_invalidate(lv_screen_active());   /* every heart is drawn as it is now */
    lv_obj_invalidate(lv_layer_top());
}
/* Customization > Now Playing: glass over the cover, or the black panel. */
static void np_style_page(void)
{
    static const char *const names[] = { "Glass", "Classic", "Centred", "Waveform" };
    static const char *const notes[] = { "The cover reflected under the controls, as in still water",
                                         "A plain black panel under the cover",
                                         "The cover in the middle, over its own colours, blurred",
                                         "The seek bar is the song's own shape, filled in as it plays" };
    lv_obj_t *p, *body = page_new("Now Playing", &p);
    int choice = global_settings.sloppy_np_style >= 0 && global_settings.sloppy_np_style <= 3 ? global_settings.sloppy_np_style : 0;
    lv_obj_t *g = set_group(body, "Style");
    for (int i = 0; i < 4; i++)
        np_style_sketch(lv_obj_get_child(set_pick(g, names[i], notes[i], i == choice, np_style_cb, (void *)(intptr_t)i), 2), i);
    lv_obj_t *c = opt_card(body, "Song", "The album on a line of its own, under the artist");
    opt_switch(c, "Show the album", global_settings.sloppy_np_album, np_album_set);
    c = opt_card(body, "Colours", "The seek bar, play button and heart in the cover's own colour");
    opt_switch(c, "Colours from the cover", global_settings.sloppy_np_cover_ink, np_cover_ink_set);
    c = opt_card(body, "Status bar", "Volume, the clock and the battery stay at the top, over the cover");
    opt_switch(c, "Show the status bar", global_settings.sloppy_np_status, np_status_set);
    static const char *const marks[] = { "Heart", "Star" };
    c = opt_card(body, "Loved songs", "How a loved song is marked, here and in your lists");
    opt_chips(c, marks, 2, global_settings.sloppy_love_star == 1, np_love_mark_set);
    page_push(p);
}

/* A little player in the hand it is held: sideways with the row across, or
 * upright with the row running down. The middle cover faces out, the
 * others are slivers either side. */
static void cf_style_sketch(lv_obj_t *pic, bool vertical)
{
    /* the player itself, in the picture's room: upright, or on its side */
    lv_obj_t *s = set_pic_screen(pic, vertical ? 60 : SET_PIC_W, vertical ? 96 : 46, 0x000000);
    lv_color_t accent = lv_color_hex(C_ACCENT);
    static const int8_t at[] = { -26, -19, 19, 26, 0 };   /* the middle last, on top */
    for (int i = 0; i < 5; i++)
    {
        bool mid = at[i] == 0;
        int len = mid ? 28 : 5, across = mid ? 28 : 22;
        lv_obj_t *c = box(s);
        lv_obj_set_size(c, vertical ? across : len, vertical ? len : across);
        lv_obj_align(c, LV_ALIGN_CENTER, vertical ? 0 : at[i], vertical ? at[i] : 0);
        lv_obj_set_style_radius(c, SL_R_MINI, 0);
        lv_obj_set_style_bg_color(c, mid ? accent : lv_color_mix(accent, lv_color_black(), 110), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    }
}

static void cf_style_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    global_settings.sloppy_cf_vertical = choice;
    set_check_mark(lv_event_get_current_target(e));
    if (settings_save() != 0) toast_warn("Could not save Cover Flow settings");
}

/* Customization > Cover Flow: the player on its side, or upright. */
static void cf_style_page(void)
{
    static const char *const names[] = { "Horizontal", "Vertical" };
    static const char *const notes[] = { "Turn the player on its side, albums in a row",
                                         "Keep it upright, albums stacked top to bottom" };
    lv_obj_t *p, *body = page_new("Cover Flow", &p);
    int choice = global_settings.sloppy_cf_vertical == 1 ? 1 : 0;
    lv_obj_t *g = set_group(body, "Orientation");
    for (int i = 0; i < 2; i++)
        cf_style_sketch(lv_obj_get_child(set_pick(g, names[i], notes[i], i == choice, cf_style_cb, (void *)(intptr_t)i), 2),
                        i == 1);
    page_push(p);
}

static void lock_player_page(void)
{
    static const char *const names[] = { "Big cover", "Small cover" };
    static const char *const notes[] = { "Art fills the middle of the screen",
                                         "Art beside the song, more wallpaper" };
    lv_obj_t *p, *body = page_new("Lock screen player", &p);
    int choice = global_settings.sloppy_lock_player == 1 ? 1 : 0;
    lv_obj_t *g = set_group(body, "Layout");
    for (int i = 0; i < 2; i++)
        lock_player_sketch(lv_obj_get_child(set_pick(g, names[i], notes[i], i == choice, lock_player_cb,
                                                     (void *)(intptr_t)i), 2), i == 0);
    opt_note(body, "Shows on the lock screen while music is playing or paused, with previous, play/pause and next.");
    page_push(p);
}

/* Settings > Screen sleep: how long the screen stays on while untouched. */
static const char *const screen_sleep_names[] = { "30 seconds", "1 minute", "2 minutes" };
static lv_obj_t *screen_sleep_row;               /* the Settings row, to refresh its subtitle */

static int screen_sleep_choice(void)
{
    int i = global_settings.sloppy_sleep;
    return i >= 0 && i < 3 ? i : 1;
}

static const char *screen_sleep_row_text(void)
{
    static char b[40];
    if (!sl_zune()) return sl_tr(screen_sleep_names[screen_sleep_choice()]);   /* the square's corner: just how long */
    snprintf(b, sizeof b, "%s %s", sl_tr("Turn off after"), sl_tr(screen_sleep_names[screen_sleep_choice()]));
    return b;
}

static void screen_sleep_row_deleted(lv_event_t *e)
{
    if (screen_sleep_row == lv_event_get_current_target(e)) screen_sleep_row = NULL;
}

static void screen_sleep_row_track(lv_obj_t *row)
{
    screen_sleep_row = row;
    lv_obj_add_event_cb(row, screen_sleep_row_deleted, LV_EVENT_DELETE, NULL);
}

static void screen_sleep_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    global_settings.sloppy_sleep = choice;
    set_check_mark(lv_event_get_current_target(e));
    if (screen_sleep_row) lv_label_set_text(lv_obj_get_child(screen_sleep_row, 1), screen_sleep_row_text());
    if (settings_save() != 0) toast_warn("Could not save screen settings");
}

static void screen_sleep_page(void)
{
    lv_obj_t *p, *body = page_new("Screen sleep", &p);
    int choice = screen_sleep_choice();
    lv_obj_t *g = set_group(body, "TURN OFF AFTER");
    for (int i = 0; i < 3; i++)
        set_check(g, NULL, 0, screen_sleep_names[i], i == choice, screen_sleep_cb, (void *)(intptr_t)i);
    opt_note(body, "The screen dims shortly before it turns off and locks. Music keeps playing.");
    page_push(p);
}

static void sounds_page(void)
{
    lv_obj_t *p, *body = page_new("Sound effects", &p);
    /* one level for every sound GabagoolOS makes itself; music has its own */
    lv_obj_t *c = set_group(body, "Sound volume");
    lv_obj_t *r = set_slider_row(c, "Effects and games", "", C_ACCENT_TXT);
    sounds_value = lv_obj_get_child(r, 1);
    sounds_slider = lv_obj_get_child(r, 2);
    lv_slider_set_range(sounds_slider, 0, 100);
    lv_slider_set_value(sounds_slider, sfx_volume_pct(), LV_ANIM_OFF);
    lv_obj_add_event_cb(sounds_slider, sounds_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sounds_slider, sounds_slider_cb, LV_EVENT_RELEASED, NULL);
    sounds_value_show(sfx_volume_pct());
    /* the interface's own sounds: pages, switches, lock, notifications */
    sounds_ui_switch = set_switch(c, "Interface sounds", "Pages, switches, lock and notifications",
                                  global_settings.sloppy_ui_sounds, NULL);
    lv_obj_add_event_cb(sounds_ui_switch, sounds_ui_cb, LV_EVENT_VALUE_CHANGED, NULL);
    opt_note(body, "Effects, games and interface sounds. Music keeps its own volume. Music steps aside for a sound "
                   "while it plays when audio prioritisation is on in Rockbox's sound settings.");
    page_push(p);
}

/* The exact colour chosen. HSV round trips drift by a unit or two, so a
 * preset or the current accent is kept as given until the wheel moves. */
static uint32_t accent_rgb;
static uint32_t accent_pick_rgb(void) { return accent_rgb; }
static void accent_pick_changed(void)
{
    accent_rgb = lv_color_to_u32(lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s,
                                                     accent_pick.v)) & 0xffffff;
}

static void accent_set_rgb(uint32_t c)
{
    accent_rgb = c & 0xffffff;
    accent_pick = lv_color_rgb_to_hsv((c >> 16) & 255, (c >> 8) & 255, c & 255);
}

static void accent_preview_update(void)
{
    uint32_t color = accent_pick_rgb();
    uint32_t ink = sl_ink_on(color);     /* the stronger of the two, by contrast (white on Zule's pink was 2.9:1 at 80 %) */
    bool same = color == (C_ACCENT & 0xffffff);
    char hex[12];
    snprintf(hex, sizeof hex, "#%06lX", (unsigned long)color);
    lv_label_set_text(accent_preview_label, sl_tr(same ? "CURRENT COLOR" : "USE THIS COLOR"));
    lv_label_set_text(accent_hex, hex);
    /* under the label however tall the language and text size make it */
    lv_obj_update_layout(accent_preview_label);
    lv_obj_set_y(accent_hex, LV_MAX(48, lv_obj_get_y2(accent_preview_label) + 1));
    lv_obj_update_layout(accent_hex);
    lv_obj_set_height(accent_preview, LV_MAX(84, lv_obj_get_y2(accent_hex) + 14));   /* and the card around both */
    lv_obj_set_style_bg_color(accent_preview, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(accent_preview_label, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(accent_hex, lv_color_hex(ink), 0);
    lv_obj_set_style_opa(accent_hex, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(accent_was_label, lv_color_hex(ink), 0);
    lv_obj_update_flag(accent_was, LV_OBJ_FLAG_HIDDEN, same);
    lv_obj_update_flag(accent_was_label, LV_OBJ_FLAG_HIDDEN, same);
    lv_obj_set_style_border_color(accent_was, lv_color_hex(ink), 0);
    lv_obj_set_style_bg_color(accent_marker, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(accent_shade, (100 - accent_pick.v) * 255 / 100, 0);
    /* The brightness track runs from black to the picked hue at full light. */
    lv_obj_set_style_bg_grad_color(accent_brightness,
        lv_color_hsv_to_rgb(accent_pick.h, accent_pick.s, 100), LV_PART_MAIN);
    for (unsigned i = 0; i < ARRAYLEN(accent_presets); i++)
        lv_obj_set_style_border_width(accent_swatches[i],
                                      accent_presets[i] == color ? 4 : 0, 0);
    float angle = accent_pick.h * 0.017453293f;
    float radius = accent_pick.s * (ACCENT_WHEEL_SIZE / 2 - 5) / 100.0f;
    lv_obj_set_pos(accent_marker,
        8 + ACCENT_WHEEL_SIZE / 2 + (int)lroundf(cosf(angle) * radius) - 15,
        8 + ACCENT_WHEEL_SIZE / 2 - (int)lroundf(sinf(angle) * radius) - 15);
}

static void accent_wheel_select(float dx, float dy)
{
    float radius = sqrtf(dx * dx + dy * dy);
    int hue = (int)(atan2f(dy, dx) * 57.2957795f + 360.5f) % 360;
    accent_pick.h = hue;
    accent_pick.s = radius >= ACCENT_WHEEL_SIZE / 2 - 5 ? 100 :
                    (uint8_t)(radius * 100 / (ACCENT_WHEEL_SIZE / 2 - 5));
    /* Picking on a wheel dimmed to nothing would show nothing; lift it. */
    if (accent_pick.v < 35)
    {
        accent_pick.v = 35;
        lv_slider_set_value(accent_brightness, 35, LV_ANIM_ON);
    }
    accent_pick_changed();
    accent_preview_update();
}

static void accent_wheel_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t touch;
    lv_area_t area;
    lv_indev_get_point(indev, &touch);
    lv_obj_get_coords(lv_event_get_current_target(e), &area);
    accent_wheel_select(touch.x - area.x1 - ACCENT_WHEEL_SIZE / 2,
                        ACCENT_WHEEL_SIZE / 2 - (touch.y - area.y1));
}

static void accent_brightness_cb(lv_event_t *e)
{
    accent_pick.v = lv_slider_get_value(lv_event_get_current_target(e));
    accent_pick_changed();
    accent_preview_update();
}


static void accent_swatch_cb(lv_event_t *e)
{
    accent_set_rgb(accent_presets[(intptr_t)lv_event_get_user_data(e)]);
    lv_slider_set_value(accent_brightness, accent_pick.v, LV_ANIM_ON);
    accent_preview_update();
}

static void accent_apply_cb(lv_event_t *e)
{
    (void)e;
    uint32_t color = accent_pick_rgb();
    if (color == (C_ACCENT & 0xffffff)) return;
    global_settings.sloppy_accent_color = color;
    sfx_play(SFX_START);
    appearance_changed();
}

/* Hue around, saturation outwards, at full brightness; brightness is a
 * shade laid over it so the slider never repaints 50k pixels. The rim is
 * anti-aliased through the alpha channel, so it sits on any wallpaper. */
static void accent_wheel_paint(void)
{
    const float centre = ACCENT_WHEEL_SIZE / 2.0f - 0.5f;
    const float outer = ACCENT_WHEEL_SIZE / 2.0f - 1.5f;
    const float full = ACCENT_WHEEL_SIZE / 2 - 5;
    for (int y = 0; y < ACCENT_WHEEL_SIZE; y++)
    {
        for (int x = 0; x < ACCENT_WHEEL_SIZE; x++)
        {
            float dx = x - centre, dy = centre - y;
            float radius = sqrtf(dx * dx + dy * dy);
            lv_color32_t *px = &accent_wheel_pixels[y * ACCENT_WHEEL_SIZE + x];
            float cover = outer + 0.5f - radius;
            if (cover <= 0)
            {
                *px = (lv_color32_t){ 0 };
                continue;
            }
            int hue = (int)(atan2f(dy, dx) * 57.2957795f + 360.5f) % 360;
            int sat = radius >= full ? 100 : (int)(radius * 100 / full);
            lv_color_t c = lv_color_hsv_to_rgb(hue, sat, 100);
            *px = (lv_color32_t){ .blue = c.blue, .green = c.green, .red = c.red,
                                  .alpha = cover >= 1 ? 255 : (uint8_t)(cover * 255) };
        }
    }
}

/* painted whole each time the page opens: 196 KB back when it closes */
static void accent_page_deleted(lv_event_t *e)
{
    (void)e;
    sl_give_back(accent_wheel_pixels, sizeof accent_wheel_pixels);
}
static void accent_page(void)
{
    lv_obj_t *p, *body = page_new("Accent color", &p);
    lv_obj_add_event_cb(p, accent_page_deleted, LV_EVENT_DELETE, NULL);
    accent_set_rgb(C_ACCENT);
    /* the wheel and its brightness, one card; the presets, another; the
     * button that wears the colour under them */
    lv_obj_t *g = set_group(body, "Color wheel");
    lv_obj_t *wrow = box(g);
    lv_obj_set_size(wrow, lv_pct(100), 240 + SL_GUTTER);
    lv_obj_t *stage = box(wrow);
    lv_obj_set_size(stage, 240, 240);
    lv_obj_align(stage, LV_ALIGN_TOP_MID, 0, SL_GUTTER);
    lv_obj_t *wheel = lv_canvas_create(stage);
    lv_canvas_set_buffer(wheel, accent_wheel_pixels, ACCENT_WHEEL_SIZE,
                         ACCENT_WHEEL_SIZE, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(wheel, 8, 8);
    accent_wheel_paint();
    lv_obj_add_flag(wheel, LV_OBJ_FLAG_CLICKABLE);
    /* Dragging on the wheel picks a colour; it must not scroll the page. */
    lv_obj_remove_flag(wheel, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(wheel, accent_wheel_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(wheel, accent_wheel_touch, LV_EVENT_PRESSING, NULL);
    accent_shade = box(stage);
    lv_obj_set_pos(accent_shade, 8, 8);
    lv_obj_set_size(accent_shade, ACCENT_WHEEL_SIZE, ACCENT_WHEEL_SIZE);
    lv_obj_set_style_radius(accent_shade, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(accent_shade, lv_color_black(), 0);
    accent_marker = box(stage);
    lv_obj_set_size(accent_marker, 30, 30);
    lv_obj_set_style_radius(accent_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(accent_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(accent_marker, lv_color_white(), 0);
    lv_obj_set_style_border_width(accent_marker, 4, 0);
    sl_elevate(accent_marker, SL_E2, 0);   /* a knob on the wheel */
    set_join_next(g);                    /* the wheel's own slider: no hairline between */
    lv_obj_t *br = set_slider_row(g, "Brightness", "", C_ACCENT_TXT);
    accent_brightness = lv_obj_get_child(br, 2);
    lv_slider_set_range(accent_brightness, 20, 100);
    lv_slider_set_value(accent_brightness, accent_pick.v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(accent_brightness, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent_brightness, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(accent_brightness, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_border_color(accent_brightness, lv_color_hex(C_DIM), LV_PART_MAIN);
    lv_obj_set_style_border_width(accent_brightness, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(accent_brightness, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_add_event_cb(accent_brightness, accent_brightness_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *swatches = box(set_group(body, "Presets"));
    lv_obj_set_size(swatches, lv_pct(100), 46 + 2 * SL_GUTTER);
    lv_obj_set_style_pad_hor(swatches, SL_INSET, 0);
    lv_obj_set_flex_flow(swatches, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_base_dir(swatches, set_rtl() ? LV_BASE_DIR_RTL : LV_BASE_DIR_LTR, 0);
    lv_obj_set_flex_align(swatches, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (unsigned i = 0; i < ARRAYLEN(accent_presets); i++)
    {
        lv_obj_t *dot = accent_swatches[i] = box(swatches);
        lv_obj_set_size(dot, 46, 46);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(accent_presets[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_transform_scale(dot, 230, LV_STATE_PRESSED);
        lv_obj_set_style_transform_pivot_x(dot, 23, 0);
        lv_obj_set_style_transform_pivot_y(dot, 23, 0);
        lv_obj_set_ext_click_area(dot, 4);
        on_click(dot, accent_swatch_cb, (void *)(intptr_t)i);
    }
    /* One button that is also the preview: it wears the new colour, and the
     * small ring on the right keeps the current one for comparison. */
    accent_preview = box(body);
    lv_obj_set_size(accent_preview, lv_pct(100), 84);
    lv_obj_set_style_margin_top(accent_preview, SL_GUTTER + SL_SP_XS, 0);   /* as set_confirm's way on */
    lv_obj_set_style_radius(accent_preview, sl_zune() ? 0 : SL_R_LG, 0);
    lv_obj_set_style_bg_opa(accent_preview, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale(accent_preview, 246, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(accent_preview, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(accent_preview, 42, 0);
    accent_preview_label = text(accent_preview, "", &ss_semibold_23, C_TEXT);
    lv_obj_set_pos(accent_preview_label, SL_INSET, 14);
    accent_hex = text(accent_preview, "", &ss_regular_18, C_TEXT);
    lv_obj_set_pos(accent_hex, SL_INSET, 48);
    accent_was = box(accent_preview);
    lv_obj_set_size(accent_was, 40, 40);
    lv_obj_align(accent_was, LV_ALIGN_RIGHT_MID, -SL_INSET, 0);
    lv_obj_set_style_radius(accent_was, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(accent_was, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(accent_was, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(accent_was, 3, 0);
    accent_was_label = text(accent_preview, "NOW", &ss_regular_16, C_TEXT);
    lv_obj_align(accent_was_label, LV_ALIGN_RIGHT_MID, -70, 0);
    on_click(accent_preview, accent_apply_cb, NULL);
    accent_preview_update();
    page_push(p);
}

static void wallpaper_choice_cb(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    bool lock = choice >= 16;
    int id = choice & 15;
    if (lock) global_settings.sloppy_lock_wallpaper = id;
    else global_settings.sloppy_home_wallpaper = id;
    if (global_settings.sloppy_home_wallpaper != WALL_CUSTOM && global_settings.sloppy_lock_wallpaper != WALL_CUSTOM)
        wallpaper_custom_free();             /* nothing shows it */
    appearance_changed();
}

/* "Set as wallpaper": a screenshot or a picture in Files, for the home
 * screen, the lock screen or both. The picture is made on a thread; the
 * page stays usable meanwhile. */
static lv_timer_t *wall_set_timer;
static int wall_set_where;                   /* 0 home, 1 lock, 2 both */
static void wall_set_poll(lv_timer_t *t)
{
    int r = sl_wall_poll();
    if (!r) return;
    lv_timer_pause(t);
    if (r < 0) { toast_warn("GabagoolOS can't make a wallpaper of this picture"); return; }
    wallpaper_custom_free();
    wall_custom_failed = false;
    if (wall_set_where != 1) global_settings.sloppy_home_wallpaper = WALL_CUSTOM;
    if (wall_set_where != 0) global_settings.sloppy_lock_wallpaper = WALL_CUSTOM;
    appearance_changed();
    toast(wall_set_where == 0 ? "Home screen wallpaper set" : wall_set_where == 1 ? "Lock screen wallpaper set" : "Wallpaper set");
}
static char wall_set_path[MAX_PATH];
static void wall_set_pick(int k)
{
    wall_set_where = k;
    int rc = sl_wall_start(wall_set_path, WALL_CUSTOM_FILE, LCD_WIDTH, LCD_HEIGHT);
    if (rc == SL_PIC_BUSY) { toast("Still making the last wallpaper"); return; }
    if (rc != SL_PIC_STARTED) { toast_warn("GabagoolOS can't make a wallpaper of this picture"); return; }
    if (!wall_set_timer) wall_set_timer = lv_timer_create(wall_set_poll, 100, NULL);
    lv_timer_reset(wall_set_timer);
    lv_timer_resume(wall_set_timer);
}
static void pick_sheet(const char *title, const char *const *names, int n, int cur, void (*pick)(int));
static void wallpaper_set_offer(const char *path)
{
    static const char *const where[] = { "Home screen", "Lock screen", "Both" };
    strlcpy(wall_set_path, path, sizeof wall_set_path);
    pick_sheet("Set as wallpaper", where, 3, -1, wall_set_pick);
}
static void wallpaper_set_cb(lv_event_t *e) { wallpaper_set_offer((const char *)lv_event_get_user_data(e)); }

static void wallpaper_page(bool lock)
{
    static const char *const names[] = { "Plain", "Soft glow", "Fine grid",
                                         "Constellation", "Waves", "Orion nebula",
                                         "Eagle nebula", "Red roots", "Nexus live", "Your picture" };
    lv_obj_t *p, *body = page_new(lock ? "Lock wallpaper" : "Home wallpaper", &p);
    lv_obj_t *g = set_group(body, "Background");
    for (int i = 0; i <= WALL_CUSTOM; i++)
    {
        if (i == WALL_CUSTOM && !wallpaper_custom_load()) break;   /* none made yet */
        lv_obj_t *row = set_pick(g, names[i], NULL, wallpaper_id(lock) == i, wallpaper_choice_cb,
                                 (void *)(intptr_t)(i | (lock ? 16 : 0)));
        /* drawn on a child, so the screen's rounded corners clip it */
        lv_obj_t *sample = box(set_pic_screen(lv_obj_get_child(row, 2), 60, 100, C_BG));
        lv_obj_set_size(sample, lv_pct(100), lv_pct(100));
        lv_obj_add_event_cb(sample, wallpaper_preview_draw, LV_EVENT_DRAW_MAIN,
                            (void *)(intptr_t)i);
        if (i == 8)
        {
            wallpaper_nexus_preview = sample;
            lv_obj_add_event_cb(sample, wallpaper_nexus_preview_deleted, LV_EVENT_DELETE, NULL);
        }
    }
    page_push(p);
}

