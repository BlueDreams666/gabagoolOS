/***************************************************************************
 * HiBy-specific helpers for the hosted ALSA PCM backend
 ****************************************************************************/

#include "autoconf.h"

#include "pcm-alsa-hiby.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char hiby_pcm_bt_mac[18];

void hiby_pcm_set_bt_mac(const char *mac)
{
    size_t i;

    if (!mac || !mac[0])
    {
        hiby_pcm_bt_mac[0] = '\0';
        return;
    }

    for (i = 0; mac[i] != '\0' && i + 1 < sizeof(hiby_pcm_bt_mac); i++)
    {
        char c = mac[i];
        if (c == ':')
            c = '_';
        hiby_pcm_bt_mac[i] = (char)toupper((unsigned char)c);
    }
    hiby_pcm_bt_mac[i] = '\0';
}

const char *hiby_pcm_get_bt_mac(void)
{
    return hiby_pcm_bt_mac[0] ? hiby_pcm_bt_mac : NULL;
}

/* Set by the ALSA poll thread when the Bluetooth PCM it writes to has gone
 * (headphones off, out of range, BlueALSA closed the transport); cleared
 * when the output is switched. Read by the button tick, which then asks
 * for the route back to the player even when no device event came. */
static volatile bool hiby_pcm_bt_lost;

void hiby_pcm_set_bt_output_lost(bool lost)
{
    hiby_pcm_bt_lost = lost;
}

bool hiby_pcm_bt_output_lost(void)
{
    return hiby_pcm_bt_lost && hiby_pcm_bt_mac[0];
}

bool hiby_pcm_is_bluealsa_device(const char *device)
{
    return device && strstr(device, "bluealsa:") != NULL;
}

/* Bluetooth's ring: four periods of 2048 frames (186 ms at 44.1 kHz).
 * It was eight (371 ms), started only when nearly full, and BlueALSA's
 * own FIFO and the speaker's buffer sit behind it: a second test R1 measured a
 * clip heard 1.7 s after it was asked for (2026-10-23). The ring only has
 * to ride out the poll thread's scheduling, which is a thread of its own;
 * UI stalls are covered by pcmbuf, seconds deep, ahead of the mixer. */
void hiby_pcm_adjust_bt_buffering(snd_pcm_sframes_t *period_size,
                                  snd_pcm_sframes_t *buffer_size,
                                  bool bluealsa_active)
{
    if (!bluealsa_active || !period_size || !buffer_size)
        return;

    *period_size *= 2;
    if (*period_size < 2048)
        *period_size = 2048;
    *buffer_size = *period_size * 4;
}

unsigned int hiby_pcm_calc_poll_interval(unsigned int sample_rate,
                                         snd_pcm_sframes_t period_size,
                                         bool bluealsa_active)
{
    unsigned int interval_us;

    if (sample_rate == 0 || period_size <= 0)
        return 10000;

    interval_us = (unsigned int)((period_size * 1000000ULL) / sample_rate);
    if (bluealsa_active)
        interval_us /= 2;
    if (interval_us < 2000)
        interval_us = 2000;
    if (interval_us > 50000)
        interval_us = 50000;
    return interval_us;
}

snd_pcm_uframes_t hiby_pcm_start_threshold(snd_pcm_sframes_t buffer_size,
                                           snd_pcm_sframes_t period_size,
                                           bool bluealsa_active)
{
    /* Bluetooth starts at half the ring too: three quarters of it (the
     * old buffer less a period) was audio waiting before the first sound */
    (void)period_size; (void)bluealsa_active;
    return (snd_pcm_uframes_t)(buffer_size / 2);
}

/* ------------------------------------------------ pause, seek, delay */
/* What is queued for a Bluetooth output (our ring, BlueALSA's FIFO) took
 * seconds to play out after a pause or a skip. A pause now drops it; so
 * that resuming loses nothing, the frames written last are kept here and
 * the ones the drop threw away (the output's delay at that moment) are
 * written again first on resume. A skip or a seek drops them for good.
 * Frames are counted from the start, so ring positions are counts modulo
 * the ring; a replay is at most half the ring, so the frames written
 * while it is replayed never land on it. */
#define HIBY_HIST_FRAMES 65536u
static unsigned char *hiby_hist;
static int hiby_hist_fb;                 /* bytes a frame */
static unsigned long hiby_hist_w;        /* frames noted so far */
static unsigned long hiby_rep_pos, hiby_rep_end;
static bool hiby_rep_hold;               /* armed by a pause, let go by the resume */

void hiby_out_hist_note(const void *frames, long n, int frame_bytes)
{
    if (n <= 0 || frame_bytes <= 0 || hiby_rep_hold)
        return;                          /* paused: what goes out is silence */
    if (!hiby_hist || hiby_hist_fb != frame_bytes)
    {
        free(hiby_hist);
        hiby_hist = malloc((size_t)HIBY_HIST_FRAMES * frame_bytes);
        hiby_hist_fb = frame_bytes;
        hiby_hist_w = hiby_rep_pos = hiby_rep_end = 0;
        if (!hiby_hist)
            return;
    }
    const unsigned char *src = frames;
    while (n > 0)
    {
        unsigned long at = hiby_hist_w % HIBY_HIST_FRAMES;
        long run = (long)(HIBY_HIST_FRAMES - at);
        if (run > n)
            run = n;
        memcpy(hiby_hist + at * frame_bytes, src, (size_t)run * frame_bytes);
        src += (size_t)run * frame_bytes;
        hiby_hist_w += run;
        n -= run;
    }
}

void hiby_out_hist_clear(void)
{
    hiby_hist_w = hiby_rep_pos = hiby_rep_end = 0;
    hiby_rep_hold = false;
}

long hiby_out_replay_arm(long delay_frames)
{
    if (!hiby_hist || delay_frames <= 0)
        return hiby_out_replay_left();
    unsigned long have = hiby_hist_w < HIBY_HIST_FRAMES / 2 ? hiby_hist_w : HIBY_HIST_FRAMES / 2;
    unsigned long n = (unsigned long)delay_frames < have ? (unsigned long)delay_frames : have;
    hiby_rep_end = hiby_hist_w;
    hiby_rep_pos = hiby_hist_w - n;
    hiby_rep_hold = true;
    return (long)n;
}

/* A pause while an earlier one's frames are still being played again:
 * the ones not played yet go to the end of the history, after those just
 * written, so one replay covers both. Returns how many moved. */
long hiby_out_replay_requeue(void)
{
    long left = hiby_out_replay_left();
    unsigned char tmp[1024 * 8];
    long per = hiby_hist_fb > 0 ? (long)(sizeof tmp / hiby_hist_fb) : 0;
    long moved = 0;
    while (per > 0 && hiby_out_replay_left() > 0)
    {
        long n = hiby_out_replay_take(tmp, per, hiby_hist_fb);
        if (n <= 0)
            break;
        hiby_out_hist_note(tmp, n, hiby_hist_fb);
        moved += n;
    }
    (void)left;
    return moved;
}

void hiby_out_replay_release(void)
{
    hiby_rep_hold = false;
}

bool hiby_out_replay_held(void)
{
    return hiby_rep_hold;
}

bool hiby_out_replay_ready(void)
{
    return !hiby_rep_hold && hiby_out_replay_left() > 0;
}

long hiby_out_replay_left(void)
{
    return (long)(hiby_rep_end - hiby_rep_pos);
}

void hiby_out_replay_clear(void)
{
    hiby_rep_pos = hiby_rep_end = 0;
    hiby_rep_hold = false;
}

long hiby_out_replay_take(void *dst, long max, int frame_bytes)
{
    long n = hiby_out_replay_left();
    if (n > max)
        n = max;
    if (n <= 0 || !hiby_hist || frame_bytes != hiby_hist_fb ||
        hiby_hist_w - hiby_rep_pos > HIBY_HIST_FRAMES)
    {
        hiby_out_replay_clear();
        return 0;
    }
    unsigned char *out = dst;
    long done = 0;
    while (done < n)
    {
        unsigned long at = hiby_rep_pos % HIBY_HIST_FRAMES;
        long run = (long)(HIBY_HIST_FRAMES - at);
        if (run > n - done)
            run = n - done;
        memcpy(out + (size_t)done * frame_bytes, hiby_hist + at * frame_bytes, (size_t)run * frame_bytes);
        hiby_rep_pos += run;
        done += run;
    }
    return done;
}

/* How far behind the mixer the ears are: frames queued past it (the ring,
 * BlueALSA and what the headphones report), set by the poll thread, read
 * by the UI for the visualizers, the lyrics and the progress bar. */
static volatile unsigned hiby_out_ms;

void hiby_out_set_delay(long frames, unsigned rate)
{
    hiby_out_ms = frames > 0 && rate ? (unsigned)((unsigned long long)frames * 1000 / rate) : 0;
}

unsigned hiby_out_delay_ms(void)
{
    return hiby_out_ms;
}

/* ------------------------------------------------ gapless over Bluetooth */
/* A track at another rate made Rockbox reopen the output at that rate
 * (audio_auto_change_frequency: a buffer remake, heard as a gap), and
 * BlueALSA's plug then resampled it back to the link's rate. While a
 * Bluetooth output is in use the mixer stays at the link's rate instead:
 * Rockbox's resampler converts each track, and nothing is reopened
 * between tracks. 0: not known, follow the track. */
static volatile unsigned hiby_bt_link_rate;

void hiby_pcm_set_bt_rate(unsigned rate)
{
    hiby_bt_link_rate = rate >= 8000 && rate <= 192000 ? rate : 0;
}

unsigned hiby_pcm_play_rate(unsigned track_rate)
{
    if (hiby_pcm_bt_mac[0] && hiby_bt_link_rate)
        return hiby_bt_link_rate;
    return track_rate;
}
