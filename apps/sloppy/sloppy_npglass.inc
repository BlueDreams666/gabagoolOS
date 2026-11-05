/*
 *  Now Playing's glass (Customization > Now Playing > Glass, the default).
 *  Below the cover lies a lake: the cover reflected in it, upside down from
 *  the cover's foot, as clear as calm water there and softening, rippling
 *  a little and sinking into a deep tint of the cover the farther down it
 *  goes; over it all, a light frosted glass the controls sit on. How dark
 *  the water is comes from the cover: a dark one shows through nearly as
 *  it is, a bright one is held down until white type reads on it.
 *
 *  Built once per cover, not per frame: sampled at 1/4 size, once lightly
 *  and once well blurred and mixed by depth, coloured, then grown back
 *  with bilinear smoothing, the ripple and an ordered dither so RGB565
 *  does not band. One RGB565A8 image; its alpha only fades the frost in
 *  over the cover's foot, so the two meet without a seam. Classic is the
 *  black panel under the cover, as before.
 */

#define NG_FADE   120                    /* rows over the cover it fades in across */
#define NG_TOP    (W - NG_FADE)
#define NG_ROWS   (H - NG_TOP)
/* free memory a reflection is made with (each is 630 KB): above the 6 MB
 * line where the low-memory release would take it straight back. The R1
 * usually runs with 6 to 8 MB free, so 16 MB meant no reflection at all. */
#define NG_MIN_FREE_KB (sl_mem_floor_kb() + 1024)   /* a backdrop with a MB to spare over the music's floor */
#define NG_S      4
#define NG_GW     (W / NG_S)
#define NG_GH     ((NG_ROWS + NG_S - 1) / NG_S + 1)

static uint8_t *np_glow_buf;             /* the RGB565 plane, then the alpha plane */
static lv_image_dsc_t np_glow_img;
static lv_obj_t *np_glow;

static bool np_glass(void) { return (global_settings.sloppy_np_style == 0 || np_wave()) && !sl_zune(); }   /* Zune: black under the art; the waveform sits on the glass */
static bool vnl_on(void);
/* Centred: a smaller cover, rounded and lifted, over the whole screen in
 * its own blurred colour (Customization > Now Playing > Centred). The
 * turntable keeps the square. */
static bool np_centred(void) { return global_settings.sloppy_np_style == 2 && !sl_zune() && !vnl_on(); }
static const uint16_t *lyr_backdrop(void);   /* sloppy_lyrics.inc: the lyrics' own, while they show */
static void lyr_bd_sync(bool force);

/* The round buttons over the cover (close, turntable, visualizer): lenses
 * of it, the cover bent round their rims; on the turntable, or classic, as
 * before. */
static lv_obj_t *np_down, *vnl_btn, *vz_btn;
static uint16_t *nc_bd_px;               /* centred: the screen's backdrop, W x H */
static uint16_t nc_small[(W / 2) * (H / 2) * 3 / 2];   /* short of memory: the backdrop at half size, RGB565
                                                        * then an opaque alpha plane (LVGL stretches only that) */
static bool nc_small_on;
static void ng_buttons(void)
{
    static lv_obj_t *lens[3];
    static uint8_t buf[3][80 * 80 * 3];   /* 56 and a 12 margin for the shadow */
    static uint16_t flat;
    lv_obj_t *btns[3] = { np_down, vnl_btn, vz_btn };
    bool pic = np_art_img && !lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN) && np_img.data && np_img.header.w;
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *b = btns[i];
        if (!b) continue;
        if (lens[i] && lv_obj_get_parent(lens[i]) != b) lens[i] = NULL;   /* rebuilt since */
        if ((!np_glass() && !(np_centred() && nc_bd_px)) || vnl_on() || !np_art)
        {
            if (lens[i]) lv_obj_add_flag(lens[i], LV_OBJ_FLAG_HIDDEN);
            if (b != vnl_btn || !vnl_on()) lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);   /* the turntable's own colour when on */
            continue;
        }
        struct sl_lens_src src = { &flat, 1, 1, 1, 0, 0, 215, NULL, 0 };
        const uint16_t *bd = lyr_backdrop();   /* over the lyrics: their backdrop, not the cover under it */
        if (!bd && np_centred()) bd = nc_bd_px;
        if (bd)
        {
            src.px = bd;
            src.stride = src.w = W;
            src.h = bd == nc_bd_px ? H : W;
        }
        else if (pic)
        {
            src.px = (const uint16_t *)np_img.data;
            src.stride = np_img.header.stride / 2;
            src.w = np_img.header.w; src.h = np_img.header.h;
            src.x0 = (W - src.w) / 2; src.y0 = (W - src.h) / 2;
        }
        else
        {
            lv_color_t c = lv_obj_get_style_bg_color(np_art, 0);
            flat = (uint16_t)((c.red >> 3) << 11 | (c.green >> 2) << 5 | (c.blue >> 3));
        }
        if (!lens[i])
        {
            lens[i] = lv_canvas_create(b);
            lv_obj_remove_flag(lens[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_move_to_index(lens[i], 0);
            sl_overflow(b, 12);
        }
        struct sl_lens_look k = sl_lens_pane;   /* frosted over the sharp cover */
        k.frost = 5;
        k.lift = 0.12f;
        sl_lens(&src, lv_obj_get_x(b) - 12, lv_obj_get_y(b) - 12, 80, 80, 12, 28, &k,
                (uint16_t *)buf[i], buf[i] + 80 * 80 * 2);
        lv_canvas_set_buffer(lens[i], buf[i], 80, 80, LV_COLOR_FORMAT_RGB565A8);
        lv_obj_set_pos(lens[i], -12, -12);
        lv_obj_remove_flag(lens[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(lens[i]);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    }
}
/* A box blur of radius r along one line of the small grid. */
static void ng_blur(uint8_t *p, int n, int step, int r)
{
    uint8_t t[NG_GH > NG_GW ? NG_GH : NG_GW];
    for (int i = 0; i < n; i++) t[i] = p[i * step];
    for (int i = 0; i < n; i++)
    {
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += t[i + k < 0 ? 0 : i + k >= n ? n - 1 : i + k];
        p[i * step] = (uint8_t)(sum / (2 * r + 1));
    }
}

/* From the cover (w x h RGB565, stride in pixels), or for a song with no
 * cover from its colour c. */
/* The lake for one cover into out (W x NG_ROWS, RGB565 then the alpha
 * plane): the cover's own for Now Playing, or a neighbour's while a
 * swipe brings it in. */
static uint8_t ng_small[NG_GW * NG_GH * 3];   /* the reflection at the grid's size: RGB565, then alpha */
static bool ng_small_on;                      /* np_glow shows it */
static bool ng_force_small;                   /* the harness: as if short of memory */
static void ng_paint(uint8_t *out, const uint16_t *src, int w, int h, int stride, lv_color_t c)
{
    static uint8_t g[3][NG_GW * NG_GH];
    for (int gy = 0; gy < NG_GH; gy++)
        for (int gx = 0; gx < NG_GW; gx++)
        {
            int r = 0, gg = 0, b = 0;
            for (int s = 0; s < 4; s++)
            {
                int vy = NG_TOP + gy * NG_S + (s >> 1) * (NG_S / 2) + NG_S / 4;
                int vx = gx * NG_S + (s & 1) * (NG_S / 2) + NG_S / 4;
                /* under the cover: its mirror, as the cover is shown (centred,
                 * one to one, the art box's colour round a smaller picture),
                 * mirrored at the picture's own foot, so a letterboxed cover
                 * meets its reflection instead of a black strip */
                int foot = src && h < W ? (W - h) / 2 + h : W;
                int cy = vy < foot ? vy : 2 * foot - 1 - vy;
                int ix = vx - (W - w) / 2, iy = (cy < 0 ? 0 : cy) - (W - h) / 2;
                if (!src || ix < 0 || ix >= w || iy < 0 || iy >= h) { r += c.red; gg += c.green; b += c.blue; continue; }
                uint16_t p = src[iy * stride + ix];
                r += (p >> 11) << 3; gg += ((p >> 5) & 63) << 2; b += (p & 31) << 3;
            }
            int i = gy * NG_GW + gx;
            g[0][i] = (uint8_t)(r / 4); g[1][i] = (uint8_t)(gg / 4); g[2][i] = (uint8_t)(b / 4);
        }
    /* the water: clear near the cover's foot, softer the deeper it goes */
    static uint8_t deep[3][NG_GW * NG_GH];
    memcpy(deep, g, sizeof deep);
    for (int ch = 0; ch < 3; ch++)
    {
        for (int y = 0; y < NG_GH; y++) ng_blur(&g[ch][y * NG_GW], NG_GW, 1, 1);
        for (int x = 0; x < NG_GW; x++) ng_blur(&g[ch][x], NG_GH, NG_GW, 1);
        for (int pass = 0; pass < 3; pass++)
        {
            for (int y = 0; y < NG_GH; y++) ng_blur(&deep[ch][y * NG_GW], NG_GW, 1, 3);
            for (int x = 0; x < NG_GW; x++) ng_blur(&deep[ch][x], NG_GH, NG_GW, 3);
        }
    }
    /* more colour, the depth's blur mixed in, and the water's own tint:
     * the deep blur's mean, darkened, which the reflection sinks into */
    static uint16_t hist[256];
    memset(hist, 0, sizeof hist);
    long n = 0, tr = 0, tg = 0, tb = 0;
    for (int gy = 0; gy < NG_GH; gy++)
    {
        int y = gy * NG_S, dep = y <= NG_FADE ? 0 : (y - NG_FADE) * 256 / (NG_ROWS - NG_FADE);
        int soft = y <= NG_FADE ? 40 : 40 + 216 * (dep > 48 ? 48 : dep) / 48;   /* of 256: the deep blur, all of it by the seek bar (a busy cover's pattern stayed sharp behind the words) */
        for (int gx = 0; gx < NG_GW; gx++)
        {
            int i = gy * NG_GW + gx, v[3];
            for (int ch = 0; ch < 3; ch++) v[ch] = g[ch][i] + ((deep[ch][i] - g[ch][i]) * soft >> 8);
            int gray = (v[0] * 77 + v[1] * 150 + v[2] * 29) >> 8;
            /* more colour in the water only: over the cover's foot the frost
             * keeps the cover's own, or a flat one shows a tinted band */
            int boost = y <= NG_FADE ? 0 : y >= NG_FADE + 16 ? 16 : y - NG_FADE;
            for (int ch = 0; ch < 3; ch++)
            {
                int c = gray + (v[ch] - gray) * (48 + boost) / 48;
                g[ch][i] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
            }
            if (y >= NG_FADE)
            {
                hist[(g[0][i] * 77 + g[1][i] * 150 + g[2][i] * 29) >> 8]++;
                tr += deep[0][i]; tg += deep[1][i]; tb += deep[2][i]; n++;
            }
        }
    }
    if (!n) n = 1;
    int water[3] = { (int)(tr / n) * 70 >> 8, (int)(tg / n) * 70 >> 8, (int)(tb / n) * 70 >> 8 };
    /* as dark as this cover needs for white type: the bright parts behind
     * the words (the 85th percentile) brought down to about 100 of 255, so
     * the grey under-lines read at 4.5:1 on a pale cover too (120 left
     * them near 3.5:1) */
    int bright = 255;
    for (long acc = 0; bright > 0 && (acc += hist[bright]) < n * 15 / 100; bright--) {}
    int body = bright > 0 ? 100 * 256 / bright : 228;
    body = body < 60 ? 60 : body > 228 ? 228 : body;
    for (int gy = 0; gy < NG_GH; gy++)
    {
        int y = gy * NG_S, dep = y <= NG_FADE ? 0 : (y - NG_FADE) * 256 / (NG_ROWS - NG_FADE);
        /* over the cover's foot the frost is barely darker; in the water
         * the reflection keeps its shapes near the top, then sinks into
         * the tint, all of it lifted a touch by the frost on top */
        int ease = y <= NG_FADE - 8 ? 0 : y >= NG_FADE + 14 ? 256 : (y - NG_FADE + 8) * 256 / 22;
        ease = ease * ease * (768 - 2 * ease) >> 16;          /* the water darkens just under the cover */
        int keep = 250 + (body - 250) * ease / 256;
        int sink = dep * dep >> 8;
        int frost = y <= NG_FADE ? 0 : y >= NG_FADE + 48 ? 12 : 12 * (y - NG_FADE) / 48;   /* not at the waterline */
        sink = sink * 200 >> 8;
        for (int gx = 0; gx < NG_GW; gx++)
        {
            int i = gy * NG_GW + gx;
            for (int ch = 0; ch < 3; ch++)
            {
                int c = g[ch][i] * keep >> 8;
                /* in the water a bright patch is held down as a whole cover
                 * would be: highlights roll off above a shoulder, so white on
                 * a dark cover softens like the rest instead of glaring */
                int knee = 140 + (255 - 140) * (256 - ease) / 256;
                if (c > knee) c = knee + (c - knee) * 3 / 8;
                c += (water[ch] - c) * sink >> 8;
                c += (255 - c) * frost >> 8;
                g[ch][i] = (uint8_t)(c > 255 ? 255 : c);
            }
        }
    }
    /* short of memory: the grid itself, stretched over the same rows when
     * it is drawn (a blur reads the same at a quarter of the size), never
     * a black floor (a test R1 over Bluetooth, 4 Nov) */
    if (!out)
    {
        uint16_t *rgb = (uint16_t *)ng_small;
        uint8_t *alpha = ng_small + NG_GW * NG_GH * 2;
        for (int gy = 0; gy < NG_GH; gy++)
        {
            int y = gy * NG_S, a = 255, from = NG_FADE - 48;
            if (y < NG_FADE) { int t = y < from ? 0 : (y - from) * 256 / 48; a = t * t * (768 - 2 * t) >> 16; }
            for (int gx = 0; gx < NG_GW; gx++)
            {
                int i = gy * NG_GW + gx;
                rgb[i] = (uint16_t)((g[0][i] >> 3) << 11 | (g[1][i] >> 2) << 5 | g[2][i] >> 3);
                alpha[i] = (uint8_t)(a > 255 ? 255 : a);
            }
        }
        return;
    }
    /* grown back: bilinear between the grid's cell centres, dithered */
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    static int16_t cx0[W]; static uint8_t cfx[W];
    for (int x = 0; x < W; x++)
    {
        int f = (x * 256 + 128) / NG_S - 128;    /* 1/256ths of a cell, from the first centre */
        f = f < 0 ? 0 : f > (NG_GW - 1) * 256 - 1 ? (NG_GW - 1) * 256 - 1 : f;
        cx0[x] = (int16_t)(f >> 8); cfx[x] = (uint8_t)(f & 255);
    }
    uint16_t *rgb = (uint16_t *)out;
    uint8_t *alpha = out + (size_t)W * NG_ROWS * 2;
    for (int y = 0; y < NG_ROWS; y++)
    {
        int f = (y * 256 + 128) / NG_S - 128;
        f = f < 0 ? 0 : f > (NG_GH - 1) * 256 - 1 ? (NG_GH - 1) * 256 - 1 : f;
        int y0 = f >> 8, fy = f & 255;
        /* the frost fades in over the cover's last rows: smoothstep, so
         * neither end shows a line */
        int a = 255, from = NG_FADE - 48;
        if (y < NG_FADE) { int t = y < from ? 0 : (y - from) * 256 / 48; a = t * t * (768 - 2 * t) >> 16; }
        memset(alpha + y * W, a > 255 ? 255 : a, W);
        /* a calm ripple: rows drift sideways a little, more the deeper,
         * on two slow waves so it never looks like a pattern */
        int ripple = 0;
        if (y > NG_FADE)
        {
            float dep = (y - NG_FADE) / (float)(NG_ROWS - NG_FADE);
            ripple = (int)(256 * 3.2f * dep * (sinf(y * 0.24f + 1.3f * sinf(y * 0.031f)) * 0.75f + sinf(y * 0.11f) * 0.25f));
        }
        const uint8_t *row0[3] = { &g[0][y0 * NG_GW], &g[1][y0 * NG_GW], &g[2][y0 * NG_GW] };
        for (int x = 0; x < W; x++)
        {
            int x0 = cx0[x], fx = cfx[x], d = bayer[y & 3][x & 3];
            if (ripple)
            {
                int fr = x0 * 256 + fx + ripple / NG_S;
                fr = fr < 0 ? 0 : fr > (NG_GW - 1) * 256 - 1 ? (NG_GW - 1) * 256 - 1 : fr;
                x0 = fr >> 8; fx = fr & 255;
            }
            int v[3];
            for (int ch = 0; ch < 3; ch++)
            {
                const uint8_t *p = row0[ch] + x0;
                int top = p[0] * (256 - fx) + p[1] * fx, bot = p[NG_GW] * (256 - fx) + p[NG_GW + 1] * fx;
                v[ch] = (top * (256 - fy) + bot * fy) >> 16;
            }
            int r = (v[0] + (d >> 1)) >> 3, gr = (v[1] + (d >> 2)) >> 2, b = (v[2] + (d >> 1)) >> 3;
            rgb[y * W + x] = (uint16_t)((r > 31 ? 31 : r) << 11 | (gr > 63 ? 63 : gr) << 5 | (b > 31 ? 31 : b));
        }
    }
}
/* The cover blurred and darkened to fill ow x oh (RGB565, stride ow): the
 * lyrics' backdrop over the cover's square, and the centred layout's
 * whole screen. The cover is taken as it would fill that box (a square
 * larger than a tall box is cut at its sides), sampled to an 8 px grid,
 * blurred four times there, its colour lifted a little and its bright
 * parts held to about target of 255 for white type, then grown back with
 * bilinear smoothing and an ordered dither (a 16 px grid: a wide blur,
 * and cheap). Built once per cover, never
 * per frame. foot, when given, is a row of W pixels the last 48 rows melt
 * into, itself softened along the row: the lake's first row, so the
 * backdrop meets the glass without a seam. */
#define NB_S 16
/* full_h: the box is ow x full_h, of which the top oh rows are made (the
 * lyrics over Centred: the same blur as its screen, without the cover's
 * shadow) */
static void nb_paint(uint16_t *out, int ow, int oh, int full_h, const uint16_t *src, int w, int h, int stride, lv_color_t c,
                     int target, const uint16_t *foot)
{
    static uint8_t g[3][(W / NB_S) * (H / NB_S)];
    int gw = ow / NB_S, gh = full_h / NB_S, side = ow > full_h ? ow : full_h;
    if (gw > W / NB_S || gh > H / NB_S || gw < 2 || gh < 2) return;
    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++)
        {
            int r = 0, gg = 0, b = 0;
            for (int k = 0; k < 4; k++)
            {
                int sx = gx * NB_S + (k & 1) * (NB_S / 2) + NB_S / 4, sy = gy * NB_S + (k >> 1) * (NB_S / 2) + NB_S / 4;
                int qx = (sx + (side - ow) / 2) * W / side, qy = (sy + (side - full_h) / 2) * W / side;
                int ix = qx - (W - w) / 2, iy = qy - (W - h) / 2;
                if (!src || ix < 0 || ix >= w || iy < 0 || iy >= h) { r += c.red; gg += c.green; b += c.blue; continue; }
                uint16_t p = src[iy * stride + ix];
                r += (p >> 11) << 3; gg += ((p >> 5) & 63) << 2; b += (p & 31) << 3;
            }
            int i = gy * gw + gx;
            g[0][i] = (uint8_t)(r / 4); g[1][i] = (uint8_t)(gg / 4); g[2][i] = (uint8_t)(b / 4);
        }
    for (int ch = 0; ch < 3; ch++)
        for (int pass = 0; pass < 4; pass++)
        {
            for (int y = 0; y < gh; y++) ng_blur(&g[ch][y * gw], gw, 1, 2);
            for (int x = 0; x < gw; x++) ng_blur(&g[ch][x], gh, gw, 2);
        }
    /* the foot row, softened along itself (three boxes of 25 px) */
    static uint8_t fr[3][W];
    for (int x = 0; foot && x < ow && x < W; x++)
    {
        uint16_t q = foot[x];
        fr[0][x] = (uint8_t)((q >> 11) << 3); fr[1][x] = (uint8_t)(((q >> 5) & 63) << 2); fr[2][x] = (uint8_t)((q & 31) << 3);
    }
    for (int ch = 0; foot && ch < 3; ch++)
        for (int pass = 0; pass < 3; pass++)
        {
            int sum = 0, r = 12, n = ow;
            static uint8_t t[W];
            memcpy(t, fr[ch], (size_t)n);
            for (int k = -r; k <= r; k++) sum += t[k < 0 ? 0 : k >= n ? n - 1 : k];
            for (int x = 0; x < n; x++)
            {
                fr[ch][x] = (uint8_t)(sum / (2 * r + 1));
                sum += t[x + r + 1 >= n ? n - 1 : x + r + 1] - t[x - r < 0 ? 0 : x - r];
            }
        }
    /* a little more colour, then as dark as white words need */
    static uint16_t hist[256];
    memset(hist, 0, sizeof hist);
    int n = gw * gh;
    for (int i = 0; i < n; i++)
    {
        int gray = (g[0][i] * 77 + g[1][i] * 150 + g[2][i] * 29) >> 8;
        for (int ch = 0; ch < 3; ch++)
        {
            int v = gray + (g[ch][i] - gray) * 5 / 4;
            g[ch][i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        hist[(g[0][i] * 77 + g[1][i] * 150 + g[2][i] * 29) >> 8]++;
    }
    int bright = 255;
    for (long acc = 0; bright > 0 && (acc += hist[bright]) < n * 15 / 100; bright--) {}
    int keep = bright > 0 ? target * 256 / bright : 230;
    keep = keep < 48 ? 48 : keep > 230 ? 230 : keep;
    for (int gy = 0; gy < gh; gy++)
    {
        /* a touch darker at the top and the foot, where the buttons and the edges are */
        int e = gy < gh / 2 ? gy : gh - 1 - gy, edge = e >= 6 ? 256 : 200 + 56 * e / 6;
        for (int gx = 0; gx < gw; gx++)
            for (int ch = 0; ch < 3; ch++)
            {
                int i = gy * gw + gx;
                g[ch][i] = (uint8_t)((g[ch][i] * keep >> 8) * edge >> 8);
            }
    }
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    for (int y = 0; y < oh; y++)
    {
        int f = (y * 256 + 128) / NB_S - 128;
        f = f < 0 ? 0 : f > (gh - 1) * 256 - 1 ? (gh - 1) * 256 - 1 : f;
        int y0 = f >> 8, fy = f & 255;
        int melt = foot && y >= oh - 48 ? (y - (oh - 48)) * 256 / 48 : 0;
        melt = melt * melt * (768 - 2 * melt) >> 16;
        for (int x = 0; x < ow; x++)
        {
            int fx = (x * 256 + 128) / NB_S - 128;
            fx = fx < 0 ? 0 : fx > (gw - 1) * 256 - 1 ? (gw - 1) * 256 - 1 : fx;
            int x0 = fx >> 8, d = bayer[y & 3][x & 3];
            fx &= 255;
            int v[3];
            for (int ch = 0; ch < 3; ch++)
            {
                const uint8_t *p = &g[ch][y0 * gw + x0];
                int top = p[0] * (256 - fx) + p[1] * fx, bot = p[gw] * (256 - fx) + p[gw + 1] * fx;
                v[ch] = (top * (256 - fy) + bot * fy) >> 16;
            }
            if (melt)
            {
                for (int ch = 0; ch < 3; ch++) v[ch] += (fr[ch][x] - v[ch]) * melt >> 8;
            }
            int r = (v[0] + (d >> 1)) >> 3, gr = (v[1] + (d >> 2)) >> 2, b = (v[2] + (d >> 1)) >> 3;
            out[y * ow + x] = (uint16_t)((r > 31 ? 31 : r) << 11 | (gr > 63 ? 63 : gr) << 5 | (b > 31 ? 31 : b));
        }
    }
}
/* What the cover shows now, as a number: the picture told apart by a few
 * of its pixels (the buffer is reused from song to song), or the colour
 * of a song with none. */
static uint32_t ng_key(void)
{
    if (!lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN) && np_img.data && np_img.header.w)
    {
        const uint16_t *px = (const uint16_t *)np_img.data;
        int w = np_img.header.w, h = np_img.header.h, st = np_img.header.stride / 2;
        uint32_t key = (uint32_t)w * 2654435761u ^ (uint32_t)h;
        for (int i = 1; i < 16; i++) key = key * 31 + px[(h * i / 16) * st + w * ((i * 7) % 16) / 16];
        return key | 1;
    }
    lv_color_t c = lv_obj_get_style_bg_color(np_art, 0);
    return (uint32_t)c.red << 17 | (uint32_t)c.green << 9 | (uint32_t)c.blue << 1;   /* even */
}
/* buf shown by o as the lake's image */
static void ng_unstretch(lv_obj_t *o);
static void ng_show(lv_obj_t *o, lv_image_dsc_t *d, uint8_t *buf)
{
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565A8;
    d->header.w = W;
    d->header.h = NG_ROWS;
    d->header.stride = W * 2;
    d->data = buf;
    d->data_size = (uint32_t)W * NG_ROWS * 3;
    lv_image_cache_drop(d);
    lv_image_set_src(o, d);
    ng_unstretch(o);
    if (o == np_glow) ng_small_on = false;
    lv_obj_invalidate(o);
}
/* back to one to one: LVGL leaves a stretch's scale behind */
static void ng_unstretch(lv_obj_t *o)
{
    lv_image_set_inner_align(o, LV_IMAGE_ALIGN_DEFAULT);
    lv_image_set_scale_x(o, LV_SCALE_NONE);
    lv_image_set_scale_y(o, LV_SCALE_NONE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
}
/* LVGL works a stretch out from the object's size when the alignment is
 * set, not when the size changes: the size first, laid out, then the
 * stretch (set afresh, or an unchanged alignment is not looked at again) */
static void ng_stretch(lv_obj_t *o)
{
    lv_obj_update_layout(o);
    lv_image_set_inner_align(o, LV_IMAGE_ALIGN_DEFAULT);
    lv_image_set_inner_align(o, LV_IMAGE_ALIGN_STRETCH);
}
/* the small one, stretched over the rows the full one covers */
static void ng_show_small(lv_obj_t *o, lv_image_dsc_t *d)
{
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565A8;
    d->header.w = NG_GW;
    d->header.h = NG_GH;
    d->header.stride = NG_GW * 2;
    d->data = ng_small;
    d->data_size = sizeof ng_small;
    lv_image_cache_drop(d);
    lv_image_set_src(o, d);
    lv_obj_set_size(o, W, NG_ROWS + NG_S);   /* the grid's last row is a cell past the end */
    ng_stretch(o);
    ng_small_on = true;
    lv_obj_invalidate(o);
}
static void np_glow_build(const uint16_t *src, int w, int h, int stride, lv_color_t c)
{
    if (!np_glow) return;
    /* the full one with room for the music to spare (the memory watch
     * tries again later); without, the small one */
    if (!np_glow_buf && !ng_force_small && sl_mem_room((long)W * NG_ROWS * 3 / 1024, "Now Playing's reflection"))
        np_glow_buf = malloc((size_t)W * NG_ROWS * 3);
    if (!np_glow_buf || ng_force_small)
    {
        ng_paint(NULL, src, w, h, stride, c);
        ng_show_small(np_glow, &np_glow_img);
        return;
    }
    ng_paint(np_glow_buf, src, w, h, stride, c);
    ng_show(np_glow, &np_glow_img, np_glow_buf);
}

/* ------------------------------------------------------------ centred */
#define NC_S   368                       /* the cover's side */
#define NC_Y   80                        /* its top, under the buttons */
#define NC_DY  (-20)                     /* the song, the bars and the buttons move up by this */
static uint16_t *nc_cov_px;              /* the cover at NC_S, made once a cover */
static lv_image_dsc_t nc_bd_dsc, nc_cov_dsc;
static lv_obj_t *nc_bd;
static uint32_t nc_key;
/* The cover's shadow, baked into the backdrop (a shadow drawn by LVGL is
 * drawn again whenever anything over it changes): darker toward the
 * cover's edges, a little lower than it, as if it floats. */
static void nc_shadow(uint16_t *px)
{
    int x0 = (W - NC_S) / 2, y0 = NC_Y + 14, x1 = x0 + NC_S, y1 = y0 + NC_S, reach = 64;
    for (int y = y0 - reach; y < y1 + reach; y++)
    {
        if (y < 0 || y >= H) continue;
        int dy = y < y0 ? y0 - y : y >= y1 ? y - y1 + 1 : 0;
        for (int x = x0 - reach; x < x1 + reach; x++)
        {
            int dx = x < x0 ? x0 - x : x >= x1 ? x - x1 + 1 : 0;
            int d2 = dx * dx + dy * dy;
            if (d2 >= reach * reach) continue;
            int t = 256 - (int)(sqrtf((float)d2) * 256 / reach);    /* 256 at the edge, 0 at reach */
            int k = 256 - ((t * t >> 8) * t >> 8) * 130 / 256;        /* up to half darker, fading out softly */
            uint16_t p = px[y * W + x];
            int r = (p >> 11) * k >> 8, g = ((p >> 5) & 63) * k >> 8, b = (p & 31) * k >> 8;
            px[y * W + x] = (uint16_t)(r << 11 | g << 5 | b);
        }
    }
}
/* the cover, w x h in a stride, resampled to NC_S square (as the art box
 * shows it: centred, its own colour round a smaller picture) */
static void nc_scale(const uint16_t *src, int w, int h, int stride, uint16_t bg)
{
    for (int y = 0; y < NC_S; y++)
        for (int x = 0; x < NC_S; x++)
        {
            int fx = (x * 2 + 1) * W * 128 / NC_S - 128, fy = (y * 2 + 1) * W * 128 / NC_S - 128;   /* 1/256ths, square's pixels */
            int sx = (fx >> 8) - (W - w) / 2, sy = (fy >> 8) - (W - h) / 2, ax = fx & 255, ay = fy & 255;
            if (sx < 0 || sy < 0 || sx >= w - 1 || sy >= h - 1)
            {
                nc_cov_px[y * NC_S + x] = sx >= 0 && sy >= 0 && sx < w && sy < h ? src[sy * stride + sx] : bg;
                continue;
            }
            const uint16_t *p = src + sy * stride + sx;
            int v[3];
            for (int ch = 0; ch < 3; ch++)
            {
                int sh = ch == 0 ? 11 : ch == 1 ? 5 : 0, m = ch == 1 ? 63 : 31;
                int a = p[0] >> sh & m, b = p[1] >> sh & m, c = p[stride] >> sh & m, d = p[stride + 1] >> sh & m;
                v[ch] = ((a * (256 - ax) + b * ax) * (256 - ay) + (c * (256 - ax) + d * ax) * ay + 32768) >> 16;
            }
            nc_cov_px[y * NC_S + x] = (uint16_t)(v[0] << 11 | v[1] << 5 | v[2]);
        }
}
static void nc_show(lv_obj_t *o, lv_image_dsc_t *d, const void *px, int w, int h)
{
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = w * 2;
    d->data = px;
    d->data_size = (uint32_t)w * h * 2;
    lv_image_cache_drop(d);
    lv_image_set_src(o, d);
    lv_obj_invalidate(o);
}
/* The backdrop and the small cover for what the cover shows now; only
 * when that changed. Short of memory: black under a square cover. */
static void nc_sync(bool force)
{
    if (!np || !np_art_img) return;
    if (!np_centred())
    {
        if (nc_bd) lv_obj_add_flag(nc_bd, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    bool pic = !lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN) && np_img.data && np_img.header.w;
    uint32_t key = ng_key();
    if (force || key != nc_key || !nc_bd_px)
    {
        if (!nc_bd_px && sl_mem_room((long)W * H * 2 / 1024, "Centred's backdrop"))
            nc_bd_px = malloc((size_t)W * H * 2);
        if (pic && !nc_cov_px) nc_cov_px = malloc((size_t)NC_S * NC_S * 2);
        if (!nc_bd)
        {
            nc_bd = lv_image_create(np);
            lv_obj_move_to_index(nc_bd, 0);
            lv_obj_add_flag(nc_bd, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        if (!nc_bd_px || ng_force_small)
        {
            /* short of memory: the same blur at half the size, stretched,
             * not black (the memory watch brings the full one back) */
            if (key == nc_key && nc_small_on) { lv_obj_remove_flag(nc_bd, LV_OBJ_FLAG_HIDDEN); return; }
            nc_key = key;
            if (pic)
                nb_paint(nc_small, W / 2, H / 2, H / 2, (const uint16_t *)np_img.data, np_img.header.w, np_img.header.h,
                         np_img.header.stride / 2, lv_color_black(), 84, NULL);
            else
                nb_paint(nc_small, W / 2, H / 2, H / 2, NULL, 0, 0, 0, lv_obj_get_style_bg_color(np_art, 0), 84, NULL);
            memset((uint8_t *)nc_small + (size_t)(W / 2) * (H / 2) * 2, 255, (size_t)(W / 2) * (H / 2));
            nc_bd_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            nc_bd_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
            nc_bd_dsc.header.w = W / 2;
            nc_bd_dsc.header.h = H / 2;
            nc_bd_dsc.header.stride = W;     /* the RGB565 plane's rows: W / 2 pixels */
            nc_bd_dsc.data = (const uint8_t *)nc_small;
            nc_bd_dsc.data_size = (uint32_t)(W / 2) * (H / 2) * 3;
            lv_image_cache_drop(&nc_bd_dsc);
            lv_image_set_src(nc_bd, &nc_bd_dsc);
            lv_obj_set_size(nc_bd, W, H);
            ng_stretch(nc_bd);
            nc_small_on = true;
            lv_obj_remove_flag(nc_bd, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        nc_key = key;
        if (nc_small_on)
        {
            ng_unstretch(nc_bd);
            nc_small_on = false;
        }
        if (pic)
            nb_paint(nc_bd_px, W, H, H, (const uint16_t *)np_img.data, np_img.header.w, np_img.header.h,
                     np_img.header.stride / 2, lv_color_black(), 84, NULL);
        else
            nb_paint(nc_bd_px, W, H, H, NULL, 0, 0, 0, lv_obj_get_style_bg_color(np_art, 0), 84, NULL);
        nc_shadow(nc_bd_px);
        nc_show(nc_bd, &nc_bd_dsc, nc_bd_px, W, H);
        if (pic && nc_cov_px)
        {
            lv_color_t c = lv_obj_get_style_bg_color(np_art, 0);
            nc_scale((const uint16_t *)np_img.data, np_img.header.w, np_img.header.h, np_img.header.stride / 2,
                     (uint16_t)((c.red >> 3) << 11 | (c.green >> 2) << 5 | (c.blue >> 3)));
        }
    }
    lv_obj_remove_flag(nc_bd, LV_OBJ_FLAG_HIDDEN);
    if (pic && nc_cov_px && lv_image_get_src(np_art_img) != &nc_cov_dsc) nc_show(np_art_img, &nc_cov_dsc, nc_cov_px, NC_S, NC_S);
}
static void nc_free(void)
{
    if (nc_bd) lv_image_set_src(nc_bd, NULL);
    lv_image_cache_drop(&nc_bd_dsc);
    lv_image_cache_drop(&nc_cov_dsc);
    if (np_art_img && lv_image_get_src(np_art_img) == &nc_cov_dsc) lv_image_set_src(np_art_img, &np_img);
    free(nc_bd_px); free(nc_cov_px);
    nc_bd_px = nc_cov_px = NULL;
}

/* The glass for whatever the cover shows now: the picture, or the song's
 * colour when it has none. Only when that changed: songs without art
 * retry every tick, and the cover's buffer is reused from song to song, so
 * the picture is told apart by a few of its pixels. */
static void np_glow_refresh(bool force)
{
    static uint32_t last;
    nc_sync(force);
    if (!np_glass() || !np_glow) { ng_buttons(); lyr_bd_sync(force); return; }
    bool pic = !lv_obj_has_flag(np_art_img, LV_OBJ_FLAG_HIDDEN) && np_img.data && np_img.header.w;
    uint32_t key = ng_key();
    if (!force && key == last && np_glow_buf) { lyr_bd_sync(false); return; }
    last = key;
    if (pic)
        np_glow_build((const uint16_t *)np_img.data, np_img.header.w, np_img.header.h,
                      np_img.header.stride / 2, lv_color_black());
    else
        np_glow_build(NULL, 0, 0, 0, lv_obj_get_style_bg_color(np_art, 0));
    ng_buttons();
    lyr_bd_sync(true);                   /* after the lake, made again: the lyrics melt into its first row */
}
