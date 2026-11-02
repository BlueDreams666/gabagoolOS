/* Liquid glass, shared by the power off slider and Now Playing: a pane a
 * little lighter than what is behind it, light caught across its top half,
 * shade along its bottom, and a rim that is bright where light would catch
 * the top edge and fades round both ends. */

/* The glass's rim: bright where light would catch the top edge, fading
 * down both ends; the border underneath carries a faint line round. */
static void sl_glass_rim_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    /* a pill's radius is LV_RADIUS_CIRCLE (32767): the arc is the round
     * end's, half the height (an arc that big read past LVGL's circle
     * buffer: the R1 crashed holding power with the glass let go) */
    int r = LV_MIN(lv_obj_get_style_radius(o, 0), LV_MIN(lv_area_get_width(&a), lv_area_get_height(&a)) / 2);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_color_white();
    d.width = 2;
    d.radius = (uint16_t)r;
    d.rounded = 1;
    d.center.y = a.y1 + r;
    for (int k = 0; k < 3; k++)              /* the light fades in three steps round each end */
    {
        d.opa = (lv_opa_t)(160 - k * 50);
        d.center.x = a.x1 + r;
        d.start_angle = 270 - 30 * (k + 1); d.end_angle = 270 - 30 * k;
        lv_draw_arc(layer, &d);
        d.center.x = a.x2 - r;
        d.start_angle = 270 + 30 * k; d.end_angle = 270 + 30 * (k + 1);
        lv_draw_arc(layer, &d);
    }
    if (a.x2 - a.x1 > 2 * r)
    {
        lv_draw_line_dsc_t l;
        lv_draw_line_dsc_init(&l);
        l.color = lv_color_white();
        l.width = 2;
        l.opa = 160;
        l.p1.x = a.x1 + r; l.p2.x = a.x2 - r;
        l.p1.y = l.p2.y = a.y1 + 1;
        lv_draw_line(layer, &l);
    }
}

/* Inside the glass: light caught across its top half, and the pane
 * thickening to a shadow along its bottom. Both are the pane's own shape,
 * graded top to bottom, so neither shows an edge or spills past its ends. */
static void sl_glass_light_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = lv_obj_get_style_radius(o, 0);
    r.bg_grad.dir = LV_GRAD_DIR_VER;
    r.bg_grad.stops_count = 2;
    r.bg_grad.stops[0].color = r.bg_grad.stops[1].color = lv_color_white();
    r.bg_grad.stops[0].opa = 58;
    r.bg_grad.stops[1].opa = LV_OPA_TRANSP;
    r.bg_grad.stops[0].frac = 0;
    r.bg_grad.stops[1].frac = 140;
    lv_draw_rect(lv_event_get_layer(e), &r, &a);
    r.bg_grad.stops[0].color = r.bg_grad.stops[1].color = lv_color_black();
    r.bg_grad.stops[0].opa = LV_OPA_TRANSP;
    r.bg_grad.stops[1].opa = 44;
    r.bg_grad.stops[0].frac = 128;
    r.bg_grad.stops[1].frac = 255;
    lv_draw_rect(lv_event_get_layer(e), &r, &a);
}

/* A pane of glass: lighter than what is behind it, lit inside, rimmed. */
static lv_obj_t *sl_glass_pane(lv_obj_t *parent, int w, int h, lv_opa_t fill)
{
    lv_obj_t *g = box(parent);
    lv_obj_set_size(g, w, h);
    lv_obj_set_style_radius(g, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(g, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g, fill, 0);
    lv_obj_set_style_border_width(g, 1, 0);
    lv_obj_set_style_border_color(g, lv_color_white(), 0);
    lv_obj_set_style_border_opa(g, 44, 0);
    lv_obj_add_event_cb(g, sl_glass_light_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(g, sl_glass_rim_draw, LV_EVENT_DRAW_POST, NULL);
    return g;
}


/* ------------------------------------------------------------ the lens */
/* Liquid glass, worked out per pixel from what is behind it, the way
 * Apple's reads: the content stays the hero, the glass barely there.
 *
 *   the body is what is behind, frosted already or magnified a little,
 *   lifted a touch towards white so it reads as a surface, or over light
 *   content dimmed instead, so white on it still reads;
 *   the last few pixels in from the edge bend: they show what lies just
 *   beyond the edge, pulled in round the rim (a thick pane's lens), red
 *   and blue a hair apart;
 *   the rim itself is one crisp line of light, brightest on the two
 *   corners that face the light and its reflection (top left, bottom
 *   right), with a faint shade just inside it, so the pane has thickness;
 *   underneath, a wide faint shadow lifts it off the page.
 *
 * The shape is a rounded rectangle; distances come from its signed
 * distance function. The result is dithered into RGB565, with an alpha
 * plane or, without one, laid over the picture behind so the pane's
 * canvas is opaque. */

struct sl_lens_src
{
    const uint16_t *px;                  /* RGB565 */
    int stride, w, h;                    /* in pixels */
    int x0, y0;                          /* where its pixel 0, 0 is on screen */
    int dim;                             /* of 256: a dark scrim over it */
    const uint16_t *px2;                 /* if set, mixed in by mix of 256 */
    int mix;
};

struct sl_lens_look
{
    float zoom;                          /* the body magnifies by this */
    float edge, bend, chroma;            /* the bending band, how far it reaches out, red/blue split */
    float lift;                          /* the body, of the way to white */
    float rim, rim_w, inner;             /* the line of light, its width, the shade inside it */
    float shadow, blur, drop;            /* under it: how dark, how wide, how far down */
    float frost;                         /* over sharp content: blurred by about this, px */
};

/* a control's pane (track, capsule, button) */
static const struct sl_lens_look sl_lens_pane = {
    1.0f, 7.0f, 7.0f, 0.18f, 0.16f, 0.85f, 1.4f, 0.10f, 0.20f, 12.0f, 3.0f, 0,
};
/* a knob or a thumb while held: clear, magnifying, its rim all it shows */
static const struct sl_lens_look sl_lens_clear = {
    1.18f, 8.0f, 9.0f, 0.25f, 0.04f, 0.95f, 1.6f, 0.12f, 0.22f, 10.0f, 4.0f, 0,
};

/* the picture's pixel at x, y (on screen), clamped to its edges */
static inline int sl_lens_col(const struct sl_lens_src *s, float x)
{
    int ix = (int)(x + 0.5f) - s->x0;
    return ix < 0 ? 0 : ix >= s->w ? s->w - 1 : ix;
}
static inline int sl_lens_row(const struct sl_lens_src *s, float y)
{
    int iy = (int)(y + 0.5f) - s->y0;
    return iy < 0 ? 0 : iy >= s->h ? s->h - 1 : iy;
}
static inline void sl_lens_at(const struct sl_lens_src *s, int at, int *r, int *g, int *b, int ch)
{
    uint16_t v = s->px[at];
    if (ch & 1) *r = ((v >> 11) << 3 | (v >> 13)) * s->dim >> 8;
    if (ch & 2) *g = (((v >> 5) & 63) << 2 | ((v >> 9) & 3)) * s->dim >> 8;
    if (ch & 4) *b = ((v & 31) << 3 | ((v >> 2) & 7)) * s->dim >> 8;
    if (s->px2 && s->mix > 0)
    {
        uint16_t u = s->px2[at];
        if (u == v) return;
        int m = s->mix, n = 256 - m;
        if (ch & 1) *r = (*r * n + (((u >> 11) << 3 | (u >> 13)) * s->dim >> 8) * m) >> 8;
        if (ch & 2) *g = (*g * n + ((((u >> 5) & 63) << 2 | ((u >> 9) & 3)) * s->dim >> 8) * m) >> 8;
        if (ch & 4) *b = (*b * n + (((u & 31) << 3 | ((u >> 2) & 7)) * s->dim >> 8) * m) >> 8;
    }
}
static inline void sl_lens_px(const struct sl_lens_src *s, float x, float y, int *r, int *g, int *b, int ch)
{
    sl_lens_at(s, sl_lens_row(s, y) * s->stride + sl_lens_col(s, x), r, g, b, ch);
}

/* signed distance to a rounded rectangle of half sizes hx + rad, hy + rad
 * centred on 0, 0: negative inside */
static inline float sl_sd(float px, float py, float hx, float hy, float rad)
{
    float qx = fabsf(px) - hx, qy = fabsf(py) - hy;
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    return sqrtf(mx * mx + my * my) + (in < 0 ? in : 0) - rad;
}

/* What a lens pixel's shape decides, apart from what it looks through:
 * how much of the pane covers it (or, outside, its shadow), which way its
 * rim bends the picture, and its shade and line of light. */
struct sl_lens_g { float cover, sa, nbx, nby, shade, lit; bool body; };   /* body: none of it, all pane */

static inline void sl_lens_geo(float px, float py, float hx, float hy, float rad, float blur,
                               const struct sl_lens_look *k, struct sl_lens_g *g)
{
    float sd = sl_sd(px, py, hx, hy, rad), d = -sd;
    g->cover = d + 0.5f;
    if (g->cover <= 0)
    {
        /* the shadow: a wide soft falloff from a shape a little lower */
        float ss = sl_sd(px, py - k->drop, hx, hy, rad);
        float f = ss <= 0 ? 1 : ss >= blur ? 0 : 1 - ss / blur;
        g->sa = k->shadow * f * f * (3 - 2 * f);
        return;
    }
    if (g->cover > 1) g->cover = 1;
    /* the rim's outward direction, from the distance field's slope; only
     * the bend and the line of light use it, so not deeper in than they go */
    float nx = 0, ny = 0;
    if (d < k->edge || d < k->rim_w + 0.5f)
    {
        nx = sl_sd(px + 0.5f, py, hx, hy, rad) - sl_sd(px - 0.5f, py, hx, hy, rad);
        ny = sl_sd(px, py + 0.5f, hx, hy, rad) - sl_sd(px, py - 0.5f, hx, hy, rad);
        float nl = sqrtf(nx * nx + ny * ny);
        if (nl > 0) { nx /= nl; ny /= nl; }
    }
    /* the bend: none in the body, reaching out past the edge at the rim */
    float e = d >= k->edge ? 0 : 1 - d / k->edge;
    float bend = k->bend * e * e;
    g->nbx = nx * bend;
    g->nby = ny * bend;
    /* a faint shade just inside the rim, then the rim's line of light */
    float in = d - k->rim_w - 1.5f;
    g->shade = in > -1.5f && in < 3 ? k->inner * (1 - fabsf(in - 0.75f) / 2.25f) : 0;
    float line = k->rim_w + 0.5f - d;
    line = line < 0 ? 0 : line > 1 ? 1 : line;
    float diag = (nx + ny) * 0.7071f;                    /* +-1 on the two lit corners */
    g->lit = line > 0 ? k->rim * line * (0.45f + 0.55f * diag * diag) : 0;
}

/* over light content the glass darkens instead, so what sits on it (white
 * glyphs) still reads: the mean brightness behind it decides */
static float sl_lens_lift(const struct sl_lens_src *s, int sx, int sy, float w, float h, int m,
                          const struct sl_lens_look *k)
{
    int lum = 0, n = 0;
    for (int gy = 0; gy < 6; gy++)
        for (int gx = 0; gx < 6; gx++)
        {
            int r, g, b;
            sl_lens_px(s, sx + m + w * (gx + 0.5f) / 6, sy + m + h * (gy + 0.5f) / 6, &r, &g, &b, 7);
            lum += (r * 77 + g * 150 + b * 29) >> 8; n++;
        }
    float bright = lum / (n * 255.0f), lift = k->lift;
    if (bright > 0.45f) lift = k->lift - (bright - 0.45f) / 0.55f * (k->lift + 0.30f);
    return lift;
}

/* One pixel of the pane, inside it: the picture under it, bent, lifted,
 * shaded and lit, dithered into RGB565 */
static inline uint16_t sl_lens_shade(const struct sl_lens_src *s, const struct sl_lens_look *k, const struct sl_lens_g *g,
                                     float bx, float by, float lift, int x, int y, int sx, int sy, bool over)
{
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    int r, gg, b;
    if (k->frost > 0)
    {
        /* frosted: five taps, the middle one weighted, the split kept */
        static const float tx[5] = { 0, -1, 1, -1, 1 }, ty[5] = { 0, -1, -1, 1, 1 };
        int sr = 0, sg = 0, sb = 0;
        for (int t = 0; t < 5; t++)
        {
            float ox = tx[t] * k->frost, oy = ty[t] * k->frost;
            int tr, tg, tb, wgt = t ? 1 : 4;
            sl_lens_px(s, bx + ox + g->nbx * (1 + k->chroma), by + oy + g->nby * (1 + k->chroma), &tr, &tg, &tb, 1);
            sl_lens_px(s, bx + ox + g->nbx, by + oy + g->nby, &tr, &tg, &tb, 2);
            sl_lens_px(s, bx + ox + g->nbx * (1 - k->chroma), by + oy + g->nby * (1 - k->chroma), &tr, &tg, &tb, 4);
            sr += tr * wgt; sg += tg * wgt; sb += tb * wgt;
        }
        r = sr >> 3; gg = sg >> 3; b = sb >> 3;
    }
    else if (g->nbx == 0 && g->nby == 0)
        sl_lens_px(s, bx, by, &r, &gg, &b, 7);   /* the body: no bend, so no split; one look for all three */
    else
    {
        sl_lens_px(s, bx + g->nbx * (1 + k->chroma), by + g->nby * (1 + k->chroma), &r, &gg, &b, 1);
        sl_lens_px(s, bx + g->nbx, by + g->nby, &r, &gg, &b, 2);
        sl_lens_px(s, bx + g->nbx * (1 - k->chroma), by + g->nby * (1 - k->chroma), &r, &gg, &b, 4);
    }
    float fr, fg, fb;
    if (lift >= 0) { fr = r + (255 - r) * lift; fg = gg + (255 - gg) * lift; fb = b + (255 - b) * lift; }
    else { fr = r * (1 + lift); fg = gg * (1 + lift); fb = b * (1 + lift); }
    if (g->shade > 0) { fr *= 1 - g->shade; fg *= 1 - g->shade; fb *= 1 - g->shade; }
    if (g->lit > 0) { fr += (255 - fr) * g->lit; fg += (255 - fg) * g->lit; fb += (255 - fb) * g->lit; }
    if (over && g->cover < 1)
    {
        int br, bg_, bb;
        sl_lens_px(s, sx + x, sy + y, &br, &bg_, &bb, 7);
        fr = br + (fr - br) * g->cover; fg = bg_ + (fg - bg_) * g->cover; fb = bb + (fb - bb) * g->cover;
    }
    int dd = bayer[y & 3][x & 3];
    int ir = ((int)fr + (dd >> 1)) >> 3, ig = ((int)fg + (dd >> 2)) >> 2, ib = ((int)fb + (dd >> 1)) >> 3;
    return (uint16_t)((ir > 31 ? 31 : ir) << 11 | (ig > 63 ? 63 : ig) << 5 | (ib > 31 ? 31 : ib));
}

/* A pane of w x h with corner radius rad, drawn into a canvas cw x ch
 * whose top left is at sx, sy on screen; the pane sits m in from the
 * canvas's edges, the shadow uses the margin. rgb is cw x ch RGB565;
 * alpha, if given, cw x ch A8 right after it, as LVGL's RGB565A8 wants. */
static void sl_lens(const struct sl_lens_src *s, int sx, int sy, int cw, int ch, int m, float rad,
                    const struct sl_lens_look *k, uint16_t *rgb, uint8_t *alpha)
{
    float w = (float)(cw - 2 * m), h = (float)(ch - 2 * m);
    if (rad > h / 2) rad = h / 2;
    if (rad > w / 2) rad = w / 2;
    float hx = w / 2 - rad, hy = h / 2 - rad, cx = sx + cw / 2.0f, cy = sy + ch / 2.0f;
    float blur = k->blur < m - k->drop - 1 ? k->blur : m - k->drop - 1;   /* gone by the canvas's edge */
    float lift = sl_lens_lift(s, sx, sy, w, h, m, k);

    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
        {
            float px = x + 0.5f - cw / 2.0f, py = y + 0.5f - ch / 2.0f;
            int i = y * cw + x;
            struct sl_lens_g g;
            sl_lens_geo(px, py, hx, hy, rad, blur, k, &g);
            if (g.cover <= 0)
            {
                if (alpha)
                {
                    rgb[i] = 0;
                    alpha[i] = (uint8_t)(g.sa * 255);
                }
                else
                {
                    int r, gg, b;
                    sl_lens_px(s, sx + x, sy + y, &r, &gg, &b, 7);
                    float keep = 1 - g.sa;
                    rgb[i] = (uint16_t)(((int)(r * keep) >> 3) << 11 | ((int)(gg * keep) >> 2) << 5 | ((int)(b * keep) >> 3));
                }
                continue;
            }
            rgb[i] = sl_lens_shade(s, k, &g, cx + px / k->zoom, cy + py / k->zoom, lift, x, y, sx, sy, !alpha);
            if (alpha) alpha[i] = (uint8_t)(g.cover * 255);
        }
}

/* A lens that moves (a knob being dragged) keeps the same shape from
 * frame to frame: its shape is worked out once, for each size it takes,
 * and a move only looks through it again. With alpha only; the same
 * pixels as sl_lens. */
struct sl_lens_cache
{
    int cw, ch, m;
    float rad;
    const struct sl_lens_look *k;
    struct sl_lens_g *g;
    float *ax, *ay;                      /* each column's and row's offset in the picture, magnified */
    int *col, *row;                      /* where they look, this draw */
    int size, lines;                     /* pixels and columns + rows there is room for */
};
static void sl_lens_cache_free(struct sl_lens_cache *c)
{
    free(c->g);
    free(c->ax);
    memset(c, 0, sizeof *c);
}
static void sl_lens_moved(struct sl_lens_cache *c, const struct sl_lens_src *s, int sx, int sy, int cw, int ch, int m,
                          float rad, const struct sl_lens_look *k, uint16_t *rgb, uint8_t *alpha)
{
    float w = (float)(cw - 2 * m), h = (float)(ch - 2 * m);
    if (rad > h / 2) rad = h / 2;
    if (rad > w / 2) rad = w / 2;
    float hx = w / 2 - rad, hy = h / 2 - rad, cx = sx + cw / 2.0f, cy = sy + ch / 2.0f;
    if (c->cw != cw || c->ch != ch || c->m != m || c->rad != rad || c->k != k || !c->g)
    {
        if (cw * ch > c->size || cw + ch > c->lines)
        {
            free(c->g);
            free(c->ax);
            c->g = malloc(sizeof *c->g * (size_t)cw * ch);
            c->ax = malloc((sizeof *c->ax + sizeof *c->col) * (size_t)(cw + ch));
            if (!c->g || !c->ax) { sl_lens_cache_free(c); sl_lens(s, sx, sy, cw, ch, m, rad, k, rgb, alpha); return; }
            c->size = cw * ch;
            c->lines = cw + ch;
        }
        c->ay = c->ax + cw;
        c->col = (int *)(c->ay + ch);
        c->row = c->col + cw;
        float blur = k->blur < m - k->drop - 1 ? k->blur : m - k->drop - 1;
        /* The shape is the same mirrored, but for the two lit corners and
         * the shadow, which falls lower: each pixel of the bottom and the
         * right halves is its mirror's, the bend turned round, unless it is
         * lit or (below) shadow. Exactly the pixels sl_lens_geo gives. */
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
            {
                struct sl_lens_g *g = &c->g[y * cw + x];
                const struct sl_lens_g *up = y > ch - 1 - y ? &c->g[(ch - 1 - y) * cw + x] : NULL;
                const struct sl_lens_g *left = x > cw - 1 - x ? g - (2 * x - cw + 1) : NULL;
                if (up && up->cover > 0 && !(up->lit > 0)) { *g = *up; g->nby = -g->nby; }
                else if (left && !(left->cover > 0 && left->lit > 0)) { *g = *left; g->nbx = -g->nbx; }
                else sl_lens_geo(x + 0.5f - cw / 2.0f, y + 0.5f - ch / 2.0f, hx, hy, rad, blur, k, g);
                g->body = g->cover >= 1 && g->nbx == 0 && g->nby == 0 && g->shade <= 0 && g->lit <= 0 && k->frost <= 0;
            }
        for (int x = 0; x < cw; x++) c->ax[x] = (x + 0.5f - cw / 2.0f) / k->zoom;
        for (int y = 0; y < ch; y++) c->ay[y] = (y + 0.5f - ch / 2.0f) / k->zoom;
        c->cw = cw; c->ch = ch; c->m = m; c->rad = rad; c->k = k;
    }
    /* the body neither bends nor splits nor is lit: one look, through a
     * table of what the lift makes of each level */
    static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    float lift = sl_lens_lift(s, sx, sy, w, h, m, k);
    int lut[256];
    for (int v = 0; v < 256; v++) lut[v] = (int)(lift >= 0 ? v + (255 - v) * lift : v * (1 + lift));
    for (int x = 0; x < cw; x++) c->col[x] = sl_lens_col(s, cx + c->ax[x]);
    for (int y = 0; y < ch; y++) c->row[y] = sl_lens_row(s, cy + c->ay[y]) * s->stride;
    const struct sl_lens_g *g = c->g;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++, g++)
        {
            int i = y * cw + x;
            if (g->cover <= 0) { rgb[i] = 0; alpha[i] = (uint8_t)(g->sa * 255); continue; }
            alpha[i] = (uint8_t)(g->cover * 255);
            if (!g->body)
            {
                rgb[i] = sl_lens_shade(s, k, g, cx + c->ax[x], cy + c->ay[y], lift, x, y, sx, sy, false);
                continue;
            }
            int r, gg, b, dd = bayer[y & 3][x & 3];
            sl_lens_at(s, c->row[y] + c->col[x], &r, &gg, &b, 7);
            int ir = (lut[r] + (dd >> 1)) >> 3, ig = (lut[gg] + (dd >> 2)) >> 2, ib = (lut[b] + (dd >> 1)) >> 3;
            rgb[i] = (uint16_t)((ir > 31 ? 31 : ir) << 11 | (ig > 63 ? 63 : ig) << 5 | (ib > 31 ? 31 : ib));
        }
}

/* LVGL draws a child past its parent only as far as the parent's extra
 * draw size; a lens canvas's shadow needs its whole margin. */
static void sl_ext_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, (int32_t)(intptr_t)lv_event_get_user_data(e));
}
static void sl_overflow(lv_obj_t *o, int px)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(o, sl_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, (void *)(intptr_t)px);
    lv_obj_refresh_ext_draw_size(o);
}

/* A canvas showing a pane of glass over the picture s, placed where it
 * is on screen; buf is the canvas's (cw x ch x 2, or x 3 with alpha). */
static lv_obj_t *sl_lens_canvas(lv_obj_t *parent, const struct sl_lens_src *s, int sx, int sy, int w, int h,
                                int m, float rad, const struct sl_lens_look *k, uint8_t *buf, bool with_alpha)
{
    int cw = w + 2 * m, ch = h + 2 * m;
    sl_lens(s, sx - m, sy - m, cw, ch, m, rad, k, (uint16_t *)buf, with_alpha ? buf + (size_t)cw * ch * 2 : NULL);
    lv_obj_t *c = lv_canvas_create(parent);
    lv_canvas_set_buffer(c, buf, cw, ch, with_alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}
