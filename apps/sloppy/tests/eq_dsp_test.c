/* The EQ's DSP on the Mac: the real lib/rbcodec/dsp/eq.c and dsp_filter.c,
 * driven through eq.c's own entry points (dsp_set_eq_coefs, its process
 * function), against
 *   - the eq.c from before typed bands (old_*), for bit-exact old EQs, and
 *   - a double-precision RBJ cookbook reference, for the typed bands.
 * Run by run-eq-dsp-test.sh, which builds the old eq.c beside it. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "platform.h"
#include "dsp_core.h"
#include "dsp_proc_entry.h"
#include "dsp_misc.h"
#include "dsp_filter.h"
#include "eq.h"
#include "pga.h"
#include "fixedpoint.h"

/* ---- the DSP around eq.c, reduced to what it calls */
static unsigned int t_fs = 44100;
static struct dsp_config *t_dsp = (struct dsp_config *)&t_fs;
struct dsp_config *dsp_get_config(unsigned int id) { (void)id; return t_dsp; }
unsigned int dsp_get_output_frequency(struct dsp_config *d) { (void)d; return t_fs; }
static bool t_enabled, t_active;
void dsp_proc_enable(struct dsp_config *d, enum dsp_proc_ids id, bool en) { (void)d; (void)id; t_enabled = en; }
bool dsp_proc_enabled(struct dsp_config *d, enum dsp_proc_ids id) { (void)d; (void)id; return t_enabled; }
void dsp_proc_activate(struct dsp_config *d, enum dsp_proc_ids id, bool a) { (void)d; (void)id; t_active = a; }
void pga_set_gain(enum pga_gain_ids id, int32_t v) { (void)id; (void)v; }
void pga_enable_gain(enum pga_gain_ids id, bool en) { (void)id; (void)en; }
int find_first_set_bit(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
/* lib/rbcodec/metadata/replaygain.c's, as it is there */
#define FP_BITS (12)
#define FP_ONE (1 << FP_BITS)
#define FP_MIN (-48 * FP_ONE)
#define FP_MAX (17 * FP_ONE)
long get_replaygain_int(long int_gain)
{
    long gain = int_gain * FP_ONE / 100;
    gain = gain < FP_MIN ? FP_MIN : gain > FP_MAX ? FP_MAX : gain;
    return fp_factor(gain, FP_BITS) << (24 - FP_BITS);
}

/* the two eq.c's: today's, and the one before typed bands */
extern const struct dsp_proc_db_entry EQUALIZER_proc_db_entry;
struct old_eq_band_setting { int cutoff, q, gain; };
extern const struct dsp_proc_db_entry old_EQUALIZER_proc_db_entry;
void old_dsp_set_eq_coefs(int band, const struct old_eq_band_setting *s);

struct eqx { const struct dsp_proc_db_entry *db; struct dsp_proc_entry e; };
static struct eqx eq_new = { &EQUALIZER_proc_db_entry, { 0 } }, eq_old = { &old_EQUALIZER_proc_db_entry, { 0 } };
static void eqx_init(struct eqx *x) { x->db->configure(&x->e, t_dsp, DSP_PROC_INIT, 0); }
static void eqx_rate(struct eqx *x, unsigned int fs) { t_fs = fs; x->db->configure(&x->e, t_dsp, DSP_SET_OUT_FREQUENCY, fs); }
static void eqx_flush(struct eqx *x) { x->db->configure(&x->e, t_dsp, DSP_FLUSH, 0); }
static void eqx_run(struct eqx *x, int32_t *l, int32_t *r, int n, int ch)
{
    struct dsp_buffer b, *bp = &b;
    memset(&b, 0, sizeof b);
    b.remcount = n; b.p32[0] = l; b.p32[1] = r ? r : l; b.format.num_channels = ch;
    x->e.process(&x->e, &bp);
}
static void set_new(int band, int hz, int q, int g10, int type)
{
    struct eq_band_setting s = { hz, q, g10, type };
    dsp_set_eq_coefs(band, &s);
}
static void set_old(int band, int hz, int q, int g10)
{
    struct old_eq_band_setting s = { hz, q, g10 };
    old_dsp_set_eq_coefs(band, &s);
}
static void all_flat(void)
{
    for (int b = 0; b < EQ_NUM_BANDS; b++) { set_new(b, 1000, 10, 0, 0); set_old(b, 1000, 10, 0); }
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static int rnd_in(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }
static int rnd_hz(void) { return (int)lround(20.0 * pow(1000.0, (rnd() % 100000) / 100000.0)); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the double reference, RBJ's cookbook as written */
struct bq { double b0, b1, b2, a1, a2; };
static struct bq rbj(int type, double fc, double q, double g, double fs)
{
    double A = pow(10.0, g / 40.0), w = 2 * M_PI * fc / fs, cw = cos(w), al = sin(w) / (2 * q), sa = 2 * sqrt(A) * al;
    double b0, b1, b2, a0, a1, a2;
    switch (type)
    {
    case EQ_TYPE_LOW_PASS:  b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case EQ_TYPE_HIGH_PASS: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case EQ_TYPE_LOW_SHELF:
        b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa; break;
    case EQ_TYPE_HIGH_SHELF:
        b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa; break;
    default: b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;
    }
    struct bq r = { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    return r;
}
static double bq_mag(const struct bq *f, double fhz, double fs)
{
    double w = 2 * M_PI * fhz / fs, c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    double nr = f->b0 + f->b1 * c1 + f->b2 * c2, ni = -(f->b1 * s1 + f->b2 * s2);
    double dr = 1 + f->a1 * c1 + f->a2 * c2, di = -(f->a1 * s1 + f->a2 * s2);
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

/* ---- 1. old EQs: the fixed layout (EQ_TYPE_AUTO, Q up to 6.4) as every
 * EQ saved before typed bands has it, now on the typed biquads, against
 * the old eq.c on its old 32-bit filters: the same response within 0.1 dB
 * from 20 Hz to 20 kHz at every rate. Measured through each eq.c's own
 * entry points with a multitone: every frequency at once, each a whole
 * number of cycles in a 2 s window after the bands have settled, so the
 * old filters' truncation (a DC offset, limit cycles) stays out of the
 * response as it stays out of what anyone hears as tone. */
#define LEG_Q_MAX 64                        /* Q 6.4: the old filters' ceiling */
#define LEG_TONES 80
static double leg_worst = 0, leg_new_worst = 0, leg_old_worst = 0;
static char leg_what[200], leg_old_what[200];
static long leg_points, leg_off, leg_over;
static int leg_cfgs_over[4];
static bool leg_cfg_over;
static int32_t *leg_x, *leg_a, *leg_b;
/* the complex amplitude of y at bin f over n samples from 'from' */
static void leg_project(const int32_t *y, long from, long n, double f, double fs, double *re, double *im)
{
    double w = 2 * M_PI * f / fs, cw = cos(w), sw = sin(w), c = cos(w * from), s = sin(w * from), r = 0, i2 = 0;
    for (long i = 0; i < n; i++)
    {
        r += y[from + i] * c; i2 -= y[from + i] * s;
        double nc = c * cw - s * sw; s = s * cw + c * sw; c = nc;
        if ((i & 1023) == 1023) { double m = sqrt(c * c + s * s); c /= m; s /= m; }
    }
    *re = 2 * r / n; *im = 2 * i2 / n;
}
static void leg_one(unsigned fs, const int *hz, const int *q, const int *g, const char *what)
{
    /* settled: the slowest band rung down a millionfold */
    double ra = 0;
    for (int b = 0; b < EQ_NUM_BANDS; b++)
    {
        if (!g[b]) continue;
        int kind = b == 0 ? EQ_TYPE_LOW_SHELF : b == EQ_NUM_BANDS - 1 ? EQ_TYPE_HIGH_SHELF : EQ_TYPE_PEAK;
        struct bq r = rbj(kind, fp_div(hz[b], fs, 32) / 4294967296.0 * fs, q[b] / 10.0, g[b] / 10.0, fs);
        double disc = r.a1 * r.a1 - 4 * r.a2, m = sqrt(fabs(r.a2));
        if (disc > 0) { double p1 = fabs((-r.a1 + sqrt(disc)) / 2), p2 = fabs((-r.a1 - sqrt(disc)) / 2); m = p1 > p2 ? p1 : p2; }
        if (m > ra) ra = m;
    }
    long L = ra < 1 ? (long)(14.0 / (1 - ra)) : 4 << 20;
    if (L > (4 << 20)) L = 4 << 20;
    L = (L + 4095) & ~4095L;
    long N = 2 * (long)fs, T = L + N;       /* bins of 0.5 Hz */
    leg_cfg_over = false;
    /* the tones: 20 a decade from 20 Hz to 20 kHz, and each band's own */
    double fr[LEG_TONES], ph[LEG_TONES];
    int nf = 0;
    for (int k = 0; k <= 60; k++) fr[nf++] = round(2 * 20.0 * pow(1000.0, k / 60.0)) / 2;
    for (int b = 0; b < EQ_NUM_BANDS; b++)
    {
        double f = round(2.0 * hz[b]) / 2;
        bool dup = f < 20 || f > 20000;
        for (int k = 0; k < nf && !dup; k++) dup = fr[k] == f;
        if (!dup) fr[nf++] = f;
    }
    const double amp = 1 << 18;             /* a sum of eighty: room for ten bands' boosts */
    for (int k = 0; k < nf; k++) ph[k] = 2 * M_PI * (rnd() % 10000) / 10000.0;
    for (long i = 0; i < T; i++) leg_x[i] = 0;
    for (int k = 0; k < nf; k++)
    {
        double w = 2 * M_PI * fr[k] / fs, cw = cos(w), sw = sin(w), c = cos(ph[k]), s = sin(ph[k]);
        for (long i = 0; i < T; i++)
        {
            leg_x[i] += (int32_t)lrint(amp * s);
            double nc = c * cw - s * sw; s = s * cw + c * sw; c = nc;
            if ((i & 1023) == 1023) { double m = sqrt(c * c + s * s); c /= m; s /= m; }
        }
    }
    memcpy(leg_a, leg_x, T * sizeof *leg_x); memcpy(leg_b, leg_x, T * sizeof *leg_x);
    eqx_rate(&eq_new, fs); eqx_rate(&eq_old, fs);
    all_flat(); eqx_flush(&eq_new); eqx_flush(&eq_old);
    for (int b = 0; b < EQ_NUM_BANDS; b++) { set_new(b, hz[b], q[b], g[b], EQ_TYPE_AUTO); set_old(b, hz[b], q[b], g[b]); }
    for (long o = 0; o < T; o += 4096)
    {
        int n = T - o < 4096 ? (int)(T - o) : 4096;
        eqx_run(&eq_new, leg_a + o, NULL, n, 1); eqx_run(&eq_old, leg_b + o, NULL, n, 1);
    }
    for (int k = 0; k < nf; k++)
    {
        double ar, ai, br, bi, xr, xi;
        leg_project(leg_a, L, N, fr[k], fs, &ar, &ai);
        leg_project(leg_b, L, N, fr[k], fs, &br, &bi);
        leg_project(leg_x, L, N, fr[k], fs, &xr, &xi);
        double in = sqrt(xr * xr + xi * xi), ha = sqrt(ar * ar + ai * ai) / in, hb = sqrt(br * br + bi * bi) / in;
        double rr = 1;                      /* the cookbook, in double: what both were designed to draw */
        for (int b = 0; b < EQ_NUM_BANDS; b++)
        {
            int kind = b == 0 ? EQ_TYPE_LOW_SHELF : b == EQ_NUM_BANDS - 1 ? EQ_TYPE_HIGH_SHELF : EQ_TYPE_PEAK;
            struct bq r = rbj(kind, fp_div(hz[b], fs, 32) / 4294967296.0 * fs, q[b] / 10.0, g[b] / 10.0, fs);
            if (g[b]) rr *= bq_mag(&r, fr[k], fs);
        }
        if (rr < 1e-3 && hb < 1e-3) continue;   /* below -60 dB: no level to speak of */
        double db = fabs(20 * log10(ha / hb)), d_new = fabs(20 * log10(ha / rr)), d_old = fabs(20 * log10(hb / rr));
        leg_points++;
        if (d_new > leg_new_worst) leg_new_worst = d_new;
        /* today's draws the design */
        CHECK(d_new < 0.01, "old EQ %s at %u Hz: today %.4f dB off the cookbook at %.1f Hz", what, fs, d_new, fr[k]);
        if (d_old < 0.05)
        {
            /* where the old filters drew it too, the two agree */
            if (db > leg_worst)
            {
                leg_worst = db;
                snprintf(leg_what, sizeof leg_what, "%s at %u Hz, measured at %.1f Hz (%.2f dB there)", what, fs, fr[k], 20 * log10(hb));
            }
            CHECK(db < 0.1, "old EQ %s at %u Hz: %.4f dB from the old filters at %.1f Hz", what, fs, db, fr[k]);
        }
        else
        {
            /* where they didn't: the old filters' own error, which today's no longer makes */
            leg_off++;
            if (db > 0.1) { leg_over++; if (!leg_cfg_over) { leg_cfg_over = true; leg_cfgs_over[fs == 44100 ? 0 : fs == 48000 ? 1 : fs == 96000 ? 2 : 3]++; } }
            if (d_old > leg_old_worst)
            {
                leg_old_worst = d_old;
                snprintf(leg_old_what, sizeof leg_old_what, "%s at %u Hz, at %.1f Hz: old %.2f dB, cookbook and today %.2f dB",
                         what, fs, fr[k], 20 * log10(hb), 20 * log10(rr));
            }
        }
    }
}
static void test_legacy(void)
{
    static const unsigned rates[] = { 44100, 48000, 96000, 192000 };
    static const int oct[EQ_NUM_BANDS] = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    size_t cap = (4 << 20) + 2 * 192000;
    leg_x = malloc(sizeof(int32_t) * cap); leg_a = malloc(sizeof(int32_t) * cap); leg_b = malloc(sizeof(int32_t) * cap);
    int configs = 0;
    for (unsigned ri = 0; ri < 4; ri++)
    {
        int hz[EQ_NUM_BANDS], q[EQ_NUM_BANDS], g[EQ_NUM_BANDS];
        char what[64];
        /* the octave bands as shipped (shelves Q 0.7, peaks Q 1), a V and a cut */
        for (int shape = 0; shape < 3; shape++)
        {
            for (int b = 0; b < EQ_NUM_BANDS; b++)
            {
                hz[b] = oct[b]; q[b] = b == 0 || b == 9 ? 7 : 10;
                g[b] = shape == 0 ? 60 : shape == 1 ? (b < 3 || b > 7 ? 90 : -30) : -120;
            }
            snprintf(what, sizeof what, "octave bands, %s", shape == 0 ? "all +6 dB" : shape == 1 ? "a V" : "all -12 dB");
            leg_one(rates[ri], hz, q, g, what); configs++;
        }
        /* strong low boosts: a +24 dB shelf at 20 Hz, a Q 6.4 peak at 20 Hz */
        for (int b = 0; b < EQ_NUM_BANDS; b++) { hz[b] = oct[b]; q[b] = 10; g[b] = 0; }
        hz[0] = 20; q[0] = 7; g[0] = 240; hz[1] = 20; q[1] = LEG_Q_MAX; g[1] = 120;
        leg_one(rates[ri], hz, q, g, "20 Hz shelf +24 dB and 20 Hz Q 6.4 peak +12 dB"); configs++;
        /* and random ones over the whole old range */
        for (int k = 0; k < 24; k++)
        {
            for (int b = 0; b < EQ_NUM_BANDS; b++)
            {
                hz[b] = rnd_hz(); q[b] = rnd_in(1, LEG_Q_MAX);
                g[b] = rnd() % 4 == 0 ? 0 : rnd_in(-120, 120);
            }
            snprintf(what, sizeof what, "random %d", k);
            leg_one(rates[ri], hz, q, g, what); configs++;
        }
    }
    free(leg_x); free(leg_a); free(leg_b);
    printf("legacy: %d old EQs (fixed layout, Q 0.1..6.4), %d at each of 44.1, 48, 96 and 192 kHz, %ld points from 20 Hz to 20 kHz\n",
           configs, configs / 4, leg_points);
    printf("legacy: today's EQ against the cookbook design: worst %.4f dB\n", leg_new_worst);
    printf("legacy: where the old filters drew the design (within 0.05 dB): today's within %.4f dB of them; worst at %s\n", leg_worst, leg_what);
    printf("legacy: where they didn't: %ld points (%.1f%%), %ld more than 0.1 dB from today's; EQs with such a point: "
           "%d at 44.1 kHz, %d at 48, %d at 96, %d at 192\n",
           leg_off, 100.0 * leg_off / leg_points, leg_over, leg_cfgs_over[0], leg_cfgs_over[1], leg_cfgs_over[2], leg_cfgs_over[3]);
    printf("legacy: the old filters' worst: %s\n", leg_old_what);
}

/* ---- 2. typed bands: response, stability and decay, through eq.c */
static const char *const type_names[] = { "auto", "peak", "low shelf", "high shelf", "low pass", "high pass" };
static double worst_db = 0, worst_lin = 0, worst_pass = 0;
static char worst_what[160];
static long typed_configs, typed_samples;
#define IMP (1 << 24)                       /* an eighth of full scale: room for +24 dB */
#define IMP_CUT (1 << 26)                   /* half: cuts and pass filters (Q 20 rings +26 dB) */
static int32_t *ir;
static void one_typed(int type, int hz, int q10, int g10, unsigned fs)
{
    double fc = hz, q = q10 / 10.0, g = g10 / 10.0;
    /* eq.c hands the filter its frequency as a 32-bit phase: the reference takes the same */
    struct bq ref = rbj(type, fp_div(hz, fs, 32) / 4294967296.0 * fs, q, g, fs);
    /* long enough for the slowest pole to die away a millionfold */
    double r = sqrt(fabs(ref.a2)), ra = r;
    double disc = ref.a1 * ref.a1 - 4 * ref.a2;
    if (disc > 0) { double p1 = fabs((-ref.a1 + sqrt(disc)) / 2), p2 = fabs((-ref.a1 - sqrt(disc)) / 2); ra = p1 > p2 ? p1 : p2; }
    if (ra < r) ra = r;
    long L = ra < 1 ? (long)(16.0 / (1 - ra)) + 2048 : 4 << 20;
    if (L > (4 << 20)) L = 4 << 20;
    eqx_rate(&eq_new, fs);
    all_flat(); eqx_flush(&eq_new);
    set_new(5, hz, q10, g10, type);
    CHECK(t_active, "%s band not active", type_names[type]);
    memset(ir, 0, sizeof(int32_t) * (L + 4096));
    const int imp = g10 > 0 ? IMP : IMP_CUT;
    ir[0] = imp;
    for (long o = 0; o < L + 4096; o += 4096) eqx_run(&eq_new, ir + o, NULL, 4096, 1);
    typed_samples += L + 4096;
    typed_configs++;
    /* decayed: nothing but rounding left at the end, and silence after */
    int32_t tail = 0;
    for (long i = L; i < L + 4096; i++) if (abs(ir[i]) > tail) tail = abs(ir[i]);
    CHECK(tail <= 2, "%s %d Hz Q %.1f %+.1f dB at %u: tail %d after %ld samples", type_names[type], hz, q, g, fs, tail, L);
    /* the response at a log sweep, at fc and its band edges */
    double fr[64]; int nf = 0;
    for (int k = 0; k < 48; k++) { double f = 20.0 * pow(10.0, 3.0 * k / 47.0); if (f < 0.45 * fs) fr[nf++] = f; }
    fr[nf++] = fc; fr[nf++] = fc * (1 + 0.5 / q); fr[nf++] = fc / (1 + 0.5 / q);
    for (int k = 0; k < nf; k++)
    {
        if (fr[k] >= 0.47 * fs || fr[k] < 10) continue;
        double w = 2 * M_PI * fr[k] / fs, re = 0, im = 0, cw = cos(w), sw = sin(w), c = 1, s = 0;
        for (long i = 0; i < L; i++)
        {
            re += ir[i] * c; im -= ir[i] * s;
            double nc = c * cw - s * sw; s = s * cw + c * sw; c = nc;
            if ((i & 1023) == 1023) { double m = sqrt(c * c + s * s); c /= m; s /= m; }
        }
        double h = sqrt(re * re + im * im) / imp, hr = bq_mag(&ref, fr[k], fs);
        double lin = fabs(h - hr);
        if (lin > worst_lin) worst_lin = lin;
        if (hr > 1e-3)                      /* dB error where there's a level to speak of (above -60 dB) */
        {
            double db = fabs(20 * log10(h / hr));
            if (hr > 0.1 && db > worst_pass) worst_pass = db;
            if (db > worst_db)
            {
                worst_db = db;
                snprintf(worst_what, sizeof worst_what, "%s %d Hz Q %.1f %+.1f dB at %u Hz, measured at %.1f Hz (%.2f dB there)",
                         type_names[type], hz, q, g, fs, fr[k], 20 * log10(hr));
            }
            CHECK(db < 0.1, "%s %d Hz Q %.1f %+.1f dB at %u: %.4f dB off at %.1f Hz", type_names[type], hz, q, g, fs, db, fr[k]);
        }
        else CHECK(lin < 2e-4, "%s %d Hz Q %.1f %+.1f dB at %u: stopband %.2e off at %.1f Hz", type_names[type], hz, q, g, fs, lin, fr[k]);
    }
}
static void test_typed(void)
{
    static const unsigned rates[] = { 44100, 48000, 96000, 192000 };
    static const int hzs[] = { 20, 30, 45, 60, 100, 200, 1000, 5000, 12000, 20000 };   /* 45, 100, 200: just past fs / 1000 */
    static const int qs[] = { 1, 2, 5, 7, 20, 64, 100, 200 };
    static const int gs[] = { -240, -120, -30, 30, 120, 240 };
    ir = malloc(sizeof(int32_t) * ((4 << 20) + 8192));
    for (int type = EQ_TYPE_PEAK; type < EQ_TYPE_COUNT; type++)
        for (unsigned ri = 0; ri < 4; ri++)
            for (unsigned hi = 0; hi < sizeof hzs / sizeof hzs[0]; hi++)
                for (unsigned qi = 0; qi < sizeof qs / sizeof qs[0]; qi++)
                {
                    if (hzs[hi] > 0.45 * rates[ri]) continue;
                    if (eq_kind_is_pass(type)) { one_typed(type, hzs[hi], qs[qi], 0, rates[ri]); continue; }
                    for (unsigned gi = 0; gi < sizeof gs / sizeof gs[0]; gi++)
                        one_typed(type, hzs[hi], qs[qi], gs[gi], rates[ri]);
                }
    printf("typed: %ld filters (5 types x 4 rates x 10 frequencies x Q 0.1..20 x gains -24..+24 dB), %ld samples\n", typed_configs, typed_samples);
    printf("typed: worst response error %.4f dB (above -60 dB), worst linear error %.2e\n", worst_db, worst_lin);
    printf("typed: worst case: %s\n", worst_what);
    printf("typed: worst error where the response is above -20 dB: %.5f dB\n", worst_pass);
}

/* ---- 3. a sweep through ten extreme bands, against the double cascade */
struct bqs { double x1, x2, y1, y2; };
static double bq_step(const struct bq *f, struct bqs *s, double x)
{
    double y = f->b0 * x + f->b1 * s->x1 + f->b2 * s->x2 - f->a1 * s->y1 - f->a2 * s->y2;
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}
static const struct { int hz, q, g, type; } extreme[EQ_NUM_BANDS] = {
    { 20, 200, -120, EQ_TYPE_PEAK }, { 25, 7, 0, EQ_TYPE_HIGH_PASS }, { 40, 200, 120, EQ_TYPE_PEAK },
    { 80, 70, 60, EQ_TYPE_LOW_SHELF }, { 1000, 200, -240, EQ_TYPE_PEAK }, { 3000, 200, 120, EQ_TYPE_PEAK },
    { 6000, 30, -60, EQ_TYPE_PEAK }, { 9000, 50, 40, EQ_TYPE_HIGH_SHELF }, { 15000, 200, -120, EQ_TYPE_PEAK },
    { 19000, 7, 0, EQ_TYPE_LOW_PASS },
};
static void test_sweep(void)
{
    static const unsigned rates[] = { 44100, 48000, 96000, 192000 };
    for (unsigned ri = 0; ri < 4; ri++)
    {
        unsigned fs = rates[ri];
        long n = (long)fs * 8;              /* 8 s, 20 Hz to 20 kHz */
        int32_t *x = malloc(n * sizeof *x);
        double *ref = malloc(n * sizeof *ref);
        eqx_rate(&eq_new, fs); all_flat(); eqx_flush(&eq_new);
        struct bq f[EQ_NUM_BANDS]; struct bqs st[EQ_NUM_BANDS];
        memset(st, 0, sizeof st);
        for (int b = 0; b < EQ_NUM_BANDS; b++)
        {
            set_new(b, extreme[b].hz, extreme[b].q, extreme[b].g, extreme[b].type);
            f[b] = rbj(extreme[b].type, fp_div(extreme[b].hz, fs, 32) / 4294967296.0 * fs, extreme[b].q / 10.0, extreme[b].g / 10.0, fs);
        }
        double amp = 0.25 * (1 << 27), k = log(1000.0), T = 8.0, peak = 0;   /* -12 dBFS: +12 dB peaks stay inside */
        for (long i = 0; i < n; i++)
        {
            double t = (double)i / fs, ph = 2 * M_PI * 20 * T / k * (exp(t * k / T) - 1);
            x[i] = (int32_t)lrint(amp * sin(ph));
            double y = x[i];
            for (int b = 0; b < EQ_NUM_BANDS; b++) y = bq_step(&f[b], &st[b], y);
            ref[i] = y;
            if (fabs(y) > peak) peak = fabs(y);
        }
        for (long o = 0; o < n; o += 1024) eqx_run(&eq_new, x + o, NULL, n - o < 1024 ? (int)(n - o) : 1024, 1);
        double err2 = 0, emax = 0, sig2 = 0;
        bool sane = true;
        for (long i = 0; i < n; i++)
        {
            if (!isfinite(ref[i])) sane = false;
            double e = x[i] - ref[i];
            err2 += e * e; sig2 += ref[i] * ref[i];
            if (fabs(e) > emax) emax = fabs(e);
        }
        double fsc = 1 << 27;
        printf("sweep %6u Hz: ten bands (Q 20 peaks at 20 Hz, 40 Hz, 1 kHz, 3 kHz, 15 kHz; HP, LP, shelves): "
               "error RMS %.1f dBFS, peak %.1f dBFS, output peak %.1f dBFS\n",
               fs, 10 * log10(err2 / n / (fsc * fsc)), 20 * log10(emax / fsc), 20 * log10(peak / fsc));
        CHECK(sane, "the reference went non-finite");
        /* filters above fs/1000 hold their coefficients to 2^-28: a Q 20
           peak lands a few millihertz off, which a sweep sees as a phase
           difference of about -85 dBFS against a +5 dBFS output */
        CHECK(20 * log10(emax / fsc) < -80, "sweep error %.1f dBFS at %u", 20 * log10(emax / fsc), fs);
        free(x); free(ref);
        (void)sig2;
    }
}

/* ---- 4. low and sharp: the noise and DC a 20 Hz +12 dB peak adds, under a
 * 1 kHz tone, in the old eq.c (its old filters) and today's */
static double dc_offset;
static double hum_residue(struct eqx *x_eq, unsigned fs, int type, int q10)
{
    long n = (long)fs * 4;
    int32_t *x = malloc(n * sizeof *x);
    eqx_rate(x_eq, fs); all_flat(); eqx_flush(x_eq);
    if (x_eq == &eq_old) set_old(1, 20, q10, 120); else set_new(1, 20, q10, 120, type);
    struct bq f = rbj(EQ_TYPE_PEAK, fp_div(20, fs, 32) / 4294967296.0 * fs, q10 / 10.0, 12, fs);
    struct bqs st = { 0 };
    double e2 = 0, fsc = 1 << 27;
    double *ref = malloc(n * sizeof *ref);
    for (long i = 0; i < n; i++)
    {
        x[i] = (int32_t)lrint(0.1 * fsc * sin(2 * M_PI * 1000.0 * i / fs));
        ref[i] = bq_step(&f, &st, x[i]);
    }
    for (long o = 0; o < n; o += 1024) eqx_run(x_eq, x + o, NULL, 1024, 1);
    double mean = 0;
    for (long i = n / 2; i < n; i++) { double e = x[i] - ref[i]; e2 += e * e; mean += e; }
    mean /= n / 2;
    free(x); free(ref);
    dc_offset = 20 * log10(fabs(mean) / fsc + 1e-30);
    printf("      DC offset %6.1f dBFS, ", dc_offset);
    return 10 * log10(e2 / (n / 2) / (fsc * fsc));
}
static void test_low_sharp(void)
{
    static const unsigned rates[] = { 44100, 48000, 96000, 192000 };
    for (int k = 0; k < 4; k++)
    {
        printf("20 Hz +12 dB at %6u Hz:\n", rates[k]);
        double old64 = hum_residue(&eq_old, rates[k], 0, 64);
        double old_dc = dc_offset;
        printf("old eq.c, Q 6.4:     noise %.1f dBFS\n", old64);
        double auto64 = hum_residue(&eq_new, rates[k], EQ_TYPE_AUTO, 64);
        double auto_dc = dc_offset;
        printf("today, the same EQ:  noise %.1f dBFS\n", auto64);
        double new200 = hum_residue(&eq_new, rates[k], EQ_TYPE_PEAK, 200);
        printf("today, typed Q 20:   noise %.1f dBFS\n", new200);
        CHECK(new200 < -120 && auto64 < -120, "low sharp noise too high at %u", rates[k]);
        /* the old filters' truncation left a DC offset (-42 dBFS at 192 kHz); gone */
        CHECK(auto_dc < -120 && dc_offset < -120, "a DC offset at %u: %.1f dBFS", rates[k], auto_dc);
        (void)old_dc;
    }
}

/* ---- 5. full scale and past it: saturates, never wraps, and recovers */
static void test_overload(void)
{
    enum { N = 1 << 16 };
    static int32_t x[N];
    static double ref[N];
    eqx_rate(&eq_new, 48000); all_flat(); eqx_flush(&eq_new);
    struct bq f[EQ_NUM_BANDS]; struct bqs st[EQ_NUM_BANDS];
    memset(st, 0, sizeof st);
    for (int b = 0; b < EQ_NUM_BANDS; b++)
    {
        set_new(b, 30 + 200 * b, 200, 240, EQ_TYPE_PEAK);
        f[b] = rbj(EQ_TYPE_PEAK, fp_div(30 + 200 * b, 48000, 32) / 4294967296.0 * 48000, 20, 24, 48000);
    }
    /* 0 dBFS at three of the peaks, each +24 dB: 16x past full scale, and past the DSP's own 16x headroom */
    for (int i = 0; i < N; i++)
        x[i] = (int32_t)lrint((1 << 27) / 3.0 * (sin(2 * M_PI * 30 * i / 48000.0) + sin(2 * M_PI * 830 * i / 48000.0) + sin(2 * M_PI * 1830 * i / 48000.0)));
    for (int i = 0; i < N; i++) { double y = x[i]; for (int b = 0; b < EQ_NUM_BANDS; b++) y = bq_step(&f[b], &st[b], y); ref[i] = y; }
    for (int o = 0; o < N; o += 1024) eqx_run(&eq_new, x + o, NULL, 1024, 1);
    long sat = 0, wrong = 0, first = -1;
    for (int i = 0; i < N; i++)
    {
        bool s = x[i] == INT32_MAX || x[i] == INT32_MIN;
        if (s) { sat++; if (first < 0) first = i; }
        /* before anything clips, the curve; when it clips, the right way */
        if (first < 0 && fabs(x[i] - ref[i]) > 1e5) wrong++;
        if (s && (x[i] > 0) != (ref[i] > 0) && fabs(ref[i]) > 2.2e9) wrong++;
    }
    /* then silence: it settles back to nothing */
    int32_t tail = 0;
    for (int r = 0; r < 64; r++)
    {
        memset(x, 0, sizeof x);
        for (int o = 0; o < N; o += 1024) eqx_run(&eq_new, x + o, NULL, 1024, 1);
    }
    for (int i = 0; i < N; i++) if (llabs(x[i]) > tail) tail = (int32_t)llabs(x[i]);
    printf("overload: 0 dBFS into ten Q 20 +24 dB peaks: %ld of %d samples clipped, %ld wrong-signed or off the curve, "
           "then 4 M samples of silence end at %d LSB\n", sat, N, wrong, tail);
    CHECK(sat > 0, "the overload test never overloaded");
    CHECK(wrong == 0, "wrapped");
    CHECK(tail <= 2, "no decay after overload: %d", tail);

    /* past the filter's own range: a Q 20 low pass rings 20x on a sine at
       half of int32, its state saturating rather than its output */
    all_flat(); eqx_flush(&eq_new);
    set_new(3, 1000, 200, 0, EQ_TYPE_LOW_PASS);
    for (int i = 0; i < N; i++) x[i] = (int32_t)lrint((1 << 30) * sin(2 * M_PI * 1000 * i / 48000.0));
    for (int o = 0; o < N; o += 1024) eqx_run(&eq_new, x + o, NULL, 1024, 1);
    sat = 0;
    for (int i = 0; i < N; i++) if (x[i] == INT32_MAX || x[i] == INT32_MIN) sat++;
    for (int r = 0; r < 8; r++)
    {
        memset(x, 0, sizeof x);
        for (int o = 0; o < N; o += 1024) eqx_run(&eq_new, x + o, NULL, 1024, 1);
    }
    tail = 0;
    for (int i = 0; i < N; i++) if (llabs(x[i]) > tail) tail = (int32_t)llabs(x[i]);
    printf("overload: a Q 20 low pass driven 20x past int32 at its peak: %ld samples saturated, then silence ends at %d LSB\n", sat, tail);
    CHECK(sat > 0 && tail <= 2, "saturated state did not recover: %d", tail);

    /* any int32 at all, into every type at its extremes (run under
       -fsanitize=undefined to see that no sum overflows), then silence */
    int worst = 0, n = 0;
    static const int hz[] = { 20, 1000, 20000 }, qs[] = { 1, 7, 200 }, gs[] = { -240, 240 };
    for (int type = EQ_TYPE_PEAK; type < EQ_TYPE_COUNT; type++)
        for (int h = 0; h < 3; h++) for (int qi = 0; qi < 3; qi++) for (int gi = 0; gi < 2; gi++)
        {
            all_flat(); eqx_flush(&eq_new);
            set_new(2, hz[h], qs[qi], gs[gi], type);
            for (int i = 0; i < N; i++) x[i] = (int32_t)rnd() ^ (int32_t)(rnd() << 16);
            for (int o = 0; o < N; o += 1024) eqx_run(&eq_new, x + o, NULL, 1024, 1);
            for (int r = 0; r < 64; r++)   /* 20 Hz at Q 20 and +24 dB: a 1.3 s time constant */
            {
                memset(x, 0, sizeof x);
                for (int o = 0; o < N; o += 1024) eqx_run(&eq_new, x + o, NULL, 1024, 1);
            }
            for (int i = 0; i < N; i++) if (llabs(x[i]) > worst) worst = (int)llabs(x[i]);
            n++;
        }
    printf("overload: full-range int32 noise into %d extreme filters (48 kHz), then 87 s of silence: ends at %d LSB\n", n, worst);
    CHECK(worst <= 2, "no recovery from full-range noise: %d", worst);
}

/* ---- 6. an edit mid-stream: no click. The same sine through one band
 * held still, and through one edited every 10 ms (Q 6.4 / 6.5, the fixed
 * layout's type / its own, as a drag does): they differ by what Q 6.5
 * does, no more. */
static void test_switch(void)
{
    enum { N = 48000 };
    static int32_t x[N], y[N];
    for (int band = 0; band < 2; band++)
    {
        /* band 4: a peak; band 9: a +24 dB high shelf, whose typed form
           divides its b's by 16 and rescales its history on each switch */
        int b = band ? 9 : 4, g = band ? 240 : 120, ty = band ? EQ_TYPE_HIGH_SHELF : EQ_TYPE_PEAK;
        for (int run = 0; run < 2; run++)
        {
            int32_t *p = run ? y : x;
            eqx_rate(&eq_new, 48000); all_flat(); eqx_flush(&eq_new);
            set_new(b, 1000, 64, g, EQ_TYPE_AUTO);
            for (int i = 0; i < N; i++) p[i] = (int32_t)lrint(0.1 * (1 << 27) * sin(2 * M_PI * 440.0 * i / 48000));
            for (int o = 0; o < N; o += 480)
            {
                int k = run ? (o / 480) % 3 : 0;
                set_new(b, 1000, k ? 65 : 64, g, k == 2 ? ty : EQ_TYPE_AUTO);
                eqx_run(&eq_new, p + o, NULL, 480, 1);
            }
        }
        int32_t amp = 0; double d = 0;
        for (int i = 0; i < N; i++) { if (abs(x[i]) > amp) amp = abs(x[i]); if (fabs((double)x[i] - y[i]) > d) d = fabs((double)x[i] - y[i]); }
        /* Q 6.4 to 6.5 moves this response by about 1.5% (worked from RBJ) */
        printf("editing %s every 10 ms: at most %.2f%% from the band held still\n",
               band ? "a +24 dB high shelf" : "a +12 dB peak", 100 * d / amp);
        CHECK(d / amp < 0.03, "a click on an edit: %.2f%%", 100 * d / amp);
    }
}

/* ---- 7. what it costs, stereo, the old eq.c against today's: the octave
 * bands at 44.1 kHz, an AutoEq profile at 96 kHz (a low shelf, eight
 * peaks, a high shelf), and ten Q 20 bands at 192 kHz */
static const struct { int hz, q, g; } autoeq[EQ_NUM_BANDS] = {   /* a typical AutoEq ParametricEQ.txt */
    { 105, 7, 55 }, { 62, 4, -12 }, { 180, 9, -21 }, { 1250, 14, 17 }, { 2600, 22, -26 },
    { 3600, 30, 38 }, { 5300, 35, -44 }, { 7200, 40, 31 }, { 8800, 12, -18 }, { 10000, 7, -25 },
};
static double cost_run(struct eqx *x, unsigned fs)
{
    enum { N = 1024 };
    static int32_t l[N], r[N], src[N];
    for (int i = 0; i < N; i++) src[i] = (int32_t)(rnd() % 0x1000000) - 0x800000;   /* -24 dBFS noise */
    long blocks = (long)fs * 20 / N;        /* 20 s of audio */
    double best = 1e9;
    for (int rep = 0; rep < 3; rep++)
    {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (long k = 0; k < blocks; k++) { memcpy(l, src, sizeof l); memcpy(r, src, sizeof r); eqx_run(x, l, r, N, 2); }
        clock_gettime(CLOCK_MONOTONIC, &b);
        double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
        if (s < best) best = s;
    }
    return best / 20 * 1000;                /* ms per second of audio */
}
static void test_cost(void)
{
    static const int oct[EQ_NUM_BANDS] = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    for (int c = 0; c < 3; c++)
    {
        unsigned fs = c == 0 ? 44100 : c == 1 ? 96000 : 192000;
        double t[2];
        for (int k = 0; k < 2; k++)
        {
            struct eqx *x = k ? &eq_new : &eq_old;
            eqx_rate(x, fs); all_flat(); eqx_flush(x);
            for (int b = 0; b < EQ_NUM_BANDS; b++)
            {
                int hz = c == 0 ? oct[b] : c == 1 ? autoeq[b].hz : 30 + 1000 * b;
                int q = c == 0 ? (b == 0 || b == 9 ? 7 : 10) : c == 1 ? autoeq[b].q : k ? 200 : LEG_Q_MAX;
                int g = c == 0 ? 60 : c == 1 ? autoeq[b].g : 60;
                if (k) set_new(b, hz, q, g, c == 2 ? EQ_TYPE_PEAK : EQ_TYPE_AUTO); else set_old(b, hz, q, g);
            }
            t[k] = cost_run(x, fs);
        }
        printf("cost (Mac): %s: old filters %.2f ms, today %.2f ms per second of audio (%.2fx)\n",
               c == 0 ? "ten octave bands, 44.1 kHz stereo" : c == 1 ? "an AutoEq profile, 96 kHz stereo" : "ten Q 20 bands, 192 kHz stereo",
               t[0], t[1], t[1] / t[0]);
    }
}

/* ---- 8. config.cfg: old lines load as the fixed layout and save back the
 * same; typed bands round-trip; and an old player reading a typed line
 * still gets its frequency, Q and gain (and ignores the type) */
#ifdef EQ_CFG_TEST
#include "eq_cfg_new.inc"
#include "eq_cfg_old.inc"
static void test_cfg(void)
{
    static const char *const old_lines[] = { "32, 7, 0", "64, 10, -35", "16000, 7, 120", "1000,14,-5", " 250, 64, 240" };
    char line[64], buf[64];
    int n = 0;
    for (unsigned i = 0; i < sizeof old_lines / sizeof old_lines[0]; i++)
    {
        struct eq_band_setting e = { 1, 2, 3, EQ_TYPE_LOW_PASS };   /* a typed band before: the line must reset it */
        struct old_eq_band_setting o = { 0 };
        strcpy(line, old_lines[i]); eq_load_from_cfg(&e, line);
        strcpy(line, old_lines[i]); old_eq_load_from_cfg(&o, line);
        CHECK(e.cutoff == o.cutoff && e.q == o.q && e.gain == o.gain && e.type == EQ_TYPE_AUTO, "old line \"%s\" read differently", old_lines[i]);
        char ob[64];
        eq_write_to_cfg(&e, buf, sizeof buf); old_eq_write_to_cfg(&o, ob, sizeof ob);
        CHECK(!strcmp(buf, ob), "old band saves as \"%s\", it was \"%s\"", buf, ob);
        n++;
    }
    for (int t = EQ_TYPE_AUTO; t < EQ_TYPE_COUNT; t++)
    {
        struct eq_band_setting e = { 30, 200, -120, t }, r = { 0 };
        struct old_eq_band_setting o = { 0 };
        eq_write_to_cfg(&e, buf, sizeof buf);
        strcpy(line, buf); eq_load_from_cfg(&r, line);
        CHECK(!memcmp(&e, &r, sizeof e), "type %d round trip \"%s\"", t, buf);
        strcpy(line, buf); old_eq_load_from_cfg(&o, line);
        CHECK(o.cutoff == 30 && o.q == 200 && o.gain == -120, "an old player misreads \"%s\"", buf);
        if (t == EQ_TYPE_HIGH_PASS) printf("cfg: a typed band saves as \"%s\"; the old player reads it as %d Hz, Q %d, gain %d\n", buf, o.cutoff, o.q, o.gain);
        n++;
    }
    /* nonsense types read as the fixed layout */
    struct eq_band_setting e = { 0 };
    strcpy(line, "100, 10, 20, 9"); eq_load_from_cfg(&e, line);
    CHECK(e.type == EQ_TYPE_AUTO && e.gain == 20, "type 9 kept");
    printf("cfg: %d lines: old lines read and save identically, every type round-trips\n", n + 1);
}
#endif

int main(int argc, char **argv)
{
    eqx_init(&eq_new); eqx_init(&eq_old);
    dsp_eq_enable(true);
    const char *only = argc > 1 ? argv[1] : "";
    if (!*only || !strcmp(only, "legacy")) test_legacy();
    if (!*only || !strcmp(only, "typed")) test_typed();
    if (!*only || !strcmp(only, "sweep")) test_sweep();
    if (!*only || !strcmp(only, "low")) test_low_sharp();
    if (!*only || !strcmp(only, "overload")) test_overload();
    if (!*only || !strcmp(only, "switch")) test_switch();
#ifdef EQ_CFG_TEST
    if (!*only || !strcmp(only, "cfg")) test_cfg();
#endif
    if (!*only || !strcmp(only, "cost")) test_cost();
    if (!strcmp(only, "ovl"))
    {
        int type = atoi(argv[2]), hz = atoi(argv[3]), q = atoi(argv[4]), g = atoi(argv[5]); unsigned fs = atoi(argv[6]);
        eqx_rate(&eq_new, fs); all_flat(); eqx_flush(&eq_new);
        if (argc > 7) for (int b = 0; b < atoi(argv[7]); b++) set_new(b, hz + 200 * b, q, g, type);
        else set_new(5, hz, q, g, type);
        static int32_t x[1 << 16];
        for (int i = 0; i < (1 << 16); i++) x[i] = (int32_t)(rnd() % (1u << 29)) - (1 << 28);
        eqx_run(&eq_new, x, NULL, 1 << 16, 1);
        int sat = 0; for (int i = 0; i < (1 << 16); i++) sat += x[i] == INT32_MAX || x[i] == INT32_MIN;
        printf("sat %d\n", sat);
        for (int r = 0; r < 40; r++) { memset(x, 0, sizeof x); eqx_run(&eq_new, x, NULL, 1 << 16, 1);
            int32_t t = 0; for (int i = 0; i < (1 << 16); i++) if (abs(x[i]) > t) t = abs(x[i]); printf("%d ", t); }
        printf("\n");
    }
    if (!strcmp(only, "cmp"))   /* band 0: old filter at argv q vs typed at q+1, a sine through each */
    {
        int q = atoi(argv[2]), g = atoi(argv[3]), hz = atoi(argv[4]);
        static int32_t a[48000], b[48000];
        for (int i = 0; i < 48000; i++) a[i] = b[i] = (int32_t)lrint(0.1 * (1 << 27) * sin(2 * M_PI * 440.0 * i / 48000));
        eqx_rate(&eq_new, 48000); all_flat(); eqx_flush(&eq_new); set_new(0, hz, q, g, 0);
        eqx_run(&eq_new, a, NULL, 48000, 1);
        all_flat(); eqx_flush(&eq_new); set_new(0, hz, q + 1, g, 0);
        eqx_run(&eq_new, b, NULL, 48000, 1);
        int32_t ma = 0, mb = 0; for (int i = 24000; i < 48000; i++) { if (abs(a[i]) > ma) ma = abs(a[i]); if (abs(b[i]) > mb) mb = abs(b[i]); }
        printf("old peak %d typed peak %d\n", ma, mb);
    }
    if (!strcmp(only, "coefs"))
    {
        int type = atoi(argv[2]), hz = atoi(argv[3]), q = atoi(argv[4]), g = atoi(argv[5]); unsigned fs = atoi(argv[6]);
        struct dsp_filter f; struct dsp_filter_ef ef;
        filter_bq_coefs(type, fp_div(hz, fs, 32), q, g, &f, &ef);
        struct bq r = rbj(type, fp_div(hz, fs, 32) / 4294967296.0 * fs, q / 10.0, g / 10.0, fs);
        double p = 1 << ef.post, c[5];
        for (int i = 0; i < 5; i++) c[i] = f.coefs[i] / ldexp(1, 28) + ef.fine[i] / ldexp(1, 40);
        printf("post %d\n", ef.post);
        printf("b0 %.14f %.14f\nb1 %.14f %.14f\nb2 %.14f %.14f\na1 %.14f %.14f\na2 %.14f %.14f\n",
               c[0] * p, r.b0, c[1] * p, r.b1, c[2] * p, r.b2, -c[3], r.a1, -c[4], r.a2);
    }
    if (!strcmp(only, "probe"))
    {
        ir = malloc(sizeof(int32_t) * (1 << 22));
        int type = atoi(argv[2]), hz = atoi(argv[3]), q = atoi(argv[4]), g = atoi(argv[5]); unsigned fs = atoi(argv[6]);
        eqx_rate(&eq_new, fs); all_flat(); eqx_flush(&eq_new); set_new(5, hz, q, g, type);
        memset(ir, 0, sizeof(int32_t) << 22); ir[0] = IMP;
        for (long o = 0; o < (1 << 22); o += 4096) eqx_run(&eq_new, ir + o, NULL, 4096, 1);
        for (long b = 1024; b < (1 << 22); b *= 2) { int32_t t = 0; for (long i = b; i < 2 * b; i++) if (abs(ir[i]) > t) t = abs(ir[i]); printf("%ld..%ld: %d\n", b, 2 * b, t); }
        for (int i = (1 << 22) - 16; i < (1 << 22); i++) printf("%d ", ir[i]);
        printf("\n");
    }
    printf(fails ? "eq_dsp_test: %d FAILED\n" : "eq_dsp_test: all passed\n", fails);
    return fails != 0;
}
