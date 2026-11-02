/*
 *  Visualizer: the music painting the screen, as MilkDrop paints a
 *  computer's, in software for a player with no GPU. Opened from Now
 *  Playing (the button beside the turntable's) or the apps list.
 *
 *  Each frame starts from the last. The picture is read back through a
 *  motion field (zoom, turn, swirl, a slow sinusoidal warp, drift), a
 *  little darker, and the music is drawn into it again: the wave, a ring
 *  on the beat, stars. Feedback does the rest: trails, tunnels, smoke.
 *
 *  The picture is 240 x 400, a quarter of the screen, at 8 bits a channel:
 *  RGB565 can't hold a 2% fade, so slow trails would stop or band. The
 *  motion is worked out on a grid of 8 px cells (31 x 51 points) and each
 *  pixel's source is interpolated across its cell in 16.16 fixed point;
 *  the read is bilinear with the fade after it. No float, division or
 *  table per pixel. Painting is straight into LVGL's buffer (the panel
 *  itself on the R1): the picture doubled with linear smoothing and
 *  dithered to RGB565. No canvas, no second full-screen copy, and nothing
 *  under it is drawn. Should the first frames take longer than 24 ms (the
 *  simulator puts the R1 near 20), the picture halves to 120 x 200, four
 *  times up, for the rest of the session.
 *
 *  Looks are presets, C structs of motion, fade, colour and wave. A change
 *  blends one into the next: the motion and colours mix, the old wave
 *  fades out as the new one comes in, a mirror folds in or out. They move
 *  on by themselves every half minute or so, sooner on a strong beat, and
 *  with a flick.
 *
 *  And it plays under a finger. A drag stirs: the picture near it follows
 *  the finger and curls round it, and the finger leaves a glowing trail
 *  that the motion carries off. A tap throws a ripple of light out from
 *  where it lands, larger on a beat. A hold is a black hole: it draws the
 *  picture in, turning, until let go, when it all bursts out again. All of
 *  it is in the motion grid and the overlays, round the finger only. A
 *  quick sideways flick is the next look; a tap near the top shows the
 *  chrome and its close button, and a pull down from there closes.
 *
 *  The samples and the FFT are the Spectrum tile's: its hook is set only
 *  while this is on screen, and the tile gives the analyzer up meanwhile.
 *  Twelve log-spaced bands make bass, mids and treble, each against its
 *  own recent average as MilkDrop measures them; a beat is the bass
 *  jumping well over its average. Paused, the picture settles for two
 *  seconds and then stops being drawn. What a frame costs goes to the log
 *  when it closes, and frames are spaced so drawing takes at most 3/4 of
 *  the processor; the warp yields between strips. The decoder must keep
 *  its share.
 */

#define VZ_W      240                    /* the picture at most: a quarter of the screen, doubled up to it */
#define VZ_H      400
#define VZ_CELL   8                      /* the motion grid's cells, px; VZ_CS is its shift */
#define VZ_CS     3
#define VZ_GW     (VZ_W / VZ_CELL)
#define VZ_GH     (VZ_H / VZ_CELL)
#define VZ_GP     ((VZ_GW + 1) * (VZ_GH + 1))
#define VZ_SLOW   24000                  /* us of work and paint a frame past which the picture halves */
#define VZ_BANDS  12
#define VZ_WAVE   192                    /* points on the wave */
#define VZ_LAG    24                     /* the knot: a sample against this many later */
#define VZ_SIN    1024
#define VZ_FRAME  33                     /* ms between frames: 30 a second when the player keeps up */
#define VZ_FRAME_BT 66                   /* 15 a second while music goes out over Bluetooth */
#define VZ_SETTLE 60                     /* frames drawn after the music stops */
#define VZ_LOOKS  10
#define VZ_STARS  64
#define VZ_BURSTS 4
#define VZ_TOP    140                    /* px: the band a tap shows the chrome from, a pull down closes from */
#define VZ_HOLD   400                    /* ms still before a press is a hold */
#define VZ_FLICK  320                    /* ms at most for a flick */

enum { VZ_NONE, VZ_RING, VZ_LINE, VZ_SCOPE, VZ_KNOT, VZ_PETALS, VZ_WAVES };

/* A look. Motion is per frame at 30 a second (other rates scale it), in
 * half widths of the screen. */
struct vz_look
{
    const char *name;
    float zoom, zoom_r;                  /* over 1 flies in; more of it towards the edges */
    float rot, swirl;                    /* turn, radians a frame; more of it near the middle */
    float warp, warp_speed, warp_scale;  /* MilkDrop's warp: a slow, wobbling flow */
    float dx, dy, cy;                    /* drift; the middle, below the centre */
    float sx, sy;                        /* stretch */
    float decay, tint;                   /* of the picture, left each frame; green's share of it */
    float hue, hue_speed, sat;           /* the wave's colour: turns, turns a second */
    float wave_r, wave_amp;              /* the wave's place and size */
    float beat_zoom, beat_rot;           /* what a beat adds to the motion */
    float fold_x, fold_y;                /* mirrored: left to right, then top to bottom */
    float stars, rings, glow, hue2;      /* stars from the middle, a ring a beat, a glow with the bass, its hue */
    int wave;
};

static const struct vz_look vz_looks[VZ_LOOKS] = {
    /* a slow flight through violet smoke, the wave a ring */
    { .name = "Nebula", .zoom = 1.022f, .rot = 0.006f, .swirl = 0.012f, .warp = 1.0f, .warp_speed = 0.9f,
      .warp_scale = 1.0f, .decay = 0.966f, .tint = 0.985f, .hue = 0.74f, .hue_speed = 0.012f, .sat = 0.75f,
      .wave = VZ_RING, .wave_r = 0.42f, .wave_amp = 0.16f, .beat_zoom = 0.025f, .rings = 0.7f,
      .glow = 0.3f, .hue2 = 0.1f },
    /* the scope down the middle, wound into a whirlpool */
    { .name = "Vortex", .zoom = 1.008f, .swirl = 0.14f, .warp = 0.5f, .warp_speed = 1.0f, .warp_scale = 1.0f,
      .decay = 0.974f, .tint = 0.97f, .hue = 0.5f, .hue_speed = 0.02f, .sat = 0.8f,
      .wave = VZ_LINE, .wave_amp = 0.35f, .beat_rot = 0.05f, .glow = 0.15f, .hue2 = 0.08f },
    /* four mirrored quarters of a turning knot */
    { .name = "Kaleidoscope", .zoom = 1.012f, .rot = 0.02f, .swirl = 0.03f, .warp = 1.6f, .warp_speed = 1.1f,
      .warp_scale = 0.8f, .decay = 0.97f, .tint = 0.99f, .hue = 0.08f, .hue_speed = 0.05f, .sat = 0.9f,
      .wave = VZ_KNOT, .wave_amp = 0.8f, .fold_x = 1, .fold_y = 1, .beat_zoom = 0.02f, .rings = 0.6f,
      .glow = 0.2f, .hue2 = 0.33f },
    /* stars streaming past, a ring thrown out on the beat */
    { .name = "Hyperspace", .zoom = 1.075f, .zoom_r = 0.04f, .rot = 0.002f, .warp_speed = 1.0f, .warp_scale = 1.0f,
      .decay = 0.88f, .tint = 1.0f, .hue = 0.6f, .hue_speed = 0.008f, .sat = 0.5f,
      .wave = VZ_RING, .wave_r = 0.1f, .wave_amp = 0.03f, .beat_zoom = 0.04f, .stars = 1, .rings = 0.45f },
    /* colour flowing on its own, the bands as petals */
    { .name = "Plasma", .zoom = 1.01f, .rot = 0.004f, .warp = 4.0f, .warp_speed = 1.4f, .warp_scale = 0.7f,
      .decay = 0.985f, .tint = 0.975f, .hue = 0.02f, .hue_speed = 0.015f, .sat = 0.85f,
      .wave = VZ_PETALS, .wave_r = 0.6f, .beat_zoom = 0.01f, .glow = 0.4f, .hue2 = 0.5f },
    /* faster at the edges than the middle: a tunnel */
    { .name = "Tunnel", .zoom = 1.035f, .zoom_r = 0.06f, .rot = 0.012f, .warp = 0.3f, .warp_speed = 1.0f,
      .warp_scale = 1.0f, .decay = 0.948f, .tint = 0.96f, .hue = 0.88f, .hue_speed = 0.03f, .sat = 0.85f,
      .wave = VZ_RING, .wave_r = 0.22f, .wave_amp = 0.12f, .beat_zoom = 0.05f, .rings = 1 },
    /* a mirrored scope rising like mist off water */
    { .name = "Lake", .zoom = 1.0f, .dy = 0.016f, .warp = 2.5f, .warp_speed = 0.8f, .warp_scale = 1.0f,
      .decay = 0.988f, .tint = 0.985f, .hue = 0.45f, .hue_speed = 0.01f, .sat = 0.7f,
      .wave = VZ_SCOPE, .wave_r = 0.9f, .wave_amp = 0.3f, .fold_x = 1 },
    /* a ring of fire drawn in to the middle */
    { .name = "Ember", .zoom = 0.972f, .rot = -0.01f, .swirl = 0.02f, .warp = 0.8f, .warp_speed = 1.0f,
      .warp_scale = 1.0f, .decay = 0.975f, .tint = 0.92f, .hue = 0.06f, .hue_speed = 0.004f, .sat = 0.95f,
      .wave = VZ_RING, .wave_r = 0.85f, .wave_amp = 0.2f, .glow = 0.4f, .hue2 = 0.04f },
    /* the scope smeared into arms by a turning sky */
    { .name = "Galaxy", .zoom = 0.985f, .rot = 0.035f, .swirl = 0.05f, .warp = 0.6f, .warp_speed = 1.0f,
      .warp_scale = 1.0f, .decay = 0.982f, .tint = 1.0f, .hue = 0.58f, .hue_speed = 0.03f, .sat = 0.6f,
      .wave = VZ_SCOPE, .wave_amp = 0.3f, .glow = 0.25f, .hue2 = 0.1f },
    /* curtains of light rising from a scope at the foot */
    { .name = "Aurora", .zoom = 1.0f, .dy = 0.012f, .warp = 3.0f, .warp_speed = 1.0f, .warp_scale = 1.6f,
      .decay = 0.988f, .tint = 0.995f, .hue = 0.36f, .hue_speed = 0.02f, .sat = 0.75f,
      .wave = VZ_SCOPE, .wave_r = 1.25f, .wave_amp = 0.25f, .fold_x = 1 },
};

static struct
{
    bool active, settled, sb_shown;
    int w, h, ups, gw, gh;               /* the picture, and its grid; 1 << ups screen px a pixel */
    float s;                             /* the looks' unit of length: half its width */
    lv_obj_t *home, *scr, *pic, *top, *bottom, *name, *hint, *title, *artist, *dots[VZ_LOOKS];
    lv_timer_t *timer, *hide;
    uint32_t *buf[2];                    /* this picture and the last, 0x00RRGGBB */
    int cur;
    /* blending from 'from' (a snapshot) to look 'to' since t0, over dur ms */
    struct vz_look from, now;
    float from_wave[VZ_WAVES], now_wave[VZ_WAVES];
    int to;
    uint32_t t0, dur, next_ms, last_ms, seed;
    float time, ph[8];                   /* seconds shown; the warp's phases */
    /* the music */
    float lvl[VZ_BANDS], loud, avg[3], rel[3], att[3], agc, beat;
    bool beat_now;
    uint32_t beat_ms, beats;
    int16_t wave[VZ_WAVE + VZ_LAG];      /* the latest window, -4096 to 4096 */
    float star_a[VZ_STARS], star_r[VZ_STARS];   /* where each star is: angle, and out from the middle */
    int quiet;
    char song[MAX_PATH];
    lv_point_t down;
    bool moved;
    /* the finger: on the screen; in the picture (folded into what moves),
     * where it was and how fast it goes (px a frame), how it turns; how
     * hard it stirs, and pulls in (a hold) or pushes out (let go) */
    struct
    {
        bool down, hold, fresh;
        lv_point_t at;
        uint32_t down_ms;
        float x, y, px, py, vx, vy, spin, stir, pull;
    } fing;
    struct { float x, y, age, power; uint32_t c; } burst[VZ_BURSTS];   /* ripples, age 0 to 1 */
    /* the meter */
    struct cfm_hist work, paint, gap;
    uint64_t opened_us, last_us, paint_us;
    uint32_t frames, changes;
} vz;
static int32_t vz_gu[VZ_GP], vz_gv[VZ_GP];      /* where each grid point reads from, 16.16 px */
static float vz_sin[VZ_SIN];
static uint16_t vz_rows[4][W] __attribute__((aligned(4)));   /* two rows grown, halfway, and a quarter way */
static int vz_row_k[3];

/* The picture's size: full (240 x 400, doubled up to the screen) or, on a
 * player too slow for that, half (120 x 200, four times up). */
static bool vz_low;                      /* this player was too slow for the full one: half, this session */
static bool vz_hinted;                   /* the gestures were shown, this session */
static void vz_size(bool low)
{
    vz.ups = low ? 2 : 1;
    vz.w = W >> vz.ups;
    vz.h = H >> vz.ups;
    vz.gw = vz.w / VZ_CELL;
    vz.gh = vz.h / VZ_CELL;
    vz.s = vz.w / 2.0f;
}

static uint32_t vz_rand(void)
{
    vz.seed = vz.seed * 1664525u + 1013904223u;
    return vz.seed >> 8;
}
static float vz_sinr(float a) { return vz_sin[(int)(a * (VZ_SIN / 6.2831853f)) & (VZ_SIN - 1)]; }
static float vz_cosr(float a) { return vz_sinr(a + 1.5707963f); }
static float vz_frac(float v) { return v - floorf(v); }

static uint32_t vz_hsv(float h, float s, float v)
{
    h = vz_frac(h) * 6;
    int i = (int)h;
    float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f)), r, g, b;
    switch (i)
    {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(b * 255);
}

/* ------------------------------------------------------------ the music */
/* The bands, the beat and the wave, for this frame. False with no music. */
static bool vz_hear(float dt, uint32_t now)
{
    float db[SP_BARS], p[3] = { 0, 0, 0 };
    bool music = sp_analyze(db);
    if (music)
    {
        float loud = -120;
        for (int b = 0; b < VZ_BANDS; b++) if (db[b] > loud) loud = db[b];
        vz.loud = loud > vz.loud ? loud : vz.loud - 3 * dt;
        if (!(vz.loud > -30 && vz.loud < 100)) vz.loud = -30;   /* NaN too */
        for (int b = 0; b < VZ_BANDS; b++)
        {
            float v = (db[b] - vz.loud + 42) / 42;
            v = v > 1 ? 1 : v > 0 ? v : 0;
            vz.lvl[b] += (v - vz.lvl[b]) * (v > vz.lvl[b] ? 0.6f : 0.25f);
            p[b * 3 / VZ_BANDS] += db[b] > -110 ? (float)pow(10, db[b] / 10) : 0;
        }
    }
    else
        for (int b = 0; b < VZ_BANDS; b++) vz.lvl[b] *= 0.8f;
    /* each against its own last few seconds: about 1 is usual, 2 is a lot */
    for (int i = 0; i < 3; i++)
    {
        if (!(p[i] >= 0 && p[i] < 1e30f)) p[i] = 0;
        if (music) vz.avg[i] = vz.avg[i] > 0 ? vz.avg[i] + (p[i] - vz.avg[i]) * (dt / 3) : p[i];
        float r = music && vz.avg[i] > 1e-12f ? p[i] / vz.avg[i] : 0;
        r = r < 4 ? r : 4;
        vz.att[i] += (r - vz.att[i]) * (r > vz.att[i] ? 0.5f : 0.08f);
        if (!(vz.att[i] >= 0 && vz.att[i] <= 4)) vz.att[i] = 0;
        vz.rel[i] = r;
    }
    vz.beat -= dt * 2.5f;
    if (vz.beat < 0) vz.beat = 0;
    vz.beat_now = music && vz.rel[0] > 1.45f && now - vz.beat_ms > 280;
    if (vz.beat_now)
    {
        float s = (vz.rel[0] - 1.2f) / 1.3f;
        vz.beat = s > 1 ? 1 : s;
        vz.beat_ms = now;
        vz.beats++;
    }
    if (!music)
    {
        for (int i = 0; i < VZ_WAVE + VZ_LAG; i++) vz.wave[i] = (int16_t)(vz.wave[i] * 3 / 4);
        return false;
    }
    /* the wave: pairs of samples, from a rising zero crossing so it holds
     * still on a steady note, scaled to the loudest of late */
    int16_t m[VZ_WAVE + VZ_LAG + 64];
    int n = VZ_WAVE + VZ_LAG + 64, peak = 1;
    uint32_t at = sp_end - 2 * n;
    for (int i = 0; i < n; i++, at += 2)
        m[i] = (int16_t)((sp_ring[at & (SP_RING - 1)] + sp_ring[(at + 1) & (SP_RING - 1)]) >> 1);
    for (int pass = 0; pass < 2; pass++)  /* smoothed, as MilkDrop's waves are: a line, not a comb */
        for (int i = 0, prev = m[0]; i < n - 1; i++)
        {
            int v = m[i];
            m[i] = (int16_t)((prev + 2 * v + m[i + 1]) >> 2);
            prev = v;
        }
    int start = 0;
    for (int i = 0; i < 63; i++)
        if (m[i] <= 0 && m[i + 1] > 0) { start = i; break; }
    for (int i = 0; i < VZ_WAVE + VZ_LAG; i++)
    {
        int v = m[start + i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    vz.agc = peak > vz.agc * 0.96f ? peak : vz.agc * 0.96f;
    if (vz.agc < 1500) vz.agc = 1500;     /* hiss is not a wave */
    float k = 4096 / vz.agc;
    for (int i = 0; i < VZ_WAVE + VZ_LAG; i++) vz.wave[i] = (int16_t)(m[start + i] * k);
    return true;
}

/* ------------------------------------------------------------ the motion */
/* Where each grid point of the next picture reads the last one from:
 * columns 0 to gw, rows 0 to gh. f is this frame in 30ths of a second. */
static void vz_grid(const struct vz_look *k, float f, int gw, int gh)
{
    float z = (float)pow(k->zoom + k->beat_zoom * vz.beat, f), zr = k->zoom_r * f;
    float rot = (k->rot + k->beat_rot * vz.beat) * f, sw = k->swirl * f;
    float wa = k->warp * 0.007f * f, wi = 1 / (k->warp_scale > 0.1f ? k->warp_scale : 0.1f);
    float f0 = 11.68f + 4 * vz_cosr(vz.ph[4]), f1 = 8.77f + 3 * vz_cosr(vz.ph[5]);
    float f2 = 10.54f + 3 * vz_cosr(vz.ph[6]), f3 = 11.49f + 4 * vz_cosr(vz.ph[7]);
    float isx = 1 / (1 + k->sx), isy = 1 / (1 + k->sy), cx = vz.w / 2.0f, cy = vz.h / 2.0f + k->cy * vz.s;
    /* the finger, in px: a stir pushes the picture its way and curls it,
     * a pull draws it in (out when negative) and turns it; both fade to
     * nothing over a radius, wider for a pull */
    const float tx = vz.fing.x, ty = vz.fing.y, stir = vz.fing.stir, pull = vz.fing.pull;
    bool touch = stir > 0.01f || pull > 0.01f || pull < -0.01f;
    float pr = pull < 0 ? -pull : pull, tr = vz.s * (0.4f + 0.3f * (pr < 1 ? pr : 1)), ir2 = 1 / (tr * tr);
    float push_x = stir * vz.fing.vx * 1.2f * f, push_y = stir * vz.fing.vy * 1.2f * f;
    float speed = sqrtf(vz.fing.vx * vz.fing.vx + vz.fing.vy * vz.fing.vy);
    float curl = stir * (0.5f * vz.fing.spin + 0.012f * speed) * f + 0.07f * pull * f, sink = 0.14f * pull * f;
    curl = curl > 0.4f ? 0.4f : curl < -0.4f ? -0.4f : curl;
    for (int gy = 0; gy <= gh; gy++)
        for (int gx = 0; gx <= gw; gx++)
        {
            float x = (gx * VZ_CELL - cx) / vz.s, y = (gy * VZ_CELL - cy) / vz.s, r2 = x * x + y * y;
            /* a small turn: its sine and cosine as series, exact enough
             * and finer than a table */
            float a = rot + sw / (1 + 4 * r2);
            a = a > 0.5f ? 0.5f : a < -0.5f ? -0.5f : a;
            float a2 = a * a, s = a * (1 - a2 / 6), c = 1 - a2 / 2 + a2 * a2 / 24;
            float iz = 1 / (z + zr * sqrtf(r2));
            float u = (x * c - y * s) * iz * isx, v = (x * s + y * c) * iz * isy;
            u += wa * (vz_sinr(vz.ph[0] + wi * (x * f0 - y * f3)) + vz_cosr(vz.ph[2] - wi * (x * f1 - y * f2)));
            v += wa * (vz_cosr(vz.ph[1] - wi * (x * f2 + y * f1)) + vz_sinr(vz.ph[3] + wi * (x * f0 + y * f3)));
            float U = cx + (u + k->dx * f) * vz.s, V = cy + (v + k->dy * f) * vz.s;
            if (touch)
            {
                float ddx = gx * VZ_CELL - tx, ddy = gy * VZ_CELL - ty, d2 = (ddx * ddx + ddy * ddy) * ir2;
                if (d2 < 1)
                {
                    float q = (1 - d2) * (1 - d2), sk = sink * q > -0.5f ? sink * q : -0.5f;
                    U += -push_x * q - curl * q * ddy + sk * ddx;
                    V += -push_y * q + curl * q * ddx + sk * ddy;
                }
            }
            /* in the picture, a pixel short of its far edges (the read
             * takes the next one too) and a hair inside the near ones (the
             * steps across a cell round down by up to 7/65536). Between two
             * points in range every pixel is, so the inner loop never checks. */
            U = !(U > 0.001f) ? 0.001f : U < vz.w - 1.004f ? U : vz.w - 1.004f;
            V = !(V > 0.001f) ? 0.001f : V < vz.h - 1.004f ? V : vz.h - 1.004f;
            vz_gu[gy * (VZ_GW + 1) + gx] = (int32_t)(U * 65536);
            vz_gv[gy * (VZ_GW + 1) + gx] = (int32_t)(V * 65536);
        }
}

/* Grid rows gy0 to gy1, cells 0 to gw: each new pixel is the old picture
 * read where its cell says, between four pixels, then faded: red and blue
 * by d_rb, green by d_g, of 256. */
static void vz_warp(const uint32_t *src, uint32_t *dst, int gy0, int gy1, int gw, uint32_t d_rb, uint32_t d_g)
{
    const int w = vz.w;
    for (int gy = gy0; gy < gy1; gy++)
    {
        const int32_t *u0 = vz_gu + gy * (VZ_GW + 1), *v0 = vz_gv + gy * (VZ_GW + 1);
        for (int j = 0; j < VZ_CELL; j++)
        {
            uint32_t *d = dst + ((gy << VZ_CS) + j) * w;
            for (int gx = 0; gx < gw; gx++)
            {
                int32_t ul = u0[gx] + ((u0[gx + VZ_GW + 1] - u0[gx]) * j >> VZ_CS);
                int32_t vl = v0[gx] + ((v0[gx + VZ_GW + 1] - v0[gx]) * j >> VZ_CS);
                int32_t ur = u0[gx + 1] + ((u0[gx + VZ_GW + 2] - u0[gx + 1]) * j >> VZ_CS);
                int32_t vr = v0[gx + 1] + ((v0[gx + VZ_GW + 2] - v0[gx + 1]) * j >> VZ_CS);
                int32_t du = (ur - ul) >> VZ_CS, dv = (vr - vl) >> VZ_CS;
                for (int i = 0; i < VZ_CELL; i++, ul += du, vl += dv)
                {
                    const uint32_t *s = src + (vl >> 16) * w + (ul >> 16);
                    uint32_t fx = (uint32_t)ul >> 8 & 255, fy = (uint32_t)vl >> 8 & 255;
                    uint32_t w11 = fx * fy >> 8, w01 = fx - w11, w10 = fy - w11, w00 = 256 - fx - fy + w11;
                    uint32_t a = s[0], b = s[1], c = s[w], e = s[w + 1];
                    uint32_t rb = ((a & 0xff00ff) * w00 + (b & 0xff00ff) * w01 +
                                   (c & 0xff00ff) * w10 + (e & 0xff00ff) * w11) >> 8 & 0xff00ff;
                    uint32_t g = ((a & 0xff00) * w00 + (b & 0xff00) * w01 +
                                  (c & 0xff00) * w10 + (e & 0xff00) * w11) >> 8 & 0xff00;
                    *d++ = (rb * d_rb >> 8 & 0xff00ff) | (g * d_g >> 8 & 0xff00);
                }
            }
        }
    }
}

/* t of 256 of the way from a to b */
static uint32_t vz_mix(uint32_t a, uint32_t b, uint32_t t)
{
    return (((a & 0xff00ff) * (256 - t) + (b & 0xff00ff) * t) >> 8 & 0xff00ff) |
           (((a & 0xff00) * (256 - t) + (b & 0xff00) * t) >> 8 & 0xff00);
}
/* The mirrors: the left half over the right, then the top over the
 * bottom, fx and fy of 256 of the way. */
static void vz_fold(uint32_t *p, uint32_t fx, uint32_t fy)
{
    for (int y = 0; fx && y < (fy == 256 ? vz.h / 2 : vz.h); y++)
    {
        uint32_t *row = p + y * vz.w;
        for (int x = vz.w / 2; x < vz.w; x++)
            row[x] = fx == 256 ? row[vz.w - 1 - x] : vz_mix(row[x], row[vz.w - 1 - x], fx);
    }
    for (int y = vz.h / 2; fy && y < vz.h; y++)
    {
        uint32_t *row = p + y * vz.w, *from = p + (vz.h - 1 - y) * vz.w;
        if (fy == 256) memcpy(row, from, vz.w * 4);
        else for (int x = 0; x < vz.w; x++) row[x] = vz_mix(row[x], from[x], fy);
    }
}

/* ------------------------------------------------------------ the drawing */
/* Light added to a pixel, w of 256 of colour c. Channels stop short of
 * full, so the dither in vz_row never carries into the next one. */
static void vz_light(int x, int y, uint32_t c, int w)
{
    uint32_t *p = vz.buf[vz.cur] + y * vz.w + x, v = *p;
    int r = (int)(v >> 16 & 255) + ((int)(c >> 16 & 255) * w >> 8);
    int g = (int)(v >> 8 & 255) + ((int)(c >> 8 & 255) * w >> 8);
    int b = (int)(v & 255) + ((int)(c & 255) * w >> 8);
    *p = (uint32_t)(r < 248 ? r : 248) << 16 | (uint32_t)(g < 252 ? g : 252) << 8 | (uint32_t)(b < 248 ? b : 248);
}
/* A point between pixels, in 1/256 px, shared out over the four it covers. */
static void vz_dot(int32_t x8, int32_t y8, uint32_t c, int w)
{
    int x = x8 >> 8, y = y8 >> 8, fx = x8 & 255, fy = y8 & 255;
    if (x < 0 || y < 0 || x >= vz.w - 1 || y >= vz.h - 1) return;
    vz_light(x, y, c, ((256 - fx) * (256 - fy) >> 8) * w >> 8);
    vz_light(x + 1, y, c, (fx * (256 - fy) >> 8) * w >> 8);
    vz_light(x, y + 1, c, ((256 - fx) * fy >> 8) * w >> 8);
    vz_light(x + 1, y + 1, c, (fx * fy >> 8) * w >> 8);
}
static bool vz_sane(float v) { return v > -2000 && v < 2000; }   /* NaN too */
static void vz_line(float x0, float y0, float x1, float y1, uint32_t c, int w)
{
    if (!vz_sane(x0) || !vz_sane(y0) || !vz_sane(x1) || !vz_sane(y1)) return;
    float dx = x1 - x0, dy = y1 - y0, len = fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy);
    int n = (int)len + 1;
    int32_t x = (int32_t)(x0 * 256), y = (int32_t)(y0 * 256);
    int32_t sx = (int32_t)(dx * 256 / n), sy = (int32_t)(dy * 256 / n);
    /* on the full picture, two pixels wide: as thick on the screen, and as
     * much light in the trails, as on the half one */
    int32_t ox = vz.ups == 1 && fabsf(dx) <= fabsf(dy) ? 256 : 0, oy = vz.ups == 1 && !ox ? 256 : 0;
    for (int i = 0; i < n; i++, x += sx, y += sy)
    {
        vz_dot(x, y, c, w);
        if (vz.ups == 1) vz_dot(x + ox, y + oy, c, w);
    }
}
/* A soft round glow, brightest in its middle. */
static void vz_disc(float cx, float cy, float r, uint32_t c, int w)
{
    if (!vz_sane(cx) || !vz_sane(cy) || !(r >= 0.7f && r < 120)) return;
    int x0 = (int)(cx - r), x1 = (int)(cx + r) + 1, y0 = (int)(cy - r), y1 = (int)(cy + r) + 1;
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > vz.w ? vz.w : x1; y1 = y1 > vz.h ? vz.h : y1;
    int32_t X = (int32_t)(cx * 16), Y = (int32_t)(cy * 16), R2 = (int32_t)(r * r * 256), k = (256 << 16) / R2;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            int32_t ddx = x * 16 + 8 - X, ddy = y * 16 + 8 - Y, d2 = ddx * ddx + ddy * ddy;
            if (d2 >= R2) continue;
            int t = (R2 - d2) * k >> 16;           /* 0 at the rim, 256 in the middle */
            vz_light(x, y, c, (t * t >> 8) * w >> 8);
        }
}
/* A soft round shadow: light taken away, up to w of 256 in its middle. */
static void vz_dark(float cx, float cy, float r, int w)
{
    if (!vz_sane(cx) || !vz_sane(cy) || !(r >= 0.7f && r < 120)) return;
    int x0 = (int)(cx - r), x1 = (int)(cx + r) + 1, y0 = (int)(cy - r), y1 = (int)(cy + r) + 1;
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > vz.w ? vz.w : x1; y1 = y1 > vz.h ? vz.h : y1;
    int32_t X = (int32_t)(cx * 16), Y = (int32_t)(cy * 16), R2 = (int32_t)(r * r * 256), k = (256 << 16) / R2;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            int32_t ddx = x * 16 + 8 - X, ddy = y * 16 + 8 - Y, d2 = ddx * ddx + ddy * ddy;
            if (d2 >= R2) continue;
            uint32_t t = (uint32_t)((R2 - d2) * k >> 16), keep = 256 - (t * (uint32_t)w >> 8);
            uint32_t *p = vz.buf[vz.cur] + y * vz.w + x, v = *p;
            *p = ((v & 0xff00ff) * keep >> 8 & 0xff00ff) | ((v & 0xff00) * keep >> 8 & 0xff00);
        }
}
static void vz_ring(float cx, float cy, float r, uint32_t c, int w)
{
    float px = cx + r, py = cy;
    for (int i = 1; i <= 64; i++)
    {
        float a = i * (6.2831853f / 64), x = cx + r * vz_cosr(a), y = cy + r * vz_sinr(a);
        vz_line(px, py, x, y, c, w);
        px = x; py = y;
    }
}

/* One wave, w of 256 bright. */
static void vz_draw_wave(int mode, const struct vz_look *k, uint32_t c, int w)
{
    float cx = vz.w / 2.0f, cy = vz.h / 2.0f + k->cy * vz.s, amp = k->wave_amp * vz.s / 4096;
    const int16_t *s = vz.wave;
    float px = 0, py = 0;
    switch (mode)
    {
        case VZ_RING:
        {
            /* the ring breathes with the bass; its last points bend back
             * to the first so it closes without a step */
            float r = k->wave_r * vz.s * (1 + 0.12f * vz.att[0]), turn = vz.time * 0.25f;
            for (int i = 0; i <= VZ_WAVE; i++)
            {
                int j = i % VZ_WAVE;
                float v = s[j];
                if (j > VZ_WAVE - 32) v += (s[0] - v) * (j - (VZ_WAVE - 32)) / 32.0f;
                float a = turn + j * (6.2831853f / VZ_WAVE), rr = r + v * amp;
                float x = cx + rr * vz_cosr(a), y = cy + rr * vz_sinr(a);
                if (i) vz_line(px, py, x, y, c, w);
                px = x; py = y;
            }
            break;
        }
        case VZ_LINE:                    /* down the middle */
            for (int i = 0; i < VZ_WAVE; i++)
            {
                float x = cx + s[i] * amp, y = vz.h * (0.06f + 0.88f * i / (VZ_WAVE - 1));
                if (i) vz_line(px, py, x, y, c, w);
                px = x; py = y;
            }
            break;
        case VZ_SCOPE:                   /* across, wave_r below the middle */
            for (int i = 0; i < VZ_WAVE; i++)
            {
                float x = vz.w * (0.04f + 0.92f * i / (VZ_WAVE - 1)), y = cy + k->wave_r * vz.s + s[i] * amp;
                if (i) vz_line(px, py, x, y, c, w);
                px = x; py = y;
            }
            break;
        case VZ_KNOT:
        {
            /* each sample against a later one: a steady note an ellipse,
             * a chord a knot; the whole turning slowly */
            float a = vz.time * 0.4f, ca = vz_cosr(a), sa = vz_sinr(a);
            for (int i = 0; i < VZ_WAVE; i++)
            {
                float u = s[i] * amp, v = s[i + VZ_LAG] * amp;
                float x = cx + u * ca - v * sa, y = cy + u * sa + v * ca;
                if (i) vz_line(px, py, x, y, c, w);
                px = x; py = y;
            }
            break;
        }
        case VZ_PETALS:
        {
            /* the bands round a circle, low to high and back, each a glow
             * as big as it is loud */
            float r = k->wave_r * vz.s, turn = vz.time * 0.15f;
            for (int i = 0; i < 2 * VZ_BANDS; i++)
            {
                int b = i < VZ_BANDS ? i : 2 * VZ_BANDS - 1 - i;
                float a = turn + i * (6.2831853f / (2 * VZ_BANDS)), l = vz.lvl[b];
                float rr = r * (0.75f + 0.35f * l);
                uint32_t pc = vz_hsv(k->hue + b * 0.02f, k->sat, 1);
                vz_disc(cx + rr * vz_cosr(a), cy + rr * vz_sinr(a), 2 + 7 * l, pc, w * (int)(80 + 176 * l) >> 8);
            }
            break;
        }
        default: break;
    }
}

/* A point of the picture folded into the part that moves when a mirror
 * shows the rest, so the finger plays in both halves (or all four). */
static void vz_folded(const struct vz_look *k, float *x, float *y)
{
    if (k->fold_x > 0.5f && *x > vz.w / 2.0f) *x = vz.w - 1 - *x;
    if (k->fold_x > 0.5f && k->fold_y > 0.5f && *y > vz.h / 2.0f) *y = vz.h - 1 - *y;
}
/* A ripple of light from where a tap (or the end of a hold) landed. */
static void vz_splash(float power)
{
    int i = 0;
    for (int j = 1; j < VZ_BURSTS; j++) if (vz.burst[j].age > vz.burst[i].age) i = j;   /* the oldest goes */
    vz.burst[i].x = vz.fing.x;
    vz.burst[i].y = vz.fing.y;
    vz.burst[i].age = 0;
    vz.burst[i].power = power;
    vz.burst[i].c = vz_hsv(vz.now.hue + 0.08f * (int)(vz_rand() % 5), vz.now.sat * 0.8f, 1);
}
/* The finger this frame: where, how fast, how it turns; a still press
 * becomes a hold, and what it did fades once it lifts. */
static void vz_finger(const struct vz_look *k, float f, uint32_t now)
{
    float keep = (float)pow(0.86, f);
    if (!vz.fing.down)
    {
        vz.fing.stir *= keep; vz.fing.vx *= keep; vz.fing.vy *= keep; vz.fing.spin *= keep;
        vz.fing.pull *= (float)pow(0.8, f);
        return;
    }
    float x = (float)vz.fing.at.x / (1 << vz.ups), y = (float)vz.fing.at.y / (1 << vz.ups);
    vz_folded(k, &x, &y);
    if (vz.fing.fresh)
    {
        vz.fing.x = vz.fing.px = x;
        vz.fing.y = vz.fing.py = y;
        vz.fing.vx = vz.fing.vy = vz.fing.spin = 0;
        vz.fing.fresh = false;
    }
    float vx = (x - vz.fing.x) / f, vy = (y - vz.fing.y) / f, v2 = vx * vx + vy * vy;
    if (v2 > 144) { vx *= 12 / sqrtf(v2); vy *= 12 / sqrtf(v2); }   /* across a fold, or a jump */
    /* the turn: how the direction changed, signed, as the stir's curl */
    float cross = vz.fing.vx * vy - vz.fing.vy * vx, m = vz.fing.vx * vz.fing.vx + vz.fing.vy * vz.fing.vy + v2 + 1;
    vz.fing.spin += (2 * cross / m - vz.fing.spin) * 0.3f;
    vz.fing.vx += (vx - vz.fing.vx) * 0.6f;
    vz.fing.vy += (vy - vz.fing.vy) * 0.6f;
    vz.fing.px = vz.fing.x; vz.fing.py = vz.fing.y;
    vz.fing.x = x; vz.fing.y = y;
    if (!vz.moved && !vz.fing.hold && now - vz.fing.down_ms >= VZ_HOLD) vz.fing.hold = true;
    if (vz.fing.hold) vz.fing.pull += (1 - vz.fing.pull) * 0.035f * f;
    else if (vz.moved) vz.fing.stir = 1;
}
/* What the finger draws: a trail, a hold's ring, the ripples. */
static void vz_draw_finger(const struct vz_look *k, float dt)
{
    uint32_t c = vz_hsv(k->hue + 0.1f, k->sat * 0.7f, 1);
    if (vz.fing.down && vz.moved)
    {
        vz_line(vz.fing.px, vz.fing.py, vz.fing.x, vz.fing.y, c, 255);
        vz_disc(vz.fing.x, vz.fing.y, vz.s * 0.07f, c, 120);
    }
    if (vz.fing.down && vz.fing.hold)
    {
        /* the event horizon: a dark core and a ring round it, tightening
         * and quickening as it pulls */
        float p = vz.fing.pull, r = vz.s * (0.22f - 0.07f * p + 0.015f * vz_sinr(vz.time * (6 + 10 * p)));
        vz_dark(vz.fing.x, vz.fing.y, r * 0.9f, (int)(120 + 130 * p));
        vz_ring(vz.fing.x, vz.fing.y, r, vz_hsv(k->hue + k->hue2, k->sat, 1), (int)(90 + 165 * p));
    }
    for (int i = 0; i < VZ_BURSTS; i++)
    {
        float a = vz.burst[i].age;
        if (a >= 1) continue;
        float w = vz.burst[i].power * (1 - a) * (1 - a) * 255;
        vz_ring(vz.burst[i].x, vz.burst[i].y, vz.s * (0.04f + 0.5f * a) * (0.7f + 0.5f * vz.burst[i].power),
                vz.burst[i].c, (int)(w > 255 ? 255 : w));
        if (a < 0.15f) vz_disc(vz.burst[i].x, vz.burst[i].y, vz.s * 0.1f * vz.burst[i].power, vz.burst[i].c, (int)(w * 0.6f));
        vz.burst[i].age = a + dt / 0.8f;
    }
}

/* Everything drawn over the moved picture this frame. */
static void vz_draw_music(const struct vz_look *k, float f)
{
    float cx = vz.w / 2.0f, cy = vz.h / 2.0f + k->cy * vz.s;
    uint32_t c = vz_hsv(k->hue, k->sat, 1), c2 = vz_hsv(k->hue + k->hue2, k->sat, 1);
    int bright = 150 + (int)(70 * (vz.att[1] < 1.5f ? vz.att[1] : 1.5f));
    for (int m = 1; m < VZ_WAVES; m++)
        if (vz.now_wave[m] > 0.01f) vz_draw_wave(m, k, c, (int)(bright * vz.now_wave[m]));
    if (k->glow > 0.01f)
    {
        float b = vz.att[0] < 2 ? vz.att[0] : 2;
        vz_disc(cx, cy, vz.s * (0.08f + 0.1f * b), c2, (int)(k->glow * (24 + 48 * b)));
    }
    if (k->rings > 0.01f && vz.beat_now)
        vz_ring(cx, cy, vz.s * (0.1f + 0.08f * vz.beat), vz_hsv(k->hue + 0.15f, k->sat * 0.6f, 1),
                (int)(k->rings * 255 * (0.5f + 0.5f * vz.beat)));
    if (k->stars > 0.01f)
    {
        /* each star flies out from near the middle, faster the farther
         * it is, a streak from where it was; a beat is a burst of speed */
        int n = (int)(k->stars * VZ_STARS);
        float grow = f * (0.035f + 0.08f * vz.beat + 0.4f * (k->zoom > 1 ? k->zoom - 1 : 0));
        uint32_t sc = vz_mix(0xffffff, c, 60);
        for (int i = 0; i < n; i++)
        {
            float r0 = vz.star_r[i], r1 = r0 * (1 + grow) + 0.004f * f;
            if (!(r0 > 0 && r0 < 2.1f))  /* gone off the screen, or never out */
            {
                vz.star_a[i] = (vz_rand() & 1023) * (6.2831853f / 1024);
                vz.star_r[i] = 0.03f + 0.3f * (vz_rand() & 255) / 255;
                continue;
            }
            vz.star_r[i] = r1;
            float ca = vz_cosr(vz.star_a[i]) * vz.s, sa = vz_sinr(vz.star_a[i]) * vz.s;
            vz_line(cx + r0 * ca, cy + r0 * sa, cx + r1 * ca, cy + r1 * sa, sc,
                    (int)(k->stars * 256 * (r1 < 0.5f ? r1 * 2 : 1)));
        }
    }
}

/* ------------------------------------------------------------ the looks */
static void vz_blend(struct vz_look *o, const struct vz_look *a, const struct vz_look *b, float t)
{
#define VZ_MIX(f) o->f = a->f + (b->f - a->f) * t
    VZ_MIX(zoom); VZ_MIX(zoom_r); VZ_MIX(rot); VZ_MIX(swirl); VZ_MIX(warp); VZ_MIX(warp_speed);
    VZ_MIX(warp_scale); VZ_MIX(dx); VZ_MIX(dy); VZ_MIX(cy); VZ_MIX(sx); VZ_MIX(sy); VZ_MIX(decay);
    VZ_MIX(tint); VZ_MIX(hue_speed); VZ_MIX(sat); VZ_MIX(wave_r); VZ_MIX(wave_amp); VZ_MIX(beat_zoom);
    VZ_MIX(beat_rot); VZ_MIX(fold_x); VZ_MIX(fold_y); VZ_MIX(stars); VZ_MIX(rings); VZ_MIX(glow); VZ_MIX(hue2);
#undef VZ_MIX
    float d = b->hue - a->hue;
    o->hue = vz_frac(a->hue + (d - floorf(d + 0.5f)) * t);   /* the short way round */
    o->name = t < 0.5f ? a->name : b->name;
    o->wave = t < 0.5f ? a->wave : b->wave;
}

/* Over to look 'to' in ms, from wherever the picture is now. */
static void vz_go(int to, uint32_t ms)
{
    vz.from = vz.now;
    memcpy(vz.from_wave, vz.now_wave, sizeof vz.from_wave);
    vz.to = to;
    vz.t0 = lv_tick_get();
    vz.dur = ms;
    vz.next_ms = vz.t0 + 22000 + vz_rand() % 12000;
    vz.changes++;
}
static int vz_pick(void)
{
    int n = (int)(vz_rand() % (VZ_LOOKS - 1));
    return n >= vz.to ? n + 1 : n;
}

/* This frame's look: the blend so far, the waves' shares of it. */
static void vz_now(float dt, uint32_t now)
{
    float t = vz.dur ? (float)(now - vz.t0) / vz.dur : 1;
    t = t >= 1 ? 1 : t * t * (3 - 2 * t);
    struct vz_look to = vz_looks[vz.to];
    to.hue = vz_frac(to.hue + vz.time * to.hue_speed);
    vz.from.hue = vz_frac(vz.from.hue + vz.from.hue_speed * dt);
    vz_blend(&vz.now, &vz.from, &to, t);
    for (int m = 0; m < VZ_WAVES; m++) vz.now_wave[m] = vz.from_wave[m] * (1 - t) + (m == to.wave ? t : 0);
    for (int i = 0; i < 8; i++)
    {
        static const float speed[8] = { 0.333f, 0.375f, 0.753f, 0.825f, 1.413f, 1.113f, 1.233f, 0.933f };
        vz.ph[i] = vz_frac((vz.ph[i] + dt * vz.now.warp_speed * speed[i]) / 6.2831853f) * 6.2831853f;
    }
}

/* From the full picture to the half one, a pixel for every four. */
static void vz_halve(void)
{
    const uint32_t *s = vz.buf[vz.cur];
    uint32_t *d = vz.buf[vz.cur ^ 1];
    for (int y = 0; y < vz.h / 2; y++)
        for (int x = 0; x < vz.w / 2; x++)
        {
            const uint32_t *p = s + 2 * y * vz.w + 2 * x;
            d[y * (vz.w / 2) + x] = vz_mix(vz_mix(p[0], p[1], 128), vz_mix(p[vz.w], p[vz.w + 1], 128), 128);
        }
    vz.cur ^= 1;
    vz_size(true);
    vz_low = true;
}

/* ------------------------------------------------------------ a frame */
static bool vz_showing(void) { return vz.active; }
static bool vz_seen(void) { return vz.active && !sl_power_hides_ui() && !po_is_open(); }

static void vz_show_chrome(void);
static void vz_tick(lv_timer_t *t)
{
    (void)t;
    bool seen = vz_seen();
    sp_listen(seen);
    if (!seen) { vz.last_ms = 0; vz.last_us = 0; return; }
    uint32_t now = lv_tick_get();
    float dt = vz.last_ms ? (now - vz.last_ms) / 1000.0f : VZ_FRAME / 1000.0f;
    dt = dt > 0.1f ? 0.1f : dt;
    vz.last_ms = now;
    uint64_t t0 = sl_perf_now_us();
    if (vz.paint_us)                     /* the last frame's painting, all its strips */
    {
        cfm_add(&vz.paint, (uint32_t)vz.paint_us);
        vz.paint_us = 0;
    }
    bool music = vz_hear(dt, now);
    struct mp3entry *id3 = music ? audio_current_track() : NULL;
    if (id3 && strcmp(id3->path, vz.song))
    {
        bool first = !vz.song[0];
        strlcpy(vz.song, id3->path, sizeof vz.song);
        if (!first) vz_show_chrome();    /* a new song: its name, a moment */
    }
    /* played with, it keeps moving even with the music paused */
    bool playing = vz.fing.down || vz.fing.stir > 0.01f || vz.fing.pull > 0.01f || vz.fing.pull < -0.01f;
    for (int i = 0; i < VZ_BURSTS; i++) if (vz.burst[i].age < 1) playing = true;
    vz.quiet = music || playing ? 0 : vz.quiet + 1;
    if (vz.quiet > VZ_SETTLE)
    {
        vz.settled = true;               /* paused: the picture rests as it is */
        vz.last_us = 0;
        return;
    }
    vz.settled = false;
    vz.time += dt;
    /* on to another look in a while, or on a strong beat once this one
     * has had most of its time */
    if (music && !vz.fing.down && ((int32_t)(now - vz.next_ms) >= 0 ||
                  (vz.beat_now && vz.beat > 0.9f && (int32_t)(now - vz.next_ms) > -10000)))
        vz_go(vz_pick(), 3000);
    vz_now(dt, now);

    const struct vz_look *k = &vz.now;
    float f = dt * 30;
    f = f < 0.25f ? 0.25f : f > 3 ? 3 : f;
    /* a full fold moves only what it shows: half, or a quarter */
    bool fx = k->fold_x > 0.996f, fy = fx && k->fold_y > 0.996f;
    int gw = fx ? (vz.gw + 1) / 2 : vz.gw, gh = fy ? (vz.gh + 1) / 2 : vz.gh;
    vz_finger(k, f, now);
    vz_grid(k, f, gw, gh);
    double fade = pow(k->decay, f);
    uint32_t d_rb = (uint32_t)(fade * 256), d_g = (uint32_t)(fade * (k->tint < 1 ? k->tint : 1) * 256);
    const uint32_t *src = vz.buf[vz.cur];
    uint32_t *dst = vz.buf[vz.cur ^ 1];
    for (int gy = 0; gy < gh; gy += 10)
    {
        vz_warp(src, dst, gy, gy + 10 < gh ? gy + 10 : gh, gw, d_rb, d_g);
        yield();                         /* the decoder, between strips */
    }
    vz.cur ^= 1;
    vz_draw_music(k, f);
    vz_draw_finger(k, dt);
    vz_fold(vz.buf[vz.cur], (uint32_t)(k->fold_x * 256 + 0.5f), (uint32_t)(k->fold_y * k->fold_x * 256 + 0.5f));
    lv_obj_invalidate(vz.pic);

    uint64_t t1 = sl_perf_now_us();
    cfm_add(&vz.work, (uint32_t)(t1 - t0));
    if (vz.last_us && t0 - vz.last_us < 250000) cfm_add(&vz.gap, (uint32_t)(t0 - vz.last_us));
    vz.last_us = t0;
    vz.frames++;
    /* frames no closer than the work takes and a third again: decoding
     * and touch get the rest */
    uint32_t paint = vz.paint.n ? (uint32_t)(vz.paint.sum / vz.paint.n) : 0;
    uint32_t busy = ((uint32_t)(t1 - t0) + paint) / 1000, period = busy * 4 / 3;
    /* over Bluetooth the encoder needs the CPU too: half the frames at
     * most, and the work gets as much time again for the rest */
    bool bt = sl_bt_streaming();
    if (bt) period = LV_MAX(busy * 2, VZ_FRAME_BT);
    lv_timer_set_period(vz.timer, period < VZ_FRAME ? VZ_FRAME : period < 100 ? period : 100);
    /* a player that can't do the full picture in time does the half one,
     * found out over its first frames and kept for the session */
    if (!vz_low && vz.work.n == 40 && vz.work.sum / 40 + paint > (bt ? VZ_SLOW / 2 : VZ_SLOW))
    {
        sl_log("visualizer: %lu us a frame at %d x %d, too slow: %d x %d from here",
               (unsigned long)(vz.work.sum / 40 + paint), vz.w, vz.h, vz.w / 2, vz.h / 2);
        vz_halve();
    }
}

/* ------------------------------------------------------------ painting */
static uint16_t vz_565(uint32_t c) { return (uint16_t)((c >> 8 & 0xf800) | (c >> 5 & 0x07e0) | (c >> 3 & 0x001f)); }
#define VZ_AVG(a, b, m) (((a) & (b)) + ((((a) ^ (b)) & (m)) >> 1))   /* RGB565 halfway, m drops each low bit */

/* Picture row k at screen width, dithered 2 x 2 so RGB565 does not band
 * the fades: each pixel, then the steps to the next (halfway, or a
 * quarter, half and three quarters). */
static const uint16_t *vz_row(const uint32_t *pic, int k)
{
    static const uint32_t dith[2][2] = { { 0x000000, 0x040204 }, { 0x060306, 0x020102 } };
    uint16_t *o = vz_rows[k & 1];
    if (vz_row_k[k & 1] == k) return o;
    vz_row_k[k & 1] = k;
    const uint32_t *s = pic + k * vz.w, *d = dith[k & 1];
    uint32_t *o32 = (uint32_t *)o, c = vz_565(s[0] + d[0]);
    int last = vz.w - 1;
    if (vz.ups == 1)
    {
        for (int x = 0; x < last; x++)
        {
            uint32_t n = vz_565(s[x + 1] + d[(x + 1) & 1]);
            o32[x] = c | VZ_AVG(c, n, 0xf7deu) << 16;
            c = n;
        }
        o32[last] = c | c << 16;
        return o;
    }
    for (int x = 0; x < last; x++, o32 += 2)
    {
        uint32_t n = vz_565(s[x + 1] + d[(x + 1) & 1]), m = VZ_AVG(c, n, 0xf7deu);
        o32[0] = c | VZ_AVG(c, m, 0xf7deu) << 16;
        o32[1] = m | VZ_AVG(m, n, 0xf7deu) << 16;
        c = n;
    }
    o32[0] = o32[1] = c | c << 16;
    return o;
}
/* A screen row between two, halfway, two pixels at a time. */
static const uint16_t *vz_between(const uint16_t *a, const uint16_t *b, uint16_t *out)
{
    const uint32_t *a32 = (const uint32_t *)a, *b32 = (const uint32_t *)b;
    uint32_t *o = (uint32_t *)out;
    for (int x = 0; x < W / 2; x++) o[x] = VZ_AVG(a32[x], b32[x], 0xf7def7deu);
    return out;
}
/* Screen row y of the picture: a picture row, or one between two, made
 * in out (the screen itself, when it takes whole rows) unless it is kept
 * for the rows after it. */
static const uint16_t *vz_screen_row(const uint32_t *pic, int y, uint16_t *out)
{
    int k = y >> vz.ups, ph = y & ((1 << vz.ups) - 1), half = 1 << vz.ups >> 1;
    const uint16_t *r = vz_row(pic, k);
    if (!ph || k + 1 >= vz.h) return r;
    out = out ? out : vz_rows[3];
    if (vz.ups == 1) return vz_between(r, vz_row(pic, k + 1), out);
    if (vz_row_k[2] != k)                /* halfway to the next, once for its three rows */
    {
        vz_between(r, vz_row(pic, k + 1), vz_rows[2]);
        vz_row_k[2] = k;
    }
    if (ph == half) return vz_rows[2];
    return ph < half ? vz_between(r, vz_rows[2], out) : vz_between(vz_rows[2], vz_row(pic, k + 1), out);
}

/* LVGL draws the picture here, a strip or (on the R1) the panel at a
 * time: straight into its buffer, as the software renderer has finished
 * whatever came before by the time an object draws. It covers the whole
 * screen, so nothing under it is drawn first. */
static void vz_paint_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_COVER_CHECK)
    {
        lv_event_set_cover_res(e, LV_COVER_RES_COVER);
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_buf_t *db = layer->draw_buf;
    lv_area_t obj, a = layer->_clip_area;
    lv_obj_get_coords(vz.pic, &obj);
    const lv_area_t *in[2] = { &layer->buf_area, &obj };   /* the clip, inside both */
    for (int i = 0; i < 2; i++)
    {
        a.x1 = LV_MAX(a.x1, in[i]->x1); a.y1 = LV_MAX(a.y1, in[i]->y1);
        a.x2 = LV_MIN(a.x2, in[i]->x2); a.y2 = LV_MIN(a.y2, in[i]->y2);
    }
    if (!vz.buf[0] || !db || layer->color_format != LV_COLOR_FORMAT_RGB565 || a.x1 > a.x2 || a.y1 > a.y2)
        return;
    uint64_t t0 = sl_perf_now_us();
    const uint32_t *pic = vz.buf[vz.cur];
    vz_row_k[0] = vz_row_k[1] = vz_row_k[2] = -1;   /* a new frame, maybe */
    uint8_t *d = db->data + (a.y1 - layer->buf_area.y1) * db->header.stride + (a.x1 - layer->buf_area.x1) * 2;
    bool whole = a.x1 == obj.x1 && a.x2 == obj.x2 && obj.x2 - obj.x1 + 1 == W &&
                 !((uintptr_t)d & 3) && !(db->header.stride & 3);
    for (int y = a.y1; y <= a.y2; y++, d += db->header.stride)
    {
        const uint16_t *r = vz_screen_row(pic, y - obj.y1, whole ? (uint16_t *)d : NULL);
        if (r != (const uint16_t *)d) memcpy(d, r + (a.x1 - obj.x1), (size_t)lv_area_get_width(&a) * 2);
    }
    vz.paint_us += sl_perf_now_us() - t0;
}

/* ------------------------------------------------------------ the chrome */
/* The look's name and dots up top, the song at the foot, each over a soft
 * shade; shown on a tap, a swipe or a new song, then fading. */
static void vz_faded_cb(lv_anim_t *a) { if (!lv_obj_get_style_opa(a->var, 0)) lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }
static void vz_fade(int to)
{
    lv_obj_t *o[2] = { vz.top, vz.bottom };
    for (int i = 0; i < 2; i++)
    {
        if (to) lv_obj_remove_flag(o[i], LV_OBJ_FLAG_HIDDEN);   /* faded out it is hidden: no unseen close button */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o[i]);
        lv_anim_set_values(&a, lv_obj_get_style_opa(o[i], 0), to);
        lv_anim_set_duration(&a, to ? SL_T_FAST : SL_T_SLOW);
        lv_anim_set_exec_cb(&a, spa_set_opa);
        if (!to) lv_anim_set_completed_cb(&a, vz_faded_cb);
        lv_anim_start(&a);
    }
}
static void vz_hide_cb(lv_timer_t *t) { (void)t; vz.hide = NULL; vz_fade(LV_OPA_TRANSP); }
static void vz_show_chrome(void)
{
    lv_label_set_text(vz.name, sl_tr(vz_looks[vz.to].name));
    for (int i = 0; i < VZ_LOOKS; i++)
        lv_obj_set_style_bg_opa(vz.dots[i], i == vz.to ? LV_OPA_COVER : 70, 0);
    struct mp3entry *id3 = audio_current_track();
    char b[MAX_PATH];
    lv_label_set_text(vz.title, id3 ? track_title(id3, b, sizeof b) : "");
    lv_label_set_text(vz.artist, id3 ? track_artist(id3) : "");
    /* the first time in a session, how to play with it */
    lv_label_set_text(vz.hint, vz_hinted ? "" : sl_tr("Drag to stir, tap to splash, hold to pull in"));
    vz_hinted = true;
    vz_fade(LV_OPA_COVER);
    if (vz.hide) lv_timer_reset(vz.hide);
    else
    {
        vz.hide = lv_timer_create(vz_hide_cb, 2600, NULL);
        lv_timer_set_repeat_count(vz.hide, 1);
    }
}
static lv_obj_t *vz_shade(int y, int h, bool down)
{
    lv_obj_t *o = box(vz.scr);
    lv_obj_set_size(o, W, h);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(o, down ? 130 : 0, 0);
    lv_obj_set_style_bg_grad_opa(o, down ? 0 : 150, 0);
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}
static lv_obj_t *vz_text(lv_obj_t *parent, const lv_font_t *f, int y, lv_opa_t opa)
{
    lv_obj_t *l = text(parent, "", f, 0xffffff);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);   /* on the picture: white in every theme */
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, W - 48);
    label_lines(l, 1);
    lv_obj_set_pos(l, 24, y);
    return l;
}

static void vz_close(void);
static void vz_done_cb(lv_event_t *e) { (void)e; vz_close(); }
/* Playing and finding the way about, without one fighting the other: a
 * drag stirs, unless it is a quick sideways flick (the next look) or a
 * pull down from the top band (close); a tap splashes, unless it is in
 * the top band (the chrome); a still press is a hold. */
static void vz_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t pt = vz.fing.at;
    if (in) lv_indev_get_point(in, &pt);
    int dx = pt.x - vz.down.x, dy = pt.y - vz.down.y;
    if (code == LV_EVENT_PRESSED)
    {
        vz.down = vz.fing.at = pt;
        vz.moved = vz.fing.hold = false;
        vz.fing.down = vz.fing.fresh = true;
        vz.fing.down_ms = lv_tick_get();
        vz.fing.stir = 0;
        return;
    }
    if (!vz.fing.down) return;
    vz.fing.at = pt;
    if (code == LV_EVENT_PRESSING)
    {
        if (!vz.fing.hold && (LV_ABS(dx) > 24 || LV_ABS(dy) > 24)) vz.moved = true;
        return;
    }
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    vz.fing.down = false;
    if (code == LV_EVENT_PRESS_LOST) return;
    uint32_t held = lv_tick_get() - vz.fing.down_ms;
    bool top = vz.down.y < VZ_TOP;
    if (vz.fing.hold)                    /* let go of a black hole: it all bursts out */
    {
        vz.fing.pull = -1.3f * vz.fing.pull;
        vz_splash(1.3f);
    }
    else if (!vz.moved)
    {
        if (top) vz_show_chrome();       /* what this is, and the way out */
        else
        {
            float x = (float)pt.x / (1 << vz.ups), y = (float)pt.y / (1 << vz.ups);
            vz_folded(&vz.now, &x, &y);
            vz.fing.x = x; vz.fing.y = y;
            vz.fing.pull = -0.35f - 0.35f * vz.beat;   /* a little push out under the ripple */
            vz_splash(0.75f + 0.5f * vz.beat);
        }
    }
    else if (held < VZ_FLICK && LV_ABS(dx) > 90 && LV_ABS(dx) > 2 * LV_ABS(dy))
    {
        vz_go((vz.to + (dx < 0 ? 1 : VZ_LOOKS - 1)) % VZ_LOOKS, 900);   /* left: the next */
        vz.fing.stir = 0;
        sfx_play(SFX_UI_TICK);
        vz_show_chrome();
    }
    else if (top && dy > 140 && dy > 2 * LV_ABS(dx)) vz_close();      /* pulled down, as Now Playing */
}

/* ------------------------------------------------------------ open, close */
static void vz_open(void)
{
    if (vz.active) return;
    uint32_t *a = malloc(VZ_W * VZ_H * 4), *b = malloc(VZ_W * VZ_H * 4);
    if (!a || !b)
    {
        free(a); free(b);
        toast_warn("Not enough memory for the visualizer");
        return;
    }
    memset(a, 0, VZ_W * VZ_H * 4);
    memset(b, 0, VZ_W * VZ_H * 4);
    if (!vz_sin[VZ_SIN / 4])
        for (int i = 0; i < VZ_SIN; i++) vz_sin[i] = sinf(i * (6.2831853f / VZ_SIN));
    cf_close();                          /* Cover Flow and Spectrum: one screen of their own at a time */
    if (sp.canvas) lv_obj_delete(sp.canvas);   /* the tile's: this takes the analyzer */
    sp_forget();
    sp.bars = VZ_BANDS;
    sp.rate = 0;                         /* band edges for twelve */

    struct cfm_hist keep = { .shift = 9 };
    memset(&vz, 0, sizeof vz);
    vz.work = vz.paint = vz.gap = keep;  /* 0.5 ms bins */
    vz.buf[0] = a; vz.buf[1] = b;
    vz_size(vz_low);
    vz.seed = lv_tick_get() | 1;
    vz.to = (int)(vz_rand() % VZ_LOOKS);
    vz.now = vz_looks[vz.to];
    vz.now_wave[vz.now.wave] = 1;
    vz_go(vz.to, 0);
    vz.changes = 0;
    vz.loud = -14;
    vz.agc = 1500;
    vz.opened_us = sl_perf_now_us();
    vz.active = true;
    vz.home = lv_screen_active();
    vz.sb_shown = !lv_obj_has_flag(sb, LV_OBJ_FLAG_HIDDEN);
    statusbar_show(false);

    vz.scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(vz.scr);
    lv_obj_set_style_bg_color(vz.scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vz.scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(vz.scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(vz.scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(vz.scr, vz_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(vz.scr, vz_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(vz.scr, vz_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(vz.scr, vz_touch_cb, LV_EVENT_PRESS_LOST, NULL);
    vz.pic = box(vz.scr);
    lv_obj_set_size(vz.pic, W, H);
    lv_obj_add_event_cb(vz.pic, vz_paint_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(vz.pic, vz_paint_cb, LV_EVENT_COVER_CHECK, NULL);

    vz.top = vz_shade(0, 130, true);
    lv_obj_t *done = box(vz.top);
    lv_obj_set_size(done, 56, 56);
    lv_obj_set_pos(done, 16, 16);
    lv_obj_set_style_radius(done, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(done, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(done, 40, 0);
    lv_obj_set_style_bg_opa(done, 90, LV_STATE_PRESSED);
    lv_obj_t *x = text(done, LV_SYMBOL_CLOSE, &sl_sym_24, 0xffffff);
    lv_obj_set_style_text_color(x, lv_color_white(), 0);
    lv_obj_center(x);
    lv_obj_set_ext_click_area(done, 14);
    on_click(done, vz_done_cb, NULL);
    lv_obj_remove_flag(done, LV_OBJ_FLAG_EVENT_BUBBLE);
    vz.name = text(vz.top, "", &ss_semibold_26, 0xffffff);
    lv_obj_set_style_text_color(vz.name, lv_color_white(), 0);
    lv_obj_align(vz.name, LV_ALIGN_TOP_MID, 0, 26);
    for (int i = 0; i < VZ_LOOKS; i++)
    {
        lv_obj_t *d = vz.dots[i] = box(vz.top);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(d, lv_color_white(), 0);
        lv_obj_set_pos(d, W / 2 - VZ_LOOKS * 8 + i * 16 + 4, 66);
    }
    vz.hint = vz_text(vz.top, &ss_regular_18, 86, 200);
    vz.bottom = vz_shade(H - 150, 150, false);
    vz.title = vz_text(vz.bottom, &ss_semibold_23, 64, LV_OPA_COVER);
    vz.artist = vz_text(vz.bottom, &ss_regular_18, 100, 200);

    lv_screen_load(vz.scr);
    vz.timer = lv_timer_create(vz_tick, VZ_FRAME, NULL);
    vz_show_chrome();
    sl_log("visualizer: open on %s", vz_looks[vz.to].name);
}

static void vz_close(void)
{
    if (!vz.active) return;
    sp_listen(false);
    vz.active = false;
    lv_timer_delete(vz.timer);
    if (vz.hide) lv_timer_delete(vz.hide);
    vz.timer = vz.hide = NULL;
    lv_anim_delete(vz.top, NULL);
    lv_anim_delete(vz.bottom, NULL);
    lv_screen_load(vz.home);
    if (vz.sb_shown) statusbar_show(true);
    lv_obj_delete(vz.scr);
    vz.scr = vz.pic = vz.top = vz.bottom = vz.name = vz.hint = vz.title = vz.artist = NULL;
    free(vz.buf[0]); free(vz.buf[1]);
    vz.buf[0] = vz.buf[1] = NULL;
    lv_obj_invalidate(lv_screen_active());
    /* what the device managed: frames apart (the rate), a frame's own
     * work (motion, music, overlays) and its painting to the screen */
    uint32_t gap = vz.gap.n ? (uint32_t)(vz.gap.sum / vz.gap.n) : 0;
    sl_log("visualizer: %lu frames in %lu s, %lu.%lu fps, %lu looks, %lu beats",
           (unsigned long)vz.frames, (unsigned long)((sl_perf_now_us() - vz.opened_us) / 1000000),
           (unsigned long)(gap ? 1000000 / gap : 0), (unsigned long)(gap ? 10000000 / gap % 10 : 0),
           (unsigned long)vz.changes, (unsigned long)vz.beats);
    if (vz.work.n)
        sl_log("visualizer: work avg %lu p95 %lu max %lu us, paint avg %lu p95 %lu max %lu us",
               (unsigned long)(vz.work.sum / vz.work.n), (unsigned long)cfm_pct(&vz.work, 95),
               (unsigned long)vz.work.max, (unsigned long)(vz.paint.n ? vz.paint.sum / vz.paint.n : 0),
               (unsigned long)cfm_pct(&vz.paint, 95), (unsigned long)vz.paint.max);
    mt_spectrum_refill();                /* the tile, if Start has one */
}

/* Now Playing's way in, beside the turntable's: a swirl. */
static void vz_btn_cb(lv_event_t *e) { (void)e; vz_open(); }
static void vz_build_button(lv_obj_t *parent)
{
    vz_btn = box(parent);
    lv_obj_set_size(vz_btn, 56, 56);
    lv_obj_set_pos(vz_btn, 136, 10);
    lv_obj_set_style_radius(vz_btn, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(vz_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vz_btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(vz_btn, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_center(sl_icon(vz_btn, IC_VISUALIZER, 32, 0xffffff, 0x000000));
    on_click(vz_btn, vz_btn_cb, NULL);
    lv_obj_remove_flag(vz_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
}
