/* Freaky Spoon rules: an endless climb out of a pot of soup. GPLv2+.
 *
 * Everything here follows from the seed and the steering, one fixed 1/120 s
 * step at a time, so the tests can play whole runs. World y grows downward
 * like the screen: the start plate's top is y = 0 and climbing is negative.
 *
 * Fairness: the level is a "spine" of platforms, each one always reachable
 * from the one before (sl_spoon_reach), with decoys, hazards and treats laid
 * around it. Nothing that can kill appears without warning: meatballs show
 * where they will fall first, soggy toast looks soggy, and the soup rises
 * slower than a spoon that keeps climbing. */
#ifndef SLOPPY_SPOON_STATE_H
#define SLOPPY_SPOON_STATE_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#define SL_SPOON_W 448                  /* the scene, in pixels */
#define SL_SPOON_H 530
#define SL_SPOON_DT (1.0f / 120)
#define SL_SPOON_PX_M 16                /* pixels to a metre of height */
#define SL_SPOON_PLATS 56
#define SL_SPOON_FOES 8
#define SL_SPOON_ITEMS 40
#define SL_SPOON_FXQ 24

#define SL_SPOON_G 1500.f
#define SL_SPOON_JUMP 790.f             /* a 208 px bounce */
#define SL_SPOON_JELLY 1240.f           /* a 512 px bounce */
#define SL_SPOON_STOMP 900.f
#define SL_SPOON_VMAX 390.f
#define SL_SPOON_ACC 5200.f
#define SL_SPOON_DEC 4400.f
#define SL_SPOON_FEET 15                /* half the width that lands */
#define SL_SPOON_BOWL 46                /* bowl centre above the handle tip */
#define SL_SPOON_BOWL_R 13              /* hurt radius, smaller than it looks */
#define SL_SPOON_PLAT_H 12
#define SL_SPOON_MAX_GAP 158            /* 0.76 of a bounce */
#define SL_SPOON_WHISK_T 2.0f
#define SL_SPOON_CHILI_T 6.0f

enum { SP_PLATE, SP_TRAY, SP_CRACKER, SP_SOGGY, SP_JELLY, SP_BLINK };
enum { SF_NONE, SF_FORK, SF_MEATBALL, SF_GOLDEN };
enum { SI_NONE, SI_O, SI_WHISK, SI_LID, SI_CHILI };
enum { SE_NONE, SE_WIND, SE_LOWG, SE_MEATBALLS, SE_DARK, SE_COINS, SE_COUNT };
enum { SE_OFF, SE_WARN, SE_ON };
enum { SD_NONE, SD_SOUP, SD_FORK, SD_MEATBALL };
enum { SL_SPOON_PHASES = 6 };
/* what happened during a step, for the picture and the sound */
enum {
    SX_LAND, SX_PERFECT, SX_MULT, SX_COMBO_LOST, SX_JELLY, SX_CRACK, SX_SOGGY,
    SX_COIN, SX_STOMP, SX_POWER, SX_SHIELD, SX_KNOCK, SX_PHASE, SX_WARN,
    SX_EVENT, SX_EVENT_END, SX_GOLDEN, SX_SECRET, SX_DIE, SX_SPLASH, SX_DROP,
    SX_BEST
};
/* secrets, found once and kept */
enum {
    SS_LOYAL, SS_FORK_OFF, SS_DIZZY, SS_GOLDEN, SS_SWEET, SS_FREAKY,
    SS_JELLY, SS_DIET, SS_HOT_FEET, SS_BUTTER, SS_COUNT
};

static const int sl_spoon_phase_m[SL_SPOON_PHASES] = { 0, 120, 300, 550, 850, 1200 };
static const int sl_spoon_mult_at[5] = { 0, 3, 6, 10, 15 };   /* combo for x1..x5 */

struct sl_spoon_plat {
    float x, y, w;                      /* left edge, top, width */
    float vx, x0, x1;                   /* trays slide between x0 and x1 (left edge) */
    float t, bump;                      /* broken/blink clock; time since landed on */
    uint8_t type, live, broken, spine, used;
};
struct sl_spoon_foe {
    float x, y, vx, vy, x0, x1, t, y0;
    uint8_t type, state;                /* fork 1 and meatball 2: knocked or splatted */
};
struct sl_spoon_item { float x, y; uint8_t type, live; };
struct sl_spoon_fx { uint8_t kind; int16_t x, y; int32_t v; };

struct sl_spoon_state {
    float x, y, vx, vy;                 /* x: centre; y: the handle's tip */
    float cam, soup, time, top_y;
    float whisk, freaky, invuln, lowg;
    int dir, wind;
    uint32_t rng;
    bool started, alive, lid, calm;     /* calm: no hazards or events (tests) */
    int death, score, height, combo, best_combo, mult, phase, best_height;
    int event, event_state;
    float event_t, next_event, event_spawn;
    float gen_y, gen_x, gen_slack;
    int gen_units, pattern, pattern_left, next_pattern, gen_dropped;
    bool golden_done, passed_best;
    int stomps, wraps, jellies, coins, same_plate, last_plate, land_plate;
    uint32_t secrets;                   /* found this run */
    int fxn;
    struct sl_spoon_fx fxq[SL_SPOON_FXQ];
    struct sl_spoon_plat plat[SL_SPOON_PLATS];
    struct sl_spoon_foe foe[SL_SPOON_FOES];
    struct sl_spoon_item item[SL_SPOON_ITEMS];
};

/* ---- small helpers -------------------------------------------------------- */
static uint32_t sl_spoon_rand(struct sl_spoon_state *s)
{ s->rng = s->rng * 1664525u + 1013904223u; return s->rng >> 8; }
static float sl_spoon_frand(struct sl_spoon_state *s, float a, float b)
{ return a + (b - a) * (sl_spoon_rand(s) % 10000) / 10000.0f; }
static bool sl_spoon_chance(struct sl_spoon_state *s, float pct)
{ return (sl_spoon_rand(s) % 10000) < pct * 100; }
static float sl_spoon_lerp(float a, float b, float t) { return a + (b - a) * t; }
static float sl_spoon_clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
/* horizontal distance on a screen whose edges wrap */
static float sl_spoon_wdx(float a, float b)
{
    float d = b - a;
    while (d > SL_SPOON_W / 2) d -= SL_SPOON_W;
    while (d < -SL_SPOON_W / 2) d += SL_SPOON_W;
    return d;
}
static int sl_spoon_metres(float y) { return y < 0 ? (int)(-y / SL_SPOON_PX_M) : 0; }
static int sl_spoon_phase_of(int m)
{
    int p = 0;
    while (p + 1 < SL_SPOON_PHASES && m >= sl_spoon_phase_m[p + 1]) p++;
    return p;
}
/* 0 at the start, 1 at 1500 m and beyond */
static float sl_spoon_diff(float y) { return sl_spoon_clampf(sl_spoon_metres(y) / 1500.0f, 0, 1); }
static int sl_spoon_mult(int combo)
{
    int m = 1;
    while (m < 5 && combo >= sl_spoon_mult_at[m]) m++;
    return m;
}
/* How far sideways a spoon can get between bouncing and landing dy higher,
 * with a big margin: it needs time to turn round, and people are not bots. */
static float sl_spoon_reach(float dy)
{
    float v = SL_SPOON_JUMP, disc = v * v - 2 * SL_SPOON_G * dy;
    if (disc < 0) return 0;
    float t = (v + sqrtf(disc)) / SL_SPOON_G;
    float r = SL_SPOON_VMAX * (t - 0.12f) * 0.62f;
    return r < 0 ? 0 : r;
}
static void sl_spoon_fx(struct sl_spoon_state *s, int kind, float x, float y, int v)
{
    if (s->fxn >= SL_SPOON_FXQ) return;
    s->fxq[s->fxn++] = (struct sl_spoon_fx){ (uint8_t)kind, (int16_t)x, (int16_t)(y - s->cam), v };
}
static void sl_spoon_secret(struct sl_spoon_state *s, int which)
{
    if (s->secrets & 1u << which) return;
    s->secrets |= 1u << which;
    sl_spoon_fx(s, SX_SECRET, s->x, s->y, which);
}
static bool sl_spoon_blink_on(const struct sl_spoon_plat *p) { return fmodf(p->t, 2.6f) < 1.7f; }
static bool sl_spoon_solid(const struct sl_spoon_plat *p)
{
    return p->live && !p->broken && (p->type != SP_BLINK || sl_spoon_blink_on(p));
}

/* ---- building the level ----------------------------------------------------- */
static struct sl_spoon_plat *sl_spoon_add_plat(struct sl_spoon_state *s, int type, float cx, float y, float w)
{
    for (int i = 0; i < SL_SPOON_PLATS; i++)
    {
        struct sl_spoon_plat *p = &s->plat[i];
        if (p->live) continue;
        *p = (struct sl_spoon_plat){0};
        p->type = (uint8_t)type; p->live = 1;
        p->w = w; p->x = sl_spoon_clampf(cx - w / 2, 6, SL_SPOON_W - 6 - w); p->y = y;
        p->x0 = p->x1 = p->x;
        p->bump = 9;
        if (type == SP_BLINK) p->t = sl_spoon_frand(s, 0, 2.6f);
        return p;
    }
    s->gen_dropped++;
    return NULL;
}
static void sl_spoon_add_item(struct sl_spoon_state *s, int type, float x, float y)
{
    for (int i = 0; i < SL_SPOON_ITEMS; i++)
        if (!s->item[i].live)
        { s->item[i] = (struct sl_spoon_item){ x, y, (uint8_t)type, 1 }; return; }
}
static struct sl_spoon_foe *sl_spoon_add_foe(struct sl_spoon_state *s, int type)
{
    for (int i = 0; i < SL_SPOON_FOES; i++)
        if (s->foe[i].type == SF_NONE)
        { s->foe[i] = (struct sl_spoon_foe){0}; s->foe[i].type = (uint8_t)type; return &s->foe[i]; }
    return NULL;
}
/* A tray's centre wanders; `slack` is how far, for the reach check. */
static float sl_spoon_tray(struct sl_spoon_state *s, struct sl_spoon_plat *p, float d)
{
    float range = sl_spoon_frand(s, 40, sl_spoon_lerp(70, 150, d));
    p->type = SP_TRAY;
    p->x0 = sl_spoon_clampf(p->x - range, 6, SL_SPOON_W - 6 - p->w);
    p->x1 = sl_spoon_clampf(p->x + range, 6, SL_SPOON_W - 6 - p->w);
    p->vx = sl_spoon_frand(s, sl_spoon_lerp(45, 80, d), sl_spoon_lerp(70, 150, d)) * (sl_spoon_rand(s) & 1 ? 1 : -1);
    p->x = sl_spoon_frand(s, p->x0, p->x1);
    return (p->x1 - p->x0) / 2;
}
/* The next spine platform, gap above the last one, somewhere a spoon can
 * certainly get to. want is the preferred sideways offset. */
static struct sl_spoon_plat *sl_spoon_spine(struct sl_spoon_state *s, float gap, int type, float w, float want)
{
    float d = sl_spoon_diff(s->gen_y);
    if (gap > SL_SPOON_MAX_GAP) gap = SL_SPOON_MAX_GAP;
    float y = s->gen_y - gap, reach = sl_spoon_reach(gap) - s->gen_slack;
    float slack_guess = type == SP_TRAY ? sl_spoon_lerp(70, 150, d) : 0;
    float room = reach - slack_guess;
    if (room < 0) { room = 0; if (type == SP_TRAY) type = SP_PLATE; }
    float off = sl_spoon_clampf(want, -room, room), cx = 0;
    for (int tries = 0; tries < 8; tries++, off *= 0.6f)
    {
        cx = s->gen_x + off;
        if (cx < 0) cx += SL_SPOON_W;
        if (cx >= SL_SPOON_W) cx -= SL_SPOON_W;
        cx = sl_spoon_clampf(cx, w / 2 + 6, SL_SPOON_W - 6 - w / 2);
        if (fabsf(sl_spoon_wdx(s->gen_x, cx)) <= room) break;
        cx = s->gen_x;
    }
    struct sl_spoon_plat *p = sl_spoon_add_plat(s, type == SP_TRAY ? SP_PLATE : type, cx, y, w);
    float slack = 0;
    if (p && type == SP_TRAY)
    {
        slack = sl_spoon_tray(s, p, d);
        /* the tray's middle must be within reach too, whatever the clamp did */
        float mid = (p->x0 + p->x1) / 2 + p->w / 2;
        if (fabsf(sl_spoon_wdx(s->gen_x, mid)) + slack > reach)
        { p->type = SP_PLATE; p->x = p->x0 = p->x1 = cx - w / 2; p->vx = 0; slack = 0; }
        else cx = mid;
    }
    if (p) { p->spine = 1; cx = p->type == SP_TRAY ? cx : p->x + p->w / 2; }
#ifdef SL_SPOON_TRACE
    SL_SPOON_TRACE(s, p, y, cx, slack);         /* the tests check every step */
#endif
    s->gen_y = y; s->gen_x = cx; s->gen_slack = slack;
    s->gen_units++;
    return p;
}
static float sl_spoon_width(struct sl_spoon_state *s)
{
    return sl_spoon_lerp(92, 60, sl_spoon_diff(s->gen_y)) + sl_spoon_frand(s, -5, 5);
}
static void sl_spoon_coin_column(struct sl_spoon_state *s, float x, float y, int n, float step)
{
    for (int i = 0; i < n; i++) sl_spoon_add_item(s, SI_O, x, y - i * step);
}
/* something between the last spine platform and the one before it */
static void sl_spoon_extras(struct sl_spoon_state *s, float below_y)
{
    float d = sl_spoon_diff(s->gen_y), gap = below_y - s->gen_y;
    int m = sl_spoon_metres(s->gen_y), ph = sl_spoon_phase_of(m);
    bool fork = false;
    /* a fork patrolling the gap, from the Cupboard up; nothing else shares
     * its height, so there is never a plate to stand on in its path */
    if (!s->calm && ph >= 1 && gap >= 100 && sl_spoon_chance(s, sl_spoon_lerp(6, 34, d)))
    {
        struct sl_spoon_foe *f = sl_spoon_add_foe(s, SF_FORK);
        if (f)
        {
            float span = sl_spoon_frand(s, 150, 360);
            f->x0 = sl_spoon_frand(s, 30, SL_SPOON_W - 30 - span);
            f->x1 = f->x0 + span;
            f->x = sl_spoon_frand(s, f->x0, f->x1);
            f->y = f->y0 = (below_y + s->gen_y) / 2;
            f->vx = sl_spoon_frand(s, sl_spoon_lerp(55, 90, d), sl_spoon_lerp(80, 150, d)) * (sl_spoon_rand(s) & 1 ? 1 : -1);
            if (sl_spoon_chance(s, 50)) sl_spoon_add_item(s, SI_O, f->x, f->y - 36);
            fork = true;
        }
    }
    if (!fork && gap >= 64 && sl_spoon_chance(s, ph ? sl_spoon_lerp(26, 42, d) : 30))
    {
        /* off to one side, never level with a spine platform, and close
         * enough to the next one to carry on from */
        float y = s->gen_y + gap * sl_spoon_frand(s, 0.4f, 0.6f);
        float side = sl_spoon_reach(y - s->gen_y);
        float x = s->gen_x + (sl_spoon_rand(s) & 1 ? 1 : -1) * sl_spoon_frand(s, 100, side < 180 ? side : 180);
        if (x < 0) x += SL_SPOON_W;
        if (x >= SL_SPOON_W) x -= SL_SPOON_W;
        int type = SP_PLATE, r = sl_spoon_rand(s) % 100;
        if (ph >= 1 && r < sl_spoon_lerp(20, 55, d)) type = SP_SOGGY;
        else if (ph >= 2 && r < sl_spoon_lerp(20, 55, d) + 20) type = SP_BLINK;
        else if (ph == 0 && r < 12) type = SP_JELLY;
        struct sl_spoon_plat *p = sl_spoon_add_plat(s, type, x, y, sl_spoon_width(s));
        /* a treat to make the detour worth it */
        if (p && type != SP_SOGGY && sl_spoon_chance(s, 45))
            sl_spoon_coin_column(s, p->x + p->w / 2, p->y - 34, 2 + sl_spoon_rand(s) % 2, 30);
    }
    /* above the new platform: a power-up now and then, or O's that a
     * perfect bounce collects */
    if (m > 60 && sl_spoon_chance(s, 1.4f)) sl_spoon_add_item(s, SI_WHISK, s->gen_x, s->gen_y - 44);
    else if (m > 120 && sl_spoon_chance(s, 1.8f)) sl_spoon_add_item(s, SI_LID, s->gen_x, s->gen_y - 44);
    else if (m > 300 && sl_spoon_chance(s, 0.9f)) sl_spoon_add_item(s, SI_CHILI, s->gen_x, s->gen_y - 44);
    else if (sl_spoon_chance(s, 20)) sl_spoon_coin_column(s, s->gen_x, s->gen_y - 40, 3, 32);
    /* the golden spoon, at most once a run */
    if (!s->calm && !s->golden_done && m > 200 && sl_spoon_chance(s, 0.5f))
    {
        struct sl_spoon_foe *f = sl_spoon_add_foe(s, SF_GOLDEN);
        if (f)
        {
            bool left = sl_spoon_rand(s) & 1;
            f->x = left ? -30 : SL_SPOON_W + 30;
            f->vx = left ? 85 : -85;
            f->y = f->y0 = s->gen_y - 60;
            s->golden_done = true;
        }
    }
}
enum { PT_NONE, PT_STAIRS, PT_JELLY, PT_CRACKERS, PT_TRAYS, PT_DECOYS, PT_BLINKS, PT_FORKS, PT_REST, PT_COUNT };
static int sl_spoon_pick_pattern(struct sl_spoon_state *s)
{
    int ph = sl_spoon_phase_of(sl_spoon_metres(s->gen_y));
    static const uint8_t from[PT_COUNT] = { 9, 0, 0, 1, 1, 2, 2, 3, 1 };
    for (int tries = 0; tries < 12; tries++)
    {
        int p = 1 + sl_spoon_rand(s) % (PT_COUNT - 1);
        if (from[p] > ph || p == s->pattern) continue;
        if (s->calm && p == PT_FORKS) continue;
        return p;
    }
    return PT_STAIRS;
}
/* one piece of level: a spine platform and its surroundings, or a set piece */
static void sl_spoon_gen_unit(struct sl_spoon_state *s)
{
    float d = sl_spoon_diff(s->gen_y);
    float below_y = s->gen_y;
    int m = sl_spoon_metres(s->gen_y), ph = sl_spoon_phase_of(m);
    if (!s->pattern_left && s->gen_units >= s->next_pattern)
    {
        s->pattern = sl_spoon_pick_pattern(s);
        static const uint8_t len[PT_COUNT] = { 0, 6, 1, 5, 4, 5, 4, 3, 1 };
        s->pattern_left = len[s->pattern];
        s->next_pattern = s->gen_units + s->pattern_left + 9 + sl_spoon_rand(s) % 7;
    }
    if (s->pattern_left)
    {
        int k = s->pattern_left--;
        struct sl_spoon_plat *p;
        switch (s->pattern)
        {
        case PT_STAIRS:         /* a quick zigzag with an O on every step */
            p = sl_spoon_spine(s, 50, SP_PLATE, 70, k & 1 ? 80 : -80);
            if (p) sl_spoon_add_item(s, SI_O, p->x + p->w / 2, p->y - 38);
            return;
        case PT_JELLY:          /* a launch pad and a column of O's up the flight */
            p = sl_spoon_spine(s, 70, SP_JELLY, 76, 0);
            if (p) sl_spoon_coin_column(s, p->x + p->w / 2, p->y - 90, 8, 44);
            /* the next spine platform must be under the top of the flight,
             * and a jelly bounce clears the usual gaps easily */
            return;
        case PT_CRACKERS:       /* no going back */
            sl_spoon_spine(s, sl_spoon_lerp(64, 96, d), SP_CRACKER, sl_spoon_width(s), sl_spoon_frand(s, -120, 120));
            return;
        case PT_TRAYS:
            sl_spoon_spine(s, sl_spoon_lerp(80, 120, d), SP_TRAY, 72, sl_spoon_frand(s, -60, 60));
            return;
        case PT_DECOYS:         /* two plates side by side; one is soggy */
            p = sl_spoon_spine(s, sl_spoon_lerp(70, 110, d), SP_PLATE, 70, sl_spoon_frand(s, -90, 90));
            if (p)
            {
                float cx = p->x + p->w / 2, other = cx + (cx < SL_SPOON_W / 2 ? 1 : -1) * sl_spoon_frand(s, 120, 170);
                sl_spoon_add_plat(s, SP_SOGGY, other, p->y + sl_spoon_frand(s, -6, 6), 70);
            }
            return;
        case PT_BLINKS:         /* a safe way, and a blinking way paved with O's */
            p = sl_spoon_spine(s, 84, SP_PLATE, 64, k & 1 ? 40 : -40);
            if (p)
            {
                float cx = p->x + p->w / 2, other = cx < SL_SPOON_W / 2 ? cx + 200 : cx - 200;
                struct sl_spoon_plat *b = sl_spoon_add_plat(s, SP_BLINK, other, p->y + 40, 76);
                if (b) sl_spoon_coin_column(s, b->x + b->w / 2, b->y - 36, 2, 30);
            }
            return;
        case PT_FORKS:          /* stacked forks over narrow plates */
            sl_spoon_spine(s, 118, SP_PLATE, 62, sl_spoon_frand(s, -70, 70));
            if (!s->calm)
            {
                struct sl_spoon_foe *f = sl_spoon_add_foe(s, SF_FORK);
                if (f)
                {
                    f->x0 = 40; f->x1 = SL_SPOON_W - 40;
                    f->x = sl_spoon_frand(s, f->x0, f->x1);
                    f->y = f->y0 = s->gen_y + 59;
                    f->vx = sl_spoon_lerp(80, 130, d) * (k & 1 ? 1 : -1);
                    sl_spoon_add_item(s, SI_O, f->x, f->y - 34);
                }
            }
            return;
        case PT_REST:           /* a wide plate, a breather and a treat */
            p = sl_spoon_spine(s, 80, SP_PLATE, 150, 0);
            if (p)
            {
                sl_spoon_add_item(s, m > 150 && (sl_spoon_rand(s) & 1) ? SI_LID : SI_WHISK, p->x + 30, p->y - 44);
                sl_spoon_coin_column(s, p->x + p->w - 30, p->y - 40, 3, 30);
            }
            return;
        }
    }
    /* an ordinary step */
    float gmin = sl_spoon_lerp(50, 74, d), gmax = sl_spoon_lerp(86, SL_SPOON_MAX_GAP, d);
    if (m < 20) gmax = 80;
    float gap = sl_spoon_frand(s, gmin, gmax);
    int r = sl_spoon_rand(s) % 100, type = SP_PLATE;
    float cracker = ph >= 1 ? sl_spoon_lerp(8, 26, d) : 0, tray = ph >= 1 ? sl_spoon_lerp(10, 32, d) : 3;
    if (m > 30 && r < 4) type = SP_JELLY;
    else if (r < 4 + cracker) type = SP_CRACKER;
    else if (r < 4 + cracker + tray) type = SP_TRAY;
    sl_spoon_spine(s, gap, type, sl_spoon_width(s), sl_spoon_frand(s, -200, 200));
    sl_spoon_extras(s, below_y);
}

/* ---- starting ------------------------------------------------------------------ */
static void sl_spoon_init(struct sl_spoon_state *s, uint32_t seed)
{
    *s = (struct sl_spoon_state){0};
    s->rng = seed ? seed : 1;
    s->alive = true;
    s->mult = 1;
    s->x = SL_SPOON_W / 2; s->y = 0; s->vy = -SL_SPOON_JUMP * 0.45f;
    s->cam = -(SL_SPOON_H - 92);
    s->soup = 74;
    s->last_plate = -1;
    s->next_event = 16;
    s->next_pattern = 6 + sl_spoon_rand(s) % 4;
    s->best_height = -1;
    /* the rim of the pot, wall to wall */
    struct sl_spoon_plat *p = sl_spoon_add_plat(s, SP_PLATE, SL_SPOON_W / 2, 0, SL_SPOON_W - 12);
    p->spine = 1; p->used = 1;
    s->gen_y = 0; s->gen_x = SL_SPOON_W / 2;
    while (s->gen_y > s->cam - SL_SPOON_H * 0.7f) sl_spoon_gen_unit(s);
}
static void sl_spoon_start(struct sl_spoon_state *s)
{
    if (s->started || !s->alive) return;
    s->started = true;
    s->time = 0;
}

/* ---- events ----------------------------------------------------------------- */
static void sl_spoon_meatball(struct sl_spoon_state *s, float x)
{
    struct sl_spoon_foe *f = sl_spoon_add_foe(s, SF_MEATBALL);
    if (!f) return;
    f->x = sl_spoon_clampf(x, 24, SL_SPOON_W - 24);
    f->y = s->cam - 20;
    f->t = 0;
    sl_spoon_fx(s, SX_DROP, f->x, s->cam + 20, 0);
}
static void sl_spoon_events(struct sl_spoon_state *s, float dt)
{
    if (s->calm || s->phase < 1) return;
    if (s->event_state == SE_OFF)
    {
        if (s->time < s->next_event) return;
        int allowed[SE_COUNT], n = 0;
        allowed[n++] = SE_WIND;
        allowed[n++] = SE_COINS;
        if (s->phase >= 2) allowed[n++] = SE_LOWG;
        if (s->phase >= 3) allowed[n++] = SE_MEATBALLS, allowed[n++] = SE_MEATBALLS;
        if (s->phase >= 4) allowed[n++] = SE_DARK;
        int e;
        do e = allowed[sl_spoon_rand(s) % n]; while (e == s->event && n > 1 && sl_spoon_chance(s, 80));
        s->event = e;
        s->event_state = SE_WARN;
        s->event_t = 1.4f;
        if (e == SE_WIND) s->wind = sl_spoon_rand(s) & 1 ? 1 : -1;
        sl_spoon_fx(s, SX_WARN, SL_SPOON_W / 2, s->cam + SL_SPOON_H / 2, e | (s->wind + 1) << 8);   /* wind: (v >> 8) - 1 */
        return;
    }
    s->event_t -= dt;
    if (s->event_state == SE_WARN && s->event_t <= 0)
    {
        static const float dur[SE_COUNT] = { 0, 6, 7, 4.5f, 6, 0.1f };
        s->event_state = SE_ON;
        s->event_t = dur[s->event];
        s->event_spawn = 0;
        sl_spoon_fx(s, SX_EVENT, SL_SPOON_W / 2, s->cam + SL_SPOON_H / 2, s->event);
        if (s->event == SE_COINS)       /* a winding ribbon of O's up ahead */
            for (int i = 0; i < 14; i++)
                sl_spoon_add_item(s, SI_O, SL_SPOON_W / 2 + 150 * sinf(i * 0.55f + s->time),
                                  s->y - 150 - i * 46);
    }
    else if (s->event_state == SE_ON)
    {
        if (s->event == SE_MEATBALLS && (s->event_spawn -= dt) <= 0)
        {
            /* about half of them aimed at the spoon */
            s->event_spawn = 0.85f;
            sl_spoon_meatball(s, sl_spoon_rand(s) & 1 ? s->x + sl_spoon_frand(s, -20, 20) : sl_spoon_frand(s, 30, SL_SPOON_W - 30));
        }
        if (s->event_t <= 0)
        {
            sl_spoon_fx(s, SX_EVENT_END, SL_SPOON_W / 2, s->cam + SL_SPOON_H / 2, s->event);
            s->event_state = SE_OFF;
            s->event = SE_NONE;
            s->wind = 0;
            s->next_event = s->time + sl_spoon_frand(s, 13, 22) - s->phase;
        }
    }
}
static float sl_spoon_gravity(const struct sl_spoon_state *s)
{ return s->event == SE_LOWG && s->event_state == SE_ON ? SL_SPOON_G * 0.55f : SL_SPOON_G; }
static float sl_spoon_bounce(const struct sl_spoon_state *s, float v)
{ return s->event == SE_LOWG && s->event_state == SE_ON ? v * 0.78f : v; }

/* ---- scoring ------------------------------------------------------------------- */
static void sl_spoon_points(struct sl_spoon_state *s, int pts) { s->score += pts * s->mult; }
static void sl_spoon_combo(struct sl_spoon_state *s, bool perfect, float x, float y)
{
    if (perfect)
    {
        s->combo++;
        if (s->combo > s->best_combo) s->best_combo = s->combo;
        int m = sl_spoon_mult(s->combo);
        sl_spoon_fx(s, SX_PERFECT, x, y, s->combo);
        if (m > s->mult) sl_spoon_fx(s, SX_MULT, x, y, m);
        s->mult = m;
        if (s->combo >= 15) sl_spoon_secret(s, SS_SWEET);
        return;
    }
    if (s->combo >= 3) sl_spoon_fx(s, SX_COMBO_LOST, x, y, s->combo);
    s->combo = 0;
    s->mult = 1;
}

/* ---- dying ------------------------------------------------------------------------ */
static void sl_spoon_die(struct sl_spoon_state *s, int cause)
{
    s->alive = false;
    s->death = cause;
    s->whisk = 0;
    if (cause != SD_SOUP) { s->vy = -520; s->vx *= 0.3f; }
    if (s->started && s->time < 4) sl_spoon_secret(s, SS_BUTTER);
    sl_spoon_fx(s, SX_DIE, s->x, s->y - SL_SPOON_BOWL, cause);
}
/* a hit: the lid takes it if there is one; true if the spoon survived */
static bool sl_spoon_hit(struct sl_spoon_state *s, int cause)
{
    if (s->whisk > 0 || s->invuln > 0) return true;
    if (s->lid)
    {
        s->lid = false;
        s->invuln = 1.0f;
        if (s->vy > -500) s->vy = -560;
        sl_spoon_fx(s, SX_SHIELD, s->x, s->y - SL_SPOON_BOWL, 0);
        return true;
    }
    sl_spoon_die(s, cause);
    return false;
}

/* ---- one step ---------------------------------------------------------------------- */
static void sl_spoon_land(struct sl_spoon_state *s, int i)
{
    struct sl_spoon_plat *p = &s->plat[i];
    float cx = p->x + p->w / 2;
    if (p->type == SP_SOGGY)
    {
        p->broken = 1; p->t = 0;
        sl_spoon_fx(s, SX_SOGGY, cx, p->y, 0);
        return;                             /* straight through */
    }
    s->y = p->y;
    s->vy = -sl_spoon_bounce(s, p->type == SP_JELLY ? SL_SPOON_JELLY : SL_SPOON_JUMP);
    if (!s->started) s->vy *= 0.45f;       /* waiting: little hops on the rim */
    p->bump = 0;
    s->land_plate = i;
    bool perfect = false;
    if (!p->used)
    {
        p->used = 1;
        float tol = p->w * 0.14f < 10 ? 10 : p->w * 0.14f;
        perfect = fabsf(sl_spoon_wdx(cx, s->x)) <= tol || s->freaky > 0;
        sl_spoon_combo(s, perfect, s->x, s->y);
    }
    if (i == s->last_plate) { if (++s->same_plate >= 8) sl_spoon_secret(s, SS_LOYAL); }
    else s->same_plate = 1;
    s->last_plate = i;
    if (s->soup - s->y < 34 && s->started && s->height >= 50) sl_spoon_secret(s, SS_HOT_FEET);
    if (p->type == SP_CRACKER) { p->broken = 1; p->t = 0; sl_spoon_fx(s, SX_CRACK, cx, p->y, 0); }
    else if (p->type == SP_JELLY)
    {
        if (++s->jellies >= 5) sl_spoon_secret(s, SS_JELLY);
        sl_spoon_fx(s, SX_JELLY, s->x, p->y, 0);
    }
    else if (!perfect) sl_spoon_fx(s, SX_LAND, s->x, p->y, p->type);
}
static float sl_spoon_soup_speed(const struct sl_spoon_state *s)
{
    static const float v[SL_SPOON_PHASES] = { 10, 24, 38, 52, 66, 82 };
    float extra = s->height > 1200 ? (s->height - 1200) * 0.04f : 0;
    return v[s->phase] + (extra > 28 ? 28 : extra);
}
static void sl_spoon_foes(struct sl_spoon_state *s, float dt)
{
    float bx = s->x, by = s->y - SL_SPOON_BOWL;
    for (int i = 0; i < SL_SPOON_FOES; i++)
    {
        struct sl_spoon_foe *f = &s->foe[i];
        if (f->type == SF_NONE) continue;
        f->t += dt;
        if (f->type == SF_FORK)
        {
            if (f->state)                   /* knocked: tumbles away */
            {
                f->vy += SL_SPOON_G * dt;
                f->x += f->vx * dt; f->y += f->vy * dt;
            }
            else
            {
                f->x += f->vx * dt;
                if (f->x < f->x0) { f->x = f->x0; f->vx = -f->vx; }
                if (f->x > f->x1) { f->x = f->x1; f->vx = -f->vx; }
                f->y = f->y0 + 6 * sinf(f->t * 4);
                float dx = sl_spoon_wdx(f->x, s->x);
                if (s->alive && s->vy > 0 && s->whisk <= 0 && s->y >= f->y - 16 && s->y <= f->y + 6 && fabsf(dx) < 30)
                {
                    f->state = 1; f->vy = -200; f->vx = dx > 0 ? -70 : 70;
                    s->y = f->y - 16;
                    s->vy = -sl_spoon_bounce(s, SL_SPOON_STOMP);
                    s->stomps++;
                    sl_spoon_combo(s, true, s->x, s->y);
                    sl_spoon_points(s, 30);
                    sl_spoon_fx(s, SX_STOMP, f->x, f->y, 30 * s->mult);
                    if (s->stomps >= 3) sl_spoon_secret(s, SS_FORK_OFF);
                }
                else if (s->alive)
                {
                    float qx = sl_spoon_clampf(dx, -22, 22), qy = sl_spoon_clampf(by - f->y, -9, 9);
                    float ex = dx - qx, ey = by - f->y - qy;
                    if (ex * ex + ey * ey < SL_SPOON_BOWL_R * SL_SPOON_BOWL_R && sl_spoon_hit(s, SD_FORK))
                    {
                        f->state = 1; f->vy = -150; f->vx = dx > 0 ? -90 : 90;
                        sl_spoon_fx(s, SX_KNOCK, f->x, f->y, SF_FORK);
                    }
                }
            }
        }
        else if (f->type == SF_MEATBALL)
        {
            if (f->state == 0)              /* the warning, pinned to the top */
            {
                f->y = s->cam - 20;
                if (f->t >= 1.0f) { f->state = 1; f->vy = 540; }
            }
            else
            {
                f->y += f->vy * dt;
                float dx = sl_spoon_wdx(f->x, bx), dy = f->y - by, r = SL_SPOON_BOWL_R + 12;
                if (s->alive && dx * dx + dy * dy < r * r && sl_spoon_hit(s, SD_MEATBALL))
                {
                    sl_spoon_fx(s, SX_KNOCK, f->x, f->y, SF_MEATBALL);
                    f->type = SF_NONE;
                    continue;
                }
            }
        }
        else if (f->type == SF_GOLDEN)
        {
            f->x += f->vx * dt;
            f->y = f->y0 + 26 * sinf(f->t * 3);
            float dx = f->x - bx, dy = f->y - by;
            if (s->alive && dx * dx + dy * dy < 34 * 34)
            {
                s->score += 300;
                sl_spoon_fx(s, SX_GOLDEN, f->x, f->y, 300);
                sl_spoon_secret(s, SS_GOLDEN);
                f->type = SF_NONE;
                continue;
            }
            if (f->x < -60 || f->x > SL_SPOON_W + 60) { f->type = SF_NONE; continue; }
        }
        if (f->y > s->cam + SL_SPOON_H + 80) f->type = SF_NONE;
    }
}
static void sl_spoon_items(struct sl_spoon_state *s)
{
    float by = s->y - SL_SPOON_BOWL;
    for (int i = 0; i < SL_SPOON_ITEMS; i++)
    {
        struct sl_spoon_item *it = &s->item[i];
        if (!it->live) continue;
        if (it->y > s->cam + SL_SPOON_H + 60) { it->live = 0; continue; }
        float dx = sl_spoon_wdx(it->x, s->x), dy = it->y - by, r = it->type == SI_O ? 26 : 32;
        if (dx * dx + dy * dy >= r * r) continue;
        it->live = 0;
        if (it->type == SI_O)
        {
            s->coins++;
            sl_spoon_points(s, 5);
            sl_spoon_fx(s, SX_COIN, it->x, it->y, 5 * s->mult);
            continue;
        }
        if (it->type == SI_WHISK) s->whisk = SL_SPOON_WHISK_T;
        else if (it->type == SI_LID) s->lid = true;
        else if (it->type == SI_CHILI) s->freaky = SL_SPOON_CHILI_T;
        sl_spoon_fx(s, SX_POWER, it->x, it->y, it->type);
    }
}
static void sl_spoon_step(struct sl_spoon_state *s)
{
    const float dt = SL_SPOON_DT;
    if (!s->alive)
    {
        /* the spoon tumbles into the soup; the world holds still */
        float was = s->y - SL_SPOON_BOWL;
        s->vy += SL_SPOON_G * dt;
        if (s->vy > 1100) s->vy = 1100;
        s->y += s->vy * dt;
        s->x += s->vx * dt;
        if (was <= s->soup && s->y - SL_SPOON_BOWL > s->soup && s->death != SD_SOUP)
            sl_spoon_fx(s, SX_SPLASH, s->x, s->soup, 0);
        return;
    }
    if (s->started) s->time += dt;
    if (s->whisk > 0 && (s->whisk -= dt) <= 0) s->invuln = 0.4f;
    if (s->freaky > 0) s->freaky -= dt;
    if (s->invuln > 0) s->invuln -= dt;

    /* steering: quick to start, quicker to turn round */
    int dir = s->started ? s->dir : 0;
    float target = dir * SL_SPOON_VMAX;
    float acc = !dir ? SL_SPOON_DEC : s->vx * dir < 0 ? SL_SPOON_ACC * 1.8f : SL_SPOON_ACC;
    if (s->vx < target) { s->vx += acc * dt; if (s->vx > target) s->vx = target; }
    else if (s->vx > target) { s->vx -= acc * dt; if (s->vx < target) s->vx = target; }
    float wind = s->event == SE_WIND && s->event_state == SE_ON ? s->wind * 115.f : 0;
    s->x += (s->vx + wind) * dt;
    if (s->x < 0 || s->x >= SL_SPOON_W)
    {
        s->x += s->x < 0 ? SL_SPOON_W : -SL_SPOON_W;
        if (++s->wraps >= 15) sl_spoon_secret(s, SS_DIZZY);
    }

    float old = s->y;
    if (s->whisk > 0) s->vy = -950;
    else
    {
        s->vy += sl_spoon_gravity(s) * dt;
        if (s->vy > 1100) s->vy = 1100;
    }
    s->y += s->vy * dt;
    if (s->vy > 0)
    {
        int hit = -1;
        for (int i = 0; i < SL_SPOON_PLATS; i++)
        {
            const struct sl_spoon_plat *p = &s->plat[i];
            if (!sl_spoon_solid(p) || old > p->y || s->y < p->y) continue;
            if (!s->started && i) continue;     /* waiting: only the rim */
            if (fabsf(sl_spoon_wdx(p->x + p->w / 2, s->x)) > p->w / 2 + SL_SPOON_FEET) continue;
            if (hit < 0 || p->y < s->plat[hit].y) hit = i;
        }
        if (hit >= 0) sl_spoon_land(s, hit);
    }

    for (int i = 0; i < SL_SPOON_PLATS; i++)
    {
        struct sl_spoon_plat *p = &s->plat[i];
        if (!p->live) continue;
        p->bump += dt;
        if (p->broken)
        {
            p->t += dt;
            p->y += (120 + 900 * p->t) * dt;
        }
        else if (p->type == SP_TRAY)
        {
            p->x += p->vx * dt;
            if (p->x < p->x0) { p->x = p->x0; p->vx = -p->vx; }
            if (p->x > p->x1) { p->x = p->x1; p->vx = -p->vx; }
        }
        else if (p->type == SP_BLINK) p->t += dt;
        if (p->y > s->cam + SL_SPOON_H + 80) p->live = 0;
    }
    sl_spoon_foes(s, dt);
    if (!s->alive) return;
    sl_spoon_items(s);

    /* height, score and phases count once the run has started */
    if (s->started && s->y < s->top_y)
    {
        s->top_y = s->y;
        int m = sl_spoon_metres(s->top_y);
        if (m > s->height)
        {
            s->score += (m - s->height) * s->mult;
            s->height = m;
            int ph = sl_spoon_phase_of(m);
            if (ph > s->phase)
            {
                s->phase = ph;
                sl_spoon_fx(s, SX_PHASE, s->x, s->y, ph);
                if (ph == SL_SPOON_PHASES - 1) sl_spoon_secret(s, SS_FREAKY);
            }
            if (m >= 300 && !s->coins) sl_spoon_secret(s, SS_DIET);
            if (s->best_height > 0 && !s->passed_best && m > s->best_height)
            {
                s->passed_best = true;
                sl_spoon_fx(s, SX_BEST, s->x, s->y, m);
            }
        }
    }
    float target_cam = s->y - SL_SPOON_H * 0.45f;
    if (target_cam < s->cam) s->cam = target_cam;

    if (s->started) s->soup -= sl_spoon_soup_speed(s) * dt;
    if (s->soup > s->cam + SL_SPOON_H + 40) s->soup = s->cam + SL_SPOON_H + 40;
    if (s->y - SL_SPOON_BOWL > s->soup)
    {
        sl_spoon_fx(s, SX_SPLASH, s->x, s->soup, 1);
        sl_spoon_die(s, SD_SOUP);
        return;
    }
    if (s->started)
    {
        sl_spoon_events(s, dt);
        /* up in space, the odd meatball between events */
        if (!s->calm && s->phase >= 4 && s->event_state == SE_OFF && (s->event_spawn -= dt) <= 0)
        {
            if (s->event_spawn > -1) sl_spoon_meatball(s, sl_spoon_frand(s, 30, SL_SPOON_W - 30));
            s->event_spawn = sl_spoon_frand(s, 5, 9) - (s->phase - 4) * 1.5f;
        }
    }
    while (s->gen_y > s->cam - SL_SPOON_H * 0.7f) sl_spoon_gen_unit(s);
}
#endif
