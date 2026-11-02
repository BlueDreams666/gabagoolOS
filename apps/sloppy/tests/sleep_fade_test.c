/* The sleep timer's fade on the Mac, at the PCM level: the real
 * firmware/pcm_mixer.c (its gliding amplitude and its C mixing loop) plays a
 * steady full-scale-ish signal on the playback channel while a 100 Hz tick
 * steps apps/sleep_fade.h's curve into the channel's amplitude, exactly as
 * pcmbuf_sleep_tick does (pcmbuf_update_volume multiplies it in at full
 * precision). Every output sample is kept, and the test checks:
 *   - the level only ever goes down, and ends at exactly 0;
 *   - no sample-to-sample step is larger than 1/250 of the signal
 *     (-48 dB; a click needs a step near the signal itself). The mixer
 *     takes a new amplitude once a mix frame (1024 frames, 23 ms, so the
 *     two or three ticks between frames land as one step);
 *   - every step is several times smaller than the pause fade's;
 *   - the curve: about -18 dB halfway, -60 dB 3 s before the end.
 * For comparison it measures what the timer did before: a radio stream cut
 * by mixer_channel_stop, and music paused by the 1/3 s fade_on_stop fade.
 * Run by run-sleep-fade-test.sh [envelope.csv]. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "pcm_mixer.c"
#include "sleep_fade.h"

/* ---- what the mixer calls */
static pcm_play_callback_type t_more;
static pcm_status_callback_type t_status;
void pcm_play_data(pcm_play_callback_type get_more, pcm_status_callback_type status_cb,
                   const void *start, size_t size)
{ t_more = get_more; t_status = status_cb; (void)start; (void)size; }
void pcm_play_lock(void) {}
void pcm_play_unlock(void) {}
void pcm_play_stop(void) { t_more = NULL; }
bool pcm_is_playing(void) { return t_more != NULL; }
void pcm_set_frequency(unsigned int f) { (void)f; }
unsigned int pcm_get_frequency(void) { return 44100; }
void pcm_do_peak_calculation(struct pcm_peaks *p, bool active, const void *addr, int count)
{ (void)p; (void)active; (void)addr; (void)count; }
void ** find_array_ptr(void **arr, void *ptr)
{
    void *curr;
    for (curr = *arr; curr != NULL && curr != ptr; curr = *(++arr));
    return arr;
}
int remove_array_ptr(void **arr, void *ptr)
{
    void *curr;
    arr = find_array_ptr(arr, ptr);
    if (*arr == NULL) return -1;
    do { void **arr1 = arr + 1; *arr++ = curr = *arr1; } while (curr != NULL);
    return 0;
}

/* ---- the music: a steady level, so the output is the envelope itself */
#define LEVEL 30000
#define RATE 44100
#define TICK_FRAMES (RATE / HZ)
static int16_t src[2048 * 2];
static void src_more(const void **start, size_t *size) { *start = src; *size = sizeof src; }

static int16_t *out;
static long n_out, cap;
static void pull(long frames)            /* the DMA taking frames from the mixer */
{
    static long have;                     /* frames of the current mixer frame not yet taken */
    static const int16_t *at;
    while (frames > 0)
    {
        if (!have)
        {
            const void *a; size_t sz;
            t_status(PCM_DMAST_STARTED);  /* mixes the next frame */
            mixer_pcm_callback(&a, &sz);
            at = a; have = (long)(sz / 4);
        }
        long k = have < frames ? have : frames;
        if (n_out + k > cap) { cap = (n_out + k) * 2; out = realloc(out, (size_t)cap * 4); }
        memcpy(out + n_out * 2, at, (size_t)k * 4);
        n_out += k; at += k * 2; have -= k; frames -= k;
    }
}

struct result { long max_step; int rises; long first_zero; };
static struct result measure(long from, long to)
{
    struct result r = { 0, 0, -1 };
    for (long i = from + 1; i < to; i++)
    {
        long a = out[(i - 1) * 2], b = out[i * 2];
        long d = labs(b - a);
        if (d > r.max_step) r.max_step = d;
        if (b > a) r.rises++;
        if (b == 0 && r.first_zero < 0) r.first_zero = i;
    }
    return r;
}
static double db(long v) { return v > 0 ? 20 * log10((double)v / LEVEL) : -INFINITY; }

static void start(void)
{
    n_out = 0;
    mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, MIX_AMP_UNITY);
    mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, src_more, NULL, 0);
    pull(RATE / 2);                       /* half a second at full level */
}

int main(int argc, char **argv)
{
    for (int i = 0; i < 2048 * 2; i++) src[i] = LEVEL;
    int fails = 0;

    /* ---- now: 30 s down the curve, one step a tick */
    start();
    long fade_from = n_out, ticks = 30 * HZ;
    for (long t = 1; t <= ticks; t++)
    {
        unsigned int g = sleep_fade_level(t, ticks);
        /* pcmbuf_update_volume: fade_vol (unity, playing) times the sleep gain */
        mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, (unsigned int)(((uint64_t)MIX_AMP_UNITY * g) >> 16));
        pull(TICK_FRAMES);
    }
    pull(RATE / 2);                       /* the pause that follows: nothing left to cut */
    long fade_to = n_out;
    struct result now = measure(fade_from, fade_to);
    long half = out[(fade_from + (fade_to - fade_from - RATE / 2) / 2) * 2];
    long before3 = out[(fade_from + 27L * RATE) * 2];
    printf("now:    30 s cubic fade; largest step %ld of %d (%.1f dB under the music), %d rises, "
           "silent %.2f s after it began; halfway %.1f dB, 3 s before the end %.1f dB\n",
           now.max_step, LEVEL, db(now.max_step), now.rises,
           now.first_zero < 0 ? -1.0 : (double)(now.first_zero - fade_from) / RATE, db(half), db(before3));
    if (argc > 1)
    {
        FILE *f = fopen(argv[1], "w");
        if (f)
        {
            fprintf(f, "seconds,level,dbfs\n");
            for (long i = fade_from - RATE / 4; i < fade_to; i += RATE / 100)
                fprintf(f, "%.3f,%d,%.2f\n", (double)(i - fade_from) / RATE, out[i * 2],
                        out[i * 2] > 0 ? 20 * log10(out[i * 2] / 32768.0) : -120.0);
            fclose(f);
        }
    }
    if (now.rises) { printf("FAIL: the level went up %d times\n", now.rises); fails++; }
    if (now.max_step * 250 > LEVEL) { printf("FAIL: a step of %ld\n", now.max_step); fails++; }
    if (now.first_zero < 0 || now.first_zero > fade_from + ticks * TICK_FRAMES + 2048)
        { printf("FAIL: not silent at the end\n"); fails++; }
    for (long i = now.first_zero; i >= 0 && i < fade_to; i++)
        if (out[i * 2] || out[i * 2 + 1]) { printf("FAIL: sound after the silence at %ld\n", i); fails++; break; }
    if (fabs(db(half) + 18) > 1.5 || db(before3) > -55)
        { printf("FAIL: the curve\n"); fails++; }

    /* ---- before: the radio stopped at the end, mid-music */
    start();
    long cut_from = n_out;
    mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
    pull(RATE / 10);
    struct result cut = measure(cut_from - 1, n_out);
    printf("before: a stream stopped at the end; one step of %ld (%.1f dB: the whole signal)\n",
           cut.max_step, db(cut.max_step));

    /* ---- before: music paused by fade_on_stop's 1/3 s linear fade (pcmbuf_fade_tick) */
    mixer_reset();
    start();
    long pause_from = n_out;
    unsigned int vol = MIX_AMP_UNITY, step = MIX_AMP_UNITY / (HZ / 3);
    while (vol)
    {
        vol -= vol < step ? vol : step;
        mixer_channel_set_amplitude(PCM_MIXER_CHAN_PLAYBACK, vol);
        pull(TICK_FRAMES);
    }
    pull(RATE / 10);
    struct result pause = measure(pause_from, n_out);
    printf("before: music paused at the end; from full to silence in %.2f s, largest step %ld (%.1f dB)\n",
           (double)(pause.first_zero - pause_from) / RATE, pause.max_step, db(pause.max_step));
    if (now.max_step * 4 > pause.max_step) { printf("FAIL: not smoother than before\n"); fails++; }

    printf(fails ? "sleep fade: %d failed\n" : "sleep fade: all passed\n", fails);
    free(out);
    return fails != 0;
}
