/*
 *  Spectrum analyzer tile: the music that is playing, three ways. The
 *  edit control that resizes other tiles cycles them here:
 *
 *    bars   an LED analyzer: lit segments, the top one glowing by how far
 *           into it the level is, and a cap floating at each peak;
 *    peaks  the caps alone, each hanging a thin stem of light down to
 *           the level now;
 *    curve  a smooth line over a soft fill, as a mixing plugin draws it,
 *           with a peak-hold line, a grid and the frequencies marked.
 *
 *  The samples are the playback channel's own, after the DSP: the mixer
 *  hands each new buffer to a hook, which folds it to mono into a ring.
 *  Nothing is played, paused or rerouted; the hook is only set while the
 *  tile is on screen and is cleared when it scrolls away, Start is covered
 *  or the tile goes. A 2048 point real FFT (21.5 Hz bins at 44.1 kHz, done
 *  as a 1024 point complex one) is summed into log-spaced bands from 40 Hz
 *  to 14 kHz, so bass gets real resolution and the highs are not a forest
 *  of empty bars.
 *
 *  Levels are in dB. The scale follows the record: its top tracks the
 *  loudest band (straight up, back down at 1.5 dB a second) and starts
 *  over with each song, so a quiet master fills the meter while a quiet
 *  passage in a loud song still looks quiet. Colour is by height, which is
 *  level, the way LED meters are built: green, then yellow, red at the
 *  top. Levels rise at once and fall smoothly; peaks hold, then drop with
 *  gravity. With no music everything settles and the tile stops drawing.
 *
 *  The mode is kept in the slot's live field, above its on/off bit, so a
 *  tiles.txt read by an older build still just means "live".
 */

#define SP_N      2048
#define SP_RING   32768                  /* 2048 for the window, the rest for a Bluetooth delay */
#define SP_BARS   72                     /* bands at most: the wide curve */
#define SP_ROWS   480                    /* canvas rows at most: the full-screen app */
#define SP_COLS   800                    /* canvas columns at most */
#define SP_RANGE  54.0f                  /* dB from an empty bar to a full one */
#define SP_LO_HZ  40.0f
#define SP_HI_HZ  14000.0f
/* a full-scale sine through the Hann window: 20 log10(32767 * N / 4), plus
 * the window's 1.76 dB spread over its neighbouring bins */
#define SP_REF_DB 146.3f
#define SP_BG     0x090d0b
#define SP_GRID   0x16211b

enum { SP_MODE_BARS, SP_MODE_PEAKS, SP_MODE_CURVE };   /* SP_MODES is with the tiles */
static const char *const sp_mode_names[SP_MODES] = { "Bars and peaks", "Peaks", "Smooth curve" };
static int sp_mode_of(const struct mt_slot *s) { return (s->live >> 1) % SP_MODES; }

static int16_t sp_ring[SP_RING];
static uint32_t sp_end;                  /* where in the ring the last analysis ended: the visualizer's wave */
static volatile uint32_t sp_written, sp_chunk_frames;
static volatile long sp_chunk_tick;
static volatile int sp_peak;             /* the loudest sample since the waveform took it, 0..32768 */
static volatile uint32_t sp_bufs;        /* buffers heard, for the waveform to tell none came */

#define SP_M      (SP_N / 2)             /* the complex FFT a real one packs into */
static int32_t sp_re[SP_M], sp_im[SP_M];
static float sp_pow[SP_M + 1];
static int16_t sp_win[SP_N], sp_cos[SP_N / 2], sp_sin[SP_N / 2];

static struct
{
    lv_obj_t *canvas;
    lv_timer_t *timer;
    uint16_t *px;
    int mode, cw, ch, stride, bars, bw, gap, x0;
    uint32_t rate;                       /* the band edges are for this rate */
    float lo[SP_BARS], hi[SP_BARS];      /* band edges, in bins */
    float lvl[SP_BARS], peak[SP_BARS], peak_v[SP_BARS], peak_hold[SP_BARS];
    int16_t drawn[SP_BARS][3];           /* bars and peaks: what each bar shows now */
    uint16_t row_led[SP_ROWS];           /* the curve's line at this row's height */
    uint16_t row_fill[SP_ROWS];          /* the curve's fill */
    uint16_t row_bg[SP_ROWS];            /* the curve's background, grid lines in it */
    /* bars and peaks: the background, and the level colour 0..16 sixteenths
     * of the way over it, per row; below the baseline, the mirrored row's
     * colour, for the reflection. Every pixel is then a table read. */
    uint16_t bgz[SP_ROWS], tab[SP_ROWS][17], pill[SP_ROWS];
    uint8_t capm[8][16];                 /* a bar's rounded top, in sixteenths */
    int base, barh, refl, rcap;          /* baseline row, full height, reflection rows */
    uint8_t vline[SP_COLS];              /* the curve's grid: a frequency mark here */
    float ydraw[SP_COLS], pdraw[SP_COLS];   /* the curve and peak line as last drawn */
    int16_t span[SP_COLS][2];            /* the rows they took up */
    float top;                           /* the loudest band lately, dB */
    char song[MAX_PATH];                 /* the scale is for this one */
    uint32_t last_ms;
    int dx0, dx1;                        /* the columns drawn since the last frame */
    bool hooked, settled;
} sp;

/* The full-screen app: landscape, on a screen of its own as Cover Flow is,
 * one mode at a time and a swipe to the next. */
static struct
{
    bool active;
    lv_obj_t *home, *scr, *stage, *chrome, *name, *dots[SP_MODES];
    lv_timer_t *hide;
    int mode;
    lv_point_t down;
    bool moved;
} spa;
static void sp_dirty(int x, int w)
{
    if (x < sp.dx0) sp.dx0 = x;
    if (x + w > sp.dx1) sp.dx1 = x + w;
}

/* The mixer's thread, once per new buffer: as cheap as it can be. */
static void sp_hook(const void *start, size_t size)
{
    const int16_t *s = start;
    uint32_t frames = size / 4, w = sp_written;
    if (frames > SP_RING / 2) frames = SP_RING / 2;
    int peak = sp_peak;                  /* the waveform seek bar's: the loudest since it last looked */
    for (uint32_t i = 0; i < frames; i++)
    {
        int m = (s[2 * i] + s[2 * i + 1]) >> 1;
        sp_ring[(w + i) & (SP_RING - 1)] = (int16_t)m;
        if (m < 0) m = -m;
        if (m > peak) peak = m;
    }
    sp_peak = peak;
    sp_bufs++;
    sp_chunk_tick = current_tick;
    sp_chunk_frames = frames;
    sp_written = w + frames;
}

/* Who listens: the analyzers (SP_FOR_VIEW) and the waveform seek bar
 * (SP_FOR_WAVE). The hook stays while either does. */
#define SP_FOR_VIEW 1
#define SP_FOR_WAVE 2
static uint8_t sp_users;
static void sp_listen_for(uint8_t who, bool on)
{
    uint8_t u = on ? (uint8_t)(sp_users | who) : (uint8_t)(sp_users & ~who);
    sp_users = u;
    if (!!u == sp.hooked) return;
    mixer_channel_set_buffer_hook(PCM_MIXER_CHAN_PLAYBACK, u ? sp_hook : NULL);
    sp.hooked = u != 0;
}
static void sp_listen(bool on) { sp_listen_for(SP_FOR_VIEW, on); }

/* Fixed point, in place, radix 2, M points. Samples are 16 bits and M is
 * 2^10, so nothing grows past 26 bits and no stage needs scaling down. */
static void sp_fft(void)
{
    for (int i = 1, j = 0; i < SP_M; i++)
    {
        int bit = SP_M >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j)
        {
            int32_t t = sp_re[i]; sp_re[i] = sp_re[j]; sp_re[j] = t;
            t = sp_im[i]; sp_im[i] = sp_im[j]; sp_im[j] = t;
        }
    }
    for (int len = 2; len <= SP_M; len <<= 1)
    {
        int half = len >> 1, step = SP_N / len;      /* the twiddles are for N points */
        for (int i = 0; i < SP_M; i += len)
            for (int k = 0; k < half; k++)
            {
                int a = i + k, b = a + half;
                int64_t wr = sp_cos[k * step], wi = sp_sin[k * step];
                int32_t tr = (int32_t)((sp_re[b] * wr - sp_im[b] * wi) >> 15);
                int32_t ti = (int32_t)((sp_re[b] * wi + sp_im[b] * wr) >> 15);
                sp_re[b] = sp_re[a] - tr; sp_im[b] = sp_im[a] - ti;
                sp_re[a] += tr; sp_im[a] += ti;
            }
    }
}

/* N real samples went in as M complex ones, even samples real, odd ones
 * imaginary. Untangle the two halves' spectra and join them: the power of
 * bins 0 to M of the N point transform. */
static void sp_split(void)
{
    for (int k = 0; k <= SP_M; k++)
    {
        int a = k % SP_M, b = (SP_M - k) % SP_M;
        float er = (sp_re[a] + (float)sp_re[b]) / 2, ei = (sp_im[a] - (float)sp_im[b]) / 2;
        float orr = (sp_im[a] + (float)sp_im[b]) / 2, oi = (sp_re[b] - (float)sp_re[a]) / 2;
        float wr = k < SP_M ? sp_cos[k] / 32767.0f : -1, wi = k < SP_M ? sp_sin[k] / 32767.0f : 0;
        float xr = er + wr * orr - wi * oi, xi = ei + wr * oi + wi * orr;
        sp_pow[k] = xr * xr + xi * xi;
    }
}
static float sp_power(int bin) { return sp_pow[bin]; }

/* Is the music playing right now? Then window the last N frames heard and
 * fill db[] per band. The window slides with the clock between buffers,
 * from where the newest buffer started playing. */
static bool sp_analyze(float *db)
{
    if (mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) != CHANNEL_PLAYING) return false;
    uint32_t written = sp_written, frames = sp_chunk_frames;
    long age = current_tick - sp_chunk_tick;
    if (!frames || age > HZ / 4) return false;              /* no buffer lately */
    uint32_t rate = mixer_get_frequency();
    if (rate < 8000) rate = 44100;
    uint32_t into = (uint32_t)age * rate / HZ;
    uint32_t end = written - frames + (into < frames ? into : frames);
    /* the speaker plays this much later over Bluetooth: draw what it plays */
    uint32_t lag = (uint32_t)((uint64_t)sl_out_latency_ms() * rate / 1000);
    if (lag > SP_RING - SP_N - SP_RING / 8) lag = SP_RING - SP_N - SP_RING / 8;
    end -= lag;
    sp_end = end;
    for (int i = 0; i < SP_M; i++)
    {
        uint32_t at = end - SP_N + 2 * i;
        sp_re[i] = sp_ring[at & (SP_RING - 1)] * sp_win[2 * i] >> 15;
        sp_im[i] = sp_ring[(at + 1) & (SP_RING - 1)] * sp_win[2 * i + 1] >> 15;
    }
    sp_fft();
    sp_split();
    if (rate != sp.rate)
    {
        sp.rate = rate;
        for (int b = 0; b < sp.bars; b++)
        {
            float f0 = SP_LO_HZ * (float)pow(SP_HI_HZ / SP_LO_HZ, (float)b / sp.bars);
            float f1 = SP_LO_HZ * (float)pow(SP_HI_HZ / SP_LO_HZ, (float)(b + 1) / sp.bars);
            sp.lo[b] = f0 * SP_N / rate;
            sp.hi[b] = f1 * SP_N / rate;
        }
    }
    for (int b = 0; b < sp.bars; b++)
    {
        /* the energy in the band; a band narrower than two bins takes its
         * share of the spectrum at its centre, read between the bins */
        float lo = sp.lo[b], hi = sp.hi[b], p = 0;
        int j0 = (int)ceilf(lo), j1 = (int)ceilf(hi);     /* the bins in [lo, hi) */
        if (j1 > SP_M) j1 = SP_M;
        if (hi - lo >= 2)
            for (int j = j0; j < j1; j++) p += sp_power(j);
        else
        {
            float c = (lo + hi) / 2;
            int j = (int)c;
            float t = c - j;
            p = (sp_power(j) * (1 - t) + sp_power(j + 1) * t) * (hi - lo);
        }
        db[b] = p > 1 ? 10 * log10f(p) - SP_REF_DB : -120;
    }
    return true;
}

/* Colour by height: green, yellow, red. */
static uint32_t sp_mix(uint32_t a, uint32_t b, int t)      /* t of 256 */
{
    int r = ((a >> 16) & 255) + ((((int)(b >> 16) & 255) - (int)((a >> 16) & 255)) * t >> 8);
    int g = ((a >> 8) & 255) + ((((int)(b >> 8) & 255) - (int)((a >> 8) & 255)) * t >> 8);
    int bl = (a & 255) + (((int)(b & 255) - (int)(a & 255)) * t >> 8);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
}
static uint16_t sp_565(uint32_t c) { return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x1f)); }
/* RGB565 for a row of a gradient: rounded up or down by the row, so a
 * dark slope steps a line at a time instead of in bands */
static uint16_t sp_565d(uint32_t c, int row)
{
    static const uint8_t q[4] = { 0, 2, 1, 3 };
    int t = q[row & 3], r = ((c >> 16) & 255) + 2 * t, g = ((c >> 8) & 255) + t, b = (c & 255) + 2 * t;
    return sp_565((uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b));
}
static uint32_t sp_888(uint16_t c)
{
    uint32_t r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}
static uint32_t sp_led(float f)
{
    static const struct { float at; uint32_t c; } stop[] = {
        { 0.00f, 0x14c45a }, { 0.52f, 0x62e03c }, { 0.72f, 0xffd626 }, { 0.87f, 0xff8b1f }, { 1.00f, 0xff3030 },
    };
    int k = 1;
    while (k < 4 && f > stop[k].at) k++;
    float t = (f - stop[k - 1].at) / (stop[k].at - stop[k - 1].at);
    return sp_mix(stop[k - 1].c, stop[k].c, (int)(t < 0 ? 0 : t > 1 ? 256 : t * 256));
}
/* a canvas pixel, a part of the way to colour c */
static void sp_blend(int x, int y, uint32_t c, int t)
{
    if (t <= 0 || y < 0 || y >= sp.ch) return;
    uint16_t *p = sp.px + y * sp.stride + x;
    *p = t >= 256 ? sp_565(c) : sp_565(sp_mix(sp_888(*p), c, t));
}

static void sp_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int r = 0; r < h; r++)
    {
        uint16_t *row = sp.px + (y + r) * sp.stride + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

/* Zune's colours, by level: violet at rest, through magenta and hot pink
 * to orange, and gold at the very top. */
static uint32_t sp_zune(float f)
{
    static const struct { float at; uint32_t c; } stop[] = {
        { 0.00f, 0x5a24d8 }, { 0.30f, 0xb21ba0 }, { 0.56f, 0xf2287a }, { 0.79f, 0xff6b2c }, { 1.00f, 0xffcd52 },
    };
    int k = 1;
    while (k < 4 && f > stop[k].at) k++;
    float t = (f - stop[k - 1].at) / (stop[k].at - stop[k - 1].at);
    return sp_mix(stop[k - 1].c, stop[k].c, (int)(t < 0 ? 0 : t > 1 ? 256 : t * 256));
}

/* w pixels of row r from column x, the level colour a sixteenths over the
 * background */
static void sp_span(int x, int r, int w, int a)
{
    if (r < 0 || r >= sp.ch || a <= 0 || w <= 0) return;
    uint16_t c = sp.tab[r][a > 16 ? 16 : a], *p = sp.px + r * sp.stride + x;
    for (int i = 0; i < w; i++) p[i] = c;
}
static void sp_clear_bar(int x)
{
    for (int r = 0; r < sp.ch; r++)
    {
        uint16_t c = sp.bgz[r], *p = sp.px + r * sp.stride + x;
        for (int i = 0; i < sp.bw; i++) p[i] = c;
    }
}
/* The row a level reaches: at rest a bar is still a two pixel nub. */
static int sp_row_of(float v)
{
    v = v > 1 ? 1 : v > 0 ? v : 0;      /* NaN too: (int)NaN is INT_MAX on the R1 */
    return sp.base - 2 - (int)(v * (sp.barh - 2) + 0.5f);
}

/* Bars: a rounded bar from the baseline up to its level, a soft glow over
 * its top, its reflection fading under the baseline, and a pill floating
 * at its peak. */
static void sp_draw_bar(int b, int top, int pk)
{
    int x = sp.x0 + b * (sp.bw + sp.gap), rc = sp.rcap < sp.base - top ? sp.rcap : sp.base - top;
    sp_dirty(x, sp.bw);
    sp_clear_bar(x);
    sp_span(x + sp.bw / 4, top - 2, sp.bw - sp.bw / 2, 2);
    sp_span(x + 1, top - 1, sp.bw - 2, 3);
    for (int r = top > 0 ? top : 0; r < sp.base; r++)
    {
        int dy = r - top;
        if (dy >= rc) { sp_span(x, r, sp.bw, 16); continue; }
        for (int i = 0; i < sp.bw; i++) sp_span(x + i, r, 1, sp.capm[dy + sp.rcap - rc][i]);
    }
    for (int k = 0; k < sp.refl && sp.base - 1 - k >= top; k++)
        sp_span(x, sp.base + 1 + k, sp.bw, 6 * (sp.refl - k) / sp.refl);
    if (pk >= 0)
    {
        sp_span(x + 1, pk - 1, sp.bw - 2, 5);
        for (int r = pk; r < pk + 2; r++)
        {
            uint16_t *p = sp.px + r * sp.stride + x;
            for (int i = 0; i < sp.bw; i++) p[i] = sp.pill[r];
        }
        sp_span(x + 1, pk + 2, sp.bw - 2, 5);
    }
}

/* Peaks: each band a glowing pill at its peak with a curtain of light
 * under it, a brighter stem hanging down to where the music is now and a
 * small tick at the level itself; a dim dot marks the band. */
static void sp_draw_peak(int b, int pk, int lv)
{
    int x = sp.x0 + b * (sp.bw + sp.gap), sw = sp.bw / 3 < 2 ? 2 : sp.bw / 3, sx = x + (sp.bw - sw) / 2;
    sp_dirty(x, sp.bw);
    sp_clear_bar(x);
    sp_span(x + 1, sp.base - 2, sp.bw - 2, 6);
    sp_span(x + 1, sp.base - 1, sp.bw - 2, 6);
    /* a soft curtain under the pill, a quarter of the meter at least,
     * and the stem down to the level brightest in its middle */
    int top = pk + 5, len = lv - top, glow = sp.barh / 4 > len ? sp.barh / 4 : len;
    if (top + glow > sp.base - 3) glow = sp.base - 3 - top;
    for (int i = 0; i < glow; i++)
        sp_span(x + 1, top + i, sp.bw - 2, 1 + 6 * (glow - i) * (glow - i) / (glow * glow));
    for (int i = 0; i < len; i++)
        sp_span(sx, top + i, sw, 3 + 7 * (len - i) * (len - i) / (len * len));
    if (len > 3)
    {
        sp_span(x + 1, lv - 1, sp.bw - 2, 6);
        sp_span(x, lv, sp.bw, 11);
        sp_span(x + 1, lv + 1, sp.bw - 2, 6);
    }
    sp_span(x + 2, pk - 2, sp.bw - 4, 3);
    sp_span(x, pk - 1, sp.bw, 7);
    for (int r = pk; r < pk + 4; r++)
    {
        if (r < 0 || r >= sp.ch) continue;
        uint16_t *p = sp.px + r * sp.stride + x;
        bool edge = r == pk || r == pk + 3;
        for (int i = 0; i < sp.bw; i++) p[i] = edge && (i == 0 || i == sp.bw - 1) ? sp.tab[r][10] : sp.pill[r];
    }
    sp_span(x, pk + 4, sp.bw, 7);
}

/* The curve at column x: Catmull-Rom through the band centres. */
static float sp_spline(const float *v, int x)
{
    float t = (x + 0.5f) * sp.bars / sp.cw - 0.5f;
    int i = (int)floorf(t);
    float f = t - i;
    float p0 = v[i - 1 < 0 ? 0 : i - 1 >= sp.bars ? sp.bars - 1 : i - 1];
    float p1 = v[i < 0 ? 0 : i >= sp.bars ? sp.bars - 1 : i];
    float p2 = v[i + 1 >= sp.bars ? sp.bars - 1 : i + 1 < 0 ? 0 : i + 1];
    float p3 = v[i + 2 >= sp.bars ? sp.bars - 1 : i + 2 < 0 ? 0 : i + 2];
    float r = p1 + 0.5f * f * (p2 - p0 + f * (2 * p0 - 5 * p1 + 4 * p2 - p3 + f * (3 * (p1 - p2) + p3 - p0)));
    return r < 0 ? 0 : r > 1 ? 1 : r;
}
/* Curve: per column, the background with its grid, the fill under the
 * line, the peak-hold line, and the line itself, two pixels wide and
 * antialiased. Only a column whose curve moved is redrawn, and only the
 * rows from where its lines were to where they are now. */
static bool sp_draw_curve(bool all)
{
    float y[SP_COLS + 2], py[SP_COLS + 2];
    int bottom = sp.ch - 1;
    for (int x = 0; x < sp.cw; x++)
    {
        y[x + 1] = bottom - sp_spline(sp.lvl, x) * (sp.ch - 3);
        py[x + 1] = bottom - sp_spline(sp.peak, x) * (sp.ch - 3);
    }
    y[0] = y[1]; y[sp.cw + 1] = y[sp.cw];
    bool changed = false;
    for (int x = 0; x < sp.cw; x++)
    {
        float yc = y[x + 1];
        if (!all && fabsf(yc - sp.ydraw[x]) < 0.2f && fabsf(py[x + 1] - sp.pdraw[x]) < 0.2f) continue;
        changed = true;
        sp_dirty(x, 1);
        sp.ydraw[x] = yc;
        sp.pdraw[x] = py[x + 1];
        /* the line covers from half way to each neighbour, plus its width */
        float a = (y[x] + yc) / 2, b = (y[x + 2] + yc) / 2;
        float lo = fminf(yc, fminf(a, b)) - 1.0f, hi = fmaxf(yc, fmaxf(a, b)) + 1.0f;
        float pk = py[x + 1];
        int top = (int)yc, r0 = (int)fminf(lo, pk), r1 = (int)hi + 1;
        int from = all ? 0 : r0 < sp.span[x][0] ? r0 : sp.span[x][0];
        int to = all ? sp.ch - 1 : r1 > sp.span[x][1] ? r1 : sp.span[x][1];
        from = from < 0 ? 0 : from;
        to = to >= sp.ch ? sp.ch - 1 : to;
        sp.span[x][0] = (int16_t)r0;
        sp.span[x][1] = (int16_t)r1;
        uint16_t *p = sp.px + from * sp.stride + x;
        for (int r = from; r <= to; r++, p += sp.stride)
            *p = r <= top ? (sp.vline[x] ? sp_565(SP_GRID) : sp.row_bg[r]) : sp.row_fill[r];
        /* the peak-hold line, faint, where it stands clear of the curve */
        if (pk < lo - 1.5f)
        {
            int r = (int)pk;
            float f = pk - r;
            sp_blend(x, r, 0x9fb3a8, (int)(150 * (1 - f)));
            sp_blend(x, r + 1, 0x9fb3a8, (int)(150 * f));
        }
        uint32_t c = sp_mix(sp_888(sp.row_led[(int)yc]), 0xffffff, 40);
        for (int r = (int)lo; r <= (int)hi; r++)
        {
            float cover = fminf(r + 1, hi) - fmaxf(r, lo);
            sp_blend(x, r, c, (int)(cover * 256));
        }
    }
    return changed;
}

static bool gb_theater_open(void);     /* the Gabagool clip, over Start */
static bool vz_showing(void);
static bool sp_seen(void)
{
    if (spa.active) return sp.canvas && !sl_power_hides_ui() && !po_is_open();
    return mt.page && sp.canvas && npages == 1 && !overlay_visible() && !sl_power_hides_ui() &&
           !gb_theater_open() && !po_is_open() && mt_on_screen(sp.canvas);
}

static void sp_tick(lv_timer_t *t)
{
    (void)t;
    bool seen = sp_seen();
    sp_listen(seen);
    if (!seen) { sp.last_ms = 0; return; }
    uint32_t now = lv_tick_get();      /* the clock this timer runs on */
    float dt = sp.last_ms ? (now - sp.last_ms) / 1000.0f : 0.033f;
    if (dt > 0.1f) dt = 0.1f;
    sp.last_ms = now;
    float db[SP_BARS];
    bool music = sp_analyze(db);
    if (!music && sp.settled) return;
    float loud = -120;
    if (music)
        for (int b = 0; b < sp.bars; b++) if (db[b] > loud) loud = db[b];
    struct mp3entry *id3 = music ? audio_current_track() : NULL;
    if (id3 && strcmp(id3->path, sp.song))
    {
        strlcpy(sp.song, id3->path, sizeof sp.song);
        sp.top = -30;                    /* a new song: its first loud bit sets the scale */
    }
    if (music && loud > -90)
    {
        /* follow the record: up at once, down slowly, a quiet one lifted by
         * at most 30 dB */
        sp.top = loud > sp.top ? loud : sp.top - 1.5f * dt;
        if (sp.top < -30) sp.top = -30;
    }
    /* the curve moves like a plugin's: a touch slower up, smoother down,
     * and each point is smoothed with its neighbours (about a third of an
     * octave in all), so it reads as a shape rather than a comb */
    bool curve = sp.mode == SP_MODE_CURVE;
    if (curve && music)
    {
        float pw[SP_BARS];
        for (int b = 0; b < sp.bars; b++) pw[b] = (float)pow(10, db[b] / 10);   /* double pow: powf needs a newer glibc than the R1 has */
        for (int b = 0; b < sp.bars; b++)
        {
            float l = pw[b > 0 ? b - 1 : b], r = pw[b + 1 < sp.bars ? b + 1 : b];
            float m = 0.25f * l + 0.5f * pw[b] + 0.25f * r;
            db[b] = m > 1e-12f ? 10 * log10f(m) : -120;
        }
    }
    if (!(sp.top > -1000 && sp.top < 1000))
    {
        /* a level that is not a number once drew billions of rows and hung
         * the R1: start the scale over, and say where it came from */
        static bool told;
        if (!told) sl_log("spectrum: scale %s, loudest %s: reset", sp.top != sp.top ? "NaN" : "out of range",
                          loud != loud ? "NaN" : "a number");
        told = true;
        sp.top = -14;
    }
    float base = sp.top + 1.5f - SP_RANGE, attack = curve ? 0.55f : 0.8f;
    float fall = curve ? 0.3f : 0.45f, ease = curve ? 1.8f : 2.6f;
    float hold = curve ? 0.9f : sp.mode == SP_MODE_PEAKS ? 0.3f : 0.55f;
    bool moving = false, changed = false;
    for (int b = 0; b < sp.bars; b++)
    {
        float v = music ? (db[b] - base) / SP_RANGE : 0;
        v = v > 1 ? 1 : v > 0 ? v : 0;      /* NaN reads as silence */
        float *l = &sp.lvl[b], *p = &sp.peak[b];
        if (!(*l >= 0)) *l = 0;
        if (!(*p >= 0)) *p = 0;
        if (v > *l) *l += (v - *l) * attack;
        else
        {
            *l -= dt * (fall + ease * (*l - v));
            if (*l < v) *l = v;
        }
        if (*l >= *p) { *p = *l; sp.peak_v[b] = 0; sp.peak_hold[b] = hold; }
        else if (sp.peak_hold[b] > 0) sp.peak_hold[b] -= dt;
        else
        {
            sp.peak_v[b] += (curve ? 0.9f : 2.2f) * dt;
            *p -= sp.peak_v[b] * dt;
            if (*p < *l) *p = *l;
        }
        if (*l > 0.001f || *p > 0.001f) moving = true;
        if (curve) continue;
        int k0, k1;
        if (sp.mode == SP_MODE_BARS)
        {
            /* the pill floats just over the peak; on the bar, it is not drawn */
            k0 = sp_row_of(*l);
            k1 = sp_row_of(*p) - 5;
            if (k1 > k0 - 6) k1 = -1;
        }
        else
        {
            k0 = sp_row_of(*p) - 2;
            k1 = sp_row_of(*l);
        }
        if (sp.drawn[b][0] != k0 || sp.drawn[b][1] != k1)
        {
            if (sp.mode == SP_MODE_BARS) sp_draw_bar(b, k0, k1);
            else sp_draw_peak(b, k0, k1);
            sp.drawn[b][0] = (int16_t)k0; sp.drawn[b][1] = (int16_t)k1;
            changed = true;
        }
    }
    if (curve) changed = sp_draw_curve(false);
    sp.settled = !music && !moving;
    /* only the columns that moved: the app's frame is rotated on the way
     * to the panel, and a whole one costs four times a few bars */
    if (changed && sp.dx1 > sp.dx0)
    {
        lv_area_t a;
        lv_obj_get_coords(sp.canvas, &a);
        a.x2 = a.x1 + sp.dx1 - 1;
        a.x1 += sp.dx0;
        lv_obj_invalidate_area(sp.canvas, &a);
    }
    sp.dx0 = SP_COLS; sp.dx1 = 0;
}

/* Before the tile is rebuilt or removed, and when Start goes. */
static void sp_forget(void)
{
    sp_listen(false);
    if (sp.timer) lv_timer_delete(sp.timer);
    free(sp.px);
    sp.timer = NULL;
    sp.px = NULL;
    sp.canvas = NULL;
}

/* A frequency mark on the curve's grid: its line, and its name under it. */
static void sp_mark(lv_obj_t *tile, int pad, float hz, const char *name)
{
    int x = (int)(log(hz / SP_LO_HZ) / log(SP_HI_HZ / SP_LO_HZ) * sp.cw);
    if (x < 1 || x >= sp.cw - 1) return;
    sp.vline[x] = 1;
    lv_obj_t *l = lv_label_create(tile);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_font(l, &ss_regular_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8fa398), 0);
    lv_obj_update_layout(l);
    int lw = lv_obj_get_width(l);
    lv_obj_set_pos(l, pad + x - lw / 2 < pad ? pad : pad + x - lw / 2, pad + sp.ch + 1);
}

/* The analyzer in a w by h box: a tile (small, medium or wide) or the app
 * (full). False when there is no memory for its canvas. */
static bool sp_build(lv_obj_t *tile, int w, int h, int mode, bool small, bool wide, bool full)
{
    if (!sp_win[SP_N / 2])
    {
        for (int i = 0; i < SP_N; i++)
            sp_win[i] = (int16_t)(32767 * (0.5f - 0.5f * cosf(6.2831853f * i / SP_N)));
        for (int i = 0; i < SP_N / 2; i++)
        {
            sp_cos[i] = (int16_t)lroundf(32767 * cosf(6.2831853f * i / SP_N));
            sp_sin[i] = (int16_t)lroundf(-32767 * sinf(6.2831853f * i / SP_N));
        }
    }
    sp_forget();
    float keep_top = sp.top ? sp.top : -14;
    char song[MAX_PATH];
    strlcpy(song, sp.song, sizeof song);
    memset(&sp, 0, sizeof sp);
    sp.top = keep_top;                   /* a rebuild keeps the scale */
    strlcpy(sp.song, song, sizeof sp.song);
    sp.mode = mode;
    sp.dx0 = SP_COLS;
    bool curve = sp.mode == SP_MODE_CURVE;
    if (curve)
    {
        /* 24 to 72 points, over a grid with the frequencies marked */
        int pad = small ? 8 : full ? 28 : 12, bottom = small ? 8 : 52;
        sp.cw = w - 2 * pad;
        sp.ch = h - pad - bottom;
        sp.bars = small ? 24 : wide ? 72 : 40;
        sp.px = malloc((size_t)sp.cw * sp.ch * 2);
        if (!sp.px) return false;
        for (int r = 0; r < sp.ch; r++)
        {
            float f = 1 - (float)r / (sp.ch - 1);
            uint32_t c = sp_led(f);
            sp.row_led[r] = sp_565(c);
            sp.row_fill[r] = sp_565d(sp_mix(SP_BG, c, (int)(16 + 76 * f)), r);
            /* the level lines: quarters of the range */
            sp.row_bg[r] = sp_565(r && r % ((sp.ch - 1) / 4) == 0 && r < sp.ch - 4 ? SP_GRID : SP_BG);
        }
        sp.canvas = lv_canvas_create(tile);
        lv_canvas_set_buffer(sp.canvas, sp.px, sp.cw, sp.ch, LV_COLOR_FORMAT_RGB565);
        sp.stride = (int)(lv_canvas_get_draw_buf(sp.canvas)->header.stride / 2);
        lv_obj_set_pos(sp.canvas, pad, pad);
        sp_fill_rect(0, 0, sp.cw, sp.ch, sp_565(SP_BG));
        if (!small)
        {
            static const struct { float hz; const char *name; bool wide_only; } marks[] = {
                { 50, "50", true }, { 100, "100", false }, { 200, "200", true }, { 500, "500", true },
                { 1000, "1k", false }, { 2000, "2k", true }, { 5000, "5k", true }, { 10000, "10k", false },
            };
            for (unsigned i = 0; i < sizeof marks / sizeof marks[0]; i++)
                if (wide || !marks[i].wide_only) sp_mark(tile, pad, marks[i].hz, marks[i].name);
        }
        sp_draw_curve(true);
    }
    else
    {
        /* bars and peaks: the whole tile is the canvas, a deep violet
         * dark that glows a little towards the baseline; 8, 14 or 28 bands */
        int mx = small ? 8 : full ? 28 : 16, top = small ? 8 : full ? 40 : 16;
        sp.cw = w;
        sp.ch = h > SP_ROWS ? SP_ROWS : h;
        sp.bars = small ? 8 : full ? 40 : wide ? 28 : 14;
        sp.gap = small ? 3 : 4;
        sp.bw = (sp.cw - 2 * mx - sp.gap * (sp.bars - 1)) / sp.bars;
        if (sp.bw > 16) sp.bw = 16;
        sp.x0 = (sp.cw - (sp.bw * sp.bars + sp.gap * (sp.bars - 1))) / 2;
        sp.base = sp.ch - (small ? 12 : full ? 70 : 46);
        sp.refl = small ? 6 : full ? 30 : 16;
        sp.barh = sp.base - top;
        sp.rcap = sp.bw / 2 > 8 ? 8 : sp.bw / 2;
        sp.px = malloc((size_t)sp.cw * sp.ch * 2);
        if (!sp.px) return false;
        for (int r = 0; r < sp.ch; r++)
        {
            float d = (float)r / (sp.ch - 1), g = r <= sp.base ? d * d : 0.9f - 0.6f * (r - sp.base) / (float)(sp.ch - sp.base);
            uint32_t bg = sp_mix(0x07050c, 0x1d0c2a, (int)(256 * (g < 0 ? 0 : g)));
            int m = r < sp.base ? r : 2 * sp.base - r;           /* below the baseline: the mirrored row */
            float f = (float)(sp.base - 1 - m) / (sp.barh - 1);
            uint32_t c = sp_zune(f < 0 ? 0 : f > 1 ? 1 : f);
            sp.bgz[r] = sp_565d(bg, r);
            for (int a = 0; a <= 16; a++) sp.tab[r][a] = sp_565d(sp_mix(bg, c, a * 16), r);
            sp.pill[r] = sp_565(sp_mix(c, 0xffffff, 120));
        }
        /* the rounded top: how much of each pixel the circle covers, 4 x 4 samples */
        for (int dy = 0; dy < sp.rcap; dy++)
            for (int i = 0; i < sp.bw; i++)
            {
                int n = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                    {
                        float px = i + (sx + 0.5f) / 4 - sp.bw / 2.0f, py = dy + (sy + 0.5f) / 4 - sp.rcap;
                        float cx = px < -(sp.bw / 2.0f - sp.rcap) ? px + (sp.bw / 2.0f - sp.rcap) :
                                   px > sp.bw / 2.0f - sp.rcap ? px - (sp.bw / 2.0f - sp.rcap) : 0;
                        if (cx * cx + py * py <= sp.rcap * sp.rcap) n++;
                    }
                sp.capm[dy][i] = (uint8_t)n;
            }
        sp.canvas = lv_canvas_create(tile);
        lv_canvas_set_buffer(sp.canvas, sp.px, sp.cw, sp.ch, LV_COLOR_FORMAT_RGB565);
        sp.stride = (int)(lv_canvas_get_draw_buf(sp.canvas)->header.stride / 2);
        for (int r = 0; r < sp.ch; r++) sp_fill_rect(0, r, sp.cw, 1, sp.bgz[r]);
        for (int b = 0; b < sp.bars; b++)
        {
            if (sp.mode == SP_MODE_BARS) sp_draw_bar(b, sp_row_of(0), -1);
            else sp_draw_peak(b, sp_row_of(0) - 2, sp_row_of(0));
            sp.drawn[b][0] = -100;
        }
    }
    lv_obj_add_flag(sp.canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
    sp.timer = lv_timer_create(sp_tick, 33, NULL);
    sp_tick(NULL);
    return true;
}

static void mt_spectrum_fill(struct mt_slot *s)
{
    lv_obj_t *tile = s->obj;
    int w = mt_px(mt_span_w(s->size)), h = mt_px(mt_span_h(s->size));
    bool small = s->size == SZ_SMALL;
    lv_obj_set_style_bg_color(tile, lv_color_hex(SP_BG), 0);
    if (spa.active || vz_showing()) return;   /* the app or the visualizer has the analyzer */
    if (!sp_build(tile, w, h, sp_mode_of(s), small, s->size == SZ_WIDE, false))
    {
        mt_icon(tile, s->id, s->size);
        return;
    }
    if (!small)
    {
        lv_obj_t *l = mt_title(tile, mt_defs[s->id].name);
        lv_obj_set_style_text_color(l, lv_color_hex(sp.mode == SP_MODE_CURVE ? 0xd8e6dc : 0xf4ecf7), 0);
    }
}

/* ------------------------------------------------------------ the app */
#define SPA_W 800
#define SPA_H 480

/* the mode's name and the dots fade in, then out after a while */
static void spa_set_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void spa_fade(int to)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, spa.chrome);
    lv_anim_set_values(&a, lv_obj_get_style_opa(spa.chrome, 0), to);
    lv_anim_set_duration(&a, to ? SL_T_FAST : SL_T_SLOW);
    lv_anim_set_exec_cb(&a, spa_set_opa);
    lv_anim_start(&a);
}
static void spa_hide_cb(lv_timer_t *t) { (void)t; spa.hide = NULL; spa_fade(LV_OPA_TRANSP); }
static void spa_show_chrome(void)
{
    lv_label_set_text(spa.name, sl_tr(sp_mode_names[spa.mode]));
    for (int i = 0; i < SP_MODES; i++)
        lv_obj_set_style_bg_opa(spa.dots[i], i == spa.mode ? LV_OPA_COVER : 70, 0);
    spa_fade(LV_OPA_COVER);
    if (spa.hide) lv_timer_reset(spa.hide);
    else
    {
        spa.hide = lv_timer_create(spa_hide_cb, 2200, NULL);
        lv_timer_set_repeat_count(spa.hide, 1);
    }
}

static void spa_build_stage(void)
{
    if (spa.stage) lv_obj_delete(spa.stage);
    spa.stage = box(spa.scr);
    lv_obj_set_size(spa.stage, SPA_W, SPA_H);
    lv_obj_add_flag(spa.stage, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_move_background(spa.stage);
    lv_obj_set_style_bg_color(spa.scr, lv_color_hex(spa.mode == SP_MODE_CURVE ? SP_BG : 0x07050c), 0);
    if (!sp_build(spa.stage, SPA_W, SPA_H, spa.mode, false, true, true))
        toast_warn("Not enough memory for the analyzer");
}

static void spa_close(void);
static void spa_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    if (code == LV_EVENT_PRESSED) { spa.down = pt; spa.moved = false; }
    else if (code == LV_EVENT_PRESSING)
    {
        if (LV_ABS(pt.x - spa.down.x) > 24 || LV_ABS(pt.y - spa.down.y) > 24) spa.moved = true;
    }
    else if (code == LV_EVENT_RELEASED)
    {
        int dx = pt.x - spa.down.x, dy = pt.y - spa.down.y;
        if (LV_ABS(dx) > 70 && LV_ABS(dx) > LV_ABS(dy))
        {
            spa.mode = (spa.mode + (dx < 0 ? 1 : SP_MODES - 1)) % SP_MODES;   /* left: the next */
            sfx_play(SFX_UI_TICK);
            spa_build_stage();
            spa_show_chrome();
        }
        else if (!spa.moved) spa_show_chrome();   /* a tap: what this is, and the way out */
    }
}
static void spa_done_cb(lv_event_t *e) { (void)e; spa_close(); }

static void spa_open(void)
{
    if (spa.active) return;
    if (sp.canvas) lv_obj_delete(sp.canvas);   /* the tile's: the app takes the analyzer */
    sp_forget();
    spa.active = true;
    spa.home = lv_screen_active();
    statusbar_show(false);
    sl_set_rotation(LV_DISPLAY_ROTATION_90);
    spa.scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(spa.scr);
    lv_obj_set_style_bg_opa(spa.scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(spa.scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(spa.scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(spa.scr, spa_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(spa.scr, spa_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(spa.scr, spa_touch_cb, LV_EVENT_RELEASED, NULL);

    /* over the analyzer, shown on a tap or a swipe: a way out, the
     * mode's name, a dot for each mode */
    spa.chrome = box(spa.scr);
    lv_obj_set_size(spa.chrome, SPA_W, SPA_H);
    lv_obj_add_flag(spa.chrome, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *done = box(spa.chrome);
    lv_obj_set_size(done, 56, 56);
    lv_obj_set_pos(done, 16, 16);
    lv_obj_set_style_radius(done, SL_R_FULL, 0);
    lv_obj_set_style_bg_color(done, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(done, 40, 0);
    lv_obj_set_style_bg_opa(done, 90, LV_STATE_PRESSED);
    lv_obj_t *x = text(done, LV_SYMBOL_CLOSE, &sl_sym_24, 0xffffff);
    lv_obj_set_style_text_color(x, lv_color_white(), 0);   /* on the analyzer: white in every theme */
    lv_obj_center(x);
    lv_obj_set_ext_click_area(done, 14);
    on_click(done, spa_done_cb, NULL);
    lv_obj_remove_flag(done, LV_OBJ_FLAG_EVENT_BUBBLE);
    spa.name = text(spa.chrome, "", &ss_semibold_26, 0xffffff);
    lv_obj_set_style_text_color(spa.name, lv_color_white(), 0);
    lv_obj_align(spa.name, LV_ALIGN_TOP_MID, 0, 26);
    for (int i = 0; i < SP_MODES; i++)
    {
        lv_obj_t *d = spa.dots[i] = box(spa.chrome);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, SL_R_FULL, 0);
        lv_obj_set_style_bg_color(d, lv_color_white(), 0);
        lv_obj_set_pos(d, SPA_W / 2 - (SP_MODES * 16) / 2 + i * 16 + 4, 64);
    }
    lv_obj_set_style_opa(spa.chrome, LV_OPA_TRANSP, 0);

    spa_build_stage();
    lv_screen_load(spa.scr);
    spa_show_chrome();
    sl_log("spectrum app: mode %d", spa.mode);
}

static void mt_spectrum_refill(void)
{
    if (!mt.page) return;
    for (int i = 0; i < mt.n; i++)
        if (mt.slot[i].id == TL_SPECTRUM && mt.slot[i].obj) mt_fill(&mt.slot[i]);
}
static void spa_close(void)
{
    if (!spa.active) return;
    sp_forget();
    spa.active = false;
    lv_anim_delete(spa.chrome, NULL);
    if (spa.hide) lv_timer_delete(spa.hide);
    spa.hide = NULL;
    sl_set_rotation(LV_DISPLAY_ROTATION_0);
    lv_screen_load(spa.home);
    statusbar_show(true);
    lv_obj_delete(spa.scr);
    spa.scr = spa.stage = spa.chrome = spa.name = NULL;
    lv_obj_invalidate(lv_screen_active());
    mt_spectrum_refill();                /* the tile, if Start has one */
}
