/* Freaky Spoon: bounce up out of a pot of soup. GPLv2+.
 *
 * The rules are in sloppy_spoon_state.h. This file is the picture, the
 * sound and the two big arrows. The scene is one RGB565 canvas drawn by hand
 * every frame (the camera moves, so almost everything moves): background rows
 * are filled 32 bits at a time and the shapes are small, anti-aliased on
 * their edges only. Words (score, banners, cards) are LVGL labels on top.
 *
 * Steering: hold an arrow. The pad is one touch target split down the
 * middle, so a thumb can rock from one arrow to the other without lifting.
 * The simulator's and R1's left/right keys steer too. The first press
 * starts a run; after a death the card offers Play again, and a fresh press
 * of either arrow does the same once the card is up. */
#include "sloppy_spoon_state.h"
#define SPN_X ((W - SL_SPOON_W) / 2)
#define SPN_Y (SB_H + 66 + 4)
#define SPN_PAD_Y (SPN_Y + SL_SPOON_H + 12)
#define SPN_PAD_H (H - SPN_PAD_Y - 14)
#define SPN_SAVE ROCKBOX_DIR "/sloppy/spoon.txt"
#define SPN_BITS 72
#define SPN_POPS 5
#define SPN_STEP_US 8333
#define SPN_CARD_DELAY 800             /* ms from a death to the card */
#define SPN_REPLAY_LOCK 350            /* and then until an arrow replays */

enum { SPN_READY, SPN_PLAY, SPN_PAUSED, SPN_OVER };

static struct sl_spoon_state spoon_state;
static uint16_t spn_px[SL_SPOON_W * SL_SPOON_H];
static lv_obj_t *spoon_page_obj, *spn_canvas, *spn_score, *spn_where, *spn_chip, *spn_chip_label;
static lv_obj_t *spn_banner, *spn_banner_sub, *spn_bubble, *spn_best_tag, *spn_last_tag, *spn_header_best;
static lv_obj_t *spn_card, *spn_card_title, *spn_card_quip, *spn_card_score, *spn_card_badge;
static lv_obj_t *spn_card_lines, *spn_card_tip, *spn_card_skin, *spn_card_go, *spn_card_hint;
static lv_obj_t *spn_pad, *spn_arrow[2], *spn_arrow_glyph[2];
static lv_obj_t *spn_pop[SPN_POPS];
static lv_timer_t *spoon_timer;

static int spn_mode;
static int spn_touch_dir, spn_key_dir, spn_pad_shown = 99;
static uint32_t spn_last_tick, spn_acc_us, spn_over_at, spn_banner_at, spn_bubble_at, spn_bubble_ms;
static uint32_t spn_shake_at, spn_land_at, spn_perfect_at, spn_twitch_at, spn_blink_at, spn_next_quip;
static uint32_t spn_score_bump, spn_seed, spn_stare_at, spn_card_at;
static float spn_shake_amp, spn_squash, spn_eye_x, spn_eye_y, spn_eye_vx, spn_eye_vy;
static int spn_shown_score = -1, spn_shown_mult = -1, spn_shown_combo = -1, spn_shown_m = -1, spn_banner_prio;
static int spn_last_height = -1, spn_run_secrets, spn_new_skin = -1, spn_pop_i;
static bool spn_new_best, spn_soup_close;
static uint32_t spn_pop_at[SPN_POPS];
static float spn_pop_x[SPN_POPS], spn_pop_y[SPN_POPS];

/* what is kept between runs */
static struct { int best, best_m, best_combo, runs, skin; uint32_t secrets; } spn_save;
static bool spn_loaded, spn_dirty;

struct spn_bit { float x, y, vx, vy, g, r, life, age; uint32_t col; };
static struct spn_bit spn_bits[SPN_BITS];

static const char *const spn_phase_name[SL_SPOON_PHASES] = {
    "Countertop", "Cupboard", "Freezer", "Rooftop", "Soup Nebula", "Freaky Zone" };
static const char *const spn_phase_sub[SL_SPOON_PHASES] = {
    "Escape the soup", "Crackers crumble, trays slide", "Soggy toast lies, forks patrol",
    "Meatballs from above", "The lights go out in space", "Everything is spoon" };
static const char *const spn_secret_name[SS_COUNT] = {
    "Loyal Customer", "Fork Off", "Dizzy", "Golden Touch", "Sweet Spot", "Freaky",
    "Jelly Legs", "On a Diet", "Hot Feet", "Butterfingers" };
static const char *const spn_secret_how[SS_COUNT] = {
    "8 bounces on one plate", "3 forks stomped in a run", "15 trips round the edge",
    "caught the golden spoon", "15 perfect landings in a row", "reached the Freaky Zone",
    "5 jellies in one run", "300 m without a single O", "bounced right over the soup",
    "gone in under 4 seconds" };

/* ---- spoons to unlock ------------------------------------------------------ */
struct spn_skin { const char *name, *how; uint32_t bowl, rim, handle, shoe; };
static const struct spn_skin spn_skins[] = {
    { "Silver",  "",                         0xe3ebf1, 0x8f9fad, 0xc9d5de, 0xff4d6d },
    { "Wooden",  "Reach the Freezer",        0xd39a5c, 0x86552a, 0xb8804a, 0x3fb56f },
    { "Spork",   "Secret: Fork Off",         0xffa24a, 0xc06418, 0xffb86e, 0x3a8dff },
    { "Golden",  "Secret: Golden Touch",     0xffd45a, 0xb98613, 0xf2c23c, 0x8c3cff },
    { "Void",    "Secret: Freaky",           0x3a2458, 0x150a24, 0x5b3b86, 0x30ffd0 },
    { "Rainbow", "Find 6 secrets",           0xffffff, 0x888888, 0xffffff, 0xffe14a },
};
#define SPN_SKINS ((int)(sizeof spn_skins / sizeof spn_skins[0]))
static int spn_secret_count(void)
{
    int n = 0;
    for (int i = 0; i < SS_COUNT; i++) n += spn_save.secrets >> i & 1;
    return n;
}
static bool spn_skin_open(int i)
{
    switch (i)
    {
    case 1: return spn_save.best_m >= sl_spoon_phase_m[2];
    case 2: return spn_save.secrets & 1u << SS_FORK_OFF;
    case 3: return spn_save.secrets & 1u << SS_GOLDEN;
    case 4: return spn_save.secrets & 1u << SS_FREAKY;
    case 5: return spn_secret_count() >= 6;
    }
    return true;
}
static int spn_skins_open(void)
{
    int n = 0;
    for (int i = 0; i < SPN_SKINS; i++) n += spn_skin_open(i);
    return n;
}

static void spn_load(void)
{
    if (spn_loaded) return;
    spn_loaded = true;
    int fd = open(SPN_SAVE, O_RDONLY);
    if (fd < 0) return;
    char b[96];
    ssize_t got = read(fd, b, sizeof b - 1);
    close(fd);
    if (got <= 0) return;
    b[got] = 0;
    int best, m, combo, runs, skin;
    unsigned secrets;
    if (sscanf(b, "SPOON2 %d %d %d %d %u %d", &best, &m, &combo, &runs, &secrets, &skin) == 6 &&
        best >= 0 && m >= 0 && combo >= 0 && runs >= 0 && best < 100000000 && m < 10000000)
    {
        spn_save.best = best; spn_save.best_m = m; spn_save.best_combo = combo; spn_save.runs = runs;
        spn_save.secrets = secrets & ((1u << SS_COUNT) - 1);
        spn_save.skin = skin >= 0 && skin < SPN_SKINS && spn_skin_open(skin) ? skin : 0;
    }
}
static void spn_store(void)
{
    if (!spn_dirty) return;
    char b[96];
    int len = snprintf(b, sizeof b, "SPOON2 %d %d %d %d %u %d\n", spn_save.best, spn_save.best_m,
                       spn_save.best_combo, spn_save.runs, (unsigned)spn_save.secrets, spn_save.skin);
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(SPN_SAVE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    bool okay = write(fd, b, len) == len;
    if (okay) okay = fsync(fd) == 0;    /* a power cut must not leave an empty file */
    if (close(fd) < 0) okay = false;
    if (!okay || rename(SPN_SAVE ".tmp", SPN_SAVE) < 0) { remove(SPN_SAVE ".tmp"); return; }
    spn_dirty = false;
}

/* ---- pixels ------------------------------------------------------------------ */
static inline uint16_t spn_565(uint32_t c)
{ return (uint16_t)((c >> 19 & 31) << 11 | (c >> 10 & 63) << 5 | (c >> 3 & 31)); }
/* a in 0..32 */
static inline uint16_t spn_mix(uint16_t d, uint16_t s, int a)
{
    uint32_t dd = (d | (uint32_t)d << 16) & 0x07e0f81f, ss = (s | (uint32_t)s << 16) & 0x07e0f81f;
    uint32_t r = ((((ss - dd) * (uint32_t)a) >> 5) + dd) & 0x07e0f81f;
    return (uint16_t)(r | r >> 16);
}
static inline void spn_dot(int x, int y, uint16_t c, int a)
{
    if ((unsigned)x >= SL_SPOON_W || (unsigned)y >= SL_SPOON_H || a <= 0) return;
    uint16_t *p = &spn_px[y * SL_SPOON_W + x];
    *p = a >= 32 ? c : spn_mix(*p, c, a);
}
static void spn_span(float fx0, float fx1, int y, uint16_t c, int a)
{
    if ((unsigned)y >= SL_SPOON_H || fx1 <= fx0) return;
    int x0 = (int)floorf(fx0), x1 = (int)floorf(fx1);
    if (x0 == x1) { spn_dot(x0, y, c, (int)(a * (fx1 - fx0))); return; }
    spn_dot(x0, y, c, (int)(a * (x0 + 1 - fx0)));
    spn_dot(x1, y, c, (int)(a * (fx1 - x1)));
    int a0 = x0 + 1 < 0 ? 0 : x0 + 1, a1 = x1 > SL_SPOON_W ? SL_SPOON_W : x1;
    uint16_t *p = &spn_px[y * SL_SPOON_W];
    if (a >= 32) for (int x = a0; x < a1; x++) p[x] = c;
    else for (int x = a0; x < a1; x++) p[x] = spn_mix(p[x], c, a);
}
static void spn_ellipse(float cx, float cy, float rx, float ry, uint32_t col, int a)
{
    if (rx <= 0.2f || ry <= 0.2f || cy + ry < 0 || cy - ry > SL_SPOON_H) return;
    uint16_t c = spn_565(col);
    int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
    for (int y = y0; y <= y1; y++)
    {
        float dy = (y + 0.5f - cy) / ry;
        if (dy <= -1 || dy >= 1) continue;
        float half = rx * sqrtf(1 - dy * dy);
        /* the top and bottom rows are only partly inside */
        float edge = (1 - fabsf(dy)) * ry;
        spn_span(cx - half, cx + half, y, c, edge < 1 ? (int)(a * edge) : a);
    }
}
static void spn_disc(float cx, float cy, float r, uint32_t col, int a) { spn_ellipse(cx, cy, r, r, col, a); }
static void spn_rrect(float x, float y, float w, float h, float r, uint32_t col, int a)
{
    if (w <= 0 || h <= 0 || y + h < 0 || y > SL_SPOON_H) return;
    uint16_t c = spn_565(col);
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    int y0 = (int)floorf(y), y1 = (int)ceilf(y + h);
    for (int yy = y0; yy < y1; yy++)
    {
        float cy = yy + 0.5f, inset = 0, cov = 1;
        if (cy < y + r) { float d = y + r - cy; inset = r - sqrtf(fmaxf(0, r * r - d * d)); }
        else if (cy > y + h - r) { float d = cy - (y + h - r); inset = r - sqrtf(fmaxf(0, r * r - d * d)); }
        if (yy < y) cov = yy + 1 - y;
        if (yy + 1 > y + h) cov = y + h - yy;
        spn_span(x + inset, x + w - inset, yy, c, (int)(a * sl_spoon_clampf(cov, 0, 1)));
    }
}
static void spn_capsule(float ax, float ay, float bx, float by, float rad, uint32_t col, int a)
{
    int x0 = (int)floorf(fminf(ax, bx) - rad - 1), x1 = (int)ceilf(fmaxf(ax, bx) + rad + 1);
    int y0 = (int)floorf(fminf(ay, by) - rad - 1), y1 = (int)ceilf(fmaxf(ay, by) + rad + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SL_SPOON_W) x1 = SL_SPOON_W;
    if (y1 > SL_SPOON_H) y1 = SL_SPOON_H;
    uint16_t c = spn_565(col);
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            float px = x + 0.5f - ax, py = y + 0.5f - ay;
            float h = l2 > 0 ? sl_spoon_clampf((px * dx + py * dy) / l2, 0, 1) : 0;
            float ex = px - dx * h, ey = py - dy * h;
            float cov = rad + 0.5f - sqrtf(ex * ex + ey * ey);
            if (cov > 0) spn_dot(x, y, c, cov >= 1 ? a : (int)(a * cov));
        }
}
static void spn_ring(float cx, float cy, float r, float th, uint32_t col, int a)
{
    int x0 = (int)(cx - r - 2), x1 = (int)(cx + r + 2), y0 = (int)(cy - r - 2), y1 = (int)(cy + r + 2);
    uint16_t c = spn_565(col);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < SL_SPOON_H; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < SL_SPOON_W; x++)
        {
            float d = fabsf(sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) - r);
            float cov = th / 2 + 0.5f - d;
            if (cov > 0) spn_dot(x, y, c, cov >= 1 ? a : (int)(a * cov));
        }
}
static uint32_t spn_hsv(float h, float s, float v)
{
    h = fmodf(h, 360); if (h < 0) h += 360;
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60, 2) - 1)), m = v - c, r, g, b;
    if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; } else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
    return (uint32_t)((r + m) * 255) << 16 | (uint32_t)((g + m) * 255) << 8 | (uint32_t)((b + m) * 255);
}
static uint32_t spn_lerp_col(uint32_t a, uint32_t b, float t)
{
    t = sl_spoon_clampf(t, 0, 1);
    int r = (int)((a >> 16 & 255) + ((int)(b >> 16 & 255) - (int)(a >> 16 & 255)) * t);
    int g = (int)((a >> 8 & 255) + ((int)(b >> 8 & 255) - (int)(a >> 8 & 255)) * t);
    int bl = (int)((a & 255) + ((int)(b & 255) - (int)(a & 255)) * t);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}
static uint32_t spn_hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ x >> 16; }

/* ---- the kitchen, the cupboard, space... ------------------------------------ */
static const uint32_t spn_top[SL_SPOON_PHASES] = { 0x16233a, 0x2e1b12, 0x0d2c44, 0x28306e, 0x05041a, 0x2a0636 };
static const uint32_t spn_bot[SL_SPOON_PHASES] = { 0x2f4b66, 0x62391f, 0x4f9cc2, 0xee8558, 0x251247, 0x0b3a3a };
static void spn_background(uint32_t now)
{
    const struct sl_spoon_state *s = &spoon_state;
    int m = sl_spoon_metres(s->cam + SL_SPOON_H / 2);
    int ph = sl_spoon_phase_of(m);
    uint32_t top = spn_top[ph], bot = spn_bot[ph];
    if (ph + 1 < SL_SPOON_PHASES && m > sl_spoon_phase_m[ph + 1] - 40)
    {
        float t = (m - (sl_spoon_phase_m[ph + 1] - 40)) / 40.0f;
        top = spn_lerp_col(top, spn_top[ph + 1], t);
        bot = spn_lerp_col(bot, spn_bot[ph + 1], t);
    }
    if (ph == SL_SPOON_PHASES - 1)          /* the colours will not sit still */
    {
        top = spn_hsv(now * 0.02f, 0.75f, 0.22f);
        bot = spn_hsv(now * 0.02f + 140, 0.7f, 0.32f);
    }
    for (int y = 0; y < SL_SPOON_H; y++)
    {
        uint16_t c = spn_565(spn_lerp_col(top, bot, y / (float)SL_SPOON_H));
        uint32_t two = c | (uint32_t)c << 16, *row = (uint32_t *)&spn_px[y * SL_SPOON_W];
        for (int x = 0; x < SL_SPOON_W / 2; x++) row[x] = two;
    }
    float cam = s->cam;
    uint16_t light = spn_565(0xffffff);
    switch (ph)
    {
    case 0:                                  /* wall tiles */
    {
        int off = ((int)(-cam * 0.5f)) % 56;
        for (int y = off; y < SL_SPOON_H; y += 56) spn_span(0, SL_SPOON_W, y, light, 3);
        for (int x = 20; x < SL_SPOON_W; x += 56)
            for (int y = 0; y < SL_SPOON_H; y++) spn_dot(x, y, light, 3);
        break;
    }
    case 1:                                  /* shelves */
    {
        int off = ((int)(-cam * 0.6f)) % 170;
        for (int y = off - 170; y < SL_SPOON_H; y += 170)
        {
            spn_rrect(0, y, SL_SPOON_W, 12, 0, 0x000000, 8);
            spn_span(0, SL_SPOON_W, y, spn_565(0xffd9a0), 5);
            for (int j = 0; j < 4; j++)      /* jars and boxes in silhouette */
            {
                uint32_t h = spn_hash((uint32_t)((y - off) / 170 - (int)(cam * 0.6f) / 170) * 7 + j);
                float jx = 20 + (h % 400), jh = 26 + (h >> 8) % 34, jw = 22 + (h >> 16) % 18;
                spn_rrect(jx, y - jh, jw, jh, 5, 0x000000, 5);
            }
        }
        break;
    }
    case 2:                                  /* snow */
        for (int i = 0; i < 44; i++)
        {
            uint32_t h = spn_hash(i * 977u);
            float x = (h % SL_SPOON_W) + 12 * sinf(now * 0.001f + i);
            float y = fmodf((h >> 12) % 700 + now * (0.02f + (h >> 24) % 20 * 0.002f) - cam * 0.3f, 700) - 80;
            if (y < 0) y += 700;
            spn_disc(x, y, 1.2f + (h >> 20) % 3 * 0.6f, 0xffffff, 16);
        }
        break;
    case 3:                                  /* clouds */
        for (int i = 0; i < 5; i++)
        {
            uint32_t h = spn_hash(i * 131u + 5);
            float x = fmodf((h % 600) + now * 0.008f * (1 + i % 2), 600) - 80;
            float y = fmodf((h >> 10) % 800 - cam * 0.3f, 800) - 120;
            if (y < -120) y += 800;
            spn_disc(x, y, 30, 0xffffff, 3);
            spn_disc(x + 32, y + 6, 24, 0xffffff, 3);
            spn_disc(x - 30, y + 8, 20, 0xffffff, 3);
        }
        break;
    case 4:                                  /* stars and a soup planet */
    {
        for (int i = 0; i < 60; i++)
        {
            uint32_t h = spn_hash(i * 31337u);
            float y = fmodf((h >> 9) % 900 - cam * 0.15f, 900);
            if (y < 0) y += 900;
            int tw = 10 + (int)(12 * (0.5f + 0.5f * sinf(now * 0.004f + i)));
            spn_dot(h % SL_SPOON_W, (int)y, light, tw);
        }
        float py = fmodf(260 - cam * 0.08f, 1400) - 300;
        spn_disc(340, py, 46, 0xd9622e, 22);
        spn_disc(328, py - 10, 34, 0xff9a52, 10);
        spn_capsule(270, py + 8, 410, py - 8, 3, 0xffd08a, 14);
        break;
    }
    default:                                 /* stripes that crawl */
    {
        int sh = (int)(now / 16) % 80;
        for (int y = 0; y < SL_SPOON_H; y++)
        {
            int o = (y + sh) % 80;
            for (int x = -o; x < SL_SPOON_W; x += 80) spn_span(x, x + 40, y, light, 3);
        }
        break;
    }
    }
}

/* ---- the soup -------------------------------------------------------------------- */
static void spn_soup(uint32_t now)
{
    const struct sl_spoon_state *s = &spoon_state;
    float top = s->soup - s->cam, t = now / 1000.0f;
    if (top < SL_SPOON_H + 10)
    {
        uint16_t foam = spn_565(0xffc07a), hot = spn_565(0xf06a2e), deep = spn_565(0xa22e1a);
        for (int x = 0; x < SL_SPOON_W; x++)
        {
            float wave = top + 3.2f * sinf(x * 0.045f + t * 2.2f) + 2 * sinf(x * 0.11f - t * 3.1f);
            int y0 = (int)wave;
            if (y0 < 0) y0 = 0;
            for (int y = y0; y < SL_SPOON_H; y++)
            {
                int d = y - (int)wave;
                uint16_t c = d < 3 ? foam : d > 90 ? deep : spn_mix(hot, deep, d * 32 / 90);
                spn_px[y * SL_SPOON_W + x] = c;
            }
            if (y0 > 0 && y0 < SL_SPOON_H) spn_dot(x, y0 - 1, foam, (int)(32 * (1 - (wave - y0))));
        }
        /* carrots, noodles and bubbles bob on it */
        for (int i = 0; i < 6; i++)
        {
            uint32_t h = spn_hash(i * 91u + 3);
            float x = fmodf((h % SL_SPOON_W) + t * (8 + i * 3), SL_SPOON_W + 40) - 20;
            float y = top + 7 + 3 * sinf(t * 2 + i) + (h >> 12) % 10;
            if (i % 3 == 0) { spn_disc(x, y, 7, 0xff8a1f, 32); spn_ring(x, y, 4, 1.4f, 0xffb35c, 32); }
            else if (i % 3 == 1) spn_capsule(x - 12, y, x + 12, y + 3 * sinf(t * 3 + i), 2.4f, 0xffe7a8, 32);
            else
            {
                float k = fmodf(t * 0.7f + i * 0.37f, 1);
                spn_ring(x, top + 30 - 30 * k, 2 + 4 * k, 1.2f, 0xffd8a8, (int)(28 * (1 - k)));
            }
        }
    }
    /* below the screen but coming: the bottom warms up */
    float gap = top - SL_SPOON_H;
    spn_soup_close = s->started && s->height > 30 && s->soup - s->y < 110;
    if (gap > 0 && gap < 160 && s->started && s->alive)
    {
        float k = 1 - gap / 160;
        uint16_t red = spn_565(0xff3b1f);
        for (int y = SL_SPOON_H - 40; y < SL_SPOON_H; y++)
            spn_span(0, SL_SPOON_W, y, red, (int)(k * 14 * (y - (SL_SPOON_H - 40)) / 40.0f));
    }
}

/* ---- platforms, treats and cutlery ------------------------------------------- */
static void spn_plate(const struct sl_spoon_plat *p, float cam, uint32_t now)
{
    float x = p->x, y = p->y - cam, w = p->w;
    if (y < -30 || y > SL_SPOON_H + 20) return;
    float squish = p->bump < 0.25f ? sinf(p->bump * 25) * (1 - p->bump * 4) * 3 : 0;
    int a = 32;
    if (p->broken)
    {
        a = (int)(32 * (1 - sl_spoon_clampf(p->t * 1.6f, 0, 1)));
        if (a <= 0) return;
    }
    switch (p->type)
    {
    case SP_PLATE:
        spn_rrect(x, y + 3, w, SL_SPOON_PLAT_H, 6, 0x9aa6b8, a);
        spn_rrect(x, y + squish, w, SL_SPOON_PLAT_H - 1, 6, 0xf6f2ea, a);
        spn_rrect(x + 6, y + 4 + squish, w - 12, 2, 1, 0x5b8def, a);
        break;
    case SP_TRAY:
        spn_rrect(x, y + 3, w, SL_SPOON_PLAT_H, 3, 0x56616d, a);
        spn_rrect(x, y + squish, w, SL_SPOON_PLAT_H - 1, 3, 0xc3ced8, a);
        spn_rrect(x + 3, y + 1 + squish, w - 6, 2, 1, 0xffffff, 18);
        /* little chevrons that say which way it is going */
        for (int i = 0; i < 2; i++)
        {
            float cx = x + w / 2 + (i ? 8 : -8), d = p->vx > 0 ? 3 : -3;
            spn_capsule(cx - d, y + 3, cx + d, y + 6, 1.1f, 0x56616d, a);
            spn_capsule(cx + d, y + 6, cx - d, y + 9, 1.1f, 0x56616d, a);
        }
        break;
    case SP_CRACKER:
        if (p->broken)                       /* two halves, falling apart */
        {
            float k = p->t * 60;
            spn_rrect(x - k, y + k * 0.3f, w / 2 - 1, SL_SPOON_PLAT_H, 3, 0xe5b46a, a);
            spn_rrect(x + w / 2 + 1 + k, y + k * 0.5f, w / 2 - 1, SL_SPOON_PLAT_H, 3, 0xe5b46a, a);
            return;
        }
        spn_rrect(x, y + 3, w, SL_SPOON_PLAT_H, 3, 0xa7732f, a);
        spn_rrect(x, y + squish, w, SL_SPOON_PLAT_H - 1, 3, 0xefc47a, a);
        for (float hx = x + 7; hx < x + w - 4; hx += 11) spn_disc(hx, y + 5 + squish, 1.5f, 0xa7732f, a);
        break;
    case SP_SOGGY:
    {
        /* droops in the middle and drips: it will not hold a spoon */
        float sag = p->broken ? p->t * 30 : 3 + sinf(now * 0.004f + x) * 1.2f;
        spn_rrect(x, y + 2, w * 0.35f, SL_SPOON_PLAT_H - 1, 5, 0x8f8a4a, a);
        spn_rrect(x + w * 0.3f, y + 2 + sag, w * 0.4f, SL_SPOON_PLAT_H - 1, 5, 0x8f8a4a, a);
        spn_rrect(x + w * 0.65f, y + 2, w * 0.35f, SL_SPOON_PLAT_H - 1, 5, 0x8f8a4a, a);
        spn_rrect(x + 2, y, w * 0.33f, 4, 2, 0xc4b870, a);
        spn_rrect(x + w * 0.65f, y, w * 0.33f, 4, 2, 0xc4b870, a);
        for (int i = 0; i < 3; i++)
        {
            float k = fmodf(now * 0.0011f + i * 0.33f + x * 0.01f, 1);
            spn_disc(x + w * (0.3f + 0.2f * i), y + 14 + sag + k * 16, 1.8f, 0x8f8a4a, (int)(a * (1 - k)));
        }
        break;
    }
    case SP_JELLY:
    {
        float wob = p->bump < 0.5f ? sinf(p->bump * 30) * (1 - p->bump * 2) * 5 : sinf(now * 0.006f + x) * 0.8f;
        spn_rrect(x - wob, y + 2 + wob * 0.5f, w + 2 * wob, SL_SPOON_PLAT_H + 2 - wob, 7, 0xd8245c, a);
        spn_rrect(x - wob, y + wob * 0.5f, w + 2 * wob, SL_SPOON_PLAT_H - wob, 7, 0xff5d8f, a);
        spn_rrect(x + 6, y + 2 + wob * 0.5f, w * 0.4f, 3, 1.5f, 0xffc1d4, a);
        break;
    }
    case SP_BLINK:
    {
        float ph = fmodf(p->t, 2.6f);
        bool on = ph < 1.7f;
        if (on)
        {
            /* flickers for its last third of a second */
            int fa = ph > 1.35f && ((int)(ph * 20) & 1) ? 14 : 32;
            spn_rrect(x, y + 3, w, SL_SPOON_PLAT_H, 6, 0x5a3bb8, fa);
            spn_rrect(x, y + squish, w, SL_SPOON_PLAT_H - 1, 6, 0xa98bff, fa);
            spn_rrect(x + 4, y + 2, w - 8, 2, 1, 0xe4dbff, fa);
        }
        else
            for (float dx = x; dx < x + w; dx += 10) spn_rrect(dx, y, 6, SL_SPOON_PLAT_H - 1, 3, 0xa98bff, 8);
        break;
    }
    }
    /* the sweet spot, until it is used */
    if (!p->used && !p->broken && p->type != SP_SOGGY && (p->type != SP_BLINK || sl_spoon_blink_on(p)))
        spn_rrect(x + w / 2 - 3, y + 1 + squish, 6, 3, 1.5f, 0xffd24a, 26);
}
static void spn_item(const struct sl_spoon_item *it, float cam, uint32_t now)
{
    float x = it->x, y = it->y - cam + 2 * sinf(now * 0.005f + it->x * 0.1f);
    if (y < -20 || y > SL_SPOON_H + 20) return;
    switch (it->type)
    {
    case SI_O:
        spn_ring(x, y, 7, 5, 0xc98a14, 32);
        spn_ring(x, y - 0.8f, 7, 3.4f, 0xffcf4d, 32);
        spn_disc(x - 3, y - 4, 1.4f, 0xffffff, 26);
        break;
    case SI_WHISK:
        spn_disc(x, y, 18, 0xffffff, 6);
        spn_capsule(x, y + 4, x, y + 15, 2.2f, 0x8a5a2e, 32);
        for (int i = -1; i <= 1; i++)
        {
            float sx = 5 * i * (0.6f + 0.4f * sinf(now * 0.02f));
            spn_capsule(x, y + 4, x + sx - 3, y - 6, 1.1f, 0xdce6ee, 32);
            spn_capsule(x + sx - 3, y - 6, x + sx, y - 12, 1.1f, 0xdce6ee, 32);
            spn_capsule(x + sx, y - 12, x + sx + 3, y - 6, 1.1f, 0xdce6ee, 32);
            spn_capsule(x + sx + 3, y - 6, x, y + 4, 1.1f, 0xdce6ee, 32);
        }
        break;
    case SI_LID:
        spn_disc(x, y, 18, 0x4fb4ff, 7);
        spn_ellipse(x, y + 2, 13, 10, 0x2f86d6, 32);
        spn_ellipse(x, y + 1, 12, 9, 0x6cc4ff, 32);
        spn_rrect(x - 15, y + 3, 30, 5, 2.5f, 0x2066a8, 32);
        spn_disc(x, y - 9, 3, 0x2066a8, 32);
        spn_disc(x - 4, y - 3, 2.4f, 0xffffff, 20);
        break;
    case SI_CHILI:
        spn_disc(x, y, 18, 0xff3b30, 7);
        spn_capsule(x - 8, y - 5, x, y + 3, 5, 0xe8261c, 32);
        spn_capsule(x, y + 3, x + 7, y + 9, 4, 0xe8261c, 32);
        spn_capsule(x + 7, y + 9, x + 10, y + 13, 2.4f, 0xe8261c, 32);
        spn_capsule(x - 8, y - 5, x - 11, y - 11, 1.8f, 0x2fae4d, 32);
        spn_disc(x - 5, y - 4, 1.6f, 0xffffff, 22);
        break;
    }
}
static void spn_spoon_art(float sx, float sy, float sqx, float sqy, float lean, float scale, int skin,
                          uint32_t now, bool alive, bool face);
static void spn_foe(const struct sl_spoon_foe *f, float cam, uint32_t now)
{
    const struct sl_spoon_state *s = &spoon_state;
    float x = f->x, y = f->y - cam;
    if (f->type == SF_MEATBALL && f->state == 0)
    {
        /* where it is going to fall, a second ahead */
        bool lit = (now / 110) & 1;
        /* below the score, so it is never hidden */
        for (float yy = 106; yy < SL_SPOON_H; yy += 18) spn_rrect(x - 1, yy, 2, 8, 1, 0xff4b3a, 8);
        spn_disc(x, 90, 13 + (lit ? 1.5f : 0), 0xff3b30, 32);
        spn_capsule(x, 83, x, 91, 2.2f, 0xffffff, 32);
        spn_disc(x, 96, 2, 0xffffff, 32);
        return;
    }
    if (y < -40 || y > SL_SPOON_H + 40) return;
    if (f->type == SF_MEATBALL)
    {
        spn_capsule(x, y - 44, x, y - 10, 5, 0xffffff, 4);
        spn_disc(x, y, 14, 0x6e3519, 32);
        spn_disc(x - 2, y - 2, 12, 0x94502b, 32);
        spn_disc(x - 5, y - 6, 3.5f, 0xc27a4a, 32);
        spn_disc(x + 5, y + 3, 2.2f, 0xd2361f, 32);
        spn_disc(x - 3, y + 6, 1.8f, 0xd2361f, 32);
        return;
    }
    if (f->type == SF_GOLDEN)
    {
        spn_disc(x, y, 30, 0xffe070, 5 + (int)(3 * sinf(now * 0.01f)));
        spn_spoon_art(x, y + 26, 1, 1, sinf(now * 0.004f) * 0.5f, 0.75f, 3, now, true, true);
        for (int i = 0; i < 3; i++)
        {
            float a = now * 0.004f + i * 2.1f;
            spn_disc(x + 26 * cosf(a), y + 20 * sinf(a), 1.8f, 0xffffff, 28);
        }
        return;
    }
    /* a fork, flying tines first, cross about it */
    float d = f->vx >= 0 ? 1 : -1, fl = f->state ? -1 : 1;
    if (f->state) y += 0;
    spn_capsule(x - 26 * d, y, x + 2 * d, y, 3.2f, 0xaeb8c2, 32);
    spn_capsule(x - 24 * d, y - 1, x, y - 1, 1.2f, 0xffffff, 16);
    spn_capsule(x + 2 * d, y, x + 10 * d, y, 7, 0xc9d2da, 32);
    for (int i = 0; i < 4; i++)
    {
        float oy = (-6 + i * 4) * fl;
        spn_capsule(x + 10 * d, y + oy, x + 25 * d, y + oy, 1.5f, 0xc9d2da, 32);
    }
    if (!f->state)
    {
        float lx = sl_spoon_wdx(x, s->x) > 0 ? 1 : -1, ly = s->y - SL_SPOON_BOWL - f->y > 0 ? 1 : -1;
        for (int i = 0; i < 2; i++)
        {
            float ex = x + (2 + i * 7) * d, ey = y - 2;
            spn_disc(ex, ey, 2.8f, 0xffffff, 32);
            spn_disc(ex + lx, ey + ly * 0.8f, 1.4f, 0x1d1d1f, 32);
        }
        spn_capsule(x + 0 * d, y - 7, x + 5 * d, y - 5, 0.9f, 0x1d1d1f, 32);
        spn_capsule(x + 13 * d, y - 7, x + 8 * d, y - 5, 0.9f, 0x1d1d1f, 32);
    }
    else
        for (int i = 0; i < 2; i++)
        {
            float ex = x + (2 + i * 7) * d, ey = y + 2;
            spn_capsule(ex - 2, ey - 2, ex + 2, ey + 2, 0.8f, 0x1d1d1f, 32);
            spn_capsule(ex - 2, ey + 2, ex + 2, ey - 2, 0.8f, 0x1d1d1f, 32);
        }
}

/* ---- the spoon ---------------------------------------------------------------------- */
static uint32_t spn_skin_bowl(int skin, uint32_t now)
{
    if (skin == 5 || spoon_state.freaky > 0) return spn_hsv(now * 0.25f, 0.55f, 1);
    return spn_skins[skin].bowl;
}
/* The spoon stands on its handle in tiny sneakers, bowl up, with a face in
 * the bowl: googly eyes that do not match, noodle arms with cartoon gloves,
 * and a tongue that will not stay in. face: false for the golden spoon's
 * calmer cousin mode is never used; everything gets the face. */
static void spn_spoon_art(float sx, float sy, float sqx, float sqy, float lean, float scale, int skin,
                          uint32_t now, bool alive, bool face)
{
    const struct sl_spoon_state *s = &spoon_state;
    const struct spn_skin *k = &spn_skins[skin];
    bool me = scale >= 1.1f;
    uint32_t bowl = me ? spn_skin_bowl(skin, now) : k->bowl, rim = k->rim, handle = me && (skin == 5 || s->freaky > 0) ? bowl : k->handle;
    float sc = scale;
    float bx = sx + lean * 14 * sc, by = sy - 40 * sqy * sc;
    float rx = 17 * sqx * sc, ry = 20 * sqy * sc;
    /* sneakers */
    float spread = (sqx - 1) * 10 * sc;
    for (int side = -1; side <= 1; side += 2)
    {
        float fx = sx + side * (5 * sc + spread);
        spn_ellipse(fx + side * 1.5f * sc, sy - 1.5f * sc, 5.2f * sc, 3.2f * sc, k->shoe, 32);
        spn_rrect(fx - 4 * sc + side * 1.5f * sc, sy + 0.2f * sc, 8.5f * sc, 1.6f * sc, 0.8f, 0xffffff, 32);
    }
    /* a rubber-hose handle that bends with the steering and wobbles on landing */
    float wob = me && now - spn_land_at < 400 ? sinf((now - spn_land_at) * 0.045f) * (1 - (now - spn_land_at) / 400.0f) * 5 : 0;
    float px = sx, py = sy - 3 * sc, cx = sx - lean * 10 * sc + wob, cy = sy - 20 * sqy * sc;
    float ex = bx, ey = by + ry - 3 * sc;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 5; i++)
        {
            float t0 = i / 5.0f, t1 = (i + 1) / 5.0f;
            float x0 = (1 - t0) * (1 - t0) * px + 2 * (1 - t0) * t0 * cx + t0 * t0 * ex;
            float y0 = (1 - t0) * (1 - t0) * py + 2 * (1 - t0) * t0 * cy + t0 * t0 * ey;
            float x1 = (1 - t1) * (1 - t1) * px + 2 * (1 - t1) * t1 * cx + t1 * t1 * ex;
            float y1 = (1 - t1) * (1 - t1) * py + 2 * (1 - t1) * t1 * cy + t1 * t1 * ey;
            if (!pass) spn_capsule(x0, y0, x1, y1, 3.8f * sc, rim, 32);
            else spn_capsule(x0 - 0.9f * sc, y0, x1 - 0.9f * sc, y1, 2.2f * sc, handle, 32);
        }
    /* noodle arms: up while rising, flailing while falling, limp when dead */
    for (int side = -1; side <= 1; side += 2)
    {
        float ax = bx + side * (rx - 2 * sc), ay = by + 4 * sc, hx, hy;
        if (!alive) { hx = ax + side * 6 * sc; hy = ay + 14 * sc; }
        else if (me && s->vy > 150)
        {
            float f = sinf(now * 0.035f + side) * 9;
            hx = ax + side * 13 * sc; hy = ay - 10 * sc + f * sc;
        }
        else { hx = ax + side * 10 * sc; hy = ay - 16 * sc + sinf(now * 0.008f + side) * 2 * sc; }
        float mx = (ax + hx) / 2 + side * 4 * sc, my = (ay + hy) / 2 + 3 * sc;
        spn_capsule(ax, ay, mx, my, 2 * sc, rim, 32);
        spn_capsule(mx, my, hx, hy, 2 * sc, rim, 32);
        spn_disc(hx, hy, 4.2f * sc, 0x1d1d1f, 32);
        spn_disc(hx, hy, 3.4f * sc, 0xffffff, 32);
        spn_disc(hx + side * 2.6f * sc, hy - 2.4f * sc, 1.9f * sc, 0xffffff, 32);   /* a thumb */
    }
    /* the bowl, and a Spork's tines */
    if (skin == 2)
        for (int i = -1; i <= 1; i++)
            spn_capsule(bx + i * 7 * sc, by - ry + 2 * sc, bx + i * 8 * sc, by - ry - 8 * sc, 2.6f * sc, rim, 32);
    spn_ellipse(bx, by, rx + 1.5f * sc, ry + 1.5f * sc, rim, 32);
    spn_ellipse(bx, by, rx, ry, bowl, 32);
    spn_ellipse(bx + 3 * sc, by + 5 * sc, rx * 0.72f, ry * 0.6f, rim, 7);
    spn_ellipse(bx - 7 * sc, by - 9 * sc, 3.8f * sc, 6 * sc, 0xffffff, 18);
    if (!face) return;
    /* googly eyes that do not match and bulge out of the bowl; the big one
     * is bloodshot, and now and then it swells even bigger */
    bool twitch = me && alive && now - spn_twitch_at < 260;
    bool blink = me && alive && now - spn_blink_at < 110;
    bool stare = me && alive && now - spn_stare_at < 1400 && s->vy > -900;
    float er[2] = { 6.4f * sc, (twitch ? 11.5f : 8.6f) * sc };
    float eyx[2] = { bx - 7.5f * sc, bx + 7.5f * sc }, eyy[2] = { by - 8 * sc, by - 11 * sc };
    for (int i = 0; i < 2; i++)
    {
        uint32_t white = skin == 4 ? 0x30ffd0 : 0xffffff;
        if (!alive)
        {
            spn_capsule(eyx[i] - 3.5f * sc, eyy[i] - 3.5f * sc, eyx[i] + 3.5f * sc, eyy[i] + 3.5f * sc, 1.4f * sc, 0x1d1d1f, 32);
            spn_capsule(eyx[i] - 3.5f * sc, eyy[i] + 3.5f * sc, eyx[i] + 3.5f * sc, eyy[i] - 3.5f * sc, 1.4f * sc, 0x1d1d1f, 32);
            continue;
        }
        spn_disc(eyx[i], eyy[i], er[i] + 1.2f * sc, 0x1d1d1f, 32);
        if (blink) { spn_disc(eyx[i], eyy[i], er[i], bowl, 32); spn_capsule(eyx[i] - er[i], eyy[i], eyx[i] + er[i], eyy[i], 1.2f * sc, 0x1d1d1f, 32); continue; }
        spn_disc(eyx[i], eyy[i], er[i], white, 32);
        if (i && skin != 4)                     /* veins */
        {
            spn_capsule(eyx[i] + er[i] * 0.95f, eyy[i] + 1, eyx[i] + er[i] * 0.45f, eyy[i] + 2.5f * sc, 0.5f * sc, 0xe0314b, 26);
            spn_capsule(eyx[i] + er[i] * 0.45f, eyy[i] + 2.5f * sc, eyx[i] + er[i] * 0.3f, eyy[i] + 4 * sc, 0.4f * sc, 0xe0314b, 22);
            spn_capsule(eyx[i] - er[i] * 0.9f, eyy[i] - 2 * sc, eyx[i] - er[i] * 0.4f, eyy[i] - 3.5f * sc, 0.45f * sc, 0xe0314b, 22);
        }
        float pr = er[i] * (stare ? 0.22f : 0.4f), room = er[i] - pr - 0.8f;
        float ox = me ? spn_eye_x : 0.4f * sinf(now * 0.003f), oy = me ? spn_eye_y : 0.3f;
        if (stare) ox = oy = 0;                 /* straight at you */
        float ol = sqrtf(ox * ox + oy * oy);
        if (ol > 1) { ox /= ol; oy /= ol; }
        spn_disc(eyx[i] + ox * room, eyy[i] + oy * room, pr, skin == 4 ? 0x2a0048 : 0x14161a, 32);
        if (!stare) spn_disc(eyx[i] + ox * room - pr * 0.4f, eyy[i] + oy * room - pr * 0.4f, pr * 0.35f, 0xffffff, 30);
    }
    /* brows: worried when falling, smug when the combo is hot */
    if (alive && me && !stare)
    {
        float worry = s->vy > 450 ? 1 : 0, smug = s->mult >= 3 ? 1 : 0;
        for (int i = 0; i < 2; i++)
        {
            float side = i ? 1 : -1, bxx = eyx[i], byy = eyy[i] - er[i] - 3 * sc;
            float tilt = (worry ? -2.5f : smug ? 2.2f : 0.8f) * side * sc;
            spn_capsule(bxx - 4.5f * sc, byy - tilt, bxx + 4.5f * sc, byy + tilt, 1.3f * sc, rim, 32);
        }
    }
    /* the mouth: too wide, too many teeth, and a tongue that will not stay in */
    float mx = bx + 1 * sc, my = by + 8 * sc;
    bool blep = me && now - spn_perfect_at < 380;
    uint32_t lip = 0x4a0f22, tongue = 0xff6f91;
    if (!alive)
    {
        spn_ellipse(mx, my, 6 * sc, 3 * sc, lip, 32);
        spn_capsule(mx + 2 * sc, my + 1 * sc, mx + 5 * sc, my + 11 * sc, 2.8f * sc, tongue, 32);
    }
    else if (stare)
        spn_capsule(mx - 5 * sc, my, mx + 5 * sc, my, 0.9f * sc, lip, 32);
    else if (me && (s->freaky > 0 || skin == 4))
    {
        float lo = sinf(now * 0.02f) * 5;
        spn_ellipse(mx, my, 8 * sc, 4.8f * sc, lip, 32);
        spn_capsule(mx, my + 1 * sc, mx + lo * sc, my + 17 * sc, 2.9f * sc, tongue, 32);
    }
    else if (me && s->vy > 450)
    {
        spn_ellipse(mx, my + 1 * sc, 4.6f * sc, 6.5f * sc, lip, 32);
        spn_disc(mx, my + 5 * sc, 2.8f * sc, tongue, 32);
    }
    else
    {
        /* the grin, with a row of little square teeth */
        spn_ellipse(mx, my, 10 * sc, 4.6f * sc, lip, 32);
        for (int t = -3; t <= 3; t++)
            spn_rrect(mx + t * 2.7f * sc - 1.1f * sc, my - 3.6f * sc + fabsf((float)t) * 0.3f * sc, 2.2f * sc, 2.4f * sc, 0.5f, 0xffffff, 32);
        /* lolls out of the corner, and flops about */
        float flop = blep ? 7 : 3.5f + sinf(now * 0.009f) * 1.5f;
        spn_capsule(mx + 5 * sc, my + 2 * sc, mx + (6 + flop * 0.3f) * sc, my + (3 + flop) * sc, 2.4f * sc, tongue, 32);
        spn_capsule(mx + 5.6f * sc, my + 3 * sc, mx + (6 + flop * 0.3f) * sc, my + (2 + flop) * sc, 0.5f * sc, 0xd94070, 20);
    }
}
static int spn_mood_blink(uint32_t now)
{
    if ((int32_t)(now - spn_blink_at) > 2600) spn_blink_at = now + (spn_hash(now) % 2800);
    if ((int32_t)(now - spn_twitch_at) > 7000) spn_twitch_at = now + 4000 + (spn_hash(now + 7) % 5000);
    /* once in a long while: the stare */
    if ((int32_t)(now - spn_stare_at) > 30000) spn_stare_at = now + 14000 + (spn_hash(now + 11) % 16000);
    return 0;
}
static void spn_draw_spoon(uint32_t now, float dt)
{
    const struct sl_spoon_state *s = &spoon_state;
    spn_mood_blink(now);
    /* googly pupils: they lag behind the spoon and slosh when it lands */
    float tx = s->vx / SL_SPOON_VMAX * 0.8f, ty = sl_spoon_clampf(s->vy / 700, -1, 1) * 0.9f;
    spn_eye_vx += ((tx - spn_eye_x) * 90 - spn_eye_vx * 9) * dt;
    spn_eye_vy += ((ty - spn_eye_y) * 90 - spn_eye_vy * 7) * dt;
    spn_eye_x += spn_eye_vx * dt;
    spn_eye_y += spn_eye_vy * dt;
    /* squash on landing, stretch while shooting up */
    uint32_t since = now - spn_land_at;
    float sq = since < 180 ? spn_squash * sinf(since / 180.0f * 3.14159f) : 0;
    float stretch = s->vy < -500 ? sl_spoon_clampf((-s->vy - 500) / 1500, 0, 0.14f) : 0;
    float sqy = 1 - sq * 0.28f + stretch, sqx = 1 + sq * 0.3f - stretch * 0.6f;
    float lean = sl_spoon_clampf(s->vx / SL_SPOON_VMAX, -1, 1) * 0.35f;
    float sy = s->y - s->cam;
    if (s->invuln > 0 && s->whisk <= 0 && (now / 70) & 1) return;
    int skin = spn_save.skin;
    for (int copy = -1; copy <= 1; copy++)   /* half off one edge, half on the other */
    {
        float sx = s->x + copy * SL_SPOON_W;
        if (sx < -40 || sx > SL_SPOON_W + 40) continue;
        float bx = sx + lean * 17, by = sy - 48 * sqy;
        if (s->lid)
        {
            spn_disc(bx, by + 6, 34, 0x4fb4ff, 5);
            spn_ring(bx, by + 6, 34, 2.4f, 0x8fd0ff, 22);
        }
        if (s->whisk > 0)
        {
            spn_capsule(bx, by - 20, bx, by - 34, 2, 0x8a5a2e, 32);
            float a = now * 0.05f;
            for (int i = 0; i < 3; i++)
            {
                float c = cosf(a + i * 2.094f);
                spn_capsule(bx, by - 36, bx + 24 * c, by - 36 + 3 * sinf(a + i * 2.094f), 1.8f, 0xdce6ee, 32);
            }
            for (int i = 0; i < 3; i++)
                spn_capsule(bx - 20 + i * 20, by + 26, bx - 20 + i * 20, by + 60 + (now / 30 + i * 7) % 20, 1.2f, 0xffffff, 8);
        }
        spn_spoon_art(sx, sy, sqx, sqy, lean, 1.2f, skin, now, s->alive, true);
    }
}

/* ---- confetti, crumbs, splashes -------------------------------------------------- */
static void spn_burst(float x, float y, int n, float power, uint32_t col, float g)
{
    static const uint32_t party[] = { 0xff375f, 0xff9f0a, 0xffd60a, 0x30d158, 0x0a84ff, 0xbf5af2 };
    for (int i = 0, j = 0; i < SPN_BITS && j < n; i++)
    {
        struct spn_bit *b = &spn_bits[i];
        if (b->life > b->age) continue;
        uint32_t h = spn_hash(spn_seed++);
        float a = (h % 628) / 100.0f, v = power * (60 + h % 140);
        *b = (struct spn_bit){ x, y, cosf(a) * v, sinf(a) * v - 80 * power, g, 1.4f + (h >> 10) % 3 * 0.7f,
                               0.5f + (h >> 14) % 40 / 100.0f, 0, col ? col : party[(h >> 20) % 6] };
        j++;
    }
}
static void spn_draw_bits(float dt)
{
    float cam = spoon_state.cam;
    for (int i = 0; i < SPN_BITS; i++)
    {
        struct spn_bit *b = &spn_bits[i];
        if (b->age >= b->life) continue;
        b->age += dt;
        b->vy += b->g * dt;
        b->x += b->vx * dt; b->y += b->vy * dt;
        float k = 1 - b->age / b->life;
        spn_disc(b->x, b->y - cam, b->r, b->col, (int)(32 * (k < 0.5f ? k * 2 : 1)));
    }
}

/* ---- lights out ------------------------------------------------------------------ */
static void spn_darkness(void)
{
    const struct sl_spoon_state *s = &spoon_state;
    if (!(s->event == SE_DARK && s->event_state == SE_ON)) return;
    float k = sl_spoon_clampf(s->event_t, 0, 0.5f) * 2;   /* fades back in at the end */
    k = fminf(k, sl_spoon_clampf((6 - s->event_t) * 2, 0, 1));
    int dark = (int)(27 * k);
    float cx = s->x, cy = s->y - SL_SPOON_BOWL - s->cam, R = 165, E = 36;
    for (int y = 0; y < SL_SPOON_H; y++)
    {
        float dy = y + 0.5f - cy;
        if (fabsf(dy) >= R + E) { spn_span(0, SL_SPOON_W, y, 0, dark); continue; }
        float ho = sqrtf((R + E) * (R + E) - dy * dy), hi = fabsf(dy) < R ? sqrtf(R * R - dy * dy) : 0;
        spn_span(0, cx - ho, y, 0, dark);
        spn_span(cx + ho, SL_SPOON_W, y, 0, dark);
        for (int x = (int)(cx - ho); x < cx + ho; x++)
        {
            float dx = fabsf(x + 0.5f - cx);
            if (dx < hi) { int nx = (int)ceilf(cx + hi); if (nx > x) x = nx - 1; continue; }
            float t = (sqrtf(dx * dx + dy * dy) - R) / E;
            spn_dot(x, y, 0, (int)(dark * sl_spoon_clampf(t, 0, 1)));
        }
    }
    /* glow-in-the-dark edges, so the way up is still there */
    for (int i = 0; i < SL_SPOON_PLATS; i++)
    {
        const struct sl_spoon_plat *p = &s->plat[i];
        if (!sl_spoon_solid(p) || p->type == SP_SOGGY) continue;
        spn_rrect(p->x, p->y - s->cam, p->w, 2, 1, p->type == SP_JELLY ? 0xff5d8f : 0x9ff6ff, 16);
    }
}

/* ---- one frame of the picture ---------------------------------------------------- */
static void spn_draw(uint32_t now, float dt)
{
    const struct sl_spoon_state *s = &spoon_state;
    float cam = s->cam;
    spn_background(now);
    /* the lines to beat: your best, and where the last run ended */
    for (int i = 0; i < 2; i++)
    {
        int m = i ? spn_last_height : spn_save.best_m;
        lv_obj_t *tag = i ? spn_last_tag : spn_best_tag;
        float y = -(float)m * SL_SPOON_PX_M - cam;
        bool show = m > 5 && y > 40 && y < SL_SPOON_H - 10 && (i == 0 || m != spn_save.best_m);
        if (show)
            for (float x = 0; x < SL_SPOON_W; x += 16)
                spn_rrect(x, y, 9, 2, 1, i ? 0xb8c4cf : 0xffd24a, i ? 14 : 24);
        if (show) { lv_obj_remove_flag(tag, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(tag, SPN_Y + (int)y - 24); }
        else lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
    }
    if (s->event == SE_WIND && s->event_state == SE_ON)
        for (int i = 0; i < 12; i++)
        {
            uint32_t h = spn_hash(i * 17u);
            float x = fmodf((h % SL_SPOON_W) + s->wind * now * 0.5f, SL_SPOON_W + 80);
            if (x < 0) x += SL_SPOON_W + 80;
            float y = (h >> 9) % SL_SPOON_H;
            spn_capsule(x - 40, y, x, y, 1, 0xffffff, 7);
        }
    for (int i = 0; i < SL_SPOON_PLATS; i++) if (s->plat[i].live) spn_plate(&s->plat[i], cam, now);
    for (int i = 0; i < SL_SPOON_ITEMS; i++) if (s->item[i].live) spn_item(&s->item[i], cam, now);
    for (int i = 0; i < SL_SPOON_FOES; i++)
        if (s->foe[i].type && !(s->foe[i].type == SF_MEATBALL && s->foe[i].state == 0)) spn_foe(&s->foe[i], cam, now);
    spn_draw_spoon(now, dt);
    spn_draw_bits(dt);
    spn_soup(now);
    spn_darkness();
    for (int i = 0; i < SL_SPOON_FOES; i++)      /* warnings stay on top of the dark */
        if (s->foe[i].type == SF_MEATBALL && s->foe[i].state == 0) spn_foe(&s->foe[i], cam, now);
    /* a soft shade behind the score */
    for (int y = 0; y < 64; y++) spn_span(0, SL_SPOON_W, y, 0, (64 - y) * 10 / 64);
    /* combo pips under the multiplier, and what power is running */
    if (s->started && s->alive)
    {
        int m = s->mult, need = m < 5 ? sl_spoon_mult_at[m] - sl_spoon_mult_at[m - 1] : 5;
        int have = m < 5 ? s->combo - sl_spoon_mult_at[m - 1] : 5;
        float x0 = SL_SPOON_W - 16 - (need - 1) * 12;
        for (int i = 0; i < need; i++)
            spn_disc(x0 + i * 12, 58, 3.4f, i < have ? 0xffd24a : 0xffffff, i < have ? 32 : 8);
        float bar = s->whisk > 0 ? s->whisk / SL_SPOON_WHISK_T : s->freaky > 0 ? s->freaky / SL_SPOON_CHILI_T : 0;
        if (bar > 0)
        {
            spn_rrect(SL_SPOON_W - 90, 70, 76, 5, 2.5f, 0xffffff, 8);
            spn_rrect(SL_SPOON_W - 90, 70, 76 * bar, 5, 2.5f, s->whisk > 0 ? 0xdce6ee : 0xff3b30, 32);
        }
        if (s->lid)
        {
            spn_ellipse(SL_SPOON_W - 104, 72, 8, 6, 0x6cc4ff, 32);
            spn_rrect(SL_SPOON_W - 114, 73, 20, 4, 2, 0x2066a8, 32);
        }
    }
    /* round the corners into the page */
    uint16_t bg = spn_565(C_BG);
    const int R = 22;
    for (int y = 0; y < R; y++)
    {
        float d = R - y - 0.5f, in = R - sqrtf(fmaxf(0, R * R - d * d));
        for (int x = 0; x < (int)ceilf(in); x++)
        {
            int a = x + 1 <= in ? 32 : (int)(32 * (in - x));
            spn_dot(x, y, bg, a); spn_dot(SL_SPOON_W - 1 - x, y, bg, a);
            spn_dot(x, SL_SPOON_H - 1 - y, bg, a); spn_dot(SL_SPOON_W - 1 - x, SL_SPOON_H - 1 - y, bg, a);
        }
    }
    lv_obj_invalidate(spn_canvas);
}

/* ---- words ------------------------------------------------------------------------- */
static void spn_num(char *b, size_t n, int v)
{
    if (v >= 1000000) snprintf(b, n, "%d,%03d,%03d", v / 1000000, v / 1000 % 1000, v % 1000);
    else if (v >= 1000) snprintf(b, n, "%d,%03d", v / 1000, v % 1000);
    else snprintf(b, n, "%d", v);
}
static void spn_say(const char *line, int ms)
{
    lv_label_set_text(spn_bubble, line);
    spn_bubble_at = lv_tick_get();
    spn_bubble_ms = ms;
}
static const char *spn_pick(const char *const *lines, int n) { return lines[spn_hash(spn_seed++) % n]; }
#define SPN_PICK(a) spn_pick(a, (int)(sizeof a / sizeof a[0]))
static const char *const spn_q_start[] = { "escape the soup!", "i have legs now. don't ask", "SPOON TIME",
    "no thoughts. only bounce", "the soup wants me back", "hehehe", "let me OUT" };
static const char *const spn_q_idle[] = { "why am i a spoon", "i can taste colours", "a fork winked at me",
    "i regret nothing", "soup is just wet food", "who designed this kitchen", "do spoons dream?",
    "my handle is tingling", "i am the cutlery now", "i licked a meatball once", "stop looking at me",
    "spoon. spoon. spoon.", "my shoes are untied", "i miss the drawer" };
static const char *const spn_q_hot[] = { "HOT HOT HOT", "not the soup!!", "it's getting brothy", "i smell broth" };
static const char *const spn_q_combo[] = { "i am the chosen spoon", "UNTOUCHABLE", "look ma, no fork", "perfection. obviously" };
static const char *const spn_q_best[] = { "new record, baby", "i'm built different", "higher than ever!!" };
static const char *const spn_q_phase[SL_SPOON_PHASES] = { "", "smells like old cereal", "my handle is FREEZING",
    "i can see my house", "am i... in space?", "everything is spoon" };
static void spn_banner_show(const char *title, const char *sub, uint32_t col, int prio)
{
    uint32_t now = lv_tick_get();
    if (prio < spn_banner_prio && now - spn_banner_at < 1100) return;
    spn_banner_prio = prio;
    spn_banner_at = now;
    lv_label_set_text(spn_banner, sl_tr(title));
    lv_obj_set_style_text_color(spn_banner, lv_color_hex(col), 0);
    lv_label_set_text(spn_banner_sub, sub ? sl_tr(sub) : "");
}
static void spn_popup(const char *t, float x, float wy, uint32_t col)
{
    /* the card is up (ready, over, paused): a word from the play behind it
     * would only show cut off at its edge */
    if (spn_card && !lv_obj_has_flag(spn_card, LV_OBJ_FLAG_HIDDEN)) return;
    int i = spn_pop_i++ % SPN_POPS;
    lv_label_set_text(spn_pop[i], t);
    lv_obj_set_style_text_color(spn_pop[i], lv_color_hex(col), 0);
    spn_pop_at[i] = lv_tick_get();
    spn_pop_x[i] = x; spn_pop_y[i] = wy;
}
static void spn_shake(float amp)
{
    if (amp >= spn_shake_amp * (1 - (lv_tick_get() - spn_shake_at) / 300.0f)) { spn_shake_amp = amp; spn_shake_at = lv_tick_get(); }
}

static void spn_card_ready(void);
static void spn_card_over(void);
static void spn_card_paused(void);
static void spn_card_hide(void) { lv_obj_add_flag(spn_card, LV_OBJ_FLAG_HIDDEN); }

static void spn_finish_run(void)
{
    const struct sl_spoon_state *s = &spoon_state;
    int before = spn_skins_open();
    spn_save.runs++;
    spn_new_best = s->score > spn_save.best && s->score > 0;
    if (spn_new_best) spn_save.best = s->score;
    if (s->height > spn_save.best_m) spn_save.best_m = s->height;
    if (s->best_combo > spn_save.best_combo) spn_save.best_combo = s->best_combo;
    spn_new_skin = -1;
    if (spn_skins_open() > before)
        for (int i = SPN_SKINS - 1; i > 0; i--)
            if (spn_skin_open(i)) { spn_new_skin = i; break; }
    spn_dirty = true;
    spn_store();
    char b[48], n[24];
    spn_num(n, sizeof n, spn_save.best);
    snprintf(b, sizeof b, "Best %s", n);
    lv_label_set_text(spn_header_best, b);
}
/* what the rules said happened, as sound and fireworks */
static void spn_effects(uint32_t now)
{
    struct sl_spoon_state *s = &spoon_state;
    char b[48];
    for (int i = 0; i < s->fxn; i++)
    {
        const struct sl_spoon_fx *f = &s->fxq[i];
        float wx = f->x, wy = f->y + s->cam;
        switch (f->kind)
        {
        case SX_LAND:
            sfx_play_vary(SFX_SPOON_BOING);
            spn_land_at = now; spn_squash = 0.8f;
            spn_eye_vy += 9;
            spn_burst(wx, wy, 4, 0.5f, 0xffffff, 500);
            break;
        case SX_PERFECT:
            spn_land_at = now; spn_perfect_at = now; spn_squash = 1;
            spn_eye_vy += 12;
            sfx_play_ex(SFX_SPOON_PERFECT, 100 + (f->v > 15 ? 15 : f->v) * 7, 100);
            spn_burst(wx, wy, 8, 0.8f, 0xffd24a, 300);
            snprintf(b, sizeof b, f->v >= 3 ? "PERFECT x%d" : "PERFECT", f->v);
            spn_popup(b, wx, wy - 60, 0xffd24a);
            break;
        case SX_MULT:
            snprintf(b, sizeof b, "x%d!", f->v);
            spn_banner_show(b, f->v >= 5 ? "maximum freak" : "combo multiplier", 0xffd24a, 1);
            if (f->v >= 5) spn_say(SPN_PICK(spn_q_combo), 1600);
            spn_score_bump = now;
            break;
        case SX_COMBO_LOST:
            sfx_play(SFX_SPOON_LOST);
            snprintf(b, sizeof b, "combo %d lost", f->v);
            spn_popup(b, wx, wy - 60, 0xb8c4cf);
            break;
        case SX_JELLY:
            sfx_play(SFX_SPOON_JELLY);
            spn_land_at = now; spn_squash = 1.4f;
            spn_eye_vy += 18;
            spn_shake(3);
            spn_burst(wx, wy, 10, 0.9f, 0xff5d8f, 600);
            break;
        case SX_CRACK:
            sfx_play(SFX_SPOON_CRACK);
            spn_burst(wx, wy + 4, 10, 0.6f, 0xe5b46a, 900);
            break;
        case SX_SOGGY:
            sfx_play(SFX_SPOON_SOGGY);
            spn_burst(wx, wy + 6, 6, 0.4f, 0x8f8a4a, 700);
            spn_popup("soggy!", wx, wy - 20, 0xc4b870);
            break;
        case SX_COIN:
            sfx_play_ex(SFX_SPOON_COIN, 96 + spn_hash(spn_seed++) % 10, 100);
            spn_burst(wx, wy, 5, 0.5f, 0xffcf4d, 200);
            snprintf(b, sizeof b, "+%d", f->v);
            spn_popup(b, wx, wy - 10, 0xffcf4d);
            spn_score_bump = now;
            break;
        case SX_STOMP:
            sfx_play(SFX_SPOON_STOMP);
            spn_shake(5);
            spn_burst(wx, wy, 14, 1, 0xdce6ee, 700);
            snprintf(b, sizeof b, "FORK OFF +%d", f->v);
            spn_popup(b, wx, wy - 30, 0x9ff6ff);
            spn_score_bump = now;
            break;
        case SX_POWER:
            sfx_play(SFX_SPOON_POWER);
            spn_burst(wx, wy, 16, 1, 0, 300);
            if (f->v == SI_WHISK) { spn_banner_show("WHISK!", "up, up and away", 0xdce6ee, 2); spn_say("WHEEEEEEE", 1400); }
            else if (f->v == SI_LID) { spn_banner_show("SAFETY LID", "takes one hit for you", 0x6cc4ff, 2); spn_say("safety first", 1200); }
            else { spn_banner_show("CHILI!", "every landing is perfect", 0xff3b30, 2); spn_say("OH NO IT'S SPICY", 1600); }
            break;
        case SX_SHIELD:
            sfx_play(SFX_SPOON_SHIELD);
            spn_shake(6);
            spn_burst(wx, wy, 18, 1.2f, 0x8fd0ff, 400);
            spn_popup("lid saved you", wx, wy - 40, 0x8fd0ff);
            break;
        case SX_KNOCK:
            spn_burst(wx, wy, 12, 0.9f, f->v == SF_MEATBALL ? 0x94502b : 0xdce6ee, 700);
            break;
        case SX_PHASE:
            sfx_play(SFX_SPOON_PHASE);
            spn_banner_show(spn_phase_name[f->v], spn_phase_sub[f->v], 0xfffffe, 4);
            spn_say(spn_q_phase[f->v], 1800);
            spn_burst(wx, wy - 40, 24, 1.4f, 0, 400);
            break;
        case SX_WARN:
        {
            int e = f->v & 255, wind = (f->v >> 8) - 1;
            sfx_play(SFX_SPOON_WARN);
            if (e == SE_WIND) spn_banner_show(wind > 0 ? "WIND  >>>" : "<<<  WIND", "it will push you", 0x9ff6ff, 3);
            else if (e == SE_LOWG) spn_banner_show("LOW GRAVITY", "floaty for a bit", 0xbf9bff, 3);
            else if (e == SE_MEATBALLS) spn_banner_show("MEATBALL STORM", "watch for the red marks", 0xff6b4a, 3);
            else if (e == SE_DARK) spn_banner_show("LIGHTS OUT", "the plates glow", 0x9ff6ff, 3);
            else spn_banner_show("O RAIN", "grab them!", 0xffcf4d, 3);
            break;
        }
        case SX_GOLDEN:
            sfx_play(SFX_SPOON_GOLDEN);
            spn_shake(4);
            spn_burst(wx, wy, 30, 1.6f, 0xffd45a, 300);
            spn_banner_show("GOLDEN SPOON", "+300. you saw nothing", 0xffd45a, 5);
            spn_say("a SHINY me??", 1600);
            spn_score_bump = now;
            break;
        case SX_SECRET:
            if (!(spn_save.secrets & 1u << f->v))
            {
                spn_save.secrets |= 1u << f->v;
                spn_run_secrets |= 1 << f->v;
                spn_dirty = true;
                sfx_play(SFX_SPOON_SECRET);
                snprintf(b, sizeof b, "Secret: %s", spn_secret_name[f->v]);
                spn_banner_show(b, spn_secret_how[f->v], 0xbf9bff, 5);
            }
            break;
        case SX_BEST:
            sfx_play(SFX_SPOON_BEST);
            spn_banner_show("NEW BEST HEIGHT", "keep going!", 0xffd24a, 4);
            spn_say(SPN_PICK(spn_q_best), 1500);
            spn_burst(wx, wy - 40, 30, 1.6f, 0, 400);
            break;
        case SX_SPLASH:
            sfx_play(SFX_SPOON_SPLASH);
            spn_burst(wx, wy, 22, 1.3f, 0xf06a2e, 900);
            spn_burst(wx, wy, 8, 1, 0xffc07a, 900);
            break;
        case SX_DIE:
            if (f->v != SD_SOUP) sfx_play(SFX_SPOON_HIT);
            spn_shake(f->v == SD_SOUP ? 7 : 10);
            spn_mode = SPN_OVER;
            spn_over_at = now;
            spn_finish_run();
            break;
        }
    }
    s->fxn = 0;
}

/* ---- the HUD, the banner, popups, the speech bubble ------------------------------- */
static void spn_hud(uint32_t now)
{
    const struct sl_spoon_state *s = &spoon_state;
    char b[64];
    if (s->score != spn_shown_score)
    {
        spn_shown_score = s->score;
        spn_num(b, sizeof b, s->score);
        lv_label_set_text(spn_score, b);
    }
    int m = s->started ? s->height : spn_save.best_m;
    if (m != spn_shown_m || spn_shown_m < 0)
    {
        spn_shown_m = m;
        if (s->started) snprintf(b, sizeof b, "%s  ·  %d m", sl_tr(spn_phase_name[s->phase]), s->height);
        else snprintf(b, sizeof b, "%s", sl_tr(spn_phase_name[0]));
        lv_label_set_text(spn_where, b);
    }
    if (s->mult != spn_shown_mult || s->combo != spn_shown_combo)
    {
        static const uint32_t cols[6] = { 0, 0x3a4552, 0x1fa38a, 0xe0a100, 0xf0642a, 0xff2d78 };
        spn_shown_mult = s->mult; spn_shown_combo = s->combo;
        snprintf(b, sizeof b, "x%d", s->mult);
        lv_label_set_text(spn_chip_label, b);
        lv_obj_set_style_bg_color(spn_chip, lv_color_hex(cols[s->mult]), 0);
    }
    if (s->started && s->alive) lv_obj_remove_flag(spn_chip, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(spn_chip, LV_OBJ_FLAG_HIDDEN);
    uint32_t t = now - spn_score_bump;
    int scale = spn_score_bump && t < 260 ? 256 + (int)(40 * sinf(t / 260.0f * 3.14159f)) : 256;
    static int shown_scale = -1, shown_dx, shown_dy;
    if (scale != shown_scale)
    {
        shown_scale = scale;
        lv_obj_set_style_transform_scale(spn_score, scale, 0);
        lv_obj_set_style_transform_scale(spn_chip, scale, 0);
    }

    /* the banner: pops in, holds, fades */
    uint32_t bt = now - spn_banner_at;
    if (spn_banner_at && bt < 1500)
    {
        float in = sl_spoon_clampf(bt / 160.0f, 0, 1), out = 1 - sl_spoon_clampf((bt - 1200) / 300.0f, 0, 1);
        int sc = (int)(256 * (0.7f + 0.3f * in + 0.08f * sinf(in * 3.14159f)));
        lv_obj_remove_flag(spn_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(spn_banner_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_scale(spn_banner, sc, 0);
        lv_obj_set_style_opa(spn_banner, (lv_opa_t)(255 * in * out), 0);
        lv_obj_set_style_opa(spn_banner_sub, (lv_opa_t)(255 * in * out), 0);
    }
    else
    {
        lv_obj_add_flag(spn_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(spn_banner_sub, LV_OBJ_FLAG_HIDDEN);
        spn_banner_prio = 0;
    }
    for (int i = 0; i < SPN_POPS; i++)
    {
        uint32_t pt = now - spn_pop_at[i];
        if (!spn_pop_at[i] || pt > 800) { lv_obj_add_flag(spn_pop[i], LV_OBJ_FLAG_HIDDEN); continue; }
        float k = pt / 800.0f;
        lv_obj_remove_flag(spn_pop[i], LV_OBJ_FLAG_HIDDEN);
        int w = lv_obj_get_width(spn_pop[i]);
        int x = (int)sl_spoon_clampf(spn_pop_x[i] - w / 2, 6, SL_SPOON_W - 6 - w);
        lv_obj_set_pos(spn_pop[i], SPN_X + x, SPN_Y + (int)(spn_pop_y[i] - s->cam - 36 * k));
        lv_obj_set_style_opa(spn_pop[i], (lv_opa_t)(255 * (1 - k * k)), 0);
    }
    /* the spoon talks */
    if (spn_mode == SPN_PLAY && s->alive && (int32_t)(now - spn_next_quip) > 0)
    {
        spn_next_quip = now + 18000 + spn_hash(spn_seed++) % 12000;
        spn_say(SPN_PICK(spn_q_idle), 1700);
    }
    if (spn_mode == SPN_PLAY && spn_soup_close && lv_tick_get() - spn_bubble_at > 5000)
        spn_say(SPN_PICK(spn_q_hot), 1100);
    uint32_t qt = lv_tick_get() - spn_bubble_at;
    if (spn_bubble_at && qt < spn_bubble_ms && spn_mode != SPN_PAUSED)
    {
        lv_obj_remove_flag(spn_bubble, LV_OBJ_FLAG_HIDDEN);
        int w = lv_obj_get_width(spn_bubble), h = lv_obj_get_height(spn_bubble);
        int x = (int)sl_spoon_clampf(s->x - w / 2, 8, SL_SPOON_W - 8 - w);
        int y = (int)sl_spoon_clampf(s->y - s->cam - 70 - h, 70, SL_SPOON_H - h - 8);
        lv_obj_set_pos(spn_bubble, SPN_X + x, SPN_Y + y);
        lv_obj_set_style_opa(spn_bubble, qt > spn_bubble_ms - 200 ? (lv_opa_t)(255 * (spn_bubble_ms - qt) / 200) : LV_OPA_COVER, 0);
    }
    else lv_obj_add_flag(spn_bubble, LV_OBJ_FLAG_HIDDEN);

    /* the shake */
    int dx = 0, dy = 0;
    uint32_t st = now - spn_shake_at;
    if (spn_shake_at && st < 320)
    {
        float k = spn_shake_amp * (1 - st / 320.0f);
        dx = (int)(k * sinf(st * 0.11f)); dy = (int)(k * 0.6f * cosf(st * 0.13f));
    }
    if (dx != shown_dx || dy != shown_dy)
    {
        shown_dx = dx; shown_dy = dy;
        lv_obj_set_style_translate_x(spn_canvas, dx, 0);
        lv_obj_set_style_translate_y(spn_canvas, dy, 0);
    }
}

/* ---- the arrows ---------------------------------------------------------------------- */
static bool spn_replay_ready(void)
{
    return spn_mode == SPN_OVER && lv_tick_get() - spn_over_at >= SPN_CARD_DELAY + SPN_REPLAY_LOCK;
}
static void spn_pad_look(void)
{
    int dir = spn_touch_dir ? spn_touch_dir : spn_key_dir;
    bool off = spn_mode == SPN_OVER && !spn_replay_ready();
    int key = off ? 7 : dir + 2;
    if (key == spn_pad_shown) return;
    spn_pad_shown = key;
    for (int i = 0; i < 2; i++)
    {
        bool down = !off && dir == (i ? 1 : -1);
        lv_obj_t *b = spn_arrow[i], *g = spn_arrow_glyph[i];
        lv_obj_set_style_bg_color(b, lv_color_hex(down ? sl_accent() : C_CARD), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(down ? sl_accent() : C_CARD_P), 0);
        lv_obj_set_style_shadow_opa(b, down ? LV_OPA_50 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_translate_y(b, down ? 4 : 0, 0);
        lv_obj_set_style_transform_scale(b, down ? 244 : 256, 0);
        lv_obj_set_style_text_color(g, lv_color_hex(down ? sl_accent_ink() : C_TEXT), 0);
        lv_obj_set_style_translate_x(g, down ? (i ? 7 : -7) : 0, 0);
        lv_obj_set_style_opa(b, off ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
}
static void spn_restart(void);
/* a fresh press of an arrow: starts, resumes or replays */
static void spn_arrow_press(void)
{
    struct sl_spoon_state *s = &spoon_state;
    if (spn_mode == SPN_READY)
    {
        spn_mode = SPN_PLAY;
        sl_spoon_start(s);
        spn_card_hide();
        sfx_play(SFX_START);
        spn_say(SPN_PICK(spn_q_start), 1400);
        spn_next_quip = lv_tick_get() + 20000;
    }
    else if (spn_mode == SPN_PAUSED)
    {
        spn_mode = SPN_PLAY;
        spn_card_hide();
        spn_last_tick = lv_tick_get();
    }
    else if (spn_replay_ready()) spn_restart();
}
static void spn_pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    { spn_touch_dir = 0; spoon_state.dir = spn_key_dir; spn_pad_look(); return; }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    spn_touch_dir = pt.x < W / 2 ? -1 : 1;
    spoon_state.dir = spn_touch_dir;
    if (code == LV_EVENT_PRESSED) spn_arrow_press();
    spn_pad_look();
}
static bool spoon_key(int button, bool rel)
{
    if (button != BUTTON_LEFT && button != BUTTON_RIGHT) return false;
    int d = button == BUTTON_LEFT ? -1 : 1;
    if (rel) { if (spn_key_dir == d) spn_key_dir = 0; }
    else
    {
        bool fresh = spn_key_dir != d;
        spn_key_dir = d;
        if (fresh) spn_arrow_press();
    }
    spoon_state.dir = spn_touch_dir ? spn_touch_dir : spn_key_dir;
    spn_pad_look();
    return true;
}
static bool spoon_on_top(void)
{
    return spoon_page_obj && npages && pages[npages - 1] == spoon_page_obj;
}

/* ---- cards ---------------------------------------------------------------------------- */
static void spn_card_show(void)
{
    lv_obj_remove_flag(spn_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(spn_card);
    lv_obj_set_style_opa(spn_card, LV_OPA_TRANSP, 0);
    spn_card_at = lv_tick_get();
    /* centred on the scene, whatever it holds */
    lv_obj_update_layout(spn_card);
    int h = lv_obj_get_height(spn_card), y = SPN_Y + (SL_SPOON_H - h) / 2;
    if (spn_mode == SPN_READY)                      /* the spoon hops below it; it clears the zone's name */
        y = LV_MAX(SPN_Y + 64, SPN_Y + 44 + lv_font_get_line_height(&ss_regular_16) + 4);
    lv_obj_set_y(spn_card, y < SPN_Y + 6 ? SPN_Y + 6 : y);
}
static void spn_skin_label(void)
{
    char b[96], who[48];
    snprintf(who, sizeof who, sl_tr("Spoon: %s"), sl_tr(spn_skins[spn_save.skin].name));
    snprintf(b, sizeof b, "%s   %d/%d  %s", who, spn_skins_open(), SPN_SKINS, sl_chevron());
    lv_obj_t *l = lv_obj_get_child(spn_card_skin, 0);
    lv_label_set_text(l, b);
    lv_obj_set_style_text_font(l, &ss_semibold_18, 0);   /* a long name in a long language ("Ложка: Деревянная") steps down to fit */
    sl_fit_now(l, 300 - 2 * 18);
}
/* the card's one-line quip: a long translation steps the size down to fit */
static void spn_quip(const char *s)
{
    lv_label_set_text(spn_card_quip, s);
    lv_obj_set_style_text_font(spn_card_quip, &ss_semibold_18, 0);
    sl_fit_now(spn_card_quip, 392 - 2 * 18);
}
static void spn_card_ready(void)
{
    char b[160], n1[24], n2[24];
    lv_label_set_text(spn_card_title, sl_tr("Freaky Spoon"));
    lv_obj_set_style_text_color(spn_card_title, lv_color_hex(0xfffffe), 0);
    spn_quip(sl_tr("Bounce up. Escape the soup."));
    lv_obj_add_flag(spn_card_score, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(spn_card_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(spn_card_go, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(spn_card_skin, LV_OBJ_FLAG_HIDDEN);
    spn_num(n1, sizeof n1, spn_save.best);
    spn_num(n2, sizeof n2, spn_save.best_m);
    if (spn_save.runs)
        snprintf(b, sizeof b, sl_tr("Best %s  ·  %s m  ·  combo %d\nSecrets %d/%d"), n1, n2, spn_save.best_combo,
                 spn_secret_count(), SS_COUNT);
    else snprintf(b, sizeof b, "%s", sl_tr("Land dead centre on the gold mark\nfor combos and a score multiplier"));
    lv_label_set_text(spn_card_lines, b);
    lv_label_set_text(spn_card_tip, sl_tr(spn_save.runs ? "Land on the gold mark for combos" : "Avoid soggy toast. Stomp forks."));
    spn_skin_label();
    lv_label_set_text_fmt(spn_card_hint, sl_tr("Hold %s or %s to start"), LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    spn_card_show();
}
static void spn_card_paused(void)
{
    lv_label_set_text(spn_card_title, sl_tr("Paused"));
    lv_obj_set_style_text_color(spn_card_title, lv_color_hex(0xfffffe), 0);
    spn_quip(sl_tr("the soup is waiting too"));
    lv_obj_add_flag(spn_card_score, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(spn_card_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(spn_card_go, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(spn_card_skin, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(spn_card_lines, "");
    lv_label_set_text(spn_card_tip, "");
    lv_label_set_text_fmt(spn_card_hint, sl_tr("Press %s or %s to carry on"), LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    spn_card_show();
}
static const char *const spn_die_soup[] = { "The soup always wins.", "You're part of the broth now.", "Mmm. Spoon stew." };
static const char *const spn_die_fork[] = { "Forked. Rude.", "The cutlery drawer remembers.", "Stabbed by a salad utensil." };
static const char *const spn_die_ball[] = { "Bonked by a meatball.", "Hit by flying dinner.", "That was a big meatball." };
static const char *const spn_tips[] = { "Tip: the gold mark on each plate is the sweet spot",
    "Tip: go off one edge to come back on the other", "Tip: soggy toast droops and drips",
    "Tip: a combo of 15 makes you x5", "Tip: blinking plates flicker before they vanish",
    "Tip: land on a fork from above" };
static void spn_card_over(void)
{
    const struct sl_spoon_state *s = &spoon_state;
    char b[320], n1[24], n2[24];
    bool caught = s->death == SD_SOUP && s->soup < s->cam + SL_SPOON_H - 4;
    const char *title = s->death == SD_FORK ? "FORKED" : s->death == SD_MEATBALL ? "MEATBALLED" : "SOUPED";
    lv_label_set_text(spn_card_title, title);
    lv_obj_set_style_text_color(spn_card_title, lv_color_hex(s->death == SD_SOUP ? 0xff8a3d : 0xff5d8f), 0);
    lv_label_set_text(spn_card_quip, s->death == SD_FORK ? SPN_PICK(spn_die_fork) :
                                     s->death == SD_MEATBALL ? SPN_PICK(spn_die_ball) : SPN_PICK(spn_die_soup));
    spn_num(n1, sizeof n1, s->score);
    lv_label_set_text(spn_card_score, n1);
    lv_obj_remove_flag(spn_card_score, LV_OBJ_FLAG_HIDDEN);
    if (spn_new_best) lv_obj_remove_flag(spn_card_badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(spn_card_badge, LV_OBJ_FLAG_HIDDEN);
    spn_num(n2, sizeof n2, spn_save.best);
    int len = snprintf(b, sizeof b, "%d m  ·  combo %d  ·  %d O%s\n", s->height, s->best_combo, s->coins, s->coins == 1 ? "" : "'s");
    if (!spn_new_best) len += snprintf(b + len, sizeof b - len, "Best %s  ·  %d m\n", n2, spn_save.best_m);
    if (s->phase + 1 < SL_SPOON_PHASES)
        len += snprintf(b + len, sizeof b - len, "Reached the %s. %s at %d m",
                        spn_phase_name[s->phase], spn_phase_name[s->phase + 1], sl_spoon_phase_m[s->phase + 1]);
    else len += snprintf(b + len, sizeof b - len, "Reached the Freaky Zone. Nothing is real");
    for (int i = 0; i < SS_COUNT && len < (int)sizeof b - 40; i++)
        if (spn_run_secrets >> i & 1) len += snprintf(b + len, sizeof b - len, "\nSecret found: %s", spn_secret_name[i]);
    if (spn_new_skin >= 0 && len < (int)sizeof b - 40)
        snprintf(b + len, sizeof b - len, "\nNew spoon: %s!", spn_skins[spn_new_skin].name);
    lv_label_set_text(spn_card_lines, b);
    /* what went wrong, said so it helps next time */
    const char *tip = s->death == SD_FORK ? "Forks hurt from the side. Land on top to stomp them" :
                      s->death == SD_MEATBALL ? "A red ! at the top shows where a meatball falls" :
                      caught ? "The soup rises. Don't dawdle" :
                      s->height < 40 ? "Hold an arrow to steer. Land on plates" : SPN_PICK(spn_tips);
    lv_label_set_text(spn_card_tip, tip);
    lv_obj_add_flag(spn_card_skin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(spn_card_go, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(spn_card_hint, "or press " LV_SYMBOL_LEFT " / " LV_SYMBOL_RIGHT);
    spn_card_show();
}
static void spn_skin_cb(lv_event_t *e)
{
    (void)e;
    for (int i = 1; i <= SPN_SKINS; i++)
    {
        int k = (spn_save.skin + i) % SPN_SKINS;
        if (!spn_skin_open(k)) continue;
        if (k != spn_save.skin) { spn_save.skin = k; spn_dirty = true; sfx_play(SFX_TAP); }
        break;
    }
    if (spn_skins_open() == 1)
    {
        /* nothing to switch to yet: say how to get the next one */
        for (int i = 1; i < SPN_SKINS; i++)
            if (!spn_skin_open(i)) { char b[64]; snprintf(b, sizeof b, "Next spoon: %s", spn_skins[i].how); spn_say(b, 1800); break; }
    }
    spn_skin_label();
}
static void spn_again_cb(lv_event_t *e) { (void)e; if (spn_mode == SPN_OVER) spn_restart(); }

/* ---- the loop -------------------------------------------------------------------------- */
static void spn_reset(bool play)
{
    struct sl_spoon_state *s = &spoon_state;
    spn_seed ^= (sl_ms() + lv_tick_get()) * 2654435761u;
    sl_spoon_init(s, spn_seed | 1);
    s->best_height = spn_save.best_m;
    s->dir = spn_touch_dir ? spn_touch_dir : spn_key_dir;
    memset(spn_bits, 0, sizeof spn_bits);
    spn_run_secrets = 0;
    spn_new_best = false;
    spn_banner_at = spn_bubble_at = 0;
    spn_shake_at = 0;
    spn_acc_us = 0;
    spn_eye_x = spn_eye_y = spn_eye_vx = spn_eye_vy = 0;
    spn_shown_m = -1;
    for (int i = 0; i < SPN_POPS; i++) spn_pop_at[i] = 0;
    spn_mode = SPN_READY;
    if (play) spn_arrow_press();
    else spn_card_ready();
}
static void spn_restart(void)
{
    spn_last_height = spoon_state.height;
    spn_reset(true);
    spn_pad_look();
}
static void spoon_frame(lv_timer_t *timer)
{
    if (!spoon_on_top() || lv_obj_has_flag(spoon_page_obj, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t now = lv_tick_get();
    uint32_t ms = spn_last_tick ? now - spn_last_tick : 16;
    if (ms > 50) ms = 50;
    spn_last_tick = now;
    /* the lock screen or a pulled-down panel pauses a run */
    if (spn_mode == SPN_PLAY && (sl_power_hides_ui() || overlay_visible()))
    {
        spn_mode = SPN_PAUSED;
        spn_touch_dir = spn_key_dir = 0;
        spoon_state.dir = 0;
        spn_card_paused();
    }
    if (spn_mode != SPN_PAUSED)
    {
        spn_acc_us += ms * 1000;
        for (int n = 0; spn_acc_us >= SPN_STEP_US && n < 8; n++)
        {
            spn_acc_us -= SPN_STEP_US;
            sl_spoon_step(&spoon_state);
            spn_effects(now);
        }
        if (spn_acc_us > SPN_STEP_US * 2) spn_acc_us = 0;
    }
    if (spn_mode == SPN_OVER && lv_obj_has_flag(spn_card, LV_OBJ_FLAG_HIDDEN) && now - spn_over_at >= SPN_CARD_DELAY)
        spn_card_over();
    if (!lv_obj_has_flag(spn_card, LV_OBJ_FLAG_HIDDEN))
    {
        float k = sl_spoon_clampf((now - spn_card_at) / 220.0f, 0, 1);
        k = 1 - (1 - k) * (1 - k);
        if (lv_obj_get_style_opa(spn_card, 0) != (lv_opa_t)(255 * k))
        {
            lv_obj_set_style_opa(spn_card, (lv_opa_t)(255 * k), 0);
            lv_obj_set_style_translate_y(spn_card, (int)(18 * (1 - k)), 0);
        }
    }
    spn_draw(now, spn_mode == SPN_PAUSED ? 0 : ms / 1000.0f);
    spn_hud(now);
    spn_pad_look();
    (void)timer;
}
static void spoon_deleted(lv_event_t *e)
{
    (void)e;
    if (spoon_timer) { lv_timer_delete(spoon_timer); spoon_timer = NULL; }
    spn_store();
    spoon_page_obj = spn_canvas = spn_score = spn_where = spn_chip = spn_chip_label = NULL;
    spn_banner = spn_banner_sub = spn_bubble = spn_best_tag = spn_last_tag = spn_header_best = NULL;
    spn_card = spn_card_title = spn_card_quip = spn_card_score = spn_card_badge = NULL;
    spn_card_lines = spn_card_tip = spn_card_skin = spn_card_go = spn_card_hint = NULL;
    spn_pad = spn_arrow[0] = spn_arrow[1] = spn_arrow_glyph[0] = spn_arrow_glyph[1] = NULL;
    for (int i = 0; i < SPN_POPS; i++) spn_pop[i] = NULL;
    spn_touch_dir = spn_key_dir = 0;
    spn_last_tick = 0;
    sl_give_back(spn_px, sizeof spn_px);   /* 463 KB, drawn again on the next open */
    mini_update();
}
static lv_obj_t *spn_label(lv_obj_t *parent, const lv_font_t *font, uint32_t col)
{
    lv_obj_t *l = text(parent, "", font, col);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}
static void spoon_page(void)
{
    if (page_bring_back(spoon_page_obj)) return;
    if (!sl_app_room(600, "Freaky Spoon")) return;
    lv_obj_t *p, *body = page_new("Freaky Spoon", &p);
    spoon_page_obj = p;
    spn_load();
    lv_obj_add_event_cb(p, spoon_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_NONE);
    lv_obj_set_layout(body, LV_LAYOUT_NONE);        /* everything is placed by hand */

    char b[48], n[24];
    spn_num(n, sizeof n, spn_save.best);
    snprintf(b, sizeof b, "Best %s", n);
    spn_header_best = text(p, spn_save.best ? b : "", &ss_semibold_18, C_SUB);
    lv_obj_set_style_text_align(spn_header_best, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(spn_header_best, 180);
    lv_obj_set_pos(spn_header_best, W - 20 - 180, SB_H + 22);
    header_title_clear(p, W - 20 - 180);   /* a long or right-to-left title ran into Best */

    spn_canvas = lv_canvas_create(p);
    lv_canvas_set_buffer(spn_canvas, spn_px, SL_SPOON_W, SL_SPOON_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(spn_canvas, SPN_X, SPN_Y);
    lv_obj_add_flag(spn_canvas, LV_OBJ_FLAG_USER_1);   /* no back-swipe across the scene */

    /* the lines to beat */
    spn_best_tag = text(p, "BEST", &ss_semibold_18, 0xffd24a);
    lv_obj_set_x(spn_best_tag, SPN_X + SL_SPOON_W - 58);
    spn_last_tag = text(p, "LAST", &ss_semibold_18, 0xb8c4cf);
    lv_obj_set_x(spn_last_tag, SPN_X + 10);

    spn_score = text(p, "0", &ss_bold_28, 0xfffffe);
    lv_obj_set_pos(spn_score, SPN_X + 18, SPN_Y + 8);
    lv_obj_set_style_transform_pivot_x(spn_score, 0, 0);
    lv_obj_set_style_transform_pivot_y(spn_score, 18, 0);
    spn_where = text(p, "", &ss_regular_16, 0xd6dee6);
    lv_obj_set_pos(spn_where, SPN_X + 19, SPN_Y + 44);

    spn_chip = box(p);
    lv_obj_set_size(spn_chip, 64, 36);
    lv_obj_set_pos(spn_chip, SPN_X + SL_SPOON_W - 64 - 14, SPN_Y + 10);
    lv_obj_set_style_radius(spn_chip, SL_R_FULL, 0);
    lv_obj_set_style_bg_opa(spn_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(spn_chip, 32, 0);
    lv_obj_set_style_transform_pivot_y(spn_chip, 18, 0);
    spn_chip_label = text(spn_chip, "x1", &ss_bold_28, 0xfffffe);
    lv_obj_center(spn_chip_label);

    spn_banner = spn_label(p, &ss_bold_28, 0xfffffe);
    lv_obj_set_width(spn_banner, SL_SPOON_W);
    lv_obj_set_pos(spn_banner, SPN_X, SPN_Y + 118);
    lv_obj_set_style_transform_pivot_x(spn_banner, SL_SPOON_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(spn_banner, 18, 0);
    spn_banner_sub = spn_label(p, &ss_semibold_18, 0xe8eef3);
    lv_obj_set_width(spn_banner_sub, SL_SPOON_W);
    lv_obj_set_pos(spn_banner_sub, SPN_X, SPN_Y + 156);

    for (int i = 0; i < SPN_POPS; i++)
    {
        spn_pop[i] = text(p, "", &ss_semibold_18, 0xffffff);
        lv_obj_add_flag(spn_pop[i], LV_OBJ_FLAG_HIDDEN);
    }
    spn_bubble = text(p, "", &ss_semibold_18, 0x1d1d1f);
    lv_obj_set_style_bg_color(spn_bubble, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(spn_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(spn_bubble, SL_R_MD, 0);
    lv_obj_set_style_pad_hor(spn_bubble, 12, 0);
    lv_obj_set_style_pad_ver(spn_bubble, 6, 0);
    lv_obj_add_flag(spn_bubble, LV_OBJ_FLAG_HIDDEN);

    /* the card: ready, paused and game over share it */
    spn_card = box(p);
    lv_obj_set_width(spn_card, 392);
    lv_obj_set_height(spn_card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(spn_card, 18, 0);
    lv_obj_set_style_pad_row(spn_card, 6, 0);
    lv_obj_set_style_radius(spn_card, SL_R_XL, 0);
    lv_obj_set_style_bg_color(spn_card, lv_color_hex(0x141821), 0);
    lv_obj_set_style_bg_opa(spn_card, 238, 0);
    lv_obj_set_style_border_width(spn_card, 2, 0);
    lv_obj_set_style_border_color(spn_card, lv_color_hex(0x2c3442), 0);
    lv_obj_set_style_shadow_width(spn_card, 30, 0);
    lv_obj_set_style_shadow_opa(spn_card, LV_OPA_50, 0);
    lv_obj_set_flex_flow(spn_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(spn_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_pos(spn_card, (W - 392) / 2, SPN_Y + 72);
    spn_card_title = spn_label(spn_card, &ss_bold_28, 0xfffffe);
    spn_card_quip = spn_label(spn_card, &ss_semibold_18, 0xd6dee6);
    spn_card_score = spn_label(spn_card, &ss_bold_56, 0xfffffe);
    spn_card_badge = box(spn_card);
    lv_obj_set_size(spn_card_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(spn_card_badge, 14, 0);
    lv_obj_set_style_pad_ver(spn_card_badge, 3, 0);
    lv_obj_set_style_radius(spn_card_badge, SL_R_MD, 0);
    lv_obj_set_style_bg_color(spn_card_badge, lv_color_hex(0xffd24a), 0);
    lv_obj_set_style_bg_opa(spn_card_badge, LV_OPA_COVER, 0);
    text(spn_card_badge, "NEW BEST!", &ss_semibold_18, 0x2a1f00);
    spn_card_lines = spn_label(spn_card, &ss_regular_18, 0xe8eef3);
    lv_obj_set_width(spn_card_lines, lv_pct(100));
    lv_obj_set_style_text_line_space(spn_card_lines, 3, 0);
    spn_card_tip = spn_label(spn_card, &ss_regular_16, 0x9fb0c0);
    lv_obj_set_width(spn_card_tip, lv_pct(100));
    spn_card_skin = box(spn_card);
    lv_obj_set_size(spn_card_skin, 300, 48);
    lv_obj_set_style_radius(spn_card_skin, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(spn_card_skin, lv_color_hex(0x252c38), 0);
    lv_obj_set_style_bg_color(spn_card_skin, lv_color_hex(0x323b4a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(spn_card_skin, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(spn_card_skin, 4, 0);
    lv_obj_center(text(spn_card_skin, "", &ss_semibold_18, 0xfffffe));
    on_click(spn_card_skin, spn_skin_cb, NULL);
    spn_card_go = box(spn_card);
    lv_obj_set_size(spn_card_go, 250, 60);
    lv_obj_set_style_margin_top(spn_card_go, 6, 0);
    lv_obj_set_style_radius(spn_card_go, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(spn_card_go, lv_color_hex(sl_accent()), 0);
    lv_obj_set_style_bg_opa(spn_card_go, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(spn_card_go, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(spn_card_go, 244, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(spn_card_go, 125, 0);
    lv_obj_set_style_transform_pivot_y(spn_card_go, 30, 0);
    lv_obj_center(text(spn_card_go, "Play again", &ss_semibold_23, sl_accent_ink() == 0xffffff ? 0xfffffe : sl_accent_ink()));
    on_click(spn_card_go, spn_again_cb, NULL);
    spn_card_hint = spn_label(spn_card, &ss_regular_16, 0x9fb0c0);
    lv_obj_set_style_text_font(spn_card_hint, &ss_regular_16, 0);

    /* two big arrows, one touch target: rock a thumb between them */
    spn_pad = box(p);
    lv_obj_set_pos(spn_pad, SPN_X, SPN_PAD_Y);
    lv_obj_set_size(spn_pad, SL_SPOON_W, SPN_PAD_H);
    lv_obj_add_flag(spn_pad, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1 | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(spn_pad, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(spn_pad, spn_pad_cb, LV_EVENT_ALL, NULL);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *a = box(spn_pad);
        spn_arrow[i] = a;
        lv_obj_set_size(a, SL_SPOON_W / 2 - 8, SPN_PAD_H);
        lv_obj_set_pos(a, i ? SL_SPOON_W / 2 + 8 : 0, 0);
        lv_obj_set_style_radius(a, 30, 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(a, 2, 0);
        lv_obj_set_style_shadow_width(a, 24, 0);
        lv_obj_set_style_shadow_color(a, lv_color_hex(sl_accent()), 0);
        lv_obj_set_style_transform_pivot_x(a, (SL_SPOON_W / 2 - 8) / 2, 0);
        lv_obj_set_style_transform_pivot_y(a, SPN_PAD_H / 2, 0);
        spn_arrow_glyph[i] = text(a, i ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT, &sl_sym_48, 0xffffff);
        lv_obj_center(spn_arrow_glyph[i]);
    }
    spn_pad_shown = 99;
    spn_last_tick = 0;
    spn_last_height = -1;
    spn_reset(false);
    spn_pad_look();
    spoon_timer = lv_timer_create(spoon_frame, 16, NULL);
    spn_draw(lv_tick_get(), 0);   /* the scene as it slides in, not a given-back (black) canvas */
    page_push(p);
    mini_update();
}
