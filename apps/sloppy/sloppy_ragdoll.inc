/* A Verlet ragdoll: grab and toss in free play, dodge knives in Knives.
 * Kept small for the R1 CPU. */
#include <math.h>

#define RD_N 10
#define RD_E 9
#define RD_NMAX 32                  /* every cut adds two joints and a bone */
#define RD_EMAX 24
static struct { float x, y, px, py; } rd_nodes[RD_NMAX];
static float rd_fx[RD_NMAX], rd_fy[RD_NMAX];   /* pull this step (Knives string and pad) */
static int rd_count = RD_N, rd_ecount = RD_E;
static float rd_grav = .28f;
static const struct { unsigned char a, b; float len; } rd_edges[RD_E] = {
    {0,1,40}, {1,2,62}, {2,3,47}, {1,4,62}, {4,5,47},
    {1,6,75}, {6,7,70}, {6,8,70}, {8,9,45}
};
/* the bones now: a cut splits one in two, and each half remembers which
 * bone it came from (kind) */
static unsigned char rd_ea[RD_EMAX], rd_eb[RD_EMAX], rd_kind[RD_EMAX];
static float rd_len[RD_EMAX];
static lv_obj_t *rd_scene, *rd_head, *rd_face, *rd_lines[RD_E];
static lv_point_precise_t rd_points[RD_E][2];
static lv_timer_t *rd_timer;
static int rd_grab = -1;
static float rd_last_x, rd_last_y;
static float rd_impact;             /* hardest landing this step, px per frame */
static uint32_t rd_thud_at;

static void rd_reset(void)
{
    static const short xy[RD_N][2] = {
        {180,86},{180,126},{130,163},{96,195},{230,163},
        {264,195},{180,201},{140,264},{220,264},{238,306}
    };
    for (int i = 0; i < RD_N; i++)
    {
        rd_nodes[i].x = rd_nodes[i].px = xy[i][0];
        rd_nodes[i].y = rd_nodes[i].py = xy[i][1];
    }
    for (int e = 0; e < RD_E; e++)
    {
        rd_ea[e] = rd_edges[e].a;
        rd_eb[e] = rd_edges[e].b;
        rd_len[e] = rd_edges[e].len;
        rd_kind[e] = e;
    }
    rd_count = RD_N;
    rd_ecount = RD_E;
    rd_grab = -1;
}

static void rd_step(void)
{
    for (int i = 0; i < rd_count; i++)
    {
        if (i == rd_grab) continue;
        float vx = (rd_nodes[i].x - rd_nodes[i].px) * .985f;
        float vy = (rd_nodes[i].y - rd_nodes[i].py) * .985f;
        rd_nodes[i].px = rd_nodes[i].x;
        rd_nodes[i].py = rd_nodes[i].y;
        rd_nodes[i].x += vx + rd_fx[i];
        rd_nodes[i].y += vy + rd_grav + rd_fy[i];
        float hit = rd_nodes[i].y > 358 ? vy : rd_nodes[i].x < 16 ? -vx :
                    rd_nodes[i].x > W - 44 ? vx : 0;
        if (hit > rd_impact) rd_impact = hit;
    }
    for (int pass = 0; pass < 4; pass++)
    {
        for (int e = 0; e < rd_ecount; e++)
        {
            int a = rd_ea[e], b = rd_eb[e];
            float dx = rd_nodes[b].x - rd_nodes[a].x;
            float dy = rd_nodes[b].y - rd_nodes[a].y;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist < 1.f) continue;
            float delta = (dist - rd_len[e]) / dist;
            float share_a = a == rd_grab ? 0.f : b == rd_grab ? 1.f : .5f;
            float share_b = b == rd_grab ? 0.f : a == rd_grab ? 1.f : .5f;
            rd_nodes[a].x += dx * delta * share_a;
            rd_nodes[a].y += dy * delta * share_a;
            rd_nodes[b].x -= dx * delta * share_b;
            rd_nodes[b].y -= dy * delta * share_b;
        }
        for (int i = 0; i < rd_count; i++)
        {
            if (i == rd_grab) continue;
            if (rd_nodes[i].x < 16) rd_nodes[i].x = 16;
            if (rd_nodes[i].x > W - 44) rd_nodes[i].x = W - 44;
            if (rd_nodes[i].y < 20) rd_nodes[i].y = 20;
            if (rd_nodes[i].y > 358) rd_nodes[i].y = 358;
        }
    }
}

/* ---- Knives: after Ragdoll Avalanche II. Steer the doll (the cog picks
 * how: grab a limb as in free play, pull it on a string, or an arrow
 * pad) while knives rain down faster and faster. A knife cuts a limb where it crosses it: the stump
 * stays on and bleeds, the rest drops off. Through the head, the neck or
 * the body the doll is done, in slow motion. The score is the number of
 * knives that reached the floor while the doll was still alive. */
#define KN_MAX 24
#define KN_DROPS 160
#define KN_STAINS 40
#define KN_BLEEDS 16
#define KN_STEP 25                  /* ms of play per physics step */
#define KN_FLOOR 374                /* where a knife tip sticks */
#define KN_BLADE 34
static struct { float x, y, vy; short stuck; bool live, spent; } kn[KN_MAX];
static struct { float x, y, vx, vy; unsigned char size; bool live; } kn_drop[KN_DROPS];
static struct { short x, w; unsigned char opa; } kn_stain[KN_STAINS];
static struct { unsigned char node, from; short left; } kn_bleed[KN_BLEEDS];
enum { KN_GRAB, KN_STRING, KN_PAD };
enum { KN_L = 1, KN_R = 2, KN_U = 4, KN_D = 8 };
static int kn_ctrl;                     /* picked under the cog, kept while the OS runs */
static unsigned kn_dir;                 /* pad directions held */
static bool kn_on, kn_dead, kn_over, kn_tap, kn_drag, kn_swallow;
static float kn_tx, kn_ty, kn_ox, kn_oy;    /* string target and finger offset */
static int kn_score, kn_best, kn_shown = -1, kn_dropped, kn_nstain, kn_stain_ring;
static unsigned kn_wounds, kn_bloody;   /* cut ends to paint red; knives that cut */
static uint32_t kn_t, kn_next, kn_last, kn_dead_at, kn_seed = 1;   /* kn_t, kn_dead_at: play ms */
static float kn_acc, kn_slow = 1.f, kn_last_x[2];
static float kn_shake, kn_flash;
static bool kn_full, kn_stains_moved;   /* redraw the whole box next frame; a stain grew */
static lv_area_t kn_was_doll;           /* what was drawn around the doll last frame */
static short kn_was_x[KN_MAX], kn_was_y[KN_MAX];   /* each knife's tip last frame, y -1 if not drawn */
static int kn_sx, kn_sy;
static lv_obj_t *kn_score_l, *kn_over_box, *kn_over_t, *kn_over_s;
static lv_obj_t *kn_pad, *kn_arrows[4], *kn_cog, *kn_menu, *kn_ticks[3];
static lv_obj_t *rd_hint, *rd_reset_row, *rd_mode_row;

static void kn_arrows_show(unsigned dir)
{
    static const unsigned bit[4] = { KN_L, KN_U, KN_D, KN_R };
    for (int i = 0; i < 4; i++)
        lv_obj_set_style_text_color(kn_arrows[i], lv_color_hex(dir & bit[i] ? 0x8bd0d2 : C_SUB), 0);
}

static uint32_t kn_rand(uint32_t n)
{
    kn_seed = kn_seed * 1103515245u + 12345u;
    return (kn_seed >> 8) % n;
}

/* Every joint still joined to the chest (node 1). */
static unsigned kn_attached(void)
{
    unsigned m = 1u << 1, was = 0;
    while (m != was)
    {
        was = m;
        for (int e = 0; e < rd_ecount; e++)
            if ((m >> rd_ea[e] | m >> rd_eb[e]) & 1) m |= 1u << rd_ea[e] | 1u << rd_eb[e];
    }
    return m;
}

static void kn_emit(float x, float y, float vx, float vy)
{
    for (int i = 0; i < KN_DROPS; i++)
    {
        if (kn_drop[i].live) continue;
        kn_drop[i].x = x;
        kn_drop[i].y = y;
        kn_drop[i].vx = vx + ((int)kn_rand(13) - 6) / 10.f * kn_slow;
        kn_drop[i].vy = vy + ((int)kn_rand(13) - 6) / 10.f * kn_slow;
        kn_drop[i].size = 2 + kn_rand(3);
        kn_drop[i].live = true;
        return;
    }
}

static void kn_splash(float x, float y, int n, float up)
{
    while (n--)
        kn_emit(x, y, ((int)kn_rand(91) - 45) / 10.f * kn_slow,
                (((int)kn_rand(71) - 50) / 10.f - up) * kn_slow);
}

/* A wound keeps pumping for a while, out of the cut end of the bone. */
static void kn_bleed_add(int node, int from, int steps)
{
    for (int i = 0; i < KN_BLEEDS; i++)
        if (kn_bleed[i].left <= 0)
        {
            kn_bleed[i].node = node;
            kn_bleed[i].from = from;
            kn_bleed[i].left = steps;
            return;
        }
}

static void kn_stain_at(float x)
{
    for (int i = 0; i < kn_nstain; i++)
        if (fabsf(kn_stain[i].x - x) < kn_stain[i].w / 2 + 3)
        {
            if (kn_stain[i].w < 70) kn_stain[i].w += 2;
            if (kn_stain[i].opa < 230) kn_stain[i].opa += 10;
            kn_stains_moved = true;
            return;
        }
    int i = kn_nstain < KN_STAINS ? kn_nstain++ : kn_stain_ring++ % KN_STAINS;
    kn_stain[i].x = x;
    kn_stain[i].w = 8;
    kn_stain[i].opa = 150;
    kn_stains_moved = true;
}

/* Cut bone e at t (0 at its end a, 1 at b): it becomes a stump and a
 * loose piece, each with a new cut end that bleeds. Whatever is still
 * joined to the chest stays on the doll; the rest falls. */
static void kn_split(int e, float t, float kick_x, float kick_y)
{
    if (rd_count + 2 > RD_NMAX || rd_ecount >= RD_EMAX || rd_len[e] < 14) return;
    if (t < .2f) t = .2f;
    if (t > .8f) t = .8f;
    unsigned before = kn_attached();
    int a = rd_ea[e], b = rd_eb[e], p = rd_count, q = rd_count + 1;
    for (int n = p; n <= q; n++)
    {
        rd_nodes[n].x = rd_nodes[a].x + (rd_nodes[b].x - rd_nodes[a].x) * t;
        rd_nodes[n].y = rd_nodes[a].y + (rd_nodes[b].y - rd_nodes[a].y) * t;
        rd_nodes[n].px = rd_nodes[a].px + (rd_nodes[b].px - rd_nodes[a].px) * t;
        rd_nodes[n].py = rd_nodes[a].py + (rd_nodes[b].py - rd_nodes[a].py) * t;
    }
    rd_count += 2;
    rd_ea[rd_ecount] = q;
    rd_eb[rd_ecount] = b;
    rd_len[rd_ecount] = rd_len[e] * (1 - t);
    rd_kind[rd_ecount] = rd_kind[e];
    rd_ecount++;
    rd_eb[e] = p;
    rd_len[e] *= t;
    kn_wounds |= 1u << p | 1u << q;
    /* the side that just came off takes the knife's shove */
    unsigned body = kn_attached(), off = (before & ~body) | (body >> p & 1 ? 1u << q : 1u << p);
    for (int n = 0; n < rd_count; n++)
        if (off >> n & 1)
        {
            rd_nodes[n].px -= kick_x;
            rd_nodes[n].py -= kick_y;
        }
    bool a_side = body >> a & 1;
    kn_bleed_add(a_side ? p : q, a_side ? a : b, 70);   /* the stump */
    kn_bleed_add(a_side ? q : p, a_side ? b : a, 28);   /* the piece */
    kn_shake = kn_shake < 5 ? 5 : kn_shake;
}

/* Bullet time: velocities scale with the clock, gravity with its square. */
static void kn_set_slow(float s)
{
    float r = s / kn_slow;
    for (int i = 0; i < rd_count; i++)
    {
        rd_nodes[i].px = rd_nodes[i].x - (rd_nodes[i].x - rd_nodes[i].px) * r;
        rd_nodes[i].py = rd_nodes[i].y - (rd_nodes[i].y - rd_nodes[i].py) * r;
    }
    for (int i = 0; i < KN_DROPS; i++) { kn_drop[i].vx *= r; kn_drop[i].vy *= r; }
    kn_slow = s;
    rd_grav = .28f * s * s;
}

static float kn_pt_d2(float px, float py, float ax, float ay, float bx, float by, float *t_out)
{
    float dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
    float t = l > 0 ? ((px - ax) * dx + (py - ay) * dy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    if (t_out) *t_out = t;
    dx = ax + dx * t - px;
    dy = ay + dy * t - py;
    return dx * dx + dy * dy;
}

/* Distance squared from bone a-b to the strip a knife blade swept this
 * step (x, top..bottom), so a fast knife cannot step over a thin arm;
 * *t is where along the bone it is closest. */
static float kn_sweep_d2(int a, int b, float x, float top, float bottom, float *t)
{
    float ax = rd_nodes[a].x, ay = rd_nodes[a].y, bx = rd_nodes[b].x, by = rd_nodes[b].y;
    if ((ax - x) * (bx - x) <= 0 && ax != bx)
    {
        *t = (x - ax) / (bx - ax);
        float y = ay + (by - ay) * *t;
        if (y >= top && y <= bottom) return 0;
    }
    float d = kn_pt_d2(x, top, ax, ay, bx, by, t), t2;
    float d2 = kn_pt_d2(x, bottom, ax, ay, bx, by, &t2);
    if (d2 < d) { d = d2; *t = t2; }
    d2 = kn_pt_d2(ax, ay, x, top, x, bottom, NULL);
    if (d2 < d) { d = d2; *t = 0; }
    d2 = kn_pt_d2(bx, by, x, top, x, bottom, NULL);
    if (d2 < d) { d = d2; *t = 1; }
    return d;
}

static void kn_die(void)
{
    kn_dead = true;
    kn_dead_at = kn_t;
    rd_grab = -1;                       /* it slips out of your fingers */
    kn_drag = false;
    kn_dir = 0;
    kn_arrows_show(0);
    if (kn_score > kn_best) kn_best = kn_score;
    /* the rest of the doll comes apart too, the way the flash game's does */
    unsigned body = kn_attached();
    for (int e = 0; e < RD_E; e++)
    {
        int k = rd_kind[e];
        if ((k == 1 || k == 3 || k == 6 || k == 7) && body >> rd_ea[e] & body >> rd_eb[e] & 1 && kn_rand(2))
            kn_split(e, .35f + kn_rand(30) / 100.f, ((int)kn_rand(9) - 4) * .6f, -2.5f);
    }
    kn_shake = 11;
    kn_flash = 110;
    kn_set_slow(.25f);
    lv_label_set_text(rd_face, "x(");
    sfx_play(SFX_SNAKE_DIE);
}

static void kn_hit(int k, float top)
{
    unsigned body = kn_attached();
    int hit = -1;
    float best = 1e9f, at = .5f, t;
    for (int e = 0; e < rd_ecount; e++)
    {
        bool on = body >> rd_ea[e] & body >> rd_eb[e] & 1;
        float d = kn_sweep_d2(rd_ea[e], rd_eb[e], kn[k].x, top, kn[k].y, &t);
        if (d >= 12 * 12) continue;
        if (!on)
        {
            /* a knife through a piece on the floor just knocks it about */
            rd_nodes[rd_ea[e]].py -= kn[k].vy * kn_slow * .08f;
            rd_nodes[rd_eb[e]].py -= kn[k].vy * kn_slow * .08f;
            continue;
        }
        /* the falling blade meets the highest bone first */
        float y = rd_nodes[rd_ea[e]].y + (rd_nodes[rd_eb[e]].y - rd_nodes[rd_ea[e]].y) * t;
        if (y < best) { best = y; hit = e; at = t; }
    }
    bool head = body & 1 &&
                kn_pt_d2(rd_nodes[0].x, rd_nodes[0].y, kn[k].x, top, kn[k].x, kn[k].y, NULL) < 26 * 26;
    if (head) { hit = 0; at = .45f; }               /* off at the neck */
    if (hit < 0) return;
    kn[k].spent = true;
    kn_bloody |= 1u << k;
    int a = rd_ea[hit], b = rd_eb[hit];
    float cx = rd_nodes[a].x + (rd_nodes[b].x - rd_nodes[a].x) * at;
    float cy = rd_nodes[a].y + (rd_nodes[b].y - rd_nodes[a].y) * at;
    bool fatal = head || rd_kind[hit] == 0 || rd_kind[hit] == 5;
    float kick = kn[k].vy * kn_slow * .5f, side = (cx < kn[k].x ? -1.8f : 1.8f) * kn_slow;
    kn_split(hit, at, side, kick);
    kn_splash(cx, cy, fatal ? 40 : 18, fatal ? 2.f : 1.f);
    sfx_play_ex(SFX_SPOON_SPLASH, fatal ? 80 : 110, 80);
    if (fatal) kn_die();
    else sfx_play_ex(SFX_SPOON_HIT, 130, 60);
}

static void kn_spawn(void)
{
    int k = 0;
    while (k < KN_MAX && kn[k].live) k++;
    if (k == KN_MAX) return;
    float s = kn_t / 1000.f, x, lo = W, hi = 0;
    for (int i = 0; i < RD_N; i++)
    {
        if (rd_nodes[i].x < lo) lo = rd_nodes[i].x;
        if (rd_nodes[i].x > hi) hi = rd_nodes[i].x;
    }
    /* fair start: the first few never drop on the doll; later on
     * some come straight for it so nobody wins by standing still */
    for (int tries = 0; ; tries++)
    {
        x = 24 + kn_rand(W - 76);
        if (s > 20 && kn_rand(4) == 0) x = rd_nodes[0].x - 30 + kn_rand(61);
        if (x < 24) x = 24;
        if (x > W - 52) x = W - 52;
        if (tries > 6) break;
        if (fabsf(x - kn_last_x[0]) < 36 || fabsf(x - kn_last_x[1]) < 36) continue;
        if (kn_dropped < 3 && x > lo - 40 && x < hi + 40) continue;
        break;
    }
    kn_last_x[1] = kn_last_x[0];
    kn_last_x[0] = x;
    kn[k].x = x;
    kn[k].y = 0;
    kn[k].vy = 3.f + s * .05f + kn_rand(8) / 10.f;
    if (kn[k].vy > 8.f) kn[k].vy = 8.f;
    kn[k].stuck = 0;
    kn[k].spent = false;
    kn[k].live = true;
    kn_bloody &= ~(1u << k);
    kn_dropped++;
}

/* The string pulls the head and chest toward the finger like a puppet:
 * it carries the doll's weight and steers it, with a limit, so the doll
 * swings after the finger instead of jumping to it. */
static void kn_pull(void)
{
    unsigned body = kn_attached();
    int n = 0;
    for (int i = 0; i < rd_count; i++) n += body >> i & 1;
    static const float share[2] = { .55f, .45f }, cap[2] = { 2.6f, 2.f };
    for (int i = 0; i < 2; i++)
    {
        if (!(body >> i & 1)) continue;
        float ty = kn_ty + (i ? rd_len[0] : 0);
        float wx = (kn_tx - rd_nodes[i].x) * .2f, wy = (ty - rd_nodes[i].y) * .2f;
        float w = sqrtf(wx * wx + wy * wy);
        if (w > 9.f) { wx *= 9.f / w; wy *= 9.f / w; }
        float fx = (wx - (rd_nodes[i].x - rd_nodes[i].px)) * .5f;
        float fy = (wy - (rd_nodes[i].y - rd_nodes[i].py)) * .5f - rd_grav * n * share[i];
        float f = sqrtf(fx * fx + fy * fy);
        if (f > cap[i]) { fx *= cap[i] / f; fy *= cap[i] / f; }
        rd_fx[i] = fx;
        rd_fy[i] = fy;
    }
}

/* The pad pulls the doll by the head and chest; the lift fades with
 * height, so held it stands the doll up but cannot fly it into the knives. */
static void kn_push(void)
{
    float vx = rd_nodes[0].x - rd_nodes[0].px, vy = rd_nodes[0].y - rd_nodes[0].py;
    float ax = (kn_dir & KN_R ? 1.f : 0.f) - (kn_dir & KN_L ? 1.f : 0.f);
    if (ax != 0 && vx * ax < 7.f)
    {
        rd_fx[0] = .9f * ax;
        rd_fx[1] = .6f * ax;
        rd_fx[6] = .35f * ax;
    }
    float lift = (rd_nodes[0].y - 90) / 110;
    if (kn_dir & KN_U && vy > -6.f && lift > 0)
    {
        if (lift > 1) lift = 1;
        rd_fy[0] = -2.f * lift;
        rd_fy[1] = -1.2f * lift;
        rd_fy[6] = -.4f * lift;
    }
    if (kn_dir & KN_D)
    {
        rd_fy[0] = .8f;
        rd_fy[1] = .5f;
    }
}

static void kn_step(void)
{
    for (int i = 0; i < RD_NMAX; i++) rd_fx[i] = rd_fy[i] = 0;
    if (!kn_dead && kn_drag) kn_pull();
    if (!kn_dead && kn_dir) kn_push();
    rd_step();
    /* muscle tone: elbows stay off the hips and the feet apart, so the
     * doll reads as a figure instead of folding into one stick */
    static const struct { unsigned char a, b; float min; } apart[3] = { {2,6,60}, {4,6,60}, {7,8,40} };
    unsigned body = kn_attached();
    for (int j = 0; j < 3; j++)
    {
        int a = apart[j].a, b = apart[j].b;
        if (!(body >> a & body >> b & 1) || a == rd_grab || b == rd_grab) continue;
        float dx = rd_nodes[b].x - rd_nodes[a].x, dy = rd_nodes[b].y - rd_nodes[a].y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d >= apart[j].min || d < 1.f) continue;
        float k = (apart[j].min - d) / d * .25f;
        rd_nodes[a].x -= dx * k; rd_nodes[a].y -= dy * k;
        rd_nodes[b].x += dx * k; rd_nodes[b].y += dy * k;
    }
    for (int i = 0; i < rd_count; i++)           /* back in the box, a little floor grip */
    {
        if (i == rd_grab) continue;
        if (rd_nodes[i].x < 16) rd_nodes[i].x = 16;
        if (rd_nodes[i].x > W - 44) rd_nodes[i].x = W - 44;
        if (rd_nodes[i].y < 20) rd_nodes[i].y = 20;
        if (rd_nodes[i].y > 358) rd_nodes[i].y = 358;
        if (rd_nodes[i].y >= 357.5f)
            rd_nodes[i].px = rd_nodes[i].x - (rd_nodes[i].x - rd_nodes[i].px) * .85f;
    }
    kn_t += KN_STEP;
    if (!kn_dead && kn_t >= kn_next)
    {
        float s = kn_t / 1000.f;
        int gap = 1100 - (int)(s * 12);
        if (gap < 260) gap = 260;
        kn_spawn();
        if (s > 30 && kn_rand(5) == 0) kn_spawn();
        kn_next = kn_t + gap * 3 / 4 + kn_rand(gap / 2);
    }
    for (int k = 0; k < KN_MAX; k++)
    {
        if (!kn[k].live) continue;
        if (kn[k].stuck)
        {
            if (++kn[k].stuck > 24) kn[k].live = false;
            continue;
        }
        float top = kn[k].y - KN_BLADE;
        kn[k].y += kn[k].vy * kn_slow;
        if (kn[k].y > KN_FLOOR) kn[k].y = KN_FLOOR;
        if (!kn[k].spent && !kn_dead) kn_hit(k, top);
        if (kn[k].y >= KN_FLOOR)
        {
            kn[k].stuck = 1;
            if (!kn[k].spent && !kn_dead) kn_score++;
        }
    }
    /* wounds pump in beats, weaker as they go */
    for (int i = 0; i < KN_BLEEDS; i++)
    {
        if (kn_bleed[i].left <= 0) continue;
        int n = kn_bleed[i].node, f = kn_bleed[i].from, left = --kn_bleed[i].left;
        if (left % 8 >= 5) continue;
        float dx = rd_nodes[n].x - rd_nodes[f].x, dy = rd_nodes[n].y - rd_nodes[f].y;
        float d = sqrtf(dx * dx + dy * dy), sp = (1.2f + left / 25.f) * kn_slow;
        if (d < .5f) { dx = 0; dy = -1; d = 1; }
        kn_emit(rd_nodes[n].x, rd_nodes[n].y, dx / d * sp + rd_nodes[n].x - rd_nodes[n].px,
                dy / d * sp + rd_nodes[n].y - rd_nodes[n].py - .8f * kn_slow);
    }
    for (int i = 0; i < KN_DROPS; i++)
    {
        if (!kn_drop[i].live) continue;
        kn_drop[i].x += kn_drop[i].vx;
        kn_drop[i].y += kn_drop[i].vy;
        kn_drop[i].vy += rd_grav;
        if (kn_drop[i].y >= 366) { kn_stain_at(kn_drop[i].x); kn_drop[i].live = false; }
        else if (kn_drop[i].x < 4 || kn_drop[i].x > W - 32 || kn_drop[i].y < -40) kn_drop[i].live = false;
    }
    kn_shake *= .88f;
    kn_flash *= .9f;
}

static void kn_draw(lv_event_t *e)
{
    if (!kn_on) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(rd_scene, &a);
    int ox = a.x1 + kn_sx, oy = a.y1 + kn_sy;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    if (lv_event_get_code(e) == LV_EVENT_DRAW_POST)
    {
        r.bg_color = lv_color_hex(0xd1121e);   /* square drops: no corner masks, far cheaper */
        for (int i = 0; i < KN_DROPS; i++)
        {
            if (!kn_drop[i].live) continue;
            int x = ox + (int)kn_drop[i].x, y = oy + (int)kn_drop[i].y, s = kn_drop[i].size;
            lv_area_t d = { x - s / 2, y - s / 2, x + s - s / 2, y + s - s / 2 + 1 };
            lv_draw_rect(layer, &r, &d);
        }
        if (kn_flash > 4)
        {
            r.radius = 26;
            r.bg_color = lv_color_hex(0xb00010);
            r.bg_opa = (lv_opa_t)kn_flash;
            lv_draw_rect(layer, &r, &a);
        }
        return;
    }
    /* blood on the floor */
    r.bg_color = lv_color_hex(0x8c0d19);
    for (int i = 0; i < kn_nstain; i++)
    {
        r.bg_opa = kn_stain[i].opa;
        lv_area_t s = { ox + kn_stain[i].x - kn_stain[i].w / 2, oy + 362,
                        ox + kn_stain[i].x + kn_stain[i].w / 2, oy + 369 };
        lv_draw_rect(layer, &r, &s);
    }
    /* knives; the ones that cut carry the blood down the blade */
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    for (int k = 0; k < KN_MAX; k++)
    {
        if (!kn[k].live) continue;
        int x = ox + (int)kn[k].x, y = oy + (int)kn[k].y;
        lv_opa_t opa = kn[k].stuck > 12 ? 255 - (kn[k].stuck - 12) * 20 : 255;
        t.bg_opa = opa;
        t.bg_color = lv_color_hex(0xdfe5ee);
        t.p[0].x = x - 4; t.p[0].y = y - KN_BLADE;
        t.p[1].x = x + 4; t.p[1].y = y - KN_BLADE;
        t.p[2].x = x;     t.p[2].y = y;
        lv_draw_triangle(layer, &t);
        if (kn_bloody >> k & 1)
        {
            t.bg_color = lv_color_hex(0xb3101b);
            t.p[0].x = x - 2; t.p[0].y = y - 16;
            t.p[1].x = x + 2; t.p[1].y = y - 16;
            lv_draw_triangle(layer, &t);
        }
        r.bg_opa = opa;
        r.bg_color = lv_color_hex(0x9aa3b5);
        lv_area_t g = { x - 8, y - KN_BLADE - 3, x + 8, y - KN_BLADE };
        lv_draw_rect(layer, &r, &g);
        r.bg_color = lv_color_hex(0x8a5a3c);
        lv_area_t h = { x - 3, y - KN_BLADE - 20, x + 3, y - KN_BLADE - 3 };
        lv_draw_rect(layer, &r, &h);
    }
    /* the doll: pieces that came off a shade darker, cut ends raw red */
    unsigned body = kn_attached();
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.width = 18;
    l.round_start = l.round_end = 1;
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < rd_ecount; k++)
        {
            bool on = body >> rd_ea[k] & 1;
            if (on != (pass == 1)) continue;
            l.color = lv_color_hex(on ? 0x8bd0d2 : 0x6aa6ab);
            l.p1.x = ox + rd_nodes[rd_ea[k]].x; l.p1.y = oy + rd_nodes[rd_ea[k]].y;
            l.p2.x = ox + rd_nodes[rd_eb[k]].x; l.p2.y = oy + rd_nodes[rd_eb[k]].y;
            lv_draw_line(layer, &l);
        }
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_opa = LV_OPA_COVER;
    r.bg_color = lv_color_hex(0xa30f1a);
    for (int n = 0; n < rd_count; n++)
    {
        if (!(kn_wounds >> n & 1)) continue;
        int x = ox + (int)rd_nodes[n].x, y = oy + (int)rd_nodes[n].y;
        lv_area_t c = { x - 7, y - 7, x + 7, y + 7 };
        lv_draw_rect(layer, &r, &c);
    }
    if (kn_drag && !kn_dead)            /* the string, while the finger is down */
    {
        l.width = 2;
        l.round_start = l.round_end = 0;
        l.color = lv_color_hex(0xffffff);
        l.opa = 90;
        l.p1.x = ox + rd_nodes[0].x; l.p1.y = oy + rd_nodes[0].y;
        l.p2.x = ox + kn_tx; l.p2.y = oy + kn_ty;
        lv_draw_line(layer, &l);
        r.bg_opa = LV_OPA_TRANSP;
        r.border_color = lv_color_hex(0xffffff);
        r.border_width = 2;
        r.border_opa = 120;
        lv_area_t c = { ox + (int)kn_tx - 9, oy + (int)kn_ty - 9, ox + (int)kn_tx + 9, oy + (int)kn_ty + 9 };
        lv_draw_rect(layer, &r, &c);
    }
}

/* Redraw only what moved: the doll with its blood and string, each
 * knife's strip, and the floor when a stain grew; last frame's areas too,
 * so nothing is left behind. Shaking or flashing, the whole box. */
static void kn_invalidate(void)
{
    lv_area_t box;
    lv_obj_get_coords(rd_scene, &box);
    int live = 0;
    for (int k = 0; k < KN_MAX; k++) live += kn[k].live;
    bool full = kn_full || kn_shake > .5f || kn_flash > 4 || live > 14;
    kn_full = kn_shake > .5f || kn_flash > 4;   /* one clean frame after it settles */
    float x1 = rd_nodes[0].x, y1 = rd_nodes[0].y, x2 = x1, y2 = y1;
    for (int i = 0; i < rd_count; i++)
    {
        if (rd_nodes[i].x < x1) x1 = rd_nodes[i].x;
        if (rd_nodes[i].x > x2) x2 = rd_nodes[i].x;
        if (rd_nodes[i].y < y1) y1 = rd_nodes[i].y;
        if (rd_nodes[i].y > y2) y2 = rd_nodes[i].y;
    }
    for (int i = 0; i < KN_DROPS; i++)
    {
        if (!kn_drop[i].live) continue;
        if (kn_drop[i].x < x1) x1 = kn_drop[i].x;
        if (kn_drop[i].x > x2) x2 = kn_drop[i].x;
        if (kn_drop[i].y < y1) y1 = kn_drop[i].y;
        if (kn_drop[i].y > y2) y2 = kn_drop[i].y;
    }
    if (kn_drag)
    {
        if (kn_tx < x1) x1 = kn_tx;
        if (kn_tx > x2) x2 = kn_tx;
        if (kn_ty < y1) y1 = kn_ty;
        if (kn_ty > y2) y2 = kn_ty;
    }
    lv_area_t doll = { box.x1 + (int)x1 - 26, box.y1 + (int)y1 - 26, box.x1 + (int)x2 + 26, box.y1 + (int)y2 + 26 };
    if (full) lv_obj_invalidate(rd_scene);
    else
    {
        lv_area_t both = { LV_MIN(doll.x1, kn_was_doll.x1), LV_MIN(doll.y1, kn_was_doll.y1),
                           LV_MAX(doll.x2, kn_was_doll.x2), LV_MAX(doll.y2, kn_was_doll.y2) };
        lv_obj_invalidate_area(rd_scene, &both);
        for (int k = 0; k < KN_MAX; k++)
        {
            bool was = kn_was_y[k] >= 0, same = was && kn[k].live && kn_was_x[k] == (int)kn[k].x;
            if (was && !same)               /* gone, or its slot reused elsewhere */
            {
                lv_area_t old = { box.x1 + kn_was_x[k] - 9, box.y1 + kn_was_y[k] - KN_BLADE - 21,
                                  box.x1 + kn_was_x[k] + 9, box.y1 + kn_was_y[k] + 1 };
                lv_obj_invalidate_area(rd_scene, &old);
            }
            if (!kn[k].live) continue;
            int x = box.x1 + (int)kn[k].x, now = (int)kn[k].y;
            int top = same && kn_was_y[k] < now ? kn_was_y[k] : now;
            lv_area_t strip = { x - 9, box.y1 + top - KN_BLADE - 21, x + 9, box.y1 + now + 1 };
            lv_obj_invalidate_area(rd_scene, &strip);
        }
        if (kn_stains_moved)
        {
            lv_area_t floor = { box.x1, box.y1 + 360, box.x2, box.y1 + 371 };
            lv_obj_invalidate_area(rd_scene, &floor);
        }
    }
    kn_was_doll = doll;
    kn_stains_moved = false;
    for (int k = 0; k < KN_MAX; k++)
    {
        kn_was_x[k] = (short)kn[k].x;
        kn_was_y[k] = kn[k].live ? (short)kn[k].y : -1;
    }
}

static void rd_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!rd_scene || lv_obj_has_flag(lv_obj_get_parent(lv_obj_get_parent(rd_scene)), LV_OBJ_FLAG_HIDDEN) ||
        sl_power_hides_ui())
    {
        kn_last = lv_tick_get();
        return;   /* fixed-step physics: under the lock screen it simply waits */
    }
    rd_impact = 0;
    if (!kn_on) rd_step();
    else
    {
        /* steps by the clock, not by timer calls, so a slow frame does not
         * slow the knives down; a long stall is dropped, not replayed */
        uint32_t dt = lv_tick_elaps(kn_last);
        kn_last += dt;
        if (lv_obj_has_flag(kn_menu, LV_OBJ_FLAG_HIDDEN))  /* the cog's menu pauses play */
            kn_acc += dt > 100 ? 100 : dt;
        while (kn_acc >= KN_STEP) { kn_step(); kn_acc -= KN_STEP; }
        /* play time, so the cog's pause holds the slow motion too */
        if (kn_dead && kn_slow < 1.f && kn_t - kn_dead_at > 1400) kn_set_slow(1.f);
        if (kn_dead && !kn_over && kn_t - kn_dead_at > 1400)
        {
            static const char *const rank[] = { "Butterfingers", "Getting the hang of it",
                                                "Nimble", "Slippery", "Untouchable" };
            int r = kn_score < 10 ? 0 : kn_score < 25 ? 1 : kn_score < 50 ? 2 : kn_score < 100 ? 3 : 4;
            kn_over = true;
            lv_label_set_text_fmt(kn_over_t, sl_tr("You dodged %d"), kn_score);
            lv_label_set_text_fmt(kn_over_s, sl_tr("%s. Best: %d\nTap to play again"), sl_tr(rank[r]), kn_best);
            lv_obj_remove_flag(kn_over_box, LV_OBJ_FLAG_HIDDEN);
        }
        if (kn_shown != kn_score)
        {
            kn_shown = kn_score;
            lv_label_set_text_fmt(kn_score_l, sl_tr("Dodged %d"), kn_score);
        }
        kn_sx = kn_shake > .5f ? (int)kn_rand((int)(kn_shake * 2) + 1) - (int)kn_shake : 0;
        kn_sy = kn_shake > .5f ? (int)kn_rand((int)(kn_shake * 2) + 1) - (int)kn_shake : 0;
        lv_obj_set_pos(rd_head, (int)rd_nodes[0].x - 23 + kn_sx, (int)rd_nodes[0].y - 23 + kn_sy);
        kn_invalidate();
    }
    /* A thud as big as the landing; a limb settling on the floor is quiet. */
    if (rd_impact > 4.f && sl_ms() - rd_thud_at > 140)
    {
        rd_thud_at = sl_ms();
        int vol = rd_impact >= 24 ? 100 : 30 + (int)(rd_impact * 70 / 24);
        sfx_play_ex(SFX_RAGDOLL_THUD, 115 - vol / 4, vol);
    }
    if (kn_on) return;                  /* Knives draws its own doll */
    for (int e = 0; e < RD_E; e++)
    {
        int a = rd_ea[e], b = rd_eb[e];
        rd_points[e][0].x = (int)rd_nodes[a].x;
        rd_points[e][0].y = (int)rd_nodes[a].y;
        rd_points[e][1].x = (int)rd_nodes[b].x;
        rd_points[e][1].y = (int)rd_nodes[b].y;
        lv_line_set_points(rd_lines[e], rd_points[e], 2);
    }
    lv_obj_set_pos(rd_head, (int)rd_nodes[0].x - 23, (int)rd_nodes[0].y - 23);
}

static void kn_clear(void)
{
    rd_reset();
    memset(kn, 0, sizeof kn);
    memset(kn_drop, 0, sizeof kn_drop);
    memset(kn_bleed, 0, sizeof kn_bleed);
    kn_nstain = kn_stain_ring = 0;
    kn_wounds = kn_bloody = 0;
    kn_slow = 1.f;
    rd_grav = .28f;
    kn_shake = kn_flash = 0;
    kn_sx = kn_sy = 0;
    kn_full = true;
    for (int k = 0; k < KN_MAX; k++) kn_was_y[k] = -1;
    for (int i = 0; i < RD_NMAX; i++) rd_fx[i] = rd_fy[i] = 0;
    kn_tap = kn_drag = kn_swallow = false;
    kn_dir = 0;
    kn_dead = kn_over = false;
}

static void kn_start(void)
{
    kn_clear();
    kn_score = kn_dropped = 0;
    kn_shown = -1;
    kn_t = 0;
    kn_next = 1500;                 /* a moment to get a grip first */
    kn_acc = 0;
    kn_last = lv_tick_get();
    kn_last_x[0] = kn_last_x[1] = -100;
    kn_seed ^= sl_ms();
    lv_label_set_text(rd_face, ":)");
    lv_obj_add_flag(kn_over_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(kn_menu, LV_OBJ_FLAG_HIDDEN);
    kn_arrows_show(0);
    kn_full = true;
    sfx_play(SFX_START);
}

/* Show what the mode and the control choice need: the score and cog in
 * Knives, the pad only for the pad (the hint makes room for it). */
static void kn_ui(void)
{
    static const char *const hint[3] = { "Grab the doll and drag it out of the way of the knives",
                                         "Drag anywhere in the box to pull your doll on a string",
                                         "Hold and slide on the pad to steer" };
    lv_obj_t *knives[] = { kn_score_l, kn_cog };
    for (int i = 0; i < 2; i++)
        if (kn_on) lv_obj_remove_flag(knives[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(knives[i], LV_OBJ_FLAG_HIDDEN);
    bool pad = kn_on && kn_ctrl == KN_PAD;
    if (pad) lv_obj_remove_flag(kn_pad, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(kn_pad, LV_OBJ_FLAG_HIDDEN);
    if (pad) lv_obj_add_flag(rd_hint, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(rd_hint, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(rd_hint, sl_tr(kn_on ? hint[kn_ctrl] : "Grab a limb and toss your ragdoll around"));
    for (int i = 0; i < 3; i++)
        if (i == kn_ctrl) lv_obj_remove_flag(kn_ticks[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(kn_ticks[i], LV_OBJ_FLAG_HIDDEN);
    kn_arrows_show(0);
    lv_obj_add_flag(kn_menu, LV_OBJ_FLAG_HIDDEN);
}

static void kn_cog_cb(lv_event_t *e)
{
    (void)e;
    if (lv_obj_has_flag(kn_menu, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(kn_menu, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(kn_menu, LV_OBJ_FLAG_HIDDEN);
    kn_last = lv_tick_get();
}

static void kn_ctrl_cb(lv_event_t *e)
{
    kn_ctrl = (int)(intptr_t)lv_event_get_user_data(e);
    kn_drag = false;
    kn_dir = 0;
    rd_grab = -1;
    kn_last = lv_tick_get();
    kn_ui();
}

/* One finger, eight ways: the side thirds steer, the middle lifts or
 * drops, and the top or bottom edge of a side adds a lift or a drop. */
static void kn_pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    unsigned dir = 0;
    if ((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) && lv_indev_active())
    {
        lv_point_t point;
        lv_area_t a;
        lv_indev_get_point(lv_indev_active(), &point);
        lv_obj_get_coords(kn_pad, &a);
        float dx = (point.x - (a.x1 + a.x2) / 2.f) / (lv_area_get_width(&a) / 2.f);
        float dy = (point.y - (a.y1 + a.y2) / 2.f) / (lv_area_get_height(&a) / 2.f);
        if (dx < -.3f) dir |= KN_L;
        if (dx > .3f) dir |= KN_R;
        if (dy < (dir ? -.5f : 0.f)) dir |= KN_U;
        else if (dy > (dir & (KN_L | KN_R) ? .5f : 0.f)) dir |= KN_D;
    }
    else if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    if (kn_dead) dir = 0;
    if (dir == kn_dir) return;
    kn_dir = dir;
    kn_arrows_show(dir);
}

/* Switch between free play and Knives; the one timer serves both. */
static void kn_mode(bool on)
{
    kn_on = on;
    kn_clear();
    lv_label_set_text(rd_face, ":)");
    lv_obj_add_flag(kn_over_box, LV_OBJ_FLAG_HIDDEN);
    if (on) kn_start();
    for (int i = 0; i < RD_E; i++)
        if (on) lv_obj_add_flag(rd_lines[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(rd_lines[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(rd_scene, lv_color_hex(on ? 0x1b2030 : 0x293247), 0);
    kn_ui();
    lv_label_set_text(lv_obj_get_child(rd_reset_row, 0), sl_tr(on ? "Restart" : "Reset ragdoll"));
    lv_label_set_text(lv_obj_get_child(rd_reset_row, 1), sl_tr(on ? "A new round of knives" : "Start again in the middle"));
    lv_label_set_text(lv_obj_get_child(rd_mode_row, 0), sl_tr(on ? "Free play" : "Knives"));
    lv_label_set_text(lv_obj_get_child(rd_mode_row, 1), sl_tr(on ? "Back to grab and toss" : "Dodge the falling knives"));
    lv_obj_invalidate(rd_scene);
    rd_tick(NULL);
}

static void rd_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (!lv_indev_active()) return;
    lv_point_t point;
    lv_area_t area;
    lv_indev_get_point(lv_indev_active(), &point);
    lv_obj_get_coords(rd_scene, &area);
    float x = point.x - area.x1, y = point.y - area.y1;
    if (kn_on)
    {
        /* a touch beside the open menu only closes it */
        if (code == LV_EVENT_PRESSED && !lv_obj_has_flag(kn_menu, LV_OBJ_FLAG_HIDDEN))
        {
            lv_obj_add_flag(kn_menu, LV_OBJ_FLAG_HIDDEN);
            kn_swallow = true;
        }
        if (kn_swallow)
        {
            if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) kn_swallow = false;
            return;
        }
        /* once the doll is gone a tap on the box starts the next round */
        if (code == LV_EVENT_PRESSED) kn_tap = kn_over;
        if (code == LV_EVENT_CLICKED && kn_tap && kn_over) { kn_start(); return; }
        if (kn_ctrl == KN_PAD) return;
        if (kn_ctrl == KN_STRING)
        {
            /* the string is tied where you touch: drag from anywhere, the
             * finger need not cover the doll */
            if (code == LV_EVENT_PRESSED)
            {
                kn_drag = !kn_dead;
                kn_ox = x - rd_nodes[0].x;
                kn_oy = y - rd_nodes[0].y;
            }
            if ((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) && kn_drag)
            {
                kn_tx = x - kn_ox;
                kn_ty = y - kn_oy;
                if (kn_tx < 16) kn_tx = 16;
                if (kn_tx > W - 44) kn_tx = W - 44;
                if (kn_ty < 20) kn_ty = 20;
                if (kn_ty > 358) kn_ty = 358;
            }
            if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) kn_drag = false;
            return;
        }
        if (kn_dead && rd_grab < 0) return;     /* the grab, as in free play */
    }
    if (x < 16) x = 16;
    if (x > W - 44) x = W - 44;
    if (y < 20) y = 20;
    if (y > 358) y = 358;
    if (code == LV_EVENT_PRESSED)
    {
        float best = 55.f * 55.f;
        rd_grab = -1;
        for (int i = 0; i < rd_count; i++)
        {
            float dx = x - rd_nodes[i].x, dy = y - rd_nodes[i].y;
            float d = dx * dx + dy * dy;
            if (d < best) { best = d; rd_grab = i; }
        }
        if (rd_grab >= 0) sfx_play_ex(SFX_RAGDOLL_GRAB, rd_grab ? 100 : 130, 100);
        rd_last_x = x; rd_last_y = y;
    }
    else if (code == LV_EVENT_PRESSING && rd_grab >= 0)
    {
        rd_last_x = rd_nodes[rd_grab].x;
        rd_last_y = rd_nodes[rd_grab].y;
        rd_nodes[rd_grab].x = x;
        rd_nodes[rd_grab].y = y;
    }
    else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && rd_grab >= 0)
    {
        float vx = (x - rd_last_x) * .65f, vy = (y - rd_last_y) * .65f;
        if (vx > 38) vx = 38;
        if (vx < -38) vx = -38;
        if (vy > 38) vy = 38;
        if (vy < -38) vy = -38;
        rd_nodes[rd_grab].px = rd_nodes[rd_grab].x - vx;
        rd_nodes[rd_grab].py = rd_nodes[rd_grab].y - vy;
        rd_grab = -1;
    }
}

static void rd_reset_cb(lv_event_t *e) { (void)e; if (kn_on) kn_start(); else rd_reset(); }
static void rd_mode_cb(lv_event_t *e) { (void)e; kn_mode(!kn_on); }
static lv_obj_t *rd_page_obj;
static void rd_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) != rd_page_obj) return;   /* only the one open copy */
    rd_page_obj = NULL;
    if (rd_timer) { lv_timer_delete(rd_timer); rd_timer = NULL; }
    rd_scene = rd_head = rd_face = NULL;
    kn_score_l = kn_over_box = kn_over_t = kn_over_s = NULL;
    kn_pad = kn_cog = kn_menu = NULL;
    rd_hint = rd_reset_row = rd_mode_row = NULL;
    kn_on = false;
    kn_clear();
}

static void ragdoll_page(void)
{
    if (page_bring_back(rd_page_obj)) return;
    lv_obj_t *p, *body = page_new("Ragdoll", &p);
    rd_page_obj = p;
    lv_obj_add_event_cb(p, rd_deleted, LV_EVENT_DELETE, NULL);
    rd_scene = box(body);
    lv_obj_set_size(rd_scene, lv_pct(100), 390);
    lv_obj_set_style_radius(rd_scene, SL_R_XL, 0);
    lv_obj_set_style_bg_color(rd_scene, lv_color_hex(0x293247), 0);
    lv_obj_set_style_bg_opa(rd_scene, LV_OPA_COVER, 0);
    lv_obj_add_flag(rd_scene, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_remove_flag(rd_scene, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(rd_scene, rd_touch, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(rd_scene, kn_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(rd_scene, kn_draw, LV_EVENT_DRAW_POST, NULL);
    for (int i = 0; i < RD_E; i++)
    {
        rd_lines[i] = lv_line_create(rd_scene);
        lv_obj_set_style_line_width(rd_lines[i], 18, 0);
        lv_obj_set_style_line_rounded(rd_lines[i], true, 0);
        lv_obj_set_style_line_color(rd_lines[i], lv_color_hex(0x8bd0d2), 0);
    }
    rd_head = box(rd_scene);
    lv_obj_set_size(rd_head, 46, 46);
    lv_obj_set_style_radius(rd_head, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(rd_head, lv_color_hex(0xf2c590), 0);
    lv_obj_set_style_bg_opa(rd_head, LV_OPA_COVER, 0);
    rd_face = text(rd_head, ":)", &ss_semibold_21, 0x303448);
    lv_obj_center(rd_face);
    kn_score_l = text(rd_scene, "", &ss_semibold_21, 0xffffff);
    lv_obj_set_pos(kn_score_l, 20, 14);
    kn_over_box = box(rd_scene);
    lv_obj_set_size(kn_over_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(kn_over_box, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_flex_flow(kn_over_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(kn_over_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(kn_over_box, 8, 0);
    kn_over_t = text(kn_over_box, "", &ss_semibold_26, 0xffffff);
    kn_over_s = text(kn_over_box, "", &ss_regular_18, C_SUB);
    lv_obj_set_style_text_align(kn_over_s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(kn_over_box, LV_OBJ_FLAG_HIDDEN);
    /* the cog sits in the header across from the back button, out of the
     * game's way; its control menu drops down from there */
    kn_cog = box(lv_obj_get_child(p, 0));
    lv_obj_set_size(kn_cog, 56, 56);
    lv_obj_align(kn_cog, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_radius(kn_cog, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(kn_cog, lv_color_hex(C_CARD_P), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kn_cog, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_center(text(kn_cog, LV_SYMBOL_SETTINGS, &sl_sym_24, 0xffffff));
    on_click(kn_cog, kn_cog_cb, NULL);
    kn_menu = box(p);
    lv_obj_set_size(kn_menu, 250, LV_SIZE_CONTENT);
    lv_obj_align(kn_menu, LV_ALIGN_TOP_RIGHT, -14, SB_H + 62);
    lv_obj_set_style_radius(kn_menu, SL_R_LG, 0);
    lv_obj_set_style_bg_color(kn_menu, lv_color_hex(0x0e121b), 0);
    lv_obj_set_style_bg_opa(kn_menu, 245, 0);
    lv_obj_set_style_border_color(kn_menu, lv_color_hex(C_SUB), 0);
    lv_obj_set_style_border_opa(kn_menu, 50, 0);
    lv_obj_set_style_border_width(kn_menu, 1, 0);
    lv_obj_set_style_pad_all(kn_menu, 8, 0);
    lv_obj_set_flex_flow(kn_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(kn_menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(kn_menu, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *head = text(kn_menu, "Controls", &ss_regular_18, C_SUB);
    lv_obj_set_style_pad_left(head, 10, 0);
    static const char *const ctrl_name[3] = { "Grab a limb", "Pull on a string", "Arrow pad" };
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *row = box(kn_menu);
        lv_obj_set_size(row, lv_pct(100), 46);
        lv_obj_set_style_radius(row, SL_R_MD, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(row, 40, LV_STATE_PRESSED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_align(text(row, ctrl_name[i], &ss_semibold_21, 0xffffff), LV_ALIGN_LEFT_MID, 10, 0);
        kn_ticks[i] = text(row, LV_SYMBOL_OK, &sl_sym_20, 0x8bd0d2);
        lv_obj_align(kn_ticks[i], LV_ALIGN_RIGHT_MID, -10, 0);
        on_click(row, kn_ctrl_cb, (void *)(intptr_t)i);
    }
    /* the pad: hold and slide, it follows the finger */
    kn_pad = box(body);
    lv_obj_add_style(kn_pad, &st_card, 0);
    lv_obj_set_size(kn_pad, lv_pct(100), 84);
    lv_obj_set_style_radius(kn_pad, SL_R_LG, 0);
    lv_obj_add_flag(kn_pad, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);
    lv_obj_remove_flag(kn_pad, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(kn_pad, kn_pad_cb, LV_EVENT_ALL, NULL);
    static const char *const glyph[4] = { LV_SYMBOL_LEFT, LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_RIGHT };
    static const lv_align_t where[4] = { LV_ALIGN_LEFT_MID, LV_ALIGN_TOP_MID, LV_ALIGN_BOTTOM_MID, LV_ALIGN_RIGHT_MID };
    static const short off[4][2] = { {44, 0}, {0, 8}, {0, -8}, {-44, 0} };
    for (int i = 0; i < 4; i++)
    {
        kn_arrows[i] = text(kn_pad, glyph[i], &sl_sym_28, C_SUB);
        lv_obj_align(kn_arrows[i], where[i], off[i][0], off[i][1]);
    }
    rd_hint = text(body, "Grab a limb and toss your ragdoll around", &ss_regular_18, C_SUB);
    lv_obj_set_width(rd_hint, lv_pct(100));
    lv_obj_set_style_text_align(rd_hint, LV_TEXT_ALIGN_CENTER, 0);   /* the caption of the stage above it */
    lv_label_set_long_mode(rd_hint, LV_LABEL_LONG_WRAP);
    rd_reset_row = card_row(body, "Reset ragdoll", "Start again in the middle", ACT_SOON);
    lv_obj_remove_event_cb_with_user_data(rd_reset_row, card_row_cb, (void *)(intptr_t)ACT_SOON);
    on_click(rd_reset_row, rd_reset_cb, NULL);
    rd_mode_row = card_row(body, "Knives", "Dodge the falling knives", ACT_SOON);
    lv_obj_remove_event_cb_with_user_data(rd_mode_row, card_row_cb, (void *)(intptr_t)ACT_SOON);
    on_click(rd_mode_row, rd_mode_cb, NULL);
    rd_timer = lv_timer_create(rd_tick, 25, NULL);
    kn_mode(false);
    page_push(p);
}
