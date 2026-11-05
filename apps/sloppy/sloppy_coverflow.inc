/*
 *  Cover Flow, as on the iPod classic: the player turns on its side and
 *  the albums stand in a row. The one in the middle faces you; the rest
 *  lean away on either side, each with a reflection on a black floor.
 *
 *  Drag or flick to move through them (it snaps to an album), tap the
 *  middle one and it flips over to show its songs, tap a song to play the
 *  album from there. Swipe the song list away and the cover flips back and
 *  the row keeps moving the way you swiped. Tap Done to go back upright.
 *
 *  The covers are drawn by a small perspective-correct renderer into one
 *  full-screen RGB565 buffer (the control centre's glass buffer, which is
 *  never in use at the same time). The display is rotated with LVGL's own
 *  rotation; flush_cb turns each strip back for the portrait panel.
 *
 *  Vertical (Customization > Cover Flow): the player stays upright and the
 *  albums run top to bottom. The renderer draws the same scene with its
 *  rows and columns swapped, both in the frame (cf_draw_cover's strides)
 *  and in each cover (left row-major, see cf_orient), so the art stays
 *  upright and the reflection falls to the right, a mirror beside the row.
 */

#define CF_W      H                 /* landscape */
#define CF_H      372               /* covers and reflections end above the labels */
#define CF_S      244               /* cover size in the middle */
#define CF_YC     166               /* covers' vertical centre */
#define CF_F      1200.0f           /* focal length: a long lens, gentle perspective */
#define CF_SIDE   200               /* middle to the first side cover: a clear gap */
#define CF_STEP   52                /* between side covers */
#define CF_TILT   1.13f             /* side cover angle, radians (65 deg) */
#define CF_DEPTH  180.0f            /* how far side covers sit back */
#define CF_DRAG   120               /* px of drag per album */
#define CF_ONE    1024              /* position fixed point */
#define CF_SLOTS  40                /* 15 on screen, the reach ahead, a few behind: 3.8 MB */
#define CF_REACH  10                /* covers kept ready either side of the middle */
#define CF_KEEP_MS (10 * 60 * 1000) /* the disk cache remains after RAM is freed */
#define CF_ART    SL_ART_GRID

/* px is column-major (px[col * h + row]): the renderer walks each cover
 * down its columns, and row-major art made every step of that walk a new
 * cache line on the X1600 (16 KB L1), about 30 ms a frame. */
struct cf_art { int album; int w, h; uint16_t *px; bool real, tried, visible, vert; uint32_t used; };
static uint16_t *cf_tmp;                 /* one cover, for turning it to columns */
static void cf_to_columns(struct cf_art *s)
{
    if (!cf_tmp) cf_tmp = malloc(CF_ART * CF_ART * 2);
    if (!cf_tmp) return;                 /* left row-major: a scrambled cover, not a crash */
    memcpy(cf_tmp, s->px, (size_t)s->w * s->h * 2);
    for (int y = 0; y < s->h; y++)
    {
        const uint16_t *row = cf_tmp + y * s->w;
        for (int x = 0; x < s->w; x++)
            s->px[x * s->h + y] = row[x];
    }
}

static bool cf_vertical(void);
/* A cover for the renderer, which walks it along the row: columns first
 * when the row runs across, rows first (as loaded, w and h swapped) when
 * it runs down. */
static void cf_orient(struct cf_art *s)
{
    s->vert = cf_vertical();
    if (!s->vert) { cf_to_columns(s); return; }
    int w = s->w;
    s->w = s->h;
    s->h = w;
}

static struct
{
    lv_obj_t *scr, *home, *canvas, *title, *artist, *scrub, *done, *panel, *list;
    lv_timer_t *timer;
    uint16_t *fb;
    int32_t pos;                     /* album position * CF_ONE, albums_by_title order */
    int n, flip, open_at, last_centre;
    int drag_x0, last_x, vel;        /* vel: CF_ONE per second */
    int32_t drag_pos0;
    uint32_t last_t, frame, moved_at;   /* moved_at: last motion, for the scroll bar */
    int bar_opa;
    bool active, dirty, dragging, swiped;
    bool vert;                       /* upright, albums top to bottom */
    int cx0;                         /* vertical: the canvas's left edge on screen */
    uint32_t renders, render_us, render_max;   /* cost, logged on close */
    uint32_t closed_at;
    unsigned art_seen;               /* sl_art_made when placeholders last looked */
    int32_t last_pos;                /* for which way the row is going */
    int heading;
    struct cf_art art[CF_SLOTS];
    lv_obj_t *ph_canvas;
} cf;

static bool cf_vertical(void) { return cf.vert; }
static void cf_close(void);
static void cf_set_pos(void *o, int32_t v);
/* The album in the middle. A drag past either end overshoots the row, so
 * clamp: the labels and the album panel index the album list with it. */
static int cf_centre(void)
{
    int c = (cf.pos + CF_ONE / 2) / CF_ONE;
    if (cf.pos < 0 || c < 0) c = 0;
    if (c > cf.n - 1) c = cf.n > 0 ? cf.n - 1 : 0;
    return c;
}

/* ------------------------------------------------------------ artwork */
static void cf_placeholder(struct cf_art *s, int album)
{
    /* the same gradient and title the album grid uses when there is no art */
    const struct sl_album *al = &sl_lib.albums[album];
    s->w = s->h = CF_ART;
    if (!cf.ph_canvas)
    {
        cf.ph_canvas = lv_canvas_create(cf.scr);
        lv_obj_add_flag(cf.ph_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    lv_canvas_set_buffer(cf.ph_canvas, s->px, CF_ART, CF_ART, LV_COLOR_FORMAT_RGB565);
    lv_layer_t layer;
    lv_canvas_init_layer(cf.ph_canvas, &layer);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hsv_to_rgb(al->hue, 62, 62);
    r.bg_grad.dir = LV_GRAD_DIR_VER;
    r.bg_grad.stops_count = 2;
    r.bg_grad.stops[0].color = lv_color_hsv_to_rgb(al->hue, 62, 62);
    r.bg_grad.stops[0].opa = LV_OPA_COVER;
    r.bg_grad.stops[1].color = lv_color_hsv_to_rgb((al->hue + 70) % 360, 70, 22);
    r.bg_grad.stops[1].opa = LV_OPA_COVER;
    r.bg_grad.stops[1].frac = 255;
    lv_area_t all = { 0, 0, CF_ART - 1, CF_ART - 1 };
    lv_draw_rect(&layer, &r, &all);
    lv_draw_label_dsc_t t;
    lv_draw_label_dsc_init(&t);
    t.color = lv_color_white();
    t.font = &sl_sym_48;
    t.text = LV_SYMBOL_AUDIO;
    t.opa = 90;
    lv_area_t note = { 20, 22, CF_ART - 1, 90 };
    lv_draw_label(&layer, &t, &note);
    t.font = &ss_semibold_23;
    t.opa = LV_OPA_COVER;
    t.text = al->title;
    t.bidi_dir = LV_BASE_DIR_AUTO;
    lv_area_t title = { 16, CF_ART - 110, CF_ART - 16, CF_ART - 40 };
    lv_draw_label(&layer, &t, &title);
    t.font = &ss_regular_18;
    t.opa = 200;
    t.text = al->artist;
    lv_area_t artist = { 16, CF_ART - 34, CF_ART - 16, CF_ART - 10 };
    lv_draw_label(&layer, &t, &artist);
    lv_canvas_finish_layer(cf.ph_canvas, &layer);
    s->real = false;
    cf_orient(s);
}

/* The thumbnail's card path, counted for the meter: cf_open's reads, or
 * the frame path's (everything cf_tick does). */
static bool cf_opening;
static const char *cf_src(int album)
{
    uint64_t t0 = sl_perf_now_us();
    const char *src = sl_art_src(album, CF_ART);
    uint32_t us = (uint32_t)(sl_perf_now_us() - t0);
    if (cf_opening) { cfm.open_checks++; cfm.open_check_us += us; }
    else
    {
        cfm.tick_checks++; cfm.tick_check_us += us;
        if (us > cfm.tick_check_max) cfm.tick_check_max = us;
    }
    return src;
}

static bool cf_load_thumb(struct cf_art *s, const char *src)
{
    uint64_t t0 = sl_perf_now_us();
    int fd = open(src + 2, O_RDONLY);                /* skip the "R:" drive */
    uint64_t t1 = sl_perf_now_us();
    cfm_add(&cfm.thumb_open, (uint32_t)(t1 - t0));
    if (fd < 0) return false;
    lv_image_header_t hdr;
    bool ok = read(fd, &hdr, sizeof hdr) == (ssize_t)sizeof hdr &&
              hdr.magic == LV_IMAGE_HEADER_MAGIC && hdr.cf == LV_COLOR_FORMAT_RGB565 &&
              hdr.w > 0 && hdr.w <= CF_ART && hdr.h > 0 && hdr.h <= CF_ART && hdr.stride == hdr.w * 2;
    if (ok) ok = read(fd, s->px, (size_t)hdr.w * hdr.h * 2) == (ssize_t)((size_t)hdr.w * hdr.h * 2);
    close(fd);
    uint32_t us = (uint32_t)(sl_perf_now_us() - t0);
    cfm_add(&cfm.thumb_read, (uint32_t)(sl_perf_now_us() - t1));
    if (cf_opening) { cfm.open_reads++; cfm.open_read_us += us; }
    else
    {
        cfm.tick_reads++; cfm.tick_read_us += us;
        if (us > cfm.tick_read_max) cfm.tick_read_max = us;
    }
    if (!ok) return false;
    s->w = hdr.w; s->h = hdr.h; s->real = true;
    cf_orient(s);
    return true;
}

/* load: read the thumbnail now instead of leaving it to cf_tick. Only used
 * while opening, so the first frames show real covers. */
/* How many covers may be held at once: all 40 with memory to spare, fewer
 * when the player is short (15 are on screen). Each is 94 KB; on a full
 * R1 the 3.8 MB of them crowded out the music being read, and playback
 * stopped (the device log, 2026-10-13). Set when Cover Flow opens. */
static int cf_cap = CF_SLOTS;
static struct cf_art *cf_art_get(int album, bool load)
{
    struct cf_art *s = NULL, *lru = NULL, *lru_held = NULL;
    int held = 0;
    for (int i = 0; i < CF_SLOTS && !s; i++)
    {
        struct cf_art *t = &cf.art[i];
        if (t->px) held++;
        if (t->px && t->album == album) s = t;
        else
        {
            if (!lru || (lru->px && (!t->px || t->used < lru->used))) lru = t;
            if (t->px && (!lru_held || t->used < lru_held->used)) lru_held = t;
        }
    }
    if (!s && lru && !lru->px && held >= cf_cap && lru_held) lru = lru_held;   /* reuse, don't grow */
    bool paint = !s;                     /* a new slot needs its placeholder */
    if (!s)
    {
        if (!lru->px) lru->px = malloc(CF_ART * CF_ART * 2);
        if (!lru->px) return NULL;
        s = lru;
        s->album = album;
        s->tried = s->real = false;
    }
    /* One card lookup per cover (each about 4 ms on the R1), found or
     * not. A missing one used to be looked up again on every frame that
     * drew it; sl_art_made clears tried when new covers are made. */
    if (load && !s->real && !s->tried)
    {
        s->tried = true;
        const char *src = cf_src(album);
        if (src) paint = !cf_load_thumb(s, src);   /* a short read may leave half a cover */
    }
    if (paint) cf_placeholder(s, album);
    s->used = cf.frame;
    s->visible = true;
    return s;
}

/* The card thumbnails persist until a library refresh. RAM can be freed
 * after a while away because a returning view loads the card before draw. */
static void cf_art_flush(void)
{
    for (int i = 0; i < CF_SLOTS; i++) { free(cf.art[i].px); cf.art[i].px = NULL; }
    free(cf_tmp); cf_tmp = NULL;
}
static void cf_art_trim(void)
{
    if (!cf.active && cf.closed_at && lv_tick_get() - cf.closed_at > CF_KEEP_MS)
    {
        cf.closed_at = 0;
        cf_art_flush();
        sl_log("cover flow: RAM freed; card thumbnails retained");
    }
}
/* Keep the covers either side of the middle ready before they come into
 * view, the way the row is heading first, so a swipe never outruns them.
 * Each is a card read, so only a few per frame, one while moving. */
static bool cf_wants(int k)
{
    int album = sl_lib.albums_by_title[k];
    for (int i = 0; i < CF_SLOTS; i++)
        if (cf.art[i].px && cf.art[i].album == album)
            return !cf.art[i].real && !cf.art[i].tried && cf_src(album);
    return true;                         /* not cached: a cover or its placeholder */
}
static void cf_prefetch(int budget)
{
    int centre = cf_centre(), ahead = cf.heading ? cf.heading : 1;
    for (int ring = 0; ring <= CF_REACH && budget > 0; ring++)
        for (int side = 0; side < 2 && budget > 0; side++)
        {
            int k = centre + (side ? -ahead : ahead) * ring;
            if ((ring == 0 && side) || k < 0 || k >= cf.n || !cf_wants(k)) continue;
            struct cf_art *s = cf_art_get(sl_lib.albums_by_title[k], true);
            if (s) s->visible = false;       /* ready, not necessarily shown */
            if (ring <= 7) cf.dirty = true;  /* on screen: draw it in */
            budget--;
        }
}

/* ------------------------------------------------------------ drawing */
/* RGB565 arithmetic on all three channels at once: two multiplies to
 * shade (6-bit light) and one to blend (5-bit alpha), instead of three
 * each. The renderer does this for every pixel of every cover, about a
 * quarter of a million times a frame on a 1 GHz in-order core. */
static inline uint16_t cf_shade(uint16_t c, int k)   /* k of 256 */
{
    uint32_t k6 = (uint32_t)(k + 2) >> 2;               /* of 64 */
    if (k6 >= 64) return c;
    uint32_t rb = (c & 0xF81Fu) * k6, g = (c & 0x07E0u) * k6;
    return (uint16_t)(((rb >> 6) & 0xF81Fu) | ((g >> 6) & 0x07E0u));
}
static inline uint32_t cf_wide(uint16_t c) { return (c | (uint32_t)c << 16) & 0x07E0F81Fu; }
static inline uint16_t cf_mix(uint16_t d, uint16_t s, int a)   /* a of 256 */
{
    uint32_t a5 = (uint32_t)(a + 4) >> 3;               /* of 32 */
    if (a5 == 0) return d;
    if (a5 >= 32) return s;
    uint32_t w = (cf_wide(s) * a5 + cf_wide(d) * (32 - a5)) >> 5;
    w &= 0x07E0F81Fu;
    return (uint16_t)(w | w >> 16);
}

/* Where album k sits for a row position: offset from the middle, lean,
 * how far back, and how bright. */
struct cf_place { float x0, a, b, z0; int light; };
static void cf_place(int32_t d, struct cf_place *pl)
{
    float t = d / (float)CF_ONE, ad = t < 0 ? -t : t, sg = t < 0 ? -1.0f : 1.0f;
    float k = ad < 1 ? ad : 1, far = ad > 1 ? ad - 1 : 0;
    /* ease the lean in over the first step so the middle cover turns smoothly */
    float lean = k * k * (3 - 2 * k);
    pl->x0 = sg * (lean * CF_SIDE + far * CF_STEP);
    float ang = lean * CF_TILT;
    pl->a = CF_S * cosf(ang);
    pl->b = sg * CF_S * sinf(ang);
    pl->z0 = lean * CF_DEPTH;
    /* side covers stay bright, as on the iPod; only the far ones fade */
    int light = 256 - (int)(lean * 34) - (int)(far * 20);
    pl->light = light < 80 ? 80 : light;
}

#ifdef SIMULATOR
static uint32_t cf_px_written;           /* bench: pixels written per frame, overdraw included */
#endif
static void cf_draw_cover(const struct cf_art *s, const struct cf_place *pl)
{
    uint16_t *fb = cf.fb;
    const float cx = CF_W / 2.0f, F = CF_F;
    /* a step down a column, and one along the row: swapped when upright */
    const int ys = cf.vert ? 1 : CF_W, xs = cf.vert ? CF_H : 1;
    /* projected left and right edges */
    float xl = cx + (pl->x0 - pl->a / 2) * F / (F + pl->z0 - pl->b / 2);
    float xr = cx + (pl->x0 + pl->a / 2) * F / (F + pl->z0 + pl->b / 2);
    int x1 = (int)(xl < xr ? xl : xr), x2 = (int)(xl < xr ? xr : xl) + 1;
    if (x1 < 0) x1 = 0;
    if (x2 > CF_W) x2 = CF_W;
    for (int x = x1; x < x2; x++)
    {
        float sx = x + 0.5f - cx;
        float den = sx * pl->b - F * pl->a;
        if (den > -1e-3f && den < 1e-3f) continue;
        float u = (F * pl->x0 - sx * (F + pl->z0)) / den;       /* -0.5 .. 0.5 */
        if (u < -0.5f || u > 0.5f) continue;
        int col = (int)((u + 0.5f) * s->w);
        if (col >= s->w) col = s->w - 1;
        /* a side cover squeezes several source columns into one pixel:
         * blend in the neighbour half a pixel over, so it doesn't sparkle */
        float du = (u + 0.5f) * s->w - col;
        int col2 = col + 1 < s->w ? col + 1 : col;
        int cmix = pl->b > 1 || pl->b < -1 ? (int)(du * 256) : 0;   /* the middle one is 1:1 */
        float h = CF_S * F / (F + pl->z0 + pl->b * u);
        float ftop = CF_YC - h / 2;
        int top = (int)ftop, hh = (int)h;
        if (hh < 2) continue;
        /* lit from the front: a side cover darkens toward its far edge */
        int light = pl->light;
        if (pl->b > 1 || pl->b < -1)
        {
            float farness = pl->b > 0 ? u + 0.5f : 0.5f - u;
            light = light * (256 - (int)(farness * 56)) >> 8;
        }
        int step = (s->h << 16) / hh;                         /* source rows per pixel, 16.16 */
        int half = step > (3 << 15) ? step >> 1 : 0;           /* two taps when squeezed */
        const uint16_t *src = s->px + col * s->h, *src2 = s->px + col2 * s->h;
        int y = top < 0 ? 0 : top, bottom = top + hh;
        int v = (y - top) * step;
        int end = bottom < CF_H ? bottom : CF_H;
        uint16_t *o = fb + y * ys + x * xs;
        /* one loop per case, chosen per column, not tested per pixel */
        if (half)
        {
            int last = (s->h << 16) - half;            /* the second tap stays inside */
            for (; y < end; y++, v += step, o += ys)
            {
                int r0 = v >> 16;
                uint16_t c = cmix ? cf_mix(src[r0], src2[r0], cmix) : src[r0];
                if (v < last) c = cf_mix(c, src[(v + half) >> 16], 128);
                *o = cf_shade(c, light);
            }
        }
        else if (cmix)
            for (; y < end; y++, v += step, o += ys)
            {
                int r0 = v >> 16;
                *o = cf_shade(cf_mix(src[r0], src2[r0], cmix), light);
            }
        else if (light < 254)
            for (; y < end; y++, v += step, o += ys)
                *o = cf_shade(src[v >> 16], light);
        else                                           /* the middle cover: a scaled copy */
            for (; y < end; y++, v += step, o += ys)
                *o = src[v >> 16];
        /* smooth top and bottom edges: the first and last rows are only
         * partly covered, so blend them with what is behind */
#ifdef SIMULATOR
        cf_px_written += (uint32_t)((bottom < CF_H ? bottom : CF_H) - (top < 0 ? 0 : top)) + (uint32_t)(hh / 3);
#endif
        int edge_a = 256 - (int)((ftop - top) * 256);
        if (top >= 0 && top < CF_H)
            fb[top * ys + x * xs] = cf_mix(fb[top * ys + x * xs], cf_shade(src[0], light), edge_a > 255 ? 255 : edge_a);
        /* reflection on the floor, right under the cover: mirrored, from
         * about 36% down to nothing, easing out over a third of its height */
        int rlen = hh / 3;
        v = (s->h - 1) << 16;
        /* alpha = 92 (f/rlen)^2 light/256, with the division done once a
         * column: two divides a pixel cost milliseconds a frame on MIPS */
        uint32_t ascale = rlen ? ((uint32_t)(92 * light) << 8) / (uint32_t)(rlen * rlen) : 0;
        for (int i = 0; i < rlen && bottom + 1 + i < CF_H; i++, v -= step)
        {
            if (v < 0) break;
            uint32_t f = (uint32_t)(rlen - i);
            int alpha = (int)((f * f * ascale) >> 16);
            uint16_t *d = &fb[(bottom + 1 + i) * ys + x * xs];
            *d = cf_mix(*d, src[v >> 16], alpha);
        }
    }
}

static void cf_render(void)
{
    cf.frame++;
    for (int i = 0; i < CF_SLOTS; i++) cf.art[i].visible = false;
    memset(cf.fb, 0, (size_t)CF_W * CF_H * 2);           /* the black floor */
    if (!cf.n) return;
    int centre = cf_centre();
    int lo = centre - 7, hi = centre + 7;
    if (lo < 0) lo = 0;
    if (hi > cf.n - 1) hi = cf.n - 1;
    int ph = 0, near_ph = 0;                             /* for the meter */
    /* back to front: outermost first, the middle cover last */
    for (int ring = 7; ring >= 0; ring--)
        for (int side = -1; side <= 1; side += 2)
        {
            int k = centre + side * ring;
            if (ring == 0 && side > 0) continue;
            if (k < lo || k > hi) continue;
            int32_t d = k * CF_ONE - cf.pos;
            struct cf_place pl;
            cf_place(d, &pl);
            if (k == cf.open_at && cf.flip)
            {
                /* flipping over: turn the middle cover edge-on */
                float ang = cf.flip / (float)CF_ONE * 1.5707963f;
                pl.a = CF_S * cosf(ang);
                pl.b = CF_S * sinf(ang);
            }
            /* Read an existing card thumbnail before drawing the cover.
             * A fast swipe must never race the idle prefetch into a flash
             * of placeholder artwork. */
            const struct cf_art *s = cf_art_get(sl_lib.albums_by_title[k], true);
            if (s) cf_draw_cover(s, &pl);
            if (s && !s->real && !sl_art_no_cover(s->album))
            {
                if (ring == 0) ph = 2;
                else if (!ph) ph = 1;
                if (ring <= 3) near_ph++;
            }
        }
    uint64_t now = sl_perf_now_us();
    cfm_placeholders(ph, now);
    cfm.near_ph = near_ph;
    if (!near_ph && !cfm.real_us && cfm.on) cfm.real_us = (uint32_t)(now - cfm.open_at);
}

static void cf_labels(void)
{
    if (!cf.n) return;
    int centre = cf_centre();
    if (centre == cf.last_centre) return;
    cf.last_centre = centre;
    const struct sl_album *al = &sl_lib.albums[sl_lib.albums_by_title[centre]];
    lv_label_set_text(cf.title, al->title);
    lv_label_set_text(cf.artist, al->artist);
    if (!lv_slider_is_dragged(cf.scrub)) lv_slider_set_value(cf.scrub, centre, LV_ANIM_OFF);
}

static void cf_tick(lv_timer_t *t)
{
    (void)t;
    if (!cf.active) return;
    /* the library can change under an open Cover Flow (a first scan
     * finishing): follow it rather than read past its end */
    if (cf.n != sl_lib.n_albums)
    {
        cf.n = sl_lib.n_albums;
        if (cf.pos > (cf.n ? cf.n - 1 : 0) * CF_ONE) cf.pos = (cf.n ? cf.n - 1 : 0) * CF_ONE;
        if (cf.open_at >= cf.n) cf.open_at = cf.n ? cf.n - 1 : 0;
        cf.last_centre = -1;
        cf.dirty = true;
    }
    uint32_t now = lv_tick_get();
    /* New thumbnails were made: placeholders may have a cover now. */
    if (cf.art_seen != sl_art_made)
    {
        cf.art_seen = sl_art_made;
        for (int i = 0; i < CF_SLOTS; i++)
            if (!cf.art[i].real) cf.art[i].tried = false;
    }
    if (cf.pos != cf.last_pos) cf.heading = cf.pos > cf.last_pos ? 1 : -1;
    cf.last_pos = cf.pos;
    SL_AT("cf prefetch");
    cf_prefetch(cf.dragging || now - cf.moved_at < 120 ? 1 : 3);
    /* covers not made yet (a first visit): ask for the ones around the
     * middle before the rest of the library, nearest first */
    static uint32_t prefer_at;
    if (now - prefer_at >= 250)
    {
        prefer_at = now;
        int c = cf_centre();
        for (int ring = CF_REACH; ring >= 0; ring--)
            for (int side = -1; side <= 1; side += 2)
            {
                int k = c + side * ring;
                if ((ring == 0 && side > 0) || k < 0 || k >= cf.n) continue;
                int album = sl_lib.albums_by_title[k];
                bool real = false;
                for (int i = 0; i < CF_SLOTS && !real; i++)
                    real = cf.art[i].px && cf.art[i].album == album && cf.art[i].real;
                if (!real) sl_art_prefer(album);
            }
    }
    /* the scroll bar shows while the row moves, then fades, as on the iPod */
    if (cf.dirty || lv_slider_is_dragged(cf.scrub)) cf.moved_at = now;
    int want = now - cf.moved_at < 900 ? 255 : 0;
    if (cf.bar_opa != want)
    {
        cf.bar_opa += want > cf.bar_opa ? 64 : -24;
        if (cf.bar_opa > 255) cf.bar_opa = 255;
        if (cf.bar_opa < 0) cf.bar_opa = 0;
        lv_obj_set_style_opa(cf.scrub, (lv_opa_t)cf.bar_opa, 0);
    }
    /* the meter: from the row coming to rest to real covers around the middle */
    bool moving = cf.dragging || lv_anim_get(&cf, cf_set_pos) || lv_slider_is_dragged(cf.scrub);
    if (moving)
    {
        if (cfm.settle_at) cfm.settle_lost++;
        cfm.settle_at = 0;
        cfm.was_moving = true;
    }
    else if (cfm.was_moving) { cfm.was_moving = false; cfm.settle_at = sl_perf_now_us(); }
    if (cfm.settle_at && !cfm.near_ph)
    {
        cfm_add(&cfm.settle, (uint32_t)(sl_perf_now_us() - cfm.settle_at));
        cfm.settle_at = 0;
    }
    if (!cf.dirty) { cf.frame++; return; }
    cf.dirty = false;
    struct timespec t0, t1, c0, c1;
    SL_AT("cf render");
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    cf_render();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
    cfm_add(&cfm.render_cpu, (uint32_t)((c1.tv_sec - c0.tv_sec) * 1000000 + (c1.tv_nsec - c0.tv_nsec) / 1000));
    uint32_t us = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
    cfm_rendered((uint64_t)t0.tv_sec * 1000000u + t0.tv_nsec / 1000u, us, cf.pos, cf.dragging);
    cf.renders++; cf.render_us += us;
    if (us > cf.render_max) cf.render_max = us;
    cf_labels();
    lv_obj_invalidate(cf.canvas);
}

/* ------------------------------------------------------------- motion */
static void cf_set_pos(void *o, int32_t v) { (void)o; cf.pos = v; cf.dirty = true; }
static void cf_set_flip(void *o, int32_t v) { (void)o; cf.flip = v; cf.dirty = true; }
static void cf_go(int32_t to, int ms)
{
    if (to < 0) to = 0;
    if (to > (cf.n - 1) * CF_ONE) to = (cf.n - 1) * CF_ONE;
    lv_anim_delete(&cf, cf_set_pos);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &cf);
    lv_anim_set_values(&a, cf.pos, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, cf_set_pos);
    lv_anim_start(&a);
}

/* ------------------------------------------------------ the song list */
static void cf_song_cb(lv_event_t *e)
{
    /* the panel stays tappable while it shrinks away after cf_close_album */
    if (cf.swiped || !cf.list) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int album = sl_lib.albums_by_title[cf.open_at];
    const struct sl_album *al = &sl_lib.albums[album];
    if (i < 0 || i >= al->n_tracks) return;
    if (play_album(album, i, false))
    {
        for (uint32_t k = 0; k < lv_obj_get_child_count(cf.list); k++)
        {
            lv_obj_t *row = lv_obj_get_child(cf.list, k);
            lv_obj_t *mark = lv_obj_get_child(row, 0);
            lv_label_set_text(mark, (int)k == i ? LV_SYMBOL_VOLUME_MAX : "");
        }
    }
}
/* Squashed to (nearly) nothing, the panel is hidden instead: LVGL 9.2
 * gives a transformed object of zero width a draw layer that is never
 * finished, so one flip left some twenty of them behind, holding the
 * panel's labels until they were freed (a use after free, and memory). */
#define CF_PANEL_MIN_SCALE 8                 /* of 256: 12 px of a 400 px panel */
static void cf_panel_scale(void *o, int32_t v)
{
    lv_obj_update_flag(o, LV_OBJ_FLAG_HIDDEN, v < CF_PANEL_MIN_SCALE);
    lv_obj_set_style_transform_scale_x(o, v < CF_PANEL_MIN_SCALE ? CF_PANEL_MIN_SCALE : v, 0);
}
static void cf_flip_back_done(lv_anim_t *a)
{
    (void)a;
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, &cf);
    lv_anim_set_values(&b, CF_ONE, 0);
    lv_anim_set_duration(&b, 200);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&b, cf_set_flip);
    lv_anim_start(&b);
}
static void cf_close_album(int direction)
{
    if (!cf.panel) return;
    lv_obj_t *panel = cf.panel;
    cf.panel = cf.list = NULL;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, panel);
    lv_anim_set_values(&a, 256, 0);
    lv_anim_set_duration(&a, 170);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, cf_panel_scale);
    lv_anim_set_completed_cb(&a, cf_flip_back_done);
    lv_anim_start(&a);
    lv_obj_delete_delayed(panel, 200);
    /* keep swiping: the row carries on the way the list was pushed */
    if (direction) cf_go((cf.open_at + direction) * CF_ONE, 520);
}
static void cf_panel_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    if (!lv_indev_active()) return;
    lv_indev_get_point(lv_indev_active(), &pt);
    if (code == LV_EVENT_PRESSED) { cf.drag_x0 = pt.x; cf.swiped = false; }
    else if (code == LV_EVENT_PRESSING && !cf.swiped)
    {
        int dx = pt.x - cf.drag_x0;
        if (dx > 60 || dx < -60)
        {
            cf.swiped = true;
            lv_indev_wait_release(lv_indev_active());
            cf_close_album(dx < 0 ? 1 : -1);
        }
    }
}
static void cf_panel_open(lv_anim_t *a)
{
    (void)a;
    int album = sl_lib.albums_by_title[cf.open_at];
    const struct sl_album *al = &sl_lib.albums[album];
    lv_obj_t *p = box(cf.scr);
    cf.panel = p;
    /* the back of the cover, as on the iPod: a glossy grey title bar over
     * a black list, the pressed song lit in that deep blue */
    lv_obj_set_size(p, 400, 300);
    if (cf.vert) lv_obj_set_pos(p, (W - 400) / 2, (CF_W - 300) / 2);
    else lv_obj_set_pos(p, (CF_W - 400) / 2, CF_YC - 150);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x050505), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0x55565b), 0);
    lv_obj_set_style_radius(p, 3, 0);
    lv_obj_set_style_clip_corner(p, true, 0);
    lv_obj_t *bar = box(p);
    lv_obj_set_size(bar, 400, 64);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x5b5c61), 0);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0x1d1e21), 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(p, 200, 0);
    cf_panel_scale(p, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p, cf_panel_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *t = lv_label_create(p);
    lv_label_set_text(t, al->title);
    lv_obj_set_style_text_font(t, &ss_semibold_21, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_base_dir(t, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_pos(t, 16, 10);
    lv_obj_set_width(t, 368);
    label_lines(t, 1);
    lv_obj_t *ar = lv_label_create(p);
    lv_label_set_text(ar, al->artist);
    lv_obj_set_style_text_font(ar, &ss_regular_16, 0);
    lv_obj_set_style_text_color(ar, lv_color_hex(0x9aa0aa), 0);
    lv_obj_set_style_base_dir(ar, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_pos(ar, 16, 38);
    lv_obj_set_width(ar, 368);
    label_lines(ar, 1);
    lv_obj_set_style_text_color(ar, lv_color_hex(0xc4c5ca), 0);
    lv_obj_t *line = box(p);
    lv_obj_set_size(line, 400, 1);
    lv_obj_set_pos(line, 0, 64);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_t *list = lv_obj_create(p);
    cf.list = list;
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 65);
    lv_obj_set_size(list, 400, 234);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_EVENT_BUBBLE);
    struct mp3entry *now = something_playing() ? audio_current_track() : NULL;
    if (!al->n_tracks)
    {
        lv_obj_t *none = lv_label_create(list);
        lv_label_set_text(none, "No songs on this album");
        lv_obj_set_style_text_font(none, &ss_regular_18, 0);
        lv_obj_set_style_text_color(none, lv_color_hex(0x9aa0aa), 0);
        lv_obj_set_style_pad_all(none, 16, 0);
    }
    for (int i = 0; i < al->n_tracks; i++)
    {
        const struct sl_track *tr = &sl_lib.tracks[al->tracks[i]];
        lv_obj_t *row = box(list);
        lv_obj_set_size(row, 400, 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(i & 1 ? 0x0b0b0c : 0x121214), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3a8ee8), LV_STATE_PRESSED);
        lv_obj_set_style_bg_grad_color(row, lv_color_hex(0x0f5fc8), LV_STATE_PRESSED);
        lv_obj_set_style_bg_grad_dir(row, LV_GRAD_DIR_VER, LV_STATE_PRESSED);
        lv_obj_t *mark = lv_label_create(row);                /* child 0: playing mark */
        lv_label_set_text(mark, now && sl_track_path_is(al->tracks[i], now->path) ? LV_SYMBOL_VOLUME_MAX : "");
        lv_obj_set_style_text_font(mark, &sl_sym_16, 0);
        lv_obj_set_style_text_color(mark, lv_color_hex(0x5aa6ff), 0);
        lv_obj_align(mark, LV_ALIGN_LEFT_MID, 12, 0);
        char b[16];
        snprintf(b, sizeof b, "%d", tr->num > 0 ? tr->num : i + 1);
        lv_obj_t *n = lv_label_create(row);
        lv_label_set_text(n, b);
        lv_obj_set_style_text_font(n, &ss_regular_16, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(0x80858f), 0);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 36, 0);
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, tr->title);
        lv_obj_set_style_text_font(nm, &ss_regular_18, 0);
        lv_obj_set_style_text_color(nm, lv_color_white(), 0);
        lv_obj_set_style_base_dir(nm, LV_BASE_DIR_AUTO, 0);
        lv_obj_set_width(nm, 270);
        label_lines(nm, 1);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 66, 0);
        fmt_time(b, sizeof b, tr->len);
        lv_obj_t *len = lv_label_create(row);
        lv_label_set_text(len, b);
        lv_obj_set_style_text_font(len, &ss_regular_16, 0);
        lv_obj_set_style_text_color(len, lv_color_hex(0x80858f), 0);
        lv_obj_align(len, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cf_song_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, p);
    lv_anim_set_values(&b, 0, 256);
    lv_anim_set_duration(&b, 190);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&b, cf_panel_scale);
    lv_anim_start(&b);
}
static void cf_open_album(void)
{
    int centre = cf_centre();
    if (cf.panel || centre < 0 || centre >= cf.n) return;
    sl_doing_is("Cover Flow's album", sl_lib.albums[sl_lib.albums_by_title[centre]].title);
    lv_anim_delete(&cf, cf_set_pos);
    cf.pos = centre * CF_ONE;
    cf.open_at = centre;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &cf);
    lv_anim_set_values(&a, 0, CF_ONE);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, cf_set_flip);
    lv_anim_set_completed_cb(&a, cf_panel_open);
    lv_anim_start(&a);
}

/* -------------------------------------------------------------- touch */
static void cf_canvas_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (!lv_indev_active()) return;
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    if (cf.vert)                         /* along the row, and across it, as the renderer sees them */
    {
        int32_t across = pt.x - cf.cx0;
        pt.x = pt.y;
        pt.y = across;
    }
    uint32_t now = lv_tick_get();
    if (code == LV_EVENT_PRESSED)
    {
        lv_anim_delete(&cf, cf_set_pos);
        cf.drag_x0 = cf.last_x = pt.x;
        cf.drag_pos0 = cf.pos;
        cf.last_t = now;
        cf.vel = 0;
        cf.dragging = false;
    }
    else if (code == LV_EVENT_PRESSING)
    {
        int dx = pt.x - cf.drag_x0;
        if (!cf.dragging && (dx > 8 || dx < -8))
        {
            cf.dragging = true;
            if (cf.panel) { cf_close_album(dx < 0 ? 1 : -1); lv_indev_wait_release(lv_indev_active()); return; }
        }
        if (!cf.dragging || cf.panel) return;
        int32_t p = cf.drag_pos0 - dx * CF_ONE / CF_DRAG;
        int32_t max = (cf.n - 1) * CF_ONE;
        if (p < 0) p /= 3;                            /* rubber band at the ends */
        if (p > max) p = max + (p - max) / 3;
        if (p != cf.pos && !cfm.pending_touch) cfm.pending_touch = cfm.touch_us;
        uint32_t dt = now - cf.last_t;
        if (dt >= 10)
        {
            int v = -(pt.x - cf.last_x) * CF_ONE / CF_DRAG * 1000 / (int)dt;
            cf.vel = (cf.vel + v * 2) / 3;
            cf.last_x = pt.x;
            cf.last_t = now;
        }
        cf_set_pos(NULL, p);
    }
    else if (code == LV_EVENT_RELEASED)
    {
        if (cf.dragging)
        {
            cf.dragging = false;
            /* flick: carry on for a moment, then settle on an album */
            int32_t target = cf.pos + (now - cf.last_t < 80 ? cf.vel / 3 : 0);
            target = (target + CF_ONE / 2) / CF_ONE * CF_ONE;
            int dist = target - cf.pos;
            if (dist < 0) dist = -dist;
            cf_go(target, 300 + dist * 220 / CF_ONE > 900 ? 900 : 300 + dist * 220 / CF_ONE);
            return;
        }
        if (cf.panel) { cf_close_album(0); return; }
        /* tap: the middle cover opens, a side cover comes to the middle */
        int centre = cf_centre();
        int dx = pt.x - CF_W / 2;
        if (dx > -CF_S / 2 && dx < CF_S / 2 && pt.y < CF_YC + CF_S / 2) cf_open_album();
        else if (dx >= CF_S / 2) cf_go((centre + 1 + (dx - CF_S / 2) / (CF_STEP + 20)) * CF_ONE, 450);
        else cf_go((centre - 1 + (dx + CF_S / 2) / (CF_STEP + 20)) * CF_ONE, 450);
    }
}
static void cf_scrub_cb(lv_event_t *e)
{
    (void)e;
    if (cf.panel) cf_close_album(0);
    lv_anim_delete(&cf, cf_set_pos);
    cf_set_pos(NULL, lv_slider_get_value(cf.scrub) * CF_ONE);
}
static void cf_done_cb(lv_event_t *e) { (void)e; cf_close(); }

/* -------------------------------------------------------- open, close */
static int cf_start = -1;                 /* albums_by_title index to open on, once */
/* Cover Flow's least: its frame and the fewest covers (on screen and one
 * either side). Short of the music's floor it opens with those alone, as
 * long as they leave the R1 above where it swaps; only under that is the
 * user asked, never turned away with a toast (a test R1 over Bluetooth,
 * 4 Nov: "Not enough memory for Cover Flow" at 7-9 MB free). */
#define CF_MIN_COVERS 18
#define CF_NEED_KB (W * H * 2 / 1024 + CF_MIN_COVERS * (CF_ART * CF_ART * 2 / 1024))
static bool cf_forced;                    /* asked and answered: open with what there is */
static long cf_stop_kb;
static void cf_open(void);
static void cf_after_stop(lv_timer_t *t)
{
    lv_timer_delete(t);
    long kb = sl_mem_available_kb();
    sl_log("cover flow: stopping the music freed %ld KiB (%ld -> %ld)", kb - cf_stop_kb, cf_stop_kb, kb);
    cf_forced = true;
    cf_open();
    cf_forced = false;
}
static void cf_short_pick(int k)
{
    if (k != 0) return;                   /* Cancel */
    cf_stop_kb = sl_mem_available_kb();
    audio_stop();
    lv_timer_create(cf_after_stop, 800, NULL);   /* what the stop gave back, a moment after */
}
static void cf_short_row_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    acts_close();
    cf_short_pick(k);
}
/* pick_sheet's look, its words wrapped: a sentence, and a long language */
static void cf_short_ask(long kb)
{
    static const char *const opts[] = { "Stop the music and open", "Cancel" };
    sl_log("cover flow: %ld KiB free, %d needed above %d: asked", kb, CF_NEED_KB, SL_MEM_SWAP_KB);
    sl_doing_is("Cover Flow's memory question", NULL);
    acts_begin();
    lv_obj_t *hd = text(acts, "Cover Flow needs more memory than is free while music plays.", &ss_semibold_18, C_SUB);
    lv_obj_set_width(hd, lv_pct(100));
    lv_obj_set_style_pad_all(hd, 12, 0);
    for (int k = 0; k < 2; k++)
    {
        lv_obj_t *r = box(acts);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(r, 64, 0);
        lv_obj_set_style_pad_hor(r, 16, 0);
        lv_obj_set_style_pad_ver(r, 12, 0);
        lv_obj_set_style_radius(r, SL_R_XL - SL_FLOAT, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_TEXT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_10, LV_STATE_PRESSED);
        char low[96];
        lv_obj_t *l = text(r, sl_zune() ? sl_lower(sl_tr(opts[k]), low, sizeof low) : opts[k], &ss_semibold_23,
                           k ? 0xffffff : sl_accent_on(C_SHEET, 45));
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, sl_ui_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        on_click(r, cf_short_row_cb, (void *)(intptr_t)k);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    acts_show();
}
static void cf_open(void)
{
    if (cf.active) return;
    if (!sl_lib.n_albums) { toast("No albums in your library yet"); return; }
    sl_doing_is("Cover Flow", NULL);
    if (!qp_glass_px && !sl_mem_room(W * H * 2 / 1024, "Cover Flow"))
    {
        long kb = sl_mem_available_kb();
        if (kb >= 0 && kb < SL_MEM_SWAP_KB + CF_NEED_KB && !cf_forced && (audio_status() & AUDIO_STATUS_PLAY))
        {
            cf_short_ask(kb);
            return;
        }
        sl_log("cover flow: lighter, %ld KiB free: the fewest covers", kb);
    }
    if (!qp_glass_px) qp_glass_px = malloc((size_t)W * H * 2);
    if (!qp_glass_px) { cf_short_ask(sl_mem_available_kb()); return; }
    /* covers as memory allows: the music's floor (more over Bluetooth)
     * and 2 MB for the code stay free */
    long kb = sl_mem_available_kb();
    cf_cap = kb < 0 ? CF_SLOTS : (int)((kb - sl_mem_floor_kb() - 2048) / (CF_ART * CF_ART * 2 / 1024));
    if (cf_cap > CF_SLOTS) cf_cap = CF_SLOTS;
    if (cf_cap < 18) cf_cap = 18;        /* what is on screen and a little either side */
    sl_log("cover flow: %ld KiB free, up to %d covers held", kb, cf_cap);
    cfm_begin();
    cfm.avail_open = sl_mem_available_kb();
    cf_opening = true;
    qp_glass_ok = false;                  /* the buffer is ours until we leave */
    cf.fb = qp_glass_px;
    cf.vert = global_settings.sloppy_cf_vertical == 1;
    cf.cx0 = W / 2 - CF_YC;              /* vertical: the covers centred across the screen */
    for (int i = 0; i < CF_SLOTS; i++)   /* covers kept from a visit the other way up */
        if (cf.art[i].px && cf.art[i].vert != cf.vert) { free(cf.art[i].px); cf.art[i].px = NULL; }
    cf.n = sl_lib.n_albums;
    cf.home = lv_screen_active();
    cf.active = true;
    sl_perf_begin("cover flow");
    for (int i = 0; i < CF_SLOTS; i++)
        if (!cf.art[i].real) cf.art[i].tried = false;
    cf.panel = cf.list = NULL;
    cf.flip = 0;
    cf.last_centre = -1;
    /* start on the album that is playing */
    cf.pos = 0;
    struct mp3entry *id3 = something_playing() ? audio_current_track() : NULL;
    int track = id3 ? sl_lib_find_path(id3->path) : -1;
    for (int i = 0; track >= 0 && i < cf.n; i++)
        if (sl_lib.albums_by_title[i] == sl_lib.tracks[track].album) { cf.pos = i * CF_ONE; break; }
    if (cf_start >= 0 && cf_start < cf.n) cf.pos = cf_start * CF_ONE;
    cf_start = -1;

    statusbar_show(false);
    if (!cf.vert) sl_set_rotation(LV_DISPLAY_ROTATION_90);
    cf.scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(cf.scr);
    lv_obj_set_style_bg_color(cf.scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cf.scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cf.scr, LV_OBJ_FLAG_SCROLLABLE);
    cf.canvas = lv_canvas_create(cf.scr);
    if (cf.vert)
    {
        lv_canvas_set_buffer(cf.canvas, cf.fb, CF_H, CF_W, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(cf.canvas, cf.cx0, 0);
    }
    else lv_canvas_set_buffer(cf.canvas, cf.fb, CF_W, CF_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(cf.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cf.canvas, cf_canvas_cb, LV_EVENT_ALL, NULL);
    if (cf.vert)
    {
        /* the names sit on a dark fade at the foot, over the last covers */
        lv_obj_t *foot = box(cf.scr);
        lv_obj_set_size(foot, W, 150);
        lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(foot, lv_color_black(), 0);
        lv_obj_set_style_bg_main_opa(foot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_color(foot, lv_color_black(), 0);
        lv_obj_set_style_bg_grad_opa(foot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_stop(foot, 0, 0);
        lv_obj_set_style_bg_grad_stop(foot, 120, 0);
        lv_obj_set_style_bg_grad_dir(foot, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    }
    cf.title = lv_label_create(cf.scr);
    lv_obj_set_style_text_font(cf.title, &ss_semibold_26, 0);
    lv_obj_set_style_text_color(cf.title, lv_color_white(), 0);
    lv_obj_set_style_text_align(cf.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_base_dir(cf.title, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_width(cf.title, cf.vert ? W - 40 : 560);
    label_lines(cf.title, 1);
    /* the two lines stacked by their heights (50 and 410 at Standard): at
     * the larger text sizes the artist ran into the title */
    int cf_title_h = lv_font_get_line_height(&ss_semibold_26), cf_artist_h = lv_font_get_line_height(&ss_regular_18);
    if (cf.vert) lv_obj_align(cf.title, LV_ALIGN_BOTTOM_MID, 0, -(22 + cf_artist_h + 6));
    else lv_obj_align(cf.title, LV_ALIGN_TOP_MID, 0, 376);
    cf.artist = lv_label_create(cf.scr);
    lv_obj_set_style_text_font(cf.artist, &ss_regular_18, 0);
    lv_obj_set_style_text_color(cf.artist, lv_color_hex(0x8e8e93), 0);
    lv_obj_set_style_text_align(cf.artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_base_dir(cf.artist, LV_BASE_DIR_AUTO, 0);
    lv_obj_set_width(cf.artist, cf.vert ? W - 40 : 560);
    label_lines(cf.artist, 1);
    if (cf.vert) lv_obj_align(cf.artist, LV_ALIGN_BOTTOM_MID, 0, -22);
    else lv_obj_align(cf.artist, LV_ALIGN_TOP_MID, 0, 376 + cf_title_h + 3);
    cf.scrub = lv_slider_create(cf.scr);
    if (cf.vert)
    {
        /* down the right edge, the first album at the top */
        lv_obj_set_size(cf.scrub, 3, 520);
        lv_obj_align(cf.scrub, LV_ALIGN_RIGHT_MID, -14, -40);
        lv_slider_set_range(cf.scrub, cf.n > 1 ? cf.n - 1 : 1, 0);
    }
    else
    {
        lv_obj_set_size(cf.scrub, 440, 3);
        lv_obj_align(cf.scrub, LV_ALIGN_BOTTOM_MID, 0, -18);
        lv_slider_set_range(cf.scrub, 0, cf.n > 1 ? cf.n - 1 : 1);
    }
    lv_obj_set_style_bg_color(cf.scrub, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cf.scrub, 40, 0);
    lv_obj_set_style_radius(cf.scrub, SL_R_FULL, 0);
    lv_obj_set_style_bg_opa(cf.scrub, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cf.scrub, lv_color_hex(0xd8d8dc), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(cf.scrub, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(cf.scrub, 3, LV_PART_KNOB);
    lv_obj_set_style_pad_hor(cf.scrub, cf.vert ? 2 : 18, LV_PART_KNOB);
    lv_obj_set_style_pad_ver(cf.scrub, cf.vert ? 18 : 2, LV_PART_KNOB);
    lv_obj_set_style_opa(cf.scrub, LV_OPA_TRANSP, 0);
    cf.bar_opa = 0;
    lv_obj_set_ext_click_area(cf.scrub, 20);
    lv_obj_add_event_cb(cf.scrub, cf_scrub_cb, LV_EVENT_VALUE_CHANGED, NULL);
    cf.done = lv_obj_create(cf.scr);
    lv_obj_remove_style_all(cf.done);
    lv_obj_set_size(cf.done, 84, 40);
    lv_obj_set_pos(cf.done, 10, 10);
    lv_obj_set_style_radius(cf.done, 8, 0);
    lv_obj_set_style_bg_color(cf.done, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cf.done, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(cf.done, 40, LV_STATE_PRESSED);
    lv_obj_t *dl = lv_label_create(cf.done);           /* quiet, out of the picture */
    lv_label_set_text(dl, sl_tr("Done"));
    lv_obj_set_style_text_font(dl, &ss_semibold_18, 0);
    lv_obj_set_style_text_color(dl, lv_color_white(), 0);
    lv_obj_set_style_text_opa(dl, 150, 0);
    lv_obj_center(dl);
    lv_obj_add_event_cb(cf.done, cf_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_ext_click_area(cf.done, 14);

    /* the row glides in from the right and settles on the album */
    int32_t target = cf.pos;
    cf.pos = target + 2 * CF_ONE > (cf.n - 1) * CF_ONE ? (cf.n - 1) * CF_ONE : target + 2 * CF_ONE;
    /* Read every cover the glide shows (7 either side of both ends), the
     * middle first. Cached covers from the last visit cost nothing. */
    int c = target / CF_ONE, lo = LV_MAX(c - 7, 0), hi = LV_MIN(cf.pos / CF_ONE + 7, cf.n - 1);
    for (int ring = 0; ring <= hi - lo; ring++)
        for (int side = 1; side >= -1; side -= 2)
        {
            int k = c + side * ring;
            if (k >= lo && k <= hi && !(ring == 0 && side < 0))
                cf_art_get(sl_lib.albums_by_title[k], true);
        }
    cf.dirty = true;
    cf_tick(NULL);
    cf_opening = false;
    cf.timer = lv_timer_create(cf_tick, 16, NULL);
    lv_screen_load(cf.scr);
    cf_go(target, 700);
    cfm.open_us = (uint32_t)(sl_perf_now_us() - cfm.open_at);
    sl_log("cover flow: %d albums", cf.n);
}

static bool cf_is_open(void) { return cf.active || spa.active || vz.active; }
static int cf_covers_held(void)
{
    int n = 0;
    for (int i = 0; i < CF_SLOTS; i++) n += cf.art[i].px != NULL;
    return n;
}   /* Spectrum and the visualizer too: their own screens */
/* The screen GabagoolOS builds on. Cover Flow has one of its own, deleted
 * when it closes: the home built again under it (a pull to refresh that
 * finished as the player turned), Now Playing, the mini player, Up Next
 * were freed with it and left dangling. They go on the home screen. */
static lv_obj_t *ui_screen(void)
{
    return cf.active ? cf.home : spa.active ? spa.home : vz.active ? vz.home : lv_screen_active();
}
/* Still long enough to make a cover between frames without a hitch. */
static bool cf_settled(void)
{
    return cf.active && !cf.dragging && !cf.dirty && lv_tick_get() - cf.moved_at > 300 &&
           !lv_slider_is_dragged(cf.scrub);
}

static void cf_close(void)
{
    spa_close();
    vz_close();
    if (!cf.active) return;
    cf.active = false;
    sl_doing_is("back from Cover Flow", NULL);
    cf.closed_at = lv_tick_get();
    if (!cf.closed_at) cf.closed_at = 1;
    lv_anim_delete(&cf, NULL);
    if (cf.timer) lv_timer_delete(cf.timer);
    cf.timer = NULL;
    if (!cf.vert) sl_set_rotation(LV_DISPLAY_ROTATION_0);
    lv_screen_load(cf.home);
    statusbar_show(true);
    lv_obj_delete(cf.scr);
    cf.scr = cf.canvas = cf.panel = cf.list = cf.ph_canvas = NULL;
    lv_obj_invalidate(lv_screen_active());
    sl_log("cover flow closed: %lu frames, %lu us average, %lu us worst",
           (unsigned long)cf.renders, (unsigned long)(cf.renders ? cf.render_us / cf.renders : 0),
           (unsigned long)cf.render_max);
    sl_perf_end();
    cfm_end();
    sl_perf_memory("cover flow closed");
    cf.renders = cf.render_us = cf.render_max = 0;
}

/* Open on one album: the tiles home's Cover Flow tile. */
static void cf_open_on(int index)
{
    cf_start = index;
    cf_open();
    cf_start = -1;
}
