#ifndef __HIBY_BLUETOOTH_H__
#define __HIBY_BLUETOOTH_H__

#include <stdbool.h>
#include <stdint.h>

#define HIBY_BT_MAX_DEVICES 32
#define HIBY_BT_NAME_LEN 80
#define HIBY_BT_CODEC_NAME_LEN 16

struct hiby_bt_device
{
    char mac[18];
    char name[HIBY_BT_NAME_LEN];
    bool paired;
};

struct hiby_bt_status
{
    bool enabled, suspended, connected, routed;
    char mac[18];
    char name[HIBY_BT_NAME_LEN];
    char codec[HIBY_BT_CODEC_NAME_LEN];
};

void hiby_bt_status(struct hiby_bt_status *status);
int hiby_bt_devices(struct hiby_bt_device *devices, int max_devices);
bool hiby_bt_power(bool on);
bool hiby_bt_connect(const struct hiby_bt_device *device);
bool hiby_bt_disconnect(void);
bool hiby_bt_forget(const char *mac);
bool hiby_bt_scan_start(void);
int hiby_bt_scan_finish(struct hiby_bt_device *devices, int max_devices);
int hiby_bt_codecs(const char *mac, char codecs[][HIBY_BT_CODEC_NAME_LEN], int max_codecs);
bool hiby_bt_codec(const char *mac, const char *codec);
/* Call first on a pthread that runs the calls above; the UI thread then
 * restarts playback when hiby_bt_take_audio_kick() says so. */
void hiby_bt_worker_thread(void);
bool hiby_bt_take_audio_kick(void);
/* Set when audio left the headphones with nothing in the jack: the UI
 * thread pauses (a worker may not call into the audio thread). */
bool hiby_bt_take_audio_pause(void);
/* Route audio to headphones the stack has connected by itself (they were
 * switched on, or came back in range). Blocking: worker thread only. */
bool hiby_bt_auto_route(void);

/* Receive from phone: the player as Bluetooth headphones (an A2DP sink).
 * One thread of its own runs the session, from the profile switch to the
 * restore of today's source mode; these calls never block. */
enum { HIBY_BTRX_OFF, HIBY_BTRX_STARTING, HIBY_BTRX_WAITING, HIBY_BTRX_CONNECTED,
       HIBY_BTRX_STOPPING };
struct hiby_btrx_status
{
    int state;
    unsigned connects;                  /* bumps on every phone that connects */
    char mac[18];
    char name[HIBY_BT_NAME_LEN];
    char codec[HIBY_BT_CODEC_NAME_LEN];
    int rate, channels;
    int volume;                         /* the phone's AVRCP level 0..127, -1 unknown */
    char title[96], artist[96];
    char player[16];                    /* the phone's player: "playing", "paused", "" */
    char error[80];                     /* why the last session ended, "" for a plain stop */
    char codecs[6][HIBY_BT_CODEC_NAME_LEN];   /* what the phone and the player both have */
    int codec_count;
    char codec_note[64];                /* why a codec asked for did not take, "" if it did */
};
/* ask the phone to send this codec instead (a sink can only ask) */
void hiby_btrx_codec(const char *codec);
bool hiby_btrx_start(void);
void hiby_btrx_stop(void);
bool hiby_btrx_running(void);
void hiby_btrx_status(struct hiby_btrx_status *status);
/* "Play", "Pause", "Next" or "Previous", sent to the phone's player */
void hiby_btrx_control(const char *method);
/* the session's steps, one line at a time, for the field log */
bool hiby_btrx_take_event(char *line, int size);
/* the phone's audio, S16 stereo at status.rate; any context, lock-free */
int hiby_btrx_read(int16_t *dst, int frames);
int hiby_btrx_buffered(void);
unsigned hiby_btrx_idle_ms(void);     /* since audio last arrived */

/* ---- names (kept in .rockbox/sloppy/bluetooth.cfg, given to BlueZ) ---- */
#define HIBY_BT_ALIAS_LEN 64
/* the player's name as others see it; "" when never set (BlueZ's stands) */
void hiby_bt_player_name(char *out, int size);
bool hiby_bt_set_player_name(const char *name);      /* blocking: worker */
/* a paired device's name here; "" gives it its own name back */
bool hiby_bt_rename(const char *mac, const char *name);   /* blocking: worker */
void hiby_bt_local_name(const char *mac, char *out, int size);   /* "" if none */

/* ---- pairing: a passkey or PIN to show, or to ask for ---- */
enum { HIBY_BT_ASK_NONE, HIBY_BT_ASK_CONFIRM, HIBY_BT_ASK_PIN, HIBY_BT_ASK_PASSKEY,
       HIBY_BT_ASK_SHOW_PASSKEY, HIBY_BT_ASK_SHOW_PIN };
struct hiby_bt_ask
{
    int kind;
    unsigned serial;                    /* a new question, a new serial */
    char mac[18];
    char name[HIBY_BT_NAME_LEN];
    char code[20];                      /* the passkey or PIN to compare or type */
};
/* any thread, never blocks: the question the pairing waits on, if any */
bool hiby_bt_pair_question(struct hiby_bt_ask *ask);
/* the answer: accept or not, and the code typed for a PIN or passkey */
void hiby_bt_pair_answer(unsigned serial, bool accept, const char *code);

/* ---- why the last connect or pairing failed ---- */
enum { HIBY_BT_ERR_NONE, HIBY_BT_ERR_RADIO, HIBY_BT_ERR_NOT_FOUND, HIBY_BT_ERR_PAIR_REJECTED,
       HIBY_BT_ERR_PAIR_FAILED, HIBY_BT_ERR_PAIR_TIMEOUT, HIBY_BT_ERR_CANCELED, HIBY_BT_ERR_NO_AUDIO,
       HIBY_BT_ERR_HUNG };
int hiby_bt_last_error(void);

/* ---- reconnecting to the last headphones (R9) ---- */
bool hiby_bt_last_device(char mac[18]);
/* One try, blocking (worker): 1 routed, 0 tried and failed, -1 nothing
 * to try (radio off, no device yet, the user let it go). */
int hiby_bt_reconnect(void);

/* ---- SBC quality: 0 standard, 1 XQ, 2 XQ+ (BlueALSA --sbc-quality) ---- */
int hiby_bt_sbc_quality(void);
bool hiby_bt_set_sbc_quality(int quality);           /* blocking: worker */

/* ---- AirPods: battery and noise control over AAP (L2CAP PSM 0x1001) ---- */
struct hiby_bt_aap
{
    bool up;                            /* the AirPods answered */
    int left, right, box;               /* percent, -1 unknown or not there */
    bool left_charging, right_charging, box_charging, in_ear;
    int noise;                          /* 0 unknown, 1 off, 2 noise cancellation, 3 transparency, 4 adaptive */
};
void hiby_bt_aap_status(struct hiby_bt_aap *out);   /* never blocks */
bool hiby_bt_aap_noise(int mode);                   /* 1..4 */
void hiby_bt_aap_parse(const uint8_t *packet, int length);

int hiby_bluetooth_menu(void);

bool bt_is_enabled_fast(void);
bool bt_disable(void);
bool bt_enable(void);
bool bt_is_connected_fast(void);
bool bt_is_routed_fast(void);
/* set while SloppyOS runs: it picks and sends the Bluetooth volume itself */
extern volatile bool hiby_bt_volume_by_ui;
bool bt_autoconnection_route_to_bluetooth(char active_mac[18], bool bt_on);
void bt_route_to_local(void);

#endif /* __HIBY_BLUETOOTH_H__ */
