/***************************************************************************
 * HiBy-specific hooks for the shared hosted ALSA backend.
 *
 * This header is included directly by pcm-alsa.c so the poll-thread
 * scheduler can reuse the shared pump logic without forking the backend.
 ****************************************************************************/

#ifndef PCM_ALSA_HIBY_HOOKS_H
#define PCM_ALSA_HIBY_HOOKS_H

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pcm-alsa-hiby.h"

static void open_hwdev(const char *device, snd_pcm_stream_t mode);
static void close_hwdev(void);
static void pcm_dma_apply_settings_nolock(void);
static void pcm_pump_locked(snd_pcm_t *handle);
static void hiby_pcm_poll_kick(void);
static bool hiby_pcm_keep_hwdev(const char *device, snd_pcm_stream_t mode);
static bool hiby_pcm_streaming(snd_pcm_state_t state);

static pthread_t hiby_pcm_poll_thread;
static volatile bool hiby_pcm_poll_thread_stop = false;
static volatile bool hiby_pcm_poll_thread_running = false;
static pthread_once_t hiby_pcm_mutex_once = PTHREAD_ONCE_INIT;
static unsigned int hiby_pcm_poll_interval_us = 10000;
/* With no stream running there is nothing to pump. The thread then waits
 * here for a start instead of waking every period, which kept the CPU
 * leaving idle 40-100 times a second whenever the player was paused. */
static pthread_mutex_t hiby_pcm_idle_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hiby_pcm_idle_cond = PTHREAD_COND_INITIALIZER;
static bool hiby_pcm_kicked;
#define HIBY_PCM_IDLE_WAIT_MS 500

static void hiby_pcm_mutex_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&pcm_mtx, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void hiby_pcm_mutex_init_once(void)
{
    pthread_once(&hiby_pcm_mutex_once, hiby_pcm_mutex_init);
}

static bool hiby_pcm_bt_active(void)
{
    const char *device = current_alsa_device ? current_alsa_device : playback_dev;
    return hiby_pcm_is_bluealsa_device(device);
}

static bool hiby_pcm_params_ready(void)
{
    return period_size > 0 && buffer_size > 0 && frames != NULL;
}

void hiby_debug_log(const char *format, ...);

/* A Bluetooth PCM whose device has gone answers -ENODEV (or -EIO) and
 * takes no more samples, so playback would sit "playing" at a frozen time
 * into nothing. Note it once; the button tick takes the route back. */
static void hiby_pcm_note_error(snd_pcm_t *pcm, long err)
{
    if (!hiby_pcm_bt_active() || hiby_pcm_bt_output_lost())
        return;
    if (err == -ENODEV || err == -EIO || err == -ENOTCONN ||
        snd_pcm_state(pcm) == SND_PCM_STATE_DISCONNECTED)
    {
        hiby_pcm_set_bt_output_lost(true);
        hiby_debug_log("pcm: Bluetooth output gone (%s), %u underruns so far",
                       err < 0 ? snd_strerror((int)err) : "disconnected", xruns);
    }
}

/* Stopping waits for the queued samples to play. A Bluetooth PCM that has
 * lost its device may never play them, and the wait holds the PCM lock
 * every Rockbox thread needs: drop them instead. */
static void hiby_pcm_finish(snd_pcm_t *pcm)
{
    /* Bluetooth is never drained: headphones that have gone without the
     * PCM noticing yet (after USB mode, out of range) never play the queue
     * out, and the wait froze the UI for 35 s and more (the R1's log,
     * 2026-10-13). Dropping loses a fraction of a second at a stop. */
    if (hiby_pcm_bt_active() || hiby_pcm_bt_output_lost() ||
        snd_pcm_state(pcm) == SND_PCM_STATE_DISCONNECTED)
        snd_pcm_drop(pcm);
    else
        snd_pcm_drain(pcm);
}

/* ---------------------------------------- Bluetooth: never block, drop */
/* A blocking write or start on a Bluetooth PCM waits on BlueALSA. The
 * start also runs on Rockbox threads (mixer_start_pcm from the codec), and
 * those all share one OS thread: a second test R1 logged the codec thread "stuck"
 * in a library for 3-6 s with the whole UI frozen (2026-10-23). Bluetooth
 * PCMs are non-blocking; a full output is simply tried again later. */
static void hiby_pcm_nonblock_if_bt(snd_pcm_t *pcm)
{
    if (pcm && hiby_pcm_bt_active())
        snd_pcm_nonblock(pcm, 1);
}

static int hiby_pcm_frame_bytes(void)
{
    return (int)(channels * sizeof(sample_t));
}

/* After frames went out: keep them for a pause's replay, and note how
 * far behind the ears are (at most four times a second: the delay call
 * asks the Bluetooth plugin). Under pcm_mtx. */
static void hiby_pcm_wrote(snd_pcm_t *pcm, const void *buf, long n)
{
    static struct timespec last;
    if (n > 0 && hiby_pcm_bt_active())
        hiby_out_hist_note(buf, n, hiby_pcm_frame_bytes());
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
    if (ms < 250 && ms >= 0)
        return;
    last = now;
    snd_pcm_sframes_t d = 0;
    if (!hiby_pcm_bt_active() || snd_pcm_delay(pcm, &d) < 0 || d < 0)
        d = 0;
    hiby_out_set_delay(d + (long)(pcm_size / 4), real_sample_rate);
}

/* The frames a pause dropped, first, as far as the output takes them now.
 * Under pcm_mtx. Returns false while some are still to go. */
static bool hiby_pcm_write_replay(snd_pcm_t *pcm)
{
    for (int guard = 0; hiby_out_replay_ready() && guard < 64; guard++)
    {
        snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);
        if (avail <= 0)
            return false;
        long want = avail < period_size ? (long)avail : (long)period_size;
        long n = hiby_out_replay_take(frames, want, hiby_pcm_frame_bytes());
        if (n <= 0)
            break;
        snd_pcm_sframes_t err = snd_pcm_writei(pcm, frames, n);
        if (err == -EPIPE)
        {
            xruns++;
            snd_pcm_recover(pcm, -EPIPE, 1);
            err = snd_pcm_writei(pcm, frames, n);
        }
        if (err < 0)
        {
            hiby_pcm_note_error(pcm, err);
            hiby_out_replay_clear();
            return true;
        }
        hiby_pcm_wrote(pcm, frames, err);
    }
    return !hiby_out_replay_ready();
}

/* Pause: the queue is dropped (BlueALSA's FIFO with it) so the music
 * stops now, not seconds later; the dropped frames are kept for resume,
 * with the rest of the buffer the mixer had handed over. Resume: what
 * was queued meanwhile (silence) goes, the kept frames play first. */
void pcm_alsa_output_pause(bool pause)
{
    hiby_pcm_mutex_init_once();
    pthread_mutex_lock(&pcm_mtx);
    if (handle && hiby_pcm_bt_active() && hiby_pcm_params_ready())
    {
        snd_pcm_state_t state = snd_pcm_state(handle);
        if (pause && hiby_out_replay_held())
        {
            /* paused already: only silence is queued */
            if (hiby_pcm_streaming(state))
            {
                snd_pcm_drop(handle);
                snd_pcm_prepare(handle);
            }
        }
        else if (pause)
        {
            snd_pcm_sframes_t d = 0;
            if (!hiby_pcm_streaming(state) || snd_pcm_delay(handle, &d) < 0 || d < 0)
                d = 0;
            /* still playing an earlier pause's frames again: those not
             * played yet come after the queue; else the mixer's buffer
             * we had not written yet does */
            long rest = hiby_out_replay_requeue();
            if (!rest && pcm_size >= 4 && pcm_data && hiby_pcm_streaming(state))
            {
                rest = (long)(pcm_size / 4);
                hiby_out_hist_note(pcm_data, rest, hiby_pcm_frame_bytes());
            }
            pcm_size = 0;
            long kept = hiby_out_replay_arm(d + rest);
            if (hiby_pcm_streaming(state))
            {
                snd_pcm_drop(handle);
                snd_pcm_prepare(handle);
            }
            /* the delay stays as it was: the ears are that far behind
             * Rockbox's position until the replay has played */
            hiby_debug_log("pcm: pause dropped %ld queued frames, %ld kept for resume", (long)d + rest, kept);
        }
        else if (hiby_out_replay_left() > 0)
        {
            hiby_out_replay_release();
            if (hiby_pcm_streaming(state))
            {
                snd_pcm_drop(handle);   /* only the silence since the pause */
                snd_pcm_prepare(handle);
            }
            pcm_size = 0;
        }
    }
    pthread_mutex_unlock(&pcm_mtx);
}

/* Stop, seek, skip: the old audio is not wanted at all. */
void pcm_alsa_output_flush(void)
{
    hiby_pcm_mutex_init_once();
    pthread_mutex_lock(&pcm_mtx);
    hiby_out_replay_clear();
    hiby_out_hist_clear();
    if (handle && hiby_pcm_bt_active() && hiby_pcm_params_ready())
    {
        if (hiby_pcm_streaming(snd_pcm_state(handle)))
        {
            snd_pcm_drop(handle);
            snd_pcm_prepare(handle);
        }
        pcm_size = 0;
        hiby_out_set_delay(0, real_sample_rate);
    }
    pthread_mutex_unlock(&pcm_mtx);
}

int pcm_alsa_switch_playback_device(const char *device)
{
    int rc;

    if (!device || !*device)
        return -1;

    playback_dev = device;
    if (!handle)
        return 0;

    hiby_pcm_mutex_init_once();
    pthread_mutex_lock(&pcm_mtx);
    /* Leaving an output: what is still queued for it is not wanted there,
     * and draining a Bluetooth PCM blocks for its whole buffer. */
    if (!hiby_pcm_keep_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK))
        snd_pcm_drop(handle);
    open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);
    hiby_pcm_set_bt_output_lost(false);
    hiby_out_replay_clear();            /* a new output: nothing to replay there */
    hiby_out_hist_clear();
    hiby_out_set_delay(0, real_sample_rate);
    pcm_dma_apply_settings_nolock();
    rc = (current_alsa_device == playback_dev) ? 0 : -1;
    /* parameters the device refused leave it unable to play anything */
    if (rc == 0 && snd_pcm_state(handle) == SND_PCM_STATE_OPEN)
    {
        hiby_debug_log("pcm: %s refused %lu Hz", playback_dev, (unsigned long)pcm_sampr);
        rc = -1;
    }
    pthread_mutex_unlock(&pcm_mtx);
    hiby_pcm_poll_kick();
    return rc;
}

void pcm_alsa_close_device(const char *device)
{
    if (playback_dev == device)
        close_hwdev();
}

static bool hiby_pcm_keep_hwdev(const char *device, snd_pcm_stream_t mode)
{
    if (!(handle && device == current_alsa_device
#ifdef HAVE_RECORDING
          && current_alsa_mode == mode
#endif
         ))
    {
        return false;
    }

    return snd_pcm_state(handle) != SND_PCM_STATE_DISCONNECTED;
}

static void hiby_pcm_stop_poll_thread(void)
{
    if (!hiby_pcm_poll_thread_running)
        return;

    hiby_pcm_poll_thread_stop = true;
    hiby_pcm_poll_kick();
    pthread_join(hiby_pcm_poll_thread, NULL);
    hiby_pcm_poll_thread_running = false;
    hiby_pcm_poll_thread_stop = false;
}

/* Wake the poll thread now: a stream is starting. */
static void hiby_pcm_poll_kick(void)
{
    pthread_mutex_lock(&hiby_pcm_idle_mtx);
    hiby_pcm_kicked = true;
    pthread_cond_signal(&hiby_pcm_idle_cond);
    pthread_mutex_unlock(&hiby_pcm_idle_mtx);
}

static bool hiby_pcm_streaming(snd_pcm_state_t state)
{
    return state == SND_PCM_STATE_RUNNING || state == SND_PCM_STATE_PREPARED ||
           state == SND_PCM_STATE_XRUN || state == SND_PCM_STATE_DRAINING;
}

static void hiby_pcm_idle_wait(void)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += HIBY_PCM_IDLE_WAIT_MS / 1000;
    until.tv_nsec += (HIBY_PCM_IDLE_WAIT_MS % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L)
    {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&hiby_pcm_idle_mtx);
    while (!hiby_pcm_kicked && !hiby_pcm_poll_thread_stop)
        if (pthread_cond_timedwait(&hiby_pcm_idle_cond, &hiby_pcm_idle_mtx, &until) == ETIMEDOUT)
            break;
    hiby_pcm_kicked = false;
    pthread_mutex_unlock(&hiby_pcm_idle_mtx);
}

static void *hiby_pcm_poll_thread_fn(void *arg)
{
    (void)arg;

    while (!hiby_pcm_poll_thread_stop)
    {
        /* Busy lock or unknown state: keep the normal cadence. */
        bool streaming = true;
        if (pthread_mutex_trylock(&pcm_mtx) == 0)
        {
            if (handle)
            {
                pcm_pump_locked(handle);
                streaming = hiby_pcm_streaming(snd_pcm_state(handle));
            }
            else
                streaming = false;
            pthread_mutex_unlock(&pcm_mtx);
        }
        if (streaming)
            usleep(hiby_pcm_poll_interval_us);
        else
            hiby_pcm_idle_wait();
    }
    return NULL;
}

static void hiby_pcm_start_poll_thread(void)
{
    int err;

    hiby_pcm_mutex_init_once();
    if (hiby_pcm_poll_thread_running)
        return;

    err = pthread_create(&hiby_pcm_poll_thread, NULL, hiby_pcm_poll_thread_fn, NULL);
    if (err == 0)
        hiby_pcm_poll_thread_running = true;
    else
        panicf("Unable to start ALSA poll thread: %s", strerror(err));
}

#endif /* PCM_ALSA_HIBY_HOOKS_H */
