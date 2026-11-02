/* Flappy Bird's art. GPLv2+.
 *
 * Every picture in the game is drawn here, in code: small maps of art
 * pixels typed out by hand (the bird, the hand, the digits, the buttons)
 * and a few shapes built from circles and runs (the clouds, the town, the
 * bushes, the medals). Nothing is taken or traced from the original game.
 *
 * One art pixel is FB_S x FB_S screen pixels, drawn nearest-neighbour, so
 * every edge is a crisp step on the R1. The world is FB_LW x FB_LH art
 * pixels: 480 x 800 divided by three (the last row is cut off at 800).
 *
 * Plain C over RGB565 buffers, with no LVGL, so the drawing can be run
 * and looked at on its own. */

#define FB_S      3
#define FB_SW     480                      /* the screen, in screen pixels */
#define FB_SH     800
#define FB_LW     160                      /* the world, in art pixels */
#define FB_LH     267
#define FB_GROUND 210                      /* the ground's top row */
#define FB_LIP_W  26                       /* a pipe: its lip, then its body */
#define FB_BODY_W 24
#define FB_LIP_H  12
#define FB_BIRD_W 17
#define FB_BIRD_H 12
#define FB_STRIPE 12                       /* the ground's stripes repeat every 12 art pixels */
#define FB_STRIPE_Y 212                    /* and fill rows 212 to 219 */
#define FB_STRIPE_H 8

#define FB_RGB(h) ((uint16_t)(((((h) >> 16) & 0xff) >> 3) << 11 | ((((h) >> 8) & 0xff) >> 2) << 5 | (((h) & 0xff) >> 3)))

struct fb_rect { int x0, y0, x1, y1; };     /* screen pixels, x1 and y1 not included */

static inline bool fb_rect_and(struct fb_rect *o, const struct fb_rect *a, const struct fb_rect *b)
{
    o->x0 = a->x0 > b->x0 ? a->x0 : b->x0;
    o->y0 = a->y0 > b->y0 ? a->y0 : b->y0;
    o->x1 = a->x1 < b->x1 ? a->x1 : b->x1;
    o->y1 = a->y1 < b->y1 ? a->y1 : b->y1;
    return o->x0 < o->x1 && o->y0 < o->y1;
}

/* ------------------------------------------------------------ palettes */
static const uint32_t fb_ink = 0x4a3640;   /* every outline: a warm near-black */

struct fb_sky {
    uint32_t sky, star, cloud, cloud_rim, town, town_rim, window, bush, bush_rim, bush_light;
};
static const struct fb_sky fb_skies[2] = {
    /* day: a clear teal sky over pale clouds and a sunlit town */
    { 0x50c2cd, 0x50c2cd, 0xe9f9df, 0xc4ead6, 0xd3efc6, 0xa6d8bc, 0xb6e2c8, 0x5fd16b, 0x3e9f4f, 0x86e283 },
    /* night: deep teal, a few stars, lit windows */
    { 0x0e6a7a, 0xdff3ee, 0x1b8590, 0x2c9ca5, 0x135a68, 0x0c4653, 0xf4dd78, 0x1d7444, 0x125530, 0x2a8d55 },
};

/* the ground, row by row from its top (FB_GROUND): an edge, a lit lip,
 * the stripes, a shadow, then the sand */
static const uint32_t fb_ground_rows[14] = {
    0x4a3640, 0xdaf39b, 0, 0, 0, 0, 0, 0, 0, 0, 0x5b9a2d, 0xc8b464, 0xe6da9c, 0xded693,
};
static const uint32_t fb_stripe_light = 0xa3e465, fb_stripe_dark = 0x78c33b, fb_sand = 0xded693;

/* a pipe's columns, left to right: an edge, a lit stripe, a broad face,
 * a dark band on the shaded side, an edge */
static const uint32_t fb_lip_cols[FB_LIP_W] = {
    0x4a3640, 0xd2f28e, 0x9ddf5c, 0x9ddf5c, 0xd2f28e, 0x9ddf5c, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a,
    0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x5c9e2a,
    0x79c33a, 0x5c9e2a, 0x5c9e2a, 0x4b8521, 0x4b8521, 0x4a3640,
};
static const uint32_t fb_body_cols[FB_BODY_W] = {
    0x4a3640, 0xd2f28e, 0x9ddf5c, 0x9ddf5c, 0xd2f28e, 0x9ddf5c, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a,
    0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x79c33a, 0x5c9e2a, 0x79c33a, 0x5c9e2a,
    0x5c9e2a, 0x4b8521, 0x4b8521, 0x4a3640,
};

/* ---------------------------------------------------------------- bird */
/* The bird, wing left out: K edge, L lit crown, Y body, B belly, W eye,
 * R upper beak, r lower beak. 17 x 12. */
static const char *const fb_bird_body[FB_BIRD_H] = {
    "......KKKKKK.....",
    "....KKLLLKWWWK...",
    "...KLLYYKWWWWWK..",
    "..KLYYYYKWWWWKWK.",
    ".KYYYYYYKWWWWKWK.",
    ".KYYYYYYYKWWWWWK.",
    "KYYYYYYYYYKKKKKK.",
    "KYYYYYYYYKRRRRRRK",
    "KYYYYYYYKKKKKKKK.",
    ".KBBBBBBBKrrrrrK.",
    "..KKBBBBBBKKKKK..",
    "....KKKKKKK......",
};
/* the wing in its three places, laid over the body ('.' keeps the body):
 * w feathers, v their shade */
struct fb_wing { int row; const char *rows[5]; };
static const struct fb_wing fb_wings[3] = {
    { 4, { "KKKK", "KwwwK", "KwwwwK", "KvwwvK", ".KKKK" } },          /* up */
    { 6, { "KKKKK", "KwwwwK", "KwwwwwK", ".KvvvK", "..KKK" } },       /* level */
    { 7, { "KKKKK", "KvwwwK", "KwwwwwK", ".KwwwK", "..KKK" } },       /* down */
};
/* three birds, as in the original: yellow, red, blue */
struct fb_feathers { uint32_t Y, L, B, R, r, w, v; };
static const struct fb_feathers fb_birds[3] = {
    { 0xf6c22f, 0xfde58a, 0xfbebb0, 0xf2602e, 0xd84a22, 0xfdfcf1, 0xe6e1c2 },
    { 0xe6503a, 0xf6a27a, 0xf6c9a8, 0xf8b43b, 0xe39a26, 0xfdfcf1, 0xead8c8 },
    { 0x4ea6d9, 0xa8ddf3, 0xd8eff6, 0xf2602e, 0xd84a22, 0xfdfcf1, 0xd4e3ea },
};
static uint16_t fb_bird_px[3][3][FB_BIRD_H][FB_BIRD_W];   /* colour, wing, row, column; 0 is clear */

static uint16_t fb_px565(uint32_t c) { uint16_t v = FB_RGB(c); return v ? v : 0x0020; }   /* 0 means clear */
static void fb_bird_build(void)
{
    for (int c = 0; c < 3; c++)
        for (int f = 0; f < 3; f++)
            for (int y = 0; y < FB_BIRD_H; y++)
                for (int x = 0; x < FB_BIRD_W; x++)
                {
                    char ch = fb_bird_body[y][x];
                    const struct fb_wing *wg = &fb_wings[f];
                    if (y >= wg->row && y < wg->row + 5 && x < (int)strlen(wg->rows[y - wg->row]) && wg->rows[y - wg->row][x] != '.')
                        ch = wg->rows[y - wg->row][x];
                    const struct fb_feathers *p = &fb_birds[c];
                    uint32_t col = ch == 'K' ? fb_ink : ch == 'L' ? p->L : ch == 'Y' ? p->Y : ch == 'B' ? p->B : ch == 'W' ? 0xffffff
                                 : ch == 'R' ? p->R : ch == 'r' ? p->r : ch == 'w' ? p->w : ch == 'v' ? p->v : 0xff000000u;
                    fb_bird_px[c][f][y][x] = col == 0xff000000u ? 0 : fb_px565(col);
                }
}

/* ------------------------------------------------------- small sprites */
/* a sprite: rows of characters and the colour of each character */
struct fb_sprite { int w, h; const char *const *rows; const char *keys; const uint32_t *cols; uint16_t *px; };   /* px: built once */

/* the hint on Get Ready: a hand, finger up, tapping */
static const char *const fb_hand_rows[] = {
    "...KK........",
    "..KWWK.......",
    "..KWWK.......",
    "..KWWK.......",
    "..KWWKKK.....",
    "..KWWKWWKK...",
    "KKKWWKWWKWKK.",
    "KWKWWWWWWKWWK",
    "KWWWWWWWWWWWK",
    "KWWWWWWWWWWWK",
    ".KWWWWWWWWWWK",
    ".KWWWWWWWWWK.",
    "..KWWWWWWWWK.",
    "..KWWWWWWWK..",
    "...KGGGGGGK..",
    "...KGGGGGGK..",
    "...KKKKKKKK..",
};
static const uint32_t fb_hand_cols[] = { 0x4a3640, 0xffffff, 0xf2a546 };
static uint16_t fb_hand_px[13 * 17];
static const struct fb_sprite fb_hand = { 13, 17, fb_hand_rows, "KWG", fb_hand_cols, fb_hand_px };

/* an arrow up, over the bird's ghost on Get Ready */
static const char *const fb_arrow_rows[] = {
    "...K...",
    "..KWK..",
    ".KWWWK.",
    "KWWWWWK",
    "KKKWKKK",
    "..KWK..",
    "..KWK..",
    "..KKK..",
};
static const uint32_t fb_arrow_cols[] = { 0x4a3640, 0xffffff };
static uint16_t fb_arrow_px[7 * 8];
static const struct fb_sprite fb_arrow = { 7, 8, fb_arrow_rows, "KW", fb_arrow_cols, fb_arrow_px };

/* the buttons: Play (on Game Over), and Pause and Play in flight */
static const char *const fb_play_rows[] = {
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    "KWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWK",
    "KWFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKKFFFFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKGKFFFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKGGKFFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKGGGKFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKGGGGKFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKGGGGGKFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKGGGGHKFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKGGGHKFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKGGHKFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKGHKFFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKHKFFFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFKKFFFFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFWK",
    "KWFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFWK",
    "KWSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSWK",
    "KWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWK",
    ".KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK.",
    "..DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD..",
};
static const uint32_t fb_play_cols[] = { 0x4a3640, 0xffffff, 0xf7f3e2, 0x5cc93a, 0x3f9a26, 0xe8dfc0, 0xa98f5c };
static uint16_t fb_play_px[40 * 22];
static const struct fb_sprite fb_play = { 40, 22, fb_play_rows, "KWFGHSD", fb_play_cols, fb_play_px };

static const char *const fb_pause_rows[] = {
    ".KKKKKKKKKKKK.",
    "KWWWWWWWWWWWWK",
    "KWFFFFFFFFFFWK",
    "KWFFKKFFKKFFWK",
    "KWFFKOKFKOKFWK",
    "KWFFKOKFKOKFWK",
    "KWFFKOKFKOKFWK",
    "KWFFKOKFKOKFWK",
    "KWFFKOKFKOKFWK",
    "KWFFKKKFKKKFWK",
    "KWFFFFFFFFFFWK",
    "KWWWWWWWWWWWWK",
    ".KKKKKKKKKKKK.",
    "..DDDDDDDDDD..",
};
static const uint32_t fb_pause_cols[] = { 0x4a3640, 0xffffff, 0xf7f3e2, 0xf08a32, 0xa98f5c };
static uint16_t fb_pause_px[14 * 14];
static const struct fb_sprite fb_pause = { 14, 14, fb_pause_rows, "KWFOD", fb_pause_cols, fb_pause_px };

/* ------------------------------------------------------------- digits */
/* the score's digits, 8 (the 1: 5) art pixels wide and 14 high, white in
 * a dark edge; drawn a stroke three pixels thick */
static const char *const fb_big[10][14] = {
    { ".######.", "########", "###..###", "###..###", "###..###", "###..###", "###..###", "###..###", "###..###", "###..###", "###..###", "###..###", "########", ".######." },
    { "..###", "#####", "#####", "..###", "..###", "..###", "..###", "..###", "..###", "..###", "..###", "..###", "..###", "..###" },
    { ".######.", "########", "###..###", "###..###", ".....###", "....####", "..#####.", ".#####..", "####....", "###.....", "###.....", "###.....", "########", "########" },
    { ".######.", "########", "###..###", ".....###", ".....###", ".....###", "..#####.", "..######", ".....###", ".....###", ".....###", "###..###", "########", ".######." },
    { "###..###", "###..###", "###..###", "###..###", "###..###", "###..###", "########", "########", ".....###", ".....###", ".....###", ".....###", ".....###", ".....###" },
    { "########", "########", "###.....", "###.....", "###.....", "#######.", "########", ".....###", ".....###", ".....###", ".....###", "###..###", "########", ".######." },
    { ".######.", "########", "###..###", "###.....", "###.....", "#######.", "########", "###..###", "###..###", "###..###", "###..###", "###..###", "########", ".######." },
    { "########", "########", ".....###", ".....###", "....###.", "....###.", "...###..", "...###..", "...###..", "..###...", "..###...", "..###...", "..###...", "..###..." },
    { ".######.", "########", "###..###", "###..###", "###..###", "###..###", ".######.", "########", "###..###", "###..###", "###..###", "###..###", "########", ".######." },
    { ".######.", "########", "###..###", "###..###", "###..###", "###..###", "########", ".#######", ".....###", ".....###", ".....###", "###..###", "########", ".######." },
};
/* the panel's digits, 5 x 7 (the 1: 3) */
static const char *const fb_small[10][7] = {
    { ".###.", "##.##", "##.##", "##.##", "##.##", "##.##", ".###." },
    { ".##", "###", ".##", ".##", ".##", ".##", ".##" },
    { ".###.", "##.##", "...##", "..##.", ".##..", "##...", "#####" },
    { "####.", "...##", "...##", ".###.", "...##", "...##", "####." },
    { "##.##", "##.##", "##.##", "#####", "...##", "...##", "...##" },
    { "#####", "##...", "####.", "...##", "...##", "##.##", ".###." },
    { ".###.", "##...", "####.", "##.##", "##.##", "##.##", ".###." },
    { "#####", "...##", "..##.", "..##.", ".##..", ".##..", ".##.." },
    { ".###.", "##.##", "##.##", ".###.", "##.##", "##.##", ".###." },
    { ".###.", "##.##", "##.##", ".####", "...##", "...##", ".###." },
};
static int fb_glyph_w(bool big, int d) { return (int)strlen(big ? fb_big[d][0] : fb_small[d][0]); }
static int fb_number_w(bool big, int n)
{
    char b[12];
    snprintf(b, sizeof b, "%d", n < 0 ? 0 : n);
    int w = 0;
    for (char *p = b; *p; p++) w += fb_glyph_w(big, *p - '0') + 1;   /* each digit in its edge, the edges shared */
    return w + 1;
}

/* ------------------------------------------------------------- medals */
struct fb_metal { uint32_t face, rim, light, dark; };
static const struct fb_metal fb_metals[4] = {
    { 0xd68d40, 0xa3622a, 0xf2bb7c, 0x7c4618 },   /* bronze, 10 */
    { 0xd5d9dd, 0x99a1a9, 0xf7f8f9, 0x6e767e },   /* silver, 20 */
    { 0xf2c23e, 0xc38f1c, 0xfde78c, 0x8f6510 },   /* gold, 30 */
    { 0xe3f2f6, 0xa3c4cc, 0xffffff, 0x7495a0 },   /* platinum, 40 */
};
#define FB_MEDAL 22
static uint16_t fb_medal_px[4][FB_MEDAL][FB_MEDAL];
static bool fb_in_star(const float *sx, const float *sy, int x, int y)
{
    bool in = false;
    for (int i = 0, j = 9; i < 10; j = i++)
        if ((sy[i] > y + 0.5f) != (sy[j] > y + 0.5f) &&
            x + 0.5f < (sx[j] - sx[i]) * (y + 0.5f - sy[i]) / (sy[j] - sy[i]) + sx[i]) in = !in;
    return in;
}
static void fb_medal_build(void)
{
    /* a five-pointed star, pressed into the middle */
    float sx[10], sy[10];
    for (int i = 0; i < 10; i++)
    {
        float a = -1.5707963f + i * 0.6283185f, r = i & 1 ? 3.4f : 7.6f;
        sx[i] = 11 + r * cosf(a);
        sy[i] = 11.6f + r * sinf(a);
    }
    for (int m = 0; m < 4; m++)
        for (int y = 0; y < FB_MEDAL; y++)
            for (int x = 0; x < FB_MEDAL; x++)
            {
                float dx = x + 0.5f - 11, dy = y + 0.5f - 11, d = sqrtf(dx * dx + dy * dy);
                uint32_t c;
                if (d > 11) { fb_medal_px[m][y][x] = 0; continue; }
                if (d > 10) c = fb_ink;
                else if (d > 8.4f) c = dx + dy < -7 ? fb_metals[m].light : fb_metals[m].rim;
                else
                {
                    c = fb_metals[m].face;
                    if (dx + dy < -10 && d > 7.4f) c = fb_metals[m].light;   /* the shine, top left */
                    /* the star raised: lit, with its shadow a step down and right */
                    if (fb_in_star(sx, sy, x, y)) c = fb_metals[m].light;
                    else if (fb_in_star(sx, sy, x - 1, y - 1)) c = fb_metals[m].dark;
                }
                fb_medal_px[m][y][x] = fb_px565(c);
            }
}
static int fb_medal_for(int score) { return score >= 40 ? 3 : score >= 30 ? 2 : score >= 20 ? 1 : score >= 10 ? 0 : -1; }

/* a glint over the medal, in five steps: a dot, a cross, a star, a cross, a dot */
static const char *const fb_glint[5][5] = {
    { ".....", ".....", "..W..", ".....", "....." },
    { ".....", "..W..", ".WWW.", "..W..", "....." },
    { "..W..", "..W..", "WWWWW", "..W..", "..W.." },
    { ".....", "..W..", ".WWW.", "..W..", "....." },
    { ".....", ".....", "..W..", ".....", "....." },
};

/* ---------------------------------------------------------- the panel */
#define FB_PANEL_W 116
#define FB_PANEL_H 58
static uint32_t fb_panel_at(int x, int y)
{
    int w = FB_PANEL_W, h = FB_PANEL_H;
    bool corner = (x < 2 && y < 2 && !(x == 1 && y == 1)) || (x >= w - 2 && y < 2 && !(x == w - 2 && y == 1)) ||
                  (x < 2 && y >= h - 2 && !(x == 1 && y == h - 2)) || (x >= w - 2 && y >= h - 2 && !(x == w - 2 && y == h - 2));
    if (corner) return 0xff000000u;
    if (x == 0 || y == 0 || x == w - 1 || y == h - 1 || (x == 1 && (y == 1 || y == h - 2)) || (x == w - 2 && (y == 1 || y == h - 2))) return fb_ink;
    if (y == h - 2 || x == w - 2) return 0xbdae5e;                       /* its lower and right lip, in shade */
    if (y == 1 || x == 1) return 0xfbf6cf;                               /* its upper and left lip, lit */
    if (y == h - 3) return 0xcfc173;
    /* the medal's place, a dish pressed in */
    float dx = x + 0.5f - 27, dy = y + 0.5f - 32, d = dx * dx + dy * dy;
    if (d <= 12.5f * 12.5f) return d > 11.2f * 11.2f ? (dy < 0 ? 0xc5b766 : 0xf4edbe) : 0xd3c77a;
    return 0xded693;
}
static uint16_t fb_panel_px[FB_PANEL_W * FB_PANEL_H];

/* ---------------------------------------------------------- drawing */
/* one art pixel as a FB_S block (or a block of s), cut to the clip */
static inline void fb_block(uint16_t *fb, const struct fb_rect *clip, int sx, int sy, int s, uint16_t c)
{
    int x0 = sx < clip->x0 ? clip->x0 : sx, x1 = sx + s > clip->x1 ? clip->x1 : sx + s;
    int y0 = sy < clip->y0 ? clip->y0 : sy, y1 = sy + s > clip->y1 ? clip->y1 : sy + s;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) fb[y * FB_SW + x] = c;
}
static inline uint16_t fb_blend(uint16_t a, uint16_t b, int t)   /* a toward b by t/256 */
{
    int r = (a >> 11) + (((b >> 11) - (a >> 11)) * t >> 8);
    int g = ((a >> 5) & 63) + ((((b >> 5) & 63) - ((a >> 5) & 63)) * t >> 8);
    int bl = (a & 31) + (((b & 31) - (a & 31)) * t >> 8);
    return (uint16_t)(r << 11 | g << 5 | bl);
}
/* a map of RGB565 (0 clear) at scale s, an art pixel at a time */
static void fb_map_draw(uint16_t *fb, const struct fb_rect *clip, const uint16_t *map, int w, int h, int sx, int sy, int s, int opa)
{
    struct fb_rect r = { sx, sy, sx + w * s, sy + h * s }, k;
    if (!fb_rect_and(&k, &r, clip)) return;
    int ax0 = (k.x0 - sx) / s, ax1 = (k.x1 - sx + s - 1) / s, ay0 = (k.y0 - sy) / s, ay1 = (k.y1 - sy + s - 1) / s;
    for (int ay = ay0; ay < ay1; ay++)
    {
        int y0 = sy + ay * s, y1 = y0 + s;
        if (y0 < k.y0) y0 = k.y0;
        if (y1 > k.y1) y1 = k.y1;
        const uint16_t *row = map + ay * w;
        for (int ax = ax0; ax < ax1; ax++)
        {
            uint16_t c = row[ax];
            if (!c) continue;
            int x0 = sx + ax * s, x1 = x0 + s;
            if (x0 < k.x0) x0 = k.x0;
            if (x1 > k.x1) x1 = k.x1;
            for (int y = y0; y < y1; y++)
            {
                uint16_t *d = fb + y * FB_SW;
                for (int x = x0; x < x1; x++) d[x] = opa >= 256 ? c : fb_blend(d[x], c, opa);
            }
        }
    }
}
/* a sprite at (sx, sy) screen pixels, at scale s; opa 0..256 */
static void fb_sprite_draw(uint16_t *fb, const struct fb_rect *clip, const struct fb_sprite *sp, int sx, int sy, int s, int opa)
{
    fb_map_draw(fb, clip, sp->px, sp->w, sp->h, sx, sy, s, opa);
}
/* the sprites and the panel as colours, once */
static void fb_sprites_build(void)
{
    const struct fb_sprite *all[] = { &fb_hand, &fb_arrow, &fb_play, &fb_pause };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++)
        for (int y = 0; y < all[i]->h; y++)
            for (int x = 0; x < all[i]->w; x++)
            {
                char ch = all[i]->rows[y][x];
                const char *p = ch == '.' ? NULL : strchr(all[i]->keys, ch);
                all[i]->px[y * all[i]->w + x] = p ? fb_px565(all[i]->cols[p - all[i]->keys]) : 0;
            }
    for (int y = 0; y < FB_PANEL_H; y++)
        for (int x = 0; x < FB_PANEL_W; x++)
        {
            uint32_t c = fb_panel_at(x, y);
            fb_panel_px[y * FB_PANEL_W + x] = c == 0xff000000u ? 0 : fb_px565(c);
        }
}
/* a number in the score's digits (big) or the panel's; x is where it
 * starts. White in a dark edge, a shadow step under the big ones. Each
 * number is laid out once, in art pixels, and kept: a few are on the
 * screen at a time, and they change far less often than they are drawn. */
#define FB_NUM_W 64
#define FB_NUM_H 18
struct fb_num { int8_t big; int n, w, h; uint16_t px[FB_NUM_W * FB_NUM_H]; };
static struct fb_num fb_nums[4];
static int fb_nums_next;
static const struct fb_num *fb_number_map(bool big, int n)
{
    if (n < 0) n = 0;
    for (int i = 0; i < 4; i++) if (fb_nums[i].w && fb_nums[i].big == big && fb_nums[i].n == n) return &fb_nums[i];
    struct fb_num *m = &fb_nums[fb_nums_next];
    fb_nums_next = (fb_nums_next + 1) & 3;
    char b[12];
    snprintf(b, sizeof b, "%d", n);
    int gh = big ? 14 : 7, w = fb_number_w(big, n), h = gh + (big ? 3 : 2);
    if (w > FB_NUM_W) w = FB_NUM_W;
    m->big = big; m->n = n; m->w = w; m->h = h;
    memset(m->px, 0, sizeof m->px);
    uint16_t white = FB_RGB(0xffffff), edge = FB_RGB(fb_ink);
    for (int pass = 0; pass < 2; pass++)   /* the edges, then the white over them */
    {
        int x = 0;
        for (char *p = b; *p; p++)
        {
            int d = *p - '0', gw = fb_glyph_w(big, d);
            for (int y = 0; y < gh; y++)
                for (int i = 0; i < gw; i++)
                {
                    if ((big ? fb_big[d][y][i] : fb_small[d][y][i]) != '#') continue;
                    int lx = x + i + 1, ly = y + 1;
                    if (pass)
                    {
                        if (lx < w) m->px[ly * FB_NUM_W + lx] = white;
                        continue;
                    }
                    for (int ey = -1; ey <= (big ? 2 : 1); ey++)
                        for (int ex = -1; ex <= 1; ex++)
                            if (lx + ex >= 0 && lx + ex < w && ly + ey < h) m->px[(ly + ey) * FB_NUM_W + lx + ex] = edge;
                }
            x += gw + 1;
        }
    }
    return m;
}
static void fb_number_draw(uint16_t *fb, const struct fb_rect *clip, bool big, int n, int sx, int sy, int opa)
{
    int gh = big ? 14 : 7;
    struct fb_rect all = { sx, sy, sx + fb_number_w(big, n) * FB_S + FB_S, sy + (gh + 3) * FB_S }, k;
    if (!fb_rect_and(&k, &all, clip)) return;
    const struct fb_num *m = fb_number_map(big, n);
    /* the map's rows are FB_NUM_W apart: drawn a row of art at a time */
    for (int y = 0; y < m->h; y++) fb_map_draw(fb, clip, m->px + y * FB_NUM_W, m->w, 1, sx, sy + y * FB_S, FB_S, opa);
}

/* ----------------------------------------------------- the background */
static uint32_t fb_hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
/* the sky, the clouds, the town and the bushes, and the still part of the
 * ground, in art pixels (FB_LW x FB_LH colours) */
static void fb_scene_build(uint32_t *art, int night)
{
    const struct fb_sky *k = &fb_skies[night];
    for (int i = 0; i < FB_LW * FB_LH; i++) art[i] = k->sky;
    if (night)   /* stars: a fixed few, some brighter */
        for (int i = 0; i < 34; i++)
        {
            uint32_t h = fb_hash(i * 7919 + 13);
            int x = h % FB_LW, y = 12 + (h >> 8) % 128;
            art[y * FB_LW + x] = (h >> 20) & 1 ? k->star : 0x7fc0c4;
        }
    /* the clouds: a row of puffs (circles) along a line, filled down */
    int top[FB_LW];
    for (int x = 0; x < FB_LW; x++) top[x] = FB_LH;
    for (int i = 0, cx = -4; cx < FB_LW + 8; i++)
    {
        uint32_t h = fb_hash(i * 31 + 7);
        int r = 6 + h % 6, cy = 168 - (int)((h >> 4) % 6);
        for (int x = cx - r; x <= cx + r; x++)
        {
            if (x < 0 || x >= FB_LW) continue;
            int dx = x - cx, t = cy - (int)sqrtf((float)(r * r - dx * dx));
            if (t < top[x]) top[x] = t;
        }
        cx += 7 + (int)((h >> 9) % 7);
    }
    for (int x = 0; x < FB_LW; x++)
        for (int y = top[x]; y < FB_GROUND; y++) art[y * FB_LW + x] = y == top[x] ? k->cloud_rim : k->cloud;
    /* the town: blocks of flats, each its own height, windows in rows */
    for (int i = 0, x0 = -3; x0 < FB_LW; i++)
    {
        uint32_t h = fb_hash(i * 131 + 3);
        int w = 8 + h % 9, ht = 10 + (h >> 5) % 17, t = 196 - ht;
        for (int x = x0; x < x0 + w; x++)
        {
            if (x < 0 || x >= FB_LW) continue;
            for (int y = t; y < FB_GROUND; y++)
            {
                bool edge = y == t || x == x0 || x == x0 + w - 1;
                bool win = !edge && y > t + 1 && (y - t) % 3 != 1 && (x - x0) % 3 != 0 && x < x0 + w - 2 &&
                           (!night || fb_hash(x * 977 + y * 31) % 5 != 0);
                art[y * FB_LW + x] = edge ? k->town_rim : win ? k->window : k->town;
            }
        }
        x0 += w - 1 + (int)((h >> 11) % 3);
    }
    /* the bushes: low puffs along the foot of the town */
    for (int x = 0; x < FB_LW; x++) top[x] = FB_LH;
    for (int i = 0, cx = -2; cx < FB_LW + 6; i++)
    {
        uint32_t h = fb_hash(i * 53 + 11);
        int r = 4 + h % 4, cy = 200 - (int)((h >> 3) % 3);
        for (int x = cx - r; x <= cx + r; x++)
        {
            if (x < 0 || x >= FB_LW) continue;
            int dx = x - cx, t = cy - (int)sqrtf((float)(r * r - dx * dx));
            if (t < top[x]) top[x] = t;
        }
        cx += 5 + (int)((h >> 7) % 5);
    }
    for (int x = 0; x < FB_LW; x++)
        for (int y = top[x]; y < FB_GROUND; y++)
            art[y * FB_LW + x] = y == top[x] ? k->bush_rim : y == top[x] + 1 ? k->bush_light : k->bush;
    /* the ground, but for its stripes (they move) */
    for (int y = FB_GROUND; y < FB_LH; y++)
        for (int x = 0; x < FB_LW; x++)
        {
            int r = y - FB_GROUND;
            art[y * FB_LW + x] = r < 14 ? (fb_ground_rows[r] ? fb_ground_rows[r] : fb_stripe_dark) : fb_sand;
        }
}
/* the scene at three times its size, into a screen buffer */
static void fb_scene_blow_up(uint16_t *bg, const uint32_t *art)
{
    for (int y = 0; y < FB_SH; y++)
    {
        const uint32_t *row = art + (y / FB_S) * FB_LW;
        uint16_t *d = bg + y * FB_SW;
        for (int x = 0; x < FB_SW; x++) d[x] = FB_RGB(row[x / FB_S]);
    }
}
/* the stripes, one strip a period wider than the screen; drawn from an
 * offset (0 to FB_STRIPE*FB_S - 1) they slide */
#define FB_STRIPE_SW (FB_SW + FB_STRIPE * FB_S)
static void fb_stripes_build(uint16_t *strip)
{
    for (int y = 0; y < FB_STRIPE_H * FB_S; y++)
        for (int x = 0; x < FB_STRIPE_SW; x++)
        {
            int lx = x / FB_S, ly = y / FB_S, k = ((lx + ly) % FB_STRIPE + FB_STRIPE) % FB_STRIPE;
            strip[y * FB_STRIPE_SW + x] = FB_RGB(k < FB_STRIPE / 2 ? fb_stripe_light : fb_stripe_dark);
        }
}
/* a pipe's two rows (body and lip), in screen pixels */
static uint16_t fb_body_row[FB_BODY_W * FB_S], fb_lip_px[FB_LIP_H * FB_S][FB_LIP_W * FB_S];
static void fb_pipe_build(void)
{
    for (int x = 0; x < FB_BODY_W * FB_S; x++) fb_body_row[x] = FB_RGB(fb_body_cols[x / FB_S]);
    for (int y = 0; y < FB_LIP_H * FB_S; y++)
        for (int x = 0; x < FB_LIP_W * FB_S; x++)
        {
            int ly = y / FB_S, lx = x / FB_S;
            uint32_t c = fb_lip_cols[lx];
            if (ly == 0 || ly == FB_LIP_H - 1) c = fb_ink;
            else if (ly == 1 && lx > 0 && lx < FB_LIP_W - 1) c = c == 0x79c33a ? 0x9ddf5c : c == 0x9ddf5c ? 0xd2f28e : c;   /* lit along the top */
            else if (ly == FB_LIP_H - 2 && lx > 0 && lx < FB_LIP_W - 1) c = 0x5c9e2a;   /* and shaded along the bottom */
            fb_lip_px[y][x] = FB_RGB(c);
        }
}
/* a pipe pair: its lip's left edge at screen x, the gap's top and bottom
 * rows (screen); drawn from the top and the ground into the gap */
static void fb_pipe_draw(uint16_t *fb, const struct fb_rect *clip, int sx, int gap_top, int gap_bot, int ground)
{
    int lw = FB_LIP_W * FB_S, bw = FB_BODY_W * FB_S, lh = FB_LIP_H * FB_S, bx = sx + FB_S;
    for (int part = 0; part < 2; part++)
    {
        int y0 = part ? gap_bot : 0, y1 = part ? ground : gap_top;
        int ly0 = part ? gap_bot : gap_top - lh;          /* the lip, at the gap */
        struct fb_rect r = { sx, y0, sx + lw, y1 }, k;
        if (!fb_rect_and(&k, &r, clip)) continue;
        for (int y = k.y0; y < k.y1; y++)
        {
            uint16_t *d = fb + y * FB_SW;
            if (y >= ly0 && y < ly0 + lh)
            {
                const uint16_t *s = fb_lip_px[y - ly0];
                memcpy(d + k.x0, s + (k.x0 - sx), (size_t)(k.x1 - k.x0) * 2);
            }
            else
            {
                int a = k.x0 > bx ? k.x0 : bx, b = k.x1 < bx + bw ? k.x1 : bx + bw;
                if (a < b) memcpy(d + a, fb_body_row + (a - bx), (size_t)(b - a) * 2);
            }
        }
    }
}

/* ------------------------------------------------- the bird, turned */
/* The bird turned by ang (radians, clockwise: nose down) about its middle
 * (cx, cy, screen pixels), into a small image. Level, every screen pixel
 * lies in one art pixel and takes it as it is: crisp. Turned, each screen
 * pixel takes four samples, nearest-neighbour, and keeps their mean and
 * how many hit the bird, so the outline turns as a clean line instead of
 * breaking into steps. The sampling steps along each row in fixed point.
 * The same image answers the collisions: a pixel at least half covered is
 * the bird, so what hits is what is drawn. */
#define FB_BC 72                           /* the image's side, screen pixels */
struct fb_bird_img { struct fb_rect r; uint16_t c[FB_BC * FB_BC]; uint8_t n[FB_BC * FB_BC]; };
static struct fb_rect fb_bird_bounds(float cx, float cy, float ang)
{
    float c = fabsf(cosf(ang)), s = fabsf(sinf(ang));
    float ex = c * FB_BIRD_W * FB_S / 2 + s * FB_BIRD_H * FB_S / 2, ey = s * FB_BIRD_W * FB_S / 2 + c * FB_BIRD_H * FB_S / 2;
    int icy = (int)lroundf(cy);
    struct fb_rect r = { (int)floorf(cx - ex) - 1, (int)floorf(icy - ey) - 1, (int)ceilf(cx + ex) + 1, (int)ceilf(icy + ey) + 1 };
    return r;
}
static void fb_bird_render(struct fb_bird_img *b, float cx, float cy, float ang, const uint16_t (*map)[FB_BIRD_W])
{
    b->r = fb_bird_bounds(cx, cy, ang);
    cy = (float)lroundf(cy);               /* on a whole pixel: level, the art's rows fall on the screen's */
    int w = b->r.x1 - b->r.x0, h = b->r.y1 - b->r.y0;
    if (w > FB_BC) w = FB_BC;
    if (h > FB_BC) h = FB_BC;
    b->r.x1 = b->r.x0 + w; b->r.y1 = b->r.y0 + h;
    float c = cosf(ang) / FB_S, s = sinf(ang) / FB_S;
    int32_t dux = (int32_t)lroundf(c * 65536), dvx = (int32_t)lroundf(-s * 65536);
    int32_t hux = dux / 2, hvx = dvx / 2, huy = (int32_t)lroundf(s * 32768), hvy = (int32_t)lroundf(c * 32768);
    for (int y = 0; y < h; y++)
    {
        float dx = b->r.x0 + 0.25f - cx, dy = b->r.y0 + y + 0.25f - cy;
        int32_t u = (int32_t)lroundf((dx * c + dy * s + FB_BIRD_W / 2.0f) * 65536);
        int32_t v = (int32_t)lroundf((-dx * s + dy * c + FB_BIRD_H / 2.0f) * 65536);
        uint16_t *oc = b->c + y * FB_BC;
        uint8_t *on = b->n + y * FB_BC;
        for (int x = 0; x < w; x++, u += dux, v += dvx)
        {
            int rr = 0, gg = 0, bb = 0, n = 0;
            for (int q = 0; q < 4; q++)
            {
                int32_t su = u + (q & 1 ? hux : 0) + (q & 2 ? huy : 0), sv = v + (q & 1 ? hvx : 0) + (q & 2 ? hvy : 0);
                if (su < 0 || sv < 0 || su >= (FB_BIRD_W << 16) || sv >= (FB_BIRD_H << 16)) continue;
                uint16_t k = map[sv >> 16][su >> 16];
                if (!k) continue;
                rr += k >> 11; gg += (k >> 5) & 63; bb += k & 31; n++;
            }
            on[x] = (uint8_t)n;
            oc[x] = n ? (uint16_t)((rr / n) << 11 | (gg / n) << 5 | (bb / n)) : 0;
        }
    }
}
static void fb_bird_draw(uint16_t *fb, const struct fb_rect *clip, const struct fb_bird_img *b)
{
    struct fb_rect k;
    if (!fb_rect_and(&k, &b->r, clip)) return;
    for (int y = k.y0; y < k.y1; y++)
    {
        const uint16_t *c = b->c + (y - b->r.y0) * FB_BC - b->r.x0;
        const uint8_t *n = b->n + (y - b->r.y0) * FB_BC - b->r.x0;
        uint16_t *d = fb + y * FB_SW;
        for (int x = k.x0; x < k.x1; x++)
            if (n[x] == 4) d[x] = c[x];
            else if (n[x]) d[x] = fb_blend(d[x], c[x], n[x] * 64);
    }
}
static bool fb_bird_hits(const struct fb_bird_img *b, const struct fb_rect *box)
{
    struct fb_rect k;
    if (!fb_rect_and(&k, &b->r, box)) return false;
    for (int y = k.y0; y < k.y1; y++)
    {
        const uint8_t *n = b->n + (y - b->r.y0) * FB_BC - b->r.x0;
        for (int x = k.x0; x < k.x1; x++) if (n[x] >= 2) return true;
    }
    return false;
}
