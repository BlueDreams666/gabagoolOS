/* Turntable scratching for vinyl mode: the record is the song itself. GPLv2+.
 *
 * The filter sits between pcmbuf and the mixer (pcmbuf_set_filter) and keeps
 * the audio it passes on in a ring, the stretch of groove around the needle.
 * Normally it passes the song through unchanged, sample for sample. While a
 * hand is on the record, the needle follows the platter instead: the ring is
 * read at the platter's speed and direction, so a slow push sounds slow and
 * low, a fast one fast and high, a pull backwards plays the song backwards,
 * and holding the record still is silence. Going forward pulls more of the
 * song out of pcmbuf, so the song really moves on. On release the motor
 * brings the platter back to speed; the needle then plays on through the
 * ring until it meets the live edge and hands back to plain playback, so
 * there is no seek, gap or click.
 *
 * Runs in the mixer callback: no allocation, no locks, no I/O there. The UI
 * talks to it through a few words that are each written by one side only.
 * tests/scratch_test.c runs it against a synthetic song. */

#define SCR_RING_BITS 18
#define SCR_RING      (1u << SCR_RING_BITS)   /* frames: 5.9 s at 44.1 kHz */
#define SCR_MASK      (SCR_RING - 1)
#define SCR_LIVE_CHUNK 512                    /* frames per call when playing through */
#define SCR_HAND_CHUNK 128                    /* ... and under the hand, for a quick feel */
#define SCR_MAX_RATE  8.0f
#define SCR_EDGE      3                       /* cubic interpolation needs a frame each side */

static int16_t *scr_ring;                     /* stereo frames */
static unsigned scr_fs = 44100;               /* set by the UI from the mixer */
/* audio side */
static uint32_t scr_head;                     /* frames written, wrapping */
static uint32_t scr_fill;                     /* frames of groove behind the head */
static double scr_lag;                        /* needle: frames behind the head */
static float scr_rate = 1;                    /* platter speed, 1 = 33 1/3 rpm */
static float scr_gain = 1;                    /* eases, so a sudden stop never clicks */
static bool scr_live = true;                  /* plain playback at the live edge */
static uint32_t scr_seen;                     /* the hand's last move, taken up */
static float scr_plan;                        /* the hand's speed since its last move */
static int16_t scr_out[2][SCR_LIVE_CHUNK * 2];
static int scr_out_i;
/* UI side */
static volatile bool scr_held;                /* a hand on the record */
static volatile bool scr_motor = true;        /* the platter is driven (playing) */
static volatile uint32_t scr_target;          /* where the hand has put the needle */
static volatile uint32_t scr_span = 1470;     /* frames between the hand's moves */
static volatile uint32_t scr_moves;           /* counts them */
static volatile bool scr_leaving;             /* play out the ring, then pass through */
/* published by the audio side */
static volatile uint32_t scr_pos_pub;         /* the needle, in the head's frames */
static volatile uint32_t scr_lag_pub;         /* how far the needle trails the song */
static volatile bool scr_still;               /* stopped: no sound, not moving */
static volatile uint32_t scr_resets;          /* stops and seeks seen */
static volatile bool scr_bypass;              /* leaving, and nothing held back: unhook me */

static void scr_reset(void)
{
    scr_head = scr_fill = 0;
    scr_lag = 0;
    scr_rate = scr_motor ? 1 : 0;
    scr_gain = 1;
    scr_live = true;
    scr_pos_pub = scr_target = 0;
    scr_lag_pub = 0;
    scr_resets++;
}

/* Take the next chunk from pcmbuf into the ring. False when there is none. */
static bool scr_pull(pcm_play_callback_type pull)
{
    const void *src = NULL;
    size_t size = 0;
    pull(&src, &size);
    if (!src || size < 4) return false;
    const int16_t *in = src;
    uint32_t frames = size / 4;
    for (uint32_t i = 0; i < frames; )
    {
        uint32_t at = scr_head & SCR_MASK, run = SCR_RING - at;
        if (run > frames - i) run = frames - i;
        memcpy(scr_ring + at * 2, in + i * 2, run * 4);
        scr_head += run;
        i += run;
    }
    scr_fill = scr_fill + frames > SCR_RING ? SCR_RING : scr_fill + frames;
    scr_lag += frames;
    return true;
}

static inline float scr_cubic(float y0, float y1, float y2, float y3, float t)
{
    /* Catmull-Rom */
    return y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
}

/* The groove at lag frames behind the head, channel c. */
static float scr_at(double lag, int c)
{
    if (lag < SCR_EDGE) lag = SCR_EDGE;
    if (lag > (double)scr_fill - SCR_EDGE) lag = (double)scr_fill - SCR_EDGE;
    double pos = -lag;
    int32_t whole = (int32_t)floor(pos);
    float t = (float)(pos - whole);
    uint32_t i0 = (scr_head + whole - 1) & SCR_MASK;
    return scr_cubic(scr_ring[i0 * 2 + c], scr_ring[((i0 + 1) & SCR_MASK) * 2 + c],
                     scr_ring[((i0 + 2) & SCR_MASK) * 2 + c], scr_ring[((i0 + 3) & SCR_MASK) * 2 + c], t);
}

static void scr_filter(const void **start, size_t *size, pcm_play_callback_type pull)
{
    if (!start) { scr_reset(); return; }          /* a stop or seek: new groove */
    if (scr_leaving && scr_live && scr_lag < 1)
    {
        scr_bypass = true;                        /* nothing of the song held back */
        pull(start, size);
        return;
    }
    int16_t *out = scr_out[scr_out_i ^= 1];
    int n = 0;
    bool held = scr_held;

    if (scr_live && !held)
    {
        /* Playing through: whole frames from the ring, as they came. */
        if (scr_lag < SCR_LIVE_CHUNK) scr_pull(pull);
        uint32_t lag = (uint32_t)scr_lag;
        n = lag < SCR_LIVE_CHUNK ? (int)lag : SCR_LIVE_CHUNK;
        for (int i = 0; i < n; i++)
        {
            uint32_t at = (scr_head - lag + i) & SCR_MASK;
            out[i * 2] = scr_ring[at * 2];
            out[i * 2 + 1] = scr_ring[at * 2 + 1];
        }
        scr_lag -= n;
        scr_rate = 1;
    }
    else
    {
        scr_live = false;
        float fs = (float)scr_fs;
        /* The finger is only read every frame or two, so the needle moves
         * at the hand's speed, arriving where the hand put the record just
         * as the next move comes: a steady hand is a steady pitch, not a
         * lurch and a stop each frame. The motor brings it up to speed in
         * about 0.3 s and lets it coast down a little slower. */
        float want = 0, follow;
        if (held)
        {
            if (scr_moves != scr_seen)
            {
                scr_seen = scr_moves;
                /* half a move behind the finger, so a hand read a little
                 * late never leaves the needle standing waiting for it */
                scr_plan = (float)(int32_t)(scr_target - (scr_head - (uint32_t)scr_lag)) / (scr_span * 1.5f);
            }
            follow = 1 / (fs * 0.002f);
        }
        else
        {
            want = scr_motor ? 1 : 0;
            follow = 1 / (fs * (want > scr_rate ? 0.08f : 0.14f));
        }
        int chunk = held ? SCR_HAND_CHUNK : SCR_LIVE_CHUNK / 2;
        bool dry = false;
        /* the motor also has a steady pull, so it gets there, not only near */
        float torque = held ? 0 : 1 / (fs * 0.3f), fade = 1 / (fs * 0.004f);
        for (; n < chunk; n++)
        {
            if (held)
            {
                /* on at the hand's speed until the needle is under the
                 * finger, then wait there for the hand's next move */
                float d = (float)(int32_t)(scr_target - (scr_head - (uint32_t)scr_lag));
                want = d * scr_plan <= 0 || fabsf(d) < fabsf(scr_plan) * fs * 0.002f
                       ? d / (fs * 0.002f) : scr_plan;
                if (want > SCR_MAX_RATE) want = SCR_MAX_RATE;
                if (want < -SCR_MAX_RATE) want = -SCR_MAX_RATE;
            }
            float step = (want - scr_rate) * follow, gap = want - scr_rate;
            if (step < torque && step > -torque)
                step = gap > torque ? torque : gap < -torque ? -torque : gap;
            scr_rate += step;
            /* forward needs groove ahead of the needle: pull it in */
            while (scr_lag < SCR_EDGE + SCR_MAX_RATE + 1 && scr_rate > 0 && !dry)
                dry = !scr_pull(pull);
            if (scr_fill < 2 * SCR_EDGE + 2) { out[n * 2] = out[n * 2 + 1] = 0; scr_rate = 0; continue; }
            if (scr_lag < SCR_EDGE) scr_lag = SCR_EDGE;
            if (scr_lag > (double)scr_fill - SCR_EDGE) scr_lag = (double)scr_fill - SCR_EDGE;
            /* a needle standing still makes no sound: fade out below a
             * tenth of the speed rather than hold a DC level */
            float speed = scr_rate < 0 ? -scr_rate : scr_rate;
            scr_gain += ((speed >= 0.1f ? 1 : speed * 10) - scr_gain) * fade;
            /* faster than the song, the stylus hears all the groove it
             * crosses, not every few frames of it: average that stretch, so
             * a quick push sounds bright, not gritty */
            int taps = speed > 1 ? (int)ceilf(speed) : 1;
            for (int c = 0; c < 2; c++)
            {
                float v = 0;
                for (int k = 0; k < taps; k++) v += scr_at(scr_lag - scr_rate * k / taps, c);
                v *= scr_gain / taps;
                out[n * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            /* move on; the ends of the groove stop the record */
            double lag = scr_lag - scr_rate;
            if (lag < SCR_EDGE) { lag = SCR_EDGE; scr_rate = 0; }
            if (lag > (double)scr_fill - SCR_EDGE) { lag = (double)scr_fill - SCR_EDGE; scr_rate = 0; }
            scr_lag = lag;
        }
        /* Back to speed with nobody touching: settle on whole frames and
         * play through from the ring again. So does a song that ran out
         * under the hand, so the channel can finish. */
        if (!held && scr_motor && (scr_rate == 1 || (dry && scr_lag <= SCR_EDGE)))
        {
            scr_rate = scr_gain = 1;
            scr_lag = floor(scr_lag + 0.5);
            scr_live = true;
        }
    }
    scr_still = !held && !scr_motor && scr_rate == 0;
    scr_pos_pub = scr_head - (uint32_t)scr_lag;
    scr_lag_pub = (uint32_t)scr_lag;
    if (n == 0) { *start = NULL; *size = 0; return; }   /* the song ended */
    *start = out;
    *size = (size_t)n * 4;
}

/* ---- UI side */
static void scr_grab(void)
{
    scr_target = scr_pos_pub;
    scr_held = true;
}
/* The hand turned the record by frames over the last ms. */
static void scr_turn(int32_t frames, uint32_t ms)
{
    uint32_t span = (uint32_t)((uint64_t)ms * scr_fs / 1000);
    scr_span = span < scr_fs / 100 ? scr_fs / 100 : span > scr_fs / 16 ? scr_fs / 16 : span;
    scr_target += (uint32_t)frames;
    scr_moves++;
}
static void scr_release(void) { scr_held = false; }
/* How far behind the song the needle is, for the clock on screen. */
static uint32_t scr_lag_ms(void) { return scr_fs ? (uint32_t)((uint64_t)scr_lag_pub * 1000 / scr_fs) : 0; }
/* How far the hand went back past the start of the groove, in frames. */
static uint32_t scr_past_groove(void)
{
    int32_t behind = (int32_t)(scr_head - scr_fill - scr_target);
    return behind > 0 ? (uint32_t)behind : 0;
}
