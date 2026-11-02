/***************************************************************************
 * HiBy-specific helpers for the hosted ALSA PCM backend
 ****************************************************************************/

#ifndef PCM_ALSA_HIBY_H
#define PCM_ALSA_HIBY_H

#include <stdbool.h>
#include <alsa/asoundlib.h>

/* HiBy-only runtime playback route control. */
int pcm_alsa_switch_playback_device(const char *device);
void hiby_pcm_set_bt_mac(const char *mac);
const char *hiby_pcm_get_bt_mac(void);

bool hiby_pcm_is_bluealsa_device(const char *device);
void hiby_pcm_set_bt_output_lost(bool lost);
bool hiby_pcm_bt_output_lost(void);

void hiby_pcm_adjust_bt_buffering(snd_pcm_sframes_t *period_size,
                                  snd_pcm_sframes_t *buffer_size,
                                  bool bluealsa_active);

unsigned int hiby_pcm_calc_poll_interval(unsigned int sample_rate,
                                         snd_pcm_sframes_t period_size,
                                         bool bluealsa_active);

snd_pcm_uframes_t hiby_pcm_start_threshold(snd_pcm_sframes_t buffer_size,
                                           snd_pcm_sframes_t period_size,
                                           bool bluealsa_active);

/* Bluetooth: what a pause drops is played again on resume (see the .c) */
void hiby_out_hist_note(const void *frames, long n, int frame_bytes);
void hiby_out_hist_clear(void);
long hiby_out_replay_arm(long delay_frames);
long hiby_out_replay_left(void);
long hiby_out_replay_take(void *dst, long max, int frame_bytes);
void hiby_out_replay_clear(void);
long hiby_out_replay_requeue(void);
void hiby_out_replay_release(void);
bool hiby_out_replay_ready(void);
bool hiby_out_replay_held(void);
void hiby_out_set_delay(long frames, unsigned rate);
unsigned hiby_out_delay_ms(void);
/* from pcmbuf: a pause (true) or a resume (false); a stop, seek or skip */
void pcm_alsa_output_pause(bool pause);
void pcm_alsa_output_flush(void);

/* gapless over Bluetooth: the mixer rate while a link is in use */
void hiby_pcm_set_bt_rate(unsigned rate);
unsigned hiby_pcm_play_rate(unsigned track_rate);

#endif /* PCM_ALSA_HIBY_H */
