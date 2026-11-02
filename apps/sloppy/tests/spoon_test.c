#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* every spine platform is checked against the one before it as it is made */
static int trace_bad, trace_steps;
static float worst_gap, worst_use;
#define SL_SPOON_TRACE(s, p, y, cx, slack) do { \
        float gap_ = (s)->gen_y - (y), use_ = fabsf(sl_spoon_wdx((s)->gen_x, (cx))) + (s)->gen_slack + (slack); \
        float reach_ = sl_spoon_reach(gap_); \
        trace_steps++; \
        if (gap_ > worst_gap) worst_gap = gap_; \
        if (reach_ > 0 && use_ / reach_ > worst_use) worst_use = use_ / reach_; \
        if (!(p) || gap_ > SL_SPOON_MAX_GAP + 0.01f || use_ > reach_ + 0.5f) trace_bad++; \
    } while (0)
#include "../sloppy_spoon_state.h"

static void step_n(struct sl_spoon_state *s, int n)
{
    for (int i = 0; i < n; i++) { s->fxn = 0; sl_spoon_step(s); }
}
static bool had_fx(const struct sl_spoon_state *s, int kind)
{
    for (int i = 0; i < s->fxn; i++) if (s->fxq[i].kind == kind) return true;
    return false;
}
/* run until an effect happens, keeping the effects of that step */
static bool step_until(struct sl_spoon_state *s, int kind, int max)
{
    for (int i = 0; i < max; i++)
    {
        s->fxn = 0;
        sl_spoon_step(s);
        if (had_fx(s, kind)) return true;
    }
    return false;
}
static int empty_plat(struct sl_spoon_state *s)
{
    for (int i = 0; i < SL_SPOON_PLATS; i++) if (!s->plat[i].live) return i;
    return -1;
}
/* drop the spoon onto a fresh platform of a type, dx off its centre */
static struct sl_spoon_plat *drop_on(struct sl_spoon_state *s, int type, float dx)
{
    for (int j = 0; j < SL_SPOON_PLATS; j++) s->plat[j].live = 0;   /* nothing else to land on */
    int i = empty_plat(s);
    assert(i >= 0);
    struct sl_spoon_plat *p = &s->plat[i];
    *p = (struct sl_spoon_plat){ .x = 180, .y = s->y - 300, .w = 80, .type = (uint8_t)type, .live = 1, .bump = 9 };
    p->x0 = p->x1 = p->x;
    s->x = 220 + dx; s->y = p->y - 3; s->vy = 200; s->vx = 0; s->dir = 0;
    return p;
}
/* A bot that aims for the middle of the next spine platform up. It knows
 * nothing about hazards, so with them on it shows how they bite. */
static float bot_floor;
static void bot_steer(struct sl_spoon_state *s)
{
    int best = -1;
    /* falling from high up (a jelly, a whisk): the first platform under it */
    if (s->vy > 0)
        for (int i = 0; i < SL_SPOON_PLATS; i++)
        {
            const struct sl_spoon_plat *p = &s->plat[i];
            if (!p->live || p->broken || !p->spine || p->y < s->y + 4 || p->y >= bot_floor - 4) continue;
            float drop = p->y - s->y, t = (-s->vy + sqrtf(s->vy * s->vy + 2 * SL_SPOON_G * drop)) / SL_SPOON_G;
            if (fabsf(sl_spoon_wdx(s->x, p->x + p->w / 2)) > SL_SPOON_VMAX * (t - 0.1f) + p->w / 2) continue;
            if (best < 0 || p->y < s->plat[best].y) best = i;
        }
    for (int i = 0, falling = best >= 0; !falling && i < SL_SPOON_PLATS; i++)
    {
        const struct sl_spoon_plat *p = &s->plat[i];
        if (!p->live || p->broken || !p->spine || p->y >= bot_floor - 4) continue;
        if (best < 0 || p->y > s->plat[best].y) best = i;
    }
    if (best < 0) { s->dir = 0; return; }
    const struct sl_spoon_plat *p = &s->plat[best];
    float dx = sl_spoon_wdx(s->x, p->x + p->w / 2 + p->vx * 0.15f);
    s->dir = dx > 5 ? 1 : dx < -5 ? -1 : 0;
}
static int bot_run(uint32_t seed, bool calm, int goal_m, int *death)
{
    static struct sl_spoon_state s;
    sl_spoon_init(&s, seed);
    s.calm = calm;
    sl_spoon_start(&s);
    bot_floor = 0;
    for (int t = 0; t < 120 * 60 * 12 && s.alive && s.height < goal_m; t++)
    {
        bot_steer(&s);
        float vy = s.vy;
        s.fxn = 0;
        sl_spoon_step(&s);
        if (vy > 0 && s.vy < 0) bot_floor = s.y;
        assert(s.gen_dropped == 0);
    }
    *death = s.alive ? 0 : s.death;
    return s.height;
}

int main(void)
{
    struct sl_spoon_state s;

    /* the start: bouncing on the rim of the pot, nothing moves the soup */
    sl_spoon_init(&s, 9);
    assert(s.alive && !s.started && s.plat[0].w > 400);
    float soup = s.soup;
    step_n(&s, 600);
    assert(s.alive && s.soup == soup && s.height == 0 && s.y <= 0.01f);
    s.dir = 1;
    step_n(&s, 30);
    assert(fabsf(s.vx) < 1e-3f);                /* no steering until started */
    sl_spoon_start(&s);
    step_n(&s, 30);
    assert(s.vx > 300);                         /* reaches speed in a quarter second */
    s.dir = -1;
    step_n(&s, 12);
    assert(s.vx < 0);                           /* turns round in a tenth */

    /* perfect landings build a combo and a multiplier; a sloppy one ends it */
    sl_spoon_init(&s, 3);
    sl_spoon_start(&s);
    for (int i = 0; i < 6; i++)
    {
        drop_on(&s, SP_PLATE, 1);
        assert(step_until(&s, SX_PERFECT, 20));
    }
    assert(s.combo == 6 && s.mult == 3 && s.best_combo == 6);
    drop_on(&s, SP_PLATE, 30);
    assert(step_until(&s, SX_COMBO_LOST, 20) && s.combo == 0 && s.mult == 1 && s.best_combo == 6);
    /* landing on the same plate again changes nothing */
    struct sl_spoon_plat *p = drop_on(&s, SP_PLATE, 0);
    assert(step_until(&s, SX_PERFECT, 20) && s.combo == 1);
    p->used = 1; s.y = p->y - 3; s.vy = 200; s.x = p->x + p->w / 2 + 30;
    assert(step_until(&s, SX_LAND, 20) && s.combo == 1);

    /* soggy toast lets go, a cracker breaks, jelly launches */
    p = drop_on(&s, SP_SOGGY, 0);
    assert(step_until(&s, SX_SOGGY, 20) && p->broken && s.vy > 0);
    p = drop_on(&s, SP_CRACKER, 0);
    assert(step_until(&s, SX_CRACK, 20) && p->broken && s.vy < 0);
    p = drop_on(&s, SP_JELLY, 0);
    assert(step_until(&s, SX_JELLY, 20) && s.vy < -SL_SPOON_JUMP * 1.4f);
    /* a blink plate is only there when it is lit */
    p = drop_on(&s, SP_BLINK, 0);
    p->t = 2.0f;                                /* dark */
    step_n(&s, 10);
    assert(s.vy > 0 && s.y > p->y);

    /* forks: stomp from above, get hurt from the side; a lid saves once */
    sl_spoon_init(&s, 4);
    sl_spoon_start(&s);
    struct sl_spoon_foe *f = sl_spoon_add_foe(&s, SF_FORK);
    f->x = 200; f->x0 = 0; f->x1 = 400; f->vx = 0; f->y = f->y0 = s.y - 200;
    s.x = 200; s.y = f->y - 20; s.vy = 300;
    assert(step_until(&s, SX_STOMP, 20) && f->state == 1 && s.vy < 0 && s.alive && s.stomps == 1 && s.combo == 1);
    f = sl_spoon_add_foe(&s, SF_FORK);
    f->x = 200; f->x0 = 0; f->x1 = 400; f->vx = 0; f->y = f->y0 = s.y - 300;
    s.x = 200; s.y = f->y + SL_SPOON_BOWL + 20; s.vy = -600; s.lid = true;
    assert(step_until(&s, SX_SHIELD, 20) && s.alive && !s.lid);
    f->state = 0; f->t = 0; s.invuln = 0; s.x = 200; s.y = f->y + SL_SPOON_BOWL + 20; s.vy = -600;
    assert(step_until(&s, SX_DIE, 20) && !s.alive && s.death == SD_FORK);

    /* a meatball warns for a second, then falls */
    sl_spoon_init(&s, 5);
    sl_spoon_start(&s);
    s.fxn = 0;
    sl_spoon_meatball(&s, s.x);
    assert(had_fx(&s, SX_DROP));
    step_n(&s, 60);
    assert(s.alive);
    assert(step_until(&s, SX_DIE, 240) && s.death == SD_MEATBALL);
    assert(s.secrets & 1u << SS_BUTTER);        /* within four seconds */

    /* the whisk flies through everything */
    sl_spoon_init(&s, 6);
    sl_spoon_start(&s);
    sl_spoon_add_item(&s, SI_WHISK, s.x, s.y - SL_SPOON_BOWL);
    step_n(&s, 1);
    assert(s.whisk > 0);
    float y0 = s.y;
    step_n(&s, 120);
    assert(s.y < y0 - 900 && s.alive);

    /* stand still long enough and the soup gets you */
    sl_spoon_init(&s, 7);
    sl_spoon_start(&s);
    s.calm = true;
    for (int i = 0; i < 120 * 90 && s.alive; i++) { s.fxn = 0; sl_spoon_step(&s); }
    assert(!s.alive && s.death == SD_SOUP);

    /* falling off the bottom is also the soup */
    sl_spoon_init(&s, 8);
    sl_spoon_start(&s);
    s.x = 10; s.y = s.cam + SL_SPOON_H - 10; s.vy = 900;
    for (int i = 0; i < 60; i++) s.plat[0].live = 0;
    assert(step_until(&s, SX_DIE, 200) && s.death == SD_SOUP);

    /* the screen wraps */
    sl_spoon_init(&s, 10);
    sl_spoon_start(&s);
    s.x = 2; s.dir = -1; s.vx = -390;
    step_n(&s, 2);
    assert(s.x > SL_SPOON_W - 20 && s.wraps == 1);

    /* same seed, same steering, same run */
    struct sl_spoon_state a, b;
    sl_spoon_init(&a, 77); sl_spoon_init(&b, 77);
    sl_spoon_start(&a); sl_spoon_start(&b);
    for (int i = 0; i < 4000; i++)
    {
        a.dir = b.dir = (i / 90) % 3 - 1;
        a.fxn = b.fxn = 0;
        sl_spoon_step(&a); sl_spoon_step(&b);
    }
    assert(a.x == b.x && a.y == b.y && a.score == b.score && a.alive == b.alive);

    /* Reachability: with hazards off, a simple bot must climb to 1500 m.
     * Every spine step it passes is checked by the trace hook above. */
    int ok = 0, runs = 300, death;
    for (int i = 0; i < runs; i++)
        if (bot_run(1000 + i * 7919, true, 1500, &death) >= 1500) ok++;
    printf("calm bot: %d/%d reached 1500 m; %d spine steps, worst gap %.0f px, worst reach use %.0f%%\n",
           ok, runs, trace_steps, worst_gap, worst_use * 100);
    assert(trace_bad == 0);
    assert(ok * 100 >= runs * 97);

    /* with hazards the same bot dies, and that is the point: report where */
    int buckets[SL_SPOON_PHASES] = {0}, causes[4] = {0}, total = 0;
    for (int i = 0; i < 200; i++)
    {
        int h = bot_run(5000 + i * 104729, false, 3000, &death);
        buckets[sl_spoon_phase_of(h)]++;
        causes[death]++;
        total += h;
    }
    printf("hazard bot: mean %d m; ended in phase", total / 200);
    for (int i = 0; i < SL_SPOON_PHASES; i++) printf(" %d", buckets[i]);
    printf("; soup %d fork %d meatball %d\n", causes[SD_SOUP], causes[SD_FORK], causes[SD_MEATBALL]);
    assert(trace_bad == 0);

    puts("PASS: spoon steering, combos, platforms, forks, lid, meatballs, whisk, soup, wrap, determinism, reachability");
    return 0;
}
