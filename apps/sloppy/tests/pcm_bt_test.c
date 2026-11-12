/* The ALSA output (firmware/target/hosted/pcm-alsa.c with the HiBy hooks)
 * on a pretend Bluetooth PCM: our ring, then 500 ms more queued behind it
 * (BlueALSA's FIFO, the codec, the speaker), drained in real time by a
 * pretend speaker that notes every frame it plays. Music frames carry
 * their own index, so the test can tell a repeat or a skip from a pause.
 * Run by run-bt-tests.sh. Mocks prove the driver's logic, not the radio:
 * a real speaker's own buffer is not dropped by snd_pcm_drop. */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "alsa/asoundlib.h"
#include "system.h"
#include "pcm.h"

void hiby_debug_log(const char *format, ...);
unsigned long pcm_sampr = 44100;

/* ---------------------------------------------------------- the device */
#define RATE 44100
#define SERVER_FRAMES 22050             /* 500 ms behind the ring */
#define QCAP (1 << 18)
static volatile int closed_gone;      /* a gone PCM was asked to drop or close */
struct fake_pcm {
    snd_pcm_state_t st;
    int nonblock;
    long buf, period, start_th, q;      /* q: frames queued, ring + server */
    int32_t fifo[QCAP];                 /* the index each queued frame carries, -1 silence */
    unsigned head;
    long last_us;
    double carry;
    volatile int gone;                  /* the headphones went: Drop and Close never answer */
};
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fake_pcm *dev;
static long heard_last = -1, heard_jumps, heard_repeats, heard_music, repeat_frames;
static long last_music_us, first_after_us;
static long expect_from = -1;           /* a seek: the next index heard may jump here */
static long old_after_flush;            /* frames from before a flush heard after it */
static int flushed;

static long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}
/* the speaker plays what time allows */
static void advance(struct fake_pcm *p)
{
    long t = now_us();
    if (p->st != SND_PCM_STATE_RUNNING) { p->last_us = t; return; }
    p->carry += (t - p->last_us) * (double)RATE / 1e6;
    p->last_us = t;
    long n = (long)p->carry;
    p->carry -= n;
    for (; n > 0 && p->q > 0; n--, p->q--)
    {
        int32_t v = p->fifo[p->head++ % QCAP];
        if (v < 0) continue;
        heard_music++;
        last_music_us = t;
        if (flushed && v < expect_from) old_after_flush++;
        if (expect_from >= 0 && v >= expect_from)
        {
            if (!first_after_us) first_after_us = t;
            heard_last = v; expect_from = -1;
            continue;
        }
        if (heard_last >= 0 && expect_from < 0)
        {
            if (v <= heard_last)
            {
                /* the frames heard between the delay call and the drop
                 * play twice: tolerated up to 5 ms, counted */
                if (heard_last - v + 1 > repeat_frames) repeat_frames = heard_last - v + 1;
                if (heard_last - v + 1 > RATE / 200) heard_repeats++;
            }
            else if (v > heard_last + 1) { if (!heard_jumps) printf("  skip: %ld -> %d\n", heard_last, v); heard_jumps++; }
        }
        heard_last = v;
    }
}
static long ring_q(struct fake_pcm *p) { return p->q > SERVER_FRAMES ? p->q - SERVER_FRAMES : 0; }

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t s, int mode)
{
    (void)name; (void)s; (void)mode;
    struct fake_pcm *p = calloc(1, sizeof *p);
    p->st = SND_PCM_STATE_OPEN;
    pthread_mutex_lock(&dev_lock); dev = p; pthread_mutex_unlock(&dev_lock);
    *pcm = p;
    return 0;
}
int snd_pcm_close(snd_pcm_t *pcm)
{
    if (pcm->gone) { closed_gone = 1; for (;;) sleep(1); }
    return 0;
}
int snd_pcm_nonblock(snd_pcm_t *pcm, int nb) { pcm->nonblock = nb; return 0; }
int snd_pcm_drain(snd_pcm_t *pcm) { (void)pcm; assert(!"a Bluetooth PCM is never drained"); return 0; }
int snd_pcm_drop(snd_pcm_t *pcm)
{
    if (pcm->gone) { closed_gone = 1; for (;;) sleep(1); }
    pthread_mutex_lock(&dev_lock);
    advance(pcm);
    pcm->head += pcm->q; pcm->q = 0;     /* the plugin's Drop empties BlueALSA's FIFO too */
    pcm->st = SND_PCM_STATE_SETUP;
    pthread_mutex_unlock(&dev_lock);
    return 0;
}
int snd_pcm_prepare(snd_pcm_t *pcm) { pthread_mutex_lock(&dev_lock); pcm->st = SND_PCM_STATE_PREPARED; pthread_mutex_unlock(&dev_lock); return 0; }
int snd_pcm_start(snd_pcm_t *pcm)
{
    pthread_mutex_lock(&dev_lock);
    if (pcm->st == SND_PCM_STATE_PREPARED) { pcm->st = SND_PCM_STATE_RUNNING; pcm->last_us = now_us(); }
    pthread_mutex_unlock(&dev_lock);
    return 0;
}
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm)
{
    pthread_mutex_lock(&dev_lock); advance(pcm); snd_pcm_state_t s = pcm->st; pthread_mutex_unlock(&dev_lock);
    return s;
}
const char *snd_pcm_state_name(snd_pcm_state_t s) { (void)s; return "state"; }
int snd_pcm_recover(snd_pcm_t *pcm, int err, int silent) { (void)err; (void)silent; return snd_pcm_prepare(pcm); }
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t n)
{
    pthread_mutex_lock(&dev_lock);
    advance(pcm);
    long room = pcm->buf - ring_q(pcm);
    if (room <= 0) { pthread_mutex_unlock(&dev_lock); assert(pcm->nonblock); return -EAGAIN; }
    long k = (long)n < room ? (long)n : room;
    const int16_t *s = buf;
    for (long i = 0; i < k; i++)
    {
        int32_t v = s[2 * i] ? (int32_t)(((uint16_t)s[2 * i + 1] << 15) | ((uint16_t)s[2 * i] - 1)) : -1;
        pcm->fifo[(pcm->head + pcm->q++) % QCAP] = v;
    }
    if (pcm->st == SND_PCM_STATE_PREPARED && ring_q(pcm) + (pcm->q < SERVER_FRAMES ? pcm->q : SERVER_FRAMES) >= pcm->start_th)
    { pcm->st = SND_PCM_STATE_RUNNING; pcm->last_us = now_us(); }
    pthread_mutex_unlock(&dev_lock);
    return k;
}
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t n) { (void)pcm; (void)buf; (void)n; return -EIO; }
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm)
{
    pthread_mutex_lock(&dev_lock); advance(pcm); long a = pcm->buf - ring_q(pcm); pthread_mutex_unlock(&dev_lock);
    return a;
}
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *d)
{
    pthread_mutex_lock(&dev_lock); advance(pcm); *d = pcm->q; pthread_mutex_unlock(&dev_lock);
    return 0;
}
const char *snd_strerror(int err) { (void)err; return "error"; }
static snd_pcm_hw_params_t hwp; static snd_pcm_sw_params_t swp;
int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **p) { *p = &hwp; return 0; }
void snd_pcm_hw_params_free(snd_pcm_hw_params_t *p) { (void)p; }
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *p) { (void)pcm; (void)p; return 0; }
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_access_t a) { (void)pcm; (void)p; (void)a; return 0; }
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_format_t f) { (void)pcm; (void)p; (void)f; return 0; }
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned c) { (void)pcm; (void)p; (void)c; return 0; }
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned *r, int *dir) { (void)pcm; (void)p; (void)dir; *r = RATE; return 0; }
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_uframes_t *v) { (void)p; pcm->buf = (long)*v; return 0; }
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_uframes_t *v, int *dir) { (void)p; (void)dir; pcm->period = (long)*v; return 0; }
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *p) { (void)p; pcm->st = SND_PCM_STATE_SETUP; return 0; }
int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **p) { *p = &swp; return 0; }
void snd_pcm_sw_params_free(snd_pcm_sw_params_t *p) { (void)p; }
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *p) { (void)pcm; (void)p; return 0; }
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *p, snd_pcm_uframes_t v) { (void)p; pcm->start_th = (long)v; return 0; }
int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *p, snd_pcm_uframes_t v) { (void)pcm; (void)p; (void)v; return 0; }
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *p) { (void)pcm; (void)p; return 0; }
int snd_pcm_format_set_silence(snd_pcm_format_t f, void *buf, unsigned n) { (void)f; memset(buf, 0, n * 4); return 0; }
snd_pcm_t *snd_async_handler_get_pcm(snd_async_handler_t *h) { (void)h; return NULL; }

/* ----------------------------------------------------------- the mixer */
/* Rockbox's mixer as the driver sees it: the next buffer of the song, or
 * silence while the channel is paused. */
static volatile int src_playing = 1;
static long src_next;                   /* the index the next music frame carries */
static int16_t src_buf[2][1024 * 2];
static int src_k;
bool pcm_play_dma_complete_callback(enum pcm_dma_status status, const void **addr, size_t *size)
{
    (void)status;
    int16_t *b = src_buf[src_k ^= 1];
    for (int i = 0; i < 1024; i++)
    {
        if (src_playing)
        {
            long v = src_next++;
            b[2 * i] = (int16_t)((v & 0x7fff) + 1);
            b[2 * i + 1] = (int16_t)(v >> 15);
        }
        else b[2 * i] = b[2 * i + 1] = 0;
    }
    *addr = b; *size = sizeof src_buf[0];
    return true;
}
enum pcm_dma_status pcm_play_dma_status_callback(enum pcm_dma_status status) { return status; }

#include "pcm-alsa.c"
#include "pcm-alsa-hiby.c"

void hiby_debug_log(const char *format, ...)
{
    va_list ap; va_start(ap, format); printf("  log: "); vprintf(format, ap); printf("\n"); va_end(ap);
}

static void wait_ms(long ms) { usleep(ms * 1000); }
static long heard_count(void) { pthread_mutex_lock(&dev_lock); advance(dev); long n = heard_music; pthread_mutex_unlock(&dev_lock); return n; }

/* pause: how long the speaker goes on playing music after it */
static long pause_quiet_ms(bool hooks)
{
    long t0 = now_us();
    src_playing = 0;                     /* pcmbuf_pause(true): the channel stops feeding */
    if (hooks) pcm_alsa_output_pause(true);
    long seen = heard_count();
    for (int i = 0; i < 300; i++)        /* until 300 ms with no music */
    {
        wait_ms(10);
        long n = heard_count();
        if (n != seen) { seen = n; i = 0; }
    }
    pthread_mutex_lock(&dev_lock);
    long quiet = last_music_us > t0 ? (last_music_us - t0) / 1000 : 0;
    pthread_mutex_unlock(&dev_lock);
    return quiet;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    hiby_pcm_set_bt_mac("AA:BB:CC:DD:EE:01");
    pcm_alsa_set_playback_device("bluealsa:DEV=AA:BB:CC:DD:EE:01,PROFILE=a2dp");
    pcm_play_dma_init();
    static int16_t first[1024 * 2];
    pcm_play_lock();
    pcm_play_dma_start(first, 0);
    pcm_play_unlock();
    assert(dev && dev->nonblock);
    printf("ring %ld frames (%ld ms), period %ld, start at %ld; %d frames (500 ms) behind it\n",
           dev->buf, dev->buf * 1000 / RATE, dev->period, dev->start_th, SERVER_FRAMES);
    assert(dev->buf * 1000 / RATE >= 180 && dev->buf * 1000 / RATE <= 200);
    printf("PASS: a Bluetooth PCM is non-blocking, its ring 186 ms (rides out a squeeze, a skip still quick).\n");

    /* the delay the visualizers and the progress bar wait out */
    /* sampled every 100 ms for 1.5 s, the median taken: it is read four
     * times a second and a busy test machine dips now and then */
    unsigned seen[15];
    for (int i = 0; i < 15; i++)
    {
        wait_ms(100);
        seen[i] = hiby_out_delay_ms();
        for (int j = i; j > 0 && seen[j] < seen[j - 1]; j--) { unsigned t = seen[j]; seen[j] = seen[j - 1]; seen[j - 1] = t; }
    }
    unsigned d = seen[7];
    long q; pthread_mutex_lock(&dev_lock); advance(dev); q = dev->q; pthread_mutex_unlock(&dev_lock);
    printf("delay reported %u ms, queued now %ld ms\n", d, q * 1000 / RATE);
    assert(d >= 500 && d <= 800 && labs((long)d - q * 1000 / RATE) < 150);
    printf("PASS: the output's delay is measured (%u ms).\n", d);

    /* before: a pause without the drop plays on for the whole queue */
    long before = pause_quiet_ms(false);
    printf("pause without the drop: music for %ld ms after it\n", before);
    assert(before >= 500);
    src_playing = 1;
    wait_ms(1500);
    heard_repeats = heard_jumps = 0;

    /* after: quiet within 200 ms, then resumed with nothing lost or repeated */
    long after = pause_quiet_ms(true);
    printf("pause with the drop: music for %ld ms after it\n", after);
    assert(after <= 200);
    printf("PASS: pause goes quiet in %ld ms (was %ld ms).\n", after, before);
    wait_ms(1000);
    long last_before = heard_last;
    pcm_alsa_output_pause(false);        /* pcmbuf_pause(false): before the channel feeds again */
    src_playing = 1;
    wait_ms(1500);
    printf("resume: last heard before %ld, %ld skips, %ld repeats over 5 ms, longest overlap %ld frames\n",
           last_before, heard_jumps, heard_repeats, repeat_frames);
    assert(heard_jumps == 0 && heard_repeats == 0 && heard_last > last_before);
    printf("PASS: resume plays on from the frame heard last: no skip, no repeat over 5 ms.\n");

    /* a second pause while the first one's frames are still replaying */
    wait_ms(30);
    src_playing = 0; pcm_alsa_output_pause(true); wait_ms(5);
    pcm_alsa_output_pause(false); src_playing = 1;
    wait_ms(1200);
    printf("quick pause and resume: %ld skips, %ld repeats\n", heard_jumps, heard_repeats);
    assert(heard_jumps == 0 && heard_repeats == 0);

    /* a seek or a skip: the old audio goes, the new is heard at once */
    long t0 = now_us();
    pthread_mutex_lock(&dev_lock);
    expect_from = 1000000; first_after_us = 0;
    pthread_mutex_unlock(&dev_lock);
    pcm_play_lock(); src_next = 1000000; pcm_play_unlock();
    pcm_alsa_output_flush();             /* pcmbuf_play_stop() */
    pthread_mutex_lock(&dev_lock); flushed = 1; pthread_mutex_unlock(&dev_lock);
    for (int i = 0; i < 100 && !first_after_us; i++) wait_ms(5), heard_count();
    long seek_ms = first_after_us ? (first_after_us - t0) / 1000 : -1;
    printf("seek: new audio heard after %ld ms, %ld old frames after the flush\n", seek_ms, old_after_flush);
    assert(seek_ms >= 0 && seek_ms <= 200 && old_after_flush == 0);
    printf("PASS: a seek or skip is heard in %ld ms, nothing old after it.\n", seek_ms);

    /* gapless: over Bluetooth every track plays at the link's rate */
    hiby_pcm_set_bt_rate(44100);
    assert(hiby_pcm_play_rate(48000) == 44100 && hiby_pcm_play_rate(96000) == 44100);
    hiby_pcm_set_bt_mac(NULL);
    assert(hiby_pcm_play_rate(96000) == 96000);
    printf("PASS: over Bluetooth the mixer keeps the link's rate between tracks.\n");

    /* the headphones gone: BlueALSA never answers the drop or the close,
     * and the route back to the speaker must not wait for it (every
     * Rockbox thread is one OS thread: a test R1 froze, 10 Nov) */
    pcm_alsa_set_playback_device("bluealsa:DEV=AA:BB:CC:DD:EE:01,PROFILE=a2dp");
    pcm_play_lock(); pcm_play_dma_start(first, 0); pcm_play_unlock();
    struct fake_pcm *bt = dev;
    bt->gone = 1;
    long s0 = now_us();
    pcm_alsa_switch_playback_device("hw:0,0");
    long switch_ms = (now_us() - s0) / 1000;
    for (int i = 0; i < 100 && !closed_gone; i++) wait_ms(5);
    printf("route back to the speaker with the headphones gone: %ld ms, close asked %d\n", switch_ms, closed_gone);
    assert(switch_ms < 500 && dev != bt && closed_gone);
    printf("PASS: a Bluetooth output that never closes doesn't hold the player.\n");
    _exit(0);                            /* its closing thread waits for good; the speaker's cleanup is no test */
}
