/* App icons, drawn as vectors so one design serves every size: the cards
 * home, the Playroom list, live tiles, the apps list and the pin picker.
 * Each icon is laid out on a 48 x 48 grid and scaled to its box. Shapes are
 * in the ink colour; cut-outs (eyes, windows, buttons) are in the colour
 * behind the icon, so an icon reads on any tile or card.
 *
 * Fonts have no Tamagotchi, spoon or pawn, and the stand-ins (an eye, an
 * arrow, a GPS pointer) said nothing about the app they opened. */

enum sl_icon_id {
    IC_NONE, IC_PET, IC_RAGDOLL, IC_SNAKE, IC_RADIO, IC_SPOON, IC_QUIZ, IC_COIN,
    IC_MAGIC, IC_CHESS, IC_PLAYROOM, IC_ARTISTS, IC_LOCK, IC_CLOCK, IC_ALBUMS,
    IC_COVERFLOW, IC_LIBRARY, IC_CUSTOMIZE, IC_UPNEXT, IC_PLAYLISTS, IC_GABAGOOL, IC_SPECTRUM, IC_FILES, IC_HEART, IC_VISUALIZER, IC_CLICKER, IC_SUN, IC_METRONOME, IC_SPACEY,
    IC_FORYOU, IC_SONGS, IC_WIFI, IC_BLUETOOTH, IC_SCREENSHOT, IC_STAR,
    IC_SHIFT, IC_SHIFT_ON, IC_CAPS, IC_GLOBE,
    IC_CARTRIDGE, IC_2048, IC_SUDOKU, IC_POINTS, IC_FLAPPY, IC_GRAVE, IC_HEADPHONES, IC_PODCAST, IC_SERVER,
    /* symbols: the glyphs labels draw beside words (sl_sym_<px>), at the
     * words' stroke weight */
    IC_G_EQ, IC_G_XFADE,
    IC_G_PLAY, IC_G_PAUSE, IC_G_STOP, IC_G_PREV, IC_G_NEXT, IC_G_SHUFFLE, IC_G_LOOP, IC_G_REFRESH, IC_G_AUDIO, IC_G_LIST, IC_G_BARS, IC_G_OK, IC_G_CLOSE, IC_G_PLUS, IC_G_MINUS, IC_G_LEFT, IC_G_RIGHT, IC_G_UP, IC_G_DOWN, IC_G_SETTINGS, IC_G_HOME, IC_G_POWER, IC_G_EDIT, IC_G_TRASH, IC_G_DIRECTORY, IC_G_FILE, IC_G_IMAGE, IC_G_VIDEO, IC_G_EYE_OPEN, IC_G_EYE_CLOSE, IC_G_BLUETOOTH, IC_G_WIFI, IC_G_VOLUME_MID, IC_G_VOLUME_MAX, IC_G_UPLOAD, IC_G_DOWNLOAD, IC_G_WARNING, IC_G_BELL, IC_G_CHARGE, IC_G_USB, IC_G_GPS, IC_G_KEYBOARD, IC_G_BACKSPACE, IC_G_COPY, IC_G_CALL, IC_G_TINT, IC_G_DRIVE, IC_G_SAVE, IC_G_BATTERY_FULL, IC_G_BATTERY_3, IC_G_BATTERY_2, IC_G_BATTERY_1, IC_G_BATTERY_EMPTY, IC_G_GLOBE,
    IC_N
};

/* The pen draws into a widget's layer, or (a8 set) into an 8-bit coverage
 * mask, a symbol font's glyph: there each shape is measured by its distance
 * from every pixel's centre, so its edges are smooth at any size. */
struct ic_pen { lv_layer_t *layer; int x, y; float s; lv_color_t ink, bg; uint8_t *a8; int aw, ah, astride; float wk; };

/* ink over what is there; a cut wipes it, as the colour behind would */
static void ic_a8_span(const struct ic_pen *p, float fx0, float fy0, float fx1, float fy1, bool cut,
                       float (*dist)(const float *, float, float), const float *k)
{
    int x0 = (int)floorf(fx0) - 1, y0 = (int)floorf(fy0) - 1, x1 = (int)ceilf(fx1) + 1, y1 = (int)ceilf(fy1) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > p->aw) x1 = p->aw;
    if (y1 > p->ah) y1 = p->ah;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            float c = 0.5f - dist(k, x + 0.5f, y + 0.5f);
            if (c <= 0) continue;
            if (c > 1) c = 1;
            uint8_t *m = p->a8 + y * p->astride + x;
            *m = cut ? (uint8_t)(*m * (1 - c) + 0.5f) : (uint8_t)(*m + (255 - *m) * c + 0.5f);
        }
}
static float ic_d_cap(const float *k, float x, float y)        /* a stroke with round ends: a, b, half width */
{
    float bx = k[2] - k[0], by = k[3] - k[1], px = x - k[0], py = y - k[1], l = bx * bx + by * by;
    float t = l > 0 ? (px * bx + py * by) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = px - t * bx, dy = py - t * by;
    return sqrtf(dx * dx + dy * dy) - k[4];
}
static float ic_d_rect(const float *k, float x, float y)       /* centre, half sizes, radius */
{
    float qx = fabsf(x - k[0]) - k[2] + k[4], qy = fabsf(y - k[1]) - k[3] + k[4];
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - k[4];
}
static float ic_d_arc(const float *k, float x, float y)        /* centre, radius, half width, from, sweep (degrees, clockwise from three o'clock) */
{
    float dx = x - k[0], dy = y - k[1];
    float a = atan2f(dy, dx) * (180 / 3.14159265f) - k[4];
    while (a < 0) a += 360;
    while (a >= 360) a -= 360;
    if (a <= k[5]) return fabsf(sqrtf(dx * dx + dy * dy) - k[2]) - k[3];
    float e[2] = { 0, 0 }, best = 1e9f;
    for (int i = 0; i < 2; i++)
    {
        float r = (k[4] + (i ? k[5] : 0)) * (3.14159265f / 180);
        e[0] = k[0] + k[2] * cosf(r); e[1] = k[1] + k[2] * sinf(r);
        float ex = x - e[0], ey = y - e[1], d = sqrtf(ex * ex + ey * ey);
        if (d < best) best = d;
    }
    return best - k[3];
}
static float ic_d_tri(const float *k, float x, float y)        /* three corners, then a rounding added all round */
{
    float d = 1e9f, s = (k[2] - k[0]) * (k[1] - k[5]) - (k[3] - k[1]) * (k[0] - k[4]) > 0 ? 1.f : -1.f, side = 1e9f;
    for (int i = 0; i < 3; i++)
    {
        int j = (i + 1) % 3;
        float ex = k[j * 2] - k[i * 2], ey = k[j * 2 + 1] - k[i * 2 + 1], vx = x - k[i * 2], vy = y - k[i * 2 + 1];
        float l = ex * ex + ey * ey, t = l > 0 ? (vx * ex + vy * ey) / l : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float qx = vx - ex * t, qy = vy - ey * t, q = qx * qx + qy * qy, c = s * (vx * ey - vy * ex);
        if (q < d) d = q;
        if (c < side) side = c;
    }
    return (side > 0 ? -sqrtf(d) : sqrtf(d)) - k[6];
}
/* points on the mask: the grid's, scaled, from the glyph's corner */
static float ic_fx(const struct ic_pen *p, float v) { return p->x + v * p->s; }
static float ic_fy(const struct ic_pen *p, float v) { return p->y + v * p->s; }

static int ic_px(const struct ic_pen *p, float v) { return (int)lroundf(v * p->s); }

static void ic_rect(const struct ic_pen *p, float x, float y, float w, float h, float r, bool cut)
{
    if (p->a8)
    {
        float k[5] = { ic_fx(p, x + w / 2), ic_fy(p, y + h / 2), w * p->s / 2, h * p->s / 2, r * p->s };
        if (k[4] > k[2]) k[4] = k[2];
        if (k[4] > k[3]) k[4] = k[3];
        ic_a8_span(p, ic_fx(p, x), ic_fy(p, y), ic_fx(p, x + w), ic_fy(p, y + h), cut, ic_d_rect, k);
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = cut ? p->bg : p->ink;
    d.bg_opa = LV_OPA_COVER;
    d.radius = ic_px(p, r);
    lv_area_t a = { p->x + ic_px(p, x), p->y + ic_px(p, y),
                    p->x + ic_px(p, x + w) - 1, p->y + ic_px(p, y + h) - 1 };
    lv_draw_rect(p->layer, &d, &a);
}
static void ic_dot(const struct ic_pen *p, float cx, float cy, float r, bool cut)
{
    ic_rect(p, cx - r, cy - r, 2 * r, 2 * r, r, cut);
}
static void ic_line(const struct ic_pen *p, float x1, float y1, float x2, float y2, float w, bool cut)
{
    if (p->a8)
    {
        float k[5] = { ic_fx(p, x1), ic_fy(p, y1), ic_fx(p, x2), ic_fy(p, y2), w * p->s * (p->wk > 0 ? p->wk : 1) / 2 };
        if (k[4] < 0.6f) k[4] = 0.6f;
        ic_a8_span(p, fminf(k[0], k[2]) - k[4], fminf(k[1], k[3]) - k[4], fmaxf(k[0], k[2]) + k[4], fmaxf(k[1], k[3]) + k[4], cut, ic_d_cap, k);
        return;
    }
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = cut ? p->bg : p->ink;
    d.width = ic_px(p, w) > 1 ? ic_px(p, w) : 1;
    d.round_start = d.round_end = 1;
    d.p1.x = p->x + ic_px(p, x1); d.p1.y = p->y + ic_px(p, y1);
    d.p2.x = p->x + ic_px(p, x2); d.p2.y = p->y + ic_px(p, y2);
    lv_draw_line(p->layer, &d);
}
static void ic_arc(const struct ic_pen *p, float cx, float cy, float r, int from, int to, float w, bool cut)
{
    if (p->a8)
    {
        float k[6] = { ic_fx(p, cx), ic_fy(p, cy), r * p->s, w * p->s * (p->wk > 0 ? p->wk : 1) / 2, (float)from, (float)(((to - from) % 360 + 360) % 360) };
        if (k[3] < 0.6f) k[3] = 0.6f;
        float o = k[2] + k[3];
        ic_a8_span(p, k[0] - o, k[1] - o, k[0] + o, k[1] + o, cut, ic_d_arc, k);
        return;
    }
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = cut ? p->bg : p->ink;
    d.width = ic_px(p, w) > 1 ? ic_px(p, w) : 1;
    d.center.x = p->x + ic_px(p, cx);
    d.center.y = p->y + ic_px(p, cy);
    d.radius = (uint16_t)ic_px(p, r + w / 2);
    d.start_angle = from;
    d.end_angle = to;
    d.rounded = 1;
    lv_draw_arc(p->layer, &d);
}
/* a triangle with its corners rounded by r: the corners given are the
 * rounding's centres, so the shape grows by r all round (masks only; a
 * layer gets the plain triangle) */
static void ic_tri_r(const struct ic_pen *p, float x1, float y1, float x2, float y2, float x3, float y3, float r, bool cut);
static void ic_tri(const struct ic_pen *p, float x1, float y1, float x2, float y2, float x3, float y3, bool cut)
{
    if (p->a8) { ic_tri_r(p, x1, y1, x2, y2, x3, y3, 0, cut); return; }
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.bg_color = cut ? p->bg : p->ink;
    d.bg_opa = LV_OPA_COVER;
    d.p[0].x = p->x + ic_px(p, x1); d.p[0].y = p->y + ic_px(p, y1);
    d.p[1].x = p->x + ic_px(p, x2); d.p[1].y = p->y + ic_px(p, y2);
    d.p[2].x = p->x + ic_px(p, x3); d.p[2].y = p->y + ic_px(p, y3);
    lv_draw_triangle(p->layer, &d);
}
static void ic_tri_r(const struct ic_pen *p, float x1, float y1, float x2, float y2, float x3, float y3, float r, bool cut)
{
    if (!p->a8) { ic_tri(p, x1, y1, x2, y2, x3, y3, cut); return; }
    float k[7] = { ic_fx(p, x1), ic_fy(p, y1), ic_fx(p, x2), ic_fy(p, y2), ic_fx(p, x3), ic_fy(p, y3), r * p->s };
    ic_a8_span(p, fminf(k[0], fminf(k[2], k[4])) - k[6], fminf(k[1], fminf(k[3], k[5])) - k[6],
               fmaxf(k[0], fmaxf(k[2], k[4])) + k[6], fmaxf(k[1], fmaxf(k[3], k[5])) + k[6], cut, ic_d_tri, k);
}
/* a rounded frame: a rect's outline w wide (the inside cut to what lies behind) */
static void ic_frame(const struct ic_pen *p, float x, float y, float w, float h, float r, float sw)
{
    if (p->a8 && p->wk > 0) sw *= p->wk;
    ic_rect(p, x - sw / 2, y - sw / 2, w + sw, h + sw, r + sw / 2, false);
    ic_rect(p, x + sw / 2, y + sw / 2, w - sw, h - sw, r > sw / 2 ? r - sw / 2 : 0, true);
}
/* an arrowhead: two strokes back from the tip at (x, y), the way (dx, dy) points */
static void ic_head(const struct ic_pen *p, float x, float y, float dx, float dy, float len, float sw)
{
    float l = sqrtf(dx * dx + dy * dy);
    dx /= l; dy /= l;
    ic_line(p, x, y, x - len * (dx + dy) * 0.7071f, y - len * (dy - dx) * 0.7071f, sw, false);
    ic_line(p, x, y, x - len * (dx - dy) * 0.7071f, y - len * (dy + dx) * 0.7071f, sw, false);
}
/* a stroke through n points (x, y pairs), round at every joint */
static void ic_poly(const struct ic_pen *p, const float *xy, int n, float w, bool cut)
{
    for (int i = 0; i + 1 < n; i++) ic_line(p, xy[i * 2], xy[i * 2 + 1], xy[i * 2 + 2], xy[i * 2 + 3], w, cut);
}

/* An icon into any layer: a widget's, or a canvas's (a game's sprite) */
static void ic_paint(const struct ic_pen *p, int id);
static void sl_icon_paint(lv_layer_t *layer, const lv_area_t *box, int id, lv_color_t ink, lv_color_t bg)
{
    lv_area_t a = *box;
    struct ic_pen pen = { layer, a.x1, a.y1, lv_area_get_width(&a) / 48.0f, ink, bg, NULL, 0, 0, 0, 1 };
    ic_paint(&pen, id);
}
static void ic_paint(const struct ic_pen *p, int id)
{
    if (id == IC_HEART && global_settings.sloppy_love_star) id = IC_STAR;   /* Customization: a favourite is a star (#142) */
    switch (id)
    {
    case IC_PET:          /* the egg-shaped handheld, a face on its screen */
        ic_rect(p, 9, 3, 30, 42, 15, false);
        ic_rect(p, 14, 12, 20, 16, 3, true);
        ic_dot(p, 20, 19, 2.2f, false); ic_dot(p, 28, 19, 2.2f, false);
        ic_rect(p, 21, 23, 6, 1.8f, 1, false);
        ic_dot(p, 17, 35, 2.6f, true); ic_dot(p, 24, 37.5f, 2.6f, true); ic_dot(p, 31, 35, 2.6f, true);
        break;
    case IC_RAGDOLL:      /* a stick figure, mid tumble */
        ic_dot(p, 31, 10, 5.5f, false);
        ic_line(p, 28, 16, 21, 30, 4, false);
        ic_line(p, 26, 20, 13, 15, 3.5f, false);
        ic_line(p, 26, 20, 37, 27, 3.5f, false);
        ic_line(p, 21, 30, 11, 36, 3.5f, false);
        ic_line(p, 21, 30, 27, 42, 3.5f, false);
        break;
    case IC_SNAKE:        /* blocks on a grid, a head and an apple */
        for (int i = 0; i < 4; i++) ic_rect(p, 6 + i * 8, 32, 7, 7, 2, false);
        ic_rect(p, 30, 24, 7, 7, 2, false);
        ic_rect(p, 30, 16, 7, 7, 2, false);
        ic_rect(p, 30, 7, 11, 10, 3, false);
        ic_dot(p, 37, 11, 1.6f, true);
        ic_dot(p, 13, 16, 4.5f, false);
        ic_line(p, 13, 11, 16, 7, 2, false);
        break;
    case IC_RADIO:        /* a transistor radio: speaker, dial, antenna */
        ic_line(p, 13, 18, 34, 6, 2.5f, false);
        ic_dot(p, 34, 6, 2.6f, false);
        ic_rect(p, 5, 17, 38, 26, 6, false);
        ic_dot(p, 17, 30, 7.5f, true);
        ic_dot(p, 17, 30, 3, false);
        for (int i = 0; i < 3; i++) ic_rect(p, 29, 24 + i * 5, 9, 2, 1, true);
        break;
    case IC_SPOON:        /* a spoon with a face, bouncing */
        ic_line(p, 27, 22, 11, 41, 4.5f, false);
        ic_line(p, 29, 17, 34, 11, 17, false);
        ic_dot(p, 28, 13, 3, true); ic_dot(p, 35, 17, 3, true);
        ic_dot(p, 28.8f, 13.6f, 1.4f, false); ic_dot(p, 35.8f, 17.6f, 1.4f, false);
        ic_arc(p, 31, 17, 6, 70, 130, 1.8f, true);
        ic_line(p, 42, 30, 45, 33, 2, false); ic_line(p, 39, 34, 42, 38, 2, false);
        break;
    case IC_QUIZ:         /* a speech bubble with a question and a note */
        ic_rect(p, 4, 6, 40, 28, 9, false);
        ic_tri(p, 10, 32, 21, 32, 9, 43, false);
        ic_arc(p, 24, 16, 5, 180, 60, 3.2f, true);
        ic_line(p, 27, 20.5f, 24, 24, 3.2f, true);
        ic_dot(p, 24, 28.5f, 2, true);
        ic_dot(p, 38, 41, 3.5f, false);
        ic_line(p, 41, 41, 41, 31, 2.2f, false);
        ic_line(p, 41, 31, 45, 33, 2.2f, false);
        break;
    case IC_COIN:         /* one coin landing on another */
        ic_arc(p, 30, 19, 12, 0, 360, 3, false);
        ic_dot(p, 20, 28, 15, true);
        ic_dot(p, 20, 28, 13, false);
        ic_arc(p, 20, 28, 8.5f, 0, 360, 2, true);
        ic_rect(p, 18.5f, 23, 3, 10, 1.5f, true);
        break;
    case IC_MAGIC:        /* the ball, its window and the 8 */
        ic_dot(p, 24, 24, 20, false);
        ic_dot(p, 24, 22, 10, true);
        ic_arc(p, 24, 18.5f, 2.6f, 0, 360, 2.2f, false);
        ic_arc(p, 24, 25, 3.4f, 0, 360, 2.2f, false);
        ic_arc(p, 24, 24, 16, 200, 245, 2.5f, true);
        break;
    case IC_CLICKER:      /* +1 */
        ic_rect(p, 4, 22, 20, 5, 2.5f, false);
        ic_rect(p, 11.5f, 14.5f, 5, 20, 2.5f, false);
        ic_rect(p, 32, 8, 6, 32, 3, false);
        ic_line(p, 34, 10, 27, 16, 5, false);
        break;
    case IC_CHESS:        /* a pawn */
        ic_dot(p, 24, 12, 6.5f, false);
        ic_rect(p, 16, 19, 16, 4, 2, false);
        ic_tri(p, 19, 22, 29, 22, 34, 37, false);
        ic_tri(p, 19, 22, 34, 37, 14, 37, false);
        ic_rect(p, 10, 36, 28, 7, 3, false);
        break;
    case IC_PLAYROOM:     /* a game pad */
        ic_rect(p, 3, 14, 42, 22, 11, false);
        ic_rect(p, 9, 23, 12, 4, 1, true);
        ic_rect(p, 13, 19, 4, 12, 1, true);
        ic_dot(p, 32, 21.5f, 3, true);
        ic_dot(p, 38, 27.5f, 3, true);
        break;
    case IC_ARTISTS:      /* a singer: head, shoulders, microphone */
        ic_dot(p, 20, 15, 8, false);
        ic_rect(p, 6, 27, 28, 20, 11, false);
        ic_line(p, 37, 22, 37, 36, 2.5f, false);
        ic_dot(p, 37, 18, 5, false);
        break;
    case IC_LOCK:         /* a padlock */
        ic_arc(p, 24, 19, 9, 180, 0, 4, false);
        ic_line(p, 13, 19, 13, 23, 4, false);
        ic_line(p, 35, 19, 35, 23, 4, false);
        ic_rect(p, 8, 22, 32, 22, 5, false);
        ic_dot(p, 24, 31, 3.2f, true);
        ic_rect(p, 22.8f, 32, 2.4f, 6, 1, true);
        break;
    case IC_CLOCK:        /* a clock face at ten past ten */
        ic_arc(p, 24, 24, 17, 0, 360, 4, false);
        ic_line(p, 24, 24, 24, 13, 3.5f, false);
        ic_line(p, 24, 24, 32, 28, 3.5f, false);
        ic_dot(p, 24, 24, 2.5f, false);
        break;
    case IC_ALBUMS:       /* a record half out of its sleeve */
        ic_dot(p, 30, 24, 15, false);
        ic_arc(p, 30, 24, 9, 0, 360, 1.2f, true);
        ic_dot(p, 30, 24, 3, true);
        ic_rect(p, 3, 8, 30, 32, 3, true);
        ic_rect(p, 5, 10, 26, 28, 2, false);
        break;
    case IC_COVERFLOW:    /* covers turning away on either side */
        ic_tri(p, 3, 14, 12, 11, 12, 37, false);
        ic_tri(p, 3, 14, 12, 37, 3, 34, false);
        ic_tri(p, 45, 14, 36, 11, 36, 37, false);
        ic_tri(p, 45, 14, 36, 37, 45, 34, false);
        ic_rect(p, 13, 8, 22, 32, 2, true);
        ic_rect(p, 15, 10, 18, 28, 2, false);
        break;
    case IC_LIBRARY:      /* books on a shelf, one leaning */
        ic_rect(p, 6, 10, 8, 30, 2, false);
        ic_rect(p, 16, 14, 7, 26, 2, false);
        ic_line(p, 30, 15, 37, 38, 7, false);
        ic_rect(p, 4, 41, 40, 3, 1, false);
        ic_rect(p, 8, 16, 4, 2, 1, true);
        break;
    case IC_CUSTOMIZE:    /* a paint palette: dabs of paint, a notch and a thumb hole */
        ic_dot(p, 23, 24, 19, false);
        ic_dot(p, 42, 34, 8, true);
        ic_dot(p, 28, 32, 4, true);
        ic_dot(p, 13, 22, 3.5f, true); ic_dot(p, 20, 12, 3.5f, true);
        ic_dot(p, 31, 13, 3.5f, true); ic_dot(p, 14, 33, 3.5f, true);
        break;
    case IC_UPNEXT:       /* a queue with the next one coming up */
        ic_rect(p, 4, 10, 24, 4, 2, false);
        ic_rect(p, 4, 22, 24, 4, 2, false);
        ic_rect(p, 4, 34, 16, 4, 2, false);
        ic_tri(p, 32, 26, 44, 33, 32, 40, false);
        break;
    case IC_PLAYLISTS:    /* lines and a note */
        ic_rect(p, 4, 9, 26, 4, 2, false);
        ic_rect(p, 4, 19, 26, 4, 2, false);
        ic_rect(p, 4, 29, 16, 4, 2, false);
        ic_dot(p, 30, 38, 5, false);
        ic_line(p, 34, 38, 34, 16, 3, false);
        ic_line(p, 34, 16, 42, 20, 3, false);
        break;
    case IC_GABAGOOL:     /* a slice of capicola, its marbling cut out */
        ic_dot(p, 24, 24, 19, false);
        ic_arc(p, 24, 24, 15, 200, 320, 1.6f, true);
        ic_arc(p, 20, 27, 8, 120, 250, 1.6f, true);
        ic_arc(p, 30, 22, 6, 300, 60, 1.6f, true);
        ic_dot(p, 26, 31, 2, true); ic_dot(p, 16, 18, 1.6f, true);
        break;
    case IC_SPECTRUM:     /* bars of an analyzer, a peak cap above each */
        for (int i = 0; i < 5; i++)
        {
            static const float top[5] = { 20, 10, 16, 26, 32 };
            ic_rect(p, 4 + i * 8.4f, top[i], 6, 43 - top[i], 1, false);
            ic_rect(p, 4 + i * 8.4f, top[i] - 6, 6, 2.5f, 1, false);
        }
        break;
    case IC_VISUALIZER:   /* a spiral winding out: half circles, over and under, each wider */
        for (int k = 1; k <= 6; k++)
            ic_arc(p, k & 1 ? 27.2f : 24, 24, 3.2f * k, k & 1 ? 180 : 0, k & 1 ? 360 : 180, 3.2f, false);
        break;
    case IC_SPACEY:       /* a tunnel: squares inside squares, each turned further */
        for (int k = 0; k < 3; k++)
        {
            static const float rad[3] = { 19.5f, 12.5f, 6 }, turn[3] = { 0, 0.42f, 0.84f };
            float x[4], y[4];
            for (int j = 0; j < 4; j++)
            {
                float a = turn[k] + j * 1.5707963f + 0.7853982f;
                x[j] = 24 + rad[k] * cosf(a);
                y[j] = 24 + rad[k] * sinf(a);
            }
            for (int j = 0; j < 4; j++) ic_line(p, x[j], y[j], x[(j + 1) & 3], y[(j + 1) & 3], 3.2f, false);
        }
        break;
    case IC_FORYOU:       /* a sparkle and a little one: picked for you */
        ic_tri(p, 19, 7, 24.5f, 27, 13.5f, 27, false);
        ic_tri(p, 13.5f, 27, 24.5f, 27, 19, 47, false);
        ic_tri(p, 1, 27, 19, 21.5f, 19, 32.5f, false);
        ic_tri(p, 19, 21.5f, 37, 27, 19, 32.5f, false);
        ic_tri(p, 37, 2, 39.8f, 12, 34.2f, 12, false);
        ic_tri(p, 34.2f, 12, 39.8f, 12, 37, 22, false);
        ic_tri(p, 27, 12, 37, 9.2f, 37, 14.8f, false);
        ic_tri(p, 37, 9.2f, 47, 12, 37, 14.8f, false);
        break;
    case IC_SONGS:        /* two notes on one beam */
        ic_dot(p, 13, 38, 6.5f, false);
        ic_dot(p, 35, 34, 6.5f, false);
        ic_line(p, 18, 38, 18, 11, 3.5f, false);
        ic_line(p, 40, 34, 40, 7, 3.5f, false);
        ic_line(p, 18.5f, 11, 39.5f, 7, 6.5f, false);
        break;
    case IC_HEART:        /* two lobes and a point */
        ic_dot(p, 15, 18, 11, false);
        ic_dot(p, 33, 18, 11, false);
        ic_tri(p, 4.6f, 22, 43.4f, 22, 24, 42, false);
        ic_rect(p, 15, 14, 18, 10, 0, false);
        break;
    case IC_STAR:         /* five points round a disc that hides the joins between them */
        ic_dot(p, 24, 25.5f, 9.6f, false);
        for (int k = 0; k < 5; k++)
        {
            float a = -1.5707963f + k * 1.2566371f, l = a - 0.6283185f, r = a + 0.6283185f;
            ic_tri(p, 24 + 22.5f * cosf(a), 25.5f + 22.5f * sinf(a), 24 + 9.2f * cosf(l), 25.5f + 9.2f * sinf(l),
                   24 + 9.2f * cosf(r), 25.5f + 9.2f * sinf(r), false);
        }
        break;
    case IC_METRONOME:    /* a pyramid, its arm swung to one side */
        ic_tri(p, 10, 42, 38, 42, 24, 5, false);
        ic_tri(p, 16, 37, 32, 37, 24, 13, true);
        ic_line(p, 24, 35, 33, 11, 3, false);
        ic_dot(p, 30.5f, 18, 3.5f, false);
        ic_rect(p, 7, 40, 34, 5, 2, false);
        break;
    case IC_SUN:          /* a disc and eight rays: brightness */
        ic_dot(p, 24, 24, 8, false);
        for (int k = 0; k < 8; k++)
        {
            float c = cosf(k * 0.7853982f), s = sinf(k * 0.7853982f);
            ic_line(p, 24 + 13 * c, 24 + 13 * s, 24 + 19 * c, 24 + 19 * s, 3.5f, false);
        }
        break;
    case IC_FILES:        /* a folder with its tab, a sheet sticking out of it */
        ic_rect(p, 4, 9, 16, 8, 3, false);
        ic_rect(p, 4, 13, 40, 28, 4, false);
        ic_rect(p, 10, 8, 26, 12, 2, true);
        ic_rect(p, 12, 10, 22, 10, 1, false);
        ic_rect(p, 4, 19, 40, 22, 4, false);
        ic_line(p, 9, 24, 39, 24, 1.5f, true);
        break;
    case IC_WIFI:         /* three rings fanning up from a dot */
        for (int k = 0; k < 3; k++) ic_arc(p, 24, 39, 8 + 9 * k, 225, 315, 4, false);
        ic_dot(p, 24, 39, 3.5f, false);
        break;
    case IC_BLUETOOTH:    /* the rune: a stem, and its two arrowheads crossing it */
        ic_line(p, 24, 6, 24, 42, 3.5f, false);
        ic_line(p, 24, 6, 34, 15, 3.5f, false);
        ic_line(p, 34, 15, 14, 33, 3.5f, false);
        ic_line(p, 24, 42, 34, 33, 3.5f, false);
        ic_line(p, 34, 33, 14, 15, 3.5f, false);
        break;
    case IC_SCREENSHOT:   /* a phone-shaped frame with its corners marked: the screen as it was */
        ic_rect(p, 13, 4, 22, 40, 5, false);
        ic_rect(p, 16, 7, 16, 34, 3, true);
        ic_rect(p, 19, 11, 10, 4, 1, false);
        ic_rect(p, 19, 18, 10, 14, 2, false);
        break;
    case IC_SHIFT:        /* the keyboard's shift: an arrow's outline */
    case IC_SHIFT_ON:     /* filled: the next letter is a capital */
    case IC_CAPS:         /* filled and underlined: capitals until it is tapped again */
        if (id == IC_SHIFT)
        {
            static const float a[][2] = { { 24, 7 }, { 41, 25 }, { 32, 25 }, { 32, 38 }, { 16, 38 }, { 16, 25 }, { 7, 25 } };
            for (int k = 0; k < 7; k++) ic_line(p, a[k][0], a[k][1], a[(k + 1) % 7][0], a[(k + 1) % 7][1], 3.4f, false);
        }
        else
        {
            ic_tri(p, 5.5f, 26, 42.5f, 26, 24, 5.5f, false);
            ic_rect(p, 15, 24, 18, 15.5f, 1.5f, false);
        }
        if (id == IC_CAPS) ic_rect(p, 15, 42, 18, 4, 2, false);
        break;
    case IC_GLOBE:        /* a sphere: its rim, the meridian and two parallels, the equator */
        ic_arc(p, 24, 24, 17, 0, 360, 3.2f, false);
        ic_arc(p, 41.5f, 24, 21.5f, 143, 217, 3.2f, false);
        ic_arc(p, 6.5f, 24, 21.5f, -37, 37, 3.2f, false);
        ic_line(p, 7.5f, 24, 40.5f, 24, 3.2f, false);
        ic_line(p, 12, 14.5f, 36, 14.5f, 3, false);
        ic_line(p, 12, 33.5f, 36, 33.5f, 3, false);
        break;
    case IC_CARTRIDGE:    /* a Game Boy cartridge: grip ridges along the top, the cut corner, its label */
        ic_rect(p, 9, 4, 30, 40, 3.5f, false);
        ic_tri(p, 31, 34, 40, 34, 40, 45, true);
        ic_rect(p, 13, 8, 22, 2, 1, true);
        ic_rect(p, 13, 12, 22, 2, 1, true);
        ic_rect(p, 13, 18, 22, 18, 2.5f, true);
        ic_rect(p, 16, 21, 16, 12, 1.5f, false);
        break;
    case IC_2048:         /* four tiles, the last one empty: the next to slide in */
        ic_rect(p, 4, 4, 19, 19, 4, false);
        ic_rect(p, 25, 4, 19, 19, 4, false);
        ic_rect(p, 4, 25, 19, 19, 4, false);
        ic_rect(p, 25, 25, 19, 19, 4, false);
        ic_rect(p, 28, 28, 13, 13, 2.5f, true);
        ic_rect(p, 9.5f, 12, 8, 3, 1.5f, true);
        break;
    case IC_SUDOKU:       /* three by three, three of them filled in */
        ic_rect(p, 4, 4, 40, 40, 6, false);
        for (int i = 0; i < 9; i++) ic_rect(p, 7.5f + (i % 3) * 11.5f, 7.5f + (i / 3) * 11.5f, 10, 10, 2, true);
        ic_dot(p, 12.5f, 12.5f, 3, false);
        ic_dot(p, 35.5f, 24, 3, false);
        ic_dot(p, 24, 35.5f, 3, false);
        break;
    case IC_POINTS:       /* two cards, the front one with a diamond */
        ic_rect(p, 5, 6, 22, 31, 3.5f, false);
        ic_rect(p, 16, 10, 28, 36, 5, true);
        ic_rect(p, 18, 12, 24, 32, 3.5f, false);
        ic_tri(p, 30, 19, 36, 28, 30, 37, true);
        ic_tri(p, 30, 19, 24, 28, 30, 37, true);
        break;
    case IC_FLAPPY:       /* a round little bird: an eye, a beak, a wing */
        ic_dot(p, 20, 26, 14, false);                 /* one round body, the beak out front */
        ic_rect(p, 28, 23, 17, 12, 5.5f, false);
        ic_line(p, 31, 29, 44, 29, 1.6f, true);       /* its two lips */
        ic_dot(p, 27, 18.5f, 8.5f, false);            /* the big eye, bulging, its pupil forward */
        ic_dot(p, 27, 18.5f, 6, true);
        ic_dot(p, 29, 19, 2.4f, false);
        ic_arc(p, 12.5f, 25.5f, 5, 0, 180, 2.6f, true);   /* the wing */
        break;
    case IC_GRAVE:        /* a headstone on its slab, a cross cut in it */
        ic_rect(p, 12, 6, 24, 36, 12, false);
        ic_rect(p, 12, 24, 24, 18, 0, false);
        ic_rect(p, 6, 39, 36, 6, 3, false);
        ic_rect(p, 22, 13, 4, 18, 1.5f, true);
        ic_rect(p, 16.5f, 18, 15, 4, 1.5f, true);
        break;
    case IC_HEADPHONES:   /* the band over the head, a cup each side */
        ic_arc(p, 24, 27, 17, 180, 0, 5, false);
        ic_rect(p, 4, 24, 11, 19, 4.5f, false);
        ic_rect(p, 33, 24, 11, 19, 4.5f, false);
        break;
    case IC_PODCAST:      /* a microphone in its cradle, on air */
        ic_rect(p, 17, 4, 14, 25, 7, false);
        for (int i = 0; i < 3; i++) ic_rect(p, 17, 10 + i * 5, 6, 2, 1, true);
        ic_arc(p, 24, 20, 12, 0, 180, 3.5f, false);
        ic_line(p, 24, 34, 24, 41, 3.5f, false);
        ic_line(p, 16, 43, 32, 43, 3.5f, false);
        ic_arc(p, 24, 17, 20, 205, 245, 3, false);
        ic_arc(p, 24, 17, 20, 295, 335, 3, false);
        break;
    case IC_SERVER:       /* a server: three slabs stacked, a light and a slot on each */
        for (int i = 0; i < 3; i++)
        {
            float y = 6 + i * 13;
            ic_rect(p, 6, y, 36, 10, 3.5f, false);
            ic_dot(p, 35, y + 5, 2, true);
            ic_rect(p, 11, y + 4, 13, 2, 1, true);
        }
        break;

    /* ---- symbols: line drawings at the words' weight (4.5 on the grid, a
     * tenth of the letter height), round ends, filled only where a shape
     * means a solid thing (transport, a knob) */
    case IC_G_EQ:         /* three faders, their knobs at different heights */
        for (int i = 0; i < 3; i++)
        {
            static const float ky[3] = { 30, 15, 25 };
            float x = 9 + i * 15;
            ic_line(p, x, 5, x, 43, 4.5f, false);
            ic_rect(p, x - 8.5f, ky[i] - 7.5f, 17, 15, 7.5f, true);
            ic_rect(p, x - 6.5f, ky[i] - 5.5f, 13, 11, 5.5f, false);
        }
        break;
    case IC_G_XFADE:      /* a solid ramp fading out, an outlined one fading in over its tail */
    {
        static const float in[] = { 16, 39, 42, 39, 42, 9, 16, 39 };
        ic_tri_r(p, 7.5f, 11, 7.5f, 37.5f, 30, 37.5f, 1.5f, false);
        ic_line(p, 16, 39, 42, 9, 11, true);
        ic_tri(p, 17, 39, 42, 39, 42, 10, true);   /* its inside clear: the solid one is behind */
        ic_poly(p, in, 4, 4.5f, false);
        break;
    }

    /* transport: solid, rounded */
    case IC_G_PLAY:
        ic_tri_r(p, 16, 10.5f, 16, 37.5f, 39, 24, 3, false);
        break;
    case IC_G_PAUSE:
        ic_rect(p, 11, 8, 10, 32, 3, false);
        ic_rect(p, 27, 8, 10, 32, 3, false);
        break;
    case IC_G_STOP:
        ic_rect(p, 9, 9, 30, 30, 6, false);
        break;
    case IC_G_NEXT:
        ic_tri_r(p, 10.5f, 12, 10.5f, 36, 29, 24, 2.5f, false);
        ic_rect(p, 32.5f, 9.5f, 6, 29, 3, false);
        break;
    case IC_G_PREV:
        ic_tri_r(p, 37.5f, 12, 37.5f, 36, 19, 24, 2.5f, false);
        ic_rect(p, 9.5f, 9.5f, 6, 29, 3, false);
        break;
    /* the rest: lines at the words' weight */
    case IC_G_SHUFFLE:
    {
        static const float b[] = { 6, 33, 13, 33, 29, 15, 39, 15 }, a[] = { 6, 15, 13, 15, 29, 33, 39, 33 };
        ic_poly(p, b, 4, 4.5f, false);
        ic_head(p, 40, 15, 1, 0, 6, 4.5f);
        ic_line(p, 17.5f, 20.5f, 24.5f, 28.5f, 9.5f, true);   /* one passes under the other */
        ic_poly(p, a, 4, 4.5f, false);
        ic_head(p, 40, 33, 1, 0, 6, 4.5f);
        break;
    }
    case IC_G_LOOP:
        ic_line(p, 9, 28, 9, 21, 4.5f, false);
        ic_arc(p, 16, 21, 7, 180, 270, 4.5f, false);
        ic_line(p, 16, 14, 38, 14, 4.5f, false);
        ic_head(p, 39, 14, 1, 0, 6, 4.5f);
        ic_line(p, 39, 20, 39, 27, 4.5f, false);
        ic_arc(p, 32, 27, 7, 0, 90, 4.5f, false);
        ic_line(p, 32, 34, 10, 34, 4.5f, false);
        ic_head(p, 9, 34, -1, 0, 6, 4.5f);
        break;
    case IC_G_REFRESH:        /* round the clock, the head at eleven o'clock pointing on */
        ic_arc(p, 24, 25, 14, 320, 250, 4.5f, false);
        ic_head(p, 24 - 14 * 0.342f, 25 - 14 * 0.940f, 0.940f, -0.342f, 8, 4.5f);
        break;
    case IC_G_AUDIO:
        ic_dot(p, 14, 36, 6, false);
        ic_dot(p, 34, 32, 6, false);
        ic_line(p, 19.5f, 36, 19.5f, 11, 4, false);
        ic_line(p, 39.5f, 32, 39.5f, 7, 4, false);
        ic_line(p, 19.5f, 11, 39.5f, 7, 6, false);
        break;
    case IC_G_LIST:
        for (int i = 0; i < 3; i++) { ic_dot(p, 9, 13 + i * 11, 3, false); ic_line(p, 17, 13 + i * 11, 40, 13 + i * 11, 4.5f, false); }
        break;
    case IC_G_BARS:
        for (int i = 0; i < 3; i++) ic_line(p, 9, 14 + i * 10, 39, 14 + i * 10, 4.5f, false);
        break;
    case IC_G_OK:
    {
        static const float t[] = { 9, 25, 19, 35, 39, 14 };
        ic_poly(p, t, 3, 5, false);
        break;
    }
    case IC_G_CLOSE:
        ic_line(p, 13, 13, 35, 35, 4.5f, false);
        ic_line(p, 35, 13, 13, 35, 4.5f, false);
        break;
    case IC_G_PLUS:
        ic_line(p, 24, 9, 24, 39, 4.5f, false);
        ic_line(p, 9, 24, 39, 24, 4.5f, false);
        break;
    case IC_G_MINUS:
        ic_line(p, 9, 24, 39, 24, 4.5f, false);
        break;
    case IC_G_LEFT: case IC_G_RIGHT: case IC_G_UP: case IC_G_DOWN:
    {
        static const float c[4][6] = { { 29, 9, 16, 24, 29, 39 }, { 19, 9, 32, 24, 19, 39 },
                                       { 9, 30, 24, 16, 39, 30 }, { 9, 18, 24, 32, 39, 18 } };
        ic_poly(p, c[id - IC_G_LEFT], 3, 5, false);
        break;
    }
    case IC_G_SETTINGS:       /* a solid cog, its hub open */
        for (int i = 0; i < 8; i++)
        {
            float a = i * 3.14159265f / 4, ca = cosf(a), sa = sinf(a);
            ic_line(p, 24 + 12 * ca, 24 + 12 * sa, 24 + 17.5f * ca, 24 + 17.5f * sa, 7.5f, false);
        }
        ic_dot(p, 24, 24, 14, false);
        ic_dot(p, 24, 24, 5.5f, true);
        break;
    case IC_G_HOME:
    {
        static const float roof[] = { 6, 24, 24, 8, 42, 24 }, body[] = { 11.5f, 20, 11.5f, 40, 36.5f, 40, 36.5f, 20 };
        ic_poly(p, roof, 3, 4.5f, false);
        ic_poly(p, body, 4, 4.5f, false);
        ic_rect(p, 20, 28, 8, 12, 2, false);
        break;
    }
    case IC_G_POWER:
        ic_arc(p, 24, 26, 14, 300, 240, 4.5f, false);
        ic_line(p, 24, 7, 24, 24, 4.5f, false);
        break;
    case IC_G_EDIT:           /* a pencil, writing down to the left */
        ic_line(p, 16, 32, 34, 14, 9, false);
        ic_tri_r(p, 8.5f, 39.5f, 11, 30, 18, 37, 1.5f, false);
        ic_line(p, 26.5f, 15.5f, 32.5f, 21.5f, 2.5f, true);
        break;
    case IC_G_TRASH:
    {
        static const float h[] = { 18.5f, 12, 18.5f, 7, 29.5f, 7, 29.5f, 12 }, b[] = { 12, 12, 14, 41, 34, 41, 36, 12 };
        ic_line(p, 7, 12, 41, 12, 4.5f, false);
        ic_poly(p, h, 4, 4, false);
        ic_poly(p, b, 4, 4.5f, false);
        ic_line(p, 20.5f, 19, 21, 34, 3.5f, false);
        ic_line(p, 27.5f, 19, 27, 34, 3.5f, false);
        break;
    }
    case IC_G_DIRECTORY:
    {
        static const float f[] = { 6, 38, 6, 11, 18, 11, 22, 16, 42, 16, 42, 38, 6, 38 };
        ic_poly(p, f, 7, 4.5f, false);
        break;
    }
    case IC_G_FILE:
    {
        static const float f[] = { 11, 6, 28, 6, 37, 15, 37, 42, 11, 42, 11, 6 }, d[] = { 28, 6, 28, 15, 37, 15 };
        ic_poly(p, f, 6, 4.5f, false);
        ic_poly(p, d, 3, 4, false);
        break;
    }
    case IC_G_IMAGE:
        ic_frame(p, 6, 9, 36, 30, 5, 4.5f);
        ic_dot(p, 32, 18, 3.5f, false);
        ic_tri_r(p, 11, 34, 20, 23, 29, 34, 1, false);
        ic_tri_r(p, 24, 34, 31, 27, 37, 34, 1, false);
        break;
    case IC_G_VIDEO:
        ic_frame(p, 5, 13, 27, 22, 5, 4.5f);
        ic_tri_r(p, 36, 24, 42.5f, 18, 42.5f, 30, 1.5f, false);
        break;
    case IC_G_EYE_OPEN: case IC_G_EYE_CLOSE:
        ic_arc(p, 24, 41.75f, 26, 223, 317, 4.5f, false);   /* the lids meet at (5, 24) and (43, 24) */
        ic_arc(p, 24, 6.25f, 26, 43, 137, 4.5f, false);
        ic_dot(p, 24, 24, 6.5f, false);
        if (id == IC_G_EYE_CLOSE)
        {
            ic_line(p, 9, 9, 39, 39, 10, true);
            ic_line(p, 9, 9, 39, 39, 4.5f, false);
        }
        break;
    case IC_G_BLUETOOTH:
    {
        static const float r[] = { 14, 15, 33, 32, 24, 41, 24, 7, 33, 16, 14, 33 };
        ic_poly(p, r, 6, 4.5f, false);
        break;
    }
    case IC_G_WIFI:
        for (int i = 0; i < 3; i++) ic_arc(p, 24, 39, 8.5f + i * 8.5f, 225, 315, 4.5f, false);
        ic_dot(p, 24, 39, 3.2f, false);
        break;
    case IC_G_VOLUME_MID: case IC_G_VOLUME_MAX:
    {
        float x = id == IC_G_VOLUME_MAX ? -3 : 0;
        ic_rect(p, x + 5, 18, 8, 12, 2.5f, false);
        ic_tri_r(p, x + 10, 24, x + 22, 11, x + 22, 37, 2, false);
        ic_arc(p, x + 24, 24, 8.5f, 315, 45, 4, false);
        if (id == IC_G_VOLUME_MAX) ic_arc(p, x + 24, 24, 16.5f, 310, 50, 4, false);
        break;
    }
    case IC_G_UPLOAD: case IC_G_DOWNLOAD:
    {
        static const float t[] = { 8, 31, 8, 40, 40, 40, 40, 31 };
        bool up = id == IC_G_UPLOAD;
        ic_line(p, 24, 7, 24, 30, 4.5f, false);
        ic_head(p, 24, up ? 7 : 30, 0, up ? -1 : 1, 10, 4.5f);
        ic_poly(p, t, 4, 4.5f, false);
        break;
    }
    case IC_G_WARNING:
    {
        static const float t[] = { 24, 7, 43, 39, 5, 39, 24, 7 };
        ic_poly(p, t, 4, 4.5f, false);
        ic_line(p, 24, 18, 24, 28, 4.5f, false);
        ic_dot(p, 24, 34, 2.6f, false);
        break;
    }
    case IC_G_BELL:
        ic_dot(p, 24, 21, 12, false);
        ic_rect(p, 12, 21, 24, 12, 0, false);
        ic_rect(p, 7, 31, 34, 6, 3, false);
        ic_dot(p, 24, 40.5f, 4, false);
        ic_dot(p, 24, 8, 3, false);
        break;
    case IC_G_CHARGE:
        ic_tri_r(p, 30, 5, 12, 27, 25, 27, 1, false);
        ic_tri_r(p, 18, 43, 36, 21, 23, 21, 1, false);
        break;
    case IC_G_USB:
    {
        static const float u[] = { 14, 24, 20, 15, 27, 15 }, d[] = { 20, 24, 26, 33, 30, 33 };
        ic_line(p, 9, 24, 37, 24, 4, false);
        ic_dot(p, 8, 24, 4, false);
        ic_tri_r(p, 36, 19, 36, 29, 43, 24, 1, false);
        ic_poly(p, u, 3, 4, false);
        ic_rect(p, 26, 12, 6, 6, 1, false);
        ic_poly(p, d, 3, 4, false);
        ic_dot(p, 31, 33, 3, false);
        break;
    }
    case IC_G_GPS:            /* the arrow that says: here */
        ic_tri_r(p, 40, 8, 9, 21, 23.5f, 25, 2, false);
        ic_tri_r(p, 40, 8, 23.5f, 25, 27, 39.5f, 2, false);
        break;
    case IC_G_KEYBOARD:
        ic_frame(p, 5, 12, 38, 25, 5, 4);
        for (int i = 0; i < 4; i++) { ic_dot(p, 13 + i * 7.3f, 19.5f, 2, false); ic_dot(p, 13 + i * 7.3f, 25, 2, false); }
        ic_line(p, 17, 30.5f, 31, 30.5f, 3, false);
        break;
    case IC_G_BACKSPACE:
    {
        static const float b[] = { 17, 10, 42, 10, 42, 38, 17, 38, 5, 24, 17, 10 };
        ic_poly(p, b, 6, 4.5f, false);
        ic_line(p, 24, 18, 34, 30, 4, false);
        ic_line(p, 34, 18, 24, 30, 4, false);
        break;
    }
    case IC_G_COPY:
        ic_frame(p, 8, 7, 22, 26, 4, 4.5f);
        ic_rect(p, 13, 12, 31, 33, 7, true);
        ic_frame(p, 17, 16, 22, 26, 4, 4.5f);
        break;
    case IC_G_CALL:           /* a handset */
        ic_arc(p, 33, 15, 18, 95, 175, 10, false);
        ic_rect(p, 25, 28, 17, 12, 6, false);
        ic_rect(p, 8, 6, 12, 17, 6, false);
        break;
    case IC_G_TINT:
        ic_dot(p, 24, 30, 11, false);
        ic_tri_r(p, 14, 26.5f, 34, 26.5f, 24, 7, 1, false);
        break;
    case IC_G_DRIVE:
        ic_frame(p, 5, 13, 38, 22, 5, 4.5f);
        ic_dot(p, 34, 24, 2.6f, false);
        ic_line(p, 12, 24, 25, 24, 4, false);
        break;
    case IC_G_SAVE:
    {
        static const float l[] = { 15, 7, 15, 16, 31, 16, 31, 7 };
        ic_frame(p, 7, 7, 34, 34, 5, 4.5f);
        ic_poly(p, l, 4, 4, false);
        ic_dot(p, 24, 30, 4, false);
        break;
    }
    case IC_G_BATTERY_FULL: case IC_G_BATTERY_3: case IC_G_BATTERY_2: case IC_G_BATTERY_1: case IC_G_BATTERY_EMPTY:
    {
        int level = IC_G_BATTERY_EMPTY - id;   /* 4 full .. 0 empty */
        ic_frame(p, 4, 15, 35, 18, 5, 4);
        ic_rect(p, 41, 20.5f, 3.5f, 7, 1.75f, false);
        if (level) ic_rect(p, 8.5f, 19.5f, 26 * level / 4.0f, 9, 2, false);
        break;
    }
    case IC_G_GLOBE:
        ic_arc(p, 24, 24, 16, 0, 359, 4, false);
        ic_line(p, 9, 24, 39, 24, 3.5f, false);
        ic_arc(p, 41, 24, 23, 135, 225, 3.5f, false);
        ic_arc(p, 7, 24, 23, 315, 45, 3.5f, false);
        break;
    default: break;
    }
}
static void sl_icon_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    sl_icon_paint(lv_event_get_layer(e), &a, (int)(intptr_t)lv_event_get_user_data(e),
                  lv_obj_get_style_text_color(o, 0), lv_obj_get_style_bg_color(o, 0));
}

/* An icon 'size' pixels square, in 'ink' over 'bg' (the colour it sits on). */
static lv_obj_t *sl_icon(lv_obj_t *parent, int id, int size, uint32_t ink, uint32_t bg)
{
    lv_obj_t *o = box(parent);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_text_color(o, lv_color_hex(ink), 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);   /* for cut-outs; not painted */
    lv_obj_add_event_cb(o, sl_icon_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)id);
    return o;
}

/* ------------------------------------------------------- symbol fonts */
/* sl_sym_<px>: the symbols labels draw, in our own drawings. Every font a
 * label uses ends its fallback chain at one of these (sl_xf_<px> falls back
 * to it, and a label that is only a symbol names it), so a symbol beside
 * words in any language comes from here. Each sits where Montserrat's did:
 * the same advance, centred on the centre of Montserrat's glyph, so no row
 * moves; anything not drawn here falls back to Montserrat. A glyph is drawn
 * the first time it shows and kept (a few KB for the sizes in use). */
#define SYM_SIZES 10
static const uint8_t sym_px[SYM_SIZES] = { 14, 16, 18, 20, 22, 24, 28, 32, 40, 48 };
static const lv_font_t *const sym_mont[SYM_SIZES] = {
    &lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_22,
    &lv_font_montserrat_24, &lv_font_montserrat_28, &lv_font_montserrat_32, &lv_font_montserrat_40, &lv_font_montserrat_48 };
/* codepoint, drawing, and the Montserrat glyph whose place it takes (its
 * own codepoint, or for a symbol Montserrat lacks, one shaped like it) */
static const struct { uint16_t cp, ref; uint8_t ic; } sym_map[] = {
    { 0xE000, 0xF013, IC_G_EQ },           /* SL_SYM_EQ */
    { 0xE001, 0xF074, IC_G_XFADE },        /* SL_SYM_XFADE */
    { 0xE002, 0xF013, IC_G_GLOBE },        /* SL_SYM_GLOBE */
    { 0xF001, 0xF001, IC_G_AUDIO },        /* LV_SYMBOL_AUDIO */
    { 0xF008, 0xF008, IC_G_VIDEO },        /* LV_SYMBOL_VIDEO */
    { 0xF00B, 0xF00B, IC_G_LIST },         /* LV_SYMBOL_LIST */
    { 0xF00C, 0xF00C, IC_G_OK },           /* LV_SYMBOL_OK */
    { 0xF00D, 0xF00D, IC_G_CLOSE },        /* LV_SYMBOL_CLOSE */
    { 0xF011, 0xF011, IC_G_POWER },        /* LV_SYMBOL_POWER */
    { 0xF013, 0xF013, IC_G_SETTINGS },     /* LV_SYMBOL_SETTINGS */
    { 0xF015, 0xF015, IC_G_HOME },         /* LV_SYMBOL_HOME */
    { 0xF019, 0xF019, IC_G_DOWNLOAD },     /* LV_SYMBOL_DOWNLOAD */
    { 0xF01C, 0xF01C, IC_G_DRIVE },        /* LV_SYMBOL_DRIVE */
    { 0xF021, 0xF021, IC_G_REFRESH },      /* LV_SYMBOL_REFRESH */
    { 0xF027, 0xF027, IC_G_VOLUME_MID },   /* LV_SYMBOL_VOLUME_MID */
    { 0xF028, 0xF028, IC_G_VOLUME_MAX },   /* LV_SYMBOL_VOLUME_MAX */
    { 0xF03E, 0xF03E, IC_G_IMAGE },        /* LV_SYMBOL_IMAGE */
    { 0xF043, 0xF043, IC_G_TINT },         /* LV_SYMBOL_TINT */
    { 0xF048, 0xF048, IC_G_PREV },         /* LV_SYMBOL_PREV */
    { 0xF04B, 0xF04B, IC_G_PLAY },         /* LV_SYMBOL_PLAY */
    { 0xF04C, 0xF04C, IC_G_PAUSE },        /* LV_SYMBOL_PAUSE */
    { 0xF04D, 0xF04D, IC_G_STOP },         /* LV_SYMBOL_STOP */
    { 0xF051, 0xF051, IC_G_NEXT },         /* LV_SYMBOL_NEXT */
    { 0xF053, 0xF053, IC_G_LEFT },         /* LV_SYMBOL_LEFT */
    { 0xF054, 0xF054, IC_G_RIGHT },        /* LV_SYMBOL_RIGHT */
    { 0xF067, 0xF067, IC_G_PLUS },         /* LV_SYMBOL_PLUS */
    { 0xF068, 0xF068, IC_G_MINUS },        /* LV_SYMBOL_MINUS */
    { 0xF06E, 0xF06E, IC_G_EYE_OPEN },     /* LV_SYMBOL_EYE_OPEN */
    { 0xF070, 0xF070, IC_G_EYE_CLOSE },    /* LV_SYMBOL_EYE_CLOSE */
    { 0xF071, 0xF071, IC_G_WARNING },      /* LV_SYMBOL_WARNING */
    { 0xF074, 0xF074, IC_G_SHUFFLE },      /* LV_SYMBOL_SHUFFLE */
    { 0xF077, 0xF077, IC_G_UP },           /* LV_SYMBOL_UP */
    { 0xF078, 0xF078, IC_G_DOWN },         /* LV_SYMBOL_DOWN */
    { 0xF079, 0xF079, IC_G_LOOP },         /* LV_SYMBOL_LOOP */
    { 0xF07B, 0xF07B, IC_G_DIRECTORY },    /* LV_SYMBOL_DIRECTORY */
    { 0xF093, 0xF093, IC_G_UPLOAD },       /* LV_SYMBOL_UPLOAD */
    { 0xF095, 0xF095, IC_G_CALL },         /* LV_SYMBOL_CALL */
    { 0xF0C5, 0xF0C5, IC_G_COPY },         /* LV_SYMBOL_COPY */
    { 0xF0C7, 0xF0C7, IC_G_SAVE },         /* LV_SYMBOL_SAVE */
    { 0xF0C9, 0xF0C9, IC_G_BARS },         /* LV_SYMBOL_BARS */
    { 0xF0E7, 0xF0E7, IC_G_CHARGE },       /* LV_SYMBOL_CHARGE */
    { 0xF0F3, 0xF0F3, IC_G_BELL },         /* LV_SYMBOL_BELL */
    { 0xF11C, 0xF11C, IC_G_KEYBOARD },     /* LV_SYMBOL_KEYBOARD */
    { 0xF124, 0xF124, IC_G_GPS },          /* LV_SYMBOL_GPS */
    { 0xF15B, 0xF15B, IC_G_FILE },         /* LV_SYMBOL_FILE */
    { 0xF1EB, 0xF1EB, IC_G_WIFI },         /* LV_SYMBOL_WIFI */
    { 0xF240, 0xF240, IC_G_BATTERY_FULL }, /* LV_SYMBOL_BATTERY_FULL */
    { 0xF241, 0xF241, IC_G_BATTERY_3 },    /* LV_SYMBOL_BATTERY_3 */
    { 0xF242, 0xF242, IC_G_BATTERY_2 },    /* LV_SYMBOL_BATTERY_2 */
    { 0xF243, 0xF243, IC_G_BATTERY_1 },    /* LV_SYMBOL_BATTERY_1 */
    { 0xF244, 0xF244, IC_G_BATTERY_EMPTY }, /* LV_SYMBOL_BATTERY_EMPTY */
    { 0xF287, 0xF287, IC_G_USB },          /* LV_SYMBOL_USB */
    { 0xF293, 0xF293, IC_G_BLUETOOTH },    /* LV_SYMBOL_BLUETOOTH */
    { 0xF2ED, 0xF2ED, IC_G_TRASH },        /* LV_SYMBOL_TRASH */
    { 0xF304, 0xF304, IC_G_EDIT },         /* LV_SYMBOL_EDIT */
    { 0xF55A, 0xF55A, IC_G_BACKSPACE },    /* LV_SYMBOL_BACKSPACE */
};
/* the drawing's square: a fifth over the size, so a line drawing inked to
 * within a tenth of its edges is as tall as the letters, at their weight */
#define SYM_BOX(px_) (((px_) * 6 + 2) / 5)
#define SYM_N ((int)(sizeof sym_map / sizeof sym_map[0]))
static uint8_t *sym_cache[SYM_SIZES][SYM_N];   /* each drawn glyph, 4 bits a pixel */
static unsigned long sym_bytes;
static int sym_find(uint32_t c)
{
    int lo = 0, hi = SYM_N - 1;
    while (lo <= hi)
    {
        int m = (lo + hi) / 2;
        if (sym_map[m].cp == c) return m;
        if (sym_map[m].cp < c) lo = m + 1; else hi = m - 1;
    }
    return -1;
}
static bool sym_dsc(const lv_font_t *f, lv_font_glyph_dsc_t *d, uint32_t c, uint32_t next)
{
    (void)next;
    if (c < 0xE000 || c > 0xF8FF) return false;
    int g = sym_find(c), si = (int)(intptr_t)f->user_data;
    if (g < 0) return false;
    lv_font_glyph_dsc_t m;
    if (!lv_font_get_glyph_dsc(sym_mont[si], &m, sym_map[g].ref, 0)) return false;
    int n = SYM_BOX(sym_px[si]);
    d->adv_w = m.adv_w;
    d->box_w = d->box_h = (uint16_t)n;
    d->ofs_x = (int16_t)((2 * m.ofs_x + m.box_w - n) / 2);
    d->ofs_y = (int16_t)((2 * m.ofs_y + m.box_h - n) / 2);
    d->format = LV_FONT_GLYPH_FORMAT_A8;
    d->is_placeholder = 0;
    d->gid.index = (uint32_t)si << 8 | (uint32_t)g;
    return true;
}
/* drawn once at a size, kept packed; unpacked into LVGL's glyph buffer at
 * each draw (as the letter packs are) */
static const void *sym_bitmap(lv_font_glyph_dsc_t *d, lv_draw_buf_t *buf)
{
    static const uint8_t opa4[16] = { 0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255 };
    int si = (int)(d->gid.index >> 8), g = (int)(d->gid.index & 0xff);
    if (si >= SYM_SIZES || g >= SYM_N || !buf) return NULL;
    int n = SYM_BOX(sym_px[si]);
    uint8_t **slot = &sym_cache[si][g];
    if (!*slot)
    {
        static uint8_t a8[SYM_BOX(48) * SYM_BOX(48)];
        memset(a8, 0, (size_t)n * n);
        /* optical sizing: the smaller the symbol, the heavier its strokes,
         * so a 16 px one has the presence a 28 px one has */
        int px = sym_px[si];
        float wk = px <= 18 ? 1.3f : px <= 22 ? 1.18f : px <= 24 ? 1.1f : 1.0f;
        struct ic_pen pen = { NULL, 0, 0, n / 48.0f, lv_color_white(), lv_color_black(), a8, n, n, n, wk };
        ic_paint(&pen, sym_map[g].ic);
        uint8_t *q = lv_malloc(((size_t)n * n + 1) / 2);
        if (!q) return NULL;
        for (int i = 0; i < n * n; i += 2)
            q[i >> 1] = (uint8_t)(((a8[i] + 8) * 15 / 255) << 4 | (i + 1 < n * n ? (a8[i + 1] + 8) * 15 / 255 : 0));
        sym_bytes += ((size_t)n * n + 1) / 2;
        *slot = q;
    }
    const uint8_t *in = *slot;
    uint8_t *out = buf->data;
    uint32_t stride = lv_draw_buf_width_to_stride(n, LV_COLOR_FORMAT_A8);
    int i = 0;
    for (int y = 0; y < n; y++, out += stride)
        for (int x = 0; x < n; x++, i++)
            out[x] = opa4[i & 1 ? in[i >> 1] & 15 : in[i >> 1] >> 4];
    return buf;
}
#define SYM_FONT(px_, i_, lh_, bl_, up_, ut_) \
    const lv_font_t sl_sym_##px_ = { .get_glyph_dsc = sym_dsc, .get_glyph_bitmap = sym_bitmap, \
        .line_height = lh_, .base_line = bl_, .subpx = LV_FONT_SUBPX_NONE, \
        .underline_position = up_, .underline_thickness = ut_, .dsc = NULL, \
        .fallback = &lv_font_montserrat_##px_, .user_data = (void *)(intptr_t)(i_) };
SYM_FONT(14, 0, 16, 3, -1, 1)
SYM_FONT(16, 1, 18, 3, -1, 1)
SYM_FONT(18, 2, 21, 4, -1, 1)
SYM_FONT(20, 3, 22, 4, -1, 1)
SYM_FONT(22, 4, 24, 4, -2, 1)
SYM_FONT(24, 5, 27, 5, -2, 1)
SYM_FONT(28, 6, 30, 5, -2, 1)
SYM_FONT(32, 7, 35, 6, -2, 2)
SYM_FONT(40, 8, 44, 8, -3, 2)
SYM_FONT(48, 9, 52, 9, -4, 2)
