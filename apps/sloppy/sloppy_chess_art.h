/* Chess pieces drawn from signed distance fields. Each piece is a few
 * shapes stacked back to front; every shape gets its own dark outline and
 * a bevel lit from the top left, so the parts read like a turned wooden
 * set. Rendered once, at any size, into RGB565A8 sprites (the colour
 * plane, then the alpha plane: LVGL draws the same buffer as an image).
 * Pure C, no UI. GPLv2+. */
#ifndef SLOPPY_CHESS_ART_H
#define SLOPPY_CHESS_ART_H

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* ---- distance primitives, in units of the square (0..1, y down) */
static inline float ca_len(float x, float y) { return sqrtf(x * x + y * y); }
static inline float ca_min(float a, float b) { return a < b ? a : b; }
static inline float ca_max(float a, float b) { return a > b ? a : b; }
static inline float ca_clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static inline float ca_circle(float x, float y, float cx, float cy, float r)
{
    return ca_len(x - cx, y - cy) - r;
}

static inline float ca_box(float x, float y, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(x - cx) - hw + r, qy = fabsf(y - cy) - hh + r;
    return ca_len(ca_max(qx, 0), ca_max(qy, 0)) + ca_min(ca_max(qx, qy), 0) - r;
}

static inline float ca_ellipse(float x, float y, float cx, float cy, float rx, float ry)
{
    float px = (x - cx) / rx, py = (y - cy) / ry;
    float k0 = ca_len(px, py), k1 = ca_len(px / rx, py / ry);
    return k1 > 0 ? k0 * (k0 - 1) / k1 : -ca_min(rx, ry);
}

static inline float ca_capsule(float x, float y, float ax, float ay, float bx, float by, float r)
{
    float px = x - ax, py = y - ay, ex = bx - ax, ey = by - ay;
    float t = ca_clamp01((px * ex + py * ey) / (ex * ex + ey * ey));
    return ca_len(px - ex * t, py - ey * t) - r;
}

/* v holds n (x, y) pairs */
static inline float ca_poly(float x, float y, const float *v, int n)
{
    float d = (x - v[0]) * (x - v[0]) + (y - v[1]) * (y - v[1]), s = 1;
    for (int i = 0, j = n - 1; i < n; j = i, i++)
    {
        float ex = v[2 * j] - v[2 * i], ey = v[2 * j + 1] - v[2 * i + 1];
        float wx = x - v[2 * i], wy = y - v[2 * i + 1];
        float t = ca_clamp01((wx * ex + wy * ey) / (ex * ex + ey * ey));
        float bx = wx - ex * t, by = wy - ey * t;
        if (bx * bx + by * by < d) d = bx * bx + by * by;
        bool c1 = y >= v[2 * i + 1], c2 = y < v[2 * j + 1], c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    }
    return s * sqrtf(d);
}

static inline float ca_smin(float a, float b, float k)
{
    float h = ca_max(k - fabsf(a - b), 0) / k;
    return ca_min(a, b) - h * h * k * 0.25f;
}

/* The foot every piece stands on: a wide plinth and a thinner step. */
static inline float ca_foot(float x, float y, int part, float hw)
{
    return part == 0 ? ca_box(x, y, 0.5f, 0.855f, hw, 0.042f, 0.03f)
                     : ca_box(x, y, 0.5f, 0.792f, hw - 0.05f, 0.03f, 0.022f);
}

enum { CA_PARTS_MAX = 6 };
static const uint8_t ca_parts[7] = { 0, 4, 4, 5, 5, 5, 5 };

/* Part `part` of piece `type` (back to front), or the ink details
 * (part = -1: eyes, slits, lines) drawn in the outline colour. */
static inline float ca_shape(int type, int part, float x, float y)
{
    switch (type) {
    case 1: /* pawn */
        if (part == 0) return ca_foot(x, y, 0, 0.25f);
        if (part == 1)
        {
            static const float body[] = { 0.42f, 0.5f, 0.58f, 0.5f, 0.6f, 0.6f, 0.65f, 0.69f, 0.71f, 0.78f,
                                          0.29f, 0.78f, 0.35f, 0.69f, 0.4f, 0.6f };
            return ca_poly(x, y, body, 8) - 0.012f;
        }
        if (part == 2) return ca_box(x, y, 0.5f, 0.495f, 0.135f, 0.026f, 0.024f);
        if (part == 3) return ca_circle(x, y, 0.5f, 0.335f, 0.122f);
        return 1;
    case 2: /* knight, facing left */
        if (part == 0) return ca_foot(x, y, 0, 0.27f);
        if (part == 1) return ca_foot(x, y, 1, 0.27f);
        if (part == 2)
        {
            static const float head[] = {
                0.75f, 0.77f, 0.77f, 0.6f, 0.74f, 0.44f, 0.67f, 0.31f, 0.57f, 0.22f,
                0.53f, 0.12f, 0.47f, 0.2f, 0.4f, 0.22f, 0.31f, 0.3f, 0.22f, 0.41f,
                0.165f, 0.49f, 0.18f, 0.56f, 0.24f, 0.585f, 0.31f, 0.555f, 0.39f, 0.52f,
                0.44f, 0.54f, 0.36f, 0.64f, 0.31f, 0.77f };
            return ca_poly(x, y, head, 18) - 0.014f;
        }
        if (part == 3)    /* the mane, a ridge down the back of the neck */
        {
            static const float mane[] = { 0.56f, 0.2f, 0.66f, 0.29f, 0.75f, 0.43f, 0.79f, 0.6f, 0.77f, 0.77f,
                                          0.7f, 0.77f, 0.71f, 0.6f, 0.68f, 0.45f, 0.6f, 0.32f, 0.53f, 0.25f };
            return ca_poly(x, y, mane, 10) - 0.01f;
        }
        if (part == -1)
            return ca_min(ca_ellipse(x, y, 0.405f, 0.315f, 0.032f, 0.026f),
                          ca_circle(x, y, 0.215f, 0.49f, 0.017f));
        return 1;
    case 3: /* bishop */
        if (part == 0) return ca_foot(x, y, 0, 0.25f);
        if (part == 1)
        {
            static const float skirt[] = { 0.4f, 0.6f, 0.6f, 0.6f, 0.63f, 0.69f, 0.69f, 0.78f, 0.31f, 0.78f, 0.37f, 0.69f };
            return ca_poly(x, y, skirt, 6) - 0.012f;
        }
        if (part == 2) return ca_box(x, y, 0.5f, 0.595f, 0.15f, 0.027f, 0.025f);
        if (part == 3)
            return ca_smin(ca_ellipse(x, y, 0.5f, 0.43f, 0.145f, 0.165f),
                           ca_ellipse(x, y, 0.5f, 0.31f, 0.06f, 0.1f), 0.06f);
        if (part == 4) return ca_circle(x, y, 0.5f, 0.185f, 0.045f);
        if (part == -1)
            return ca_min(ca_capsule(x, y, 0.505f, 0.355f, 0.585f, 0.45f, 0.013f),
                          ca_capsule(x, y, 0.47f, 0.53f, 0.53f, 0.53f, 0.009f));
        return 1;
    case 4: /* rook */
        if (part == 0) return ca_foot(x, y, 0, 0.28f);
        if (part == 1) return ca_foot(x, y, 1, 0.28f);
        if (part == 2)
        {
            static const float body[] = { 0.335f, 0.38f, 0.665f, 0.38f, 0.69f, 0.77f, 0.31f, 0.77f };
            return ca_poly(x, y, body, 4) - 0.01f;
        }
        if (part == 3)
        {
            float top = ca_box(x, y, 0.5f, 0.27f, 0.215f, 0.095f, 0.02f);
            for (int i = 0; i < 2; i++)
                top = ca_max(top, -ca_box(x, y, i ? 0.6f : 0.4f, 0.2f, 0.035f, 0.06f, 0.008f));
            return top;
        }
        if (part == 4) return ca_box(x, y, 0.5f, 0.375f, 0.19f, 0.024f, 0.02f);
        return 1;
    case 5: /* queen */
        if (part == 0) return ca_foot(x, y, 0, 0.28f);
        if (part == 1)
        {
            static const float body[] = { 0.37f, 0.53f, 0.63f, 0.53f, 0.7f, 0.78f, 0.3f, 0.78f };
            return ca_poly(x, y, body, 4) - 0.012f;
        }
        if (part == 2)
        {
            static const float crown[] = { 0.3f, 0.56f, 0.19f, 0.3f, 0.3f, 0.41f, 0.33f, 0.235f, 0.425f, 0.39f,
                                           0.5f, 0.205f, 0.575f, 0.39f, 0.67f, 0.235f, 0.7f, 0.41f, 0.81f, 0.3f,
                                           0.7f, 0.56f };
            return ca_poly(x, y, crown, 11) - 0.012f;
        }
        if (part == 3) return ca_box(x, y, 0.5f, 0.56f, 0.2f, 0.03f, 0.028f);
        if (part == 4)
        {
            static const float tip[5][2] = { { 0.19f, 0.29f }, { 0.33f, 0.225f }, { 0.5f, 0.19f },
                                             { 0.67f, 0.225f }, { 0.81f, 0.29f } };
            float d = 1;
            for (int i = 0; i < 5; i++) d = ca_min(d, ca_circle(x, y, tip[i][0], tip[i][1], 0.043f));
            return d;
        }
        return 1;
    case 6: /* king */
        if (part == 0) return ca_foot(x, y, 0, 0.28f);
        if (part == 1)
        {
            static const float body[] = { 0.36f, 0.53f, 0.64f, 0.53f, 0.7f, 0.78f, 0.3f, 0.78f };
            return ca_poly(x, y, body, 4) - 0.012f;
        }
        if (part == 2)    /* the cross, behind the crown */
            return ca_min(ca_box(x, y, 0.5f, 0.205f, 0.024f, 0.09f, 0.01f),
                          ca_box(x, y, 0.5f, 0.19f, 0.075f, 0.024f, 0.01f));
        if (part == 3)
        {
            float lobes = ca_smin(ca_circle(x, y, 0.36f, 0.43f, 0.13f), ca_circle(x, y, 0.64f, 0.43f, 0.13f), 0.08f);
            lobes = ca_smin(lobes, ca_ellipse(x, y, 0.5f, 0.38f, 0.075f, 0.1f), 0.05f);
            return ca_max(lobes, y - 0.55f);
        }
        if (part == 4) return ca_box(x, y, 0.5f, 0.56f, 0.2f, 0.03f, 0.028f);
        return 1;
    }
    return 1;
}

static inline uint16_t ca_565(float r, float g, float b)
{
    int R = (int)(ca_clamp01(r) * 31 + 0.5f), G = (int)(ca_clamp01(g) * 63 + 0.5f), B = (int)(ca_clamp01(b) * 31 + 0.5f);
    return (uint16_t)(R << 11 | G << 5 | B);
}

struct ca_ink { float fill_top[3], fill_bottom[3], line[3], detail[3], light, dark; };

/* White ivory and ebony. */
static const struct ca_ink ca_inks[2] = {
    { { 1.0f, 0.99f, 0.96f }, { 0.87f, 0.82f, 0.74f }, { 0.13f, 0.11f, 0.1f }, { 0.2f, 0.17f, 0.15f }, 0.12f, 0.3f },
    { { 0.33f, 0.31f, 0.29f }, { 0.13f, 0.12f, 0.12f }, { 0.04f, 0.035f, 0.03f }, { 0.75f, 0.71f, 0.65f }, 0.42f, 0.25f },
};

/* Renders piece type (1..6) at s x s pixels into white[] and black[]
 * (each s*s*3 bytes, RGB565A8), turned by `turn` radians about the
 * middle of its foot (a king lying down: +-pi/2). The light stays where
 * it is. Returns false when out of memory. */
static inline bool sl_chess_art_render_turned(int type, int s, uint8_t *white, uint8_t *black, float turn)
{
    float tc = cosf(turn), ts = sinf(turn);
    int nparts = ca_parts[type];
    int n = s * s;
    float *grid = malloc(sizeof(float) * (size_t)n * (size_t)(nparts + 2));
    if (!grid) return false;
    float *uni = grid + (size_t)n * (size_t)nparts, *ink = uni + n;
    float inv = 1.0f / (float)s;
    /* A part is a small piece of the square, and a distance only falls by
     * a pixel a pixel: from a pixel `d` away from a part, the next d - far
     * along the row are still more than `far` from it, and are given that
     * much (a floor under the real distance) without being worked out.
     * Nothing below reads a distance past `far` for what it is: an edge
     * and its bevel want half a pixel and the pixels beside it, the drop
     * shadow `soft`. Worked out at every pixel, the set took 0.66 s on the
     * R1 each time Chess opened (a test R1's log, Nov 12). */
    float far = s * 0.05f + 0.5f + 2.5f;
    for (int i = 0; i < n; i++) uni[i] = 1e9f;
    for (int p = -1; p < nparts; p++)
    {
        float *out = p < 0 ? ink : grid + (size_t)p * n;
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++)
            {
                /* a little larger than the unit drawing: the pieces fill the square */
                float u = 0.5f + ((x + 0.5f) * inv - 0.5f) / 1.07f, v = 0.506f + ((y + 0.5f) * inv - 0.5f) / 1.07f;
                if (turn != 0)
                {
                    /* lying down: about the middle of the piece, then settled onto the floor */
                    float du = u - 0.5f, dv = v - 0.5f - (fabsf(ts) * 0.2f);
                    u = 0.5f + du * tc + dv * ts; v = 0.5f - du * ts + dv * tc;
                }
                float d = ca_shape(type, p, u, v) * (float)s;
                out[y * s + x] = d;
                if (p >= 0 && d < uni[y * s + x]) uni[y * s + x] = d;
                for (float left = d - 1; left > far && x + 1 < s; left -= 1)
                {
                    x++;
                    out[y * s + x] = left;
                    if (p >= 0 && left < uni[y * s + x]) uni[y * s + x] = left;
                }
            }
    }
    float line = s * 0.032f;              /* outline width, px */
    if (line < 1.05f) line = 1.05f;
    float bevel = s * 0.1f;
    int so = (int)(s * 0.035f + 0.5f);     /* drop shadow offset */
    float soft = s * 0.05f + 0.5f;
    /* The edge, the bevel and the light of a part are the same for both
     * colours: worked out once a pixel, then inked twice. A pixel no part
     * covers (most of the square) has only the shadow to take. */
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++)
        {
            int i = y * s + x;
            float v = (y + 0.5f) * inv;
            float r[2] = { 0, 0 }, g[2] = { 0, 0 }, b[2] = { 0, 0 }, a[2] = { 0, 0 };
            for (int p = 0; p < nparts && uni[i] < 0.5f; p++)
            {
                const float *gp = grid + p * n;
                float d = gp[i], cov = ca_clamp01(0.5f - d);
                if (cov <= 0) continue;
                float gx = gp[y * s + (x < s - 1 ? x + 1 : x)] - gp[y * s + (x ? x - 1 : x)];
                float gy = gp[(y < s - 1 ? y + 1 : y) * s + x] - gp[(y ? y - 1 : y) * s + x];
                float gl = ca_len(gx, gy);
                float lit = gl > 0 ? (-0.55f * gx - 0.83f * gy) / gl : 0;
                float depth = -(d + line);
                float edge = depth > 0 ? 1 - ca_clamp01(depth / bevel) : 1;
                edge *= edge;
                float t = ca_clamp01((v - 0.15f) / 0.75f);
                float in = ca_clamp01(0.5f - (d + line));
                for (int c = 0; c < 2; c++)
                {
                    const struct ca_ink *k = &ca_inks[c];
                    float fr = k->fill_top[0] + (k->fill_bottom[0] - k->fill_top[0]) * t;
                    float fg = k->fill_top[1] + (k->fill_bottom[1] - k->fill_top[1]) * t;
                    float fb = k->fill_top[2] + (k->fill_bottom[2] - k->fill_top[2]) * t;
                    float shade = lit > 0 ? edge * lit * k->light : edge * lit * k->dark;
                    if (shade > 0) { fr += (1 - fr) * shade; fg += (1 - fg) * shade; fb += (1 - fb) * shade; }
                    else { fr *= 1 + shade; fg *= 1 + shade; fb *= 1 + shade; }
                    float pr = k->line[0] + (fr - k->line[0]) * in;
                    float pg = k->line[1] + (fg - k->line[1]) * in;
                    float pb = k->line[2] + (fb - k->line[2]) * in;
                    r[c] = pr * cov + r[c] * (1 - cov); g[c] = pg * cov + g[c] * (1 - cov);
                    b[c] = pb * cov + b[c] * (1 - cov); a[c] = cov + a[c] * (1 - cov);
                }
            }
            /* a soft shadow under the piece, down and to the right */
            int sx = x - so / 2, sy = y - so;
            float sd = sx >= 0 && sy >= 0 ? uni[sy * s + sx] : 1e3f;
            for (int c = 0; c < 2; c++)
            {
                const struct ca_ink *k = &ca_inks[c];
                float ic = ca_clamp01(0.5f - ink[i] + 0.25f) * a[c];
                if (ic > 0)
                {
                    r[c] += (k->detail[0] * a[c] - r[c]) * ic; g[c] += (k->detail[1] * a[c] - g[c]) * ic;
                    b[c] += (k->detail[2] * a[c] - b[c]) * ic;
                }
                float sh = ca_clamp01((soft - sd) / (2 * soft)) * 0.32f * (1 - a[c]);
                float ta = a[c] + sh;
                uint8_t *out = c ? black : white;
                uint16_t col = ta > 0 ? ca_565(r[c] / ta, g[c] / ta, b[c] / ta) : 0;
                out[2 * i] = (uint8_t)(col & 255); out[2 * i + 1] = (uint8_t)(col >> 8);
                out[2 * n + i] = (uint8_t)(ca_clamp01(ta) * 255 + 0.5f);
            }
        }
    free(grid);
    return true;
}

static inline bool sl_chess_art_render(int type, int s, uint8_t *white, uint8_t *black)
{
    return sl_chess_art_render_turned(type, s, white, black, 0);
}

#endif
