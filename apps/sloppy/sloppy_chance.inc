/* Two offline touch toys: Flip a Coin and Magic 8 Ball. GPLv2+. */
#define CHANCE_BG 0x0b1220
#define CHANCE_CARD 0x152338
#define CHANCE_INK 0xf4f2ed
#define CHANCE_MUTED 0xa9b7ca

static void chance_page_skin(lv_obj_t *page, uint32_t bg, uint32_t ink)
{
    lv_obj_set_style_bg_color(page, lv_color_hex(bg), 0);
    lv_obj_t *header = lv_obj_get_child(page, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(header); i++)
    {
        lv_obj_t *item = lv_obj_get_child(header, i);
        if (!lv_obj_check_type(item, &lv_label_class) && lv_obj_get_child_count(item))
            item = lv_obj_get_child(item, 0);
        if (lv_obj_check_type(item, &lv_label_class))
            lv_obj_set_style_text_color(item, lv_color_hex(ink), 0);
    }
}

/* ------------------------------------------------------------ Flip a Coin
 * A clean white table in the spirit of Tiny Decisions, and one thick gold
 * coin. Swipe up (or tap, or press Flip) and it is thrown almost to the
 * status bar, tumbling end over end, then drops, bounces twice and rocks
 * itself flat. Heads is a tabby cat on pink, tails is the dog on gold.
 *
 * The coin is drawn by hand into one small ARGB canvas every frame. It only
 * ever turns about the horizontal axis, so the visible face is its texture
 * squeezed vertically (one row lookup per line) and the reeded rim is the
 * union of the face ellipses between the two faces. Everything is
 * anti-aliased analytically, and the page costs ~50k pixels a frame. */

#define COIN_TEX 224                          /* face texture and diameter */
#define COIN_R (COIN_TEX / 2)
#define COIN_FIELD (COIN_R - 14)               /* coloured field inside the rim */
#define COIN_T 20                              /* rim thickness */
#define COIN_CW (COIN_TEX + 4)
#define COIN_CH (COIN_TEX + COIN_T + 10)
#define COIN_REST_Y 500                        /* screen y of the coin at rest */
#define COIN_TOP_Y (SB_H + COIN_CH / 2 + 6)    /* highest the centre may go */
#define COIN_SETTLE_MS 980                     /* touchdown to rest */
#define COIN_REVEAL_MS 110                     /* touchdown to the verdict */
#define COIN_BITS 22                           /* confetti pieces */
#define COIN_HISTORY 10
/* Tiny Decisions: white in the light theme, true black in either dark one */
#define COIN_LIGHT (sl_theme() == 1)
#define COIN_PAPER (COIN_LIGHT ? 0xffffff : 0x000000)
#define COIN_INK (COIN_LIGHT ? 0x1d1d1f : 0xffffff)
#define COIN_GREY (COIN_LIGHT ? 0x6e6e73 : 0x9a9aa2)   /* 5:1 on the light paper */
#define COIN_SOFT (COIN_LIGHT ? 0x5c5c63 : 0xc7c7cc)
#define COIN_FILL (COIN_LIGHT ? 0xf0f0f4 : 0x1c1c1e)
#define COIN_FILL_P (COIN_LIGHT ? 0xdedee4 : 0x2c2c2e)
#define COIN_PINK 0xe8577a
#define COIN_AMBER 0xd48a16
#define COIN_BLUE 0x3478f6

enum { COIN_IDLE, COIN_DRAG, COIN_SPRING, COIN_FLIGHT };

static lv_color32_t coin_tex[2][COIN_TEX * COIN_TEX];   /* 0 heads, 1 tails */
static lv_color32_t coin_px[COIN_CW * COIN_CH];
static uint8_t coin_gold_lut[256][3];
static uint8_t coin_rim_col[COIN_CW];
#define COIN_SHW 200
#define COIN_SHH 36
static lv_color32_t coin_shadow_px[COIN_SHW * COIN_SHH];
static bool coin_art_ready;

static lv_obj_t *coin_page_obj, *coin_canvas, *coin_shadow, *coin_stage;
static lv_obj_t *coin_title_box, *coin_caption, *coin_tally, *coin_history;
static lv_obj_t *coin_hint, *coin_hint_arrow, *coin_flip_btn, *coin_button_text, *coin_reset_btn;
static lv_obj_t *coin_bit[COIN_BITS];
static lv_timer_t *coin_timer;
static int coin_state;
static uint32_t coin_started, coin_air_ms, coin_flips, coin_head_count, coin_seed;
static uint32_t coin_spring_at, coin_glint_at, coin_next_glint, coin_burst_at, coin_pop_at;
static bool coin_heads, coin_front_heads, coin_in_flight, coin_landed, coin_revealed;
static float coin_theta, coin_lift, coin_sheen = -9;
static float coin_theta0, coin_lift0, coin_turn, coin_rise, coin_power;
static float coin_spring_theta, coin_spring_lift;
static int coin_drag_y0, coin_drag_last_y;
static uint32_t coin_drag_last_ms;
static float coin_drag_vy;
static int16_t coin_bit_vx[COIN_BITS], coin_bit_vy[COIN_BITS], coin_bit_spin[COIN_BITS];
static uint8_t coin_bit_w[COIN_BITS], coin_bit_h[COIN_BITS];

/* The tally and the last flips outlive the page and a reboot: "SLC1 flips
 * heads HTTH..." with the newest flip last. */
#define COIN_FILE ROCKBOX_DIR "/sloppy/coin.txt"
static char coin_hist[COIN_HISTORY + 1];
static bool coin_loaded;
static void coin_load(void)
{
    if (coin_loaded) return;
    coin_loaded = true;
    char b[64];
    int fd = open(COIN_FILE, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (fd >= 0) close(fd);
    if (got < 5 || strncmp(b, "SLC1 ", 5)) return;
    b[got] = 0;
    char *end;
    unsigned long flips = strtoul(b + 5, &end, 10), heads = strtoul(end, &end, 10);
    if (heads > flips) return;
    coin_flips = (uint32_t)flips;
    coin_head_count = (uint32_t)heads;
    while (*end == ' ') end++;
    int n = 0;
    for (; (end[n] == 'H' || end[n] == 'T') && n < COIN_HISTORY; n++) coin_hist[n] = end[n];
    coin_hist[n] = 0;
    if (n) coin_heads = coin_hist[n - 1] == 'H';
}
static void coin_save(void)
{
    char b[64];
    int len = snprintf(b, sizeof b, "SLC1 %lu %lu %s\n", (unsigned long)coin_flips,
                       (unsigned long)coin_head_count, coin_hist);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(COIN_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool ok = write(fd, b, len) == len;
    if (ok) ok = fsync(fd) == 0;   /* a power cut must not leave an empty file */
    if (close(fd) < 0) ok = false;
    if (!ok || rename(COIN_FILE ".tmp", COIN_FILE) < 0) remove(COIN_FILE ".tmp");
}

static uint32_t coin_now(void) { return lv_tick_get(); }
static float coin_clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static uint32_t coin_rand(void)
{
    coin_seed = coin_seed * 1664525u + 1013904223u + coin_now();
    return coin_seed >> 8;
}

/* ---- the art: signed distance shapes, painted once per page ---------- */
static float cf_len(float x, float y) { return sqrtf(x * x + y * y); }
static float cf_circle(float x, float y, float cx, float cy, float r)
{
    return cf_len(x - cx, y - cy) - r;
}
static float cf_ellipse(float x, float y, float cx, float cy, float rx, float ry, float rot)
{
    float dx = x - cx, dy = y - cy;
    if (rot != 0)
    {
        float c = cosf(rot), s = sinf(rot), t = dx * c + dy * s;
        dy = -dx * s + dy * c;
        dx = t;
    }
    return (cf_len(dx / rx, dy / ry) - 1) * (rx < ry ? rx : ry);
}
static float cf_segment(float x, float y, float ax, float ay, float bx, float by, float r)
{
    float pax = x - ax, pay = y - ay, bax = bx - ax, bay = by - ay;
    float h = coin_clamp01((pax * bax + pay * bay) / (bax * bax + bay * bay));
    return cf_len(pax - bax * h, pay - bay * h) - r;
}
/* the lower half of a ring: smiles and the cat's "w" */
static float cf_smile(float x, float y, float cx, float cy, float r, float w)
{
    float d = fabsf(cf_len(x - cx, y - cy) - r) - w;
    return d > cy - y ? d : cy - y;
}
/* rounded triangle (Inigo Quilez's exact triangle distance) */
static float cf_tri(float x, float y, float x0, float y0, float x1, float y1,
                    float x2, float y2, float round)
{
    float e[3][2] = { { x1 - x0, y1 - y0 }, { x2 - x1, y2 - y1 }, { x0 - x2, y0 - y2 } };
    float v[3][2] = { { x - x0, y - y0 }, { x - x1, y - y1 }, { x - x2, y - y2 } };
    float s = e[0][0] * e[2][1] - e[0][1] * e[2][0] > 0 ? 1 : -1;
    float best = 1e9f, side = 1e9f;
    for (int i = 0; i < 3; i++)
    {
        float h = coin_clamp01((v[i][0] * e[i][0] + v[i][1] * e[i][1]) /
                               (e[i][0] * e[i][0] + e[i][1] * e[i][1]));
        float qx = v[i][0] - e[i][0] * h, qy = v[i][1] - e[i][1] * h;
        float dd = qx * qx + qy * qy;
        float sd = s * (v[i][0] * e[i][1] - v[i][1] * e[i][0]);
        if (dd < best) best = dd;
        if (sd < side) side = sd;
    }
    return -sqrtf(best) * (side < 0 ? -1 : 1) - round;
}
static void cf_paint(float *c, uint32_t rgb, float d, float alpha)
{
    float a = 0.5f - d;
    if (a <= 0) return;
    a = (a > 1 ? 1 : a) * alpha;
    c[0] += ((rgb >> 16 & 255) - c[0]) * a;
    c[1] += ((rgb >> 8 & 255) - c[1]) * a;
    c[2] += ((rgb & 255) - c[2]) * a;
}

/* dark bronze to white gold; everything metal is a lookup into this */
static void coin_build_luts(void)
{
    static const float stop[6][3] = {
        { 74, 42, 12 }, { 142, 90, 28 }, { 206, 148, 52 },
        { 240, 196, 92 }, { 255, 230, 150 }, { 255, 250, 228 } };
    for (int i = 0; i < 256; i++)
    {
        float v = i / 255.0f * 5;
        int k = v >= 5 ? 4 : (int)v;
        float f = v - k;
        for (int ch = 0; ch < 3; ch++)
            coin_gold_lut[i][ch] = (uint8_t)(stop[k][ch] + (stop[k + 1][ch] - stop[k][ch]) * f);
    }
    /* the reeded edge seen as a cylinder: lit from the upper left, with
     * ridges that crowd together towards both sides */
    for (int i = 0; i < COIN_CW; i++)
    {
        float nx = (i + 0.5f - COIN_CW / 2.0f) / COIN_R;
        if (nx > 1) nx = 1;
        if (nx < -1) nx = -1;
        float face = sqrtf(1 - nx * nx);
        float ridge = 0.5f + 0.5f * cosf(asinf(nx) * 118);
        float v = 0.30f + 0.42f * face - 0.16f * nx;
        v *= 1 - 0.30f * ridge * face;
        coin_rim_col[i] = (uint8_t)(coin_clamp01(v) * 255);
    }
}
static void coin_gold(float v, float *c)
{
    const uint8_t *g = coin_gold_lut[(int)(coin_clamp01(v) * 255)];
    c[0] = g[0]; c[1] = g[1]; c[2] = g[2];
}

static void coin_art_cat(float x, float y, float *c)
{
    /* a soft relief shadow under the whole head, then the tabby itself */
    float sx = x - 1.5f, sy = y - 2.5f;
    float sil = cf_ellipse(sx, sy, 0, 10, 58, 47, 0);
    float ear = cf_tri(sx, sy, -52, -12, -44, -60, -14, -36, 4);
    if (ear < sil) sil = ear;
    ear = cf_tri(sx, sy, 52, -12, 44, -60, 14, -36, 4);
    if (ear < sil) sil = ear;
    cf_paint(c, 0xa4485e, sil - 1, 0.30f);
    for (int side = -1; side <= 1; side += 2)
    {
        cf_paint(c, 0xd9803a, cf_tri(x, y, side * 52.f, -12, side * 44.f, -60, side * 14.f, -36, 4) - 2, 1);
        cf_paint(c, 0xf6a957, cf_tri(x, y, side * 52.f, -12, side * 44.f, -60, side * 14.f, -36, 4), 1);
        cf_paint(c, 0xf7b2bd, cf_tri(x, y, side * 44.f, -22, side * 41.f, -50, side * 24.f, -35, 3), 1);
    }
    cf_paint(c, 0xd9803a, cf_ellipse(x, y, 0, 10, 58, 47, 0) - 2, 1);
    cf_paint(c, 0xf6a957, cf_ellipse(x, y, 0, 10, 58, 47, 0), 1);
    /* a lighter crown and tabby stripes */
    cf_paint(c, 0xf9bd74, cf_ellipse(x, y, -8, -14, 34, 18, -0.2f), 0.55f);
    cf_paint(c, 0xdc7f2e, cf_segment(x, y, 0, -35, 0, -22, 3.4f), 1);
    cf_paint(c, 0xdc7f2e, cf_segment(x, y, -13, -32, -9, -21, 3), 1);
    cf_paint(c, 0xdc7f2e, cf_segment(x, y, 13, -32, 9, -21, 3), 1);
    cf_paint(c, 0xdc7f2e, cf_segment(x, y, -57, 4, -46, 6, 2.6f), 1);
    cf_paint(c, 0xdc7f2e, cf_segment(x, y, 57, 4, 46, 6, 2.6f), 1);
    /* muzzle, blush, eyes, nose, mouth, whiskers */
    cf_paint(c, 0xfff3e3, cf_circle(x, y, -11, 24, 14), 1);
    cf_paint(c, 0xfff3e3, cf_circle(x, y, 11, 24, 14), 1);
    cf_paint(c, 0xff8aa0, cf_ellipse(x, y, -38, 16, 9, 6, 0), 0.55f);
    cf_paint(c, 0xff8aa0, cf_ellipse(x, y, 38, 16, 9, 6, 0), 0.55f);
    for (int side = -1; side <= 1; side += 2)
    {
        cf_paint(c, 0x3b2417, cf_ellipse(x, y, side * 22.f, -3, 7.5f, 10, 0), 1);
        cf_paint(c, 0xffffff, cf_circle(x, y, side * 22.f + 2.4f, -7, 2.8f), 1);
        cf_paint(c, 0xffffff, cf_circle(x, y, side * 22.f - 2.6f, 1.5f, 1.3f), 0.8f);
        cf_paint(c, 0xb0683a, cf_segment(x, y, side * 44.f, 17, side * 70.f, 11, 1.1f), 0.8f);
        cf_paint(c, 0xb0683a, cf_segment(x, y, side * 45.f, 24, side * 71.f, 26, 1.1f), 0.8f);
    }
    cf_paint(c, 0xe86f84, cf_ellipse(x, y, 0, 13, 6.5f, 4.6f, 0), 1);
    cf_paint(c, 0x6b3a26, cf_segment(x, y, 0, 16, 0, 20, 1.1f), 1);
    cf_paint(c, 0x6b3a26, cf_smile(x, y, -5.5f, 19, 5.5f, 1.2f), 1);
    cf_paint(c, 0x6b3a26, cf_smile(x, y, 5.5f, 19, 5.5f, 1.2f), 1);
}

/* tails is literally the dog's tail: the same brown dog seen from behind,
 * floppy ears over its shoulders, paw pads up, and its tail wagging */
static float coin_tail(float x, float y, float grow)
{
    /* a quadratic curve up and to the right, thinning towards the tip */
    float best = 1e9f, px = 2, py = 4;
    for (int k = 1; k <= 10; k++)
    {
        float t = k / 10.0f, u = 1 - t;
        float qx = u * u * 2 + 2 * u * t * 46 + t * t * 38;
        float qy = u * u * 4 + 2 * u * t * 8 + t * t * -44;
        float d = cf_segment(x, y, px, py, qx, qy, 8.5f - 3 * t + grow);
        if (d < best) best = d;
        px = qx; py = qy;
    }
    return best;
}
static void coin_art_dog(float x, float y, float *c)
{
    float sx = x - 1.5f, sy = y - 2.5f;
    float sil = cf_ellipse(sx, sy, 0, 20, 52, 46, 0);
    float part = cf_circle(sx, sy, 0, -36, 30);
    if (part < sil) sil = part;
    part = coin_tail(sx, sy, 0);
    if (part < sil) sil = part;
    cf_paint(c, 0x7a4f12, sil - 1, 0.30f);
    /* the back of the head, ears hanging over the shoulders */
    cf_paint(c, 0x51301f, cf_circle(x, y, 0, -36, 30) - 2, 1);
    cf_paint(c, 0x6e4429, cf_circle(x, y, 0, -36, 30), 1);
    for (int side = -1; side <= 1; side += 2)
    {
        cf_paint(c, 0x2e190d, cf_ellipse(x, y, side * 33.f, -24, 11, 25, side * 0.45f) - 1.5f, 1);
        cf_paint(c, 0x3f2415, cf_ellipse(x, y, side * 33.f, -24, 11, 25, side * 0.45f), 1);
    }
    /* the round rump and two hind paws with their pads showing */
    for (int side = -1; side <= 1; side += 2)
    {
        cf_paint(c, 0x51301f, cf_ellipse(x, y, side * 27.f, 58, 18, 13, 0) - 2, 1);
        cf_paint(c, 0x6e4429, cf_ellipse(x, y, side * 27.f, 58, 18, 13, 0), 1);
    }
    cf_paint(c, 0x51301f, cf_ellipse(x, y, 0, 20, 52, 46, 0) - 2, 1);
    cf_paint(c, 0x6e4429, cf_ellipse(x, y, 0, 20, 52, 46, 0), 1);
    cf_paint(c, 0x80523a, cf_ellipse(x, y, -14, 0, 26, 16, -0.3f), 0.55f);
    cf_paint(c, 0x5a3520, cf_segment(x, y, 0, 30, 0, 58, 1.2f), 0.6f);
    for (int side = -1; side <= 1; side += 2)
    {
        float px = side * 27.f;
        cf_paint(c, 0x6e4429, cf_ellipse(x, y, px, 62, 16, 10, 0), 1);
        cf_paint(c, 0xe8b271, cf_ellipse(x, y, px, 64, 7, 5.5f, 0), 1);
        for (int toe = -1; toe <= 1; toe++)
            cf_paint(c, 0xe8b271, cf_circle(x, y, px + toe * 6.5f, 56 - (toe ? 0 : 2), 2.8f), 1);
    }
    /* the tail, a tan tip, and wag lines on both sides */
    cf_paint(c, 0x3a2012, coin_tail(x, y, 2.5f), 1);
    cf_paint(c, 0x8f5d38, coin_tail(x, y, 0), 1);
    cf_paint(c, 0xa87048, coin_tail(x - 2, y + 1, -4.5f), 0.6f);
    cf_paint(c, 0xe8b271, cf_circle(x, y, 38, -44, 5.5f), 1);
    cf_paint(c, 0x5a3520, cf_segment(x, y, 52, -52, 60, -60, 1.6f), 0.7f);
    cf_paint(c, 0x5a3520, cf_segment(x, y, 55, -38, 65, -40, 1.6f), 0.7f);
    cf_paint(c, 0x5a3520, cf_segment(x, y, 24, -56, 22, -66, 1.6f), 0.5f);
}

static void coin_paint_row(lv_color32_t *out, bool heads, int j)
{
    /* field: blush pink for the cat, warm gold for the dog, lit from the
     * upper left and sunk a little below the rim */
    const float top[2][3] = { { 255, 222, 228 }, { 255, 228, 160 } };
    const float low[2][3] = { { 241, 170, 186 }, { 232, 180, 92 } };
    int k = heads ? 0 : 1;
    for (int i = 0; i < COIN_TEX; i++)
    {
        float x = i + 0.5f - COIN_R, y = j + 0.5f - COIN_R, r = cf_len(x, y);
        float c[3], m[3];
        float lit = coin_clamp01(0.5f + (-x * 0.45f - y * 0.8f) / (COIN_FIELD * 2.2f));
        float sink = 1 - 0.20f * pow(coin_clamp01(r / COIN_FIELD), 12);
        for (int ch = 0; ch < 3; ch++)
            c[ch] = (low[k][ch] + (top[k][ch] - low[k][ch]) * lit) * sink;
        if (r < COIN_FIELD)
        {
            if (heads) coin_art_cat(x, y, c);
            else coin_art_dog(x, y, c);
        }
        /* a thin dark groove, then the bevelled rim */
        coin_gold(0.16f, m);
        float a = coin_clamp01(r - COIN_FIELD + 0.5f);
        for (int ch = 0; ch < 3; ch++) c[ch] += (m[ch] - c[ch]) * a;
        float t = coin_clamp01((r - COIN_FIELD - 1.5f) / (COIN_R - COIN_FIELD - 1.5f));
        float ndl = r > 0.5f ? -cosf(t * 3.14159f) * (-x * 0.55f - y * 0.8f) / r : 0;
        coin_gold(0.60f + 0.34f * ndl + 0.10f * sinf(t * 3.14159f) +
                  0.025f * sinf(r * 1.7f), m);
        a = coin_clamp01(r - COIN_FIELD - 1.5f + 0.5f);
        for (int ch = 0; ch < 3; ch++) c[ch] += (m[ch] - c[ch]) * a;
        out[j * COIN_TEX + i] = (lv_color32_t){ .blue = (uint8_t)c[2], .green = (uint8_t)c[1],
                                                .red = (uint8_t)c[0], .alpha = 255 };
    }
}

/* Both faces took 1.4 s to paint on the R1 (float shading of 100k
 * pixels), all at once on the first open: the watchdog caught it. They
 * are painted a few rows at a time while the player sits idle, so the
 * page opens at once; whatever is left is finished on open, as before. */
static int coin_rows;                    /* of 2 * COIN_TEX painted */
static bool coin_paint(int ms)           /* 0: all that is left */
{
    uint32_t t0 = sl_ms();
    if (!coin_rows) coin_build_luts();
    while (coin_rows < 2 * COIN_TEX && (!ms || (int)(sl_ms() - t0) < ms))
    {
        bool heads = coin_rows < COIN_TEX;
        coin_paint_row(coin_tex[heads ? 0 : 1], heads, coin_rows % COIN_TEX);
        coin_rows++;
    }
    coin_art_ready = coin_rows == 2 * COIN_TEX;
    return coin_art_ready;
}
static void coin_warm(lv_timer_t *t)
{
    if (coin_art_ready) { lv_timer_delete(t); return; }
    /* on a still home screen only: a slice during a game would hitch it */
    if (npages <= 1 && !np_open && g_state == G_IDLE && !lv_anim_count_running() &&
        sl_ms() - last_touch_ms > 1200)
        coin_paint(8);
}

/* ---- one frame of the coin ------------------------------------------- */
static void coin_render(void)
{
    float c = cosf(coin_theta), s = sinf(coin_theta), ac = fabsf(c);
    float sgn = c >= 0 ? 1 : -1;
    bool heads = c >= 0 ? coin_front_heads : !coin_front_heads;
    const lv_color32_t *tex = coin_tex[heads ? 0 : 1];
    float h = COIN_R * ac + 0.5f;                     /* half height, AA-padded */
    float yv = -(COIN_T * 0.5f) * s * sgn;            /* visible face centre */
    float lo = yv < 0 ? yv : -yv, hi = -lo;           /* the rim joins both faces */
    bool rim = fabsf(s) > 0.004f;
    /* a face turned up catches the light; one turned down goes dusky */
    float dot = 0.5f * s * sgn + 0.866f * ac;
    int face_l = (int)(256 * (0.70f + 0.36f * dot));
    int rim_k = (int)(256 * (1 + (yv < 0 ? -0.22f : 0.22f) * ac));
    /* a band of light sweeps the face as it turns, or when it glints */
    float sheen = coin_sheen > -8 ? coin_sheen : sinf(coin_theta) * 1.5f;
    float sheen_amp = coin_sheen > -8 ? 0.34f : 0.40f * ac;
    float inv_ac = 1.0f / (ac > 0.001f ? ac : 0.001f);
    int x0 = (COIN_CW - COIN_TEX) / 2;

    for (int j = 0; j < COIN_CH; j++)
    {
        lv_color32_t *row = &coin_px[j * COIN_CW];
        memset(row, 0, sizeof(lv_color32_t) * COIN_CW);
        float y = j + 0.5f - COIN_CH / 2.0f;
        float d = y - (y < lo ? lo : y > hi ? hi : y);
        float hr = rim && fabsf(d) < h ? COIN_R * sqrtf(1 - d * d / (h * h)) : -1;
        float df = y - yv;
        float hf = fabsf(df) < h ? COIN_R * sqrtf(1 - df * df / (h * h)) : -1;
        if (hr < 0 && hf < 0) continue;
        float fy = COIN_R + df * inv_ac - 0.5f;
        if (fy < 0) fy = 0;
        if (fy > COIN_TEX - 1.001f) fy = COIN_TEX - 1.001f;
        int r0 = (int)fy, wy = (int)((fy - r0) * 256);
        const lv_color32_t *t0 = &tex[r0 * COIN_TEX], *t1 = &tex[(r0 + 1) * COIN_TEX];
        float v = (fy - COIN_R) / COIN_R;
        float span = (hf > hr ? hf : hr) + 1.5f;
        int i0 = (int)(COIN_CW / 2.0f - span), i1 = (int)(COIN_CW / 2.0f + span) + 1;
        if (i0 < 0) i0 = 0;
        if (i1 > COIN_CW) i1 = COIN_CW;
        for (int i = i0; i < i1; i++)
        {
            float x = i + 0.5f - COIN_CW / 2.0f, ax = fabsf(x);
            float cf = hf < 0 ? 0 : coin_clamp01(hf - ax + 0.5f);
            float cr = hr < 0 ? 0 : coin_clamp01(hr - ax + 0.5f);
            if (cf <= 0 && cr <= 0) continue;
            int rr = 0, rg = 0, rb = 0, fr = 0, fg = 0, fb = 0;
            if (cr > 0)
            {
                int g = coin_rim_col[i] * rim_k >> 8;
                const uint8_t *m = coin_gold_lut[g > 255 ? 255 : g];
                rr = m[0]; rg = m[1]; rb = m[2];
            }
            if (cf > 0)
            {
                int tx = i - x0;
                if (tx < 0) tx = 0;
                if (tx >= COIN_TEX) tx = COIN_TEX - 1;
                const lv_color32_t *p0 = &t0[tx], *p1 = &t1[tx];
                fr = (p0->red * (256 - wy) + p1->red * wy) >> 8;
                fg = (p0->green * (256 - wy) + p1->green * wy) >> 8;
                fb = (p0->blue * (256 - wy) + p1->blue * wy) >> 8;
                fr = fr * face_l >> 8; fg = fg * face_l >> 8; fb = fb * face_l >> 8;
                float q = (x * 0.6f - v * COIN_R * 0.8f) / COIN_R - sheen;
                if (q > -0.24f && q < 0.24f)
                {
                    float w = 1 - q * q / 0.0576f;
                    int add = (int)(w * w * sheen_amp * 255);
                    fr += add; fg += add; fb += add * 9 / 10;
                }
                if (fr > 255) fr = 255;
                if (fg > 255) fg = 255;
                if (fb > 255) fb = 255;
            }
            /* face over rim, straight alpha */
            float a = cf + cr * (1 - cf);
            float wf = cf / a, wr = 1 - wf;
            row[i] = (lv_color32_t){
                .red = (uint8_t)(fr * wf + rr * wr), .green = (uint8_t)(fg * wf + rg * wr),
                .blue = (uint8_t)(fb * wf + rb * wr), .alpha = (uint8_t)(a * 255) };
        }
    }
    int max_rise = COIN_REST_Y - COIN_TOP_Y;
    lv_obj_set_pos(coin_canvas, (W - COIN_CW) / 2, COIN_REST_Y - (int)lroundf(coin_lift) - COIN_CH / 2);
    lv_obj_invalidate(coin_canvas);
    /* the shadow stays on the table, shrinking and fading as it rises */
    float up = coin_clamp01(coin_lift / max_rise);
    int sw = (int)(170 - 100 * up);
    lv_image_set_scale_x(coin_shadow, (uint32_t)(256 * sw / 170));
    lv_obj_set_style_image_opa(coin_shadow, (lv_opa_t)(255 - 190 * up), 0);
}

/* ---- the page around it ---------------------------------------------- */
static void coin_set_tally(void)
{
    char b[80];
    uint32_t tails = coin_flips - coin_head_count;
    char f[40], h[40], t[40];   /* each count with its word, in the language */
    if (!coin_flips) snprintf(b, sizeof b, "%s", sl_tr("No flips yet"));
    else snprintf(b, sizeof b, "%s  \xc2\xb7  %s  \xc2\xb7  %s",
                  sl_count(f, sizeof f, (int)coin_flips, "%s flip", "%s flips"),
                  sl_count(h, sizeof h, (int)coin_head_count, "%s head", "%s heads"),
                  sl_count(t, sizeof t, (int)tails, "%s tail", "%s tails"));
    lv_label_set_text(coin_tally, b);
}
static void coin_set_title(const char *s, const lv_font_t *font, uint32_t color)
{
    s = sl_tr(s);
    /* the big bold face is ASCII only: other words take the largest face that has them */
    for (const unsigned char *c = (const unsigned char *)s; font == &ss_bold_56 && *c; c++)
        if (*c >= 0x80) font = &ss_semibold_38;
    lv_label_set_text(coin_caption, s);
    lv_obj_set_style_text_font(coin_caption, font, 0);
    lv_obj_set_style_text_color(coin_caption, lv_color_hex(color), 0);
    lv_obj_center(coin_caption);
}
static void coin_history_dot(bool heads)
{
    if (lv_obj_get_child_count(coin_history) >= COIN_HISTORY)
        lv_obj_delete(lv_obj_get_child(coin_history, 0));
    lv_obj_t *dot = box(coin_history);
    lv_obj_set_size(dot, 26, 26);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(heads ? COIN_PINK : COIN_AMBER), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    uint32_t fill = heads ? COIN_PINK : COIN_AMBER;
    lv_obj_t *l = text(dot, heads ? "H" : "T", &ss_semibold_18, 0xfffffe);
    lv_obj_set_style_text_color(l, lv_color_hex(sl_ink_on(fill)), 0);   /* the stronger ink: white on amber was 2.8:1 */
    lv_obj_center(l);
}
static void coin_add_history(bool heads)
{
    size_t n = strlen(coin_hist);
    if (n >= COIN_HISTORY) memmove(coin_hist, coin_hist + 1, n--);
    coin_hist[n] = heads ? 'H' : 'T';
    coin_hist[n + 1] = 0;
    coin_history_dot(heads);
}
static void coin_hint_visible(bool on)
{
    lv_obj_set_style_opa(coin_hint, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void coin_burst(void)
{
    static const uint32_t colors[] = { 0xf2545b, 0xf7b538, 0x43aa8b, 0x4d9de0,
                                       0x9b5de5, 0xf15bb5, 0xffd23f, 0x2ec4b6 };
    coin_burst_at = coin_now();
    for (int i = 0; i < COIN_BITS; i++)
    {
        uint32_t r = coin_rand();
        float a = -3.14159f * (0.08f + 0.84f * (i + (r & 255) / 256.0f) / COIN_BITS);
        float v = 330 + (r >> 8) % 300;
        coin_bit_vx[i] = (int16_t)(cosf(a) * v);
        coin_bit_vy[i] = (int16_t)(sinf(a) * v - 120);
        coin_bit_spin[i] = (int16_t)(8 + (r >> 4) % 14);
        coin_bit_w[i] = (uint8_t)(i & 1 ? 8 : 11);
        coin_bit_h[i] = (uint8_t)(i & 1 ? 16 : 11);
        lv_obj_set_style_bg_color(coin_bit[i], lv_color_hex(colors[(i + (r >> 12)) % 8]), 0);
        lv_obj_set_style_radius(coin_bit[i], i & 1 ? 3 : LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(coin_bit[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static void coin_burst_step(uint32_t now)
{
    if (!coin_burst_at) return;
    uint32_t t = now - coin_burst_at;
    if (t > 1100)
    {
        for (int i = 0; i < COIN_BITS; i++) lv_obj_add_flag(coin_bit[i], LV_OBJ_FLAG_HIDDEN);
        coin_burst_at = 0;
        return;
    }
    float ts = t / 1000.0f;
    int cy = COIN_REST_Y - COIN_R / 3;
    for (int i = 0; i < COIN_BITS; i++)
    {
        /* air drag on the way out, gravity on the way down */
        float drag = (1 - exp(-ts * 3.2f)) / 3.2f;
        float x = W / 2 + coin_bit_vx[i] * drag;
        float y = cy + coin_bit_vy[i] * drag + 620 * ts * ts;
        float tumble = fabsf(cosf(ts * coin_bit_spin[i]));
        int w = coin_bit_w[i], h = (int)(coin_bit_h[i] * (0.25f + 0.75f * tumble)) + 1;
        lv_obj_set_size(coin_bit[i], w, h);
        lv_obj_set_pos(coin_bit[i], (int)x - w / 2, (int)y - h / 2);
        lv_obj_set_style_bg_opa(coin_bit[i],
            (lv_opa_t)(t < 700 ? 255 : 255 * (1100 - t) / 400), 0);
    }
}

static void coin_reveal(void)
{
    coin_revealed = true;
    coin_flips++;
    if (coin_heads) coin_head_count++;
    sfx_play(coin_heads ? SFX_COIN_HEADS : SFX_COIN_TAILS);
    coin_set_title(coin_heads ? "Heads!" : "Tails!", &ss_bold_56,
                   coin_heads ? COIN_PINK : COIN_AMBER);
    lv_obj_set_style_opa(coin_title_box, LV_OPA_COVER, 0);
    coin_pop_at = coin_now();
    coin_add_history(coin_heads);
    coin_set_tally();
    coin_save();
    coin_burst();
}

static void coin_arm(uint32_t period)
{
    lv_timer_set_period(coin_timer, period);
    lv_timer_resume(coin_timer);
}

static void coin_launch(float power)
{
    if (coin_state == COIN_FLIGHT) return;
    power = coin_clamp01(power);
    coin_power = power;
    coin_heads = (coin_rand() >> 7 & 1) != 0;
    /* a whole number of half turns; the parity decides the side */
    int half = 7 + (int)lroundf(power * 5);
    if ((half & 1) != (coin_heads != coin_front_heads)) half++;
    coin_theta0 = coin_theta;
    coin_lift0 = coin_lift;
    coin_turn = half * 3.14159265f;
    float max_rise = COIN_REST_Y - COIN_TOP_Y;
    coin_rise = max_rise * (0.74f + 0.26f * power) - coin_lift0 * 0.5f;
    coin_air_ms = (uint32_t)(1220 + 380 * power);
    coin_started = coin_now();
    coin_state = COIN_FLIGHT;
    coin_in_flight = true;
    coin_landed = coin_revealed = false;
    coin_sheen = -9;
    coin_glint_at = 0;
    sfx_play(SFX_COIN_FLIP);
    lv_obj_set_style_opa(coin_title_box, LV_OPA_40, 0);
    lv_label_set_text(coin_button_text, sl_tr("Flipping"));
    lv_obj_add_state(coin_flip_btn, LV_STATE_DISABLED);
    coin_hint_visible(false);
    coin_arm(16);
}

static void coin_settle(void)
{
    coin_state = COIN_IDLE;
    coin_in_flight = false;
    coin_theta = 0;
    coin_lift = 0;
    coin_front_heads = coin_heads;
    lv_label_set_text(coin_button_text, sl_tr("Flip again"));
    lv_obj_remove_state(coin_flip_btn, LV_STATE_DISABLED);
    coin_hint_visible(true);
    /* a glint across the fresh face, then one every few seconds */
    coin_glint_at = coin_now() + 120;
    coin_next_glint = 0;
}

static void coin_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!coin_page_obj) return;
    uint32_t now = coin_now();
    bool busy = false;
    if (coin_state == COIN_FLIGHT)
    {
        busy = true;
        uint32_t t = now - coin_started;
        if (t < coin_air_ms)
        {
            float u = t / (float)coin_air_ms;
            coin_lift = coin_lift0 * (1 - u) + coin_rise * 4 * u * (1 - u);
            float prof = 0.55f * u + 0.45f * (1 - (1 - u) * (1 - u));
            coin_theta = coin_theta0 + (coin_turn - coin_theta0) * prof;
        }
        else
        {
            uint32_t tau = t - coin_air_ms;
            if (!coin_landed) { coin_landed = true; sfx_play(SFX_COIN_LAND); }
            if (!coin_revealed && tau >= COIN_REVEAL_MS) coin_reveal();
            if (tau >= COIN_SETTLE_MS)
                coin_settle();
            else
            {
                /* two hops, and a rock that keeps the landing spin's momentum */
                float hop1 = 20 + 16 * coin_power;
                if (tau < 290) { float v = tau / 290.0f; coin_lift = hop1 * 4 * v * (1 - v); }
                else if (tau < 450) { float v = (tau - 290) / 160.0f; coin_lift = 7 * 4 * v * (1 - v); }
                else coin_lift = 0;
                float omega = (coin_turn - coin_theta0) * 0.55f / coin_air_ms;
                float wd = 6.2831853f / 300;
                float amp = omega / wd;
                if (amp > 0.5f) amp = 0.5f;
                coin_theta = coin_turn + amp * sinf(wd * tau) * exp(-(float)tau / 190);
            }
        }
    }
    else if (coin_state == COIN_SPRING)
    {
        busy = true;
        uint32_t t = now - coin_spring_at;
        if (t >= 420)
        {
            coin_state = COIN_IDLE;
            coin_lift = coin_theta = 0;
        }
        else
        {
            float k = exp(-(float)t / 95) * cosf(t * 0.013f);
            coin_lift = coin_spring_lift * k;
            coin_theta = coin_spring_theta * k;
        }
    }
    else if (coin_state == COIN_DRAG)
        busy = true;

    if (coin_state == COIN_IDLE)
    {
        if (!coin_glint_at && coin_next_glint && (int32_t)(now - coin_next_glint) >= 0)
            coin_glint_at = now;
        if (coin_glint_at && (int32_t)(now - coin_glint_at) >= 0)
        {
            uint32_t g = now - coin_glint_at;
            busy = true;
            if (g >= 700)
            {
                coin_sheen = -9;
                coin_glint_at = 0;
                coin_next_glint = now + 4200;
            }
            else
            {
                float u = g / 700.0f;
                coin_sheen = -1.7f + 3.4f * (u * u * (3 - 2 * u));
            }
        }
        else if (coin_glint_at) busy = true;
        else if (!coin_next_glint) coin_next_glint = now + 4200;
    }

    if (coin_burst_at) { busy = true; coin_burst_step(now); }
    if (coin_pop_at)
    {
        busy = true;
        uint32_t t = now - coin_pop_at;
        if (t >= 420)
        {
            lv_obj_set_style_transform_scale(coin_title_box, 256, 0);
            coin_pop_at = 0;
        }
        else
        {
            /* ease out back: overshoot, then settle */
            float u = t / 420.0f - 1, s = 1 + 2.4f * u * u * u + 1.4f * u * u;
            lv_obj_set_style_transform_scale(coin_title_box, (int32_t)(256 * (0.55f + 0.45f * s)), 0);
        }
    }
    if (busy) coin_render();
    coin_arm(busy ? 16 : 250);
}

/* ---- touch: drag the coin up and let go to throw it ------------------ */
static void coin_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (indev) lv_indev_get_point(indev, &p);
    uint32_t now = coin_now();
    if (code == LV_EVENT_PRESSED)
    {
        if (coin_state == COIN_FLIGHT) return;
        coin_state = COIN_DRAG;
        coin_drag_y0 = coin_drag_last_y = p.y;
        coin_drag_last_ms = now;
        coin_drag_vy = 0;
        coin_sheen = -9;
        coin_glint_at = 0;
        coin_arm(16);
    }
    else if (code == LV_EVENT_PRESSING && coin_state == COIN_DRAG)
    {
        uint32_t dt = now - coin_drag_last_ms;
        if (dt > 0)
        {
            float v = (coin_drag_last_y - p.y) * 1000.0f / dt;
            coin_drag_vy = coin_drag_vy * 0.35f + v * 0.65f;
            coin_drag_last_y = p.y;
            coin_drag_last_ms = now;
        }
        /* the coin follows the finger on a rubber band and winds back,
         * top towards you, ready to spin the other way */
        float d = (float)(coin_drag_y0 - p.y);
        coin_lift = d > 0 ? 84 * (1 - exp(-d / 120)) : 0;
        coin_theta = -0.42f * coin_lift / 84;
        coin_render();
    }
    else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && coin_state == COIN_DRAG)
    {
        int d = coin_drag_y0 - coin_drag_last_y;
        if (now - coin_drag_last_ms > 90) coin_drag_vy = 0;   /* held still, then let go */
        bool tap = abs(coin_drag_y0 - p.y) < 12 && abs(d) < 12 &&
                   abs(p.y - COIN_REST_Y) < COIN_R + 30;
        if (code == LV_EVENT_RELEASED && tap)
            coin_launch(0.5f);
        else if (code == LV_EVENT_RELEASED && (coin_drag_vy > 260 || d > 120))
            coin_launch((coin_drag_vy - 260) / 1600 + (d > 120 ? 0.25f : 0));
        else
        {
            coin_state = COIN_SPRING;
            coin_spring_at = now;
            coin_spring_lift = coin_lift;
            coin_spring_theta = coin_theta;
            coin_arm(16);
        }
    }
}
static void coin_flip_cb(lv_event_t *e)
{
    (void)e;
    if (coin_state == COIN_FLIGHT) return;
    coin_launch(0.7f);
}
static void coin_reset_cb(lv_event_t *e)
{
    (void)e;
    if (coin_state == COIN_FLIGHT) return;
    coin_flips = coin_head_count = 0;
    coin_hist[0] = 0;
    lv_obj_clean(coin_history);
    coin_set_tally();
    coin_save();
    coin_set_title("Heads or tails?", &ss_bold_28, COIN_INK);
    lv_label_set_text(coin_button_text, sl_tr("Flip"));
    sfx_play(SFX_TAP);
}
static void coin_deleted(lv_event_t *e)
{
    (void)e;
    if (coin_timer) { lv_timer_delete(coin_timer); coin_timer = NULL; }
    coin_page_obj = coin_canvas = coin_shadow = coin_stage = NULL;
    coin_title_box = coin_caption = coin_tally = coin_history = NULL;
    coin_hint = coin_hint_arrow = coin_flip_btn = coin_button_text = coin_reset_btn = NULL;
    for (int i = 0; i < COIN_BITS; i++) coin_bit[i] = NULL;
    coin_state = COIN_IDLE;
    coin_in_flight = false;
    coin_burst_at = coin_pop_at = coin_glint_at = 0;
    /* both are drawn again on the next open; the faces stay, warmed at start-up */
    sl_give_back(coin_px, sizeof coin_px);
    sl_give_back(coin_shadow_px, sizeof coin_shadow_px);
    statusbar_retheme();
}
static void coin_arrow_bob(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }

static bool coin_on_top(void) { return coin_page_obj && npages && pages[npages - 1] == coin_page_obj; }
static void coin_page(void)
{
    if (page_bring_back(coin_page_obj)) return;
    if (!coin_art_ready) coin_paint(0);   /* opened before the warm-up finished */
    lv_obj_t *p, *body = page_new("Flip a Coin", &p);
    coin_page_obj = p;
    coin_load();
    coin_state = COIN_IDLE;
    coin_in_flight = false;
    coin_front_heads = true;
    coin_theta = coin_lift = 0;
    coin_sheen = -9;
    coin_burst_at = coin_pop_at = 0;
    coin_seed ^= sl_ms();
    chance_page_skin(p, COIN_PAPER, COIN_INK);
    statusbar_ink(COIN_INK);
    lv_obj_add_event_cb(p, coin_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(body, LV_LAYOUT_NONE);        /* children are placed by hand */

    /* a grey pill, a big answer, the recent flips and a running count */
    lv_obj_t *chip = box(body);
    lv_obj_set_size(chip, W - 64, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(chip, 18, 0);
    lv_obj_set_style_pad_ver(chip, 6, 0);
    lv_obj_set_style_radius(chip, SL_R_LG, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(COIN_FILL), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t *chip_text = text(chip, "It's a slop summer, live your life!\nGet sober, buy drugs, rob a bank",
                               &ss_regular_16, COIN_SOFT);
    lv_obj_set_width(chip_text, lv_pct(100));
    lv_obj_set_style_text_align(chip_text, LV_TEXT_ALIGN_CENTER, 0);
    coin_title_box = box(body);
    lv_obj_set_size(coin_title_box, W - 28, 84);
    lv_obj_align(coin_title_box, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_transform_pivot_x(coin_title_box, (W - 28) / 2, 0);
    lv_obj_set_style_transform_pivot_y(coin_title_box, 42, 0);
    coin_caption = text(coin_title_box, "", &ss_bold_28, COIN_INK);
    coin_set_title("Heads or tails?", &ss_bold_28, COIN_INK);
    coin_history = box(body);
    lv_obj_set_size(coin_history, W - 28, 26);
    lv_obj_align(coin_history, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_flex_flow(coin_history, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(coin_history, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(coin_history, 8, 0);
    for (const char *h = coin_hist; *h; h++) coin_history_dot(*h == 'H');
    coin_tally = text(body, "", &ss_regular_16, COIN_GREY);
    lv_obj_set_width(coin_tally, W - 28);
    lv_obj_set_style_text_align(coin_tally, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(coin_tally, LV_ALIGN_TOP_MID, 0, 170);
    coin_set_tally();

    /* the table: everything from the header to the buttons takes the swipe */
    coin_stage = box(p);
    lv_obj_set_pos(coin_stage, 0, SB_H + 66);
    lv_obj_set_size(coin_stage, W, H - SB_H - 66 - 104);
    lv_obj_add_flag(coin_stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(coin_stage, coin_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(coin_stage, coin_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(coin_stage, coin_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(coin_stage, coin_touch_cb, LV_EVENT_PRESS_LOST, NULL);

    /* a soft elliptical blob on the table, painted once and only scaled */
    for (int j = 0; j < COIN_SHH; j++)
        for (int i = 0; i < COIN_SHW; i++)
        {
            float dx = (i + 0.5f - COIN_SHW / 2.0f) / (COIN_SHW / 2.0f);
            float dy = (j + 0.5f - COIN_SHH / 2.0f) / (COIN_SHH / 2.0f);
            float k = coin_clamp01(1 - (dx * dx + dy * dy));
            /* warm grey on white; deeper and black on a dark table */
            coin_shadow_px[j * COIN_SHW + i] = COIN_LIGHT ?
                (lv_color32_t){ .blue = 0x20, .green = 0x23, .red = 0x2a, .alpha = (uint8_t)(k * k * 70) } :
                (lv_color32_t){ .alpha = (uint8_t)(k * k * 170) };
        }
    coin_shadow = lv_canvas_create(p);
    lv_canvas_set_buffer(coin_shadow, coin_shadow_px, COIN_SHW, COIN_SHH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(coin_shadow, (W - COIN_SHW) / 2, COIN_REST_Y + COIN_R + 16 - COIN_SHH / 2);
    lv_obj_remove_flag(coin_shadow, LV_OBJ_FLAG_CLICKABLE);

    coin_hint = box(p);
    lv_obj_set_size(coin_hint, W, LV_MAX(56, 22 + sl_line_h(&ss_regular_18)));   /* Arabic's deeper letters inside it at every size */
    lv_obj_set_pos(coin_hint, 0, COIN_REST_Y + COIN_R + 40);
    coin_hint_arrow = text(coin_hint, LV_SYMBOL_UP, &sl_sym_16, COIN_GREY);   /* as its words: it was 2:1 on light */
    lv_obj_align(coin_hint_arrow, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *hint_text = text(coin_hint, "Swipe up to flip", &ss_regular_18, COIN_GREY);
    lv_obj_align(hint_text, LV_ALIGN_TOP_MID, 0, 22);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, coin_hint_arrow);
    lv_anim_set_exec_cb(&a, coin_arrow_bob);
    lv_anim_set_values(&a, 0, -7);
    lv_anim_set_duration(&a, 650);
    lv_anim_set_playback_duration(&a, 650);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    /* Tiny Decisions' bottom bar: a round reset and a blue Flip pill */
    coin_reset_btn = box(p);
    lv_obj_set_size(coin_reset_btn, 58, 58);
    lv_obj_set_pos(coin_reset_btn, 24, H - 58 - 30);
    lv_obj_set_style_radius(coin_reset_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(coin_reset_btn, lv_color_hex(COIN_FILL), 0);
    lv_obj_set_style_bg_color(coin_reset_btn, lv_color_hex(COIN_FILL_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(coin_reset_btn, LV_OPA_COVER, 0);
    lv_obj_t *ic = text(coin_reset_btn, LV_SYMBOL_REFRESH, &sl_sym_22, COIN_SOFT);
    lv_obj_center(ic);
    on_click(coin_reset_btn, coin_reset_cb, NULL);
    coin_flip_btn = box(p);
    lv_obj_set_size(coin_flip_btn, 178, 58);
    lv_obj_set_pos(coin_flip_btn, (W - 178) / 2, H - 58 - 30);
    lv_obj_set_style_radius(coin_flip_btn, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(coin_flip_btn, lv_color_hex(COIN_BLUE), 0);
    lv_obj_set_style_bg_color(coin_flip_btn, lv_color_hex(0x2a63d4), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(coin_flip_btn, lv_color_hex(COIN_LIGHT ? 0x9dbcf8 : 0x2b4a80), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(coin_flip_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(coin_flip_btn, lv_color_hex(COIN_BLUE), 0);
    lv_obj_set_style_shadow_width(coin_flip_btn, 18, 0);
    lv_obj_set_style_shadow_offset_y(coin_flip_btn, 6, 0);
    lv_obj_set_style_shadow_opa(coin_flip_btn, LV_OPA_30, 0);
    lv_obj_set_style_transform_scale(coin_flip_btn, 244, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(coin_flip_btn, 89, 0);
    lv_obj_set_style_transform_pivot_y(coin_flip_btn, 29, 0);
    coin_button_text = text(coin_flip_btn, "Flip", &ss_semibold_23, 0xfffffe);
    lv_obj_center(coin_button_text);
    on_click(coin_flip_btn, coin_flip_cb, NULL);

    for (int i = 0; i < COIN_BITS; i++)
    {
        coin_bit[i] = box(p);
        lv_obj_set_style_bg_opa(coin_bit[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(coin_bit[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* last, so it flies over the header and everything else */
    coin_canvas = lv_canvas_create(p);
    lv_canvas_set_buffer(coin_canvas, coin_px, COIN_CW, COIN_CH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_remove_flag(coin_canvas, LV_OBJ_FLAG_CLICKABLE);
    coin_render();

    coin_timer = lv_timer_create(coin_tick, 250, NULL);
    coin_next_glint = coin_now() + 1500;
    page_push(p);
}

static const char *const magic_answers[] = {
    "It is certain", "Without a doubt", "You may rely on it",
    "Yes, definitely", "Outlook good", "Most likely", "Signs point to yes",
    "Ask again later", "Better not tell you now", "Cannot predict now",
    "Concentrate and ask again", "Reply hazy, try again",
    "Do not count on it", "My reply is no", "Outlook not so good",
    "Very doubtful", "As I see it, yes", "Yes", "My sources say no",
    "It is decidedly so", "You're hot"
};
static lv_obj_t *magic_page_obj, *magic_ball, *magic_answer, *magic_caption;
static lv_obj_t *magic_star[8], *magic_window, *magic_verdict, *magic_button_text;
static lv_obj_t *magic_sky;
static lv_draw_buf_t *magic_ball_buf;
static lv_obj_t *magic_shot;           /* the ball as one picture while it shakes */
static lv_draw_buf_t *magic_shot565;
static lv_timer_t *magic_timer;
static uint32_t magic_started, magic_seed;
static int magic_choice = -1;
static bool magic_thinking, magic_revealed;

#define MAGIC_REVEAL_MS 900
#define MAGIC_DONE_MS 1520

static const char *magic_tone(int choice)
{
    if (choice == 20) return "WILD CARD";
    if (choice <= 6 || choice == 16 || choice == 17 || choice == 19) return "LOOKS LIKE YES";
    if (choice >= 12 && choice <= 15) return "LOOKS LIKE NO";
    if (choice == 18) return "LOOKS LIKE NO";
    return "STILL A MYSTERY";
}

static uint32_t magic_tone_color(int choice)
{
    if (choice == 20) return 0xffcb88;
    if (choice <= 6 || choice == 16 || choice == 17 || choice == 19) return 0x85e5ce;
    if ((choice >= 12 && choice <= 15) || choice == 18) return 0xffa6bb;
    return 0xcbb8ff;
}

/* The sky behind the ball: the scene's own background, the purple glow and
 * the three pulsing halos, painted by hand into one opaque RGB565 canvas,
 * the way Freaky Spoon draws its scene. As LVGL objects the halos alone were
 * ~30 ms of a ~70 ms shake frame on the R1 (a round border is masked and
 * blended row by row across its whole box, and the halos change size every
 * frame), and the glow was blended over all 420x420 of its picture. Here a
 * frame restores only the ring the glow and halos can reach from a still
 * copy of the background and paints them back in, with LVGL's own blend
 * formulas; LVGL then just copies the canvas. */
#define MAGIC_GLOW 420
#define MAGIC_SKY_W 412
#define MAGIC_SKY_H 416
#define MAGIC_SKY_IN 136                 /* always under the ball, even mid-sway */
#define MAGIC_SKY_OUT 208                /* past the widest halo and the swaying glow */
static uint16_t *magic_sky_px, *magic_sky_bg;
static uint8_t *magic_glow_a;            /* the glow's alpha, MAGIC_GLOW square */
static int magic_halo_d[3];              /* each halo's diameter */
static lv_opa_t magic_halo_opa[3];
static int magic_glow_x, magic_glow_y;   /* the glow's sway */
static lv_opa_t magic_glow_opa = LV_OPA_COVER;
static const uint32_t magic_halo_col[3] = { 0x8d42ec, 0xb76aff, 0x6034c0 };

static float magic_glow_edge(float d, float r, float blur)
{
    float t = (r + blur / 2 - d) / blur;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}
static uint8_t *magic_glow_make(void)
{
    uint8_t *a = malloc(MAGIC_GLOW * MAGIC_GLOW);
    if (!a) return NULL;
    const float c = (MAGIC_GLOW - 1) / 2.0f;
    for (int y = 0; y < MAGIC_GLOW; y++)
        for (int x = 0; x < MAGIC_GLOW; x++)
        {
            float d = sqrtf((x - c) * (x - c) + (y - c) * (y - c));
            /* each glow only outside its own shape, as the two LVGL shadows were */
            float ring = d < 183 ? 0 : 0.6f * magic_glow_edge(d, 185, 20);  /* the outer ring */
            float ball = d < 152 ? 0 : 0.5f * magic_glow_edge(d, 152, 38);  /* the ball */
            a[y * MAGIC_GLOW + x] = (uint8_t)((ring + ball * (1 - ring)) * 255 + 0.5f);
        }
    return a;
}

static void magic_halo_scale(int i, int scale) { magic_halo_d[i] = (370 - i * 26) * scale / 256; }

/* x0..x1 of row y where r_in <= distance from (cx, cy) < r_out, as two spans */
static int magic_sky_spans(float cx, float cy, int y, float r_in, float r_out, int sp[4])
{
    float dy = y + 0.5f - cy, o2 = r_out * r_out - dy * dy;
    if (o2 <= 0) return 0;
    float xo = sqrtf(o2), i2 = r_in * r_in - dy * dy, xi = i2 > 0 ? sqrtf(i2) : 0;
    int l0 = (int)floorf(cx - xo), r1 = (int)ceilf(cx + xo);
    if (l0 < 0) l0 = 0;
    if (r1 > MAGIC_SKY_W) r1 = MAGIC_SKY_W;
    if (xi < 1) { sp[0] = l0; sp[1] = r1; return 1; }
    int l1 = (int)ceilf(cx - xi) + 1, r0 = (int)floorf(cx + xi) - 1;   /* keep a pixel either side */
    sp[0] = l0; sp[1] = l1 < r1 ? l1 : r1;
    sp[2] = r0 > l0 ? r0 : l0; sp[3] = r1;
    return 2;
}

static void magic_sky_paint(void)
{
    if (!magic_sky || !magic_sky_bg) return;
    lv_obj_t *scene = lv_obj_get_parent(magic_sky);
    lv_area_t cont, sky;
    lv_obj_get_content_coords(scene, &cont);
    lv_obj_get_coords(magic_sky, &sky);
    int cw = lv_area_get_width(&cont), ch = lv_area_get_height(&cont);
    /* where LVGL centres the halos and the glow (content / 2 - size / 2),
     * in canvas pixels */
    int ox = cont.x1 - sky.x1 + cw / 2, oy = cont.y1 - sky.y1 - 30 + ch / 2;
    float cx = ox, cy = oy;                 /* the halos' shared centre */
    int sp[8];

    /* 1. the background back, around the ball */
    for (int y = 0; y < MAGIC_SKY_H; y++)
    {
        int n = magic_sky_spans(cx, cy, y, MAGIC_SKY_IN, MAGIC_SKY_OUT, sp);
        for (int k = 0; k < n; k++)
            if (sp[2 * k + 1] > sp[2 * k])
                memcpy(&magic_sky_px[y * MAGIC_SKY_W + sp[2 * k]], &magic_sky_bg[y * MAGIC_SKY_W + sp[2 * k]],
                       (size_t)(sp[2 * k + 1] - sp[2 * k]) * 2);
    }

    /* 2. the glow, as LVGL blends an ARGB8888 image over RGB565 */
    if (magic_glow_a && magic_glow_opa > LV_OPA_MIN)
    {
        int gx = ox - MAGIC_GLOW / 2 + magic_glow_x, gy = oy - MAGIC_GLOW / 2 + magic_glow_y;
        float gc = MAGIC_GLOW / 2.0f;
        for (int y = gy < 0 ? 0 : gy; y < MAGIC_SKY_H && y < gy + MAGIC_GLOW; y++)
        {
            /* where the glow is not clear: the ball's (152..171) and the ring's (183..195) */
            int n = magic_sky_spans(gx + gc, gy + gc, y, 151, 172, sp);
            n += magic_sky_spans(gx + gc, gy + gc, y, 182, 196, sp + 2 * n);
            const uint8_t *arow = &magic_glow_a[(y - gy) * MAGIC_GLOW - gx];
            uint16_t *row = &magic_sky_px[y * MAGIC_SKY_W];
            for (int k = 0; k < n; k++)
                for (int x = LV_MAX(sp[2 * k], gx); x < sp[2 * k + 1] && x < gx + MAGIC_GLOW; x++)
                {
                    lv_opa_t mix = magic_glow_opa >= LV_OPA_MAX ? arow[x] : LV_OPA_MIX2(arow[x], magic_glow_opa);
                    if (!mix) continue;
                    uint16_t c2 = row[x];
                    if (mix == 255) { row[x] = (0x98 << 8) | (0x38 << 3) | (0xff >> 3); continue; }
                    lv_opa_t inv = 255 - mix;
                    row[x] = (uint16_t)(((((0x9a >> 3) * mix + ((c2 >> 11) & 0x1f) * inv) << 3) & 0xf800) +
                                        ((((0x3a >> 2) * mix + ((c2 >> 5) & 0x3f) * inv) >> 3) & 0x07e0) +
                                        (((0xff >> 3) * mix + (c2 & 0x1f) * inv) >> 8));
                }
        }
    }

    /* 3. the halos: rings anti-aliased on both edges, blended as LVGL
     * blends a border (mask times opacity) */
    for (int i = 0; i < 3; i++)
    {
        int d = magic_halo_d[i];
        float r = d / 2.0f, ri = r - (3 + i);
        float hx = ox - d / 2 + r, hy = oy - d / 2 + r;
        uint16_t col = lv_color_to_u16(lv_color_hex(magic_halo_col[i]));
        lv_opa_t opa = magic_halo_opa[i];
        if (opa <= LV_OPA_MIN) continue;
        /* squared distances: past the outer edge, wholly inside the ring, before the inner edge */
        float full_out = (r + 0.5f) * (r + 0.5f), full_out_in = (r - 0.5f) * (r - 0.5f);
        float full_in = (ri + 0.5f) * (ri + 0.5f), none_in = ri > 0.5f ? (ri - 0.5f) * (ri - 0.5f) : 0;
        for (int y = (int)floorf(hy - r) - 1; y <= (int)ceilf(hy + r); y++)
        {
            if (y < 0 || y >= MAGIC_SKY_H) continue;
            int n = magic_sky_spans(hx, hy, y, ri - 1, r + 1, sp);
            float dy = y + 0.5f - hy;
            uint16_t *row = &magic_sky_px[y * MAGIC_SKY_W];
            for (int k = 0; k < n; k++)
                for (int x = sp[2 * k]; x < sp[2 * k + 1]; x++)
                {
                    float dx = x + 0.5f - hx, d2 = dx * dx + dy * dy;
                    if (d2 >= full_out || d2 <= none_in) continue;      /* clear of the ring */
                    lv_opa_t m = 255;
                    if (d2 > full_out_in || d2 < full_in)               /* an edge: how much is covered */
                    {
                        float dist = sqrtf(d2), out = r - dist + 0.5f, in = dist - ri + 0.5f;
                        float cov = (out < 0 ? 0 : out > 1 ? 1 : out) * (in < 0 ? 0 : in > 1 ? 1 : in);
                        m = (lv_opa_t)(cov * 255 + 0.5f);
                    }
                    if (m <= LV_OPA_MIN) continue;
                    lv_opa_t mix = opa >= LV_OPA_MAX ? m : LV_OPA_MIX2(m, opa);
                    row[x] = lv_color_16_16_mix(col, row[x], mix);
                }
        }
    }
    lv_obj_invalidate(magic_sky);
}

/* The ball's body (its gradient, rim and shine) as one picture, made by
 * LVGL itself: drawn live it was two full-size circles of row-by-row masks,
 * ~25 ms of every frame the halos and glow redraw around it. The window and
 * the answer stay live objects on top: their colour and text change. */
static void magic_ball_flatten(lv_obj_t *shine)
{
    lv_obj_add_flag(magic_window, LV_OBJ_FLAG_HIDDEN);
    magic_ball_buf = lv_snapshot_take(magic_ball, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_remove_flag(magic_window, LV_OBJ_FLAG_HIDDEN);
    if (!magic_ball_buf) return;           /* no memory: LVGL keeps drawing it */
    lv_obj_t *body = lv_image_create(magic_ball);
    lv_image_set_src(body, magic_ball_buf);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(body, 0);
    lv_obj_center(body);
    lv_obj_delete(shine);
    lv_obj_set_style_bg_opa(magic_ball, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(magic_ball, LV_OPA_TRANSP, 0);   /* its width still insets the window */
}

/* The still background, once: the scene drawn on its own (its children
 * hidden for a moment) over the page colour that shows at its corners. */
static bool magic_sky_capture(void)
{
    lv_obj_t *scene = lv_obj_get_parent(magic_sky);
    lv_obj_update_layout(magic_page_obj);
    uint32_t n = lv_obj_get_child_count(scene);
    bool shown[24];
    for (uint32_t i = 0; i < n && i < 24; i++)
    {
        lv_obj_t *c = lv_obj_get_child(scene, i);
        shown[i] = !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
    lv_draw_buf_t *snap = lv_snapshot_take(scene, LV_COLOR_FORMAT_ARGB8888);
    for (uint32_t i = 0; i < n && i < 24; i++)
        if (shown[i]) lv_obj_remove_flag(lv_obj_get_child(scene, i), LV_OBJ_FLAG_HIDDEN);
    if (!snap) return false;
    lv_area_t sc, sky;
    lv_obj_get_coords(scene, &sc);
    lv_obj_get_coords(magic_sky, &sky);
    lv_color_t page = lv_obj_get_style_bg_color(magic_page_obj, 0);
    for (int y = 0; y < MAGIC_SKY_H; y++)
        for (int x = 0; x < MAGIC_SKY_W; x++)
        {
            int sx = sky.x1 + x - sc.x1, sy = sky.y1 + y - sc.y1;
            lv_color_t c = page;
            if (sx >= 0 && sy >= 0 && sx < (int)snap->header.w && sy < (int)snap->header.h)
            {
                const lv_color32_t *p = (const lv_color32_t *)(snap->data + sy * snap->header.stride) + sx;
                c = lv_color_mix(lv_color_make(p->red, p->green, p->blue), page, p->alpha);
            }
            magic_sky_bg[y * MAGIC_SKY_W + x] = lv_color_to_u16(c);
        }
    lv_draw_buf_destroy(snap);
    memcpy(magic_sky_px, magic_sky_bg, (size_t)MAGIC_SKY_W * MAGIC_SKY_H * 2);
    return true;
}

static lv_draw_buf_t *magic_tilt_get(int sway);
static void magic_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!magic_page_obj) return;
    uint32_t elapsed = sl_ms() - magic_started;
    if (elapsed >= MAGIC_DONE_MS)
    {
        lv_obj_set_style_transform_scale(magic_ball, 256, 0);
        lv_obj_set_style_transform_rotation(magic_ball, 0, 0);
        lv_obj_set_style_translate_x(magic_ball, 0, 0);
        lv_obj_set_style_translate_y(magic_ball, 0, 0);
        if (magic_shot) lv_obj_add_flag(magic_shot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(magic_ball, LV_OBJ_FLAG_HIDDEN);
        magic_glow_x = magic_glow_y = 0;
        magic_glow_opa = LV_OPA_COVER;
        for (int i = 0; i < 3; ++i) {
            magic_halo_scale(i, 256);
            magic_halo_opa[i] = (lv_opa_t)(100 - i * 16);
        }
        magic_sky_paint();
        uint32_t tone = magic_tone_color(magic_choice);
        lv_obj_set_style_border_color(magic_window, lv_color_hex(tone), 0);
        lv_obj_set_style_text_font(magic_answer, &ss_semibold_23, 0);
        lv_obj_set_style_text_color(magic_answer, lv_color_hex(CHANCE_INK), 0);
        lv_label_set_text(magic_answer, sl_tr(magic_answers[magic_choice]));
        lv_obj_set_style_opa(magic_answer, LV_OPA_COVER, 0);
        lv_obj_set_style_transform_scale(magic_answer, 256, 0);
        lv_obj_center(magic_answer);
        lv_label_set_text(magic_verdict, sl_tr(magic_tone(magic_choice)));
        lv_obj_set_style_text_color(magic_verdict, lv_color_hex(tone), 0);
        lv_label_set_text(magic_caption, sl_tr(magic_choice == 20 ?
                          "So... what are you doing later tonight?" :
                          "The answer is in your hands."));
        lv_label_set_text(magic_button_text, sl_tr("ASK AGAIN"));
        magic_thinking = false;
        lv_timer_pause(magic_timer);
        return;
    }
    int thinking = elapsed < MAGIC_REVEAL_MS ? (int)elapsed : MAGIC_REVEAL_MS;
    int phase = thinking % 240;
    int triangle = phase < 120 ? phase : 240 - phase;
    int decay = MAGIC_REVEAL_MS - thinking;
    int sway = (triangle - 60) * decay / (MAGIC_REVEAL_MS * 8);
    int lift = -(12 * 4 * thinking * decay) / (MAGIC_REVEAL_MS * MAGIC_REVEAL_MS);
    if (magic_shot && !lv_obj_has_flag(magic_shot, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_set_style_translate_x(magic_shot, sway, 0);
        lv_obj_set_style_translate_y(magic_shot, lift, 0);
        lv_draw_buf_t *pic = magic_tilt_get(sway);
        lv_image_set_src(magic_shot, pic ? pic : magic_shot565);
        lv_image_set_rotation(magic_shot, pic ? 0 : sway * 8);
        lv_image_set_scale(magic_shot, pic ? 256 : 256 + abs(sway) / 2);
        if (elapsed >= MAGIC_REVEAL_MS)   /* still again (sway and lift are 0): the live ball back */
        {
            lv_obj_add_flag(magic_shot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(magic_ball, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else
    {
        lv_obj_set_style_translate_x(magic_ball, sway, 0);
        lv_obj_set_style_transform_rotation(magic_ball, sway * 8, 0);
        lv_obj_set_style_transform_scale(magic_ball, 256 + abs(sway) / 2, 0);
        lv_obj_set_style_translate_y(magic_ball, lift, 0);
    }
    magic_glow_x = sway;
    magic_glow_y = lift;
    {
        int wave = (int)(elapsed % 720), crest = wave < 360 ? wave : 720 - wave;
        magic_glow_opa = (lv_opa_t)(190 + crest * 65 / 360);
    }
    for (int i = 0; i < 3; ++i) {
        int wave = (int)((elapsed + i * 180) % 720);
        int crest = wave < 360 ? wave : 720 - wave;
        magic_halo_scale(i, 248 + crest * 34 / 360);
        magic_halo_opa[i] = (lv_opa_t)(42 + crest * 75 / 360);
    }
    magic_sky_paint();
    for (int i = 0; i < 8; ++i) {
        int wave = (int)((elapsed + i * 97) % 440);
        int crest = wave < 220 ? wave : 440 - wave;
        lv_obj_set_style_bg_opa(magic_star[i], (lv_opa_t)(55 + crest * 180 / 220), 0);
    }
    if (elapsed >= MAGIC_REVEAL_MS)
    {
        if (!magic_revealed) {
            magic_revealed = true;
            const char *tone = magic_tone(magic_choice);
            sfx_play(magic_choice == 20 ? SFX_MAGIC_HOT :
                     !strcmp(tone, "LOOKS LIKE YES") ? SFX_MAGIC_YES :
                     !strcmp(tone, "LOOKS LIKE NO") ? SFX_MAGIC_NO : SFX_MAGIC_MAYBE);
            lv_label_set_text(magic_answer, sl_tr(magic_answers[magic_choice]));
            lv_obj_set_style_text_font(magic_answer, &ss_semibold_23, 0);
            lv_obj_set_style_border_color(magic_window, lv_color_hex(magic_tone_color(magic_choice)), 0);
            lv_obj_center(magic_answer);
        }
        int reveal = (int)(elapsed - MAGIC_REVEAL_MS);
        int fade = reveal * 255 / 300;
        if (fade > 255) fade = 255;
        lv_obj_set_style_opa(magic_answer, (lv_opa_t)fade, 0);
        lv_obj_set_style_transform_scale(magic_answer, 228 + fade * 28 / 255, 0);
    }
}
/* While it shakes, the ball does not change inside ("?" in the thinking
 * colours, on every question), and the tilt only depends on the sway, a
 * whole number from -5 to 5. So each tilt is turned once by LVGL and kept
 * as a picture (RGB565 plus alpha, ~330 KB, 3.4 MB for all ten while the
 * page is open); a frame of the shake is then a copy, not a layer render and
 * a bilinear transform (~17 ms a frame on the R1). They are made in idle time
 * after the page opens, or on first use. A wider sway (the first few ms) is
 * still turned live. */
#define MAGIC_TILT 5
static lv_draw_buf_t *magic_tilt[2 * MAGIC_TILT + 1];
static bool magic_tilt_short;            /* the budget said no: no more asking until the page opens again */
static lv_timer_t *magic_warm_timer;

/* the still ball, as it looks while thinking */
static bool magic_upright(void)
{
    if (magic_shot565) return true;
    if (!magic_shot) return false;
    lv_obj_update_layout(magic_ball);
    lv_draw_buf_t *b = lv_snapshot_take(magic_ball, LV_COLOR_FORMAT_ARGB8888);
    if (!b) return false;
    magic_shot565 = sl_rgb565a8_from(b);
    lv_draw_buf_destroy(b);
    return magic_shot565 != NULL;
}

static lv_draw_buf_t *magic_tilt_get(int sway)
{
    if (!sway) return magic_shot565;
    if (sway < -MAGIC_TILT || sway > MAGIC_TILT || !magic_shot565) return NULL;
    lv_draw_buf_t **t = &magic_tilt[sway + MAGIC_TILT];
    if (*t) return *t;
    /* each kept turn is ~0.4 MB, all ten about 4 MB: only with room for the
     * music to spare (the snapshot borrows a third more while it is made);
     * without one the ball is turned as it is drawn */
    long kb = (long)magic_shot565->header.w * magic_shot565->header.h * 7 / 1024;
    if (magic_tilt_short || !sl_mem_room(kb, "the 8-ball's shake")) { magic_tilt_short = true; return NULL; }
    /* turn the still picture on the shake's own image and keep the result */
    bool hidden = lv_obj_has_flag(magic_shot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(magic_shot, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(magic_shot, magic_shot565);
    lv_image_set_rotation(magic_shot, sway * 8);
    lv_image_set_scale(magic_shot, 256 + abs(sway) / 2);
    lv_obj_update_layout(magic_shot);
    lv_draw_buf_t *b = lv_snapshot_take(magic_shot, LV_COLOR_FORMAT_ARGB8888);
    lv_image_set_rotation(magic_shot, 0);
    lv_image_set_scale(magic_shot, 256);
    if (hidden) lv_obj_add_flag(magic_shot, LV_OBJ_FLAG_HIDDEN);
    if (!b) return NULL;
    *t = sl_rgb565a8_from(b);
    lv_draw_buf_destroy(b);
    return *t;
}

/* one tilt per tick while the page sits still, the commonest first */
static void magic_warm(lv_timer_t *timer)
{
    lv_timer_set_period(timer, 60);
    if (magic_thinking || g_state != G_IDLE) return;
    if (!magic_shot565)
    {
        if (magic_choice >= 0) return;          /* made on the next question */
        lv_label_set_text(magic_answer, "?");   /* the page opens on "8", the rest already matches */
        lv_obj_center(magic_answer);
        bool ok = magic_upright();
        lv_label_set_text(magic_answer, "8");
        lv_obj_center(magic_answer);
        if (!ok) { lv_timer_delete(timer); magic_warm_timer = NULL; }
        return;
    }
    for (int k = 1; k <= 2 * MAGIC_TILT; k++)
    {
        int sway = k & 1 ? (k + 1) / 2 : -k / 2;
        if (magic_tilt[sway + MAGIC_TILT]) continue;
        if (!magic_tilt_get(sway)) break;       /* no memory: stop trying */
        return;
    }
    lv_timer_delete(timer);
    magic_warm_timer = NULL;
}

static void magic_shoot(void)
{
    if (!magic_upright()) return;              /* no memory: the ball itself shakes */
    lv_image_set_src(magic_shot, magic_shot565);
    lv_obj_add_flag(magic_ball, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(magic_shot, LV_OBJ_FLAG_HIDDEN);
}
static void magic_ask_cb(lv_event_t *e)
{
    (void)e;
    if (magic_thinking) return;
    magic_thinking = true;
    magic_revealed = false;
    int previous = magic_choice;
    magic_seed = magic_seed * 1664525u + 1013904223u + sl_ms();
    magic_choice = magic_seed % 128 == 0 ? 20 : (int)((magic_seed >> 8) % 20);
    if (magic_choice == previous && magic_choice != 20)
        magic_choice = (magic_choice + 1) % 20;
    magic_started = sl_ms();
    sfx_play(SFX_MAGIC_SHAKE);
    lv_label_set_text(magic_caption, sl_tr("Reading the signs..."));
    lv_label_set_text(magic_verdict, sl_tr("THE ORACLE IS THINKING"));
    lv_obj_set_style_text_color(magic_verdict, lv_color_hex(0xcbb8ff), 0);
    lv_obj_set_style_border_color(magic_window, lv_color_hex(0x9f91f8), 0);
    lv_obj_set_style_text_font(magic_answer, &ss_regular_60, 0);
    lv_obj_set_style_opa(magic_answer, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_scale(magic_answer, 256, 0);
    lv_label_set_text(magic_answer, "?");
    lv_obj_center(magic_answer);
    lv_label_set_text(magic_button_text, sl_tr("THINKING"));
    magic_shoot();
    lv_timer_reset(magic_timer);
    lv_timer_resume(magic_timer);
}
static void magic_deleted(lv_event_t *e)
{
    (void)e;
    if (magic_timer) { lv_timer_delete(magic_timer); magic_timer = NULL; }
    magic_page_obj = magic_ball = magic_answer = magic_caption = magic_window = magic_sky = NULL;
    free(magic_sky_px); free(magic_sky_bg); free(magic_glow_a);
    magic_tilt_short = false;
    magic_sky_px = magic_sky_bg = NULL;
    magic_glow_a = NULL;
    if (magic_ball_buf) { lv_draw_buf_destroy(magic_ball_buf); magic_ball_buf = NULL; }
    if (magic_shot565) { lv_draw_buf_destroy(magic_shot565); magic_shot565 = NULL; }
    for (int i = 0; i < 2 * MAGIC_TILT + 1; i++)
        if (magic_tilt[i]) { lv_draw_buf_destroy(magic_tilt[i]); magic_tilt[i] = NULL; }
    if (magic_warm_timer) { lv_timer_delete(magic_warm_timer); magic_warm_timer = NULL; }
    magic_shot = NULL;
    magic_verdict = magic_button_text = NULL;
    magic_thinking = magic_revealed = false;
    for (int i = 0; i < 8; ++i) magic_star[i] = NULL;
}
static void magic_page(void)
{
    if (page_bring_back(magic_page_obj)) return;
    lv_obj_t *p, *body = page_new("Magic 8 Ball", &p);
    magic_page_obj = p;
    magic_choice = -1;
    magic_thinking = magic_revealed = false;
    chance_page_skin(p, CHANCE_BG, CHANCE_INK);
    lv_obj_add_event_cb(p, magic_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_pad_row(body, 12, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);   /* it fits: no pointless wiggle */
    /* centred over the ball, as THE ORACLE AWAITS is under it */
    lv_obj_t *eyebrow = text(body, "SEEK AN ANSWER", &ss_semibold_18, 0xcbb8ff);
    lv_obj_set_style_text_letter_space(eyebrow, 3, 0);
    lv_obj_t *ask = text(body, "Think of a yes or no question.", &ss_regular_18, CHANCE_MUTED);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *l = i ? ask : eyebrow;
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *scene = box(body);
    lv_obj_set_size(scene, lv_pct(100), 406);
    lv_obj_set_style_radius(scene, 30, 0);
    lv_obj_set_style_bg_color(scene, lv_color_hex(0x1b1538), 0);
    lv_obj_set_style_bg_grad_color(scene, lv_color_hex(0x10172c), 0);
    lv_obj_set_style_bg_grad_dir(scene, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scene, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scene, 1, 0);
    lv_obj_set_style_border_color(scene, lv_color_hex(0x453969), 0);
    magic_sky_px = malloc((size_t)MAGIC_SKY_W * MAGIC_SKY_H * 2);
    magic_sky_bg = malloc((size_t)MAGIC_SKY_W * MAGIC_SKY_H * 2);
    magic_glow_a = magic_glow_make();
    if (magic_sky_px && magic_sky_bg)
    {
        magic_sky = lv_canvas_create(scene);
        lv_canvas_set_buffer(magic_sky, magic_sky_px, MAGIC_SKY_W, MAGIC_SKY_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(magic_sky, LV_ALIGN_CENTER, 0, -30);
        lv_obj_remove_flag(magic_sky, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < 3; ++i) {
        magic_halo_scale(i, 256);
        magic_halo_opa[i] = (lv_opa_t)(100 - i * 16);
    }
    magic_glow_x = magic_glow_y = 0;
    magic_glow_opa = LV_OPA_COVER;
    static const int sx[8] = {-179, -134, -51, 134, 179, 145, 45, -142};
    static const int sy[8] = {-107, -159, -176, -145, -20, 120, 136, 103};
    for (int k = 0; k < 8; ++k) {
        int i = (k & 3) * 2 + (k >> 2);   /* the small ones, then the big ones: LVGL keeps one shadow */
        magic_star[i] = box(scene);
        lv_obj_set_size(magic_star[i], i & 1 ? 8 : 5, i & 1 ? 8 : 5);
        lv_obj_align(magic_star[i], LV_ALIGN_CENTER, sx[i], sy[i] - 30);
        lv_obj_set_style_radius(magic_star[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(magic_star[i], lv_color_hex(0xe1b4ff), 0);
        lv_obj_set_style_bg_opa(magic_star[i], LV_OPA_50, 0);
        lv_obj_set_style_shadow_color(magic_star[i], lv_color_hex(0xce75ff), 0);
        lv_obj_set_style_shadow_width(magic_star[i], 12, 0);
    }
    magic_ball = box(scene);
    lv_obj_set_size(magic_ball, 304, 304);
    lv_obj_align(magic_ball, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_transform_pivot_x(magic_ball, 152, 0);
    lv_obj_set_style_transform_pivot_y(magic_ball, 152, 0);
    lv_obj_set_style_radius(magic_ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(magic_ball, lv_color_hex(0x6745a0), 0);
    lv_obj_set_style_bg_grad_color(magic_ball, lv_color_hex(0x0b1028), 0);
    lv_obj_set_style_bg_grad_dir(magic_ball, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(magic_ball, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(magic_ball, 5, 0);
    lv_obj_set_style_border_color(magic_ball, lv_color_hex(0xa68bd9), 0);
    magic_window = box(magic_ball);
    lv_obj_set_size(magic_window, 222, 170);
    lv_obj_center(magic_window);
    lv_obj_set_style_radius(magic_window, 42, 0);
    lv_obj_set_style_bg_color(magic_window, lv_color_hex(0x293887), 0);
    lv_obj_set_style_bg_grad_color(magic_window, lv_color_hex(0x111834), 0);
    lv_obj_set_style_bg_grad_dir(magic_window, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(magic_window, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(magic_window, 2, 0);
    lv_obj_set_style_border_color(magic_window, lv_color_hex(0x9f91f8), 0);
    lv_obj_t *shine = box(magic_ball);
    lv_obj_set_size(shine, 114, 16);
    lv_obj_align(shine, LV_ALIGN_TOP_MID, -44, 25);
    lv_obj_set_style_radius(shine, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shine, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(shine, 38, 0);
    magic_answer = text(magic_window, "8", &ss_regular_60, CHANCE_INK);
    lv_obj_set_width(magic_answer, 190);
    lv_label_set_long_mode(magic_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(magic_answer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(magic_answer);
    on_click(magic_ball, magic_ask_cb, NULL);
    lv_obj_add_flag(magic_ball, LV_OBJ_FLAG_USER_1);
    magic_caption = text(scene, "Tap the ball to reveal", &ss_semibold_21, CHANCE_INK);
    lv_obj_set_width(magic_caption, W - 60);
    lv_obj_set_style_text_align(magic_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(magic_caption, LV_LABEL_LONG_WRAP);
    lv_obj_align(magic_caption, LV_ALIGN_BOTTOM_MID, 0, -18);
    magic_verdict = text(body, "THE ORACLE AWAITS", &ss_semibold_18, 0xcbb8ff);
    lv_obj_set_width(magic_verdict, lv_pct(100));
    lv_obj_set_style_text_align(magic_verdict, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(magic_verdict, 2, 0);
    lv_obj_t *button = box(body);
    lv_obj_set_size(button, lv_pct(100), 68);
    lv_obj_set_style_radius(button, SL_R_LG, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xc2adfa), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xd9caff), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(button, 248, LV_STATE_PRESSED);
    magic_button_text = text(button, "REVEAL AN ANSWER", &ss_semibold_21, 0x241d38);
    lv_obj_center(magic_button_text);
    on_click(button, magic_ask_cb, NULL);
    magic_timer = lv_timer_create(magic_tick, 16, NULL);
    lv_timer_pause(magic_timer);
    lv_obj_update_layout(p);
    /* a long language at the biggest text: it scrolls rather than run under the mini player */
    if (lv_obj_get_scroll_bottom(body) > 0) lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    magic_ball_flatten(shine);
    magic_shot = lv_image_create(scene);
    lv_obj_move_to_index(magic_shot, lv_obj_get_index(magic_ball) + 1);
    lv_obj_align(magic_shot, LV_ALIGN_CENTER, 0, -30);
    lv_obj_remove_flag(magic_shot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(magic_shot, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_USER_1);
    magic_warm_timer = lv_timer_create(magic_warm, 700, NULL);   /* after the page has slid in */
    if (magic_sky && !magic_sky_capture())          /* no memory for the still copy: no sky */
    {
        lv_obj_delete(magic_sky);
        magic_sky = NULL;
    }
    magic_sky_paint();
    page_push(p);
}
