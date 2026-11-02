/* A pretend libasound for pcm_bt_test.c: the calls pcm-alsa.c makes, on
 * a model of a Bluetooth output (see the test). */
#ifndef FAKE_ASOUNDLIB_H
#define FAKE_ASOUNDLIB_H
#include <stddef.h>
#include <errno.h>
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
typedef struct fake_pcm snd_pcm_t;
typedef struct { int dummy; } snd_pcm_hw_params_t, snd_pcm_sw_params_t;
typedef struct fake_async snd_async_handler_t;
typedef enum { SND_PCM_STATE_OPEN, SND_PCM_STATE_SETUP, SND_PCM_STATE_PREPARED, SND_PCM_STATE_RUNNING,
               SND_PCM_STATE_XRUN, SND_PCM_STATE_DRAINING, SND_PCM_STATE_PAUSED, SND_PCM_STATE_SUSPENDED,
               SND_PCM_STATE_DISCONNECTED } snd_pcm_state_t;
typedef enum { SND_PCM_STREAM_PLAYBACK, SND_PCM_STREAM_CAPTURE } snd_pcm_stream_t;
typedef enum { SND_PCM_ACCESS_RW_INTERLEAVED = 3 } snd_pcm_access_t;
typedef enum { SND_PCM_FORMAT_S16 = 2, SND_PCM_FORMAT_S16_LE = 2, SND_PCM_FORMAT_S32_LE = 10 } snd_pcm_format_t;
int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode);
int snd_pcm_close(snd_pcm_t *pcm);
int snd_pcm_nonblock(snd_pcm_t *pcm, int nonblock);
int snd_pcm_drain(snd_pcm_t *pcm);
int snd_pcm_drop(snd_pcm_t *pcm);
int snd_pcm_prepare(snd_pcm_t *pcm);
int snd_pcm_start(snd_pcm_t *pcm);
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm);
const char *snd_pcm_state_name(snd_pcm_state_t s);
int snd_pcm_recover(snd_pcm_t *pcm, int err, int silent);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t n);
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t n);
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm);
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *d);
const char *snd_strerror(int err);
int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **p);
void snd_pcm_hw_params_free(snd_pcm_hw_params_t *p);
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *p);
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_access_t a);
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_format_t f);
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned c);
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned *r, int *dir);
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_uframes_t *v);
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_uframes_t *v, int *dir);
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *p);
int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **p);
void snd_pcm_sw_params_free(snd_pcm_sw_params_t *p);
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *p);
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *p, snd_pcm_uframes_t v);
int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *p, snd_pcm_uframes_t v);
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *p);
int snd_pcm_format_set_silence(snd_pcm_format_t f, void *buf, unsigned n);
snd_pcm_t *snd_async_handler_get_pcm(snd_async_handler_t *h);
#endif
