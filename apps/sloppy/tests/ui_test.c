/* Executes production LVGL screens with fake Rockbox services; does not emulate hardware. */
#include <assert.h>
#include <malloc/malloc.h>
#include <stdint.h>
/* the glass reads the panel: here, the harness's copy of it */
static const uint16_t *sl_test_pixels(void);
#define SL_SCREEN_PIXELS() sl_test_pixels()
#define SL_SCREEN_STRIDE W
#include "../sloppy.c"
#include "gb_testrom.h"
/* the Audiobook Mod, as far as the Audiobooks page sees it (defined at the end) */
static struct ab_book fake_books[3];
static int ab_play_file = -1;
static unsigned long ab_play_ms;          /* and how far into it */
static const char *fake_files[3];         /* the chapters ab_db_list_files gives */
static int ab_saves;                      /* ab_progress_save calls */
static bool test_book_playing;            /* audiobooks_is_book says yes */              /* the file audiobooks_play_at was asked for */
static const char *fake_book_str[3][AB_NUM_STR];
static struct ab_progress fake_prog[3];
static bool fake_prog_on[3];
static int fake_books_n, ab_inits, ab_scans, ab_plays = -1;
#include <sys/socket.h>
#include <netinet/in.h>
/* a phone's browser in one call: frames run while the player answers */
static void frame(unsigned ms);
static char rm_cookie[64];               /* "Cookie: gbtok=...\r\n" once rm_test_pair() paired, as a phone sends it */
static int http_get(const char *path, char *out, int cap)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(SL_REMOTE_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { shutdown(s, SHUT_RDWR); return -1; }
    char req[400];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n%s\r\n", path, rm_cookie);
    send(s, req, n, 0);
    int got = 0;
    /* up to 5 s of the Mac's own time: frames run faster than that, and a
     * loaded Mac can take longer than 300 of them to answer */
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int k = 0; got < cap - 1; k++)
    {
        ssize_t r = recv(s, out + got, cap - 1 - got, MSG_DONTWAIT);
        if (r > 0) { got += (int)r; continue; }
        if (r == 0) break;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (k >= 300 && (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > 5000) break;
        frame(20);
    }
    out[got] = '\0';
    shutdown(s, SHUT_RDWR);
    (close)(s);   /* the real close, past the fake card's macro (it leaked a socket a call) */
    return got;
}
/* the remote obeys a paired phone only: pair as its page does, the code
 * read off the player, and keep the cookie it gives for every request */
static void rm_test_pair(void)
{
    static char r[2048];
    char code[8], path[32];
    unsigned serial;
    rm_cookie[0] = '\0';
    assert(http_get("/pair", r, sizeof r) > 0 && strstr(r, "202 Accepted"));
    assert(sl_net_remote_pair_code(code, sizeof code, &serial) && strlen(code) == 6);
    snprintf(path, sizeof path, "/pair?code=%s", code);
    assert(http_get(path, r, sizeof r) > 0 && strstr(r, "200 OK"));
    char *c = strstr(r, "Set-Cookie: gbtok=");
    assert(c && strspn(c + 18, "0123456789abcdef") == 32);
    snprintf(rm_cookie, sizeof rm_cookie, "Cookie: %.38s\r\n", c + 12);
}
#include "src/misc/lv_bidi_private.h"
#include "src/misc/lv_text_ap.h"
#include "src/misc/lv_text_private.h"
#include "src/display/lv_display_private.h"
#undef close
int fake_close(int fd);
int sim_close(int fd) { return fd >= 100 ? fake_close(fd) : close(fd); }
#define close sim_close                  /* the tests' own closes too: a fake card file left open took one of its eight handles */
struct user_settings global_settings;
struct system_status global_status;
volatile long current_tick;
struct sl_lib sl_lib;
static int fake_dir_index;
static bool fake_playlists;
static bool fake_many;
static int fake_pl_count = 1;           /* saved playlists the fake card has */
/* a folder whose files live in the fake card below (fake_fs) lists them */
static char fake_dir_listed[MAX_PATH];
static struct sim_dirent *fake_fs_readdir(void);
/* /Card: a sandboxed card in memory (fake_fs), with folders: the file
 * manager's tests make, rename, move and copy only in here. Nothing under
 * it is ever a real path on the Mac. */
#define FAKE_CARD "/Card"
static bool fake_card(const char *p) { return !strncmp(p,FAKE_CARD,5) && (p[5]=='\0'||p[5]=='/'); }
static bool fake_is_dir(const char *p);
static bool fake_huge;                   /* a folder of 2,000: 200 folders and 1,800 songs, in no order */
static int fake_opendirs;                /* folders open now: each closed, as on the card */
static int fake_dirs_open;               /* any folder, on /Card or not: one left open keeps the R1's card from the computer */
DIR *sim_opendir(const char *path) {
    fake_dir_listed[0]='\0';
    if (fake_card(path)) { if (!fake_is_dir(path)) return NULL; strlcpy(fake_dir_listed,path,sizeof fake_dir_listed); fake_dir_index=0; fake_opendirs++; fake_dirs_open++; return (DIR *)2; }
    fake_dirs_open++;
    fake_huge=strstr(path,"Huge")!=NULL;
    if (strstr(path,"spotify")||strstr(path,"sp/Playlists")||strstr(path,"AutoEq")||!strcmp(path,"/peq")) strlcpy(fake_dir_listed,path,sizeof fake_dir_listed);
    fake_playlists=strstr(path,"Playlists")!=NULL; fake_many=strstr(path,"Lots")!=NULL; fake_dir_index=0; return (DIR *)1; }
struct sim_dirent *sim_readdir(DIR *dir) {
    (void)dir;
    if (fake_dir_listed[0]) return fake_fs_readdir();
    static struct sim_dirent entry;
    if (fake_huge) {   /* "Disc 7", "Song 12.flac": numbers that sort wrong as words */
        if (fake_dir_index>=2000) return NULL;
        int k=(fake_dir_index*7919)%2000;   /* a stride prime to 2,000: every one once, scrambled */
        if (k<200) snprintf(entry.d_name,sizeof entry.d_name,"Disc %d",k+1);
        else snprintf(entry.d_name,sizeof entry.d_name,"Song %d.flac",k-199);
        fake_dir_index++;
        return &entry;
    }
    const char *names[] = {"Music", "song.mp3", "notes.txt"};
    static const char *const pls[] = {"mix.m3u8", "Road trip.m3u8", "Late night.m3u8", "Gym.m3u8"};
    if (fake_dir_index >= (fake_many ? 150 : fake_playlists ? fake_pl_count : 3)) return NULL;
    if (fake_many) snprintf(entry.d_name,sizeof entry.d_name,"track%03d.mp3",fake_dir_index);
    else strlcpy(entry.d_name,fake_playlists ? pls[fake_dir_index] : names[fake_dir_index],sizeof entry.d_name);
    fake_dir_index++;
    return &entry;
}
int sim_closedir(DIR *dir) { if (dir) fake_dirs_open--; if (dir==(DIR *)2) fake_opendirs--; return 0; }
static long fake_size(const char *path);
struct dirinfo dir_get_info(DIR *dir, struct sim_dirent *entry) {
    (void)dir;
    struct dirinfo info={0};
    if (fake_dir_listed[0] && fake_card(fake_dir_listed)) {
        char p[400]; snprintf(p,sizeof p,"%s/%s",fake_dir_listed,entry->d_name);
        if (fake_is_dir(p)) info.attribute=ATTR_DIRECTORY; else info.size=fake_size(p)*(sl_audio_name(p)?12000:1);   /* songs as big as songs are (the bytes are a stand-in) */
        return info;
    }
    if (!strcmp(entry->d_name,"Music") || (fake_huge && !strncmp(entry->d_name,"Disc ",5))) info.attribute=ATTR_DIRECTORY;
    else if (fake_huge) info.size=31457280;
    return info;
}
static int queued, started = -1, insert_fail = -1, test_status;
static char card_paths[256][MAX_PATH];        /* queued files from /Card: track -2 - k */
static int card_n;
#include "test_paths.h"   /* tpath, tsplit: a fake song's path as folder and name */
/* song t's path (or a fake song's, by pointer) for a test to read; a few at once */
static char th_pbuf[8][MAX_PATH]; static int th_pk;
static const char *TP(int t) { th_pk=(th_pk+1)&7; return sl_track_path(t,th_pbuf[th_pk],sizeof th_pbuf[th_pk]); }
static const char *TPT(const struct sl_track *t) { th_pk=(th_pk+1)&7;
    snprintf(th_pbuf[th_pk],sizeof th_pbuf[th_pk],"%s%s",t->file?th_dirs[t->dir]:"",t->file?t->file:""); return th_pbuf[th_pk]; }
static int queue_tracks[10002];
static bool queue_flags[10002];
static bool rebuild_result = true;
static struct mp3entry test_track;
static struct bitmap test_bitmap;
static uint32_t test_tick;
static uint16_t pixels[W * H];
static bool test_no_screen;   /* the screen cannot be read: no glass */
static const uint16_t *sl_test_pixels(void) { return test_no_screen ? NULL : pixels; }
static int fake_find(const char *path);
static long fake_size(const char *path);
static bool fake_equals(const char *path, const char *want);
int audio_status(void) { return test_status; }
struct mp3entry *audio_current_track(void) { return test_status ? &test_track : NULL; }
static struct mp3entry test_next;
struct mp3entry *audio_next_track(void) { return test_status ? &test_next : NULL; }
void audio_resume(void) { test_status &= ~AUDIO_STATUS_PAUSE; }
void audio_pause(void) { test_status |= AUDIO_STATUS_PAUSE; }
static int audio_stops;
void audio_stop(void) { test_status = 0; audio_stops++; }
static int audio_nexts, audio_prevs;
void audio_next(void) { audio_nexts++; }
void audio_prev(void) { audio_prevs++; }
void audio_pre_ff_rewind(void) {}
static int audio_seeks;
void audio_ff_rewind(long p) { test_track.elapsed = p; audio_seeks++; }
void audio_flush_and_reload_tracks(void) {}
static struct playlist_info test_pl = { .last_insert_pos = -1 };
struct playlist_info *playlist_get_current(void) { return &test_pl; }
static bool create_fail;
int playlist_create(const char *d, const char *f)
{ (void)d; (void)f; queued = 0; test_pl.last_insert_pos = -1; return create_fail ? -1 : 0; }
int playlist_insert_track(struct playlist_info *p, const char *f, int pos, bool q, bool sync) {
    (void)p;(void)sync;
    if (queued == insert_fail || queued >= 10002) return -1;
    int track = sl_lib_find_path(f);
    if (track < 0 && fake_card(f)) { track = -2 - card_n % 256; strlcpy(card_paths[card_n++ % 256], f, sizeof card_paths[0]); }   /* a file on the sandboxed card, not in the library */
    if (track == -1) return -1;
    /* Rockbox: PLAYLIST_INSERT goes after last_insert_pos, else after the
     * playing track; INSERT_FIRST after the playing track; the rest append */
    int lip = test_pl.last_insert_pos;
    int index = pos == PLAYLIST_INSERT && lip >= 0 && lip < queued ? lip + 1 :
                (pos == PLAYLIST_INSERT || pos == PLAYLIST_INSERT_FIRST) && started >= 0 ? started + 1 : queued;
    if (index > queued) index = queued;   /* Rockbox's playing index is always in the list */
    test_pl.last_insert_pos = index;
    memmove(&queue_tracks[index+1], &queue_tracks[index], (queued-index)*sizeof(int));
    memmove(&queue_flags[index+1], &queue_flags[index], (queued-index)*sizeof(bool));
    queue_tracks[index] = track; queue_flags[index] = q; queued++;
    return index;
}
int playlist_insert_playlist(struct playlist_info *p, const char *f, int pos, bool q) {
    (void)p;(void)f;(void)pos;(void)q; queued=2; return 0;
}
int playlist_insert_playlist_ex(struct playlist_info *p, const char *f, int pos, bool q, bool progress) {
    /* the Rockbox "Inserted N tracks" splash would draw over GabagoolOS */
    if (progress) { fprintf(stderr, "FAIL: saved playlist insert asked for the Rockbox splash\n"); exit(1); }
    return playlist_insert_playlist(p, f, pos, q);
}
void playlist_sync(struct playlist_info *p) { (void)p; }
int playlist_shuffle(int seed, int start) { (void)seed; return start; }
static unsigned long last_start_elapsed, last_start_offset;
void playlist_start(int i, unsigned long e, unsigned long o) {last_start_elapsed=e;last_start_offset=o; started = i;test_status = AUDIO_STATUS_PLAY;}
int playlist_resume(void) { return -1; }
static int status_saves;
void status_save(bool force) { (void)force; status_saves++; }
void playlist_resume_track(int i, unsigned int c, unsigned long e, unsigned long o) {(void)i;(void)c;(void)e;(void)o;}
int playlist_randomise(struct playlist_info *p, unsigned int seed, bool start) {(void)p;(void)seed;(void)start;return 0;}
int playlist_sort(struct playlist_info *p, bool start) {(void)p;(void)start;return 0;}
int playlist_amount(void) { return queued; }
int playlist_add(const char *f) { (void)f; queued++; return 0; }
const char *sim_root_dir = "/tmp/sloppy-ui-test-root";   /* sloppy_net.c's card root */
volatile bool sim_card_away;   /* USB mode's handover, as usb_card_away() says on the R1 */
#include "../../../firmware/target/hosted/card-guard.h"   /* the R1's file layer's very check */
#define SIM_CARD_GUARD(p) do { if (card_guard_refuses((p), sim_card_away, "")) { errno = EROFS; return -1; } } while (0)
static int test_sound_setting = -1, test_sound_value = -1;
void sound_set(int s, int v) { test_sound_setting = s; test_sound_value = v; }
static bool test_dither;
void dsp_dither_enable(bool on) { test_dither = on; }
const char *get_codec_string(int type) { (void)type; return "FLAC"; }
void replaygain_update(void) {}
void dsp_set_crossfeed_type(int t) { (void)t; }
static int32_t test_speed = PITCH_SPEED_100;
void dsp_timestretch_enable(bool b) { (void)b; }
void dsp_set_timestretch(int32_t p) { test_speed = p; }
int32_t dsp_get_timestretch(void) { return test_speed; }
void set_poweroff_timeout(int t) { (void)t; }
static int test_wake_swallows = -1;   /* Clicker turns it off while open */
void button_set_wake_swallows(bool b) { test_wake_swallows = b; }
int playlist_get_display_index(void) { return started + 1; }
static int settings_reset_calls, settings_apply_calls, settings_skins_calls, settings_save_calls;
int settings_save(void) { settings_save_calls++; return 0; }
void settings_reset(void) {
    settings_reset_calls++;
    global_settings.sloppy_theme=0;
    global_settings.sloppy_home_style=0;
    global_settings.sloppy_accent_color=0x72d8cd;
    global_settings.sloppy_home_wallpaper=0;
    global_settings.sloppy_lock_wallpaper=0;
    global_settings.sloppy_skull_icon=false;
    global_settings.sloppy_sfx_volume=70;global_settings.sloppy_ui_sounds=1;global_settings.sloppy_volume_step=1;
}
void settings_apply(bool read_disk) { assert(read_disk); settings_apply_calls++; }
void settings_apply_skins(void) { settings_skins_calls++; }
void setvol(void);
void adjust_volume(int v) { global_status.volume += v * sound_steps(SOUND_VOLUME); setvol(); }
/* The sound effects channel: render the whole sound through the real
 * generator and keep its length and peak. */
static long mix_frames; static int mix_peak, mix_lr_diff;
static int16_t mix_wav[44100*4*2];
static const char *const sfx_names[SFX_COUNT]={"tap","error","win","lose","draw","start","coin-flip","coin-land","coin-heads","coin-tails","magic-shake","magic-yes","magic-no","magic-maybe","magic-hot","snake-eat","snake-die","spoon-boing","spoon-fall","spoon-perfect","spoon-jelly","spoon-crack","spoon-soggy","spoon-coin","spoon-stomp","spoon-power","spoon-shield","spoon-warn","spoon-phase","spoon-best","spoon-hit","spoon-splash","spoon-secret","spoon-golden","spoon-lost","ragdoll-grab","ragdoll-thud","fm-tap","fm-buy","chess-select","chess-move","chess-capture","chess-check","chess-undo","chess-promote","quiz-question","quiz-right","quiz-streak","quiz-wrong","quiz-timeup","quiz-tick","quiz-pop","quiz-cash","quiz-zap","quiz-applause","pet-chirp","pet-chomp","pet-bubble","pet-lullaby","pet-ekg","pet-beat","pet-calm","pet-wild","pet-poke","pet-farewell","pet-ghost","pet-hatch","pet-done","pet-select","pet-catch","pet-miss","pet-swap","pet-gem","pet-yes","pet-nope","pet-win","pet-lose","pet-rip","vinyl-drop","vinyl-scratch","tape-click","snake-gold","tile-slide","tile-merge","sudoku-place","points-join","flappy-wing","flappy-point","flappy-hit","flappy-die","flappy-swoosh","ui-open","ui-back","ui-on","ui-off","ui-lock","ui-unlock","ui-panel-open","ui-panel-close","ui-notify","ui-pickup","ui-drop","ui-remove","ui-tick","ui-shutter","sys-boot","sys-shutdown","sys-charge","sys-unplug","sys-batt-low"};
/* SFX_WAV_DIR=dir ./ui-test writes every sound as a WAV to listen to. */
static void sfx_write_wav(int id)
{
    const char *dir=getenv("SFX_WAV_DIR");if(!dir)return;
    char path[512];snprintf(path,sizeof path,"%s/%02d-%s.wav",dir,id,sfx_names[id]);
    FILE *f=fopen(path,"wb");assert(f);
    long n=mix_frames*4<(long)sizeof mix_wav?mix_frames*4:(long)sizeof mix_wav;
    uint32_t h[]={0x46464952,36+n,0x45564157,0x20746d66,16,0x00020001,44100,44100*4,0x00100004,0x61746164,n};
    fwrite(h,4,11,f);fwrite(mix_wav,1,n,f);fclose(f);
}
static uint32_t test_peak;
void mixer_channel_calculate_peaks(enum pcm_mixer_channel c, struct pcm_peaks *p) { (void)c; p->left=p->right=test_peak; }
static unsigned int fake_mixer_hz = 44100;
unsigned int mixer_get_frequency(void) { return fake_mixer_hz; }
/* ==== Receive from phone: the voice channel is pulled by hand ==== */
static pcm_play_callback_type test_voice_more;
static unsigned test_voice_amp = MIX_AMP_UNITY;
/* ==== end Receive from phone ==== */
void mixer_channel_stop(enum pcm_mixer_channel c) { if (c==PCM_MIXER_CHAN_VOICE) test_voice_more=NULL; }
void pcm_play_lock(void) {}
void pcm_play_unlock(void) {}
const void *mixer_channel_get_buffer(enum pcm_mixer_channel c, int *count) { (void)c; *count = 0; return NULL; }
static chan_buffer_hook_fn_type test_hook;
void mixer_channel_set_buffer_hook(enum pcm_mixer_channel c, chan_buffer_hook_fn_type fn) { assert(c==PCM_MIXER_CHAN_PLAYBACK); test_hook = fn; }
/* a playback buffer through the hook: sines at f[i] Hz, amplitudes a[i] */
static void test_feed(int n, const float *f, const float *a) {
    static int16_t b[2048*2];static uint32_t t;
    for (int i=0;i<2048;i++,t++){float v=0;for(int k=0;k<n;k++)v+=a[k]*sinf(6.2831853f*f[k]*t/44100);
        b[i*2]=b[i*2+1]=(int16_t)v;}
    if (test_hook) test_hook(b,sizeof b);
}
static enum channel_status test_chan = CHANNEL_PLAYING;
enum channel_status mixer_channel_status(enum pcm_mixer_channel c) { (void)c; return test_chan; }
void mixer_channel_play_pause(enum pcm_mixer_channel c, bool play) { (void)c; test_chan = play ? CHANNEL_PLAYING : CHANNEL_PAUSED; }
static pcmbuf_filter_fn test_filter;
void pcmbuf_set_filter(pcmbuf_filter_fn fn) { test_filter = fn; }
static int test_fades;   /* Your taste: a snippet faded out as its card went */
void pcmbuf_fade(bool fade, bool in) { if (fade && !in) test_fades++; }
static long test_sleep_fade = -2; static bool test_sleep_fade_voice; static int test_sleep_fades;
void pcmbuf_sleep_fade(long ticks, bool voice) { test_sleep_fade = ticks; test_sleep_fade_voice = voice; test_sleep_fades++; }
/* a song for the turntable's filter: frame numbers, both channels */
static uint32_t test_song_frame;
static int16_t test_song[2048 * 2];
static void test_pull(const void **start, size_t *size) {
    for (int i = 0; i < 2048; i++) test_song[i * 2] = test_song[i * 2 + 1] = (int16_t)((test_song_frame + i) & 0x7fff);
    test_song_frame += 2048; *start = test_song; *size = sizeof test_song;
}
void mixer_channel_set_amplitude(enum pcm_mixer_channel c, unsigned int a) { if (c==PCM_MIXER_CHAN_VOICE) test_voice_amp=a; }
void beep_duck(unsigned int duration, int percent) { (void)duration; (void)percent; }
void mixer_channel_play_data(enum pcm_mixer_channel c, pcm_play_callback_type more, const void *start, size_t size) {
    if (c==PCM_MIXER_CHAN_VOICE) { assert(more && start && size); test_voice_more=more; return; }   /* Receive from phone */
    assert(c==PCM_MIXER_CHAN_BEEP);
    mix_frames=0;mix_peak=0;mix_lr_diff=0;
    for(int guard=0;start && size && guard<10000;guard++){
        const int16_t *p=start;
        for(size_t i=0;i<size/2;i++){int v=abs(p[i]);if(v>mix_peak)mix_peak=v;
            if(i&1){int d=abs(p[i]-p[i-1]);if(d>mix_lr_diff)mix_lr_diff=d;}}
        for(size_t i=0;i<size/2 && mix_frames*2+i<sizeof mix_wav/2;i++)mix_wav[mix_frames*2+i]=p[i];
        mix_frames+=size/4;
        size=0;if(more)more(&start,&size);
    }
}
#define SFX_RESET() (sfx_playing_until=0,sfx_last=-1)
/* the R1's scale (hibylinux_codec.h): -70 to -10 dB in half-dB steps */
/* the R1's ranges (firmware/export/hibylinux_codec.h, dsp): volume -70..-10 dB,
 * balance -100..100, stereo width 0..250 % */
int sound_min(int s) { return s==SOUND_BASS||s==SOUND_TREBLE?-24:s==SOUND_BALANCE?-100:s==SOUND_STEREO_WIDTH?0:-700; }
int sound_max(int s) { return s==SOUND_BASS||s==SOUND_TREBLE?24:s==SOUND_BALANCE?100:s==SOUND_STEREO_WIDTH?250:-100; }
int sound_steps(int s) { (void)s;return 5; }
int sound_numdecimals(int s) { (void)s;return 1; }
void backlight_set_brightness(int v) { (void)v; }
static int test_lcd_sleep = -100;
static int test_aa_used = 1;           /* GabagoolOS holds the Now Playing art slot */
int playback_claim_aa_slot(struct dim *d) { (void)d; test_aa_used++; return 0; }
void playback_release_aa_slot(int slot) { (void)slot; test_aa_used--; }
void lcd_set_sleep_after_backlight_off(int s) { test_lcd_sleep = s; }
void backlight_set_dim_brightness(int v) { (void)v; }
void backlight_set_fade_in(bool v) { (void)v; }
void backlight_set_off_timeout(int v) { (void)v; }
void backlight_set_timeout(int v) { (void)v; }
void backlight_set_timeout_plugged(int v) { (void)v; }
void touchscreen_set_mode(enum touchscreen_mode mode) { (void)mode; }
int plugin_load(const char *p, const void *a) { (void)p; (void)a; return 0; }
#include <unistd.h>
extern char sl_net_demo_url[512];        /* sloppy_net.c, NET_DEMO: the last address fetched */
static int fake_find(const char *path);
bool sim_file_exists(const char *file) { return fake_card(file) ? fake_find(file)>=0 && !fake_is_dir(file) : access(file, F_OK) == 0; }   /* file_exists in the sim */
bool sim_dir_exists(const char *dir) { return fake_card(dir) ? fake_is_dir(dir) : false; }
int button_status(void) { return BUTTON_NONE; }
static int test_events;
static int test_lib_frees, test_lib_begins, test_lib_begin_state = SL_LIB_READY, test_usb_lib_frees = -1, test_usb_dirs_open = -1;
static int test_usb_fds = -1, test_usb_screens;
static int test_open_fds(void) { int n = 0; for (int fd = 0; fd < 1024; fd++) if (fcntl(fd, F_GETFD) != -1) n++; return n; }
long default_event_handler(long event) {
    test_events++; if (event == SYS_PHONE_UNPLUGGED) audio_pause();
    if (event == SYS_USB_CONNECTED) { test_usb_lib_frees = test_lib_frees; test_usb_dirs_open = fake_dirs_open; test_usb_fds = test_open_fds(); test_usb_screens++; }   /* what had let go when Rockbox took the card */
    return event;
}
struct viewport *lcd_set_viewport(struct viewport *vp) { return vp; }
static int poweroffs;
void sys_poweroff(void) { poweroffs++; }
void viewportmanager_theme_enable(enum screen_type screen, bool enable, struct viewport *viewport)
{ (void)screen; (void)enable; (void)viewport; }
bool tagcache_rebuild(void) { return rebuild_result; }
static struct tagcache_stat tc_stat_stub;
struct tagcache_stat *tagcache_get_stat(void) { return &tc_stat_stub; }
int tagcache_get_max_commit_step(void) { return 8; }
bool sl_lib_keep_apart;
/* the tag database's update (sloppy_dbupdate.inc's change check) and its exclude list */
static int test_updates;
static int32_t test_db_key[6] = { 1, 2, 3, 4, 5, 6 };
static int test_cache_drops;
volatile int tagcache_updates_done, tagcache_entries_deleted;
bool tagcache_update(void) { test_updates++; return false; }
bool sl_lib_db_key(int32_t key[6]) { memcpy(key, test_db_key, sizeof test_db_key); return true; }
void sl_lib_cache_drop(void) { test_cache_drops++; }
bool tagcache_folder_listed(const char *path) {
    size_t n = strlen(path);
    for (const char *p = global_settings.db_exclude_folders; *p; ) {
        const char *e = strchr(p, ':'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncasecmp(p, path, n)) return true;
        if (!e) break; p = e + 1; }
    return false;
}
bool tagcache_set_folder_excluded(const char *path, bool exclude) {
    char out[sizeof global_settings.db_exclude_folders] = ""; size_t n = strlen(path);
    for (const char *p = global_settings.db_exclude_folders; *p; ) {
        const char *e = strchr(p, ':'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && !(l == n && !strncasecmp(p, path, n))) { if (out[0]) strlcat(out, ":", sizeof out); strncat(out, p, l); }
        if (!e) break; p = e + 1; }
    if (exclude) { if (strlen(out) + n + 2 > sizeof out) return false; if (out[0]) strlcat(out, ":", sizeof out); strlcat(out, path, sizeof out); }
    strlcpy(global_settings.db_exclude_folders, out, sizeof global_settings.db_exclude_folders);
    return true;
}
int sl_lib_begin(void) { test_lib_begins++; return test_lib_begin_state; }   /* the harness fills sl_lib itself */
static bool test_lib_scanning = true;   /* false: the read is library.bin's (no banner) */
bool sl_lib_scanning(void) { return test_lib_scanning; }
void sl_lib_free(void) { test_lib_frees++; }   /* the real one closes the tag database (library_test.c) */
void sl_art_reset(void) {}
static int test_prefer[64], test_n_prefer;
void sl_art_prefer(int album) { if (test_n_prefer < 64) test_prefer[test_n_prefer++] = album; }
void sl_art_invalidate_cache(void) {}
/* the Files viewer's picture: no decoder here; the bytes' first one picks
 * what happens (0xff: too big, 0x00: unreadable, else a 1600 x 1200 photo
 * of a dusk sky, fitted) */
static int test_pic_polls;
static unsigned char test_pic_kind;
static int test_pic_max_w, test_pic_max_h;
int sl_pic_start(unsigned char *b, int len, int mw, int mh) {
    unsigned char k = b && len > 0 ? b[0] : 0; free(b);
    if (k == 0xff) return SL_PIC_TOO_BIG;
    if (k == 0x00) return SL_PIC_UNREADABLE;
    test_pic_kind = k; test_pic_max_w = mw; test_pic_max_h = mh; test_pic_polls = 0; return SL_PIC_STARTED;
}
int sl_pic_poll(uint16_t **px, int *w, int *h, int *iw, int *ih) {
    if (!test_pic_max_w) return -1;
    if (++test_pic_polls < 3) return 0;   /* a few frames of decoding */
    int ow = test_pic_max_w, oh = ow * 3 / 4; if (oh > test_pic_max_h) { oh = test_pic_max_h; ow = oh * 4 / 3; }
    uint16_t *p = malloc((size_t)ow * oh * 2);
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
        int r = 40 + 200 * y / oh, g = 60 + 80 * x / ow, bl = 160 - 120 * y / oh;
        if ((x - ow * 3 / 4) * (x - ow * 3 / 4) + (y - oh * 2 / 3) * (y - oh * 2 / 3) < oh * oh / 36) { r = 255; g = 214; bl = 120; }
        p[y * ow + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
    }
    *px = p; *w = ow; *h = oh; *iw = 1600; *ih = 1200; test_pic_max_w = 0; return 1;
}
/* a picture made a wallpaper: no decoder here either; a dusk gradient with
 * a sun, written raw where it is asked for, after a few polls */
static int test_wall_polls = -1; static char test_wall_in[256];
int sl_wall_start(const char *path, const char *out, int tw, int th) {
    if (test_wall_polls >= 0) return SL_PIC_BUSY;
    if (!path || !path[0] || strstr(path, "broken")) return SL_PIC_UNREADABLE;
    strlcpy(test_wall_in, path, sizeof test_wall_in);
    uint16_t *p = malloc((size_t)tw * th * 2);
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
        int r = 30 + 150 * y / th, g = 40 + 60 * x / tw, bl = 140 - 100 * y / th;
        if ((x - tw / 3) * (x - tw / 3) + (y - th / 3) * (y - th / 3) < tw * tw / 49) { r = 230; g = 190; bl = 110; }
        p[y * tw + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
    }
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666); assert(fd >= 0);
    assert(write(fd, p, (size_t)tw * th * 2) == (ssize_t)((size_t)tw * th * 2)); sim_close(fd); free(p);   /* close is POSIX here */
    test_wall_polls = 0; return SL_PIC_STARTED;
}
int sl_wall_poll(void) { if (test_wall_polls < 0) return -1; if (++test_wall_polls < 3) return 0; test_wall_polls = -1; return 1; }
/* a boot logo fitted (no decoder here): 300 x 150, orange on the left, cream
 * on the right, with its head, as sloppy_art.c writes it; polled as above */
int sl_bootlogo_start(const char *path, const char *out, int tw, int th) {
    if (test_wall_polls >= 0) return SL_PIC_BUSY;
    if (!path || !path[0] || strstr(path, "broken") || tw < 300 || th < 150) return SL_PIC_UNREADABLE;
    strlcpy(test_wall_in, path, sizeof test_wall_in);
    static uint16_t p[300 * 150];
    for (int y = 0; y < 150; y++) for (int x = 0; x < 300; x++) p[y * 300 + x] = x < 150 ? 0xfb40 : 0xff9b;
    unsigned char hd[8] = { 'S', 'L', 'B', 'L', 300 & 255, 300 >> 8, 150, 0 };
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666); assert(fd >= 0);
    assert(write(fd, hd, 8) == 8 && write(fd, p, sizeof p) == (ssize_t)sizeof p); sim_close(fd);
    test_wall_polls = 0; return SL_PIC_STARTED;
}
void sl_art_release(void) {}
/* the podcast worker's pictures: stb_image, as sloppy_art.c decodes them on the R1 */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "../../../lib/stb/stb_image.h"
#pragma clang diagnostic pop
static int test_pod_rgb_loads;
unsigned char *sl_art_rgb_load(const unsigned char *b, int len, int *w, int *h) { int c; test_pod_rgb_loads++; return stbi_load_from_memory(b, len, w, h, &c, 3); }
void sl_art_rgb_free(unsigned char *rgb) { stbi_image_free(rgb); }
static bool test_art;   /* covers: each album a colour of its own */
static uint16_t test_art_px(int album) { return (uint16_t)(0x0841u * (unsigned)(album % 30 + 1)); }
/* SLOPPY_REAL_ART (or test_real_art): covers that look like covers, so Now
 * Playing, the album page and the lyrics are judged on real artwork, not a
 * flat square or the record placeholder. Six kinds, by album: a sunset, bold
 * stripes, a dark sleeve with one thin ring, a pale plain one, a busy
 * pattern, and soft blobs; each in the album's own hue. */
static bool test_real_art;
static int test_cover_hue(int album) { return album >= 0 && album < sl_lib.n_albums ? sl_lib.albums[album].hue % 360 : (album * 53) % 360; }
static void test_hsv(int h, int s, int v, int *r, int *g, int *b)
{
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)(((h % 360) + 360) % 360), (uint8_t)s, (uint8_t)v);
    *r = c.red; *g = c.green; *b = c.blue;
}
/* SLOPPY_SHOWCASE=<dir>: real sleeves for the front page's shots, <dir>/NN.jpg
 * by album (batch_showcase.inc loads them); NULL: the made-up ones */
static struct { unsigned char *rgb; int w, h, hue, sat; } sc_art[10];
static unsigned char *sc_pod_jpg, *sc_sub_jpg;   /* the podcast's and the music server's covers, as served */
static size_t sc_pod_n, sc_sub_n;
static bool sc_on;   /* names too: the fakes' podcast and music server read as real ones */
static bool sc_cover(int album, int size, uint16_t *dst)
{
    if (album < 0 || album >= 10 || !sc_art[album].rgb) return false;
    int w = sc_art[album].w, h = sc_art[album].h, s = w < h ? w : h, x0 = (w - s) / 2, y0 = (h - s) / 2;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
        {   /* the square's middle, each pixel the mean of the ones it covers */
            int sx0 = x * s / size, sx1 = (x + 1) * s / size, sy0 = y * s / size, sy1 = (y + 1) * s / size;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++)
                {
                    const unsigned char *p = sc_art[album].rgb + ((size_t)(y0 + yy) * w + x0 + xx) * 3;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            r /= n; g /= n; b /= n;
            dst[y * size + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    return true;
}
static void test_cover(int album, int size, uint16_t *dst)
{
    if (sc_cover(album, size, dst)) return;
    int kind = (album < 0 ? 0 : album) % 6, hue = test_cover_hue(album);
    int a[3], b[3], c[3];
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
        {
            float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            int r, g, bl;
            switch (kind)
            {
            case 0: {   /* sunset: sky to a deep horizon, a sun, the sea dark under it */
                test_hsv(hue + 20, 70, 95, &a[0], &a[1], &a[2]); test_hsv(hue - 40, 80, 45, &b[0], &b[1], &b[2]);
                test_hsv(hue + 40, 30, 100, &c[0], &c[1], &c[2]);
                float t = v < 0.62f ? v / 0.62f : 1.0f;
                r = a[0] + (int)((b[0] - a[0]) * t); g = a[1] + (int)((b[1] - a[1]) * t); bl = a[2] + (int)((b[2] - a[2]) * t);
                float dx = u - 0.5f, dy = v - 0.5f, d = sqrtf(dx * dx + dy * dy);
                if (v < 0.62f && d < 0.18f) { r = c[0]; g = c[1]; bl = c[2]; }
                if (v >= 0.62f) { float k = 0.35f + 0.1f * sinf(u * 40 + v * 90); r = (int)(b[0] * k); g = (int)(b[1] * k); bl = (int)(b[2] * k); }
                break; }
            case 1: {   /* bold diagonal stripes */
                test_hsv(hue, 85, 90, &a[0], &a[1], &a[2]); test_hsv(hue + 180, 60, 30, &b[0], &b[1], &b[2]);
                int s = (int)((u + v) * 7) & 1;
                r = s ? a[0] : b[0]; g = s ? a[1] : b[1]; bl = s ? a[2] : b[2];
                break; }
            case 2: {   /* a dark sleeve, one thin ring of colour */
                test_hsv(hue, 85, 90, &a[0], &a[1], &a[2]);
                float dx = u - 0.5f, dy = v - 0.5f, d = sqrtf(dx * dx + dy * dy);
                r = g = bl = 14 + (int)(10 * v);
                if (fabsf(d - 0.26f) < 0.012f) { r = a[0]; g = a[1]; bl = a[2]; }
                break; }
            case 3: {   /* pale and plain: off-white, a small dark block and two lines of "type" */
                test_hsv(hue, 10, 96, &a[0], &a[1], &a[2]); test_hsv(hue, 60, 25, &b[0], &b[1], &b[2]);
                r = a[0]; g = a[1]; bl = a[2];
                bool blk = u > 0.12f && u < 0.30f && v > 0.12f && v < 0.30f;
                bool line = u > 0.12f && ((v > 0.78f && v < 0.80f && u < 0.7f) || (v > 0.83f && v < 0.845f && u < 0.5f));
                if (blk || line) { r = b[0]; g = b[1]; bl = b[2]; }
                break; }
            case 4: {   /* busy: rings and a checker, detail everywhere */
                test_hsv(hue, 75, 85, &a[0], &a[1], &a[2]); test_hsv(hue + 120, 70, 70, &b[0], &b[1], &b[2]);
                test_hsv(hue + 240, 50, 95, &c[0], &c[1], &c[2]);
                float dx = u - 0.3f, dy = v - 0.35f, d = sqrtf(dx * dx + dy * dy);
                int ring = (int)(d * 30) % 3, chk = ((int)(u * 16) + (int)(v * 16)) & 1;
                int *p = ring == 0 ? a : ring == 1 ? b : c;
                r = chk ? p[0] : p[0] * 3 / 4; g = chk ? p[1] : p[1] * 3 / 4; bl = chk ? p[2] : p[2] * 3 / 4;
                break; }
            default: {  /* soft blobs over a mid colour */
                test_hsv(hue, 45, 55, &a[0], &a[1], &a[2]);
                float fr = a[0], fg = a[1], fb = a[2];
                static const float bx[3] = { 0.25f, 0.7f, 0.55f }, by[3] = { 0.3f, 0.35f, 0.8f }, br[3] = { 0.28f, 0.22f, 0.3f };
                for (int k = 0; k < 3; k++)
                {
                    float dx = u - bx[k], dy = v - by[k], w = 1.0f - (dx * dx + dy * dy) / (br[k] * br[k]);
                    if (w <= 0) continue;
                    w = w * w;
                    test_hsv(hue + 60 * (k + 1), 70, 95, &b[0], &b[1], &b[2]);
                    fr += (b[0] - fr) * w; fg += (b[1] - fg) * w; fb += (b[2] - fb) * w;
                }
                r = (int)fr; g = (int)fg; bl = (int)fb;
                break; }
            }
            r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; bl = bl < 0 ? 0 : bl > 255 ? 255 : bl;
            dst[y * size + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (bl >> 3));
        }
}
bool sl_art_load_square(int album, int size, uint16_t *dst)
{
    if ((!test_art && !test_real_art) || album < 0) return false;
    if (test_real_art) { test_cover(album, size, dst); return true; }
    for (int i = 0; i < size * size; i++) dst[i] = test_art_px(album);
    return true;
}
/* the Big cover: made after a few asks, as the cover thread would (real-looking covers only) */
static int test_big_album = -1, test_big_asks;
void sl_art_big_start(int album, int size) { (void)size; test_big_album = album; test_big_asks = 0; }
int sl_art_big_poll(int album, uint16_t **px, int *w, int *h) {
    if (album != test_big_album || !test_real_art) return -1;
    if (++test_big_asks < 3) return 0;
    int s = 452; *px = malloc((size_t)s * s * 2); if (!*px) return -1;
    test_cover(album, s, *px); *w = *h = s; test_big_album = -1; return 1; }
size_t lv_image_cache_get_used_size(void) { return 0; }
void system_reboot(void) {}
/* a tag database with three genres: track i has genre i % 3 */
static bool tc_usable = true;
static const char *const tc_genres[] = { "Hard Rock", "Pop", "<Untagged>" };
bool tagcache_is_usable(void) { return tc_usable; }
bool tagcache_search(struct tagcache_search *t, int tag) { memset(t,0,sizeof *t); t->type=tag; t->valid=true; return tc_usable; }
bool tagcache_search_add_filter(struct tagcache_search *t, int tag, int seek) { (void)tag; t->filter_seek[t->filter_count++]=seek; return true; }
bool tagcache_get_next(struct tagcache_search *t, char *buf, long size) {
    if (t->type==tag_genre) {
        if (t->list_position>=3) return false;
        strlcpy(buf,tc_genres[t->list_position],size); t->result_seek=t->list_position++; return true;
    }
    while (t->list_position<sl_lib.n_tracks) {
        int i=t->list_position++;
        if (i%3==t->filter_seek[0]) { sl_track_path(i,buf,size); return true; }
    }
    return false;
}
void tagcache_search_finish(struct tagcache_search *t) { t->valid=false; }
void yield(void) {}
int battery_level(void) { return 83; }
static int test_battery_minutes = 600;   /* the estimate the sleep timer checks against */
int battery_time(void) { return test_battery_minutes; }
int battery_voltage(void) { return 4012; }
static bool test_charging;
bool charger_inserted(void) { return test_charging; }
struct tm *get_time(void) { static struct tm t = {.tm_year=126,.tm_mon=8,.tm_mday=23,.tm_hour=9,.tm_min=41};return &t; }
static long rtc_added;   /* the clock moved on by the tests, seconds (within the day) */
static void rtc_add(long s) { struct tm *t=get_time(); long v=t->tm_hour*3600L+t->tm_min*60+t->tm_sec+s; rtc_added+=s;
    t->tm_hour=(int)(v/3600%24); t->tm_min=(int)(v/60%60); t->tm_sec=(int)(v%60); }
int playback_current_aa_hid(int slot) { (void)slot;return 1; }
ssize_t bufgetdata(int h, size_t n, void **out) {(void)h;(void)n;*out=&test_bitmap;return sizeof(test_bitmap);}
static uint32_t ticks(void) { return test_tick; }
static long flushed_px;   /* pixels sent to the panel, for the idle benchmark */
static uint64_t flush_ns;  /* CPU time in the flush, for SLOPPY_PERF */
static void render(lv_display_t *d, const lv_area_t *area, uint8_t *map) {
    struct timespec f0,f1; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&f0);
    lv_area_t rotated=*area; const lv_area_t *a=&rotated;
    flushed_px+=(long)lv_area_get_width(area)*lv_area_get_height(area);
    sl_flushed_px+=(unsigned long)lv_area_get_width(area)*lv_area_get_height(area);   /* as flush_cb counts */
    const uint8_t *buf=sl_flush_portrait(d,&rotated,map);
    for (int y=a->y1; y<=a->y2; y++)
        memcpy(&pixels[y*W+a->x1],buf+(y-a->y1)*(a->x2-a->x1+1)*2,(a->x2-a->x1+1)*2);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&f1); flush_ns+=(f1.tv_sec-f0.tv_sec)*1000000000ull+(f1.tv_nsec-f0.tv_nsec);
    lv_display_flush_ready(d);
}
static void frame(unsigned ms) {test_tick += ms;lv_timer_handler();lv_refr_now(disp);}
static void shot_now(const char *name) {
    FILE *f=fopen(name,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",W,H);
    for (int i=0;i<W*H;i++) {unsigned p=pixels[i];unsigned char rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};fwrite(rgb,1,3,f);}fclose(f);
}
static void shot(const char *name) {frame(500);shot_now(name);}
/* Some label under o reads s. */
static bool has_text(lv_obj_t *o, const char *s) {
    if (lv_obj_check_type(o, &lv_label_class) && strstr(lv_label_get_text(o), s)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) if (has_text(lv_obj_get_child(o, i), s)) return true;
    return false;
}

/* SLOPPY_PERF: per-frame cost of real touch-driven scenarios. Drawing time
 * on the Mac is scaled to the R1 with PF_K (device/Mac ratio measured from
 * the same code: glass capture 50 ms vs ~1.0 ms, Cover Flow render 24 ms vs
 * ~0.35 ms). The flush is costed per pixel from the R1's own Cover Flow log
 * (rotate 3.5 + lcd_bitmap 3.6 + copy 1.7 ms for a 297600 px area), and the
 * frame is rounded up to whole 60 Hz vsyncs, as the R1's flip waits. */
#define PF_K 60.0
#define PF_MAX 4096
static struct { const char *name; int n; uint32_t us[PF_MAX], flush_us[PF_MAX]; long px[PF_MAX]; } pf;
static void pf_begin(const char *name) { pf.name=name; pf.n=0; }
static int pf_feed;   /* SLOPPY_PERF_SPECTRUM: playback buffers keep coming, one per 46 ms */
static void pf_frame(unsigned ms) {
    if (pf_feed && pf_feed++ % 3 == 0) {
        static const float f[]={55,110,220,330,660,1300,2600,6000,9500};
        float a[]={pf_feed%18<6?9000:1500,5000,3500,2500,1800,1100,700,380,240};
        test_feed(9,f,a);
    }
    long px0=flushed_px; uint64_t fl0=flush_ns; struct timespec a,b; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);   /* CPU time: other apps don't count */
    test_tick+=ms; lv_timer_handler();   /* as the R1's loop: no extra lv_refr_now, which drew a release twice */
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
    if (pf.n<PF_MAX) { pf.us[pf.n]=(uint32_t)((b.tv_sec-a.tv_sec)*1000000+(b.tv_nsec-a.tv_nsec)/1000); pf.px[pf.n]=flushed_px-px0; pf.flush_us[pf.n]=(uint32_t)((flush_ns-fl0)/1000); pf.n++; }
}
static int pf_cmp(const void *a, const void *b) { double x=*(const double*)a,y=*(const double*)b; return x<y?-1:x>y; }
static void pf_end(void) {
    static double dev[PF_MAX]; int n=0, over=0; double sum=0, raw=0, worst_run=0, run=0; long px=0;
    for (int i=0;i<pf.n;i++) {
        if (!pf.px[i]) continue;        /* nothing drawn: no frame */
        double ms=(pf.us[i]-pf.flush_us[i])*PF_K/1000.0 + pf.px[i]*(3.5+3.6+1.7)/297600.0;
        raw+=ms;ms=ceil(ms/16.667)*16.667;
        dev[n++]=ms; sum+=ms; px+=pf.px[i];
        if (ms>34) { over++; run+=ms; if (run>worst_run) worst_run=run; } else run=0;
    }
    if (getenv("SLOPPY_PERF_FRAMES"))   /* every frame: R1 estimate and pixels */
        for (int i=0;i<pf.n&&i<120;i++) printf("  %s %3d %6.1f ms %7ld px\n",pf.name,i,(pf.us[i]-pf.flush_us[i])*PF_K/1000.0+pf.px[i]*(3.5+3.6+1.7)/297600.0,pf.px[i]);
    if (!n) { printf("perf %-14s no frames\n",pf.name); return; }
    qsort(dev,n,sizeof dev[0],pf_cmp);
    printf("perf %-14s %4d frames  R1 est ms: work %5.1f avg %5.1f p50 %5.1f p95 %5.1f p99 %5.1f max %5.1f  >33ms %3d (%2.0f%%)  %4.2f screens/frame\n",
           pf.name,n,raw/n,sum/n,dev[n/2],dev[(n*95)/100],dev[(n*99)/100],dev[n-1],over,100.0*over/n,px/(double)n/(W*H));
}

static int first_index;
int playlist_get_resume_info(int *i) { *i = started; return 0; }
int playlist_get_first_index(const struct playlist_info *p) { (void)p; return first_index; }
static long track_info_reads;   /* file-name reads: each is card I/O on the device */
int playlist_get_track_info(struct playlist_info *p, int i, struct playlist_track_info *out) {
    (void)p; if (i < 0 || i >= queued) return -1;
    strlcpy(out->filename, queue_tracks[i] <= -2 ? card_paths[-2 - queue_tracks[i]] : TP(queue_tracks[i]), sizeof out->filename);
    out->attr = queue_flags[i] ? PLAYLIST_ATTR_QUEUED : 0; track_info_reads++; return 0;
}
int playlist_get_track_attr(struct playlist_info *p, int i) {
    (void)p; if (i < 0 || i >= queued) return -1;
    return queue_flags[i] ? PLAYLIST_ATTR_QUEUED : 0;
}
int playlist_delete(struct playlist_info *p, int i) {
    (void)p; assert(i >= 0 && i < queued);
    memmove(&queue_tracks[i], &queue_tracks[i+1], (queued-i-1)*sizeof(int));
    memmove(&queue_flags[i], &queue_flags[i+1], (queued-i-1)*sizeof(bool));
    if (i < started) started--;
    if (i <= test_pl.last_insert_pos) test_pl.last_insert_pos--;
    queued--; return 0;
}
static int playlist_moves;
int playlist_move(struct playlist_info *p, int from, int to) {
    /* Rockbox's move with first_index 0: to a later place it lands after
     * the song at 'to', to an earlier one in its place */
    (void)p; if (from == to || from < 0 || to < 0 || from >= queued || to >= queued) return -1;
    int t=queue_tracks[from]; bool f=queue_flags[from], cur=from==started;
    memmove(&queue_tracks[from], &queue_tracks[from+1], (queued-from-1)*sizeof(int));
    memmove(&queue_flags[from], &queue_flags[from+1], (queued-from-1)*sizeof(bool));
    if (from < started) started--;
    memmove(&queue_tracks[to+1], &queue_tracks[to], (queued-1-to)*sizeof(int));
    memmove(&queue_flags[to+1], &queue_flags[to], (queued-1-to)*sizeof(bool));
    queue_tracks[to]=t; queue_flags[to]=f;
    if (cur) started=to; else if (to <= started) started++;
    playlist_moves++; return 0;
}
void audio_skip(int n, int t) {(void)t;started+=n;}
void audio_set_crossfade(int n) {(void)n;}
void dsp_set_eq_coefs(int b, const struct eq_band_setting *s);
void dsp_set_eq_precut(int n);
static struct eq_band_setting test_eq_dsp[EQ_NUM_BANDS];static int test_eq_precut;static bool test_eq_on;static int test_pbe=-1;
void dsp_eq_enable(bool b) {test_eq_on=b;}
void dsp_pbe_enable(int v) {test_pbe=v;}
void set_sleeptimer_duration(int n) {(void)n;}
void dsp_set_eq_coefs(int b, const struct eq_band_setting *s) {if(b>=0&&b<EQ_NUM_BANDS)test_eq_dsp[b]=*s;}
void dsp_set_eq_precut(int n) {test_eq_precut=n;}
/* as apps/misc.c: a line without its \r\n; the bytes used, 0 at the end */
ssize_t sim_read(int fd,void *b,size_t n);
int read_line(int fd, char *b, int n) {
    int count=0,used=0;char c;
    if(!b||n--<=0)return -1;
    while(count<n&&sim_read(fd,&c,1)==1){used++;if(c=='\n')break;if(c!='\r')b[count++]=c;}
    b[count]='\0';return used;
}
/* as apps/misc.c: the clamp is kept */
static int setvol_calls;
void setvol(void) {
    int v=global_status.volume;
    if(v<sound_min(SOUND_VOLUME))v=sound_min(SOUND_VOLUME);
    if(v>sound_max(SOUND_VOLUME))v=sound_max(SOUND_VOLUME);
    if(v>global_settings.volume_limit)v=global_settings.volume_limit;
    global_status.volume=v;setvol_calls++;
}
void backlight_on_ignore(bool b, int t) {(void)b;(void)t;}
static bool test_lcd_on = true;
void lcd_enable(bool on) {test_lcd_on=on;}
void backlight_off(void) {test_lcd_on=false;}
void backlight_wake(void) {}
/* with real art: each album's cover at each size made once and kept, shown
 * as sloppy_art.c's attach does (an image in the box, its corners clipped) */
static const void *test_cover_dsc(int album, int size)
{
    static struct { int album, size; lv_image_dsc_t *d; } made[256];
    static int n;
    for (int i = 0; i < n; i++) if (made[i].album == album && made[i].size == size) return made[i].d;
    if (n >= 256 || size <= 0 || size > 480) return NULL;
    lv_image_dsc_t *d = calloc(1, sizeof *d + (size_t)size * size * 2);
    if (!d) return NULL;
    test_cover(album, size, (uint16_t *)(d + 1));
    d->header.magic = LV_IMAGE_HEADER_MAGIC; d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = size; d->header.h = size; d->header.stride = size * 2;
    d->data = (const uint8_t *)(d + 1); d->data_size = (uint32_t)size * size * 2;
    made[n].album = album; made[n].size = size; made[n].d = d; n++;
    return d;
}
static bool test_cover_attach(lv_obj_t *c, int a, int size)
{
    if (!test_real_art || a < 0 || a >= sl_lib.n_albums) return false;
    const void *d = test_cover_dsc(a, size);
    if (!d) return false;
    lv_obj_t *img = lv_image_create(c);
    lv_image_set_src(img, d);
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(img, lv_obj_get_style_radius(c, 0), 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    return true;
}
void sl_art_apply(lv_obj_t *c, int a, int size) {test_cover_attach(c,a,size);}
/* SLOPPY_SHOWCASE: Cover Flow reads its thumbnails from the card; the real sleeves, written there once */
const char *sl_art_src(int a, int size) {
    static char src[10][56];
    if (!sc_on || size != SL_ART_GRID || a < 0 || a >= 10 || !sc_art[a].rgb) return NULL;
    if (!src[a][0]) {
        char p[48]; snprintf(p, sizeof p, "/.rockbox/sloppy/sc-cover-%d.bin", a);   /* "sloppy/": a path the fake card keeps */
        uint16_t *px = malloc((size_t)size * size * 2); if (!px) return NULL;
        sc_cover(a, size, px);
        lv_image_header_t h = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = size, .h = size, .stride = size * 2 };
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { write(fd, &h, sizeof h); write(fd, px, (size_t)size * size * 2); sim_close(fd); snprintf(src[a], sizeof src[a], "R:%s", p); }
        free(px);
    }
    return src[a][0] ? src[a] : NULL;
}
/* the phone remote's folder cover: this file, when set (a Rockbox path) */
static const char *test_folder_cover;
bool search_albumart_files(const struct mp3entry *id3, const char *size_string, char *buf, int buflen)
{ (void)id3; (void)size_string; if (!test_folder_cover) return false; strlcpy(buf, test_folder_cover, buflen); return true; }
bool sl_art_apply_scaled(lv_obj_t *c, int a, int size) {return test_cover_attach(c,a,size);}
unsigned sl_art_made;
bool sl_art_busy(void) { return false; }
bool sl_art_no_cover(int a) {(void)a;return false;}
static signed char test_art_state[64];   /* the cover fetcher's view: 1 has, 0 none, -1 not looked at; 0 by default */
static int test_art_again = -1;
int sl_art_state(int a) { return a >= 0 && a < 64 ? test_art_state[a] : 1; }
void sl_art_again(int a) { test_art_again = a; }
int test_art_hue=-1;                     /* the cover colour sl_art_hue reports; -1: no thumbnail */
bool sl_art_hue(int a, int *hue, int *sat) {
    if(test_real_art&&a>=0&&a<10&&sc_art[a].rgb){*hue=sc_art[a].hue;*sat=sc_art[a].sat;return true;}
    if(test_real_art&&a>=0){static const int sh[6]={20,0,0,0,0,0},ss[6]={75,85,20,10,75,45};
        *hue=(test_cover_hue(a)+sh[a%6])%360;*sat=ss[a%6];return true;}
    if(test_art_hue<0)return false;*hue=test_art_hue;*sat=80;return true;}
/* sloppy_lib.c's lookup, as on the player: a hash of the paths, made again
 * when the library changes (a scan of every song made perf numbers that
 * look songs up by path, Picked for you and Your taste, read ~60x too slow) */
int sl_lib_find_path(const char *p) {
    static int *ix; static int cap, n0; static const struct sl_track *t0;
    if(!p||!sl_lib.n_tracks) return -1;
    if(sl_lib.tracks!=t0||sl_lib.n_tracks!=n0){
        free(ix);cap=1024;while(cap<sl_lib.n_tracks*2)cap*=2;ix=malloc(cap*sizeof *ix);assert(ix);
        for(int i=0;i<cap;i++)ix[i]=-1;
        for(int i=0;i<sl_lib.n_tracks;i++){if(!sl_lib.tracks[i].file)continue;unsigned h=sl_track_path_fnv(i);
            int j=0;while(ix[(h+j)&(cap-1)]>=0&&!sl_track_same_file(ix[(h+j)&(cap-1)],i))j++;
            if(ix[(h+j)&(cap-1)]<0)ix[(h+j)&(cap-1)]=i;}
        t0=sl_lib.tracks;n0=sl_lib.n_tracks;}
    unsigned h=2166136261u;for(const char *q=p;*q;q++)h=(h^(unsigned char)*q)*16777619u;
    for(int j=0;j<cap;j++){int t=ix[(h+j)&(cap-1)];if(t<0)return -1;if(sl_track_path_is(t,p))return t;}
    return -1;
}
int sl_lib_find_path_at(const char *p, long ms) {   /* sloppy_lib.c's: a cue sheet's track by where the file is */
    int t=sl_lib_find_path(p); if(t<0||!sl_lib.tracks[t].cue) return t;
    while(t+1<sl_lib.n_tracks&&sl_lib.tracks[t+1].cue==sl_lib.tracks[t].cue+1&&sl_lib.tracks[t+1].start<=ms&&sl_track_same_file(t+1,t)) t++;
    return t; }
static int objects(lv_obj_t *o) {int n=1;for(unsigned i=0;i<lv_obj_get_child_count(o);i++)n+=objects(lv_obj_get_child(o,i));return n;}
static lv_obj_t *row_named(lv_obj_t *body, const char *name) {
    for(uint32_t i=0;i<lv_obj_get_child_count(body);i++) {
        lv_obj_t *row=lv_obj_get_child(body,i);
        if(!lv_obj_get_child_count(row)) continue;
        lv_obj_t *label=lv_obj_get_child(row,0);
        if(lv_obj_check_type(label,&lv_label_class) && !strcmp(lv_label_get_text(label),name)) return row;
        if(!lv_obj_check_type(label,&lv_label_class)) { lv_obj_t *in=row_named(row,name); if(in) return in; }   /* a Settings group */
    }
    return NULL;
}
static void fixture(void) {
    sl_lib.n_tracks=10000;sl_lib.n_albums=1;sl_lib.n_artists=1;
    sl_lib.tracks=calloc(sl_lib.n_tracks,sizeof(*sl_lib.tracks));
    sl_lib.albums=calloc(1,sizeof(*sl_lib.albums));
    sl_lib.songs_by_title=calloc(sl_lib.n_tracks,sizeof(int));
    sl_lib.albums_by_title=calloc(1,sizeof(int));sl_lib.albums_by_artist=calloc(1,sizeof(int));
    struct sl_album *a=sl_lib.albums;a->title="Blue Hours";a->artist="Mira Sol";a->hue=210;a->year=2026;a->n_tracks=10000;a->tracks=calloc(10000,sizeof(int));
    static const char *titles[] = {"Blue Hours", "After the Rain", "Night Tide", "Glass Avenue",
        "Silent City", "High Voltage", "Neon Sky", "Small Hours", "Runaway", "Night Drive"};
    for(int i=0;i<10000;i++) {
        sl_lib.songs_by_title[i]=a->tracks[i]=i;
        struct sl_track *t=&sl_lib.tracks[i];t->title=titles[i%10];
        char path[64];snprintf(path,sizeof path,"/Music/song-%d.flac",i);tpath(t,strdup(path));
        t->album=0;t->num=i+1;t->len=240000;
    }
}
/* a fake touchscreen behind the real touch_read, so gestures run end to end */
static int t_x, t_y; static bool t_down;
void reset_poweroff_timer(void) {}
int button_status_wdata(int *p) { *p = (t_x << 16) | t_y; return t_down ? BUTTON_TOUCHSCREEN : 0; }
static intptr_t t_event_pos;
intptr_t button_get_data(void) { return t_event_pos; }
/* the first label under 'o' showing 's' */
static lv_obj_t *find_label(lv_obj_t *o, const char *s) {
    if (lv_obj_check_type(o,&lv_label_class) && !strcmp(lv_label_get_text(o),s)) return o;
    for (uint32_t i=0;i<lv_obj_get_child_count(o);i++){lv_obj_t *f=find_label(lv_obj_get_child(o,i),s);if(f)return f;}
    return NULL;
}
static void touch_at(int x, int y, bool down) { t_x = x; t_y = y; t_down = down; frame(40); }
static void key_tap(lv_obj_t *k) { lv_area_t a;lv_obj_get_coords(k,&a);int x=(a.x1+a.x2)/2,y=(a.y1+a.y2)/2;touch_at(x,y,true);touch_at(x,y,false); }
/* Cover Flow is landscape: a point (x, y) there is (y, 799 - x) on the panel */
static void ltouch(int x, int y, bool down) { touch_at(y, H - 1 - x, down); }
static void ldrag(int x0, int x1, int y) { ltouch(x0, y, true); for (int i = 1; i <= 6; i++) ltouch(x0 + (x1 - x0) * i / 6, y, true); ltouch(x1, y, false); }
static void ltap(int x, int y) { ltouch(x, y, true); ltouch(x, y, false); }
/* the keyboard of the page on top, and its keys by letter or by job */
static struct kb *kb_now(void) { return npages ? kb_of_page(pages[npages - 1]) : NULL; }
/* no letter cut in half: every lead byte has all its followers */
static bool utf8_whole(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        int n = *p < 0x80 ? 1 : *p >= 0xf0 ? 4 : *p >= 0xe0 ? 3 : *p >= 0xc0 ? 2 : 0;
        if (!n) return false;
        for (int i = 1; i < n; i++) if ((p[i] & 0xc0) != 0x80) return false;
        p += n;
    }
    return true;
}
static int kb_find(struct kb *kb, uint32_t cp)
{
    for (int i = 0; i < kb->nk; i++) if (kb->k[i].act == KA_CHAR && kb->k[i].cp == cp) return i;
    for (int i = 0; i < kb->nk; i++) if (kb->k[i].act == KA_CHAR && kb->k[i].cp == st_lower(cp)) return i;
    return -1;
}
static int kb_find_act(struct kb *kb, int act) { for (int i = 0; i < kb->nk; i++) if (kb->k[i].act == act) return i; return -1; }
static void kb_xy(struct kb *kb, int i, int *x, int *y)
{
    lv_area_t a; lv_obj_get_coords(kb->k[i].o, &a); *x = (a.x1 + a.x2) / 2; *y = (a.y1 + a.y2) / 2;
}
static void kb_tap(struct kb *kb, int i) { int x, y; assert(i >= 0); kb_xy(kb, i, &x, &y); touch_at(x, y, true); touch_at(x, y, false); frame(60); }
static void kb_tap_act(struct kb *kb, int act) { kb_tap(kb, kb_find_act(kb, act)); }
/* type a string as a finger would: Shift for a capital, the symbols' pages for the rest */
static void kb_type(struct kb *kb, const char *s)
{
    uint32_t c;
    while ((c = st_utf8_next(&s)))
    {
        if (c == ' ') { kb_tap_act(kb, KA_SPACE); continue; }
        int i = kb_find(kb, c);
        if (i < 0)
            for (int tries = 0; tries < 3 && i < 0; tries++)
            {
                if (kb->page == 1) kb_tap_act(kb, KA_SYMS); else kb_tap_act(kb, KA_MODE);
                i = kb_find(kb, c);
            }
        assert(i >= 0);
        if (!kb->page && c != st_lower(c) && !kb->shift) kb_tap_act(kb, KA_SHIFT);
        if (!kb->page && c == st_lower(c) && kb->shift == 1) kb_tap_act(kb, KA_SHIFT);
        kb_tap(kb, kb_find(kb, c));
        if (kb->page) kb_tap_act(kb, KA_MODE);
    }
}
/* a finger gliding through a word's keys: corners met, four steps between; held when !lift */
static void kb_glide(struct kb *kb, const char *word, bool lift)
{
    int px = -1, py = -1; bool first = true;
    uint32_t c;
    while ((c = st_utf8_next(&word)))
    {
        int i = kb_find(kb, c);
        if (i < 0) { uint32_t f = st_fold(st_lower(c)); i = f ? kb_find(kb, f) : -1; }
        if (c == '\'') continue;
        assert(i >= 0);
        int x, y; kb_xy(kb, i, &x, &y);
        if (first) { touch_at(x, y, true); first = false; }
        else for (int s = 1; s <= 4; s++) touch_at(px + (x - px) * s / 4, py + (y - py) * s / 4, true);
        px = x; py = y;
    }
    if (lift) { touch_at(px, py, false); frame(100); }
}
static void glide_word(const char *word) { kb_glide(kb_now(), word, true); }
/* every letter of every label under o has a glyph (no placeholder box); how many lack one */
static int glyphs_missing(lv_obj_t *o) {
    int miss = 0;
    if (lv_obj_check_type(o, &lv_label_class)) {
        const lv_font_t *f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
        const char *t = lv_label_get_text(o); uint32_t c;
        while (t && (c = st_utf8_next(&t))) {
            if (c < 0x20 || (c >= 0x2000 && c <= 0x206f)) continue;
            lv_font_glyph_dsc_t g; if (!lv_font_get_glyph_dsc(f, &g, c, 0) || g.is_placeholder) miss++; }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) miss += glyphs_missing(lv_obj_get_child(o, i));
    return miss;
}
void __sanitizer_print_stack_trace(void);
static void fuzz_page_deleted(lv_event_t *e)
{
    lv_obj_t *o=lv_event_get_target(e);
    for(int i=0;i<npages;i++)if(pages[i]==o){fprintf(stderr,"FUZZ: live page %d deleted here:\n",i);__sanitizer_print_stack_trace();}
}


/* SLOPPY_DTAP: every button on every page tapped twice, quickly, as a
 * thumb does: no page may open twice, and every page has a way back */
static void dt_text(lv_obj_t *o, char *out, size_t cap) {
    if (lv_obj_check_type(o, &lv_label_class) && !out[0]) { strlcpy(out, lv_label_get_text(o), cap); return; }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o) && !out[0]; i++) dt_text(lv_obj_get_child(o, i), out, cap);
}
static void dt_title(lv_obj_t *p, char *out, size_t cap) {
    out[0] = 0; lv_obj_t *hdr = lv_obj_get_child_count(p) ? lv_obj_get_child(p, 0) : NULL;
    if (!hdr) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(hdr) && !out[0]; i++) {
        lv_obj_t *c = lv_obj_get_child(hdr, i);
        if (lv_obj_check_type(c, &lv_label_class)) strlcpy(out, lv_label_get_text(c), cap);
    }
    if (!out[0]) dt_text(hdr, out, cap);
}
static int dt_n; static lv_point_t dt_pt[64]; static char dt_lbl[64][40];
static void dt_collect(lv_obj_t *o) {
    if (dt_n >= 64 || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_child_count(o) < 12 && a.x2 > a.x1 && a.y2 > a.y1
        && a.x1 >= 0 && a.x2 < W && a.y1 >= SB_H && a.y2 < H && lv_area_get_width(&a) < W * 2) {
        int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
        bool dup = false;
        for (int i = 0; i < dt_n; i++) if (abs(dt_pt[i].x - x) < 8 && abs(dt_pt[i].y - y) < 8) dup = true;
        if (!dup) { dt_pt[dt_n].x = x; dt_pt[dt_n].y = y; dt_lbl[dt_n][0] = 0; dt_text(o, dt_lbl[dt_n], sizeof dt_lbl[0]); dt_n++; }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) dt_collect(lv_obj_get_child(o, i));
}

/* Freaky Spoon helpers: jump the run to a height, with a plate underfoot */
static void spoon_warp(int metres, int event)
{
    struct sl_spoon_state *s=&spoon_state;
    for(int i=0;i<SL_SPOON_PLATS;i++)s->plat[i].live=0;
    for(int i=0;i<SL_SPOON_ITEMS;i++)s->item[i].live=0;
    for(int i=0;i<SL_SPOON_FOES;i++)s->foe[i].type=SF_NONE;
    float y=-(float)metres*SL_SPOON_PX_M;
    s->y=y-60;s->vy=-300;s->top_y=s->y;s->height=metres;s->phase=sl_spoon_phase_of(metres);
    s->cam=y-SL_SPOON_H*0.55f;s->soup=s->cam+SL_SPOON_H+40;
    s->gen_y=y;s->gen_x=s->x;s->gen_slack=0;
    sl_spoon_add_plat(s,SP_PLATE,s->x,y,90)->spine=1;
    s->event=event;s->event_state=event?SE_ON:SE_OFF;s->event_t=3;s->wind=1;
    if(event==SE_MEATBALLS){sl_spoon_meatball(s,120);struct sl_spoon_foe*f=sl_spoon_add_foe(s,SF_MEATBALL);f->x=300;f->y=s->cam+200;f->state=1;f->vy=540;}
    while(s->gen_y>s->cam-SL_SPOON_H*0.7f)sl_spoon_gen_unit(s);
    s->fxn=0;
}
static void spoon_warp_test(void)
{
    struct sl_spoon_state *s=&spoon_state;
    /* hold left: the run starts and the spoon goes left at once */
    touch_at(100,SPN_PAD_Y+60,true);
    assert(spn_mode==SPN_PLAY && s->started && s->dir==-1 && lv_obj_has_flag(spn_card,LV_OBJ_FLAG_HIDDEN));
    touch_at(100,SPN_PAD_Y+60,true);frame(40);frame(40);
    assert(s->vx<-200);
    /* slide the thumb across without lifting: now it is right */
    touch_at(380,SPN_PAD_Y+60,true);touch_at(380,SPN_PAD_Y+60,true);
    assert(s->dir==1 && spn_touch_dir==1);
    touch_at(380,SPN_PAD_Y+60,false);assert(s->dir==0 && spn_touch_dir==0);
    /* the keys steer too */
    assert(spoon_key(BUTTON_LEFT,false) && s->dir==-1);
    assert(spoon_key(BUTTON_LEFT,true) && s->dir==0);
    s->calm=true;
    frame(600);shot("spoon-play.ppm");
    struct { int m, event; const char *name; } scenes[]={
        {40,0,"spoon-countertop.ppm"},{160,SE_WIND,"spoon-cupboard.ppm"},{330,SE_LOWG,"spoon-freezer.ppm"},
        {600,SE_MEATBALLS,"spoon-rooftop.ppm"},{900,SE_DARK,"spoon-nebula.ppm"},{1300,0,"spoon-freaky.ppm"}};
    for(unsigned i=0;i<sizeof scenes/sizeof scenes[0];i++)
    {
        spoon_warp(scenes[i].m,scenes[i].event);
        s->combo=i*3;s->mult=sl_spoon_mult(s->combo);s->lid=i==2;s->whisk=i==4?1.5f:0;s->freaky=i==5?3:0;
        frame(16);frame(16);frame(16);shot_now(scenes[i].name);
        {struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);for(int k=0;k<50;k++)spn_draw(lv_tick_get()+k*16,0.016f);clock_gettime(CLOCK_MONOTONIC,&b);
         printf("spoon draw %s: %.0f us per frame\n",scenes[i].name,((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/50e3);}
        assert(s->alive);
    }
    s->calm=false;
    /* a real fork from the side ends it: FORKED, a card with the score,
     * arrows off until the card is ready, then a fresh press replays */
    spoon_warp(700,0);
    struct sl_spoon_foe *f=sl_spoon_add_foe(s,SF_FORK);
    f->x=s->x;f->x0=0;f->x1=SL_SPOON_W;f->vx=0;f->y=f->y0=s->y-SL_SPOON_BOWL-30;s->vy=-600;s->lid=false;s->whisk=0;s->invuln=0;
    s->score=4321;
    frame(40);frame(40);
    assert(!s->alive && s->death==SD_FORK && spn_mode==SPN_OVER);
    frame(300);assert(lv_obj_has_flag(spn_card,LV_OBJ_FLAG_HIDDEN));
    touch_at(100,SPN_PAD_Y+60,true);touch_at(100,SPN_PAD_Y+60,false);
    assert(spn_mode==SPN_OVER);                         /* too early: ignored */
    frame(600);shot("spoon-over.ppm");
    assert(!lv_obj_has_flag(spn_card,LV_OBJ_FLAG_HIDDEN) && !strcmp(lv_label_get_text(spn_card_title),"FORKED"));
    assert(spn_save.best>=4321 && spn_new_best && !lv_obj_has_flag(spn_card_badge,LV_OBJ_FLAG_HIDDEN));
    assert(spn_save.runs>=1 && spn_save.best_m>=700);
    frame(400);
    touch_at(380,SPN_PAD_Y+60,true);
    assert(spn_mode==SPN_PLAY && s->alive && s->started && s->score==0 && s->dir==1);
    assert(spn_last_height>=700);
    touch_at(380,SPN_PAD_Y+60,false);
    /* the lock screen pauses; an arrow carries on */
    bool running=sl_power_running;int mode=sl_power.mode;
    sl_power_running=true;sl_power.mode=SL_POWER_LOCKED;frame(40);
    assert(spn_mode==SPN_PAUSED && !lv_obj_has_flag(spn_card,LV_OBJ_FLAG_HIDDEN));
    float y=s->y;frame(300);assert(s->y==y);
    sl_power.mode=mode;sl_power_running=running;
    frame(40);assert(spn_mode==SPN_PAUSED);
    assert(spoon_key(BUTTON_RIGHT,false) && spn_mode==SPN_PLAY);
    spoon_key(BUTTON_RIGHT,true);
    /* soup: stand still long enough and it wins; the Play again button works */
    s->calm=true;
    for(int i=0;i<1200 && s->alive;i++)frame(50);
    assert(!s->alive && s->death==SD_SOUP);
    frame(900);assert(!strcmp(lv_label_get_text(spn_card_title),"SOUPED"));
    lv_obj_send_event(spn_card_go,LV_EVENT_CLICKED,NULL);
    assert(spn_mode==SPN_PLAY && s->alive);
    /* saves survive reopening */
    int best=spn_save.best;
    spn_loaded=false;memset(&spn_save,0,sizeof spn_save);spn_load();
    assert(spn_save.best==best && spn_save.runs>=2);
}
static void spoon_play_test(void)
{
    /* sounds for the moments that matter */
    struct sl_spoon_state *s=&spoon_state;
    spn_arrow_press();
    SFX_RESET();
    spoon_warp(20,0);
    /* only the test's own pieces: the warp generates a random level from
     * the spoon's height up, and about one run in six put a plate or a
     * fork between the spoon and the plate below it */
    for(int i=0;i<SL_SPOON_PLATS;i++)s->plat[i].live=0;
    for(int i=0;i<SL_SPOON_ITEMS;i++)s->item[i].live=0;
    for(int i=0;i<SL_SPOON_FOES;i++)s->foe[i].type=SF_NONE;
    s->gen_y=s->y-2000;s->calm=true;
    struct sl_spoon_plat *p=sl_spoon_add_plat(s,SP_PLATE,s->x,s->y+20,80);
    s->vy=300;for(int i=0;i<8&&!p->used;i++)frame(40);assert(sfx_last==SFX_SPOON_PERFECT && s->combo==1 && p->used);
    SFX_RESET();sl_spoon_add_item(s,SI_O,s->x,s->y-SL_SPOON_BOWL);frame(40);assert(sfx_last==SFX_SPOON_COIN);
    SFX_RESET();s->y=s->soup+SL_SPOON_BOWL+10;frame(40);assert(!s->alive && sfx_last==SFX_SPOON_SPLASH);
    s->calm=false;
}

/* Chess: the screen point at the middle of a square, the board as it is turned now */
/* ---- the control centre, pulled further (Android quick settings) ---- */
static int ccx_tile(int f) { for (int k=0;k<qp_total;k++) if (qp_fn[k]==f) return k; assert(0); return -1; }
static void ccx_center(lv_obj_t *o,int *x,int *y) { lv_area_t a;lv_obj_get_coords(o,&a);*x=(a.x1+a.x2)/2;*y=(a.y1+a.y2)/2; }
static void ccx_settle(void) { for(int i=0;i<8;i++)frame(60); }
static void ccx_open(void) {
    touch_at(240,4,true);for(int y=20;y<=500;y+=60)touch_at(240,y,true);touch_at(240,500,false);
    for(int i=0;i<5;i++)frame(100);
    assert(qp_open && lv_obj_get_style_translate_y(qp,0)==0 && qx_px==0 && lv_obj_has_flag(qp_scroll,LV_OBJ_FLAG_SCROLLABLE));
}
/* a slow drag from y0 to y1, held still before it lets go: no flick */
static void ccx_drag(int x,int y0,int y1) {
    int st=y1>y0?20:-20;touch_at(x,y0,true);
    for(int y=y0+st;st>0?y<=y1:y>=y1;y+=st)touch_at(x,y,true);
    touch_at(x,y1,true);frame(120);touch_at(x,y1,false);ccx_settle();
}
static void ccx_grow(void) {
    ccx_drag(240,200,460);
    assert(qx_px==qx_grow && lv_obj_get_height(qp_tp)==QX_TP_H+qx_grow && !lv_obj_has_flag(qp_scroll,LV_OBJ_FLAG_SCROLLABLE));
}
static void ccx_closed(void) { ccx_settle(); assert(!qp_open && qx_px==0 && lv_obj_has_flag(qp,LV_OBJ_FLAG_HIDDEN)); }
static void ccx_tap(lv_obj_t *o) { key_tap(o);frame(100); }
static int ccx_state(int f) {
    switch(f){
        case 0: return global_settings.playlist_shuffle;
        case 1: return global_settings.repeat_mode;
        case 2: return xf_seconds();
        case 3: return global_settings.eq_enabled;
        case 4: return sleep_choice;
        case 8: return global_settings.replaygain_settings.type;
        case 9: return sl_bt_state.enabled;
        case 10: return speed_pct();
        case 11: return global_settings.sloppy_remote_keys;
        case 12: return global_settings.sloppy_wifi;
        case 15: return global_settings.sloppy_notifications;
    }
    assert(0);return 0;
}
/* Every tile's name stays in its own cell, clear of its circle and of the
 * names beside and below it: each language, text size and theme, folded
 * and grown, with the longest states the names take. */
static void cc_label_check(const char *what)
{
    lv_obj_update_layout(qp);
    {   /* the battery clear of Edit and Power, the heading of the card, the slider names of their sliders */
        lv_area_t d,e,h,t,c0,c1,s0,s1;lv_obj_get_coords(qp_date,&d);lv_obj_get_coords(qx_edit,&e);lv_obj_get_coords(qp_head,&h);lv_obj_get_coords(qp_tp,&t);
        lv_obj_get_coords(qp_cap[0],&c0);lv_obj_get_coords(qp_cap[1],&c1);lv_obj_get_coords(qp_bright,&s0);lv_obj_get_coords(qp_vol,&s1);
        bool ok=d.y2<e.y1 && h.y2<t.y1 && h.x2<e.x1 && c0.y2<s0.y1-6 && c1.y2<s1.y1-6;   /* -6: the knob stands proud of the track */
        if(!ok){printf("cc label %s: battery, heading or slider names overlap (battery %d/%d, heading %d/%d x %d/%d, names %d/%d %d/%d)\n",what,(int)d.y2,(int)e.y1,(int)h.y2,(int)t.y1,(int)h.x2,(int)e.x1,(int)c0.y2,(int)s0.y1,(int)c1.y2,(int)s1.y1);assert(getenv("CC_LABEL_REPORT"));} }
    for (int k=0;k<qp_total;k++) {
        if (lv_obj_has_flag(qp_tiles[k],LV_OBJ_FLAG_HIDDEN) || k/QP_PAGE!=qx_page) continue;
        lv_area_t l,c,o;lv_obj_get_coords(qp_tile_lbl[k],&l);lv_obj_get_coords(qp_tiles[k],&c);lv_obj_get_coords(lv_obj_get_child(qp_tiles[k],0),&o);
        if (!(l.x1>=c.x1 && l.x2<=c.x2 && l.y2<=c.y2 && l.y1>o.y2)) {
            printf("cc label %s: \"%s\" (%d,%d %d,%d) leaves its cell (%d,%d %d,%d) or meets its circle (bottom %d)\n",
                   what,lv_label_get_text(qp_tile_lbl[k]),(int)l.x1,(int)l.y1,(int)l.x2,(int)l.y2,(int)c.x1,(int)c.y1,(int)c.x2,(int)c.y2,(int)o.y2);
            assert(getenv("CC_LABEL_REPORT"));   /* =1: list every one, don't stop */
        }
        {   /* nothing cut off: the name's lines fit the label */
            lv_point_t z;lv_obj_t *t=qp_tile_lbl[k];
            lv_text_get_size(&z,lv_label_get_text(t),lv_obj_get_style_text_font(t,0),0,lv_obj_get_style_text_line_space(t,0),lv_obj_get_content_width(t),LV_TEXT_FLAG_NONE);
            if (z.y>lv_obj_get_height(t)||z.x>lv_obj_get_width(t)) { printf("cc label %s: \"%s\" cut off\n",what,lv_label_get_text(t)); assert(getenv("CC_LABEL_REPORT")); } }
        for (int j=0;j<k;j++) { lv_area_t m;lv_obj_get_coords(qp_tile_lbl[j],&m);
            if (lv_area_is_on(&l,&m)) { printf("cc label %s: \"%s\" meets \"%s\"\n",what,lv_label_get_text(qp_tile_lbl[k]),lv_label_get_text(qp_tile_lbl[j])); assert(getenv("CC_LABEL_REPORT")); } }
    }
}
static void cc_label_tests(void)
{
    int lang0=global_settings.sloppy_language,step0=sl_text_step,theme0=global_settings.sloppy_theme;
    int rep0=global_settings.repeat_mode,sleep0=sleep_choice,speed0=speed_pct();
    int checks=0;
    for (int lang=0;lang<=SL_LANG_LAST;lang++) for (int step=0;step<SL_TEXT_STEPS;step++) for (int th=0;th<4;th++) for (int lng=0;lng<2;lng++) {
        global_settings.sloppy_language=lang;sl_text_step=step;global_settings.sloppy_theme=th;
        global_settings.repeat_mode=lng?REPEAT_ONE:REPEAT_ALL;sleep_choice=lng?3:0;speed_set(lng?125:100);
        quick_panel_rebuild();qp_refresh();
        lv_obj_remove_flag(qp,LV_OBJ_FLAG_HIDDEN);lv_obj_set_style_translate_y(qp,0,0);
        char what[48];snprintf(what,sizeof what,"%s, text %d, theme %d%s",sl_lang_code(lang),step,th,lng?", long states":"");
        cc_label_check(what);qx_set(qx_grow);cc_label_check(what);qx_set(qx_grow/2);cc_label_check(what);
        qx_set(0);lv_obj_add_flag(qp,LV_OBJ_FLAG_HIDDEN);checks+=3;
    }
    global_settings.sloppy_language=lang0;sl_text_step=step0;global_settings.sloppy_theme=theme0;
    global_settings.repeat_mode=rep0;sleep_choice=sleep0;speed_set(speed0);
    quick_panel_rebuild();frame(100);
    printf("cc labels: %d layouts, %d languages x %d text sizes x 4 themes, folded, half and grown: none overlap\n",checks,SL_LANG_LAST+1,SL_TEXT_STEPS);
    /* and as Tai sees them: pulled down and grown, the biggest text */
    static const struct { int lang, step; const char *name; } looks[]={{0,3,"en-largest"},{3,3,"de-largest"},{5,3,"ar-largest"},{4,2,"he-larger"},{3,0,"de-standard"},{2,1,"fr-large"}};
    for (unsigned i=0;i<sizeof looks/sizeof looks[0];i++) {
        global_settings.sloppy_language=looks[i].lang;sl_text_step=looks[i].step;global_settings.sloppy_text_size=looks[i].step;
        global_settings.repeat_mode=REPEAT_ONE;
        appearance_reload((void *)2);frame(700);while(npages>1){page_pop();frame(400);}
        char b[64];ccx_open();snprintf(b,sizeof b,"cc-labels-%s-folded.ppm",looks[i].name);shot(b);cc_label_check(b);
        ccx_grow();snprintf(b,sizeof b,"cc-labels-%s-grown.ppm",looks[i].name);shot(b);cc_label_check(b);
        qp_close();ccx_closed();
    }
    global_settings.sloppy_language=lang0;sl_text_step=step0;global_settings.sloppy_text_size=step0;global_settings.repeat_mode=rep0;
    appearance_reload((void *)2);frame(700);
}
static void cc_expand_tests(void)
{
    while(npages>1){page_pop();frame(400);}
    if(np_open){np_hide(0);frame(500);}
    if(qp_open){qp_close();frame(400);}
    int theme0=global_settings.sloppy_theme,style0=global_settings.sloppy_home_style,bright0=global_settings.brightness;
    /* what the tests after these look at: put back as it was at the end */
    static struct note notes0[NOTE_MAX];int nnotes0=nh.n;uint32_t next0=nh.next_id;memcpy(notes0,nh.list,sizeof notes0);
    struct sl_power_state power0=sl_power;
    global_settings.sloppy_theme=0;appearance_reload(NULL);frame(700);while(npages>1){page_pop();frame(400);}
    global_settings.sloppy_qp_tiles[0]='\0';quick_panel_rebuild();frame(200);
    /* every kind, the chosen eight first, each once: two pages when grown */
    { unsigned seen=0;for(int k=0;k<QP_FNS;k++){assert(!(seen&1u<<qp_fn[k]));seen|=1u<<qp_fn[k];}
      assert(seen==(1u<<QP_FNS)-1 && qp_n==8 && qp_total==QP_FNS && qp_fn[0]==0 && qp_fn[8]==8 && qx_pages==2);
      assert(qx_grow==2*QX_ROW+QX_DOTS); }

    /* folded: as it always was */
    ccx_open();shot("cc-collapsed.ppm");
    int objs=objects(qp),top=(int)lv_obj_get_child_count(lv_layer_top());
    assert(lv_obj_has_flag(qx_edit,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(qx_power,LV_OBJ_FLAG_HIDDEN));
    {   /* a drag down on the shortcuts: the card follows the finger, no tile is pressed */
        touch_at(240,200,true);touch_at(240,240,true);touch_at(240,300,true);
        assert(g_state==G_GROW && qx_px==100 && lv_obj_get_height(qp_tp)==QX_TP_H+100);
        assert(lv_obj_get_style_translate_y(qp_sp,0)==100 && !lv_obj_has_flag(qx_edit,LV_OBJ_FLAG_HIDDEN));
        for(int k=0;k<qp_total;k++)assert(!lv_obj_has_state(qp_tiles[k],LV_STATE_PRESSED));
        lv_refr_now(disp);shot_now("cc-mid-drag.ppm");
        /* held there and let go short of half: it folds back */
        frame(120);touch_at(240,300,false);ccx_settle();
        assert(qp_open && qx_px==0 && lv_obj_has_flag(qx_edit,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(qp_scroll,LV_OBJ_FLAG_SCROLLABLE));
    }
    /* past half: it grows the rest of the way */
    ccx_grow();shot("cc-expanded.ppm");
    {   lv_area_t a;lv_obj_get_coords(qp_np_card,&a);assert(a.y1>=H);     /* Now Playing has gone off the bottom */
        lv_area_t c;lv_obj_get_coords(qp_tp,&c);
        lv_obj_get_coords(qp_sp,&a);assert(a.y1>c.y2 && a.y2<H-QP_EDGE);  /* the sliders stay, under the card, clear of the bottom edge */
        lv_obj_get_coords(qp_tiles[QP_PAGE-1],&a);assert(a.y2<=c.y2);    /* page one's last row fits */
        lv_obj_get_coords(qx_dots,&a);assert(!lv_obj_has_flag(qx_dots,LV_OBJ_FLAG_HIDDEN) && a.y1>=c.y2-QX_DOTS && a.y2<c.y2);   /* the dots under it, on the card */
        lv_obj_get_coords(qp_tiles[QP_PAGE],&a);assert(a.x1>c.x2);       /* page two waits to the right */
        assert(lv_obj_get_style_opa(qx_edit,0)==LV_OPA_COVER && qp_open); }
    /* a drag down when grown goes nowhere; one on a slider stays the slider's */
    ccx_drag(240,300,420);assert(qx_px==qx_grow);
    {   int x,y;ccx_center(qp_bright,&x,&y);touch_at(x,y,true);touch_at(x,y+30,true);touch_at(x,y+60,true);
        assert(g_state!=G_GROW && qx_px==qx_grow);touch_at(x,y+60,false);ccx_settle();assert(qx_px==qx_grow); }
    /* sideways on the tiles: the pages turn, under the finger; nothing is pressed, nothing folds */
    {   int sx=lv_obj_get_scroll_x(qp_tp);assert(sx==0 && qx_page==0);
        lv_area_t first;lv_obj_get_coords(qp_tiles[0],&first);
        touch_at(400,300,true);touch_at(380,302,true);touch_at(340,304,true);touch_at(300,305,true);
        assert(g_state==G_CCPAGE && lv_obj_get_scroll_x(qp_tp)==100 && qx_px==qx_grow);
        for(int k=0;k<qp_total;k++)assert(!lv_obj_has_state(qp_tiles[k],LV_STATE_PRESSED));
        frame(120);touch_at(300,305,false);ccx_settle();
        assert(qx_page==0 && lv_obj_get_scroll_x(qp_tp)==0);              /* short of a third, held: back */
        touch_at(400,300,true);for(int x=380;x>=200;x-=20)touch_at(x,300,true);frame(120);touch_at(200,300,false);ccx_settle();
        assert(qx_page==1 && lv_obj_get_scroll_x(qp_tp)==QX_PAGE_W && qx_px==qx_grow);
        lv_area_t a;lv_obj_get_coords(qp_tiles[QP_PAGE],&a);assert(a.x1==first.x1 && a.y1==first.y1);   /* page two's first where page one's was */
        assert(lv_obj_get_style_bg_opa(lv_obj_get_child(qx_dots,1),0)>lv_obj_get_style_bg_opa(lv_obj_get_child(qx_dots,0),0));
        shot("cc-expanded-page-2.ppm");
        /* past the last page it gives a little, then settles back */
        touch_at(300,300,true);for(int x=280;x>=100;x-=20)touch_at(x,300,true);
        assert(lv_obj_get_scroll_x(qp_tp)>QX_PAGE_W && lv_obj_get_scroll_x(qp_tp)<QX_PAGE_W+100);
        frame(120);touch_at(100,300,false);ccx_settle();assert(qx_page==1 && lv_obj_get_scroll_x(qp_tp)==QX_PAGE_W);
        /* a tile on page two works */
        {   int x,y;ccx_center(qp_tiles[QP_PAGE],&x,&y);lv_obj_t *o=lv_indev_search_obj(qp,&(lv_point_t){x,y});
            while(o&&o!=qp_tiles[QP_PAGE])o=lv_obj_get_parent(o);assert(o);
            ccx_center(qp_tiles[QP_PAGE-4],&x,&y);assert(x<0);   /* page one has gone off to the left */ }
        /* a quick flick back */
        touch_at(150,300,true);touch_at(190,300,true);touch_at(240,300,true);touch_at(240,300,false);ccx_settle();
        assert(qx_page==0 && lv_obj_get_scroll_x(qp_tp)==0);
        /* the bottom edge and the top still do as ever from page two: folding brings page one back */
        touch_at(400,300,true);touch_at(340,300,true);touch_at(250,300,true);touch_at(150,300,true);touch_at(150,300,false);ccx_settle();
        assert(qx_page==1);
        ccx_drag(240,560,360);assert(qp_open && qx_px==0 && qx_page==0 && lv_obj_get_scroll_x(qp_tp)==0);
        ccx_grow();assert(qx_page==0); }
    /* pushed up past half: folds */
    ccx_drag(240,500,360);assert(qp_open && qx_px==0);
    /* a flick decides, however short the drag */
    touch_at(240,200,true);touch_at(240,230,true);touch_at(240,260,true);touch_at(240,290,true);
    assert(qx_px==90);touch_at(240,290,false);ccx_settle();assert(qx_px==qx_grow);
    touch_at(240,500,true);touch_at(240,470,true);touch_at(240,440,true);touch_at(240,410,true);
    assert(qx_px==qx_grow-90);touch_at(240,410,false);ccx_settle();assert(qp_open && qx_px==0);
    /* folded, with the list scrolled: a drag down scrolls it back, it does not grow */
    {   for(int i=0;i<12;i++){char b[32];snprintf(b,sizeof b,"Grow note %d",i);notify_ex(NS_SETTINGS,NOTE_INFO,0,NA_NONE,NULL,b);}
        frame(200);lv_obj_update_layout(qp_scroll);
        lv_obj_scroll_to_y(qp_scroll,200,LV_ANIM_OFF);frame(100);assert(lv_obj_get_scroll_y(qp_scroll)==200);
        touch_at(240,300,true);touch_at(240,330,true);touch_at(240,360,true);
        assert(g_state!=G_GROW && qx_px==0);touch_at(240,360,false);ccx_settle();
        lv_obj_scroll_to_y(qp_scroll,0,LV_ANIM_OFF);frame(100); }
    /* a tap on the empty foot, below the sliders, closes it, and it comes back folded */
    ccx_grow();{lv_area_t f;lv_obj_get_coords(qp_sp,&f);int fy=(f.y2+H)/2;touch_at(240,fy,true);touch_at(240,fy,false);}ccx_closed();
    /* so does a swipe up from the bottom edge, as ever */
    ccx_open();ccx_grow();
    touch_at(240,H-20,true);for(int y=H-60;y>=H-300;y-=40)touch_at(240,y,true);touch_at(240,H-300,false);ccx_closed();

    /* one long pull from the top: open, then grown */
    touch_at(240,4,true);
    for(int y=20;y<=690;y+=30)touch_at(240,y,true);
    assert(g_state==G_PULL && lv_obj_get_style_translate_y(qp,0)==0 && qx_px>0 && qx_px<qx_grow);
    lv_refr_now(disp);shot_now("cc-pull-grow.ppm");
    for(int y=720;y<=780;y+=30)touch_at(240,y,true);
    frame(120);touch_at(240,780,false);ccx_settle();assert(qp_open && qx_px==qx_grow);
    qp_close();ccx_closed();
    /* a quick pull just past mid-screen opens it folded, as before */
    touch_at(240,4,true);for(int y=34;y<=634;y+=40)touch_at(240,y,true);touch_at(240,634,false);
    ccx_settle();assert(qp_open && qx_px==0);
    /* nor one let go short of half grown, however fast */
    qp_close();ccx_closed();
    touch_at(240,4,true);for(int y=34;y<=674;y+=40)touch_at(240,y,true);assert(qx_px>0);
    touch_at(240,674,false);ccx_settle();assert(qp_open && qx_px==0);

    /* every toggle there is, on the real setting and back, the panel staying grown */
    sl_bt_known=true;sl_bt_demo_on=true;sl_bt_state.enabled=true;
    {   static const int tog[]={0,1,2,3,4,8,9,10,11,12,15};
        for(unsigned i=0;i<sizeof tog/sizeof tog[0];i++){
            int f=tog[i],k=ccx_tile(f),before=ccx_state(f),taps=0;
            if(!qx_px)ccx_grow();
            do{ccx_tap(qp_tiles[k]);while(wf_busy)frame(100);frame(250);taps++;
                if(taps==1)assert(ccx_state(f)!=before);
                bool on=qp_tile_on(f);lv_obj_t *c=lv_obj_get_child(qp_tiles[k],0);
                assert(lv_obj_get_style_bg_opa(c,0)==(on?LV_OPA_COVER:52));    /* the circle says so */
            }while(ccx_state(f)!=before && taps<8);
            assert(ccx_state(f)==before && qp_open && qx_px==qx_grow);
            printf("cc tile %-16s %d tap%s and back\n",qp_fn_names[f],taps,taps>1?"s":"");
        }
    }
    sl_bt_known=false;
    /* the pretend radio lost its network when it went off: join it again */
    sl_net_wifi_power(true);sl_net_wifi_connect("Home network",NULL);
    /* a tile in the grown rows, long pressed: its Settings page */
    {   static const struct { int f; const char *title; } lp[]={{12,"Wi-Fi"},{9,"Bluetooth"},{13,"Time and power"},{15,"Notification Center"},{14,"Theme"}};
        for(unsigned i=0;i<sizeof lp/sizeof lp[0];i++){
            if(!qp_open)ccx_open();
            if(!qx_px)ccx_grow();
            int x,y;ccx_center(qp_tiles[ccx_tile(lp[i].f)],&x,&y);
            int was=ccx_state(lp[i].f==14||lp[i].f==13?15:lp[i].f);
            for(int t=0;t<12;t++)touch_at(x,y,true);
            touch_at(x,y,false);frame(500);
            assert(npages==2 && has_text(pages[1],lp[i].title) && !qp_open);
            assert(ccx_state(lp[i].f==14||lp[i].f==13?15:lp[i].f)==was);        /* held: not toggled too */
            if(lp[i].f==14)shot("cc-long-press.ppm");
            page_pop();frame(500);ccx_closed();
        }
    }
    /* shortcuts go where they go */
    {   static const struct { int f; const char *title; } sc[]={{6,"Settings"},{13,"Time and power"},{7,NULL}};
        for(unsigned i=0;i<sizeof sc/sizeof sc[0];i++){
            ccx_open();ccx_grow();ccx_tap(qp_tiles[ccx_tile(sc[i].f)]);frame(500);
            assert(npages==2 && !qp_open && (!sc[i].title||has_text(pages[1],sc[i].title)));
            while(npages>1){page_pop();frame(500);}ccx_closed();
        }
        ccx_open();ccx_grow();ccx_tap(qp_tiles[ccx_tile(5)]);ccx_closed();   /* Lock: the panel goes first */
        if(sl_power_running){sl_power=power0;sl_power_apply();frame(200);}
    }
    /* Edit: the shortcuts page; Power: the power sheet, once the panel has gone */
    ccx_open();ccx_grow();ccx_tap(qx_edit);frame(500);
    assert(npages==2 && has_text(pages[1],"Notification Center") && !qp_open);page_pop();frame(500);ccx_closed();
    ccx_open();ccx_grow();ccx_tap(qx_power);frame(100);assert(!po_is_open());frame(300);
    assert(po_is_open() && !qp_is_visible());po_close_now();frame(300);ccx_closed();

    /* Dark mode: the panel goes, then the theme turns, on the home it was on */
    ccx_open();ccx_grow();ccx_tap(qp_tiles[ccx_tile(14)]);frame(400);frame(400);
    assert(global_settings.sloppy_theme==1 && npages==1 && !qp_open && qx_px==0);
    ccx_open();ccx_grow();shot("cc-expanded-light.ppm");
    assert(!qp_tile_on(14));
    ccx_tap(qp_tiles[ccx_tile(14)]);frame(400);frame(400);assert(global_settings.sloppy_theme==0 && npages==1);
    /* from Zule and back, its list home with it */
    global_settings.sloppy_theme=3;global_settings.sloppy_home_style=2;appearance_reload((void *)2);frame(700);
    ccx_open();ccx_grow();ccx_tap(qp_tiles[ccx_tile(14)]);frame(400);frame(400);
    assert(global_settings.sloppy_theme==1 && global_settings.sloppy_home_style==1);
    ccx_open();ccx_grow();ccx_tap(qp_tiles[ccx_tile(14)]);frame(400);frame(400);
    assert(global_settings.sloppy_theme==3 && global_settings.sloppy_home_style==2);

    /* every look: folded and grown */
    {   static const char *names[]={"dark","light","true-dark","zule"};
        for(int th=0;th<4;th++){
            global_settings.sloppy_theme=th;global_settings.sloppy_home_style=th==3?2:1;
            appearance_reload((void *)2);frame(700);frame(700);
            char b[48];ccx_open();snprintf(b,sizeof b,"cc-collapsed-%s.ppm",names[th]);shot(b);
            touch_at(240,200,true);touch_at(240,240,true);touch_at(240,320,true);lv_refr_now(disp);
            snprintf(b,sizeof b,"cc-mid-drag-%s.ppm",names[th]);shot_now(b);
            touch_at(240,460,true);frame(120);touch_at(240,460,false);ccx_settle();assert(qx_px==qx_grow);
            snprintf(b,sizeof b,"cc-expanded-%s.ppm",names[th]);shot(b);
            qp_close();ccx_closed();
        }
        global_settings.sloppy_theme=0;global_settings.sloppy_home_style=style0;appearance_reload((void *)2);frame(700);
    }
    /* three chosen: the rest of the first rows come with the grown card */
    strlcpy((char *)global_settings.sloppy_qp_tiles,"012",sizeof global_settings.sloppy_qp_tiles);quick_panel_rebuild();frame(100);
    ccx_open();assert(qp_n==3 && lv_obj_has_flag(qp_tiles[3],LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(qp_tiles[7],LV_OBJ_FLAG_HIDDEN));
    ccx_grow();assert(!lv_obj_has_flag(qp_tiles[3],LV_OBJ_FLAG_HIDDEN) && lv_obj_get_style_opa(qp_tiles[7],0)==LV_OPA_COVER);
    shot("cc-expanded-three.ppm");
    ccx_drag(240,500,300);assert(qx_px==0 && lv_obj_has_flag(qp_tiles[5],LV_OBJ_FLAG_HIDDEN));
    qp_close();ccx_closed();
    /* chosen and ordered (Settings writes it whole, with a '.'): sixteen are the panel as it was before
     * there were more kinds, one page, no dots, the old distance; twelve grow by a row; eight don't grow */
    strlcpy((char *)global_settings.sloppy_qp_tiles,"0123456789abcdef.",sizeof global_settings.sloppy_qp_tiles);quick_panel_rebuild();frame(100);
    assert(qp_total==16 && qp_n==8 && qx_pages==1 && !qx_dots && qx_grow==2*QX_ROW);
    ccx_open();ccx_grow();
    {   lv_area_t a;lv_obj_get_coords(qp_sp,&a);assert(a.y2<735);   /* the sliders end where they did */
        touch_at(400,300,true);touch_at(340,300,true);touch_at(250,300,true);assert(g_state!=G_CCPAGE);touch_at(250,300,false);ccx_settle();
        assert(lv_obj_get_scroll_x(qp_tp)==0 && qx_px==qx_grow); }
    touch_at(240,735,true);touch_at(240,735,false);ccx_closed();   /* the empty foot under the sliders, as ever */
    strlcpy((char *)global_settings.sloppy_qp_tiles,"c5a01b3e7f4.",sizeof global_settings.sloppy_qp_tiles);quick_panel_rebuild();frame(100);
    assert(qp_total==11 && qp_n==8 && qx_grow==QX_ROW && qp_fn[0]==12 && qp_fn[1]==5 && qp_fn[10]==4);   /* in the order chosen */
    ccx_open();ccx_grow();shot("cc-expanded-eleven.ppm");qp_close();ccx_closed();
    strlcpy((char *)global_settings.sloppy_qp_tiles,"3210.",sizeof global_settings.sloppy_qp_tiles);quick_panel_rebuild();frame(100);
    assert(qp_total==4 && qp_n==4 && !qx_grow);
    ccx_open();touch_at(240,200,true);touch_at(240,240,true);touch_at(240,300,true);assert(g_state!=G_GROW && qx_px==0);
    touch_at(240,300,false);ccx_settle();qp_close();ccx_closed();
    global_settings.sloppy_qp_tiles[0]='\0';quick_panel_rebuild();frame(100);

    /* open, grow, fold, close, again and again: nothing left behind */
    ccx_open();qp_close();ccx_closed();
    objs=objects(qp);top=(int)lv_obj_get_child_count(lv_layer_top());
    malloc_statistics_t m0,m1;malloc_zone_statistics(NULL,&m0);
    for(int r=0;r<24;r++){
        if(r%3==2){touch_at(240,4,true);for(int y=20;y<=780;y+=40)touch_at(240,y,true);frame(120);touch_at(240,780,false);ccx_settle();assert(qx_px==qx_grow);}
        else{ccx_open();ccx_grow();}
        if(r&1){ccx_drag(240,500,300);assert(qx_px==0);ccx_grow();}
        switch(r%3){
            case 0: qp_close();break;
            case 1: {lv_area_t f;lv_obj_get_coords(qp_sp,&f);int fy=(f.y2+H)/2;touch_at(240,fy,true);touch_at(240,fy,false);}break;
            case 2: touch_at(240,H-20,true);for(int y=H-60;y>=H-300;y-=40)touch_at(240,y,true);touch_at(240,H-300,false);break;
        }
        ccx_closed();
        assert(objects(qp)==objs && (int)lv_obj_get_child_count(lv_layer_top())==top);
    }
    frame(1000);assert(lv_anim_count_running()==0);
    malloc_zone_statistics(NULL,&m1);
    printf("cc grow: 24 cycles, %d objects, heap %+ld KiB\n",objs,((long)m1.size_in_use-(long)m0.size_in_use)/1024);
#if !__has_feature(address_sanitizer)
    assert((long)m1.size_in_use-(long)m0.size_in_use<64*1024);
#endif
    global_settings.sloppy_theme=theme0;global_settings.sloppy_home_style=style0;
    appearance_reload(NULL);frame(700);while(npages>1){page_pop();frame(400);}
    memcpy(nh.list,notes0,sizeof notes0);nh.n=nnotes0;nh.next_id=next0;qp_notes_refresh();
    global_settings.brightness=bright0;backlight_set_brightness(bright0);frame(100);
    cc_label_tests();
    printf("PASS cc grow\n");
}
/* SLOPPY_PERF: the panel growing under the finger, folding on a flick, and one long pull */
static void cc_expand_perf(int reps)
{
    for(int r=0;r<5*reps;r++){
        t_x=240;t_y=4;t_down=true;frame(16);for(int y=4;y<=500;y+=60){t_y=y;frame(16);}t_down=false;for(int i=0;i<30;i++)frame(16);
        assert(qp_open);
        if(r==0)pf_begin("cc grow drag");
        t_x=240;t_y=200;t_down=true;pf_frame(16);
        for(int y=200;y<=460;y+=13){t_y=y;pf_frame(16);}
        t_down=false;pf_frame(16);for(int i=0;i<25;i++)pf_frame(16);
        if(r==0){pf_end();pf_begin("cc fold flick");}
        t_x=240;t_y=500;t_down=true;pf_frame(16);
        for(int y=500;y>=380;y-=30){t_y=y;pf_frame(16);}
        t_down=false;pf_frame(16);for(int i=0;i<25;i++)pf_frame(16);
        assert(qp_open && qx_px==0);
        qp_close();for(int i=0;i<30;i++)frame(16);
        if(r==0){pf_end();pf_begin("cc long pull");}
        t_x=240;t_y=4;t_down=true;pf_frame(16);
        for(int y=4;y<=784;y+=30){t_y=y;pf_frame(16);}
        for(int i=0;i<8;i++)pf_frame(16);t_down=false;pf_frame(16);for(int i=0;i<25;i++)pf_frame(16);
        assert(qp_open && qx_px==qx_grow);
        if(r==0){pf_end();pf_begin("cc page turn");}
        t_x=420;t_y=300;t_down=true;pf_frame(16);
        for(int x=420;x>=120;x-=25){t_x=x;pf_frame(16);}
        t_down=false;pf_frame(16);for(int i=0;i<25;i++)pf_frame(16);
        t_x=120;t_y=300;t_down=true;pf_frame(16);
        for(int x=120;x<=420;x+=25){t_x=x;pf_frame(16);}
        t_down=false;pf_frame(16);for(int i=0;i<25;i++)pf_frame(16);
        assert(qx_page==0);
        if(r==0)pf_end();
        qp_close();for(int i=0;i<30;i++)frame(16);
        if(r==0){pf_begin("cc grow repeat");}
    }
    pf_end();
}
/* ---- end of the control centre, pulled further ---- */
static void chess_center(int sq, int *x, int *y)
{ int c=chess_cell_of(sq); *x=CHESS_BX+(c&7)*CHESS_SQ+CHESS_SQ/2; *y=CHESS_BOARD_Y+(c>>3)*CHESS_SQ+CHESS_SQ/2; }
/* the computer thinks on its own thread, against the real clock */
static void chess_wait_reply(void) { for(int i=0;i<800 && (chess_thinking||chess_job.busy);i++){usleep(5000);frame(30);} frame(400); }
/* ---- taste-swipe: the Your taste deck (helpers) ---- */
static lv_obj_tree_walk_res_t tsw_count_cb(lv_obj_t *o, void *n) { (void)o; (*(int *)n)++; return LV_OBJ_TREE_WALK_NEXT; }
static int tsw_objs(void) { int n = 0; lv_obj_tree_walk(lv_screen_active(), tsw_count_cb, &n); return n; }
/* a finger on the top card: down at its middle, through (dx, dy) in steps px a 16 ms frame, held, lifted */
static void tsw_drag(int dx, int dy, int steps, int hold_ms, bool lift) {
    lv_area_t a; lv_obj_get_coords(td_slot_of(td.at)->img, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    t_x = cx; t_y = cy; t_down = true; frame(16);
    for (int i = 1; i <= steps; i++) { t_x = cx + dx * i / steps; t_y = cy + dy * i / steps; frame(16); }
    for (int t = 0; t < hold_ms; t += 16) frame(16);
    if (lift) { t_down = false; frame(16); }
}
static void tsw_up(void) { t_down = false; frame(16); }
static void tsw_settle(void) { for (int i = 0; i < 60; i++) frame(16); }
/* ---- end taste-swipe helpers ---- */

/* ==== quiz-fix: helpers. The quiz shows every question whole. ==== */
/* l laid out as tall as its whole text, and inside box's content area. */
static int quizfix_whole(lv_obj_t *l, lv_obj_t *box, const char *what) {
    lv_point_t sz;lv_area_t a,b;
    lv_text_get_size(&sz,lv_label_get_text(l),lv_obj_get_style_text_font(l,0),lv_obj_get_style_text_letter_space(l,0),
                     lv_obj_get_style_text_line_space(l,0),lv_obj_get_content_width(l),LV_TEXT_FLAG_NONE);
    lv_obj_get_coords(l,&a);lv_obj_get_content_coords(box,&b);
    bool ok=lv_obj_get_content_height(l)>=sz.y && a.y1>=b.y1 && a.y2<=b.y2 && a.x1>=b.x1 && a.x2<=b.x2;
    if(!ok)printf("quiz-fix: %s: clipped, text %d px, label %d px at y %d..%d in %d..%d: %s\n",what,(int)sz.y,
                  (int)lv_obj_get_content_height(l),(int)a.y1,(int)a.y2,(int)b.y1,(int)b.y2,lv_label_get_text(l));
    return !ok;
}
static bool quizfix_meet(const lv_area_t *a, const lv_area_t *b) {
    return a->x1<=b->x2 && b->x1<=a->x2 && a->y1<=b->y2 && b->y1<=a->y2;
}
/* The question whole on the stage, clear of the stage's other text; every
 * answer below the stage, a usable size, on the screen. */
static int quizfix_check(const char *what) {
    lv_obj_update_layout(quiz_page_obj);
    int bad=quizfix_whole(quiz_question,quiz_stage,what);
    lv_area_t q,o,st,c,a;
    lv_obj_get_coords(quiz_question,&q);lv_obj_get_coords(quiz_stage,&st);
    lv_obj_t *others[]={quiz_mode_label,quiz_timer_label,quiz_feedback,quiz_next_button};
    for(int i=0;i<4;i++){
        if(lv_obj_has_flag(others[i],LV_OBJ_FLAG_HIDDEN))continue;
        lv_obj_get_coords(others[i],&o);
        if(quizfix_meet(&q,&o)){printf("quiz-fix: %s: question overlaps stage item %d\n",what,i);bad++;}
    }
    if(st.y2>H-8){printf("quiz-fix: %s: stage off the screen\n",what);bad++;}
    for(int i=0;i<quiz.choice_count;i++){
        lv_obj_get_coords(quiz_choices[i],&c);lv_obj_get_coords(lv_obj_get_child(quiz_choices[i],0),&a);
        if(c.y1<=st.y2 || c.y2>H-8 || lv_obj_get_height(quiz_choices[i])<QUIZ_ROW_MIN || a.y1<c.y1 || a.y2>c.y2)
        {printf("quiz-fix: %s: answer %d at %d..%d, its text %d..%d\n",what,i,(int)c.y1,(int)c.y2,(int)a.y1,(int)a.y2);bad++;}
    }
    return bad;
}
/* The bonus prompt and claim whole in their card, the round on the screen
 * without scrolling, each option inside its button. */
static int quizfix_bonus(const char *what) {
    lv_obj_update_layout(quiz_page_obj);
    int bad=quizfix_whole(quiz_bonus_prompt,quiz_bonus_card,what);
    if(!lv_obj_has_flag(quiz_bonus_big,LV_OBJ_FLAG_HIDDEN))bad+=quizfix_whole(quiz_bonus_big,quiz_bonus_card,what);
    if(lv_obj_get_scroll_top(quiz_overlay)>0||lv_obj_get_scroll_bottom(quiz_overlay)>0)
    {printf("quiz-fix: %s: the round needs scrolling\n",what);bad++;}
    for(int i=0;i<2;i++){
        lv_area_t a,b;lv_obj_get_coords(lv_obj_get_child(quiz_bonus_btn[i],0),&a);lv_obj_get_coords(quiz_bonus_btn[i],&b);
        if(a.y1<b.y1||a.y2>b.y2||a.x1<b.x1||a.x2>b.x2){printf("quiz-fix: %s: option %d spills\n",what,i);bad++;}
    }
    return bad;
}
/* ==== end quiz-fix helpers ==== */
/* ---- scrobbling (scrobble): a fake ListenBrainz ---- */
static int lbf_requests, lbf_answer = 200;
static char lbf_url[128], lbf_body[16384];
static int lbf(const char *url, const char *auth, const char *body, char *out, size_t cap)
{
    lbf_requests++;
    snprintf(lbf_url, sizeof lbf_url, "%s", url);
    snprintf(lbf_body, sizeof lbf_body, "%s", body ? body : "");
    if (strstr(url, "validate-token"))
    {
        snprintf(out, cap, strstr(auth, "Token good") ? "{\"code\":200,\"message\":\"Token valid.\",\"valid\":true,\"user_name\":\"tai\"}"
                                                      : "{\"code\":200,\"message\":\"Token invalid.\",\"valid\":false}");
        return 200;
    }
    if (lbf_answer == 200) snprintf(out, cap, "{\"status\":\"ok\"}");
    return lbf_answer;
}
static void lbf_play(const char *path, const char *title, unsigned long ms)
{
    strlcpy(test_track.path, path, sizeof test_track.path);
    test_track.title = (char *)title;
    test_track.elapsed = 0;
    for (unsigned long t = 0; t <= ms; t += 250) { hist_poll(); test_track.elapsed += 250; }
}
static bool lbf_log_has(const char *needle);   /* in sloppy.log, on the fake card */
static void lbf_log_print(void);
/* ---- end scrobbling ---- */
static void fake_put(const char *p, const void *d, size_t n);
static void fake_card_reset(void);
static bool fake_has(const char *p, const char *needle);
static bool fake_any(const char *in_path);
#include "ui_audit.inc"
#include "batch_e.inc"
#include "batch_f.inc"
#include "batch_i.inc"
#include "batch_h.inc"
static long fake_size(const char *p);
#include "batch_wave.inc"
#include "batch_epub.inc"
#include "batch_sub.inc"
#include "batch_abs.inc"
#include "batch_remote.inc"
#include "batch_ota.inc"
/* ---- taste-shots: SLOPPY_TASTE_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: the deck at rest and a
 * swipe as a run of frames (mid-drag, the throw, the next card, a spring back, undo), dark, light and Zule ---- */
static void taste_shot(const char *dir, const char *look, int *k, const char *what, const char *tag) {
    char p[600]; snprintf(p, sizeof p, "%s/taste-%s-%02d-%s-%s.ppm", dir, look, (*k)++, what, tag); shot_now(p);
}
static void taste_shots(const char *dir, const char *tag) {
    static const struct { const char *name; int look; } looks[] = { { "dark", 0 }, { "light", 1 }, { "zule", 6 } };
    char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
    au_library();
    for (unsigned i = 0; i < sizeof looks / sizeof looks[0]; i++) {
        au_apply(looks[i].look, 0, 0);
        test_status = AUDIO_STATUS_PLAY; mini_update(); global_settings.sloppy_notifications = 0;
        ts_load(); int rated0 = ts.n_rated; ts.n_rated = 0; ts.gen++;
        taste_page(false); tsw_settle();
        int k = 1; const char *L = looks[i].name;
        taste_shot(dir, L, &k, "stack", tag);
        lv_area_t a; lv_obj_get_coords(td_slot_of(td.at)->img, &a);
        int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
        /* a slow drag right, past the line; let go: it flies off and the next card comes up */
        t_x = cx; t_y = cy; t_down = true; frame(16);
        for (int s = 1; s <= 30; s++) { t_x = cx + s * 5; t_y = cy + s / 3; frame(16); if (s == 8 || s == 18 || s == 30) taste_shot(dir, L, &k, "drag", tag); }
        t_down = false; frame(16);
        for (int f = 1; f <= 12; f++) { frame(16); if (f == 2 || f == 5 || f == 9) taste_shot(dir, L, &k, "throw", tag); }
        tsw_settle(); taste_shot(dir, L, &k, "next", tag);
        /* short of the line: it springs back, just past the middle once */
        t_x = cx; t_y = cy; t_down = true; frame(16);
        for (int s = 1; s <= 14; s++) { t_x = cx - s * 5; frame(16); }
        for (int f = 0; f < 10; f++) frame(16);
        taste_shot(dir, L, &k, "held", tag);
        t_down = false; frame(16);
        for (int f = 1; f <= 14; f++) { frame(16); if (f == 3 || f == 7 || f == 14) taste_shot(dir, L, &k, "spring", tag); }
        tsw_settle();
#ifndef TASTE_BEFORE
        /* up: love */
        t_x = cx; t_y = cy; t_down = true; frame(16);
        for (int s = 1; s <= 20; s++) { t_x = cx + s / 3; t_y = cy - s * 8; frame(16); }
        for (int f = 0; f < 10; f++) frame(16);
        taste_shot(dir, L, &k, "love", tag);
        t_down = false; frame(16);
        for (int f = 1; f <= 6; f++) { frame(16); if (f == 3) taste_shot(dir, L, &k, "love-throw", tag); }
        tsw_settle();
#endif
        /* undo: the last card flies back in */
        key_tap(td.btn[2]); for (int f = 0; f < 4; f++) frame(16);
        taste_shot(dir, L, &k, "undo", tag);
        tsw_settle(); taste_shot(dir, L, &k, "undone", tag);
        page_pop(); frame(500);
        ts.n_rated = rated0; ts.gen++;
        au_reset(); au_empty(false);
    }
    au_library_done();
}
/* ---- end taste-shots ---- */
#include "batch_taste.inc"
#include "batch_showcase.inc"
/* the letter packs (sloppy_xfont.inc) on the fake card, as the R1's zip puts them there */
static void test_packs(void) {
    if (getenv("SLOPPY_NO_PACKS")) return;   /* the card as it was before the packs: for "before" shots */
    char dir[600]; strlcpy(dir, __FILE__, sizeof dir); char *sl = strrchr(dir, '/'); if (sl) *sl = '\0';
    for (unsigned i = 0; i < XF_PACKS; i++) {   /* every pack the player reads */
        char src[700], dst[100]; snprintf(src, sizeof src, "%s/../fonts/packs/%s.slf", dir, xf_packs[i].name);
        FILE *f = fopen(src, "rb"); assert(f);
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        void *b = malloc(n); assert(fread(b, 1, n, f) == (size_t)n); fclose(f);
        snprintf(dst, sizeof dst, ROCKBOX_DIR "/sloppy/fonts/%s.slf", xf_packs[i].name); fake_put(dst, b, n); free(b);
    }
}
/* SLOPPY_B9_PERF=1: frames of the batch 9 games, at the R1's estimated cost */
static void b9_perf(void) {
    arc_root=FAKE_CARD;
    static uint8_t rom[32768];
    mkdir(FAKE_CARD "/Games");mkdir(FAKE_CARD "/Games/GB");
    gbt_make(rom,"PERF",1);
    int fd=open(FAKE_CARD "/Games/GB/Perf.gb",O_WRONLY|O_CREAT|O_TRUNC,0666);if(fd>=0){if(write(fd,rom,sizeof rom)<0){}sim_close(fd);}
    arcade_page();frame(300);arc_play(0);frame(300);
    pf_begin("arcade game");for(int i=0;i<600;i++){if(i%40==0)arc.touch^=GB_RIGHT;pf_frame(i%3==2?16:17);}pf_end();
    uint64_t t0=sl_perf_now_us();for(int i=0;i<600;i++)gb_frame(0,true);
    printf("arcade core alone: %.3f ms a frame on this Mac (x%.0f: %.1f ms est. on the R1)\n",(sl_perf_now_us()-t0)/600000.0,PF_K,(sl_perf_now_us()-t0)/600000.0*PF_K);
    t0=sl_perf_now_us();for(int i=0;i<600;i++)arc_blit();
    printf("arcade 3x scale: %.3f ms a frame on this Mac (%.1f ms est. on the R1)\n",(sl_perf_now_us()-t0)/600000.0,(sl_perf_now_us()-t0)/600000.0*PF_K);
    page_pop();frame(300);page_pop();frame(300);
    fl_loaded=true;flappy_page();frame(400);fl_flap();
    /* through the pipes for ten seconds: the gaps kept where the bird flies */
    pf_begin("flappy flying");for(int i=0;i<600;i++){
        for(int k=0;k<FL_PIPES;k++)if(fl_pipe_sx(&fl.pipe[k],fl_cam_px())>=FB_SW)fl.pipe[k].gap=70;
        if(fl.y>100)fl_flap();pf_frame(i%3==2?16:17);}pf_end();
    assert(fl.state==FL_FLY && fl.score>=6);
    /* the end of a round: the hit, the fall, Game Over, the panel rising, the count */
    fl.score=31;fl_die(true);
    pf_begin("flappy game over");for(int i=0;i<220;i++)pf_frame(i%3==2?16:17);pf_end();
    assert(fl.state==FL_OVER);
    page_pop();frame(300);
    snake_page();frame(100);snake_steer(1,0);
    pf_begin("snake running");for(int i=0;i<300;i++){if(i%30==0)snake_steer(i%60?0:1,i%60?1:0);if(i%30==15)snake_steer(i%60<30?-1:1,0);pf_frame(16);}pf_end();
    page_pop();frame(300);
    t48_page();frame(100);
    pf_begin("2048 slides");for(int i=0;i<200;i++){if(i%12==0)t48_move((i/12)%4);pf_frame(16);}pf_end();
    page_pop();frame(300);
    sdk_loaded=true;sudoku_page();frame(100);
    pf_begin("sudoku taps");for(int i=0;i<120;i++){if(i%6==0){sdk_sel=(i*7)%81;sdk_show();}pf_frame(16);}pf_end();
    page_pop();frame(300);
}
static void b9_tests(void) {
        {   /* Arcade: cartridges from Games/GB and Games/GBC, played here with sound, keys, saves and the place kept */
            while(npages>1){page_pop();frame(300);}
            sl_games_page();frame(300);assert(find_label(pages[npages-1],"Arcade"));
            arc_root=FAKE_CARD;arc_last[0]=0;
            arcade_page();frame(300);
            assert(find_label(pages[npages-1],"No games on the card yet.") && has_text(pages[npages-1],"Games/GB") &&
                   find_label(pages[npages-1],"Over Wi-Fi") && find_label(pages[npages-1],"With a cable") && find_label(pages[npages-1],"Doom"));
            shot("arcade-empty.ppm");
            page_pop();frame(300);
            static uint8_t rom[32768];
            mkdir(FAKE_CARD "/Games");mkdir(FAKE_CARD "/Games/GB");mkdir(FAKE_CARD "/Games/GBC");
            gbt_make(rom,"SLOPPYTEST",0);
            int fd=open(FAKE_CARD "/Games/GB/Test Cart (World) (Rev 1).gb",O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);assert(write(fd,rom,sizeof rom)==(ssize_t)sizeof rom);sim_close(fd);
            gbt_make(rom,"COLORTEST",1);
            fd=open(FAKE_CARD "/Games/GBC/Color Test.gbc",O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);assert(write(fd,rom,sizeof rom)==(ssize_t)sizeof rom);sim_close(fd);
            fd=open(FAKE_CARD "/Games/GB/._Test Cart (World) (Rev 1).gb",O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);sim_close(fd);   /* a Mac's shadow: not a game */
            arcade_page();frame(300);
            assert(arc_n==2 && arc_roms[0].color && !strcmp(arc_roms[1].name,"Test Cart"));
            assert(find_label(pages[npages-1],"GAME BOY COLOR") && find_label(pages[npages-1],"GAME BOY") && find_label(pages[npages-1],"Test Cart"));
            assert(!find_label(pages[npages-1],"CONTINUE PLAYING"));
            shot("arcade-list.ppm");
            /* the pad under the finger */
            assert(arc_keys_at(ARC_DPAD_X,ARC_DPAD_Y-60)==GB_UP && arc_keys_at(ARC_DPAD_X+60,ARC_DPAD_Y)==GB_RIGHT);
            assert(arc_keys_at(ARC_DPAD_X+50,ARC_DPAD_Y+50)==(GB_RIGHT|GB_DOWN) && arc_keys_at(ARC_DPAD_X,ARC_DPAD_Y)==0);
            assert(arc_keys_at(ARC_A_X,ARC_A_Y)==GB_A && arc_keys_at(ARC_B_X,ARC_B_Y)==GB_B && arc_keys_at((ARC_A_X+ARC_B_X)/2,(ARC_A_Y+ARC_B_Y)/2)==(GB_A|GB_B));
            assert(arc_keys_at(ARC_SEL_X,ARC_SS_Y)==GB_SELECT && arc_keys_at(ARC_START_X,ARC_SS_Y)==GB_START);
            test_status=AUDIO_STATUS_PLAY;test_voice_more=NULL;
            if(np_open){np_hide(0);frame(500);}
            int k=1;arc_play(k);frame(100);
            assert(arc.page && arc_on_top() && gb_running() && !gb_color() && (test_status&AUDIO_STATUS_PAUSE));   /* the song waits */
            assert(test_voice_more && arc_sound_on);
            for(int i=0;i<60;i++)frame(17);
            if(arc.frames<50)printf("arcade dbg: frames %u paused %d sheet %d hides %d top %d ov %d np %d acts %d\n",arc.frames,arc.paused,arc.sheet,sl_power_hides_ui(),arc_on_top(),overlay_visible(),np_open,acts_open_now);
            assert(arc.frames>=50);
            {const uint16_t *pic=gb_picture();int diff=0;for(int i=1;i<GB_W*GB_H;i++)diff+=pic[i]!=pic[0];assert(diff>1000);}
            {   /* the game's square wave reaches the mixer */
                const void *st;size_t sz;long nz=0;
                for(int i=0;i<8;i++){test_voice_more(&st,&sz);const int16_t *q=st;for(size_t j=0;j<sz/2;j++)nz+=q[j]!=0;}
                assert(nz>100);
            }
            /* the R1's keys: Play is A, Next is B */
            assert(arcade_key(BUTTON_LEFT,false) && (arc.keys&GB_A));
            assert(arcade_key(BUTTON_LEFT,true) && !(arc.keys&GB_A));
            assert(arcade_key(BUTTON_RIGHT,false) && (arc.keys&GB_B));arcade_key(BUTTON_RIGHT,true);
            arc.touch=GB_UP;for(int i=0;i<6;i++)frame(17);arc.touch=0;
            shot("arcade-game.ppm");
            /* the menu: the game stops, a state is saved by hand */
            uint32_t f0=arc.frames;arc_screen_cb(NULL);frame(300);assert(arc.sheet && arc.paused && acts_open_now);
            shot("arcade-menu.ppm");
            for(int i=0;i<10;i++)frame(17);assert(arc.frames==f0);
            arc_save_cb(NULL);frame(300);assert(arc_has_state(arc_roms[k].path,".state"));
            for(int i=0;i<4;i++)frame(17);assert(!arc.sheet && !arc.paused);
            /* leaving keeps the place, the picture and the battery save (Up held as it goes) */
            arc.touch=GB_UP;for(int i=0;i<4;i++)frame(17);
            page_pop();frame(400);
            assert(!arc.page && !gb_running() && arc_has_state(FAKE_CARD "/Games/GB/Test Cart (World) (Rev 1).gb",".resume") &&
                   arc_has_state(FAKE_CARD "/Games/GB/Test Cart (World) (Rev 1).gb",".thumb"));
            {unsigned char sv[2]={0};int f=open(FAKE_CARD "/Games/GB/Test Cart (World) (Rev 1).sav",O_RDONLY);assert(f>=0);assert(read(f,sv,2)==2);sim_close(f);
             assert(sv[0]==0x42 && sv[1]==0x04);}   /* the cartridge's own RAM: its mark, and Up held */
            frame(300);
            assert(find_label(pages[npages-1],"CONTINUE PLAYING") && find_label(pages[npages-1],"Game Boy \xc2\xb7 Saved where you left it"));
            shot("arcade-continue.ppm");
            arc_play(1);frame(100);assert(arc.page && arc.note && find_label(arc.page,"Back where you left off"));
            page_pop();frame(400);
            {   /* a place cut short by a power cut: refused whole, the game starts
                 * afresh; saves go through a .new, none left beside them */
                char rp[MAX_PATH],np[MAX_PATH+4];arc_state_path(FAKE_CARD "/Games/GB/Test Cart (World) (Rev 1).gb",".resume",rp,sizeof rp);
                snprintf(np,sizeof np,"%s.new",rp);assert(fake_find(np)<0 && fake_size(rp)>4096);
                {static char cut[1000];memset(cut,0x5a,sizeof cut);fake_put(rp,cut,sizeof cut);}
                arc_play(1);frame(100);assert(arc.page && gb_running() && !find_label(arc.page,"Back where you left off"));
                for(int i=0;i<10;i++)frame(17);
                page_pop();frame(400);assert(fake_size(rp)>4096 && fake_find(np)<0);
            }
            {   /* an MBC2 cartridge: no RAM in its header, its RAM in the chip, written all the same
                 * (the cart's code writes A000): the emulator gave it 0 bytes and took 8 KB past them */
                const char *rp=FAKE_CARD "/Games/GB/Test Cart (World) (Rev 1).gb";
                gbt_make(rom,"SLOPPYTEST",0);rom[0x147]=0x05;rom[0x149]=0x00;
                int fd=open(rp,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);assert(write(fd,rom,sizeof rom)==(ssize_t)sizeof rom);sim_close(fd);
                arc_play(1);frame(100);assert(arc.page && gb_running());
                for(int i=0;i<30;i++)frame(17);
                assert(arc.frames>=20);
                page_pop();frame(400);
                gbt_make(rom,"SLOPPYTEST",0);
                fd=open(rp,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);assert(write(fd,rom,sizeof rom)==(ssize_t)sizeof rom);sim_close(fd);
                printf("PASS: an MBC2 cartridge (no RAM in its header) runs and writes its RAM inside a bank of its own.\n");
            }
            {   /* Doom: its file, then it leaves for classic Rockbox and comes back */
                doom_open();frame(300);
                if(file_exists(ARC_DOOM_WAD)){
                    lv_obj_t *go=find_label(pages[npages-1],"Start it now");assert(go && !want_plugin);
                    lv_obj_send_event(lv_obj_get_parent(go),LV_EVENT_CLICKED,NULL);frame(100);
                    assert(want_classic && want_plugin && strstr(want_plugin,"doom.rock") && !want_plugin_arg);
                    want_classic=false;want_plugin=NULL;
                } else assert(find_label(pages[npages-1],"ONE FILE MISSING"));
            }
            while(npages>1){page_pop();frame(300);}
            test_status=0;
            printf("PASS: Arcade: the empty state, cartridges by system, play with sound and keys, the menu, the place kept.\n");
        }
    {   /* Snake: three turns wait their steps, the pace is chosen and kept, every fifth apple is golden and worth 3 */
        struct sl_snake_state st;
        sl_snake_init(&st,false,7);
        assert(sl_snake_period(&st)==190);
        sl_snake_turn(&st,0,-1);sl_snake_turn(&st,-1,0);sl_snake_turn(&st,0,1);sl_snake_turn(&st,1,0);
        assert(st.next_dy==-1 && st.queued==3 && st.queue_dx[0]==-1 && st.queue_dy[1]==1 && st.queue_dx[2]==1);
        sl_snake_turn(&st,0,-1);assert(st.queued==3);                 /* a fourth waits for room */
        sl_snake_step(&st);assert(st.dy==-1 && st.next_dx==-1 && st.queued==2);
        sl_snake_init(&st,false,7);sl_snake_turn(&st,0,-1);sl_snake_turn(&st,0,-1);sl_snake_turn(&st,0,1);
        assert(st.queued==0);                                         /* the same way again, or back onto itself: no turn */
        struct sl_snake_state r=st;r.speed=SL_SNAKE_RELAXED;struct sl_snake_state f=st;f.speed=SL_SNAKE_FAST;
        assert(sl_snake_period(&r)>sl_snake_period(&st) && sl_snake_period(&f)<sl_snake_period(&st));
        sl_snake_init(&st,true,7);st.eaten=4;sl_snake_food(&st);assert(st.food_gold);
        st.food_x=st.x[0]+1;st.food_y=st.y[0];st.score=8;
        assert(sl_snake_step(&st)==SL_SNAKE_ATE && st.score==11 && st.level==2 && !st.food_gold);   /* 8 + 3 crosses ten: the next level */
        snake_page();frame(100);
        lv_obj_send_event(snake_mode_buttons[0],LV_EVENT_CLICKED,NULL);frame(50);
        lv_obj_send_event(snake_speed_chips[2],LV_EVENT_CLICKED,NULL);frame(50);
        assert(snake_speed==SL_SNAKE_FAST && snake_state.speed==SL_SNAKE_FAST && fake_has(SNAKE_SCORES,"SNAKE2"));
        snake_speed=SL_SNAKE_NORMAL;snake_scores_loaded=false;snake_scores_load();assert(snake_speed==SL_SNAKE_FAST);
        lv_obj_send_event(snake_speed_chips[1],LV_EVENT_CLICKED,NULL);frame(50);assert(snake_speed==SL_SNAKE_NORMAL);
        /* a golden apple: +3, its own sound, and its clock runs out into a red one */
        snake_reset(false);snake_steer(1,0);assert(snake_running());
        /* the first apple is placed from the clock: when it lay right ahead, that first step ate it */
        int gold0=snake_state.score;
        snake_state.food_gold=true;snk_gold_at=snk_now();snake_state.food_x=snake_state.x[0]+1;snake_state.food_y=snake_state.y[0];
        snake_state.next_dx=1;snake_state.next_dy=0;snake_state.queued=0;
        SFX_RESET();snake_tick(NULL);assert(snake_state.score==gold0+3 && sfx_last==SFX_SNAKE_GOLD && !strcmp(lv_label_get_text(snake_plus),"+3"));
        snake_state.food_gold=true;snk_gold_at=snk_now()-SNK_GOLD_MS-10;frame(20);assert(!snake_state.food_gold);
        /* the crash flashes the board */
        snake_state.x[0]=SL_SNAKE_GRID-1;snake_state.next_dx=1;snake_state.next_dy=0;snake_tick(NULL);
        assert(!snake_state.alive && !lv_obj_has_flag(snake_flash,LV_OBJ_FLAG_HIDDEN));
        for(int i=0;i<60;i++)frame(16);
        assert(lv_obj_has_flag(snake_flash,LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(snake_speed_row,LV_OBJ_FLAG_HIDDEN));
        page_pop();frame(300);
        printf("PASS: Snake: a three-turn queue, Relaxed / Normal / Fast kept on the card, golden apples, the red flash.\n");
    }
    {   /* 2048: slides join twins once, a swipe mid-slide waits its turn, undo, the board kept */
        t48_page();frame(100);
        static const uint8_t v0[16]={1,1,2,0, 0,0,0,0, 0,0,0,0, 0,0,0,3};
        memcpy(t48.v,v0,16);t48.score=0;t48_over=false;t48_render(false);
        t48_rng=1;   /* the spawn is seeded from the real clock: pinned, so the new tile lands off the top row */
        t48_move(3);assert(t48.v[0]==2 && t48.v[1]==2 && t48.v[2]==0 && t48.score==4 && t48_slide_timer && t48_born>3);
        t48_move(1);assert(t48_queued==1);                            /* came during the slide: next */
        const int slide_frames=(T48_SLIDE_MS+10+15)/16;               /* the slide's timer, in simulated 16 ms frames */
        for(int i=0;i<slide_frames;i++)frame(16);
        assert(t48_queued==-1 && t48_slide_timer && t48.score==12);  /* the first slide ended, the queued one began */
        for(int i=0;i<slide_frames;i++)frame(16);
        assert(t48.v[3]==3 && t48.score==12 && !t48_slide_timer);        /* 4 + 4 joined on the right */
        t48_undo_cb(NULL);assert(t48.v[0]==2 && t48.v[1]==2);
        assert(t48_key(BUTTON_LEFT,false));for(int i=0;i<20;i++)frame(16);assert(t48.v[0]==3);
        static const uint8_t stuck[16]={1,2,1,2, 2,1,2,1, 1,2,1,2, 2,1,2,1};
        struct t48 g;memcpy(g.v,stuck,16);assert(t48_stuck(&g));g.v[0]=2;assert(!t48_stuck(&g));
        uint8_t keep[16];memcpy(keep,t48.v,16);uint32_t sc=t48.score;
        page_pop();frame(300);
        t48_loaded=false;memset(&t48,0,sizeof t48);t48_load();assert(!memcmp(t48.v,keep,16) && t48.score==sc);
        printf("PASS: 2048: joins, queued swipes, undo, the keys, a stuck board, the game kept.\n");
    }
    {   /* Sudoku: one answer at every level, mistakes in red, notes cleared, hints, solved and timed */
        for(int d=0;d<4;d++){
            sdk_rng=1234+d;uint64_t t0=sl_perf_now_us();sdk_make(d);uint64_t us=sl_perf_now_us()-t0;
            int giv=0;for(int i=0;i<81;i++)giv+=sdk.given[i];
            uint8_t g[81];memcpy(g,sdk.val,81);assert(sdk_solve(g,2,false)==1);
            for(int i=0;i<81;i++)assert(!sdk.given[i]||sdk.val[i]==sdk.sol[i]);
            printf("sudoku %s: %d given, made in %.1f ms\n",sdk_diff_names[d],giv,us/1000.0);
            assert(giv<=sdk_givens[d]+4);
        }
        sdk_loaded=true;sdk_rng=77;sdk_make(0);sudoku_page();frame(100);
        int i=0;while(sdk.given[i])i++;
        sdk_sel=i;int wrong=sdk.sol[i]%9+1;sdk_put(wrong);assert(sdk_wrong(i) && sdk.mistakes==1);
        int j=-1;for(int k=0;k<81;k++)if(k!=i&&!sdk.given[k]&&sdk_peer(k,i)){j=k;break;}
        sdk_notes_mode=true;sdk_sel=j;sdk_put(sdk.sol[i]);assert(sdk.notes[j]&(1u<<sdk.sol[i]));sdk_notes_mode=false;
        sdk_sel=i;sdk_put(sdk.sol[i]);assert(!sdk_wrong(i) && !(sdk.notes[j]&(1u<<sdk.sol[i])));
        sdk_undo_cb(NULL);assert(sdk.val[i]==wrong);
        sdk_sel=i;sdk_hint_cb(NULL);assert(sdk.val[i]==sdk.sol[i] && sdk.hints==1);
        sdk.ms=0;for(int k=0;k<10;k++)frame(250);assert(sdk.ms>=2000);   /* the clock runs while it is seen */
        for(int k=0;k<81;k++)if(!sdk.given[k]&&sdk.val[k]!=sdk.sol[k]){sdk_sel=k;sdk_put(sdk.sol[k]);}
        assert(sdk.done && !lv_obj_has_flag(sdk_card,LV_OBJ_FLAG_HIDDEN));
        page_pop();frame(300);
        sdk_loaded=false;memset(&sdk,0,sizeof sdk);sdk_load();assert(!sdk.live);   /* a solved game is not resumed */
        sdk_loaded=true;sdk_rng=5;sdk_make(2);sdk.val[0]=sdk.given[0]?sdk.val[0]:0;sdk.ms=61000;sdk_save();
        uint8_t sol[81];memcpy(sol,sdk.sol,81);
        sdk_loaded=false;memset(&sdk,0,sizeof sdk);sdk_load();assert(sdk.live && sdk.diff==2 && sdk.ms==61000 && !memcmp(sdk.sol,sol,81));
        printf("PASS: Sudoku: unique puzzles at four levels, mistakes, notes, undo, hints, the clock, solved, kept.\n");
    }
    {   /* 24 Points: every deal can be made, cards join, 24 wins, division by nothing refused */
        struct pt_val v[4]={pt_make(8,1),pt_make(8,1),pt_make(3,1),pt_make(3,1)};assert(pt_solve(v,4,NULL,NULL,NULL));   /* 8 / (3 - 8/3) */
        struct pt_val o[4]={pt_make(1,1),pt_make(1,1),pt_make(1,1),pt_make(1,1)};assert(!pt_solve(o,4,NULL,NULL,NULL));
        pt_loaded=true;pt_solved=pt_streak=0;
        points_page();frame(100);
        for(int k=0;k<20;k++){pt_deal();struct pt_val d[4];for(int q=0;q<4;q++)d[q]=pt.c[q];assert(pt_solve(d,4,NULL,NULL,NULL));}
        static const int deal[4]={6,4,1,1};for(int k=0;k<4;k++){pt.deal[k]=deal[k];pt.c[k]=pt_make(deal[k],1);}pt.nhist=0;pt.won=false;pt_show();
        lv_obj_send_event(pt_cards[0],LV_EVENT_CLICKED,NULL);lv_obj_send_event(pt_ops[2],LV_EVENT_CLICKED,NULL);
        lv_obj_send_event(pt_cards[1],LV_EVENT_CLICKED,NULL);assert(pt.c[1].n==24 && !pt.c[0].live && pt.sel==1);
        pt_undo_cb(NULL);assert(pt.c[0].live && pt.c[1].n==4);
        pt.sel=0;pt.op=2;lv_obj_send_event(pt_cards[1],LV_EVENT_CLICKED,NULL);
        pt.op=2;lv_obj_send_event(pt_cards[2],LV_EVENT_CLICKED,NULL);
        pt.op=2;lv_obj_send_event(pt_cards[3],LV_EVENT_CLICKED,NULL);
        assert(pt.won && pt_solved==1 && pt_streak==1 && fake_has(PT_FILE,"PTS1 1 1 1"));
        frame(1400);assert(!pt.won && pt_live()==4);                  /* the next deal comes in */
        static const int zero[4]={5,5,1,1};for(int k=0;k<4;k++){pt.c[k]=pt_make(zero[k],1);}pt.nhist=0;
        pt.sel=0;pt.op=1;lv_obj_send_event(pt_cards[1],LV_EVENT_CLICKED,NULL);assert(pt.c[1].n==0);
        pt.sel=2;pt.op=3;lv_obj_send_event(pt_cards[1],LV_EVENT_CLICKED,NULL);assert(pt.c[2].live && pt.c[1].n==0);   /* 1 / 0: refused */
        pt_hint_cb(NULL);
        assert(!pt_next_timer);pt_new_cb(NULL);frame(100);assert(!pt_next_timer && pt_live()==4);   /* New deal with none on its way (it crashed) */
        page_pop();frame(300);
        printf("PASS: 24 Points: solvable deals, joins, undo, a win and its streak kept, 1/0 refused, New deal any time.\n");
    }
    {   /* Flappy Bird: the original's rules, a tap flaps at once, a point a pipe, a hit to the pixel, medals, the best kept */
        fl_loaded=true;fl_best=0;remove(FL_FILE);
        flappy_page();frame(100);
        assert(fl.state==FL_READY && !lv_obj_has_flag(fl.hdr,LV_OBJ_FLAG_HIDDEN));
        /* the rules: a flap sets the speed, gravity pulls, the fall tops out */
        float y0=fl.y;
        lv_obj_send_event(fl.canvas,LV_EVENT_PRESSED,NULL);   /* a tap flaps in its own event, before any frame */
        assert(fl.state==FL_FLY && fl.vy==FL_FLAP && fl.y==y0);
        assert(lv_obj_has_flag(fl.hdr,LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(fl.pause_btn,LV_OBJ_FLAG_HIDDEN));
        float top=fl.y;for(int i=0;i<60;i++){fl_step();if(fl.y<top)top=fl.y;}
        assert(fabsf((y0-top)-135.0f*135.0f/900.0f)<1.0f);           /* the climb: v^2 / 2g, 20 art px */
        fl.y=40;fl.vy=0;for(int i=0;i<72;i++)fl_step();
        assert(fl.state==FL_FLY && fl.vy==FL_TERMINAL && fl.rot>=89.9f);    /* 0.6 s later: full speed, nose down */
        fl.y=y0;fl.vy=0;fl.rot=0;
        /* a point as a pipe's middle passes the bird's */
        int cam=fl_cam_px();
        for(int k=0;k<FL_PIPES;k++){fl.pipe[k].x=(cam+FB_SW)/FB_S+40+k*FL_SPACING;fl.pipe[k].scored=false;}
        fl.pipe[0].x=(cam+FL_BIRD_X*FB_S-FB_LIP_W*FB_S/2+FB_S)/FB_S+1;fl.pipe[0].gap=(int)fl.y-FL_GAP/2;
        fl.vy=FL_FLAP;frame(16);frame(16);frame(16);
        assert(fl.score==1 && fl.state==FL_FLY);
        /* Play and Next flap; held, a key flaps once */
        fl.vy=50;assert(fl_key(BUTTON_RIGHT) && fl.vy==FL_FLAP);
        fl.vy=50;assert(fl_key(BUTTON_RIGHT|BUTTON_REPEAT) && fl.vy==50);
        assert(fl_key(BUTTON_RIGHT|BUTTON_REL));
        /* to the pixel: level, its top row right at a lip's foot is clear; a pixel higher hits */
        for(int k=0;k<FL_PIPES;k++)fl.pipe[k].x=(cam+FB_SW)/FB_S+40+k*FL_SPACING;
        fl.rot=0;fl.pipe[1].x=(fl_cam_px()+FL_BIRD_X*FB_S)/FB_S-FB_LIP_W/2;fl.pipe[1].gap=80;fl.pipe[1].scored=true;
        int hi=-1;
        for(float y=110;y>60&&hi<0;y-=1.0f/FB_S){fl.y=y;fl_bird_now(&fl_img);
            int ytop=-1;
            for(int yy=0;yy<fl_img.r.y1-fl_img.r.y0&&ytop<0;yy++)for(int xx=0;xx<fl_img.r.x1-fl_img.r.x0;xx++)if(fl_img.n[yy*FB_BC+xx]>=2){ytop=fl_img.r.y0+yy;break;}
            if(ytop==80*FB_S){hi=0;assert(!fl_hits_pipe());fl.y-=1.0f/FB_S;assert(fl_hits_pipe());}}
        assert(hi==0);
        /* turned, it hits by what is drawn: nose down, the bird is taller than it is level */
        fl.rot=90;fl.y=80+FB_BIRD_W/2.0f-1;assert(fl_hits_pipe());fl.y=80+FB_BIRD_W/2.0f+1;assert(!fl_hits_pipe());
        fl.rot=0;assert(!fl_hits_pipe());
        /* paused by its button, a tap carries on */
        fl.y=y0;fl.vy=0;fl.pipe[1].x+=200;
        lv_obj_send_event(fl.pause_btn,LV_EVENT_CLICKED,NULL);assert(fl.state==FL_PAUSE && !lv_obj_has_flag(fl.play_btn,LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(fl.canvas,LV_EVENT_PRESSED,NULL);assert(fl.state==FL_FLY);
        /* a pipe ends it: the fall, Game Over, the best saved, Play again */
        fl.pipe[1].x=(fl_cam_px()+FL_BIRD_X*FB_S)/FB_S-FB_LIP_W/2;fl.pipe[1].gap=FL_GAP_HI;fl.pipe[1].scored=true;fl.y=FL_GAP_HI-20;
        frame(16);assert(fl.state==FL_FALL && !lv_obj_has_flag(fl.flash,LV_OBJ_FLAG_HIDDEN));
        for(int i=0;i<200 && fl.state!=FL_OVER;i++)frame(16);
        assert(fl.state==FL_OVER && fl_best==1 && fl.new_best && fake_has(FL_FILE,"FLB1 1"));
        assert(!fl_key(BUTTON_UP) && fl_key(BUTTON_LEFT) && fl.state==FL_OVER);   /* not before the panel is up */
        for(int i=0;i<80;i++)frame(16);
        assert(lv_obj_has_flag(fl.flash,LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(fl.play_btn,LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(fl.play_btn,LV_EVENT_CLICKED,NULL);assert(fl.state==FL_READY && fl.score==0);
        /* the ground ends it at once */
        fl_flap();fl.y=FB_GROUND-8;fl.vy=FL_TERMINAL;frame(16);frame(16);
        assert(fl.state==FL_OVER && fl_best==1 && !fl.new_best);
        for(int i=0;i<80;i++)frame(16);
        assert(fl_key(BUTTON_LEFT) && fl.state==FL_READY);
        assert(fb_medal_for(9)<0 && fb_medal_for(10)==0 && fb_medal_for(20)==1 && fb_medal_for(30)==2 && fb_medal_for(40)==3 && fb_medal_for(99)==3);
        page_pop();frame(300);
        printf("PASS: Flappy Bird: the original's climb and fall, a tap flaps at once, points, pixel hits, pause, Game Over, best kept.\n");
    }

}

/* ==== batch 13: Bluetooth names, pairing questions, honest errors, SBC,
 * reconnecting with backoff, and a flaky or stuck stack that never blocks
 * the UI. The simulator's stack (sl_bt_demo_*) plays each part. ==== */
static bool bt_note_has(const char *s)
{
    for (int i = 0; i < nh.n && i < 4; i++)
        if (strstr(nh.list[i].title, s) || strstr(nh.list[i].body, s)) return true;
    return false;
}
static long bt_real_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }
/* the label reading text whose parent takes taps (a row, a chip), not a
 * label that only says the same (the output row, a title) */
static lv_obj_t *bt_find_tappable(lv_obj_t *o, const char *text)
{
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), text) &&
        lv_obj_has_flag(lv_obj_get_parent(o), LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_event_count(lv_obj_get_parent(o)))
        return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
    {
        lv_obj_t *f = bt_find_tappable(lv_obj_get_child(o, i), text);
        if (f) return f;
    }
    return NULL;
}
static void bt_tap_label(lv_obj_t *root, const char *text)
{
    lv_obj_t *l = bt_find_tappable(root, text);
    if (!l) printf("no label '%s'\n", text);
    assert(l);
    lv_obj_send_event(lv_obj_get_parent(l), LV_EVENT_CLICKED, NULL);
}
static void bt13_reset(void)
{
    sl_bt_demo_on = true; sl_bt_demo_index = -1; sl_bt_demo_err = sl_bt_demo_fail = 0;
    sl_bt_demo_ask_kind = 0; sl_bt_demo_ask.kind = 0; sl_bt_demo_answered = false; sl_bt_demo_hang = false;
    sl_bt_demo_delay_ms = 0; sl_bt_demo_rc_misses = 0; sl_bt_demo_devices[2].paired = false;
    strlcpy(sl_bt_demo_devices[2].name, "Nearby headphones", sizeof sl_bt_demo_devices[2].name);
    sl_bt_err_mac[0] = '\0'; sl_bt_rc_disarm("test");
}
static void bt13_tests(void)
{
    bt13_reset();
    sl_bt_page(); frame(500); frame(100); frame(100);
    assert(sl_bt_page_obj == pages[npages - 1] && !sl_bt_busy);

    /* a car showing a passkey: the page shows it, Pair answers */
    sl_bt_demo_ask_kind = HIBY_BT_ASK_CONFIRM;
    bt_tap_label(sl_bt_body, "Nearby headphones"); frame(100); frame(100); frame(400);
    assert(sl_bt_ask_page_obj && pages[npages - 1] == sl_bt_ask_page_obj && find_label(sl_bt_ask_page_obj, "123456"));
    bt_tap_label(sl_bt_ask_page_obj, "Pair"); frame(400); frame(100); frame(100); frame(400);
    assert(!sl_bt_ask_page_obj && sl_bt_state.routed && sl_bt_demo_index == 2 && !sl_bt_busy);
    printf("PASS: pairing: the passkey is shown on its own page, Pair pairs.\n");
    sl_bt_demo_index = -1; sl_bt_demo_devices[2].paired = false; frame(4100); frame(100);

    /* a PIN to type (#168); a wrong one fails and says so on the row */
    sl_bt_demo_ask_kind = HIBY_BT_ASK_PIN;
    bt_tap_label(sl_bt_body, "Nearby headphones"); frame(100); frame(100); frame(400);
    assert(sl_bt_ask_ta && find_label(sl_bt_ask_page_obj, "Pair"));
    lv_textarea_set_text(sl_bt_ask_ta, "9999");
    bt_tap_label(sl_bt_ask_page_obj, "Pair"); frame(400); frame(100); frame(100); frame(400);
    assert(!sl_bt_state.routed && sl_bt_err_code == HIBY_BT_ERR_PAIR_FAILED && bt_note_has("Pairing with Nearby headphones failed"));
    sl_bt_rebuild(); assert(find_label(sl_bt_body, "Pairing failed · tap to try again"));
    bt_tap_label(sl_bt_body, "Nearby headphones"); frame(100); frame(100); frame(400);
    lv_textarea_set_text(sl_bt_ask_ta, "1234");
    bt_tap_label(sl_bt_ask_page_obj, "Pair"); frame(400); frame(100); frame(100); frame(400);
    assert(sl_bt_state.routed && sl_bt_err_mac[0] == '\0');
    printf("PASS: pairing: a PIN is typed; a wrong one says why on the row and in a message.\n");
    sl_bt_demo_index = -1; sl_bt_demo_devices[2].paired = false; frame(4100); frame(100);

    /* Back on the question is a No: canceled, not "failed" */
    sl_bt_demo_ask_kind = HIBY_BT_ASK_CONFIRM;
    bt_tap_label(sl_bt_body, "Nearby headphones"); frame(100); frame(100); frame(400);
    assert(sl_bt_ask_page_obj); page_pop(); frame(400); frame(100); frame(100);
    assert(!sl_bt_state.routed && bt_note_has("Pairing canceled"));
    /* a code to type over there: shown, nothing asked here */
    sl_bt_demo_ask_kind = HIBY_BT_ASK_SHOW_PASSKEY;
    bt_tap_label(sl_bt_body, "Nearby headphones"); frame(100); frame(100); frame(400);
    assert(sl_bt_ask_page_obj && find_label(sl_bt_ask_page_obj, "654321") && !find_label(sl_bt_ask_page_obj, "Pair"));
    for (int i = 0; i < 25; i++) frame(100);
    assert(!sl_bt_ask_page_obj && sl_bt_state.routed);
    sl_bt_demo_ask_kind = 0;
    printf("PASS: pairing: Back cancels; a code to type there is shown until it pairs.\n");

    /* rename a device, name the player, SBC quality */
    sl_bt_demo_index = 0; frame(4100); frame(100);
    lv_obj_t *studio = lv_obj_get_parent(find_label(sl_bt_body, "Studio headphones"));
    lv_obj_send_event(lv_obj_get_child(studio, lv_obj_get_child_count(studio) - 1), LV_EVENT_CLICKED, NULL); frame(400);
    assert(pages[npages - 1] != sl_bt_page_obj && find_label(pages[npages - 1], "Forget device"));
    bt_tap_label(pages[npages - 1], "Rename"); frame(400);
    assert(sl_bt_name_ta && !strcmp(lv_textarea_get_text(sl_bt_name_ta), "Studio headphones"));
    lv_textarea_set_text(sl_bt_name_ta, "Desk cans");
    bt_tap_label(pages[npages - 1], "Save"); frame(400); frame(100); frame(100); frame(400);
    assert(pages[npages - 1] == sl_bt_page_obj && find_label(sl_bt_body, "Desk cans") && !strcmp(sl_bt_demo_devices[0].name, "Desk cans"));
    sl_bt_player_name_cb(NULL); frame(400);
    lv_textarea_set_text(sl_bt_name_ta, "Tai's R1");
    bt_tap_label(pages[npages - 1], "Save"); frame(400); frame(100); frame(100); frame(400);
    assert(!strcmp(sl_bt_demo_name, "Tai's R1") && find_label(sl_bt_body, "Tai's R1"));
    sl_bt_sbc_page(NULL); frame(400);
    bt_tap_label(pages[npages - 1], "XQ · up to 452 kb/s"); frame(100); frame(100);
    assert(sl_bt_demo_sbc == 1); page_pop(); frame(400); frame(100);
    assert(find_label(sl_bt_body, "XQ"));
    sl_bt_rename(sl_bt_demo_devices[0].mac, ""); sl_bt_demo_name[0] = '\0'; sl_bt_demo_sbc = 0;
    printf("PASS: names: a device renamed, the player named, SBC XQ chosen, all shown on the page.\n");

    /* reconnecting: out of range twice, then back; the waits grow */
    sl_bt_demo_index = -1; frame(4100); frame(100);
    strlcpy(sl_bt_demo_last, sl_bt_demo_devices[0].mac, sizeof sl_bt_demo_last);
    sl_bt_demo_rc_misses = 2;
    sl_bt_on_lost();
    assert(sl_bt_rc.armed);
    int secs = 0;
    while (!sl_bt_state.routed && secs < 60) { frame(1000); frame(100); secs++; }
    printf("reconnect: back after %d s, %d misses first\n", secs, sl_bt_rc.tries);
    assert(sl_bt_state.routed && !sl_bt_rc.armed && secs >= 16 && secs <= 22 && bt_note_has("Playing on"));
    /* let go by the user: not looked for again */
    frame(100); frame(100);
    bt_tap_label(sl_bt_body, "Disconnect"); frame(100); frame(100);
    sl_bt_demo_index = -1; frame(4100); frame(100);
    assert(!sl_bt_rc.armed);
    printf("PASS: reconnect: missed at 2 and 5 s, back at 10 s more; a disconnect asked for stays off.\n");

    /* a slow stack (3 s a job): every frame stays quick, pages still open */
    sl_bt_demo_delay_ms = 3000;
    long worst = 0;
    bt_tap_label(sl_bt_body, "Studio headphones");
    for (int i = 0; i < 40 && sl_bt_busy; i++)
    {
        long t0 = bt_real_ms(); frame(100); long d = bt_real_ms() - t0; if (d > worst) worst = d;
        if (i == 5) { sl_bt_sbc_page(NULL); frame(400); page_pop(); frame(400); }
    }
    assert(!sl_bt_busy && sl_bt_state.routed);
    printf("slow stack: worst frame %ld ms while a 3 s connect ran\n", worst);
    assert(worst < 250);
    /* stuck: the watchdog lets it go, says so, and the next job runs */
    sl_bt_demo_index = -1; sl_bt_demo_delay_ms = 0; frame(4100); frame(100);
    sl_bt_demo_hang = true;
    bt_tap_label(sl_bt_body, "Studio headphones");
    worst = 0; int waited = 0;
    while (sl_bt_busy && waited < 200) { long t0 = bt_real_ms(); frame(1000); long d = bt_real_ms() - t0; if (d > worst) worst = d; waited++; }
    printf("stuck stack: let go after %d s, worst frame %ld ms\n", waited, worst);
    assert(!sl_bt_busy && waited >= 179 && waited <= 182 && bt_note_has("Bluetooth stopped answering") && worst < 250);
    sl_bt_demo_hang = false;
    bt_tap_label(sl_bt_body, "Studio headphones"); frame(100); frame(100);
    assert(sl_bt_state.routed);
    printf("PASS: a slow or stuck Bluetooth never blocks the UI; the watchdog lets go at 180 s and says why.\n");

    /* AirPods: battery and noise control from AAP, a mode asked for */
    sl_bt_demo_aap = (struct hiby_bt_aap){ .up = true, .left = 99, .right = 100, .box = 17, .left_charging = true, .noise = 2 };
    sl_bt_demo_index = 1; frame(4100); frame(100); frame(100);
    assert(find_label(sl_bt_body, "Battery") && find_label(sl_bt_body, "Noise cancellation"));
    bt_tap_label(sl_bt_body, "Transparency"); frame(100);
    assert(sl_bt_demo_aap.noise == 3);
    sl_bt_demo_aap = (struct hiby_bt_aap){ .left = -1, .right = -1, .box = -1 };
    sl_bt_demo_index = -1; frame(4100); frame(100); frame(100);
    assert(!find_label(sl_bt_body, "Battery"));
    printf("PASS: AirPods: battery and noise control shown while AAP answers; a mode tap is sent.\n");

    /* receive: the codecs the phone shares, one asked for */
    sl_bt_demo_index = -1; frame(4100); frame(100);
    page_pop(); frame(400);
    au_btrx_connected();
    sl_btrx_demo.codec_count = 3;
    strlcpy(sl_btrx_demo.codecs[0], "SBC", 16); strlcpy(sl_btrx_demo.codecs[1], "AAC", 16); strlcpy(sl_btrx_demo.codecs[2], "LDAC", 16);
    frame(200);
    assert(find_label(sl_btrx_body, "AUDIO CODEC") || find_label(sl_btrx_body, "Audio codec"));
    bt_tap_label(sl_btrx_body, "LDAC"); frame(200); frame(200);
    assert(!strcmp(sl_btrx_demo.codec, "AAC") && sl_btrx_demo.codec_note[0]);
    bt_tap_label(sl_btrx_body, "SBC"); frame(200); frame(200);
    assert(!strcmp(sl_btrx_demo.codec, "SBC"));
    sl_btrx_stop(); frame(300); while (npages > 1) { page_pop(); frame(300); }
    sl_btrx_demo_auto = true;
    printf("PASS: receive: the shared codecs listed, one asked for, a refusal said plainly.\n");
    bt13_reset();
}

static void log_dump_atexit(void);   /* SLOPPY_LOG_DUMP=<file>: the fake card's sloppy.prev.log and sloppy.log, written out at exit */
LV_FONT_DECLARE(ss_heb_regular_16) LV_FONT_DECLARE(ss_heb_regular_18) LV_FONT_DECLARE(ss_heb_regular_21) LV_FONT_DECLARE(ss_heb_regular_23) LV_FONT_DECLARE(ss_heb_regular_26)
LV_FONT_DECLARE(ss_heb_semibold_18) LV_FONT_DECLARE(ss_heb_semibold_21) LV_FONT_DECLARE(ss_heb_semibold_23) LV_FONT_DECLARE(ss_heb_semibold_26) LV_FONT_DECLARE(ss_heb_semibold_30)
LV_FONT_DECLARE(ss_heb_semibold_34) LV_FONT_DECLARE(ss_heb_semibold_38) LV_FONT_DECLARE(ss_heb_bold_28)
/* SLOPPY_DTAP=1 [SLOPPY_DTAP_ONLY=<n>]: see dt_collect */
static void dt_probe(void)
{
        static void (*const opens[])(void) = { sl_games_page, sl_library_page, system_page, appearance_page,
            settings_icon_page, theme_page, home_style_page, lock_player_page, sounds_page, accent_page,
            coin_page, magic_page, grave_page, wrapped_page, mix_page, foryou_page, pet_page,
            eq_page, play_settings_page, qp_sleep_page, qp_display_page, quiz_page, search_page,
            radio_page, albums_page, songs_page, artists_page, music_page, genres_page, years_page, loved_page,
            sound_page, buttons_page, shortcuts_page, time_power_page, language_page, text_size_page, keyboards_page,
            library_options_page, advanced_page, settings_layout_page, np_style_page, cf_style_page, screen_sleep_page,
            sl_bt_page, sl_wifi_page, clicker_page, metronome_page, points_page, books_page, podcasts_page, webradio_page,
            ebooks_page, subsonic_page, update_page, mu_arrange_page, home_page, saver_page, car_page, lyrics_color_page,
            /* 60 on: the rest a tap can reach on its own (not the erase, reboot or start-up-home ones) */
            abs_page, abs_local_page, arcade_page, chess_page, eq_find_page, eq_profiles_page, flappy_page, hidden_page,
            lfm_page, libjob_page, lo_folders_page, lo_keep_page, lo_seps_page, pet_name_page, picks_page, pls_del_page,
            pod_downloads_page, pod_find_page, pod_opml_page, pod_url_page, ragdoll_page, rd_contents_page, scr_page,
            scr_token_page, settings_search_page, sl_btrx_page, snake_page, sp_page, spacey_page, spoon_page,
            sub_find_page, sub_offline_page, sub_servers_page, sudoku_page, t48_page, tartists_page, tone_page, tz_page,
            wf_other_page, abs_edit_page, taste_reset_page, up_revert_page, reset_all_settings_page };
        int theme=global_settings.sloppy_theme,style=global_settings.sloppy_home_style,found=0,taps=0;
        int only=getenv("SLOPPY_DTAP_ONLY")?atoi(getenv("SLOPPY_DTAP_ONLY")):-1;
        /* SLOPPY_DTAP_LIB=0: an empty library; =1: one song on one album */
        static struct sl_track one_tr[1];static struct sl_album one_al[1];static int one_idx[1]={0},one_tracks[1]={0};
        if(getenv("SLOPPY_DTAP_LIB")){
            while(npages>1){page_pop();frame(400);}
            memset(&sl_lib,0,sizeof sl_lib);
            queued=0;started=-1;test_status=0;   /* the pretend queue held the old library's songs */
            if(atoi(getenv("SLOPPY_DTAP_LIB"))==2){   /* =2: the edges: a 400-letter title, emoji, a name in two directions, none at all; 0:00, 10 h, 99 h */
                static struct sl_track et[5];static struct sl_album ea[2];static int ea0[3]={0,1,2},ea1[2]={3,4},eby[2]={0,1},esongs[5]={4,3,1,2,0};
                static char longt[401];memset(longt,'W',400);longt[400]=0;
                static const char *const tt[5]={longt,"\xf0\x9f\x8e\xa7\xf0\x9f\x94\xa5\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb","Love \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 12","","Zz"};
                static const long tl[5]={0,36000000L,359999000L,1000,61000};
                for(int i=0;i<5;i++){char pth[64];snprintf(pth,sizeof pth,"/Music/edge/%d.flac",i);tpath(&et[i],strdup(pth));et[i].title=tt[i];et[i].album=i<3?0:1;et[i].num=i+1;et[i].len=tl[i];et[i].lossless=true;}
                ea[0].title=longt;ea[0].artist="";ea[0].tracks=ea0;ea[0].n_tracks=3;ea[0].year=9999;
                ea[1].title="\xf0\x9f\x8e\xb5";ea[1].artist="\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d Artist";ea[1].tracks=ea1;ea[1].n_tracks=2;ea[1].year=0;
                sl_lib.tracks=et;sl_lib.n_tracks=5;sl_lib.albums=ea;sl_lib.n_albums=2;sl_lib.n_artists=2;
                sl_lib.albums_by_title=eby;sl_lib.albums_by_artist=eby;sl_lib.songs_by_title=esongs;
            }
            if(atoi(getenv("SLOPPY_DTAP_LIB"))==1){
                tpath(&one_tr[0],strdup("/Music/one/only.mp3"));one_tr[0].title="The Only Song";one_tr[0].album=0;one_tr[0].num=1;one_tr[0].len=0;
                one_al[0].title="The Only Album";one_al[0].artist="The Only Artist";one_al[0].tracks=one_tracks;one_al[0].n_tracks=1;
                sl_lib.tracks=one_tr;sl_lib.n_tracks=1;sl_lib.albums=one_al;sl_lib.n_albums=1;sl_lib.n_artists=1;
                sl_lib.albums_by_title=one_idx;sl_lib.albums_by_artist=one_idx;sl_lib.songs_by_title=one_idx;
            }
            lib_epoch++;sl_art_reset();
            appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(300);}
        }
        /* SLOPPY_DTAP_LANG=<n> (4 Hebrew, 5 Arabic: right to left), SLOPPY_DTAP_TEXT=<step> (3 the biggest) */
        int lang=getenv("SLOPPY_DTAP_LANG")?atoi(getenv("SLOPPY_DTAP_LANG")):0,tstep=getenv("SLOPPY_DTAP_TEXT")?atoi(getenv("SLOPPY_DTAP_TEXT")):0;
        if(lang||tstep){global_settings.sloppy_language=lang;global_settings.sloppy_text_size=tstep;appearance_reload(NULL);frame(600);}
        while(npages>1){page_pop();frame(400);}
        for(int w=0;w<(int)(sizeof opens/sizeof opens[0]);w++){
            if(only>=0&&w!=only)continue;
            if(opens[w]!=home_page)opens[w]();for(int f=0;f<8;f++)frame(80);
            int depth=npages;char t0[64];dt_title(pages[npages-1],t0,sizeof t0);
            dt_n=0;dt_collect(pages[npages-1]);int n=dt_n;
            lv_point_t pts[64];char lbl[64][40];memcpy(pts,dt_pt,sizeof pts);memcpy(lbl,dt_lbl,sizeof lbl);
            while(npages>1){page_pop();frame(300);}
            for(int b=0;b<n;b++){
                if(opens[w]!=home_page)opens[w]();for(int f=0;f<8;f++)frame(80);
                if(npages!=depth)break;
                int kind=getenv("SLOPPY_DTAP_KIND")?atoi(getenv("SLOPPY_DTAP_KIND")):0;
                if(kind==1){   /* a tap, and back before the page has slid in */
                    touch_at(pts[b].x,pts[b].y,true);touch_at(pts[b].x,pts[b].y,false);frame(30);
                    if(npages>depth)page_pop();
                    for(int f=0;f<12;f++)frame(80);taps++;
                    lv_obj_t *top=pages[npages-1];
                    if(npages==depth&&(lv_obj_has_flag(top,LV_OBJ_FLAG_HIDDEN)||lv_obj_get_x(top)!=0))
                        {printf("DTAP back mid-slide: %s (%d) button %d \"%s\": the page left at x %d%s\n",t0,w,b,lbl[b],(int)lv_obj_get_x(top),lv_obj_has_flag(top,LV_OBJ_FLAG_HIDDEN)?", hidden":"");found++;}
                } else if(kind==2){   /* pressed, then the thumb slides off it and lifts: no tap */
                    int dx=pts[b].x<W/2?150:-150;
                    touch_at(pts[b].x,pts[b].y,true);for(int k=1;k<=6;k++)touch_at(pts[b].x+dx*k/6,pts[b].y,true);touch_at(pts[b].x+dx,pts[b].y,false);
                    for(int f=0;f<10;f++)frame(80);taps++;
                    if(npages>depth){char tn[64];dt_title(pages[npages-1],tn,sizeof tn);printf("DTAP slid off: %s (%d) button %d \"%s\" still opened \"%s\"\n",t0,w,b,lbl[b],tn);found++;}
                } else {
                touch_at(pts[b].x,pts[b].y,true);touch_at(pts[b].x,pts[b].y,false);frame(60);
                touch_at(pts[b].x,pts[b].y,true);touch_at(pts[b].x,pts[b].y,false);
                for(int f=0;f<10;f++)frame(80);taps++;
                }
                char ta[64],tb[64];
                if(getenv("SLOPPY_DTAP_SHOTS")){   /* each page reached once, for a look */
                    static char seen[400][64];static int nseen;char tn[64],nm[512];
                    lv_obj_t *top=np_open?np:acts_open_now?acts:uq_open?uq:pages[npages-1];
                    dt_title(top,tn,sizeof tn);if(!tn[0])dt_text(top,tn,sizeof tn);
                    bool got=false;for(int i=0;i<nseen;i++)if(!strcmp(seen[i],tn))got=true;
                    if(!got&&nseen<400){strlcpy(seen[nseen++],tn,64);
                        char clean[40];int k=0;for(const char *q=tn;*q&&k<38;q++)clean[k++]=isalnum((unsigned char)*q)?*q:'_';clean[k]=0;
                        snprintf(nm,sizeof nm,"%s/%03d-%s.ppm",getenv("SLOPPY_DTAP_SHOTS"),nseen,clean);frame(300);shot_now(nm);}
                }
                for(int i=depth;i<npages;i++){
                    dt_title(pages[i],ta,sizeof ta);dt_title(pages[i-1],tb,sizeof tb);
                    if(ta[0]&&!strcmp(ta,tb)){printf("DTAP twice: %s (%d) button %d \"%s\" opened \"%s\" over itself, %d pages deep\n",t0,w,b,lbl[b],ta,npages);found++;break;}
                }
                if(npages-depth>=2)printf("DTAP two pushes: %s (%d) button %d \"%s\": %d pages\n",t0,w,b,lbl[b],npages-depth);
                {int nl=0;for(lv_layer_t *l=disp->layer_head;l;l=l->next)nl++;if(nl!=1)printf("DTAP layers: %s button %d: %d\n",t0,b,nl);}
                if(cf_is_open()){cf_close();frame(300);}
                if(np_open){np_hide(0);frame(500);}
                if(uq_open){uq_close();frame(500);}
                if(acts_open_now){acts_close();frame(400);}
                if(qp_open){qp_close();frame(600);}
                if(pet_care_locked){pet_care_until=sl_ms()-1;pet_tick();}
                while(npages>1){page_pop();frame(300);}
                for(int f=0;f<5;f++)frame(100);
                if(global_settings.sloppy_theme!=theme||global_settings.sloppy_home_style!=style||global_settings.sloppy_language!=lang||sl_text_step!=tstep){
                    global_settings.sloppy_theme=theme;global_settings.sloppy_home_style=style;global_settings.sloppy_language=lang;global_settings.sloppy_text_size=tstep;
                    appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(300);}}
            }
        }
        printf("DTAP done: %d taps, %d pages opened twice\n",taps,found);
}

int main(void) {
    if (getenv("SLOPPY_LOG_DUMP")) atexit(log_dump_atexit);
    sl_share_web_port=sl_share_ls_port=0;   /* any free ports: the Mac may run the real ones */
    {   /* the harness's clock never moves, so every screenshot has one name; the card's Screenshots
         * folder is a real one that outlives a run, and past 99 of a name a shot can't be saved */
        char d[600];snprintf(d,sizeof d,"rm -rf '%s/Screenshots'",sim_root_dir);system(d);
    }
    test_packs();
    {   /* the cover's colour for Now Playing's ink: a red sleeve reads red, a grey one leaves the accent */
        static uint16_t px[64*64];for(int i=0;i<64*64;i++)px[i]=0xF800;
        uint32_t ink=cover_ink(px,64,64,64);assert(ink && ((ink>>16)&255)>((ink>>8)&255) && ((ink>>16)&255)>(ink&255));
        for(int i=0;i<64*64;i++)px[i]=0x8410;assert(!cover_ink(px,64,64,64));
        int was=global_settings.sloppy_np_cover_ink;uint32_t old=np_cover_ink;
        global_settings.sloppy_np_cover_ink=1;np_cover_ink=ink;assert(np_ink()==ink);
        global_settings.sloppy_np_cover_ink=0;assert(np_ink()==(uint32_t)C_ACCENT);
        global_settings.sloppy_np_cover_ink=was;np_cover_ink=old; }
    { char bd[32]="3:05";lossless_badge(bd,sizeof bd,"/a/b.WAV");assert(!strcmp(bd,"3:05  \xc2\xb7  WAV"));
      bd[0]=0;lossless_badge(bd,sizeof bd,"/a/b.dsf");assert(!strcmp(bd,"  \xc2\xb7  DSD"));
      bd[0]=0;lossless_badge(bd,sizeof bd,"/a/b.mp3");assert(!bd[0]); }
    { char tb[16]; fmt_time(tb,sizeof tb,4*60*60*1000L+61000);assert(!strcmp(tb,"4:01:01"));
      fmt_time(tb,sizeof tb,185000);assert(!strcmp(tb,"3:05")); fmt_time(tb,sizeof tb,-5);assert(!strcmp(tb,"0:00")); }
    {   /* natural order: a run of digits is its number */
        assert(sl_sort_cmp("Track 2","Track 10")<0 && sl_sort_cmp("Track 10","Track 9")>0 && sl_sort_cmp("Track 100","Track 20")>0);
        assert(sl_sort_cmp("Track 02","Track 2")!=0 && sl_sort_cmp("a1b2","a1b10")<0 && sl_sort_cmp("Song","Song")==0 && sl_sort_cmp("9 to 5","Abba")<0);
        /* a count as the language writes it */
        char cb[16];assert(!strcmp(sl_fmt_count(cb,sizeof cb,10000),"10,000") && !strcmp(sl_fmt_count(cb,sizeof cb,999),"999") && !strcmp(sl_fmt_count(cb,sizeof cb,1234567),"1,234,567"));
        global_settings.sloppy_language=1;assert(!strcmp(sl_fmt_count(cb,sizeof cb,10000),"10.000"));global_settings.sloppy_language=0; }
    global_settings.sloppy_accent_color=0x72d8cd;
    global_settings.sloppy_sfx_volume=70;global_settings.sloppy_ui_sounds=1;global_settings.sloppy_volume_step=1;
    global_settings.sloppy_sleep=1;   /* the settings_list default: 1 minute */
    /* the other settings_list defaults the harness relies on */
    global_settings.sloppy_auto_np=1;global_settings.sloppy_remote_keys=1;global_settings.sloppy_lyrics_size=1;global_settings.sloppy_boot_volume=-1;global_settings.sloppy_eq_auto_level=1;
    global_settings.volume_limit=-100;global_settings.sloppy_volume_step=1;global_settings.sloppy_tap_wake=1;global_settings.sloppy_swipe_typing=1;
    global_settings.lcd_sleep_after_backlight_off=2;
    global_settings.sloppy_gain=1;global_settings.sloppy_auto_time=1;
    global_settings.stereo_width=100;global_settings.channel_config=SOUND_CHAN_STEREO;
    global_settings.sloppy_notifications=1;   /* the history and banners most checks below are about; toasts are tested on their own */
    lv_init();lv_tick_set_cb(ticks);disp=lv_display_create(W,H);
    static uint16_t buffer[W*100] __attribute__((aligned(64)));   /* LVGL asserts (and spins) on a misaligned draw buffer */
    lv_display_set_color_format(disp,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp,buffer,NULL,sizeof(buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(disp,render);
    lv_display_add_event_cb(disp,cfm_refr_cb,LV_EVENT_REFR_START,NULL);lv_display_add_event_cb(disp,cfm_refr_cb,LV_EVENT_REFR_READY,NULL);
    lv_display_add_event_cb(disp,cfm_refr_cb,LV_EVENT_RENDER_READY,NULL);   /* the meter, as lv_core_init */
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);
    touch=lv_indev_create();lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(touch,touch_read);
    styles_init();
    /* the R1 makes the styles at the splash, before the settings are read:
     * a light theme from boot used to keep dark pages under light text */
    { lv_style_value_t v;global_settings.sloppy_theme=1;styles_retheme();
      lv_style_get_prop(&st_page,LV_STYLE_BG_COLOR,&v);assert(lv_color_to_u32(v.color)==(0xff000000|C_BG));
      lv_style_get_prop(&st_sub,LV_STYLE_TEXT_COLOR,&v);assert(lv_color_to_u32(v.color)==(0xff000000|C_SUB));
      global_settings.sloppy_theme=0;styles_retheme(); }
    {   /* the splash: drawn still, then it lifts away as home arrives */
        uint32_t top=lv_obj_get_child_count(lv_layer_top());
        splash_build();frame(0);shot_now("splash.ppm");
        assert(lv_anim_count_running()==0);   /* nothing moves while Rockbox starts */
        splash_fade_out();frame(0);frame(150);shot_now("splash-out.ppm");frame(400);
        assert(!splash && lv_obj_get_child_count(lv_layer_top())==top);
    }
    fixture();
    /* a song's path, made from its folder (there once) and its name, reads back whole; its hash and lookup as the whole path's */
    assert(sl_lib.n_dirs>=1&&!strcmp(TP(42),"/Music/song-42.flac")&&sl_track_path_is(42,"/Music/song-42.flac")&&!sl_track_path_is(42,"/Music/song-4.flac"));
    assert(sl_track_hist_hash(42)==sl_hist_hash("/Music/song-42.flac")&&sl_lib_find_path("/Music/song-42.flac")==42&&sl_track_same_file(42,42)&&!sl_track_same_file(42,43));
    home_page();mini_build();np_build();statusbar_build();statusbar_update();frame(0);frame(160);shot_now("home-intro.ppm");frame(300);shot("home.ppm");
    if (getenv("SLOPPY_REAL_ART")) test_real_art = true;   /* shot modes with every album's generated cover */
    if (getenv("SLOPPY_GLYPH_SHEET")) {   /* SLOPPY_GLYPH_SHEET=<dir> SLOPPY_AUDIT_TAG=...: every symbol in use at 16/20/24/28/48, beside words, dark and light, then stop */
        static const char *const sy[] = { LV_SYMBOL_PLAY, LV_SYMBOL_PAUSE, LV_SYMBOL_STOP, LV_SYMBOL_PREV, LV_SYMBOL_NEXT, LV_SYMBOL_SHUFFLE,
            LV_SYMBOL_LOOP, LV_SYMBOL_REFRESH, LV_SYMBOL_AUDIO, LV_SYMBOL_LIST, LV_SYMBOL_BARS, LV_SYMBOL_OK, LV_SYMBOL_CLOSE, LV_SYMBOL_PLUS,
            LV_SYMBOL_MINUS, LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_SETTINGS, LV_SYMBOL_HOME, LV_SYMBOL_POWER,
            LV_SYMBOL_EDIT, LV_SYMBOL_TRASH, LV_SYMBOL_DIRECTORY, LV_SYMBOL_FILE, LV_SYMBOL_IMAGE, LV_SYMBOL_VIDEO,
            LV_SYMBOL_EYE_OPEN, LV_SYMBOL_EYE_CLOSE, LV_SYMBOL_BLUETOOTH, LV_SYMBOL_WIFI, LV_SYMBOL_VOLUME_MID, LV_SYMBOL_VOLUME_MAX,
            LV_SYMBOL_UPLOAD, LV_SYMBOL_DOWNLOAD, LV_SYMBOL_WARNING, LV_SYMBOL_BELL, LV_SYMBOL_CHARGE, LV_SYMBOL_USB, LV_SYMBOL_GPS,
            LV_SYMBOL_KEYBOARD, LV_SYMBOL_BACKSPACE, LV_SYMBOL_COPY, LV_SYMBOL_CALL, LV_SYMBOL_TINT, LV_SYMBOL_DRIVE, LV_SYMBOL_SAVE,
            LV_SYMBOL_BATTERY_FULL, LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_2, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_EMPTY,
            SL_SYM_EQ, SL_SYM_XFADE, SL_SYM_GLOBE };
        static const lv_font_t *const fs[] = { &sl_sym_16, &sl_sym_20, &sl_sym_24, &sl_sym_28, &sl_sym_48 };
        const char *dir = getenv("SLOPPY_GLYPH_SHEET"), *tag = getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now";
        char p[600]; snprintf(p, sizeof p, "mkdir -p '%s'", dir); system(p);
        const int n = (int)(sizeof sy / sizeof sy[0]), per = 28;
        for (int th = 0; th < 2; th++)
            for (int pg = 0; pg * per < n; pg++) {
                global_settings.sloppy_theme = th; appearance_reload((void *)2); frame(300);
                lv_obj_t *sh = box(lv_layer_top());
                lv_obj_set_size(sh, W, H);
                lv_obj_set_style_bg_color(sh, lv_color_hex(C_BG), 0);
                lv_obj_set_style_bg_opa(sh, LV_OPA_COVER, 0);
                for (int i = pg * per; i < n && i < (pg + 1) * per; i++) {
                    int k = i - pg * per, x0 = (k % 2) * 240 + 6, y0 = (k / 2) * 56 + 6, x = x0;
                    for (int f = 0; f < 5; f++) {
                        lv_obj_t *l = lv_label_create(sh);
                        lv_label_set_text(l, sy[i]);
                        lv_obj_set_style_text_font(l, fs[f], 0);
                        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
                        lv_obj_set_pos(l, x, y0 + 50 - lv_font_get_line_height(fs[f]));
                        x += f == 4 ? 0 : lv_font_get_line_height(fs[f]) + 8;
                    }
                }
                lv_obj_t *w = lv_label_create(sh);   /* beside words, as rows and pills draw them */
                lv_label_set_text(w, LV_SYMBOL_PLAY "  Play  " LV_SYMBOL_SHUFFLE "  Shuffle  " LV_SYMBOL_RIGHT);
                lv_obj_set_style_text_font(w, &ss_semibold_23, 0);
                lv_obj_set_style_text_color(w, lv_color_hex(C_TEXT), 0);
                lv_obj_set_pos(w, 10, H - 40);
                frame(0); frame(0);
                snprintf(p, sizeof p, "%s/glyphs-%d-%s-%s.ppm", dir, pg, th ? "light" : "dark", tag);
                shot_now(p);
                lv_obj_delete(sh);
            }
        return 0;
    }
    if (getenv("SLOPPY_AUDIT_SNAP")) { au_snap(getenv("SLOPPY_AUDIT_SNAP"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_BIDI_SHOT")) {   /* scratch: how LVGL orders a count and a Hebrew word */
        static const struct { const char *t; int dir; } v[] = {
            { "80 \xd7\xa9\xd7\x99\xd7\xa8\xd7\x99\xd7\x9d", LV_BASE_DIR_AUTO }, { "80 \xd7\xa9\xd7\x99\xd7\xa8\xd7\x99\xd7\x9d", LV_BASE_DIR_RTL },
            { "\xd7\xa9\xd7\x99\xd7\xa8\xd7\x99\xd7\x9d 80", LV_BASE_DIR_LTR }, { "\xd7\xa9\xd7\x99\xd7\xa8\xd7\x99\xd7\x9d 80", LV_BASE_DIR_RTL },
            { "\xd7\xa9\xd7\x99\xd7\xa8\xd7\x99\xd7\x9d 80", LV_BASE_DIR_AUTO },
            { "80 \xd7\xa9\xd7\x99\xd7\xa8\xd7\x99\xd7\x9d \xc2\xb7 10 \xd7\x90\xd7\x9c\xd7\x91", LV_BASE_DIR_RTL },
            { "\xd7\x90\xd7\x9c\xd7\x91 10 \xc2\xb7 \xd7\xa9\xd7\x99\xd7\xa8\xd7\x99\xd7\x9d 80", LV_BASE_DIR_LTR },
            { "\xd7\x90\xd7\x9c\xd7\x91 10 \xc2\xb7 \xd7\xa9\xd7\x99\xd7\xa8\xd7\x99\xd7\x9d 80", LV_BASE_DIR_RTL },
        };
        lv_obj_t *scr = lv_obj_create(NULL); lv_screen_load(scr);
        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
            lv_obj_t *l = lv_label_create(scr); lv_label_set_text(l, v[i].t); lv_obj_set_style_base_dir(l, v[i].dir, 0);
            lv_obj_set_style_text_font(l, &ss_semibold_26, 0); lv_obj_set_width(l, 400); lv_obj_set_pos(l, 40, 40 + 80 * i);
        }
        frame(0); frame(100); shot_now(getenv("SLOPPY_BIDI_SHOT")); return 0;
    }
    if (getenv("SLOPPY_SHOWCASE")) { showcase_shots(getenv("SLOPPY_SHOWCASE"), getenv("SLOPPY_SHOWCASE_OUT") ? getenv("SLOPPY_SHOWCASE_OUT") : "."); return 0; }   /* the front page: real songs and sleeves */
    if (getenv("SLOPPY_CENTER_SHOTS")) { center_shots(getenv("SLOPPY_CENTER_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_E_TESTS")) { au_library(); e_tests(); au_library_done(); return 0; }   /* batch E alone, while working on it */
    if (getenv("SLOPPY_F_TESTS")) { au_library(); f_tests(); au_library_done(); return 0; }   /* batch F alone */
    if (getenv("SLOPPY_F_SHOTS")) { f_shots(getenv("SLOPPY_F_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_E_SHOTS")) { e_shots(getenv("SLOPPY_E_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_D_SHOTS")) { d_shots(getenv("SLOPPY_D_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_A1_SHOTS")) { a1_shots(getenv("SLOPPY_A1_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_MUSIC_SHOTS")) { music_shots(getenv("SLOPPY_MUSIC_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_CC_PROOF")) { cc_proof(getenv("SLOPPY_CC_PROOF"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_B9_PERF")) { au_library(); b9_perf(); au_library_done(); return 0; }
    if (getenv("SLOPPY_B9_TESTS")) { au_library(); b9_tests(); au_library_done(); return 0; }   /* batch 9 alone */
    if (getenv("SLOPPY_DTAP")) { au_library(); dt_probe(); au_library_done(); return 0; }   /* every button tapped twice */
    if (getenv("SLOPPY_I_TESTS")) { au_library(); i_tests(); au_library_done(); return 0; }   /* batch I alone */
    if (getenv("SLOPPY_SUB_TESTS")) { au_library(); sb_tests(); au_library_done(); return 0; }   /* the music server alone */
    if (getenv("SLOPPY_REMOTE_TESTS")) { au_library(); rm_tests(); au_library_done(); return 0; }   /* the phone remote alone */
    if (getenv("SLOPPY_OTA_TESTS")) { au_library(); ot_tests(); au_library_done(); return 0; }   /* software updates alone */
    if (getenv("SLOPPY_REMOTE_SERVE")) { au_library(); rm_serve(atoi(getenv("SLOPPY_REMOTE_SERVE"))); au_library_done(); return 0; }
    if (getenv("SLOPPY_ABS_TESTS")) { au_library(); ab_tests(); au_library_done(); return 0; }   /* Audiobookshelf alone */
    if (getenv("SLOPPY_EPUB_TESTS")) { au_library(); ep_tests(); au_library_done(); return 0; }   /* the e-book reader alone */
    if (getenv("SLOPPY_WAVE_TESTS")) { au_library(); wave_tests(); au_library_done(); return 0; }   /* the waveform alone */
    if (getenv("SLOPPY_H_TESTS")) { au_library(); h_tests(); au_library_done(); return 0; }   /* batch H alone */
    if (getenv("SLOPPY_H_SHOTS")) { h_shots(getenv("SLOPPY_H_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_I_SHOTS")) { i_shots(getenv("SLOPPY_I_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_TASTE_LEARN")) { taste_learn_tests(); return 0; }
    if (getenv("SLOPPY_TASTE_FUZZ")) { taste_fuzz(atoi(getenv("SLOPPY_TASTE_FUZZ"))); return 0; }
    if (getenv("SLOPPY_TASTE_SHOTS")) { taste_shots(getenv("SLOPPY_TASTE_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_FLAPPY_SHOTS")) { flappy_shots(getenv("SLOPPY_FLAPPY_SHOTS")); return 0; }
    if (getenv("SLOPPY_B13_SHOTS")) { b13_shots(getenv("SLOPPY_B13_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_B9_SHOTS")) { b9_shots(getenv("SLOPPY_B9_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_B8_SHOTS")) { b8_shots(getenv("SLOPPY_B8_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_BKH_SHOTS")) { bkh_shots(getenv("SLOPPY_BKH_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }   /* the Audiobooks home */
    if (getenv("SLOPPY_B16_SHOTS")) { b16_shots(getenv("SLOPPY_B16_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_B2_SHOTS")) { b2_shots(getenv("SLOPPY_B2_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_B5_SHOTS")) { b5_shots(getenv("SLOPPY_B5_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_KB_SHOTS")) { kb_shots(getenv("SLOPPY_KB_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_GLIDE_ACC")) { glide_accuracy(false); return 0; }
    if (getenv("SLOPPY_KB_PERF")) { kb_perf(); return 0; }
    if (getenv("SLOPPY_B4_SHOTS")) { b4_shots(getenv("SLOPPY_B4_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_B3_SHOTS")) { b3_shots(getenv("SLOPPY_B3_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_SHEET_SHOTS")) { sheet_shots(getenv("SLOPPY_SHEET_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_TM_SHOTS")) { tm_shots(getenv("SLOPPY_TM_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_SC_SHOTS")) { sc_shots(getenv("SLOPPY_SC_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_GB_SHOTS")) {   /* SLOPPY_GB_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: the Gabagool tile in every size, its pin-picker row and a tap, in Dark, Light and Zule; the Cards and List homes beside them */
        const char *dir=getenv("SLOPPY_GB_SHOTS"),*tag=getenv("SLOPPY_AUDIT_TAG")?getenv("SLOPPY_AUDIT_TAG"):"now";
        static const int ths[3]={0,1,3};static const char *const thn[3]={"dark","light","zule"};char nm[512];
        global_settings.sloppy_notifications=0;
        for(int t=0;t<3;t++){
            global_settings.sloppy_theme=ths[t];
            global_settings.sloppy_home_style=0;appearance_reload((void*)2);frame(600);while(npages>1){page_pop();frame(400);}
            snprintf(nm,sizeof nm,"%s/cards-home-%s-%s.ppm",dir,thn[t],tag);shot(nm);
            global_settings.sloppy_home_style=2;appearance_reload((void*)2);frame(600);while(npages>1){page_pop();frame(400);}
            snprintf(nm,sizeof nm,"%s/list-home-%s-%s.ppm",dir,thn[t],tag);shot(nm);
            global_settings.sloppy_home_style=1;appearance_reload((void*)2);frame(600);while(npages>1){page_pop();frame(400);}
            if(mt_find(TL_GABAGOOL)<0){mt_pin(TL_GABAGOOL,SZ_MED);frame(300);}
            int k=mt_find(TL_GABAGOOL);
            for(int z=0;z<3;z++){
                lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(mt.slot[k].obj)-60,LV_ANIM_OFF);frame(600);
                const char *sz=mt.slot[k].size==SZ_WIDE?"wide":mt.slot[k].size==SZ_SMALL?"small":"medium";
                snprintf(nm,sizeof nm,"%s/tiles-%s-%s-%s.ppm",dir,sz,thn[t],tag);shot_now(nm);
                g_dragged=false;lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(60);frame(60);
                snprintf(nm,sizeof nm,"%s/tiles-%s-tap-%s-%s.ppm",dir,sz,thn[t],tag);shot_now(nm);frame(2000);
                mt_edit_enter(k);mt_size_cb(NULL);frame(300);mt_edit_exit();frame(200);k=mt_find(TL_GABAGOOL);
            }
            mt_edit_enter(k);mt_unpin_cb(NULL);frame(300);mt_edit_exit();frame(200);
            mt_add_cb(NULL);frame(500);
            {   lv_obj_t *row=find_label(pages[npages-1],"Gabagool");
                for(lv_obj_t *o=row;o;o=lv_obj_get_parent(o))if(lv_obj_get_scroll_dir(o)==LV_DIR_VER&&lv_obj_has_flag(o,LV_OBJ_FLAG_SCROLLABLE)){lv_obj_scroll_to_view_recursive(row,LV_ANIM_OFF);break;}
                frame(300);snprintf(nm,sizeof nm,"%s/pin-picker-%s-%s.ppm",dir,thn[t],tag);shot(nm); }
            page_pop();frame(400);
        }
        global_settings.sloppy_theme=0;global_settings.sloppy_home_style=1;
        return 0;
    }
    if (getenv("SLOPPY_SCC_SHOTS")) {   /* SLOPPY_SCC_SHOTS=<dir> SLOPPY_AUDIT_TAG=...: arranging the control centre, Dark and Light: the list, a shortcut mid-drag, the ones left out */
        const char *dir=getenv("SLOPPY_SCC_SHOTS"),*tag=getenv("SLOPPY_AUDIT_TAG")?getenv("SLOPPY_AUDIT_TAG"):"now";char nm[512];
        global_settings.sloppy_notifications=0;
        for(int t=0;t<2;t++){
            global_settings.sloppy_theme=t;appearance_reload((void*)2);frame(600);while(npages>1){page_pop();frame(400);}
            strlcpy((char *)global_settings.sloppy_qp_tiles,"0123456789ab.",sizeof global_settings.sloppy_qp_tiles);quick_panel_rebuild();
            shortcuts_page();frame(500);lv_obj_scroll_to_view_recursive(scc.d.rows[0],LV_ANIM_OFF);frame(300);
            snprintf(nm,sizeof nm,"%s/cc-arrange-%s-%s.ppm",dir,t?"light":"dark",tag);shot(nm);
            lv_area_t a;lv_obj_get_coords(lv_obj_get_child(scc.d.rows[1],3),&a);
            t_x=(a.x1+a.x2)/2;t_y=(a.y1+a.y2)/2;t_down=true;frame(30);
            for(int i=1;i<=8;i++){t_y=(a.y1+a.y2)/2+i*scc.d.row_h*15/80;frame(30);}
            frame(200);snprintf(nm,sizeof nm,"%s/cc-arrange-drag-%s-%s.ppm",dir,t?"light":"dark",tag);shot_now(nm);
            t_down=false;t_y=(a.y1+a.y2)/2;frame(30);frame(400);
            lv_obj_scroll_to_y(scc.d.body,LV_COORD_MAX,LV_ANIM_OFF);frame(300);
            snprintf(nm,sizeof nm,"%s/cc-arrange-more-%s-%s.ppm",dir,t?"light":"dark",tag);shot(nm);
            page_pop();frame(400);
        }
        global_settings.sloppy_qp_tiles[0]='\0';global_settings.sloppy_theme=0;return 0;
    }
    if (getenv("SLOPPY_MUA_SHOTS")) {   /* SLOPPY_MUA_SHOTS=<dir> SLOPPY_AUDIT_TAG=...: Arrange Music, Dark and Light: the page, a card mid-drag */
        const char *dir=getenv("SLOPPY_MUA_SHOTS"),*tag=getenv("SLOPPY_AUDIT_TAG")?getenv("SLOPPY_AUDIT_TAG"):"now";char nm[512];
        global_settings.sloppy_notifications=0;remove(MU_FILE);mu_read=false;mu_load();
        for(int t=0;t<2;t++){
            global_settings.sloppy_theme=t;appearance_reload((void*)2);frame(600);while(npages>1){page_pop();frame(400);}
            mu_arrange_page();frame(500);
            snprintf(nm,sizeof nm,"%s/music-arrange-%s-%s.ppm",dir,t?"light":"dark",tag);shot(nm);
            lv_obj_scroll_to_view_recursive(mua[1].rows[2],LV_ANIM_OFF);frame(300);
            lv_area_t a;lv_obj_get_coords(lv_obj_get_child(mua[1].rows[1],2),&a);
            t_x=(a.x1+a.x2)/2;t_y=(a.y1+a.y2)/2;t_down=true;frame(30);
            for(int i=1;i<=8;i++){t_y=(a.y1+a.y2)/2+i*mua[1].row_h*15/80;frame(30);}
            frame(200);snprintf(nm,sizeof nm,"%s/music-arrange-drag-%s-%s.ppm",dir,t?"light":"dark",tag);shot_now(nm);
            t_down=false;t_y=(a.y1+a.y2)/2;frame(30);frame(400);
            page_pop();frame(400);
        }
        remove(MU_FILE);mu_read=false;mu_load();global_settings.sloppy_theme=0;return 0;
    }
    if (getenv("SLOPPY_PL_SHOTS")) {   /* SLOPPY_PL_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: the Playlists page with six lists, then picking two, in Dark and Light */
        const char *dir=getenv("SLOPPY_PL_SHOTS"),*tag=getenv("SLOPPY_AUDIT_TAG")?getenv("SLOPPY_AUDIT_TAG"):"now";char nm[512],pp[200],bd[300];
        global_settings.sloppy_notifications=0;fake_card_reset();mkdir(FAKE_CARD "/Playlists");
        strlcpy((char *)global_settings.playlist_catalog_dir,FAKE_CARD "/Playlists",sizeof global_settings.playlist_catalog_dir);
        static const char *const pn[6]={"Late night drive","Gym","Sunday morning","Road trip 2026","Focus","Old favourites"};
        for(int i=0;i<6;i++){int bl=snprintf(bd,sizeof bd,"#EXTM3U\n%s\n%s\n",TP(1),TP(2));snprintf(pp,sizeof pp,FAKE_CARD "/Playlists/%s.m3u8",pn[i]);fake_put(pp,bd,(size_t)bl);}
        for(int t=0;t<2;t++){
            global_settings.sloppy_theme=t;appearance_reload((void*)2);frame(600);while(npages>1){page_pop();frame(400);}
            sl_saved_playlists_page(0);for(int i=0;i<10;i++)frame(100);
            snprintf(nm,sizeof nm,"%s/playlists-%s-%s.ppm",dir,t?"light":"dark",tag);shot(nm);
            sel_start(SEL_PLAYLISTS,pls_rows?pls_rows:top_body(),pages[npages-1],1);frame(300);
            snprintf(nm,sizeof nm,"%s/playlists-pick-%s-%s.ppm",dir,t?"light":"dark",tag);shot(nm);
            sel_end();frame(300);while(npages>1){page_pop();frame(400);}
        }
        global_settings.sloppy_theme=0;return 0;
    }
    if (getenv("SLOPPY_TL_SHOTS")) {   /* SLOPPY_TL_SHOTS=<dir> SLOPPY_AUDIT_TAG=before|after: the Tiles home in Dark, Light and True dark, music playing: top, middle, the apps, a tile held, a tile opening, the entrance */
        const char *dir=getenv("SLOPPY_TL_SHOTS"),*tag=getenv("SLOPPY_AUDIT_TAG")?getenv("SLOPPY_AUDIT_TAG"):"now";
        static const int ths[3]={0,1,2};static const char *const thn[3]={"dark","light","truedark"};char nm[512];
        global_settings.sloppy_notifications=0;test_real_art=true;remove(MT_FILE);
        test_status=AUDIO_STATUS_PLAY;
        if(getenv("SLOPPY_TL_ACCENT"))global_settings.sloppy_accent_color=(unsigned)strtoul(getenv("SLOPPY_TL_ACCENT"),NULL,16);   /* =2f6fd6: another accent */
        for(int t=0;t<3;t++){
            global_settings.sloppy_theme=ths[t];global_settings.sloppy_home_style=1;
            appearance_reload((void*)2);frame(600);while(npages>1){page_pop();frame(400);}
            mini_update();for(int i=0;i<20;i++)frame(100);
            snprintf(nm,sizeof nm,"%s/%s-top-%s.ppm",dir,thn[t],tag);shot(nm);
            lv_obj_scroll_to_y(mt.body,560,LV_ANIM_OFF);frame(300);snprintf(nm,sizeof nm,"%s/%s-middle-%s.ppm",dir,thn[t],tag);shot(nm);
            lv_obj_scroll_to_y(mt.body,LV_COORD_MAX,LV_ANIM_OFF);frame(300);snprintf(nm,sizeof nm,"%s/%s-apps-%s.ppm",dir,thn[t],tag);shot(nm);
            lv_obj_scroll_to_y(mt.body,0,LV_ANIM_OFF);frame(300);
            if(t)continue;
            lv_area_t a;lv_obj_get_coords(mt.slot[mt_find(TL_ALBUMS)].obj,&a);
            t_x=(a.x1+a.x2)/2;t_y=(a.y1+a.y2)/2;t_down=true;for(int i=0;i<12;i++)frame(16);
            snprintf(nm,sizeof nm,"%s/%s-held-%s.ppm",dir,thn[t],tag);shot_now(nm);
            t_down=false;frame(16);for(int i=0;i<40;i++)frame(16);
            frame(600);while(npages>1){page_pop();frame(400);}if(qp_open){qp_close();frame(400);}
            g_dragged=false;lv_obj_send_event(mt.slot[mt_find(TL_PLAYLISTS)].obj,LV_EVENT_CLICKED,NULL);
            static const int at[3]={48,112,208};int done=0;
            for(int k=0;k<3;k++){while(done<at[k]){frame(16);done+=16;}snprintf(nm,sizeof nm,"%s/%s-open-%dms-%s.ppm",dir,thn[t],at[k],tag);shot_now(nm);}
            frame(600);while(npages>1){page_pop();frame(400);}
            home_intro_ms=60;appearance_reload((void*)2);
            static const int in[4]={16,96,192,320};done=0;
            for(int k=0;k<4;k++){while(done<in[k]){frame(16);done+=16;}snprintf(nm,sizeof nm,"%s/%s-entrance-%dms-%s.ppm",dir,thn[t],in[k],tag);shot_now(nm);}
            home_intro_ms=0;frame(800);while(npages>1){page_pop();frame(400);}
        }
        global_settings.sloppy_theme=0;
        return 0;
    }
    if (getenv("SLOPPY_SET_SHOTS")) { set_shots(getenv("SLOPPY_SET_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "now"); return 0; }
    if (getenv("SLOPPY_FONT_SHOTS")) { xf_shots(getenv("SLOPPY_FONT_SHOTS"), getenv("SLOPPY_AUDIT_TAG") ? getenv("SLOPPY_AUDIT_TAG") : "after"); return 0; }
    if (getenv("SLOPPY_AUDIT")) {   /* SLOPPY_AUDIT=<dir>: every screen in every look, read for overlaps, clipping, chrome, tap size and contrast; then stop */
        au_full(getenv("SLOPPY_AUDIT"));
        return 0;
    }
    if (getenv("SLOPPY_DS_SHOTS")) {   /* SLOPPY_DS_SHOTS=<dir> SLOPPY_DS_TAG=before|after: the design-system screens, light and dark, then stop */
        const char *dir=getenv("SLOPPY_DS_SHOTS"),*tag=getenv("SLOPPY_DS_TAG")?getenv("SLOPPY_DS_TAG"):"now";
        char nm[512];snprintf(nm,sizeof nm,"mkdir -p '%s'",dir);system(nm);   /* the folder, as the other shot modes make theirs */
        #define DS_SHOT(s) do{snprintf(nm,sizeof nm,"%s/%s-%s-%s.ppm",dir,(s),th?"light":"dark",tag);shot(nm);}while(0)
        test_track.title="Night Drive";test_track.artist="Mira Sol";test_track.length=240000;test_track.elapsed=72000;
        test_status=AUDIO_STATUS_PLAY;queued=1;started=0;queue_tracks[0]=2;
        for(int th=0;th<2;th++){
            global_settings.sloppy_theme=th;global_settings.sloppy_home_style=1;
            appearance_reload((void*)2);frame(400);mini_update();frame(400);
            DS_SHOT("home-mini");
            music_page();frame(500);DS_SHOT("music");
            songs_page();frame(500);DS_SHOT("all-list");page_pop();frame(400);page_pop();frame(400);
            album_page(0);frame(500);DS_SHOT("album");page_pop();frame(400);
            system_page();frame(500);DS_SHOT("settings");
            sound_page();frame(500);DS_SHOT("slider");page_pop();frame(400);page_pop();frame(400);
            np_show();frame(600);uq_show();frame(600);DS_SHOT("queue-empty");uq_close();frame(500);np_hide(0);frame(500);
            global_settings.sloppy_notifications=0;songs_page();frame(500);
            toast("Added 12 songs to Road trip");frame(0);frame(700);DS_SHOT("toast-page");frame(5000);
            global_settings.sloppy_notifications=1;
            toast("Library updated");frame(0);frame(700);DS_SHOT("banner-page");frame(6000);
            page_pop();frame(400);
        }
        #undef DS_SHOT
        return 0;
    }
    if (getenv("SLOPPY_AL_SHOTS")) {   /* SLOPPY_AL_SHOTS=<dir> SLOPPY_DS_TAG=before|after: the All list, light and dark, then stop */
        const char *dir=getenv("SLOPPY_AL_SHOTS"),*tag=getenv("SLOPPY_DS_TAG")?getenv("SLOPPY_DS_TAG"):"now";
        char nm[512];snprintf(nm,sizeof nm,"mkdir -p '%s'",dir);system(nm);   /* the folder, as the other shot modes make theirs */
        #define AL_SHOT(s) do{snprintf(nm,sizeof nm,"%s/%s-%s-%s.ppm",dir,(s),th?"light":"dark",tag);shot(nm);}while(0)
        test_track.title="Night Drive";test_track.artist="Mira Sol";test_track.length=240000;test_track.elapsed=72000;
        test_status=AUDIO_STATUS_PLAY;queued=1;started=0;queue_tracks[0]=2;
        for(int th=0;th<2;th++){
            global_settings.sloppy_theme=th;global_settings.sloppy_home_style=1;global_settings.sloppy_notifications=0;
            appearance_reload((void*)2);frame(400);page_pop();frame(400);mini_update();frame(400);
            songs_sort=0;songs_page();frame(500);AL_SHOT("all-top");
            {   lv_obj_t *sp=pages[npages-1],*rail=NULL,*sb=NULL;
                for(uint32_t i=0;i<lv_obj_get_child_count(sp);i++){lv_obj_t *c=lv_obj_get_child(sp,i);if(lv_obj_get_width(c)==AZ_W && lv_obj_get_x(c)==AZ_X)rail=c;}
                for(uint32_t i=0;i<lv_obj_get_child_count(sp);i++)if(lv_obj_get_scroll_dir(lv_obj_get_child(sp,i))==LV_DIR_VER)sb=lv_obj_get_child(sp,i);
                if(sb){lv_obj_scroll_to_y(sb,5000*82,LV_ANIM_OFF);frame(100);}
                if(rail){struct az_state *r=lv_event_dsc_get_user_data(lv_obj_get_event_dsc(rail,lv_obj_get_event_count(rail)-1));
                    az_wake(r);frame(200);lv_label_set_text(r->bubble_txt,"M");lv_obj_set_y(r->bubble,300);lv_obj_remove_flag(r->bubble,LV_OBJ_FLAG_HIDDEN);
                    frame(100);shot_now((snprintf(nm,sizeof nm,"%s/all-rail-%s-%s.ppm",dir,th?"light":"dark",tag),nm));lv_obj_add_flag(r->bubble,LV_OBJ_FLAG_HIDDEN);}
            }
            page_pop();frame(400);
            songs_page();frame(500);songs_sort_cb(NULL);frame(400);AL_SHOT("all-sort-sheet");acts_close();frame(400);page_pop();frame(400);
            songs_sort=2;songs_page();frame(500);AL_SHOT("all-recent");page_pop();frame(400);songs_sort=0;
            {   const char *o1=sl_lib.tracks[1].title,*o2=sl_lib.tracks[2].title;
                sl_lib.tracks[1].title="רגעים";sl_lib.tracks[2].title="יום חדש - New Day";
                songs_page();frame(500);AL_SHOT("all-rtl");page_pop();frame(400);
                sl_lib.tracks[1].title=o1;sl_lib.tracks[2].title=o2; }
            {   songs_page();frame(500);
                lv_obj_t *sb=lv_obj_get_child(pages[npages-1],1),*host=NULL,*r0=NULL,*r1=NULL;
                for(uint32_t i=0;i<lv_obj_get_child_count(sb);i++)if(lv_obj_get_user_data(lv_obj_get_child(sb,i)))host=lv_obj_get_child(sb,i);
                for(uint32_t i=0;host&&i<lv_obj_get_child_count(host);i++){lv_obj_t *c=lv_obj_get_child(host,i);if(lv_obj_get_y(c)==0)r0=c;}
                if(r0){lv_obj_send_event(r0,LV_EVENT_LONG_PRESSED,NULL);frame(300);
                    for(uint32_t i=0;i<lv_obj_get_child_count(host);i++){lv_obj_t *c=lv_obj_get_child(host,i);if(lv_obj_get_y(c)==164)r1=c;}
                    if(r1)lv_obj_send_event(r1,LV_EVENT_CLICKED,NULL);frame(400);}
                AL_SHOT("all-picking");sel_end();frame(300);page_pop();frame(400); }
            songs_page();frame(500);toast("Added 12 songs to Road trip");frame(0);frame(700);AL_SHOT("all-toast");frame(5000);page_pop();frame(400);
            global_settings.sloppy_language=1;songs_page();frame(500);AL_SHOT("all-count-es");page_pop();frame(400);global_settings.sloppy_language=0;
            {   static const char *tn[]={"Track 10","Track 2","Track 1","Track 12","Track 3","Track 11","Track 9","Track 20","Track 4","Track 100"};
                int nt=sl_lib.n_tracks;const char *ot[10];for(int i=0;i<10;i++){ot[i]=sl_lib.tracks[i].title;sl_lib.tracks[i].title=tn[i];sl_lib.songs_by_title[i]=i;}
                sl_lib.n_tracks=10;
                for(int i=1;i<10;i++)for(int j=i;j>0&&sl_sort_cmp(sl_lib.tracks[sl_lib.songs_by_title[j-1]].title,sl_lib.tracks[sl_lib.songs_by_title[j]].title)>0;j--){int x=sl_lib.songs_by_title[j];sl_lib.songs_by_title[j]=sl_lib.songs_by_title[j-1];sl_lib.songs_by_title[j-1]=x;}
                songs_page();frame(500);AL_SHOT("all-natural");page_pop();frame(400);
                sl_lib.n_tracks=nt;for(int i=0;i<10;i++){sl_lib.tracks[i].title=ot[i];sl_lib.songs_by_title[i]=i;} }
        }
        if(getenv("SLOPPY_AL_ZULE")){   /* Zule's big title beside the header buttons */
            global_settings.sloppy_theme=3;global_settings.sloppy_home_style=2;appearance_reload((void*)2);frame(400);page_pop();frame(400);
            songs_page();frame(500);snprintf(nm,sizeof nm,"%s/all-top-zule-%s.ppm",dir,tag);shot(nm);page_pop();frame(400);
            global_settings.sloppy_theme=0;global_settings.sloppy_home_style=1; }
        #undef AL_SHOT
        return 0;
    }
    if (getenv("SLOPPY_SPACEY_SHOTS")) {   /* SLOPPY_SPACEY_SHOTS=<dir>: the Playroom with Spacey Lines, its hint, and a swirl in each tunnel, light and dark, then stop */
        const char *dir=getenv("SLOPPY_SPACEY_SHOTS");char nm[512];
        global_settings.sloppy_notifications=0;
        for(int th=0;th<2;th++){
            const char *tn=th?"light":"dark";
            global_settings.sloppy_theme=th;global_settings.sloppy_home_style=1;
            appearance_reload((void*)2);frame(400);while(npages>1){page_pop();frame(400);}
            sl_games_page();frame(500);
            {lv_obj_t *l=find_label(pages[npages-1],"Spacey Lines");assert(l);lv_obj_scroll_to_view_recursive(l,LV_ANIM_OFF);frame(100);}
            snprintf(nm,sizeof nm,"%s/playroom-%s.ppm",dir,tn);shot(nm);
            spc_hinted=false;spacey_page();frame(500);
            snprintf(nm,sizeof nm,"%s/spacey-hint-%s.ppm",dir,tn);shot(nm);
            lv_area_t sa;lv_obj_get_coords(spc.stage,&sa);int cx=(sa.x1+sa.x2)/2,cy=(sa.y1+sa.y2)/2;
            for(int mode=0;mode<2;mode++){
                int x=cx+60,y=cy;touch_at(x,y,true);   /* each touch turns the tunnel: odd, then even */
                for(int i=0;i<90;i++){float a=i*0.14f,r=60+i*1.5f;x=cx+(int)(r*cosf(a));y=cy+(int)(r*1.3f*sinf(a));touch_at(x,y,true);}
                touch_at(x,y,false);
                for(int f=1;f<=3;f++){for(int i=0;i<6;i++)frame(16);
                    snprintf(nm,sizeof nm,"%s/spacey-%s-%d-%s.ppm",dir,spc.clicks&1?"odd":"even",f,tn);shot_now(nm);}
            }
            for(int i=0;i<10;i++)touch_at(sa.x2+6,sa.y2+6,true);   /* a finger held in the bottom right corner: it all fades */
            snprintf(nm,sizeof nm,"%s/spacey-corner-fade-%s.ppm",dir,tn);shot_now(nm);touch_at(sa.x2+6,sa.y2+6,false);
            page_pop();frame(400);page_pop();frame(400);
        }
        return 0;
    }
    {   /* plugging in: the drop rises out of the port, holds, sinks back, and is gone */
        uint32_t top=lv_obj_get_child_count(lv_layer_top());
        charge_bubble_show(42);frame(0);frame(160);shot_now("charge-rise.ppm");
        assert(lv_obj_get_y(cb.drop)<H && lv_obj_get_y(cb.drop)>CB_REST-CB_D/2);
        frame(140);shot_now("charge-stretch.ppm");
        for(int i=0;i<6;i++)frame(100);shot_now("charge.ppm");
        assert(abs(lv_obj_get_y(cb.drop)+CB_D/2-CB_REST)<=4 && lv_obj_get_style_opa(cb.pct,0)==LV_OPA_COVER);
        charge_bubble_show(42);assert(lv_obj_get_child_count(lv_layer_top())==top+1);   /* one at a time */
        for(int i=0;i<10;i++)frame(100);shot_now("charge-sink.ppm");
        for(int i=0;i<8;i++)frame(100);
        assert(!cb.root && lv_obj_get_child_count(lv_layer_top())==top);
        /* unplugged on the way up: it goes straight back down */
        charge_bubble_show(42);frame(0);frame(200);charge_bubble_unplugged();for(int i=0;i<8;i++)frame(100);
        assert(!cb.root);
        /* charging: a bolt over the battery in the status bar */
        test_charging=true;statusbar_update();frame(50);assert(!lv_obj_has_flag(sb_bolt,LV_OBJ_FLAG_HIDDEN));
        shot_now("statusbar-charging.ppm");
        test_charging=false;statusbar_update();assert(lv_obj_has_flag(sb_bolt,LV_OBJ_FLAG_HIDDEN));
    }
    {   /* Music: the search bar, then six tiles with their counts and ten low cards (Books, Podcasts, then a music server's Server before Radio); Songs opens with a big Shuffle */
        music_page();frame(500);
        lv_obj_t *body=lv_obj_get_child(pages[npages-1],1);
        if(lv_obj_get_child_count(body)!=17)body=lv_obj_get_child(pages[npages-1],0);
        assert(lv_obj_get_child_count(body)==17);
        assert(find_label(body,"Server"));
        {   /* a deliberate fold: the first low row ends a gutter above the mini player, the next starts under it */
            lv_area_t a,b;lv_obj_get_coords(lv_obj_get_child(body,7),&a);lv_obj_get_coords(lv_obj_get_child(body,9),&b);
            assert(a.y2+1==H-SL_FLOAT-82-SL_GUTTER && b.y1>=H-SL_FLOAT-82);
            /* the tiles drawn in one family, and the Songs tile counts the library */
            char n[16];assert(find_label(lv_obj_get_child(body,2),sl_fmt_count(n,sizeof n,sl_lib.n_tracks)));
        }
        shot("music.ppm");
        page_pop();frame(500);
        /* arranged: Radio up to the top, Loved hidden; kept on the card; and back as it was */
        remove(MU_FILE);mu_read=false;mu_load();
        mu_arrange_page();frame(300);
        {   /* Radio's handle, dragged up past every card: it stays among the cards, held at the top
             * while the page scrolls on; a tile's handle can't take it among the cards */
            lv_obj_t *rw=lv_obj_get_parent(find_label(pages[npages-1],"Radio"));
            int sl=0;while(sl<mua[1].n&&mua[1].rows[sl]!=rw)sl++;
            assert(mua[0].n==6&&mua[1].n==MU_N-6&&sl==MU_N-7);
            lv_obj_scroll_to_view_recursive(rw,LV_ANIM_OFF);frame(300);
            lv_area_t a,b;lv_obj_get_coords(lv_obj_get_child(rw,2),&a);lv_obj_get_coords(mua[1].body,&b);   /* name, eye, handle */
            t_x=(a.x1+a.x2)/2;t_y=(a.y1+a.y2)/2;t_down=true;frame(30);
            for(int i=1;i<=10;i++){t_y=(a.y1+a.y2)/2-i*((a.y1+a.y2)/2-b.y1-30)/10;frame(30);}
            for(int i=0;i<300&&mua[1].target;i++)frame(30);
            assert(mua[1].drag==rw&&mua[1].target==0&&!mua[0].drag);
            t_down=false;frame(30);frame(400);assert(!mua[1].drag&&mua[1].rows[0]&&find_label(mua[1].rows[0],"Radio"));
            lv_obj_scroll_to_y(mua[0].body,0,LV_ANIM_OFF);frame(300);
            lv_obj_get_coords(lv_obj_get_child(mua[0].rows[5],2),&a);   /* the last tile, let go where it was */
            t_x=(a.x1+a.x2)/2;t_y=(a.y1+a.y2)/2;t_down=true;frame(30);
            for(int i=1;i<=10;i++){t_y=(a.y1+a.y2)/2+i*mua[0].row_h*3/10;frame(30);}
            assert(mua[0].drag&&mua[0].target==5);
            t_down=false;frame(30);frame(400);assert(!mua[0].drag&&mu_defs[mu_order[5]].act==ACT_LOVED);
        }
        /* Loved's eye hides it, a tap on its row shows it, the eye hides it again */
        lv_obj_send_event(lv_obj_get_child(lv_obj_get_parent(find_label(pages[npages-1],"Loved")),1),LV_EVENT_CLICKED,NULL);frame(300);
        assert(mu_hidden[5]);
        lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages-1],"Loved")),LV_EVENT_CLICKED,NULL);frame(300);
        assert(!mu_hidden[5]);
        lv_obj_send_event(lv_obj_get_child(lv_obj_get_parent(find_label(pages[npages-1],"Loved")),1),LV_EVENT_CLICKED,NULL);frame(300);
        assert(mu_hidden[5]);
        lv_obj_scroll_to_y(mua[0].body,0,LV_ANIM_OFF);frame(300);
        shot("music-arrange.ppm");
        int ra=0;while(mu_defs[mu_order[ra]].act!=ACT_WEBRADIO)ra++;
        for(int k=0;k<ra;k++)assert(mu_defs[mu_order[k]].tile);   /* first of the cards, after every tile */
        assert(mu_defs[mu_order[ra+1]].act==ACT_ALBUMARTISTS && mu_defs[mu_order[ra+2]].act==ACT_GENRES);
        mu_read=false;mu_load();assert(mu_defs[mu_order[ra]].act==ACT_WEBRADIO && mu_hidden[5]);
        page_pop();frame(300);music_page();frame(500);shot("music-arranged.ppm");
        body=lv_obj_get_child(pages[npages-1],1);assert(lv_obj_get_child_count(body)==16 && !find_label(pages[npages-1],"Loved"));   /* seventeen, less Loved */
        page_pop();frame(400);
        remove(MU_FILE);mu_read=false;mu_load();
        {   /* the Tiles home as arranged: places, sizes, live faces and shades come back after Cards, another theme and a restart */
            int hs=global_settings.sloppy_home_style,th=global_settings.sloppy_theme;
            global_settings.sloppy_home_style=1;appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(300);}
            mt_load();assert(mt.n>4);
            struct mt_slot t=mt.slot[0];mt.slot[0]=mt.slot[3];mt.slot[3]=t;   /* moved */
            mt.slot[1].size=mt.slot[1].size==SZ_WIDE?SZ_SMALL:SZ_WIDE;mt.slot[2].live=0;   /* resized, its face still */
            memmove(&mt.slot[4],&mt.slot[5],(size_t)(mt.n-5)*sizeof mt.slot[0]);mt.n--;   /* unpinned */
            mt_save();appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(300);}
            uint8_t id[TL_COUNT],sz[TL_COUNT],lv[TL_COUNT];uint32_t col[TL_COUNT];int n=mt.n;
            for(int i=0;i<n;i++){id[i]=mt.slot[i].id;sz[i]=mt.slot[i].size;lv[i]=mt.slot[i].live;
                col[i]=mt.slot[i].obj?lv_color_to_u32(lv_obj_get_style_bg_color(mt.slot[i].obj,0)):0;}
            global_settings.sloppy_home_style=0;appearance_reload(NULL);frame(600);
            global_settings.sloppy_theme=th==1?0:1;global_settings.sloppy_home_style=1;appearance_reload(NULL);frame(600);
            global_settings.sloppy_theme=th;appearance_reload(NULL);frame(600);
            memset(mt.slot,0,sizeof mt.slot);mt.n=0;   /* a restart: only the card remembers */
            global_settings.sloppy_home_style=1;appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(300);}
            assert(mt.n==n);
            for(int i=0;i<n;i++){assert(mt.slot[i].id==id[i]&&mt.slot[i].size==sz[i]&&mt.slot[i].live==lv[i]);
                if(mt.slot[i].obj&&col[i]&&lv_color_to_u32(lv_obj_get_style_bg_color(mt.slot[i].obj,0))!=col[i])
                    {printf("tile %d (id %d): %06x, was %06x\n",i,id[i],(unsigned)lv_color_to_u32(lv_obj_get_style_bg_color(mt.slot[i].obj,0)),(unsigned)col[i]);assert(0);}}
            remove(MT_FILE);global_settings.sloppy_home_style=hs;global_settings.sloppy_theme=th;appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(300);}
            printf("PASS: Tiles home: moved, resized, a still face and an unpinned tile kept through Cards, a theme and a restart, each tile its own shade.\n");
        }
        songs_page();frame(500);shot("songs.ppm");
        {   /* picking songs: hold one, tap another, the bar counts them; Love loves both; a tap
             * on a picked row unpicks it; the last one unpicked ends picking; leaving clears it */
            lv_obj_t *sb=lv_obj_get_child(pages[npages-1],1),*host=NULL,*r0=NULL,*r1=NULL;
            for(uint32_t i=0;i<lv_obj_get_child_count(sb);i++)if(lv_obj_get_user_data(lv_obj_get_child(sb,i)))host=lv_obj_get_child(sb,i);
            assert(host);
            for(uint32_t i=0;i<lv_obj_get_child_count(host);i++){lv_obj_t *c=lv_obj_get_child(host,i);if(lv_obj_get_y(c)==0)r0=c;if(lv_obj_get_y(c)==82)r1=c;}
            assert(r0&&r1);int t0=(int)(intptr_t)lv_obj_get_user_data(r0),t1=(int)(intptr_t)lv_obj_get_user_data(r1);
            {   /* a row eases into its pressed shade and settles back out of it, as the tiles do */
                lv_obj_add_state(r0,LV_STATE_PRESSED);frame(0);frame(SL_T_TOUCH/2);
                int mid=lv_obj_get_style_bg_opa(r0,0);frame(SL_T_TOUCH);assert(mid>0 && mid<LV_OPA_COVER && lv_obj_get_style_bg_opa(r0,0)==LV_OPA_COVER);
                lv_obj_remove_state(r0,LV_STATE_PRESSED);frame(0);frame(SL_T_PAGE/2);
                mid=lv_obj_get_style_bg_opa(r0,0);frame(SL_T_PAGE);assert(mid>0 && mid<LV_OPA_COVER && lv_obj_get_style_bg_opa(r0,0)==LV_OPA_TRANSP); }
            lv_obj_send_event(r0,LV_EVENT_LONG_PRESSED,NULL);frame(300);
            assert(sel_on() && sel_count==1 && sel_has(t0) && lv_obj_has_flag(mini,LV_OBJ_FLAG_HIDDEN));
            for(uint32_t i=0;i<lv_obj_get_child_count(host);i++){lv_obj_t *c=lv_obj_get_child(host,i);if(lv_obj_get_y(c)==82)r1=c;}
            int was=started;started=-7;lv_obj_send_event(r1,LV_EVENT_CLICKED,NULL);frame(100);
            assert(sel_count==2 && sel_has(t1) && started==-7);started=was;   /* picked, not played */
            assert(find_label(pages[npages-1],"2 songs"));shot("songs-select.ppm");
            lv_obj_send_event(r1,LV_EVENT_CLICKED,NULL);frame(100);assert(sel_count==1 && !sel_has(t1));
            lv_obj_send_event(r1,LV_EVENT_CLICKED,NULL);frame(100);assert(sel_count==2);
            sel_love_cb(NULL);frame(300);assert(!sel_on() && ts_rating(t0)==1 && ts_rating(t1)==1);
            ts_love(t0,false);ts_love(t1,false);
            for(uint32_t i=0;i<lv_obj_get_child_count(host);i++){lv_obj_t *c=lv_obj_get_child(host,i);if(lv_obj_get_y(c)==0)r0=c;}
            #define SEL_ROW0() for(uint32_t i=0;i<lv_obj_get_child_count(host);i++){lv_obj_t *c=lv_obj_get_child(host,i);if(lv_obj_get_y(c)==0)r0=c;}
            lv_obj_send_event(r0,LV_EVENT_LONG_PRESSED,NULL);frame(300);assert(sel_on());SEL_ROW0();   /* the rows were made again */
            lv_obj_send_event(r0,LV_EVENT_CLICKED,NULL);frame(300);assert(!sel_on());SEL_ROW0();   /* the last one unpicked */
            lv_obj_send_event(r0,LV_EVENT_LONG_PRESSED,NULL);frame(300);assert(sel_on());
            /* Select all picks the whole list; again, none, and picking ends */
            assert(has_text(sel_bar,"Select all"));
            sel_all_cb(NULL);frame(300);assert(sel_on() && sel_count==sl_lib.n_tracks && has_text(sel_bar,"Deselect all") && find_label(sel_bar,"10,000 songs"));
            sel_all_cb(NULL);frame(300);assert(!sel_on());SEL_ROW0();   /* the rows were made again */
            lv_obj_send_event(r0,LV_EVENT_LONG_PRESSED,NULL);frame(300);assert(sel_on());
            #undef SEL_ROW0
        }
        page_pop();frame(500);assert(!sel_on() && !sel_bits);
        /* the sort button: newest first is its own list, played from the row tapped; back to A-Z after */
        songs_sort=2;songs_page();frame(500);shot("songs-newest.ppm");
        assert(find_label(pages[npages-1],"10,000 SONGS") && find_label(pages[npages-1],"Recently added"));
        {   /* no rail in this order: no rail's gutter, and a scroll bar says where the list is */
            lv_obj_t *nb=lv_obj_get_child(pages[npages-1],1);
            assert(lv_obj_get_style_pad_right(nb,0)==SL_GUTTER && lv_obj_get_scrollbar_mode(nb)==LV_SCROLLBAR_MODE_AUTO); }
        /* the order button says the order, and opens a sheet with it ticked; a pick is kept */
        lv_obj_t *ob=find_label(pages[npages-1],"Recently added");while(ob&&!lv_obj_has_flag(ob,LV_OBJ_FLAG_CLICKABLE))ob=lv_obj_get_parent(ob);
        assert(ob);lv_obj_send_event(ob,LV_EVENT_CLICKED,NULL);frame(300);
        assert(acts_open_now && find_label(acts,"Sort by") && find_label(acts,"Recently added"));shot("songs-sort-sheet.ppm");
        lv_obj_t *az=find_label(acts,"A to Z");while(az&&!lv_obj_has_flag(az,LV_OBJ_FLAG_CLICKABLE))az=lv_obj_get_parent(az);
        lv_obj_send_event(az,LV_EVENT_CLICKED,NULL);frame(400);
        assert(!acts_open_now && songs_sort==0 && global_settings.sloppy_songs_sort==0 && find_label(pages[npages-1],"A to Z"));
        page_pop();frame(500);songs_sort=0;
        {   /* Z to A has its rail too, its letters the list's way down */
            songs_sort=1;songs_page();frame(500);struct az_state *r=NULL;lv_obj_t *pg=pages[npages-1];
            for(uint32_t i=0;i<lv_obj_get_child_count(pg)&&!r;i++){lv_obj_t *c=lv_obj_get_child(pg,i);
                for(uint32_t k=0;k<lv_obj_get_event_count(c);k++){lv_event_dsc_t *d=lv_obj_get_event_dsc(c,k);if(lv_event_dsc_get_cb(d)==az_rail_cb)r=lv_event_dsc_get_user_data(d);}}
            assert(r && r->nb>=2);   /* each mark the letter of the row it goes to (this fixture's list isn't in order) */
            struct song_list_view *zv=pm_songs(pg);assert(zv);
            for(int k=0;k<r->nb;k++){int c=az_initial(sl_lib.tracks[zv->tracks[r->start[k]]].title);assert(r->lab[k][0]==(c=='#'?'#':c));}
            shot("songs-za-rail.ppm");page_pop();frame(500);songs_sort=0; }
        {   /* the song sheet's Song info: what the library knows, on a page of its own */
            int t0=sl_lib.songs_by_title[0];acts_open(t0);frame(300);
            lv_obj_t *si=find_label(acts,"Song info");while(si&&!lv_obj_has_flag(si,LV_OBJ_FLAG_CLICKABLE))si=lv_obj_get_parent(si);
            assert(si);int depth=npages;lv_obj_send_event(si,LV_EVENT_CLICKED,NULL);frame(400);
            assert(npages==depth+1&&find_label(pages[npages-1],"Song info")&&find_label(pages[npages-1],TP(t0)));
            shot("song-info.ppm");page_pop();frame(300);
        }
        {   /* an artist: how much of them, and all of it played from the top, album by album */
            int k0=0;while(k0<sl_lib.n_albums&&!sl_lib.albums[sl_lib.albums_by_artist[k0]].n_tracks)k0++;
            assert(k0<sl_lib.n_albums);const char *nm=sl_lib.albums[sl_lib.albums_by_artist[k0]].artist;
            while(k0>0&&!strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[k0-1]].artist,nm))k0--;
            int na=0,songs=0;
            while(k0+na<sl_lib.n_albums&&!strcasecmp(sl_lib.albums[sl_lib.albums_by_artist[k0+na]].artist,nm))songs+=sl_lib.albums[sl_lib.albums_by_artist[k0+na++]].n_tracks;
            artist_page(k0);frame(500);shot("artist.ppm");
            assert(find_label(pages[npages-1],"Play") || has_text(pages[npages-1],"Play"));
            lv_obj_t *pb=NULL;for(lv_obj_t *l=find_label(pages[npages-1],LV_SYMBOL_PLAY "  Play");l;l=NULL){pb=l;while(pb&&!lv_obj_has_flag(pb,LV_OBJ_FLAG_CLICKABLE))pb=lv_obj_get_parent(pb);}
            assert(pb);int st=test_status;lv_obj_send_event(pb,LV_EVENT_CLICKED,NULL);frame(200);
            assert(queued==songs);
            test_status=st;if(np_open)np_hide(0);frame(300);page_pop();frame(400);
        }
        {   /* an album on two discs: a heading over each, and the rows below them in step */
            int big=0;for(int i=0;i<sl_lib.n_albums;i++)if(sl_lib.albums[i].n_tracks>sl_lib.albums[big].n_tracks)big=i;
            const struct sl_album *al=&sl_lib.albums[big];int half=al->n_tracks>40?20:al->n_tracks/2;
            int d0[64];for(int i=0;i<al->n_tracks&&i<64;i++){d0[i]=sl_lib.tracks[al->tracks[i]].disc;sl_lib.tracks[al->tracks[i]].disc=i<half?1:2;}
            int saved_disc_rest=al->n_tracks>64?sl_lib.tracks[al->tracks[64]].disc:0;
            for(int i=64;i<al->n_tracks;i++)sl_lib.tracks[al->tracks[i]].disc=2;
            album_page(big);frame(500);
            assert(find_label(pages[npages-1],"DISC 1"));
            lv_obj_t *body=lv_obj_get_child(pages[npages-1],1);
            lv_obj_scroll_to_y(body,(half+3)*82,LV_ANIM_OFF);frame(100);
            lv_obj_t *d2=find_label(pages[npages-1],"DISC 2");assert(d2);
            shot("album-discs.ppm");
            /* the first song of disc 2 sits under its heading, not over it */
            lv_obj_t *host=lv_obj_get_parent(d2);bool under=false;
            for(unsigned i=0;i<lv_obj_get_child_count(host);i++){lv_obj_t *r=lv_obj_get_child(host,i);
                if((int)(intptr_t)lv_obj_get_user_data(r)==al->tracks[half]&&!lv_obj_check_type(r,&lv_label_class))
                    under=lv_obj_get_y(r)>=lv_obj_get_y(d2)+20;}
            assert(under);
            page_pop();frame(400);
            for(int i=0;i<al->n_tracks&&i<64;i++)sl_lib.tracks[al->tracks[i]].disc=d0[i];
            for(int i=64;i<al->n_tracks;i++)sl_lib.tracks[al->tracks[i]].disc=saved_disc_rest;
        }
        {   /* sleep timer at the end of this song, or of this album: a pause just before the end, not after */
            int big=0;for(int i=0;i<sl_lib.n_albums;i++)if(sl_lib.albums[i].n_tracks>sl_lib.albums[big].n_tracks)big=i;
            assert(sl_lib.albums[big].n_tracks>=2);
            int st=test_status;struct mp3entry tt=test_track,tn=test_next;
            const struct sl_track *a0=&sl_lib.tracks[sl_lib.albums[big].tracks[0]],*a1=&sl_lib.tracks[sl_lib.albums[big].tracks[1]];
            test_status=AUDIO_STATUS_PLAY;strlcpy(test_track.path,TPT(a0),sizeof test_track.path);
            test_track.length=200000;test_track.elapsed=100000;strlcpy(test_next.path,TPT(a1),sizeof test_next.path);
            sleep_set(SLEEP_SONG);sleep_end_poll(250);assert(is_playing() && sleep_choice==SLEEP_SONG);
            test_track.elapsed=199700;sleep_end_poll(250);assert(!is_playing() && sleep_choice==0);
            test_status=AUDIO_STATUS_PLAY;test_track.elapsed=1000;
            sleep_set(SLEEP_ALBUM);test_track.elapsed=199700;sleep_end_poll(250);assert(is_playing());   /* the album goes on */
            strlcpy(test_next.path,"/elsewhere.mp3",sizeof test_next.path);sleep_end_poll(1000);assert(!is_playing() && !sleep_choice);
            test_status=AUDIO_STATUS_PLAY;sleep_set(SLEEP_SONG);strlcpy(test_track.path,TPT(a1),sizeof test_track.path);
            sleep_end_poll(250);assert(is_playing() && !sleep_choice);   /* skipped by hand: the timer lets go */
            /* the song ran out between polls (a stall): stopped anyway, not taken for a skip */
            strlcpy(test_track.path,TPT(a0),sizeof test_track.path);test_track.elapsed=199000;sleep_set(SLEEP_SONG);sleep_end_poll(250);
            assert(is_playing());strlcpy(test_track.path,TPT(a1),sizeof test_track.path);test_track.elapsed=500;sleep_end_poll(250);
            assert(!is_playing() && !sleep_choice);test_status=AUDIO_STATUS_PLAY;
            /* repeat all on one album: the wrap back to its first song is the end */
            strlcpy(test_track.path,TPT(a1),sizeof test_track.path);test_track.elapsed=199700;strlcpy(test_next.path,TPT(a0),sizeof test_next.path);
            sleep_set(SLEEP_ALBUM);sleep_end_poll(250);assert(!is_playing() && !sleep_choice);test_status=AUDIO_STATUS_PLAY;
            char lb[24];assert(!strcmp(qp_tile_label(4,lb,sizeof lb),"Sleep"));
            sleep_set(SLEEP_ALBUM);assert(!strcmp(qp_tile_label(4,lb,sizeof lb),"End of album"));sleep_set(0);
            /* minutes: kept for the tile, which starts and stops it with one tap; at the end everything stops and it goes off */
            global_settings.sloppy_sleep_timer=2;sleep_set(3);assert(global_settings.sloppy_sleep_timer==3);sleep_set(0);
            assert(global_settings.sloppy_sleep_timer==3&&!sleep_choice);   /* off keeps the length */
            {   int sv=settings_save_calls;qp_sleep_page();frame(300);   /* picked on its page: saved, so a restart keeps it for the tile */
                lv_obj_t *r=lv_obj_get_parent(find_label(pages[npages-1],"60 minutes"));lv_obj_send_event(r,LV_EVENT_CLICKED,NULL);frame(100);
                assert(sleep_choice==3&&settings_save_calls>sv);sleep_set(0);page_pop();frame(300);}
            sleep_tile_tap();assert(sleep_choice==3&&!strcmp(qp_tile_label(4,lb,sizeof lb),"60 min"));
            sleep_tile_tap();assert(!sleep_choice&&global_settings.sloppy_sleep_timer==3);
            test_status=AUDIO_STATUS_PLAY;sleep_tile_tap();sleep_end_poll(250);assert(is_playing()&&sleep_choice==3);
            /* the last 30 s ease down to silence (#38): asked once, for the time left, then let go at the end */
            {int n=test_sleep_fades;sleep_end_ms=sl_ms()+45000;sleep_end_poll(1000);assert(test_sleep_fades==n);
             sleep_end_ms=sl_ms()+29500;sleep_end_poll(1000);assert(test_sleep_fades==n+1&&test_sleep_fade==30*HZ-HZ/2&&!test_sleep_fade_voice&&sleep_fading);
             sleep_end_poll(1000);assert(test_sleep_fades==n+1);   /* once */
             sleep_end_ms=sl_ms()-1;sleep_end_poll(250);assert(!is_playing()&&!sleep_choice&&!sleep_fading&&test_sleep_fade==-1&&test_sleep_fades==n+2);
             /* turned off on the way down: the level comes straight back */
             test_status=AUDIO_STATUS_PLAY;sleep_set(1);sleep_end_ms=sl_ms()+10000;sleep_end_poll(1000);assert(sleep_fading&&test_sleep_fade==10*HZ);
             sleep_set(0);assert(!sleep_fading&&test_sleep_fade==0&&test_sleep_fades==n+4);
             test_status=AUDIO_STATUS_PLAY;sleep_set(3);sleep_end_ms=sl_ms()-1;}
            sleep_end_poll(250);assert(!is_playing()&&!sleep_choice&&global_settings.sloppy_sleep_timer==3);
            /* end of song kept, nothing playing: the tap gives 30 minutes and keeps the choice */
            test_status=0;global_settings.sloppy_sleep_timer=SLEEP_SONG;sleep_tile_tap();
            assert(sleep_choice==2&&global_settings.sloppy_sleep_timer==SLEEP_SONG);sleep_set(0);
            /* the battery would run out first: said, once, as a warning */
            test_status=AUDIO_STATUS_PLAY;test_battery_minutes=20;sleep_set(3);sleep_battery_check();
            frame(0);frame(300);assert(has_text(banner_obj,"about 20 min left"));for(int i=0;i<12;i++)frame(400);
            assert(lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN));
            test_battery_minutes=600;sleep_set(0);global_settings.sloppy_sleep_timer=2;
            test_status=st;test_track=tt;test_next=tn;
        }
    }
    /* home motion: the cascade and icon rise settle exactly at rest, with no scaled layers left behind */
    { lv_obj_t *g=lv_obj_get_child(lv_obj_get_child(pages[0],0),0);
      for(uint32_t i=0;i<lv_obj_get_child_count(g);i++){lv_obj_t *c=lv_obj_get_child(g,i);
        assert(lv_obj_get_style_translate_y(c,0)==0 && lv_obj_get_style_opa(c,0)==LV_OPA_COVER);
        assert(lv_obj_get_style_translate_y(lv_obj_get_child(c,0),0)==0 && lv_obj_get_style_transform_scale_x(lv_obj_get_child(c,0),0)==256);} }
    /* loading banner: rises in, sweeps, fills smoothly, then fades out and is freed */
    { uint32_t top=lv_obj_get_child_count(lv_layer_top());
      test_lib_scanning=false;banner_show();assert(!lib_banner);test_lib_scanning=true;   /* library.bin: under a second, no banner */
      banner_show();lv_obj_t *first=lib_banner;banner_show();assert(lib_banner==first);   /* shown once */
      frame(0);frame(120);shot_now("banner-in.ppm");frame(400);shot_now("banner-sweep.ppm");
      sl_lib.scan_total=300;sl_lib.scan_done=180;banner_update();frame(300);shot_now("banner-progress.ppm");
      assert(lv_obj_get_width(lib_banner_fill)==BANNER_TRACK*60/100 && lv_obj_get_x(lib_banner_fill)==0);
      sl_lib.scan_total=sl_lib.scan_done=0;
      banner_hide();assert(!lib_banner);frame(0);frame(100);shot_now("banner-out.ppm");frame(300);
      assert(lv_obj_get_child_count(lv_layer_top())==top); }
    /* toast: slides in, lifts away and ends hidden at rest */
    toast("Hello");frame(0);frame(80);shot_now("toast-in.ppm");frame(300);assert(lv_obj_get_style_opa(banner_obj,0)==LV_OPA_COVER);
    frame(2400);frame(300);assert(lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN)&&lv_obj_get_style_opa(banner_obj,0)==LV_OPA_COVER);
    qp_build();qp_refresh();qp_glass_capture();lv_obj_remove_flag(qp,LV_OBJ_FLAG_HIDDEN);qp_open=true;shot("control-center.ppm");
    {   /* no glass on a pull (short of memory on the R1): the panel is solid,
         * never clear over the page from the pull before */
        assert(qp_glass_ok && lv_obj_get_style_bg_opa(qp,0)==LV_OPA_TRANSP);
        test_no_screen=true;qp_glass_capture();test_no_screen=false;
        assert(!qp_glass_ok && lv_obj_get_style_bg_opa(qp,0)==LV_OPA_COVER);
        qp_glass_capture();assert(qp_glass_ok && lv_obj_get_style_bg_opa(qp,0)==LV_OPA_TRANSP);
    }
    bool shuffle_before=global_settings.playlist_shuffle;
    lv_obj_send_event(qp_tiles[0],LV_EVENT_LONG_PRESSED,NULL);
    lv_obj_send_event(qp_tiles[0],LV_EVENT_CLICKED,NULL);
    assert(global_settings.playlist_shuffle==shuffle_before && npages==2);
    frame(500);page_pop();frame(500);
    const int long_tiles[]={3,4,5,7};
    for(unsigned j=0;j<sizeof long_tiles/sizeof long_tiles[0];j++) {
        int k=long_tiles[j];bool eq_before=global_settings.eq_enabled;int sleep_before=sleep_choice;
        qp_open=true;lv_obj_remove_flag(qp,LV_OBJ_FLAG_HIDDEN);lv_obj_set_style_translate_y(qp,0,0);
        lv_obj_send_event(qp_tiles[k],LV_EVENT_LONG_PRESSED,NULL);
        lv_obj_send_event(qp_tiles[k],LV_EVENT_CLICKED,NULL);
        assert(npages==2 && global_settings.eq_enabled==eq_before && sleep_choice==sleep_before);
        frame(500);page_pop();frame(500);
    }
    lv_obj_add_flag(qp,LV_OBJ_FLAG_HIDDEN);qp_open=false;frame(100);
    {   /* A dim screen accepts its first tap; mini-player swipes skip tracks. */
        struct sl_power_state p;
        sl_power_state_init(&p,sl_ms());p.mode=SL_POWER_DIM;
        assert(!sl_power_state_touch(&p,true,80,80,sl_ms()) && p.mode==SL_POWER_ACTIVE);
        assert(!sl_power_state_touch(&p,false,80,80,sl_ms()));
        test_status=AUDIO_STATUS_PLAY;
        test_track.title="Blue Hours";test_track.artist="Mira Sol";
        test_track.length=180000;test_track.elapsed=10000;
        mini_update();frame(100);
        touch_at(80,H-50,true);touch_at(80,H-50,false);
        assert(np_open);np_hide(0);frame(500);
        int next=audio_nexts, prev=audio_prevs;
        touch_at(320,H-50,true);touch_at(270,H-50,true);
        touch_at(190,H-50,true);touch_at(150,H-50,false);
        assert(audio_nexts==next+1 && audio_prevs==prev && !np_open);
        frame(200);
        touch_at(80,H-50,true);touch_at(130,H-50,true);
        touch_at(210,H-50,true);touch_at(220,H-50,false);
        assert(audio_prevs==prev+1 && audio_nexts==next+1 && !np_open);
        for(int k=0;k<12;k++)frame(50);assert(lv_obj_get_style_translate_x(mini_txt,0)==0 && !mini_leaving);
        /* a slow, careful swipe is a swipe too, not a tap that opens Now Playing */
        for(int x=320;x>=170;x-=15){touch_at(x,H-50,true);frame(60);}
        touch_at(170,H-50,false);
        assert(audio_nexts==next+2 && !np_open);for(int k=0;k<12;k++)frame(50);
        /* the title follows the finger one to one; a short nudge springs back */
        test_next.title="Next Light";test_next.artist="Mira Sol";
        touch_at(240,H-50,true);touch_at(225,H-50,true);touch_at(200,H-50,true);
        assert(g_state==G_MINI && lv_obj_get_style_translate_x(mini_txt,0)==-40);
        /* with the next song peeking in beside it */
        assert(!strcmp(lv_label_get_text(mini_peek_title),"Next Light") &&
               lv_obj_get_style_translate_x(mini_peek,0)==MINI_STEP-40);
        touch_at(110,H-50,true);lv_refr_now(disp);shot_now("mini-peek.ppm");touch_at(200,H-50,true);
        frame(200);touch_at(200,H-50,false);for(int k=0;k<12;k++)frame(50);
        assert(audio_prevs==prev+1 && audio_nexts==next+2 && !np_open && lv_obj_get_style_translate_x(mini_txt,0)==0 &&
               lv_obj_get_style_translate_x(mini_peek,0)==MINI_STEP && !strcmp(lv_label_get_text(mini_title),"Blue Hours"));
        {   /* the cover (P1): none while its thumbnail is not made, never a stand-in; when it is, it fades in as the words make room */
            char keep[MAX_PATH];strlcpy(keep,test_track.path,sizeof keep);sl_track_path(0,test_track.path,sizeof test_track.path);
            bool ra=test_real_art;test_real_art=false;mini_art_path[0]=0;mini_art_show(-1,false);mini_update();frame(0);
            assert(lv_obj_has_flag(mini_art,LV_OBJ_FLAG_HIDDEN) && lv_obj_get_style_x(mini_lane,0)==SL_INSET && mini_txt_w==MINI_TXT_W);
            frame(100);mini_update();assert(lv_obj_has_flag(mini_art,LV_OBJ_FLAG_HIDDEN));   /* nothing new made: not looked for again */
            test_real_art=true;sl_art_made++;mini_update();frame(SL_T_PAGE/2);
            assert(!lv_obj_has_flag(mini_art,LV_OBJ_FLAG_HIDDEN) && lv_obj_get_style_opa(mini_art,0)>0 && lv_obj_get_style_opa(mini_art,0)<255);
            for(int k=0;k<8;k++)frame(50);
            assert(lv_obj_get_style_x(mini_lane,0)==MINI_ART_X && mini_txt_w==MINI_TXT_W-(MINI_ART_X-SL_INSET) &&
                   lv_obj_get_style_opa(mini_art,0)==255 && lv_obj_get_child_count(mini_art)==1 && lv_obj_get_width(mini_title)==mini_txt_w);
            shot("mini-cover.ppm");
            test_real_art=ra;sl_art_made--;strlcpy(test_track.path,keep,sizeof test_track.path);mini_art_path[0]=0;mini_art_show(-1,false);mini_update();frame(0);
            assert(lv_obj_get_style_x(mini_lane,0)==SL_INSET && mini_txt_w==MINI_TXT_W && !lv_obj_get_child_count(mini_art));
            printf("PASS: the mini player's cover waits for its thumbnail, then fades in as the words make room.\n");
        }
        /* a quick flick skips even when short */
        touch_at(300,H-50,true);touch_at(285,H-50,true);touch_at(255,H-50,true);touch_at(250,H-50,false);
        assert(audio_nexts==next+3 && !np_open);for(int k=0;k<12;k++)frame(50);
        {   /* the idle touch reader is paused; a quick tap wakes it but has lifted
             * before the first read: the tap still lands where it was */
            frame(600);last_touch_ms=sl_ms()-5000;t_down=false;
            assert(touch_idle(sl_ms()) && lv_timer_get_paused(lv_indev_get_read_timer(touch)));
            t_event_pos=(80<<16)|(H-50);touch_wake(BUTTON_TOUCHSCREEN,button_get_data());
            frame(40);frame(40);
            assert(np_open && !touch_paused && tap_replay==0);np_hide(0);frame(500);
            /* a finger still down when the reader looks is read as it is */
            last_touch_ms=sl_ms()-5000;assert(touch_idle(sl_ms()));
            t_x=80;t_y=H-50;t_down=true;touch_wake(BUTTON_TOUCHSCREEN,button_get_data());frame(40);
            assert(tap_replay==0);touch_at(80,H-50,false);
            assert(np_open);np_hide(0);frame(500);
        }
        music_page(); /* a fresh contact works while the page is still sliding */
        touch_at(80,H-50,true);touch_at(80,H-50,false);
        assert(np_open);np_hide(0);frame(500);page_pop();frame(400);
        t_down=true;music_page();assert(nav_contact_blocked);
        touch_at(80,H-50,true);touch_at(80,H-50,false);
        assert(!nav_contact_blocked && g_state==G_IDLE && !np_open);
        touch_at(80,H-50,true);touch_at(80,H-50,false);
        assert(np_open);np_hide(0);frame(500);page_pop();frame(400);
        test_status=0;mini_update();frame(200);
    }
    char label_buf[32];assert(!strcmp(qp_tile_label(7,label_buf,sizeof label_buf),"Library"));
    action(ACT_CLASSIC);frame(500);shot("rockbox-confirm.ppm");assert(npages==2 && !want_classic);
    lv_obj_t *confirm_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(row_named(confirm_body,"Stay in GabagoolOS"),LV_EVENT_CLICKED,NULL);
    frame(500);assert(npages==1 && !want_classic);
    action(ACT_CLASSIC);frame(500);
    confirm_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(row_named(confirm_body,"Open Classic Rockbox"),LV_EVENT_CLICKED,NULL);
    assert(want_classic);want_classic=false;page_pop();frame(500);
    action(ACT_BLUETOOTH);frame(500);shot("bluetooth-page.ppm");
    assert(sl_bt_page_obj==pages[npages-1] && sl_bt_count==3 && !sl_bt_state.connected);
    /* the switch row, the scan row and the device rows, found by their words */
    #define BT_ROW(s) lv_obj_get_parent(find_label(sl_bt_body,(s)))
    lv_obj_t *bt_switch=sl_bt_switch;assert(bt_switch && lv_obj_check_type(bt_switch,&lv_switch_class));
    lv_obj_remove_state(bt_switch,LV_STATE_CHECKED);
    lv_obj_send_event(bt_switch,LV_EVENT_VALUE_CHANGED,NULL);frame(100);frame(100);
    assert(!sl_bt_demo_on && !sl_bt_state.enabled);
    bt_switch=sl_bt_switch;
    lv_obj_add_state(bt_switch,LV_STATE_CHECKED);
    lv_obj_send_event(bt_switch,LV_EVENT_VALUE_CHANGED,NULL);frame(100);frame(100);
    assert(sl_bt_demo_on && sl_bt_state.enabled);
    lv_obj_send_event(BT_ROW("Scan for devices"),LV_EVENT_CLICKED,NULL);frame(100);
    assert(sl_bt_scanning);
    frame(15100);frame(100);assert(!sl_bt_scanning && sl_bt_count==3);
    lv_obj_send_event(BT_ROW(sl_bt_found[0].name),LV_EVENT_CLICKED,NULL);frame(100);frame(100);
    assert(sl_bt_state.connected && sl_bt_state.routed);
    shot("bluetooth-connected.ppm");
    lv_obj_t *bt_row=lv_obj_get_parent(find_label(sl_bt_body,"Connected · tap to disconnect"));assert(bt_row);
    lv_obj_send_event(lv_obj_get_child(bt_row,lv_obj_get_child_count(bt_row)-1),LV_EVENT_CLICKED,NULL);frame(400);
    assert(npages==3 && find_label(pages[2],"Rename"));   /* its own page: Rename, Disconnect, Forget */
    lv_obj_send_event(lv_obj_get_parent(find_label(pages[2],"Forget device")),LV_EVENT_CLICKED,NULL);
    assert(npages==4 && !strcmp(sl_bt_forget_mac,sl_bt_demo_devices[0].mac));
    page_pop();frame(400);page_pop();frame(400);
    lv_obj_send_event(BT_ROW("Disconnect"),LV_EVENT_CLICKED,NULL);frame(100);frame(100);
    assert(!sl_bt_state.connected);
    {   /* the page follows the stack by itself: headphones that connect
         * behind its back show up within a few seconds */
        sl_bt_demo_index=1;frame(4100);frame(100);
        assert(sl_bt_state.connected && !strcmp(sl_bt_state.name,"Pocket speakers"));
        /* before the first answer the switch is disabled, and a tap on it
         * never leaves it showing something that is not true */
        sl_bt_known=false;sl_bt_rebuild();
        assert(lv_obj_has_state(sl_bt_switch,LV_STATE_DISABLED));
        lv_obj_remove_state(sl_bt_switch,LV_STATE_CHECKED);
        lv_obj_send_event(sl_bt_switch,LV_EVENT_VALUE_CHANGED,NULL);
        assert(lv_obj_has_state(sl_bt_switch,LV_STATE_CHECKED) && sl_bt_demo_on);
        frame(4100);frame(100);frame(100);assert(sl_bt_known && !lv_obj_has_state(sl_bt_switch,LV_STATE_DISABLED));
        sl_bt_demo_index=-1;frame(4100);frame(100);assert(!sl_bt_state.connected);
    }
    {   /* headphones the stack connects by itself are routed on the worker,
         * after the job already running, once however often the tick asks;
         * a note says where the sound went */
        sl_bt_begin(SL_BT_JOB_REFRESH,"");sl_bt_demo_index=0;sl_bt_auto_request();sl_bt_auto_request();
        assert(sl_bt_busy && sl_bt_auto_wanted && sl_bt_job==SL_BT_JOB_REFRESH);
        frame(100);frame(100);frame(100);frame(100);assert(!sl_bt_auto_wanted && !sl_bt_busy && sl_bt_state.routed);
        assert(!strcmp(nh.list[0].title,"Playing on Studio headphones"));
        /* already there: asked again, it says nothing */
        int notes_before=nh.n;sl_bt_auto_request();frame(100);frame(100);assert(nh.n==notes_before);
        /* linked but the sound still on the player: the row offers to play
         * there, and a tap routes instead of disconnecting */
        sl_bt_state.routed=false;sl_bt_rebuild();
        bool offered=false;
        {   lv_obj_t *l=find_label(sl_bt_body,"Connected · tap to play here");
            if(l){offered=true;lv_obj_send_event(lv_obj_get_parent(l),LV_EVENT_CLICKED,NULL);
                assert(sl_bt_job==SL_BT_JOB_CONNECT);} }
        assert(offered);frame(100);frame(100);assert(sl_bt_state.routed && sl_bt_demo_index==0);
        sl_bt_demo_index=-1;frame(4100);frame(100);assert(!sl_bt_state.connected);
    }
    page_pop();frame(400);assert(!sl_bt_page_obj);
    {   /* Bluetooth's output delay (Joe's R1: 1.7 s): what is drawn follows
         * what is heard. The progress bar and the time, the lyrics, and
         * the spectrum's window all trail Rockbox's position by it. */
        unsigned long el0=test_track.elapsed,len0=test_track.length;
        test_track.length=240000;test_track.elapsed=10000;
        sl_test_out_latency_ms=0;assert(visible_elapsed(&test_track)==10000);
        sl_test_out_latency_ms=640;assert(visible_elapsed(&test_track)==9360);
        test_track.elapsed=300;assert(visible_elapsed(&test_track)==0);   /* never before the start */
        test_track.elapsed=10000;
        char lrc[]="[00:09.00]first\n[00:09.80]second\n[00:10.40]third\n";
        static struct sl_lrc l;memset(&l,0,sizeof l);sl_lrc_parse(lrc,&l);
        sl_test_out_latency_ms=0;int heard_now=sl_lrc_find(&l,(uint32_t)visible_elapsed(&test_track)+150);
        sl_test_out_latency_ms=640;int heard_bt=sl_lrc_find(&l,(uint32_t)visible_elapsed(&test_track)+150);
        assert(heard_now==1 && heard_bt==0);   /* "second" at the mixer, "first" in the speaker */
        int st0=test_status;test_status=AUDIO_STATUS_PLAY;
        np_show();frame(500);np_update(true);frame(100);assert(np_open);
        assert(!strcmp(lv_label_get_text(np_t0),"0:09"));
        sl_test_out_latency_ms=0;np_update(true);frame(100);
        assert(!strcmp(lv_label_get_text(np_t0),"0:10"));
        np_hide(0);frame(400);test_status=st0;
        /* the spectrum: the window ends latency frames earlier in the ring */
        enum channel_status ch0=test_chan;test_chan=CHANNEL_PLAYING;
        static int16_t pcm[1024*2];
        float db[SP_BARS];
        sp_hook(pcm,sizeof pcm);sl_test_out_latency_ms=0;assert(sp_analyze(db));uint32_t e0=sp_end;
        sp_hook(pcm,sizeof pcm);sp_chunk_tick=current_tick;   /* same moment, delayed */
        uint32_t written=sp_written;(void)written;
        sl_test_out_latency_ms=500;assert(sp_analyze(db));uint32_t e1=sp_end;
        uint32_t rate=mixer_get_frequency()<8000?44100:mixer_get_frequency();
        printf("spectrum: window end %u frames back for 500 ms at %u Hz\n",(unsigned)(e0+1024-e1),(unsigned)rate);
        assert(e0+1024-e1==rate/2 || e0+1024-e1==rate/2+1);
        test_chan=ch0;sl_test_out_latency_ms=0;test_track.elapsed=el0;test_track.length=len0;
        printf("PASS: over Bluetooth the progress bar, lyrics and spectrum follow the heard position.\n");
    }
    bt13_tests();
    lv_obj_t *home_body=lv_obj_get_child(pages[0],0);
    lv_obj_scroll_to_y(home_body,450,LV_ANIM_OFF);frame(100);shot("home-apps.ppm");
    assert(lv_obj_get_scroll_y(home_body)>0);
    lv_obj_scroll_to_y(home_body,0,LV_ANIM_OFF);frame(100);
    sl_files_page("/",0);frame(500);shot("files.ppm");assert(objects(pages[npages-1])<35);page_pop();frame(500);
    sl_files_page("/Lots",0);frame(500);assert(objects(pages[npages-1])<500);page_pop();frame(500);
    sl_files_page("/Lots",72);frame(500);assert(objects(pages[npages-1])<500);page_pop();frame(500);
    {   /* batch 7: Files on the sandboxed card (/Card, in memory) */
        /* natural order: digit runs as numbers (#164) */
        static const char *const want[] = { "Disc 1", "disc 2", "Disc 9", "Disc 10", "Track 01", "Track 1b", "Track 2", "Track 10", "track 10a", "Track 100" };
        for (unsigned i = 0; i + 1 < sizeof want / sizeof want[0]; i++) assert(sl_natcmp(want[i], want[i + 1]) < 0 && sl_natcmp(want[i + 1], want[i]) > 0);
        assert(sl_natcmp("a", "a") == 0 && sl_natcmp("Track 1", "Track 01") < 0 && sl_natcmp("x9", "x10") < 0);
        /* every extension Rockbox's metadata table plays is shown (#100): read from the source */
        FILE *mf = fopen("sloppyos/lib/rbcodec/metadata/metadata.c", "r");
        assert(mf);
        char ln[512]; int exts = 0;
        while (fgets(ln, sizeof ln, mf)) {
            if (!strstr(ln, "AFMT_ENTRY(") || strstr(ln, "\"???\"")) continue;
            char *q = strrchr(ln, '"'); if (!q) continue; *q = 0; char *o = strrchr(ln, '"'); if (!o) continue;
            /* "mp3\0mp2\0": the literal's \0 separators */
            for (char *x = o + 1; *x; ) { char *z = strstr(x, "\\0"); if (z) *z = 0; if (*x) { char nm[24]; snprintf(nm, sizeof nm, "a.%s", x); if (!sl_audio_name(nm)) { fprintf(stderr, "not shown: %s\n", x); assert(0); } exts++; } if (!z) break; x = z + 2; }
        }
        fclose(mf);
        assert(exts > 60 && sl_audio_name("x.MOD") && sl_audio_name("y.wv") && sl_audio_name("z.dsf") && !sl_audio_name("notes.txt") && !sl_audio_name("mp3"));
        printf("PASS: Files shows every format Rockbox decodes (%d extensions, read from metadata.c).\n", exts);
        /* a value the screens can pick is one config.cfg can keep: a table that leaves it out loads the default (Your picture
         * as a wallpaper was plain again after every start) */
        {   FILE *sf=fopen("sloppyos/apps/settings_list.c","r");assert(sf);
            static char src[400000];size_t sn=fread(src,1,sizeof src-1,sf);src[sn]=0;fclose(sf);
            static const struct { const char *cfg; int top; } want[] = {
                { "sloppy home wallpaper", WALL_CUSTOM }, { "sloppy lock wallpaper", WALL_CUSTOM }, { "sloppy theme", 3 },
                { "sloppy home style", 2 }, { "sloppy now playing style", 3 }, { "sloppy language", SL_LANG_LAST },
                { "sloppy text size", SL_TEXT_STEPS - 1 }, { "sloppy screen sleep", 2 } };
            for(unsigned i=0;i<sizeof want/sizeof want[0];i++){
                char key[64];snprintf(key,sizeof key,"\"%s\"",want[i].cfg);char *at=strstr(src,key);assert(at);
                char *end=strstr(at,"),");assert(end);char *v=strstr(at,"NULL, NULL, NULL,");assert(v&&v<end);
                int cnt=(int)strtol(v+17,&v,10),top=-1;
                for(int k=0;k<cnt;k++){while(*v==','||isspace((unsigned char)*v))v++;int x=(int)strtol(v,&v,10);if(x>top)top=x;}
                if(top<want[i].top){fprintf(stderr,"%s keeps up to %d, the screen picks %d\n",want[i].cfg,top,want[i].top);assert(0);}
            }
            printf("PASS: config.cfg keeps every wallpaper (Your picture too), theme, home, Now Playing style, language, text size and screen sleep a screen can pick.\n");
        }
        /* a folder in natural order, folders first; a song plays the folder from it, in that order */
        au_card();
        sl_files_page(AU_KITE, 0); frame(500);
        struct fm_view *v = fm_find(AU_KITE);
        assert(v && v->n == 10 && v->folders == 1 && !strcmp(fm_name(v, 0), "Scans"));
        static const char *const kite_order[] = { "Scans", "1 Morning Wire.flac", "2 Paper Kite.flac", "3 Harbor Lights.flac", "4 Glass Avenue.flac",
            "9 Weekend.flac", "10 Undertow.flac", "11 Last Light.flac", "cover.jpg", "notes.txt" };
        for (int k = 0; k < 10; k++) assert(!strcmp(fm_name(v, k), kite_order[k]));
        assert(v->e[8].kind == FK_PIC && v->e[9].kind == FK_TEXT && v->e[6].kind == FK_AUDIO);
        lv_obj_t *r6 = NULL; for (int s2 = 0; s2 < FM_SLOTS; s2++) if (v->idx[s2] == 6) r6 = v->rows[s2];
        assert(r6); lv_obj_send_event(r6, LV_EVENT_CLICKED, NULL); frame(400);
        assert(queued == 7 && started == 5 && !strcmp(card_paths[(-2 - queue_tracks[0]) % 256], AU_KITE "/1 Morning Wire.flac"));
        if (np_open) { np_hide(0); frame(400); }
        /* a huge folder: one page, rows only on screen; scrolled to the end, the last is there */
        sl_files_page("/Huge", 0); frame(500);
        struct fm_view *hv = fm_find("/Huge");
        assert(hv && hv->n == 2000 && hv->folders == 200 && !strcmp(fm_name(hv, 0), "Disc 1") && !strcmp(fm_name(hv, 9), "Disc 10")
               && !strcmp(fm_name(hv, 200), "Song 1.flac") && !strcmp(fm_name(hv, 1999), "Song 1800.flac"));
        assert(objects(pages[npages - 1]) < 160);
        lv_obj_scroll_to_y(hv->body, lv_obj_get_scroll_bottom(hv->body) + lv_obj_get_scroll_y(hv->body), LV_ANIM_OFF); frame(100);
        { bool last = false; for (int s2 = 0; s2 < FM_SLOTS; s2++) if (hv->idx[s2] == 1999 && hv->rows[s2]) last = true; assert(last); }
        assert(objects(pages[npages - 1]) < 160);
        page_pop(); frame(400);
        /* a cue sheet as its tracks: a track plays the file from its index (#163) */
        sl_files_page(AU_LIVE, 0); frame(400);
        struct fm_view *lv2 = fm_find(AU_LIVE);
        assert(lv2 && lv2->n == 3 && lv2->e[1].kind == FK_CUE);
        fm_open_entry(lv2, 1); frame(400);
        assert(find_label(pages[npages - 1], "Harbor Lights") && find_label(pages[npages - 1], "Kite with Mira Sol \xc2\xb7 5:03"));
        lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages - 1], "Harbor Lights")), LV_EVENT_CLICKED, NULL); frame(300);
        assert(queued == 1 && started == 0 && last_start_elapsed == 303000 && !strcmp(card_paths[(-2 - queue_tracks[0]) % 256], AU_LIVE "/Live at the Harbor.flac"));
        if (np_open) { np_hide(0); frame(400); }
        page_pop(); frame(400);
        /* converted since: the sheet's .wav is a .flac now, found by its name */
        const char *cue2 = "FILE \"Live at the Harbor.wav\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"Intro\"\n    INDEX 01 00:00:00\n";
        fake_put(AU_LIVE "/Converted.cue", cue2, strlen(cue2));
        assert(cue_parse(AU_LIVE "/Converted.cue") && !cue.missing && !strcmp(cue.files[0], AU_LIVE "/Live at the Harbor.flac"));
        const char *cue3 = "FILE \"Gone.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n";
        fake_put(AU_LIVE "/Gone.cue", cue3, strlen(cue3));
        assert(cue_parse(AU_LIVE "/Gone.cue") && cue.missing);
        fm_cue_page(AU_LIVE "/Gone.cue"); frame(300);   /* says so, plays nothing */
        assert(find_label(pages[npages - 1], "Track 1"));
        page_pop(); frame(300);
        sim_remove(AU_LIVE "/Converted.cue"); sim_remove(AU_LIVE "/Gone.cue");
        /* the text and picture viewers */
        fm_open_entry(v, 9); frame(300);
        assert(find_label(pages[npages - 1], "notes.txt") || find_label(pages[npages - 1], "notes.txt"));
        page_pop(); frame(300);
        fm_open_entry(v, 8); for (int i = 0; i < 8; i++) frame(30);
        assert(pic.px && pic.info && strstr(lv_label_get_text(pic.info), "1600"));
        page_pop(); frame(300); assert(!pic.px && pic.fd < 0);
        { unsigned char big = 0xff; fake_put(AU_KITE "/huge.png", &big, 1); }
        fm_reload(v); int hk = -1; for (int k = 0; k < v->n; k++) if (!strcmp(fm_name(v, k), "huge.png")) hk = k;
        fm_open_entry(v, hk); for (int i = 0; i < 6; i++) frame(30);
        assert(find_label(pages[npages - 1], "This picture is too big to open here"));
        page_pop(); frame(300); sim_remove(AU_KITE "/huge.png"); fm_reload(v);
        {   /* RD #5: the side keys step through a folder's pictures in place, and turn a text's pages */
            unsigned char b5[1800]; memset(b5, 0x5a, sizeof b5);
            fake_put(AU_KITE "/back.jpg", b5, sizeof b5); fake_put(AU_KITE "/zoo.jpg", b5, sizeof b5); fm_reload(v);
            int ck = -1; for (int k = 0; k < v->n; k++) if (!strcmp(fm_name(v, k), "cover.jpg")) ck = k;
            fm_open_entry(v, ck); for (int i = 0; i < 8; i++) frame(30);
            lv_obj_t *pg = pages[npages - 1]; int n0 = npages;
            assert(pic.page == pg && pic.view == v && pic.px);
            #define PIC_WAIT() for (int i = 0; i < 20 && (pic.fd >= 0 || pic.decoding); i++) frame(30)
            assert(fm_key(BUTTON_DOWN, false, false)); PIC_WAIT();   /* past notes.txt to zoo.jpg */
            assert(fm_key(BUTTON_DOWN, true, false));
            assert(npages == n0 && pages[npages - 1] == pg && strstr(pic.path, "/zoo.jpg") && pic.px && find_label(pg, "zoo.jpg"));
            shot("files-picture-next.ppm");
            fm_key(BUTTON_DOWN, false, false); PIC_WAIT(); assert(strstr(pic.path, "/zoo.jpg"));   /* the last: it stays */
            fm_key(BUTTON_UP, false, false); PIC_WAIT(); assert(strstr(pic.path, "/cover.jpg"));
            fm_key(BUTTON_UP, false, false); PIC_WAIT(); assert(strstr(pic.path, "/back.jpg") && find_label(pg, "back.jpg"));
            fm_key(BUTTON_UP, false, false); PIC_WAIT(); assert(strstr(pic.path, "/back.jpg"));
            assert(fm_key(BUTTON_RIGHT, false, false)); PIC_WAIT(); assert(strstr(pic.path, "/cover.jpg") && pic.px);
            assert(!fm_key(BUTTON_LEFT, false, false));   /* Play stays the music's */
            fm_key(BUTTON_DOWN, false, false); page_pop(); frame(400);   /* gone while one opens */
            for (int i = 0; i < 20; i++) frame(30);
            assert(!pic.page && !pic.px && pic.fd < 0 && !fm_key(BUTTON_DOWN, false, false));   /* the folder's list: the volume */
            #undef PIC_WAIT
            assert(v->e[ck + 1].kind == FK_TEXT); fm_open_entry(v, ck + 1); frame(300);   /* notes.txt */
            assert(fm_text_pg == pages[npages - 1] && fm_key(BUTTON_DOWN, false, false) && fm_key(BUTTON_UP, true, false));
            assert(!fm_key(BUTTON_RIGHT, false, false));   /* Next stays the music's in a text */
            page_pop(); frame(400); assert(!fm_text_pg);
            sim_remove(AU_KITE "/back.jpg"); sim_remove(AU_KITE "/zoo.jpg"); fm_reload(v);
            printf("PASS: the side keys step through a folder's pictures and turn a text's pages (RD #5)\n");
        }
        /* rename: the extension kept; a taken name refused; nothing replaced */
        fm_name_page(v, 1); frame(300);
        assert(!strcmp(lv_textarea_get_text(fm_nm_ta), "1 Morning Wire") && find_label(pages[npages - 1], "Stays a flac file"));
        lv_textarea_set_text(fm_nm_ta, "2 Paper Kite");   /* taken */
        fm_nm_go_cb(NULL); frame(100);
        assert(fm_nm_ta && fake_find(AU_KITE "/1 Morning Wire.flac") >= 0);
        lv_textarea_set_text(fm_nm_ta, "a/b");
        fm_nm_go_cb(NULL); frame(100);
        assert(fm_nm_ta && fake_find(AU_KITE "/1 Morning Wire.flac") >= 0);
        lv_textarea_set_text(fm_nm_ta, "01 Morning Wire");
        fm_nm_go_cb(NULL); frame(400);
        assert(fake_find(AU_KITE "/01 Morning Wire.flac") >= 0 && fake_find(AU_KITE "/1 Morning Wire.flac") < 0 && !strcmp(fm_name(v, 1), "01 Morning Wire.flac"));
        /* a new folder; its name taken next time gets a number */
        fm_name_page(v, -1); frame(300);
        assert(!strcmp(lv_textarea_get_text(fm_nm_ta), "New folder"));
        fm_nm_go_cb(NULL); frame(400);
        assert(sim_dir_exists(AU_KITE "/New folder") && v->folders == 2);
        fm_name_page(v, -1); frame(300);
        assert(!strcmp(lv_textarea_get_text(fm_nm_ta), "New folder 2"));
        page_pop(); frame(300);
        /* picking files (#37): two songs and a folder to the queue */
        int ks[3] = { 2, 3, 0 };   /* 01 Morning Wire and 2 Paper Kite (two folders now) */
        test_status = AUDIO_STATUS_PLAY; started = 0; queued = 1; queue_tracks[0] = 1;
        sel_start(SEL_FILES, v, v->page, 2); frame(100);
        assert(sel_kind_on(SEL_FILES) && sel_count == 1);
        for (int s2 = 0; s2 < FM_SLOTS; s2++) if (v->idx[s2] == 3) { lv_obj_send_event(v->rows[s2], LV_EVENT_CLICKED, NULL); }
        frame(100); assert(sel_count == 2 && sel_has(2) && sel_has(3));
        sel_all_cb(NULL); frame(100); assert(sel_count == v->n);
        sel_all_cb(NULL); frame(100); assert(!sel_on());     /* again: none, picking ends */
        (void)ks;
        sel_start(SEL_FILES, v, v->page, 2); frame(100);
        for (int s2 = 0; s2 < FM_SLOTS; s2++) if (v->idx[s2] == 3) { lv_obj_send_event(v->rows[s2], LV_EVENT_CLICKED, NULL); }
        frame(100);
        sel_queue_cb(NULL); frame(200);
        assert(!sel_on() && queued == 3 && !strcmp(fm_name(v, 3), "2 Paper Kite.flac") && !strcmp(card_paths[(-2 - queue_tracks[2]) % 256], AU_KITE "/2 Paper Kite.flac"));
        /* a folder into a playlist: its songs, in order, by path (not in the library) */
        {
            char pd[MAX_PATH]; strlcpy(pd, sl_playlist_dir(), sizeof pd); if (pd[strlen(pd) - 1] == '/') pd[strlen(pd) - 1] = 0;
            char pp[MAX_PATH]; snprintf(pp, sizeof pp, "%s/Kite folder.m3u8", pd);
            sim_remove(pp);
            struct fm_view *root = NULL; sl_files_page(FAKE_CARD "/Music", 0); frame(400); root = fm_find(FAKE_CARD "/Music");
            int kk = -1; for (int k = 0; k < root->n; k++) if (!strcmp(fm_name(root, k), "Kite - Paper Kite (1975)")) kk = k;
            assert(kk >= 0);
            fm_do(root, FA_PLAYLIST, &kk, 1); frame(300);
            assert(pl_visible() && pl_npaths == 7 && pl_track < 0 && find_label(pl_sheet, "Add 7 songs"));
            lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet, "New playlist")), LV_EVENT_CLICKED, NULL); frame(100);
            lv_textarea_set_text(pl_input, "Kite folder");
            lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet, "Create")), LV_EVENT_CLICKED, NULL); frame(300);
            assert(!pl_open_now && fake_find(pp) >= 0);
            assert(fake_has(pp, AU_KITE "/01 Morning Wire.flac\n" AU_KITE "/2 Paper Kite.flac\n"));
            /* the same name again: Replace writes these over it, whole */
            fm_do(root, FA_PLAYLIST, &kk, 1); frame(300);
            lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet, "New playlist")), LV_EVENT_CLICKED, NULL); frame(100);
            lv_textarea_set_text(pl_input, "Kite folder");
            lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet, "Create")), LV_EVENT_CLICKED, NULL); frame(100);
            long before = fake_size(pp);
            lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet, "Add to it")), LV_EVENT_CLICKED, NULL); frame(300);
            assert(!pl_open_now && fake_size(pp) > before);
            fm_do(root, FA_PLAYLIST, &kk, 1); frame(300);
            lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet, "New playlist")), LV_EVENT_CLICKED, NULL); frame(100);
            lv_textarea_set_text(pl_input, "Kite folder");
            lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet, "Create")), LV_EVENT_CLICKED, NULL); frame(100);
            lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet, "Replace")), LV_EVENT_CLICKED, NULL); frame(300);
            assert(!pl_open_now && fake_size(pp) == before);
            sim_remove(pp);
            page_pop(); frame(400);
        }
        /* move into a folder; a clash asks, keeps both or skips, never replaces */
        fake_put(AU_KITE "/New folder/notes.txt", "older notes", 11);
        int nk = 9; for (int k = 0; k < v->n; k++) if (!strcmp(fm_name(v, k), "notes.txt")) nk = k;
        fm_op_start(FM_MOVE, v, &nk, 1); frame(400);
        struct fm_view *dv = NULL; for (int i = 0; i < MAX_PAGES; i++) if (fm_views[i] && fm_views[i]->dest) dv = fm_views[i];
        assert(dv && dv->n == 2);    /* folders only: Scans and New folder */
        int nf = -1; for (int k = 0; k < dv->n; k++) if (!strcmp(fm_name(dv, k), "New folder")) nf = k;
        fm_open_entry(dv, nf); frame(400);
        struct fm_view *dv2 = NULL; for (int i = 0; i < MAX_PAGES; i++) if (fm_views[i] && fm_views[i]->dest && !strcmp(fm_views[i]->path, AU_KITE "/New folder")) dv2 = fm_views[i];
        assert(dv2);
        { lv_event_t ev; memset(&ev, 0, sizeof ev); ev.user_data = dv2; fm_here_cb(&ev); } frame(400);
        assert(find_label(pages[npages - 1], "Keep both") && find_label(pages[npages - 1], "Skip them"));
        { lv_event_t ev; memset(&ev, 0, sizeof ev); ev.user_data = (void *)FM_KEEP_BOTH; fm_conflict_cb(&ev); } frame(600);
        assert(fake_equals(AU_KITE "/New folder/notes.txt", "older notes") && fake_find(AU_KITE "/New folder/notes 2.txt") >= 0 && fake_find(AU_KITE "/notes.txt") < 0);
        assert(pages[npages - 1] == v->page && lbf_log_has("files: moved"));
        /* a folder into itself is refused */
        int sk = -1; for (int k = 0; k < v->n; k++) if (!strcmp(fm_name(v, k), "New folder")) sk = k;
        fm_op_start(FM_MOVE, v, &sk, 1); frame(400);
        { struct fm_view *d3 = NULL; for (int i = 0; i < MAX_PAGES; i++) if (fm_views[i] && fm_views[i]->dest) d3 = fm_views[i];
          assert(d3); for (int k = 0; k < d3->n; k++) assert(strcmp(fm_name(d3, k), "New folder")); }   /* not offered as its own destination */
        fm_dest_cancel_cb(NULL); frame(600); assert(pages[npages - 1] == v->page && !fm_op.n);
        /* copy a folder: made whole through .part files, the original kept */
        int ck = -1; for (int k = 0; k < v->n; k++) if (!strcmp(fm_name(v, k), "New folder")) ck = k;
        fm_op_start(FM_COPY, v, &ck, 1); frame(400);
        { struct fm_view *d4 = NULL; for (int i = 0; i < MAX_PAGES; i++) if (fm_views[i] && fm_views[i]->dest) d4 = fm_views[i];
          lv_event_t ev; memset(&ev, 0, sizeof ev); ev.user_data = d4; fm_here_cb(&ev); } frame(300);
        /* its own folder holds it: keep both, as "New folder 2" */
        assert(find_label(pages[npages - 1], "Keep both"));
        { lv_event_t ev; memset(&ev, 0, sizeof ev); ev.user_data = (void *)FM_KEEP_BOTH; fm_conflict_cb(&ev); }
        for (int i = 0; i < 40 && cp.running; i++) frame(16);
        frame(600);
        assert(!cp.running && sim_dir_exists(AU_KITE "/New folder 2") && fake_equals(AU_KITE "/New folder 2/notes.txt", "older notes")
               && fake_find(AU_KITE "/New folder 2/notes 2.txt") >= 0 && fake_equals(AU_KITE "/New folder/notes.txt", "older notes"));
        assert(!fake_any(".part"));
        assert(lbf_log_has("files: copy done"));
        assert(fake_opendirs == 0);    /* every folder opened was closed */
        {   /* delete (#91): off by default; on, a held entry and the picking bar offer it, and it always asks */
            assert(!global_settings.sloppy_files_delete);
            fake_put(AU_KITE "/old notes.txt", "x", 1); fm_reload(v);
            int tk = -1; for (int k = 0; k < v->n; k++) if (!strcmp(fm_name(v, k), "old notes.txt")) tk = k;
            assert(tk >= 0);
            fm_acts(v, tk); frame(300); assert(!find_label(acts, "Delete")); acts_close(); frame(300);
            sel_start(SEL_FILES, v, v->page, tk); frame(100); fm_sel_more(); frame(300);
            assert(!find_label(acts, "Delete")); acts_close(); frame(300); sel_end(); frame(100);
            library_options_page(); frame(400);
            lv_obj_t *sw = find_label(pages[npages - 1], "Allow deleting files"); assert(sw);
            lv_obj_send_event(lv_obj_get_parent(sw), LV_EVENT_CLICKED, NULL); frame(300);
            assert(global_settings.sloppy_files_delete == 1);
            page_pop(); frame(400);
            /* never the card, a climb out of it, or .rockbox in any case */
            assert(fm_del_refused("/") && fm_del_refused("") && fm_del_refused("Music") && fm_del_refused("/.rockbox")
                   && fm_del_refused("/.ROCKBOX/config.cfg") && fm_del_refused("/Music/../.rockbox") && fm_del_refused("/Music/..")
                   && fm_del_refused("//.rockbox") && !fm_del_refused("/Music") && !fm_del_refused("/.rockboxed"));
            /* the sheet's last row, in red */
            fm_acts(v, tk); frame(300);
            lv_obj_t *dl = find_label(acts, "Delete"); assert(dl && lv_obj_get_parent(dl) == lv_obj_get_child(acts, -1));
            assert(lv_color_to_u32(lv_obj_get_style_text_color(dl, 0)) != lv_color_to_u32(lv_obj_get_style_text_color(find_label(acts, "Select"), 0)));
            lv_obj_send_event(lv_obj_get_parent(dl), LV_EVENT_CLICKED, NULL); frame(500);
            /* one: its name in the question; Cancel keeps it */
            assert(find_label(pages[npages - 1], "Delete \"old notes.txt\"?"));
            assert(find_label(pages[npages - 1], "This removes it from the card and can't be undone.") && find_label(pages[npages - 1], "Cancel"));
            shot("files-delete-one.ppm");
            lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages - 1], "Cancel")), LV_EVENT_CLICKED, NULL); frame(400);
            assert(pages[npages - 1] == v->page && fake_find(AU_KITE "/old notes.txt") >= 0 && !fdl.n);
            /* and Delete takes it */
            fm_do(v, FA_DELETE, &tk, 1); frame(400);
            lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages - 1], "Delete")), LV_EVENT_CLICKED, NULL); frame(400);
            assert(fake_find(AU_KITE "/old notes.txt") < 0 && pages[npages - 1] == v->page && lbf_log_has("files: delete done"));
            for (int k = 0; k < v->n; k++) assert(strcmp(fm_name(v, k), "old notes.txt"));
            /* several, picked: named, the playing one among them: it stops first, and leaves the queue */
            fake_put(AU_KITE "/New folder 2/.hidden", "x", 1);   /* hidden files go too, or the folder could not */
            char doomed[2][MAX_PATH] = { AU_KITE "/New folder 2", AU_KITE "/cover.jpg" };
            int pk[2] = { -1, -1 };
            for (int k = 0; k < v->n; k++) for (int j = 0; j < 2; j++) if (!strcmp(fm_name(v, k), fm_base(doomed[j]))) pk[j] = k;
            assert(pk[0] >= 0 && pk[1] >= 0);
            test_status = AUDIO_STATUS_PLAY; strlcpy(test_track.path, AU_KITE "/New folder 2/notes 2.txt", sizeof test_track.path);
            queued = 0; started = 0;
            playlist_insert_track(NULL, AU_KITE "/New folder 2/notes 2.txt", PLAYLIST_INSERT_LAST, false, false);
            playlist_insert_track(NULL, AU_KITE "/2 Paper Kite.flac", PLAYLIST_INSERT_LAST, false, false);
            playlist_insert_track(NULL, AU_KITE "/cover.jpg", PLAYLIST_INSERT_LAST, false, false);
            assert(queued == 3);
            sel_start(SEL_FILES, v, v->page, pk[0]); frame(100);
            for (int s2 = 0; s2 < FM_SLOTS; s2++) if (v->idx[s2] == pk[1]) lv_obj_send_event(v->rows[s2], LV_EVENT_CLICKED, NULL);
            frame(100); assert(sel_count == 2);
            fm_sel_more(); frame(300);
            lv_obj_send_event(lv_obj_get_parent(find_label(acts, "Delete")), LV_EVENT_CLICKED, NULL); frame(500);
            assert(find_label(pages[npages - 1], "Delete 2 items?") && find_label(pages[npages - 1], "New folder 2") && find_label(pages[npages - 1], "cover.jpg"));
            assert(find_label(pages[npages - 1], "This removes them from the card and can't be undone.") && find_label(pages[npages - 1], "Playing now"));
            shot("files-delete-many.ppm");
            int stops0 = audio_stops;
            fd_go_cb(NULL);
            assert(audio_stops == stops0 + 1 && !test_status);
            assert(fdl.page && pages[npages - 1] == fdl.page && find_label(fdl.page, "Stop"));   /* a folder: the progress page */
            lv_timer_pause(fdl.timer); fd_run(0, 2); frame(400);
            { char want[48]; snprintf(want, sizeof want, "Deleting 3 of %d", fdl.items);
              assert(fdl.running && fdl.at == 2 && fdl.items == 5 && find_label(fdl.page, want)); }   /* three in the folder, it, and the cover */
            shot_now("files-delete-progress.ppm");
            lv_timer_resume(fdl.timer);
            for (int i = 0; i < 40 && fdl.running; i++) frame(16);
            frame(600);
            assert(!fdl.running && !sim_dir_exists(AU_KITE "/New folder 2") && fake_find(AU_KITE "/cover.jpg") < 0 && !fake_any("New folder 2/"));
            assert(pages[npages - 1] == v->page && fake_opendirs == 0);
            assert(queued == 1 && !strcmp(card_paths[(-2 - queue_tracks[0]) % 256], AU_KITE "/2 Paper Kite.flac"));   /* the queue let go of them */
            /* many in a folder, stopped partway: what went went, the rest stays */
            mkdir(AU_KITE "/Big");
            for (int i = 0; i < 40; i++) { char bp[200]; snprintf(bp, sizeof bp, AU_KITE "/Big/Song %d.flac", i); fake_put(bp, "x", 1); }
            fm_reload(v);
            int bk = -1; for (int k = 0; k < v->n; k++) if (!strcmp(fm_name(v, k), "Big")) bk = k;
            fm_do(v, FA_DELETE, &bk, 1); frame(300);
            fd_go_cb(NULL); assert(fdl.running && fdl.items == 41);
            fd_stop_cb(NULL); frame(400);   /* before a single tick: all of it stays */
            assert(!fdl.running && sim_dir_exists(AU_KITE "/Big") && fake_find(AU_KITE "/Big/Song 39.flac") >= 0 && lbf_log_has("files: delete stopped"));
            fm_do(v, FA_DELETE, &bk, 1); frame(300);
            fd_go_cb(NULL); for (int i = 0; i < 40 && fdl.running; i++) frame(16);
            assert(!sim_dir_exists(AU_KITE "/Big") && !fake_any("/Big/"));
            /* off again: no Delete anywhere */
            global_settings.sloppy_files_delete = 0;
            fm_acts(v, 0); frame(300); assert(!find_label(acts, "Delete")); acts_close(); frame(300);
        }
        while (npages > 1) { page_pop(); frame(300); }
        global_settings.sloppy_files_delete = 0;
        test_status = 0; queued = 0; started = -1;
        printf("PASS: Files: natural order, every format, cue sheets, viewers, rename, new folder, picking, playlists by path, move and copy that never replace, delete that asks.\n");
    }
    {   /* batch 7: 600 playlists: no cap anywhere, in search, picked and deleted together (#110) */
        char saved_dir[sizeof global_settings.playlist_catalog_dir];
        strlcpy(saved_dir, (char *)global_settings.playlist_catalog_dir, sizeof saved_dir);
        fake_card_reset(); mkdir(FAKE_CARD "/Playlists");
        strlcpy((char *)global_settings.playlist_catalog_dir, FAKE_CARD "/Playlists", sizeof global_settings.playlist_catalog_dir);
        char pp[200], body[300];
        int bl = snprintf(body, sizeof body, "#EXTM3U\n%s\n", TP(1));
        for (int i = 600; i >= 1; i--) { snprintf(pp, sizeof pp, FAKE_CARD "/Playlists/List %d.m3u8", i); fake_put(pp, body, (size_t)bl); }
        sl_saved_playlists_page(0); frame(400);
        assert(pls_all_n == 600 && !strcmp(pls_all[0], "List 1.m3u8") && !strcmp(pls_all[1], "List 2.m3u8") && !strcmp(pls_all[599], "List 600.m3u8"));
        assert(find_label(pages[npages - 1], "1 TO 72 OF 600") && find_label(pages[npages - 1], "List 72") && !find_label(pages[npages - 1], "List 73"));
        sl_saved_playlists_page(576); frame(400);
        assert(find_label(pages[npages - 1], "List 600") && find_label(pages[npages - 1], "577 TO 600 OF 600"));
        page_pop(); frame(400);
        /* the add sheet lists all 600, a few rows a tick */
        pl_open(9); frame(100);
        assert(pl_count == 600 && pl_rows_made == PL_FIRST);
        for (int i = 0; i < 120 && pl_rows_made < 600; i++) frame(16);
        assert(pl_rows_made == 600 && find_label(pl_sheet, "List 600"));
        pl_close(); frame(400);
        /* in search, by name */
        search_page(); frame(300); sq_text[0] = 0; sq_add("L"); sq_add("i"); sq_add("s"); sq_add("t"); sq_add(" "); sq_add("5"); sq_add("9"); sq_add("9"); frame(400);
        assert(find_label(sq_body, "PLAYLISTS · 1") && find_label(sq_body, "List 599"));
        lv_obj_send_event(lv_obj_get_parent(find_label(sq_body, "List 599")), LV_EVENT_CLICKED, NULL); frame(400);
        assert(find_label(pages[npages - 1], "List 599"));
        while (npages > 1) { page_pop(); frame(300); }
        /* pick two, delete them after a yes; select all takes every page's */
        sl_saved_playlists_page(0); frame(400);
        sel_start(SEL_PLAYLISTS, top_body(), pages[npages - 1], 0); frame(100);
        assert(sel_kind_on(SEL_PLAYLISTS) && sel_count == 1);
        lv_obj_t *l2 = find_label(pages[npages - 1], "List 2"); while (l2 && !lv_obj_has_flag(l2, LV_OBJ_FLAG_CLICKABLE)) l2 = lv_obj_get_parent(l2);
        lv_obj_send_event(l2, LV_EVENT_CLICKED, NULL); frame(100);
        assert(sel_count == 2 && npages >= 2);
        sel_delete_cb(NULL); frame(400);
        assert(find_label(pages[npages - 1], "Delete 2 playlists?") && fake_find(FAKE_CARD "/Playlists/List 1.m3u8") >= 0);
        lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages - 1], "Keep them")), LV_EVENT_CLICKED, NULL); frame(400);
        assert(sel_on() && fake_find(FAKE_CARD "/Playlists/List 2.m3u8") >= 0);
        sel_delete_cb(NULL); frame(400);
        lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages - 1], "Delete 2 playlists")), LV_EVENT_CLICKED, NULL); frame(600);
        assert(!sel_on() && fake_find(FAKE_CARD "/Playlists/List 1.m3u8") < 0 && fake_find(FAKE_CARD "/Playlists/List 2.m3u8") < 0 && pls_all_n == 598);
        sel_start(SEL_PLAYLISTS, top_body(), pages[npages - 1], -1); frame(100);
        sel_all_cb(NULL); frame(100); assert(sel_count == 598);
        sel_end(); frame(100);
        while (npages > 1) { page_pop(); frame(300); }
        fake_card_reset();
        strlcpy((char *)global_settings.playlist_catalog_dir, saved_dir, sizeof global_settings.playlist_catalog_dir);
        printf("PASS: 600 playlists: all listed and in the add sheet, found by search, picked and deleted after a yes.\n");
    }
    {   /* a Windows playlist of 20,000 C:\ paths: no more than PLS_MAX lines looked up on the screen's
         * thread (each was a card lookup, and the page froze for as long as the list was long) */
        static char win[20000 * 24]; size_t k = 0;
        for (int i = 0; i < 20000; i++) k += (size_t)sprintf(win + k, "C:\\M\\%05d.mp3\r\n", i);
        fake_put(FAKE_CARD "/Windows.m3u", win, k);
        int *l = NULL, miss = 0, n = pls_read(FAKE_CARD "/Windows.m3u", &l, &miss);
        assert(n == 0 && miss == PLS_MAX);
        free(l); remove(FAKE_CARD "/Windows.m3u");
    }
    {   /* PS14: playlist work timed (host CPU; the card's writes are the R1's cost): a new one of 500
         * songs, 500 more added, opened, renamed, deleted; each as the pages do it */
        char saved_dir[sizeof global_settings.playlist_catalog_dir];
        strlcpy(saved_dir, (char *)global_settings.playlist_catalog_dir, sizeof saved_dir);
        fake_card_reset(); mkdir(FAKE_CARD "/Playlists");
        strlcpy((char *)global_settings.playlist_catalog_dir, FAKE_CARD "/Playlists", sizeof global_settings.playlist_catalog_dir);
        static int five[500]; for (int i = 0; i < 500; i++) five[i] = (i * 7) % sl_lib.n_tracks;
        struct timespec a, b; double ms[5];
        const char *pp = FAKE_CARD "/Playlists/Timed.m3u8", *to = FAKE_CARD "/Playlists/Timed again.m3u8";
        free(pl_many); pl_many = malloc(sizeof five); memcpy(pl_many, five, sizeof five); pl_nmany = 500; pl_track = five[0]; pl_npaths = 0;
        clock_gettime(CLOCK_MONOTONIC, &a); assert(pl_write_new(pp)); clock_gettime(CLOCK_MONOTONIC, &b);
        ms[0] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        struct pl_entry en; memset(&en, 0, sizeof en); strlcpy(en.path, pp, sizeof en.path);
        clock_gettime(CLOCK_MONOTONIC, &a); assert(pl_append(&en)); clock_gettime(CLOCK_MONOTONIC, &b);
        ms[1] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        int *l = NULL, miss = 0;
        clock_gettime(CLOCK_MONOTONIC, &a); int n = pls_read(pp, &l, &miss); clock_gettime(CLOCK_MONOTONIC, &b);
        ms[2] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        assert(n == 1000 && !miss); free(l);
        clock_gettime(CLOCK_MONOTONIC, &a); assert(rename(pp, to) == 0); clock_gettime(CLOCK_MONOTONIC, &b);
        ms[3] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        clock_gettime(CLOCK_MONOTONIC, &a); assert(remove(to) == 0); clock_gettime(CLOCK_MONOTONIC, &b);
        ms[4] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        free(pl_many); pl_many = NULL; pl_nmany = 0; pl_track = -1;
        fake_card_reset();
        strlcpy((char *)global_settings.playlist_catalog_dir, saved_dir, sizeof global_settings.playlist_catalog_dir);
        printf("playlist work (host): new with 500 songs %.2f ms, 500 more added %.2f ms, 1,000 opened %.2f ms, renamed %.3f ms, deleted %.3f ms\n",
               ms[0], ms[1], ms[2], ms[3], ms[4]);
        printf("PASS: playlist work timed: made, grown to 1,000, opened whole, renamed and deleted.\n");
    }
    int previous_queue=queued;{struct fm_paths none={0};assert(!fm_play_paths(&none,0,false));}assert(queued==previous_queue);
    {   /* a saved playlist opens as its songs: Play, Shuffle, a tap plays from a song; Rename and Delete ask first */
        char pd[MAX_PATH],pmix[MAX_PATH],proad[MAX_PATH];strlcpy(pd,sl_playlist_dir(),sizeof pd);
        if(pd[strlen(pd)-1]=='/')pd[strlen(pd)-1]=0;snprintf(pmix,sizeof pmix,"%s/mix.m3u8",pd);snprintf(proad,sizeof proad,"%s/road trip.m3u8",pd);
        char pl[600];snprintf(pl,sizeof pl,"\xef\xbb\xbf#EXTM3U\n%s\n#EXTINF:1,x\n%s\n/Music/not-there.flac\n",TP(1),TP(2));
        int fd=open(pmix,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);
        assert(write(fd,pl,strlen(pl))==(ssize_t)strlen(pl));sim_close(fd);
        sl_saved_playlists_page(0);for(int i=0;i<5;i++)frame(50);
        assert(find_label(pages[npages-1],"mix") && find_label(pages[npages-1],"3 songs"));   /* counted, one a frame */
        lv_obj_t *mr=find_label(pages[npages-1],"mix");while(mr&&!lv_obj_has_flag(mr,LV_OBJ_FLAG_CLICKABLE))mr=lv_obj_get_parent(mr);
        assert(mr);lv_obj_send_event(mr,LV_EVENT_CLICKED,NULL);frame(500);
        assert(pages[npages-1]==pls_page_obj && pls_n==2 && pls_missing==1 && pls_tracks[0]==1 && pls_tracks[1]==2);
        assert(has_text(pages[npages-1],"2 SONGS") && has_text(pages[npages-1],"1 song here is no longer on the card.") && !has_text(pages[npages-1],"not in your library"));   /* gone from the card: marked missing, not "Play still finds it" */
        shot("playlist-open.ppm");
        int was=started,st0=test_status,q0=queued;started=-7;pls_play(false);assert(started==0);started=was;test_status=st0;queued=q0;
        if(np_open){np_hide(0);frame(500);}
        pls_rename_page(NULL);frame(400);lv_textarea_set_text(pls_name_ta,"road trip");pls_rename_go_cb(NULL);frame(500);
        assert(fake_find(proad)>=0 && fake_find(pmix)<0);
        assert(!pls_page_obj && pages[npages-1]==pls_list_obj);
        strlcpy(pls_path,proad,sizeof pls_path);strlcpy(pls_name,"road trip",sizeof pls_name);
        pls_delete_page(NULL);frame(400);assert(find_label(pages[npages-1],"Keep it"));shot("playlist-delete.ppm");
        pls_delete_go_cb(NULL);frame(500);assert(fake_find(proad)<0);
        page_pop();frame(500);
    }
    sl_saved_playlists_page(0);frame(500);shot("playlists.ppm");assert(objects(pages[npages-1])<25);
    lv_obj_t *playlist_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(lv_obj_get_child(playlist_body,1),LV_EVENT_CLICKED,NULL);
    assert(queued==previous_queue);page_pop();frame(500);
    sl_games_page();frame(500);shot("playroom.ppm");page_pop();frame(500);
    {   /* Metronome: 120 to start; beats keep time on the tick, the first of the bar louder;
         * three taps 400 ms apart set 150; 3/4 makes three dots; leaving stops the timer */
        metronome_page();frame(400);assert(find_label(pages[npages-1],"120") && find_label(pages[npages-1],"Allegro"));
        shot("metronome.ppm");
        lv_obj_send_event(mn_go,LV_EVENT_CLICKED,NULL);assert(mn_running && mn_played==1);
        for(int i=0;i<10;i++)frame(100);
        assert(mn_played==3);   /* at 0, 500 and 1000 ms */
        shot_now("metronome-playing.ppm");
        lv_obj_send_event(mn_go,LV_EVENT_CLICKED,NULL);assert(!mn_running);
        for(int i=0;i<3;i++){mn_tap_cb(NULL);frame(400);}
        assert(mn_bpm==150 && find_label(pages[npages-1],"150"));
        lv_obj_send_event(mn_sigs[1],LV_EVENT_CLICKED,NULL);frame(100);
        assert(mn_beats==3 && lv_obj_get_child_count(mn_dots_row)==3);
        mn_set_bpm(1000);assert(mn_bpm==250);mn_set_bpm(1);assert(mn_bpm==30);mn_set_bpm(120);mn_beats=4;
        page_pop();frame(500);assert(!mn_timer && !mn_page_obj);
    }
    {   /* Flip a Coin's faces are painted in slices while home sits idle (all
         * at once on open took 1.4 s on the R1), and come out the same */
        static lv_color32_t whole[2][COIN_TEX*COIN_TEX];
        assert(npages==1 && !coin_art_ready);
        coin_paint(0);memcpy(whole,coin_tex,sizeof whole);
        memset(coin_tex,0,sizeof coin_tex);coin_rows=0;coin_art_ready=false;
        lv_timer_t *warm=lv_timer_create(coin_warm,100,NULL);lv_timer_pause(warm);
        last_touch_ms=sl_ms()-5000;g_state=G_IDLE;
        int slices=0;uint32_t worst=0;
        for(int k=0;k<20;k++)frame(500);                         /* the page slides settle: idle */
        /* each slice's own work (the thread's CPU time): the Mac's clock also counts a loaded Mac's waits */
        while(!coin_art_ready&&slices<5000){struct timespec c0,c1;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&c0);coin_warm(warm);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&c1);uint32_t d=(uint32_t)((c1.tv_sec-c0.tv_sec)*1000+(c1.tv_nsec-c0.tv_nsec)/1000000);if(d>worst)worst=d;slices++;}
        assert(coin_art_ready && worst<=20 && !memcmp(whole,coin_tex,sizeof whole));
        coin_warm(warm);                                          /* done: the timer deletes itself */
        printf("coin faces: %d idle slices, worst %u ms\n",slices,worst);
    }
    coin_page();frame(500);shot("coin-ready.ppm");assert(coin_page_obj && coin_timer && coin_state==COIN_IDLE);
    /* swipe up through the real touch path: the coin rides the finger and winds back */
    touch_at(240,500,true);touch_at(240,480,true);touch_at(240,420,true);
    assert(coin_state==COIN_DRAG && coin_lift>20 && coin_theta<0);
    touch_at(240,330,true);touch_at(240,300,false);
    assert(coin_state==COIN_FLIGHT && coin_in_flight && coin_power>0.8f && coin_flips==0);
    frame(150);shot("coin-inflight.ppm");
    assert(coin_lift>200 && lv_obj_get_y(coin_canvas)<COIN_REST_Y-200-COIN_CH/2);
    assert(lv_obj_get_y(coin_canvas)>=SB_H);
    lv_obj_send_event(coin_flip_btn,LV_EVENT_CLICKED,NULL);assert(coin_state==COIN_FLIGHT);
    coin_started=coin_now()-coin_air_ms-COIN_REVEAL_MS-20;frame(16);
    assert(coin_landed && coin_revealed && coin_flips==1 && coin_burst_at);
    assert(!strcmp(lv_label_get_text(coin_caption),coin_heads?"Heads!":"Tails!"));
    frame(120);shot("coin-landing.ppm");
    coin_started-=COIN_SETTLE_MS;frame(16);
    assert(coin_state==COIN_IDLE && !coin_in_flight && coin_front_heads==coin_heads);
    assert(coin_theta==0 && coin_lift==0 && lv_obj_get_y(coin_canvas)==COIN_REST_Y-COIN_CH/2);
    frame(1200);assert(!coin_burst_at && !coin_pop_at);
    shot("coin-result.ppm");
    /* a short downward drag springs back without a flip */
    touch_at(240,500,true);touch_at(240,530,true);touch_at(240,540,false);
    assert(coin_state==COIN_SPRING && coin_flips==1);frame(500);assert(coin_state==COIN_IDLE);
    /* a tap on the coin and the Flip pill both throw it */
    touch_at(240,500,true);touch_at(240,500,false);assert(coin_state==COIN_FLIGHT);
    coin_started-=4000;frame(16);frame(16);assert(coin_state==COIN_IDLE && coin_flips==2);
    lv_obj_send_event(coin_flip_btn,LV_EVENT_CLICKED,NULL);assert(coin_state==COIN_FLIGHT);
    coin_started-=4000;frame(16);frame(16);assert(coin_flips==3);
    assert(lv_obj_get_child_count(coin_history)==3);
    { bool was=coin_front_heads;coin_front_heads=false;coin_set_title("Tails!",&ss_bold_56,COIN_AMBER);coin_render();
      shot("coin-dog.ppm");coin_front_heads=true;coin_set_title("Heads!",&ss_bold_56,COIN_PINK);coin_render();
      shot("coin-cat.ppm");coin_front_heads=was;coin_render(); }
    /* light theme: the white Tiny Decisions table; the default above is dark */
    global_settings.sloppy_theme=1;page_pop();frame(500);coin_page();frame(500);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(coin_page_obj,0))==0xffffffff);shot("coin-light.ppm");
    global_settings.sloppy_theme=0;page_pop();frame(500);coin_page();frame(500);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(coin_page_obj,0))==0xff000000);
    {   /* the tally and recent flips survive reopening the page and a reboot */
        assert(coin_flips==3 && lv_obj_get_child_count(coin_history)==3 && strlen(coin_hist)==3);
        uint32_t heads=coin_head_count;bool last=coin_heads;
        coin_loaded=false;coin_flips=coin_head_count=0;coin_hist[0]=0;coin_heads=!last;   /* as after a reboot */
        coin_load();
        assert(coin_flips==3 && coin_head_count==heads && strlen(coin_hist)==3 && coin_heads==last);
        assert((coin_hist[2]=='H')==last);
        char saved[COIN_HISTORY+1];strcpy(saved,coin_hist);
        for(int k=0;k<12;k++)coin_add_history(k&1);                        /* ten at most, newest last */
        assert(strlen(coin_hist)==COIN_HISTORY && coin_hist[COIN_HISTORY-1]=='H' && lv_obj_get_child_count(coin_history)==COIN_HISTORY);
        strcpy(coin_hist,saved);page_pop();frame(500);coin_page();frame(500);
        assert(lv_obj_get_child_count(coin_history)==3);
    }
    /* edge-on and mid-tumble frames render the rim */
    coin_theta=1.5707963f;coin_render();assert(coin_px[(COIN_CH/2)*COIN_CW+COIN_CW/2].alpha==255);
    assert(coin_px[4*COIN_CW+COIN_CW/2].alpha==0);
    coin_theta=0.9f;coin_lift=120;coin_render();shot("coin-tumble.ppm");coin_theta=0;coin_lift=0;coin_render();
    lv_obj_send_event(coin_reset_btn,LV_EVENT_CLICKED,NULL);
    assert(coin_flips==0 && lv_obj_get_child_count(coin_history)==0);
    coin_loaded=false;coin_flips=7;coin_load();assert(coin_flips==0 && !coin_hist[0]);   /* Reset is saved too */
    page_pop();frame(500);assert(!coin_page_obj && !coin_timer);
    magic_page();frame(500);shot("magic-ready.ppm");assert(magic_page_obj && magic_timer);
    lv_obj_send_event(magic_ball,LV_EVENT_CLICKED,NULL);
    int first_choice=magic_choice;
    lv_obj_send_event(magic_ball,LV_EVENT_CLICKED,NULL);assert(magic_choice==first_choice);
    magic_started=sl_ms()-120;frame(16);shot_now("magic-shake.ppm");   /* near the widest tilt */
    magic_started-=1000;frame(50);shot("magic-reveal.ppm");
    magic_started-=700;frame(100);
    shot("magic-result.ppm");
    assert(magic_choice>=0 && magic_choice<=20);
    assert(!strcmp(lv_label_get_text(magic_answer),magic_answers[magic_choice]));
    assert(!magic_thinking && !strcmp(lv_label_get_text(magic_button_text),"ASK AGAIN"));
    magic_choice=20;magic_started=sl_ms()-1700;lv_timer_resume(magic_timer);frame(100);
    assert(!strcmp(lv_label_get_text(magic_answer),"You're hot"));
    assert(!strcmp(lv_label_get_text(magic_caption),"So... what are you doing later tonight?"));
#if LV_USE_PERF_MONITOR
    lv_sysmon_hide_performance(disp);
#endif
    shot("magic-youre-hot.ppm");
    magic_choice=10;magic_started=sl_ms()-1700;lv_timer_resume(magic_timer);frame(100);
    shot("magic-long-answer.ppm");
    lv_obj_send_event(lv_obj_get_parent(magic_button_text),LV_EVENT_CLICKED,NULL);magic_started-=1700;frame(100);
    page_pop();frame(500);assert(!magic_page_obj && !magic_timer);
    /* Chess: the new game sheet, tap and drag moves with their hints, the
     * computer on its own thread, undo, review, promotion, every draw rule,
     * the game over card, and the save. */
    mkdir(".rockbox");mkdir(".rockbox/sloppy");remove(".rockbox/sloppy/chess.dat");
    chess_loaded=false;chess_page();frame(500);
    assert(chess_page_obj && chess_art_ready && chess_sheet_shown && chess_on_top());   /* nothing to continue: the sheet */
    assert(lv_obj_has_flag(mini,LV_OBJ_FLAG_HIDDEN));    /* the toolbar sits where the mini player would */
    assert(lv_obj_has_flag(chess_sheet_close,LV_OBJ_FLAG_HIDDEN));
    shot("chess-sheet.ppm");
    lv_obj_send_event(chess_lvl_chip[4],LV_EVENT_CLICKED,NULL);assert(chess_setup_level==4);
    assert(strstr(lv_label_get_text(chess_sheet_blurb),"punishes"));
    lv_obj_send_event(lv_obj_get_parent(chess_seg_lbl[1]),LV_EVENT_CLICKED,NULL);frame(300);
    assert(chess_setup_mode==0 && lv_obj_has_flag(chess_sheet_cpu,LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(chess_sheet_friend,LV_EVENT_CLICKED,NULL);assert(!chess_setup_turn);
    lv_obj_send_event(chess_sheet_friend,LV_EVENT_CLICKED,NULL);assert(chess_setup_turn);
    shot("chess-sheet-friend.ppm");
    lv_obj_send_event(lv_obj_get_parent(chess_seg_lbl[0]),LV_EVENT_CLICKED,NULL);frame(300);
    assert(chess_setup_mode==1 && !lv_obj_has_flag(chess_sheet_cpu,LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(chess_lvl_chip[1],LV_EVENT_CLICKED,NULL);
    lv_obj_send_event(chess_side_chip[0],LV_EVENT_CLICKED,NULL);assert(chess_setup_side==0);
    lv_obj_send_event(lv_obj_get_parent(find_label(chess_sheet,"Start game")),LV_EVENT_CLICKED,NULL);frame(500);
    assert(!chess_sheet_shown && chess.mode==1 && chess.human==1 && chess.level==1 && !chess_thinking && chess_timer);
    assert(!chess_flipped && objects(pages[npages-1])<160);
    assert(has_text(chess_bar[1],"Computer") && has_text(chess_bar[1],"Easy") && has_text(chess_bar[0],"Your move"));
    chess_tap_square(CH_SQ(4,6));assert(chess_selected==-1);   /* not your piece */
    chess_tap_square(CH_SQ(6,0));assert(chess_selected==CH_SQ(6,0) && chess_ntargets==2);
    chess_tap_square(CH_SQ(6,0));assert(chess_selected==-1);   /* a second tap puts it down */
    chess_tap_square(CH_SQ(4,1));assert(chess_selected==CH_SQ(4,1) && chess_ntargets==2);
    shot("chess-select.ppm");
    chess_tap_square(CH_SQ(4,1));assert(chess_selected==-1);
    {   /* a real finger: press e2, drag it up, the piece rides above the finger with a ring on e4 */
        int x,y,tx,ty;chess_center(CH_SQ(4,1),&x,&y);chess_center(CH_SQ(4,3),&tx,&ty);ty+=CHESS_DRAG_LIFT;
        touch_at(x,y,true);assert(chess_selected==CH_SQ(4,1));
        for(int i=1;i<=6;i++)touch_at(x+(tx-x)*i/6,y+(ty-y)*i/6,true);
        assert(chess_dragging && chess_hover==CH_SQ(4,3) && npages>1 && g_state!=G_BACK);
        shot_now("chess-drag.ppm");
        touch_at(tx,ty,false);
        assert(!chess_dragging && chess.plies==1 && chess.pos.board[CH_SQ(4,3)]==CH_P && chess_thinking);
        assert(!strcmp(chess_san[0],"e4") && chess_strip_n==1 && has_text(chess_bar[1],"Thinking"));
        /* dropped off any target: back home, no move */
    }
    chess_tap_square(CH_SQ(3,1));assert(chess_selected==-1);   /* no input while it thinks */
    chess_wait_reply();
    assert(chess.plies==2 && !chess_thinking && chess.pos.side==1 && !chess_job.busy && chess_strip_n==2);
    shot("chess-reply.ppm");
    {   /* a drag that ends on no target glides home */
        int x,y;chess_center(CH_SQ(3,1),&x,&y);
        touch_at(x,y,true);touch_at(x+30,y-40,true);touch_at(x+60,y-90,true);assert(chess_dragging);
        touch_at(x+60,y-90,false);assert(!chess_dragging && chess.plies==2 && chess_fly_on);
        frame(300);assert(!chess_fly_on && chess.pos.board[CH_SQ(3,1)]==CH_P);
    }
    /* a tapped move glides: caught halfway */
    chess_tap_square(CH_SQ(6,0));chess_tap_square(CH_SQ(5,2));frame(80);
    assert(chess_fly_on && chess_nfly==1 && chess_fly_t>0 && chess_fly_t<1024);
    shot_now("chess-glide.ppm");
    chess_wait_reply();assert(chess.plies==4);
    /* review: step back through the moves and forward again */
    lv_obj_send_event(chess_btn[CHESS_BTN_BACK],LV_EVENT_CLICKED,NULL);frame(400);
    assert(chess_view==3 && !chess_live() && chess_shown.board[CH_SQ(5,2)]==CH_N);
    lv_obj_send_event(chess_btn[CHESS_BTN_BACK],LV_EVENT_CLICKED,NULL);frame(400);
    assert(chess_view==2 && chess_shown.board[CH_SQ(6,0)]==CH_N);
    shot("chess-review.ppm");
    chess_tap_square(CH_SQ(1,0));assert(chess_live() && chess_selected==-1);   /* a tap goes back to the game */
    lv_obj_send_event(chess_strip_lbl[0],LV_EVENT_CLICKED,NULL);frame(400);assert(chess_view==1);
    lv_obj_send_event(chess_btn[CHESS_BTN_FWD],LV_EVENT_CLICKED,NULL);frame(400);assert(chess_view==2);
    chess_show(chess.plies);chess_redraw();chess_ui_update();
    lv_obj_send_event(chess_btn[CHESS_BTN_UNDO],LV_EVENT_CLICKED,NULL);frame(400);
    assert(chess.plies==2 && chess.pos.side==1 && chess_strip_n==2);   /* your move and its reply */
    lv_obj_send_event(chess_btn[CHESS_BTN_FLIP],LV_EVENT_CLICKED,NULL);frame(100);
    assert(chess_flipped && chess_bar_side(0)==-1 && !strcmp(lv_label_get_text(chess_coord[0]),"h"));
    shot("chess-flipped.ppm");
    lv_obj_send_event(chess_btn[CHESS_BTN_FLIP],LV_EVENT_CLICKED,NULL);frame(100);assert(!chess_flipped);
    {   /* Hard thinks for up to 1.6 s: the screen keeps drawing meanwhile; an
         * undo while it thinks stops the search at once */
        lv_obj_send_event(chess_btn[CHESS_BTN_UNDO],LV_EVENT_CLICKED,NULL);frame(400);assert(chess.plies==0);
        chess.level=3;
        uint64_t sync_us;
        {   /* before: the same search ran inside one frame */
            struct sl_chess_pos q;sl_chess_init(&q);struct sl_chess_move e4={CH_SQ(4,1),CH_SQ(4,3),0,0},d5={CH_SQ(3,6),CH_SQ(3,4),0,0},bm;struct sl_chess_undo u;
            sl_chess_make(&q,&e4,&u);sl_chess_make(&q,&d5,&u);uint64_t t0=sl_perf_now_us();sl_chess_think_timed(&q,3,7,&bm,sl_ms,1600);
            sync_us=sl_perf_now_us()-t0;
        }
        chess_tap_square(CH_SQ(4,1));chess_tap_square(CH_SQ(4,3));assert(chess.plies==1 && chess_thinking);
        uint64_t worst=0;int frames=0;
        while((chess_thinking||chess_job.busy) && frames<600){uint64_t t0=sl_perf_now_us();frame(16);uint64_t d=sl_perf_now_us()-t0;if(d>worst)worst=d;frames++;usleep(2000);}
        assert(!chess_thinking && chess.plies==2);
        printf("chess hard: %d frames while thinking, worst frame %llu us (one synchronous search: %llu us)\n",
               frames,(unsigned long long)worst,(unsigned long long)sync_us);
        assert(worst<sync_us/4);               /* no frame carried the search */
        frame(400);
        chess.level=4;
        chess_tap_square(CH_SQ(3,1));chess_tap_square(CH_SQ(3,3));assert(chess.plies==3);
        for(int i=0;i<40 && !chess_job.busy;i++){usleep(5000);frame(30);}
        assert(chess_job.busy && chess_thinking);
        uint32_t t0=sl_ms();
        lv_obj_send_event(chess_btn[CHESS_BTN_UNDO],LV_EVENT_CLICKED,NULL);
        assert(chess.plies==2 && !chess_thinking && chess.pos.side==1);
        for(int i=0;i<300 && chess_job.busy;i++){usleep(2000);frame(16);}
        assert(!chess_job.busy && chess.plies==2 && sl_ms()-t0<400);   /* stopped, and not played */
        chess.level=1;frame(400);
    }
    /* promotion: the four pieces stand on the file, over a dimmed board */
    sl_chess_new(&chess,0,1,1);
    assert(sl_chess_from_fen(&chess.pos,"8/P6k/8/8/8/8/6p1/K7 w - -"));
    chess.hashes[0]=sl_chess_hash(&chess.pos);chess_strip_sync();chess_reset_view();chess_redraw();chess_ui_update();
    chess_tap_square(CH_SQ(0,6));chess_tap_square(CH_SQ(0,7));
    assert(chess_promo_open && chess.plies==0);
    shot("chess-promo.ppm");
    chess_tap_square(CH_SQ(3,3));assert(!chess_promo_open && chess.plies==0);   /* elsewhere: cancelled */
    chess_tap_square(CH_SQ(0,6));chess_tap_square(CH_SQ(0,7));assert(chess_promo_open);
    chess_tap_square(CH_SQ(0,6));   /* the second slot down: a knight */
    assert(chess.plies==1 && chess.pos.board[CH_SQ(0,7)]==CH_N && !strcmp(chess_san[0],"a8=N"));
    frame(400);frame(400);assert(chess_flipped);   /* two players: the board turns for Black */
    /* check: the king glows red */
    sl_chess_new(&chess,0,1,1);assert(sl_chess_from_fen(&chess.pos,"4k3/8/8/1B6/8/8/4P3/4K3 w - -"));
    chess.hashes[0]=sl_chess_hash(&chess.pos);chess_reset_view();chess_redraw();
    chess_tap_square(CH_SQ(1,4));chess_tap_square(CH_SQ(2,5));   /* Bc6+ */
    frame(400);assert(has_text(chess_bar[chess_flipped?0:1],"Check") && !strcmp(chess_san[0],"Bc6+"));
    frame(400);frame(400);shot("chess-check.ppm");
    {   /* every way a game can be drawn gets its own card */
        struct { const char *fen; const char *from, *to; int halfmove; enum sl_chess_status st; const char *why; } draws[] = {
            { "7k/8/6Q1/8/8/8/8/K7 w - -", "g6", "f7", 0, CH_STALEMATE, "Stalemate" },
            { "7k/8/8/8/8/8/R7/K7 w - -", "a2", "b2", 99, CH_DRAW_FIFTY, "Fifty moves" },
            { "8/8/4k3/8/8/8/3pK3/2B5 w - -", "c1", "d2", 0, CH_DRAW_MATERIAL, "Neither side" },
        };
        for(int i=0;i<3;i++)
        {
            sl_chess_new(&chess,0,1,1);assert(sl_chess_from_fen(&chess.pos,draws[i].fen));
            chess.pos.halfmove=(uint16_t)draws[i].halfmove;chess.hashes[0]=sl_chess_hash(&chess.pos);
            chess_reset_view();chess_redraw();chess_ui_update();
            chess_tap_square(CH_SQ(draws[i].from[0]-'a',draws[i].from[1]-'1'));
            chess_tap_square(CH_SQ(draws[i].to[0]-'a',draws[i].to[1]-'1'));
            assert(sl_chess_game_status(&chess)==draws[i].st);
            for(int k=0;k<4;k++)frame(300);
            assert(!lv_obj_has_flag(chess_over,LV_OBJ_FLAG_HIDDEN) && has_text(chess_over,draws[i].why));
            assert(has_text(chess_bar[0],"Draw"));
            if(!i) shot("chess-stalemate.ppm");
            lv_obj_send_event(lv_obj_get_parent(find_label(chess_over,"Review")),LV_EVENT_CLICKED,NULL);
            assert(lv_obj_has_flag(chess_over,LV_OBJ_FLAG_HIDDEN));
            chess_tap_square(CH_SQ(0,0));assert(chess_selected==-1);   /* game over: the board is still */
        }
        /* threefold repetition, knights out and back twice */
        lv_obj_send_event(chess_btn[CHESS_BTN_NEW],LV_EVENT_CLICKED,NULL);frame(400);assert(chess_sheet_shown);
        lv_obj_send_event(lv_obj_get_parent(chess_seg_lbl[1]),LV_EVENT_CLICKED,NULL);
        lv_obj_send_event(lv_obj_get_parent(find_label(chess_sheet,"Start game")),LV_EVENT_CLICKED,NULL);frame(500);
        assert(chess.mode==0 && chess.plies==0);
        const char *shuffle[]={"g1f3","g8f6","f3g1","f6g8","g1f3","g8f6","f3g1","f6g8"};
        for(int i=0;i<8;i++){
            chess_tap_square(CH_SQ(shuffle[i][0]-'a',shuffle[i][1]-'1'));chess_tap_square(CH_SQ(shuffle[i][2]-'a',shuffle[i][3]-'1'));
            frame(400);frame(400);}
        assert(chess.plies==8 && sl_chess_game_status(&chess)==CH_DRAW_REPETITION);
        for(int k=0;k<4;k++)frame(300);
        assert(has_text(chess_over,"three times"));
        lv_obj_send_event(lv_obj_get_parent(find_label(chess_over,"Review")),LV_EVENT_CLICKED,NULL);
    }
    /* fool's mate between two players: Black wins, White's king topples */
    sl_chess_new(&chess,0,1,1);chess_strip_sync();chess_reset_view();chess_redraw();chess_ui_update();
    const char *fool[]={"f2f3","e7e5","g2g4","d8h4"};
    for(int i=0;i<4;i++){
        chess_tap_square(CH_SQ(fool[i][0]-'a',fool[i][1]-'1'));chess_tap_square(CH_SQ(fool[i][2]-'a',fool[i][3]-'1'));
        frame(400);frame(400);}
    assert(sl_chess_game_status(&chess)==CH_CHECKMATE && !strcmp(chess_san[3],"Qh4#"));
    for(int k=0;k<4;k++)frame(300);
    assert(has_text(chess_over,"Checkmate") && has_text(chess_over,"Black wins"));
    assert(lv_image_get_src(chess_over_king[0])==&chess_big_dsc[2] && lv_image_get_src(chess_over_king[1])==&chess_big_dsc[1]);
    shot("chess-mate.ppm");
    lv_obj_send_event(lv_obj_get_parent(find_label(chess_over,"Review")),LV_EVENT_CLICKED,NULL);frame(300);
    assert(has_text(chess_bar[chess_flipped?1:0],"Winner")==false && has_text(chess_bar[chess_flipped?0:1],"Winner"));
    shot("chess-mate-board.ppm");
    /* beating the computer: you win, then a rematch with the colours swapped */
    sl_chess_new(&chess,1,3,1);assert(sl_chess_from_fen(&chess.pos,"6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - -"));
    chess.hashes[0]=sl_chess_hash(&chess.pos);chess_strip_sync();chess_reset_view();chess_redraw();chess_ui_update();
    chess_tap_square(CH_SQ(3,0));chess_tap_square(CH_SQ(3,7));
    for(int k=0;k<5;k++)frame(300);
    assert(has_text(chess_over,"You win") && !chess_thinking);
    shot("chess-win.ppm");
    lv_obj_send_event(lv_obj_get_parent(find_label(chess_over,"Rematch")),LV_EVENT_CLICKED,NULL);frame(300);
    assert(chess.mode==1 && chess.level==3 && chess.human==-1 && chess.plies==0 && chess_flipped && chess_thinking);
    assert(lv_obj_has_flag(chess_over,LV_OBJ_FLAG_HIDDEN));
    chess.level=1;chess_wait_reply();
    assert(chess.plies==1 && chess.pos.side==-1 && !chess_thinking);
    assert(lv_obj_has_state(chess_btn[CHESS_BTN_UNDO],LV_STATE_DISABLED));   /* the computer's first move stays */
    chess_tap_square(CH_SQ(4,6));chess_tap_square(CH_SQ(4,4));assert(chess.plies==2);
    chess_wait_reply();assert(chess.plies==3);
    char saved_san[3][8];memcpy(saved_san,chess_san,sizeof saved_san);
    /* the save survives a restart: board, moves in notation and all */
    page_pop();frame(500);assert(!chess_page_obj && !chess_timer);
    struct sl_chess_pos was=chess.pos;
    memset(&chess,0,sizeof chess);memset(chess_san,0,sizeof chess_san);chess_loaded=false;chess_load();
    assert(sl_chess_game_valid(&chess) && chess.plies==3 && chess.mode==1 && !memcmp(&was,&chess.pos,sizeof was));
    for(int i=0;i<3;i++)assert(!strcmp(saved_san[i],chess_san[i]));
    chess_page();frame(500);
    assert(!chess_sheet_shown && chess_strip_n==3 && chess_flipped && chess_view==3);   /* straight back to the game */
    page_pop();frame(500);
    {   /* a save from before the five levels: kept, its Hard still Hard */
        struct sl_chess_game old;sl_chess_new(&old,1,2,1);old.magic=SL_CHESS_MAGIC_V1;old.checksum=sl_chess_game_checksum(&old);
        int fd=open(CHESS_FILE,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0 && write(fd,&old,sizeof old)==(ssize_t)sizeof old);sim_close(fd);
        chess_loaded=false;chess_load();assert(sl_chess_game_valid(&chess) && chess.level==3);
        fd=open(CHESS_FILE,O_WRONLY|O_TRUNC,0666);assert(fd>=0 && write(fd,"junk",4)==4);sim_close(fd);   /* the fake card has 8 handles */
        chess_loaded=false;chess_load();assert(sl_chess_game_valid(&chess) && chess.plies==0);   /* unreadable: a fresh board */
    }
    {   /* the board is the accent's own colour, in either theme */
        uint32_t accent=global_settings.sloppy_accent_color;
        global_settings.sloppy_accent_color=0xe28435;global_settings.sloppy_theme=1;styles_retheme();
        chess_loaded=false;chess_load();chess_page();frame(500);
        lv_obj_send_event(lv_obj_get_parent(find_label(chess_sheet,"Start game")),LV_EVENT_CLICKED,NULL);frame(500);
        chess_tap_square(CH_SQ(3,1));chess_tap_square(CH_SQ(3,3));chess_wait_reply();
        chess_tap_square(CH_SQ(2,1));frame(300);
        uint16_t a565=chess_cell[0];assert(a565!=chess_565(0xecd8b8));
        shot("chess-accent-light.ppm");
        chess_tap_square(CH_SQ(2,1));page_pop();frame(500);
        global_settings.sloppy_accent_color=accent;global_settings.sloppy_theme=0;styles_retheme();
    }
    chess_loaded=false;remove(CHESS_FILE);chess_load();
    page_pop();frame(500);
    snake_page();frame(500);shot("snake.ppm");assert(objects(pages[npages-1])<55);
    lv_obj_send_event(snake_mode_buttons[1],LV_EVENT_CLICKED,NULL);assert(snake_state.levels);
    snake_state.score=9;snake_state.food_x=9;snake_state.food_y=8;
    snake_started=true;snake_tick(NULL);
    assert(snake_state.score==10 && snake_state.obstacle_count==4);
    assert(snake_level_banner_until>sl_ms() && strstr(lv_label_get_text(snake_overlay_label),"Level 2"));
    shot("snake-levels.ppm");
    page_pop();frame(500);assert(snake_timer==NULL);
    mkdir(".rockbox");mkdir(".rockbox/sloppy");
    radio_page();frame(500);shot("radio.ppm");
    double coins_before=radio_state.listeners;
    lv_obj_send_event(radio_dial,LV_EVENT_CLICKED,NULL);assert(radio_state.listeners>coins_before);
    radio_state.listeners=radio_state.run_total=1e6;
    assert(radio_tab==RT_STUDIO && radio_nrows==1 && radio_row_id[0]==FM_INTERN);
    lv_obj_send_event(radio_rows[0],LV_EVENT_CLICKED,NULL);assert(radio_state.owned[FM_INTERN]==1);
    assert(!lv_obj_has_flag(radio_tab_badge[RT_UPGRADES],LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(radio_tab_badge[RT_STUDIO],LV_OBJ_FLAG_HIDDEN));
    frame(1100);assert(radio_nrows==6 && radio_row_id[5]==FM_CHOPPER);   /* up to what 1e6 can see */
    lv_obj_send_event(radio_amounts[3],LV_EVENT_CLICKED,NULL);
    lv_obj_send_event(radio_rows[1],LV_EVENT_CLICKED,NULL);assert(radio_state.owned[FM_JINGLE]>20);
    frame(300);shot("radio-studio.ppm");lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),560,LV_ANIM_OFF);frame(100);shot("radio-studio-list.ppm");lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),0,LV_ANIM_OFF);
    {   /* a refresh between a tab tap and its rebuild leaves the old tab's rows alone (their ids index that tab's tables) */
        char was[64];snprintf(was,sizeof was,"%s",lv_label_get_text(radio_row_cost[0]));
        lv_obj_send_event(radio_tabs[RT_UPGRADES],LV_EVENT_CLICKED,NULL);radio_refresh();
        assert(radio_tab==RT_UPGRADES && !strcmp(lv_label_get_text(radio_row_cost[0]),was));
    }
    frame(100);
    {   /* the 200 ms refresh must stay cheap: time it on the busiest tab */
        struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
        for(int i=0;i<500;i++)radio_refresh();
        clock_gettime(CLOCK_MONOTONIC,&t1);
        double us=((t1.tv_sec-t0.tv_sec)*1e9+(t1.tv_nsec-t0.tv_nsec))/1e3/500;
        printf("radio_refresh: %.1f us per call (%d rows)\n",us,radio_nrows);
        lv_refr_now(disp);
        /* nothing changed, so a refresh must not dirty the screen */
        radio_refresh();assert(!lv_display_get_default()->inv_p);
    }
    assert(radio_tab==RT_UPGRADES && radio_nrows>0);shot("radio-upgrades.ppm");
    {lv_obj_t *body=lv_obj_get_parent(radio_list);lv_obj_update_layout(body);
     for(uint32_t i=0;i<lv_obj_get_child_count(radio_list);i++){lv_obj_t *c=lv_obj_get_child(radio_list,i);
         if(lv_obj_check_type(c,&lv_label_class)&&!strcmp(lv_label_get_text(c),"COMING UP")){
             lv_obj_scroll_to_y(body,lv_obj_get_y(radio_list)+lv_obj_get_y(c)-20,LV_ANIM_OFF);lv_refr_now(disp);shot("radio-coming.ppm");break;}}
     lv_obj_scroll_to_y(body,0,LV_ANIM_OFF);}lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),560,LV_ANIM_OFF);frame(100);shot("radio-upgrades-list.ppm");lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),0,LV_ANIM_OFF);
    {int id=radio_row_id[0];lv_obj_send_event(radio_rows[0],LV_EVENT_CLICKED,NULL);assert(sl_fm_has(&radio_state,id));}
    frame(100);{int before=fm_popcount(radio_state.upgrades,FM_UPG_WORDS);radio_state.listeners=1e7;
     lv_obj_send_event(radio_buy_all,LV_EVENT_CLICKED,NULL);assert(fm_popcount(radio_state.upgrades,FM_UPG_WORDS)>before+1);}
    frame(600);lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),lv_obj_get_y(radio_tab_row)+150,LV_ANIM_OFF);frame(100);shot("radio-bought.ppm");
    /* bought upgrades stay on the list, under Your upgrades */
    frame(100);assert(radio_owned_head && strstr(lv_label_get_text(radio_owned_head),"YOUR UPGRADES"));
    {lv_obj_t *card=lv_obj_get_child(radio_list,0);   /* summary on top of the tab */
     assert(!strcmp(lv_label_get_text(lv_obj_get_child(card,1)),"Your upgrades"));
     /* everything bought is still a row in the shop, marked Owned */
     int owned_rows=0;for(int k=0;k<radio_nrows;k++)if(sl_fm_has(&radio_state,radio_row_id[k])){owned_rows++;
         assert(strstr(lv_label_get_text(radio_row_cost[k]),"Owned"));}
     assert(owned_rows==fm_popcount(radio_state.upgrades,FM_UPG_WORDS));
     SFX_RESET();for(int k=0;k<radio_nrows;k++)if(sl_fm_has(&radio_state,radio_row_id[k])){
         lv_obj_send_event(radio_rows[k],LV_EVENT_CLICKED,NULL);break;}
     assert(sfx_last!=SFX_ERROR && strstr(lv_label_get_text(lv_obj_get_child(banner_obj,0)),"Already yours"));
     lv_obj_update_layout(radio_list);
     lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),lv_obj_get_y(radio_owned_head)+560,LV_ANIM_OFF);frame(100);shot("radio-owned.ppm");
     lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),0,LV_ANIM_OFF);}
    radio_state.listeners=45000;
    {   /* cost of a tap plus the frames it causes */
        lv_obj_send_event(radio_tabs[RT_STUDIO],LV_EVENT_CLICKED,NULL);frame(100);
        struct timespec t0,t1;double tap=0,draw=0;
        for(int i=0;i<30;i++){
            clock_gettime(CLOCK_MONOTONIC,&t0);lv_obj_send_event(radio_dial,LV_EVENT_CLICKED,NULL);clock_gettime(CLOCK_MONOTONIC,&t1);
            tap+=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6;
            for(int f=0;f<3;f++){clock_gettime(CLOCK_MONOTONIC,&t0);frame(33);clock_gettime(CLOCK_MONOTONIC,&t1);
                draw+=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6;}
        }
        printf("radio tap: %.2f ms handler, %.2f ms per frame while tapping\n",tap/30,draw/90);
        lv_obj_send_event(radio_tabs[RT_UPGRADES],LV_EVENT_CLICKED,NULL);frame(100);
    }
    /* Signal Lock: quick taps build the combo and multiply taps */
    {double t0=radio_state.tap_total;for(int i=0;i<20;i++)lv_obj_send_event(radio_dial,LV_EVENT_CLICKED,NULL);
     assert(radio_combo>1.0-1e-9 && radio_state.tap_total-t0>20*radio_rates.per_tap*1.5);frame(300);
     assert(radio_vu_lit[0]==RADIO_SEGS && strstr(lv_label_get_text(radio_knob_caption),"SIGNAL LOCK"));
     shot("radio-combo.ppm");radio_combo_ms-=5000;radio_last_ms-=3000;frame(300);assert(radio_combo<0.1);}
    assert(strncmp(lv_label_get_text(radio_ticker),"NEWS",4)==0 || strstr(lv_label_get_text(radio_ticker),"BREAKING"));
    radio_caller_next=sl_ms();frame(300);assert(!lv_obj_has_flag(radio_caller,LV_OBJ_FLAG_HIDDEN));shot("radio-caller.ppm");
    {radio_ticker_ms-=radio_ticker_len;const char *old=lv_label_get_text(radio_ticker);char prev[160];strlcpy(prev,old,sizeof prev);
     frame(250);assert(strcmp(prev,lv_label_get_text(radio_ticker)));}
    lv_obj_send_event(radio_caller,LV_EVENT_CLICKED,NULL);
    assert(radio_state.callers==1 && lv_obj_has_flag(radio_caller,LV_OBJ_FLAG_HIDDEN));
    radio_state.all_total=radio_state.run_total=8.5*FM_RIGHT_BASE;
    lv_obj_send_event(radio_tabs[RT_SYNDICATE],LV_EVENT_CLICKED,NULL);frame(100);shot("radio-syndicate.ppm");
    lv_obj_send_event(radio_synd_btn,LV_EVENT_CLICKED,NULL);assert(radio_state.rights==0);   /* first tap arms */
    lv_obj_send_event(radio_synd_btn,LV_EVENT_CLICKED,NULL);
    assert(radio_state.rights==2 && radio_state.syndications==1 && !radio_state.owned[FM_INTERN] && radio_state.listeners<1);
    frame(300);assert(!lv_obj_has_flag(radio_banner,LV_OBJ_FLAG_HIDDEN));shot("radio-syndicated.ppm");
    frame(2500);assert(lv_obj_has_flag(radio_banner,LV_OBJ_FLAG_HIDDEN));
    frame(100);assert(radio_row_id[PK_CONTRACT]==PK_CONTRACT);
    lv_obj_send_event(radio_rows[PK_CONTRACT],LV_EVENT_CLICKED,NULL);assert(sl_fm_perk(&radio_state,PK_CONTRACT));
    lv_obj_send_event(radio_rows[PK_SEED1],LV_EVENT_CLICKED,NULL);assert(!sl_fm_perk(&radio_state,PK_SEED1));
    frame(100);lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),560,LV_ANIM_OFF);frame(100);shot("radio-deals.ppm");lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),0,LV_ANIM_OFF);
    lv_obj_send_event(radio_tabs[RT_STATS],LV_EVENT_CLICKED,NULL);frame(100);
    assert(!strcmp(lv_label_get_text(radio_stat_val[ST_SYND]),"1"));
    {lv_obj_t *shelf=lv_obj_get_child(radio_list,lv_obj_get_child_count(radio_list)-1);
     assert(lv_obj_get_child_count(shelf)==FM_PLAQUE_COUNT);
     lv_obj_send_event(lv_obj_get_child(shelf,0),LV_EVENT_CLICKED,NULL);
     assert(strstr(lv_label_get_text(lv_obj_get_child(banner_obj,0)),"Earned: 100 listeners"));
     lv_obj_send_event(lv_obj_get_child(shelf,FM_PL_TOTAL+24),LV_EVENT_CLICKED,NULL);
     assert(strstr(lv_label_get_text(lv_obj_get_child(banner_obj,0)),"Locked:"));}lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),560,LV_ANIM_OFF);frame(100);shot("radio-stats.ppm");lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),0,LV_ANIM_OFF);
    lv_obj_send_event(radio_tabs[RT_STUDIO],LV_EVENT_CLICKED,NULL);frame(100);
    /* Station Formats: open after a syndication, one switch per 10 minutes */
    assert(radio_formats[FMT_TOP40] && radio_state.format==FMT_NONE);
    {double before=radio_rates.per_tap;
     lv_obj_send_event(radio_formats[FMT_TALK],LV_EVENT_CLICKED,NULL);
     assert(radio_state.format==FMT_TALK && fabs(radio_rates.per_tap-3*before)<1e-9);}
    SFX_RESET();lv_obj_send_event(radio_formats[FMT_TOP40],LV_EVENT_CLICKED,NULL);
    assert(radio_state.format==FMT_TALK && sfx_last==SFX_ERROR);
    radio_state.format_since-=FM_FORMAT_WAIT;
    lv_obj_send_event(radio_formats[FMT_TOP40],LV_EVENT_CLICKED,NULL);assert(radio_state.format==FMT_TOP40);
    frame(300);assert(strstr(lv_label_get_text(radio_format_info),"+20%"));
    lv_obj_scroll_to_y(lv_obj_get_parent(radio_list),560,LV_ANIM_OFF);frame(100);shot("radio-format.ppm");
    page_pop();frame(500);assert(radio_timer==NULL && !radio_page_obj);
    radio_loaded=false;memset(&radio_state,0,sizeof radio_state);radio_load();
    assert(radio_state.rights==2 && sl_fm_perk(&radio_state,PK_CONTRACT) && radio_state.callers==1 && radio_state.format==FMT_TOP40);
    {int fd=open(RADIO_FILE,O_WRONLY|O_TRUNC);assert(write(fd,"junk",4)==4);sim_close(fd);   /* the fake card has 8 handles */}   /* torn save */
    radio_loaded=false;memset(&radio_state,0,sizeof radio_state);radio_load();
    assert(radio_state.syndications==1 && sl_fm_valid(&radio_state));
    /* Freaky Spoon: a ready card, big arrows, no Restart button, the mini
     * player out of the way; a run starts on the first arrow press. */
    spoon_page();frame(500);shot("spoon.ppm");assert(objects(pages[npages-1])<90);
    assert(!find_label(pages[npages-1],"Restart") && find_label(pages[npages-1],"Freaky Spoon"));
    assert(spn_mode==SPN_READY && !lv_obj_has_flag(spn_card,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(mini,LV_OBJ_FLAG_HIDDEN));
    spoon_warp_test();
    page_pop();frame(500);assert(spoon_timer==NULL && !spoon_page_obj);
    /* Sound effects: every sound renders, bounded and audible; the level
     * setting scales it, Off silences it, and a tick never cuts a fanfare. */
    for(int id=0;id<SFX_COUNT;id++){
        SFX_RESET();sfx_play(id);
        assert(sfx_last==id && mix_frames>=441 && mix_peak>800 && mix_peak<=32767);
        sfx_write_wav(id);
    }
    global_settings.sloppy_sfx_volume=55;SFX_RESET();sfx_play(SFX_WIN);int quiet_peak=mix_peak;
    global_settings.sloppy_sfx_volume=90;SFX_RESET();sfx_play(SFX_WIN);assert(mix_peak>quiet_peak*2);
    global_settings.sloppy_sfx_volume=0;SFX_RESET();{unsigned n=sfx_count;sfx_play(SFX_WIN);assert(sfx_count==n && sfx_last==-1);}
    global_settings.sloppy_sfx_volume=70;
    SFX_RESET();sfx_play(SFX_WIN);sfx_play(SFX_TAP);assert(sfx_last==SFX_WIN);
    /* the room: a tail after the last note, and the two ears differ */
    SFX_RESET();sfx_play(SFX_WIN);assert(mix_frames>=44100*(780+SFX_TAIL_MS)/1000-2 && mix_lr_diff>200);
    /* interface sounds: quieter than app sounds, give way to them, and
     * have their own switch */
    SFX_RESET();sfx_play(SFX_UI_OPEN);assert(sfx_last==SFX_UI_OPEN);
    SFX_RESET();sfx_play(SFX_TAP);sfx_play(SFX_UI_OPEN);assert(sfx_last==SFX_TAP);
    global_settings.sloppy_ui_sounds=0;SFX_RESET();sfx_play(SFX_UI_OPEN);assert(sfx_last==-1);
    sfx_play(SFX_TAP);assert(sfx_last==SFX_TAP);global_settings.sloppy_ui_sounds=1;
    SFX_RESET();toast("Hello");assert(sfx_last==SFX_UI_NOTIFY);
    SFX_RESET();sl_games_page();frame(50);assert(sfx_last==SFX_UI_OPEN);
    SFX_RESET();page_pop();frame(400);assert(sfx_last==SFX_UI_BACK);
    SFX_RESET();sl_power_applied_mode=SL_POWER_ACTIVE;sl_power.mode=SL_POWER_LOCKED;sl_power_running=true;
    sl_power_apply();assert(sfx_last==SFX_UI_LOCK);
    SFX_RESET();sl_power.mode=SL_POWER_ACTIVE;sl_power_apply();assert(sfx_last==SFX_UI_UNLOCK);
    /* the same sound again within 60 ms is dropped; a different one is not */
    SFX_RESET();assert(sfx_play(SFX_TAP) && !sfx_play(SFX_TAP));
    usleep(61000);assert(sfx_play(SFX_TAP));
    SFX_RESET();assert(sfx_play(SFX_UI_TICK) && sfx_play(SFX_TAP));
    /* power sounds are interface sounds: the switch silences even the boot chime */
    global_settings.sloppy_ui_sounds=0;SFX_RESET();assert(!sfx_play(SFX_SYS_BOOT) && !sfx_play(SFX_SYS_SHUTDOWN));
    global_settings.sloppy_ui_sounds=1;SFX_RESET();assert(sfx_play(SFX_SYS_SHUTDOWN));
    /* charger: the first look only learns; then in and out are heard */
    SFX_RESET();power_sounds(60,true);assert(sfx_last==-1);
    power_sounds(60,true);assert(sfx_last==-1);
    power_sounds(60,false);assert(sfx_last==SFX_SYS_UNPLUG);
    SFX_RESET();power_sounds(60,true);assert(sfx_last==SFX_SYS_CHARGE);
    /* plugged in with the screen dark: lit on the lock screen for the bubble (#130); unplugged: left dark */
    { bool run=sl_power_running;sl_power_running=true;
      power_sounds(60,false);for(int i=0;i<40&&cb.root;i++)frame(100);assert(!cb.root);
      sl_power.mode=SL_POWER_DARK;sl_power_apply();assert(!test_lcd_on);
      power_sounds(60,true);assert(sl_power.mode==SL_POWER_LOCKED&&wallpaper_screen_dark==false&&cb.root);
      frame(0);frame(700);shot_now("charge-wake.ppm");
      sl_power.mode=SL_POWER_DARK;sl_power_apply();power_sounds(60,false);assert(sl_power.mode==SL_POWER_DARK);
      sl_power.mode=SL_POWER_ACTIVE;sl_power_apply();power_sounds(60,true);for(int i=0;i<8;i++)frame(200);sl_power_running=run; }
    /* low battery: once at 15, once at 5, with a toast; quiet in between */
    SFX_RESET();power_sounds(15,true);assert(sfx_last==-1);
    power_sounds(15,false);assert(sfx_last==SFX_SYS_BATT_LOW);
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(banner_obj,0)),"Battery low: 15%"));
    SFX_RESET();power_sounds(14,false);power_sounds(16,false);power_sounds(6,false);assert(sfx_last==-1);
    power_sounds(5,false);assert(sfx_last==SFX_SYS_BATT_LOW);
    SFX_RESET();power_sounds(3,false);assert(sfx_last==-1);
    power_sounds(3,true);SFX_RESET();power_sounds(12,false);assert(sfx_last==SFX_SYS_BATT_LOW);   /* re-armed by charging */
    SFX_RESET();power_sounds(19,false);power_sounds(12,false);assert(sfx_last==-1);             /* 19 % is not recovery */
    power_sounds(20,false);power_sounds(12,false);assert(sfx_last==SFX_SYS_BATT_LOW);           /* 20 % is */
    SFX_RESET();power_sounds(-1,false);power_sounds(50,false);assert(sfx_last==-1);
    /* volume keys tick when nothing plays, pitched by level; music answers for itself */
    { int saved=test_status;
      test_status=0;SFX_RESET();volume_key(1);assert(sfx_last==SFX_UI_TICK);
      test_status=AUDIO_STATUS_PLAY;SFX_RESET();volume_key(-1);assert(sfx_last==-1);
      test_status=saved; }
    /* the volume bar: still up 2 s after the key, a slide moves the level
     * from where it was (no jump), a tap sets it, and the bar's own margin
     * is never taken by a system gesture */
    { int saved_sl=sl_power_running;sl_power_running=false;
      global_status.volume=volume_from_pct(50);volume_key(0);frame(0);frame(120);shot_now("volume-glide.ppm");
      assert(lv_obj_get_style_translate_x(hud,0)>0);                /* still gliding in */
      frame(2000);assert(!lv_obj_has_flag(hud,LV_OBJ_FLAG_HIDDEN) && lv_obj_get_style_translate_x(hud,0)==0);
      shot_now("volume-hud.ppm");
      lv_area_t a;lv_obj_get_coords(hud,&a);int cx=(a.x1+a.x2)/2,cy=(a.y1+a.y2)/2,h=a.y2-a.y1;
      touch_at(cx,cy+40,true);assert(volume_pct()==50);         /* a press alone does not jump */
      for(int k=1;k<=10;k++)touch_at(cx,cy+40-k*h/20,true);     /* half the bar up */
      assert(volume_pct()>=95 && g_state==G_LVGL);
      touch_at(cx,cy+40-h/2,false);frame(1000);assert(!lv_obj_has_flag(hud,LV_OBJ_FLAG_HIDDEN));
      touch_at(a.x2+20,a.y2-h/4,true);touch_at(a.x2+20,a.y2-h/4,false);   /* tap in the margin */
      assert(volume_pct()>=22 && volume_pct()<=28);
      frame(1600);frame(300);assert(lv_obj_has_flag(hud,LV_OBJ_FLAG_HIDDEN));   /* glided out */
      sl_power_running=saved_sl; }
    sl_power_running=false;
    /* the lock screen follows the swipe, then slides up into home */
    frame(600);sl_power_running=true;sl_power_state_init(&sl_power,sl_ms());
    sl_power.mode=SL_POWER_LOCKED;sl_power_apply();
    assert(!lv_obj_has_flag(sl_power_overlay,LV_OBJ_FLAG_HIDDEN));
    sl_power_filter_touch(true,W/2,600);usleep(300000);sl_power_filter_touch(true,W/2,540);   /* slowly */
    assert(lv_obj_get_style_y(sl_power_overlay,0)==-60);
    usleep(300000);sl_power_filter_touch(false,0,0);assert(sl_power_is_locked());
    frame(400);assert(lv_obj_get_style_y(sl_power_overlay,0)==0);
    sl_power_filter_touch(true,W/2,600);sl_power_filter_touch(true,W/2,400);
    assert(lv_obj_get_style_y(sl_power_overlay,0)==-200);
    sl_power_filter_touch(false,0,0);assert(!sl_power_is_locked());
    assert(!lv_obj_has_flag(sl_power_overlay,LV_OBJ_FLAG_HIDDEN));
    frame(60);assert(lv_obj_get_style_y(sl_power_overlay,0)<-200);
    {   /* swipes the R1 logged as staying locked: 68 px in 256 ms, and a
         * thumb's slant (70 up, 90 sideways) */
        struct sl_power_state p;sl_power_state_init(&p,1000);p.mode=SL_POWER_LOCKED;
        sl_power_state_touch(&p,true,240,600,1000);sl_power_state_touch(&p,false,240,532,1256);
        assert(p.mode==SL_POWER_ACTIVE);
        p.mode=SL_POWER_LOCKED;
        sl_power_state_touch(&p,true,200,600,2000);sl_power_state_touch(&p,false,290,530,2400);
        assert(p.mode==SL_POWER_ACTIVE);
        p.mode=SL_POWER_LOCKED;    /* a sideways drag is not an unlock */
        sl_power_state_touch(&p,true,100,600,3000);sl_power_state_touch(&p,false,300,530,3200);
        assert(p.mode==SL_POWER_LOCKED);
    }
    frame(500);assert(lv_obj_has_flag(sl_power_overlay,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_y(sl_power_overlay,0)==0);
    /* a dark screen draws nothing; waking redraws the whole screen */
    if(!qp_glass_px)qp_glass_px=malloc((size_t)W*H*2);
    frame(50);sl_power.mode=SL_POWER_DARK;sl_power_apply();
    assert(!qp_glass_px&&!qp_sharp_px);   /* screen off: the 1.5 MB of glass goes back */
    lv_obj_invalidate(lv_screen_active());assert(!lv_display_get_default()->inv_p);
    /* labels wait while dark and catch up on wake */
    char clock_before[16];strlcpy(clock_before,lv_label_get_text(sb_clock),sizeof clock_before);
    int saved_fmt=global_settings.timeformat;global_settings.timeformat=!saved_fmt;
    for(int i=0;i<3;i++)frame(100);
    assert(!strcmp(lv_label_get_text(sb_clock),clock_before));
    sl_power.mode=SL_POWER_LOCKED;sl_power_apply();
    assert(sl_power_wake_pending && lv_display_get_default()->inv_p);
    assert(!test_lcd_on);   /* the panel is off: a frame drawn now would be dropped */
    {   /* headphones out while dark: the music pauses and the screen stays dark */
        int ev=test_events,st=test_status;test_status=AUDIO_STATUS_PLAY;
        sl_power.mode=SL_POWER_DARK;sl_power_apply();
        handle_button(SYS_PHONE_UNPLUGGED);assert(test_events==ev+1&&(test_status&AUDIO_STATUS_PAUSE));
        assert(sl_power.mode==SL_POWER_DARK&&!sl_power_suspended);
        handle_button(SYS_PHONE_PLUGGED);assert(sl_power.mode==SL_POWER_DARK);
        test_status=st;sl_power.mode=SL_POWER_LOCKED;sl_power_apply();
    }
    sl_power_finish_wake();assert(!sl_power_wake_pending && test_lcd_on);
    {   /* USB mode during the library read (2026-10-23: its open tag database
         * kept the card from the Mac): the read lets go before Rockbox takes
         * the card, starts over after, and the update after USB mode waits
         * for it */
        int saved=lib_state,frees=test_lib_frees,begins=test_lib_begins,dbu=db_update_state;
        lib_state=SL_LIB_LOADING;test_lib_begin_state=SL_LIB_LOADING;
        handle_button(SYS_USB_CONNECTED);
        assert(test_lib_frees==frees+1&&test_usb_lib_frees==frees+1);
        /* 2026-10-23, later: a folder left open (/.rockbox, then the card's
         * root) kept the card from the Mac for a whole session: none of ours
         * is open when Rockbox takes the card, nor after */
        assert(test_usb_dirs_open==0&&fake_dirs_open==0);
        assert(test_lib_begins==begins+1&&lib_state==SL_LIB_LOADING);
        assert(access(DB_USB_NOTE,F_OK)==0&&db_update_state==dbu);
        unlink(DB_USB_NOTE);test_lib_begin_state=SL_LIB_READY;lib_state=saved;
    }
    assert(strcmp(lv_label_get_text(sb_clock),clock_before));
    global_settings.timeformat=saved_fmt;statusbar_update();
    assert(!lv_obj_has_flag(sl_power_overlay,LV_OBJ_FLAG_HIDDEN));
    sl_power.mode=SL_POWER_ACTIVE;sl_power_apply();
    frame(500);
    {   /* 2026-10-23: USB mode reached the Mac from the home screen only. From
         * every kind of page the interface answers Rockbox (the USB screen)
         * holding no more of the card than home does, and the USB trace and
         * sloppy.log name the page it came on */
        static void (*const opens[])(void) = { music_page, albums_page, songs_page, sound_page, system_page,
                                               arcade_page, snake_page, chess_page, foryou_page, radio_page, sl_wifi_page };
        sl_power.mode=SL_POWER_ACTIVE;sl_power_apply();
        while(npages>1){page_pop();frame(400);}
        sl_usb_traced[0]='\0';handle_button(SYS_USB_CONNECTED);unlink(DB_USB_NOTE);
        int home_fds=test_usb_fds,screens=test_usb_screens;
        assert(strstr(sl_usb_traced,"ui: USB mode asked on home, 1 page deep")&&strstr(sl_usb_traced,"answering, the USB screen is up"));
        for(unsigned i=0;i<sizeof opens/sizeof opens[0];i++)
        {
            opens[i]();frame(400);
            char want[80];snprintf(want,sizeof want,"ui: USB mode asked on page \"");
            sl_usb_traced[0]='\0';handle_button(SYS_USB_CONNECTED);unlink(DB_USB_NOTE);
            if(test_usb_screens!=screens+1||test_usb_dirs_open!=0||test_usb_fds>home_fds||!strstr(sl_usb_traced,want))
                printf("USB from page %u: screens %d, dirs %d, fds %d (home %d), trace %s\n",i,test_usb_screens-screens,test_usb_dirs_open,test_usb_fds,home_fds,sl_usb_traced);
            assert(test_usb_screens==screens+1&&test_usb_dirs_open==0&&test_usb_fds<=home_fds&&strstr(sl_usb_traced,want));
            screens=test_usb_screens;
            while(npages>1){page_pop();frame(400);}
        }
        int st=test_status;test_status=AUDIO_STATUS_PLAY;   /* Now Playing opens only over music */
        np_show();frame(600);assert(np_open);sl_usb_traced[0]='\0';handle_button(SYS_USB_CONNECTED);unlink(DB_USB_NOTE);
        if(test_usb_screens!=screens+1||test_usb_fds>home_fds||!strstr(sl_usb_traced,"Now Playing open"))
            printf("USB from Now Playing: screens %d, fds %d (home %d), np_open %d, trace %s\n",test_usb_screens-screens,test_usb_fds,home_fds,np_open,sl_usb_traced);
        assert(test_usb_screens==screens+1&&test_usb_fds<=home_fds&&strstr(sl_usb_traced,"Now Playing open"));
        np_hide(0);frame(500);test_status=st;
    }
    /* the goodbye screen: it draws in, then clean_shutdown() keeps it up */
    bye_show(0,true);frame(150);shot_now("shutdown-anim.ppm");
    frame(600);assert(lv_obj_get_width(bye_line)==180);shot_now("shutdown.ppm");
    assert(sloppy_shutdown_screen(1));assert(!strcmp(lv_label_get_text(bye_note),"Battery low"));
    shot_now("shutdown-battery.ppm");
    bye_cancel();assert(lv_obj_has_flag(bye,LV_OBJ_FLAG_HIDDEN));
    sl_power_running=false;assert(!sloppy_shutdown_screen(0));   /* classic UI draws its own */
    sfx_play(SFX_LOSE);assert(sfx_last==SFX_LOSE);
    coin_page();frame(100);SFX_RESET();
    lv_obj_send_event(coin_flip_btn,LV_EVENT_CLICKED,NULL);assert(sfx_last==SFX_COIN_FLIP);
    coin_started=coin_now()-coin_air_ms-20;frame(20);assert(sfx_last==SFX_COIN_LAND);
    coin_started-=COIN_REVEAL_MS;frame(20);assert(sfx_last==(coin_heads?SFX_COIN_HEADS:SFX_COIN_TAILS));
    page_pop();frame(300);
    magic_page();frame(100);SFX_RESET();
    lv_obj_send_event(magic_ball,LV_EVENT_CLICKED,NULL);assert(sfx_last==SFX_MAGIC_SHAKE);
    magic_started-=950;frame(50);
    assert(sfx_last>=SFX_MAGIC_YES && sfx_last<=SFX_MAGIC_HOT);
    page_pop();frame(300);
    snake_page();frame(100);SFX_RESET();
    assert(!snake_state.levels && !lv_obj_has_flag(lv_obj_get_child(snake_page_obj,1),LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_send_event(snake_mode_buttons[1],LV_EVENT_CLICKED,NULL);
    assert(snake_state.levels);
    {int hx=snake_state.x[0];snake_tick(NULL);snake_tick(NULL);   /* nothing moves before the first input */
     assert(snake_state.x[0]==hx && snake_state.alive && strstr(lv_label_get_text(snake_overlay_sub),"Swipe"));}
    frame(400);
    assert(!find_label(snake_page_obj,LV_SYMBOL_RIGHT) && !find_label(snake_page_obj,LV_SYMBOL_UP));   /* no d-pad */
    {   /* a swipe anywhere below the header steers: here under the board, on the hint */
        int y=SNAKE_BY+SNAKE_BOARD+130;
        touch_at(150,y,true);touch_at(170,y,true);assert(g_state==G_GAME);
        touch_at(200,y,true);touch_at(200,y,false);frame(50);
    }
    assert(snake_started && snake_state.next_dx==1);
    lv_obj_send_event(lv_obj_get_parent(snake_pause_label),LV_EVENT_CLICKED,NULL);
    assert(snake_paused);
    {int hx=snake_state.x[0];snake_tick(NULL);assert(snake_state.x[0]==hx);}
    lv_obj_send_event(lv_obj_get_parent(snake_pause_label),LV_EVENT_CLICKED,NULL);
    assert(!snake_paused);
    assert(snake_key(BUTTON_UP) && snake_state.next_dy==-1 && snake_started);
    snake_state.food_x=snake_state.x[0]+snake_state.next_dx;snake_state.food_y=snake_state.y[0]+snake_state.next_dy;
    snake_tick(NULL);assert(sfx_last==SFX_SNAKE_EAT);
    shot("snake-playing.ppm");
    snake_state.x[0]=snake_state.next_dx>0?SL_SNAKE_GRID-1:snake_state.next_dx<0?0:snake_state.x[0];
    snake_state.y[0]=snake_state.next_dy>0?SL_SNAKE_GRID-1:snake_state.next_dy<0?0:snake_state.y[0];
    snake_tick(NULL);assert(!snake_state.alive && sfx_last==SFX_SNAKE_DIE);
    assert(fake_find(SNAKE_SCORES)>=0);
    snake_best[1]=0;snake_scores_loaded=false;snake_scores_load();
    assert(snake_best[1]>=1); /* the Levels best survives a reload */
    snake_reset(false);
    {lv_area_t aa;lv_obj_get_coords(snake_board,&aa);
     touch_at(aa.x1+192,aa.y1+210,true);
     touch_at(aa.x1+192,aa.y1+150,true);
     touch_at(aa.x1+192,aa.y1+140,false);
     assert(snake_started && snake_state.next_dy==-1);
     assert(lv_obj_get_scroll_y(lv_obj_get_child(snake_page_obj,1))==0);}
    {   struct sl_snake_state *st=&snake_state;int period=sl_snake_period(st);
        /* one stroke, two turns: right then down, both kept (the second queued) */
        snk_step_at=snk_now()-period/2;
        assert(st->dx==0 && st->dy==-1);
        snake_swipe_begin(200,500);snake_swipe_move(220,501);snake_swipe_move(240,502);
        assert(st->next_dx==1 && st->next_dy==0);
        snake_swipe_move(241,522);
        assert(st->has_queued_turn && st->queued_dx==0 && st->queued_dy==1);
        /* a U-turn is refused */
        snake_reset(false);snake_steer(0,-1);assert(snake_started);
        snk_step_at=snk_now()-period/2;snake_steer(0,1);assert(st->next_dy==-1 && !st->has_queued_turn);
        /* just after a step, a turn re-steers that step: the corner is where the head was */
        /* no food on the board: a random one beside the head made that step
         * a meal, which is never re-steered (the flake of old) */
        snake_reset(false);st->food_x=st->food_y=-1;snake_steer(1,0);assert(snake_started && st->dx==1);
        snk_step_at=snk_now();
        int bx=snk_before.x[0],by=snk_before.y[0];
        snake_swipe_begin(200,500);snake_swipe_move(201,480);
        assert(st->x[0]==bx && st->y[0]==by-1 && st->dy==-1);
        /* late in a step, a turn takes the next step at once */
        snk_step_at=snk_now()-period*7/10;
        int hx=st->x[0],hy=st->y[0];
        snake_steer(1,0);
        assert(st->x[0]==hx+1 && st->y[0]==hy && snk_step_at==snk_now());
        page_pop();frame(300);snake_page();frame(100);
    }
    page_pop();frame(300);
    spoon_page();frame(300);spoon_play_test();page_pop();frame(300);
    {   /* a game opened again over itself goes back to the one copy (two
         * copies shared its globals: closing one crashed the other's taps) */
        spoon_page();frame(400);int n=npages;lv_obj_t *first=spoon_page_obj;
        music_page();frame(400);spoon_page();frame(400);
        assert(npages==n&&pages[npages-1]==first&&spoon_page_obj==first);
        spoon_page();frame(100);assert(npages==n);
        lv_obj_send_event(spn_pad,LV_EVENT_PRESSED,NULL);lv_obj_send_event(spn_pad,LV_EVENT_RELEASED,NULL);
        page_pop();frame(400);assert(!spoon_page_obj);
    }
    ragdoll_page();frame(100);SFX_RESET();
    rd_nodes[9].y=356;rd_nodes[9].py=336;rd_thud_at=0;rd_tick(NULL);
    assert(sfx_last==SFX_RAGDOLL_THUD && mix_peak>800);
    page_pop();frame(300);
    radio_page();frame(100);SFX_RESET();
    lv_obj_send_event(radio_dial,LV_EVENT_CLICKED,NULL);assert(sfx_last==SFX_FM_TAP);
    radio_state.listeners=0;lv_obj_send_event(radio_rows[0],LV_EVENT_CLICKED,NULL);assert(sfx_last==SFX_ERROR);
    radio_state.listeners=1000000;SFX_RESET();
    lv_obj_send_event(radio_rows[0],LV_EVENT_CLICKED,NULL);assert(sfx_last==SFX_FM_BUY);
    page_pop();frame(300);
    chess_loaded=false;remove(".rockbox/sloppy/chess.dat");chess_page();frame(100);
    chess_start(0,1,1);frame(200);SFX_RESET();
    chess_tap_square(CH_SQ(4,1));assert(sfx_last==SFX_CHESS_SELECT);
    chess_tap_square(CH_SQ(4,3));assert(sfx_last==SFX_CHESS_MOVE);frame(800);
    chess_tap_square(CH_SQ(3,6));chess_tap_square(CH_SQ(3,4));frame(800);
    chess_tap_square(CH_SQ(4,3));chess_tap_square(CH_SQ(3,4));assert(sfx_last==SFX_CHESS_CAPTURE);
    lv_obj_send_event(chess_btn[CHESS_BTN_UNDO],LV_EVENT_CLICKED,NULL);assert(sfx_last==SFX_CHESS_UNDO);
    chess_start(0,1,1);frame(200);                                   /* fool's mate: f3 e5 g4 Qh4 */
    chess_tap_square(CH_SQ(5,1));chess_tap_square(CH_SQ(5,2));frame(800);
    chess_tap_square(CH_SQ(4,6));chess_tap_square(CH_SQ(4,4));frame(800);
    chess_tap_square(CH_SQ(6,1));chess_tap_square(CH_SQ(6,3));frame(800);
    SFX_RESET();chess_tap_square(CH_SQ(3,7));chess_tap_square(CH_SQ(7,3));
    assert(sl_chess_game_status(&chess)==CH_CHECKMATE && sfx_last==SFX_WIN);
    page_pop();frame(300);
    memset(&chess,0,sizeof chess);chess_loaded=false;remove(".rockbox/sloppy/chess.dat");
    /* Icon sheet: every drawn icon at list, tile and home sizes, on a tile
     * colour and on a card. */
    {
        lv_obj_t *sheet=box(lv_screen_active());lv_obj_set_size(sheet,W,H);
        lv_obj_set_style_bg_color(sheet,lv_color_hex(C_BG),0);lv_obj_set_style_bg_opa(sheet,LV_OPA_COVER,0);
        int n=IC_PLAYLISTS;
        for(int id=1;id<=n;id++){
            int row=(id-1)/4,col=(id-1)%4,x=col*120,y=SB_H+row*(H-SB_H)/5;
            lv_obj_t *t=box(sheet);lv_obj_set_pos(t,x+6,y+4);lv_obj_set_size(t,70,70);
            lv_obj_set_style_bg_color(t,lv_color_hex(0x2a8f86),0);lv_obj_set_style_bg_opa(t,LV_OPA_COVER,0);
            lv_obj_center(sl_icon(t,id,58,0xffffff,0x2a8f86));
            lv_obj_t *c=box(sheet);lv_obj_set_pos(c,x+78,y+4);lv_obj_set_size(c,36,36);
            lv_obj_set_style_radius(c,10,0);lv_obj_set_style_bg_color(c,lv_color_hex(0xc47d70),0);lv_obj_set_style_bg_opa(c,LV_OPA_COVER,0);
            lv_obj_center(sl_icon(c,id,26,0xffffff,0xc47d70));
            lv_obj_set_pos(sl_icon(sheet,id,34,C_TEXT,C_BG),x+80,y+44);
        }
        if(banner_obj)lv_obj_add_flag(banner_obj,LV_OBJ_FLAG_HIDDEN);
        shot("icons.ppm");lv_obj_delete(sheet);frame(50);
    }
    sounds_page();frame(200);
    assert(lv_slider_get_value(sounds_slider)==70);
    assert(!strcmp(lv_label_get_text(sounds_value),"70%"));
    lv_slider_set_value(sounds_slider,88,LV_ANIM_OFF);SFX_RESET();
    lv_obj_send_event(sounds_slider,LV_EVENT_VALUE_CHANGED,NULL);
    assert(global_settings.sloppy_sfx_volume==90 && sfx_last==-1);   /* no preview mid-drag */
    lv_obj_send_event(sounds_slider,LV_EVENT_RELEASED,NULL);assert(sfx_last==SFX_WIN);
    lv_slider_set_value(sounds_slider,0,LV_ANIM_OFF);lv_obj_send_event(sounds_slider,LV_EVENT_VALUE_CHANGED,NULL);
    assert(global_settings.sloppy_sfx_volume==0 && !strcmp(lv_label_get_text(sounds_value),"Off"));
    SFX_RESET();sfx_play(SFX_TAP);assert(sfx_last==-1);
    lv_slider_set_value(sounds_slider,70,LV_ANIM_OFF);lv_obj_send_event(sounds_slider,LV_EVENT_VALUE_CHANGED,NULL);
    assert(global_settings.sloppy_sfx_volume==70);
    lv_obj_t *ui_sw=sounds_ui_switch;assert(lv_obj_has_state(ui_sw,LV_STATE_CHECKED));
    lv_obj_remove_state(ui_sw,LV_STATE_CHECKED);lv_obj_send_event(ui_sw,LV_EVENT_VALUE_CHANGED,NULL);
    assert(global_settings.sloppy_ui_sounds==0);
    lv_obj_add_state(ui_sw,LV_STATE_CHECKED);SFX_RESET();lv_obj_send_event(ui_sw,LV_EVENT_VALUE_CHANGED,NULL);
    assert(global_settings.sloppy_ui_sounds==1 && sfx_last==SFX_UI_ON);
    shot("sounds.ppm");
    page_pop();frame(300);
    sl_library_page();frame(500);shot("library.ppm");page_pop();frame(500);
    sl_pet_init(&pet, pet_clock());pet_loaded=true;
    pet_page();frame(500);shot("pet-select.ppm");
    lv_obj_t *pet_select_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(lv_obj_get_child(pet_select_body,1),LV_EVENT_CLICKED,NULL);
    frame(500);assert(pet_pending_species==SL_PET_SLOP && pet_name_input);
    {   /* the system keyboard, typed on by touch: every key, and the gaps between them, land */
        struct kb *kb=kb_now();assert(kb);lv_area_t a;
        lv_obj_get_coords(kb->root,&a);assert(a.y2==H-1 && lv_obj_has_flag(kb->root,LV_OBJ_FLAG_USER_1) && kb->pal);
        lv_textarea_set_text(pet_name_input,"");kb_changed(kb);frame(50);
        assert(kb->shift==1);                                /* a name starts with a capital */
        kb_tap(kb,kb_find(kb,'p'));kb_tap(kb,kb_find(kb,'i'));kb_tap(kb,kb_find(kb,'p'));
        assert(!strcmp(lv_textarea_get_text(pet_name_input),"Pip"));
        lv_area_t ki,ko;lv_obj_get_coords(kb->k[kb_find(kb,'i')].o,&ki);lv_obj_get_coords(kb->k[kb_find(kb,'o')].o,&ko);
        int gx=(ki.x2+ko.x1)/2,gy=(ki.y1+ki.y2)/2;   /* between i and o */
        touch_at(gx,gy,true);touch_at(gx,gy,false);frame(40);
        assert(strlen(lv_textarea_get_text(pet_name_input))==4);
        lv_area_t k1;lv_obj_get_coords(kb->k[kb_find(kb,'a')].o,&k1);
        int vy=(ki.y2+k1.y1)/2;
        touch_at((ki.x1+ki.x2)/2,vy,true);touch_at((ki.x1+ki.x2)/2,vy,false);frame(40);  /* between the rows */
        assert(strlen(lv_textarea_get_text(pet_name_input))==5);
    }
    shot("pet-name.ppm");
    lv_textarea_set_text(pet_name_input,"Pip");
    SFX_RESET();pet_name_done_cb(NULL);frame(500);assert(sfx_last==SFX_PET_WIN);
    assert(pet.species==SL_PET_SLOP && !strcmp(pet.name,"Pip"));
    assert(pet_page_obj && objects(pages[npages-1])<180);shot("pet.ppm");
    /* a new pet is an egg: only the egg shows, care is refused, taps crack it open */
    assert(sl_pet_is_egg(&pet) && lv_obj_has_flag(pet_actions_obj,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(pet_choose_obj,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(lv_obj_get_parent(pet_stat_canvas),LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(pet_stage_label),"EGG") && !strcmp(lv_label_get_text(pet_tap_label),"TAP THE EGG TO HATCH IT"));
    { uint8_t f=pet.food;pet_begin_care(SL_PET_FEED,"",PET_FEEDING,1000);assert(!pet_care_locked && pet.food==f); }
    pet_tick();assert(sl_pet_is_egg(&pet));                  /* no hatching by itself */
    lv_obj_t *pet_bezel=lv_obj_get_parent(pet_hab_canvas);
    for(int i=1;i<PET_EGG_TAPS;i++){SFX_RESET();lv_obj_send_event(pet_bezel,LV_EVENT_CLICKED,NULL);
        assert(pet_egg_cracks==i && sfx_last==SFX_PET_POKE && sl_pet_is_egg(&pet));frame(300);}
    shot("pet-egg-cracking.ppm");assert(!strcmp(lv_label_get_text(pet_mood),"IT'S COMING OUT!"));
    SFX_RESET();lv_obj_send_event(pet_bezel,LV_EVENT_CLICKED,NULL);
    assert(!sl_pet_is_egg(&pet) && sfx_last==SFX_PET_HATCH && !pet_egg_cracks);
    assert(!lv_obj_has_flag(pet_actions_obj,LV_OBJ_FLAG_HIDDEN) && !strcmp(lv_label_get_text(pet_stage_label),"BABY"));
    frame(240);shot("pet-hatching.ppm");frame(1500);
    SFX_RESET();pet_refresh();pet_tick();shot("pet-puppy.ppm");
    assert(!strcmp(lv_label_get_text(pet_stage_label),"BABY"));
    uint8_t food_before=pet.food;
    lv_obj_t *pet_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(pet_body,2),0),LV_EVENT_CLICKED,NULL);
    assert(!pet_care_locked && pet.food==food_before);
    lv_obj_t *food_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(lv_obj_get_child(food_body,0),LV_EVENT_CLICKED,NULL);
    assert(pet_care_locked && pet.food==food_before);
    SFX_RESET();pet_care_started -= 1100; frame(50); assert(sfx_last==SFX_PET_CHOMP); shot("pet-eat-pizza.ppm");
    int locked_depth=npages;page_pop();assert(npages==locked_depth);
    SFX_RESET();pet_care_until=sl_ms()-1;pet_tick();
    assert(!pet_care_locked && pet.food>=food_before && pet.meals==1 && sfx_last==SFX_PET_DONE);
    lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(pet_body,2),1),LV_EVENT_CLICKED,NULL);
    assert(pet_care_locked); pet_care_started -= 800; frame(50); shot("pet-bath.ppm");
    pet_care_until=sl_ms()-1;pet_tick();
    assert(!pet_care_locked && pet.baths==1);
    lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(pet_body,2),3),LV_EVENT_CLICKED,NULL);
    assert(pet_care_locked && sfx_last==SFX_PET_LULLABY); pet_care_started -= 800; frame(50); shot("pet-sleep.ppm");
    pet_care_until=sl_ms()-1;pet_tick();
    assert(!pet_care_locked && pet.rests==1);
    /* Living habitat: roaming, thought bubbles, petting and night. */
    int roam_start=pet_hab_x;
    for(int i=0;i<60;i++)frame(80);
    assert(pet_hab_x!=roam_start || pet_hab_mode==PET_HAB_NAP);
    uint8_t food_saved=pet.food;pet.food=20;pet_hab_t=0;shot("pet-hungry.ppm");
    assert(pet_hab_need_icon()==pet_ic_pizza);
    pet.food=food_saved;pet.checksum=sl_pet_checksum(&pet);
    pet.stimulation=30;pet.checksum=sl_pet_checksum(&pet);
    uint8_t cuddle_joy=pet.joy;
    SFX_RESET();pet_screen_cb(NULL);assert(sfx_last==SFX_PET_CHIRP);
    assert(pet.stimulation==26 && pet.joy>=cuddle_joy && (int32_t)(lv_tick_get()-pet_hab_pet_until)<0);
    pet_screen_cb(NULL);assert(pet.stimulation==26); /* debounced */
    shot("pet-cuddle.ppm");
    lv_obj_scroll_to_y(pet_body,700,LV_ANIM_OFF);frame(100);shot("pet-actions.ppm");
    lv_obj_scroll_to_y(pet_body,0,LV_ANIM_OFF);frame(100);
    get_time()->tm_hour=22;shot("pet-night.ppm");get_time()->tm_hour=9;
    /* Music: dances by genre on the beat, loves its species' style and its
     * favorite artist, sways in the evening, dozes late, wonders when paused,
     * hums between songs, remembers what it heard, and pays it out. */
    pet.joy=60;pet.food=80;pet.clean=80;pet.energy=80;pet.checksum=sl_pet_checksum(&pet);
    test_status=AUDIO_STATUS_PLAY;strlcpy(test_track.path,"/Music/metal.mp3",sizeof test_track.path);
    test_track.genre_string="Heavy Metal";test_track.artist="Mira Sol";test_peak=30000;
    unsigned songs_before=pet.songs_heard;
    pet_mus.last=0;pet_music_poll();
    assert(pet_mus.playing && pet_music_dance()==PET_DANCE_HEADBANG && pet.songs_heard==songs_before+1);
    assert(pet_mus.loved && !pet_mus.favorite && !strcmp(pet_mus.artist,"Mira Sol")); /* Slop loves rock */
    pet_refresh();assert(!strcmp(lv_label_get_text(pet_mood),"OOH, NEW SONG!"));
    unsigned beats=pet_mus.beats;pet_mus.beat=lv_tick_get()-1000;pet_mus.last=0;pet_music_poll();
    assert(pet_mus.beats==beats+1 && pet_mus.level>0);
    /* Tempo tracking: a 120 BPM kick, then the song slows to 100 BPM. */
    pet_mus.interval=700;
    for(int ms=0;ms<10000;ms+=50){test_peak=ms%500<50?28000:5000;frame(50);pet_music_poll();}
    assert(abs(pet_mus.interval-500)<=40);
    for(int ms=0;ms<12000;ms+=50){test_peak=ms%600<50?28000:5000;frame(50);pet_music_poll();}
    assert(abs(pet_mus.interval-600)<=45);
    /* A kick on every other beat still reads as 120, not 60. */
    pet_mus.interval=500;
    for(int ms=0;ms<8000;ms+=50){test_peak=ms%1000<50?28000:5000;frame(50);pet_music_poll();}
    assert(abs(pet_mus.interval-500)<=40);
    test_peak=30000;
    pet_mus.song_at=lv_tick_get()-5000;pet_refresh();
    const char *music_mood=lv_label_get_text(pet_mood);
    assert(!strcmp(music_mood,"HEADBANGING!") || !strcmp(music_mood,"LOVES THIS SONG!"));
    pet_motion=PET_IDLE;
    for(int i=0;i<6;i++){pet_mus.beats++;pet_mus.beat=lv_tick_get()-(i==5?0:300);pet_mus.last=lv_tick_get();frame(80);}
    shot("pet-music-headbang.ppm");
    assert(pet_hab_mode==PET_HAB_GAZE);
    strlcpy(test_track.path,"/Music/disco.mp3",sizeof test_track.path);test_track.genre_string="Disco";
    test_track.artist="Various Artists";
    pet_mus.last=0;pet_music_poll();pet_refresh();
    assert(pet_music_dance()==PET_DANCE_GROOVE && !pet_mus.loved && !pet_mus.artist[0]);
    assert(!strcmp(lv_label_get_text(pet_mood),"OOH, NEW SONG!") && pet.songs_heard==songs_before+2);
    pet_mus.song_at=lv_tick_get()-5000;pet_refresh();assert(!strcmp(lv_label_get_text(pet_mood),"GROOVING"));
    for(int i=0;i<6;i++){pet_mus.beats++;pet_mus.beat=lv_tick_get()-(i==5?0:300);pet_mus.last=lv_tick_get();frame(80);}
    shot("pet-music-groove.ppm");
    /* A whole minute of listening is banked and remembered. */
    unsigned minutes_before=pet.music_minutes;
    pet_mus.listen_ms=59990;pet_mus.last=lv_tick_get()-100;pet_music_poll();
    assert(pet.music_minutes==minutes_before+1 && pet.bank_day>=1 && sl_pet_valid(&pet));
    /* Thirty minutes of Mira Sol make her the favorite. */
    for(int i=0;i<30;i++) sl_pet_listen(&pet,"Mira Sol",false,false);
    strlcpy(test_track.path,"/Music/metal-2.mp3",sizeof test_track.path);
    test_track.genre_string="Heavy Metal";test_track.artist="Mira Sol";
    pet_mus.last=0;pet_music_poll();pet_mus.song_at=lv_tick_get()-5000;pet_refresh();
    assert(pet_mus.favorite && !strcmp(lv_label_get_text(pet_mood),"FAVORITE! MIRA SOL"));
    for(int i=0;i<4;i++){pet_mus.beats++;pet_mus.beat=lv_tick_get()-(i==3?60:300);pet_mus.last=lv_tick_get();frame(80);}
    shot("pet-music-favorite.ppm");
    assert(strstr(lv_label_get_text(pet_note_obj),"FAVORITE ARTIST: MIRA SOL"));
    /* Music beats bedtime: it keeps dancing in the evening and at 1 AM,
     * even straight out of a nap. */
    get_time()->tm_hour=20;pet_refresh();
    assert(pet_music_dance()==PET_DANCE_HEADBANG);
    get_time()->tm_hour=1;pet_mus.favorite=false;pet_hab_mode=PET_HAB_NAP;pet_hab_until=lv_tick_get()+60000;
    pet_refresh();frame(80);
    assert(pet_music_dance()==PET_DANCE_HEADBANG && pet_hab_mode==PET_HAB_GAZE);
    const char *night_mood=lv_label_get_text(pet_mood);
    assert(!strcmp(night_mood,"HEADBANGING!") || !strcmp(night_mood,"LOVES THIS SONG!"));
    for(int i=0;i<4;i++){pet_mus.beats++;pet_mus.beat=lv_tick_get()-(i==3?60:300);pet_mus.last=lv_tick_get();frame(80);}
    shot("pet-music-night-dance.ppm");
    /* Only a spent pet dozes off to the music, and those minutes bank Energy. */
    uint8_t night_energy=pet.energy;pet.energy=15;pet.checksum=sl_pet_checksum(&pet);pet_refresh();
    assert(pet_music_dance()==PET_DANCE_LULLED && !strcmp(lv_label_get_text(pet_mood),"LULLED TO SLEEP"));
    for(int i=0;i<200 && pet_hab_mode!=PET_HAB_NAP;i++) frame(80);
    assert(pet_hab_mode==PET_HAB_NAP);shot("pet-music-lulled.ppm");
    uint16_t bank_night=pet.bank_night;
    pet_mus.listen_ms=59990;pet_mus.last=lv_tick_get()-100;pet_music_poll();
    assert(pet.bank_night>bank_night);
    pet.energy=night_energy;pet.checksum=sl_pet_checksum(&pet);
    get_time()->tm_hour=9;
    test_status|=AUDIO_STATUS_PAUSE;pet_mus.last=0;pet_music_poll();pet_refresh();
    assert(!pet_mus.playing && !strcmp(lv_label_get_text(pet_mood),"WHERE'D THE MUSIC GO?"));
    pet_hab_mode=PET_HAB_GAZE;frame(80);shot("pet-music-paused.ppm");
    test_status=0;pet_mus.last=0;pet_music_poll();pet_mus.paused_at=lv_tick_get()-61000;pet_refresh();
    assert(strcmp(lv_label_get_text(pet_mood),"WHERE'D THE MUSIC GO?"));
    /* With the music off it hums its favorite now and then. */
    /* start at the beginning of a humming spell, whatever the clock says */
    for(int i=0;i<80 && pet_music_humming();i++) frame(500);
    for(int i=0;i<80 && !pet_music_humming();i++) frame(500);
    assert(pet_music_humming());
    for(int i=0;i<12;i++) frame(80);
    pet_refresh();assert(!strcmp(lv_label_get_text(pet_mood),"HUMMING MIRA SOL"));
    shot("pet-music-humming.ppm");
    int music_joy,music_energy;
    pet.joy=60;pet.bank_day=12;pet.bank_night=5;pet.checksum=sl_pet_checksum(&pet);
    assert(sl_pet_music_claim(&pet,&music_joy,&music_energy)==15 && music_joy==4 && music_energy==2);
    assert(pet.bank_day==2 && pet.bank_night==0 && pet.joy==64);
    pet.bank_day=5;pet.checksum=sl_pet_checksum(&pet);pet_tick();assert(pet.joy==66 && pet.bank_day==0);
    /* Dance With Me: needs music, follows its tempo, judges each tap. */
    pet_game_menu();frame(300);
    SFX_RESET();pet_game_choose(PET_RHYTHM);assert(!pet_game_page_obj);
    shot("pet-games-dance.ppm");
    test_status=AUDIO_STATUS_PLAY;pet_mus.last=0;pet_mus.interval=500;
    pet_game_choose(PET_RHYTHM);frame(40);
    assert(pet_game_page_obj && pet_game_kind==PET_RHYTHM && pet_rhy_interval==500);
    assert((int32_t)(pet_rhy_hit(0)-lv_tick_get())>=3*500);
    uint8_t dance_joy=pet.joy;
    for(int i=0;i<PET_RHY_NOTES;i++){
        int late=i==3?150:i==5?400:0; /* good; missed, and that tap is early for the next */
        int32_t wait=(int32_t)(pet_rhy_hit(i)+late-lv_tick_get());
        if(wait>0) frame(wait);
        pet_game_tap(64,60);
        if(i==8){frame(1);shot("pet-dance-game.ppm");}
    }
    frame(200);frame(700);frame(100);
    assert(pet_rhy_perfect==13 && pet_rhy_good==2 && pet_rhy_judged[5]==3 && pet_game_finished && pet_game_won);
    assert(pet.joy>dance_joy);shot("pet-dance-won.ppm");
    pet_game_open(PET_RHYTHM);frame(40);
    for(int i=0;i<40 && !pet_game_finished;i++) frame(400);
    assert(pet_game_finished && !pet_game_won && pet_rhy_perfect+pet_rhy_good==0);
    page_pop();frame(500);
    test_status=0;test_track.genre_string=NULL;test_track.artist=NULL;test_peak=0;pet_mus.last=0;pet_music_poll();
    pet.stimulation=0;pet.checksum=sl_pet_checksum(&pet);
    pet_game_menu();frame(300);shot("pet-games.ppm");
    lv_obj_t *menu_body=lv_obj_get_child(pages[npages-1],1);
    assert(lv_obj_get_child_count(menu_body)==7);
    pet_game_open(PET_CATCH);frame(250);shot("pet-catch.ppm");
    assert(pet_game_timer && pet_game_canvas);
    uint8_t fun_before=pet.joy;
    SFX_RESET();frame(400);pet_game_tap(2,134);assert(pet_game_misses==1 && pet_game_hits==0 && sfx_last==SFX_PET_MISS);
    for(int i=0;i<5;i++) {
        frame(100);pet_game_tap(pet_bug_x/16,pet_bug_y/16);
        assert(pet_game_hits==i+1);
        if(i==1){pet_game_fx_at-=380;pet_game_float_until+=500;shot("pet-catch-hit.ppm");}
    }
    assert(pet_game_finished && pet_game_won && pet.joy>=fun_before && sfx_last==SFX_PET_WIN);
    assert(!lv_obj_has_flag(pet_game_again,LV_OBJ_FLAG_HIDDEN));
    page_pop();frame(500);assert(!pet_game_timer && !pet_game_page_obj);
    pet_game_menu();pet_game_open(PET_MEMORY);frame(300);
    pet_mem_phase_at=lv_tick_get()-100;shot("pet-memory.ppm");
    for(int r=0;r<3;r++) {
        pet_mem_phase_at-=100000;frame(40);assert(!pet_mem_demo);
        for(int i=0;i<3+r;i++) {
            if(!i){int tx,ty;pet_mem_tile(pet_game_pattern[0],&tx,&ty);pet_game_tap(tx+27,ty+18);}
            else {SFX_RESET();pet_mem_press(pet_game_pattern[i]);assert(sfx_last==(i<2+r||r<2?SFX_PET_GEM:SFX_PET_WIN));}
        }
        assert(pet_mem_round==(r<2?r+1:2));
    }
    assert(pet_game_finished && pet_game_won && pet.games==4);
    shot("pet-memory-win.ppm");
    page_pop();frame(500);
    pet_game_menu();pet_game_open(PET_CATCH);frame(200);
    for(int i=0;i<5;i++){frame(400);pet_game_tap(pet_bug_x/16,pet_bug_y/16);}
    assert(pet.manic && pet.stimulation>=80);
    lv_obj_send_event(pet_game_again,LV_EVENT_CLICKED,NULL);frame(300);
    assert(pet_game_page_obj && !pet_game_finished); /* wired pets keep playing */
    page_pop();frame(500);pet_refresh();pet_tick();shot("pet-manic.ppm");
    assert(strstr(lv_label_get_text(pet_mood),"MANIC"));
    lv_obj_t *care_actions=lv_obj_get_child(pet_body,2);
    lv_obj_send_event(lv_obj_get_child(care_actions,4),LV_EVENT_CLICKED,NULL);
    lv_obj_t *mania_body=lv_obj_get_child(pages[npages-1],1);
    assert(lv_obj_get_child_count(mania_body)==2+SL_PET_BUMP && find_label(mania_body,"DROP ACID"));   /* relax and five substances */
    shot("pet-substances.ppm");
    lv_obj_send_event(lv_obj_get_child(mania_body,2),LV_EVENT_CLICKED,NULL);
    assert(pet_care_locked && !pet.crisis && pet_pending_sub==SL_PET_SNORT);
    pet_care_until=sl_ms()-1;pet_tick();
    assert(pet.crisis && pet.manic);
    lv_obj_send_event(lv_obj_get_child(care_actions,4),LV_EVENT_CLICKED,NULL);
    mania_body=lv_obj_get_child(pages[npages-1],1);
    assert(lv_obj_get_child_count(mania_body)==4 && find_label(mania_body,"ONE MORE BUMP"));
    lv_obj_send_event(lv_obj_get_child(mania_body,2),LV_EVENT_CLICKED,NULL);
    assert(pet_care_locked); pet_care_started -= 1600; frame(50); shot("pet-hospital.ppm");
    pet_care_until=sl_ms()-1;pet_tick();
    assert(!pet.crisis && !pet.dead);   /* one of the hospital's five endings */
    if(pet.manic){sl_pet_care(&pet,SL_PET_CALM);sl_pet_care(&pet,SL_PET_CALM);pet.manic=0;pet.energy=90;pet.checksum=sl_pet_checksum(&pet);}
    pet.energy=90;pet.checksum=sl_pet_checksum(&pet);
    assert(!pet.manic && pet.energy==90);
    pet_tick();shot("pet-calm.ppm");
    pet_game_menu();pet_game_open(PET_CUPS);frame(200);
    pet_game_started=lv_tick_get()-800;shot("pet-cups-bounce.ppm");
    pet_game_started=lv_tick_get()+500-(PET_CUP_SHUFFLE_AT+PET_CUP_SWAP_MS*2+180);shot("pet-cups.ppm");
    assert(pet_cup_swooshed==3);
    pet_game_tap(64,80);assert(pet_cup_chosen<0); /* no guessing during the shuffle */
    pet_game_started-=10000;frame(40);
    pet_game_tap(pet_cup_slot_x(pet_cup_pos[pet_game_target]),80);
    assert(pet_game_finished && pet_game_won && pet_cup_chosen==pet_game_target);
    pet_cup_reveal_at-=100;shot("pet-cups-win.ppm");
    lv_obj_send_event(pet_game_again,LV_EVENT_CLICKED,NULL);frame(300);
    assert(pet_game_page_obj && pet_game_timer && !pet_game_finished);
    pet_game_started-=10000;frame(40);
    int wrong_cup=(pet_game_target+1)%3;
    pet_game_tap(pet_cup_slot_x(pet_cup_pos[wrong_cup]),80);
    assert(pet_game_finished && !pet_game_won);
    pet_cup_reveal_at-=200;shot("pet-cups-miss.ppm");
    page_pop();frame(300);
    pet_game_menu();pet_game_open(PET_HEADING);frame(300);shot("pet-heading.ppm");
    for(int r=0;r<PET_HEAD_ROUNDS;r++) {
        int dir=r<3?pet_head_dir:-pet_head_dir;
        SFX_RESET();lv_obj_send_event(pet_game_pads[dir>0],LV_EVENT_CLICKED,NULL);
        assert(pet_head_guess==dir && sfx_last==(r<3?SFX_PET_YES:SFX_PET_NOPE));
        if(r==0)shot("pet-heading-turn.ppm");
        if(r==3)shot("pet-heading-nope.ppm");
        pet_head_turn_at-=2000;frame(40);
    }
    assert(pet_game_finished && pet_game_won && pet_head_score==3);
    shot("pet-heading-win.ppm");
    page_pop();frame(300);
    pet_game_menu();frame(300);
    lv_obj_t *games_body=lv_obj_get_child(pages[npages-1],1);
    SFX_RESET();lv_obj_send_event(lv_obj_get_child(games_body,6),LV_EVENT_CLICKED,NULL);
    assert(pet_care_locked && sfx_last==SFX_PET_BEAT);
    pet_care_started -= 1800; frame(50); shot("pet-club.ppm");
    uint8_t club_fun=pet.joy;
    pet_care_until=sl_ms()-1;pet_tick();
    assert(!pet_care_locked && pet.joy>=club_fun);
    /* An exhausted manic pet can still play, and can collapse mid-game. */
    pet.manic=1;pet.stimulation=90;pet.energy=8;pet.checksum=sl_pet_checksum(&pet);
    pet_game_menu();pet_game_open(PET_CATCH);frame(400);
    for(int i=0;i<3;i++){frame(100);pet_game_tap(2,134);}
    assert(pet_game_finished && pet.dead && sfx_last==SFX_PET_RIP);
    assert(lv_obj_has_flag(pet_game_again,LV_OBJ_FLAG_HIDDEN));
    shot("pet-game-collapse.ppm");
    page_pop();frame(500);
    assert(!lv_obj_has_flag(pet_choose_obj,LV_OBJ_FLAG_HIDDEN));
    pet.dead=0;pet.checksum=sl_pet_checksum(&pet);pet_refresh();
    /* Partying while overwhelmed on an empty tank is fatal. */
    pet.manic=1;pet.crisis=1;pet.stimulation=95;pet.energy=20;pet.checksum=sl_pet_checksum(&pet);
    pet_game_menu();frame(300);
    games_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(lv_obj_get_child(games_body,6),LV_EVENT_CLICKED,NULL);
    assert(pet_care_locked);
    pet_care_until=sl_ms()-1;pet_tick();
    assert(pet.dead && pet.energy==0);
    pet_refresh();frame(100);
    assert(!lv_obj_has_flag(pet_choose_obj,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(pet_actions_obj,LV_OBJ_FLAG_HIDDEN));
    shot("pet-ghost.ppm");
    lv_obj_scroll_to_y(pet_body,700,LV_ANIM_OFF);frame(100);shot("pet-ghost-button.ppm");
    /* poke it with a stick: an animation, and it stays dead */
    lv_obj_send_event(lv_obj_get_child(pet_choose_obj,0),LV_EVENT_CLICKED,NULL);
    assert(pet_care_locked && pet_motion==PET_POKE && pet_pending_poke);
    pet_care_started-=700;frame(50);shot("pet-poke-1.ppm");
    pet_care_started-=1700;frame(50);shot("pet-poke-2.ppm");
    pet_care_started-=1200;frame(50);shot("pet-poke-ghost.ppm");
    pet_care_until=sl_ms()-1;pet_tick();frame(200);
    assert(pet.dead && !pet_care_locked && !pet_pending_poke && pet_page_obj);
    assert(!lv_obj_has_flag(pet_choose_obj,LV_OBJ_FLAG_HIDDEN));
    lv_obj_scroll_to_y(pet_body,0,LV_ANIM_OFF);frame(100);
    lv_obj_send_event(pet_choose_btn,LV_EVENT_CLICKED,NULL);
    frame(300);
    lv_obj_t *reselect_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(lv_obj_get_child(reselect_body,3),LV_EVENT_CLICKED,NULL);
    frame(300);
    assert(pet_pending_species==SL_PET_SPAGHET);
    pet_name_done_cb(NULL);frame(500);
    assert(pet.species==SL_PET_SPAGHET && !strcmp(pet.name,"Spaghet") && pet.games==0);
    assert(sl_pet_is_egg(&pet));sl_pet_hatch(&pet,pet_clock());
    pet_refresh();pet_tick();shot("pet-spaghet.ppm");
    pet.dead=1;pet.checksum=sl_pet_checksum(&pet);pet_refresh();shot("pet-dead.ppm");
    assert(strstr(lv_label_get_text(pet_mood),"GOODBYE"));
    pet.dead=0;pet.checksum=sl_pet_checksum(&pet);pet_refresh();
    page_pop();frame(500);assert(!pet_page_obj);
    play_settings_page();frame(500);assert(xf_graph && xf_graph_timer);
    shot("crossfade-visualizer.ppm");
    lv_slider_set_value(xf_slider,9,LV_ANIM_OFF);
    lv_obj_send_event(xf_slider,LV_EVENT_VALUE_CHANGED,NULL);
    for(int i=0;i<20;i++)frame(40);                 /* the preview eases to the new length */
    assert(xf_shown==900 && !strcmp(lv_label_get_text(xf_val),"9 s"));
    shot("crossfade-nine-seconds.ppm");
    lv_slider_set_value(xf_slider,0,LV_ANIM_OFF);
    lv_obj_send_event(xf_slider,LV_EVENT_VALUE_CHANGED,NULL);
    for(int i=0;i<25;i++)frame(40);
    assert(xf_shown==0);shot("crossfade-off.ppm");
    page_pop();frame(500);assert(!xf_graph_timer);
    ragdoll_page();frame(500);assert(objects(pages[npages-1]) < 100);shot("ragdoll.ppm");
    for(int i=0;i<100;i++) rd_step();
    for(int i=0;i<RD_N;i++) assert(rd_nodes[i].x>=15 && rd_nodes[i].y<=359);
    rd_nodes[0].x=27;
    lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages-1],"Reset ragdoll")),LV_EVENT_CLICKED,NULL);
    assert(rd_nodes[0].x==180);
    {   /* Knives: a round under the same timer, drag, cuts, blood, death, score, back */
        lv_obj_t *pg=pages[npages-1];lv_timer_t *timer=rd_timer;int objs=objects(pg);
        static float free_run[RD_N][2];
        rd_reset();for(int i=0;i<100;i++)rd_step();
        for(int i=0;i<RD_N;i++){free_run[i][0]=rd_nodes[i].x;free_run[i][1]=rd_nodes[i].y;}
        lv_obj_send_event(lv_obj_get_parent(find_label(pg,"Knives")),LV_EVENT_CLICKED,NULL);
        assert(kn_on&&lv_obj_has_flag(rd_lines[0],LV_OBJ_FLAG_HIDDEN)&&!lv_obj_has_flag(kn_score_l,LV_OBJ_FLAG_HIDDEN));
        assert(find_label(pg,"Restart")&&find_label(pg,"Free play")&&rd_timer==timer&&objects(pg)==objs);
        kn_seed=7;
        for(int i=0;i<36;i++)frame(40);
        for(int k=0;k<KN_MAX;k++)assert(!kn[k].live);      /* a quiet first moment */
        for(int i=0;i<4;i++)frame(40);
        int live=0;for(int k=0;k<KN_MAX;k++)live+=kn[k].live;assert(live==1);
        /* the clock, not the frame rate, sets the pace; a stall is dropped */
        kn_start();kn_next=1000000;kn_t=0;
        for(int i=0;i<10;i++)frame(100);
        assert(kn_t==1000);
        for(int i=0;i<50;i++)frame(20);
        assert(kn_t==2000);
        frame(3000);assert(kn_t==2100);
        /* grab and drag, end to end, as in free play: the head follows the
         * finger, the body hangs off it, and a flick tosses the doll */
        lv_area_t sa;lv_obj_get_coords(rd_scene,&sa);
        for(int i=0;i<40;i++)frame(40);
        int fx=sa.x1+(int)rd_nodes[0].x,fy=sa.y1+(int)rd_nodes[0].y;
        touch_at(fx,fy,true);assert(rd_grab==0);
        for(int i=1;i<=10;i++)touch_at(fx+i*15,fy-i*16,true);
        for(int i=0;i<20;i++)touch_at(fx+150,fy-160,true);
        assert(fabsf(sa.x1+rd_nodes[0].x-(fx+150))<1&&fabsf(sa.y1+rd_nodes[0].y-(fy-160))<1&&rd_nodes[6].y<rd_nodes[1].y+90);
        shot("ragdoll-knives-drag.ppm");
        touch_at(fx+150,fy-160,false);assert(rd_grab<0);
        for(int i=0;i<40;i++)frame(40);
        assert(rd_nodes[0].y>sa.y2-sa.y1-120);              /* let go, it drops */
        /* the cog: opens the control menu, which pauses play, and picks */
        assert(!lv_obj_has_flag(kn_cog,LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(kn_menu,LV_OBJ_FLAG_HIDDEN)&&kn_ctrl==KN_GRAB);
        lv_obj_send_event(kn_cog,LV_EVENT_CLICKED,NULL);
        assert(!lv_obj_has_flag(kn_menu,LV_OBJ_FLAG_HIDDEN));
        uint32_t paused=kn_t;for(int i=0;i<20;i++)frame(40);assert(kn_t==paused);
        shot("ragdoll-knives-menu.ppm");
        touch_at(sa.x1+60,sa.y2-40,true);touch_at(sa.x1+60,sa.y2-40,false);   /* a tap beside it closes it */
        assert(lv_obj_has_flag(kn_menu,LV_OBJ_FLAG_HIDDEN)&&rd_grab<0);
        frame(40);assert(kn_t>paused);
        lv_obj_send_event(kn_cog,LV_EVENT_CLICKED,NULL);
        lv_obj_send_event(lv_obj_get_parent(find_label(pg,"Pull on a string")),LV_EVENT_CLICKED,NULL);
        assert(kn_ctrl==KN_STRING&&lv_obj_has_flag(kn_menu,LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(kn_pad,LV_OBJ_FLAG_HIDDEN));
        assert(find_label(pg,"Drag anywhere in the box to pull your doll on a string"));
        /* the string, end to end: from low in the box, right, then up; the
         * doll swings after the finger and hangs where it is held */
        kn_start();kn_next=1000000;for(int i=0;i<40;i++)frame(40);
        fx=sa.x1+60;fy=sa.y2-20;float x0=rd_nodes[0].x,y0=rd_nodes[0].y;
        touch_at(fx,fy,true);assert(kn_drag&&rd_grab<0);x0=kn_tx;y0=kn_ty;
        for(int i=1;i<=10;i++)touch_at(fx+i*15,fy,true);
        for(int i=0;i<30;i++)touch_at(fx+150,fy,true);
        assert(fabsf(kn_tx-(x0+150))<1&&rd_nodes[0].x>x0+110);
        for(int i=1;i<=10;i++)touch_at(fx+150,fy-i*16,true);
        for(int i=0;i<40;i++)touch_at(fx+150,fy-160,true);
        assert(rd_nodes[0].y<y0-110&&fabsf(rd_nodes[0].y-kn_ty)<30&&fabsf(rd_nodes[0].x-kn_tx)<30);
        shot("ragdoll-knives-string.ppm");
        touch_at(fx+150,fy-160,false);assert(!kn_drag);
        /* the pad, end to end: hold right, then up, then the top-left corner */
        lv_obj_send_event(kn_cog,LV_EVENT_CLICKED,NULL);
        lv_obj_send_event(lv_obj_get_parent(find_label(pg,"Arrow pad")),LV_EVENT_CLICKED,NULL);
        assert(kn_ctrl==KN_PAD&&!lv_obj_has_flag(kn_pad,LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(rd_hint,LV_OBJ_FLAG_HIDDEN));
        kn_start();kn_next=1000000;for(int i=0;i<40;i++)frame(40);
        lv_area_t pa;lv_obj_get_coords(kn_pad,&pa);int cy=(pa.y1+pa.y2)/2,cx=(pa.x1+pa.x2)/2;
        x0=rd_nodes[0].x;
        touch_at(cx,cy,true);touch_at(cx,cy,false);
        for(int i=0;i<25;i++)touch_at(pa.x2-30,cy,true);
        assert(kn_dir==KN_R&&rd_nodes[0].x>x0+60);
        touch_at(pa.x2-30,cy,false);assert(kn_dir==0);
        y0=rd_nodes[0].y;
        for(int i=0;i<25;i++)touch_at(cx,pa.y1+10,true);
        assert(kn_dir==KN_U&&rd_nodes[0].y<y0-60);
        shot("ragdoll-knives-pad.ppm");
        touch_at(pa.x1+30,pa.y1+6,true);assert(kn_dir==(KN_L|KN_U));
        touch_at(pa.x1+30,cy,true);assert(kn_dir==KN_L);
        touch_at(cx,pa.y2-6,true);assert(kn_dir==KN_D);
        touch_at(cx,pa.y2-6,false);assert(kn_dir==0);
        /* touching the box does nothing with the pad picked */
        touch_at(sa.x1+(int)rd_nodes[0].x,sa.y1+(int)rd_nodes[0].y,true);assert(rd_grab<0&&!kn_drag);
        touch_at(sa.x1+(int)rd_nodes[0].x,sa.y1+(int)rd_nodes[0].y,false);
        lv_obj_send_event(kn_cog,LV_EVENT_CLICKED,NULL);
        lv_obj_send_event(lv_obj_get_parent(find_label(pg,"Grab a limb")),LV_EVENT_CLICKED,NULL);
        assert(kn_ctrl==KN_GRAB&&lv_obj_has_flag(kn_pad,LV_OBJ_FLAG_HIDDEN)&&!lv_obj_has_flag(rd_hint,LV_OBJ_FLAG_HIDDEN));
        /* a knife through a strip it swept, not only where it stopped */
        rd_reset();float tt;
        rd_nodes[0].x=100;rd_nodes[0].y=200;rd_nodes[1].x=140;rd_nodes[1].y=200;
        assert(kn_sweep_d2(0,1,120,150,260,&tt)==0&&tt==.5f&&kn_sweep_d2(0,1,120,150,180,&tt)==400);
        /* a fast knife takes half an arm: the stump stays and bleeds, the
         * rest falls off and lies there */
        kn_start();kn_next=1000000;              /* standing: the forearm is clear */
        float ax=rd_nodes[2].x+(rd_nodes[3].x-rd_nodes[2].x)*.4f,ay=rd_nodes[2].y+(rd_nodes[3].y-rd_nodes[2].y)*.4f;
        kn[0].x=ax;kn[0].y=ay-50;kn[0].vy=100;kn[0].live=true;
        kn_step();
        unsigned on=kn_attached();
        assert(kn[0].spent&&!kn_dead&&rd_count==RD_N+2&&rd_ecount==RD_E+1&&!(on>>3&1)&&(on>>2&1)&&(on>>1&1));
        assert(rd_len[2]>8&&rd_len[2]<40&&fabsf(rd_len[2]+rd_len[RD_E]-47)<.01f&&rd_kind[RD_E]==2&&kn_bloody==1);
        assert(kn_wounds==(1u<<RD_N|1u<<(RD_N+1)));
        int drops=0;for(int i=0;i<KN_DROPS;i++)drops+=kn_drop[i].live;assert(drops>=10);
        for(int i=0;i<60;i++)kn_step();
        assert(kn_nstain>0&&rd_nodes[3].y<=358&&rd_nodes[3].x>=16);
        float piece=hypotf(rd_nodes[3].x-rd_nodes[RD_N+1].x,rd_nodes[3].y-rd_nodes[RD_N+1].y);
        assert(fabsf(piece-rd_len[RD_E])<2);                /* the piece keeps its length */
        /* a dodged knife scores when it sticks, then goes */
        kn[1].x=rd_nodes[0].x<200?W-60:40;kn[1].y=KN_FLOOR-4;kn[1].vy=6;kn[1].live=true;
        kn_step();assert(kn_score==1&&kn[1].stuck);
        for(int i=0;i<25;i++)kn_step();
        assert(!kn[1].live);
        /* through the head: off it comes, in slow motion, then the score */
        kn[2].x=rd_nodes[0].x;kn[2].y=rd_nodes[0].y-20;kn[2].vy=6;kn[2].live=true;
        for(int i=0;i<6&&!kn_dead;i++)kn_step();
        assert(kn_dead&&kn_slow==.25f&&!(kn_attached()&1)&&!strcmp(lv_label_get_text(rd_face),"x(")&&kn_flash>50);
        assert(kn_best==1&&rd_grab<0);
        /* the cog's menu holds the slow motion and the score screen too */
        lv_obj_send_event(kn_cog,LV_EVENT_CLICKED,NULL);
        for(int i=0;i<60;i++)frame(40);
        assert(!kn_over&&kn_slow==.25f);
        lv_obj_send_event(kn_cog,LV_EVENT_CLICKED,NULL);
        for(int i=0;i<10;i++)frame(40);
        assert(!kn_over&&lv_obj_has_flag(kn_over_box,LV_OBJ_FLAG_HIDDEN));
        shot("ragdoll-knives-death.ppm");
        for(int i=0;i<30;i++)frame(40);
        assert(kn_over&&kn_slow==1.f&&!lv_obj_has_flag(kn_over_box,LV_OBJ_FLAG_HIDDEN));
        assert(find_label(pg,"You dodged 1")&&find_label(pg,"Butterfingers. Best: 1\nTap to play again"));
        shot("ragdoll-knives-over.ppm");
        /* tap the arena: straight into a new round, clean */
        touch_at((sa.x1+sa.x2)/2,(sa.y1+sa.y2)/2,true);touch_at((sa.x1+sa.x2)/2,(sa.y1+sa.y2)/2,false);
        assert(!kn_dead&&!kn_over&&kn_score==0&&rd_count==RD_N&&rd_ecount==RD_E&&!kn_nstain&&!kn_wounds);
        assert(lv_obj_has_flag(kn_over_box,LV_OBJ_FLAG_HIDDEN));
        /* left alone, the knives get it sooner or later; nothing leaks */
        kn_seed=11;int max_live=0;uint32_t first=0;
        for(int i=0;i<3000&&!kn_over;i++){
            frame(40);live=0;
            for(int k=0;k<KN_MAX;k++)if(kn[k].live){live++;assert(kn[k].y<=KN_FLOOR&&kn[k].x>=24&&kn[k].x<=W-52);}
            for(int n=0;n<rd_count;n++)assert(rd_nodes[n].x>=16&&rd_nodes[n].x<=W-44&&rd_nodes[n].y<=358);
            assert(rd_count<=RD_NMAX&&rd_ecount<=RD_EMAX);
            if(live&&!first)first=kn_t;
            if(live>max_live)max_live=live;
        }
        printf("knives idle: first at %u ms, died after %u ms, dodged %d, at most %d in the air, %d joints\n",first,kn_dead_at?kn_t:0,kn_score,max_live,rd_count);
        assert(kn_over&&first>=1500&&max_live<KN_MAX);
        /* Restart, over and over: one timer, the same objects, a clean arena */
        for(int i=0;i<5;i++){lv_obj_send_event(rd_reset_row,LV_EVENT_CLICKED,NULL);frame(40);}
        live=0;for(int k=0;k<KN_MAX;k++)live+=kn[k].live;
        drops=0;for(int i=0;i<KN_DROPS;i++)drops+=kn_drop[i].live;
        assert(!live&&!drops&&rd_timer==timer&&objects(pg)==objs&&!kn_dead&&kn_score==0);
        /* drawing only what moved leaves nothing behind: after a stretch
         * of partial frames, redrawing the whole box changes no pixel */
        {bool checked=false;
         for(int seed=1;seed<10&&!checked;seed++){
            kn_start();kn_seed=seed;kn_t=kn_next=25000;
            for(int i=0;i<30&&!kn_dead;i++)frame(40);
            if(!kn_dead){kn_split(3,.5f,1,-2);kn_splash(rd_nodes[1].x,rd_nodes[1].y,30,2);}
            for(int i=0;i<32&&!kn_dead;i++)frame(40);
            int drops=0;for(int i=0;i<KN_DROPS;i++)drops+=kn_drop[i].live;
            int live=0;for(int k=0;k<KN_MAX;k++)live+=kn[k].live;
            if(kn_dead||kn_shake>.5f||live<3||!kn_nstain||!drops)continue;
            static uint16_t before[W*H];memcpy(before,pixels,sizeof pixels);
            lv_obj_invalidate(rd_scene);lv_refr_now(disp);
            lv_area_t sb;lv_obj_get_coords(rd_scene,&sb);int bad=0;
            for(int y=sb.y1;y<=sb.y2;y++)for(int x=sb.x1;x<=sb.x2;x++)bad+=before[y*W+x]!=pixels[y*W+x];
            printf("knives partial redraw: %d stale pixels with %d knives, %d drops, %d stains (seed %d)\n",bad,live,drops,kn_nstain,seed);
            assert(!bad);checked=true;}
         assert(checked);}
        lv_obj_send_event(rd_reset_row,LV_EVENT_CLICKED,NULL);frame(40);
        /* a busy, bloody round, for the picture */
        kn_seed=3;kn_t=38000;kn_next=38000;
        {struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);for(int i=0;i<40;i++)frame(40);clock_gettime(CLOCK_MONOTONIC,&t1);
         printf("knives frame: %ld us\n",((t1.tv_sec-t0.tv_sec)*1000000000L+t1.tv_nsec-t0.tv_nsec)/40000);}
        if(!kn_dead){kn[3].x=rd_nodes[8].x+(rd_nodes[9].x-rd_nodes[8].x)*.5f;kn[3].y=rd_nodes[8].y-30;kn[3].vy=40;kn[3].live=true;kn[3].spent=false;}
        frame(40);frame(40);frame(40);
        shot("ragdoll-knives.ppm");
        {struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);for(int i=0;i<20;i++)frame(40);clock_gettime(CLOCK_MONOTONIC,&t1);
         printf("knives frame, bleeding: %ld us\n",((t1.tv_sec-t0.tv_sec)*1000000000L+t1.tv_nsec-t0.tv_nsec)/20000);}
        /* back to free play: no knives, the doll's own lines, the same physics */
        lv_obj_send_event(lv_obj_get_parent(find_label(pg,"Free play")),LV_EVENT_CLICKED,NULL);
        live=0;for(int k=0;k<KN_MAX;k++)live+=kn[k].live;
        assert(!kn_on&&!live&&!kn_nstain&&!lv_obj_has_flag(rd_lines[0],LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(kn_score_l,LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(kn_cog,LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(kn_pad,LV_OBJ_FLAG_HIDDEN));
        assert(find_label(pg,"Reset ragdoll")&&find_label(pg,"Knives")&&rd_count==RD_N&&rd_ecount==RD_E&&rd_grav==.28f);
        rd_reset();for(int i=0;i<100;i++)rd_step();
        for(int i=0;i<RD_N;i++)assert(rd_nodes[i].x==free_run[i][0]&&rd_nodes[i].y==free_run[i][1]);
        /* closing the page mid-round leaves nothing running */
        lv_obj_send_event(lv_obj_get_parent(find_label(pg,"Knives")),LV_EVENT_CLICKED,NULL);
        frame(40);
    }
    page_pop();frame(500);
    assert(!rd_timer&&!kn_on&&!rd_page_obj);
    ragdoll_page();frame(100);assert(!kn_on&&!lv_obj_has_flag(rd_lines[0],LV_OBJ_FLAG_HIDDEN));
    page_pop();frame(500);
    appearance_page();frame(500);shot_now("settings-customization.ppm");
    settings_icon_page();frame(500);shot("icon-previews.ppm");
    {   /* a second copy opened and closed must not leave the first one's taps
         * writing to freed rows (found by the fuzzer) */
        lv_obj_t *first=pages[npages-1];
        settings_icon_page();frame(500);page_pop();frame(500);assert(pages[npages-1]==first);
        lv_obj_send_event(lv_obj_get_parent(find_label(first,"Classic cog")),LV_EVENT_CLICKED,NULL);
        lv_obj_t *cog=lv_obj_get_parent(find_label(first,"Classic cog"));   /* the row: its tick (child 1) shows */
        assert(!global_settings.sloppy_skull_icon&&!lv_obj_has_flag(lv_obj_get_child(cog,1),LV_OBJ_FLAG_HIDDEN));
    }
    lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages-1],"Angular skull")),LV_EVENT_CLICKED,NULL);
    assert(global_settings.sloppy_skull_icon);
    shot("skull-selected.ppm");page_pop();frame(500);page_pop();frame(500);shot("home-skull.ppm");
    const char *old_artist=sl_lib.albums[0].artist;
    const char *old_title5=sl_lib.tracks[5].title, *old_title6=sl_lib.tracks[6].title;
    sl_lib.albums[0].artist="שולה חן";
    sl_lib.tracks[5].title="רגעים";
    sl_lib.tracks[6].title="יום חדש - New Day";
    songs_page();frame(500);lv_obj_t *body=lv_obj_get_child(pages[npages-1],1);
    assert(lv_bidi_detect_base_dir("רגעים")==LV_BASE_DIR_RTL);
    char visual_hebrew[96];
    lv_bidi_process("יום חדש - New Day",visual_hebrew,LV_BASE_DIR_AUTO);
    assert(!strncmp(visual_hebrew,"New Day",7));
    lv_obj_t *hebrew_host=lv_obj_get_child(body,1);   /* after the count */
    lv_obj_t *hebrew_row=lv_obj_get_child(hebrew_host,5);
    assert(lv_obj_get_style_base_dir(lv_obj_get_child(hebrew_row,1),0)==LV_BASE_DIR_AUTO);
    shot("hebrew-songs.ppm");
    int before=objects(pages[npages-1]);assert(before<200);shot("songs.ppm");
    {   /* the song playing: its title in the accent and bars over its cover; a new song moves the mark */
        int t0=(int)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(hebrew_host,0));
        int t1=(int)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(hebrew_host,1)),saved=np_love_track;
        #define ROW_INK(t) ({ uint32_t ink=0; for(unsigned i=0;i<lv_obj_get_child_count(hebrew_host);i++){ lv_obj_t *r=lv_obj_get_child(hebrew_host,i); \
            if((int)(intptr_t)lv_obj_get_user_data(r)!=(t))continue; for(unsigned j=0;j<lv_obj_get_child_count(r);j++){ lv_obj_t *l=lv_obj_get_child(r,j); \
            if(lv_obj_check_type(l,&lv_label_class)&&!strcmp(lv_label_get_text(l),sl_lib.tracks[t].title)) ink=lv_color_to_u32(lv_obj_get_style_text_color(l,0)); } } ink; })
        uint32_t accent=lv_color_to_u32(lv_color_hex(C_ACCENT));
        assert(t0!=t1 && ROW_INK(t0)!=accent);
        np_love_track=t0;song_lists_mark(-1,t0);frame(200);assert(ROW_INK(t0)==accent && objects(pages[npages-1])<=before+8);
        shot("songs-playing.ppm");
        np_love_track=t1;song_lists_mark(t0,t1);frame(200);assert(ROW_INK(t1)==accent && ROW_INK(t0)!=accent && ROW_INK(t0));
        /* the next song comes with Now Playing shut: the mark goes to it, and the picker's "Go to current song" with it */
        {   int st=test_status;char p0[sizeof test_track.path];strlcpy(p0,test_track.path,sizeof p0);assert(!np_open);
            test_status=AUDIO_STATUS_PLAY;strlcpy(test_track.path,TPT(&sl_lib.tracks[t0]),sizeof test_track.path);
            periodic_ui();frame(200);assert(np_love_track==t0&&ROW_INK(t0)==accent&&ROW_INK(t1)!=accent&&pm_now()==t0);
            test_status=st;strlcpy(test_track.path,p0,sizeof test_track.path);np_love_track=t1;song_lists_mark(t0,t1);frame(100);}
        np_love_track=saved;song_lists_mark(t1,saved);frame(100);
        #undef ROW_INK
    }
    for(int i=0;i<10000;i+=127){lv_obj_scroll_to_y(body,i*82,LV_ANIM_OFF);frame(20);assert(objects(pages[npages-1])<=before);}
    lv_obj_scroll_to_y(body,9999*82,LV_ANIM_OFF);frame(20);
    lv_obj_t *host=lv_obj_get_child(body,1);bool found=false;
    for(unsigned i=0;i<lv_obj_get_child_count(host);i++) {
        lv_obj_t *r=lv_obj_get_child(host,i);
        if((intptr_t)lv_obj_get_user_data(r)==9999){lv_obj_send_event(r,LV_EVENT_CLICKED,NULL);found=true;}
    }
    assert(found && started==9999 && queued==10000);
    { /* Up Next over a 10,000-song queue reads file names only for its rows */
        int was_started=started,was_repeat=global_settings.repeat_mode;
        started=0;global_settings.repeat_mode=REPEAT_ALL;
        long reads=track_info_reads;uq_show();
        assert(uq_next_count==UQ_FIRST && track_info_reads-reads<=1+uq_queue_count+UQ_FIRST+4);   /* opens with a screenful */
        for(int i=0;i<100&&uq_next_count<60;i++)frame(100);   /* the rest while it sits still, 2 a tick */
        assert(uq_next_count==60 && track_info_reads-reads<=1+uq_queue_count+uq_next_count+4+(60-UQ_FIRST)/2);
        /* a song change rebuilds it: a screenful again, not all 60 (the R1 froze ~600 ms) */
        lv_obj_scroll_to_y(uq_list,0,LV_ANIM_OFF);reads=track_info_reads;uq_build();
        assert(uq_next_count==UQ_FIRST && track_info_reads-reads<=1+uq_queue_count+UQ_FIRST+4);
        for(int i=0;i<100&&uq_next_count<60;i++)frame(100);
        lv_obj_scroll_to_y(uq_list,20*UQ_ROW_H,LV_ANIM_OFF);int at=lv_obj_get_scroll_y(uq_list);uq_build();   /* scrolled down: the place is kept */
        assert(uq_next_count==UQ_FIRST+at/UQ_ROW_H && lv_obj_get_scroll_y(uq_list)==at);
        uq_close();frame(300);started=was_started;global_settings.repeat_mode=was_repeat;
    }
    started=-1;insert_fail=2;assert(!play_list(sl_lib.songs_by_title,100,0,false));
    assert(started==-1 && !test_status && q_kept_n==0);insert_fail=-1;
    test_status=AUDIO_STATUS_PLAY;create_fail=true;
    assert(sl_new_list(NULL)<0 && !test_status && q_kept_n==0);
    create_fail=false;
    assert(!play_list(sl_lib.songs_by_title,100,-1,false));
    page_pop();frame(500);
    sl_lib.albums[0].artist=old_artist;
    sl_lib.tracks[5].title=old_title5;sl_lib.tracks[6].title=old_title6;
    for(int i=0;i<50;i++){songs_page();frame(500);page_pop();frame(500);}
    toast("First");lv_timer_t *timer=nb.timer;toast("Second");assert(timer==nb.timer);
    np_img_buf=calloc(W*W,2);aa_slot=0;test_bitmap.width=W+1;test_bitmap.height=W;test_bitmap.data=(void*)1;
    test_status=AUDIO_STATUS_PLAY;sl_track_path(0,test_track.path,sizeof test_track.path);test_track.title="Blue Hours";test_track.artist="Mira Sol";test_track.album="Blue Hours";test_track.length=5*60*60*1000UL;test_track.elapsed=4*60*60*1000UL;
    np_update(true);assert(lv_obj_has_flag(np_art_img,LV_OBJ_FLAG_HIDDEN));assert(lv_slider_get_value(np_seek)==800);
    {   /* A skip while scrubbing must not seek the new song. */
        int seeks=audio_seeks;
        lv_obj_send_event(np_seek,LV_EVENT_PRESSED,NULL);
        sl_track_path(1,test_track.path,sizeof test_track.path);
        lv_obj_send_event(np_seek,LV_EVENT_RELEASED,NULL);
        assert(audio_seeks==seeks);
        lv_obj_send_event(np_seek,LV_EVENT_PRESSED,NULL);
        lv_obj_send_event(np_seek,LV_EVENT_RELEASED,NULL);
        assert(audio_seeks==seeks+1);
        int old_index=started;
        lv_obj_send_event(np_seek,LV_EVENT_PRESSED,NULL);
        started++;
        lv_obj_send_event(np_seek,LV_EVENT_RELEASED,NULL);
        assert(audio_seeks==seeks+1);
        started=old_index;
        sl_track_path(0,test_track.path,sizeof test_track.path);
        test_track.elapsed=4*60*60*1000UL;
    }
    frame(50);np_update(false);assert(!lv_display_get_default()->inv_p);   /* no art: an idle tick repaints nothing */
    queued=6;started=2;first_index=0;for(int i=0;i<6;i++){queue_tracks[i]=i;queue_flags[i]=i!=2;}
    np_show();frame(500);shot("now-playing.ppm");
    {   /* the heart: a tap loves the song, another unloves it, the file says so */
        int t=sl_lib_find_path(audio_current_track()->path);
        if(t>=0){assert(!lv_obj_has_flag(np_love,LV_OBJ_FLAG_HIDDEN) && ts_rating(t)!=1);
            lv_obj_send_event(np_love,LV_EVENT_CLICKED,NULL);frame(100);assert(ts_rating(t)==1);shot("np-loved.ppm");
            lv_obj_send_event(np_love,LV_EVENT_CLICKED,NULL);frame(100);assert(ts_rating(t)==0);}
    }
    {   /* an audiobook: back and ahead are thirty seconds, and say so */
        unsigned long el0=test_track.elapsed,len0=test_track.length;int s0=audio_seeks;
        test_book_playing=true;np_update(true);frame(100);
        assert(np_book && !strcmp(lv_label_get_text(lv_obj_get_child(np_next_btn,0)),"+30"));
        test_track.length=600000;test_track.elapsed=100000;
        lv_obj_send_event(np_next_btn,LV_EVENT_CLICKED,NULL);assert(test_track.elapsed==130000 && audio_seeks==s0+1);
        lv_obj_send_event(np_prev_btn,LV_EVENT_CLICKED,NULL);lv_obj_send_event(np_prev_btn,LV_EVENT_CLICKED,NULL);assert(test_track.elapsed==70000);
        shot("np-audiobook.ppm");
        test_book_playing=false;np_update(true);frame(100);assert(!np_book && !strcmp(lv_label_get_text(lv_obj_get_child(np_next_btn,0)),LV_SYMBOL_NEXT));
        test_track.elapsed=el0;test_track.length=len0;
    }
    {   /* a letterboxed cover (480 x 420) meets its reflection at its own foot, not the black strip under it */
        static uint16_t pic[480*420];for(int i=0;i<480*420;i++)pic[i]=0xf800;
        np_glow_build(pic,480,420,480,lv_color_black());
        const uint16_t *g=(const uint16_t *)np_glow_buf;int r=0,gb=0;
        for(int x=100;x<380;x+=20){uint16_t v=g[(NG_FADE+4)*W+x];r+=v>>11;gb+=((v>>5)&63)/2+(v&31);}
        assert(r>40 && r>gb);np_glow_refresh(true);
    }
    {   /* Classic, dark then light: plain discs, the accent on the sliders */
        global_settings.sloppy_np_style=1;now_playing_retheme();frame(300);shot("np-classic.ppm");
        assert(lv_obj_get_style_bg_opa(np_shuffle,0)==LV_OPA_TRANSP && lv_obj_get_style_bg_opa(np_pp,0)>LV_OPA_50 && lv_obj_get_style_height(np_seek,0)==6);   /* one disc: play; a thin line */
        global_settings.sloppy_theme=1;now_playing_retheme();frame(300);shot("np-classic-light.ppm");
        global_settings.sloppy_theme=0;global_settings.sloppy_np_style=0;now_playing_retheme();frame(300);
        assert(lv_obj_get_style_bg_opa(np_shuffle,0)==LV_OPA_TRANSP);   /* glass too: bare glyphs */
    }
    {   /* the sliders by touch: a drag seeks once, on release, where it was let go; the knob swells
         * while held; the volume follows the finger and the side buttons; neither closes the page */
        lv_area_t sk;lv_obj_get_coords(np_seek,&sk);int sy=(sk.y1+sk.y2)/2,sw=sk.x2-sk.x1,seeks=audio_seeks;
        int knob0=lv_obj_get_style_pad_top(np_seek,LV_PART_KNOB);
        touch_at(sk.x1+sw/5,sy+14,true);             /* a little below the track: still the slider's */
        for(int i=1;i<=6;i++)touch_at(sk.x1+sw/5+i*sw/10,sy+14,true);
        assert(audio_seeks==seeks && lv_obj_has_state(np_seek,LV_STATE_PRESSED));
        assert(lv_obj_get_style_pad_top(np_seek,LV_PART_KNOB)>knob0);
        touch_at(sk.x1+sw*4/5,sy+14,false);frame(200);
        assert(audio_seeks==seeks+1 && labs((long)test_track.elapsed-(long)test_track.length*4/5)<(long)test_track.length/30);
        assert(np_open && lv_obj_get_y(np)==0);
        lv_area_t vk;lv_obj_get_coords(np_vol,&vk);int vy=(vk.y1+vk.y2)/2,vw=vk.x2-vk.x1,v0=global_status.volume;
        touch_at(vk.x1+vw/4,vy,true);for(int i=1;i<=4;i++)touch_at(vk.x1+vw/4+i*vw/8,vy,true);
        touch_at(vk.x1+vw*3/4,vy,false);frame(100);
        assert(volume_pct()>=70 && volume_pct()<=80 && np_open && lv_obj_get_y(np)==0);
        { int v=lv_slider_get_value(np_vol);for(int i=0;i<4;i++){np_update(false);frame(250);}assert(lv_slider_get_value(np_vol)==v); }   /* no snap back after a drag */
        global_status.volume=sound_min(SOUND_VOLUME);np_update(false);assert(lv_slider_get_value(np_vol)==0);
        global_status.volume=v0;np_update(false);
    }
    {   struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);for(int i=0;i<5;i++)np_glow_refresh(true);
        clock_gettime(CLOCK_MONOTONIC,&t1);
        printf("now playing glass: %ld us per cover\n",((t1.tv_sec-t0.tv_sec)*1000000000L+(t1.tv_nsec-t0.tv_nsec))/5000);}
    {   /* short of memory with Now Playing shut: its glass goes, and comes back when it opens */
        assert(np_glow_buf);np_hide(0);frame(500);sl_mem_release("test",false);
        assert(!np_glow_buf && !lv_image_get_src(np_glow));
        np_show();frame(500);assert(np_glow_buf && lv_image_get_src(np_glow)==&np_glow_img);
        sl_mem_release("test",false);assert(np_glow_buf);   /* open: kept */
    }
    {   /* swipe the cover: the neighbour's cover follows the finger, and is the song that plays */
        int q0=queued,s0=started,f0=first_index,rep0=global_settings.repeat_mode;char p0[MAX_PATH];strlcpy(p0,test_track.path,sizeof p0);
        unsigned long el0=test_track.elapsed;
        int qt0[6],qf0[6];for(int i=0;i<6;i++){qt0[i]=queue_tracks[i];qf0[i]=queue_flags[i];}
        queued=6;first_index=0;for(int i=0;i<6;i++){queue_tracks[i]=i;queue_flags[i]=0;}
        #define AT(i) do{started=(i);sl_track_path(i,test_track.path,sizeof test_track.path);np_update(false);frame(50);}while(0)
        #define ALB(i) (sl_lib.tracks[i].album)
        test_art=true;global_settings.repeat_mode=REPEAT_OFF;test_track.elapsed=1000;
        if(!np_open){np_show();frame(500);}
        AT(2);np_update(true);frame(800);
        assert(np_img_album==ALB(2));
        /* slow, past a third: the next song; its reflection made at the touch when the preload could not */
        nps.glow_ready=false;
        int n0=audio_nexts,p0n=audio_prevs;
        touch_at(300,300,true);for(int i=1;i<=12;i++)touch_at(300-i*18,300,true);
        assert(nps.side==1 && nps.album==ALB(3) && nps.art && nps.buf[0]==test_art_px(ALB(3)) &&
               !lv_obj_has_flag(nps.peek,LV_OBJ_FLAG_HIDDEN) && lv_obj_get_style_translate_x(np_art,0)==-216 &&
               lv_obj_get_y(np)==0);
        /* the reflections travel with their covers while the finger is down, not after */
        assert(nps.glow_ready && !lv_obj_has_flag(nps.glow,LV_OBJ_FLAG_HIDDEN) &&
               lv_obj_get_style_translate_x(np_glow,0)==-216 && lv_obj_get_style_translate_x(nps.glow,0)==W-216);
        shot_now("np-swipe-next.ppm");
        uint8_t *slid_in=nps.glow_buf;
        touch_at(84,300,false);assert(audio_nexts==n0+1);          /* plays at the let go, not after the slide */
        for(int i=0;i<30;i++)frame(16);
        assert(!nps.sliding && lv_obj_get_style_translate_x(np_art,0)==0 && np_img_album==ALB(3) &&
               ((uint16_t *)np_img.data)[0]==test_art_px(ALB(3)) && np_glow_at);
        assert(np_glow_buf==slid_in && lv_obj_get_style_translate_x(np_glow,0)==0 && lv_obj_has_flag(nps.glow,LV_OBJ_FLAG_HIDDEN));   /* the lake that slid in stays */
        AT(3);assert(np_img_album==ALB(3));frame(800);np_update(false);assert(!np_glow_at);   /* the glass, once */
        /* short of a third, slowly: back, nothing plays */
        touch_at(300,300,true);for(int i=1;i<=6;i++){touch_at(300-i*10,300,true);frame(60);}
        touch_at(240,300,false);for(int i=0;i<30;i++)frame(16);
        assert(audio_nexts==n0+1 && lv_obj_get_style_translate_x(np_art,0)==0 && np_img_album==ALB(3));
        /* a quick flick, a short way: the next song */
        touch_at(300,300,true);for(int i=1;i<=4;i++)touch_at(300-i*30,300,true);touch_at(180,300,false);
        assert(audio_nexts==n0+2);for(int i=0;i<30;i++)frame(16);AT(4);
        /* to the right: the previous song */
        touch_at(150,300,true);for(int i=1;i<=12;i++)touch_at(150+i*18,300,true);
        assert(nps.side==-1 && nps.album==ALB(3));touch_at(366,300,false);
        assert(audio_prevs==p0n+1);for(int i=0;i<30;i++)frame(16);assert(np_img_album==ALB(3));AT(3);
        /* changed its mind mid-way: the other neighbour */
        touch_at(240,300,true);for(int i=1;i<=6;i++)touch_at(240-i*20,300,true);assert(nps.side==1);
        for(int i=1;i<=15;i++)touch_at(120+i*20,300,true);assert(nps.side==-1 && nps.album==ALB(2));
        touch_at(420,300,false);assert(audio_prevs==p0n+2);for(int i=0;i<30;i++)frame(16);AT(2);
        /* the first song, repeat off: no previous; the cover only gives */
        AT(0);touch_at(150,300,true);for(int i=1;i<=12;i++)touch_at(150+i*18,300,true);
        assert(nps.side==0 && lv_obj_get_style_translate_x(np_art,0)==216/4);
        touch_at(366,300,false);for(int i=0;i<30;i++)frame(16);
        assert(audio_prevs==p0n+2 && lv_obj_get_style_translate_x(np_art,0)==0);
        /* repeat all: round to the last; repeat one: a skip goes on */
        global_settings.repeat_mode=REPEAT_ALL;assert(np_neighbour(-1)==5);
        global_settings.repeat_mode=REPEAT_ONE;assert(np_neighbour(1)==1 && np_neighbour(-1)==5);
        global_settings.repeat_mode=REPEAT_OFF;AT(5);assert(np_neighbour(1)==-1);
        first_index=3;AT(2);assert(np_neighbour(1)==-1 && np_neighbour(-1)==1);   /* a list begun mid-way: 3 4 5 0 1 2 */
        AT(3);assert(np_neighbour(-1)==-1 && np_neighbour(1)==4);first_index=0;
        /* the buttons slide too; pressed fast, each one skips, and the cover follows the song that plays */
        AT(1);int n1=audio_nexts;
        np_skip(1);assert(audio_nexts==n1+1 && nps.sliding);
        np_skip(1);np_skip(1);assert(audio_nexts==n1+3 && !nps.sliding);   /* no slide for the ones still on their way */
        for(int i=0;i<30;i++)frame(16);AT(4);frame(100);assert(np_img_album==ALB(4));
        /* previous: a few seconds in, it starts the song again, as before */
        int seeks=audio_seeks,pr=audio_prevs;test_track.elapsed=9000;np_skip(-1);
        assert(audio_seeks==seeks+1 && audio_prevs==pr && !nps.sliding);
        test_track.elapsed=1000;np_skip(-1);assert(audio_prevs==pr+1 && nps.sliding);for(int i=0;i<30;i++)frame(16);AT(3);
        /* no art: the neighbour's colour, no picture */
        test_art=false;nps.path[0]=0;np_skip(1);assert(nps.sliding && !nps.art && lv_obj_has_flag(nps.peek_img,LV_OBJ_FLAG_HIDDEN));
        for(int i=0;i<30;i++)frame(16);
        assert(lv_obj_has_flag(np_art_img,LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(np_art_none,LV_OBJ_FLAG_HIDDEN));
        test_art=true;AT(4);
        /* a pull down is still a pull: the sheet closes */
        touch_at(240,200,true);for(int i=1;i<=10;i++)touch_at(240+i,200+i*22,true);touch_at(250,420,false);frame(500);
        assert(!np_open && !nps.buf);np_show();frame(500);
        #undef AT
        #undef ALB
        for(int i=0;i<6;i++){queue_tracks[i]=qt0[i];queue_flags[i]=qf0[i];}
        test_art=false;queued=q0;started=s0;first_index=f0;global_settings.repeat_mode=rep0;test_track.elapsed=el0;
        np_img_album=-1;hidden_if_changed(np_art_img,true);   /* the test's covers go with it */
        strlcpy(test_track.path,p0,sizeof test_track.path);np_update(true);frame(300);
    }
    {   /* the edge cases: no tags, no bit rate, no list place; very long names; shuffle shown on the button */
        const char *t0=test_track.title,*a0=test_track.artist;unsigned long br=test_track.bitrate;
        char want[64];const char *slash=strrchr(test_track.path,'/');strlcpy(want,slash?slash+1:test_track.path,sizeof want);
        char *dot=strrchr(want,'.');if(dot)*dot=0;
        test_track.title=NULL;test_track.artist=NULL;test_track.albumartist=NULL;test_track.bitrate=0;
        int q=queued;queued=0;np_update(true);frame(100);
        assert(!strcmp(lv_label_get_text(np_title),want)&&!strcmp(lv_label_get_text(np_artist),"Unknown artist"));
        assert(!strstr(lv_label_get_text(np_fmt),"kbps")&&!strstr(lv_label_get_text(np_fmt),"0/0"));
        assert(!lv_obj_has_flag(np_art_none,LV_OBJ_FLAG_HIDDEN));   /* no cover: the record placeholder */
        qp_refresh();assert(!strcmp(lv_label_get_text(qp_np_t),want));   /* not "Nothing playing" */
        shot("np-untagged.ppm");queued=q;
        test_track.title="A Very Long Song Title That Keeps Going Well Past The Edge Of The Screen";
        test_track.artist="The Extraordinarily Long Named Philharmonic Orchestra and Choir";test_track.bitrate=320;
        global_settings.playlist_shuffle=true;np_update(true);frame(100);
        assert(lv_obj_has_state(np_shuffle,LV_STATE_CHECKED)&&strstr(lv_label_get_text(np_fmt),"320 kbps"));
        assert(lv_obj_get_y(np_artist)+lv_obj_get_height(np_artist)<lv_obj_get_y(np_seek));   /* one line, clear of the bar */
        shot("np-long.ppm");
        global_settings.playlist_shuffle=false;np_update(false);assert(!lv_obj_has_state(np_shuffle,LV_STATE_CHECKED));
        /* repeat beside the transport: off, all, one (a "1" on it), as the control centre's tile */
        int rep0=global_settings.repeat_mode;
        global_settings.repeat_mode=REPEAT_OFF;np_update(false);assert(!lv_obj_has_state(np_repeat,LV_STATE_CHECKED));
        lv_obj_send_event(np_repeat,LV_EVENT_CLICKED,NULL);assert(global_settings.repeat_mode==REPEAT_ALL&&lv_obj_has_state(np_repeat,LV_STATE_CHECKED));
        assert(lv_obj_has_flag(lv_obj_get_child(np_repeat,1),LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(np_repeat,LV_EVENT_CLICKED,NULL);assert(global_settings.repeat_mode==REPEAT_ONE&&!lv_obj_has_flag(lv_obj_get_child(np_repeat,1),LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(np_repeat,LV_EVENT_CLICKED,NULL);assert(global_settings.repeat_mode==REPEAT_OFF&&!lv_obj_has_state(np_repeat,LV_STATE_CHECKED));
        { char lb[24];global_settings.repeat_mode=REPEAT_ONE;assert(!strcmp(qp_tile_label(1,lb,sizeof lb),"Repeat one")); }
        global_settings.repeat_mode=rep0;np_update(false);
        /* the queue sits up top with the other views */
        { lv_obj_t *row=lv_obj_get_parent(np_pp);assert(lv_obj_get_child_count(row)==5&&lv_obj_get_child(row,4)==np_repeat&&lv_obj_get_child(row,0)==np_shuffle);
          assert(lv_obj_get_parent(np_queue)==np&&lv_obj_get_y(np_queue)==10);
          /* beside the Lyrics button, or in its place */
          bool lh=lv_obj_has_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN);np_update(false);lv_obj_update_layout(np);assert(lv_obj_get_x(np_queue)+56+8==lv_obj_get_x(lyr_btn));
          lv_obj_add_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN);np_update(false);lv_obj_update_layout(np);assert(lv_obj_get_x(np_queue)==W-8-56);
          lv_obj_update_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN,lh);np_update(false); }
        test_track.title=t0;test_track.artist=a0;test_track.bitrate=br;np_update(true);frame(100);
    }
    {   /* vinyl mode: a turntable instead of the cover; the label turns, the arm follows the song, drag to scratch */
        SFX_RESET();
        lv_obj_send_event(vnl_btn,LV_EVENT_CLICKED,NULL);
        assert(vnl_on() && vnl && vnl_timer && lv_obj_has_flag(np_art,LV_OBJ_FLAG_HIDDEN) && sfx_last==SFX_VINYL_DROP);
        for(int i=0;i<60;i++){vnl_last_ms=sl_ms()-33;frame(33);}
        assert(vnl_angle!=0 && vnl_arm_deg>VNL_ARM_REST+1);
        float nr=vnl_needle_r(vnl_arm_deg);assert(nr<120 && nr>88);       /* 80% in: near the run-out */
        shot("vinyl.ppm");
        unsigned long at=test_track.elapsed;
        lv_area_t va;lv_obj_get_coords(vnl,&va);int cx=va.x1+VNL_CX,cy=va.y1+VNL_CY;
        /* the song runs through the scratch filter while the turntable shows */
        assert(vnl_needle && test_filter==scr_filter && scr_ring);
        for(int k=0;k<40;k++){const void *st;size_t sz;test_filter(&st,&sz,test_pull);assert(st && sz);}
        assert(scr_live);
        SFX_RESET();
        uint32_t needle=scr_pos_pub;
        for(int k=0;k<=6;k++){float th=k*3.14159265f/12;touch_at(cx+(int)lroundf(150*cosf(th)),cy+(int)lroundf(150*sinf(th)),true);}
        /* a quarter turn moves the needle 450 ms of song, and the sound is the song itself, not an effect */
        assert(vnl_dragging && scr_held && sfx_last!=SFX_VINYL_SCRATCH);
        {int32_t moved=(int32_t)(scr_target-needle);assert(moved>44100*44/100 && moved<44100*46/100);}
        touch_at(cx,cy+150,false);
        assert(test_track.elapsed==at && np_open && !vnl_dragging && !scr_held);   /* no seek: the ring holds it */
        touch_at(cx+150,cy,true);touch_at(cx+150,cy,false);assert(test_track.elapsed==at);   /* a tap is no seek */
        /* wound back past the groove the ring holds: an ordinary seek back */
        for(int k=0;k<6;k++){const void *st;size_t sz;test_filter(&st,&sz,test_pull);}
        touch_at(cx+150,cy,true);scr_turn(-(int32_t)SCR_RING-44100*2,33);touch_at(cx+150,cy+1,true);touch_at(cx+150,cy+1,false);
        assert(test_track.elapsed<at-2000 && test_track.elapsed>at-(SCR_RING/44+4000));
        test_track.elapsed=at;scr_filter(NULL,NULL,NULL);
        for(int i=0;i<30;i++){vnl_last_ms=sl_ms()-33;frame(33);}
        test_status|=AUDIO_STATUS_PAUSE;float sp=vnl_speed;assert(sp>1.5f);
        for(int i=0;i<60;i++){vnl_last_ms=sl_ms()-33;frame(33);}
        assert(vnl_speed<sp/4);test_status&=~AUDIO_STATUS_PAUSE;           /* paused: it coasts to a stop */
        /* off the record, a swipe down still closes Now Playing; closed, the turntable's timer sleeps */
        for(int y=440;y<=680;y+=40)touch_at(30,y,y<680);frame(500);
        assert(!np_open && !vnl_dragging);frame(100);assert(lv_timer_get_paused(vnl_timer));
        np_show();frame(500);assert(vnl && !lv_timer_get_paused(vnl_timer) && lv_obj_has_flag(np_art,LV_OBJ_FLAG_HIDDEN));
        {const void *st;size_t sz;test_filter(&st,&sz,test_pull);assert(scr_lag>0);}   /* some song held in the ring */
        lv_obj_send_event(vnl_btn,LV_EVENT_CLICKED,NULL);
        assert(!vnl_on() && !vnl && !vnl_timer && !vnl_disc_buf && !vnl_label_buf && !lv_obj_has_flag(np_art,LV_OBJ_FLAG_HIDDEN));
        /* off: the filter plays out what it holds, then unhooks and frees the ring */
        vnl_needle_poll();assert(vnl_needle && test_filter && scr_leaving);
        for(int k=0;k<8 && !scr_bypass;k++){const void *st;size_t sz;test_filter(&st,&sz,test_pull);}
        assert(scr_bypass);vnl_needle_poll();assert(!vnl_needle && !test_filter && !scr_ring);
        test_track.elapsed=at;
    }
    assert(!lyr.n && lv_obj_has_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN)==!lyr_can_search(audio_current_track()));   /* no .lrc: a search, if its tags allow one */
    {   /* synced lyrics: button appears, panel follows the song, a tap seeks */
        const char *lrc="[ar:Mira Sol]\n[00:10.00]First light on the water\n[00:20.00]\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d\n"
                        "[00:30.00][01:30.00]Blue hours, blue hours\n[00:40.50]<00:40.50>Hold <00:41.00>on\n[00:50]\n[01:00]Last verse line\n";
        int fd=open("/Music/song-5.lrc",O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);
        assert(write(fd,lrc,strlen(lrc))==(ssize_t)strlen(lrc));sim_close(fd);
        sl_track_path(5,test_track.path,sizeof test_track.path);
        test_track.length=180000;test_track.elapsed=31000;
        np_update(true);frame(100);
        assert(!lv_obj_has_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN) && lyr.synced && lyr.n==7 && !lyr_visible());
        lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);frame(300);
        assert(lyr_visible() && lyr_cur==2 && !strcmp(lv_label_get_text(lyr_rows[3]),"Hold on"));
        shot("lyrics.ppm");
        {   /* reading along: the screen does not go dark under the words; paused, it may */
            int st=test_status;bool running=sl_power_running;enum sl_power_mode mode=sl_power.mode;uint32_t at=sl_power.activity_at;
            test_status=AUDIO_STATUS_PLAY;sl_power_running=true;sl_power.mode=SL_POWER_ACTIVE;
            assert(sl_reading_lyrics());sl_power.activity_at=sl_ms()-600000;sl_power_tick();
            assert(sl_power.mode==SL_POWER_ACTIVE && sl_ms()-sl_power.activity_at<1000);
            test_status=AUDIO_STATUS_PLAY|AUDIO_STATUS_PAUSE;assert(!sl_reading_lyrics());
            test_status=st;sl_power_running=running;sl_power.mode=mode;sl_power.activity_at=at;
        }
        test_track.elapsed=41000;frame(400);assert(lyr_cur==3);
        {   /* Spotify style: the sung line bigger, lit and in the middle; the others smaller and dim */
            frame(400);lv_area_t la,ra;lv_obj_get_coords(lyr_list,&la);lv_obj_get_coords(lyr_rows[3],&ra);
            int mid=(la.y1+la.y2)/2,c=(ra.y1+ra.y2)/2;
            assert(abs(c-mid)<=3);
            assert(lv_obj_get_style_text_font(lyr_rows[3],0)==&ss_semibold_30 && lv_obj_get_style_text_font(lyr_rows[2],0)==&ss_semibold_23);
            assert(lv_obj_get_style_text_opa(lyr_rows[3],0)==LV_OPA_COVER && lv_obj_get_style_text_opa(lyr_rows[2],0)<200);
            shot_now("lyrics-centered.ppm");
            /* the next line: nothing on screen jumps when the sizes swap, then it glides up */
            lv_obj_get_coords(lyr_rows[4],&ra);int y4=ra.y1;
            test_track.elapsed=50500;lyr_follow(false);lv_obj_update_layout(lyr_list);
            lv_obj_get_coords(lyr_rows[4],&ra);assert(lyr_cur==4 && abs(ra.y1-y4)<=1);
            frame(30);lv_obj_get_coords(lyr_rows[4],&ra);assert(ra.y1<y4);   /* moving, not there yet */
            frame(400);frame(400);lv_obj_get_coords(lyr_rows[4],&ra);assert(abs((ra.y1+ra.y2)/2-mid)<=3);
            /* a finger scrolls away: the panel is left alone, then glides back 4 s after */
            touch_at(240,300,true);for(int k=1;k<=8;k++)touch_at(240,300-k*20,true);touch_at(240,140,false);frame(600);
            int sy=lv_obj_get_scroll_y(lyr_list);
            test_track.elapsed=61000;frame(300);assert(lyr_cur==5 && abs(lv_obj_get_scroll_y(lyr_list)-sy)<=60);
            assert(lyr_hold_until && lyr_held());
            lyr_hold_until=sl_ms()-1;              /* the hold is on the wall clock: as if 4 s went by */
            frame(200);frame(400);frame(400);
            lv_obj_get_coords(lyr_rows[5],&ra);assert(abs((ra.y1+ra.y2)/2-mid)<=3);
            /* before the first line: nothing lit, the first line waits in the middle */
            test_track.elapsed=2000;frame(400);frame(400);assert(lyr_cur==-1);
            lv_obj_get_coords(lyr_rows[0],&ra);assert(abs((ra.y1+ra.y2)/2-mid)<=3 && lv_obj_get_style_text_font(lyr_rows[0],0)==&ss_semibold_23);
            test_track.elapsed=41000;frame(400);frame(400);assert(lyr_cur==3);
            /* opened by a real tap: the scroll to the line is ours, not a finger's,
             * so the panel keeps following */
            lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);frame(100);assert(!lyr_visible());
            lv_area_t ba;lv_obj_get_coords(lyr_btn,&ba);
            touch_at((ba.x1+ba.x2)/2,(ba.y1+ba.y2)/2,true);touch_at((ba.x1+ba.x2)/2,(ba.y1+ba.y2)/2,false);frame(100);
            assert(lyr_visible() && lyr_cur==3 && !lyr_held());
        }
        np_hide(0);frame(500);np_show();frame(500);         /* same song again: still following */
        test_track.elapsed=51000;frame(400);assert(lyr_visible() && lyr_cur==4);
        lv_obj_send_event(lyr_rows[1],LV_EVENT_CLICKED,NULL);assert(test_track.elapsed==20000);
        frame(300);assert(lyr_cur==1);shot("lyrics-hebrew.ppm");
        /* the next song has no lyrics: the panel closes and the button hides */
        sl_track_path(6,test_track.path,sizeof test_track.path);np_update(false);frame(100);
        assert(!lyr_visible() && lv_obj_has_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN)==!lyr_can_search(audio_current_track()) && !lyr.n);
        {   /* a song without lyrics: off Wi-Fi the button asks to turn it on; on it, LRCLIB (the
             * pretend server) answers with every version, synced first and nearest in length; a
             * choice lands next to the song and the panel opens on it. One that plainly matches
             * (synced, the song's own length, no other near it) is taken without asking. */
            const char *t6=test_track.title,*a6=test_track.artist;bool want6=lyr_want;test_track.title="Night Drive";test_track.artist="Mira Sol";test_track.length=200000;
            int wifi6=global_settings.sloppy_wifi;global_settings.sloppy_wifi=0;
            wf_st.connected=false;lyr_path[0]='\0';lyr_track(test_track.path);frame(100);
            assert(!lv_obj_has_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN) && !lyr.n);
            lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);frame(300);
            assert(acts_open_now && find_label(acts,sl_tr("Turn on Wi-Fi?")) && find_label(acts,sl_tr("Turn on Wi-Fi")) && find_label(acts,sl_tr("Not now")));
            shot("lyrics-wifi.ppm");
            lv_obj_send_event(lv_obj_get_parent(find_label(acts,sl_tr("Not now"))),LV_EVENT_CLICKED,NULL);frame(300);
            assert(!acts_open_now && !lyr_wait_wifi);
            /* asked, then Wi-Fi connects: the search runs by itself */
            strlcpy(lyr_found_for,test_track.path,sizeof lyr_found_for);lyr_wait_wifi=true;wf_st.connected=true;lyr_online();frame(300);
            assert(!lyr_wait_wifi && strstr(sl_net_demo_url,"lrclib.net/api/search?track_name=Night%20Drive&artist_name=Mira%20Sol"));
            assert(acts_open_now && lyr_n_found==3 && lyr_found[0].synced && lyr_found[0].secs==240 && lyr_found[1].synced && !lyr_found[2].synced);
            assert(find_label(acts,sl_tr("Choose lyrics")) && find_label(acts,sl_tr("Text only")));
            shot("lyrics-picker.ppm");
            lv_obj_t *live=lv_obj_get_parent(find_label(acts,"Night Drive (Live) \xc2\xb7 Mira Sol"));assert(live);
            lv_obj_send_event(live,LV_EVENT_CLICKED,NULL);frame(300);
            assert(!acts_open_now && lyr.n==1 && lyr.synced && lyr_visible() && !lyr_n_found);shot("lyrics-fetched.ppm");
            lyr_show(false);assert(fake_find("/Music/song-6.lrc")>=0);sim_remove("/Music/song-6.lrc");
            /* the song's own length: taken at once */
            test_track.length=240000;lyr_path[0]='\0';lyr_track(test_track.path);frame(100);
            wf_st.connected=true;   /* the job's status was the pretend radio's, which is off */
            lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);frame(300);
            assert(!acts_open_now && lyr.n==2 && lyr.synced && lyr_visible());
            /* held: choose again, though the card has some now */
            wf_st.connected=true;lv_obj_send_event(lyr_btn,LV_EVENT_LONG_PRESSED,NULL);frame(300);
            assert(acts_open_now && lyr_n_found==3);acts_close();frame(300);lyr_found_free();
            lyr_show(false);lyr_want=want6;sim_remove("/Music/song-6.lrc");wf_st.connected=false;global_settings.sloppy_wifi=wifi6;
            test_track.title=t6;test_track.artist=a6;lyr_path[0]='\0';lyr_track(test_track.path);frame(100);
            assert(lv_obj_has_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN)==!lyr_can_search(audio_current_track()) && !lyr.n);
        }
        /* plain lyrics reopen by themselves, unsynced, and never seek */
        const char *plain="Just words\nno times here\n";
        fd=open("/Music/song-7.LRC",O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);
        assert(write(fd,plain,strlen(plain))==(ssize_t)strlen(plain));sim_close(fd);
        sl_track_path(7,test_track.path,sizeof test_track.path);np_update(false);frame(100);
        assert(lyr_visible() && !lyr.synced && lyr.n==2);
        long before=test_track.elapsed;lv_obj_send_event(lyr_rows[0],LV_EVENT_CLICKED,NULL);assert(test_track.elapsed==before);
        lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);frame(100);assert(!lyr_visible() && !lyr_want);
        remove("/Music/song-5.lrc");remove("/Music/song-7.LRC");
        sl_track_path(0,test_track.path,sizeof test_track.path);
        test_track.length=5*60*60*1000UL;test_track.elapsed=4*60*60*1000UL;np_update(true);frame(100);
    }
    test_track.title="יום חדש - New Day";test_track.artist="שולה חן";
    np_update(true);frame(500);shot("hebrew-now-playing.ppm");
    {   /* Hebrew drawn by its own font beside each UI font, a step heavier and a tenth bigger, so it
         * reads as the Latin beside it does (sloppy-dev/hebrew-fonts.py) */
        const lv_font_t *const ui[]={&ss_regular_16,&ss_regular_18,&ss_regular_21,&ss_regular_23,&ss_regular_26,&ss_semibold_18,&ss_semibold_21,&ss_semibold_23,&ss_semibold_26,&ss_semibold_30,&ss_semibold_34,&ss_semibold_38,&ss_bold_28};
        const lv_font_t *const he[]={&ss_heb_regular_16,&ss_heb_regular_18,&ss_heb_regular_21,&ss_heb_regular_23,&ss_heb_regular_26,&ss_heb_semibold_18,&ss_heb_semibold_21,&ss_heb_semibold_23,&ss_heb_semibold_26,&ss_heb_semibold_30,&ss_heb_semibold_34,&ss_heb_semibold_38,&ss_heb_bold_28};
        for(int i=0;i<13;i++){lv_font_glyph_dsc_t g,a;
            assert(lv_font_get_glyph_dsc(ui[i],&g,0x5D0,0)&&g.resolved_font==he[i]);   /* alef */
            assert(lv_font_get_glyph_dsc(ui[i],&a,'A',0)&&a.resolved_font==ui[i]&&g.box_h>a.box_h*3/5);   /* as tall as a capital, near enough */
            assert(lv_font_get_glyph_dsc(ui[i],&g,0x627,0)&&g.resolved_font!=he[i]);}   /* Arabic still from the Arabic fonts */
    }
    test_track.title="Blue Hours";test_track.artist="Mira Sol";np_update(true);
    /* lock screen Now Playing card: art, controls and both layouts */
    {
        static uint16_t art_px[300*300];
        for(int i=0;i<300*300;i++) art_px[i]=(uint16_t)(((i%300)*31/299)<<11|((i/300)*63/299)<<5|12);
        struct bitmap saved_bm=test_bitmap;int saved_lock_wall=global_settings.sloppy_lock_wallpaper;
        test_bitmap.width=300;test_bitmap.height=300;test_bitmap.data=(void*)art_px;
        global_settings.sloppy_lock_player=0;global_settings.sloppy_lock_wallpaper=5;
        sl_power_running=true;sl_power_state_init(&sl_power,sl_ms());
        sl_power.mode=SL_POWER_LOCKED;sl_power_apply();frame(100);
        assert(lp_card && !lv_obj_has_flag(lp_card,LV_OBJ_FLAG_HIDDEN) && lp_art_ready);
        assert(!strcmp(lv_label_get_text(lp_title),"Blue Hours"));
        lv_obj_update_layout(lp_card);
        lv_area_t pp;lv_obj_get_coords(lp_btn[1],&pp);
        int cx=(pp.x1+pp.x2)/2,cy=(pp.y1+pp.y2)/2;
        sl_power_filter_touch(true,cx,cy);assert(lv_obj_has_state(lp_btn[1],LV_STATE_PRESSED));
        sl_power_filter_touch(true,cx+3,cy-4);assert(lv_obj_get_style_y(sl_power_overlay,0)==0);
        sl_power_filter_touch(false,0,0);
        assert((test_status&AUDIO_STATUS_PAUSE) && sl_power_is_locked());
        assert(!lv_obj_has_state(lp_btn[1],LV_STATE_PRESSED));
        assert(!strcmp(lv_label_get_text(lv_obj_get_child(lp_btn[1],0)),LV_SYMBOL_PLAY));
        frame(100);shot("lock-player-big.ppm");
        sl_power_filter_touch(true,cx,cy);sl_power_filter_touch(false,0,0);
        assert(!(test_status&AUDIO_STATUS_PAUSE));
        /* scrub the song: the drag previews, the release seeks, nothing unlocks */
        lv_area_t sk;lv_obj_get_coords(lp_track,&sk);int sy=(sk.y1+sk.y2)/2;
        sl_power_filter_touch(true,sk.x1+10,sy+15);
        assert(lp_held==LP_SEEK && lp_scrub_ms>=0);
        sl_power_filter_touch(true,(sk.x1+sk.x2)/2,sy-150);
        long target=lp_scrub_ms;
        assert(labs(target-(long)test_track.length/2)<(long)test_track.length/50);
        assert(lv_obj_get_style_y(sl_power_overlay,0)==0);
        sl_power_filter_touch(false,0,0);
        assert(sl_power_is_locked() && lp_scrub_ms==-1 && (long)test_track.elapsed==target);
        /* volume follows the finger, past either end */
        int saved_vol=global_status.volume;
        lv_area_t vl;lv_obj_get_coords(lp_vol_track,&vl);int vy=(vl.y1+vl.y2)/2;
        sl_power_filter_touch(true,vl.x1+5,vy-12);sl_power_filter_touch(true,vl.x2+30,vy-140);
        assert(volume_pct()==100);assert(lv_obj_get_style_height(lp_vol_track,0)==10);
        lv_refr_now(disp);shot_now("lock-player-volume.ppm");  /* a frame would lift the finger */
        sl_power_filter_touch(false,0,0);assert(sl_power_is_locked());
        sl_power_filter_touch(true,vl.x1+(vl.x2-vl.x1)/4,vy);sl_power_filter_touch(false,0,0);
        assert(volume_pct()>=22 && volume_pct()<=28);
        {   /* the keys on the lock screen: its own bar moves, no side bar */
            if(hud)lv_obj_add_flag(hud,LV_OBJ_FLAG_HIDDEN);
            lv_area_t f0,f1;lp_refresh();lv_obj_update_layout(lp_vol_fill);lv_obj_get_coords(lp_vol_fill,&f0);
            for(int i=0;i<6;i++)volume_key(1);
            lv_obj_update_layout(lp_vol_fill);lv_obj_get_coords(lp_vol_fill,&f1);
            printf("lock volume keys: hud %s, fill %d -> %d, shown %d, pct %d\n",!hud?"none":lv_obj_has_flag(hud,LV_OBJ_FLAG_HIDDEN)?"hidden":"SHOWN",(int)f0.x2,(int)f1.x2,lp_volume_shown(),volume_pct());
            assert((!hud||lv_obj_has_flag(hud,LV_OBJ_FLAG_HIDDEN)) && f1.x2>f0.x2);}
        global_status.volume=saved_vol;
        /* the song's title, tapped: it dims under the finger, then unlocks straight into Now Playing */
        if(np_open){np_hide(0);frame(500);}
        {   lv_area_t st;lv_obj_get_coords(lp_title,&st);int tx=(st.x1+st.x2)/2,ty=(st.y1+st.y2)/2;
            sl_power_filter_touch(true,tx,ty);assert(lp_held==LP_SONG&&lv_obj_get_style_opa(lp_title,0)<LV_OPA_COVER);
            sl_power_filter_touch(true,tx+2,ty-3);sl_power_filter_touch(false,0,0);frame(600);
            assert(!sl_power_is_locked()&&np_open&&lv_obj_get_style_opa(lp_title,0)==LV_OPA_COVER);
            np_hide(0);frame(500);sl_power.mode=SL_POWER_LOCKED;sl_power_apply();frame(100);
            /* a swipe up from the title unlocks to where it was, not Now Playing */
            sl_power_filter_touch(true,tx,ty);sl_power_filter_touch(true,tx,ty-200);sl_power_filter_touch(false,0,0);frame(600);
            assert(!sl_power_is_locked()&&!np_open);
            sl_power.mode=SL_POWER_LOCKED;sl_power_apply();frame(100);}
        /* a swipe that starts on a button still unlocks, without pressing it */
        sl_power_filter_touch(true,cx,cy);sl_power_filter_touch(true,cx,cy-200);
        assert(!lv_obj_has_state(lp_btn[1],LV_STATE_PRESSED));
        assert(lv_obj_get_style_y(sl_power_overlay,0)<-150);
        sl_power_filter_touch(false,0,0);
        assert(!sl_power_is_locked() && !(test_status&AUDIO_STATUS_PAUSE));
        frame(600);
        /* a quick short flick unlocks too; the same distance dragged slowly does not */
        sl_power.mode=SL_POWER_LOCKED;sl_power_apply();frame(100);
        sl_power_filter_touch(true,W/2,360);sl_power_filter_touch(true,W/2,310);
        sl_power_filter_touch(false,0,0);assert(!sl_power_is_locked());frame(600);
        sl_power.mode=SL_POWER_LOCKED;sl_power_apply();frame(100);
        sl_power_filter_touch(true,W/2,360);usleep(300000);sl_power_filter_touch(true,W/2,310);usleep(300000);
        sl_power_filter_touch(false,0,0);assert(sl_power_is_locked());frame(600);
        /* locked, the volume keys still work, and the volume shows over the lock screen */
        global_status.volume=(sound_min(SOUND_VOLUME)+sound_max(SOUND_VOLUME))/2;
        { int v0=global_status.volume;
          handle_button(BUTTON_UP);handle_button(BUTTON_UP|BUTTON_REL);frame(100);
          assert(global_status.volume>v0&&sl_power_is_locked());
          /* the player card's own bar, or the side bar over the lock screen */
          assert(lp_volume_shown()||(hud&&!lv_obj_has_flag(hud,LV_OBJ_FLAG_HIDDEN)&&lv_obj_get_index(hud)>lv_obj_get_index(sl_power_overlay))); }
        {   /* dark: the reader is stopped, but a tap on the panel still wakes
             * the lock screen (the R1 posts no event for it), and nothing
             * under the finger is pressed */
            sl_power.mode=SL_POWER_DARK;sl_power_apply();frame(100);
            t_down=false;touch_dark_poll();assert(touch_paused);
            touch_dark_poll();assert(touch_paused);              /* no finger: still stopped */
            t_x=W/2;t_y=400;t_down=true;touch_dark_poll();assert(!touch_paused);
            frame(40);assert(sl_power.mode==SL_POWER_LOCKED);
            t_down=false;frame(40);frame(40);
            assert(sl_power_is_locked()&&sl_power.mode==SL_POWER_LOCKED);
            /* and the LCD (with the touch panel) never sleeps while Tap to
             * wake is on; off, Rockbox's own sleep is back */
            sl_power_take_display();assert(test_lcd_sleep==-1);
            global_settings.sloppy_tap_wake=0;sl_power_tap_wake_apply();assert(test_lcd_sleep==2);
            global_settings.sloppy_tap_wake=1;sl_power_tap_wake_apply();assert(test_lcd_sleep==-1);
            sl_power_restore_display();assert(test_lcd_sleep==2);sl_power_take_display();
            printf("PASS: a tap wakes a dark screen to the lock screen, the LCD stays awake for it.\n");
            /* Tap to wake off: a finger on the dark screen (a pocket) is not
             * looked at, and one the panel posts anyway is swallowed */
            global_settings.sloppy_tap_wake=0;
            sl_power.mode=SL_POWER_DARK;sl_power_apply();frame(100);
            t_down=false;touch_dark_poll();assert(touch_paused);
            t_x=W/2;t_y=400;t_down=true;touch_dark_poll();assert(touch_paused);
            frame(40);assert(sl_power.mode==SL_POWER_DARK);
            assert(sl_power_filter_touch(true,W/2,400)&&sl_power.mode==SL_POWER_DARK);
            assert(sl_power_filter_touch(true,W/2,200)&&sl_power_filter_touch(false,0,0));
            t_down=false;frame(40);assert(sl_power.mode==SL_POWER_DARK);
            sl_power_filter_button(BUTTON_POWER);assert(sl_power.mode==SL_POWER_LOCKED);
            sl_power_filter_button(BUTTON_POWER|BUTTON_REL);
            assert(!sl_power_filter_touch(false,0,0)||sl_power.mode==SL_POWER_LOCKED);
            global_settings.sloppy_tap_wake=1;touch_wake(0,0);   /* the reader on again, as a key would */
            printf("PASS: with Tap to wake off a touch leaves a dark screen dark; the power key still wakes it.\n");
        }
        /* the power key wakes a dark screen on its press, and its release keeps it awake */
        sl_power.mode=SL_POWER_DARK;sl_power_apply();
        sl_power_filter_button(BUTTON_POWER);assert(sl_power.mode==SL_POWER_LOCKED);
        frame(200);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);assert(sl_power.mode==SL_POWER_LOCKED);
        sl_power_filter_button(BUTTON_POWER);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);
        assert(sl_power.mode==SL_POWER_DARK);                      /* and a press locks it again */
        sl_power.mode=SL_POWER_ACTIVE;sl_power_apply();frame(600);
        {   /* power held, then volume up: a screenshot, and the power key's release does not lock */
            int d0=shot_taken;
            sl_power_filter_button(BUTTON_POWER);assert(sl_power.power_down);
            assert(sl_power_filter_button(BUTTON_UP));assert(shot_taken==d0+1);
            sl_power_filter_button(BUTTON_UP|BUTTON_REL);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);frame(100);
            assert(sl_power.mode==SL_POWER_ACTIVE && shot_taken==d0+1);
            sl_power_filter_button(BUTTON_UP);sl_power_filter_button(BUTTON_UP|BUTTON_REL);assert(shot_taken==d0+1);   /* alone: the volume */
            assert(sl_power_filter_button(BUTTON_UP|BUTTON_POWER));assert(shot_taken==d0+2);   /* as one code */
            sl_power_filter_button(BUTTON_UP|BUTTON_POWER|BUTTON_REL);sl_power.power_down=false;
            /* volume up held, then power: Sonix's order; the power key's release does not lock */
            sl_power_filter_button(BUTTON_UP);assert(sl_power_filter_button(BUTTON_POWER));assert(shot_taken==d0+3);
            sl_power_filter_button(BUTTON_POWER|BUTTON_REL);sl_power_filter_button(BUTTON_UP|BUTTON_REL);frame(100);
            assert(sl_power.mode==SL_POWER_ACTIVE&&!sl_power.power_down);
            sl_power_filter_button(BUTTON_POWER);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);assert(shot_taken==d0+3);   /* alone: the power key */
            assert(sl_power.mode==SL_POWER_DARK);
            for(int i=0;i<200&&sl_shot_busy();i++)usleep(10000);sl_shot_wait();
            sl_power.mode=SL_POWER_ACTIVE;sl_power_apply();frame(300);
        }
        {   /* R7: volume down held, then power, locks the player's keys; the same again unlocks */
            int d0=shot_taken,offs=poweroffs;
            frame(3400);   /* the screenshot's banner gone */
            sl_power_filter_button(BUTTON_DOWN);assert(sl_power_filter_button(BUTTON_POWER));assert(sl_keys_locked);
            sl_power_filter_button(BUTTON_POWER|BUTTON_REL);assert(sl_power_filter_button(BUTTON_DOWN|BUTTON_REL));frame(100);
            assert(sl_power.mode==SL_POWER_ACTIVE && !sl_power.power_down && shot_taken==d0);   /* the release neither darkens nor shoots */
            frame(300);assert(has_text(lv_layer_top(),"Buttons locked") || (notes_on() && nh.n && !strcmp(nh.list[0].title,"Buttons locked")));
            shot("keys-locked.ppm");
            static const long keys[]={BUTTON_UP,BUTTON_DOWN,BUTTON_LEFT,BUTTON_RIGHT};
            for (int i=0;i<4;i++)
            {
                assert(sl_power_filter_button(keys[i]));assert(sl_power_filter_button(keys[i]|BUTTON_REPEAT));
                assert(sl_power_filter_button(keys[i]|BUTTON_REL));
            }
            assert(!sl_power_filter_button(BUTTON_PLAY) && !sl_power_filter_button(BUTTON_NEXT));   /* a headset's still work */
            frame(3400);sl_power_filter_button(BUTTON_UP);sl_power_filter_button(BUTTON_UP|BUTTON_REL);
            frame(100);shot("keys-locked-press.ppm");
            /* a hold of power offers no slider and does not force it off */
            sl_power_filter_button(BUTTON_POWER);sl_power.power_at-=SL_POWER_FORCE_MS+50;sl_power_tick();frame(300);
            assert(!po_is_open() && poweroffs==offs);
            sl_power_filter_button(BUTTON_POWER|BUTTON_REL);frame(100);assert(sl_power.mode==SL_POWER_ACTIVE);
            /* a press of power still darkens and lights the screen */
            sl_power_filter_button(BUTTON_POWER);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);assert(sl_power.mode==SL_POWER_DARK);
            sl_power_filter_button(BUTTON_POWER);frame(100);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);assert(sl_power.mode==SL_POWER_LOCKED);
            /* power held, then volume down, from the lock screen: unlocked */
            sl_power_filter_button(BUTTON_POWER);assert(sl_power_filter_button(BUTTON_DOWN));assert(!sl_keys_locked);
            sl_power_filter_button(BUTTON_DOWN|BUTTON_REL);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);frame(100);
            assert(sl_power.mode==SL_POWER_LOCKED && !sl_power.power_down);
            assert(!sl_power_filter_button(BUTTON_DOWN));sl_power_filter_button(BUTTON_DOWN|BUTTON_REL);   /* the volume again */
            assert(sl_power_filter_button(BUTTON_DOWN|BUTTON_POWER) && sl_keys_locked);   /* as one code */
            sl_power_filter_button(BUTTON_DOWN|BUTTON_POWER|BUTTON_REL);sl_power.power_down=false;
            sl_power_filter_button(BUTTON_DOWN);sl_power_filter_button(BUTTON_POWER);assert(!sl_keys_locked);
            sl_power_filter_button(BUTTON_POWER|BUTTON_REL);sl_power_filter_button(BUTTON_DOWN|BUTTON_REL);
            sl_power.mode=SL_POWER_ACTIVE;sl_power_apply();frame(600);
            printf("PASS: Volume down + Power locks and unlocks the keys; power still lights the screen (R7)\n");
        }
        {   /* the card guard: from the handover until the card is back nothing writes to it (the
             * R1's file layer and the harness's fake card share card-guard.h; our POSIX workers see no card) */
            assert(card_guard_refuses("/data/mnt/sd_0/.rockbox/config.cfg",true,"/data/mnt/sd_0")
                   && card_guard_refuses("/usr/data/mnt/sd_0/.rockbox/font.gc",true,"/data/mnt/sd_0")
                   && card_guard_refuses("/data/mnt/sd_0",true,"/data/mnt/sd_0")
                   && !card_guard_refuses("/data/mnt/sd_0x/a",true,"/data/mnt/sd_0")
                   && !card_guard_refuses("/usr/data/rockbox-crash.log",true,"/data/mnt/sd_0")
                   && !card_guard_refuses("/tmp/x",true,"/data/mnt/sd_0")
                   && !card_guard_refuses("/data/mnt/sd_0/.rockbox/config.cfg",false,"/data/mnt/sd_0"));
            int fd=open(ROCKBOX_DIR "/sloppy/guard.txt",O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);write(fd,"before",6);sim_close(fd);
            char real[600];snprintf(real,sizeof real,"%s/guard-posix.txt",sl_net_card_root());
            int log_fd0=log_fd;   /* the log as it was: put back after (the fake card has eight handles) */
            sim_card_away=true;sl_log_hold(true);
            /* Rockbox's writes: settings, the glyph cache, playlists, the log are all refused */
            errno=0;assert(open(ROCKBOX_DIR "/sloppy/guard.txt",O_WRONLY|O_TRUNC)<0 && errno==EROFS);
            assert(open(ROCKBOX_DIR "/sloppy/new.txt",O_WRONLY|O_CREAT,0666)<0 && fake_find(ROCKBOX_DIR "/sloppy/new.txt")<0);
            assert(mkdir(ROCKBOX_DIR "/guard-dir")<0 && rename(ROCKBOX_DIR "/sloppy/guard.txt",ROCKBOX_DIR "/sloppy/g2.txt")<0 && remove(ROCKBOX_DIR "/sloppy/guard.txt")<0);
            assert(fake_equals(ROCKBOX_DIR "/sloppy/guard.txt","before"));   /* untouched */
            fd=open(ROCKBOX_DIR "/sloppy/guard.txt",O_RDONLY);assert(fd>=0);sim_close(fd);   /* reading may go on */
            /* our own workers: no card root to write under */
            assert(!strcmp(sl_net_card_root(),"/dev/null/card-away"));
            {char p2[600];snprintf(p2,sizeof p2,"%s/x.txt",sl_net_card_root());FILE *f=fopen(p2,"w");assert(!f);}
            /* the log keeps its lines meanwhile */
            sl_log("guard: a line while the card is away");
            sim_card_away=false;sl_log_hold(false);
            sl_log("guard: back");
            assert(strcmp(sl_net_card_root(),"/dev/null/card-away"));
            fd=open(ROCKBOX_DIR "/sloppy/g3.txt",O_WRONLY|O_CREAT,0666);assert(fd>=0);sim_close(fd);   /* writes again */
            assert(fake_has(ROCKBOX_DIR "/sloppy/sloppy.log","bytes written while the card was the computer's")
                   && fake_has(ROCKBOX_DIR "/sloppy/sloppy.log","guard: a line while the card is away")
                   && fake_has(ROCKBOX_DIR "/sloppy/sloppy.log","guard: back"));
            remove(ROCKBOX_DIR "/sloppy/guard.txt");remove(ROCKBOX_DIR "/sloppy/g3.txt");(void)real;
            if(log_fd0<0&&log_fd>=0){sim_close(log_fd);log_fd=-1;}
            printf("PASS: card guard: while the card is a computer's no write reaches it (refused, EROFS), workers see no card, the log's lines wait and come after\n");
        }
        global_settings.sloppy_lock_player=1;
        sl_power.mode=SL_POWER_LOCKED;sl_power_apply();frame(100);
        assert(lp_style==LP_SMALL && !lv_obj_has_flag(lp_card,LV_OBJ_FLAG_HIDDEN) && lp_art_ready);
        shot("lock-player-small.ppm");
        test_status=0;lp_refresh();assert(lv_obj_has_flag(lp_card,LV_OBJ_FLAG_HIDDEN));
        test_status=AUDIO_STATUS_PLAY;
        sl_power_running=false;sl_power_overlay_show(false);
        global_settings.sloppy_lock_player=0;global_settings.sloppy_lock_wallpaper=saved_lock_wall;
        test_bitmap=saved_bm;
    }
    lock_player_page();frame(300);shot("lock-player-settings.ppm");
    lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1),1),LV_EVENT_CLICKED,NULL);   /* body, group, its second row */
    assert(global_settings.sloppy_lock_player==1);global_settings.sloppy_lock_player=0;
    np_style_page();frame(300);shot("np-style-settings.ppm");page_pop();frame(300);
    {   /* the biggest text: every word stays inside its card, in every language */
        int step0=sl_text_step,size0=global_settings.sloppy_text_size,lang0=global_settings.sloppy_language;
        for(int lang=0;lang<=SL_LANG_LAST;lang++){
            global_settings.sloppy_language=lang;sl_text_step=3;global_settings.sloppy_text_size=3;
            appearance_reload((void *)2);frame(700);while(npages>1){page_pop();frame(400);}
            for(int pg=0;pg<2;pg++){
                if(pg)cf_style_page();else np_style_page();frame(300);
                lv_obj_t *body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);   /* the group */
                for(int i=0;i<2;i++){lv_obj_t *row=lv_obj_get_child(body,i),*col=lv_obj_get_child(row,0);
                    lv_area_t ra,ca;lv_obj_get_coords(row,&ra);lv_obj_get_coords(col,&ca);
                    assert(ca.y2<=ra.y2-8 && ca.y1>=ra.y1+8);}
                if(!lang&&!pg)shot("np-style-largest.ppm");
                page_pop();frame(300);}
        }
        global_settings.sloppy_language=lang0;sl_text_step=step0;global_settings.sloppy_text_size=size0;
        appearance_reload((void *)2);frame(700);while(npages>1){page_pop();frame(400);}
    }
    page_pop();frame(400);
    sl_lib.tracks[2].title="Night Tide";sl_lib.tracks[3].title="Glass Avenue";sl_lib.tracks[4].title="Silent City";
    sl_lib.albums=realloc(sl_lib.albums,2*sizeof(*sl_lib.albums));
    assert(sl_lib.albums);
    sl_lib.albums[1]=(struct sl_album){.title="Postcards at Dawn",.artist="Neon Harbor",.year=2024};
    sl_lib.n_albums=2;
    /* and the album orders with it: they held the one album, and a walk of them read past it */
    sl_lib.albums_by_title=realloc(sl_lib.albums_by_title,2*sizeof(int));sl_lib.albums_by_artist=realloc(sl_lib.albums_by_artist,2*sizeof(int));
    assert(sl_lib.albums_by_title&&sl_lib.albums_by_artist);
    sl_lib.albums_by_title[0]=0;sl_lib.albums_by_title[1]=1;sl_lib.albums_by_artist[0]=0;sl_lib.albums_by_artist[1]=1;
    sl_lib.tracks[3].album=1;
    test_track.title="Night Tide";sl_track_path(2,test_track.path,sizeof test_track.path);
    test_track.elapsed=test_track.length+9000;test_track.offset=123456789;
    lib_state=SL_LIB_READY;
    quiz_page();frame(150);shot("music-quiz-modes.ppm");
    lv_obj_t *quiz_menu_body=lv_obj_get_child(pages[npages-1],1);
    assert(lv_obj_get_child_count(quiz_menu_body)>=4);
    lv_obj_t *mode_grid = lv_obj_get_child(quiz_menu_body, lv_obj_get_child_count(quiz_menu_body) - 1);
    assert(lv_obj_get_child_count(mode_grid) == 4);
    {   /* the menu fits: nothing to scroll, no mini player over the last tiles */
        assert(!lv_obj_has_flag(quiz_menu_body,LV_OBJ_FLAG_SCROLLABLE) && quiz_menu_shown());
        lv_area_t last;lv_obj_get_coords(lv_obj_get_child(mode_grid,3),&last);assert(last.y2<H);
        int st=test_status;test_status=AUDIO_STATUS_PLAY;mini_update();assert(lv_obj_has_flag(mini,LV_OBJ_FLAG_HIDDEN));
        test_status=st;mini_update();
    }
    lv_obj_send_event(lv_obj_get_child(mode_grid,0),LV_EVENT_CLICKED,NULL);
    frame(500);shot("music-quiz.ppm");assert(quiz.mode==QUIZ_MODE_MUSIC);
    assert(quiz.kind==QUIZ_SONG && quiz.clip.active && queued==7 && quiz_active);
    assert(quiz.clip.elapsed==0 && quiz.clip.offset==0);
    assert(quiz.choice_count==5);
    {   /* the clip's ten seconds start when it is heard, not when it was asked for */
        assert(quiz.question_deadline==0 && !quiz.clip.audible && !quiz.clip.deadline);
        quiz_tick(NULL);frame(0);
        assert(!strcmp(lv_label_get_text(quiz_timer_label),"...") && quiz.question_deadline==0);
        char was_path[sizeof test_track.path];unsigned long was_elapsed=test_track.elapsed;
        strlcpy(was_path,test_track.path,sizeof was_path);
        strlcpy(test_track.path,quiz.clip.path,sizeof test_track.path);test_track.elapsed=90000;
        quiz_tick(NULL);assert(!quiz.clip.audible);          /* seen, but not moving yet */
        uint32_t saved_peak=test_peak;test_peak=0;
        test_track.elapsed=90100;quiz_tick(NULL);
        assert(!quiz.clip.audible && quiz.question_deadline==0);   /* time moves, nothing at the output yet */
        test_peak=9000;quiz_tick(NULL);test_peak=saved_peak;
        uint32_t now=sl_ms();
        assert(quiz.clip.audible && quiz.question_deadline>now+9900 && quiz.clip.deadline>now+2900);
        assert(quiz_aa_released && test_aa_used==0);   /* no cover decode ahead of the clip's audio */
        strlcpy(test_track.path,was_path,sizeof test_track.path);test_track.elapsed=was_elapsed;
    }
    for(int i=0;i<5;i++)for(int j=i+1;j<5;j++)assert(strcasecmp(quiz.choices[i],quiz.choices[j]));
    int choice_y[5];
    for(int i=0;i<5;i++)choice_y[i]=lv_obj_get_y(quiz_choices[i]);
    int eliminated=quiz.elimination_order[0];
    quiz.question_deadline=sl_ms()+7900;
    quiz_tick(NULL);frame(0);
    assert(quiz.eliminated_count==1 && !quiz.answered);
    assert(!lv_obj_has_flag(quiz_choices[eliminated],LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(quiz_choices[eliminated],LV_OBJ_FLAG_CLICKABLE));
    assert(lv_obj_get_style_opa(quiz_choices[eliminated],0)==LV_OPA_TRANSP);
    for(int i=0;i<5;i++)assert(lv_obj_get_y(quiz_choices[i])==choice_y[i]);
    lv_obj_send_event(quiz_choices[eliminated],LV_EVENT_CLICKED,NULL);
    assert(!quiz.answered);
    lv_obj_send_event(quiz_choices[quiz.answer],LV_EVENT_CLICKED,NULL);
    /* the clip goes quiet at once; the song comes back after the answer's sound */
    assert(quiz.clip.active && quiz.clip.hushed && test_status==0 && quiz.clip.restore_at>=sl_ms());
    quiz.clip.restore_at=sl_ms();quiz_tick(NULL);
    assert(!quiz.clip.active && started==2 && queued==6 && quiz.score>0);
    assert(last_start_offset==0); /* restore by time, never the decoder byte offset */
    quiz_game_page(QUIZ_MODE_MUSIC);frame(200);
    if (quiz.clip.active)
    {
        /* A user skip during the sample changes the resume index. */
        int changed_index = (quiz.clip.sample_index + 1) % queued;
        started = changed_index;
        int selected_track = queue_tracks[started];
        quiz_restore_clip();
        assert(queue_tracks[started] == selected_track && !quiz.clip.active);
    }
    page_pop();frame(500);assert(!quiz_active && quiz_timer==NULL);
    page_pop();frame(500);
    {   /* a clip that never plays gives way to another kind of question */
        int fq=queued,fs=started,ft[8];bool ff[8];memcpy(ft,queue_tracks,sizeof ft);memcpy(ff,queue_flags,sizeof ff);
        queued=6;started=2;for(int i=0;i<6;i++){queue_tracks[i]=i;queue_flags[i]=false;}
        sl_track_path(2,test_track.path,sizeof test_track.path);
        quiz_game_page(QUIZ_MODE_MUSIC);frame(200);
        quiz.round=1;quiz_next();frame(100);
        assert(quiz.kind==QUIZ_SONG && quiz.clip.active && !quiz.question_deadline);
        int round=quiz.round,qnum=quiz.qnum;
        quiz.clip.asked_at=sl_ms()-5000;quiz_tick(NULL);
        assert(quiz.kind!=QUIZ_SONG && !quiz.clip.active && quiz.round==round && quiz.qnum==qnum);
        assert(quiz.question_deadline && !quiz.no_clip);
        page_pop();frame(500);page_pop();frame(500);
        queued=fq;started=fs;memcpy(queue_tracks,ft,sizeof ft);memcpy(queue_flags,ff,sizeof ff);
    }
    quiz_game_page(QUIZ_MODE_MUSIC);frame(200);
    assert(quiz_prepare_kind(QUIZ_MUSIC_FACT));
    assert(!strcmp(quiz.choices[0], "True") && !strcmp(quiz.choices[1], "False"));
    assert(quiz.choices[0][0] && quiz.choices[1][0] && !quiz.choices[2][0] && !quiz.choices[3][0]);
    const char *fact_artist = sl_lib.albums[sl_lib.tracks[quiz.track].album].artist;
    assert((quiz.answer == 0) == (strstr(quiz.fact_question, fact_artist) != NULL));
    assert(quiz_prepare_kind(QUIZ_YEAR));
    for(int i=0;i<4;i++)for(int j=i+1;j<4;j++)assert(strcmp(quiz.choices[i],quiz.choices[j]));
    if(quiz.clip.active){
        int changed_index=(quiz.clip.sample_index+1)%queued;
        started=changed_index;
        int selected_track=queue_tracks[started];
        quiz_restore_clip();
        assert(queue_tracks[started]==selected_track && !quiz.clip.active);
    }
    page_pop();frame(200);started=2;
    /* My Music must start while playback is stopped and rotate through
     * answer formats rather than requiring an already playing song. */
    test_status=0;
    global_status.resume_index=7; global_status.resume_crc32=0x1234;
    global_status.resume_elapsed=4321; global_status.resume_offset=0;
    int stopped_queue=queued;
    quiz_game_page(QUIZ_MODE_MUSIC);frame(200);
    assert(quiz.round==1 && quiz.clip.active && !quiz.clip.was_playing);
    assert(queued==stopped_queue+1 && test_status==AUDIO_STATUS_PLAY);
    /* audio_stop() records the clip as the resume point; the restore must
     * put the user's saved resume point back. */
    global_status.resume_index=99; global_status.resume_crc32=0xdead;
    int status_saves_before=status_saves;
    quiz_restore_clip();
    assert(!quiz_aa_released && test_aa_used==1);   /* the art slot is back for the music */
    assert(!quiz.clip.active && queued==stopped_queue && test_status==0);
    assert(global_status.resume_index==7 && global_status.resume_crc32==0x1234 &&
           global_status.resume_elapsed==4321 && status_saves==status_saves_before+1);
    bool heard_song=false, heard_fact=false;
    for(int i=0;i<5;i++){
        heard_song |= quiz.kind==QUIZ_SONG;
        heard_fact |= quiz.kind==QUIZ_MUSIC_FACT || quiz.kind==QUIZ_YEAR;
        if(quiz.kind==QUIZ_MUSIC_FACT)shot("quiz-truefalse.ppm");
        if(quiz.answered)break;
        lv_obj_send_event(quiz_choices[quiz.answer],LV_EVENT_CLICKED,NULL);
        quiz_next();frame(120);
    }
    assert(heard_song && heard_fact);
    /* Album covers: a sleeve on the stage for artist questions, one per row
     * for album questions; rows grow to fill the screen. */
    struct sl_album *fixture_albums=sl_lib.albums;int fixture_n_albums=sl_lib.n_albums,fixture_album_of[10];
    static struct sl_album five_albums[5]={
        {.title="Postcards at Dawn",.artist="Neon Harbor",.year=2024,.hue=200},
        {.title="Relapse: Refill",.artist="Eminem",.year=2009,.hue=120},
        {.title="Parachutes",.artist="Coldplay",.year=2000,.hue=40},
        {.title="Mirror Master",.artist="Young The Giant",.year=2018,.hue=300},
        {.title="Blue Hours",.artist="Mira Sol",.year=2021,.hue=250}};
    sl_lib.albums=five_albums;sl_lib.n_albums=5;
    for(int t=0;t<10;t++){fixture_album_of[t]=sl_lib.tracks[t].album;sl_lib.tracks[t].album=t%5;}
    memset(quiz.asked_music_kind,0,sizeof quiz.asked_music_kind);quiz.asked_music_count=0;
    quiz.qnum=quiz.strikes=0;quiz.answered=true;quiz.round=5;quiz_next();frame(400);
    int four_rows=(QUIZ_CHOICE_ROOM-8*3)/4;
    assert(quiz.kind==QUIZ_ARTIST && quiz_cover && lv_obj_get_height(quiz_choices[0])==four_rows);
    shot("quiz-artist.ppm");
    five_albums[1].title="Eminem";five_albums[0].title="Neon Harbor Live";
    for(int k=0;k<20;k++){
        quiz.asked_music_count=0;
        if(quiz_prepare_kind(QUIZ_ARTIST)){int al=sl_lib.tracks[quiz.track].album;assert(al!=0 && al!=1);}
    }
    five_albums[1].title="Relapse: Refill";five_albums[0].title="Postcards at Dawn";
    quiz.qnum=quiz.strikes=0;quiz.answered=true;quiz.round=7;quiz_next();frame(400);
    assert(quiz.kind==QUIZ_ALBUM && !quiz_cover);
    for(int c=0;c<4;c++)assert(quiz_choice_cover[c] && lv_obj_get_height(quiz_choice_cover[c])==SL_ART_ROW);
    shot("quiz-album.ppm");
    lv_area_t last_row;lv_obj_get_coords(quiz_choices[3],&last_row);assert(last_row.y2<H-8);
    quiz_restore_clip();
    sl_lib.albums=fixture_albums;sl_lib.n_albums=fixture_n_albums;
    for(int t=0;t<10;t++)sl_lib.tracks[t].album=fixture_album_of[t];
    page_pop();frame(200);test_status=0;started=2;
    int saved_tracks=sl_lib.n_tracks, saved_albums=sl_lib.n_albums;
    sl_lib.n_tracks=sl_lib.n_albums=0;test_status=0;
    SFX_RESET();quiz_game_page(QUIZ_MODE_MOVIES);frame(200);shot("music-quiz-trivia.ppm");
    assert(sfx_last==SFX_QUIZ_QUESTION);
    assert(quiz.kind==QUIZ_MOVIE && quiz.round==1 && !quiz.clip.active);
    assert((uint32_t)(quiz.question_deadline - sl_ms()) <= 10000);
    assert((uint32_t)(quiz.question_deadline - sl_ms()) >= 9900);
    int movie_questions = 0, tv_questions = 0;
    for (unsigned i = 0; i < sizeof quiz_trivia / sizeof quiz_trivia[0]; i++)
    {
        const struct quiz_trivia_item *item = &quiz_trivia[i];
        assert(item->kind == QUIZ_MOVIE || item->kind == QUIZ_TV);
        assert(item->question[0] && strlen(item->question) < 180);
        assert(item->answer >= 0 && item->answer < 4);
        for (int a = 0; a < 4; a++)
        {
            assert(item->choices[a] && item->choices[a][0] && strlen(item->choices[a]) < sizeof quiz.choices[0]);
            for (int b = 0; b < a; b++) assert(strcasecmp(item->choices[a], item->choices[b]));
        }
        for (unsigned j = 0; j < i; j++) assert(strcasecmp(item->question, quiz_trivia[j].question));
        if (quiz_trivia[i].kind == QUIZ_MOVIE) movie_questions++;
        else if (quiz_trivia[i].kind == QUIZ_TV) tv_questions++;
    }
    assert(movie_questions == 101 && tv_questions == 103);
    uint64_t seen_movie[QUIZ_TRIVIA_MAX_PER_MODE / 64] = {0};
    memset(quiz.asked_movie, 0, sizeof quiz.asked_movie);
    for (int i = 0; i < movie_questions; i++)
    {
        assert(quiz_prepare_kind(QUIZ_MOVIE));
        assert(!strcmp(quiz.choices[quiz.answer], quiz_trivia[quiz.trivia_index].choices[0]));
        int slot = 0;
        for (int j = 0; j < quiz.trivia_index; j++)
            if (quiz_trivia[j].kind == QUIZ_MOVIE) slot++;
        uint64_t bit = (uint64_t)1 << (slot & 63);
        assert(!(seen_movie[slot >> 6] & bit));
        seen_movie[slot >> 6] |= bit;
    }
    assert(!quiz_prepare_kind(QUIZ_MOVIE));
    uint64_t seen_tv[QUIZ_TRIVIA_MAX_PER_MODE / 64] = {0};
    memset(quiz.asked_tv, 0, sizeof quiz.asked_tv);
    for (int i = 0; i < tv_questions; i++)
    {
        assert(quiz_prepare_kind(QUIZ_TV));
        assert(!strcmp(quiz.choices[quiz.answer], quiz_trivia[quiz.trivia_index].choices[0]));
        int slot = 0;
        for (int j = 0; j < quiz.trivia_index; j++)
            if (quiz_trivia[j].kind == QUIZ_TV) slot++;
        uint64_t bit = (uint64_t)1 << (slot & 63);
        assert(!(seen_tv[slot >> 6] & bit));
        seen_tv[slot >> 6] |= bit;
    }
    assert(!quiz_prepare_kind(QUIZ_TV));
    quiz_next();
    assert(quiz.answered && quiz.question_deadline == 0);
    assert(lv_obj_has_flag(quiz_next_button, LV_OBJ_FLAG_HIDDEN));
    quiz_game_page(QUIZ_MODE_MOVIES);frame(200);
    /* the countdown runs on the real clock: move the deadline rather than
     * sleep through it, so a busy machine can't skip the third second */
    assert(quiz.question_deadline);
    quiz.question_deadline = sl_ms() + 2800;
    SFX_RESET();quiz_tick(NULL);assert(sfx_last==SFX_QUIZ_TICK && quiz_tick_second==3);
    uint32_t question_end = quiz.question_deadline = sl_ms() - 1;
    SFX_RESET();quiz_tick(NULL);assert(sfx_last==SFX_QUIZ_TIMEUP);
    assert(quiz.answered && quiz.question_deadline == question_end);
    assert(!lv_obj_has_flag(quiz_next_button,LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(quiz_choices[quiz.answer],LV_EVENT_CLICKED,NULL);
    assert(quiz.score==0 && !lv_obj_has_flag(quiz_next_button,LV_OBJ_FLAG_HIDDEN));
    quiz_next();assert(quiz.round==2 && quiz.kind>=QUIZ_MOVIE);
    SFX_RESET();lv_obj_send_event(quiz_choices[quiz.answer],LV_EVENT_CLICKED,NULL);assert(sfx_last==SFX_QUIZ_RIGHT);
    quiz_next();SFX_RESET();lv_obj_send_event(quiz_choices[(quiz.answer+1)%quiz.choice_count],LV_EVENT_CLICKED,NULL);
    assert(sfx_last==SFX_QUIZ_WRONG);
    page_pop();frame(200);
    /* The show: eight answers clear a round, the Wheel of Chance picks a
     * bonus, a wager rides on it, and three strikes end the game. */
    remove(QUIZ_STATS);
    memset(&quiz_stats,0,sizeof quiz_stats);
    quiz_game_page(QUIZ_MODE_MOVIES);frame(300);
    for(int i=0;i<QUIZ_PER_ROUND;i++){
        assert(quiz.phase==QP_PLAY && quiz.qnum==i+1 && !quiz.answered && quiz.stage==1);
        lv_obj_send_event(quiz_choices[quiz.answer],LV_EVENT_CLICKED,NULL);
        assert(quiz.pip[i]==1 && quiz.streak==i+1);
        if(i==2)shot("quiz-streak.ppm");
        if(i<QUIZ_PER_ROUND-1)quiz_next();
    }
    assert(quiz.correct==QUIZ_PER_ROUND && quiz.best_streak==QUIZ_PER_ROUND && quiz.score>=QUIZ_PER_ROUND*100);
    assert(!strncmp(lv_label_get_text(lv_obj_get_child(quiz_next_button,0)),"BONUS",5));
    for(int i=0;i<80 && quiz.shown_cash!=quiz.score;i++)frame(33);
    assert(quiz.shown_cash==quiz.score);
    int banked=quiz.score;
    quiz_next();
    assert(quiz.phase==QP_WHEEL && quiz_wheel && !lv_obj_has_flag(quiz_overlay,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(quiz_wager_grid,LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(lv_obj_get_child(quiz_wager_grid,3),LV_EVENT_CLICKED,NULL);
    assert(quiz.phase==QP_WHEEL && quiz.wager==0); /* no betting before the spin */
    shot("quiz-wheel.ppm");
    lv_obj_send_event(quiz_wheel_btn,LV_EVENT_CLICKED,NULL);
    assert(quiz.wheel_spinning && lv_obj_has_flag(quiz_wheel_btn,LV_OBJ_FLAG_HIDDEN));
    frame(700);frame(33);assert(quiz.wheel_rot!=quiz.wheel_from);
    for(int i=0;i<30 && quiz.wheel_spinning;i++)frame(200);
    assert(!quiz.wheel_spinning && quiz.wheel_seg>=0 && quiz.wheel_seg<8);
    assert(quiz.wheel_rot==quiz.wheel_to && quiz.wheel_to-quiz.wheel_from>=360*4);
    assert(!lv_obj_has_flag(quiz_wager_grid,LV_OBJ_FLAG_HIDDEN));
    shot("quiz-wheel-landed.ppm");
    quiz.wheel_seg=2; /* Lightning, x3 */
    lv_obj_send_event(lv_obj_get_child(quiz_wager_grid,3),LV_EVENT_CLICKED,NULL);
    assert(quiz.phase==QP_BONUS && quiz.wager==banked && quiz.bonus_n==1);
    assert(!strcmp(quiz.bonus_opt[0],"TRUE") && !strcmp(quiz.bonus_opt[1],"FALSE") && quiz.bonus_big[0]);
    shot("quiz-lightning.ppm");
    for(int i=0;i<QUIZ_BONUS;i++){
        assert(quiz.phase==QP_BONUS && quiz.bonus_n==i+1);
        lv_obj_send_event(quiz_bonus_btn[quiz.bonus_answer],LV_EVENT_CLICKED,NULL);
        assert(quiz.bonus_won);
        frame(400);frame(33);
    }
    assert(quiz.phase==QP_BONUS_DONE && quiz.score==banked*3);
    shot("quiz-bonus-win.ppm");
    lv_obj_send_event(lv_obj_get_child(quiz_overlay,-1),LV_EVENT_CLICKED,NULL);
    assert(quiz.phase==QP_PLAY && quiz.stage==2 && quiz.qnum==1 && quiz.strikes==0 && quiz.pip[1]==0);
    assert(lv_obj_has_flag(quiz_overlay,LV_OBJ_FLAG_HIDDEN));
    assert(quiz_payout(10000)==(200+800)*18/10); /* round 2 base, full speed, 8 in a row */
    assert(quiz_payout(0)==200*18/10);
    for(int i=0;i<QUIZ_STRIKES;i++){
        lv_obj_send_event(quiz_choices[(quiz.answer+1)%quiz.choice_count],LV_EVENT_CLICKED,NULL);
        assert(quiz.strikes==i+1 && quiz.streak==0 && quiz.pip[i]==2);
        if(i==1)shot("quiz-strikes.ppm");
        if(i<QUIZ_STRIKES-1)quiz_next();
    }
    assert(!strncmp(lv_label_get_text(lv_obj_get_child(quiz_next_button,0)),"RESULTS",7));
    int final_cash=quiz.score;
    quiz_next();
    assert(quiz.phase==QP_OVER && quiz.recorded && quiz.question_deadline==0);
    assert(quiz_stats.games==1 && quiz_stats.best_cash==final_cash && quiz_stats.best_streak==QUIZ_PER_ROUND);
    assert(quiz_stats.best_react>0 && quiz_stats.best_react<=10000);
    shot("quiz-game-over.ppm");
    memset(&quiz_stats,0,sizeof quiz_stats);quiz_stats_load();
    assert(quiz_stats.games==1 && quiz_stats.best_cash==final_cash && quiz_stats.best_stage==2);
    lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(quiz_overlay,-1),0),LV_EVENT_CLICKED,NULL); /* PLAY AGAIN */
    assert(quiz.phase==QP_PLAY && quiz.round==1 && quiz.score==0 && quiz.stage==1 && !quiz.recorded);
    assert(lv_obj_has_flag(quiz_overlay,LV_OBJ_FLAG_HIDDEN));
    /* A bust: This or That with half the bank riding on it. */
    quiz.qnum=QUIZ_PER_ROUND;quiz.answered=true;quiz.score=quiz.shown_cash=1000;
    quiz_next();assert(quiz.phase==QP_WHEEL);
    quiz_wheel_spin_cb(NULL);for(int i=0;i<30 && quiz.wheel_spinning;i++)frame(200);
    quiz.wheel_seg=5; /* This or That, x3 */
    lv_obj_send_event(lv_obj_get_child(quiz_wager_grid,2),LV_EVENT_CLICKED,NULL);
    assert(quiz.phase==QP_BONUS && quiz.wager==500);
    assert(quiz.bonus_opt[0][0] && quiz.bonus_opt[1][0] && strcasecmp(quiz.bonus_opt[0],quiz.bonus_opt[1]));
    shot("quiz-this-or-that.ppm");
    lv_obj_send_event(quiz_bonus_btn[!quiz.bonus_answer],LV_EVENT_CLICKED,NULL);
    assert(!quiz.bonus_won);
    lv_obj_send_event(quiz_bonus_btn[quiz.bonus_answer],LV_EVENT_CLICKED,NULL); /* too late */
    assert(!quiz.bonus_won);
    frame(1200);frame(33);
    assert(quiz.phase==QP_BONUS_DONE && quiz.score==500);
    shot("quiz-busted.ppm");
    /* Too slow in the bonus counts as a miss; passing skips it. */
    lv_obj_send_event(lv_obj_get_child(quiz_overlay,-1),LV_EVENT_CLICKED,NULL);
    assert(quiz.stage==2 && quiz.phase==QP_PLAY);
    quiz.qnum=QUIZ_PER_ROUND;quiz.answered=true;quiz_next();
    quiz_wheel_spin_cb(NULL);for(int i=0;i<30 && quiz.wheel_spinning;i++)frame(200);
    lv_obj_send_event(lv_obj_get_child(quiz_wager_grid,1),LV_EVENT_CLICKED,NULL);
    assert(quiz.phase==QP_BONUS && quiz.wager==120);
    frame(4100);frame(33);assert(!quiz.bonus_won && quiz.bonus_wait);
    frame(1200);frame(33);assert(quiz.phase==QP_BONUS_DONE && quiz.score==380);
    lv_obj_send_event(lv_obj_get_child(quiz_overlay,-1),LV_EVENT_CLICKED,NULL);
    quiz.qnum=QUIZ_PER_ROUND;quiz.answered=true;quiz_next();
    quiz_wheel_spin_cb(NULL);for(int i=0;i<30 && quiz.wheel_spinning;i++)frame(200);
    lv_obj_send_event(lv_obj_get_child(quiz_wager_grid,0),LV_EVENT_CLICKED,NULL);
    assert(quiz.phase==QP_PLAY && quiz.stage==4 && quiz.score==380 && quiz.wager==0);
    /* Clearing round five crowns a champion. */
    quiz.stage=QUIZ_ROUNDS;quiz.qnum=QUIZ_PER_ROUND;quiz.answered=true;quiz_next();
    quiz_wheel_spin_cb(NULL);for(int i=0;i<30 && quiz.wheel_spinning;i++)frame(200);
    lv_obj_send_event(lv_obj_get_child(quiz_wager_grid,0),LV_EVENT_CLICKED,NULL);
    assert(quiz.phase==QP_OVER && quiz_stats.wins==1 && quiz_stats.games==2);
    shot("quiz-champion.ppm");
    page_pop();frame(200);
    assert(quiz_stats.games==2); /* already counted: leaving does not count it twice */
    quiz_page();frame(300);shot("quiz-menu-stats.ppm");page_pop();frame(200);
    sl_lib.n_tracks=saved_tracks;sl_lib.n_albums=saved_albums;test_status=AUDIO_STATUS_PLAY;
    /* Library bonus statements need no playback. */
    quiz.rng=7;quiz.bonus_n=0;quiz.mode=QUIZ_MODE_MUSIC;
    for(int i=0;i<8;i++)quiz.bonus_used[i]=-1;
    for(int i=0;i<20;i++){
        assert(quiz_bonus_music(true) && quiz.bonus_prompt[0] && quiz.bonus_big[0]);
        assert(!strcmp(quiz.bonus_opt[0],"TRUE") && (quiz.bonus_answer==0 || quiz.bonus_answer==1));
        assert(quiz_bonus_music(false) && quiz.bonus_opt[0][0] && strcasecmp(quiz.bonus_opt[0],quiz.bonus_opt[1]));
    }
    const char *old_queue_title=sl_lib.tracks[1].title,*old_queue_artist=sl_lib.albums[0].artist;
    sl_lib.tracks[1].title="רגעים";sl_lib.albums[0].artist="שולה חן";
    const char *old_long_title=sl_lib.tracks[3].title;
    sl_lib.tracks[3].title="A Very Long Title That Must Not Wrap Into The Artist Line (Deluxe Edition)";
    global_settings.repeat_mode=REPEAT_ALL;uq_show();frame(500);shot("queue.ppm");
    assert(!has_text(uq,"Drag") && !has_text(uq,"Swipe left to queue,"));   /* the rows speak for themselves */
    lv_obj_scroll_to_y(uq_list,160,LV_ANIM_OFF);frame(50);
    int queue_scroll=lv_obj_get_scroll_y(uq_list);
    uq_build();frame(50);
    assert(lv_obj_get_scroll_y(uq_list)==queue_scroll);
    for(int i=0;i<uq_queue_count;i++){
        lv_obj_t *qt=lv_obj_get_child(uq_queue_rows[i],1);lv_obj_update_layout(qt);
        assert(lv_obj_get_height(qt)<=lv_font_get_line_height(&ss_semibold_26)); /* one line, dotted */
    }
    sl_lib.tracks[3].title=old_long_title;
    {   /* pulling Up Next down: the sheet follows the finger, a nudge springs
         * back, a long pull or a flick closes it, and a scrolled list or a
         * drag handle keeps its own gesture */
        lv_obj_scroll_to_y(uq_list,0,LV_ANIM_OFF);frame(50);
        touch_at(240,400,true);for(int i=1;i<=3;i++)touch_at(240,400+20*i,true);
        assert(g_state==G_SHEET && lv_obj_get_style_translate_y(uq,0)==60);
        frame(200);touch_at(240,460,false);for(int i=0;i<10;i++)frame(40);
        assert(uq_open && lv_obj_get_style_translate_y(uq,0)==0);
        touch_at(240,300,true);for(int i=1;i<=10;i++)touch_at(240,300+25*i,true);
        assert(lv_obj_get_style_translate_y(uq,0)==250);
        frame(200);touch_at(240,550,false);for(int i=0;i<10;i++)frame(40);
        assert(!uq_open && lv_obj_has_flag(uq,LV_OBJ_FLAG_HIDDEN));
        uq_show();frame(500);lv_obj_scroll_to_y(uq_list,0,LV_ANIM_OFF);frame(50);
        touch_at(240,120,true);touch_at(240,160,true);touch_at(240,210,true);touch_at(240,210,false);  /* a flick on the header */
        for(int i=0;i<10;i++)frame(40);
        assert(!uq_open);
        uq_show();frame(500);lv_obj_scroll_to_y(uq_list,160,LV_ANIM_OFF);frame(50);
        touch_at(240,400,true);for(int i=1;i<=4;i++)touch_at(240,400+30*i,true);
        assert(g_state==G_LVGL && lv_obj_get_style_translate_y(uq,0)==0);
        touch_at(240,520,false);for(int i=0;i<10;i++)frame(40);
        assert(uq_open);
        lv_obj_scroll_to_y(uq_list,0,LV_ANIM_OFF);frame(50);
        lv_area_t ha;lv_obj_get_coords(lv_obj_get_child(uq_queue_rows[1],lv_obj_get_child_count(uq_queue_rows[1])-1),&ha);
        int hx=(ha.x1+ha.x2)/2,hy=(ha.y1+ha.y2)/2;
        touch_at(hx,hy,true);for(int i=1;i<=3;i++)touch_at(hx,hy+15*i,true);
        assert(g_state!=G_SHEET && uq_dragging && lv_obj_get_style_translate_y(uq,0)==0);
        touch_at(hx,hy,true);touch_at(hx,hy,false);for(int i=0;i<10;i++)frame(40);
        assert(uq_open && !uq_dragging);
        uq_build();frame(50);
        /* the music stops mid-drag: Up next goes from under the finger, and the next drag still works */
        lv_obj_get_coords(lv_obj_get_child(uq_queue_rows[1],-1),&ha);hx=(ha.x1+ha.x2)/2;hy=(ha.y1+ha.y2)/2;
        touch_at(hx,hy,true);for(int i=1;i<=3;i++)touch_at(hx,hy+15*i,true);assert(uq_dragging);
        {   int st=test_status;test_status=0;periodic_ui();for(int i=0;i<10;i++)frame(40);assert(!uq_open);
            touch_at(hx,hy+45,false);for(int i=0;i<10;i++)frame(40);
            test_status=st;uq_show();frame(500);assert(uq_open&&!uq_dragging&&uq_queue_count==5);
            lv_obj_get_coords(lv_obj_get_child(uq_queue_rows[1],-1),&ha);hx=(ha.x1+ha.x2)/2;hy=(ha.y1+ha.y2)/2;
            touch_at(hx,hy,true);for(int i=1;i<=3;i++)touch_at(hx,hy+15*i,true);assert(uq_dragging);
            touch_at(hx,hy,true);touch_at(hx,hy,false);for(int i=0;i<10;i++)frame(40);assert(!uq_dragging);
            uq_build();frame(50);}
    }
    assert(uq_queue_count==5);uq_clear_cb(NULL);frame(200);
    assert(queued==1 && started==0 && queue_tracks[0]==2);
    uq_build();frame(300);shot("queue-empty.ppm");   /* one empty state under Now playing */
    assert(has_text(uq,"Nothing up next") && !has_text(uq,"Swipe") && !has_text(uq,"NEXT UP") && !has_text(uq,"IN YOUR QUEUE"));   /* one empty state, not two */
    {   /* Earlier: the songs heard last, above Now playing, opened with Now playing at the top */
        int was_n=hist_recent_n;int was[HIST_RECENT];memcpy(was,hist_recent_t,sizeof was);
        uint32_t now=hist_clock(),wasw[HIST_RECENT];memcpy(wasw,hist_recent_w,sizeof wasw);
        /* by day, kept with when they were heard: three days ago, yesterday, today */
        hist_recent_n=0;hist_recent_push(7,now-3*86400);hist_recent_push(5,now-86400);hist_recent_push(6,now-60);
        uq_close();frame(500);uq_show();frame(500);
        assert(has_text(uq,"TODAY") && has_text(uq,"YESTERDAY") && find_label(uq,sl_lib.tracks[6].title) && find_label(uq,sl_lib.tracks[7].title));
        {   time_t tw=(time_t)(now-3*86400);struct tm a;localtime_r(&tw,&a);static const char *const dn[]={"SUNDAY","MONDAY","TUESDAY","WEDNESDAY","THURSDAY","FRIDAY","SATURDAY"};
            assert(has_text(uq,dn[a.tm_wday])); }
        lv_obj_scroll_to_y(uq_list,0,LV_ANIM_OFF);frame(50);shot("queue-earlier.ppm");
        lv_obj_t *er=find_label(uq,sl_lib.tracks[6].title);while(er&&!lv_obj_has_flag(er,LV_OBJ_FLAG_CLICKABLE))er=lv_obj_get_parent(er);
        int q0=queued;lv_obj_send_event(er,LV_EVENT_CLICKED,NULL);frame(100);assert(queued==q0+1);   /* plays again next */
        /* a hold: the song's sheet, Add to queue puts it at the end */
        uq_build();frame(100);er=find_label(uq,sl_lib.tracks[5].title);while(er&&!lv_obj_has_flag(er,LV_OBJ_FLAG_CLICKABLE))er=lv_obj_get_parent(er);
        lv_obj_send_event(er,LV_EVENT_LONG_PRESSED,NULL);frame(300);assert(acts_open_now && has_text(acts,"Play next"));
        acts_close();frame(300);
        /* its handle dragged down between two queued songs: it lands there */
        uq_build();frame(100);
        {   int qc=uq_queue_count;assert(qc>=1);
            er=find_label(uq,sl_lib.tracks[7].title);while(er&&!lv_obj_has_flag(er,LV_OBJ_FLAG_CLICKABLE))er=lv_obj_get_parent(er);
            lv_obj_t *h=lv_obj_get_child(er,-1);lv_area_t ha,qa;lv_obj_get_coords(h,&ha);
            lv_obj_scroll_to_view(uq_queue_rows[0],LV_ANIM_OFF);frame(50);lv_obj_get_coords(h,&ha);lv_obj_get_coords(uq_queue_rows[0],&qa);
            int hx=(ha.x1+ha.x2)/2,hy=(ha.y1+ha.y2)/2,ty=qa.y1+8;   /* just inside the first queued row's top half */
            int before=q_run();
            touch_at(hx,hy,true);assert(uq_dragging);
            for(int i=1;i<=10;i++){touch_at(hx,hy+(ty-hy)*i/10,true);frame(20);}
            assert(uq_hist_at==0);frame(50);shot("queue-earlier-drag.ppm");
            touch_at(hx,ty,false);frame(300);
            assert(!uq_dragging && q_run()==before+1);
            struct playlist_track_info inf;assert(playlist_get_track_info(NULL,q_at(1),&inf)>=0 && !strcmp(inf.filename,TP(7)));   /* first in the queue */
        }
        hist_recent_n=was_n;memcpy(hist_recent_t,was,sizeof was);memcpy(hist_recent_w,wasw,sizeof wasw);
    }
    uq_close();frame(500);np_hide(0);frame(500);
    sl_lib.tracks[1].title=old_queue_title;sl_lib.albums[0].artist=old_queue_artist;
    /* Spotify-style swipes through the real touch path: left queues the
     * song, right opens Add to playlist; short swipes and scrolls do nothing. */
    {
        char pl_dir_path[MAX_PATH],mix[MAX_PATH],mine[MAX_PATH],mine2[MAX_PATH];
        strlcpy(pl_dir_path,sl_playlist_dir(),sizeof pl_dir_path);
        const char *sep=pl_dir_path[strlen(pl_dir_path)-1]=='/'?"":"/";
        snprintf(mix,sizeof mix,"%s%smix.m3u8",pl_dir_path,sep);
        snprintf(mine,sizeof mine,"%s%sMy playlist.m3u8",pl_dir_path,sep);
        snprintf(mine2,sizeof mine2,"%s%sMy playlist 2.m3u8",pl_dir_path,sep);
        const char *seed="#EXTM3U\n#EXTINF:1,Someone - Else\n/Music/elsewhere.mp3";   /* no final newline */
        int fd=open(mix,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);
        assert(write(fd,seed,strlen(seed))==(ssize_t)strlen(seed));close(fd);
        songs_page();frame(500);
        lv_obj_t *row=lv_indev_search_obj(lv_screen_active(),&(lv_point_t){200,300});
        assert(row && lv_obj_has_flag(row,LV_OBJ_FLAG_PRESS_LOCK));
        int track=(int)(intptr_t)lv_obj_get_user_data(row);
        int before=queued,was_started=started;
        lv_area_t home;lv_obj_get_coords(row,&home);
        /* half a swipe arms nothing and springs back; it starts where the
         * swipe was recognised, without a jump */
        touch_at(300,300,true);touch_at(285,300,true);
        assert(sw_state==SW_DRAG && sw_offset==0);
        for(int i=1;i<=4;i++)touch_at(285-12*i,300,true);
        {lv_area_t now;lv_obj_get_coords(row,&now);
         assert(!sw_armed && sw_offset==-48 && now.x1==home.x1-48 && sw_tint>0 && sw_tint<SW_TINTS);}
        /* a relayout that snaps the row home is corrected on the next move */
        lv_obj_mark_layout_as_dirty(lv_obj_get_parent(row));lv_obj_update_layout(row);
        touch_at(236,300,true);{lv_area_t now;lv_obj_get_coords(row,&now);assert(now.x1==home.x1-49);}
        touch_at(236,300,false);for(int i=0;i<8;i++)frame(40);
        {lv_area_t now;lv_obj_get_coords(row,&now);assert(now.x1==home.x1 && now.x2==home.x2);}
        assert(sw_state==SW_IDLE && !sw_reveal && sw_offset==0);
        assert(queued==before && started==was_started && !np_open);
        /* a long swipe left, held past the long-press time: armed, no song sheet */
        touch_at(330,300,true);for(int i=1;i<=8;i++)touch_at(330-25*i,300,true);
        frame(500);assert(sw_armed && sw_side<0 && !acts_open_now && sw_tint==SW_TINTS+1);
        /* the rubber band keeps giving, ever less, and never passes its limit */
        {int prev=sw_offset;touch_at(40,300,true);assert(sw_offset<prev && sw_offset>-(SW_TRIGGER+SW_STRETCH));
         touch_at(-400,300,true);assert(sw_offset>-(SW_TRIGGER+SW_STRETCH));touch_at(130,300,true);}
        shot_now("swipe-queue.ppm");
        touch_at(130,300,false);for(int i=0;i<10;i++)frame(40);
        assert(queued==before+1 && queue_tracks[queued-1]==track && queue_flags[queued-1]);
        assert(started==was_started && !np_open && sw_state==SW_IDLE && !sw_reveal);
        /* a vertical drag scrolls, never swipes */
        lv_obj_t *body=lv_obj_get_child(pages[npages-1],1);
        int y_before=lv_obj_get_scroll_y(body);
        touch_at(200,500,true);for(int i=1;i<=8;i++)touch_at(200+i,500-30*i,true);touch_at(208,260,false);frame(300);
        assert(lv_obj_get_scroll_y(body)>y_before && sw_state==SW_IDLE && !sw_reveal && queued==before+1);
        lv_obj_scroll_to_y(body,0,LV_ANIM_OFF);frame(100);
        /* a quick flick commits from 60% of the way; the same distance
         * dragged slowly, or stopped before letting go, does not */
        before=queued;
        touch_at(300,300,true);touch_at(286,300,true);
        for(int i=1;i<=4;i++)touch_at(286-20*i,300,true);            /* 500 px/s */
        touch_at(206,300,false);for(int i=0;i<10;i++)frame(40);
        assert(queued==before && sw_state==SW_IDLE);
        touch_at(300,300,true);touch_at(286,300,true);
        touch_at(236,300,true);touch_at(186,300,true);                /* 1250 px/s */
        assert(!sw_armed && -sw_offset>=SW_TRIGGER*3/5);
        touch_at(186,300,false);for(int i=0;i<10;i++)frame(40);
        assert(queued==before+1 && sw_state==SW_IDLE);
        touch_at(300,300,true);touch_at(286,300,true);
        touch_at(236,300,true);touch_at(186,300,true);
        for(int i=0;i<6;i++)touch_at(186,300,true);                  /* hold still */
        touch_at(186,300,false);for(int i=0;i<10;i++)frame(40);
        assert(queued==before+1 && sw_state==SW_IDLE);
        /* a touch the system takes away mid-swipe (screen lock) sends no
         * release: the row still goes home */
        lv_obj_get_coords(lv_indev_search_obj(lv_screen_active(),&(lv_point_t){200,300}),&home);
        touch_at(300,300,true);touch_at(286,300,true);touch_at(236,300,true);
        assert(sw_state==SW_DRAG);
        lv_indev_reset(touch,NULL);touch_at(236,300,false);
        for(int i=0;i<12;i++)frame(40);
        assert(sw_state==SW_IDLE && !sw_reveal && queued==before+1);
        {lv_area_t now;lv_obj_get_coords(lv_indev_search_obj(lv_screen_active(),&(lv_point_t){200,300}),&now);assert(now.x1==home.x1);}
        row=lv_indev_search_obj(lv_screen_active(),&(lv_point_t){200,300});
        track=(int)(intptr_t)lv_obj_get_user_data(row);
        /* swipe right: the playlist sheet lists mix with its one song */
        touch_at(100,300,true);for(int i=1;i<=8;i++)touch_at(100+25*i,300,true);
        frame(100);assert(sw_armed && sw_side>0);shot_now("swipe-playlist.ppm");
        assert(!pl_visible());
        touch_at(300,300,false);for(int i=0;i<10;i++)frame(40);
        assert(pl_visible() && pl_count==1 && pl_entries[0].scanned);
        assert(pl_entries[0].songs==1 && !pl_entries[0].has && pl_entries[0].extm3u);
        assert(!strcmp(lv_label_get_text(pl_subs[0]),"1 song"));
        assert(queued==before+1 && started==was_started && !np_open && !acts_open_now);
        shot("playlist-sheet.ppm");
        lv_obj_t *mix_row=lv_obj_get_parent(find_label(pl_sheet,"mix"));
        g_dragged=false;lv_obj_send_event(mix_row,LV_EVENT_CLICKED,NULL);frame(400);
        assert(!pl_visible());
        {   char want[512];snprintf(want,sizeof want,"%s\n#EXTINF:240,%s - %s\n%s\n",seed,
                sl_lib.albums[0].artist,sl_lib.tracks[track].title,TP(track));
            assert(fake_equals(mix,want)); }
        /* the same song again: marked as already added, the file is left alone */
        pl_open(track);
        assert(pl_count==1 && !pl_entries[0].scanned && !strcmp(lv_label_get_text(pl_subs[0]),"…"));
        frame(100);assert(!pl_entries[0].scanned);            /* not while sliding in */
        frame(300);
        assert(pl_count==1 && pl_entries[0].songs==2 && pl_entries[0].has);
        assert(!strcmp(lv_label_get_text(pl_subs[0]),"Already added · 2 songs"));
        long mix_size=fake_size(mix);
        mix_row=lv_obj_get_parent(find_label(pl_sheet,"mix"));
        lv_obj_send_event(mix_row,LV_EVENT_CLICKED,NULL);frame(100);
        assert(pl_visible() && fake_size(mix)==mix_size);
        /* New playlist: named, created with this song as its first */
        lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet,"New playlist")),LV_EVENT_CLICKED,NULL);frame(300);
        assert(pl_input && !strcmp(lv_textarea_get_text(pl_input),"My playlist"));
        shot("playlist-new.ppm");
        lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet,"Create")),LV_EVENT_CLICKED,NULL);frame(400);
        assert(!pl_visible());
        {   char want[512];snprintf(want,sizeof want,"#EXTM3U\n#EXTINF:240,%s - %s\n%s\n",
                sl_lib.albums[0].artist,sl_lib.tracks[track].title,TP(track));
            assert(fake_equals(mine,want)); }
        pl_open(track);frame(300);
        lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet,"New playlist")),LV_EVENT_CLICKED,NULL);frame(100);
        assert(!strcmp(lv_textarea_get_text(pl_input),"My playlist 2"));
        lv_textarea_set_text(pl_input,"My playlist");   /* taken */
        lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet,"Create")),LV_EVENT_CLICKED,NULL);frame(100);
        assert(pl_visible() && fake_find(mine2)<0);
        /* a taken name asks: Replace, Add to it, or Back to typing (#83) */
        assert(!pl_input && find_label(pl_sheet,"Replace it?") && find_label(pl_sheet,"Replace") && find_label(pl_sheet,"Add to it"));
        lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet,"Back")),LV_EVENT_CLICKED,NULL);frame(100);
        assert(pl_input && !strcmp(lv_textarea_get_text(pl_input),"My playlist"));
        lv_textarea_set_text(pl_input," ./:  ");       /* nothing usable */
        lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet,"Create")),LV_EVENT_CLICKED,NULL);frame(100);
        assert(pl_visible());
        lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet,"Cancel")),LV_EVENT_CLICKED,NULL);frame(100);
        assert(!pl_input && find_label(pl_sheet,"mix"));
        pl_close();frame(400);assert(!pl_entries && !pl_count);
        /* tapping a playlist before its scan comes round scans it first */
        pl_open(track);lv_obj_send_event(lv_obj_get_parent(find_label(pl_sheet,"mix")),LV_EVENT_CLICKED,NULL);
        assert(pl_visible() && pl_entries[0].scanned && pl_entries[0].has);pl_close();frame(400);
        { char n[64];pl_clean_name(n,sizeof n,"  ..Road/Trip: 2*  mix.. ");assert(!strcmp(n,"RoadTrip 2 mix")); }
        { char n[6];pl_clean_name(n,sizeof n,"ab\xd7\xa9\xd7\xa9\xd7\xa9");assert(!strcmp(n,"ab\xd7\xa9"));   /* whole letters */
          pl_display_name(n,sizeof n,"x\xe2\x80\x94\xe2\x80\x94.m3u");assert(!strcmp(n,"x\xe2\x80\x94")); }
        assert(pl_same("song.mp3","/Music/A/song.mp3") && pl_same("./A/song.mp3","/Music/A/song.mp3"));
        assert(!pl_same("ong.mp3","/Music/A/song.mp3") && !pl_same("/Other/song.mp3","/Music/A/song.mp3"));
        /* the song menu offers Add to playlist too */
        acts_open(track);frame(300);assert(find_label(acts,"Add to playlist"));
        acts_close();frame(300);
        {   /* the sheet pulled down: it follows the finger, its shade fading, a nudge springs
             * back, a long pull or a flick closes it, and a tap still picks a row */
            acts_open(track);frame(400);
            lv_area_t sa;lv_obj_get_coords(acts,&sa);int sx=W/2,sy=sa.y1+150;
            touch_at(sx,sy,true);for(int i=1;i<=3;i++)touch_at(sx,sy+20*i,true);
            assert(g_state==G_SHEET&&lv_obj_get_style_translate_y(acts,0)==60&&lv_obj_get_style_bg_opa(acts_scrim,0)<110);
            frame(200);touch_at(sx,sy+60,false);for(int i=0;i<10;i++)frame(40);
            assert(acts_open_now&&lv_obj_get_style_translate_y(acts,0)==0&&lv_obj_get_style_bg_opa(acts_scrim,0)==110);
            int far=lv_obj_get_height(acts)/4+30;
            touch_at(sx,sy,true);for(int i=1;i<=10;i++)touch_at(sx,sy+far*i/10,true);
            assert(lv_obj_get_style_translate_y(acts,0)==far);
            frame(200);touch_at(sx,sy+far,false);for(int i=0;i<10;i++)frame(40);
            assert(!acts_open_now&&lv_obj_has_flag(acts,LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(acts_scrim,LV_OBJ_FLAG_HIDDEN));
            acts_open(track);frame(400);
            touch_at(sx,sy,true);touch_at(sx,sy+40,true);touch_at(sx,sy+90,true);touch_at(sx,sy+90,false);   /* a flick */
            for(int i=0;i<10;i++)frame(40);
            assert(!acts_open_now&&lv_obj_has_flag(acts,LV_OBJ_FLAG_HIDDEN));
            acts_open(track);frame(400);
            lv_obj_get_coords(lv_obj_get_parent(find_label(acts,"Song info")),&sa);
            int n0=npages;touch_at((sa.x1+sa.x2)/2,(sa.y1+sa.y2)/2,true);touch_at((sa.x1+sa.x2)/2,(sa.y1+sa.y2)/2,false);frame(800);
            assert(!acts_open_now&&npages==n0+1&&find_label(pages[npages-1],"Song info"));
            page_pop();frame(500);
            /* Go to album on the album's own page, and Go to artist on the artist's:
             * the sheet closes there, no second copy of the page over it */
            album_page(sl_lib.tracks[track].album);frame(600);n0=npages;
            acts_open(track);frame(400);lv_obj_send_event(lv_obj_get_parent(find_label(acts,"Go to album")),LV_EVENT_CLICKED,NULL);frame(800);
            assert(!acts_open_now&&npages==n0);
            acts_open(track);frame(400);lv_obj_send_event(lv_obj_get_parent(find_label(acts,"Go to artist")),LV_EVENT_CLICKED,NULL);frame(800);
            assert(!acts_open_now&&npages==n0+1);   /* another page: it opens */
            acts_open(track);frame(400);lv_obj_send_event(lv_obj_get_parent(find_label(acts,"Go to artist")),LV_EVENT_CLICKED,NULL);frame(800);
            assert(!acts_open_now&&npages==n0+1);
            page_pop();frame(500);
            {   int st0=test_status;struct mp3entry tt0=test_track;   /* and from Now Playing over the album */
                test_status=AUDIO_STATUS_PLAY;sl_track_path(track,test_track.path,sizeof test_track.path);
                np_show();frame(600);lv_obj_send_event(np_more,LV_EVENT_CLICKED,NULL);frame(400);
                lv_obj_send_event(lv_obj_get_parent(find_label(acts,"Go to album")),LV_EVENT_CLICKED,NULL);frame(800);
                assert(!np_open&&!acts_open_now&&npages==n0);
                test_status=st0;test_track=tt0;frame(300);
            }
            page_pop();frame(500);
            acts_open(track);frame(300);acts_close();frame(300);   /* the list's sheet again, as the tests below read it */
        }
        {   /* Split on changed with Artists, an artist and Genres under Settings (the control centre's Library tile):
             * the lists are made again a moment later, so pages counting by the old ones go, not read past their end */
            int n0=npages,aw=global_settings.sloppy_artists_whole;
            tartists_page();frame(400);assert(tg_ready()&&tgx.listed>0);
            tartist_page(tgx.order[0]);frame(400);genres_page();frame(400);
            sl_library_page();frame(400);library_options_page();frame(400);
            lv_obj_t *lo=pages[npages-1];assert(npages==n0+5);
            lo_split_artists(global_settings.sloppy_artists_whole);frame(100);   /* the switch: the other way */
            assert(npages==n0+2&&pages[npages-1]==lo);
            for(int i=n0;i<npages;i++){intptr_t u=(intptr_t)lv_obj_get_user_data(pages[i]);assert(u!=PG_TAGS&&(u&~0xffffff)!=PG_TARTIST);}
            for(int f=0;f<20;f++)frame(100);tg_ready();   /* made again, as the idle loop does */
            page_pop();frame(400);page_pop();frame(400);assert(npages==n0);
            global_settings.sloppy_artists_whole=aw;tg_ready();
        }
        assert(!find_label(acts,"Playback"));   /* a list's sheet: no settings in it */
        /* Now Playing's dots (P22): the song's sheet, Playback's settings at its end */
        {   int st0=test_status;struct mp3entry tt0=test_track;
            test_status=AUDIO_STATUS_PLAY;sl_track_path(track,test_track.path,sizeof test_track.path);
            np_show();frame(600);
            assert(np_love_track==track && !lv_obj_has_flag(np_more,LV_OBJ_FLAG_HIDDEN));
            lv_obj_send_event(np_more,LV_EVENT_CLICKED,NULL);frame(400);
            assert(find_label(acts,"Go to album") && find_label(acts,"Song info") && find_label(acts,"Add to playlist"));
            lv_obj_t *pb=find_label(acts,"Playback");assert(pb);
            int n0=npages;
            lv_obj_send_event(lv_obj_get_parent(pb),LV_EVENT_CLICKED,NULL);frame(800);
            assert(!np_open && npages==n0+1 && find_label(pages[npages-1],"Playback"));
            /* again, over that Playback page: back to it, not a second one */
            np_show();frame(600);lv_obj_send_event(np_more,LV_EVENT_CLICKED,NULL);frame(400);
            lv_obj_send_event(lv_obj_get_parent(find_label(acts,"Playback")),LV_EVENT_CLICKED,NULL);frame(800);
            assert(!np_open && npages==n0+1 && pages[npages-1]==xf_graph_owner && xf_graph_timer);
            page_pop();frame(500);
            acts_open(track);frame(300);assert(!find_label(acts,"Playback"));acts_close();frame(300);
            /* T2.4 #11: twenty pauses and resumes, wired and over a pretend
             * Bluetooth delay: the bar, its time and its pixels stay where
             * they were (the song moves on only while it plays) */
            test_track.length=240000;
            np_show();frame(600);assert(np_open);
            for (int bt=0;bt<2;bt++)
            {
                sl_test_out_latency_ms=bt?180:0;
                for (int k=0;k<20;k++)
                {
                    test_status=AUDIO_STATUS_PLAY;test_track.elapsed=20000+k*1500+bt*40000;frame(300);
                    lv_area_t a;lv_obj_get_coords(np_seek,&a);
                    assert(a.y1>0 && a.y2<H);   /* on the screen */
                    int y0=a.y1-12,y1=a.y2+12>=H?H-1:a.y2+12;
                    static uint16_t strip[W*80];assert((y1-y0+1)<=80);
                    memcpy(strip,&pixels[y0*W],(size_t)(y1-y0+1)*W*2);
                    int v=lv_slider_get_value(np_seek);char t[16];snprintf(t,sizeof t,"%s",lv_label_get_text(np_t0));
                    for (int side=0;side<2;side++)
                    {
                        toggle_play();frame(300);
                        assert((audio_status()&AUDIO_STATUS_PAUSE)==(side==0?AUDIO_STATUS_PAUSE:0));
                        assert(lv_slider_get_value(np_seek)==v && !strcmp(lv_label_get_text(np_t0),t));
                        assert(!memcmp(strip,&pixels[y0*W],(size_t)(y1-y0+1)*W*2));
                    }
                }
            }
            sl_test_out_latency_ms=0;
            np_hide(0);frame(400);
            test_status=st0;test_track=tt0;frame(200);
            printf("PASS: Now Playing's bar stays put over 40 pauses, wired and Bluetooth (#11)\n");
            test_status=st0;test_track=tt0;frame(200);
            printf("PASS: Now Playing's dots open the song's sheet with Playback's settings (P22)\n");
        }
        page_pop();frame(500);
        /* inside Up Next: swipe a Next Up row left to queue it, the playing row right */
        {   /* queued songs play first, and a new list keeps them */
            int list[]={5,6,7,8},other[]={9,10,11};
            audio_stop();assert(play_list(list,4,0,false));
            q_add(20);q_add(21);q_play_next(22);
            assert(queue_tracks[1]==22 && queue_tracks[2]==20 && queue_tracks[3]==21 && queue_tracks[4]==6);
            assert(queue_flags[1] && queue_flags[3] && !queue_flags[4]);
            assert(play_list(other,3,1,false) && queue_tracks[started]==10);
            assert(queue_tracks[started+1]==22 && queue_tracks[started+2]==20 && queue_tracks[started+3]==21);
            uq_show();frame(500);assert(uq_queue_count==3 && uq_next_count==(global_settings.repeat_mode==REPEAT_ALL?2:1));
            uq_close();frame(500);
            audio_stop();assert(play_list(list,4,0,false));
        }
        uq_show();frame(500);
        assert(uq_next_count==3 && uq_queue_count==0);
        int queued_rows=uq_queue_count;before=queued;
        lv_obj_t *next_row=uq_next_rows[0];lv_area_t na;lv_obj_get_coords(next_row,&na);
        int ny=(na.y1+na.y2)/2;
        touch_at(300,ny,true);for(int i=1;i<=8;i++)touch_at(300-25*i,ny,true);
        frame(100);assert(sw_armed && sw_side<0);shot_now("swipe-in-queue.ppm");
        touch_at(100,ny,false);for(int i=0;i<12;i++)frame(40);
        assert(queued==before+1 && uq_queue_count==queued_rows+1 && sw_state==SW_IDLE);
        /* the list rebuilding under a held swipe (a track change) drops it cleanly */
        lv_obj_get_coords(uq_next_rows[0],&na);ny=(na.y1+na.y2)/2;before=queued;
        touch_at(300,ny,true);for(int i=1;i<=8;i++)touch_at(300-25*i,ny,true);
        assert(sw_state==SW_DRAG && sw_reveal);
        uq_build();assert(sw_state==SW_IDLE && !sw_row && !sw_reveal);
        touch_at(100,ny,false);for(int i=0;i<10;i++)frame(40);
        assert(queued==before && sw_state==SW_IDLE);
        lv_obj_t *now_row=lv_obj_get_child(uq_list,1);lv_area_t nr;lv_obj_get_coords(now_row,&nr);
        int ry=(nr.y1+nr.y2)/2;
        /* a press the system took away (no release) must not cost the playing
         * row its highlight when the next row is touched */
        assert(lv_obj_get_style_bg_opa(now_row,0)==LV_OPA_20);
        touch_at(200,ry,true);assert(sw_state==SW_TRACK && sw_row==now_row);
        lv_indev_reset(touch,NULL);touch_at(200,ry,false);
        touch_at(200,ny,true);assert(sw_state==SW_TRACK && sw_row!=now_row);
        assert(lv_obj_get_style_bg_opa(now_row,0)==LV_OPA_20);
        lv_indev_reset(touch,NULL);touch_at(200,ny,false);frame(100);
        touch_at(100,ry,true);for(int i=1;i<=8;i++)touch_at(100+25*i,ry,true);touch_at(300,ry,false);
        for(int i=0;i<10;i++)frame(40);
        assert(pl_visible() && pl_track==queue_tracks[started]);
        pl_close();frame(400);uq_close();frame(500);
        /* cost of a frame while a finger drags a row, in a full Up Next
         * (60 Next Up + 128 queued rows) and in the 10000-song list */
        {   static int list[300];for(int i=0;i<300;i++)list[i]=i;
            assert(play_list(list,200,0,false));
            for(int i=0;i<128;i++)playlist_insert_track(NULL,TP(250+(i%40)),PLAYLIST_INSERT,true,true);
            uq_show();for(int i=0;i<400&&uq_next_count<60;i++)frame(100);assert(uq_queue_count==uq_queue_run && uq_queue_run>=128 && uq_next_count==60);   /* every queued row: no 128 cap */   /* the queue made as it sits still, then Next up */
            lv_obj_scroll_to_y(uq_list,1050,LV_ANIM_OFF);frame(50);
            lv_area_t handle_area,list_area;
            lv_obj_get_coords(lv_obj_get_child(uq_queue_rows[13],
                              lv_obj_get_child_count(uq_queue_rows[13])-1),&handle_area);
            lv_obj_get_coords(uq_list,&list_area);
            int handle_x=(handle_area.x1+handle_area.x2)/2;
            int handle_y=(handle_area.y1+handle_area.y2)/2;
            int before_scroll=lv_obj_get_scroll_y(uq_list);
            touch_at(handle_x,handle_y,true);
            assert(uq_dragging && uq_drag_slot==13);
            touch_at(handle_x,list_area.y1+20,true);
            for(int i=0;i<12;i++)frame(30);
            assert(lv_obj_get_scroll_y(uq_list)<before_scroll && uq_drag_target<uq_drag_slot);
            touch_at(handle_x,list_area.y1+20,false);
            for(int i=0;i<8;i++)frame(40);
            assert(!uq_dragging);
            lv_obj_scroll_to_y(uq_list,0,LV_ANIM_OFF);frame(50);
            lv_area_t qa;lv_obj_get_coords(uq_queue_rows[1],&qa);int qy=(qa.y1+qa.y2)/2;
            struct timespec t0,t1;
            touch_at(300,qy,true);touch_at(280,qy,true);touch_at(260,qy,true);
            clock_gettime(CLOCK_MONOTONIC,&t0);
            for(int i=0;i<60;i++)touch_at(260-(i%30)*4,qy,true);
            clock_gettime(CLOCK_MONOTONIC,&t1);
            double drag_us=((t1.tv_sec-t0.tv_sec)*1e9+(t1.tv_nsec-t0.tv_nsec))/1e3/60;
            touch_at(250,qy,false);for(int i=0;i<10;i++)frame(40);
            assert(sw_state==SW_IDLE);
            clock_gettime(CLOCK_MONOTONIC,&t0);
            for(int i=0;i<60;i++){lv_obj_invalidate(uq_queue_rows[1]);touch_at(250,qy+300,false);}
            clock_gettime(CLOCK_MONOTONIC,&t1);
            double still_us=((t1.tv_sec-t0.tv_sec)*1e9+(t1.tv_nsec-t0.tv_nsec))/1e3/60;
            printf("up next swipe: %.0f us per dragged frame, %.0f us per plain row repaint (%d rows)\n",drag_us,still_us,uq_queue_count+uq_next_count);
            /* past a thousand queued: the first thousand shown, and it says so */
            for(int i=uq_queue_run;i<UQ_QMAX+5;i++)playlist_insert_track(NULL,TP(300+(i%40)),PLAYLIST_INSERT,true,true);
            uq_build();frame(100);assert(uq_queue_run>UQ_QMAX && has_text(uq,"The first 1,000 queued songs are shown"));
            uq_close();frame(500);
            songs_page();frame(500);
            touch_at(300,300,true);touch_at(280,300,true);touch_at(260,300,true);
            clock_gettime(CLOCK_MONOTONIC,&t0);
            for(int i=0;i<60;i++)touch_at(260-(i%30)*4,300,true);
            clock_gettime(CLOCK_MONOTONIC,&t1);
            drag_us=((t1.tv_sec-t0.tv_sec)*1e9+(t1.tv_nsec-t0.tv_nsec))/1e3/60;
            touch_at(250,300,false);for(int i=0;i<10;i++)frame(40);
            printf("song list swipe: %.0f us per dragged frame\n",drag_us);
            page_pop();frame(500);
        }
    }
    /* Render an actual five-choice song round with RTL, mixed-script and
     * punctuation-heavy titles. Keep the indexed library fixture intact. */
    const char *hebrew_titles[] = {
        "רגעים", "יום חדש - New Day", "אור, ים ושמש?", "שלום (Live)",
        "The Road - הדרך", "שיר קטן: אהבה!"
    };
    const char *saved_hebrew_titles[6];
    for (int i=0;i<6;i++) {
        saved_hebrew_titles[i]=sl_lib.tracks[i].title;
        sl_lib.tracks[i].title=hebrew_titles[i];
    }
    int hebrew_track_count=sl_lib.n_tracks;
    /* Keep the playback fixture independent of this round. */
    int hebrew_queued=queued,hebrew_started=started,hebrew_status=test_status;
    test_status=0;
    int hebrew_queue[8];bool hebrew_flags[8];
    memcpy(hebrew_queue,queue_tracks,sizeof hebrew_queue);memcpy(hebrew_flags,queue_flags,sizeof hebrew_flags);
    queued=6;started=2;for(int i=0;i<6;i++){queue_tracks[i]=i;queue_flags[i]=false;}
    sl_lib.n_tracks=6;
    quiz_game_page(QUIZ_MODE_MUSIC);frame(300);
    quiz.round=1;quiz_next();frame(300);
    assert(quiz.kind==QUIZ_SONG && quiz.choice_count==5 && quiz.clip.active);
    bool rtl_choice=false,mixed_choice=false;
    for(int i=0;i<5;i++) {
        lv_obj_t *label=lv_obj_get_child(quiz_choices[i],0);
        assert(lv_obj_get_style_base_dir(label,0)==LV_BASE_DIR_AUTO);
        if(strstr(quiz.choices[i],"רגעים")) rtl_choice=true;
        if(strstr(quiz.choices[i],"New Day") || strstr(quiz.choices[i],"Live")) mixed_choice=true;
    }
    assert(rtl_choice || mixed_choice);
    shot("hebrew-quiz-choices.ppm");
    lv_obj_send_event(quiz_choices[quiz.answer],LV_EVENT_CLICKED,NULL);
    assert(quiz.clip.hushed && test_status==0);
    quiz_restore_clip();assert(!quiz.clip.active && test_status==0);
    page_pop();frame(200);
    queued=hebrew_queued;started=hebrew_started;test_status=hebrew_status;
    memcpy(queue_tracks,hebrew_queue,sizeof hebrew_queue);memcpy(queue_flags,hebrew_flags,sizeof hebrew_flags);
    /* Long, mixed-direction and punctuation-heavy titles take at most two
     * lines per choice, inside it, and every choice must be reachable. */
    const char *long_titles[] = {
        "A Very Long English Song Title That Keeps Going (Remastered 2011)",
        "\u05e9\u05d9\u05e8 \u05d0\u05e8\u05d5\u05da \u05de\u05d0\u05d5\u05d3 \u05e2\u05dd \u05d4\u05e8\u05d1\u05d4 \u05de\u05d9\u05dc\u05d9\u05dd \u05e9\u05dc\u05d0 \u05e0\u05d2\u05de\u05e8\u05d5\u05ea \u05d0\u05e3 \u05e4\u05e2\u05dd",
        "Hallelujah (Live) - \u05d4\u05dc\u05dc\u05d5\u05d9\u05d4 \u05d1\u05d4\u05d5\u05e4\u05e2\u05d4 \u05d7\u05d9\u05d4 2019",
        "\u05de\u05e1\u05e4\u05e8 123 \u05d5-English (feat. Someone) [Remix]",
        "\"Quotes\", commas; & ampersands... dashes -- and?!",
        "\u05d0\u05d1\u05d2/ABC: \u05e9\u05d9\u05e8 (\u05d2\u05e8\u05e1\u05d4 \u05de\u05d5\u05e8\u05d7\u05d1\u05ea)"
    };
    for (int i=0;i<6;i++) sl_lib.tracks[i].title=long_titles[i];
    queued=6;started=2;for(int i=0;i<6;i++){queue_tracks[i]=i;queue_flags[i]=false;}
    test_status=0;
    quiz_game_page(QUIZ_MODE_MUSIC);frame(300);
    quiz.round=1;quiz_next();frame(300);
    assert(quiz.kind==QUIZ_SONG && quiz.choice_count==5 && quiz.clip.active);
    for(int i=0;i<5;i++) {
        lv_obj_t *label=lv_obj_get_child(quiz_choices[i],0);
        lv_obj_update_layout(label);
        assert(lv_obj_get_height(label)<=lv_obj_get_height(quiz_choices[i])-10); /* inside its row */
        assert(lv_obj_get_x(label)+lv_obj_get_width(label)<=lv_obj_get_width(quiz_choices[i]));
    }
    shot("quiz-long-titles.ppm");
    lv_obj_t *long_body=lv_obj_get_parent(quiz_choices[4]);
    lv_obj_scroll_to_y(long_body,LV_COORD_MAX,LV_ANIM_OFF);frame(100);
    lv_area_t last_choice,next_area;
    lv_obj_get_coords(quiz_choices[4],&last_choice);
    assert(last_choice.y2 < H-8 && lv_obj_has_flag(mini,LV_OBJ_FLAG_HIDDEN)); /* fills the screen; no mini player */
    lv_obj_send_event(quiz_choices[quiz.answer],LV_EVENT_CLICKED,NULL);frame(100);
    lv_obj_scroll_to_y(long_body,LV_COORD_MAX,LV_ANIM_OFF);frame(100);
    lv_obj_get_coords(quiz_next_button,&next_area);
    assert(next_area.y2 < H-8);
    shot("quiz-long-titles-bottom.ppm");
    assert(quiz.clip.hushed || !quiz.clip.active);
    page_pop();frame(200);
    sl_lib.n_tracks=hebrew_track_count;
    for(int i=0;i<6;i++)sl_lib.tracks[i].title=saved_hebrew_titles[i];
    /* ==== quiz-fix: every question whole, with the longest tags and
     * prompts, at the largest Text size, in both themes. ==== */
    {
        static const char *const fix_album_titles[6]={
            "Supercalifragilistic Midnight Symphonies for the Broken-Hearted (Deluxe Anniversary Edition)",
            "שירים ארוכים מאוד על אהבה, ים ושמש בלילות הקיץ (חדש)",
            "أغاني طويلة جدا عن الحب والبحر والشمس في ليالي الصيف",
            "Wonderfulwonderfulwonderfulwonderfulwonderfulwonderfulwonderfulwonderfulwonderfulwonderful12",
            "Hallelujah (Live) - הללויה בהופעה חיה בקיסריה עם התזמורת 2019",
            "Short"};
        static const char *const fix_artists[6]={
            "The Unbelievably Long-Named Orchestra and Choir of Somewhere Far Away feat. Everyone Else",
            "فرقة الأوركسترا الوطنية والجوقة الكبرى مع الضيوف",
            "אהבה, ים ושמש בלילות הקיץ החמים עם להקת הנמל",
            "Neon Harbor","Mira Sol",
            /* 118 bytes: too long to quote whole, so never asked about */
            "שירים ארוכים מאוד על אהבה, ים ושמש בלילות הקיץ החמים (גרסה מורחבת)"};
        static const char *const fix_titles[6]={
            "A Very Long English Song Title That Keeps Going and Going Forever (Remastered 2011 Mix)",
            "שיר ארוך מאוד עם הרבה מילים שלא נגמרות אף פעם",
            "أغنية طويلة جدا عن الليل والقمر والنجوم (مباشر)",
            "\"Quotes\", commas; & ampersands... dashes -- and?! (feat. Somebody With a Long Name)",
            "מספר 123 ו-English (feat. Someone) [Extended Club Remix]",
            "Never Asked"};
        static struct sl_album fix_albums[6];static struct sl_track fix_tracks[6];static int fix_album_tracks[6];
        static char fix_paths[6][24];
        for(int i=0;i<6;i++){
            assert(strlen(fix_album_titles[i])<sizeof quiz.choices[0] && strlen(fix_titles[i])<sizeof quiz.choices[0]);
            fix_album_tracks[i]=i;
            fix_albums[i]=(struct sl_album){.title=fix_album_titles[i],.artist=fix_artists[i],.year=1990+i*4,
                                            .n_tracks=1,.hue=i*60,.tracks=&fix_album_tracks[i]};
            snprintf(fix_paths[i],sizeof fix_paths[i],"/Music/fix%d.flac",i);
            fix_tracks[i]=(struct sl_track){.file=fix_paths[i],.title=fix_titles[i],.album=i,.num=1,.len=200000};tsplit(&fix_tracks[i]);
        }
        struct sl_track *fix_saved_t=sl_lib.tracks;struct sl_album *fix_saved_a=sl_lib.albums;
        int fix_saved_nt=sl_lib.n_tracks,fix_saved_na=sl_lib.n_albums;
        int fix_queued=queued,fix_started=started,fix_status=test_status,fix_queue[8];bool fix_flags[8];
        memcpy(fix_queue,queue_tracks,sizeof fix_queue);memcpy(fix_flags,queue_flags,sizeof fix_flags);
        int longest=0;
        for(unsigned i=1;i<sizeof quiz_trivia/sizeof quiz_trivia[0];i++)
            if(strlen(quiz_trivia[i].question)>strlen(quiz_trivia[longest].question))longest=(int)i;
        int fails=0,asked=0;bool seen[QUIZ_KIND_COUNT]={false};
        static const char *const kind_names[QUIZ_KIND_COUNT]={"song","artist","album","year","movie","tv","truefalse"};
        static const struct { int theme, step; } passes[]={{0,3},{0,0},{3,3}};   /* Zule: theme 3 */
        for(unsigned pass=0;pass<sizeof passes/sizeof passes[0];pass++){
            int theme=passes[pass].theme,step=passes[pass].step;
            global_settings.sloppy_theme=theme;sl_text_step=step;
            appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}
            sl_lib.tracks=fix_tracks;sl_lib.n_tracks=6;sl_lib.albums=fix_albums;sl_lib.n_albums=6;
            assert(strlen(fix_artists[5])>=sizeof quiz.choices[0] && !quiz_track_ok(5) && quiz_track_ok(4));
            queued=6;started=2;test_status=0;for(int i=0;i<6;i++){queue_tracks[i]=i;queue_flags[i]=false;}
            char what[64],name[64];
            /* Library questions: each music kind in turn, then the answer shown */
            quiz_game_page(QUIZ_MODE_MUSIC);frame(300);
            for(int r=0;r<10;r++){
                quiz.asked_music_count=quiz.asked_song_count=0;quiz.no_clip=false;
                quiz.qnum=quiz.strikes=0;quiz.answered=true;quiz.round=r;quiz_next();frame(600);
                assert(quiz.phase==QP_PLAY && !quiz.answered);
                seen[quiz.kind]=true;asked++;
                snprintf(what,sizeof what,"theme %d size %d %s",theme,step,kind_names[quiz.kind]);
                fails+=quizfix_check(what);
                /* the subject is quoted whole; the tag too long for that is never used */
                const char *subject=quiz.kind==QUIZ_ARTIST||quiz.kind==QUIZ_YEAR?fix_albums[sl_lib.tracks[quiz.track].album].title:
                                    quiz.kind==QUIZ_ALBUM||quiz.kind==QUIZ_MUSIC_FACT?sl_lib.tracks[quiz.track].title:NULL;
                if(subject&&((unsigned char)subject[0]<0xd8||(unsigned char)subject[0]>0xdb))   /* Arabic is stored joined */
                    if(!strstr(lv_label_get_text(quiz_question),subject)){printf("quiz-fix: %s: subject cut\n",what);fails++;}
                if(quiz.kind!=QUIZ_SONG)assert(quiz.track!=5);
                for(int i=0;i<quiz.choice_count;i++)assert(strcmp(quiz.choices[i],"Never Asked")&&strcmp(quiz.choices[i],"Short"));
                if(r<5&&pass!=1){snprintf(name,sizeof name,"quiz-fix-%s-%s.ppm",theme?"zule":"largest",kind_names[quiz.kind]);shot(name);}
                if(r==9){   /* answered: NEXT shows, the question stays clear of it */
                    lv_obj_send_event(quiz_choices[quiz.answer],LV_EVENT_CLICKED,NULL);frame(100);
                    snprintf(what,sizeof what,"theme %d size %d answered",theme,step);fails+=quizfix_check(what);
                }
                quiz_restore_clip();
            }
            /* Bonus rounds: Lightning (true/false claims) and This or That */
            for(int b=0;b<2;b++){
                quiz.wheel_seg=b?5:2;quiz.wager=500;quiz.qnum=QUIZ_PER_ROUND;quiz_bonus_start();frame(400);
                for(int k=0;k<10&&quiz.phase==QP_BONUS;k++){
                    snprintf(what,sizeof what,"theme %d size %d %s %d",theme,step,b?"this-or-that":"lightning",k);
                    fails+=quizfix_bonus(what);
                    if(k==0&&pass!=1){snprintf(name,sizeof name,"quiz-fix-%s-%s.ppm",theme?"zule":"largest",b?"this-or-that":"lightning");shot(name);}
                    quiz.bonus_n=0;for(int i=0;i<8;i++)quiz.bonus_used[i]=-1;
                    quiz_bonus_item_next();frame(400);
                }
            }
            page_pop();frame(300);
            /* Every movie and TV prompt, the longest among them */
            for(int m=0;m<2;m++){
                int kind=m?QUIZ_TV:QUIZ_MOVIE;
                quiz_game_page(m?QUIZ_MODE_TV:QUIZ_MODE_MOVIES);frame(300);
                uint64_t *asked_bits=m?quiz.asked_tv:quiz.asked_movie;
                for(unsigned i=0,slot=0;i<sizeof quiz_trivia/sizeof quiz_trivia[0];i++){
                    if(quiz_trivia[i].kind!=kind)continue;
                    memset(asked_bits,0xff,sizeof quiz.asked_movie);asked_bits[slot>>6]&=~((uint64_t)1<<(slot&63));slot++;
                    quiz.qnum=quiz.strikes=0;quiz.answered=true;quiz_next();frame(600);   /* the wipe done */
                    assert(quiz.trivia_index==(int)i && !strcmp(lv_label_get_text(quiz_question),quiz_trivia[i].question));
                    seen[kind]=true;asked++;
                    snprintf(what,sizeof what,"theme %d size %d trivia %u",theme,step,i);
                    fails+=quizfix_check(what);
                    if((int)i==longest&&pass!=1){frame(600);snprintf(name,sizeof name,"quiz-fix-%s-longest-trivia.ppm",theme?"zule":"largest");shot(name);}
                }
                page_pop();frame(300);
            }
            sl_lib.tracks=fix_saved_t;sl_lib.n_tracks=fix_saved_nt;sl_lib.albums=fix_saved_a;sl_lib.n_albums=fix_saved_na;
        }
        for(int k=0;k<QUIZ_KIND_COUNT;k++)assert(seen[k]);
        printf("quiz-fix: %d questions over %d passes, %d not shown whole\n",asked,(int)(sizeof passes/sizeof passes[0]),fails);
        assert(fails==0);
        queued=fix_queued;started=fix_started;test_status=fix_status;
        memcpy(queue_tracks,fix_queue,sizeof fix_queue);memcpy(queue_flags,fix_flags,sizeof fix_flags);
        global_settings.sloppy_theme=0;sl_text_step=0;appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}
        if(getenv("SLOPPY_QUIZ_PERF")){   /* frames while questions come in and their clocks run down */
            sl_text_step=1;quiz_game_page(QUIZ_MODE_MOVIES);frame(400);
            pf_begin("quiz questions");
            for(int q=0;q<6;q++){quiz.qnum=quiz.strikes=0;quiz.answered=true;quiz_next();for(int i=0;i<120;i++)pf_frame(16);}
            pf_end();
            page_pop();frame(400);sl_text_step=0;
        }
    }
    /* ==== end quiz-fix ==== */
    sl_power_overlay_show(true);shot("locked.ppm");
    { /* clock centered on top, the player card at the bottom */
        lv_area_t ck,cd;lv_obj_get_coords(lp_clock,&ck);lv_obj_get_coords(lp_card,&cd);
        assert(ck.y1<220 && abs((ck.x1+ck.x2)/2-W/2)<=2 && cd.y2>H-80 && cd.y1>ck.y2);
        assert(lv_label_get_text(lp_clock)[2]==':' || lv_label_get_text(lp_clock)[1]==':');
        /* the digits start under the date, at every text size (the device photo had them touching) */
        int step0=sl_text_step;
        for(int st=0;st<SL_TEXT_STEPS;st++){
            sl_text_step=st;lv_obj_set_style_text_font(lp_date,sl_fonts[st][2],0);lp_clock_update();lv_obj_update_layout(sl_power_overlay);
            lv_area_t da,ca;lv_obj_get_coords(lp_date,&da);lv_obj_get_coords(lp_clock,&ca);
            const lv_font_t *cf=lv_obj_get_style_text_font(lp_clock,0);lv_font_glyph_dsc_t g;assert(lv_font_get_glyph_dsc(cf,&g,'0',0));
            int digit_top=ca.y1+(lv_font_get_line_height(cf)-cf->base_line)-(g.box_h+g.ofs_y);
            assert(digit_top>=da.y2+4);}
        sl_text_step=step0;lv_obj_set_style_text_font(lp_date,&ss_semibold_21,0);lp_clock_update();
    }
    sl_power_overlay_show(false);
    assert(lv_obj_get_child_count(sl_power_overlay)<=5); /* swipe bar, date, clock, optional player and its big art */
    sl_lib.n_albums=5000;sl_lib.n_artists=5000;
    sl_lib.albums=calloc(5000,sizeof(*sl_lib.albums));
    sl_lib.albums_by_title=calloc(5000,sizeof(int));
    sl_lib.albums_by_artist=calloc(5000,sizeof(int));
    for(int i=0;i<5000;i++) {
        char name[40];snprintf(name,sizeof name,"Artist %04d",i);
        sl_lib.albums[i].artist=strdup(name);sl_lib.albums[i].title="One album";
        sl_lib.albums[i].hue=i%360;sl_lib.albums[i].genre=i%3?"Rock":"Jazz";sl_lib.albums[i].year=1990+i%30;
        sl_lib.albums_by_title[i]=sl_lib.albums_by_artist[i]=i;
    }
    albums_page();frame(500);shot("albums-scroll.ppm");assert(objects(pages[npages-1])<250);
    albums_sort=2;albums_page_at(0,true);frame(500);assert(find_label(pages[npages-1],"5,000 ALBUMS") && find_label(pages[npages-1],"Year"));
    {   /* by Year, a rail of decades (the years run 1990 to 2019): a tap on 90 goes to the 1990s */
        struct az_state *r=NULL;lv_obj_t *pg=pages[npages-1];
        for(uint32_t i=0;i<lv_obj_get_child_count(pg)&&!r;i++){lv_obj_t *c=lv_obj_get_child(pg,i);
            for(uint32_t k=0;k<lv_obj_get_event_count(c);k++){lv_event_dsc_t *d=lv_obj_get_event_dsc(c,k);if(lv_event_dsc_get_cb(d)==az_rail_cb)r=lv_event_dsc_get_user_data(d);}}
        assert(r && r->nb==3 && !strcmp(r->lab[0],"10") && !strcmp(r->lab[2],"90") && !strcmp(r->big[2],"1990s"));
        shot("albums-year-rail.ppm");
        lv_area_t a;lv_obj_get_coords(r->rail,&a);int y=a.y1+lv_area_get_height(&a)*5/6;
        touch_at(a.x1+10,y,true);frame(50);touch_at(a.x1+10,y,false);frame(200);
        int first=(lv_obj_get_scroll_y(album_view->body)-lv_obj_get_y(album_view->host))/av_row()*av_cols();
        assert(sl_lib.albums[albums_order()[first]].year/10==199);
    }
    albums_sort=0;
    /* Genres and Years: every genre and year with its album count, each opening a paged grid */
    genres_page();frame(500);shot("genres.ppm");assert(find_label(pages[npages-1],"Jazz") && find_label(pages[npages-1],"3,333 albums"));
    group_page(0x30000);frame(500);shot("genre-albums.ppm");assert(find_label(pages[npages-1],"1,667 ALBUMS") && objects(pages[npages-1])<250);
    page_pop();frame(400);page_pop();frame(400);
    years_page();frame(500);shot("years.ppm");assert(find_label(pages[npages-1],"2019"));
    group_page(0x40000|2019);frame(500);assert(find_label(pages[npages-1],"166 ALBUMS"));
    page_pop();frame(400);page_pop();frame(400);
    {   /* search past four albums: a "See all" row, and every match on its own page */
        search_page();frame(300);sq_text[0]=0;sq_add("A");sq_add("l");sq_add("b");frame(400);
        assert(find_label(sq_body,"ALBUMS"));
        lv_obj_t *sa=NULL;for(unsigned i=0;i<lv_obj_get_child_count(sq_body)&&!sa;i++){lv_obj_t *c=lv_obj_get_child(sq_body,i);
            if(lv_obj_get_child_count(c)&&lv_obj_check_type(lv_obj_get_child(c,0),&lv_label_class)&&strstr(lv_label_get_text(lv_obj_get_child(c,0)),"albums")&&strstr(lv_label_get_text(lv_obj_get_child(c,0)),"See all"))sa=c;}
        assert(sa && sq_n_all_albums>4);shot("search-see-all.ppm");
        int depth=npages;lv_obj_send_event(sa,LV_EVENT_CLICKED,NULL);frame(500);assert(npages==depth+1);
        shot("search-all-albums.ppm");page_pop();frame(400);page_pop();frame(400);
    }
    {   /* Audiobooks: the mod starts on first open; the book being heard leads with Resume and
         * Start over; each row says how far; the arrows read the folder again, silently */
        static const char *const ti[3]={"The Hobbit","Dune","Atomic Habits"},*const au[3]={"J. R. R. Tolkien","Frank Herbert","James Clear"};
        fake_books_n=3;
        for(int i=0;i<3;i++){fake_books[i].id=11*(i+1);fake_books[i].length=3600000u*(5+i*3);fake_book_str[i][AB_S_TITLE]=ti[i];fake_book_str[i][AB_S_AUTHOR]=au[i];fake_prog[i].id=fake_books[i].id;}
        fake_prog_on[0]=true;fake_prog[0].position=fake_books[0].length*43/100;
        fake_prog_on[1]=true;fake_prog[1].finished=1;
        books_page();frame(500);
        assert(ab_inits>=1 && find_label(pages[npages-1],"CONTINUE LISTENING") && find_label(pages[npages-1],"3 BOOKS"));
        assert(has_text(pages[npages-1],"Frank Herbert \xc2\xb7 Finished") && has_text(pages[npages-1],"43% \xc2\xb7 2 h 51 min left"));
        shot("audiobooks.ppm");
        lv_obj_t *row=find_label(pages[npages-1],"Atomic Habits");while(row&&!lv_obj_has_flag(row,LV_OBJ_FLAG_CLICKABLE))row=lv_obj_get_parent(row);
        assert(row);int bd=npages;lv_obj_send_event(row,LV_EVENT_CLICKED,NULL);frame(400);assert(npages==bd+1);   /* its own page */
        assert(find_label(pages[npages-1],"3 CHAPTERS") && find_label(pages[npages-1],"02 Arrakis"));
        lv_obj_t *pl=find_label(pages[npages-1],LV_SYMBOL_PLAY "  Play");while(pl&&!lv_obj_has_flag(pl,LV_OBJ_FLAG_CLICKABLE))pl=lv_obj_get_parent(pl);
        assert(pl);lv_obj_send_event(pl,LV_EVENT_CLICKED,NULL);frame(300);assert(ab_plays==4);   /* book 2, from where it is */
        if(np_open){np_hide(0);frame(500);}
        fake_books[0].nfiles=3;fake_prog[0].file=1;bk_book_page(0);frame(400);shot("audiobook-page.ppm");
        assert(has_text(pages[npages-1],"You are here"));
        lv_obj_t *ch=find_label(pages[npages-1],"03 The Desert");while(ch&&!lv_obj_has_flag(ch,LV_OBJ_FLAG_CLICKABLE))ch=lv_obj_get_parent(ch);
        assert(ch);lv_obj_send_event(ch,LV_EVENT_CLICKED,NULL);frame(300);assert(ab_plays==0 && ab_play_file==2);
        if(np_open){np_hide(0);frame(500);}
        page_pop();frame(400);page_pop();frame(400);books_page();frame(400);
        ab_plays=-1;bk_play(0,true);assert(ab_plays==1);   /* start over */
        if(np_open){np_hide(0);frame(500);}
        bk_scan_cb(NULL);frame(500);assert(ab_scans==1 && !ab_scan_silent && find_label(pages[npages-1],"3 BOOKS"));
        fake_books_n=0;books_page_at(0,true);frame(500);assert(find_label(pages[npages-1],"No audiobooks on the card yet."));
        shot("audiobooks-empty.ppm");page_pop();frame(500);
    }
    {   /* Batch 8: a book's bookmarks (the mod's .bmark, Rockbox's own format with a title),
         * its speed, finished or not, and the ways into the library */
        static const char *const ti[3]={"The Hobbit","Dune","Atomic Habits"},*const au[3]={"J. R. R. Tolkien","Frank Herbert","James Clear"};
        static const char *const pa[3]={"/Audiobooks/The Hobbit","/Audiobooks/Dune","/Audiobooks/Atomic Habits.m4b"};
        fake_books_n=3;
        for(int i=0;i<3;i++){memset(&fake_prog[i],0,sizeof fake_prog[i]);fake_prog_on[i]=false;fake_books[i].id=11*(i+1);fake_books[i].length=3600000u*(5+i*3);
            fake_books[i].nfiles=i==2?1:3;fake_books[i].single=i==2;
            fake_book_str[i][AB_S_TITLE]=ti[i];fake_book_str[i][AB_S_AUTHOR]=au[i];fake_book_str[i][AB_S_PATH]=pa[i];fake_prog[i].id=fake_books[i].id;}
        fake_book_str[1][AB_S_SERIES]="Dune Chronicles";fake_book_str[1][AB_S_SERIES_NO]="1";
        fake_prog_on[0]=true;fake_prog[0].position=fake_books[0].length*43/100;fake_prog[0].last_played=100;
        /* another book's line in the same file stays as it was */
        {int fd=open("/Audiobooks/Dune.bmark",O_WRONLY|O_CREAT|O_TRUNC,0666);const char *o=">3;0;0;0;5000;0;0;10000;10000;/Audiobooks/Dune/;Not This Book.mp3\n";write(fd,o,strlen(o));sim_close(fd);}
        char tp0[sizeof test_track.path];strlcpy(tp0,test_track.path,sizeof tp0);unsigned long te0=test_track.elapsed;int ts0=test_status;
        test_status=AUDIO_STATUS_PLAY;test_book_playing=true;strlcpy(test_track.path,fake_files[1],sizeof test_track.path);test_track.elapsed=754000;
        bk_book_page(1);frame(400);
        assert(find_label(pages[npages-1],"Add a bookmark") && has_text(pages[npages-1],"02 Arrakis \xc2\xb7 12:34"));
        assert(has_text(pages[npages-1],"No bookmarks yet.") && has_text(pages[npages-1],"Dune Chronicles \xc2\xb7 Book 1"));
        lv_obj_t *add=find_label(pages[npages-1],"Add a bookmark");while(add&&!lv_obj_has_flag(add,LV_OBJ_FLAG_CLICKABLE))add=lv_obj_get_parent(add);
        lv_obj_send_event(add,LV_EVENT_CLICKED,NULL);frame(300);
        assert(bk_nbm==1 && bk_bms[0].file==1 && bk_bms[0].ms==754000);
        assert(fake_has("/Audiobooks/Dune.bmark",">7;1;0;0;754000;0;0;10000;10000;02 Arrakis;/Audiobooks/Dune/;02 Arrakis.mp3\n"));
        assert(fake_has("/Audiobooks/Dune.bmark","Not This Book.mp3"));
        add=find_label(pages[npages-1],"Add a bookmark");while(add&&!lv_obj_has_flag(add,LV_OBJ_FLAG_CLICKABLE))add=lv_obj_get_parent(add);
        lv_obj_send_event(add,LV_EVENT_CLICKED,NULL);frame(300);assert(bk_nbm==1);   /* the same place is bookmarked once */
        test_track.elapsed=95000;strlcpy(test_track.path,fake_files[0],sizeof test_track.path);
        add=find_label(pages[npages-1],"Add a bookmark");while(add&&!lv_obj_has_flag(add,LV_OBJ_FLAG_CLICKABLE))add=lv_obj_get_parent(add);
        lv_obj_send_event(add,LV_EVENT_CLICKED,NULL);frame(300);
        assert(bk_nbm==2 && bk_bms[0].file==0 && bk_bms[1].file==1);   /* in the book's order */
        shot("audiobook-bookmarks.ppm");
        page_pop();frame(400);bk_book_page(1);frame(400);assert(bk_nbm==2);   /* read back from the file */
        lv_obj_t *bm=find_label(pages[npages-1],"02 Arrakis");while(bm&&!lv_obj_has_flag(bm,LV_OBJ_FLAG_CLICKABLE))bm=lv_obj_get_parent(bm);
        assert(bm);ab_plays=-1;lv_obj_send_event(bm,LV_EVENT_CLICKED,NULL);frame(300);assert(ab_plays==2 && ab_play_file==1 && ab_play_ms==754000);
        if(np_open){np_hide(0);frame(500);}
        bm=find_label(pages[npages-1],"01 Prologue");while(bm&&!lv_obj_has_flag(bm,LV_OBJ_FLAG_CLICKABLE))bm=lv_obj_get_parent(bm);
        lv_obj_send_event(lv_obj_get_child(bm,-1),LV_EVENT_CLICKED,NULL);frame(300);   /* its delete button */
        assert(bk_nbm==1 && bk_bms[0].file==1 && !fake_has("/Audiobooks/Dune.bmark","01 Prologue") && fake_has("/Audiobooks/Dune.bmark","Not This Book"));
        /* sixty-four, and no more */
        for(int k=0;k<70;k++){test_track.elapsed=1000u*(k+1);bk_bm_add_cb(NULL);}
        frame(300);assert(bk_nbm==64);
        while(bk_nbm){lv_obj_t *r=lv_obj_get_child(bk_bm_group,-1);lv_obj_send_event(lv_obj_get_child(r,-1),LV_EVENT_CLICKED,NULL);frame(50);}
        assert(fake_find("/Audiobooks/Dune.bmark")>=0 && fake_has("/Audiobooks/Dune.bmark","Not This Book") && !fake_has("/Audiobooks/Dune.bmark","02 Arrakis"));
        /* its speed: kept for the book, heard now as it plays */
        lv_obj_t *ch=find_label(pages[npages-1],"1.5x");while(ch&&!lv_obj_has_flag(ch,LV_OBJ_FLAG_CLICKABLE))ch=lv_obj_get_parent(ch);
        int saves=ab_saves;lv_obj_send_event(ch,LV_EVENT_CLICKED,NULL);frame(200);
        assert(fake_prog[1].speed==150*PITCH_SPEED_PRECISION && dsp_get_timestretch()==150*PITCH_SPEED_PRECISION && ab_saves>saves);
        dsp_set_timestretch(PITCH_SPEED_100);
        /* finished, and back */
        lv_obj_t *fin=find_label(pages[npages-1],"Mark as finished");while(fin&&!lv_obj_has_flag(fin,LV_OBJ_FLAG_CLICKABLE))fin=lv_obj_get_parent(fin);
        lv_obj_send_event(fin,LV_EVENT_CLICKED,NULL);frame(200);
        assert(fake_prog[1].finished && find_label(pages[npages-1],"Mark as not finished") && has_text(pages[npages-1],"Finished"));
        page_pop();frame(400);
        test_book_playing=false;
        /* the ways in */
        fake_prog_on[2]=true;fake_prog[2].position=60000;fake_prog[2].last_played=50;   /* two being heard: Continue listening says See all */
        books_page();frame(400);
        assert(find_label(pages[npages-1],"Library") && find_label(pages[npages-1],"Authors") && find_label(pages[npages-1],"Series") &&
               find_label(pages[npages-1],"Finished") && find_label(pages[npages-1],"See all"));
        shot("audiobooks-browse.ppm");
        lv_obj_t *lib=find_label(pages[npages-1],"Library");while(lib&&!lv_obj_has_flag(lib,LV_OBJ_FLAG_CLICKABLE))lib=lv_obj_get_parent(lib);
        lv_obj_send_event(lib,LV_EVENT_CLICKED,NULL);frame(400);   /* every book by its cover, by title */
        assert(find_label(pages[npages-1],"Atomic Habits") && find_label(pages[npages-1],"Dune") && find_label(pages[npages-1],"The Hobbit"));
        lib=find_label(pages[npages-1],"Dune");while(lib&&!lv_obj_has_flag(lib,LV_OBJ_FLAG_CLICKABLE))lib=lv_obj_get_parent(lib);
        lv_obj_send_event(lib,LV_EVENT_CLICKED,NULL);frame(400);assert(find_label(pages[npages-1],"Add a bookmark"));   /* its page */
        page_pop();frame(400);page_pop();frame(400);
        lv_obj_t *r=find_label(pages[npages-1],"Authors");while(r&&!lv_obj_has_flag(r,LV_OBJ_FLAG_CLICKABLE))r=lv_obj_get_parent(r);
        lv_obj_send_event(r,LV_EVENT_CLICKED,NULL);frame(400);assert(find_label(pages[npages-1],"Frank Herbert") && has_text(pages[npages-1],"1 book"));
        r=find_label(pages[npages-1],"Frank Herbert");while(r&&!lv_obj_has_flag(r,LV_OBJ_FLAG_CLICKABLE))r=lv_obj_get_parent(r);
        lv_obj_send_event(r,LV_EVENT_CLICKED,NULL);frame(400);assert(find_label(pages[npages-1],"Dune"));
        page_pop();frame(400);page_pop();frame(400);
        r=find_label(pages[npages-1],"Series");while(r&&!lv_obj_has_flag(r,LV_OBJ_FLAG_CLICKABLE))r=lv_obj_get_parent(r);
        lv_obj_send_event(r,LV_EVENT_CLICKED,NULL);frame(400);assert(has_text(pages[npages-1],"Frank Herbert \xc2\xb7 1 book"));
        r=find_label(pages[npages-1],"Dune Chronicles");while(r&&!lv_obj_has_flag(r,LV_OBJ_FLAG_CLICKABLE))r=lv_obj_get_parent(r);
        lv_obj_send_event(r,LV_EVENT_CLICKED,NULL);frame(400);assert(has_text(pages[npages-1],"Book 1 \xc2\xb7 Finished"));
        page_pop();frame(400);page_pop();frame(400);
        r=find_label(pages[npages-1],"Finished");while(r&&!lv_obj_has_flag(r,LV_OBJ_FLAG_CLICKABLE))r=lv_obj_get_parent(r);
        lv_obj_send_event(r,LV_EVENT_CLICKED,NULL);frame(400);assert(find_label(pages[npages-1],"Dune") && !find_label(pages[npages-1],"The Hobbit"));
        page_pop();frame(400);
        r=find_label(pages[npages-1],"See all");assert(r && lv_obj_has_flag(r,LV_OBJ_FLAG_CLICKABLE));
        lv_obj_send_event(r,LV_EVENT_CLICKED,NULL);frame(400);
        assert(find_label(pages[npages-1],"The Hobbit") && find_label(pages[npages-1],"Atomic Habits") && !find_label(pages[npages-1],"Dune"));
        page_pop();frame(400);page_pop();frame(400);
        for(int i=0;i<3;i++){fake_book_str[i][AB_S_PATH]=NULL;fake_book_str[i][AB_S_SERIES]=NULL;fake_book_str[i][AB_S_SERIES_NO]=NULL;fake_books[i].single=0;}
        fake_prog_on[2]=false;memset(&fake_prog[2],0,sizeof fake_prog[2]);
        strlcpy(test_track.path,tp0,sizeof test_track.path);test_track.elapsed=te0;test_status=ts0;
    }
    {   /* a book's own cover: a picture in its folder, or the one in its audio (ID3's unsynchronised,
         * a Vorbis comment's base64 picture block), made into squares once by the worker; the card
         * being heard takes its colour; a picture that cannot be read is tried once */
        static struct ab_book sv_b[3];static const char *sv_s[3][AB_NUM_STR];static struct ab_progress sv_p[3];static bool sv_on[3];int sv_n=fake_books_n;
        memcpy(sv_b,fake_books,sizeof sv_b);memcpy(sv_s,fake_book_str,sizeof sv_s);memcpy(sv_p,fake_prog,sizeof sv_p);memcpy(sv_on,fake_prog_on,sizeof sv_on);
        char d[700];snprintf(d,sizeof d,"rm -rf '%s" SL_BOOK_ART_DIR "'; mkdir -p '%s/Audiobooks/Covered'",sim_root_dir,sim_root_dir);system(d);
        enum { CS = 64 };
        unsigned char *rgb=malloc(CS*CS*3);for(int i=0;i<CS*CS;i++){rgb[i*3]=200;rgb[i*3+1]=30;rgb[i*3+2]=40;}   /* red */
        for(int i=0;i<CS*CS/8;i++)rgb[i*3+1]=255;   /* and an 0xff byte or two for the unsynchronising */
        unsigned char *png=NULL;size_t pn=sl_png_encode_rgb(rgb,CS,CS,&png);free(rgb);assert(pn>0);
        /* the picture file; an "audio" file with it unsynchronised at 100; one with a base64 FLAC picture block at 7 */
        snprintf(d,sizeof d,"%s/Audiobooks/Covered/cover.png",sim_root_dir);FILE *f=fopen(d,"wb");fwrite(png,1,pn,f);fclose(f);
        unsigned char *un=malloc(pn*2+100);memset(un,0x11,100);size_t un_n=100;
        for(size_t i=0;i<pn;i++){un[un_n++]=png[i];if(png[i]==0xff)un[un_n++]=0;}
        snprintf(d,sizeof d,"%s/Audiobooks/Covered/01 Unsync.mp3",sim_root_dir);f=fopen(d,"wb");fwrite(un,1,un_n,f);fclose(f);
        size_t bl=32+9+pn;unsigned char *blk=calloc(1,bl);size_t at=0;
        blk[3]=3;at=4;blk[at+3]=9;at+=4;memcpy(blk+at,"image/png",9);at+=9;at+=4;at+=16;
        blk[at]=(unsigned char)(pn>>24);blk[at+1]=(unsigned char)(pn>>16);blk[at+2]=(unsigned char)(pn>>8);blk[at+3]=(unsigned char)pn;at+=4;
        memcpy(blk+at,png,pn);at+=pn;
        static const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char *enc=malloc(at*4/3+16);size_t en=7;memcpy(enc,"#######",7);   /* a little before it */
        for(size_t i=0;i<at;i+=3){unsigned v=(unsigned)blk[i]<<16|(i+1<at?(unsigned)blk[i+1]<<8:0)|(i+2<at?blk[i+2]:0);
            enc[en++]=b64[v>>18&63];enc[en++]=b64[v>>12&63];enc[en++]=i+1<at?b64[v>>6&63]:'=';enc[en++]=i+2<at?b64[v&63]:'=';}
        snprintf(d,sizeof d,"%s/Audiobooks/Covered/02 Vorbis.ogg",sim_root_dir);f=fopen(d,"wb");fwrite(enc,1,en,f);fclose(f);
        snprintf(d,sizeof d,"%s/Audiobooks/Covered/broken.jpg",sim_root_dir);f=fopen(d,"wb");fputs("not a picture",f);fclose(f);
        free(un);free(blk);free(png);
        static const char *const ti[3]={"Picture","Unsync","Vorbis"};
        fake_books_n=3;
        for(int i=0;i<3;i++){memset(&fake_prog[i],0,sizeof fake_prog[i]);fake_prog_on[i]=false;memset(&fake_books[i],0,sizeof fake_books[i]);
            fake_books[i].id=101+i;fake_books[i].length=3600000u;fake_book_str[i][AB_S_TITLE]=ti[i];fake_book_str[i][AB_S_AUTHOR]="Someone";fake_prog[i].id=fake_books[i].id;}
        fake_book_str[0][AB_S_COVER]="/Audiobooks/Covered/cover.png";
        fake_book_str[1][AB_S_COVER]="/Audiobooks/Covered/01 Unsync.mp3";fake_books[1].cover_pos=100;fake_books[1].cover_size=(uint32_t)(un_n-100);
        fake_books[1].cover_type=AA_TYPE_PNG|AA_FLAG_ID3_UNSYNC;
        fake_book_str[2][AB_S_COVER]="/Audiobooks/Covered/02 Vorbis.ogg";fake_books[2].cover_pos=7;fake_books[2].cover_size=(uint32_t)(en-7);
        fake_books[2].cover_type=AA_TYPE_PNG|AA_FLAG_VORBIS_BASE64;
        fake_prog_on[0]=true;fake_prog[0].position=600000;fake_prog[0].last_played=10;
        books_page();
        for(int k=0;k<100&&(!sl_book_art_state(bk_art_key(&fake_books[0]))||!sl_book_art_state(bk_art_key(&fake_books[1]))||!sl_book_art_state(bk_art_key(&fake_books[2]))||bk_npend);k++)frame(50);
        for(int i=0;i<3;i++)assert(sl_book_art_state(bk_art_key(&fake_books[i]))==1);
        uint32_t tint=sl_book_art_tint(bk_art_key(&fake_books[0]));assert((tint>>16)>150 && (tint&0xff)<90);   /* red */
        assert(bk_hero && lv_color_to_u32(lv_obj_get_style_bg_color(bk_hero,0)) != lv_color_to_u32(lv_color_mix(lv_color_hex(bk_tint(&fake_books[0])),lv_color_hex(C_CARD),sl_theme()==1?70:110)));
        int imgs=0;{lv_obj_t *st[64];int sp=0;st[sp++]=pages[npages-1];while(sp){lv_obj_t *o=st[--sp];if(lv_obj_check_type(o,&lv_image_class))imgs++;
            for(unsigned i=0;i<lv_obj_get_child_count(o)&&sp<64;i++)st[sp++]=lv_obj_get_child(o,i);}}
        assert(imgs>=4);   /* the card's, and a row's each */
        shot("audiobooks-covers.ppm");
        page_pop();frame(400);
        /* not a picture: tried once, its mark kept */
        fake_book_str[0][AB_S_COVER]="/Audiobooks/Covered/broken.jpg";
        books_page();for(int k=0;k<60&&!sl_book_art_state(bk_art_key(&fake_books[0]));k++)frame(50);
        assert(sl_book_art_state(bk_art_key(&fake_books[0]))==-1);
        page_pop();frame(400);books_page();frame(200);assert(!bk_npend);page_pop();frame(400);
        memcpy(fake_books,sv_b,sizeof sv_b);memcpy(fake_book_str,sv_s,sizeof sv_s);memcpy(fake_prog,sv_p,sizeof sv_p);memcpy(fake_prog_on,sv_on,sizeof sv_on);fake_books_n=sv_n;
        printf("PASS: audiobooks: covers from a picture, an unsynchronised ID3 one and a base64 Vorbis one; the card in its colour; a bad one tried once.\n");
    }
    album_page(0);frame(500);shot("album.ppm");assert(find_label(pages[npages-1],"One album"));page_pop();frame(500);
    test_art_hue=0;album_page(0);frame(500);shot("album-red-cover.ppm");   /* a red sleeve: a red wash */
    { lv_color_t c=lv_obj_get_style_bg_color(pages[npages-1],0);
      assert(sl_zune() || (c.red>c.green && c.red>c.blue)); }
    page_pop();frame(400);test_art_hue=-1;
    albums_page_at(24,true);frame(500);assert(objects(pages[npages-1])<250);
    albums_page_at(4968,true);frame(500);assert(objects(pages[npages-1])<250);page_pop();frame(500);
    artists_page();frame(500);assert(objects(pages[npages-1])<350);
    {   /* a tap on an artist opens that artist (it opened a genre's page, or nothing, when its code was read as a genre's) */
        lv_obj_t *row=NULL,*body=artists_view->host;   /* the rows made so far */
        for(uint32_t i=0;i<lv_obj_get_child_count(body)&&!row;i++)if((intptr_t)lv_obj_get_user_data(lv_obj_get_child(body,i))==3)row=lv_obj_get_child(body,i);
        assert(row);int depth=npages;lv_obj_send_event(row,LV_EVENT_CLICKED,NULL);frame(500);
        assert(npages==depth+1 && find_label(pages[npages-1],sl_lib.albums[sl_lib.albums_by_artist[2]].artist));page_pop();frame(500);
    }
    {   /* one list, made as it scrolls: the end of it as light as the start, no Previous or More */
        lv_obj_t *body=lv_obj_get_child(pages[npages-1],1);assert(artists_view && artists_view->count==5000);
        lv_obj_scroll_to_y(body,LV_COORD_MAX,LV_ANIM_OFF);frame(500);assert(objects(pages[npages-1])<350);
        assert(find_label(pages[npages-1],"Artist 4999") && !find_label(pages[npages-1],"More artists"));
        page_pop();frame(500);
    }
    lv_obj_t *saved_test=sl_target_row(lv_screen_active(),"Saved","","/missing-playlist.m3u8",0,false,sl_saved_row_cb);
    int queue_before=queued;lv_obj_send_event(saved_test,LV_EVENT_CLICKED,NULL);assert(queued==queue_before);
    lv_obj_delete(saved_test);
    /* Update database: tagcache_rebuild() only queues the job (and returns
     * false). It gets a screen of its own that follows the scan through,
     * rebuilds the pages under it around the new library and fades to home. */
    {
        system_page();frame(400);int depth=npages;assert(depth>1);
        rebuild_result=false;action(ACT_REBUILD);frame(300);
        assert(db_update_state==DBU_POSTED && dbu_showing() && overlay_visible());
        assert(find_label(dbu,"Now updating") && find_label(dbu,"please hold on boi"));
        tc_usable=false;tc_stat_stub.processed_entries=0;db_update_poll();
        assert(db_update_state==DBU_SCANNING && !strcmp(lv_label_get_text(dbu_phase),"Scanning your music"));
        static char scanning_path[]="/Music/Mira Sol/Blue Hours/01 Tide.mp3";
        tc_stat_stub.curentry=scanning_path;tc_stat_stub.processed_entries=412;db_update_poll();frame(100);
        assert(strstr(lv_label_get_text(dbu_detail),"412 items") && strstr(lv_label_get_text(dbu_detail),"Blue Hours"));
        shot("library-update.ppm");
        /* Hide: it keeps going behind; tapping Update again brings it back */
        lv_obj_send_event(dbu_btn,LV_EVENT_CLICKED,NULL);frame(400);
        assert(!dbu && db_update_state==DBU_SCANNING && npages==depth);
        action(ACT_REBUILD);frame(300);assert(dbu_showing() && db_update_state==DBU_SCANNING);
        tc_stat_stub.curentry=NULL;tc_stat_stub.commit_step=3;db_update_poll();
        assert(!strcmp(lv_label_get_text(dbu_phase),"Saving your library") && strstr(lv_label_get_text(dbu_detail),"3 of 8"));
        /* the commit lands: pages rebuilt around the new library, then home */
        tc_stat_stub.commit_step=0;tc_usable=true;db_update_poll();
        assert(db_update_state==DBU_READY);
        db_update_poll();frame(500);
        assert(db_update_state==DBU_IDLE && npages==1 && lib_state==SL_LIB_READY);
        assert(strstr(lv_label_get_text(lv_obj_get_child(banner_obj,0)),"Library updated"));
        frame(400);assert(!dbu);
        /* scanned but the commit waits for a restart: say so on the screen */
        action(ACT_REBUILD);frame(300);
        tc_usable=false;db_update_poll();tc_stat_stub.commit_delayed=true;db_update_poll();
        assert(db_update_state==DBU_IDLE && dbu_showing() && !strcmp(lv_label_get_text(dbu_btn_txt),"Close"));
        tc_stat_stub.commit_delayed=false;tc_usable=true;
        lv_obj_send_event(dbu_btn,LV_EVENT_CLICKED,NULL);frame(400);assert(!dbu);
        /* by itself (at start-up, after USB mode, Files, pull to refresh): the
         * database's own update, no rebuild; the library is read again only
         * if anything changed, a removal included (which leaves the key) */
        {   int ups=test_updates,drops=test_cache_drops,begins=test_lib_begins;
            db_update_begin(false);assert(db_update_state==DBU_CHECKING && !dbu && test_updates==ups+1);
            assert(strstr(lv_label_get_text(lv_obj_get_child(banner_obj,0)),"Looking for new music")!=NULL);
            db_update_poll();assert(db_update_state==DBU_CHECKING);          /* still walking the card */
            tagcache_updates_done++;db_update_poll();
            assert(db_update_state==DBU_IDLE && test_cache_drops==drops && test_lib_begins==begins);
            assert(strstr(lv_label_get_text(lv_obj_get_child(banner_obj,0)),"up to date")!=NULL);
            db_update_begin(false);tagcache_entries_deleted+=3;tagcache_updates_done++;db_update_poll();
            assert(db_update_state==DBU_READY && test_cache_drops==drops+1);   /* songs gone: read again, not from the cache */
            db_update_poll();frame(500);assert(db_update_state==DBU_IDLE && test_lib_begins==begins+1);
            db_update_begin(false);test_db_key[4]++;tagcache_updates_done++;db_update_poll();
            assert(db_update_state==DBU_READY);db_update_poll();frame(500);assert(db_update_state==DBU_IDLE);   /* new songs committed */
            /* the screen's own button still rebuilds from the start */
            action(ACT_REBUILD);frame(300);assert(db_update_state==DBU_POSTED && dbu_showing());
            lv_obj_send_event(dbu_btn,LV_EVENT_CLICKED,NULL);frame(400);db_update_state=DBU_IDLE;
        }
    }
    tc_usable=true;db_update_state=DBU_IDLE;
    { int saved=lib_state;lib_state=SL_LIB_LOADING;action(ACT_REBUILD);lib_state=saved; }
    assert(strstr(lv_label_get_text(lv_obj_get_child(banner_obj,0)),"still loading")!=NULL);
    assert(db_update_state==DBU_IDLE);
    {   /* back from USB mode (the interface restarted): the library updates by itself, once */
        FILE *u=fopen(DB_USB_NOTE,"w");assert(u);fclose(u);
        int saved_auto=global_settings.sloppy_db_auto;global_settings.sloppy_db_auto=0;
        long saved_tick=current_tick;current_tick=1;
        db_auto_poll();assert(db_update_state==DBU_IDLE);   /* not before the player settles */
        current_tick=4*HZ;
        db_auto_poll();assert(db_update_state==DBU_CHECKING && access(DB_USB_NOTE,F_OK)!=0 && db_check_quiet);
        db_update_state=DBU_IDLE;db_auto_poll();assert(db_update_state==DBU_IDLE);
        db_check_quiet=false;
        global_settings.sloppy_db_auto=saved_auto;current_tick=saved_tick;
        /* Folders to scan: a folder turned off is on the exclude list, and leaving looks for changes */
        global_settings.db_exclude_folders[0]=0;global_settings.db_exclude_audiobooks=true;
        lo_folders_page();frame(500);shot("folders-to-scan.ppm");
        lv_obj_t *fl=find_label(pages[npages-1],"Music");assert(fl && find_label(pages[npages-1],"10,000 songs in the library"));
        lv_obj_send_event(lv_obj_get_parent(fl),LV_EVENT_CLICKED,NULL);frame(200);
        assert(!strcmp(global_settings.db_exclude_folders,"/Music"));
        int ups=test_updates;page_pop();frame(500);assert(test_updates==ups+1 && db_update_state==DBU_CHECKING);
        tagcache_updates_done++;db_update_poll();db_update_state=DBU_IDLE;
        library_options_page();frame(500);assert(find_label(pages[npages-1],"1 left out"));page_pop();frame(400);
        global_settings.db_exclude_folders[0]=0;
    }
    rebuild_result=true;
    {   /* Zune: chosen from Theme, it brings the List home, black and pink,
         * big lowercase titles, flat groups and sheets; True dark then
         * still takes (Zune reads as True dark underneath) and it all goes */
        int saved_style=global_settings.sloppy_home_style,saved_theme=global_settings.sloppy_theme;
        appearance_page();theme_page();frame(300);
        lv_obj_t *tb=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
        assert(lv_obj_get_child_count(tb)==4);
        lv_obj_send_event(lv_obj_get_child(tb,3),LV_EVENT_CLICKED,NULL);frame(700);
        assert(global_settings.sloppy_theme==3 && sl_zune() && sl_theme()==2 && global_settings.sloppy_home_style==2);
        assert(C_ACCENT==ZN_PINK && C_BG==0x000000);
        while(npages>1){page_pop();frame(300);}
        shot("zune-home.ppm");
        sl_library_page();frame(500);assert(has_text(pages[npages-1],"library"));shot("zune-library.ppm");page_pop();frame(400);
        music_page();frame(500);shot("zune-music.ppm");page_pop();frame(400);
        system_page();frame(500);shot("zune-settings.ppm");
        sound_page();frame(500);assert(find_label(pages[npages-1],"VOLUME"));shot("zune-sound.ppm");page_pop();frame(400);
        page_pop();frame(400);
        test_status=AUDIO_STATUS_PLAY;np_show();frame(600);
        assert(!lv_obj_has_flag(np_zn,LV_OBJ_FLAG_HIDDEN));shot("zune-now-playing.ppm");
        uq_show();frame(500);shot("zune-queue.ppm");uq_close();frame(500);np_hide(0);frame(500);
        acts_open(0);frame(400);shot("zune-song-sheet.ppm");acts_close();frame(400);
        notify_ex(NS_MUSIC,NOTE_INFO,0,NA_NONE,"x","Radio from Night Tide");frame(400);shot("zune-banner.ppm");frame(4000);
        global_settings.sloppy_notifications=0;toast("Radio from Night Tide");frame(600);shot_now("zune-pop.ppm");
        frame(3000);frame(400);global_settings.sloppy_notifications=1;
        {   /* the lock screen, Control Center and mini player in Zune */
            int saved_run=sl_power_running;sl_power_running=true;
            sl_power_state_init(&sl_power,sl_ms());sl_power_applied_mode=-1;sl_power.mode=SL_POWER_LOCKED;sl_power_apply();frame(400);
            assert(lv_obj_get_style_text_font(lp_clock,0)==&ss_regular_104);shot("zune-lock.ppm");
            sl_power.mode=SL_POWER_ACTIVE;sl_power_apply();frame(400);sl_power_running=saved_run;
            qp_glass_capture();lv_obj_remove_flag(qp,LV_OBJ_FLAG_HIDDEN);qp_open=true;frame(200);shot("zune-control-center.ppm");
            qp_open=false;lv_obj_add_flag(qp,LV_OBJ_FLAG_HIDDEN);frame(200);
            assert(lv_obj_get_style_radius(mini,0)==0);
            sl_library_page();frame(400);albums_page_at(0,false);frame(600);shot("zune-albums.ppm");
            while(npages>1){page_pop();frame(300);}
        }
        test_status=0;
        /* back to True dark: a choice that reads the same as Zune's base still takes */
        appearance_page();theme_page();frame(300);tb=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
        lv_obj_send_event(lv_obj_get_child(tb,2),LV_EVENT_CLICKED,NULL);frame(700);
        assert(global_settings.sloppy_theme==2 && !sl_zune() && C_ACCENT!=ZN_PINK);
        assert(global_settings.sloppy_home_style==1);   /* leaving Zule brings the tiles back */
        /* Zule with tiles, on purpose: Home layout after the theme */
        appearance_page();theme_page();frame(300);tb=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
        assert(has_text(lv_obj_get_child(tb,3),"Zule") && !has_text(lv_obj_get_child(tb,3),"Zune"));
        lv_obj_send_event(lv_obj_get_child(tb,3),LV_EVENT_CLICKED,NULL);frame(700);
        assert(global_settings.sloppy_home_style==2);
        home_style_page();frame(300);
        lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1),1),LV_EVENT_CLICKED,NULL);frame(700);
        assert(sl_zune() && global_settings.sloppy_home_style==1);
        while(npages>1){page_pop();frame(300);}
        appearance_page();theme_page();frame(300);tb=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
        lv_obj_send_event(lv_obj_get_child(tb,2),LV_EVENT_CLICKED,NULL);frame(700);
        np_show();frame(400);assert(lv_obj_has_flag(np_zn,LV_OBJ_FLAG_HIDDEN));np_hide(0);frame(400);
        while(npages>1){page_pop();frame(300);}
        global_settings.sloppy_home_style=saved_style;global_settings.sloppy_theme=saved_theme;appearance_reload(NULL);frame(600);
        while(npages>1){page_pop();frame(300);}
        printf("PASS: Zune theme: chosen, carried through pages, sheets, Now Playing; switches back.\n");
    }
    appearance_page();theme_page();frame(300);
    lv_obj_t *theme_body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
    lv_obj_send_event(lv_obj_get_child(theme_body,2),LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_theme==2 && C_BG==0x000000);
    shot("theme-true-dark.ppm");
    assert(lv_color_eq(lv_obj_get_style_bg_color(qp,0),lv_color_hex(C_BG)));
    qp_glass_capture();lv_obj_remove_flag(qp,LV_OBJ_FLAG_HIDDEN);qp_open=true;shot("quick-true-dark.ppm");
    qp_open=false;lv_obj_add_flag(qp,LV_OBJ_FLAG_HIDDEN);
    wallpaper_page(false);frame(300);
    lv_obj_t *wall_body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
    lv_obj_send_event(lv_obj_get_child(wall_body,1),LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_home_wallpaper==1);
    wallpaper_page(true);frame(300);
    wall_body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
    lv_obj_send_event(lv_obj_get_child(wall_body,3),LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_lock_wallpaper==3);
    wallpaper_page(false);frame(300);
    wall_body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
    assert(lv_obj_get_child_count(wall_body)==9); /* nine previews */
    lv_obj_scroll_to_y(lv_obj_get_parent(wall_body), 500, LV_ANIM_OFF);frame(100);
    shot("wallpaper-picker.ppm");
    lv_obj_send_event(lv_obj_get_child(wall_body,5),LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_home_wallpaper==5);
    wallpaper_page(true);frame(300);
    wall_body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
    lv_obj_scroll_to_y(lv_obj_get_parent(wall_body), 500, LV_ANIM_OFF);frame(100);
    shot("lock-wallpaper-picker.ppm");
    lv_obj_send_event(lv_obj_get_child(wall_body,7),LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_lock_wallpaper==7);
    wallpaper_page(false);frame(300);
    wall_body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
    lv_obj_send_event(lv_obj_get_child(wall_body,8),LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_home_wallpaper==8);
    wallpaper_page(true);frame(300);
    wall_body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
    lv_obj_send_event(lv_obj_get_child(wall_body,8),LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_lock_wallpaper==8);
    wallpaper_page(true);frame(300);
    wall_body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
    lv_obj_send_event(lv_obj_get_child(wall_body,7),LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_lock_wallpaper==7);
    lv_obj_t *wall_test=box(lv_layer_top());
    lv_obj_set_size(wall_test,W,H);
    lv_obj_set_style_bg_color(wall_test,lv_color_hex(C_BG),0);
    lv_obj_set_style_bg_opa(wall_test,LV_OPA_COVER,0);
    lv_obj_add_event_cb(wall_test,sl_wallpaper_draw,LV_EVENT_DRAW_MAIN,NULL);
    const char *const pattern_shots[]={"glow-redesign.ppm","grid-redesign.ppm",
                                       "constellation-redesign.ppm","waves-redesign.ppm"};
    for(int id=1;id<=4;id++) {
        global_settings.sloppy_home_wallpaper=id;
        lv_obj_invalidate(wall_test);shot(pattern_shots[id-1]);
    }
    global_settings.sloppy_home_wallpaper=8;
    wallpaper_tick(NULL);lv_obj_invalidate(wall_test);frame(300);
    static uint16_t nexus_before[W*H];
    memcpy(nexus_before,pixels,sizeof pixels);
    frame(700);wallpaper_tick(NULL);lv_obj_invalidate(wall_test);frame(0);
    assert(memcmp(nexus_before,pixels,sizeof pixels)!=0);
    shot("nexus-live.ppm");
    wallpaper_nexus_tap_at(W/2,H/2);
    frame(400);wallpaper_tick(NULL);lv_obj_invalidate(wall_test);
    shot("nexus-tap.ppm");
    /* redrawing only the pulses' strips must match a full redraw */
    for(int k=0;k<240;k++) {
        uint32_t before=wallpaper_nexus_now;
        if(k==40) wallpaper_nexus_tap_at(W/3,H/4);
        frame(33);wallpaper_nexus_now=lv_tick_get();
        wallpaper_nexus_invalidate(wall_test,before,wallpaper_nexus_now);frame(0);
        memcpy(nexus_before,pixels,sizeof pixels);
        lv_obj_invalidate(wall_test);frame(0);
        assert(memcmp(nexus_before,pixels,sizeof pixels)==0);
    }
    lv_obj_delete(wall_test);
    global_settings.sloppy_home_wallpaper=5;
    accent_page();frame(300);
    lv_obj_t *accent_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_t *accent_wheel=lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(accent_body,1),0),0),0);   /* group, its row, the stage */
    assert(lv_obj_get_width(accent_wheel)==ACCENT_WHEEL_SIZE);
    int accent_before=global_settings.sloppy_accent_color;
    /* anti-aliased rim: solid inside, see-through outside, partial on the edge */
    assert(accent_wheel_pixels[(ACCENT_WHEEL_SIZE/2)*ACCENT_WHEEL_SIZE+ACCENT_WHEEL_SIZE/2].alpha==255);
    assert(accent_wheel_pixels[0].alpha==0);
    {int partial=0;for(int x=0;x<ACCENT_WHEEL_SIZE;x++){uint8_t a=accent_wheel_pixels[(ACCENT_WHEEL_SIZE/2)*ACCENT_WHEEL_SIZE+x].alpha;if(a>0&&a<255)partial++;}assert(partial>=1);}
    assert(!lv_obj_has_flag(accent_wheel,LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    /* the apply button says so when nothing would change, and ignores the tap */
    assert(!strcmp(lv_label_get_text(accent_preview_label),"CURRENT COLOR"));
    assert(lv_obj_has_flag(accent_was,LV_OBJ_FLAG_HIDDEN));
    /* a preset swatch picks its colour and rings itself */
    lv_obj_send_event(accent_swatches[1],LV_EVENT_CLICKED,NULL);frame(50);
    assert(accent_pick_rgb()==0x5aa9ff);
    assert(lv_obj_get_style_border_width(accent_swatches[1],0)==4);
    assert(!strcmp(lv_label_get_text(accent_preview_label),"USE THIS COLOR"));
    assert(!lv_obj_has_flag(accent_was,LV_OBJ_FLAG_HIDDEN));
    accent_wheel_select(0,80);assert(accent_pick.h==90 && accent_pick.s>70);
    lv_slider_set_value(accent_brightness,60,LV_ANIM_OFF);
    lv_obj_send_event(accent_brightness,LV_EVENT_VALUE_CHANGED,NULL);
    assert(accent_pick.v==60 && global_settings.sloppy_accent_color==accent_before);
    assert(lv_obj_get_style_bg_opa(accent_shade,0)==40*255/100);
    {lv_color_t m=lv_obj_get_style_bg_color(accent_marker,0);assert(((uint32_t)m.red<<16|m.green<<8|m.blue)==accent_pick_rgb());}
    shot("accent-wheel.ppm");
    uint32_t selected=accent_pick_rgb();
    lv_obj_send_event(accent_preview,LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_accent_color==(int)selected);
    accent_page();frame(300);
    accent_wheel_select(-80,0);
    page_pop();frame(350);
    assert(global_settings.sloppy_accent_color==(int)selected);
    theme_page();frame(300);
    theme_body=lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),1);
    lv_obj_send_event(lv_obj_get_child(theme_body,1),LV_EVENT_CLICKED,NULL);
    frame(600);assert(global_settings.sloppy_theme==1 && C_BG==0xf1f4f7);
    shot("theme-light.ppm");
    music_page();frame(500);shot("music-light.ppm");songs_page();frame(500);shot("songs-light.ppm");page_pop();frame(400);page_pop();frame(400);
    qp_glass_capture();lv_obj_remove_flag(qp,LV_OBJ_FLAG_HIDDEN);qp_open=true;shot("quick-light.ppm");
    qp_open=false;lv_obj_add_flag(qp,LV_OBJ_FLAG_HIDDEN);
    page_pop();frame(350);page_pop();frame(350);
    shot("home-wallpaper.ppm");
    {lv_obj_t *hb=lv_obj_get_child(pages[0],0);lv_obj_scroll_to_y(hb,450,LV_ANIM_OFF);frame(100);shot("home-apps-light.ppm");
     lv_obj_scroll_to_y(hb,0,LV_ANIM_OFF);frame(100);}
    sl_power_overlay_show(true);frame(100);shot("lock-wallpaper.ppm");
    sl_power_overlay_show(false);
    /* A-Z rail: songs jump while scrubbing, the rail's state dies with its page */
    songs_page();frame(500);
    {
        lv_obj_t *sp=pages[npages-1], *rail=NULL;
        for(uint32_t i=0;i<lv_obj_get_child_count(sp);i++){lv_obj_t *c=lv_obj_get_child(sp,i);if(lv_obj_get_width(c)==AZ_W && lv_obj_get_x(c)==AZ_X)rail=c;}
        assert(rail);
        struct az_state *r=lv_event_dsc_get_user_data(lv_obj_get_event_dsc(rail,lv_obj_get_event_count(rail)-1));
        assert(r && r->n==sl_lib.n_tracks);
        /* a leading article does not count: The Beatles sort and jump under B */
        assert(!strcmp(sl_sort_key("The Beatles"),"Beatles") && !strcmp(sl_sort_key("An Album"),"Album") && !strcmp(sl_sort_key("A Day"),"Day"));
        assert(!strcmp(sl_sort_key("Theatre"),"Theatre") && !strcmp(sl_sort_key("The"),"The") && !strcmp(sl_sort_key("Abba"),"Abba"));
        assert(az_initial("The Beatles")=='B' && az_initial("Zebra")=='Z' && az_initial("99 Problems")=='#');
        int m=az_find(r,'M');assert(m>=0 && (az_initial(az_song_key(m))>='M'));   /* M, or the next letter there is */
        if(m>0)assert(az_initial(az_song_key(m-1))<'M' || az_initial(az_song_key(m-1))=='#');
        r->jump(r,m,false);frame(100);
        lv_obj_t *sb=lv_obj_get_child(sp,1);assert(lv_obj_get_scroll_y(sb)>0);
        lv_label_set_text(r->bubble_txt,"M");lv_obj_set_y(r->bubble,300);lv_obj_remove_flag(r->bubble,LV_OBJ_FLAG_HIDDEN);
        shot("az-rail.ppm");lv_obj_add_flag(r->bubble,LV_OBJ_FLAG_HIDDEN);
        page_pop();frame(500);
    }
    artists_page();frame(500);assert(objects(pages[npages-1])<400);
    {   /* row text fits beside the rail and the chevron */
        lv_obj_t *row=lv_obj_get_child(artists_view->host,0);   /* the first row made */
        lv_obj_t *title=lv_obj_get_child(row,0);
        assert(lv_obj_get_x(title)+lv_obj_get_width(title) <= lv_obj_get_width(row)-58);
        lv_area_t ra;lv_obj_get_coords(row,&ra);assert(ra.x2 < AZ_X);
        shot("az-artists.ppm");
    }
    page_pop();frame(500);

    /* Cover Flow: landscape, placeholders, flip open, keep swiping, Done */
    lv_obj_t *home_scr=lv_screen_active();
    cf_open();frame(900);
    /* each cover is looked up on the card once (about 4 ms on the R1),
     * found or missing; a missing one was looked up again every frame */
    {assert(cfm.open_checks>0 && cfm.open_checks<=17);uint32_t t=cfm.tick_checks;
     for(int i=0;i<10;i++){cf.dirty=true;frame(100);}assert(cfm.tick_checks==t);}
    assert(cf.active && lv_display_get_rotation(disp)==LV_DISPLAY_ROTATION_90);
    assert(lv_display_get_horizontal_resolution(disp)==800 && lv_screen_active()==cf.scr);
    for(int i=0;i<10;i++)frame(100);
    shot("coverflow.ppm");
    cf_set_pos(NULL,CF_ONE*5+CF_ONE*4/10);frame(50);cf_tick(NULL);shot("coverflow-moving.ppm");
    cf_go(5*CF_ONE,300);for(int i=0;i<6;i++)frame(100);
    assert(cf.pos==5*CF_ONE);
    cf_open_album();for(int i=0;i<8;i++)frame(100);
    assert(cf.panel && cf.open_at==5 && lv_obj_get_child_count(cf.list)>=1);
    shot("coverflow-album.ppm");
    cf_close_album(1);for(int i=0;i<10;i++)frame(100);
    assert(!cf.panel && cf.flip==0 && cf.pos==6*CF_ONE);
    cf_close();frame(300);
    assert(!cf.active && lv_display_get_rotation(disp)==LV_DISPLAY_ROTATION_0 && lv_screen_active()==home_scr);
    { /* covers stay cached: reopening reads nothing and shows the same art */
        int cached=0;for(int i=0;i<CF_SLOTS;i++)cached+=cf.art[i].px!=NULL;
        assert(cached>=8);
        cf_open();assert(cf.active);
        int again=0;for(int i=0;i<CF_SLOTS;i++)again+=cf.art[i].px!=NULL;
        assert(again>=cached);
        /* the covers either side are ready before they come into view */
        for(int i=0;i<12;i++)frame(100);
        int c=(cf.pos+CF_ONE/2)/CF_ONE;
        for(int k=LV_MAX(c-CF_REACH,0);k<=LV_MIN(c+CF_REACH,cf.n-1);k++){
            bool in=false;
            for(int i=0;i<CF_SLOTS;i++)in|=cf.art[i].px && cf.art[i].album==sl_lib.albums_by_title[k];
            assert(in);
        }
        /* a placeholder looks again once a thumbnail has been made */
        int ph=-1;for(int i=0;i<CF_SLOTS;i++)if(cf.art[i].px && !cf.art[i].real){ph=i;break;}
        assert(ph>=0);cf.art[ph].tried=true;sl_art_made++;cf_tick(NULL);assert(!cf.art[ph].tried);
        /* covers not made yet are asked for, the middle one last (so first) */
        test_n_prefer=0;frame(300);cf_tick(NULL);
        assert(test_n_prefer>0);
        {int c=cf_centre(),last=test_prefer[test_n_prefer-1];bool mid_real=false;
         for(int i=0;i<CF_SLOTS;i++)mid_real|=cf.art[i].px&&cf.art[i].album==sl_lib.albums_by_title[c]&&cf.art[i].real;
         assert(mid_real||last==sl_lib.albums_by_title[c]);
         for(int i=0;i<test_n_prefer;i++){bool r=false;for(int j=0;j<CF_SLOTS;j++)r|=cf.art[j].px&&cf.art[j].album==test_prefer[i]&&cf.art[j].real;assert(!r);}}
        cf_close();frame(300);
        {   /* the home built again while Cover Flow is up (a pull to refresh finishing as the
             * player turned) is built on the home screen, not freed when Cover Flow closes */
            while(npages>1){page_pop();frame(400);}
            cf_open();frame(300);assert(cf.active && npages==1 && pages[0]==ptr.page);
            ptr_rebuild(NULL);frame(100);lv_obj_t *home=pages[0];
            assert(lv_obj_get_screen(home)==cf.home);
            cf_close();frame(300);
            assert(lv_obj_is_valid(home) && pages[0]==home);
            music_page();frame(400);assert(npages==2);page_pop();frame(400);
            db_update_state=DBU_IDLE;}   /* the refresh's library update: not under test here */
        /* left alone long enough, the memory goes back */
        cf_art_trim();assert(cf.art[ph].px);
        cf.closed_at=lv_tick_get()-CF_KEEP_MS-1;cf_art_trim();
        for(int i=0;i<CF_SLOTS;i++)assert(!cf.art[i].px);
    }
    shot("coverflow-closed.ppm");
    {   /* a page opened over Cover Flow while an album is open (the control
         * centre's Library tile, or Now Playing): it closes Cover Flow, and
         * nothing may still draw the album's labels (found by the fuzzer) */
        cf_open();for(int i=0;i<6;i++)frame(100);
        cf_open_album();
        /* every frame of the flip, squashed to nothing included, finishes
         * its layers: none may be left behind holding the panel's labels */
        int max_layers=0;
        for(int i=0;i<60;i++){frame(16);int n=0;for(lv_layer_t *l=disp->layer_head;l;l=l->next)n++;if(n>max_layers)max_layers=n;}
        fprintf(stderr,"cover flow flip: at most %d layers between frames\n",max_layers);
        assert(max_layers==1);
        assert(cf.panel);
        test_status=AUDIO_STATUS_PLAY;np_show();frame(500);
        music_page();for(int i=0;i<6;i++)frame(100);
        assert(!cf.active);
        np_hide(0);frame(500);test_status=0;
        page_pop();frame(500);
    }

    {   /* vertical: upright, albums top to bottom, the art still upright */
        global_settings.sloppy_cf_vertical=1;
        cf_open();for(int i=0;i<12;i++)frame(100);
        assert(cf.active && cf.vert && lv_display_get_rotation(disp)==LV_DISPLAY_ROTATION_0);
        for(int i=0;i<CF_SLOTS;i++)assert(!cf.art[i].px || cf.art[i].vert);
        shot("coverflow-vertical.ppm");
        cf_set_pos(NULL,CF_ONE*5+CF_ONE*4/10);frame(50);cf_tick(NULL);shot("coverflow-vertical-moving.ppm");
        /* a real drag up the screen moves on through the albums and settles on one */
        cf_go(3*CF_ONE,200);for(int i=0;i<4;i++)frame(100);
        int32_t before=cf.pos;
        touch_at(240,600,true);for(int i=1;i<=6;i++)touch_at(240,600-350*i/6,true);touch_at(240,250,false);
        for(int i=0;i<12;i++)frame(100);
        assert(cf.pos>before && cf.pos%CF_ONE==0);
        /* a tap on the middle cover opens it */
        cf_go(4*CF_ONE,200);for(int i=0;i<4;i++)frame(100);
        touch_at(240,400,true);touch_at(240,400,false);for(int i=0;i<8;i++)frame(100);
        assert(cf.panel && cf.open_at==4);
        shot("coverflow-vertical-album.ppm");
        cf_close();frame(300);
        assert(!cf.active && lv_display_get_rotation(disp)==LV_DISPLAY_ROTATION_0 && lv_screen_active()==home_scr);
        global_settings.sloppy_cf_vertical=0;
        cf_style_page();frame(400);shot("coverflow-setting.ppm");page_pop();frame(500);
    }

    {   /* Cover Flow left by the power key: dark, then the lock screen draws
         * and a swipe up unlocks (the R1 log: a dark screen, swipes of 0 px) */
        int saved_run=sl_power_running;
        sl_power_running=true;sl_power_state_init(&sl_power,sl_ms());sl_power_applied_mode=-1;sl_power_apply();frame(100);
        cf_open();for(int i=0;i<8;i++)frame(100);
        assert(cf.active);
        sl_power_filter_button(BUTTON_POWER);frame(50);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);frame(100);
        assert(!cf.active && sl_power.mode==SL_POWER_DARK && lv_display_get_rotation(disp)==LV_DISPLAY_ROTATION_0);
        sl_power_filter_touch(true,240,600);sl_power_filter_touch(false,240,600);sl_power_finish_wake();frame(300);
        assert(sl_power.mode==SL_POWER_LOCKED);
        shot("cf-power-lock.ppm");
        sl_power_filter_touch(true,240,700);sl_power_filter_touch(true,240,560);sl_power_filter_touch(true,240,420);
        sl_power_filter_touch(false,240,420);frame(700);
        assert(sl_power.mode==SL_POWER_ACTIVE);
        shot("cf-power-unlocked.ppm");
        sl_power_running=saved_run;
    }

    {   /* drawing in place, as on the R1: leaving a landscape app must give
         * the upright screen a buffer with upright rows (it kept 800 px rows
         * and LVGL asserted in lv_refr.c, a freeze on Done) */
        bool saved_ok=sl_direct_ok;sl_direct_ok=true;sl_set_rotation(LV_DISPLAY_ROTATION_0);frame(100);
        assert(sl_direct && lv_display_get_buf_active(disp)->header.stride==W*2);
        spa_open();frame(400);assert(!sl_direct);
        spa_close();
        assert(sl_direct && lv_display_get_buf_active(disp)->header.stride==W*2);
        frame(300);
        cf_open();for(int i=0;i<6;i++)frame(100);
        cf_close();
        assert(sl_direct && lv_display_get_buf_active(disp)->header.stride==W*2);
        frame(300);
        sl_direct_ok=saved_ok;sl_set_rotation(LV_DISPLAY_ROTATION_0);frame(100);
    }
    {   /* the Spectrum app used like on the R1: taps bring the chrome back
         * and it fades, swipes change mode mid fade, ~8 s at 60 Hz */
        spa_open();
        for(int round=0;round<6;round++){
            ltap(400,300);                       /* the chrome comes back */
            for(int i=0;i<12;i++)frame(16);      /* fading in */
            if(round&1)ldrag(600,300,240);       /* a mode change mid fade */
            for(int i=0;i<160;i++)frame(16);     /* shown, then fading out */
        }
        assert(spa.active);
        spa_close();frame(300);
    }
    {   /* a level that is not a number: the R1 turned it into INT_MAX rows
         * and hung in 'lvgl' a few seconds into the Spectrum app */
        spa_open();frame(400);
        sp.top=NAN;sp.lvl[3]=NAN;sp.peak[5]=NAN;sp.settled=false;
        for(int i=0;i<5;i++)frame(33);
        assert(sp.top==sp.top && sp.lvl[3]==sp.lvl[3] && sp.peak[5]==sp.peak[5]);
        assert(sp_row_of(NAN)==sp.base-2);
        spa_close();frame(300);
    }

    /* real touches: flick, tap, swipe away, Done (deletes its screen mid-click) */
    cf_open();for(int i=0;i<10;i++)frame(100);
    int32_t before_flick=cf.pos;
    ldrag(600,250,200);for(int i=0;i<12;i++)frame(100);
    assert(cf.pos>before_flick && cf.pos%CF_ONE==0);
    /* the meter saw the drag: touch reads, the frames it moved, the wait between */
    assert(cfm.on && cfm.touch_gap.n>0 && cfm.render.n>0 && cfm.refr.n>0 && cfm.frame.n>0 && cfm.touch_lat.n>0);
    { struct cfm_hist h={.shift=9};for(uint32_t us=100;us<=10000;us+=100)cfm_add(&h,us);
      assert(h.n==100 && cfm_pct(&h,50)>=5000 && cfm_pct(&h,50)<=5512 && cfm_pct(&h,99)==10000 && cfm_pct(&h,100)==10000); }
    ltap(400,170);for(int i=0;i<8;i++)frame(100);
    assert(cf.panel);
    int32_t before_swipe=cf.pos;
    ldrag(500,300,200);for(int i=0;i<12;i++)frame(100);
    assert(!cf.panel && cf.pos==before_swipe+CF_ONE);
    ltap(400,170);for(int i=0;i<8;i++)frame(100);assert(cf.panel);
    ltap(64,36);frame(300);
    assert(!cf.active && lv_display_get_rotation(disp)==LV_DISPLAY_ROTATION_0);
    for(int i=0;i<5;i++)frame(100);
    /* the A-Z rail on Artists, scrubbed with a finger: the page replaces itself */
    artists_page();frame(500);
    touch_at(W-17,SB_H+80,true);for(int y=SB_H+80;y<600;y+=40)touch_at(W-17,y,true);touch_at(W-17,600,false);
    for(int i=0;i<5;i++)frame(100);
    page_pop();frame(500);
    /* pull-down from the top edge still opens the control centre */
    touch_at(240,4,true);for(int y=20;y<=500;y+=60)touch_at(240,y,true);touch_at(240,500,false);
    for(int i=0;i<5;i++)frame(100);
    assert(qp_open);qp_close();frame(400);

    {   /* from the bottom edge, up: the panel goes, whatever its list could
         * still scroll (it used to scroll the notifications first) */
        for(int i=0;i<12;i++){char b[32];snprintf(b,sizeof b,"Note %d",i);toast(b);}
        frame(3000);frame(400);
        touch_at(240,4,true);for(int y=20;y<=500;y+=60)touch_at(240,y,true);touch_at(240,500,false);
        for(int i=0;i<5;i++)frame(100);
        assert(qp_open);
        lv_obj_scroll_to_y(qp_scroll,0,LV_ANIM_OFF);frame(100);
        assert(lv_obj_get_scroll_bottom(qp_scroll)>0);           /* more to scroll to */
        /* a short, slow drag springs back, nothing scrolled */
        touch_at(240,H-20,true);touch_at(240,H-40,true);touch_at(240,H-60,true);
        assert(lv_obj_get_width(qp_grab)==96 && lv_obj_get_style_translate_y(qp,0)==-40);
        shot_now("cc-edge-grab.ppm");
        frame(120);touch_at(240,H-60,false);frame(400);
        assert(qp_open && lv_obj_get_style_translate_y(qp,0)==0 && lv_obj_get_scroll_y(qp_scroll)==0 &&
               lv_obj_get_width(qp_grab)==64);
        /* a real swipe up: it follows the finger and goes */
        touch_at(240,H-20,true);for(int y=H-60;y>=H-300;y-=40)touch_at(240,y,true);
        assert(lv_obj_get_scroll_y(qp_scroll)==0 && lv_obj_get_style_translate_y(qp,0)<=-200);
        touch_at(240,H-300,false);for(int i=0;i<5;i++)frame(100);
        assert(!qp_open);
        /* a quick flick from the edge, half the distance, goes too */
        touch_at(240,4,true);for(int y=20;y<=500;y+=60)touch_at(240,y,true);touch_at(240,500,false);
        for(int i=0;i<5;i++)frame(100);
        assert(qp_open);
        touch_at(240,H-20,true);touch_at(240,H-50,true);touch_at(240,H-80,true);touch_at(240,H-80,false);
        for(int i=0;i<5;i++)frame(100);
        assert(!qp_open);
    }
    {   /* the history off, as it ships: a toast pops up, nothing is kept */
        global_settings.sloppy_notifications=0;
        int n0=nh.n;
        SFX_RESET();toast("Hello there");frame(0);frame(90);shot_now("pop-in.ppm");frame(600);shot_now("pop.ppm");
        assert(pop.up && !lv_obj_has_flag(pop.obj,LV_OBJ_FLAG_HIDDEN) && nh.n==n0 && sfx_last==SFX_UI_NOTIFY &&
               (!banner_obj || lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN)));
        lv_area_t a;lv_obj_get_coords(pop.obj,&a);
        assert(a.x1>=16 && a.x2<=W-16 && a.y1>=6 && abs((a.x1+a.x2)/2-W/2)<=2 && lv_obj_get_style_opa(pop.obj,0)==LV_OPA_COVER);
        toast("Hello there");frame(450);
        assert(pop.count==2 && !strcmp(lv_label_get_text(pop.badge),"\xc3\x97" "2") && !lv_obj_has_flag(pop.badge,LV_OBJ_FLAG_HIDDEN));
        shot_now("pop-again.ppm");
        notify_ex(NS_MUSIC,NOTE_INFO,0,NA_NONE,NULL,"Saved to Playlists as Road trip. Twelve songs, ready to go.");frame(500);
        assert(pop.count==1 && !lv_obj_has_flag(pop.body,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(pop.badge,LV_OBJ_FLAG_HIDDEN));
        lv_obj_get_coords(pop.obj,&a);assert(a.x1>=16 && a.x2<=W-16);
        shot_now("pop-body.ppm");
        /* a failure: amber, a shake, and its moment before a routine one */
        toast_warn("Could not join Home Wi-Fi. Check the password.");frame(330);shot_now("pop-warn-shake.ppm");
        assert(lv_obj_get_style_translate_x(pop.obj,0)!=0);
        frame(600);shot_now("pop-warn.ppm");
        assert(lv_obj_get_style_translate_x(pop.obj,0)==0 && pop.cur.kind==NOTE_WARN);
        toast("Routine");frame(100);assert(pop.has_next && pop.cur.kind==NOTE_WARN);
        frame(1200);frame(400);frame(200);assert(!pop.has_next && !strcmp(pop.cur.title,"Routine") && pop.up);
        frame(2400);frame(400);assert(!pop.up && lv_obj_has_flag(pop.obj,LV_OBJ_FLAG_HIDDEN) && nh.n==n0);
        /* an ongoing event updates in place */
        notify_ex(NS_LIBRARY,NOTE_INFO,0,NA_NONE,"library","Updating your library. Keep listening.");frame(500);
        notify_ex(NS_LIBRARY,NOTE_INFO,0,NA_NONE,"library","Library updated");frame(300);
        assert(pop.up && pop.count==1 && !strcmp(pop.cur.title,"Library updated"));
        /* a tap goes where it leads */
        notify_ex(NS_MUSIC,NOTE_INFO,0,NA_PLAYLISTS,NULL,"Saved to Playlists as Road trip");frame(500);
        lv_obj_send_event(pop.obj,LV_EVENT_PRESSED,NULL);lv_obj_send_event(pop.obj,LV_EVENT_CLICKED,NULL);frame(500);
        assert(!pop.up && npages>=2);
        while(npages>1){page_pop();frame(400);}
        /* a flick up sends it away */
        toast("Flick me away");frame(500);lv_obj_get_coords(pop.obj,&a);
        touch_at(W/2,a.y1+30,true);touch_at(W/2,a.y1+10,true);touch_at(W/2,a.y1+4,true);touch_at(W/2,a.y1+4,false);frame(400);
        assert(!pop.up);
        /* the panel ends at your pet: no history to show */
        qp_glass_capture();lv_obj_remove_flag(qp,LV_OBJ_FLAG_HIDDEN);qp_open=true;qp_notes_build_rows();frame(300);
        assert(lv_obj_has_flag(qp_notes_rows,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(qp_notes_head,LV_OBJ_FLAG_HIDDEN) &&
               lv_obj_has_flag(qp_notes_empty,LV_OBJ_FLAG_HIDDEN));
        toast("Shown over the panel too");frame(600);assert(pop.up);shot_now("pop-over-panel.ppm");
        qp_close();frame(400);frame(3000);frame(400);
        /* the switch, in Settings: it answers in the style it chose */
        shortcuts_page();frame(500);assert(has_text(pages[npages-1],"Notification history"));shot("notes-settings.ppm");
        notes_switch(1);frame(400);assert(nb.id && !pop.up && notes_on());shot_now("notes-on-banner.ppm");
        notes_switch(0);frame(500);assert(pop.up && !nb.id && strstr(pop.cur.title,"Toasts it is"));shot_now("notes-off-pop.ppm");
        page_pop();frame(400);frame(3000);frame(400);
        global_settings.sloppy_notifications=1;
    }
    {   /* back from mid screen, as newer iOS: a clear swipe right anywhere */
        int depth=npages;
        system_page();frame(500);assert(npages==depth+1);
        touch_at(180,420,true);for(int x=200;x<=440;x+=30)touch_at(x,424,true);touch_at(440,424,false);
        for(int i=0;i<6;i++)frame(100);
        assert(npages==depth);
        /* a mostly vertical drag is still a scroll, not a back */
        system_page();frame(500);
        touch_at(200,600,true);for(int k=1;k<=6;k++)touch_at(200+k*8,600-k*40,true);touch_at(248,360,false);
        for(int i=0;i<6;i++)frame(100);
        assert(npages==depth+1);
        page_pop();frame(500);
        /* a song row keeps its own swipe right (add to a playlist) */
        songs_page();frame(500);
        lv_obj_t *row=NULL,*sb=lv_obj_get_child(pages[npages-1],1);
        for(lv_obj_t *h=lv_obj_get_child(sb,1);h&&!row;h=NULL)
            for(unsigned i=0;i<lv_obj_get_child_count(h);i++){lv_obj_t *r=lv_obj_get_child(h,i);
                if(lv_obj_has_flag(r,LV_OBJ_FLAG_USER_3)){lv_area_t ra;lv_obj_get_coords(r,&ra);if(ra.y1>SB_H+60&&ra.y2<H-120){row=r;break;}}}
        assert(row);
        lv_area_t ra;lv_obj_get_coords(row,&ra);int ry=(ra.y1+ra.y2)/2,songs_depth=npages;
        touch_at(150,ry,true);touch_at(180,ry,true);touch_at(210,ry,true);touch_at(220,ry,false);
        for(int i=0;i<6;i++)frame(100);
        assert(npages==songs_depth && g_state==G_IDLE);
        page_pop();frame(500);
    }

    /* picking something new while a song plays cuts it, no crossfade under the new one */
    test_status=AUDIO_STATUS_PLAY;{int stops=audio_stops;int one=0;
    assert(play_list(&one,1,0,false) && audio_stops==stops+1 && test_status==AUDIO_STATUS_PLAY);
    test_status=0;assert(play_list(&one,1,0,false) && audio_stops==stops+1);}   /* nothing to stop */

    /* Pet: pixel food picker, the control centre notification, the farewell */
    pet_load();
    sl_pet_init(&pet,pet_clock());sl_pet_choose(&pet,SL_PET_BLOB,"Mochi",pet_clock());
    pet.born_epoch=pet_clock()-2*24*3600;pet.food=pet.clean=pet.joy=pet.energy=90;pet.checksum=sl_pet_checksum(&pet);
    assert(pet_save());
    qp_build();qp_glass_capture();lv_obj_remove_flag(qp,LV_OBJ_FLAG_HIDDEN);qp_open=true;
    lv_obj_update_layout(qp);qp_refresh();frame(300);
    assert(!lv_obj_has_flag(qp_pet,LV_OBJ_FLAG_HIDDEN) && strstr(lv_label_get_text(qp_pet_name),"Mochi is happy"));
    shot("pet-notification.ppm");
    pet.food=10;pet.checksum=sl_pet_checksum(&pet);qp_refresh();frame(100);
    assert(strstr(lv_label_get_text(qp_pet_name),"is hungry"));
    lv_obj_send_event(qp_pet,LV_EVENT_CLICKED,NULL);for(int i=0;i<4;i++)frame(100);
    assert(pet_page_obj && !qp_open);
    pet.food=90;pet.checksum=sl_pet_checksum(&pet);
    pet_food_menu();frame(500);
    {lv_obj_t *fb=lv_obj_get_child(pages[npages-1],1);assert(lv_obj_get_child_count(fb)==15);}
    shot("pet-food-menu.ppm");
    lv_obj_scroll_to_y(lv_obj_get_child(pages[npages-1],1),400,LV_ANIM_OFF);frame(100);shot("pet-food-menu-2.ppm");
    page_pop();frame(500);
    int depth=npages;
    pet_long_press_cb(NULL);frame(400);assert(npages==depth+1);
    pet_end_confirm_cb(NULL);frame(100);
    assert(pet_care_locked && pet_motion==PET_FAREWELL && pet_pending_farewell && !pet.dead);
    pet_care_started-=1000;frame(50);shot("pet-farewell-cuddle.ppm");
    pet_care_started-=2000;frame(50);shot("pet-farewell-rest.ppm");
    pet_care_started-=2600;frame(50);shot("pet-farewell-ghost.ppm");
    pet_care_until=sl_ms()-1;pet_tick();frame(400);
    assert(pet.dead && !pet_care_locked && !pet_pending_farewell);
    /* back on its own screen: a stone, a ghost, poke or choose */
    assert(pet_page_obj && npages==depth && !lv_obj_has_flag(pet_choose_obj,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(pet_actions_obj,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(lv_obj_get_parent(pet_stat_canvas),LV_OBJ_FLAG_HIDDEN));   /* no needs panel */
    shot("pet-after-farewell.ppm");
    qp_refresh();assert(lv_obj_has_flag(qp_pet,LV_OBJ_FLAG_HIDDEN));
    qp_open=false;lv_obj_add_flag(qp,LV_OBJ_FLAG_HIDDEN);
    while(npages>1){page_pop();frame(400);}

    /* Tiles: editing like the phone, with real touches */
    global_settings.sloppy_home_style=1;appearance_reload(NULL);frame(600);
    while(npages>1){page_pop();frame(400);}
    for(int i=0;i<10;i++)frame(100);
    remove(MT_FILE);
    assert(mt.page==pages[0] && mt.n==15 && mt.slot[0].id==TL_MUSIC && mt.slot[0].size==SZ_WIDE);
    assert(mt_find(TL_BLUETOOTH)>=0 && mt_find(TL_COVERFLOW)==1 && mt_find(TL_ALBUMS)==2);
    {   /* the default start has no holes: the tiles fill every cell of their rows */
        int cells=0;for(int k=0;k<mt.n;k++)cells+=mt_span_w(mt.slot[k].size)*mt_span_h(mt.slot[k].size);
        assert(cells==mt.rows*4 && mt.rows==10);
        assert(mt.slot[mt_find(TL_ALBUMS)].back && mt.slot[mt_find(TL_COVERFLOW)].faces);
    }
    assert(mt.slot[mt_find(TL_PET)].size==SZ_MED && mt.pet_canvas);
    int pet_start_x=lv_obj_get_x(mt.pet_canvas);
    frame(250);
    assert(lv_obj_get_x(mt.pet_canvas)>pet_start_x);
    for(unsigned app=0;app<sizeof mt_apps;app++) {
        bool in_picker=false;
        for(unsigned option=0;option<sizeof mt_group_apps;option++)
            if(mt_apps[app]==mt_group_apps[option]) in_picker=true;
        for(unsigned option=0;option<sizeof mt_group_music;option++)   /* Cover Flow, Files */
            if(mt_apps[app]==mt_group_music[option]) in_picker=true;
        assert(in_picker);
    }
    shot("tiles-default.ppm");
    lv_obj_scroll_to_y(mt.body,700,LV_ANIM_OFF);frame(300);shot("tiles-pet-live.ppm");
    lv_obj_scroll_to_y(mt.body,0,LV_ANIM_OFF);frame(300);
    {   /* the apps list is the cards home's list */
        uint32_t rows=0;for(uint32_t k=0;k<lv_obj_get_child_count(mt.body);k++)if(lv_obj_get_height(lv_obj_get_child(mt.body,k))==78)rows++;
        assert(rows==sizeof mt_apps);
    }
    {   /* the editing tests below start from the earlier eight-tile start */
        static const uint8_t legacy[][2]={{TL_MUSIC,SZ_WIDE},{TL_CLOCK,SZ_MED},{TL_PLAYLISTS,SZ_MED},{TL_PLAYROOM,SZ_MED},
            {TL_BLUETOOTH,SZ_SMALL},{TL_UPNEXT,SZ_SMALL},{TL_SETTINGS,SZ_SMALL},{TL_PET,SZ_MED}};
        mt.n=0;for(unsigned k=0;k<sizeof legacy/sizeof legacy[0];k++)mt.slot[mt.n++]=(struct mt_slot){.id=legacy[k][0],.size=legacy[k][1],.live=1};
        mt_save();appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}
        assert(mt.n==8 && mt.slot[2].id==TL_PLAYLISTS && mt.pet_canvas);
    }
    lv_area_t ta;lv_obj_get_coords(mt.slot[2].obj,&ta);              /* Playlists, medium */
    int px=(ta.x1+ta.x2)/2, py=(ta.y1+ta.y2)/2;
    touch_at(px,py,true);for(int i=0;i<15;i++)touch_at(px,py,true);touch_at(px,py,false);frame(300);
    assert(mt.edit && mt.sel==2 && !lv_obj_has_flag(mt.ctl_size,LV_OBJ_FLAG_HIDDEN));
    shot("tiles-edit.ppm");
    for(int k=0;k<mt.n;k++){mt_select(k);   /* the edit controls stay on screen for every tile */
        assert(lv_obj_get_x(mt.ctl_unpin)>=0 && lv_obj_get_x(mt.ctl_unpin)+44<=W);
        assert(lv_obj_get_x(mt.ctl_live)>=0);}
    mt_select(2);
    {   /* small, medium, wide: growing, then back to small; resized, a tile stays where it was */
        int r0=mt.slot[2].r;
        lv_obj_send_event(mt.ctl_size,LV_EVENT_CLICKED,NULL);frame(60);
        assert(mt.slot[2].size==SZ_WIDE && lv_obj_get_width(mt.slot[2].obj)<mt_px(4));   /* growing, not jumped */
        frame(300);assert(lv_obj_get_width(mt.slot[2].obj)==mt_px(4));
        shot("tiles-edit-wide.ppm");
        lv_obj_send_event(mt.ctl_size,LV_EVENT_CLICKED,NULL);frame(400);assert(mt.slot[2].size==SZ_SMALL);
        assert(mt.slot[2].r>=r0);                                   /* never up into an earlier gap */
        shot_now("tiles-edit-small.ppm");
        lv_obj_send_event(mt.ctl_size,LV_EVENT_CLICKED,NULL);frame(400);assert(mt.slot[2].size==SZ_MED && mt.slot[2].r==r0);
    }
    int clock=mt_find(TL_CLOCK);mt_select(clock);
    lv_obj_send_event(mt.ctl_live,LV_EVENT_CLICKED,NULL);frame(300);assert(!mt.slot[clock].live && !mt.slot[clock].faces);
    lv_obj_send_event(mt.ctl_live,LV_EVENT_CLICKED,NULL);frame(300);assert(mt.slot[clock].live && mt.slot[clock].faces);
    mt_select(mt_find(TL_PET));
    lv_obj_send_event(mt.ctl_live,LV_EVENT_CLICKED,NULL);frame(300);assert(!mt.pet_canvas);
    lv_obj_send_event(mt.ctl_live,LV_EVENT_CLICKED,NULL);frame(300);assert(mt.pet_canvas);
    mt_select(mt_find(TL_BLUETOOTH));
    lv_obj_send_event(mt.ctl_unpin,LV_EVENT_CLICKED,NULL);frame(400);
    assert(mt.n==7 && mt_find(TL_BLUETOOTH)<0);
    assert(mt.sel==-1 && lv_obj_has_flag(mt.ctl_unpin,LV_OBJ_FLAG_HIDDEN));   /* a second tap unpins nothing */
    {   /* the X: a generous target above the tile; held it turns red, and a press
         * on it neither drags nor opens the tile */
        mt_select(mt_find(TL_PET));frame(300);lv_obj_update_layout(mt.body);
        lv_area_t xa;lv_obj_get_coords(mt.ctl_unpin,&xa);
        assert(lv_area_get_width(&xa)>=48);
        int n0=mt.n,np0=npages;
        touch_at(xa.x1-12,xa.y2+12,true);frame(30);   /* past its corner, still the X */
        assert(lv_obj_has_state(mt.ctl_unpin,LV_STATE_PRESSED) && !mt.dragging);
        shot_now("tiles-unpin-pressed.ppm");
        touch_at(xa.x1-12,xa.y2+12,false);frame(300);
        assert(mt.n==n0-1 && mt_find(TL_PET)<0 && npages==np0 && !mt.dragging);
        mt_pin(TL_PET,SZ_MED);frame(300);
    }
    mt_select(mt_find(TL_SETTINGS));
    /* drag the chosen tile onto the first one: it takes its place */
    int moving=mt.slot[mt.sel].id;
    lv_obj_get_coords(mt.slot[mt.sel].obj,&ta);px=(ta.x1+ta.x2)/2;py=(ta.y1+ta.y2)/2;
    lv_area_t first;lv_obj_get_coords(mt.slot[0].obj,&first);
    touch_at(px,py,true);for(int k=1;k<=8;k++)touch_at(px+((first.x1+60)-px)*k/8,py+((first.y1+60)-py)*k/8,true);
    touch_at(first.x1+60,first.y1+60,false);for(int i=0;i<4;i++)frame(100);
    assert(mt.slot[0].id==moving && mt.sel==0);
    {   /* held, a tile takes its new place at once; near the bottom edge the page scrolls on */
        mt_select(0);int id0=mt.slot[0].id;
        lv_area_t a0,a1;lv_obj_get_coords(mt.slot[0].obj,&a0);lv_obj_get_coords(mt.slot[1].obj,&a1);
        int sx=(a0.x1+a0.x2)/2,sy=(a0.y1+a0.y2)/2,tx=(a1.x1+a1.x2)/2,ty=(a1.y1+a1.y2)/2;
        touch_at(sx,sy,true);for(int k=1;k<=10;k++)touch_at(sx+(tx-sx)*k/10,sy+(ty-sy)*k/10,true);
        for(int k=0;k<4;k++)touch_at(tx,ty,true);
        assert(mt.dragging && mt.slot[1].id==id0 && lv_obj_get_style_outline_width(mt.slot[1].obj,0)==3);
        shot_now("tiles-drag.ppm");
        int y0=lv_obj_get_scroll_y(mt.body);
        for(int k=0;k<20;k++)touch_at(tx,H-120,true);
        assert(lv_obj_get_scroll_y(mt.body)>y0);
        touch_at(tx,H-120,false);frame(400);
        assert(!mt.dragging && lv_obj_get_style_outline_width(mt.slot[mt.sel].obj,0)==0);
        assert(lv_obj_get_style_translate_x(mt.slot[mt.sel].obj,0)==0 && lv_obj_get_style_translate_y(mt.slot[mt.sel].obj,0)==0);
        lv_obj_scroll_to_y(mt.body,0,LV_ANIM_OFF);frame(100);
        moving=mt.slot[0].id;
    }
    /* "+" lists what is not pinned; pinning adds a medium tile */
    lv_obj_send_event(mt.add_tile,LV_EVENT_CLICKED,NULL);frame(500);assert(npages==2);
    shot("tiles-add.ppm");
    mt_pin(TL_COVERFLOW,SZ_MED);page_pop();frame(500);
    assert(mt.n==8 && mt.slot[7].id==TL_COVERFLOW && mt.slot[7].faces && mt.slot[7].back);
    lv_obj_send_event(mt.done,LV_EVENT_CLICKED,NULL);frame(400);assert(!mt.edit);
    /* the layout survives a rebuild */
    appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}
    assert(mt.n==8 && mt.slot[0].id==moving && mt_find(TL_BLUETOOTH)<0 && mt_find(TL_COVERFLOW)==7);
    for(int i=0;i<13;i++)frame(1000);         /* live tiles peek, Cover Flow's back included */
    shot("tiles-custom.ppm");
    {   /* more live tiles: Cover Flow's fan flows on, covers swap, games show scores */
        int cfi=mt_find(TL_COVERFLOW),pos=mt.cf_pos;
        lv_obj_t *front=lv_obj_get_child(mt.slot[cfi].faces,0);
        assert(lv_obj_get_child_count(lv_obj_get_child(front,0))==5);   /* 2 dimmed sides and the centre */
        assert(mt_back(&mt.slot[cfi]) && mt.cf_pos==pos+1 && mt.cf_flow_at==mt.ticks+5);
        for(int i=0;i<7;i++)frame(1000);
        assert(!mt.cf_flow_at);
        {   /* a tap opens Cover Flow on the album the tile shows, even while something plays */
            int want=((mt.cf_pos%sl_lib.n_albums)+sl_lib.n_albums)%sl_lib.n_albums;
            g_dragged=false;lv_obj_send_event(mt.slot[cfi].obj,LV_EVENT_CLICKED,NULL);
            for(int i=0;i<12;i++)frame(100);
            assert(cf.active && cf.pos==want*CF_ONE);
            cf_close();frame(300);assert(!cf.active && mt.page);
        }
        mt_pin(TL_ALBUMS,SZ_WIDE);mt_pin(TL_ARTISTS,SZ_MED);mt_pin(TL_QUIZ,SZ_MED);
        mt_pin(TL_MAGIC,SZ_MED);mt_pin(TL_COIN,SZ_SMALL);mt_pin(TL_SONGS,SZ_SMALL);frame(300);
        struct mt_slot *al=&mt.slot[mt_find(TL_ALBUMS)];
        assert(al->back && !al->faces && lv_obj_get_child_count(al->back)==8);
        for(int k=0;k<12;k++)mt_mosaic_swap(al->back);
        for(uint32_t k=0;k<8;k++)assert(lv_obj_get_child_count(lv_obj_get_child(al->back,k))<=2);
        assert(mt.slot[mt_find(TL_ARTISTS)].faces && mt.slot[mt_find(TL_SONGS)].faces);
        {   /* small game tiles turn too: one short thing, centred */
            struct mt_slot *co=&mt.slot[mt_find(TL_COIN)];
            assert(co->size==SZ_SMALL && co->faces && mt_back(co));
            const char *t=lv_label_get_text(lv_obj_get_child(co->back,0));
            assert(!strcmp(t,"flip?")||!strcmp(t,"heads")||!strcmp(t,"tails"));
            mt_pin(TL_MAGIC,SZ_MED);struct mt_slot *mg=&mt.slot[mt_find(TL_MAGIC)];
            mg->size=SZ_SMALL;mt_fill(mg);mt_layout(false);
            assert(mg->faces && mt_back(mg) && lv_obj_get_child_count(mg->back)==1);
            mg->size=SZ_MED;mt_fill(mg);mt_layout(false);
        }
        {int st=test_status;test_status=0;                                   /* nothing playing, no peek */
         assert(!mt_back(&mt.slot[mt_find(TL_UPNEXT)]));test_status=st;}
        for(int k=0;k<mt.n;k++)if(mt.slot[k].faces&&mt.slot[k].id!=TL_UPNEXT)assert(mt_back(&mt.slot[k]));
        lv_obj_t *qb=mt.slot[mt_find(TL_QUIZ)].back;
        assert(!strcmp(lv_label_get_text(lv_obj_get_child(qb,0)),quiz_stats.games?"best winnings":"name that song"));
        lv_obj_t *mb=mt.slot[mt_find(TL_MAGIC)].back;                     /* kicker, answer, name */
        assert(lv_obj_get_child_count(mb)==3 && !strcmp(lv_label_get_text(lv_obj_get_child(mb,0)),"the ball says"));
        lv_obj_t *ab=mt.slot[mt_find(TL_ARTISTS)].back;                    /* the artist over the cover */
        assert(lv_obj_get_child_count(ab)==2);
        for(int k=0;k<mt.n;k++)if(mt.slot[k].faces){mt_peek(mt.slot[k].faces,mt_px(mt_span_h(mt.slot[k].size)));mt.slot[k].down_at=mt.ticks+4;}
        frame(700);lv_obj_scroll_to_y(mt.body,0,LV_ANIM_OFF);frame(100);shot("tiles-live-backs.ppm");
        lv_obj_scroll_to_y(mt.body,600,LV_ANIM_OFF);frame(100);shot("tiles-live-backs-2.ppm");
        lv_obj_scroll_to_y(mt.body,0,LV_ANIM_OFF);
        for(int i=0;i<5;i++)frame(1000);
        for(int i=0;i<24;i++)frame(1000);                                  /* turns run with many live tiles */
        {   /* tiles scrolled out of sight do not take turns */
            int far=-1;for(int k=0;k<mt.n;k++)if(mt.slot[k].faces&&!mt_on_screen(mt.slot[k].obj))far=k;
            assert(far>=0);int y0=lv_obj_get_y(mt.slot[far].faces);
            for(int i=0;i<3*mt.n;i++){frame(1000);assert(!mt.slot[far].down_at);}
            assert(lv_obj_get_y(mt.slot[far].faces)==y0);
        }
        {   /* between moves no animation runs, so the main loop can sleep; faces come back */
            int idle=0;for(int i=0;i<60;i++){frame(100);if(!lv_anim_count_running())idle++;}
            assert(idle>=20);
            for(int k=0;k<mt.n;k++)if(mt.slot[k].faces&&!mt.slot[k].down_at&&!lv_anim_get(mt.slot[k].faces,mt_set_face))
                assert(lv_obj_get_y(mt.slot[k].faces)==0);
        }
        shot("tiles-live.ppm");
        lv_obj_scroll_to_y(mt.body,600,LV_ANIM_OFF);frame(100);shot("tiles-live-2.ppm");
        lv_obj_scroll_to_y(mt.body,0,LV_ANIM_OFF);frame(100);
    }
    /* more tiles: grouped picker, Now playing (live) and page tiles */
    mt_edit_enter(0);lv_obj_send_event(mt.add_tile,LV_EVENT_CLICKED,NULL);frame(500);
    shot("tiles-picker.ppm");page_pop();frame(400);mt_edit_exit();frame(300);
    mt_pin(TL_NOWPLAYING,SZ_WIDE);mt_pin(TL_EQ,SZ_SMALL);frame(300);
    assert(mt_find(TL_NOWPLAYING)>=0 && mt_find(TL_EQ)>=0);
    test_status=AUDIO_STATUS_PLAY;for(int i=0;i<2;i++)frame(1000);
    assert(mt.np_state==1);
    sl_track_path(3,test_next.path,sizeof test_next.path);test_next.title="Next Light";test_next.artist="Mira Sol";
    {   int un=mt_find(TL_UPNEXT);assert(un>=0 && mt.slot[un].faces && mt_back(&mt.slot[un])); }
    lv_obj_scroll_to_y(mt.body,500,LV_ANIM_OFF);frame(200);shot("tiles-nowplaying.ppm");
    g_dragged=false;   /* a real press clears it; this event is synthetic */
    lv_obj_send_event(mt.slot[mt_find(TL_EQ)].obj,LV_EVENT_CLICKED,NULL);frame(500);
    assert(npages==2);page_pop();frame(500);
    {   /* the equalizer: octave bands, a response that matches the gains, presets */
        bool was_on=global_settings.eq_enabled;
        global_settings.eq_enabled=false;
        for(int i=0;i<EQ_NUM_BANDS;i++) global_settings.eq_band_settings[i].gain=0;
        eq_page();frame(300);
        assert(fabsf(eq_response(1000))<0.01f);
        eq_set(5,6);assert(global_settings.eq_enabled && eq_gain(5)==6);   /* an edit turns it on */
        assert(global_settings.eq_band_settings[5].cutoff==1000 && global_settings.eq_band_settings[5].q==14);
        assert(fabsf(eq_response(1000)-6)<0.3f && fabsf(eq_response(250))<0.5f);   /* one octave's reach */
        eq_set(0,6);assert(eq_response(31)>4.5f && eq_response(20)>5.5f);             /* the shelf reaches 31 Hz */
        eq_set(9,-4);assert(eq_response(16000)<-3.0f);
        eq_set(3,20);assert(eq_gain(3)==12);eq_set(3,-20);assert(eq_gain(3)==-12);   /* +-12 dB */
        lv_obj_send_event(eq_chips[5],LV_EVENT_CLICKED,NULL);frame(300);             /* Rock */
        for(int i=0;i<EQ_NUM_BANDS;i++) assert(eq_gain(i)==eq_presets[5].g[i]);
        {   /* the cut covers the curve's own highest point, which overlapping bands lift past any one band */
            float pk=0;for(int k=0;k<=60;k++){float r=eq_response(20.0f*powf(10.0f,k/20.0f));if(r>pk)pk=r;}
            assert(global_settings.eq_precut>=40 && global_settings.eq_precut>=(int)ceilf(pk*10)-1);
            assert(test_eq_precut==global_settings.eq_precut && test_eq_on && test_eq_dsp[0].gain==40);
        }
        eq_sel=3;frame(200);eq_refresh();frame(200);shot_now("equalizer.ppm");
        {   /* AutoEq: a headphone profile on the card tunes the bands' own frequency, gain and Q */
            eq_profiles_page();frame(400);shot("eq-profiles-empty.ppm");page_pop();frame(400);
            const char *af="Preamp: -6.4 dB\nFilter 1: ON LSC Fc 105 Hz Gain 5.2 dB Q 0.70\n"
                "Filter 2: ON PK Fc 160 Hz Gain -3.4 dB Q 0.53\nFilter 3: ON PK Fc 1300 Hz Gain 2.75 dB Q 1.41\n"
                "Filter 4: ON PK Fc 3000 Hz Gain -4.1 dB Q 3.20\nFilter 5: ON HSC Fc 10000 Hz Gain -2.5 dB Q 0.70\n";
            int fd=open("/AutoEq/Test Phones ParametricEQ.txt",O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);
            assert(write(fd,af,strlen(af))==(ssize_t)strlen(af));sim_close(fd);
            eq_profiles_page();frame(400);assert(find_label(pages[npages-1],"Test Phones"));shot("eq-profiles.ppm");
            lv_obj_t *row=find_label(pages[npages-1],"Test Phones");while(row&&!lv_obj_has_flag(row,LV_OBJ_FLAG_CLICKABLE))row=lv_obj_get_parent(row);
            assert(row);lv_obj_send_event(row,LV_EVENT_CLICKED,NULL);frame(500);
            const struct eq_band_setting *b=global_settings.eq_band_settings;
            assert(eq_parametric() && !strcmp(eq_profile,"Test Phones") && global_settings.eq_enabled);
            assert(b[0].cutoff==105 && b[0].gain==52 && b[0].q==7);
            assert(b[1].cutoff==160 && b[1].gain==-34 && b[1].q==5 && b[2].gain==28 && b[3].q==32);
            assert(b[9].cutoff==10000 && b[9].gain==-25 && b[5].gain==0);
            assert(eq_response(30)>3.0f && eq_response(3000)<-3.0f && global_settings.eq_precut>=52);
            assert(lv_obj_has_flag(eq_labels[0],LV_OBJ_FLAG_HIDDEN));
            eq_sel=2;eq_refresh();frame(200);assert(find_label(pages[npages-1],"+2.8 dB"));
            assert(!lv_obj_has_flag(eq_pq_row,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(eq_chips_box,LV_OBJ_FLAG_HIDDEN));
            shot_now("equalizer-autoeq.ppm");
            {   /* by hand: a sixth of an octave up, a tenth of Q wider and back */
                lv_obj_t *fq=lv_obj_get_child(eq_pq_row,0),*wq=lv_obj_get_child(eq_pq_row,1);
                lv_obj_send_event(lv_obj_get_child(fq,2),LV_EVENT_CLICKED,NULL);frame(100);   /* children: label, -, + */
                assert(b[2].cutoff==1459 && find_label(pages[npages-1],"Peak \xc2\xb7 1459 Hz \xc2\xb7 Q 1.4"));
                lv_obj_send_event(lv_obj_get_child(wq,2),LV_EVENT_CLICKED,NULL);frame(100);assert(b[2].q==15);
                lv_obj_send_event(lv_obj_get_child(wq,1),LV_EVENT_CLICKED,NULL);frame(100);assert(b[2].q==14);
                lv_obj_send_event(lv_obj_get_child(fq,1),LV_EVENT_CLICKED,NULL);frame(100);assert(b[2].cutoff==1300);
            }
            lv_obj_send_event(eq_chips[0],LV_EVENT_CLICKED,NULL);frame(200);   /* a preset: the octave bands again */
            assert(!eq_parametric() && b[5].cutoff==1000 && b[5].q==14 && !lv_obj_has_flag(eq_labels[0],LV_OBJ_FLAG_HIDDEN));
            sim_remove("/AutoEq/Test Phones ParametricEQ.txt");
            assert(eq_tenths("-3.45")==-35 && eq_tenths("0.7")==7 && eq_tenths("12")==120);
            {   /* AutoEq by name: the list comes over Wi-Fi once, a search finds the pair, a tap tunes the EQ to them */
                bool wc=wf_st.connected;wf_st.connected=true;int depth=npages;
                eq_find_page();frame(400);lv_textarea_set_text(eq_find_ta,"test");shot("eq-find.ppm");
                eq_find_go_cb(NULL);for(int i=0;i<6;i++)frame(100);
                assert(fake_find(EQ_INDEX_FILE)>=0 && eq_found_n==1 && !strcmp(eq_found[0].name,"Test Phones"));
                assert(!strcmp(eq_found[0].path,"oratory1990/over-ear/Test%20Phones") && !strcmp(eq_found[0].by,"oratory1990"));
                shot("eq-found.ppm");
                lv_obj_t *fr=find_label(pages[npages-1],"Test Phones");while(fr&&!lv_obj_has_flag(fr,LV_OBJ_FLAG_CLICKABLE))fr=lv_obj_get_parent(fr);
                assert(fr);lv_obj_send_event(fr,LV_EVENT_CLICKED,NULL);for(int i=0;i<10;i++)frame(100);
                assert(eq_parametric() && !strcmp(eq_profile,"Test Phones") && b[0].gain==30 && b[1].cutoff==2000);
                assert(fake_find("/AutoEq/Test Phones ParametricEQ.txt")>=0 && pages[npages-1]==eq_page_obj && npages==depth);
                eq_found_n=0;assert(eq_index_search("nothing like this") && eq_found_n==0);   /* offline, from the card */
                sim_remove("/AutoEq/Test Phones ParametricEQ.txt");sim_remove(EQ_INDEX_FILE);wf_st.connected=wc;
            }
        }
        {   /* batch 6: PEQ files of three kinds, the preamp, typed values, Tone tuning */
            const struct eq_band_setting *b=global_settings.eq_band_settings;
            #define EQ_FILE(path,txt) do{int fd_=open(path,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd_>=0);assert(write(fd_,txt,strlen(txt))==(ssize_t)strlen(txt));sim_close(fd_);}while(0)
            #define NOTE_HAS(w) (nh.n && (strstr(nh.list[0].title,w)||strstr(nh.list[0].body,w)))
            global_settings.sloppy_eq_preamp=0;global_settings.sloppy_eq_auto_level=1;
            /* AutoEq: Preamp and ten filters, shelves at both ends */
            EQ_FILE("/AutoEq/Ten ParametricEQ.txt","Preamp: -6.4 dB\r\n"
                "Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.70\r\nFilter 2: ON PK Fc 62 Hz Gain -1.9 dB Q 0.39\r\n"
                "Filter 3: ON PK Fc 180 Hz Gain -3.6 dB Q 0.71\r\nFilter 4: ON PK Fc 1240 Hz Gain 2.2 dB Q 1.47\r\n"
                "Filter 5: ON PK Fc 2900 Hz Gain -3.4 dB Q 2.45\r\nFilter 6: ON PK Fc 4400 Hz Gain 4.1 dB Q 3.10\r\n"
                "Filter 7: ON PK Fc 6100 Hz Gain -4.6 dB Q 4.20\r\nFilter 8: ON PK Fc 8300 Hz Gain 2.8 dB Q 2.90\r\n"
                "Filter 9: ON PK Fc 13000 Hz Gain -1.5 dB Q 1.20\r\nFilter 10: ON HSC Fc 10000 Hz Gain -2.5 dB Q 0.70\r\n");
            assert(eq_autoeq_load("/AutoEq/Ten ParametricEQ.txt","Ten"));frame(100);
            assert(eq_parametric() && b[0].cutoff==105 && b[0].gain==55 && b[0].q==7 && b[9].cutoff==10000 && b[9].gain==-25);
            assert(b[1].cutoff==62 && b[1].q==4 && b[8].cutoff==13000 && b[4].cutoff==2900 && b[4].q==25);   /* peaks low to high */
            assert(global_settings.sloppy_eq_preamp==-64 && NOTE_HAS("Tuned to your headphones"));
            assert(test_eq_precut==eq_applied_precut && eq_applied_precut>=64 && test_eq_dsp[0].gain==55);   /* -6.4 dB, or lower if a boost needs it */
            eq_refresh();frame(100);
            for(int i=0;i<10;i++) assert(!lv_obj_has_flag(eq_ticks[i],LV_OBJ_FLAG_HIDDEN));   /* Hz under a profile's graph */
            assert(!lv_obj_has_flag(lv_obj_get_parent(eq_pre_row),LV_OBJ_FLAG_HIDDEN) && lv_label_get_text(eq_level_line)[0]);
            {   /* a ten-filter profile leaves Tone no band: its sliders say so */
                tone_page();frame(400);
                assert(find_label(pages[npages-1],"No free band"));
                assert(lv_obj_has_state(lv_obj_get_child(tone_rows[TN_DEEP],2),LV_STATE_DISABLED));
                assert(!lv_obj_has_state(lv_obj_get_child(tone_rows[TN_TEMP],2),LV_STATE_DISABLED));   /* the tone controls are free */
                page_pop();frame(400);
            }
            sim_remove("/AutoEq/Ten ParametricEQ.txt");
            /* the HiBy Music app's preset, in /peq: its name, its preamp, an off band left out, a low pass kept (any filter on any band) */
            EQ_FILE("/peq/warm.json","{\n  \"name\": \"HD 600 Warm\",\n  \"pre_amp\": -3.5,\n  \"bands\": [\n"
                "    {\"filter\": \"LSC\", \"on\": true, \"freq\": 90, \"gain\": 4.0, \"Q\": 0.7},\n"
                "    {\"filter\": \"PK\", \"on\": true, \"freq\": 1000.0, \"gain\": -2.5, \"Q\": 1.41},\n"
                "    {\"filter\": \"PK\", \"on\": false, \"freq\": 3000, \"gain\": 6, \"Q\": 2},\n"
                "    {\"filter\": \"HSC\", \"on\": true, \"freq\": 12000, \"gain\": 1.5, \"Q\": 0.71},\n"
                "    {\"filter\": \"LP\", \"on\": true, \"freq\": 18000, \"gain\": 0, \"Q\": 0.71}\n  ]\n}\n");
            eq_profiles_page();frame(400);
            lv_obj_t *row=find_label(pages[npages-1],"warm");while(row&&!lv_obj_has_flag(row,LV_OBJ_FLAG_CLICKABLE))row=lv_obj_get_parent(row);
            assert(row && find_label(pages[npages-1],"HiBy Music preset"));
            lv_obj_send_event(row,LV_EVENT_CLICKED,NULL);frame(400);
            assert(pages[npages-1]==eq_page_obj && !strcmp(eq_profile,"HD 600 Warm") && global_settings.sloppy_eq_preamp==-35);
            /* four filters, a low pass among them: typed, low to high from band 2, the ends left free for Tone */
            assert(b[1].cutoff==90 && b[1].gain==40 && b[1].type==EQ_TYPE_LOW_SHELF && b[2].cutoff==1000 && b[2].gain==-25 && b[2].q==14 && b[2].type==EQ_TYPE_PEAK);
            assert(b[3].cutoff==12000 && b[3].gain==15 && b[3].type==EQ_TYPE_HIGH_SHELF && b[4].cutoff==18000 && b[4].type==EQ_TYPE_LOW_PASS && b[4].q==7);
            assert(b[0].gain==0 && b[0].type==EQ_TYPE_AUTO && b[9].gain==0 && NOTE_HAS("Tuned to your headphones"));
            assert(test_eq_dsp[4].type==EQ_TYPE_LOW_PASS && eq_response(19500)<-3.0f && eq_response(30)>3.0f);
            {   /* a pass band: its kind where the gain was, no gain to step, its dot on its own curve */
                eq_sel=4;eq_refresh();frame(100);
                assert(find_label(pages[npages-1],"Low pass") && find_label(pages[npages-1],"18000 Hz \xc2\xb7 Q 0.7"));
                assert(lv_obj_has_state(eq_steps[1],LV_STATE_DISABLED));
                eq_set(4,6);assert(b[4].gain==0);
                assert(fabsf(eq_dot_db(4)+3.0f)<0.5f);
                eq_sel=2;eq_refresh();frame(100);
                assert(!lv_obj_has_state(eq_steps[1],LV_STATE_DISABLED) && find_label(pages[npages-1],"Peak \xc2\xb7 1000 Hz \xc2\xb7 Q 1.4"));
            }
            sim_remove("/peq/warm.json");
            /* Equalizer APO: a bare "Filter:", a shelf without Q, a slope, a bandwidth in octaves, an OFF line, comments */
            EQ_FILE("/peq/config.txt","# Equalizer APO\nDevice: all\nChannel: L R\nPreamp: -4 dB\n"
                "Filter: ON LS Fc 100 Hz Gain 4 dB\nFilter 2: ON PK Fc 50 Hz Gain 3 dB Q 0.7\n"
                "Filter 3: ON HS 12dB Fc 8000 Hz Gain -2 dB\nFilter 4: ON PK Fc 2000 Hz Gain -1.5 dB BW Oct 1.0\n"
                "Filter 5: OFF PK Fc 300 Hz Gain 5 dB Q 1\n");
            assert(eq_autoeq_load("/peq/config.txt","config"));
            assert(b[0].cutoff==100 && b[0].gain==40 && b[0].q==7 && b[9].cutoff==8000 && b[9].gain==-20);
            assert(b[1].cutoff==50 && b[2].cutoff==2000 && b[2].q==14 && b[3].gain==0 && global_settings.sloppy_eq_preamp==-40);
            for(int i=0;i<EQ_NUM_BANDS;i++) assert(b[i].type==EQ_TYPE_AUTO);   /* the fixed layout holds it: placed as before types */
            assert(NOTE_HAS("Tuned to your headphones"));
            /* twelve peaks: the ten loudest, typed, on all ten bands; a Q of 20 kept */
            char many[1200];int ml=0;
            for(int i=0;i<12;i++) ml+=snprintf(many+ml,sizeof many-ml,"Filter %d: ON PK Fc %d Hz Gain %d.0 dB Q %s\n",i+1,100+i*500,i<4?1:-(i+2),i==11?"20":"1");
            EQ_FILE("/peq/many.txt",many);
            assert(eq_autoeq_load("/peq/many.txt","many") && NOTE_HAS("10 of 12 filters fit"));
            {   int loud=0;for(int i=0;i<10;i++){assert(b[i].type==EQ_TYPE_PEAK);loud+=abs(b[i].gain)>=30;}
                assert(loud==8); }                                                  /* two of the four 1 dB ones left out */
            assert(b[9].q==200 && b[9].cutoff==5600);
            EQ_FILE("/peq/sharp.txt","Filter 1: ON PK Fc 3000 Hz Gain -6 dB Q 20\n");
            assert(eq_autoeq_load("/peq/sharp.txt","sharp") && NOTE_HAS("Tuned to your headphones") && b[1].q==200);
            {   /* past Q 20, and kinds the EQ doesn't have: said so */
                EQ_FILE("/peq/sharper.txt","Filter 1: ON PK Fc 3000 Hz Gain -6 dB Q 30\nFilter 2: ON HPQ Fc 25 Hz Q 0.5\n");
                assert(eq_autoeq_load("/peq/sharper.txt","sharper") && NOTE_HAS("widened to 20") && b[2].q==200 && b[1].type==EQ_TYPE_HIGH_PASS && b[1].q==5);
                EQ_FILE("/peq/sharper.txt","Filter 1: ON PK Fc 3000 Hz Gain -6 dB Q 2\nFilter 2: ON BP Fc 1000 Hz Q 2\n");
                assert(eq_autoeq_load("/peq/sharper.txt","sharper") && NOTE_HAS("Band pass, notch and all-pass"));
                /* auto level covers what the new filters do past their gain: a Q 10 low pass rings +20 dB
                 * at its corner; a Q 20 one rings +26 dB in a band narrower than the curve's sample step,
                 * here midway between two samples (1244 Hz), and still counts */
                EQ_FILE("/peq/sharper.txt","Preamp: 0 dB\nFilter 1: ON LPQ Fc 5000 Hz Q 10\n");
                assert(eq_autoeq_load("/peq/sharper.txt","sharper") && global_settings.sloppy_eq_auto_level);
                assert(eq_applied_precut>=195 && eq_applied_precut<=205);
                EQ_FILE("/peq/sharper.txt","Preamp: 0 dB\nFilter 1: ON LPQ Fc 1244 Hz Q 20\n");
                assert(eq_autoeq_load("/peq/sharper.txt","sharper") && eq_applied_precut>=258);
                sim_remove("/peq/sharper.txt");
                assert(eq_autoeq_load("/peq/sharp.txt","sharp"));
            }
            /* malformed: rejected with a reason, the EQ as it was */
            static const struct { const char *text, *why; } bad[] = {
                { "", "No filters in that file" },
                { "hello world\nthis is not an eq\n", "No filters in that file" },
                { "{ \"bands\": [ {\"filter\": \"PK\", \"freq\": 1000", "isn't a HiBy Music EQ preset" },
                { "{ \"name\": \"x\", \"preset\": 3 }", "no EQ bands" },
                { "{ \"bands\": [ {\"filter\": \"PK\", \"gain\": 3} ] }", "has no frequency" },
                { "{ \"bands\": [ 3, 4 ] }", "isn't a HiBy Music EQ preset" },
                { "GraphicEQ: 20 0; 40 1.5; 80 2\n", "GraphicEQ" },
                { "Filter 1: ON BP Fc 1800 Hz Q 0.7\nFilter 2: ON NO Fc 60 Hz Q 30\n", "No filters the EQ has" },
            };
            for(unsigned k=0;k<sizeof bad/sizeof bad[0];k++){
                EQ_FILE("/peq/bad.txt",bad[k].text);
                assert(!eq_autoeq_load("/peq/bad.txt","bad"));
                if(!NOTE_HAS(bad[k].why)){fprintf(stderr,"bad %u: %s / %s\n",k,nh.list[0].title,nh.list[0].body);assert(0);}
                assert(!strcmp(eq_profile,"sharp") && b[1].q==200);
            }
            assert(!eq_autoeq_load("/peq/missing.txt","missing") && NOTE_HAS("Could not read the profile"));
            sim_remove("/peq/bad.txt");sim_remove("/peq/many.txt");sim_remove("/peq/sharp.txt");sim_remove("/peq/config.txt");
            {   /* typed: a profile's band, all three values; nonsense and out of range kept out */
                eq_sel=1;int depth=npages;eq_band_page(NULL);frame(400);
                assert(npages==depth+1 && kb_now() && kb_now()->page==1 && eq_ty_f && eq_ty_q);
                lv_textarea_set_text(eq_ty_g,"-3,5");lv_textarea_set_text(eq_ty_f,"1500");lv_textarea_set_text(eq_ty_q,"2.5");
                shot_now("eq-band-typed.ppm");
                eq_ty_done_cb(NULL);frame(300);
                assert(npages==depth && b[1].gain==-35 && b[1].cutoff==1500 && b[1].q==25 && find_label(pages[npages-1],"-3.5 dB"));
                eq_band_page(NULL);frame(300);lv_textarea_set_text(eq_ty_f,"25000");eq_ty_done_cb(NULL);frame(100);
                assert(npages==depth+1 && b[1].cutoff==1500 && NOTE_HAS("20 to 20000"));
                lv_textarea_set_text(eq_ty_f,"1500");lv_textarea_set_text(eq_ty_g,"-");eq_ty_done_cb(NULL);frame(100);
                assert(npages==depth+1 && NOTE_HAS("-12 to +12"));
                page_pop();frame(300);
                /* Q 20 typed; past it, said so. The band's own kind untouched stays the fixed layout */
                eq_band_page(NULL);frame(300);assert(b[1].type==EQ_TYPE_AUTO && eq_ty_type==EQ_TYPE_PEAK);
                lv_textarea_set_text(eq_ty_q,"25");eq_ty_done_cb(NULL);frame(100);assert(npages==depth+1 && NOTE_HAS("Q 0.1 to 20"));
                lv_textarea_set_text(eq_ty_q,"20");eq_ty_done_cb(NULL);frame(300);
                assert(npages==depth && b[1].q==200 && b[1].type==EQ_TYPE_AUTO && find_label(pages[npages-1],"Peak \xc2\xb7 1500 Hz \xc2\xb7 Q 20.0"));
                /* the type chips: a high pass takes the gain field away and keeps no gain */
                eq_band_page(NULL);frame(300);
                lv_obj_t *chips=NULL;
                {   lv_obj_t *l=find_label(pages[npages-1],"High pass");assert(l);chips=lv_obj_get_parent(l);
                    lv_obj_send_event(chips,LV_EVENT_CLICKED,NULL);frame(200); }
                assert(eq_ty_type==EQ_TYPE_HIGH_PASS && lv_obj_has_flag(eq_ty_gain_card,LV_OBJ_FLAG_HIDDEN));
                lv_textarea_set_text(eq_ty_f,"30");lv_textarea_set_text(eq_ty_q,"0.7");
                shot_now("eq-band-type.ppm");
                eq_ty_done_cb(NULL);frame(300);
                assert(npages==depth && b[1].type==EQ_TYPE_HIGH_PASS && b[1].gain==0 && b[1].cutoff==30 && test_eq_dsp[1].type==EQ_TYPE_HIGH_PASS);
                assert(eq_response(15)<-6.0f && fabsf(eq_response(1000))<0.2f && find_label(pages[npages-1],"High pass"));
                /* and back to a peak: the gain field again */
                eq_band_page(NULL);frame(300);
                {   lv_obj_t *l=find_label(pages[npages-1],"Peak");assert(l);lv_obj_send_event(lv_obj_get_parent(l),LV_EVENT_CLICKED,NULL);frame(200); }
                assert(!lv_obj_has_flag(eq_ty_gain_card,LV_OBJ_FLAG_HIDDEN));
                lv_textarea_set_text(eq_ty_g,"-6");lv_textarea_set_text(eq_ty_f,"1500");eq_ty_done_cb(NULL);frame(300);
                assert(b[1].type==EQ_TYPE_PEAK && b[1].gain==-60);
                (void)chips;
            }
            /* back to the octave bands: the profile's preamp goes with it */
            lv_obj_send_event(eq_chips[0],LV_EVENT_CLICKED,NULL);frame(200);
            assert(!eq_parametric() && global_settings.sloppy_eq_preamp==0);
            for(int i=0;i<10;i++) assert(lv_obj_has_flag(eq_ticks[i],LV_OBJ_FLAG_HIDDEN));
            {   /* typed on an octave band: the gain alone, to a tenth */
                eq_sel=5;eq_band_page(NULL);frame(300);assert(!eq_ty_f && !eq_ty_q);
                lv_textarea_set_text(eq_ty_g,"2.5");eq_ty_done_cb(NULL);frame(200);
                assert(b[5].gain==25 && find_label(pages[npages-1],"+2.5 dB"));
                eq_set(5,0);
            }
            {   /* the preamp: by hand, held down by auto level, warned about without it */
                global_settings.sloppy_eq_preamp=-60;eq_apply_all();assert(test_eq_precut==60);   /* flat, -6 dB */
                eq_set(4,6);assert(eq_applied_precut>=60);
                global_settings.sloppy_eq_preamp=-20;eq_apply_all();eq_refresh();
                assert(eq_applied_precut>=60 && eq_auto_cut>=40 && strstr(lv_label_get_text(eq_level_line),"so boosts don't clip"));
                int lh=lv_obj_get_height(eq_level_line);
                eq_auto_level(0);assert(eq_applied_precut==20 && strstr(lv_label_get_text(eq_level_line),"can clip"));
                lv_obj_t *ps=lv_obj_get_child(eq_pre_row,2);lv_slider_set_value(ps,8,LV_ANIM_OFF);
                lv_obj_send_event(ps,LV_EVENT_VALUE_CHANGED,NULL);assert(global_settings.sloppy_eq_preamp==40 && eq_applied_precut==-40);   /* +4 dB, as asked */
                eq_auto_level(1);assert(eq_applied_precut>=60);
                global_settings.sloppy_eq_preamp=0;eq_set(4,0);eq_refresh();
                lv_obj_update_layout(eq_level_line);
                assert(eq_applied_precut==0 && strstr(lv_label_get_text(eq_level_line),"Nothing boosted") && lv_obj_get_height(eq_level_line)==lh);   /* its words change, its room does not */
                shot_now("equalizer-level.ppm");
            }
            {   /* Tone tuning: on the octave bands with the EQ off, on the tone controls, on PBE */
                bool on=global_settings.eq_enabled;global_settings.eq_enabled=false;eq_apply_all();
                assert(!test_eq_on);
                tone_page();frame(400);
                lv_obj_t *s=lv_obj_get_child(tone_rows[TN_DEEP],2);lv_slider_set_value(s,4,LV_ANIM_OFF);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);
                assert(global_settings.sloppy_tone_deep==4 && test_eq_on && test_eq_dsp[0].gain==40 && test_eq_dsp[1].gain==12 && test_eq_dsp[5].gain==0);
                assert(eq_applied_precut>=40 && find_label(pages[npages-1],"+4 dB"));
                s=lv_obj_get_child(tone_rows[TN_TEMP],2);lv_slider_set_value(s,-2,LV_ANIM_OFF);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);
                assert(test_sound_setting==SOUND_TREBLE && test_sound_value==global_settings.treble-2 && find_label(pages[npages-1],"Warmer 2"));
                s=lv_obj_get_child(tone_rows[TN_TEXTURE],2);lv_slider_set_value(s,2,LV_ANIM_OFF);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);
                assert(global_settings.pbe==50 && test_pbe==50 && eq_applied_precut>=70 && find_label(pages[npages-1],"50%"));
                s=lv_obj_get_child(tone_rows[TN_AIR],2);lv_slider_set_value(s,-3,LV_ANIM_OFF);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);
                assert(test_eq_dsp[9].gain==-30 && tone_response(test_eq_dsp,30)>3.0f && tone_response(test_eq_dsp,16000)<-2.0f);
                assert(!lv_obj_has_state(s,LV_STATE_DISABLED) && !find_label(pages[npages-1],"No free band"));
                shot_now("tone-tuning.ppm");
                /* with a five-filter profile the free bands carry it: Deep bass and Air have none */
                const char *af="Filter 1: ON LSC Fc 105 Hz Gain 5.2 dB Q 0.70\nFilter 2: ON PK Fc 160 Hz Gain -3.4 dB Q 0.53\n"
                    "Filter 3: ON PK Fc 1300 Hz Gain 2.75 dB Q 1.41\nFilter 4: ON PK Fc 3000 Hz Gain -4.1 dB Q 3.20\nFilter 5: ON HSC Fc 10000 Hz Gain -2.5 dB Q 0.70\n";
                EQ_FILE("/AutoEq/Five ParametricEQ.txt",af);
                page_pop();frame(300);assert(eq_autoeq_load("/AutoEq/Five ParametricEQ.txt","Five"));
                tone_page();frame(400);
                s=lv_obj_get_child(tone_rows[TN_VOCAL],2);lv_slider_set_value(s,3,LV_ANIM_OFF);lv_obj_send_event(s,LV_EVENT_VALUE_CHANGED,NULL);
                assert(test_eq_dsp[5].cutoff==3200 && test_eq_dsp[5].gain==30 && test_eq_dsp[5].q==12);   /* bands 1-4 are the profile's */
                assert(test_eq_dsp[0].gain==52 && test_eq_dsp[9].gain==-25);                              /* Deep bass and Air: no room */
                assert(lv_obj_has_state(lv_obj_get_child(tone_rows[TN_DEEP],2),LV_STATE_DISABLED) && find_label(pages[npages-1],"No free band"));
                shot_now("tone-tuning-profile.ppm");
                /* back to flat, everything */
                tone_flat_cb(NULL);frame(200);
                assert(!tone_any() && test_pbe==0 && test_eq_dsp[5].gain==0 && test_sound_value==global_settings.treble);
                page_pop();frame(300);sim_remove("/AutoEq/Five ParametricEQ.txt");
                lv_obj_send_event(eq_chips[0],LV_EVENT_CLICKED,NULL);frame(200);
                global_settings.eq_enabled=on;eq_apply_all();
                /* Sound's Tone card opens it, and says whether it is on */
                sound_page();frame(400);assert(tone_snd_row && find_label(pages[npages-1],"Tone tuning"));
                lv_obj_send_event(tone_snd_row,LV_EVENT_CLICKED,NULL);frame(400);assert(pages[npages-1]==tone_page_obj);
                page_pop();frame(300);page_pop();frame(300);assert(!tone_snd_row);
            }
            #undef EQ_FILE
            #undef NOTE_HAS
        }
        lv_obj_send_event(eq_chips[0],LV_EVENT_CLICKED,NULL);frame(200);
        global_settings.eq_enabled=false;eq_refresh();frame(200);shot_now("equalizer-off.ppm");
        global_settings.eq_enabled=was_on;page_pop();frame(500);
    }
    /* Sloppy FM live tile shows the listener count */
    mt_pin(TL_RADIO,SZ_WIDE);frame(300);
    {int k=mt_find(TL_RADIO);assert(k>=0);bool found=false;lv_obj_t *t=mt.slot[k].obj;
     for(uint32_t i=0;i<lv_obj_get_child_count(t);i++){lv_obj_t *c=lv_obj_get_child(t,i);
         if(lv_obj_check_type(c,&lv_label_class)&&strstr(lv_label_get_text(c),"per second"))found=true;}
     assert(found);lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(t)-120,LV_ANIM_OFF);frame(200);shot("tiles-radio.ppm");}
    {   /* Spectrum: hooked only while seen, follows the playback buffers, settles on pause */
        mt_pin(TL_SPECTRUM,SZ_MED);frame(300);
        int k=mt_find(TL_SPECTRUM);assert(k>=0 && sp.canvas && sp.bars==14 && sp.px);
        lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(mt.slot[k].obj)-60,LV_ANIM_OFF);frame(100);
        assert(test_hook==sp_hook);
        test_chan=CHANNEL_PLAYING;
        float bass_f[]={80},loud[]={14000};
        for(int i=0;i<20;i++){test_feed(1,bass_f,loud);frame(40);}
        assert(sp.lvl[1]>0.75f && sp.lvl[10]<0.2f && sp.lvl[13]<0.2f);   /* 80 Hz: band 1 */
        float hi_f[]={5000};
        for(int i=0;i<40;i++){test_feed(1,hi_f,loud);frame(40);}
        assert(sp.lvl[11]>0.75f && sp.lvl[1]<0.2f);                       /* 5 kHz: band 11 */
        /* quiet: within a song the scale comes down slowly, so a quiet passage reads quiet... */
        float quiet[]={520,52};
        float mix_f[]={100,2500};
        for(int i=0;i<25;i++){test_feed(2,mix_f,quiet);frame(40);}
        assert(sp.top>-12 && sp.lvl[2]<0.65f);
        /* ...and a quiet record (-36 dB) gets its own as its song starts: full, a part 20 dB under it short */
        char was[MAX_PATH];strlcpy(was,test_track.path,sizeof was);strlcpy(test_track.path,"/Music/quiet.flac",sizeof test_track.path);
        for(int i=0;i<25;i++){test_feed(2,mix_f,quiet);frame(40);}
        assert(sp.top<-29 && sp.lvl[2]>0.8f && sp.lvl[9]<sp.lvl[2]-0.3f);
        strlcpy(test_track.path,was,sizeof test_track.path);
        /* music-ish: a kick, a voice and its overtones, hats */
        float mf[]={55,110,220,330,660,1300,2600,6000,9500},ma[]={9000,5000,3500,2500,1800,1100,700,380,240};
        for(int i=0;i<40;i++){ma[0]=i%6<2?9000:1500;test_feed(9,mf,ma);frame(40);}
        shot_now("tiles-spectrum.ppm");
        /* pause: no buffers, the bars fall and the tile stops redrawing */
        test_chan=CHANNEL_PAUSED;for(int i=0;i<80;i++)frame(40);
        for(int b=0;b<sp.bars;b++)assert(sp.lvl[b]==0 && sp.peak[b]<0.001f);
        assert(sp.settled);test_chan=CHANNEL_PLAYING;
        for(int i=0;i<10;i++){test_feed(9,mf,ma);frame(40);}assert(!sp.settled && sp.lvl[0]>0.3f);
        /* the control cycles the modes (kept in tiles.txt) with its own icon; every mode at every size */
        mt_edit_enter(k);
        assert(!strcmp(lv_label_get_text(lv_obj_get_child(mt.ctl_size,0)),LV_SYMBOL_REFRESH));
        float rf[24],ra[24];for(int j=0;j<24;j++)rf[j]=48*powf(1.27f,(float)j);
        static const char *const mode_shot[]={"bars","peaks","curve"};
        static const int sizes[]={SZ_WIDE,SZ_MED,SZ_SMALL};
        for(int m=0;m<3;m++){
            for(int z=0;z<3;z++){
                k=mt_find(TL_SPECTRUM);mt.slot[k].size=(uint8_t)sizes[z];mt_fill(&mt.slot[k]);mt_layout(false);
                mt_edit_exit();frame(300);
                lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(mt.slot[k].obj)-60,LV_ANIM_OFF);frame(100);
                assert(sp.mode==m && sp_mode_of(&mt.slot[k])==m && (mt.slot[k].live&1) && test_hook==sp_hook);
                assert(sp.bars==(m==2?(z==0?72:z==1?40:24):(z==0?28:z==1?14:8)));
                for(int i=0;i<45;i++){
                    for(int j=0;j<24;j++)ra[j]=(2600+1900*sinf(i*0.31f+j*1.7f))/(1+j*0.11f);
                    ra[0]=i%6<2?9000:2200;ra[1]=i%6<2?6000:2500;test_feed(24,rf,ra);frame(33);}
                char name[48];snprintf(name,sizeof name,"tiles-spectrum-%s-%s.ppm",mode_shot[m],z==0?"wide":z==1?"medium":"small");
                shot_now(name);
                mt_edit_enter(k);
            }
            mt.slot[k].size=SZ_WIDE;mt_fill(&mt.slot[k]);mt_layout(false);
            mt_size_cb(NULL);frame(300);k=mt_find(TL_SPECTRUM);
            assert(mt.slot[k].size==SZ_WIDE && sp.mode==(m+1)%3);   /* a mode, not a size */
        }
        mt_size_cb(NULL);mt_size_cb(NULL);frame(300);assert(sp.mode==SP_MODE_CURVE);
        mt_live_cb(NULL);frame(200);assert(!sp.canvas);mt_live_cb(NULL);frame(200);assert(sp.mode==SP_MODE_CURVE);   /* off and on keeps it */
        mt_edit_exit();frame(300);
        appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}
        k=mt_find(TL_SPECTRUM);assert(k>=0 && sp.mode==SP_MODE_CURVE && mt.slot[k].size==SZ_WIDE);
        lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(mt.slot[k].obj)-60,LV_ANIM_OFF);frame(100);
        /* away from view: the hook goes */
        lv_obj_scroll_to_y(mt.body,0,LV_ANIM_OFF);frame(100);
        if(!mt_on_screen(sp.canvas))assert(test_hook==NULL);
        lv_obj_scroll_to_y(mt.body,lv_obj_get_y(mt.slot[k].obj)-60,LV_ANIM_OFF);frame(100);assert(test_hook==sp_hook);
        np_show();frame(400);assert(test_hook==NULL);np_hide(0);frame(400);
        po_offer();frame(300);assert(test_hook==NULL);po_close_now();frame(100);assert(test_hook==sp_hook);   /* not under the power off sheet */
        /* live off: a plain icon; unpinned: nothing left behind */
        mt_edit_enter(k);mt_live_cb(NULL);frame(200);assert(!sp.canvas && test_hook==NULL);
        mt_live_cb(NULL);frame(200);assert(sp.canvas);
        mt_edit_exit();frame(300);
        {   /* the app: a tap on the tile, landscape, a swipe to each mode, back to the tile */
            int tk=mt_find(TL_SPECTRUM);
            lv_obj_send_event(mt.slot[tk].obj,LV_EVENT_CLICKED,NULL);frame(400);
            assert(spa.active);
            assert(spa.active && lv_display_get_rotation(disp)==LV_DISPLAY_ROTATION_90 && lv_screen_active()==spa.scr);
            assert(sp.canvas && sp.cw==SPA_W && test_hook==sp_hook);
            int m0=spa.mode;
            for(int m=0;m<3;m++){
                for(int i=0;i<45;i++){
                    for(int j=0;j<24;j++)ra[j]=(2600+1900*sinf(i*0.31f+j*1.7f))/(1+j*0.11f);
                    ra[0]=i%6<2?9000:2200;ra[1]=i%6<2?6000:2500;test_feed(24,rf,ra);frame(33);}
                char name[48];snprintf(name,sizeof name,"spectrum-app-%s.ppm",mode_shot[spa.mode]);shot_now(name);
                int before=spa.mode;
                touch_at(240,620,true);for(int y=600;y>=180;y-=60)touch_at(240,y,true);touch_at(240,180,false);frame(300);
                assert(spa.mode!=before && sp.canvas && sp.mode==spa.mode);
            }
            assert(spa.mode==m0);   /* three swipes: round to the start */
            /* a frame that changes a few bars invalidates only them */
            assert(sp.dx0==SP_COLS && sp.dx1==0);
            po_offer();frame(300);assert(!spa.active && test_hook==NULL);po_close_now();frame(200);   /* portrait sheets close it */
            assert(lv_display_get_rotation(disp)==LV_DISPLAY_ROTATION_0 && lv_screen_active()==home_scr);
            tk=mt_find(TL_SPECTRUM);assert(tk>=0 && sp.canvas && lv_obj_get_parent(sp.canvas)==mt.slot[tk].obj);
            action(ACT_SPECTRUM);frame(300);assert(spa.active);
            lv_obj_t *done=lv_obj_get_child(spa.chrome,0);lv_obj_send_event(done,LV_EVENT_CLICKED,NULL);frame(300);
            assert(!spa.active && lv_screen_active()==home_scr && sp.canvas);
        }
        mt_edit_enter(mt_find(TL_SPECTRUM));
        mt_unpin_cb(NULL);frame(300);mt_edit_exit();frame(200);
        assert(mt_find(TL_SPECTRUM)<0 && !sp.canvas && !sp.px && !sp.timer && test_hook==NULL);
    }
    {   /* Gabagool: a chomp without the clip; with one, the clip, once at a time */
        mt_pin(TL_GABAGOOL,SZ_MED);frame(300);int k=mt_find(TL_GABAGOOL);assert(k>=0 && gb.art && gb.bubble);
        lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(mt.slot[k].obj)-60,LV_ANIM_OFF);frame(300);
        shot_now("tiles-gabagool.ppm");
        SFX_RESET();g_dragged=false;
        lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(60);
        assert(sfx_last==SFX_PET_CHOMP && !lv_obj_has_flag(gb.bubble,LV_OBJ_FLAG_HIDDEN) && gb_clip.tried && !gb_clip.pcm);
        frame(40);shot_now("tiles-gabagool-tap.ppm");
        for(int i=0;i<5;i++){lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(30);}   /* hammered */
        assert(lv_anim_count_running()<40);
        frame(2000);
        /* a 1 s, 22.05 kHz mono clip: resampled to 44.1 kHz, at the effects volume */
        {   static uint8_t w[44+22050*2];memcpy(w,"RIFF",4);uint32_t v=36+22050*2;memcpy(w+4,&v,4);memcpy(w+8,"WAVEfmt ",8);
            v=16;memcpy(w+16,&v,4);uint16_t h=1;memcpy(w+20,&h,2);memcpy(w+22,&h,2);v=22050;memcpy(w+24,&v,4);
            v=44100;memcpy(w+28,&v,4);h=2;memcpy(w+32,&h,2);h=16;memcpy(w+34,&h,2);memcpy(w+36,"data",4);v=22050*2;memcpy(w+40,&v,4);
            for(int i=0;i<22050;i++){int16_t sm=(int16_t)(16000*sinf(6.2831853f*440*i/22050));memcpy(w+44+i*2,&sm,2);}
            mkdir(ROCKBOX_DIR "/sloppy");int fd=open(GB_CLIP,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);
            assert(write(fd,w,sizeof w)==(ssize_t)sizeof w);close(fd);}
        gb_clip_free();int vol=global_settings.sloppy_sfx_volume;global_settings.sloppy_sfx_volume=50;
        lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(60);
        assert(gb_clip.pcm && gb_clip.frames==22050 && labs(mix_frames-44100)<600 && mix_peak>7000 && mix_peak<8300 && mix_lr_diff==0);
        global_settings.sloppy_sfx_volume=0;mix_frames=0;
        lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(60);assert(mix_frames==0);   /* effects off: silent */
        global_settings.sloppy_sfx_volume=vol;
        /* a 96 kHz song since: the clip follows the mixer's rate, not the one it was loaded at */
        fake_mixer_hz=96000;mix_frames=0;
        lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(60);
        assert(labs(mix_frames-96000)<1300);fake_mixer_hz=44100;
        {   /* the shipped clip: 3 s at 44.1 kHz, past the 65536 frames a Q16
             * position in 32 bits can count. It used to wrap and play 45 ms. */
            enum { N=133056 };static uint8_t w[44+N*2];memcpy(w,"RIFF",4);uint32_t v=36+N*2;memcpy(w+4,&v,4);memcpy(w+8,"WAVEfmt ",8);
            v=16;memcpy(w+16,&v,4);uint16_t h=1;memcpy(w+20,&h,2);memcpy(w+22,&h,2);v=44100;memcpy(w+24,&v,4);
            v=88200;memcpy(w+28,&v,4);h=2;memcpy(w+32,&h,2);h=16;memcpy(w+34,&h,2);memcpy(w+36,"data",4);v=N*2;memcpy(w+40,&v,4);
            for(int i=0;i<N;i++){int16_t sm=(int16_t)(12000*sinf(6.2831853f*220*i/44100));memcpy(w+44+i*2,&sm,2);}
            int fd=open(GB_CLIP,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);assert(write(fd,w,sizeof w)==(ssize_t)sizeof w);close(fd);
            gb_clip_free();global_settings.sloppy_sfx_volume=100;mix_frames=0;
            lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(60);
            printf("gabagool long clip: %d frames in the file, %ld mixed\n",(int)gb_clip.frames,mix_frames);
            assert(gb_clip.frames==N && labs(mix_frames-N)<600);
            gb_clip_free();global_settings.sloppy_sfx_volume=vol;
            unlink(GB_CLIP);}
        {   /* the picture: a theater over Start, frames as they are due, gone a beat after the last */
            static uint8_t v[12+64*36*2*10];memcpy(v,"SLV1",4);uint16_t hd[]={64,36,1000,10};memcpy(v+4,hd,8);
            for(int f=0;f<10;f++)for(int i=0;i<64*36;i++){uint16_t c=(uint16_t)(f*2000+i);memcpy(v+12+(f*64*36+i)*2,&c,2);}
            int fd=open(GB_VIDEO,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);assert(write(fd,v,sizeof v)==(ssize_t)sizeof v);close(fd);
            g_dragged=false;lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(30);
            assert(gbv.scrim && gbv.frames==10 && gbv.shown==0 && gbv.w==64 && gb_theater_open());
            frame(250);frame(100);assert(gbv.shown>=2 && gbv.shown<=4);shot_now("tiles-gabagool-theater.ppm");
            uint16_t px0;memcpy(&px0,v+12+(gbv.shown*64*36+40*1+30)*2,2);
            for(int i=0;i<12;i++)frame(100);
            assert(gbv.shown==9 || !gbv.scrim);
            for(int i=0;i<10;i++)frame(100);
            assert(!gbv.scrim && !gbv.px && gbv.fd==-1);                 /* closed itself */
            lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(30);assert(gbv.scrim);
            lv_obj_send_event(gbv.scrim,LV_EVENT_CLICKED,NULL);frame(400);assert(!gbv.scrim);   /* a tap closes it */
            lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(30);assert(gbv.scrim);
            (void)px0;remove(GB_VIDEO);
        }
        for(int z=0;z<3;z++){mt_edit_enter(k);mt_size_cb(NULL);frame(300);mt_edit_exit();frame(200);k=mt_find(TL_GABAGOOL);
            assert(gb.art);lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(300);
            lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(mt.slot[k].obj)-60,LV_ANIM_OFF);
            lv_obj_send_event(mt.slot[k].obj,LV_EVENT_CLICKED,NULL);frame(100);
            char name[40];snprintf(name,sizeof name,"tiles-gabagool-%s.ppm",mt.slot[k].size==SZ_WIDE?"wide":mt.slot[k].size==SZ_SMALL?"small":"medium");shot_now(name);}
        /* both survive Start being rebuilt, as saved, and the old ones' resources go with it */
        mt_pin(TL_SPECTRUM,SZ_SMALL);frame(300);int gsize=mt.slot[k].size;
        appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}
        k=mt_find(TL_GABAGOOL);int sk=mt_find(TL_SPECTRUM);
        assert(k>=0 && sk>=0 && mt.slot[k].size==gsize && mt.slot[sk].size==SZ_SMALL && gb.art && sp.canvas && sp.bars==8);
        mt_edit_enter(sk);mt_unpin_cb(NULL);frame(300);mt_edit_exit();frame(200);k=mt_find(TL_GABAGOOL);
        mt_edit_enter(k);
        lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(mt.slot[k].obj)-60,LV_ANIM_OFF);frame(300);
        mt_unpin_cb(NULL);frame(300);mt_edit_exit();frame(200);
        assert(mt_find(TL_GABAGOOL)<0 && !gb.art && !gb_clip.file && !sp.canvas && test_hook==NULL);
        remove(GB_CLIP);
    }
    {   /* a long toast wraps inside the screen (the R1's Bluetooth error ran off both edges) */
        toast("Disconnected");frame(400);lv_obj_update_layout(banner_obj);int one=lv_obj_get_height(banner_obj);
        toast("Could not pair or route audio. Check pairing mode and retry.");frame(400);lv_obj_update_layout(banner_obj);
        lv_area_t ta;lv_obj_get_coords(banner_obj,&ta);
        assert(ta.x1>=8 && ta.x2<=W-8 && lv_obj_get_height(banner_obj)>=one && !lv_obj_has_flag(nb.body,LV_OBJ_FLAG_HIDDEN) &&
               !strcmp(lv_label_get_text(nb.body),"Check pairing mode and retry."));   /* its second sentence, under it */
        shot_now("toast-long.ppm");frame(2500);
    }
    {   /* notifications: a banner, kept in the history, read, opened, cleared */
        while(npages>1){page_pop();frame(400);}
        note_clear(0);frame(100);assert(nh.n==0);
        #define NT(i) (&nh.list[i])
        /* a title and its detail, the source from the file that said it */
        toast("Could not save the playlist. Check card space.");frame(300);
        assert(nh.n==1 && !strcmp(NT(0)->title,"Could not save the playlist") && !strcmp(NT(0)->body,"Check card space.") &&
               NT(0)->src==NS_SYSTEM && !NT(0)->read && !lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN) &&
               !strcmp(lv_label_get_text(nb.title),"Could not save the playlist") && !lv_obj_has_flag(nb.body,LV_OBJ_FLAG_HIDDEN));
        assert(note_src("/x/apps/sloppy/sloppy_wifi.inc")==NS_WIFI && note_src("sloppy_pet_games.inc")==NS_PET && note_src(NULL)==NS_SYSTEM);
        shot_now("banner-detail.ppm");
        /* the same words again at once: counted; a while later: a new event, not merged */
        long rtc0=rtc_added;
        toast("Event A");toast("Event A");frame(100);
        assert(nh.n==2 && NT(0)->count==2 && strstr(lv_label_get_text(nb.title),"\xc3\x97" "2"));
        rtc_add(30);toast("Event A");frame(100);
        assert(nh.n==3 && NT(0)->count==1);
        /* something ongoing: its one entry is brought up to date and to the top, unread again */
        notify_ex(NS_LIBRARY,NOTE_INFO,0,NA_NONE,"library","Updating your library. Keep listening.");frame(100);
        uint32_t lib=NT(0)->id;toast("Something else");
        notify_ex(NS_LIBRARY,NOTE_INFO,0,NA_MUSIC,"library","Library updated");frame(100);
        assert(nh.n==5 && NT(0)->id==lib && !strcmp(NT(0)->title,"Library updated") && NT(0)->action==NA_MUSIC);
        /* an error is not pushed off by routine words: they wait behind it, the newest of them */
        toast_warn("Could not join the network");frame(300);uint32_t err=NT(0)->id;
        toast("Routine one");toast("Routine two");frame(100);
        assert(nb.id==err && nb.next==NT(0)->id && strstr(lv_label_get_text(nb.title),"Could not join") &&
               lv_color_eq(lv_obj_get_style_bg_color(nb.icon,0),lv_color_hex(NOTE_WARN_COLOR)));
        shot_now("banner-warn.ppm");
        for(int i=0;i<25;i++)frame(100);   /* its moment over, the newest routine one shows */
        assert(!strcmp(lv_label_get_text(nb.title),"Routine two") && nb.next==0);
        /* a newer error replaces an error at once */
        toast_warn("First failure");frame(100);toast_warn("Second failure");frame(100);
        assert(strstr(lv_label_get_text(nb.title),"Second failure"));
        for(int i=0;i<60;i++)frame(100);
        assert(lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN) && nb.id==0);   /* timed out: still in the history, unread */
        for(int i=0;i<nh.n;i++)assert(!NT(i)->read);
        /* a tap on a banner that leads nowhere sends it away; one that leads somewhere goes there */
        toast("Tap me away");frame(300);assert(lv_obj_has_flag(nb.chev,LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(banner_obj,LV_EVENT_CLICKED,NULL);frame(400);
        assert(lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN) && npages==1 && !NT(0)->read);
        notify_ex(NS_MUSIC,NOTE_INFO,0,NA_PLAYLISTS,NULL,"Saved to Playlists as Road trip");frame(300);
        assert(!lv_obj_has_flag(nb.chev,LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(banner_obj,LV_EVENT_PRESSED,NULL);lv_obj_send_event(banner_obj,LV_EVENT_CLICKED,NULL);frame(500);
        assert(npages==2 && NT(0)->read && lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN));
        page_pop();frame(500);
        /* a flick up sends one away */
        toast("Flick me away");frame(300);
        {   lv_area_t b;lv_obj_get_coords(banner_obj,&b);int bx=(b.x1+b.x2)/2,by=(b.y1+b.y2)/2;
            touch_at(bx,by,true);touch_at(bx,by-10,true);touch_at(bx,by-30,true);touch_at(bx,by-30,false);frame(400);
            assert(lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN) && npages==1);}
        /* long words: the banner keeps to two lines; control characters become spaces */
        toast("A very long notification about something that happened, which goes on and on well past two lines of text on the banner\nwith a newline");
        frame(300);lv_obj_update_layout(banner_obj);
        assert(lv_obj_get_height(nb.title)==2*lv_font_get_line_height(lv_obj_get_style_text_font(nb.title,0)) && !strchr(NT(0)->title,'\n'));
        shot_now("banner-long.ppm");
        /* a burst: one banner, the newest; one chime; the history bounded */
        frame(500);int chimes=0;SFX_RESET();   /* the last chime a while ago */
        for(int i=0;i<NOTE_MAX+10;i++){char b[32];snprintf(b,sizeof b,"Burst %d",i);toast(b);if(sfx_last==SFX_UI_NOTIFY){chimes++;SFX_RESET();}}
        frame(300);
        assert(nh.n==NOTE_MAX && !strcmp(lv_label_get_text(nb.title),"Burst 49") && !strcmp(NT(NOTE_MAX-1)->title,"Burst 10") && chimes==1);
        assert(lv_obj_get_child_count(lv_layer_top())<40);
        note_clear(0);
        toast("Older");rtc_add(10);toast_warn("Could not pair or route audio. Check pairing mode and retry.");
        notify_ex(NS_BT,NOTE_INFO,0,NA_BT,NULL,"Connected");rtc_add(10);
        for(int i=0;i<6;i++){char b[32];snprintf(b,sizeof b,"Note %d",i);rtc_add(10);toast(b);}
        /* the Notification Center: no banner while it is out */
        qp_drag_to(H);qp_release(H,900);frame(500);
        assert(qp_is_visible() && lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN));
        notify_ex(NS_MUSIC,NOTE_INFO,NOTE_PRIVATE,NA_NOW_PLAYING,"cast","Joined from a phone");frame(100);
        assert(lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN) && NT(0)->priv);
        int rows=(int)lv_obj_get_child_count(qp_notes_rows);
        assert(rows==nh.n && has_text(lv_obj_get_child(qp_notes_rows,0),"Joined from a phone") &&
               !lv_obj_has_flag(qp_notes_head,LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(qp_notes_empty,LV_OBJ_FLAG_HIDDEN) &&
               strstr(lv_label_get_text(qp_notes_new),"10"));
        /* a source and an age over each; a chevron only where it leads somewhere */
        assert(has_text(lv_obj_get_child(qp_notes_rows,0),"Music") && has_text(lv_obj_get_child(qp_notes_rows,0),"now"));
        {   int with=0,without=0;
            for(int i=0;i<rows;i++){lv_obj_t *r=lv_obj_get_child(qp_notes_rows,i);const struct note *e=note_find((uint32_t)(uintptr_t)lv_obj_get_user_data(r));
                if(e->action){assert(lv_obj_get_child_count(r)==7 && lv_obj_has_flag(r,LV_OBJ_FLAG_CLICKABLE));with++;}
                else{assert(lv_obj_get_child_count(r)==6 && !lv_obj_has_flag(r,LV_OBJ_FLAG_CLICKABLE));without++;}}
            assert(with==2 && without==rows-2);}
        hidden_if_changed(qp_pet,false);frame(100);shot_now("nc-with-pet.ppm");   /* under the pet */
        {   lv_area_t pa,ha;lv_obj_get_coords(qp_pet,&pa);lv_obj_get_coords(qp_notes_head,&ha);assert(ha.y1>pa.y2);}
        hidden_if_changed(qp_pet,true);frame(100);
        {   lv_area_t na,ha;lv_obj_get_coords(qp_notes,&na);lv_obj_get_coords(qp_notes_head,&ha);assert(ha.y1==na.y1);}
        /* read: only what was in sight for a second; the rows below the fold stay unread.
         * (periodic_ui() refreshes the open panel on the player; the harness calls it) */
        for(int i=0;i<5;i++){frame(300);qp_refresh();}
        shot_now("nc-no-pet.ppm");
        int nread=0;for(int i=0;i<nh.n;i++)nread+=NT(i)->read;
        assert(nread>0 && nread<nh.n && !NT(nh.n-1)->read && note_unread()==nh.n-nread);
        assert(lv_obj_has_flag(lv_obj_get_child(lv_obj_get_child(qp_notes_rows,0),0),LV_OBJ_FLAG_HIDDEN));   /* its dot gone */
        lv_obj_scroll_to_y(qp_scroll,LV_COORD_MAX,LV_ANIM_OFF);frame(300);for(int i=0;i<5;i++){frame(300);qp_refresh();}
        assert(NT(nh.n-1)->read);   /* scrolled to: read */
        {   /* the whole panel scrolls, tiles and all; a drag up only puts it
             * away from the foot of the list, never mid-scroll */
            lv_obj_scroll_to_y(qp_scroll,0,LV_ANIM_OFF);frame(100);
            assert(lv_obj_get_scroll_bottom(qp_scroll)>200);
            lv_area_t t0,t1;lv_obj_get_coords(qp_tiles[0],&t0);
            for(int y=700;y>=400;y-=30)touch_at(30,y,true);
            touch_at(30,400,false);frame(600);
            lv_obj_get_coords(qp_tiles[0],&t1);shot_now("nc-scrolled.ppm");
            printf("panel scroll: tiles moved %d px, panel at %d\n",(int)(t0.y1-t1.y1),(int)lv_obj_get_style_translate_y(qp,0));
            assert(qp_open && t1.y1<t0.y1-150 && lv_obj_get_style_translate_y(qp,0)==0);
            lv_obj_scroll_to_y(qp_scroll,LV_COORD_MAX,LV_ANIM_OFF);frame(100);
            for(int y=700;y>=400;y-=30)touch_at(30,y,true);
            touch_at(30,400,false);frame(600);assert(!qp_open);
            qp_drag_to(H);qp_release(H,900);frame(500);qp_refresh();
            assert(qp_open && lv_obj_get_scroll_y(qp_scroll)==0);   /* back at the top */
        }
        /* the ages change in place, a minute on, without a rebuild */
        lv_obj_t *first=lv_obj_get_child(qp_notes_rows,0);rtc_add(120);frame(300);qp_refresh();
        assert(lv_obj_get_child(qp_notes_rows,0)==first && strstr(lv_label_get_text(lv_obj_get_child(first,2)),"min"));
        /* a row that leads somewhere opens it and puts the panel away */
        {   lv_obj_t *bt=NULL;for(uint32_t i=0;i<lv_obj_get_child_count(qp_notes_rows);i++)if(has_text(lv_obj_get_child(qp_notes_rows,i),"Connected"))bt=lv_obj_get_child(qp_notes_rows,i);
            assert(bt);lv_obj_send_event(bt,LV_EVENT_CLICKED,NULL);frame(600);
            assert(!qp_open && npages==2 && sl_bt_page_obj==pages[1]);page_pop();frame(500);
            qp_drag_to(H);qp_release(H,900);frame(500);}
        /* clear one: its x */
        uint32_t older=0;for(int i=0;i<nh.n;i++)if(!strcmp(NT(i)->title,"Older"))older=NT(i)->id;
        lv_obj_t *row=NULL;for(uint32_t i=0;i<lv_obj_get_child_count(qp_notes_rows);i++)if(lv_obj_get_user_data(lv_obj_get_child(qp_notes_rows,i))==(void *)(uintptr_t)older)row=lv_obj_get_child(qp_notes_rows,i);
        assert(older && row);int n0=nh.n;
        lv_obj_send_event(lv_obj_get_child(row,5),LV_EVENT_CLICKED,NULL);frame(100);
        assert(nh.n==n0-1 && !note_find(older) && (int)lv_obj_get_child_count(qp_notes_rows)==nh.n);
        /* saved to the card a moment later: read state kept, the private one left off */
        for(int i=0;i<40;i++)frame(100);
        {   struct note keep[NOTE_MAX];int kn=nh.n;memcpy(keep,nh.list,sizeof keep);
            memset(&nh,0,sizeof nh);notes_load();
            assert(nh.n==kn-1 && !strcmp(NT(0)->title,keep[1].title) && NT(0)->read==keep[1].read && NT(0)->src==keep[1].src);
            for(int i=0;i<nh.n;i++)assert(!strstr(NT(i)->title,"from a phone"));
            memcpy(nh.list,keep,sizeof keep);nh.n=kn;nh.loaded=true;}
        /* power off before the save is due: written at once */
        toast("Just before power off");remove(NOTE_FILE);notes_flush();
        {   int fd=open(NOTE_FILE,O_RDONLY);assert(fd>=0);char b[64]={0};assert(read(fd,b,5)==5 && !memcmp(b,"NOT2\n",5));close(fd);}
        /* clear all: the empty state, no heading, nothing to scroll */
        lv_obj_send_event(lv_obj_get_child(qp_notes_head,2),LV_EVENT_CLICKED,NULL);frame(100);
        assert(nh.n==0 && lv_obj_get_child_count(qp_notes_rows)==0 && lv_obj_has_flag(qp_notes_head,LV_OBJ_FLAG_HIDDEN) &&
               !lv_obj_has_flag(qp_notes_empty,LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(qp_notes,LV_OBJ_FLAG_SCROLLABLE));
        shot_now("nc-empty.ppm");
        /* a row glimpsed, the panel closed and opened again: not read at once, a second later */
        {   toast("Glimpsed");frame(100);qp_refresh();
            uint32_t gl=NT(0)->id;assert(!NT(0)->read && NT(0)->seen_at);
            qp_close();for(int i=0;i<30;i++)frame(100);
            qp_drag_to(H);qp_release(H,900);frame(500);qp_refresh();
            assert(!note_find(gl)->read);
            for(int i=0;i<5;i++){frame(300);qp_refresh();}
            assert(note_find(gl)->read);note_clear(0);frame(100);}
        /* closing while one arrives: it waits for no one; the banner comes after, not under the panel */
        qp_close();toast("Arrived while closing");frame(600);
        assert(!qp_is_visible() && nh.n==1);
        note_clear(0);for(int i=0;i<40;i++)frame(100);
        assert(lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN));
        rtc_add(rtc0-rtc_added);
        #undef NT
    }
    {   /* pull to refresh is off unless chosen: a long pull only springs back */
        while(npages>1){page_pop();frame(400);}
        assert(global_settings.sloppy_pull_refresh==0);
        lv_obj_scroll_to_y(mt.body,0,LV_ANIM_OFF);frame(300);
        lv_obj_t *before=pages[0];int dbs=db_update_state;
        lv_obj_set_user_data(before,(void *)0x5eed);
        touch_at(240,380,true);for(int i=1;i<=22;i++)touch_at(240,380+i*12,true);
        assert(!ptr.armed && lv_obj_has_flag(ptr.ring,LV_OBJ_FLAG_HIDDEN));
        touch_at(240,644,false);for(int i=0;i<60;i++)frame(16);
        assert(!ptr.refreshing && lv_obj_get_user_data(pages[0])==(void *)0x5eed && db_update_state==dbs && lv_obj_get_scroll_y(mt.body)==0);
        /* the switch in Customization > Home layout turns it on, and is saved */
        home_style_page();frame(400);
        lv_obj_t *sw=NULL;
        for(lv_obj_t *o=lv_obj_get_child(pages[npages-1],0);o;){   /* depth-first, for the switch */
            if(lv_obj_check_type(o,&lv_switch_class)){sw=o;break;}
            if(lv_obj_get_child_count(o)){o=lv_obj_get_child(o,0);continue;}
            while(o && o!=pages[npages-1] && !lv_obj_get_sibling(o,1))o=lv_obj_get_parent(o);
            o=o && o!=pages[npages-1]?lv_obj_get_sibling(o,1):NULL;}
        assert(sw && !lv_obj_has_state(sw,LV_STATE_CHECKED));
        shot_now("home-layout-pull-refresh.ppm");
        int saves=settings_save_calls;
        lv_obj_send_event(lv_obj_get_parent(sw),LV_EVENT_CLICKED,NULL);frame(100);
        assert(global_settings.sloppy_pull_refresh==1 && lv_obj_has_state(sw,LV_STATE_CHECKED) && settings_save_calls>saves);
        page_pop();frame(400);
    }
    {   /* pull to refresh on Start: a short pull springs back, a long one rebuilds Start and scans the library */
        while(npages>1){page_pop();frame(400);}
        lv_obj_scroll_to_y(mt.body,0,LV_ANIM_OFF);frame(300);
        lv_obj_t *before=pages[0];
        lv_obj_set_user_data(before,(void *)0x5eed);   /* a new page may reuse its address */
        touch_at(240,420,true);for(int i=1;i<=6;i++)touch_at(240,420+i*10,true);
        assert(!ptr.armed && !lv_obj_has_flag(ptr.ring,LV_OBJ_FLAG_HIDDEN) && ptr.progress>0);
        touch_at(240,480,false);for(int i=0;i<30;i++)frame(16);
        assert(!ptr.refreshing && pages[0]==before && lv_obj_get_user_data(pages[0])==(void *)0x5eed && lv_obj_get_scroll_y(mt.body)==0);
        SFX_RESET();
        touch_at(240,380,true);for(int i=1;i<=22;i++)touch_at(240,380+i*12,true);
        assert(ptr.armed && sfx_last==SFX_UI_TICK);frame(100);shot_now("pull-refresh-armed.ppm");
        touch_at(240,644,false);frame(200);
        assert(ptr.refreshing);shot_now("pull-refresh-spin.ppm");
        int dbs=db_update_state;
        for(int i=0;i<60;i++)frame(16);frame(600);
        assert(lv_obj_get_user_data(pages[0])!=(void *)0x5eed && npages==1 && mt.page==pages[0] && !ptr.refreshing && dbs==DBU_IDLE && db_update_state!=DBU_IDLE);
        while(db_update_state!=DBU_IDLE){db_update_state=DBU_IDLE;}   /* the test card has no scan to run */
        frame(500);
        global_settings.sloppy_pull_refresh=0;
    }
    {   /* widgets: Up next, bigger, shows what comes next; Playlists lists them and plays one */
        while(npages>1){page_pop();frame(400);}
        int q0=queued,s0=started,st0=test_status;
        queued=6;for(int k=0;k<6;k++){queue_tracks[k]=k;queue_flags[k]=false;}started=0;test_status=AUDIO_STATUS_PLAY;
        mt_pin(TL_UPNEXT,SZ_MED);int k=mt_find(TL_UPNEXT);
        mt.slot[k].size=SZ_MED;mt.slot[k].live|=1;mt_fill(&mt.slot[k]);mt_layout(false);frame(300);
        lv_obj_t *t=mt.slot[k].obj;
        assert(has_text(t,sl_lib.tracks[1].title) && has_text(t,"UP NEXT") && has_text(t,"5"));
        lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(t)-60,LV_ANIM_OFF);frame(200);shot_now("tiles-upnext-medium.ppm");
        mt.slot[k].size=SZ_WIDE;mt_fill(&mt.slot[k]);mt_layout(false);frame(300);t=mt.slot[k].obj;
        assert(has_text(t,sl_lib.tracks[1].title) && has_text(t,sl_lib.tracks[2].title) && has_text(t,sl_lib.tracks[3].title));
        assert(!has_text(t,sl_lib.tracks[4].title) || !strcmp(sl_lib.tracks[4].title,sl_lib.tracks[1].title));
        lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(t)-60,LV_ANIM_OFF);frame(200);shot_now("tiles-upnext-wide.ppm");
        started=1;frame(1100);t=mt.slot[mt_find(TL_UPNEXT)].obj;
        {char want[16];snprintf(want,sizeof want,"%d songs",q_ahead());assert(has_text(t,sl_lib.tracks[4].title) && has_text(t,want) && !has_text(t,sl_lib.tracks[1].title));}
        mt.slot[k].size=SZ_SMALL;mt_fill(&mt.slot[k]);mt_layout(false);frame(200);
        assert(mt.queue_badge && !has_text(mt.slot[k].obj,"UP NEXT"));             /* small: as it was */
        fake_pl_count=4;
        mt_pin(TL_PLAYLISTS,SZ_WIDE);int p=mt_find(TL_PLAYLISTS);
        mt.slot[p].size=SZ_WIDE;mt_fill(&mt.slot[p]);mt_layout(false);frame(300);t=mt.slot[p].obj;
        assert(mt.pl_rows==3 && has_text(t,"Road trip") && has_text(t,"Late night") && has_text(t,"4") && !has_text(t,"Gym"));
        lv_obj_update_layout(mt.body);lv_obj_scroll_to_y(mt.body,lv_obj_get_y(t)-60,LV_ANIM_OFF);frame(200);shot_now("tiles-playlists-wide.ppm");
        lv_area_t ta;lv_obj_get_coords(t,&ta);
        assert(mt_playlist_hit(t,ta.y1+mt.pl_y[1]+20)==1 && mt_playlist_hit(t,ta.y1+5)==-1);
        g_dragged=false;touch_at(ta.x1+120,ta.y1+mt.pl_y[1]+20,true);touch_at(ta.x1+120,ta.y1+mt.pl_y[1]+20,false);frame(300);
        assert(npages==1);   /* a row plays its playlist (here: a missing file, said in a toast), not the page */
        mt.slot[p].size=SZ_SMALL;mt_fill(&mt.slot[p]);mt_layout(false);frame(200);assert(!has_text(mt.slot[p].obj,"Road trip"));
        fake_pl_count=1;queued=q0;started=s0;test_status=st0;
        mt_edit_enter(mt_find(TL_PLAYLISTS));mt_unpin_cb(NULL);frame(200);mt_select(mt_find(TL_UPNEXT));mt_unpin_cb(NULL);frame(200);mt_edit_exit();frame(300);
    }
    {   /* slide to power off: a hold brings it up, a full slide turns off, anything else puts it away */
        bool was_running=sl_power_running;sl_power_running=true;sl_power_suspended=false;
        sl_power_state_init(&sl_power,sl_ms());sl_power_apply();frame(100);
        int offs=poweroffs;
        sl_power_filter_button(BUTTON_POWER);sl_power.power_at-=SL_POWER_HOLD_MS+50;sl_power_tick();frame(300);
        assert(po_is_open() && poweroffs==offs);
        sl_power_filter_button(BUTTON_POWER|BUTTON_REL);frame(100);assert(po_is_open());   /* letting go of that hold keeps it */
        /* what comes meanwhile waits under it, a failure before routine words, nothing moving */
        toast_warn("Could not save while sliding");uint32_t held=nh.list[0].id;toast("Routine while sliding");uint32_t routine=nh.list[0].id;frame(300);
        assert(nb.id!=held && nb.next==held && !lv_anim_get(banner_obj,NULL));
        frame(900);shot_now("power-slider.ppm");
        lv_area_t k;lv_obj_get_coords(po.knob,&k);int kx=(k.x1+k.x2)/2,ky=(k.y1+k.y2)/2;
        {   /* the knob starts in the back-swipe edge: on a pushed page, a drag from it is the slider's */
            int np0=npages;play_settings_page();frame(400);assert(npages==np0+1 && page_depth()>1);
            int ex=k.x1+6;touch_at(ex,ky,true);for(int i=1;i<=6;i++){touch_at(ex+i*14,ky,true);frame(16);}
            assert(g_state==G_LVGL && po.dragging && po.x>=60 && npages==np0+1);
            touch_at(ex+84,ky,false);for(int i=0;i<40;i++)frame(16);
            assert(po_is_open() && po.x==0 && npages==np0+1 && poweroffs==offs);
            /* the same picture whether the knob's shape is kept or worked out again */
            int cw=PO_KNOB+26+2*PO_GROW+2*PO_KM,chh=PO_KNOB+PO_GROW+2*PO_KM;uint8_t *b1=malloc(cw*chh*3),*b2=malloc(cw*chh*3);
            struct sl_lens_cache lc={0};int tw=PO_TRACK_W+2*PO_M,th=PO_TRACK_H+2*PO_M;
            struct sl_lens_src src={(const uint16_t *)po.track_buf,tw,tw,th,PO_X-PO_M,PO_Y-PO_M,256,(const uint16_t *)po.words_buf,180};
            for(int j=0;j<6;j++){int ww=PO_KNOB+(j%3)*13,hh=PO_KNOB-(j%3)*4,cx=ww+2*PO_KM,cy=hh+2*PO_KM,ox=PO_X+j*57;
                memset(b1,1,cw*chh*3);memset(b2,2,cw*chh*3);
                sl_lens(&src,ox,PO_Y,cx,cy,PO_KM,hh/2.0f,&sl_lens_clear,(uint16_t*)b1,b1+cx*cy*2);
                sl_lens_moved(&lc,&src,ox,PO_Y,cx,cy,PO_KM,hh/2.0f,&sl_lens_clear,(uint16_t*)b2,b2+cx*cy*2);
                assert(!memcmp(b1,b2,cx*cy*3));}
            for(int j=0;j<2;j++){int ww=PO_KNOB+(j?7:20),hh=PO_KNOB+(j?3:10),cx=ww+2*PO_KM,cy=hh+2*PO_KM;   /* every pixel along, odd and even */
                for(int ox=PO_X-PO_KM;ox<PO_X+PO_TRAVEL;ox++){
                    sl_lens(&src,ox,PO_Y,cx,cy,PO_KM,hh/2.0f,&sl_lens_clear,(uint16_t*)b1,b1+cx*cy*2);
                    sl_lens_moved(&lc,&src,ox,PO_Y,cx,cy,PO_KM,hh/2.0f,&sl_lens_clear,(uint16_t*)b2,b2+cx*cy*2);
                    assert(!memcmp(b1,b2,cx*cy*3));}}
            sl_lens_cache_free(&lc);free(b1);free(b2);
            page_pop();frame(400);assert(npages==np0);
            lv_obj_get_coords(po.knob,&k);kx=(k.x1+k.x2)/2;ky=(k.y1+k.y2)/2;
        }
        assert(lv_area_get_height(&k)==PO_KNOB && PO_TRACK_H-PO_KNOB==2*PO_INSET);       /* the handle centred in the thick track */
        touch_at(kx,ky,true);frame(200);touch_at(kx,ky,false);for(int i=0;i<40;i++)frame(16);
        assert(po_is_open() && po.x==0 && poweroffs==offs && po.dark==0);   /* a tap is not a slide */
        assert(po.rim && lv_obj_get_style_image_opa(po.rim,0)==LV_OPA_COVER && !lv_obj_has_flag(po.rim,LV_OBJ_FLAG_HIDDEN));   /* the rim, lit */
        touch_at(kx,ky,true);for(int i=1;i<=8;i++){touch_at(kx+i*18,ky,true);frame(16);}
        assert(po.x>100 && po.w>PO_KNOB);shot_now("power-slider-drag.ppm");                  /* dragged fast, it stretches */
        assert(lv_obj_get_style_image_opa(po.rim,0)>0 && lv_obj_get_style_image_opa(po.rim,0)<200);        /* the rim going with the slide */
        assert(po.dark>60 && lv_obj_get_index(po.track)>lv_obj_get_index(po.dim) && po.shown>0 && po.shown<po.dark &&
               lv_obj_get_style_opa(po.cancel,0)<LV_OPA_COVER);   /* the rest dims, following; the slider stays lit */
        for(int i=0;i+1<PO_STRIPS;i++)assert(abs(po.strip_dark[i]-po.strip_dark[i+1])<=(PO_STRIPS-1)*PO_DARK_STEP);   /* no strip far from its neighbour */
        assert(po.lens_opa>0 && po.knob_buf && !lv_obj_has_flag(po.lens,LV_OBJ_FLAG_HIDDEN));   /* held: a lens */
        {   struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);for(int i=0;i<20;i++)po_lens_draw();clock_gettime(CLOCK_MONOTONIC,&t1);
            long us=((t1.tv_sec-t0.tv_sec)*1000000000L+(t1.tv_nsec-t0.tv_nsec))/20000;
            uint16_t *tb=malloc((432)*(102)*2);clock_gettime(CLOCK_MONOTONIC,&t0);
            struct sl_lens_src bs={qp_glass_px,W,W,H,0,0,160,NULL,0};sl_lens(&bs,24,120,432,102,12,39,&sl_lens_pane,tb,NULL);
            clock_gettime(CLOCK_MONOTONIC,&t1);free(tb);
            printf("glass: knob lens %ld us per frame, track pane %ld us once\n",us,((t1.tv_sec-t0.tv_sec)*1000000000L+(t1.tv_nsec-t0.tv_nsec))/1000);}
        touch_at(kx+144,ky,false);for(int i=0;i<40;i++)frame(16);
        assert(po_is_open() && po.x==0 && po.w==PO_KNOB && poweroffs==offs);                /* short of the end: back */
        assert(po.dark==0 && lv_obj_get_style_image_opa(po.rim,0)==LV_OPA_COVER && !lv_obj_has_flag(po.rim,LV_OBJ_FLAG_HIDDEN));   /* the screen lights again, the rim comes back */
        sl_power_filter_button(BUTTON_POWER);frame(50);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);frame(400);
        assert(!po_is_open() && sl_power.mode<SL_POWER_LOCKED);                             /* a press puts it away */
        assert(nb.id==held && !lv_obj_has_flag(banner_obj,LV_OBJ_FLAG_HIDDEN));               /* and the failure shows now */
        note_clear(held);note_clear(routine);frame(300);
        sl_power_filter_button(BUTTON_POWER);sl_power.power_at-=SL_POWER_HOLD_MS+50;sl_power_tick();frame(300);
        sl_power_filter_button(BUTTON_POWER|BUTTON_REL);assert(po_is_open());
        lv_obj_t *cancel=po.cancel;
        lv_obj_send_event(cancel,LV_EVENT_CLICKED,NULL);frame(400);assert(!po_is_open());
        /* opened again: fresh, nothing left over from last time */
        sl_power_filter_button(BUTTON_POWER);sl_power.power_at-=SL_POWER_HOLD_MS+50;sl_power_tick();frame(300);
        sl_power_filter_button(BUTTON_POWER|BUTTON_REL);frame(300);
        assert(po_is_open() && po.x==0 && po.lens_opa==0 && !po.off && po.dark==0 &&
               po.words_opa==LV_OPA_COVER);
        {   /* profile: a slow drag, then the spring back; CPU time and pixels redrawn per frame */
            lv_obj_get_coords(po.knob,&k);int sx=(k.x1+k.x2)/2,sy=(k.y1+k.y2)/2;
            touch_at(sx,sy,true);for(int i=0;i<12;i++)frame(16);
            long px_sum=0,px_max=0;uint64_t us_sum=0,us_max=0;int nf=0;
            for(int i=1;i<=50;i++){
                long px0=flushed_px;struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);
                touch_at(sx+i*5,sy,true);frame(16);
                clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);uint64_t us=(uint64_t)((b.tv_sec-a.tv_sec)*1000000+(b.tv_nsec-a.tv_nsec)/1000);
                long px=flushed_px-px0;px_sum+=px;if(px>px_max)px_max=px;us_sum+=us;if(us>us_max)us_max=us;nf++;}
            long rpx_sum=0;uint64_t rus_sum=0;
            {   long px0=flushed_px;struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);
                touch_at(sx+250,sy,false);for(int i=0;i<40;i++)frame(16);
                clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);rus_sum=(uint64_t)((b.tv_sec-a.tv_sec)*1000000+(b.tv_nsec-a.tv_nsec)/1000);rpx_sum=flushed_px-px0;}
            printf("power slider drag: %llu us avg, %llu us worst per step; %ld px avg (%.0f%% of the screen), %ld px worst; spring back: %llu us, %ld px over 41 frames\n",
                   (unsigned long long)(us_sum/nf),(unsigned long long)us_max,px_sum/nf,100.0*px_sum/nf/(W*H),px_max,
                   (unsigned long long)rus_sum,rpx_sum);
            assert(po_is_open() && po.x==0 && poweroffs==offs);
            /* reversed while held: the rim and the light come back with the knob */
            touch_at(sx,sy,true);for(int i=1;i<=40;i++){touch_at(sx+i*6,sy,true);frame(16);}
            int far_rim=lv_obj_get_style_image_opa(po.rim,0),far_dark=po.dark;
            for(int i=39;i>=8;i--){touch_at(sx+i*6,sy,true);frame(16);}
            assert(lv_obj_get_style_image_opa(po.rim,0)>far_rim && po.dark<far_dark && po.x<80 && po.dragging);
            touch_at(sx+48,sy,false);for(int i=0;i<40;i++)frame(16);
            assert(po_is_open() && po.x==0 && po.dark==0 && lv_obj_get_style_image_opa(po.rim,0)==LV_OPA_COVER && poweroffs==offs);
        }
        /* the finger far off the track still drives the knob, clamped at the end;
         * let go one short of the threshold, it goes back */
        lv_obj_get_coords(po.knob,&k);kx=(k.x1+k.x2)/2;ky=(k.y1+k.y2)/2;
        touch_at(kx,ky,true);for(int i=1;i<=12;i++){touch_at(kx+i*36,ky+i*25,true);frame(16);}
        assert(po.dragging && po.x==PO_TRAVEL);
        touch_at(kx+PO_TRAVEL*9/10-1,ky+300,true);frame(16);assert(po.x==PO_TRAVEL*9/10-1);
        touch_at(kx+PO_TRAVEL*9/10-1,ky+300,false);for(int i=0;i<40;i++)frame(16);
        assert(po_is_open() && po.x==0 && !po.off && poweroffs==offs);
        {   /* opened and put away again and again: nothing left running */
            uint32_t ev0=lv_display_get_event_count(disp);int t0=0;for(lv_timer_t *t=lv_timer_get_next(NULL);t;t=lv_timer_get_next(t))t0++;
            lv_obj_send_event(po.cancel,LV_EVENT_CLICKED,NULL);frame(400);assert(!po_is_open());
            for(int r=0;r<3;r++){
                sl_power_filter_button(BUTTON_POWER);sl_power.power_at-=SL_POWER_HOLD_MS+50;sl_power_tick();frame(300);
                sl_power_filter_button(BUTTON_POWER|BUTTON_REL);frame(100);assert(po_is_open());
                lv_obj_get_coords(po.knob,&k);touch_at(k.x1+40,ky,true);touch_at(k.x1+140,ky,true);touch_at(k.x1+140,ky,false);
                lv_obj_send_event(po.cancel,LV_EVENT_CLICKED,NULL);
                lv_obj_get_coords(po.knob,&k);touch_at(k.x1+40,ky,true);frame(16);assert(!po.dragging);   /* going away: not taken */
                touch_at(k.x1+40,ky,false);frame(400);assert(!po_is_open());}
            int t1=0;for(lv_timer_t *t=lv_timer_get_next(NULL);t;t=lv_timer_get_next(t))t1++;
            assert(lv_display_get_event_count(disp)==ev0-1 && t1==t0-2);   /* its refresh hook, its two timers: gone */
            sl_power_filter_button(BUTTON_POWER);sl_power.power_at-=SL_POWER_HOLD_MS+50;sl_power_tick();frame(300);
            sl_power_filter_button(BUTTON_POWER|BUTTON_REL);frame(100);assert(po_is_open());}
        /* all the way, then the contact taken away instead of let go: back, not off */
        lv_obj_get_coords(po.knob,&k);kx=(k.x1+k.x2)/2;ky=(k.y1+k.y2)/2;
        touch_at(kx,ky,true);for(int i=1;i<=24;i++){touch_at(kx+i*16,ky,true);frame(16);}
        assert(po.x>=PO_TRAVEL*9/10);
        lv_obj_send_event(po.knob,LV_EVENT_PRESS_LOST,NULL);touch_at(kx+384,ky,false);for(int i=0;i<40;i++)frame(16);
        assert(po_is_open() && po.x==0 && !po.off && poweroffs==offs && po.dark==0);
        lv_obj_send_event(po.cancel,LV_EVENT_CLICKED,NULL);frame(400);assert(!po_is_open());
        sl_power_filter_button(BUTTON_POWER);sl_power.power_at-=SL_POWER_HOLD_MS+50;sl_power_tick();frame(300);
        sl_power_filter_button(BUTTON_POWER|BUTTON_REL);
        lv_obj_get_coords(po.knob,&k);kx=(k.x1+k.x2)/2;ky=(k.y1+k.y2)/2;
        touch_at(kx,ky,true);for(int i=1;i<=24;i++){touch_at(kx+i*16,ky,true);frame(16);}
        for(int i=0;i<236/PO_DARK_STEP+PO_STRIPS;i++)frame(16);   /* held at the end: the dark catches up */
        assert(po.dark>200 && lv_obj_has_flag(po.rim,LV_OBJ_FLAG_HIDDEN));shot_now("power-slider-end.ppm");    /* at the end: only the slider lit, no rim */
        {   /* the dark strips: each frame brings one, and they meet */
            int same=po.shown==po.dark;for(int i=0;i<PO_STRIPS;i++)same&=po.strip_dark[i]==po.dark;
            assert(same && lv_obj_get_style_bg_opa(po.strip[PO_STRIPS-1],0)==po_dim_opa(po.dark));}
        touch_at(kx+384,ky,false);
        sl_power_filter_button(BUTTON_POWER);sl_power_filter_button(BUTTON_POWER|BUTTON_REL);   /* a press in the last glide changes nothing */
        for(int i=0;i<30;i++)frame(16);
        /* not from inside LVGL's timers (the goodbye could not draw there): the main loop's turn */
        assert(poweroffs==offs && po_is_open() && po_power_off);
        lv_obj_send_event(po.knob,LV_EVENT_PRESSED,NULL);assert(!po.dragging);             /* nothing takes it back now */
        sl_power_tick();
        assert(poweroffs==offs+1 && !po_is_open() && bye && !lv_obj_has_flag(bye,LV_OBJ_FLAG_HIDDEN));   /* all the way: off, the goodbye up */
        sl_power_state_init(&sl_power,sl_ms());
        sl_power_filter_button(BUTTON_POWER);sl_power.power_at-=SL_POWER_FORCE_MS+50;sl_power_tick();frame(100);
        assert(poweroffs==offs+2 && !po_is_open());                                         /* held on and on: off, always */
        sl_power_filter_button(BUTTON_POWER|BUTTON_REL);sl_power_state_init(&sl_power,sl_ms());sl_power_apply();
        bye_cancel();   /* the goodbye screen: on the player, nothing is left to see it */
        sl_power_running=was_running;frame(300);
    }
    test_status=0;
    remove(MT_FILE);

    /* tiles as a separate choice: light theme with live tiles */
    global_settings.sloppy_theme=1;global_settings.sloppy_home_style=1;appearance_reload(NULL);frame(600);
    while(npages>1){page_pop();frame(400);}
    frame(800);assert(mt.page==pages[0]);shot("home-tiles-light.ppm");
    {   /* light theme on a photo wallpaper: white status bar and apps on home, the theme's on pages */
        int wall=global_settings.sloppy_home_wallpaper;
        global_settings.sloppy_home_wallpaper=5;appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}
        assert(lv_color_eq(lv_obj_get_style_text_color(sb_clock,0),lv_color_white()) && sb_on_wall);
        system_page();frame(400);
        assert(lv_color_eq(lv_obj_get_style_text_color(sb_clock,0),lv_color_hex(C_TEXT)) && !sb_on_wall);
        page_pop();frame(400);
        assert(lv_color_eq(lv_obj_get_style_text_color(sb_clock,0),lv_color_white()));
        action(ACT_COIN);frame(400);                                       /* the coin keeps its own ink */
        assert(lv_color_eq(lv_obj_get_style_text_color(sb_clock,0),lv_color_hex(COIN_INK)));
        page_pop();for(int i=0;i<5;i++)frame(100);
        assert(npages==1 && lv_color_eq(lv_obj_get_style_text_color(sb_clock,0),lv_color_white()));
        shot("home-tiles-light-wall.ppm");
        global_settings.sloppy_home_wallpaper=wall;appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}
    }
    global_settings.sloppy_home_style=0;
    home_style_page();frame(100);
    lv_obj_t *layout_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(layout_body,1),1),LV_EVENT_CLICKED,NULL);
    assert(appearance_reload_pending && global_settings.sloppy_home_style==1);
    frame(20);assert(appearance_reload_pending);
    frame(50);assert(!appearance_reload_pending && npages==3 && mt.page==pages[0]);
    wallpaper_page(false);frame(100);
    lv_obj_t *nexus_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(nexus_body,1),8),LV_EVENT_CLICKED,NULL);
    frame(50);assert(global_settings.sloppy_home_wallpaper==8 && npages==3);
    global_settings.sloppy_home_wallpaper=0;
    global_settings.sloppy_home_style=0;
    appearance_reload(NULL);frame(100);
    while(npages>1){page_pop();frame(400);}

    /* true dark: Windows Phone start screen and the frosted control centre */
    global_settings.sloppy_accent_color=0x72d8cd;   /* the default accent */
    global_settings.sloppy_theme=2;global_settings.sloppy_home_style=1;appearance_reload(NULL);frame(600);
    while(npages>1){page_pop();frame(400);}
    frame(800);
    assert(mt.page==pages[0] && mt.timer && mt.clock_time && mt.queue_badge);
    assert(!strcmp(lv_label_get_text(mt.clock_time),"9:41"));
    shot("home-metro.ppm");
    lv_obj_t *metro_body=lv_obj_get_child(pages[0],0);
    lv_obj_scroll_to_y(metro_body,560,LV_ANIM_OFF);frame(100);shot("home-metro-apps.ppm");
    assert(lv_obj_get_scroll_y(metro_body)>0);
    lv_obj_scroll_to_y(metro_body,0,LV_ANIM_OFF);frame(100);
    for(int i=0;i<6;i++)frame(1000);       /* a live tile peeks */
    frame(300);shot("home-metro-peek.ppm");
    for(int i=0;i<5;i++)frame(1000);
    {   /* the fast wallpapers draw the very same pixels as the shapes they replace */
        static uint16_t fast[W*H];int worst=0;
        int wps[]={1,8};
        for(int w=0;w<2;w++){
            global_settings.sloppy_home_wallpaper=wps[w];appearance_reload(NULL);frame(700);frame(700);
            while(npages>1){page_pop();frame(400);}
            lv_obj_add_flag(pages[0],LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(pages[0],LV_OBJ_FLAG_HIDDEN);
            for(unsigned c=0;c<lv_obj_get_child_count(pages[0]);c++)lv_obj_add_flag(lv_obj_get_child(pages[0],c),LV_OBJ_FLAG_HIDDEN);
            if(wps[w]==8)assert(nexus_sprites_ok);else assert(wallpaper_caches[0].buf);
            wallpaper_nexus_now=123456;lv_obj_invalidate(lv_screen_active());lv_refr_now(disp);
            memcpy(fast,pixels,sizeof fast);
            if(wps[w]==8)nexus_sprites_ok=false;else wallpaper_caches[0].id=-1,wallpaper_caches[0].pending=true;
            wallpaper_nexus_now=123456;lv_obj_invalidate(lv_screen_active());lv_refr_now(disp);
            for(int i=0;i<W*H;i++){unsigned a=fast[i],b=pixels[i];
                int d=abs((int)(a>>11)-(int)(b>>11));int g=abs((int)((a>>5)&63)-(int)((b>>5)&63))/2;int bl=abs((int)(a&31)-(int)(b&31));
                if(g>d)d=g;if(bl>d)d=bl;if(d>worst)worst=d;}
            printf("wallpaper %d fast path: worst difference %d/31\n",wps[w],worst);
            assert(worst<=1);
            if(wps[w]==8)nexus_sprites_ok=true;else wallpaper_caches[0].pending=false;
            appearance_reload(NULL);frame(700);
        }
        global_settings.sloppy_home_wallpaper=0;appearance_reload(NULL);frame(700);frame(700);
    }
    qp_drag_to(430);frame(50);shot("glass-pull.ppm");
    assert(qp_glass_ok && lv_obj_get_style_bg_opa(qp,0)==LV_OPA_TRANSP);
    /* below the panel's edge the screen stays sharp, even after the finger
     * has been further down and come back up */
    qp_drag_to(650);frame(50);qp_drag_to(430);frame(50);
    for(int y=440;y<H;y+=7)for(int x=0;x<W;x+=11)assert(pixels[y*W+x]==qp_sharp_px[y*W+x]);
    qp_release(430,900);frame(400);shot("glass-true-dark.ppm");
    qp_close();frame(400);
    {   /* the list home: big lowercase words, the same places, apps below, now playing over them */
        int st0=test_status;char path0[MAX_PATH];strlcpy(path0,test_track.path,sizeof path0);
        test_status=0;global_settings.sloppy_theme=0;global_settings.sloppy_home_style=2;appearance_reload(NULL);frame(600);
        while(npages>1){page_pop();frame(400);}
        for(int i=0;i<60;i++)frame(16);
        assert(zn.page==pages[0] && mt.page==NULL && ptr.page==pages[0] && lv_obj_has_flag(zn.np,LV_OBJ_FLAG_HIDDEN));
        const char *words[]={"music","playlists","up next","playroom","bluetooth","settings","apps","tamagotchi","chess"};
        for(unsigned i=0;i<sizeof words/sizeof words[0];i++)assert(has_text(pages[0],words[i]));
        shot_now("home-list.ppm");
        /* each word goes where the cards home's tile goes */
        static const struct { const char *word, *title; } go[]={{"music","Music"},{"playroom","Playroom"},{"settings","Settings"}};
        for(unsigned i=0;i<sizeof go/sizeof go[0];i++){
            lv_obj_t *hit=NULL;lv_obj_t *body=lv_obj_get_child(pages[0],4);
            for(uint32_t c=0;c<lv_obj_get_child_count(body)&&!hit;c++){lv_obj_t *r=lv_obj_get_child(body,c);
                if(lv_obj_get_child_count(r)&&lv_obj_check_type(lv_obj_get_child(r,0),&lv_label_class)&&!strcmp(lv_label_get_text(lv_obj_get_child(r,0)),go[i].word))hit=r;}
            assert(hit);lv_area_t a;lv_obj_get_coords(hit,&a);
            touch_at(a.x1+60,(a.y1+a.y2)/2,true);frame(30);
            assert(lv_obj_get_style_translate_x(hit,0)>0 || lv_obj_has_state(hit,LV_STATE_PRESSED));   /* pressed: it lights and nudges */
            touch_at(a.x1+60,(a.y1+a.y2)/2,false);frame(500);
            assert(npages==2 && has_text(pages[1],go[i].title));
            page_pop();frame(500);
        }
        /* playing: the line over the words, and a tap on it opens Now Playing */
        test_status=AUDIO_STATUS_PLAY;sl_track_path(3,test_track.path,sizeof test_track.path);
        frame(1100);assert(!lv_obj_has_flag(zn.np,LV_OBJ_FLAG_HIDDEN) && strlen(lv_label_get_text(zn.np_title))>0);
        shot_now("home-list-playing.ppm");
        lv_obj_send_event(zn.np,LV_EVENT_CLICKED,NULL);frame(500);assert(np_open);np_hide(0);frame(500);
        test_status=0;frame(1100);assert(lv_obj_has_flag(zn.np,LV_OBJ_FLAG_HIDDEN));
        /* large text: the app lines grow, nothing runs off the screen */
        sl_text_step=1;appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}for(int i=0;i<60;i++)frame(16);
        lv_obj_scroll_to_y(lv_obj_get_child(pages[0],4),700,LV_ANIM_OFF);frame(100);shot_now("home-list-large.ppm");
        sl_text_step=0;
        /* Hebrew: the big type has no Hebrew, so the words take the face that has */
        global_settings.sloppy_language=4;appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}for(int i=0;i<60;i++)frame(16);
        {   lv_obj_t *body=lv_obj_get_child(pages[0],4),*r=lv_obj_get_child(body,1);   /* the first place */
            assert(lv_obj_get_style_text_font(lv_obj_get_child(r,0),0)==&ss_semibold_30);}
        shot_now("home-list-hebrew.ppm");
        global_settings.sloppy_language=0;
        test_status=st0;strlcpy(test_track.path,path0,sizeof test_track.path);
    }
    global_settings.sloppy_theme=0;global_settings.sloppy_home_style=0;appearance_reload(NULL);frame(600);
    while(npages>1){page_pop();frame(400);}
    assert(mt.page==NULL && mt.timer==NULL && zn.page==NULL);
    qp_drag_to(H);qp_release(H,900);frame(400);shot("glass-dark.ppm");qp_close();frame(400);
    {   /* the glass is blurred at touch-down on the top edge, while the finger
         * travels, and the pull reuses it; a big redraw in between does not */
        for(int big=0;big<2;big++){
            frame(500);qp_glass_ok=false;
            touch_at(240,8,true);
            assert(qp_glass_ok && lv_obj_has_flag(qp,LV_OBJ_FLAG_HIDDEN));   /* taken before any pull */
            uint8_t mark=(uint8_t)(qg_ch[0][0]^0x55);qg_ch[0][0]=mark;        /* this capture's glass */
            if(big){lv_obj_invalidate(lv_screen_active());lv_refr_now(disp);}   /* the whole screen drawn again */
            touch_at(240,40,true);touch_at(240,120,true);touch_at(240,200,true);
            assert(!lv_obj_has_flag(qp,LV_OBJ_FLAG_HIDDEN) && qp_glass_ok);
            assert(big ? qg_ch[0][0]!=mark : qg_ch[0][0]==mark);
            touch_at(240,200,false);frame(400);qp_close();frame(400);
            assert(lv_obj_has_flag(qp,LV_OBJ_FLAG_HIDDEN));
        }
    }

    /* Reset has a cancel path, restores Rockbox and GabagoolOS defaults, and
     * clears only the separately saved tile layout. */
    global_settings.sloppy_theme=2;global_settings.sloppy_home_style=1;
    global_settings.sloppy_accent_color=0xe28435;
    appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(400);}
    mt_pin(TL_COIN,SZ_MED);assert(fake_find(MT_FILE)>=0);
    int pet_file_before=fake_find(PET_FILE), chess_file_before=fake_find(CHESS_FILE);
    advanced_page();frame(400);   /* Reset lives under Settings > Advanced */
    lv_obj_t *settings_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_t *reset_row=row_named(settings_body,"Reset all settings");assert(reset_row);
    lv_obj_send_event(reset_row,LV_EVENT_CLICKED,NULL);frame(400);assert(npages==3);
    lv_obj_t *reset_confirm_body=lv_obj_get_child(pages[npages-1],1);
    shot("settings-reset-confirm.ppm");
    lv_obj_t *keep_row=row_named(reset_confirm_body,"Keep my settings");assert(keep_row);
    int resets_before=settings_reset_calls;
    lv_obj_send_event(keep_row,LV_EVENT_CLICKED,NULL);frame(400);
    assert(npages==2 && settings_reset_calls==resets_before && fake_find(MT_FILE)>=0);
    lv_obj_send_event(reset_row,LV_EVENT_CLICKED,NULL);frame(400);
    reset_confirm_body=lv_obj_get_child(pages[npages-1],1);
    lv_obj_t *confirm_row=row_named(reset_confirm_body,"Reset all settings");assert(confirm_row);
    int saves_before=settings_save_calls;
    lv_obj_send_event(confirm_row,LV_EVENT_CLICKED,NULL);frame(600);
    assert(settings_reset_calls==resets_before+1 && settings_save_calls==saves_before+1);
    assert(settings_apply_calls==1 && settings_skins_calls==1);
    assert(global_settings.sloppy_theme==0 && global_settings.sloppy_home_style==0);
    assert(global_settings.sloppy_accent_color==0x72d8cd && fake_find(MT_FILE)<0);
    assert(npages==2 && mt.page==NULL);
    if(pet_file_before>=0) assert(fake_find(PET_FILE)>=0);
    if(chess_file_before>=0) assert(fake_find(CHESS_FILE)>=0);
    shot("settings-after-reset.ppm");
    {   /* FM_SAVE=path: show a real save's Upgrades tab (read-only copy) */
        const char *path=getenv("FM_SAVE");FILE *f=path?fopen(path,"rb"):NULL;
        if(f){struct sl_fm_state saved;size_t got=fread(&saved,1,sizeof saved,f);fclose(f);
            assert(got==sizeof saved && sl_fm_valid(&saved));
            radio_state=saved;radio_loaded=true;sl_fm_recalc(&radio_state,&radio_rates);
            radio_page();frame(300);lv_obj_send_event(radio_tabs[RT_UPGRADES],LV_EVENT_CLICKED,NULL);frame(800);
            shot("save-upgrades-1.ppm");
            lv_obj_t *body=lv_obj_get_parent(radio_list);
            lv_obj_scroll_by(body,0,-600,LV_ANIM_OFF);frame(100);shot("save-upgrades-2.ppm");
            lv_obj_scroll_by(body,0,-600,LV_ANIM_OFF);frame(100);shot("save-upgrades-3.ppm");
            int rows=0;for(uint32_t i=1;i<lv_obj_get_child_count(radio_list);i++){lv_obj_t *c=lv_obj_get_child(radio_list,i);
                if(lv_obj_get_child_count(c)==4&&lv_obj_check_type(lv_obj_get_child(c,1),&lv_label_class))rows++;}
            printf("save: %d upgrades owned, %d owned rows shown\n",fm_popcount(radio_state.upgrades,FM_UPG_WORDS),rows);
            int owned=fm_popcount(radio_state.upgrades,FM_UPG_WORDS);
            if(owned<=RADIO_OPEN_ALL) assert(rows==owned);
            else {  /* folded: open the first group, its rows appear */
                assert(radio_open_group==-1);
                lv_obj_t *head=lv_obj_get_child(radio_list,lv_obj_get_index(radio_owned_head)+1);
                lv_obj_send_event(head,LV_EVENT_CLICKED,NULL);frame(300);
                assert(radio_open_group>=0);
                lv_obj_update_layout(radio_list);
                lv_obj_scroll_to_y(body,lv_obj_get_y(radio_owned_head)+560,LV_ANIM_OFF);frame(100);shot("save-upgrades-open.ppm");
            }
            radio_dirty=false;page_pop();frame(500);}
    }
    {   /* listening history, smart playlists, Wrapped and search on a small library */
        while(npages>1){page_pop();frame(400);}
        struct sl_lib saved=sl_lib;memset(&sl_lib,0,sizeof sl_lib);
        static struct sl_track tr[12];static struct sl_album al[4];
        static int by_title[4]={0,1,2,3},by_artist[4]={2,0,1,3},songs[12],alt[4][12];
        static const char *titles[12]={"Blue Hours","Night Tide","After the Rain","Neon Sky","Runaway","Small Hours",
            "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d","Glass Avenue","High Voltage","Silent City","Caf\xc3\xa9 Noir","Night Drive"};
        static const char *atitle[4]={"Blue Hours","Neon","Rain Days","\xd7\x90\xd7\x9c\xd7\x91\xd7\x95\xd7\x9d"};
        static const char *aartist[4]={"Mira Sol","Mira Sol","Ada Lane","\xd7\xa9\xd7\x95\xd7\x9c\xd7\x94 \xd7\x97\xd7\x9f"};
        for(int a=0;a<4;a++){al[a].title=atitle[a];al[a].artist=aartist[a];al[a].hue=a*80;al[a].year=2020+a;al[a].tracks=alt[a];al[a].n_tracks=0;}
        for(int i=0;i<12;i++){char path[40];snprintf(path,sizeof path,"/Music/h-%d.mp3",i);
            tpath(&tr[i],strdup(path));tr[i].title=titles[i];tr[i].album=i%4;tr[i].num=i/4+1;tr[i].len=200000;tr[i].added=1000+i;
            al[i%4].tracks[al[i%4].n_tracks++]=i;songs[i]=i;}
        for(int i=0;i<12;i++)for(int j=i+1;j<12;j++)if(strcasecmp(titles[songs[j]],titles[songs[i]])<0){int s=songs[i];songs[i]=songs[j];songs[j]=s;}
        sl_lib.tracks=tr;sl_lib.n_tracks=12;sl_lib.albums=al;sl_lib.n_albums=4;sl_lib.n_artists=3;
        sl_lib.albums_by_title=by_title;sl_lib.albums_by_artist=by_artist;sl_lib.songs_by_title=songs;
        /* a log: song 3 five times, song 5 twice, the Hebrew song once late at night (all
         * September 2026), song 0 three times in January 2025, and a deleted song */
        uint32_t sep=1789000000u,jan=1736000000u,magic=SL_HIST_MAGIC;
        struct sl_play log[12];int nl=0;
        for(int i=0;i<5;i++)log[nl++]=(struct sl_play){sep+i*3600,sl_hist_hash(TPT(&tr[3])),200000};
        for(int i=0;i<2;i++)log[nl++]=(struct sl_play){sep+86400+i*600,sl_hist_hash(TPT(&tr[5])),200000};
        log[nl++]=(struct sl_play){sep-sep%86400+23*3600+86400*3,sl_hist_hash(TPT(&tr[6])),200000};
        for(int i=0;i<3;i++)log[nl++]=(struct sl_play){jan+i*60,sl_hist_hash(TPT(&tr[0])),180000};
        log[nl++]=(struct sl_play){sep+100,sl_hist_hash("/Music/gone.mp3"),90000};
        int fd=open(HIST_FILE,O_WRONLY|O_CREAT|O_TRUNC,0666);assert(fd>=0);
        assert(write(fd,&magic,4)==4 && write(fd,log,nl*sizeof log[0])==(ssize_t)(nl*sizeof log[0]));sim_close(fd);
        struct hist_view *hv=hist_view_load();assert(hv && hv->n==nl);
        assert(hv->count[3]==5 && hv->count[0]==3 && hv->count[6]==1 && hv->track[nl-1]==-1);
        uint32_t now=hist_clock();int *list,n;
        n=smart_build(hv,SMART_MOST,now,&list,NULL);assert(n==4 && list[0]==3 && list[1]==0 && list[2]==5);free(list);
        n=smart_build(hv,SMART_RECENT,now,&list,NULL);assert(n==4 && list[0]==6 && list[3]==0);free(list);
        n=smart_build(hv,SMART_UNPLAYED,now,&list,NULL);assert(n==8 && list[0]==11);free(list);
        n=smart_build(hv,SMART_ADDED,now,&list,NULL);assert(n==12 && list[0]==11);free(list);
        n=smart_build(hv,SMART_FORGOTTEN,now,&list,NULL);assert(n==1 && list[0]==0);free(list);
        struct wrapped w;assert(wrapped_compute(hv,2026,&w));
        assert(w.plays==9 && w.songs==3 && w.artists==3 && w.n_years==2 && w.years[0]==2026);
        assert(w.top_artist[0]==3 && w.top_song[0]==3 && w.top_song_plays[0]==5 && w.month_ms[8]==8*200000+90000);
        assert(w.hour_plays[23]==1);
        assert(wrapped_compute(hv,2025,&w) && w.plays==3 && w.top_artist[0]==1);
        hist_view_free(hv);
        foryou_page();frame(600);shot("for-you.ppm");
        assert(find_label(pages[npages-1],"Forgotten favorites"));
        assert(find_label(pages[npages-1],"Loved") && find_label(pages[npages-1],"Your taste"));
        {   /* Your taste: songs dealt as cards, each a snippet; right likes, left passes */
            int st0=test_status,q0=queued,s0=started;
            test_status=0;queued=1;started=0;queue_tracks[0]=0;
            taste_page(false);frame(400);
            assert(td.page && td.n>=3 && td_slot_of(0) && quiz_clip_active());
            int t0=td.deck[0],t1=td.deck[1];
            assert(!strcmp(quiz.clip.path,TP(t0)));
            shot("taste-card.ppm");
            lv_area_t a;lv_obj_get_coords(td_slot_of(td.at)->img,&a);int cx=(a.x1+a.x2)/2,cy=(a.y1+a.y2)/2;
            touch_at(cx,cy,true);for(int x=cx;x<=cx+90;x+=15)touch_at(x,cy,true);
            assert(lv_obj_get_style_image_opa(td.stamp[1],0)>150);shot_now("taste-swipe.ppm");
            for(int x=cx+90;x<=cx+180;x+=15)touch_at(x,cy,true);touch_at(cx+180,cy,false);frame(400);
            assert(td.at==1 && ts_rating(t0)==1 && !strcmp(quiz.clip.path,TP(t1)));
            assert(queued==2);   /* one snippet in the queue at a time */
            td_throw(false,0,0);frame(400);assert(td.at==2 && ts_rating(t1)==-1);
            /* a short drag springs back and rates nothing */
            lv_obj_get_coords(td_slot_of(td.at)->img,&a);cx=(a.x1+a.x2)/2;
            touch_at(cx,cy,true);touch_at(cx+40,cy,true);touch_at(cx+40,cy,false);frame(400);frame(400);
            assert(td.at==2 && lv_obj_get_style_translate_x(td_slot_of(td.at)->img,0)==0);
            int *l,n=ts_loved(&l);bool in=false;for(int i=0;i<n;i++)in|=l[i]==t0;free(l);assert(in);
            page_pop();frame(500);
            assert(!quiz_clip_active() && queued==1 && ts.asked && !td.page);   /* your song back */
            {   /* Back, then Your taste again before the last deck has slid away: that page's delete freed
                 * the new deck's pictures, and its next frame drew freed memory (a crash on the R1; ASan) */
                taste_page(false);frame(400);
                lv_obj_t *old=td.page;page_pop();frame(30);assert(lv_obj_is_valid(old));   /* still sliding */
                taste_page(false);assert(td.page && pages[npages-1]==td.page);
                tsw_settle();assert(td.page && td.timer && td_slot_of(td.at) && td_slot_of(td.at)->buf);
                page_pop();frame(500);assert(!td.page && !quiz_clip_active());
            }
            taste_learn_tests();
            /* don't play this artist: out of Loved and Radio until allowed again */
            ts_hide_artist(sl_lib.albums[sl_lib.tracks[t0].album].artist);
            assert(ts_blocked_track(t0));
            n=ts_loved(&l);in=false;for(int i=0;i<n;i++)in|=l[i]==t0;free(l);assert(!in);
            hidden_page();frame(400);assert(find_label(pages[npages-1],"Allow"));shot("hidden-artists.ppm");
            ts_show_artist(0);hidden_fill();frame(200);assert(!ts_blocked_track(t0) && !find_label(pages[npages-1],"Allow"));
            page_pop();frame(400);
            loved_page();frame(500);shot("loved.ppm");page_pop();frame(400);
            test_status=st0;queued=q0;started=s0;
        }
        /* ---- taste-swipe: the Your taste deck as a stack that follows the finger ---- */
        {   int st0=test_status,q0=queued,s0=started;
            test_status=0;queued=1;started=0;queue_tracks[0]=0;
            struct sl_lib keep=sl_lib;
            static struct sl_track ttr[40];static struct sl_album tal[40];static char tart[40][16],tti[40][16],tpa[40][24];
            for(int i=0;i<40;i++){snprintf(tart[i],16,"Artist %d",i);snprintf(tti[i],16,"Song %d",i);snprintf(tpa[i],24,"/Music/t-%d.mp3",i);
                tal[i]=(struct sl_album){0};tal[i].title=tti[i];tal[i].artist=tart[i];tal[i].hue=i*37%360;
                ttr[i]=(struct sl_track){0};tpath(&ttr[i],tpa[i]);ttr[i].title=tti[i];ttr[i].album=i;ttr[i].len=200000;}
            sl_lib.tracks=ttr;sl_lib.n_tracks=40;sl_lib.albums=tal;sl_lib.n_albums=40;
            int rated0=ts.n_rated;ts.n_rated=0;ts.gen++;
            taste_page(false);frame(400);tsw_settle();
            assert(td.n==TS_DECK && td.at==0);
            /* the stack: two cards under the top one, smaller and dimmer, the top one a plain copy */
            struct td_slot *s0p=td_slot_of(0),*s1p=td_slot_of(1),*s2p=td_slot_of(2);
            assert(s0p&&s1p&&s2p&&s0p->buf&&s1p->buf&&s2p->buf);
            assert(lv_image_get_scale(s0p->img)==256&&lv_image_get_rotation(s0p->img)==0);
            /* the card under: copies at each step of its size, made while the stack sat still, drawn as they are */
            assert(td.step_card==1&&td.step_ok==(1u<<TD_STEPS)-1&&lv_image_get_src(s1p->img)==td.step[0]&&lv_image_get_scale(s1p->img)==256);
            assert(lv_obj_get_style_image_recolor_opa(s1p->img,0)==80);
            assert(lv_obj_has_flag(s2p->img,LV_OBJ_FLAG_HIDDEN)&&!lv_obj_has_flag(td.ghost,LV_OBJ_FLAG_HIDDEN));   /* the bottom one: a plain shape */
            assert(lv_obj_has_flag(s0p->img,LV_OBJ_FLAG_CLICKABLE)&&!lv_obj_has_flag(s1p->img,LV_OBJ_FLAG_CLICKABLE));
            assert(lv_obj_get_style_opa(td.btn[2],0)<LV_OPA_COVER);   /* nothing to undo yet */
            shot("taste-stack.ppm");
            /* a slow drag, short of the line: follows 1:1, turns, lifts, then springs back past the middle once */
            tsw_drag(70,20,14,200,false);
            assert(td.x==70&&td.y==20&&lv_obj_get_style_translate_y(s0p->img,0)==td.turn_oy+20);   /* 1:1 with the finger */
            assert(lv_image_get_src(s0p->img)==td.turn&&td.turn_rot==35&&td.turn_scale==256+TD_LIFT);   /* turned 3.5 degrees, lifted */
            assert(lv_image_get_src(s1p->img)==td.step[1]);   /* the next one coming up, nothing scaled during the drag */
            tsw_up();
            int lo=0;bool shotted=false;
            for(int i=0;i<40;i++){frame(16);if(td.x<lo)lo=td.x;if(!shotted&&td.x<20){shot_now("taste-spring.ppm");shotted=true;}}
            assert(lo<0 && lo>-12);   /* a slight overshoot */
            assert(td.at==0&&td.x==0&&td.y==0&&td.lift==0&&lv_image_get_src(s0p->img)==s0p->buf&&lv_image_get_rotation(s0p->img)==0&&lv_image_get_scale(s0p->img)==256);
            assert(lv_image_get_src(s1p->img)==td.step[0]&&lv_obj_has_flag(td.stamp[0],LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(td.stamp[1],LV_OBJ_FLAG_HIDDEN));
            /* past the line to the left: PASS stamps down, the pass button fills; a flick back to the middle keeps it */
            tsw_drag(-150,10,15,200,false);
            assert(!lv_obj_has_flag(td.stamp[0],LV_OBJ_FLAG_HIDDEN)&&lv_obj_get_style_image_opa(td.stamp[0],0)==255);
            assert(lv_obj_has_flag(td.stamp[1],LV_OBJ_FLAG_HIDDEN)&&lv_image_get_src(td.stamp[0])==td.stamp_turn&&(td.stamp_key&1023)==256+TD_LIFT);   /* turned and sized by us, once a step */
            assert(td.bfill[0]>200&&td.bfill[1]==0&&td.bsc[0]>256&&td.bsc[1]<256);   /* drawn filled and grown; nothing scaled through a layer */
            assert(lv_image_get_src(s1p->img)==td.step[TD_STEPS-1]);
            shot_now("taste-drag-left.ppm");
            for(int i=1;i<=3;i++){t_x+=30;frame(16);}tsw_up();tsw_settle();
            assert(td.at==0&&td.x==0&&ts.n_rated==0);
            /* past the line to the right, held still, let go: liked */
            assert(td.dealt==TD_STACK);   /* each card is picked as it comes up, knowing the swipes before it */
            int c0=td.deck[0],c1=td.deck[1],c2=td.deck[2],c3=-1,c4=-1;
            tsw_drag(160,-10,16,200,false);
            assert(!lv_obj_has_flag(td.stamp[1],LV_OBJ_FLAG_HIDDEN)&&lv_obj_get_style_image_opa(td.stamp[1],0)==255);
            assert(td.bfill[1]>200);
            shot_now("taste-drag-right.ppm");
            tsw_up();assert(td.flying);tsw_settle();
            assert(td.at==1&&ts_rating(c0)==1&&!strcmp(quiz.clip.path,TPT(&ttr[c1]))&&td_slot_of(1)&&td_slot_of(2));
            assert(td_slot_of(3));   /* the card under them, pictured once still */
            c3=td.deck[3];
            assert(lv_obj_get_style_opa(td.btn[2],0)==LV_OPA_COVER);
            /* flicks: 50 px quick to the left passes, to the right likes; a fling is caught in the air */
            tsw_drag(-50,-8,2,0,true);assert(td.flying);
            frame(16);frame(16);frame(16);shot_now("taste-fling.ppm");
            assert(lv_image_get_src(td_slot_of(1)->img)==td.turn&&!td.turn_fine&&td.turn_rot<0&&td.turn_rot%15==0&&lv_image_get_rotation(td_slot_of(1)->img)==0);   /* flying: the quick copy in coarser steps, nothing turned live */
            tsw_settle();assert(td.at==2&&ts_rating(c1)==-1);c4=td.deck[4];
            tsw_drag(50,0,2,0,true);assert(td.flying);tsw_settle();assert(td.at==3&&ts_rating(c2)==1);
            /* the same short drag, slowly: not a flick */
            tsw_drag(50,0,20,0,true);tsw_settle();assert(td.at==3&&ts_rating(c3)==0);
            /* the buttons throw it the same way, with a press bounce */
            key_tap(td.btn[0]);assert(td.flying);tsw_settle();assert(td.at==4&&ts_rating(c3)==-1&&td.bscale[0]==256);
            key_tap(td.btn[1]);tsw_settle();assert(td.at==5&&ts_rating(c4)==1);
            assert(td.bsc[1]==256&&td.liked==3&&lv_obj_get_style_transform_scale_x(td.btn[1],0)==256);
            /* undo: the last card flies back in from the right, its like taken back */
            int n0=ts.n_rated;
            key_tap(td.btn[2]);assert(td.at==4&&ts_rating(c4)==0&&ts.n_rated==n0-1&&td.liked==2&&td.x>200);
            frame(16);frame(16);frame(16);frame(16);shot_now("taste-undo.ppm");
            tsw_settle();assert(td.x==0&&!strcmp(quiz.clip.path,TPT(&ttr[c4]))&&td_slot_of(5)&&td_slot_of(6));
            key_tap(td.btn[2]);tsw_settle();assert(td.at==3&&ts_rating(c3)==0);
            /* up: LOVE stamps down low on the card and the love button fills; let go past the line: loved */
            {   int cu=td.deck[td.at],at0=td.at,liked0=td.liked,f0=test_fades;
                quiz.clip.audible=true;quiz.clip.hushed=false;
                tsw_drag(6,-150,15,200,false);
                assert(!lv_obj_has_flag(td.stamp[2],LV_OBJ_FLAG_HIDDEN)&&lv_obj_get_style_image_opa(td.stamp[2],0)==255);
                assert(lv_obj_has_flag(td.stamp[0],LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(td.stamp[1],LV_OBJ_FLAG_HIDDEN));
                assert(td.bfill[3]>200&&td.bfill[1]==0);
                shot_now("taste-drag-up.ppm");
                tsw_up();assert(td.flying&&td.ty<-(TD_TOP+TD_CARD_H));
                frame(16);frame(16);frame(16);shot_now("taste-love.ppm");
                tsw_settle();
                assert(td.at==at0+1&&ts_rating(cu)==1&&ts.rated[ts.n_rated-1][0]=='*'&&td.liked==liked0+1);
                assert(test_fades==f0+1);   /* the snippet faded out as its card went */
                /* undo brings it back down from the top */
                key_tap(td.btn[2]);assert(td.at==at0&&td.y<-200&&ts_rating(cu)==0);tsw_settle();assert(td.y==0&&td.x==0);
                /* a quick flick up loves it from short of the line; so does the button */
                tsw_drag(0,-40,2,0,true);assert(td.flying&&td_threw=='*');tsw_settle();assert(ts_rating(cu)==1);
                key_tap(td.btn[2]);tsw_settle();
                key_tap(td.btn[3]);assert(td.flying&&td_threw=='*');tsw_settle();assert(ts_rating(cu)==1&&td.at==at0+1);
                key_tap(td.btn[2]);tsw_settle();assert(td.at==at0&&ts_rating(cu)==0);
            }
            /* held low on the card it turns the other way; held, it is lifted over a shadow */
            {   lv_area_t a;lv_obj_get_coords(td_slot_of(td.at)->img,&a);
                t_x=(a.x1+a.x2)/2;t_y=a.y1+TD_CARD_H*5/6;t_down=true;frame(16);
                for(int i=1;i<=14;i++){t_x+=5;frame(16);}
                frame(16);frame(16);   /* a turn is made over two frames, the last one showing meanwhile */
                assert(td.x==70&&td.turn_rot==-35&&!td.turn_fine);   /* moving: the quick copy */
                for(int i=0;i<45;i++)frame(16);
                assert(td.turn_fine&&td.turn_rot==-35);              /* held still: made fine, a band a tick */
                if(!sl_zune())   /* lifted over its shadow, which follows it */
                    for(int j=0;j<3;j++)assert(!lv_obj_has_flag(td.shadow[j],LV_OBJ_FLAG_HIDDEN)&&lv_obj_get_style_image_opa(td.shadow[j],0)==255);
                if(!sl_zune())assert(lv_obj_get_style_translate_x(td.shadow[0],0)==(W-TD_CARD_W)/2+70-TD_SH_BLUR);
                shot_now("taste-lift.ppm");
                t_x+=4;frame(16);assert(td.turn_fine&&td.turn_rot==-35);   /* within the same half degree: still the fine copy */
                t_x+=8;frame(16);assert(td.back_row>0&&td.turn_rot==-35);  /* the next step: half made behind the one showing, */
                frame(16);assert(!td.turn_fine&&td.turn_rot==-40&&td.back_row<0);   /* and shown a frame later, the quick kind */
                t_down=false;frame(16);tsw_settle();assert(td.x==0&&td.lift==0&&(!td.shadow[0]||lv_obj_has_flag(td.shadow[0],LV_OBJ_FLAG_HIDDEN)));
            }
            /* every card says why it was dealt; the page takes its cover's colour */
            {   lv_obj_t *why=lv_obj_get_child(td_slot_of(td.at)->src,-1);
                assert(lv_obj_check_type(why,&lv_label_class)&&lv_label_get_text(why)[0]);
                if(!sl_zune())assert(lv_color_eq(td.wash_cur,td_slot_of(td.at)->wash)&&td.wash_img);}
            /* through many decks: the objects and the heap stay put */
            int objs0=0,deals=0;malloc_statistics_t m0,m1;   /* counted as each fresh deck sits dealt */
            for(int r=0;r<70;r++){
                if(td.at>=td.n){ts.n_rated=0;td_more_cb(NULL);tsw_settle();
                    if(!deals++){objs0=tsw_objs();malloc_zone_statistics(NULL,&m0);}else assert(tsw_objs()==objs0);}
                if(r&1)tsw_drag(r%4==1?-60:60,0,2,0,true);else key_tap(td.btn[r%4?1:0]);
                tsw_settle();
            }
            assert(deals>=3);
            while(td.at<td.n){td_throw(true,0,0);tsw_settle();}   /* the deck out: the end */
            assert(!lv_obj_has_flag(td.hint,LV_OBJ_FLAG_HIDDEN)&&!td_slot_of(td.at));
            key_tap(td.btn[2]);tsw_settle();assert(td.at==td.n-1&&lv_obj_has_flag(td.hint,LV_OBJ_FLAG_HIDDEN)&&td_slot_of(td.at));
            ts.n_rated=0;td_throw(false,0,0);tsw_settle();td_more_cb(NULL);tsw_settle();
            assert(tsw_objs()==objs0);
            malloc_zone_statistics(NULL,&m1);
            printf("taste swipe: %d decks, %d objects dealt, heap %+ld KiB\n",deals,objs0,((long)m1.size_in_use-(long)m0.size_in_use)/1024);
#if !__has_feature(address_sanitizer)   /* its quarantine keeps what was freed "in use"; it checks for leaks its own way */
            assert((long)m1.size_in_use-(long)m0.size_in_use<64*1024);
#endif
            page_pop();frame(500);
            assert(!td.page&&!quiz_clip_active());
            ts.n_rated=rated0;ts.gen++;sl_lib=keep;
            test_status=st0;queued=q0;started=s0;
        }
        /* ---- end taste-swipe ---- */
        /* ---- taste-model: a made-up listener teaches the deck ---- */
        {   /* 1200 songs: artist A (0) has 120 of them, in Rock; 27 more artists have 40 each; six of
             * those play Jazz (G), the rest Pop, Folk or Metal. The listener passes on every song
             * by A, likes every Jazz song, and likes a quarter of the rest. */
            struct sl_lib keep=sl_lib;
            static struct sl_track mt[1200];static struct sl_album ma[30];static int mtr[30][40];
            static char mpa[1200][24],mti[1200][12],mar[28][12],mal[30][12];
            static const char *const gn[5]={"Rock","Jazz","Pop","Folk","Metal"};
            #define TMS_ARTIST(a) ((a)<3?0:(a)-2)
            #define TMS_GENRE(k) ((k)==0?0:(k)<=6?1:2+(k)%3)
            for(int k=0;k<28;k++)snprintf(mar[k],12,"Band %c%c",'A'+k%26,k>=26?'2':' ');
            for(int a=0;a<30;a++){int k=TMS_ARTIST(a);snprintf(mal[a],12,"LP %d",a);
                ma[a]=(struct sl_album){0};ma[a].title=mal[a];ma[a].artist=mar[k];ma[a].genre=gn[TMS_GENRE(k)];
                ma[a].year=1960+k*2;ma[a].hue=a*37%360;ma[a].n_tracks=40;ma[a].tracks=mtr[a];
                for(int j=0;j<40;j++){int t=a*40+j;mtr[a][j]=t;snprintf(mpa[t],24,"/Music/m-%d.flac",t);snprintf(mti[t],12,"Tune %d",t);
                    mt[t]=(struct sl_track){0};tpath(&mt[t],mpa[t]);mt[t].title=mti[t];mt[t].album=a;mt[t].len=200000;mt[t].num=j+1;}}
            sl_lib.tracks=mt;sl_lib.n_tracks=1200;sl_lib.albums=ma;sl_lib.n_albums=30;
            ts_load();int rated0=ts.n_rated;char (*saved)[MAX_PATH]=malloc((rated0?rated0:1)*MAX_PATH);memcpy(saved,ts.rated,(size_t)rated0*MAX_PATH);
            ts.n_rated=0;ts.gen++;
            /* one pass dents an artist, three sink it; one like and one pass cancel */
            {   assert(tm_ready());uint32_t k0=tm.al[0][TM_ARTIST];
                assert(fabs(tm_guess(k0,NULL)-0.5)<1e-6);
                ts_swipe(0,'-');assert(tm_fresh()&&fabs(tm_guess(k0,NULL)-1.0/3)<0.01);
                ts_swipe(1,'-');ts_swipe(2,'-');double g3=tm_guess(k0,NULL);assert(g3>0.18&&g3<0.21);
                assert(ts_passed_album(0)&&!ts_liked_album(5));
                ts_unswipe(2);ts_unswipe(1);ts_unswipe(0);assert(ts.n_rated==0&&fabs(tm_guess(k0,NULL)-0.5)<0.01);
                ts_swipe(0,'-');ts_swipe(1,'+');assert(fabs(tm_guess(k0,NULL)-0.5)<0.01&&!ts_passed_album(0)&&!ts_liked_album(0));
                ts.n_rated=0;ts.gen++;}
            memset(&td,0,sizeof td);td.rng=20261023u;
            enum{TMS_N=200};int nA[3]={0},nG[3]={0},nNew[3]={0},nWhy[3]={0},cnt[3]={0};unsigned u=7;
            for(int s=0;s<TMS_N;s++){
                if(td.at>=td.n){td.n=TS_DECK;td.at=td.dealt=0;td.kept=0;}
                td_deal_to(td.at+TD_STACK);assert(td.at<td.n);
                int t=td.deck[td.at],k=TMS_ARTIST(mt[t].album),g=TMS_GENRE(k);
                int ph=s<TS_DECK?0:s>=TMS_N-80?2:1;   /* the first deck, the last 80 cards */
                cnt[ph]++;nA[ph]+=k==0;nG[ph]+=g==1;nNew[ph]+=td.why[td.at]==TM_WHY_NEW;
                nWhy[ph]+=td.why[td.at]==TM_WHY_GENRE||td.why[td.at]==TM_WHY_ARTIST||td.why[td.at]==TM_WHY_ALBUM;
                u=u*1103515245u+12345u;
                char way=k==0?'-':g==1?'+':(u>>16)%4==0?'+':'-';
                assert(ts_swipe(t,way));td.at++;
            }
            printf("taste model: %d swipes by a listener who passes on artist A and likes genre G\n",TMS_N);
            printf("taste model:   library share  A %4.1f%%  G %4.1f%%\n",100.0*120/1200,100.0*240/1200);
            printf("taste model:   first %d cards A %4.1f%%  G %4.1f%%  new %4.1f%%\n",cnt[0],100.0*nA[0]/cnt[0],100.0*nG[0]/cnt[0],100.0*nNew[0]/cnt[0]);
            printf("taste model:   last %d cards  A %4.1f%%  G %4.1f%%  new %4.1f%%  'because you liked' %4.1f%%\n",cnt[2],100.0*nA[2]/cnt[2],100.0*nG[2]/cnt[2],100.0*nNew[2]/cnt[2],100.0*nWhy[2]/cnt[2]);
            assert(nA[2]*cnt[0]<=nA[0]*cnt[2]&&nA[2]*100<cnt[2]*4);   /* A's share drops, under 4 % (10 % of the library) */
            assert(nG[2]*cnt[0]>nG[0]*cnt[2]&&nG[2]*100>cnt[2]*50);   /* G's share rises, past half (20 % of the library) */
            assert(nNew[2]>0);                            /* and it still explores */
            /* Picked for you: G first, never A; the mixtape takes a few of them */
            {   int *l=NULL,n=ts_picks(&l),g=0,a=0;assert(n>=10);
                for(int i=0;i<n;i++){int k=TMS_ARTIST(mt[l[i]].album);g+=TMS_GENRE(k)==1;a+=k==0;}
                printf("taste model:   Picked for you: %d songs, %d Jazz, %d by A\n",n,g,a);
                assert(a==0&&g*10>=n*8);free(l);}
            picks_page();frame(500);shot("taste-picks.ppm");assert(find_label(pages[npages-1],"Picked for you"));page_pop();frame(400);
            /* Start over: asks first, then forgets every swipe */
            taste_reset_page();frame(400);shot("taste-reset.ppm");
            {   lv_obj_t *go=find_label(pages[npages-1],"Forget every swipe now");assert(go);
                lv_obj_send_event(lv_obj_get_parent(go),LV_EVENT_CLICKED,NULL);frame(400);}
            assert(ts.n_rated==0&&tm_ready()&&tm.swipes==0);
            memcpy(ts.rated,saved,(size_t)rated0*MAX_PATH);ts.n_rated=rated0;ts.gen++;free(saved);
            ts_save_file(TS_TASTE,true);
            sl_lib=keep;
            #undef TMS_ARTIST
            #undef TMS_GENRE
        }
        /* ---- end taste-model ---- */
        smart_list_page(SMART_MOST);frame(600);shot("most-played.ppm");page_pop();frame(400);
        smart_list_page(SMART_FORGOTTEN);frame(400);page_pop();frame(400);
        wrapped_page();frame(600);shot("wrapped.ppm");
        lv_obj_scroll_by(lv_obj_get_child(pages[npages-1],1),0,-700,LV_ANIM_OFF);frame(100);shot("wrapped-2.ppm");
        wrapped_page_year(2025,true);frame(500);shot("wrapped-2025.ppm");
        while(npages>1){page_pop();frame(400);}
        /* hist_poll: 35 s of a song, then the next one starts: one more record */
        test_status=AUDIO_STATUS_PLAY;strlcpy(test_track.path,TPT(&tr[7]),sizeof test_track.path);
        test_track.length=200000;test_track.elapsed=0;
        long before=fake_size(HIST_FILE);
        for(int i=0;i<150;i++){hist_poll();test_track.elapsed+=250;}
        assert(fake_size(HIST_FILE)==before);
        strlcpy(test_track.path,TPT(&tr[8]),sizeof test_track.path);test_track.elapsed=0;hist_poll();
        assert(fake_size(HIST_FILE)==before+12);
        test_track.elapsed=20000;hist_poll();hist_flush();assert(fake_size(HIST_FILE)==before+12);   /* seek: no credit */
        {   /* a torn record is dropped before the next append; junk files start over */
            fd=open(HIST_FILE,O_WRONLY|O_APPEND,0666);assert(write(fd,"xyz",3)==3);sim_close(fd);
            struct sl_play extra={sep+500,sl_hist_hash(TPT(&tr[9])),60000};hist_append(&extra);
            assert(fake_size(HIST_FILE)==before+24);
            hv=hist_view_load();assert(hv && hv->n==nl+2 && hv->track[hv->n-1]==9);hist_view_free(hv);
            fd=open(HIST_FILE,O_WRONLY|O_TRUNC,0666);assert(write(fd,"ab",2)==2);sim_close(fd);
            hist_append(&extra);assert(fake_size(HIST_FILE)==16);
            hv=hist_view_load();assert(hv && hv->n==1);hist_view_free(hv);
            /* a log past twice the cap keeps the newest plays and is trimmed */
            int big=2*SL_HIST_MAX+10;struct sl_play *many=malloc(big*sizeof *many);
            for(int i=0;i<big;i++)many[i]=(struct sl_play){sep+i,sl_hist_hash(TPT(&tr[i%12])),i==big-1?999999999u:30000};
            fd=open(HIST_FILE,O_WRONLY|O_TRUNC,0666);assert(write(fd,&magic,4)==4 && write(fd,many,big*sizeof *many)==(ssize_t)(big*sizeof *many));sim_close(fd);
            free(many);
            hv=hist_view_load();assert(hv && hv->n==SL_HIST_MAX && hv->plays[0].when==sep+big-SL_HIST_MAX);
            assert(hv->plays[hv->n-1].ms==3u*3600u*1000u);          /* absurd listening time capped */
            hist_view_free(hv);assert(fake_size(HIST_FILE)==4+SL_HIST_MAX*12);
            hv=hist_view_load();assert(wrapped_compute(hv,2027,&w) && w.years[0]==2027 && !w.plays);  /* next year reachable */
            hist_view_free(hv);
        }
        {   /* Skip Graveyard: three skips bury a song; haunt it, then bring it back */
            grave_flush(true);remove(GRAVE_FILE);graves_loaded=false;hist_tracker=(struct sl_hist_tracker){0};
            test_status=AUDIO_STATUS_PLAY;test_track.length=200000;
            for(int k=0;k<3;k++){
                strlcpy(test_track.path,TPT(&tr[2]),sizeof test_track.path);test_track.elapsed=0;
                for(int i=0;i<20;i++){hist_poll();test_track.elapsed+=250;}      /* 5 s */
                strlcpy(test_track.path,TPT(&tr[4]),sizeof test_track.path);test_track.elapsed=0;hist_poll();
            }
            assert(grave_dirty && fake_size(GRAVE_FILE)<0);    /* not written while the next song loads */
            grave_dirty_at-=5000;hist_poll();assert(!grave_dirty && fake_size(GRAVE_FILE)==(long)sizeof graves);
            graves_loaded=false;graves_load();     /* it was saved */
            int gi=sl_grave_find(&graves,sl_hist_hash(TPT(&tr[2])));
            assert(gi>=0 && graves.g[gi].skips==3 && sl_grave_buried(&graves.g[gi]));
            int buried[4];assert(grave_buried(buried,4)==1 && buried[0]==2);
            foryou_page();frame(600);assert(find_label(pages[npages-1],"Skip Graveyard"));page_pop();frame(400);
            grave_page();frame(700);assert(grave_canvas && grave_timer && find_label(pages[npages-1],"R.I.P."));
            shot("graveyard.ppm");
            lv_obj_t *stone=lv_obj_get_child(grave_list,1);
            lv_obj_send_event(stone,LV_EVENT_CLICKED,NULL);frame(500);
            assert(grave_sheet_track==2 && find_label(pages[npages-1],"HAUNT IT"));shot("graveyard-sheet.ppm");
            lv_obj_t *sheet=lv_obj_get_child(pages[npages-1],1);
            lv_obj_send_event(lv_obj_get_child(sheet,4),LV_EVENT_CLICKED,NULL);frame(500);
            assert(sl_grave_haunted(&graves,sl_hist_hash(TPT(&tr[2]))) && npages>1 &&
                   pages[npages-1]==grave_page_obj && find_label(grave_list,"HAUNTED"));
            shot("graveyard-haunted.ppm");
            /* the ghost skips it when it comes up, but not forever */
            queued=5;int nexts=audio_nexts;
            strlcpy(test_track.path,TPT(&tr[2]),sizeof test_track.path);test_track.elapsed=0;hist_poll();
            assert(audio_nexts==nexts+1);
            for(int k=0;k<8;k++){strlcpy(test_track.path,k&1?TPT(&tr[2]):"/x/other.mp3",sizeof test_track.path);hist_poll();}
            strlcpy(test_track.path,TPT(&tr[4]),sizeof test_track.path);hist_poll();
            /* bring it back: forgiven, and it plays next */
            lv_obj_send_event(lv_obj_get_child(grave_list,1),LV_EVENT_CLICKED,NULL);frame(500);
            assert(find_label(pages[npages-1],"STOP HAUNTING"));
            started=0;queued=3;
            lv_obj_send_event(lv_obj_get_child(lv_obj_get_child(pages[npages-1],1),3),LV_EVENT_CLICKED,NULL);frame(500);
            assert(sl_grave_find(&graves,sl_hist_hash(TPT(&tr[2])))<0 && queue_tracks[1]==2);
            assert(lv_obj_get_child_count(grave_list)==1 && find_label(grave_list,"Nothing buried yet"));   /* the one empty state */
            shot("graveyard-empty.ppm");
            page_pop();frame(500);assert(!grave_page_obj && !grave_timer && !grave_buf);
            remove(GRAVE_FILE);test_status=0;
        }
        {   /* the pet's mixtape: favourite artist first, its loved style, nothing buried */
            struct sl_pet_state saved_pet=pet;bool saved_loaded=pet_loaded;
            sl_pet_init(&pet,pet_clock());sl_pet_choose(&pet,SL_PET_SLOP,"Mochi",pet_clock());sl_pet_hatch(&pet,pet_clock());
            strlcpy(pet.taste[0].artist,"Ada Lane",sizeof pet.taste[0].artist);pet.taste[0].minutes=20;
            strlcpy(pet.taste[1].artist,"Mira Sol",sizeof pet.taste[1].artist);pet.taste[1].minutes=5;
            pet.checksum=sl_pet_checksum(&pet);pet_loaded=true;
            remove(GRAVE_FILE);graves_loaded=false;graves_load();
            sl_grave_skip(&graves,sl_hist_hash(TPT(&tr[10])));sl_grave_haunt(&graves,sl_hist_hash(TPT(&tr[10])),true);
            mix_n=0;mix_rerolls=0;mix_record();
            assert(mix_n==11 && !mix_has(10) && sl_lib.tracks[mix_tracks[0]].album==2);   /* Ada Lane opens */
            for(int t=0;t<12;t+=3)assert(t==9||mix_has(t)||t==10);                         /* the rock songs */
            int first[MIX_MAX];memcpy(first,mix_tracks,sizeof first);
            mix_record();assert(!memcmp(first,mix_tracks,mix_n*sizeof(int)));            /* the same all day */
            mix_rerolls++;mix_record();assert(memcmp(first,mix_tracks,mix_n*sizeof(int)));mix_rerolls=0;mix_record();
            /* a pet with no taste and no tag database still makes a tape */
            {   struct sl_pet_taste keep[SL_PET_TASTE];memcpy(keep,pet.taste,sizeof keep);memset(pet.taste,0,sizeof pet.taste);
                tc_usable=false;mix_record();assert(mix_n==11 && !mix_has(10));tc_usable=true;
                memcpy(pet.taste,keep,sizeof keep);mix_record();}
            pet_page();frame(600);assert(find_label(pages[npages-1],"MAKE ME A MIXTAPE") && !lv_obj_has_flag(pet_mix_btn,LV_OBJ_FLAG_HIDDEN));
            lv_obj_send_event(pet_mix_btn,LV_EVENT_CLICKED,NULL);frame(600);
            assert(mix_page_obj && pages[npages-1]==mix_page_obj && find_label(mix_body,"Mochi's mix") && find_label(mix_body,"SIDE B"));
            shot("mixtape.ppm");
            lv_obj_scroll_by(mix_body,0,-560,LV_ANIM_OFF);frame(100);shot("mixtape-songs.ppm");lv_obj_scroll_to_y(mix_body,0,LV_ANIM_OFF);
            started=-1;SFX_RESET();
            lv_obj_send_event(lv_obj_get_child(mix_body,2),LV_EVENT_CLICKED,NULL);frame(100);
            assert(started==0 && queued==11 && queue_tracks[0]==mix_tracks[0] && np_open && sfx_last==SFX_TAPE_CLICK);
            np_hide(0);frame(500);
            test_status=AUDIO_STATUS_PLAY;strlcpy(test_track.path,TPT(&tr[mix_tracks[0]]),sizeof test_track.path);
            mix_reel_angle=0;frame(300);assert(mix_reel_angle>0);                          /* the reels turn */
            lv_obj_send_event(lv_obj_get_child(mix_body,3),LV_EVENT_CLICKED,NULL);
            char mp[MAX_PATH];assert(pl_path_for(mp,sizeof mp,"Mochi's Mixtape") && fake_size(mp)>100);
            lv_obj_send_event(lv_obj_get_child(mix_body,4),LV_EVENT_CLICKED,NULL);frame(100);
            assert(mix_rerolls==1 && mix_n==11 && find_label(mix_body,"SIDE A"));
            page_pop();frame(500);assert(!mix_page_obj && !mix_timer);
            page_pop();frame(500);
            remove(mp);remove(GRAVE_FILE);graves_loaded=false;test_status=0;
            pet=saved_pet;pet_loaded=saved_loaded;mix_n=0;mix_rerolls=0;
        }
        {   /* Settings search: the bubble springs into the bar, words find
             * settings by name or by what they do, a result opens its page */
            while(npages>1){page_pop();frame(300);}
            system_page();frame(500);
            lv_obj_t *pill=lv_obj_get_parent(find_label(pages[npages-1],"Search settings"));assert(pill);
            lv_obj_send_event(pill,LV_EVENT_CLICKED,NULL);
            frame(120);assert(sqb);shot("settings-search-bubble.ppm");
            for(int i=0;i<10;i++)frame(40);
            assert(!sqb&&sq_page&&pages[npages-1]==sq_page&&sq_scope==SQ_SCOPE_SETTINGS);
            frame(300);
            sq_add("V");sq_add("O");sq_add("L");frame(300);
            assert(find_label(sq_body,"Volume button step")&&find_label(sq_body,"Power-on volume"));
            shot("settings-search.ppm");
            sq_clear_cb(NULL);sq_add("W");sq_add("A");sq_add("K");sq_add("E");frame(300);   /* by what it does */
            assert(find_label(sq_body,"Tap to wake"));
            lv_obj_send_event(lv_obj_get_parent(find_label(sq_body,"Tap to wake")),LV_EVENT_CLICKED,NULL);frame(500);
            assert(find_label(pages[npages-1],"Tap to wake")&&pages[npages-1]!=sq_page);   /* Buttons */
            while(npages>1){page_pop();frame(300);}
            assert(!sq_page);
            printf("PASS: Settings search: bubble, names and keywords, opens the page.\n");
        }
        /* search: letters narrow results; Hebrew; drawing a letter */
        search_page();frame(600);shot("search.ppm");
        {   /* QWERTY: q starts the top row, Delete ends the third; the space bar names the keyboard */
            struct kb *kb=sq_kbd;assert(kb&&kb==kb_now()&&kb->lay==KB_EN&&kb->kind==KB_SEARCH);
            int q=kb_find(kb,'q'),w=kb_find(kb,'w'),del=kb_find_act(kb,KA_BKSP),sp=kb_find_act(kb,KA_SPACE);
            assert(q>=0&&w>=0&&kb->k[q].row==0&&kb->k[q].col==0&&kb->k[del].row==2&&kb->k[del-1].cp=='m');
            assert(!strcmp(lv_label_get_text(kb->k[sp].l),"English")&&kb_find_act(kb,KA_GLOBE)>=0&&kb_find_act(kb,KA_DRAW)>=0);
            /* through the real touch path: keys type on the lift, with the letter shown big over the key */
            int qx,qy,wx,wy;kb_xy(kb,q,&qx,&qy);kb_xy(kb,w,&wx,&wy);
            touch_at(qx,qy,true);assert(!lv_obj_has_flag(kb->pop,LV_OBJ_FLAG_HIDDEN)&&!sq_text[0]);
            shot("search-key-preview.ppm");
            touch_at(qx,qy,false);assert(lv_obj_has_flag(kb->pop,LV_OBJ_FLAG_HIDDEN)&&!strcmp(sq_text,"q"));
            kb_tap(kb,del);assert(!sq_text[0]);
            /* a tap in the gap between q and w types the nearer one */
            lv_area_t qa,wa;lv_obj_get_coords(kb->k[q].o,&qa);lv_obj_get_coords(kb->k[w].o,&wa);
            touch_at((qa.x2+wa.x1)/2,qy,true);touch_at((qa.x2+wa.x1)/2,qy,false);
            assert(strlen(sq_text)==1&&(sq_text[0]=='q'||sq_text[0]=='w'));sq_backspace();
            /* press q, drift onto w, lift: w, as the highlight said */
            touch_at(qx,qy,true);touch_at(qx+10,qy,true);touch_at(wx,qy,true);
            assert(kb->hot==w);touch_at(wx,qy,false);assert(!strcmp(sq_text,"w"));sq_backspace();
            frame(300);
        }
        assert(search_hides_mini() && sq_kb_shown);
        sq_add("N");sq_add("I");frame(300);
        assert(!strcmp(sq_text,"NI") && find_label(sq_body,"SONGS \xc2\xb7 2"));
        shot("search-results.ppm");
        lv_obj_t *row=NULL;for(uint32_t i=0;i<lv_obj_get_child_count(sq_body);i++){lv_obj_t *c=lv_obj_get_child(sq_body,i);if(lv_obj_get_child_count(c))row=lv_obj_get_child(c,0);}
        assert(row);started=-1;lv_obj_send_event(row,LV_EVENT_CLICKED,NULL);frame(100);
        assert(started==0 && queued==2 && np_open);np_hide(0);frame(500);
        sq_clear_cb(NULL);kb_tap_act(sq_kbd,KA_GLOBE);frame(100);
        {   /* the globe: the Israeli layout, ק leading the top row, ש the middle, Delete ending the top */
            struct kb *kb=sq_kbd;assert(kb->lay==KB_HE);
            int k0=kb_find(kb,0x5e7),sh=kb_find(kb,0x5e9),del=kb_find_act(kb,KA_BKSP);
            assert(kb->k[k0].row==0&&kb->k[k0].col==0&&kb->k[sh].row==1&&kb->k[sh].col==0&&kb->k[del].row==0&&kb_find_act(kb,KA_SHIFT)<0);
            assert(!strcmp(lv_label_get_text(kb->k[kb_find_act(kb,KA_SPACE)].l),"\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa"));
            kb_tap(kb,sh);assert(!strcmp(sq_text,"\xd7\xa9"));
            kb_tap(kb,del);assert(!sq_text[0]);frame(300);
        }
        sq_add("\xd7\xa9");frame(300);
        assert(find_label(sq_body,"ARTISTS") && find_label(sq_body,"SONGS \xc2\xb7 4"));shot("search-hebrew.ppm");   /* her 3 songs and the title */
        sq_backspace();frame(300);assert(!sq_text[0]);
        sq_add("C");sq_add("A");sq_add("F");sq_add("E");frame(300);assert(find_label(sq_body,"SONGS \xc2\xb7 1"));  /* é folds to e */
        sq_clear_cb(NULL);kb_tap_act(sq_kbd,KA_GLOBE);frame(100);assert(sq_kbd->lay==KB_EN);
        {   /* swipe typing: a glide through n-i-g-h-t types "night", the
             * runners-up wait in the bar; off, the same glide types one letter */
            for(int i=0;i<20;i++)frame(40);                 /* the library's words are read while the page is open */
            assert(sq_kbd->voc.w&&sq_kbd->voc.n>1000&&!kb_voc_building(sq_kbd));
            glide_word("night");
            printf("glide: \"%s\" from %d points, %d words\n",sq_text,sq_kbd->pn,sq_kbd->voc.n);
            assert(!strcmp(sq_text,"night")&&sq_kbd->sug_glide&&sq_kbd->nsug>1);
            frame(300);shot("search-glide.ppm");
            glide_word("tide");assert(!strcmp(sq_text,"night tide"));
            sq_clear_cb(NULL);global_settings.sloppy_swipe_typing=0;
            glide_word("night");assert(strlen(sq_text)==1);
            global_settings.sloppy_swipe_typing=1;sq_clear_cb(NULL);frame(300);
        }
        kb_tap_act(sq_kbd,KA_DRAW);frame(100);
        lv_area_t pad;lv_obj_get_coords(sq_pad,&pad);
        int x0=pad.x1+120,y0=pad.y1+40;
        for(int i=0;i<=10;i++)touch_at(x0,y0+i*16,true);
        for(int i=1;i<=8;i++)touch_at(x0+i*14,y0+160,true);
        touch_at(x0+112,y0+160,false);shot("search-draw.ppm");frame(800);
        assert(!strcmp(sq_text,"L"));
        /* back to the keys right after a stroke (before it is read) must not crash */
        touch_at(x0,y0,true);touch_at(x0+40,y0+40,true);touch_at(x0+40,y0+40,false);
        assert(sq_ink[0] && lv_obj_is_valid(sq_ink[0]) && sq_recognize_at);   /* a stroke waiting to be read */
        lv_obj_send_event(lv_obj_get_parent(find_label(sq_kb,"ABC")),LV_EVENT_CLICKED,NULL);frame(800);
        assert(!sq_ink[0] && !strcmp(sq_text,"L") && !sq_pad && !lv_obj_has_flag(sq_kbd->keys,LV_OBJ_FLAG_HIDDEN));
        kb_tap_act(sq_kbd,KA_DRAW);frame(100);
        sq_show_kb(false);frame(100);assert(!sq_kb_shown && lv_obj_has_flag(sq_kb,LV_OBJ_FLAG_HIDDEN));
        page_pop();frame(500);assert(!sq_page && !search_hides_mini());
        remove(HIST_FILE);
        sl_lib=saved;
        {   /* the keyboard (sloppy_kb.inc), on a name field: every behaviour through the real touch path */
            int f0=global_settings.sloppy_kb_first,s0=global_settings.sloppy_kb_second;
            eq_save_page(NULL);frame(400);
            struct kb *kb=kb_now();assert(kb&&kb->kind==KB_NAME&&kb->lay==KB_EN);
            lv_obj_t *ta=kb->ta;lv_textarea_set_text(ta,"");kb_changed(kb);
            assert(kb->shift==1);                                   /* autocapital: a name starts big */
            for(const char *c="late";*c;c++)kb_tap(kb,kb_find(kb,(uint32_t)*c));
            assert(!strcmp(lv_textarea_get_text(ta),"Late")&&!kb->shift);
            kb_tap(kb,kb_find(kb,'.'));kb_tap_act(kb,KA_SPACE);assert(kb->shift==1);                  /* and so does a sentence */
            lv_textarea_set_text(ta,"Late nig");kb_changed(kb);   /* typed: the bar finishes the word */
            assert(kb->nsug>=2&&!strcmp(kb->sug[0],"night"));
            lv_obj_send_event(kb->sug_o[1],LV_EVENT_CLICKED,NULL);   /* the middle: the best */
            assert(!strcmp(lv_textarea_get_text(ta),"Late night "));
            /* Delete held: one letter at once, more while held, then whole words */
            lv_textarea_set_text(ta,"ab cd ef gh ij kl");kb_changed(kb);   /* the field holds 20 */
            int del=kb_find_act(kb,KA_BKSP),dx,dy;kb_xy(kb,del,&dx,&dy);
            touch_at(dx,dy,true);assert(strlen(lv_textarea_get_text(ta))==16);
            for(int i=0;i<12;i++)touch_at(dx,dy,true);              /* 480 ms: repeating */
            size_t mid=strlen(lv_textarea_get_text(ta));assert(mid<16&&mid>=12);
            for(int i=0;i<16;i++)touch_at(dx,dy,true);              /* past eight: words go */
            touch_at(dx,dy,false);
            const char *left=lv_textarea_get_text(ta);printf("delete held 2.1 s: \"%s\"\n",left);
            assert(strlen(left)+4<=mid&&strlen(left)%3==0);   /* whole words went: "ab cd " */
            /* the cursor rides the space bar: 14 px a letter */
            lv_textarea_set_text(ta,"abcdef");kb_changed(kb);
            int sp=kb_find_act(kb,KA_SPACE),sx,sy;kb_xy(kb,sp,&sx,&sy);
            touch_at(sx,sy,true);for(int i=1;i<=6;i++)touch_at(sx-i*10,sy,true);touch_at(sx-60,sy,false);
            assert(lv_textarea_get_cursor_pos(ta)==2&&!strcmp(lv_textarea_get_text(ta),"abcdef"));   /* 60 px: four back, no space typed */
            kb_type(kb,"x");assert(!strcmp(lv_textarea_get_text(ta),"abxcdef"));
            /* a held letter offers its accents; the finger slides to one */
            lv_textarea_set_text(ta,"caf");kb_changed(kb);
            int e=kb_find(kb,'e'),ex,ey;kb_xy(kb,e,&ex,&ey);
            touch_at(ex,ey,true);for(int i=0;i<12;i++)touch_at(ex,ey,true);
            assert(kb->accenting&&!lv_obj_has_flag(kb->acc,LV_OBJ_FLAG_HIDDEN)&&kb->acc_n>=6&&kb->acc_cp[0]=='e'&&kb->acc_cp[1]=='3');
            lv_area_t aa;lv_obj_get_coords(kb->acc,&aa);touch_at(aa.x1+SL_SP_XS+2*44+20,ey-60,true);
            assert(kb->acc_sel==2);touch_at(aa.x1+SL_SP_XS+2*44+20,ey-60,false);
            assert(!strcmp(lv_textarea_get_text(ta),"caf\xc3\xa9"));       /* é */
            /* held and let go without sliding: the letter itself */
            lv_textarea_set_text(ta,"");kb_changed(kb);kb_set_shift(kb,0);
            touch_at(ex,ey,true);for(int i=0;i<12;i++)touch_at(ex,ey,true);touch_at(ex,ey,false);
            assert(!strcmp(lv_textarea_get_text(ta),"e"));
            /* Hebrew glide (it used to find nothing: the only Hebrew words it knew were a few settings' names) */
            kb_tap_act(kb,KA_GLOBE);assert(kb->lay==KB_HE&&kb->voc.w&&kb->voc.n>300);
            lv_textarea_set_text(ta,"");kb_changed(kb);
            kb_glide(kb,"\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d",true);          /* שלום */
            assert(!strcmp(lv_textarea_get_text(ta),"\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"));
            kb_glide(kb,"\xd7\x96\xd7\x9e\xd7\x9f",true);                   /* זמן, ending on a final letter */
            assert(!strcmp(lv_textarea_get_text(ta),"\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\x96\xd7\x9e\xd7\x9f"));
            kb_tap_act(kb,KA_GLOBE);assert(kb->lay==KB_EN);
            /* Arabic, chosen as the second keyboard: its own layout and words */
            global_settings.sloppy_kb_second=KB_AR+1;
            kb_tap_act(kb,KA_GLOBE);assert(kb->lay==KB_AR&&kb->voc.n>300&&kb_find_act(kb,KA_SHIFT)<0);
            assert((unsigned char)lv_label_get_text(kb->k[kb_find_act(kb,KA_SPACE)].l)[0]>=0xd8);   /* its name, shaped */
            lv_textarea_set_text(ta,"");kb_changed(kb);
            kb_glide(kb,"\xd8\xad\xd8\xa8\xd9\x8a\xd8\xa8\xd9\x8a",true);   /* حبيبي */
            assert(!strcmp(sl_ta_text(ta),"\xd8\xad\xd8\xa8\xd9\x8a\xd8\xa8\xd9\x8a"));   /* read back as letters, not the shapes LVGL stores */
            kb_glide(kb,"\xd8\xa3\xd8\xba\xd9\x86\xd9\x8a\xd8\xa9",true);   /* أغنية: the hamza rides the alif's key */
            assert(strstr(sl_ta_text(ta),"\xd8\xa3\xd8\xba\xd9\x86\xd9\x8a\xd8\xa9"));
            frame(200);shot("keyboard-arabic-glide.ppm");
            /* German as the first: QWERTZ with ü ö ä on keys, ß held on s */
            global_settings.sloppy_kb_first=KB_DE+1;
            kb_switch(kb,kb_chosen(0));assert(kb->lay==KB_DE&&kb_find(kb,0xfc)>=0&&kb_find(kb,0xf6)>=0&&kb_find(kb,0xe4)>=0);
            assert(kb->k[kb_find(kb,'z')].row==0&&kb->k[kb_find(kb,'y')].row==2);
            lv_textarea_set_text(ta,"");kb_changed(kb);kb_type(kb,"Gr");
            int s_=kb_find(kb,'s'),sx2,sy2;kb_xy(kb,s_,&sx2,&sy2);
            touch_at(sx2,sy2,true);for(int i=0;i<12;i++)touch_at(sx2,sy2,true);
            lv_obj_get_coords(kb->acc,&aa);touch_at(aa.x1+SL_SP_XS+44+20,sy2-60,true);touch_at(aa.x1+SL_SP_XS+44+20,sy2-60,false);
            assert(!strcmp(lv_textarea_get_text(ta),"Gr\xc3\x9f"));
            kb_glide(kb,"sch\xc3\xb6n",true);assert(strstr(lv_textarea_get_text(ta),"sch\xc3\xb6n"));
            /* pinyin: letters become syllables, the bar offers hanzi; space takes the first */
            kb_switch(kb,KB_ZH);lv_textarea_set_text(ta,"");kb_changed(kb);
            for(const char *c="nihao";*c;c++)kb_tap(kb,kb_find(kb,(uint32_t)*c));
            assert(!strcmp(kb->py_buf,"nihao")&&kb->py_n>3&&!memcmp(kb->py_c[0],"\xe4\xbd\xa0\xe5\xa5\xbd",6));   /* 你好 */
            assert(!lv_obj_has_flag(kb->py_row,LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(kb->bar,LV_OBJ_FLAG_HIDDEN));
            frame(200);shot("keyboard-pinyin.ppm");
            kb_tap_act(kb,KA_SPACE);assert(!strcmp(sl_ta_text(ta),"\xe4\xbd\xa0\xe5\xa5\xbd")&&!kb->py_buf[0]);
            for(const char *c="zjl";*c;c++)kb_tap(kb,kb_find(kb,(uint32_t)*c));          /* initials: 周杰伦 */
            assert(kb->py_n&&!memcmp(kb->py_c[0],"\xe5\x91\xa8\xe6\x9d\xb0\xe4\xbc\xa6",9));
            kb_tap_act(kb,KA_BKSP);assert(!strcmp(kb->py_buf,"zj"));                       /* Delete takes a letter first */
            kb_tap_act(kb,KA_BKSP);kb_tap_act(kb,KA_BKSP);assert(!kb->py_buf[0]&&lv_obj_has_flag(kb->py_row,LV_OBJ_FLAG_HIDDEN));
            for(const char *c="de";*c;c++)kb_tap(kb,kb_find(kb,(uint32_t)*c));
            assert(!memcmp(kb->py_c[0],"\xe7\x9a\x84",3));                                   /* 的 */
            lv_obj_send_event(lv_obj_get_child(kb->py_row,2),LV_EVENT_CLICKED,NULL);         /* the second: 地 */
            assert(!strcmp(sl_ta_text(ta),"\xe4\xbd\xa0\xe5\xa5\xbd\xe5\x9c\xb0")&&!kb->py_buf[0]);
            for(const char *c="yinyueh";*c;c++)kb_tap(kb,kb_find(kb,(uint32_t)*c));       /* a word, then a begun syllable */
            assert(kb->py_n&&!memcmp(kb->py_c[0],"\xe9\x9f\xb3\xe4\xb9\x90",6)&&kb->py_used[0]==6);   /* 音乐, the h stays */
            kb_py_pick(kb,0);assert(!strcmp(kb->py_buf,"h"));
            kb_tap_act(kb,KA_ENTER);assert(strstr(sl_ta_text(ta),"\xe9\x9f\xb3\xe4\xb9\x90h")&&!kb->py_buf[0]);   /* Enter: the letters left, as typed */
            /* romaji: kana as it is typed; the bar offers hiragana, the kanji word, katakana */
            kb_switch(kb,KB_JA);lv_textarea_set_text(ta,"");kb_changed(kb);
            for(const char *c="konnichiha";*c;c++)kb_tap(kb,kb_find(kb,(uint32_t)*c));
            assert(kb->py_n==2&&!strcmp(kb->ja_c[0],"こんにちは")&&!strcmp(kb->ja_c[1],"コンニチハ"));
            kb_tap_act(kb,KA_SPACE);assert(!strcmp(sl_ta_text(ta),"こんにちは")&&!kb->py_buf[0]);
            for(const char *c="ongaku";*c;c++)kb_tap(kb,kb_find(kb,(uint32_t)*c));
            assert(kb->py_n==3&&!memcmp(kb->py_c[1],"音楽",6));
            frame(200);shot("keyboard-kana.ppm");
            kb_py_pick(kb,1);assert(!strcmp(sl_ta_text(ta),"こんにちは音楽"));
            for(const char *c="kitten";*c;c++)kb_tap(kb,kb_find(kb,(uint32_t)*c));       /* a small tsu; a last n is ん on Enter */
            kb_tap_act(kb,KA_BKSP);assert(!strcmp(kb->py_buf,"kitte"));
            for(const char *c="n";*c;c++)kb_tap(kb,kb_find(kb,(uint32_t)*c));
            kb_tap_act(kb,KA_ENTER);assert(!strcmp(sl_ta_text(ta),"こんにちは音楽きってん")&&!kb->py_buf[0]);
            kb_py_pick(kb,0);
            {   /* every new language: picked by its own name, and its words */
                static const char *const settings_in[]={"Impostazioni","Instellingen","Ajustes","Ustawienia","Nastaven\xc3\xad","\xd0\x9d\xd0\xb0\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd0\xba\xd0\xb8","\xe8\xa8\xad\xe5\xae\x9a"};
                int l0=global_settings.sloppy_language;
                for(int l=SL_LANG_IT;l<=SL_LANG_JA;l++){global_settings.sloppy_language=l;assert(!strcmp(sl_tr("Settings"),settings_in[l-SL_LANG_IT]));}
                global_settings.sloppy_language=SL_LANG_RU;
                assert(!strcmp(sl_tr_count(1,"song","songs"),sl_tr("song"))&&!strcmp(sl_tr_count(21,"song","songs"),sl_tr("song")));
                assert(!strcmp(sl_tr_count(3,"song","songs"),"\xd0\xbf\xd0\xb5\xd1\x81\xd0\xbd\xd0\xb8")&&!strcmp(sl_tr_count(12,"song","songs"),sl_tr("songs")));   /* песни; 12 песен */
                global_settings.sloppy_language=SL_LANG_PL;
                assert(!strcmp(sl_tr_count(23,"album","albums"),"albumy")&&!strcmp(sl_tr_count(21,"album","albums"),sl_tr("albums")));
                {   char b[64];assert(!strcmp(sl_upper("\xc5\x82\xc3\xb3" "d\xc5\xba",b,sizeof b),"\xc5\x81\xc3\x93" "D\xc5\xb9"));   /* łódź: ŁÓDŹ */
                    assert(!strcmp(sl_lower("\xd0\x9c\xd0\xa3\xd0\x97\xd0\xab\xd0\x9a\xd0\x90",b,sizeof b),"\xd0\xbc\xd1\x83\xd0\xb7\xd1\x8b\xd0\xba\xd0\xb0"));}   /* МУЗЫКА */
                global_settings.sloppy_language=l0;
            }
            /* Chinese and Russian titles in the library show, at every size the lists use */
            {   const char *t0=sl_lib.tracks[0].title,*t1=sl_lib.tracks[1].title;
                sl_lib.tracks[0].title="\xe6\x99\xb4\xe5\xa4\xa9 \xc2\xb7 \xe5\x91\xa8\xe6\x9d\xb0\xe5\x80\xab";   /* 晴天 · 周杰倫: a traditional character too */
                sl_lib.tracks[1].title="\xd0\x9a\xd1\x83\xd0\xba\xd0\xbb\xd0\xb0 \xd0\x9a\xd0\xbe\xd0\xbb\xd0\xb4\xd1\x83\xd0\xbd\xd1\x8c\xd1\x8f";   /* Кукла Колдунья */
                songs_page();frame(500);
                assert(find_label(pages[npages-1],sl_lib.tracks[0].title)&&!glyphs_missing(pages[npages-1]));
                shot("library-cjk-cyrillic.ppm");page_pop();frame(400);
                sl_lib.tracks[0].title=t0;sl_lib.tracks[1].title=t1;
                assert(xf_packs[XF_CJK+1].mem&&xf_packs[XF_CJKT+1].mem&&xf_packs[XF_CYR+3].mem);   /* loaded as needed */
            }
            /* emoji in a name (headphones, a title): drawn, and what joins them takes no room */
            {   const char *t0=sl_lib.tracks[0].title,*t1=sl_lib.tracks[1].title;
                assert(!xf_packs[XF_EMO+1].mem);
                sl_lib.tracks[0].title="Tai's \xf0\x9f\x8e\xa7\xef\xb8\x8f Buds \xe2\x9c\xa8";                 /* 🎧️ ✨ */
                sl_lib.tracks[1].title="\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb \xe2\x9d\xa4\xef\xb8\x8f";   /* 👍🏽 👩‍💻 ❤️ */
                songs_page();frame(500);
                assert(find_label(pages[npages-1],sl_lib.tracks[0].title)&&!glyphs_missing(pages[npages-1]));
                shot("library-emoji.ppm");page_pop();frame(400);
                sl_lib.tracks[0].title=t0;sl_lib.tracks[1].title=t1;
                assert(xf_packs[XF_EMO+1].mem);
                lv_font_glyph_dsc_t g;
                assert(lv_font_get_glyph_dsc(&ss_semibold_23,&g,0x1f3a7,0)&&!g.is_placeholder&&g.adv_w>10);
                assert(lv_font_get_glyph_dsc(&ss_semibold_23,&g,0xfe0f,0)&&!g.is_placeholder&&g.adv_w==0);
                assert(lv_font_get_glyph_dsc(&ss_semibold_23,&g,0x1f3fd,0)&&g.adv_w==0);   /* a skin tone */
            }
            /* every layout builds, fits and names itself */
            for(int k=0;k<KB_N;k++){kb_switch(kb,k);lv_obj_update_layout(kb->root);
                for(int i=0;i<kb->nk;i++){lv_area_t ka;lv_obj_get_coords(kb->k[i].o,&ka);assert(ka.x1>=0&&ka.x2<W&&lv_area_get_width(&ka)>=36);}}
            global_settings.sloppy_kb_first=f0;global_settings.sloppy_kb_second=s0;
            page_pop();frame(400);
            /* Settings > Keyboards: the two, and a picker for each */
            keyboards_page();frame(400);assert(find_label(pages[npages-1],"First keyboard")&&find_label(pages[npages-1],"Second keyboard"));
            lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages-1],"Second keyboard")),LV_EVENT_CLICKED,NULL);frame(400);
            lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages-1],"Fran\xc3\xa7" "ais")),LV_EVENT_CLICKED,NULL);frame(100);
            assert(global_settings.sloppy_kb_second==KB_FR+1&&kb_chosen(1)==KB_FR);
            page_pop();frame(400);assert(find_label(pages[npages-1],"Fran\xc3\xa7" "ais"));
            page_pop();frame(400);
            global_settings.sloppy_kb_first=f0;global_settings.sloppy_kb_second=s0;
            glide_accuracy(true);
            printf("PASS: keyboard: autocapitals, the bar, Delete held, the cursor on the space bar, accents, Hebrew and Arabic glides, QWERTZ, every layout, Settings > Keyboards, glide accuracy.\n");
        }
        /* a full field, long words and every keyboard at both ends of what is typed */
        {   eq_save_page(NULL);frame(400);struct kb *kb=kb_now();lv_obj_t *ta=eq_name_ta;assert(kb&&ta);
            int t9p=global_settings.sloppy_kb_t9;
            /* T9 with room for three: Delete takes back the word, not what was there */
            kb_switch(kb,KB_T9);global_settings.sloppy_kb_t9=1;
            lv_textarea_set_text(ta,"abcdefghijklmnopq");t9_commit(kb);
            for(const char *d="46637";*d;d++)t9_press(kb,*d-'0');
            assert(strlen(lv_textarea_get_text(ta))==20);
            while(kb->t9_seq[0])kb_backspace(kb,false);
            assert(!strcmp(lv_textarea_get_text(ta),"abcdefghijklmnopq"));
            /* multi-tap on a full field: the second press is not "the next letter" of one that never went in */
            global_settings.sloppy_kb_t9=0;lv_textarea_set_text(ta,"abcdefghijklmnopqrst");t9_commit(kb);
            t9_press(kb,2);t9_press(kb,2);assert(!strcmp(lv_textarea_get_text(ta),"abcdefghijklmnopqrst"));
            global_settings.sloppy_kb_t9=t9p;
            /* a name in Hebrew or emoji fills the field: saved whole letters, not half of one */
            lv_textarea_set_text(ta,"");for(int i=0;i<20;i++)lv_textarea_add_text(ta,"\xd7\xa9");
            {   char name[24];strlcpy(name,sl_ta_text(ta),sizeof name);pl_utf8_trim(name);assert(utf8_whole(name)&&strlen(name)==22);}
            /* every keyboard: Delete and a letter with the cursor at the start, then at the end */
            for(int k=0;k<KB_N;k++){
                kb_switch(kb,k);lv_obj_update_layout(kb->root);
                lv_textarea_set_text(ta,"Hi \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d 12 \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85");kb_changed(kb);
                char was[128];strlcpy(was,lv_textarea_get_text(ta),sizeof was);
                lv_textarea_set_cursor_pos(ta,0);kb_backspace(kb,false);kb_backspace(kb,true);
                assert(!strcmp(lv_textarea_get_text(ta),was));
                int key=kb_find_act(kb,KA_CHAR);if(key<0)key=kb_find_act(kb,KA_T9);assert(key>=0);
                kb_tap(kb,key);kb_tap_act(kb,KA_SPACE);
                assert(utf8_whole(lv_textarea_get_text(ta))&&strlen(lv_textarea_get_text(ta))>strlen(was)&&lv_textarea_get_cursor_pos(ta)>0);
                assert(!strcmp(lv_textarea_get_text(ta)+strlen(lv_textarea_get_text(ta))-strlen(was),was));   /* in front, the rest untouched */
                lv_textarea_set_cursor_pos(ta,LV_TEXTAREA_CURSOR_LAST);
                kb_tap(kb,key);kb_tap_act(kb,KA_BKSP);kb_backspace(kb,true);kb_tap_act(kb,KA_SPACE);
                assert(utf8_whole(lv_textarea_get_text(ta)));
                t9_commit(kb);kb->py_buf[0]=0;
            }
            page_pop();frame(400);
            /* read back whole: a 600-letter password (Audiobookshelf's field), and a pet named in Hebrew */
            {   static char big[601];memset(big,'k',600);big[600]=0;assert(strlen(sl_unshape(big))==600);
                struct sl_pet_state ps;memset(&ps,0,sizeof ps);
                sl_pet_choose(&ps,SL_PET_BUN,"\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d",0);   /* 8 letters, 16 bytes */
                assert(utf8_whole(ps.name)&&strlen(ps.name)==14);
            }
            /* search: a query of Hebrew to the field's limit */
            search_page();frame(300);
            {   lv_textarea_set_text(sq_ta,"");for(int i=0;i<SL_Q_MAX-8;i++)lv_textarea_add_text(sq_ta,"\xd7\x90");
                lv_obj_send_event(sq_ta,LV_EVENT_VALUE_CHANGED,NULL);frame(200);assert(utf8_whole(sq_text)&&sq_text[0]);}
            page_pop();frame(400);
            printf("PASS: keyboard: a full field (T9 words, multi-tap), long and Hebrew names saved whole, every layout at both ends of the words.\n");
        }
        /* search cost on the 10,000-song library */
        search_page();frame(300);strlcpy(sq_text,"NIGHT",sizeof sq_text);
        uint32_t t0=sl_ms();sq_run();printf("search: %lu ms over %d songs\n",(unsigned long)(sl_ms()-t0),sl_lib.n_tracks);
        assert(find_label(sq_body,"SONGS \xc2\xb7 BEST 300 OF 2000"));
        page_pop();frame(500);
    }
    if (getenv("SLOPPY_MEMTRACE")) {   /* the heap after each round of an ordinary session: growth is a leak */
        while(npages>1){page_pop();frame(300);}
        test_status=AUDIO_STATUS_PLAY;
        for(int round=0;round<atoi(getenv("SLOPPY_MEMTRACE"));round++){
            static const char *names[]={"pages","search","foryou","quiz","spectrum","coverflow","np","panel","settings"};
            for(int step=0;step<9;step++){
                malloc_statistics_t a,b;malloc_zone_statistics(NULL,&a);
                switch(step){
                case 0: system_page();frame(300);sound_page();frame(300);page_pop();frame(300);page_pop();frame(300);
                        sl_library_page();frame(300);albums_page();frame(400);page_pop();frame(300);page_pop();frame(300);break;
                case 1: search_page();frame(400);for(int i=0;i<5;i++)frame(100);page_pop();frame(300);break;
                case 2: foryou_page();frame(400);if(td.page){page_pop();frame(300);}loved_page();frame(400);page_pop();frame(300);
                        while(npages>1){page_pop();frame(300);}break;
                case 3: quiz_page();frame(400);while(npages>1){page_pop();frame(300);}break;
                case 4: spa_open();frame(400);for(int i=0;i<10;i++)frame(33);spa_close();frame(300);break;
                case 5: cf_open();frame(900);for(int i=0;i<10;i++){cf.dirty=true;frame(50);}cf_close();frame(300);break;
                case 6: np_show();frame(500);uq_show();frame(400);uq_close();frame(400);np_hide(0);frame(400);break;
                case 7: qp_drag_to(650);frame(50);qp_release(650,900);frame(400);qp_close();frame(400);break;
                case 8: appearance_page();frame(300);text_size_page();frame(300);while(npages>1){page_pop();frame(300);}break;
                }
                malloc_zone_statistics(NULL,&b);
                printf("memtrace round %d %-9s %+8ld KiB  (in use %lu KiB)\n",round,names[step],
                       ((long)b.size_in_use-(long)a.size_in_use)/1024,(unsigned long)b.size_in_use/1024);
            }
        }
        test_status=0;
    }
    if (getenv("SLOPPY_FUZZ")) {   /* SLOPPY_FUZZ=<rounds>: random taps and drags on every page (run the ASan build) */
        static void (*const opens[])(void) = { sl_games_page, sl_library_page, system_page, appearance_page,
            settings_icon_page, theme_page, home_style_page, lock_player_page, sounds_page, accent_page,
            coin_page, magic_page, chess_page, grave_page, wrapped_page, mix_page, foryou_page, pet_page,
            eq_page, play_settings_page, qp_sleep_page, qp_display_page, quiz_page, ragdoll_page, search_page,
            radio_page, spoon_page, snake_page, albums_page, songs_page, artists_page, music_page };
        int fuzz_theme=global_settings.sloppy_theme,fuzz_style=global_settings.sloppy_home_style;
        int fuzz_vol=global_status.volume,fuzz_step=global_settings.sloppy_volume_step,fuzz_maxvol=global_settings.volume_limit;
        int rounds=atoi(getenv("SLOPPY_FUZZ"));srand(getenv("SLOPPY_FUZZ_SEED")?(unsigned)atoi(getenv("SLOPPY_FUZZ_SEED")):12345);
        while(npages>1){page_pop();frame(400);}
        for(int r=0;r<rounds;r++){
            test_status=rand()%3?AUDIO_STATUS_PLAY:0;
            int which=rand()%(int)(sizeof opens/sizeof opens[0]);
            if(getenv("SLOPPY_FUZZ_LOG"))fprintf(stderr,"fuzz round %d page %d\n",r,which);
            opens[which]();frame(400);
            if(getenv("SLOPPY_FUZZ_LOG"))for(int i=1;i<npages;i++)lv_obj_add_event_cb(pages[i],fuzz_page_deleted,LV_EVENT_DELETE,NULL);
            for(int e=0;e<30;e++){
                int x=rand()%W,y=rand()%H,kind=rand()%10;
                if(getenv("SLOPPY_FUZZ_LOG"))fprintf(stderr,"  ev %d kind %d at %d,%d npages %d np %d\n",e,kind,x,y,npages,np_open);
                if(kind<5){touch_at(x,y,true);touch_at(x,y,false);}
                else if(kind<7){int dx=(rand()%600)-300,dy=(rand()%600)-300;
                    for(int k=0;k<=6;k++)touch_at(x+dx*k/6<0?0:x+dx*k/6>=W?W-1:x+dx*k/6,y+dy*k/6<0?0:y+dy*k/6>=H?H-1:y+dy*k/6,k<6);}
                else if(kind==7)frame(rand()%1500);
                else if(kind==8&&npages>1){pet_tick();page_pop();frame(300);}
                else if(test_status){if(np_open)np_hide(0);else np_show();frame(500);}
                assert(npages>=1 && npages<=MAX_PAGES);
                {int nl=0;for(lv_layer_t *l=disp->layer_head;l;l=l->next)nl++;
                 if(nl!=1){fprintf(stderr,"FUZZ: %d draw layers left after round %d event %d (page %d)\n",nl,r,e,which);abort();}}
                for(int i=0;i<npages;i++)if(!lv_obj_is_valid(pages[i])){fprintf(stderr,"FUZZ: pages[%d] of %d deleted after round %d event %d (kind %d at %d,%d)\n",i,npages,r,e,kind,x,y);abort();}
            }
            if(np_open){np_hide(0);frame(500);}
            if(pet_care_locked){pet_care_until=sl_ms()-1;pet_tick();}   /* the UI tick ends care scenes; the harness has none */
            while(npages>1){page_pop();frame(300);}
            if(r%25==24){malloc_statistics_t ms;malloc_zone_statistics(NULL,&ms);   /* a soak: the heap should level off */
                printf("fuzz: %d rounds, heap %zu KB\n",r+1,ms.size_in_use/1024);}
        }
        /* a tap may have opened Cover Flow, Spectrum or the visualizer: their own
         * screens, with no system gestures over them */
        if(cf_is_open()){printf("fuzz: cover flow left open (cf %d spa %d vz %d)\n",cf.active,spa.active,vz.active);cf_close();frame(300);}
        /* a swipe may have left Up Next or the actions sheet up: no pull
         * from the top edge over them */
        if(uq_open){printf("fuzz: Up Next left open\n");uq_close();frame(500);}
        if(acts_open_now){printf("fuzz: actions sheet left open\n");acts_close();frame(400);}
        /* a drag from the top edge may have left the control centre open */
        if(qp_open||(qp&&!lv_obj_has_flag(qp,LV_OBJ_FLAG_HIDDEN))){printf("fuzz: control centre left open, grown %d\n",qx_px);qp_close();frame(600);}
        /* a tap may have picked another theme (Zune lowercases the words
         * the tests below look for): put the look back */
        if(global_settings.sloppy_theme!=fuzz_theme||global_settings.sloppy_home_style!=fuzz_style){
            global_settings.sloppy_theme=fuzz_theme;global_settings.sloppy_home_style=fuzz_style;
            appearance_reload(NULL);frame(600);while(npages>1){page_pop();frame(300);}}
        test_status=0;printf("PASS fuzz: %d rounds\n",rounds);
        /* what random taps may have changed, for the tests that follow */
        global_settings.sloppy_language=0;sl_text_step=0;test_track.elapsed=0;sl_bt_known=false;vz_hinted=false;
        global_settings.sloppy_listenbrainz=0;global_settings.sloppy_scrobble_log=0;
        global_status.volume=fuzz_vol;global_settings.sloppy_volume_step=fuzz_step;global_settings.volume_limit=fuzz_maxvol;
    }
    void probe_quiz_game(void);
    if (getenv("SLOPPY_SCROLLPROBE")) {   /* which game pages can be dragged up and down */
        while(npages>1){page_pop();frame(400);}
        struct { const char *name; void (*open)(void); } g[] = {
            {"pet",pet_page},{"snake",snake_page},{"radio",radio_page},{"coin",coin_page},{"chess",chess_page},
            {"quiz",quiz_page},{"magic",magic_page},{"spoon",spoon_page},{"ragdoll",ragdoll_page},{"grave",grave_page},{"quizgame",probe_quiz_game} };
        for(unsigned k=0;k<sizeof g/sizeof g[0];k++){
            g[k].open();frame(700);frame(700);
            lv_obj_t *stack[512];int n=0;stack[n++]=pages[npages-1];
            while(n){lv_obj_t *o=stack[--n];
                if(lv_obj_has_flag(o,LV_OBJ_FLAG_SCROLLABLE)&&!lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN)){
                    int over=lv_obj_get_scroll_top(o)+lv_obj_get_scroll_bottom(o);
                    int overx=lv_obj_get_scroll_left(o)+lv_obj_get_scroll_right(o);
                    if(over>0||overx>0)printf("scroll %-8s obj h=%d w=%d overflow y=%d x=%d dir=%d\n",g[k].name,(int)lv_obj_get_height(o),(int)lv_obj_get_width(o),over,overx,(int)lv_obj_get_scroll_dir(o));}
                for(uint32_t c=0;c<lv_obj_get_child_count(o)&&n<511;c++)stack[n++]=lv_obj_get_child(o,c);}
            while(npages>1){page_pop();frame(400);}
        }
    }
    if (getenv("SLOPPY_NP_LOOKS")) {   /* SLOPPY_NP_LOOKS=1: the player and an empty library in other looks (rebuilds Now Playing, so last) */
        while(npages>1){page_pop();frame(400);}
        if(np_open){np_hide(0);frame(500);}
        test_status=AUDIO_STATUS_PLAY;test_track.title=NULL;test_track.artist=NULL;test_track.albumartist=NULL;
        mini_update();frame(300);shot("mini-untagged.ppm");
        int nt=sl_lib.n_tracks,na=sl_lib.n_albums;sl_lib.n_tracks=sl_lib.n_albums=0;
        songs_page();frame(500);shot("library-empty-songs.ppm");page_pop();frame(400);
        albums_page();frame(500);shot("library-empty-albums.ppm");page_pop();frame(400);
        sl_lib.n_tracks=nt;sl_lib.n_albums=na;
        for(int look=0;look<2;look++){
            global_settings.sloppy_theme=look?2:1;sl_text_step=1;
            lv_obj_add_flag(np,LV_OBJ_FLAG_HIDDEN);np_build();   /* built in this look, as at boot: no retheme */
            assert(lv_color_eq(lv_obj_get_style_text_color(np_artist,0),lv_color_hex(C_SUB)) &&
                   lv_obj_get_style_bg_opa(np_pp,0)>LV_OPA_10 && !lv_color_eq(lv_obj_get_style_bg_color(np_pp,0),lv_color_hex(C_BG)));
            test_track.title=look?NULL:"A Very Long Song Title That Keeps Going Well Past The Edge";
            test_track.artist=look?NULL:"שולה חן";test_track.bitrate=look?0:256;
            np_show();frame(600);shot(look?"np-true-dark-large.ppm":"np-light-large.ppm");
            lv_area_t a,f;lv_obj_get_coords(np_artist,&a);lv_obj_get_coords(np_fmt,&f);
            assert(a.y2<f.y1 && lv_obj_get_width(np_fmt)<W-150);   /* nothing overlaps the times or the artist */
            np_hide(0);frame(500);
        }
        global_settings.sloppy_theme=0;sl_text_step=0;
    }
    cc_expand_tests();   /* the control centre, pulled further */
    if (getenv("SLOPPY_PERF")) {   /* SLOPPY_PERF=1 ./ui-test: touch-driven frames on the R1's own settings, music playing */
        while(npages>1){page_pop();frame(400);}
        sl_power_state_init(&sl_power,sl_ms());sl_power_apply();
        global_settings.sloppy_home_style=1;global_settings.sloppy_home_wallpaper=2;
        global_settings.sloppy_text_size=1;sl_text_step=1;global_settings.sloppy_dev_overlay=1;
        if(getenv("SLOPPY_PERF_THEME")){   /* =3: Zune (with its List home), =2: True dark, ... */
            global_settings.sloppy_theme=atoi(getenv("SLOPPY_PERF_THEME"));
            if(global_settings.sloppy_theme==3)global_settings.sloppy_home_style=2;}
        for(int32_t i=(int32_t)lv_obj_get_child_count(lv_layer_top())-1;i>=0;i--){   /* backdrops left by the tests' own qp_build() calls: the R1 has one */
            lv_obj_t *o=lv_obj_get_child(lv_layer_top(),i);
            if(o!=qp_bd&&lv_obj_get_event_count(o)&&lv_event_dsc_get_cb(lv_obj_get_event_dsc(o,0))==qp_bd_draw)lv_obj_delete(o);}
        if(getenv("SLOPPY_PERF_DIRECT")){sl_direct_ok=true;sl_set_rotation(LV_DISPLAY_ROTATION_0);}   /* =1: one full-screen buffer, as the R1 draws upright */
        appearance_reload(NULL);frame(700);frame(700);devhud_show(true);
        if(getenv("SLOPPY_PERF_SPECTRUM")){   /* =1: a wide live analyzer on top, =2: the same tile not live */
            mt_load();memmove(&mt.slot[1],&mt.slot[0],(size_t)mt.n*sizeof mt.slot[0]);mt.n++;
            mt.slot[0]=(struct mt_slot){.id=TL_SPECTRUM,.size=SZ_WIDE,.live=(uint8_t)(atoi(getenv("SLOPPY_PERF_SPECTRUM"))==2?0:atoi(getenv("SLOPPY_PERF_SPECTRUM")))};   /* 1 bars, 3 peaks, 5 curve */
            mt_save();appearance_reload(NULL);frame(700);frame(700);while(npages>1){page_pop();frame(400);}devhud_show(true);pf_feed=1;
            printf("perf with the spectrum tile %s\n",sp.canvas?"live":"not live");
        }
        test_status=AUDIO_STATUS_PLAY;mini_update();
        for(int i=0;i<200;i++)frame(16);   /* art and warm-ups settle */
        if(pf_feed&&sp.canvas){   /* the analyzer alone: FFT and bands, then a whole tick with the bars drawn */
            float db[SP_BARS];struct timespec a,b,c;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);
            for(int i=0;i<200;i++)sp_analyze(db);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            for(int i=0;i<200;i++){sp.drawn[i%SP_BARS][0]=-1;sp.last_ms=0;sp_tick(NULL);}
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&c);
            printf("perf spectrum analyze R1 est %.2f ms, tick, bars redrawn %.2f ms\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/200e6*PF_K,((c.tv_sec-b.tv_sec)*1e9+(c.tv_nsec-b.tv_nsec))/200e6*PF_K);
        }
        const char *only=getenv("SLOPPY_PERF_ONLY");int reps=only?60:1;   /* one scenario, looped, for a profiler */
        if(only&&!strcmp(only,"quizbare")){   /* the game page over home alone: is it the page or the menu below? */
        pf_begin("quiz game over home");
        for(int r=0;r<reps;r++){lib_state=SL_LIB_READY;quiz_game_page(QUIZ_MODE_MUSIC);for(int i=0;i<30;i++)pf_frame(16);
            while(npages>1){page_pop();for(int i=0;i<20;i++)pf_frame(16);}}
        pf_end();
        }
        if(!only||!strcmp(only,"quiz")){   /* the quiz menu, then a music game's first question */
        pf_begin("quiz open");
        for(int r=0;r<reps;r++){lib_state=SL_LIB_READY;quiz_page();for(int i=0;i<40;i++)pf_frame(16);
            quiz_game_page(QUIZ_MODE_MUSIC);for(int i=0;i<90;i++)pf_frame(16);
            while(npages>1){page_pop();for(int i=0;i<20;i++)pf_frame(16);}}
        pf_end();
        }
        if(!only||!strcmp(only,"page")){   /* a page pushed and popped: Zune's title glides in */
        pf_begin("page open");
        for(int r=0;r<5*reps;r++){sl_library_page();for(int i=0;i<35;i++)pf_frame(16);page_pop();for(int i=0;i<20;i++)pf_frame(16);}
        pf_end();
        }
        if(!only||!strcmp(only,"np")){   /* Now Playing on a real cover: up, a song playing, the lyrics opened and shut, down */
        bool ra=test_real_art;test_real_art=true;
        int keep_np=global_settings.sloppy_np_style;
        {char lp[MAX_PATH];if(sl_lrc_path(test_track.path,lp,sizeof lp,false)){int fd=open(lp,O_WRONLY|O_CREAT|O_TRUNC,0666);
            const char *t="[00:01.00]One\n[00:02.00]Two\n[00:03.00]Three\n[00:04.00]Four\n[00:05.00]Five\n";
            if(fd>=0){write(fd,t,strlen(t));sim_close(fd);}}lyr_path[0]=0;}
        static const char *const npn[]={"np glass","np classic","np centred","np waveform"};
        for(int sty=0;sty<4;sty++){
#ifndef LYR_FADE
            if(sty==2)break;                 /* no Centred before batch 5 */
#endif
            global_settings.sloppy_np_style=sty;now_playing_retheme();
            pf_begin(npn[sty]);
            for(int r=0;r<3*reps;r++){
                np_show();for(int i=0;i<40;i++)pf_frame(16);
                for(int i=0;i<60;i++){test_track.elapsed+=16;pf_frame(16);}
                if(lyr_btn&&!lv_obj_has_flag(lyr_btn,LV_OBJ_FLAG_HIDDEN)){lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);
                    if(r==0)printf("perf %s: lyrics %s\n",npn[sty],lyr_visible()?"shown":"missing");
                    for(int i=0;i<60;i++){test_track.elapsed+=16;pf_frame(16);}
                    lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);for(int i=0;i<20;i++)pf_frame(16);}
                np_hide(0);for(int i=0;i<30;i++)pf_frame(16);
            }
            pf_end();
        }
        for(int sty=0;sty<4;sty+=3){   /* playing, Now Playing up: the thin bar against the waveform, its shape known */
            global_settings.sloppy_np_style=sty;now_playing_retheme();
            np_show();for(int i=0;i<40;i++)frame(16);
            wv_poll();if(wv){for(int c=0;c<WV_N;c++)wv->col[c]=wave_shape(c);wv->changed=false;lv_obj_invalidate(np_seek);frame(16);}
            pf_begin(sty?"np wave playing":"np glass playing");
            for(int i=0;i<240;i++){test_track.elapsed+=16;float f=440,a=9000;test_feed(1,&f,&a);pf_frame(16);}
            pf_end();
            {   /* a scrub along it: every frame draws the whole bar */
                lv_area_t sb;lv_obj_get_coords(np_seek,&sb);int y=(sb.y1+sb.y2)/2;
                pf_begin(sty?"np wave scrub":"np glass scrub");
                t_x=sb.x1+40;t_y=y;t_down=true;pf_frame(16);
                for(int i=0;i<60;i++){t_x=sb.x1+40+i*5;pf_frame(16);}
                t_down=false;pf_frame(16);
                pf_end();
                for(int i=0;i<20;i++)frame(16);
            }
            np_hide(0);for(int i=0;i<30;i++)frame(16);
        }
        {   /* what one cover costs, thread CPU on the Mac x PF_K: the glass's lake (and its buttons) */
            global_settings.sloppy_np_style=0;now_playing_retheme();np_show();for(int i=0;i<40;i++)frame(16);
            if(lyr_visible())lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);
            struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);for(int i=0;i<20;i++)np_glow_refresh(true);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            printf("perf np cover: glass %.1f ms R1 est\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20e6*PF_K);
#ifdef LYR_FADE
            lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);frame(100);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);for(int i=0;i<20;i++)lyr_bd_sync(true);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            printf("perf np cover: lyrics backdrop %.1f ms R1 est (once a cover, only while the lyrics show)\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20e6*PF_K);
            lv_obj_send_event(lyr_btn,LV_EVENT_CLICKED,NULL);frame(100);
            global_settings.sloppy_np_style=2;now_playing_retheme();frame(100);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);for(int i=0;i<20;i++)nc_sync(true);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            printf("perf np cover: centred backdrop and cover %.1f ms R1 est\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20e6*PF_K);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);for(int i=0;i<20;i++)np_glow_refresh(true);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            printf("perf np cover: centred, all of it %.1f ms R1 est\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20e6*PF_K);
#endif
            np_hide(0);for(int i=0;i<30;i++)frame(16);
        }
        global_settings.sloppy_np_style=keep_np;now_playing_retheme();test_real_art=ra;
        }
        if(!only||!strcmp(only,"files")){   /* Files on a folder of 2,000 (200 folders, 1,800 songs): opened, scrolled, closed; the build timed apart, first page and last */
        double build=0,last=0;int builds=0;
        pf_begin("files 2000 open");
        for(int r=0;r<5*reps;r++){struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);sl_files_page("/Huge",0);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            build+=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;builds++;
            for(int i=0;i<35;i++)pf_frame(16);
            lv_obj_t *fb=lv_obj_get_child(pages[npages-1],1);
            for(int i=0;i<40;i++){lv_obj_scroll_by(fb,0,-48,LV_ANIM_OFF);pf_frame(16);}
            page_pop();for(int i=0;i<20;i++)pf_frame(16);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);sl_files_page("/Huge",1944);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            last+=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;
            for(int i=0;i<35;i++)pf_frame(16);
            page_pop();for(int i=0;i<20;i++)pf_frame(16);}
        pf_end();
        printf("perf files build: %.2f ms first page, %.2f ms last page, host CPU per open (x%.0f on the R1 by PF_K)\n",build/builds,last/builds,(double)PF_K);
        }
        if(!only||!strcmp(only,"settings")){   /* Settings opened, scrolled to the end and closed; its build timed apart */
        double build=0;int builds=0;
        global_settings.sloppy_settings_tiles=getenv("SLOPPY_PERF_TILES")!=NULL;   /* SLOPPY_PERF_TILES=1: the hub as tiles */
        pf_begin("settings open");
        for(int r=0;r<5*reps;r++){struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);system_page();clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            build+=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;builds++;
            for(int i=0;i<35;i++)pf_frame(16);
            lv_obj_t *sb=lv_obj_get_child(pages[npages-1],1);
            for(int i=0;i<40;i++){lv_obj_scroll_by(sb,0,-24,LV_ANIM_OFF);pf_frame(16);}
            page_pop();for(int i=0;i<20;i++)pf_frame(16);}
        pf_end();
        printf("perf settings build: %.2f ms host CPU per open (x%.0f on the R1 by PF_K)\n",build/builds,(double)PF_K);
        global_settings.sloppy_settings_tiles=0;
        }
        if(!only||!strcmp(only,"subpages")){   /* the Settings pages with ways on, each built in List and in Tiles */
        static void (*const sp[])(void)={sound_page,play_settings_page,advanced_page,time_power_page,keyboards_page,eq_page,scr_page,sl_wifi_page,sl_bt_page};
        for(int tl=0;tl<2;tl++){double build=0;int builds=0;
            global_settings.sloppy_settings_tiles=tl;
            for(int r=0;r<5*reps;r++)for(unsigned k=0;k<sizeof sp/sizeof sp[0];k++){struct timespec a,b;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);sp[k]();clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
                build+=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;builds++;
                for(int i=0;i<12;i++)pf_frame(16);
                page_pop();for(int i=0;i<12;i++)pf_frame(16);}
            printf("perf settings subpages build (%s): %.3f ms host CPU per open, %d opens\n",tl?"tiles":"list",build/builds,builds);}
        global_settings.sloppy_settings_tiles=0;
        }
        if(!only||!strcmp(only,"eq")){   /* the Equalizer opened, scrolled and closed; a band dragged; the curve applied, timed apart */
        double build=0,apply=0;int builds=0,applies=0;
        pf_begin("eq open");
        for(int r=0;r<5*reps;r++){struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);eq_page();clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            build+=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;builds++;
            for(int i=0;i<35;i++)pf_frame(16);
            for(int i=0;i<24;i++){clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);eq_set(3+(i&3),(i%9)-4);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
                apply+=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;applies++;pf_frame(16);}
            lv_obj_t *sb=lv_obj_get_child(pages[npages-1],1);
            for(int i=0;i<30;i++){lv_obj_scroll_by(sb,0,-24,LV_ANIM_OFF);pf_frame(16);}
            page_pop();for(int i=0;i<20;i++)pf_frame(16);}
        pf_end();
        for(int i=0;i<EQ_NUM_BANDS;i++)global_settings.eq_band_settings[i].gain=0;
        printf("perf eq build: %.2f ms host CPU per open; a band moved: %.3f ms host CPU (x%.0f on the R1 by PF_K)\n",build/builds,apply/applies,(double)PF_K);
        }
        if(!only||!strcmp(only,"music")){   /* the Music page opened, scrolled to its end and closed; its build timed apart */
        double build=0;int builds=0;
        pf_begin("music open");
        for(int r=0;r<5*reps;r++){struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);music_page();clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            build+=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;builds++;
            for(int i=0;i<35;i++)pf_frame(16);
            lv_obj_t *sb=lv_obj_get_child(pages[npages-1],1);
            for(int i=0;i<20;i++){lv_obj_scroll_by(sb,0,-12,LV_ANIM_OFF);pf_frame(16);}
            page_pop();for(int i=0;i<20;i++)pf_frame(16);}
        pf_end();
        printf("perf music build: %.2f ms host CPU per open (x%.0f on the R1 by PF_K)\n",build/builds,(double)PF_K);
        }
        if(!only||!strcmp(only,"wifi")){   /* Wi-Fi on and joined: its page opened, scrolled to its end and closed */
        double build=0;int builds=0;
        global_settings.sloppy_wifi=1;wf_arg_on=true;wf_begin(WF_JOB_POWER);while(wf_busy)pf_frame(16);
        wf_begin(WF_JOB_SCAN);while(wf_busy)pf_frame(16);
        pf_begin("wifi open");
        for(int r=0;r<5*reps;r++){struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);sl_wifi_page();clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            build+=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;builds++;
            for(int i=0;i<35;i++)pf_frame(16);
            lv_obj_t *sb=lv_obj_get_child(pages[npages-1],1);
            for(int i=0;i<20;i++){lv_obj_scroll_by(sb,0,-12,LV_ANIM_OFF);pf_frame(16);}
            page_pop();for(int i=0;i<20;i++)pf_frame(16);while(wf_busy)pf_frame(16);}
        pf_end();
        printf("perf wifi build: %.2f ms host CPU per open (x%.0f on the R1 by PF_K)\n",build/builds,(double)PF_K);
        global_settings.sloppy_wifi=0;wf_arg_on=false;wf_begin(WF_JOB_POWER);while(wf_busy)pf_frame(16);
        }
        if(!only||!strcmp(only,"cc")){
        pf_begin("cc pull+open");
        for(int r=0;r<5*reps;r++){
            t_x=240;t_y=4;t_down=true;pf_frame(16);
            for(int y=4;y<=604;y+=30){t_y=y;pf_frame(16);}
            t_down=false;pf_frame(16);for(int i=0;i<25;i++)pf_frame(16);
            if(r==0){pf_end();pf_begin("cc close");}
            t_x=240;t_y=H-20;t_down=true;pf_frame(16);   /* from the bottom edge: the body scrolls the list */
            for(int y=H-60;y>=260;y-=40){t_y=y;pf_frame(16);}
            t_down=false;pf_frame(16);for(int i=0;i<25;i++)pf_frame(16);
            if(r==0){pf_end();pf_begin("cc repeat");}
        }
        pf_end();assert(!qp_open);}
        if(!only||!strcmp(only,"ccx"))cc_expand_perf(reps);   /* the control centre, pulled further */
        if(!only||!strcmp(only,"homein")){   /* the Tiles home arriving: its tiles rush in, staggered */
        pf_begin("home intro");
        for(int r=0;r<reps/4+1;r++){home_intro_ms=60;appearance_reload((void*)2);for(int i=0;i<70;i++)pf_frame(16);home_intro_ms=0;while(npages>1){page_pop();frame(400);}}
        pf_end();devhud_show(true);}
        if(!only||!strcmp(only,"home")){
        pf_begin("home scroll");
        for(int r=0;r<5*reps;r++)for(int dir=0;dir<2;dir++){
            int y0=dir?220:700;t_x=200;t_y=y0;t_down=true;pf_frame(16);
            for(int i=1;i<=20;i++){t_y=y0+(dir?24:-24)*i;pf_frame(16);}
            t_down=false;pf_frame(16);for(int i=0;i<60;i++)pf_frame(16);
        }
        pf_end();}
        /* ---- taste-swipe: the Your taste deck, dragged, flung and thrown by its buttons ---- */
        if(!only||!strcmp(only,"taste")){
        lib_state=SL_LIB_READY;int rated0=ts.n_rated;ts.n_rated=0;
        taste_page(false);for(int i=0;i<60;i++)frame(16);
        lv_area_t ca;lv_obj_get_coords(td_slot_of(td.at)->img,&ca);int cx=(ca.x1+ca.x2)/2,cy=(ca.y1+ca.y2)/2;
        pf_begin("taste drag");
        for(int r=0;r<3*reps;r++){   /* slow out past the line and back, let go: springs back */
            t_x=cx;t_y=cy;t_down=true;pf_frame(16);
            for(int i=1;i<=40;i++){t_x=cx+i*4;t_y=cy+i/4;pf_frame(16);}
            for(int i=39;i>=0;i--){t_x=cx+i*4;pf_frame(16);}
            t_down=false;pf_frame(16);for(int i=0;i<40;i++)pf_frame(16);
        }
        pf_end();
        pf_begin("taste fling");
        for(int r=0;r<6*reps;r++){
            if(td.at>=td.n-1){ts.n_rated=0;td_finish();td_more_cb(NULL);for(int i=0;i<30;i++)frame(16);}
            int d=r&1?-1:1;t_x=cx;t_y=cy;t_down=true;pf_frame(16);
            for(int i=1;i<=4;i++){t_x=cx+d*i*25;t_y=cy-i*3;pf_frame(16);}
            t_down=false;pf_frame(16);for(int i=0;i<40;i++)pf_frame(16);
        }
        pf_end();
        pf_begin("taste button");
        for(int r=0;r<6*reps;r++){
            if(td.at>=td.n-1){ts.n_rated=0;td_finish();td_more_cb(NULL);for(int i=0;i<30;i++)frame(16);}
            lv_area_t b;lv_obj_get_coords(td.btn[r&1],&b);
            t_x=(b.x1+b.x2)/2;t_y=(b.y1+b.y2)/2;t_down=true;pf_frame(16);pf_frame(16);t_down=false;
            for(int i=0;i<40;i++)pf_frame(16);
        }
        pf_end();
        {   /* a card's picture, made as one comes up under the stack */
            struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);
            int pi=td.at+2<td.n?td.at+2:td.at;
            for(int i=0;i<20;i++)td_slot_make(pi);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            printf("perf taste picture   R1 est %.1f ms\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20e6*PF_K);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);
            for(int i=0;i<20;i++)td_scale_into(td_slot_of(pi)->buf,td.step[0],338-i%4*2,328-i%4*2);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);td.step_card=-1;
            printf("perf taste step copy R1 est %.1f ms\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20e6*PF_K);
            td_restack();td_layout();
        }
        pf_begin("taste love");   /* slowly up past the line, let go: flies off the top */
        for(int r=0;r<6*reps;r++){
            if(td.at>=td.n-1){ts.n_rated=0;ts.gen++;td_finish();td_more_cb(NULL);for(int i=0;i<30;i++)frame(16);}
            t_x=cx;t_y=cy;t_down=true;pf_frame(16);
            for(int i=1;i<=20;i++){t_y=cy-i*8;t_x=cx+i/3;pf_frame(16);}
            for(int i=0;i<6;i++)pf_frame(16);
            t_down=false;pf_frame(16);for(int i=0;i<40;i++)pf_frame(16);
        }
        pf_end();
        {   /* the model on a 12000-song library: worked out, 300 swipes in, a card picked, Picked for you */
            struct sl_lib keep=sl_lib;int na=300,nt=12000;
            struct sl_track *bt=calloc(nt,sizeof *bt);struct sl_album *ba=calloc(na,sizeof *ba);int *btr=malloc(nt*sizeof *btr);char *bp=malloc(nt*32);
            static char bar[300][16],bal[300][16];
            for(int a=0;a<na;a++){snprintf(bar[a],16,"Artist %d",a/3);snprintf(bal[a],16,"LP %d",a);ba[a].title=bal[a];ba[a].artist=bar[a];
                ba[a].genre=a%5?"Rock":"Jazz";ba[a].year=1960+a%60;ba[a].n_tracks=40;ba[a].tracks=btr+a*40;ba[a].hue=a;}
            for(int t=0;t<nt;t++){btr[t]=t;snprintf(bp+t*32,32,"/Music/big/%d.flac",t);tpath(&bt[t],bp+t*32);bt[t].title=bp+t*32;bt[t].album=t/40;bt[t].len=200000;}
            sl_lib.tracks=bt;sl_lib.n_tracks=nt;sl_lib.albums=ba;sl_lib.n_albums=na;
            int nr0=ts.n_rated;ts.n_rated=0;ts.gen++;
            struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);tm_ready();clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            printf("perf taste model     R1 est %.1f ms (worked out again: a 12000-song library, once a library or outside change)\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/1e6*PF_K);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);for(int i=0;i<300;i++)ts_swipe((i*37)%nt,i%3?'+':'-');clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            printf("perf taste swipe in  R1 est %.3f ms\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/300e6*PF_K);
            int d0=td.dealt,n0=td.n;td.dealt=0;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);for(int i=0;i<20;i++){uint8_t w;uint32_t ak;td_pick(&w,&ak);}clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            printf("perf taste pick      R1 est %.2f ms (the next card, as the last one lands)\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20e6*PF_K);
            td.dealt=d0;td.n=n0;
            int *l=NULL;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);int n=ts_picks(&l);clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);free(l);
            printf("perf taste picked    R1 est %.1f ms (Picked for you: %d songs)\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/1e6*PF_K,n);
            ts.n_rated=nr0;ts.gen++;sl_lib=keep;free(bt);free(ba);free(btr);free(bp);
        }
        page_pop();for(int i=0;i<50;i++)frame(16);ts.n_rated=rated0;}
        /* ---- end taste-swipe ---- */
        if(!only||!strcmp(only,"magic")){
        magic_page();for(int i=0;i<50;i++)frame(16);
        pf_begin("magic 8 ball");
        for(int r=0;r<5*reps;r++){
            lv_area_t b;lv_obj_get_coords(magic_ball,&b);
            t_x=(b.x1+b.x2)/2;t_y=(b.y1+b.y2)/2;t_down=true;pf_frame(16);t_down=false;
            for(int i=0;i<110;i++){magic_started-=16;pf_frame(16);}   /* the shake runs on sl_ms(), wall time */
            assert(!magic_thinking);
        }
        pf_end();
        {   /* the hand-painted sky alone, mid-shake */
            magic_glow_x=5;magic_glow_y=-9;for(int j=0;j<3;j++){magic_halo_scale(j,265);magic_halo_opa[j]=80;}
                struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);for(int i=0;i<200;i++)magic_sky_paint();clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);
            printf("perf magic sky paint  R1 est %.1f ms\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/200e6*PF_K);
            magic_started=sl_ms()-100000;frame(16);
        }page_pop();for(int i=0;i<50;i++)frame(16);}
        if(!only||!strcmp(only,"power")){   /* slide to power off: slow there and back, quick partial swipes, never to the end */
        bool was_running=sl_power_running;sl_power_running=true;sl_power_suspended=false;
        sl_power_state_init(&sl_power,sl_ms());sl_power_apply();frame(100);
        pf_begin("power open");
        sl_power_filter_button(BUTTON_POWER);sl_power.power_at-=SL_POWER_HOLD_MS+50;sl_power_tick();
        for(int i=0;i<30;i++)pf_frame(16);pf_end();sl_power_filter_button(BUTTON_POWER|BUTTON_REL);for(int i=0;i<60;i++)frame(16);
        assert(po_is_open());int offs=poweroffs;
        pf_begin("power slide");
        for(int r=0;r<3*reps;r++){
            lv_area_t k;lv_obj_get_coords(po.knob,&k);int kx=(k.x1+k.x2)/2,ky=(k.y1+k.y2)/2;
            t_x=kx;t_y=ky;t_down=true;pf_frame(16);
            for(int i=1;i<=60;i++){t_x=kx+i*5;pf_frame(16);}          /* slow, to 89 % */
            for(int i=59;i>=0;i--){t_x=kx+i*5;pf_frame(16);}          /* and back */
            t_down=false;pf_frame(16);for(int i=0;i<20;i++)pf_frame(16);
            t_x=kx;t_down=true;pf_frame(16);
            for(int i=1;i<=8;i++){t_x=kx+i*32;pf_frame(16);}          /* a quick swipe, let go short */
            t_down=false;pf_frame(16);for(int i=0;i<40;i++)pf_frame(16);
        }
        pf_end();assert(poweroffs==offs && po_is_open());
        pf_begin("power close");po_close();for(int i=0;i<30;i++)pf_frame(16);pf_end();assert(!po_is_open());sl_power_running=was_running;for(int i=0;i<30;i++)frame(16);}
        if(!only||!strcmp(only,"list")){   /* the list home, scrolled */
        global_settings.sloppy_home_style=2;appearance_reload(NULL);frame(700);frame(700);
        for(int i=0;i<60;i++)frame(16);
        pf_begin("list scroll");
        for(int r=0;r<5*reps;r++)for(int dir=0;dir<2;dir++){
            int y0=dir?220:700;t_x=200;t_y=y0;t_down=true;pf_frame(16);
            for(int i=1;i<=20;i++){t_y=y0+(dir?24:-24)*i;pf_frame(16);}
            t_down=false;pf_frame(16);for(int i=0;i<60;i++)pf_frame(16);
        }
        pf_end();}
        if(!only||!strcmp(only,"songs")){   /* the All list, 10,000 songs, flicked down and back */
        while(npages>1){page_pop();frame(400);}
        songs_page();for(int i=0;i<60;i++)pf_frame(16);
        pf_begin("songs scroll");
        for(int r=0;r<5*reps;r++)for(int dir=0;dir<2;dir++){
            int y0=dir?220:700;t_x=200;t_y=y0;t_down=true;pf_frame(16);
            for(int i=1;i<=20;i++){t_y=y0+(dir?24:-24)*i;pf_frame(16);}
            t_down=false;pf_frame(16);for(int i=0;i<60;i++)pf_frame(16);
        }
        pf_end();while(npages>1){page_pop();for(int i=0;i<20;i++)frame(16);}}
        if(!only||!strcmp(only,"albums")){   /* the album grid, 600 albums, flicked down and back */
        static struct sl_track ptr[2400];static struct sl_album pal[600];static int patr[2400],pord[600];static char pti[600][24];
        struct sl_lib keep=sl_lib;
        for(int a=0;a<600;a++){snprintf(pti[a],sizeof pti[a],"Album %03d",a);
            pal[a]=(struct sl_album){pti[a],a%7?"Mira Sol":"Neon Harbor",1990+a%30,4,(a*37)%360,patr+a*4,"Pop"};pord[a]=a;
            for(int k=0;k<4;k++){patr[a*4+k]=a*4+k;ptr[a*4+k]=(struct sl_track){"/Music/p.flac",pti[a],a,k+1,1,200000,(uint32_t)(a*4+k),true,NULL};tsplit(&ptr[a*4+k]);}}
        sl_lib.tracks=ptr;sl_lib.n_tracks=2400;sl_lib.albums=pal;sl_lib.n_albums=600;sl_lib.albums_by_title=pord;sl_lib.albums_by_artist=pord;
        sl_lib.album_hash=NULL;sl_lib.album_hash_cap=0;
        while(npages>1){page_pop();frame(400);}
        albums_page();for(int i=0;i<60;i++)pf_frame(16);
        pf_begin("albums scroll");
        for(int r=0;r<5*reps;r++)for(int dir=0;dir<2;dir++){
            int y0=dir?220:700;t_x=200;t_y=y0;t_down=true;pf_frame(16);
            for(int i=1;i<=20;i++){t_y=y0+(dir?24:-24)*i;pf_frame(16);}
            t_down=false;pf_frame(16);for(int i=0;i<60;i++)pf_frame(16);
        }
        pf_end();while(npages>1){page_pop();for(int i=0;i<20;i++)frame(16);}
        sl_lib=keep;}
        if(!only||!strcmp(only,"artists")){   /* batch C: 10,000 albums, the Artists list and a genre of 4,000 albums, flicked down and back */
        c_scale_lib();
        static const char *const pn[]={"artists scroll","genre scroll","album artists"};
        for(int which=0;which<3;which++){
            while(npages>1){page_pop();frame(400);}
            if(which==0)tartists_page();else if(which==1){int j=0;assert(tg_ready());while(strcmp(TG_GNAME(j),"Jazz"))j++;group_page(0x30000|j);}else artists_page();
            for(int i=0;i<60;i++)pf_frame(16);
            pf_begin(pn[which]);
            for(int r=0;r<5*reps;r++)for(int dir=0;dir<2;dir++){
                int y0=dir?220:700;t_x=200;t_y=y0;t_down=true;pf_frame(16);
                for(int i=1;i<=20;i++){t_y=y0+(dir?24:-24)*i;pf_frame(16);}
                t_down=false;pf_frame(16);for(int i=0;i<60;i++)pf_frame(16);
            }
            pf_end();
        }
        while(npages>1){page_pop();for(int i=0;i<20;i++)frame(16);}
        c_scale_done();}
        if(!only||!strcmp(only,"visualizer")){   /* the visualizer, look by look, music playing */
        int feed=pf_feed;pf_feed=1;
        for(int half=0;half<2;half++){   /* the full picture, then the half one a slow player falls back to */
        vz_low=half;vz_open();for(int i=0;i<30;i++)pf_frame(16);
        for(int l=0;l<VZ_LOOKS;l++){
            vz_go(l,0);vz.next_ms=lv_tick_get()+600000;for(int i=0;i<20;i++)pf_frame(16);
            struct cfm_hist w0=vz.work,p0=vz.paint;
            char nm[40];snprintf(nm,sizeof nm,"vz%s %s",half?"/2":"",vz_looks[l].name);pf_begin(nm);
            for(int r=0;r<reps;r++)for(int i=0;i<60;i++)pf_frame(16);
            pf_end();
            /* drawn in place on the R1: no rotation or strips, only the copy to the panel */
            double wk=(double)(vz.work.sum-w0.sum)/(vz.work.n-w0.n),pt=(double)(vz.paint.sum-p0.sum)/(vz.paint.n-p0.n);
            printf("perf %-14s Mac work %4.0f us paint %4.0f us; R1 est work %4.1f ms + paint %4.1f ms + present 2.2 ms\n",
                   nm,wk,pt,wk*PF_K/1000,pt*PF_K/1000);
        }
        {   /* a finger stirring round, then holding: what playing with it adds */
            vz_go(0,0);for(int i=0;i<20;i++)pf_frame(16);
            struct cfm_hist w0=vz.work;char nm[40];snprintf(nm,sizeof nm,"vz%s finger",half?"/2":"");pf_begin(nm);
            for(int r=0;r<reps;r++){for(int i=0;i<90;i++){t_x=240+(int)(140*cosf(i*0.2f));t_y=450+(int)(140*sinf(i*0.2f));t_down=true;pf_frame(16);}
                for(int i=0;i<60;i++){t_x=240;t_y=300;pf_frame(16);}t_down=false;for(int i=0;i<30;i++)pf_frame(16);}
            pf_end();
            printf("perf %-14s Mac work %4.0f us; R1 est work %4.1f ms\n",nm,(double)(vz.work.sum-w0.sum)/(vz.work.n-w0.n),
                   (double)(vz.work.sum-w0.sum)/(vz.work.n-w0.n)*PF_K/1000);
        }
        vz_close();for(int i=0;i<30;i++)frame(16);
        }
        vz_low=false;pf_feed=feed;
        }
        if(!only||!strcmp(only,"spacey")){   /* Spacey Lines: a finger swirling, in each tunnel */
        spacey_page();for(int i=0;i<30;i++)pf_frame(16);
        lv_area_t sa;lv_obj_get_coords(spc.stage,&sa);int cx=(sa.x1+sa.x2)/2,cy=(sa.y1+sa.y2)/2;
        for(int mode=0;mode<2;mode++){
            t_x=cx+150;t_y=cy;t_down=true;pf_frame(16);   /* a touch: odd, then even */
            for(int i=0;i<30;i++){t_x=cx+(int)(150*cosf(i*0.15f));t_y=cy+(int)(220*sinf(i*0.15f));pf_frame(16);}
            struct cfm_hist w0=spc.work,p0=spc.paint;char nm[40];snprintf(nm,sizeof nm,"spacey %s",spc.clicks&1?"odd":"even");pf_begin(nm);
            for(int r=0;r<reps;r++)for(int i=0;i<120;i++){t_x=cx+(int)(150*cosf(i*0.15f));t_y=cy+(int)(220*sinf(i*0.15f));pf_frame(16);}
            pf_end();
            double wk=(double)(spc.work.sum-w0.sum)/(spc.work.n-w0.n),pt=(double)(spc.paint.sum-p0.sum)/(spc.paint.n-p0.n);
            printf("perf %-14s Mac work %4.0f us paint %4.0f us; R1 est work %4.1f ms + paint %4.1f ms\n",nm,wk,pt,wk*PF_K/1000,pt*PF_K/1000);
            t_down=false;pf_frame(16);
        }
        {   /* let go: the trail gathers to a dot and keeps feeding the tunnel */
            struct cfm_hist w0=spc.work;pf_begin("spacey idle");
            for(int r=0;r<reps;r++)for(int i=0;i<120;i++)pf_frame(16);
            pf_end();
            printf("perf %-14s Mac work %4.0f us; R1 est work %4.1f ms\n","spacey idle",(double)(spc.work.sum-w0.sum)/(spc.work.n-w0.n),
                   (double)(spc.work.sum-w0.sum)/(spc.work.n-w0.n)*PF_K/1000);
        }
        page_pop();for(int i=0;i<30;i++)frame(16);
        }
        spoon_page();for(int i=0;i<50;i++)frame(16);
        pf_begin("spoon (ref)");
        { lv_area_t pa;lv_obj_get_coords(spn_pad,&pa);
          for(int i=0;i<300;i++){t_x=(i/40)&1?pa.x2-40:pa.x1+40;t_y=(pa.y1+pa.y2)/2;t_down=true;pf_frame(16);} t_down=false;pf_frame(16); }
        pf_end();page_pop();for(int i=0;i<50;i++)frame(16);
        return 0;   /* a measuring run: the tests after it expect the default home and text size */
    }
    if (getenv("SLOPPY_BENCH")) {   /* SLOPPY_BENCH=1 ./ui-test: full redraw and live frame cost per screen */
        while(npages>1){page_pop();frame(400);}
        sl_power_state_init(&sl_power,sl_ms());sl_power_apply();frame(400);   /* awake and unlocked */
        printf("bench: invalidation %s, power mode %d\n",lv_display_is_invalidation_enabled(disp)?"on":"OFF",sl_power.mode);
        {   /* Cover Flow: the renderer alone, at rest and between two albums */
            cf_open();for(int i=0;i<12;i++)frame(100);
            int32_t rests[]={CF_ONE*5,CF_ONE*5+CF_ONE/2,CF_ONE*5+CF_ONE/5};
            for(unsigned r=0;r<3;r++){cf.pos=rests[r];
                struct timespec a,b;cf_px_written=0;cf_render();uint32_t px=cf_px_written;
                clock_gettime(CLOCK_MONOTONIC,&a);for(int i=0;i<300;i++)cf_render();clock_gettime(CLOCK_MONOTONIC,&b);
                printf("bench cover flow pos %.1f: %.0f us per render, %lu px written (%.2fx the %d px canvas)\n",
                       rests[r]/(float)CF_ONE,((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/300e3,(unsigned long)px,px/(float)(CF_W*CF_H),CF_W*CF_H);}
            {   /* a whole moving frame: render, LVGL's canvas copy, rotation, flush */
                struct timespec a,b;uint64_t ren=0;
                clock_gettime(CLOCK_MONOTONIC,&a);
                for(int i=0;i<200;i++){cf.pos=CF_ONE*5+(i%10)*CF_ONE/10;cf.dirty=true;
                    struct timespec r0,r1;clock_gettime(CLOCK_MONOTONIC,&r0);cf_render();clock_gettime(CLOCK_MONOTONIC,&r1);
                    ren+=(r1.tv_sec-r0.tv_sec)*1000000000ull+(r1.tv_nsec-r0.tv_nsec);
                    lv_obj_invalidate(cf.canvas);lv_refr_now(disp);}
                clock_gettime(CLOCK_MONOTONIC,&b);
                double tot=((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/200e3;
                printf("bench cover flow frame: %.0f us total, %.0f us render, %.0f us LVGL draw+rotate+flush\n",tot,ren/200e3,tot-ren/200e3);
            }
            cf_close();frame(400);
        }
        struct { const char *name; void (*open)(void); } screens[] = {
            {"home",NULL},{"music",music_page},{"albums",albums_page},{"songs",songs_page},{"artists",artists_page},
            {"for you",foryou_page},{"search",search_page},{"pet",pet_page},{"snake",snake_page},{"radio",radio_page},
            {"coin",coin_page},{"chess",chess_page},{"settings",system_page},{"quiz",quiz_page},
            {"crossfade",play_settings_page},{"magic",magic_page},{"spoon",spoon_page},{"ragdoll",ragdoll_page} };
        for(unsigned k=0;k<sizeof screens/sizeof screens[0];k++){
            if(screens[k].open){screens[k].open();}
            frame(700);frame(700);
            struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
            for(int i=0;i<20;i++){lv_obj_invalidate(lv_screen_active());lv_refr_now(disp);}
            clock_gettime(CLOCK_MONOTONIC,&b);
            double full=((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20/1e6;
            clock_gettime(CLOCK_MONOTONIC,&a);
            for(int i=0;i<60;i++)frame(16);
            clock_gettime(CLOCK_MONOTONIC,&b);
            double live=((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/60/1e6;
            printf("bench %-10s full redraw %6.2f ms   idle frame %6.3f ms\n",screens[k].name,full,live);
            while(npages>1){page_pop();frame(400);}
        }
        /* idle home, tiles style with the live pet tile: what still gets redrawn */
        for(int style=0;style<2;style++){
            global_settings.sloppy_home_style=style;appearance_reload(NULL);frame(700);frame(700);
            flushed_px=0;for(int i=0;i<313;i++)frame(16);           /* ~5 s */
            printf("bench home %-5s idle: %ld pixels/s flushed (%.1f screens/s)\n",style?"tiles":"cards",
                   flushed_px/5,flushed_px/5.0/(W*H));
        }
        {   /* the 8 ball shake: every frame moves the ball, its glow and the halos */
            magic_page();frame(700);magic_ask_cb(NULL);frame(16);
            struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
            for(int i=0;i<30;i++)frame(16);
            clock_gettime(CLOCK_MONOTONIC,&b);
            printf("bench %-10s shake frame %6.2f ms\n","magic",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/30/1e6);
            magic_started=sl_ms()-100000;frame(100);page_pop();frame(500);
        }
        for(int style=0;style<2;style++){   /* pulling the control centre down, finger-driven frames */
            global_settings.sloppy_home_style=style;appearance_reload(NULL);frame(700);frame(700);
            struct timespec a,b;
            clock_gettime(CLOCK_MONOTONIC,&a);qp_drag_to(20);frame(16);clock_gettime(CLOCK_MONOTONIC,&b);
            double first=((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/1e6;
            clock_gettime(CLOCK_MONOTONIC,&a);
            for(int i=1;i<=40;i++){qp_drag_to(20+i*18);frame(16);}
            clock_gettime(CLOCK_MONOTONIC,&b);
            printf("bench pull %-5s first frame %6.2f ms, drag frame %6.2f ms\n",style?"tiles":"cards",first,
                   ((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/40/1e6);
            qp_release(740,0);frame(400);qp_close();frame(400);
            clock_gettime(CLOCK_MONOTONIC,&a);for(int i=0;i<20;i++)qp_glass_capture();clock_gettime(CLOCK_MONOTONIC,&b);
            printf("bench pull glass capture %6.2f ms\n",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20/1e6);
            qp_drag_to(20);frame(16);
            for(int pass=0;pass<2;pass++){
                if(pass)for(uint32_t c=0;c<lv_obj_get_child_count(qp);c++)lv_obj_add_flag(lv_obj_get_child(qp,c),LV_OBJ_FLAG_HIDDEN);
                clock_gettime(CLOCK_MONOTONIC,&a);
                for(int i=1;i<=40;i++){qp_drag_to(20+i*18);frame(16);}
                clock_gettime(CLOCK_MONOTONIC,&b);
                printf("bench pull %s drag frame %6.2f ms\n",pass?"glass only":"full panel",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/40/1e6);
                qp_drag_to(20);frame(16);
            }
            qp_release(0,0);frame(400);quick_panel_rebuild();frame(100);
        }
        {   /* Cover Flow: one full render of the row while it moves */
            cf_open();frame(900);
            struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
            for(int i=0;i<40;i++){cf.pos=CF_ONE*3+i*CF_ONE/10;cf_render();}
            clock_gettime(CLOCK_MONOTONIC,&b);
            printf("bench %-10s render %6.2f ms\n","coverflow",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/40/1e6);
            cf_close();frame(300);
        }
        {   /* the new pages: building them, and an idle second on each */
            struct timespec a,b;
            clock_gettime(CLOCK_MONOTONIC,&a);grave_page();clock_gettime(CLOCK_MONOTONIC,&b);
            printf("bench %-10s open %6.2f ms\n","graveyard",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/1e6);
            frame(700);flushed_px=0;clock_gettime(CLOCK_MONOTONIC,&a);for(int i=0;i<60;i++)frame(16);clock_gettime(CLOCK_MONOTONIC,&b);
            printf("bench %-10s idle frame %6.3f ms, %ld px/s flushed\n","graveyard",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/60/1e6,flushed_px);
            page_pop();frame(500);
            test_status=AUDIO_STATUS_PLAY;global_settings.sloppy_np_vinyl=1;
            clock_gettime(CLOCK_MONOTONIC,&a);np_show();clock_gettime(CLOCK_MONOTONIC,&b);
            printf("bench %-10s open %6.2f ms\n","vinyl",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/1e6);
            for(int i=0;i<90;i++){vnl_last_ms=sl_ms()-33;frame(33);}flushed_px=0;for(int i=0;i<60;i++){vnl_last_ms=sl_ms()-16;frame(16);}
            printf("bench %-10s %ld px/s flushed while spinning\n","vinyl",flushed_px);
            global_settings.sloppy_np_vinyl=0;vnl_apply(np,false);np_hide(0);frame(500);test_status=0;
        }
        {   /* vinyl: one frame of the spinning label and the arm */
            test_status=AUDIO_STATUS_PLAY;global_settings.sloppy_np_vinyl=1;np_show();frame(700);
            struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
            for(int i=0;i<30;i++){vnl_last_ms=sl_ms()-33;frame(33);}
            clock_gettime(CLOCK_MONOTONIC,&b);
            printf("bench %-10s spin frame %6.2f ms\n","vinyl",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/30/1e6);
            global_settings.sloppy_np_vinyl=0;vnl_apply(np,false);np_hide(0);frame(500);
        }
        {   /* home tiles: one live tile peeking, and one cover mosaic swapping */
            global_settings.sloppy_home_style=1;appearance_reload(NULL);frame(700);frame(700);
            for(int kind=0;kind<2;kind++){
                int k;for(k=0;k<mt.n;k++){struct mt_slot *sl=&mt.slot[k];
                    if(!mt_on_screen(sl->obj))continue;
                    if(kind==0&&sl->faces&&mt_back(sl)){mt_peek(sl->faces,mt_px(mt_span_h(sl->size)));break;}
                    if(kind==1&&!sl->faces&&sl->back){mt_mosaic_swap(sl->back);break;}}
                if(k==mt.n){printf("bench tiles: no %s on screen\n",kind?"mosaic":"live tile");continue;}
                flushed_px=0;struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
                for(int i=0;i<44;i++)frame(16);
                clock_gettime(CLOCK_MONOTONIC,&b);
                printf("bench tiles %-7s %6.3f ms per frame, %ld px flushed per animation\n",kind?"mosaic":"peek",
                       ((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/44/1e6,flushed_px);
                frame(3000);
            }
        }
        {   /* home tiles over each wallpaper: a full redraw, a scroll frame, a tile peeking */
            static const int wps[]={0,1,2,3,4,5,6,7,8};
            for(unsigned w=0;w<sizeof wps/sizeof wps[0];w++){
                global_settings.sloppy_home_style=1;global_settings.sloppy_home_wallpaper=wps[w];
                appearance_reload(NULL);frame(700);frame(700);
                if(np_open){np_hide(0);frame(500);}while(npages>1){page_pop();frame(400);}frame(700);
                struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
                for(int i=0;i<20;i++){lv_obj_invalidate(lv_screen_active());lv_refr_now(disp);}
                clock_gettime(CLOCK_MONOTONIC,&b);double full=((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20/1e6;
                touch_at(240,500,true);touch_at(240,480,true);touch_at(240,460,true);
                clock_gettime(CLOCK_MONOTONIC,&a);
                for(int i=0;i<30;i++)touch_at(240,460-(i%15)*12,true);
                clock_gettime(CLOCK_MONOTONIC,&b);double drag=((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/30/1e6;
                touch_at(240,300,false);for(int i=0;i<30;i++)frame(40);
                lv_obj_scroll_to_y(lv_obj_get_child(pages[0],0),0,LV_ANIM_OFF);frame(100);   /* home was rebuilt: metro_body is gone */
                double peek=-1;
                for(int k=0;k<mt.n;k++){struct mt_slot *sl=&mt.slot[k];
                    if(!mt_on_screen(sl->obj)||!sl->faces||!mt_back(sl))continue;
                    mt_peek(sl->faces,mt_px(mt_span_h(sl->size)));
                    clock_gettime(CLOCK_MONOTONIC,&a);for(int i=0;i<44;i++)frame(16);clock_gettime(CLOCK_MONOTONIC,&b);
                    peek=((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/44/1e6;frame(3000);break;}
                printf("bench tiles wallpaper %d: full redraw %6.2f ms, scroll frame %6.2f ms, peek frame %6.3f ms\n",
                       wps[w],full,drag,peek);
            }
            global_settings.sloppy_home_wallpaper=0;
        }
        global_settings.sloppy_home_style=0;appearance_reload(NULL);frame(700);
        np_show();frame(700);
        struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
        for(int i=0;i<20;i++){lv_obj_invalidate(lv_screen_active());lv_refr_now(disp);}
        clock_gettime(CLOCK_MONOTONIC,&b);
        printf("bench %-10s full redraw %6.2f ms\n","now play",((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/20/1e6);
        np_hide(0);frame(500);
    }
    { /* Settings added for the missing stock R1 features */
        sound_page();frame(500);shot("settings-sound.ppm");
        assert(find_label(pages[npages-1],"REPLAYGAIN")&&find_label(pages[npages-1],"Fixed power-on volume"));
        {int saves=status_saves;speed_set(150);assert(global_status.resume_speed==150*PITCH_SPEED_PRECISION&&status_saves>saves);}
        char spd[16];assert(!strcmp(qp_tile_label(10,spd,sizeof spd),"1.5x"));
        speed_set(200);assert(!strcmp(qp_tile_label(10,spd,sizeof spd),"2x"));
        speed_set(75);assert(!strcmp(qp_tile_label(10,spd,sizeof spd),"0.75x"));
        speed_set(150);assert(speed_pct()==150);speed_next();assert(speed_pct()==200);speed_next();assert(speed_pct()==75);speed_set(100);
        rg_type(2);assert(global_settings.replaygain_settings.type==REPLAYGAIN_ALBUM);rg_type(0);
        vol_boot_fixed(1);assert(global_settings.sloppy_boot_volume==volume_pct());vol_boot_fixed(0);assert(global_settings.sloppy_boot_volume==-1);
        page_pop();frame(500);
        buttons_page();frame(500);shot("settings-buttons.ppm");page_pop();frame(500);
        time_power_page();frame(500);shot("settings-time-power.ppm");page_pop();frame(500);
        lyrics_color_page();frame(500);shot("settings-lyrics-colour.ppm");page_pop();frame(500);
        play_settings_page();frame(500);lv_obj_scroll_to_y(lv_obj_get_child(pages[npages-1],1),2000,LV_ANIM_OFF);frame(300);
        shot("settings-playback.ppm");assert(find_label(pages[npages-1],"Play through albums"));page_pop();frame(500);
        system_page();frame(500);shot("settings-root.ppm");
        {int nt=sl_lib.n_tracks,na=sl_lib.n_albums;sl_lib.n_tracks=12345;sl_lib.n_albums=1234;page_pop();frame(500);system_page();frame(500);for(lv_obj_t *o=find_label(pages[npages-1],"GabagoolOS");o;o=lv_obj_get_parent(o))lv_obj_scroll_to_y(o,0,LV_ANIM_OFF);frame(100);shot("settings-hero-big.ppm");sl_lib.n_tracks=nt;sl_lib.n_albums=na;page_pop();frame(500);system_page();frame(500);}
        lv_obj_scroll_to_y(lv_obj_get_child(pages[npages-1],1),4000,LV_ANIM_OFF);frame(300);shot("settings-root-end.ppm");
        assert(!strcmp(sl_update_footer(),"GABAGOOLOS \xc2\xb7 GAME ON THEN UPDATE"));
        assert(find_label(pages[npages-1],"HiBy R1 \xc2\xb7 Game on then"));
        assert(find_label(pages[npages-1],sl_update_footer()));page_pop();frame(500);

        /* shortcuts: up to 8 of 12, the control centre follows */
        strlcpy((char *)global_settings.sloppy_qp_tiles,"8ab",sizeof global_settings.sloppy_qp_tiles);
        if (!qp) qp_build(); else quick_panel_rebuild();
        assert(qp_n==3&&qp_fn[0]==8&&qp_fn[1]==10&&qp_fn[2]==11);
        shortcuts_page();frame(500);shot("settings-shortcuts.ppm");page_pop();frame(500);
        {   /* arranging the control centre: drag a shortcut by its handle, take one out, put one back, the usual order again */
            #define QPS (const char *)global_settings.sloppy_qp_tiles
            strlcpy((char *)global_settings.sloppy_qp_tiles,"8ab3.",sizeof global_settings.sloppy_qp_tiles);quick_panel_rebuild();
            shortcuts_page();frame(500);
            assert(scc.d.n==4&&scc.d.rows[0]&&scc.d.rows[3]&&!scc.d.rows[4]&&!lv_obj_has_flag(scc.more,LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_get_child_count(scc.more)==QP_FNS-4);
            /* the first row's handle, dragged down two rows: it lands third, the others slide up */
            lv_obj_t *hd=lv_obj_get_child(scc.d.rows[0],3);   /* name, icon, cross, handle (then a hairline) */
            lv_area_t a;lv_obj_get_coords(hd,&a);
            int saves=settings_save_calls;
            t_x=(a.x1+a.x2)/2;t_y=(a.y1+a.y2)/2;t_down=true;frame(30);
            for(int i=1;i<=10;i++){t_y=(a.y1+a.y2)/2+i*scc.d.row_h*2/10;frame(30);}
            assert(scc.d.drag&&scc.d.target==2&&lv_obj_get_style_translate_y(scc.d.rows[1],0)<=0);
            t_down=false;frame(30);frame(400);
            assert(!strcmp(QPS,"ab83.")&&settings_save_calls>saves&&!scc.d.drag);
            assert(qp_n==4&&qp_fn[0]==10&&qp_fn[1]==11&&qp_fn[2]==8&&qp_fn[3]==3);   /* the panel follows at once */
            assert(scc.d.n==4&&scc.fn[2]==8&&lv_obj_get_style_translate_y(scc.d.rows[2],0)==0);   /* drawn again in order */
            /* let go where it was picked up: nothing moves, nothing is written */
            lv_obj_get_coords(lv_obj_get_child(scc.d.rows[1],3),&a);saves=settings_save_calls;
            t_x=(a.x1+a.x2)/2;t_y=(a.y1+a.y2)/2;t_down=true;frame(30);t_y+=8;frame(30);t_down=false;frame(30);frame(400);
            assert(!strcmp(QPS,"ab83.")&&settings_save_calls==saves);
            /* the cross takes one out; it waits under MORE SHORTCUTS */
            lv_obj_t *x=lv_obj_get_child(scc.d.rows[0],2);assert(lv_event_dsc_get_cb(lv_obj_get_event_dsc(x,lv_obj_get_event_count(x)-1))==scc_remove_cb);lv_obj_send_event(x,LV_EVENT_CLICKED,NULL);frame(300);
            assert(!strcmp(QPS,"b83.")&&scc.d.n==3&&lv_obj_get_child_count(scc.more)==QP_FNS-3);
            /* a tap on one of those puts it back at the end */
            lv_obj_send_event(lv_obj_get_child(scc.more,0),LV_EVENT_CLICKED,NULL);frame(300);
            assert(!strcmp(QPS,"b830.")&&scc.d.n==4&&qp_n==4&&qp_fn[3]==0);
            /* the last one stays */
            strlcpy((char *)global_settings.sloppy_qp_tiles,"5.",sizeof global_settings.sloppy_qp_tiles);quick_panel_rebuild();
            page_pop();frame(400);shortcuts_page();frame(500);
            lv_obj_send_event(lv_obj_get_child(scc.d.rows[0],2),LV_EVENT_CLICKED,NULL);frame(300);
            assert(!strcmp(QPS,"5.")&&scc.d.n==1);
            /* the usual order again: all of them, the eight first, MORE SHORTCUTS gone */
            lv_obj_t *rs=find_label(pages[npages-1],"Reset to the usual order");while(rs&&!lv_obj_has_flag(rs,LV_OBJ_FLAG_CLICKABLE))rs=lv_obj_get_parent(rs);
            assert(rs);lv_obj_send_event(rs,LV_EVENT_CLICKED,NULL);frame(300);
            assert(!strcmp(QPS,"01234567")&&scc.d.n==QP_FNS&&qp_n==8&&qp_fn[0]==0&&qp_fn[7]==7&&lv_obj_has_flag(scc.more,LV_OBJ_FLAG_HIDDEN));
            /* the order is the setting itself: what the config keeps across a restart is read back the same */
            strlcpy((char *)global_settings.sloppy_qp_tiles,"c5a01b3e7f4.",sizeof global_settings.sloppy_qp_tiles);quick_panel_rebuild();
            {int fn[QP_FNS],n;qp_fn_parse(fn,&n);assert(n==11&&fn[0]==12&&fn[1]==5&&fn[10]==4);}
            page_pop();frame(400);shortcuts_page();frame(500);assert(scc.d.n==11&&scc.fn[0]==12&&scc.fn[10]==4);
            page_pop();frame(400);assert(!scc.d.in);
            global_settings.sloppy_qp_tiles[0]='\0';quick_panel_rebuild();
            #undef QPS
            printf("PASS: the control centre is arranged in Settings: dragged by a handle, taken out, put back, the usual order again; saved as the setting.\n");
        }
        strlcpy((char *)global_settings.sloppy_qp_tiles,"zz",sizeof global_settings.sloppy_qp_tiles);
        quick_panel_rebuild();assert(qp_n==8&&qp_fn[7]==7);   /* junk: the defaults */
        global_settings.sloppy_qp_tiles[0]='\0';quick_panel_rebuild();

        {   /* a quick double tap on a row opens its page once: the second tap
             * lands on the page sliding away underneath */
            while(npages>1){page_pop();frame(400);}
            system_page();frame(500);int n=npages;
            lv_obj_t *row=find_label(pages[npages-1],"Sound");assert(row);
            /* clear of the mini player, which a fuzz run may have left up */
            lv_obj_t *body1=lv_obj_get_child(pages[npages-1],1);lv_obj_update_layout(body1);
            {lv_area_t r0;lv_obj_get_coords(row,&r0);if(r0.y2>H-140)lv_obj_scroll_by(body1,0,-(r0.y2-(H-140)),LV_ANIM_OFF);}frame(100);
            lv_area_t a;lv_obj_get_coords(row,&a);int x=(a.x1+a.x2)/2,y=(a.y1+a.y2)/2;
            touch_at(x,y,true);touch_at(x,y,false);
            assert(npages==n+1);
            touch_at(x,y,true);touch_at(x,y,false);frame(400);
            assert(npages==n+1);
            page_pop();frame(400);page_pop();frame(400);
        }
        /* keys: the play key, and remote keys turned off */
        test_status=AUDIO_STATUS_PLAY;test_track.elapsed=0;   /* near the start: previous goes back a song */
        int nexts=audio_nexts,prevs=audio_prevs;
        global_settings.sloppy_play_key=1;handle_button(BUTTON_LEFT|BUTTON_REL);assert(audio_nexts==nexts+1);
        global_settings.sloppy_play_key=2;handle_button(BUTTON_LEFT|BUTTON_REL);assert(audio_prevs==prevs+1);
        global_settings.sloppy_play_key=0;
        global_settings.sloppy_remote_keys=0;handle_button(BUTTON_NEXT|BUTTON_REL);assert(audio_nexts==nexts+1);
        global_settings.sloppy_remote_keys=1;handle_button(BUTTON_NEXT|BUTTON_REL);assert(audio_nexts==nexts+2);
        /* the R1's next key: a tap skips, a hold goes back, and the hold's release does not skip */
        nexts=audio_nexts;prevs=audio_prevs;int seeks=audio_seeks;
        handle_button(BUTTON_RIGHT);handle_button(BUTTON_RIGHT|BUTTON_REL);assert(audio_nexts==nexts+1);
        test_track.elapsed=1000;global_settings.sloppy_next_hold=1;
        handle_button(BUTTON_RIGHT);handle_button(BUTTON_RIGHT|BUTTON_REPEAT);assert(audio_prevs==prevs);  /* not yet a hold */
        usleep(650000);handle_button(BUTTON_RIGHT|BUTTON_REPEAT);handle_button(BUTTON_RIGHT|BUTTON_REPEAT);
        handle_button(BUTTON_RIGHT|BUTTON_REL|BUTTON_REPEAT);
        assert(audio_prevs==prevs+1&&audio_nexts==nexts+1);
        /* a few seconds into a song, previous restarts it instead */
        test_track.elapsed=5000;handle_button(BUTTON_PREV|BUTTON_REL);
        assert(audio_prevs==prevs+1&&audio_seeks==seeks+1&&test_track.elapsed==0);
        global_settings.sloppy_next_hold=0;handle_button(BUTTON_RIGHT);usleep(650000);
        handle_button(BUTTON_RIGHT|BUTTON_REPEAT);handle_button(BUTTON_RIGHT|BUTTON_REL|BUTTON_REPEAT);
        assert(audio_prevs==prevs+1&&audio_nexts==nexts+2);   /* hold off: the release skips */
        global_settings.sloppy_next_hold=1;
        {   /* hold to seek: steps you hear, longer the longer it is held, never into the next song; the release does nothing more */
            test_status=AUDIO_STATUS_PLAY;test_track.length=200000;test_track.elapsed=10000;
            int n0=audio_nexts,p0=audio_prevs;global_settings.sloppy_next_hold=2;
            handle_button(BUTTON_RIGHT);handle_button(BUTTON_RIGHT|BUTTON_REPEAT);assert(test_track.elapsed==10000);
            usleep(650000);handle_button(BUTTON_RIGHT|BUTTON_REPEAT);assert(test_track.elapsed==15000);
            handle_button(BUTTON_RIGHT|BUTTON_REPEAT);assert(test_track.elapsed==15000);   /* a step at most every 300 ms */
            usleep(320000);handle_button(BUTTON_RIGHT|BUTTON_REPEAT);assert(test_track.elapsed==20000);
            handle_button(BUTTON_RIGHT|BUTTON_REL|BUTTON_REPEAT);assert(audio_nexts==n0&&audio_prevs==p0);
            test_track.elapsed=197000;handle_button(BUTTON_RIGHT);usleep(650000);handle_button(BUTTON_RIGHT|BUTTON_REPEAT);
            assert(test_track.elapsed==198000&&audio_nexts==n0);handle_button(BUTTON_RIGHT|BUTTON_REL);
            /* the play key held: back */
            global_settings.sloppy_play_hold=1;test_track.elapsed=60000;
            handle_button(BUTTON_LEFT);usleep(650000);handle_button(BUTTON_LEFT|BUTTON_REPEAT);handle_button(BUTTON_LEFT|BUTTON_REL);
            assert(test_track.elapsed==55000&&is_playing());global_settings.sloppy_play_hold=0;
            /* press twice: the play key's second press is previous; one press waits a quarter second, then plays or pauses */
            global_settings.sloppy_play_double=2;global_settings.sloppy_next_hold=1;test_track.elapsed=1000;
            handle_button(BUTTON_LEFT);handle_button(BUTTON_LEFT|BUTTON_REL);assert(is_playing());
            handle_button(BUTTON_LEFT);assert(audio_prevs==p0+1&&is_playing());handle_button(BUTTON_LEFT|BUTTON_REL);
            frame(300);assert(is_playing()&&audio_prevs==p0+1);
            handle_button(BUTTON_LEFT);handle_button(BUTTON_LEFT|BUTTON_REL);assert(is_playing());frame(300);assert(!is_playing());
            test_status=AUDIO_STATUS_PLAY;global_settings.sloppy_play_double=0;
            /* the next key twice: play/pause, and no skip */
            global_settings.sloppy_next_double=2;handle_button(BUTTON_RIGHT);handle_button(BUTTON_RIGHT|BUTTON_REL);
            handle_button(BUTTON_RIGHT);handle_button(BUTTON_RIGHT|BUTTON_REL);frame(300);assert(!is_playing()&&audio_nexts==n0);
            test_status=AUDIO_STATUS_PLAY;global_settings.sloppy_next_double=0;
            /* a book: next and previous are 30 seconds, on every key; the next key can go back instead (#108) */
            test_book_playing=true;test_track.elapsed=100000;
            handle_button(BUTTON_RIGHT);handle_button(BUTTON_RIGHT|BUTTON_REL);assert(test_track.elapsed==130000&&audio_nexts==n0);
            handle_button(BUTTON_PREV|BUTTON_REL);assert(test_track.elapsed==100000&&audio_prevs==p0+1);
            global_settings.sloppy_book_next=1;handle_button(BUTTON_RIGHT);handle_button(BUTTON_RIGHT|BUTTON_REL);
            assert(test_track.elapsed==70000);global_settings.sloppy_book_next=0;
            /* and after a pause it picks up a little earlier: more the longer it waited */
            global_settings.sloppy_book_rewind=1;toggle_play();assert(!is_playing()&&sl_paused_at);
            toggle_play();assert(is_playing()&&test_track.elapsed==70000);   /* at once: no rewind */
            toggle_play();sl_paused_at=sl_ms()-70000;toggle_play();assert(test_track.elapsed==60000);
            test_status|=AUDIO_STATUS_PAUSE;sl_paused_poll();sl_paused_at=sl_ms()-4000;toggle_play();assert(test_track.elapsed==57000);
            global_settings.sloppy_book_rewind=0;toggle_play();sl_paused_at=sl_ms()-70000;toggle_play();assert(test_track.elapsed==57000);
            test_book_playing=false;toggle_play();sl_paused_at=sl_ms()-70000;global_settings.sloppy_book_rewind=1;toggle_play();
            assert(test_track.elapsed==57000);   /* music: never */
            /* each output its own maximum and step */
            global_settings.sloppy_jack_limit=80;global_settings.sloppy_bt_limit=50;global_settings.sloppy_bt_step=4;global_settings.sloppy_volume_step=2;
            global_status.sloppy_volume_on_bt=1;sl_out_limit_apply();assert(global_settings.volume_limit==volume_from_pct(50)&&sl_vol_step()==4);
            global_status.sloppy_volume_on_bt=0;sl_out_limit_apply();assert(global_settings.volume_limit==volume_from_pct(80)&&sl_vol_step()==2);
            global_settings.sloppy_dac_limit=40;global_settings.sloppy_dac_step=3;
            global_status.sloppy_volume_on_bt=2;sl_out_limit_apply();assert(global_settings.volume_limit==volume_from_pct(40)&&sl_vol_step()==3);
            global_status.sloppy_volume_on_bt=0;sl_out_limit_apply();global_settings.sloppy_dac_limit=100;global_settings.sloppy_dac_step=1;
            global_settings.sloppy_jack_limit=-1;global_settings.volume_limit=sound_max(SOUND_VOLUME);sl_out_volume_startup();
            assert(global_settings.sloppy_jack_limit==100&&global_settings.volume_limit==sound_max(SOUND_VOLUME));
            global_settings.sloppy_bt_limit=100;global_settings.sloppy_bt_step=1;global_settings.sloppy_volume_step=1;
            test_track.elapsed=0;
        }

        /* status bar: 12 hour clock and no percentage */
        global_settings.timeformat=1;global_settings.battery_display=0;statusbar_update();
        assert(!strcmp(lv_label_get_text(sb_clock),"9:41")&&!strchr(lv_label_get_text(sb_batt),'%'));
        global_settings.timeformat=0;global_settings.battery_display=1;statusbar_update();
        assert(!strcmp(lv_label_get_text(sb_clock),"09:41")&&strchr(lv_label_get_text(sb_batt),'%'));

        /* play through albums: the next album joins as the last song plays */
        /* two short albums of songs that exist, listed B then A by title */
        static int ta[3]={0,1,2},tb[2]={3,4};
        struct sl_album *keep=sl_lib.albums;int *keep_order=sl_lib.albums_by_title,keep_n=sl_lib.n_albums;
        static struct sl_album two[2];static int order[2]={1,0};
        two[0].tracks=ta;two[0].n_tracks=3;two[0].title="Zed";two[1].tracks=tb;two[1].n_tracks=2;two[1].title="Aye";
        sl_lib.albums=two;sl_lib.albums_by_title=order;sl_lib.n_albums=2;
        int a0=1,a1=0;
        global_settings.sloppy_album_through=1;
        assert(play_album(a0,0,false)&&album_chain==0);
        int n0=queued;album_chain_poll();assert(queued==n0);   /* not the last song yet */
        started=queued-1;album_chain_poll();
        assert(queued==n0+sl_lib.albums[a1].n_tracks&&album_chain==1);
        /* a clip does not advance the chain; any other list ends it */
        assert(play_album(a0,0,false)&&album_chain==0);
        quiz.clip.active=true;n0=queued;started=queued-1;album_chain_poll();assert(queued==n0&&album_chain==0);quiz.clip.active=false;
        sl_start_list(0);assert(album_chain==-1);
        assert(play_album(a0,0,true)&&album_chain==-1);        /* shuffled: no chain */
        global_settings.sloppy_album_through=0;
        sl_lib.albums=keep;sl_lib.albums_by_title=keep_order;sl_lib.n_albums=keep_n;
        int shows=np_open;global_settings.sloppy_auto_np=0;np_show_auto();assert(np_open==shows);global_settings.sloppy_auto_np=1;
        test_status=0;
        printf("PASS: sound, buttons, shortcuts, time and power, lyrics colour, play through albums.\n");

        /* language: the interface text changes, song names do not */
        global_settings.sloppy_language=1;
        assert(!strcmp(sl_tr("Settings"),"Ajustes")&&!strcmp(sl_tr("Blue Hours"),"Blue Hours"));
        system_page();frame(500);shot("lang-spanish.ppm");assert(find_label(pages[npages-1],"Ajustes"));page_pop();frame(500);
        global_settings.sloppy_language=4;
        system_page();frame(500);shot("lang-hebrew.ppm");assert(find_label(pages[npages-1],"הגדרות"));page_pop();frame(500);
        global_settings.sloppy_language=3;toast("Queue cleared");assert(find_label(lv_layer_top(),"Warteschlange geleert"));
        {   /* Arabic: every phrase, joined the way LVGL joins it, has a glyph
             * for each letter in every face that shows menu text */
            global_settings.sloppy_language=5;
            assert(!strcmp(sl_tr("Settings"),"الإعدادات"));
            const lv_font_t *faces[]={&ss_regular_16,&ss_regular_18,&ss_regular_21,&ss_regular_23,&ss_semibold_18,
                                      &ss_semibold_21,&ss_semibold_23,&ss_semibold_26,&ss_semibold_30,&ss_bold_28};
            int letters=0,missing=0;static char shaped[4096];
            for(size_t i=0;i<ARRAYLEN(sl_phrases);i++){
                const char *ar=sl_phrases[i].t[4];assert(ar && ar[0]);
                assert(lv_text_ap_calc_bytes_count(ar)<=sizeof shaped);lv_text_ap_proc(ar,shaped);
                uint32_t at=0,cp;
                while((cp=lv_text_encoded_next(shaped,&at))!=0){
                    if(cp<0x600)continue;
                    letters++;
                    for(size_t f=0;f<ARRAYLEN(faces);f++){lv_font_glyph_dsc_t g;
                        if(!lv_font_get_glyph_dsc(faces[f],&g,cp,0))
                        {if(missing++<5)printf("arabic: no glyph U+%04X in face %d (%s)\n",(unsigned)cp,(int)f,sl_phrases[i].en);}}}}
            printf("arabic: %d phrases, %d letters checked in %d faces, %d missing\n",(int)ARRAYLEN(sl_phrases),letters,(int)ARRAYLEN(faces),missing);
            assert(missing==0);
            system_page();frame(500);shot("lang-arabic.ppm");
            page_pop();frame(500);
            language_page();frame(500);shot("lang-arabic-picker.ppm");page_pop();frame(500);
            sl_bt_page();frame(500);shot("lang-arabic-bluetooth.ppm");page_pop();frame(500);
            toast("Queue cleared");frame(300);shot_now("lang-arabic-toast.ppm");
        }
        global_settings.sloppy_language=0;

        /* text size: body text one step up */
        int small=lv_font_get_line_height(&ss_regular_18);
        sl_text_step=1;assert(lv_font_get_line_height(&ss_regular_18)>small);
        system_page();frame(500);shot("text-large.ppm");page_pop();frame(500);
        sl_text_step=0;
        {   /* the slider: four steps, the sample follows the drag, release applies */
            int saved=global_settings.sloppy_text_size;
            text_size_page();frame(400);
            lv_obj_t *ts=NULL;
            for(lv_obj_t *o=find_label(pages[npages-1],"Size");o&&!ts;o=lv_obj_get_parent(o))
                for(uint32_t i=0;i<lv_obj_get_child_count(o)&&!ts;i++)
                    if(lv_obj_check_type(lv_obj_get_child(o,i),&lv_slider_class))ts=lv_obj_get_child(o,i);
            assert(ts && lv_slider_get_max_value(ts)==3);
            lv_slider_set_value(ts,3,LV_ANIM_OFF);lv_obj_send_event(ts,LV_EVENT_VALUE_CHANGED,NULL);frame(100);
            assert(global_settings.sloppy_text_size==saved && find_label(pages[npages-1],"Largest"));
            shot("text-size-slider.ppm");
            lv_obj_send_event(ts,LV_EVENT_RELEASED,NULL);frame(600);
            assert(global_settings.sloppy_text_size==3 && sl_text_step==3);
            assert(lv_font_get_line_height(&ss_semibold_26)>lv_font_get_line_height(sl_fonts[1][5]));
            while(npages>1){page_pop();frame(300);}
            system_page();frame(500);shot("text-largest-settings.ppm");
            sound_page();frame(500);shot("text-largest-sound.ppm");page_pop();frame(300);page_pop();frame(300);
            sl_library_page();frame(500);songs_page();frame(500);shot("text-largest-songs.ppm");
            while(npages>1){page_pop();frame(300);}
            test_status=AUDIO_STATUS_PLAY;np_show();frame(600);shot("text-largest-np.ppm");np_hide(0);frame(500);
            global_settings.sloppy_text_size=saved;sl_text_step=saved;appearance_reload(NULL);frame(600);
            while(npages>1){page_pop();frame(300);}
        }

        {   /* screenshots: a PNG in /Screenshots at the card's top, a flash, the card with its thumbnail, the
             * viewer and Delete; a clash gets " 2"; the control centre's tile leaves the panel out of the shot;
             * USB mode says the card is busy */
            while(npages>1){page_pop();frame(300);}
            frame(3000);
            char d[600];snprintf(d,sizeof d,"rm -rf '%s/Screenshots'; mkdir -p '%s'",sim_root_dir,sim_root_dir);system(d);
            unsigned t0=shot_taken;
            sl_screenshot();assert(shot_taken==t0+1);
            frame(40);shot_now("screenshot-flash.ppm");
            for(int i=0;i<300&&!sc.up;i++){usleep(10000);frame(20);}
            assert(sc.up&&strstr(shot_last,"/Screenshots/gabagoolOS ")&&strstr(shot_last,".png")&&!strstr(shot_last," 2.png"));
            {   uint16_t *px;int w,h;assert(sl_png_load565(shot_last,W,H,&px,&w,&h)&&w==W&&h==H);free(px);
                assert(sl_png_load565(shot_last,SHOT_VIEW_W,SHOT_VIEW_H,&px,&w,&h)&&w==SHOT_VIEW_W&&h==SHOT_VIEW_H);free(px);
                /* a small file claiming 30000 x 30000 (box art, a picture in Screenshots): refused before
                 * anything is made (decoded, it took the player's memory) */
                static const unsigned char huge[33]={0x89,'P','N','G','\r','\n',0x1a,'\n',0,0,0,13,'I','H','D','R',0,0,0x75,0x30,0,0,0x75,0x30,8,6,0,0,0};
                char hp[700];snprintf(hp,sizeof hp,"%s/Screenshots/huge.png",sim_root_dir);
                FILE *hf=fopen(hp,"wb");assert(hf);fwrite(huge,1,sizeof huge,hf);fclose(hf);
                assert(!sl_png_load565("/Screenshots/huge.png",W,H,&px,&w,&h)&&!px);remove(hp); }
            frame(600);shot("screenshot-toast.ppm");
            char first[256];strlcpy(first,shot_last,sizeof first);
            sl_screenshot();for(int i=0;i<300&&!strstr(shot_last," 2.png");i++){usleep(10000);frame(20);}
            assert(strstr(shot_last," 2.png"));   /* the same second: a second name */
            frame(400);lv_obj_send_event(sc.obj,LV_EVENT_PRESSED,NULL);lv_obj_send_event(sc.obj,LV_EVENT_CLICKED,NULL);frame(600);
            assert(has_text(pages[npages-1],"Delete")&&shot_view_px);shot("screenshot-viewer.ppm");
            char real[600];snprintf(real,sizeof real,"%s%s",sim_root_dir,shot_view_path);assert(!access(real,F_OK));
            shot_delete_cb(NULL);frame(600);assert(access(real,F_OK)&&npages==1&&!shot_view_px);
            snprintf(real,sizeof real,"%s%s",sim_root_dir,first);assert(!access(real,F_OK));
            {   /* two viewers at once (the card tapped twice): closing the top one leaves
                 * the one under it its own picture (ASan: none read after it is freed) */
                int n0=npages;shot_view_page(first);frame(300);shot_view_page(first);frame(300);assert(npages==n0+2);
                page_pop();frame(600);lv_obj_invalidate(lv_screen_active());frame(100);
                assert(npages==n0+1&&has_text(pages[npages-1],"Delete"));
                page_pop();frame(600);assert(npages==n0&&!shot_view_px); }
            frame(5000);assert(!sc.up);
            /* the control centre's tile: the panel closes first, then the page is taken */
            nbanner_hide_now();pop_hide_now();frame(600);
            static uint16_t before[W*H];memcpy(before,pixels,sizeof before);
            ccx_open();ccx_grow();
            touch_at(400,300,true);for(int x=380;x>=160;x-=20)touch_at(x,300,true);touch_at(160,300,false);ccx_settle();   /* its page */
            assert(qx_page==ccx_tile(16)/QP_PAGE);
            unsigned t1=shot_taken;ccx_tap(qp_tiles[ccx_tile(16)]);
            for(int i=0;i<300&&!sc.up;i++){usleep(10000);frame(20);}
            assert(shot_taken==t1+1&&!qp_open&&sc.up);
            {   uint16_t *px;int w,h;assert(sl_png_load565(shot_last,W,H,&px,&w,&h));
                int same=0;for(int i=0;i<W*H;i++)same+=abs((px[i]>>11)-(before[i]>>11))<=1&&abs(((px[i]>>5)&63)-((before[i]>>5)&63))<=1;
                free(px);printf("screenshot from the tile: %d.%d%% of pixels as the page was\n",same*100/(W*H),same*1000/(W*H)%10);
                assert(same>W*H*97/100); }
            shot_card_hide();frame(600);
            log_held=true;sl_screenshot();frame(100);log_held=false;assert(!sl_shot_busy());
            frame(5000);
        }
        {   /* Set as wallpaper: from a screenshot, for home, lock or both; the page lists
             * "Your picture" once there is one; a picture it can't read says so */
            while(npages>1){page_pop();frame(300);}
            global_settings.sloppy_home_wallpaper=global_settings.sloppy_lock_wallpaper=0;wallpaper_custom_free();
            wallpaper_page(false);frame(500);assert(!find_label(pages[npages-1],"Your picture"));page_pop();frame(400);
            nbanner_hide_now();pop_hide_now();frame(600);
            sl_screenshot();for(int i=0;i<300&&!sc.up;i++){usleep(10000);frame(20);}
            shot_card_hide();frame(600);
            shot_view_page(shot_last);frame(600);lv_obj_t *sw=find_label(pages[npages-1],"Set as wallpaper");assert(sw);
            lv_obj_scroll_to_view_recursive(sw,LV_ANIM_OFF);frame(200);shot("wallpaper-set-row.ppm");
            lv_obj_send_event(lv_obj_get_parent(sw),LV_EVENT_CLICKED,NULL);frame(500);
            assert(find_label(acts,"Home screen")&&find_label(acts,"Lock screen")&&find_label(acts,"Both"));shot("wallpaper-set-sheet.ppm");
            lv_obj_send_event(lv_obj_get_parent(find_label(acts,"Both")),LV_EVENT_CLICKED,NULL);
            assert(!strcmp(test_wall_in,shot_last));
            for(int i=0;i<20&&global_settings.sloppy_home_wallpaper!=WALL_CUSTOM;i++)frame(100);
            assert(global_settings.sloppy_home_wallpaper==WALL_CUSTOM&&global_settings.sloppy_lock_wallpaper==WALL_CUSTOM);
            frame(200);assert(find_label(lv_layer_top(),"Wallpaper set"));
            while(npages>1){page_pop();frame(300);}
            for(int i=0;i<10;i++)frame(200);
            assert(wall_custom_px&&wallpaper_id(false)==WALL_CUSTOM&&wallpaper_id(true)==WALL_CUSTOM);
            shot("wallpaper-home-custom.ppm");
            {   /* the home screen shows it: the sun's colour where the sun is */
                uint16_t c=pixels[310*W+160];   /* between the card rows */int r=c>>11,g=(c>>5)&63;printf("home on your picture: sun pixel %d,%d\n",r,g);
                assert(r>=24&&g>=40); }
            sl_power_overlay_show(true);frame(400);shot("wallpaper-lock-custom.ppm");sl_power_overlay_show(false);for(int i=0;i<6;i++)frame(100);
            wallpaper_page(true);frame(500);assert(find_label(pages[npages-1],"Your picture"));shot("wallpaper-page-custom.ppm");page_pop();frame(400);
            /* home only, from a picture in Files */
            global_settings.sloppy_lock_wallpaper=0;
            strlcpy(wall_set_path,"/Pictures/broken.jpg",sizeof wall_set_path);wall_set_pick(0);frame(200);
            assert(find_label(lv_layer_top(),"GabagoolOS can't make a wallpaper of this picture"));
            pop_hide_now();frame(300);
            wallpaper_set_offer("/Pictures/dusk.jpg");frame(400);lv_obj_send_event(lv_obj_get_parent(find_label(acts,"Home screen")),LV_EVENT_CLICKED,NULL);
            for(int i=0;i<10;i++)frame(100);
            assert(global_settings.sloppy_home_wallpaper==WALL_CUSTOM&&global_settings.sloppy_lock_wallpaper==0);
            /* nothing shows it: out of memory; the file stays for the page */
            global_settings.sloppy_home_wallpaper=0;wallpaper_custom_free();appearance_changed();for(int i=0;i<6;i++)frame(100);
            assert(!wall_custom_px);
            pop_hide_now();while(npages>1){page_pop();frame(300);}
            printf("PASS: Set as wallpaper from a screenshot, home and lock, Your picture in the list.\n");
        }
        {   /* below brightness 1 (PS24): the slider's last steps lay a black veil over
             * everything that takes no touches, and screenshots are taken without it */
            while(npages>1){page_pop();frame(300);}
            nbanner_hide_now();pop_hide_now();frame(600);
            static uint16_t lit[W*H];memcpy(lit,pixels,sizeof lit);
            int b0=global_settings.brightness;
            sl_bright_set(MIN_BRIGHTNESS_SETTING-SL_NIGHT_STEPS);frame(100);
            assert(global_settings.brightness==MIN_BRIGHTNESS_SETTING&&global_settings.sloppy_night_dim==SL_NIGHT_STEPS);
            assert(sl_night_veil&&!lv_obj_has_flag(sl_night_veil,LV_OBJ_FLAG_HIDDEN)&&!lv_obj_has_flag(sl_night_veil,LV_OBJ_FLAG_CLICKABLE));
            assert(sl_bright_value()==MIN_BRIGHTNESS_SETTING-SL_NIGHT_STEPS);
            {   long a=0,b=0;for(int i=0;i<W*H;i++){a+=(lit[i]>>5)&63;b+=(pixels[i]>>5)&63;}
                printf("night veil, darkest: %.0f%% of the light left\n",a?100.0*b/a:0);assert(a&&b*100<=a*20); }
            shot("night-veil-darkest.ppm");
            unsigned t0=shot_taken;sl_screenshot();assert(shot_taken==t0+1);
            for(int i=0;i<300&&!sc.up;i++){usleep(10000);frame(20);}
            assert(!lv_obj_has_flag(sl_night_veil,LV_OBJ_FLAG_HIDDEN));   /* back over the room */
            {   uint16_t *px;int w,h;assert(sl_png_load565(shot_last,W,H,&px,&w,&h));
                long a=0,b=0;for(int i=0;i<W*H;i++){a+=(lit[i]>>5)&63;b+=(px[i]>>5)&63;}free(px);
                printf("screenshot under the veil: %.0f%% of the page's light\n",a?100.0*b/a:0);assert(b*100>=a*90); }
            shot_card_hide();frame(600);
            sl_bright_set(MIN_BRIGHTNESS_SETTING-4);assert(global_settings.sloppy_night_dim==4&&lv_obj_get_style_bg_opa(sl_night_veil,0)==150);
            ccx_open();assert(lv_slider_get_value(qp_bright)==MIN_BRIGHTNESS_SETTING-4&&lv_slider_get_min_value(qp_bright)==MIN_BRIGHTNESS_SETTING-SL_NIGHT_STEPS);
            lv_obj_add_flag(sl_night_veil,LV_OBJ_FLAG_HIDDEN);frame(300);shot("night-veil-cc.ppm");lv_obj_remove_flag(sl_night_veil,LV_OBJ_FLAG_HIDDEN);
            qp_close();ccx_closed();   /* opened by a drag through the veil: it takes no touches */
            qp_display_page();frame(500);
            {lv_obj_t *r=lv_obj_get_parent(find_label(pages[npages-1],"Brightness"));lv_obj_t *sl=lv_obj_get_child(r,2);
             assert(lv_slider_get_min_value(sl)==MIN_BRIGHTNESS_SETTING-SL_NIGHT_STEPS&&lv_slider_get_value(sl)==MIN_BRIGHTNESS_SETTING-4);
             lv_slider_set_value(sl,40,LV_ANIM_OFF);lv_obj_send_event(sl,LV_EVENT_VALUE_CHANGED,NULL);
             assert(global_settings.brightness==40&&!global_settings.sloppy_night_dim&&lv_obj_has_flag(sl_night_veil,LV_OBJ_FLAG_HIDDEN));}
            frame(300);shot("night-display-page.ppm");page_pop();frame(400);
            global_settings.sloppy_night_dim=0;global_settings.brightness=b0;sl_night_apply();assert(lv_obj_has_flag(sl_night_veil,LV_OBJ_FLAG_HIDDEN));
            printf("PASS: night veil below brightness 1, out of screenshots, on both sliders.\n");
        }
        /* Wi-Fi against the pretend radio */
        global_settings.sloppy_wifi=1;wf_arg_on=true;wf_begin(WF_JOB_POWER);
        sl_wifi_page();assert(wf_scan_requested);frame(300);frame(600);assert(wf_count==3);shot("wifi.ppm");
        /* A failed refresh must not replace the visible network list with
         * the misleading "No networks found" state. */
        wf_busy=true;wf_job=WF_JOB_SCAN;wf_res=(struct wf_result){.job=WF_JOB_SCAN,.count=-1,.st=wf_st};wf_done=true;
        wf_job_cb(wf_job_timer);assert(wf_count==3&&wf_scanned);
        while(wf_busy)frame(100);
        wf_join("Home network",NULL);frame(300);assert(wf_st.connected&&!strcmp(wf_st.ssid,"Home network"));
        statusbar_update();assert(strstr(lv_label_get_text(sb_radio),LV_SYMBOL_WIFI));   /* the bar says it is online */
        {int sv=global_status.volume;global_status.volume=sound_max(SOUND_VOLUME)+40;assert(volume_pct()==100);global_status.volume=sv;}
        {   /* the phone remote: the page, what plays, a button and a search reach the player over a real socket */
            static char r[32768];char v[32];
            rm_set(1);assert(sl_net_remote_on());rm_test_pair();
            assert(http_get("/",r,sizeof r)>0 && strstr(r,"200 OK") && strstr(r,"GabagoolOS"));
            assert(http_get("/api/now",r,sizeof r)>0 && strstr(r,"application/json") && strstr(r,"\"title\":"));
            assert(http_get("/api/cmd?c=up",r,sizeof r)>0 && strstr(r,"\"vol\":"));
            assert(http_get("/api/search?q=night",r,sizeof r)>0 && strstr(r,"\"songs\":[{") && strstr(r,"Night"));
            assert(http_get("/nope",r,sizeof r)>0 && strstr(r,"404"));
            {   /* the remote's Take screenshot: the shot, then the page fetches it */
                assert(http_get("/",r,sizeof r)>0 && strstr(r,"Take screenshot"));
                unsigned q=sl_shot_seq();
                assert(http_get("/api/cmd?c=shot",r,sizeof r)>0 && strstr(r,"\"shot\":"));
                for(int i=0;i<300&&sl_shot_seq()==q;i++){usleep(10000);frame(20);}
                assert(sl_shot_seq()==q+1);
                int n=http_get("/shot.png",r,sizeof r);assert(n>0 && strstr(r,"image/png"));
                char *body=strstr(r,"\r\n\r\n");assert(body && !memcmp(body+4,"\x89PNG",4));
                shot_card_hide();frame(600);
            }
            assert(sl_net_query_param("a=1&q=blue%20h+x","q",v,sizeof v) && !strcmp(v,"blue h x"));
            wf_rebuild();frame(300);lv_obj_t *rl=find_label(pages[npages-1],"PHONE REMOTE");assert(rl);
            lv_obj_scroll_to_view_recursive(rl,LV_ANIM_OFF);frame(100);shot("wifi-remote.ppm");
            rm_set(0);assert(!sl_net_remote_on());
        }
        {   /* internet radio: the directory's stations; a real MPEG stream decoded on the network thread and
             * handed to the mixer; Stop; and a station that does not answer */
            FILE *mf=fopen("/tmp/sloppy-radio-test.mp2","wb");assert(mf);
            unsigned char fr[32]={0xFF,0xFF,0x10,0xC0};   /* MPEG-1 Layer I, 32 kbps, 44.1 kHz, mono: silence */
            for(int i=0;i<400;i++)fwrite(fr,1,sizeof fr,mf);fclose(mf);
            free(st_list);st_list=NULL;st_n=0;
            {   /* the AAC station: a real ADTS file, from afconvert */
                FILE *in=fopen("sloppyos/apps/sloppy/tests/tone.aac","rb"),*out=fopen("/tmp/sloppy-radio-test.aac","wb");assert(in&&out);
                char cb[4096];size_t cn;while((cn=fread(cb,1,sizeof cb,in))>0)fwrite(cb,1,cn,out);fclose(in);fclose(out);
            }
            webradio_page();for(int i=0;i<30 && st_n<4;i++)frame(100);
            /* near you first: the country from the address, then its stations */
            assert(!strcmp(st_cc,"GB") && strstr(sl_net_demo_url,"bycountrycodeexact/GB?") && has_text(pages[npages-1],"STATIONS IN GB"));
            assert(st_n==4 && !strcmp(st_list[0].name,"Test FM") && !strcmp(st_list[1].name,"Caf\xc3\xa9 \"Noir\""));
            assert(has_text(pages[npages-1],"GB \xc2\xb7 jazz \xc2\xb7 32 kbps") && has_text(pages[npages-1],"FR \xc2\xb7 128 kbps"));
            shot("radio.ppm");
            lv_obj_t *sr=find_label(pages[npages-1],"Test FM");while(sr&&!lv_obj_has_flag(sr,LV_OBJ_FLAG_CLICKABLE))sr=lv_obj_get_parent(sr);
            assert(sr);lv_obj_send_event(sr,LV_EVENT_CLICKED,NULL);
            for(int i=0;i<150 && !st_active;i++){usleep(10000);frame(20);}
            assert(st_active && st_on() && sl_net_radio_rate()==44100 && test_voice_more && find_label(pages[npages-1],"ON AIR"));
            frame(300);shot("radio-playing.ppm");
            st_stop_cb(NULL);frame(300);assert(!st_on() && !test_voice_more && sl_net_radio_state()==SL_RADIO_OFF);
            st_play(1);frame(300);assert(!st_on());   /* nowhere to connect: said, not left hanging */
            /* a resolved address that fails: its own, then plain HTTP, until one answers */
            char u[256];
            assert(st_url(&st_list[2],0,u) && !strcmp(u,"http://nowhere") && st_url(&st_list[2],1,u) && !strcmp(u,"https://nowhere")
                   && st_url(&st_list[2],2,u) && !strcmp(u,"/tmp/sloppy-radio-test.mp2") && !st_url(&st_list[2],3,u));
            st_play(2);for(int i=0;i<150 && !st_active;i++){usleep(10000);frame(20);}
            assert(st_active && st_try==2);
            /* AAC: decoded by libfaad, at the file's own rate */
            st_play(3);for(int i=0;i<300 && !st_active;i++){usleep(10000);frame(20);}
            assert(st_active && sl_net_radio_rate()>=32000);   /* on air: decoded, at the rate the encoder chose */
            st_stop_cb(NULL);frame(300);assert(!st_on());
            st_play(2);for(int i=0;i<150 && !st_active;i++){usleep(10000);frame(20);}assert(st_active);
            /* saved: kept on the card, listed above the rest, and removed again */
            remove(ST_SAVED);st_saved_read=false;st_nsaved=0;
            st_save_cb(NULL);frame(200);
            assert(st_nsaved==1 && has_text(pages[npages-1],"SAVED") && has_text(pages[npages-1],"Saved"));
            st_saved_read=false;st_nsaved=0;st_saved_load();assert(st_nsaved==1 && !strcmp(st_saved[0].name,"Backup FM") && !strcmp(st_saved[0].alt,"/tmp/sloppy-radio-test.mp2"));
            shot("radio-saved.ppm");
            st_save_cb(NULL);frame(200);assert(st_nsaved==0 && !has_text(pages[npages-1],"SAVED"));
            st_stop_cb(NULL);frame(200);
            /* a kind of music from the chips */
            lv_obj_t *jz=find_label(pages[npages-1],"Jazz");while(jz&&!lv_obj_has_flag(jz,LV_OBJ_FLAG_CLICKABLE))jz=lv_obj_get_parent(jz);
            assert(jz);lv_obj_send_event(jz,LV_EVENT_CLICKED,NULL);for(int i=0;i<30 && st_loading;i++)frame(100);
            assert(st_kind==ST_TAGS && strstr(sl_net_demo_url,"bytagexact/jazz?") && st_n==4 && has_text(pages[npages-1],"JAZZ"));
            shot("radio-jazz.ppm");
            st_kind=ST_NEAR;
            page_pop();frame(400);
        }
        wf_rebuild();frame(300);shot("wifi-connected.ppm");
        wf_password_page("Neighbours 5G");frame(500);shot("wifi-password.ppm");
        lv_obj_t *show_label=find_label(pages[npages-1],"Show password");assert(show_label);
        lv_obj_t *show_switch=lv_obj_get_child(lv_obj_get_parent(show_label),1);assert(show_switch);
        lv_obj_add_state(show_switch,LV_STATE_CHECKED);
        lv_obj_send_event(show_switch,LV_EVENT_VALUE_CHANGED,NULL);
        assert(!lv_textarea_get_password_mode(wf_pw_ta));
        /* the keyboard: one capital, then lower case, a digit, a symbol from the second page */
        lv_textarea_set_text(wf_pw_ta,"");
        assert(kb_now()&&kb_now()->kind==KB_PASSWORD&&!kb_now()->shift);   /* a password: no capital of its own */
        kb_type(kb_now(),"Qq0\\");
        assert(!strcmp(lv_textarea_get_text(wf_pw_ta),"Qq0\\"));
        kb_tap_act(kb_now(),KA_MODE);frame(300);shot("wifi-keyboard.ppm");kb_tap_act(kb_now(),KA_MODE);
        lv_textarea_set_text(wf_pw_ta,"short");wf_pw_join_cb(NULL);assert(npages>0&&wf_pw_ta);   /* too short: stays */
        page_pop();frame(500);
        {   /* joining by hand: a hidden network by its name, with a password, then an open one */
            frame(3000);wf_other_page();frame(500);shot("wifi-other.ppm");
            assert(wf_ot_name&&wf_ot_pw&&kb_now()->ta==wf_ot_name&&lv_obj_has_state(wf_ot_name,LV_STATE_FOCUSED));
            wf_ot_join_cb(NULL);assert(wf_ot_name);   /* no name: stays */
            kb_type(kb_now(),"Q");
            assert(!strcmp(lv_textarea_get_text(wf_ot_name),"Q"));
            lv_textarea_set_text(wf_ot_name,"Attic Studio");
            lv_obj_send_event(wf_ot_pw,LV_EVENT_PRESSED,NULL);assert(kb_now()->ta==wf_ot_pw&&!lv_obj_has_state(wf_ot_name,LV_STATE_FOCUSED));
            lv_textarea_set_text(wf_ot_pw,"short");wf_ot_join_cb(NULL);assert(wf_ot_name);   /* too short: stays */
            lv_textarea_set_text(wf_ot_pw,"correct horse");wf_ot_join_cb(NULL);
            for(int i=0;i<20&&(wf_busy||wf_join_pending);i++)frame(100);
            frame(500);
            assert(!wf_ot_name&&wf_arg_hidden&&wf_st.connected&&!strcmp(wf_st.ssid,"Attic Studio"));
            wf_other_page();frame(400);
            lv_obj_t *op=find_label(pages[npages-1],"Open");while(op&&!lv_obj_has_flag(op,LV_OBJ_FLAG_CLICKABLE))op=lv_obj_get_parent(op);
            assert(op);lv_obj_send_event(op,LV_EVENT_CLICKED,NULL);frame(200);
            assert(wf_ot_secure==0&&lv_obj_has_flag(wf_ot_pw_group,LV_OBJ_FLAG_HIDDEN)&&lv_obj_has_flag(wf_ot_pw_head,LV_OBJ_FLAG_HIDDEN));
            lv_textarea_set_text(wf_ot_name,"Home network");frame(3000);shot("wifi-other-open.ppm");wf_ot_join_cb(NULL);
            for(int i=0;i<20&&(wf_busy||wf_join_pending);i++)frame(100);
            assert(!wf_join_has_psk&&wf_st.connected&&!strcmp(wf_st.ssid,"Home network"));
            wf_ot_secure=1;frame(500);
        }
        wf_share_cb(NULL);frame(500);shot("wifi-transfer.ppm");assert(wf_share_page_obj);page_pop();frame(500);
        page_pop();frame(500);
        while(wf_busy)frame(100);
        /* screen off and nothing using the network: the radio rests, then wakes with the screen */
        for(int i=0;i<40&&(wf_busy||wf_share_on);i++)frame(250);   /* the transfer server stops with its page */
        assert(wf_st.on&&!wf_airplay_on&&!wf_dlna_on&&!wf_share_on);
        wallpaper_screen_dark=true;frame(1100);assert(!wf_resting&&wf_st.on);
        wf_dark_since-=WF_REST_AFTER_MS;frame(1100);assert(wf_resting);
        while(wf_busy)frame(100);assert(!wf_st.on&&global_settings.sloppy_wifi==1);
        wallpaper_screen_dark=false;frame(1100);assert(!wf_resting);
        while(wf_busy)frame(100);assert(wf_st.on);
        global_settings.sloppy_airplay=1;wf_airplay_on=true;   /* AirPlay on: stays reachable */
        wallpaper_screen_dark=true;frame(1100);wf_dark_since-=WF_REST_AFTER_MS;frame(1100);
        assert(!wf_resting&&wf_st.on);
        wallpaper_screen_dark=false;global_settings.sloppy_airplay=0;wf_airplay_on=false;
        while(wf_busy)frame(100);
        st_active=true;   /* internet radio playing: it stopped three minutes into a dark screen */
        wallpaper_screen_dark=true;frame(1100);wf_dark_since-=WF_REST_AFTER_MS;frame(1100);
        assert(!wf_resting&&wf_st.on);
        wallpaper_screen_dark=false;st_active=false;frame(1100);
        while(wf_busy)frame(100);
        global_settings.sloppy_wifi=0;wf_arg_on=false;wf_begin(WF_JOB_POWER);frame(300);assert(!wf_st.on);
        frame(1200);assert(!wf_tick_timer||lv_timer_get_paused(wf_tick_timer));   /* Wi-Fi off: no idle wake-ups */

        {   /* an R1 with the newer AIC8800 chip: the id read whichever way the kernel writes it, both pages
             * say so plainly, nothing is bound or started, the Settings rows and the control centre agree */
            assert(sl_net_soc_parse("0x4a00\n")==0x4a00 && sl_net_soc_parse("4a00")==0x4a00 && sl_net_soc_parse("18944")==0x4a00 &&
                   sl_net_soc_parse("0a00")==0x0a00 && sl_net_soc_parse("2560")==0x0a00 && sl_net_soc_parse("4900")==0x4900 && sl_net_soc_parse("")==-1);
            char pr[200];sl_net_aic_fake=0;   /* no AIC driver on this system: a boot menu built on 1.6 */
            sl_net_soc_fake=0x0a00;assert(!sl_net_wireless_unsupported()&&!sl_net_wireless_aic());
            sl_net_wireless_probe(pr,sizeof pr);assert(strstr(pr,"0x0a00 (X1600E)")&&strstr(pr,"Broadcom"));
            sl_net_soc_fake=0x4a00;assert(sl_net_wireless_unsupported());
            sl_net_wireless_probe(pr,sizeof pr);assert(strstr(pr,"0x4a00 (X1600EN)")&&strstr(pr,"AIC8800D40"));
            assert(!sl_net_wifi_power(true)&&strstr(sl_net_wifi_last_error(),"AIC8800"));
            assert(!wf_begin(WF_JOB_POWER)&&!wf_busy);
            sl_wifi_page();frame(500);
            assert(has_text(pages[npages-1],"Wi-Fi isn't supported on this R1 yet")&&has_text(pages[npages-1],"newer wireless chip")&&
                   has_text(pages[npages-1],"X1600EN")&&!find_label(pages[npages-1],"Scan for networks"));
            shot("wifi-aic.ppm");page_pop();frame(400);
            int bj=sl_bt_job;bool bb=sl_bt_busy;sl_net_soc_fake=0x4900;
            sl_bt_page();frame(500);assert(sl_bt_busy==bb&&sl_bt_job==bj);   /* no refresh asked of BlueZ */
            assert(has_text(pages[npages-1],"Bluetooth isn't supported on this R1 yet")&&has_text(pages[npages-1],"X1600HN")&&
                   !find_label(pages[npages-1],"Scan for devices"));
            shot("bluetooth-aic.ppm");page_pop();frame(400);
            assert(!strcmp(wf_row_text(),"Not supported")&&!strcmp(set_v_bluetooth(),"Not supported"));
            qp_wifi_toggle();frame(100);assert(global_settings.sloppy_wifi==0);
            assert(strstr(pr,"needs the 2.0 boot menu"));

            frame(4000);   /* the toast from the switch above has gone */
            /* the same unit on the boot menu built on HiBy 2.0: HiBy's AIC driver is there and loaded, so
             * Wi-Fi and Bluetooth are the ordinary pages, and the radio turns on, scans and joins */
            sl_net_aic_fake=1;sl_net_soc_fake=0x4a00;
            assert(sl_net_wireless_aic()&&!sl_net_wireless_unsupported()&&!sl_net_wifi_radio_missing());
            sl_net_wireless_probe(pr,sizeof pr);assert(strstr(pr,"AIC8800D40 (HiBy 2.0 drivers)"));
            assert(strcmp(wf_row_text(),"Not supported")&&strcmp(set_v_bluetooth(),"Not supported"));
            global_settings.sloppy_wifi=1;wf_arg_on=true;assert(wf_begin(WF_JOB_POWER));frame(600);
            for(int i=0;i<40&&wf_busy;i++)frame(100);
            assert(wf_st.on&&!wf_busy);
            sl_wifi_page();frame(500);
            assert(!has_text(pages[npages-1],"newer wireless chip")&&find_label(pages[npages-1],"Scan for networks"));
            shot("wifi-aic-2.0.ppm");page_pop();frame(400);
            sl_net_soc_fake=0x4900;
            sl_bt_page();frame(500);
            assert(!has_text(pages[npages-1],"newer wireless chip")&&find_label(pages[npages-1],"Scan for devices"));
            shot("bluetooth-aic-2.0.ppm");page_pop();frame(400);
            /* Wi-Fi or Bluetooth asked for again (the control centre, Now Playing's lyrics, a station)
             * while its page is further down: back to it, where the tap used to do nothing */
            sl_wifi_page();frame(500);wf_other_page();frame(500);
            {int n=npages;assert(pages[n-1]!=wf_page_obj&&pages[n-2]==wf_page_obj);
             sl_wifi_page();frame(600);assert(npages==n-1&&pages[npages-1]==wf_page_obj&&!lv_obj_has_flag(wf_page_obj,LV_OBJ_FLAG_HIDDEN));}
            page_pop();frame(400);
            sl_bt_page();frame(500);sl_bt_sbc_page(NULL);frame(500);
            {int n=npages;assert(pages[n-1]!=sl_bt_page_obj&&pages[n-2]==sl_bt_page_obj);
             sl_bt_page();frame(600);assert(npages==n-1&&pages[npages-1]==sl_bt_page_obj&&!lv_obj_has_flag(sl_bt_page_obj,LV_OBJ_FLAG_HIDDEN));}
            page_pop();frame(400);
            global_settings.sloppy_wifi=0;wf_arg_on=false;wf_begin(WF_JOB_POWER);frame(300);
            for(int i=0;i<40&&wf_busy;i++)frame(100);
            assert(!wf_st.on);

            /* HiBy 2.0's driver there but failed to load at boot: the plain page again, and it says so */
            sl_net_aic_fake=-1;
            assert(sl_net_wireless_unsupported());
            sl_net_wireless_probe(pr,sizeof pr);assert(strstr(pr,"failed to load"));
            assert(!sl_net_wifi_power(true));
            sl_wifi_page();frame(500);
            assert(has_text(pages[npages-1],"didn't start")&&has_text(pages[npages-1],"X1600HN")&&!find_label(pages[npages-1],"Scan for networks"));
            shot("wifi-aic-failed.ppm");page_pop();frame(400);

            sl_net_aic_fake=-2;sl_net_soc_fake=-1;assert(!sl_net_wireless_unsupported()&&!sl_net_wireless_aic());
        }

        /* time zones */
        strlcpy((char *)global_settings.sloppy_timezone,"Asia/Tokyo",sizeof global_settings.sloppy_timezone);tz_apply();
        assert(!strcmp(getenv("TZ"),"Asia/Tokyo"));
        { char b[64];tz_label(b,sizeof b,"Asia/Kolkata");assert(strstr(b,"Kolkata (UTC+5:30)"));tz_apply(); }
        time_power_page();frame(500);shot("time-power.ppm");page_pop();frame(500);
        /* battery care: "80%" writes the 4.20 V limit, "Full" HiBy's 4.40 V, never "off" */
        global_settings.hiby_charge_limit_voltage=0;
        time_power_page();frame(500);
        { lv_obj_t *l=find_label(pages[npages-1],"Charge to");assert(l);   /* in the Battery card now */
          lv_obj_t *c80=find_label(pages[npages-1],"80%");assert(c80);
          lv_obj_send_event(lv_obj_get_parent(c80),LV_EVENT_CLICKED,NULL);frame(100);
          assert(global_settings.hiby_charge_limit_voltage==3);
          lv_obj_scroll_to_view_recursive(c80,LV_ANIM_OFF);frame(300);shot("time-power-care.ppm");
          lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages-1],"Full")),LV_EVENT_CLICKED,NULL);frame(100);
          assert(global_settings.hiby_charge_limit_voltage==1); }
        page_pop();frame(500);global_settings.hiby_charge_limit_voltage=0;
        tz_page();frame(500);shot("time-zones.ppm");page_pop();frame(500);
        global_settings.sloppy_timezone[0]='\0';tz_apply();

        /* output card */
        sound_page();frame(500);lv_obj_scroll_to_y(lv_obj_get_child(pages[npages-1],1),900,LV_ANIM_OFF);frame(300);
        shot("sound-output.ppm");assert(find_label(pages[npages-1],"Low gain"));page_pop();frame(500);
        {   /* volume: percentages round-trip on the half-dB grid, the top and
             * bottom hold, the button step and holding it, each output's own level */
            int saved_step=global_settings.sloppy_volume_step,saved_vol=global_status.volume,saved_status=test_status;
            for(int p=0;p<=100;p++){global_status.volume=volume_from_pct(p);assert(volume_pct()==p&&(global_status.volume+700)%5==0);}
            test_status=AUDIO_STATUS_PLAY;
            global_status.volume=sound_max(SOUND_VOLUME);for(int i=0;i<10;i++)volume_key(1);
            assert(global_status.volume==sound_max(SOUND_VOLUME)&&volume_pct()==100);
            volume_key(-1);assert(global_status.volume==sound_max(SOUND_VOLUME)-5);   /* the first press down counts */
            global_settings.sloppy_volume_step=4;global_status.volume=volume_from_pct(50);int v=global_status.volume;
            volume_key(1);assert(global_status.volume==v+20);volume_key(-1);volume_key(-1);assert(global_status.volume==v-20);
            global_status.volume=sound_min(SOUND_VOLUME)+10;volume_key(-1);assert(global_status.volume==sound_min(SOUND_VOLUME));
            int held[2];
            for(int k=0;k<2;k++){   /* held half a second: step 4 climbs about as far as step 1 */
                global_settings.sloppy_volume_step=k?4:1;global_status.volume=v=volume_from_pct(20);
                current_tick+=100;handle_button(BUTTON_UP);
                for(int i=0;i<10;i++){current_tick+=5;handle_button(BUTTON_UP|BUTTON_REPEAT);}
                handle_button(BUTTON_UP|BUTTON_REL);held[k]=global_status.volume-v;}
            printf("volume keys held 500 ms: step 1 +%d, step 4 +%d (0.1 dB)\n",held[0],held[1]);
            assert(held[0]==55&&held[1]==60);
            global_settings.sloppy_volume_step=1;
            /* the jack and Bluetooth keep their own level; a first Bluetooth
             * pair never starts louder than half */
            sl_bt_demo_on=true;sl_bt_demo_index=-1;global_status.sloppy_volume_on_bt=0;
            global_status.sloppy_bt_volume=global_status.sloppy_jack_volume=1;
            global_status.volume=volume_from_pct(80);sl_bt_volume_follow();assert(volume_pct()==80);
            sl_bt_demo_index=0;sl_bt_volume_follow();
            assert(global_status.sloppy_volume_on_bt&&volume_pct()==50&&global_status.sloppy_jack_volume==volume_from_pct(80));
            assert(!strncmp(lv_label_get_text(sb_vol),LV_SYMBOL_BLUETOOTH,strlen(LV_SYMBOL_BLUETOOTH)));
            global_status.volume=volume_from_pct(30);
            sl_bt_demo_index=-1;sl_bt_volume_follow();   /* headphones gone: wired earphones get 80, not 30 or more */
            assert(!global_status.sloppy_volume_on_bt&&volume_pct()==80);
            assert(!strncmp(lv_label_get_text(sb_vol),LV_SYMBOL_VOLUME_MAX,strlen(LV_SYMBOL_VOLUME_MAX)));
            global_status.volume=volume_from_pct(90);
            sl_bt_demo_index=0;sl_bt_volume_follow();assert(volume_pct()==30);   /* back: its own 30, not the jack's 90 */
            int c=setvol_calls;sl_bt_demo_index=1;sl_bt_volume_follow();          /* another pair is told the level */
            assert(volume_pct()==30&&setvol_calls==c+1);
            sl_bt_volume_follow();assert(setvol_calls==c+1);                      /* nothing new: nothing sent */
            sl_bt_demo_index=-1;global_status.sloppy_volume_on_bt=1;              /* powered off on Bluetooth */
            sl_bt_volume_startup();assert(!global_status.sloppy_volume_on_bt&&volume_pct()==90&&global_status.sloppy_bt_volume==volume_from_pct(30));
            global_status.sloppy_bt_volume=1;global_status.volume=volume_from_pct(20);
            sl_bt_demo_index=0;sl_bt_volume_follow();assert(volume_pct()==20);   /* a quiet level is never raised */
            sl_bt_demo_index=-1;sl_bt_volume_follow();global_status.sloppy_bt_volume=global_status.sloppy_jack_volume=1;
            /* the player as a computer's USB DAC (R11): a level of its own, the first
             * one no louder than -30 dB, and the jack's back when it ends */
            global_status.sloppy_dac_volume=1;global_status.volume=volume_from_pct(90);
            sl_usb_dac_demo=true;sl_bt_volume_follow();
            assert(global_status.sloppy_volume_on_bt==2&&global_status.volume==-300&&global_status.sloppy_jack_volume==volume_from_pct(90));
            assert(!strncmp(lv_label_get_text(sb_vol),LV_SYMBOL_USB,strlen(LV_SYMBOL_USB)));
            global_status.volume=-250;sl_usb_dac_demo=false;sl_bt_volume_follow();
            assert(!global_status.sloppy_volume_on_bt&&volume_pct()==90&&global_status.sloppy_dac_volume==-250);
            sl_usb_dac_demo=true;sl_bt_volume_follow();assert(global_status.volume==-250);   /* plugged in again: its own level */
            sl_bt_demo_index=0;sl_bt_volume_follow();assert(global_status.sloppy_volume_on_bt==2);   /* the DAC session outranks headphones */
            sl_bt_demo_index=-1;sl_usb_dac_demo=false;sl_bt_volume_follow();
            global_status.sloppy_dac_volume=1;global_status.volume=-450;   /* a quiet jack is never raised */
            sl_usb_dac_demo=true;sl_bt_volume_follow();assert(global_status.volume==-450);sl_usb_dac_demo=false;sl_bt_volume_follow();
            global_status.sloppy_dac_volume=1;global_settings.sloppy_dac_limit=20;global_status.volume=volume_from_pct(90);   /* nor above its maximum */
            sl_usb_dac_demo=true;sl_bt_volume_follow();assert(global_status.volume==volume_from_pct(20));
            global_status.volume=volume_from_pct(15);sl_usb_dac_demo=false;   /* powered off as a DAC: the jack's level at the next start */
            sl_bt_volume_startup();assert(!global_status.sloppy_volume_on_bt&&volume_pct()==90&&global_status.sloppy_dac_volume==volume_from_pct(15));
            global_settings.sloppy_dac_limit=100;global_status.sloppy_dac_volume=global_status.sloppy_bt_volume=global_status.sloppy_jack_volume=1;
            /* Settings > Sound shows the step */
            sound_page();frame(500);lv_obj_t *pg=pages[npages-1];
            lv_obj_t *row=lv_obj_get_parent(find_label(pg,"Volume button step"));assert(row);
            assert(!strcmp(lv_label_get_text(lv_obj_get_child(row,1)),"0.5 dB"));
            lv_obj_t *sl=lv_obj_get_child(row,2);lv_slider_set_value(sl,3,LV_ANIM_OFF);lv_obj_send_event(sl,LV_EVENT_VALUE_CHANGED,NULL);
            assert(global_settings.sloppy_volume_step==3&&!strcmp(lv_label_get_text(lv_obj_get_child(row,1)),"1.5 dB"));
            lv_slider_set_value(sl,10,LV_ANIM_OFF);lv_obj_send_event(sl,LV_EVENT_VALUE_CHANGED,NULL);
            assert(!strcmp(lv_label_get_text(lv_obj_get_child(row,1)),"5 dB"));
            lv_obj_scroll_to_view_recursive(row,LV_ANIM_OFF);frame(300);shot("settings-volume-step.ppm");
            lv_slider_set_value(sl,1,LV_ANIM_OFF);lv_obj_send_event(sl,LV_EVENT_VALUE_CHANGED,NULL);frame(100);
            sl_text_step=1;page_pop();frame(400);sound_page();frame(500);pg=pages[npages-1];
            row=lv_obj_get_parent(find_label(pg,"Volume button step"));lv_obj_scroll_to_view_recursive(row,LV_ANIM_OFF);frame(300);
            shot("settings-volume-step-min.ppm");sl_text_step=0;
            page_pop();frame(500);
            global_settings.sloppy_volume_step=saved_step;global_status.volume=saved_vol;test_status=saved_status;
            printf("PASS: volume grid, top and bottom, button step and hold, jack, Bluetooth and USB DAC levels.\n");
        }
        {   /* hardware: the signal path, the CS43131's filter, dither, a tap on a switch's label */
            sl_bt_known=false;   /* through the jack, whatever earlier tests left routed */
            test_status=0;sound_page();frame(500);lv_obj_t *pg=pages[npages-1];
            assert(find_label(pg,"SIGNAL PATH"));
            test_status=AUDIO_STATUS_PLAY;test_track.frequency=44100;frame(1100);
            bool found=has_text(pg,"no resampling")&&has_text(pg,"CS43131");
            assert(found);
            shot("sound-signal.ppm");
            test_track.frequency=352800;frame(1100);
            assert(!find_label(pg,"x"));   /* page still alive */
            lv_area_t a;
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
            lv_obj_t *ss=find_label(pg,"Super slow");assert(ss);
            lv_obj_scroll_to_view_recursive(ss,LV_ANIM_OFF);frame(100);
            lv_obj_get_coords(lv_obj_get_parent(ss),&a);
            touch_at((a.x1+a.x2)/2,(a.y1+a.y2)/2,true);touch_at((a.x1+a.x2)/2,(a.y1+a.y2)/2,false);frame(100);
            assert(test_sound_setting==SOUND_FILTER_ROLL_OFF&&test_sound_value==4&&global_settings.roll_off==4);
#else
            assert(!find_label(pg,"DAC filter"));
#endif
            lv_obj_t *dl=find_label(pg,"Dither to 16-bit");assert(dl);
            lv_obj_scroll_to_view_recursive(dl,LV_ANIM_OFF);frame(100);
            lv_obj_get_coords(dl,&a);bool was=global_settings.dithering_enabled;
            touch_at(a.x1+10,(a.y1+a.y2)/2,true);touch_at(a.x1+10,(a.y1+a.y2)/2,false);frame(100);
            assert(global_settings.dithering_enabled==!was&&test_dither==!was);
            shot("sound-hardware.ppm");
#ifdef AUDIOHW_HAVE_FILTER_ROLL_OFF
            global_settings.roll_off=0;
#endif
            global_settings.dithering_enabled=was;test_status=0;test_track.frequency=0;
            page_pop();frame(500);
        }
        {   /* Mono (R14): the DSP's mono mode; the width slider gets the custom
             * mode it needs (it did nothing in plain stereo) and hides in mono */
            sound_page();frame(500);lv_obj_t *pg=pages[npages-1];
            lv_obj_t *ml=find_label(pg,"Mono");assert(ml&&snd_width_row&&!lv_obj_has_flag(snd_width_row,LV_OBJ_FLAG_HIDDEN));
            lv_obj_scroll_to_view_recursive(ml,LV_ANIM_OFF);frame(100);lv_area_t a;lv_obj_get_coords(ml,&a);
            touch_at(a.x1+10,(a.y1+a.y2)/2,true);touch_at(a.x1+10,(a.y1+a.y2)/2,false);frame(300);
            assert(global_settings.channel_config==SOUND_CHAN_MONO&&test_sound_setting==SOUND_CHANNELS&&test_sound_value==SOUND_CHAN_MONO);
            assert(lv_obj_has_flag(snd_width_row,LV_OBJ_FLAG_HIDDEN));
            snd_mono(0);assert(global_settings.channel_config==SOUND_CHAN_STEREO&&!lv_obj_has_flag(snd_width_row,LV_OBJ_FLAG_HIDDEN));
            snd_width(150);assert(global_settings.channel_config==SOUND_CHAN_CUSTOM&&test_sound_value==SOUND_CHAN_CUSTOM);
            snd_mono(1);assert(global_settings.channel_config==SOUND_CHAN_MONO);
            snd_mono(0);assert(global_settings.channel_config==SOUND_CHAN_CUSTOM);   /* back to the width it had */
            snd_width(100);assert(global_settings.channel_config==SOUND_CHAN_STEREO);
            global_settings.channel_config=SOUND_CHAN_KARAOKE;snd_width(120);assert(global_settings.channel_config==SOUND_CHAN_KARAOKE);   /* Classic's own modes stay */
            snd_mono(1);assert(global_settings.channel_config==SOUND_CHAN_MONO);snd_mono(0);snd_width(100);
            page_pop();frame(400);
            assert(global_settings.channel_config==SOUND_CHAN_STEREO&&!snd_width_row);
            printf("PASS: Mono, and the stereo width in the mode that applies it.\n");
        }
        printf("PASS: language, text size, Wi-Fi, time zones, output, signal path, dither.\n");
    }
    {   /* Radio from this song: the seed first, a queued song kept right
         * after it, no repeats, no artist twice running, more added near
         * the end, Stop radio and playing anything else end it. Its own
         * small library: two artists with two albums each, a compilation,
         * two loners; small enough that the whole library tops it up. */
        struct sl_lib saved_lib=sl_lib;
        queued=0;started=-1;   /* the queue held the big library's songs (the fuzz leaves some) */
        static struct sl_track rt[28];static struct sl_album ra[7];static int rtr[7][4],rby[7]={5,6,0,1,2,3,4},rlist[28];
        static char rpath[28][24];
        static const char *const rart[7]={"Mira Sol","Mira Sol","Neon Harbor","Neon Harbor","Various Artists","Kite","Lumen"};
        static const int ryear[7]={2010,2012,2011,2003,2011,1975,2020};
        for(int a=0;a<7;a++){ra[a]=(struct sl_album){"Album",rart[a],ryear[a],4,0,rtr[a]};
            for(int k=0;k<4;k++){int t=a*4+k;rtr[a][k]=t;snprintf(rpath[t],sizeof rpath[t],"/r/a%d/t%d.mp3",a,k);
                rt[t]=(struct sl_track){rpath[t],"Song",a,k+1,1,200000,0,false};tsplit(&rt[t]);rlist[t]=t;}}
        sl_lib.tracks=rt;sl_lib.n_tracks=28;sl_lib.albums=ra;sl_lib.n_albums=7;sl_lib.albums_by_artist=rby;
        int nt=28,seed=0;
        global_settings.playlist_shuffle=false;
        test_status=AUDIO_STATUS_PLAY;play_list(rlist,12,3,false);
        int qt=20;q_add(qt);
        {   /* Spotify top artists (opt-in on the computer): Kite leans in */
            const char *top="#SLOPPY-SPOTIFY-TOP\t1\n#exported\t2026-10-13\nkite\nNobody Here\n";
            int fd=open(SR_TOP_FILE,O_WRONLY|O_CREAT|O_TRUNC);assert(fd>=0);write(fd,top,strlen(top));sim_close(fd);
        }
        sr_start(seed);
        assert(sr_on()&&started==0&&queue_tracks[0]==seed);
        assert(sr.weight[5]>=3&&sr.weight[6]<=1);            /* Kite yes; Lumen only the top-up */
        remove(SR_TOP_FILE);
        /* the queue plays first, whatever else was queued before (a fuzz run
         * can leave some), and radio never picks a queued song again */
        int qrun=0;while(1+qrun<queued&&queue_flags[1+qrun])qrun++;
        bool qt_first=false;for(int i=1;i<=qrun;i++)if(queue_tracks[i]==qt)qt_first=true;
        assert(qt_first);
        int *seen=calloc(nt,sizeof *seen);int repeats=0,runs=0,prev=-2;
        for(int i=0;i<queued;i++){int t=queue_tracks[i];if(seen[t]++)repeats++;
            int ar=sr.artist[sl_lib.tracks[t].album];if(i>1&&ar==prev&&sr_artist_usable(sl_lib.albums[sl_lib.tracks[t].album].artist))runs++;prev=ar;}
        int first=queued;
        started=queued-1;sr_poll();                          /* on the last song: more come */
        int more=queued-first;
        for(int i=first;i<queued;i++)if(seen[queue_tracks[i]]++)repeats++;
        printf("radio: %d to start, %d added at the end, %d repeats, %d same-artist runs, pool %d\n",first,more,repeats,runs,sr.pool_tracks);
        assert(more>0&&more<=SR_MORE&&repeats==0);
        assert(runs<=1);                                     /* only once the near songs ran out */
        free(seen);
        static const char stations[]="Wire\thttp://example.org/w\n";
        {   /* a power-off: the memory goes, the saved seed stays; the first
             * playback after start-up carries radio on, nothing listed again;
             * internet radio's saved stations are not touched */
            fake_put(ST_SAVED,stations,sizeof stations-1);
            assert(fake_find(SR_SAVE)>=0);
            int listed=queued;sr.active=false;sr_free();sr_resume_checked=false;
            sr_poll();
            assert(sr_on()&&sr.seed==seed);
            for(int i=0;i<listed;i++){int t=queue_tracks[i];assert(sr.played[t/32]&(1u<<(t%32)));}
            /* a list that is not radio's: no radio, and the note goes */
            sr.active=false;sr_free();sr_resume_checked=false;
            int saved0=queue_tracks[0];queue_tracks[0]=(seed+1)%nt;sr_poll();
            assert(!sr_on()&&fake_find(SR_SAVE)<0);queue_tracks[0]=saved0;
            sr_start(seed);assert(sr_on()&&fake_find(SR_SAVE)>=0);
        }
        /* Up next says it is radio, and Stop radio stops it */
        uq_show();frame(500);shot("radio-up-next.ppm");
        assert(has_text(uq,"RADIO")&&has_text(uq,"Stop radio")&&!has_text(uq,"NEXT UP"));
        np_show();frame(400);assert(strstr(lv_label_get_text(np_fmt),"Radio"));np_hide(0);frame(400);
        lv_obj_t *sb=lv_obj_get_parent(find_label(uq,"Stop radio"));lv_obj_send_event(sb,LV_EVENT_CLICKED,NULL);frame(400);
        assert(!sr_on()&&has_text(uq,"NEXT UP")&&fake_find(SR_SAVE)<0);
        assert(fake_equals(ST_SAVED,stations));remove(ST_SAVED);   /* through resume, start and stop */
        int n=queued;started=queued-1;sr_poll();assert(queued==n);   /* stopped: nothing more */
        uq_close();frame(500);
        /* playing something else ends it */
        sr_start(seed);assert(sr_on());play_list(sl_lib.songs_by_title,3,0,false);assert(!sr_on());
        /* a one-song library has nothing to play around it */
        static int one_by_artist[1]={0};
        sl_lib.n_tracks=1;sl_lib.n_albums=1;ra[0].n_tracks=1;sl_lib.albums_by_artist=one_by_artist;sr_start(0);assert(!sr_on());
        ra[0].n_tracks=4;sl_lib=saved_lib;queued=0;started=-1;
        test_status=0;
        printf("PASS: radio from a song, queue kept, no repeats, refills, stops.\n");
    }
    {   /* Spotify playlists: an exported list is matched to the card in
         * order, saved as a playlist, saved again in place (no second copy,
         * even renamed), and a playlist of the same name that is someone's
         * own is left alone */
        struct sl_lib saved_lib=sl_lib;char saved_dir[sizeof global_settings.playlist_catalog_dir];
        strlcpy(saved_dir,global_settings.playlist_catalog_dir,sizeof saved_dir);
        static struct sl_track st[4];static struct sl_album sa[2];static int str0[2]={0,1},str1[2]={2,3};
        sa[0]=(struct sl_album){"Harbor Lights","Mira Sol",2010,2,0,str0};
        sa[1]=(struct sl_album){"City","Kite",2012,2,0,str1};
        st[0]=(struct sl_track){"/m/a/night.flac","Night Tide",0,1,1,241000,0,true};
        st[1]=(struct sl_track){"/m/a/live.flac","Night Tide - Live",0,2,1,268000,0,true};
        st[2]=(struct sl_track){"/m/b/silent.mp3","Silent City",1,1,1,300000,0,false};
        st[3]=(struct sl_track){"/m/b/long.mp3","Long Song",1,2,1,400000,0,false};
        for(int i=0;i<4;i++)tsplit(&st[i]);
        sl_lib.tracks=st;sl_lib.n_tracks=4;sl_lib.albums=sa;sl_lib.n_albums=2;
        strlcpy(global_settings.playlist_catalog_dir,"sp/Playlists",sizeof global_settings.playlist_catalog_dir);
        const char *tsv="#SLOPPY-SPOTIFY\t1\n#id\t37i9road\n#name\tRoad Trip\n#exported\t2026-10-12\n"
                  "Silent City\tKite\tCity\t300500\t\n"
                  "Unknown Song\tNobody\tNowhere\t200000\t\n"
                  "Night Tide - 2011 Remaster\tMira Sol\tHarbor Lights\t241000\tUSX\n"
                  "Night Tide - Acoustic\tMira Sol\tSessions\t230000\t\n";
        int fd=open(SPO_DIR "/37i9road.tsv",O_WRONLY|O_CREAT|O_TRUNC);assert(fd>=0);write(fd,tsv,strlen(tsv));sim_close(fd);
        {const char *top="#SLOPPY-SPOTIFY-TOP\t1\nMira Sol\nNobody\n";
         int fd2=open(SPO_DIR "/top-artists.txt",O_WRONLY|O_CREAT|O_TRUNC);write(fd2,top,strlen(top));sim_close(fd2);}
        sp_page();frame(400);lv_obj_t *pg=pages[npages-1];
        assert(find_label(pg,"1 of your 2 Spotify top artists are on this player. Radio leans towards them."));
        lv_obj_t *row=find_label(pg,"Road Trip");
        assert(row);
        assert(find_label(pg,"4 songs, read 2026-10-12"));
        lv_obj_send_event(lv_obj_get_parent(row),LV_EVENT_CLICKED,NULL);frame(400);
        assert(spo.n==4&&spo.found==2&&spo.match[0]==2&&spo.match[1]==-1&&spo.match[2]==0&&spo.match[3]==-1);
        pg=pages[npages-1];assert(find_label(pg,"2 of 4 songs are on this player")&&find_label(pg,"Save as playlist"));
        assert(find_label(pg,"NOT ON THIS PLAYER"));
        shot("spotify-match.ppm");
        sp_save_cb(NULL);
        char buf[600]={0};fd=open("sp/Playlists/Road Trip.m3u8",O_RDONLY);assert(fd>=0);read(fd,buf,sizeof buf-1);sim_close(fd);
        assert(!strncmp(buf,"#EXTM3U\n#SLOPPY-SPOTIFY:37i9road ",33));
        char *a=strstr(buf,"/m/b/silent.mp3"),*b=strstr(buf,"/m/a/night.flac");
        assert(a&&b&&a<b&&!strstr(buf,"live.flac"));   /* the playlist's order; never the live take */
        /* renamed by hand, then refreshed: the same file, no copy */
        rename("sp/Playlists/Road Trip.m3u8","sp/Playlists/Renamed.m3u8");
        page_pop();frame(400);lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages-1],"Road Trip")),LV_EVENT_CLICKED,NULL);frame(400);
        assert(find_label(pages[npages-1],"Update playlist"));
        sp_save_cb(NULL);
        assert(pl_exists("sp/Playlists/Renamed.m3u8")&&!pl_exists("sp/Playlists/Road Trip.m3u8"));
        /* someone's own "Road Trip": left alone, the import gets (Spotify) */
        remove("sp/Playlists/Renamed.m3u8");
        fd=open("sp/Playlists/Road Trip.m3u8",O_WRONLY|O_CREAT|O_TRUNC);write(fd,"/m/b/long.mp3\n",14);sim_close(fd);
        sp_save_cb(NULL);
        fd=open("sp/Playlists/Road Trip.m3u8",O_RDONLY);memset(buf,0,sizeof buf);read(fd,buf,sizeof buf-1);sim_close(fd);
        assert(!strcmp(buf,"/m/b/long.mp3\n")&&pl_exists("sp/Playlists/Road Trip (Spotify).m3u8"));
        page_pop();frame(400);page_pop();frame(400);
        /* nothing exported: it says what is needed, offers nothing to tap */
        remove(SPO_DIR "/37i9road.tsv");remove(SPO_DIR "/top-artists.txt");
        sp_page();frame(400);assert(find_label(pages[npages-1],"can't sign in")||has_text(pages[npages-1],"No Spotify playlists"));
        shot("spotify-empty.ppm");page_pop();frame(400);
        remove("sp/Playlists/Road Trip.m3u8");remove("sp/Playlists/Road Trip (Spotify).m3u8");
        sl_lib=saved_lib;strlcpy(global_settings.playlist_catalog_dir,saved_dir,sizeof global_settings.playlist_catalog_dir);
        printf("PASS: Spotify playlists matched in order, saved, refreshed in place, own playlists kept.\n");
    }
    {   /* Shuffle all starts on a random song, not the first by title */
        int firsts=0,n=sl_lib.n_tracks<200?sl_lib.n_tracks:200;
        for(int i=0;i<8;i++){current_tick+=137;assert(play_list(sl_lib.songs_by_title,n,0,true));if(started==0)firsts++;}
        assert(n<4 || firsts<3);
        /* the developer overlay's switch shows once the version line is tapped */
        while(npages>1){page_pop();frame(400);}
        system_page();frame(400);
        lv_obj_t *pg=pages[npages-1];
        lv_obj_t *dl=find_label(pg,"Performance overlay");assert(dl);
        lv_obj_t *dev=lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(dl)));
        assert(lv_obj_has_flag(dev,LV_OBJ_FLAG_HIDDEN));
        lv_obj_t *ver=find_label(pg,sl_update_footer());assert(ver);
        lv_obj_send_event(ver,LV_EVENT_CLICKED,NULL);frame(400);
        assert(!lv_obj_has_flag(dev,LV_OBJ_FLAG_HIDDEN));
        lv_obj_t *sw=lv_obj_get_child(lv_obj_get_parent(dl),2);assert(lv_obj_check_type(sw,&lv_switch_class));
        lv_obj_add_state(sw,LV_STATE_CHECKED);lv_obj_send_event(sw,LV_EVENT_VALUE_CHANGED,NULL);
        assert(global_settings.sloppy_dev_overlay && dev_hud.bar);
        for(int i=0;i<70;i++){lv_obj_invalidate(pg);frame(16);}
        devhud_tick(NULL);frame(0);
        printf("overlay: %s | %s | %s\n",lv_label_get_text(dev_hud.cell[0]),lv_label_get_text(dev_hud.cell[1]),lv_label_get_text(dev_hud.cell[2]));
        assert(!strncmp(lv_label_get_text(dev_hud.cell[0]),"FPS",3));
        shot_now("dev-overlay.ppm");
        lv_obj_remove_state(sw,LV_STATE_CHECKED);lv_obj_send_event(sw,LV_EVENT_VALUE_CHANGED,NULL);
        assert(!global_settings.sloppy_dev_overlay && !dev_hud.bar);
        page_pop();frame(400);
        printf("PASS: shuffle start, developer overlay.\n");
    }
    test_status=AUDIO_STATUS_PLAY;
    sl_track_path(0,test_track.path,sizeof test_track.path);
    if (!np_open) np_show();
    if (!uq_open) uq_show();
    assert(np_open && uq_open);
    audio_stop();periodic(NULL);
    assert(!np_open && !uq_open);
    printf("PASS: icon toggle, 10000-song bounded rows (%d objects), 5000-album/artist paging, folder paging, missing-file safety, 50 navigation cycles, queue clear.\n",before);
    {   /* the owner's simulator: tiles, red roots, a dark red accent, large text */
        while(npages>1){page_pop();frame(400);}
        global_settings.sloppy_home_style=1;global_settings.sloppy_home_wallpaper=7;global_settings.sloppy_lock_wallpaper=7;
        global_settings.sloppy_accent_color=0x75302c;sl_text_step=1;
        appearance_reload((void *)1);frame(800);shot("user-home.ppm");
        qp_drag_to(430);frame(50);qp_release(430,900);frame(500);shot("user-cc.ppm");
        qp_close();frame(500);
        /* settings controls stay visible in any accent and theme */
        static const uint32_t accents[]={0x72d8cd,0x75302c,0x3a5fcd,0xffd24a,0x000000,0xffffff};
        for(int th=0;th<3;th++)for(unsigned a=0;a<sizeof accents/sizeof accents[0];a++){
            global_settings.sloppy_theme=th;global_settings.sloppy_accent_color=accents[a];
            assert(sl_contrast_x10(C_ACCENT_UI,C_CARD)>=30||(accents[a]==0x000000&&th!=1)||(accents[a]==0xffffff&&th==1));
            assert(sl_contrast_x10(C_ACCENT_TXT,C_CARD)>=45||(accents[a]==0x000000&&th!=1)||(accents[a]==0xffffff&&th==1));
            assert(sl_contrast_x10(C_DIM,C_CARD)>=45&&sl_contrast_x10(sl_ink_on(C_ACCENT_UI),C_ACCENT_UI)>=45);
        }
        global_settings.sloppy_theme=0;global_settings.sloppy_accent_color=0x75302c;
        buttons_page();frame(500);shot("user-buttons.ppm");page_pop();frame(500);
        sound_page();frame(500);lv_obj_scroll_to_y(lv_obj_get_child(pages[npages-1],1),900,LV_ANIM_OFF);frame(300);shot("user-sound.ppm");page_pop();frame(500);
        global_settings.sloppy_theme=1;global_settings.sloppy_accent_color=0x72d8cd;appearance_reload((void *)1);frame(500);
        buttons_page();frame(500);shot("light-buttons.ppm");page_pop();frame(500);
        global_settings.sloppy_theme=0;appearance_reload((void *)1);frame(500);
        sl_text_step=0;
    }
    {   /* the visualizer: Now Playing's button, music through the hook, every
         * look drawn, a tap and swipes, the looks moving on by themselves,
         * paused it rests, Done; then open and shut over and over, and
         * nothing is left behind */
        while(npages>1){page_pop();frame(400);}
        home_scr=lv_screen_active();
        int saved_status=test_status;test_status=AUDIO_STATUS_PLAY;test_chan=CHANNEL_PLAYING;
        strlcpy(test_track.path,"/Music/visualizer.mp3",sizeof test_track.path);
        test_track.title="Blue Hours";test_track.artist="Mira Sol";
        static const float vf[]={55,110,220,440,880,1760,3520,7040};
        float va[8];
        #define VZ_FEED(i) do{for(int j_=0;j_<8;j_++)va[j_]=(2200+1600*sinf((i)*0.37f+j_*1.3f))/(1+j_*0.2f); \
            va[0]=(i)%8<2?11000:1500;va[1]=(i)%8<2?6000:1800;test_feed(8,vf,va);}while(0)   /* a kick every 8 frames */
        np_show();frame(500);assert(np_open && vz_btn && !lv_obj_has_flag(vz_btn,LV_OBJ_FLAG_HIDDEN));
        shot("visualizer-now-playing.ppm");
        lv_obj_send_event(vz_btn,LV_EVENT_CLICKED,NULL);frame(100);
        assert(vz.active && lv_screen_active()==vz.scr && test_hook==sp_hook && vz.buf[0] && vz.buf[1] && vz.timer);
        assert(sp.bars==VZ_BANDS && cf_is_open() && ui_screen()==home_scr);
        for(int i=0;i<40;i++){VZ_FEED(i);frame(33);}
        assert(lv_obj_get_style_opa(vz.top,0)>200);shot_now("visualizer-chrome.ppm");   /* the name and the song, at first */
        assert(lv_label_get_text(vz.hint)[0]);   /* and, the first time, how to play with it */
        uint32_t beats0=vz.beats;
        for(int l=0;l<VZ_LOOKS;l++){
            vz_go(l,0);vz.next_ms=lv_tick_get()+600000;
            for(int i=0;i<100;i++){VZ_FEED(i);frame(33);}
            assert(vz.to==l && !vz.settled && lv_obj_get_style_opa(vz.top,0)==0);
            char name[64];snprintf(name,sizeof name,"visualizer-%s.ppm",vz_looks[l].name);
            for(char *c=name;*c;c++)if(*c>='A'&&*c<='Z')*c=(char)(*c+32);
            shot_now(name);
            /* something to see, and not white-out: the mean level of the picture */
            assert(vz.w==VZ_W && vz.ups==1);
            uint64_t sum=0;for(int i=0;i<vz.w*vz.h;i++){uint32_t v=vz.buf[vz.cur][i];sum+=(v>>16&255)+(v>>8&255)+(v&255);}
            double mean=sum/(3.0*vz.w*vz.h);printf("visualizer %-13s mean level %5.1f\n",vz_looks[l].name,mean);
            assert(mean>3 && mean<170);
        }
        assert(vz.beats-beats0>=VZ_LOOKS*8);   /* the kicks were heard */
        /* a player too slow for the full picture: halved where it stands,
         * then grown four times; the next open starts there */
        vz_go(0,0);for(int i=0;i<60;i++){VZ_FEED(i);frame(33);}
        vz_halve();assert(vz.w==VZ_W/2 && vz.h==VZ_H/2 && vz.ups==2 && vz.gw==15 && vz_low);
        for(int i=0;i<60;i++){VZ_FEED(i);frame(33);}
        shot_now("visualizer-nebula-half.ppm");
        vz_go(2,0);for(int i=0;i<100;i++){VZ_FEED(i);frame(33);}   /* folded: a quarter moved, the rest mirrored */
        shot_now("visualizer-kaleidoscope-half.ppm");
        vz_low=false;
        /* a look half way into the next, folds and all */
        vz_go(1,0);for(int i=0;i<30;i++){VZ_FEED(i);frame(33);}
        vz_go(2,3000);for(int i=0;i<45;i++){VZ_FEED(i);frame(33);}
        assert(vz.now.fold_x>0.1f && vz.now.fold_x<0.99f && vz.now_wave[VZ_LINE]>0.05f && vz.now_wave[VZ_KNOT]>0.05f);
        shot_now("visualizer-blend.ppm");
        /* playing with it: music on, a finger on the picture */
        int vt=0;
        #define VZ_TOUCH(x,y,dn) do{VZ_FEED(vt);vt++;touch_at((x),(y),(dn));}while(0)
        vz_close();frame(100);lv_obj_send_event(vz_btn,LV_EVENT_CLICKED,NULL);frame(100);   /* the full picture again */
        assert(vz.active && vz.ups==1);
        vz_go(0,0);vz.next_ms=lv_tick_get()+600000;for(int i=0;i<100;i++){VZ_FEED(i);frame(33);}
        assert(lv_obj_get_style_opa(vz.top,0)==0 && lv_obj_has_flag(vz.top,LV_OBJ_FLAG_HIDDEN));   /* faded: no unseen close button */
        int was=vz.to;
        /* a slow drag round and round stirs: the trail and the curl follow, the look stays */
        for(int i=0;i<=36;i++){float a=i*0.35f;VZ_TOUCH(240+(int)(130*cosf(a)),470+(int)(130*sinf(a)),true);
            if(i==4)assert(vz.moved && !vz.fing.hold && vz.fing.stir>0.99f);}
        assert(vz.to==was && vz.fing.down && fabsf(vz.fing.spin)>0.02f);
        shot_now("visualizer-touch-stir.ppm");
        VZ_TOUCH(240+(int)(130*cosf(37*0.35f)),470+(int)(130*sinf(37*0.35f)),false);
        assert(vz.to==was && vz.active && !vz.fing.down && lv_obj_has_flag(vz.top,LV_OBJ_FLAG_HIDDEN));
        for(int i=0;i<40;i++){VZ_FEED(i);frame(33);}assert(vz.fing.stir<0.05f);   /* and it fades once the finger lifts */
        /* a slow sideways drag and one down the middle stir too: no look change, no close */
        for(int x=60;x<=420;x+=12)VZ_TOUCH(x,560,true);VZ_TOUCH(420,560,false);
        assert(vz.to==was && vz.active);
        for(int y=200;y<=700;y+=60)VZ_TOUCH(240,y,true);VZ_TOUCH(240,700,false);
        assert(vz.to==was && vz.active);
        /* a tap splashes a ripple, and pushes out under it; bigger on a beat */
        for(int i=0;i<30;i++){VZ_FEED(i);frame(33);}
        vz.beat=0;VZ_TOUCH(150,600,true);VZ_TOUCH(150,600,false);
        int bi=-1;for(int i=0;i<VZ_BURSTS;i++)if(vz.burst[i].age<0.2f)bi=i;
        assert(bi>=0 && vz.fing.pull<0 && lv_obj_has_flag(vz.top,LV_OBJ_FLAG_HIDDEN));
        float p0=vz.burst[bi].power;
        for(int i=0;i<4;i++){VZ_FEED(i);frame(33);}
        vz.beat=1;VZ_TOUCH(330,300,true);VZ_TOUCH(330,300,false);
        float p1=0;for(int i=0;i<VZ_BURSTS;i++)if(vz.burst[i].age<0.15f)p1=vz.burst[i].power;
        assert(p1>p0+0.3f);
        for(int i=0;i<5;i++){VZ_FEED(i);frame(33);}
        shot_now("visualizer-touch-splash.ppm");
        /* a still press is a black hole: everything drawn in, turning; let go, it bursts out */
        for(int i=0;i<40;i++){VZ_FEED(i);frame(33);}
        VZ_TOUCH(300,260,true);for(int i=0;i<40;i++)VZ_TOUCH(300+(i&1)*6,260,true);   /* a finger is never quite still */
        assert(vz.fing.hold && !vz.moved && vz.fing.pull>0.6f && vz.to==was);
        shot_now("visualizer-touch-hold.ppm");
        VZ_TOUCH(300,260,false);
        assert(!vz.fing.hold || !vz.fing.down);assert(vz.fing.pull<-0.6f);
        for(int i=0;i<6;i++){VZ_FEED(i);frame(33);}
        shot_now("visualizer-touch-release.ppm");
        /* in the mirrored looks the finger plays in every quarter */
        vz_go(2,0);for(int i=0;i<60;i++){VZ_FEED(i);frame(33);}
        for(int i=0;i<=30;i++){float a=i*0.4f;VZ_TOUCH(330+(int)(80*cosf(a)),560+(int)(80*sinf(a)),true);}
        assert(vz.fing.x<vz.w/2.0f && vz.fing.y<vz.h/2.0f);   /* folded into the quarter that moves */
        shot_now("visualizer-touch-kaleidoscope.ppm");
        VZ_TOUCH(330,560,false);
        vz_go(was,0);for(int i=0;i<30;i++){VZ_FEED(i);frame(33);}
        /* a tap in the top band shows the chrome; a tap where its hidden close button was does not close */
        for(int i=0;i<100;i++){VZ_FEED(i);frame(33);}
        assert(lv_obj_has_flag(vz.top,LV_OBJ_FLAG_HIDDEN));
        VZ_TOUCH(44,44,true);VZ_TOUCH(44,44,false);for(int i=0;i<6;i++)frame(33);
        assert(vz.active && lv_obj_get_style_opa(vz.top,0)>200 && !lv_label_get_text(vz.hint)[0]);
        /* quick flicks go to the next look and back */
        VZ_TOUCH(400,420,true);for(int x=320;x>=80;x-=80)VZ_TOUCH(x,420,true);VZ_TOUCH(80,420,false);
        assert(vz.to==(was+1)%VZ_LOOKS && vz.active);
        VZ_TOUCH(80,420,true);for(int x=160;x<=400;x+=80)VZ_TOUCH(x,420,true);VZ_TOUCH(400,420,false);
        assert(vz.to==was && vz.active);
        /* paused, a finger still plays; let go, it settles again */
        test_chan=CHANNEL_PAUSED;for(int i=0;i<VZ_SETTLE+40;i++)frame(33);assert(vz.settled);
        { long q0=flushed_px;for(int y=300;y<=500;y+=20)touch_at(200,y,true);
          assert(!vz.settled && flushed_px>q0);touch_at(200,500,false);
          for(int i=0;i<VZ_SETTLE+60;i++)frame(33);assert(vz.settled); }
        test_chan=CHANNEL_PLAYING;for(int i=0;i<5;i++){VZ_FEED(i);frame(33);}
        #undef VZ_TOUCH
        /* left alone, it moves on by itself */
        uint32_t ch=vz.changes;vz.next_ms=lv_tick_get();for(int i=0;i<3;i++){VZ_FEED(i);frame(33);}
        assert(vz.changes==ch+1 && vz.to!=was);
        /* paused: two seconds to settle, then not a pixel more */
        test_chan=CHANNEL_PAUSED;for(int i=0;i<VZ_SETTLE+10;i++)frame(33);
        assert(vz.settled && test_hook==sp_hook);
        for(int i=0;i<40;i++)frame(100);long px0=flushed_px;for(int i=0;i<30;i++)frame(33);assert(flushed_px==px0);
        test_chan=CHANNEL_PLAYING;for(int i=0;i<5;i++){VZ_FEED(i);frame(33);}assert(!vz.settled && flushed_px>px0);
        /* Done: Now Playing as it was, the hook and the pictures gone */
        lv_obj_t *done=lv_obj_get_child(vz.top,0);lv_obj_send_event(done,LV_EVENT_CLICKED,NULL);frame(300);
        assert(!vz.active && !vz.buf[0] && !vz.buf[1] && !vz.timer && !vz.scr && test_hook==NULL);
        assert(lv_screen_active()==home_scr && np_open && !cf_is_open());
        /* pulled down, it closes too; the power off sheet closes it */
        lv_obj_send_event(vz_btn,LV_EVENT_CLICKED,NULL);frame(100);assert(vz.active);
        touch_at(240,60,true);for(int y=120;y<=600;y+=60)touch_at(240,y,true);touch_at(240,600,false);   /* from the top band */
        assert(!vz.active && test_hook==NULL && np_open);
        lv_obj_send_event(vz_btn,LV_EVENT_CLICKED,NULL);frame(100);assert(vz.active);
        po_offer();frame(300);assert(!vz.active && test_hook==NULL);po_close_now();frame(200);
        {   /* the plain glass (no memory for the frost): a pill's rim is its round ends,
             * not an arc of LV_RADIUS_CIRCLE (the R1 crashed holding power) */
            lv_obj_t *g=sl_glass_pane(lv_layer_top(),300,76,38);lv_obj_center(g);
            lv_obj_invalidate(g);lv_refr_now(disp);frame(50);
            lv_obj_delete(g);frame(50);
            printf("PASS: the plain glass pane draws its rim with round ends.\n");
        }
        /* drawn in place, as on the R1 */
        bool saved_ok=sl_direct_ok;sl_direct_ok=true;sl_set_rotation(LV_DISPLAY_ROTATION_0);frame(100);
        lv_obj_send_event(vz_btn,LV_EVENT_CLICKED,NULL);for(int i=0;i<20;i++){VZ_FEED(i);frame(33);}
        assert(vz.active && sl_direct && vz.paint.n>0);
        vz_close();frame(100);
        sl_direct_ok=saved_ok;sl_set_rotation(LV_DISPLAY_ROTATION_0);frame(100);
        /* open, swipe, close, again and again: no objects, memory or hook left */
        uint32_t kids=lv_obj_get_child_count(home_scr),top_kids=lv_obj_get_child_count(lv_layer_top());
        malloc_statistics_t m0,m1;malloc_zone_statistics(NULL,&m0);
        for(int r=0;r<25;r++){
            vz_open();for(int i=0;i<6;i++){VZ_FEED(i);frame(33);}
            if(r&1){touch_at(400,420,true);touch_at(200,420,true);touch_at(80,420,false);}
            vz_go((r*3)%VZ_LOOKS,r&2?0:900);for(int i=0;i<4;i++){VZ_FEED(i);frame(33);}
            if(r%3==0)cf_close();else vz_close();
            frame(100);
            assert(!vz.active && test_hook==NULL && !vz.buf[0]);
        }
        frame(3000);malloc_zone_statistics(NULL,&m1);
        printf("visualizer: 25 opens, heap %+ld KiB\n",((long)m1.size_in_use-(long)m0.size_in_use)/1024);
        assert(lv_obj_get_child_count(home_scr)==kids && lv_obj_get_child_count(lv_layer_top())==top_kids);
#if !__has_feature(address_sanitizer)   /* its quarantine keeps what was freed "in use" */
        assert((long)m1.size_in_use-(long)m0.size_in_use<64*1024);
#endif
        b9_tests();
        /* from the apps list, only with a song */
        np_hide(0);frame(500);
        test_status=0;action(ACT_VISUALIZER);frame(100);assert(!vz.active);
        test_status=AUDIO_STATUS_PLAY;
        sl_games_page();frame(500);
        lv_obj_t *row=find_label(pages[npages-1],"Visualizer");assert(row);
        lv_obj_scroll_to_view_recursive(row,LV_ANIM_OFF);frame(100);shot("visualizer-apps.ppm");
        lv_obj_send_event(lv_obj_get_parent(row),LV_EVENT_CLICKED,NULL);frame(100);assert(vz.active);
        vz_close();frame(300);page_pop();frame(500);
        assert(lv_screen_active()==home_scr && !np_open && !vz.active);
        /* a Spectrum tile on Start gives the analyzer up meanwhile, and has it back */
        mt_pin(TL_SPECTRUM,SZ_WIDE);frame(300);assert(mt_find(TL_SPECTRUM)>=0 && sp.canvas && sp.bars!=VZ_BANDS);
        action(ACT_VISUALIZER);frame(100);
        assert(vz.active && !sp.canvas && !sp.timer && sp.bars==VZ_BANDS && test_hook==sp_hook);
        mt_spectrum_refill();frame(100);assert(!sp.canvas);   /* not rebuilt under it */
        vz_close();frame(300);
        assert(!vz.active && sp.canvas && sp.timer && sp.bars!=VZ_BANDS && lv_screen_active()==home_scr);
        mt_edit_enter(mt_find(TL_SPECTRUM));mt_unpin_cb(NULL);frame(300);mt_edit_exit();frame(200);
        assert(mt_find(TL_SPECTRUM)<0 && !sp.canvas && test_hook==NULL);
        #undef VZ_FEED
        test_status=saved_status;
    }
    /* ==== scrobbling (scrobble) ==================================== */
    {   /* ListenBrainz and .scrobbler.log: found by Settings search, turned
         * on, a token typed and checked, plays that count and plays that do
         * not, offline, a server in trouble, a bad token; the token never in
         * the log nor on screen */
        assert(!system("rm -rf /tmp/sloppy-ui-test-root/.scrobbler.log /tmp/sloppy-ui-test-root/.rockbox/sloppy/scrobble.tsv "
                       "/tmp/sloppy-ui-test-root/.rockbox/sloppy/listenbrainz.token && mkdir -p /tmp/sloppy-ui-test-root/.rockbox/sloppy"));
        sl_net_scrobble_fake = lbf;
        while(npages>1){page_pop();frame(300);}
        int st0=test_status; struct mp3entry tt0=test_track; bool conn0=wf_st.connected;
        wf_st.connected=true;sl_net_scrobble_online(true);
        settings_search_page();for(int i=0;i<12;i++)frame(40);frame(300);
        sq_add("L");sq_add("I");sq_add("S");sq_add("T");sq_add("E");sq_add("N");frame(300);
        lv_obj_t *hit=find_label(sq_body,"Scrobbling");assert(hit);
        lv_obj_send_event(lv_obj_get_parent(hit),LV_EVENT_CLICKED,NULL);frame(500);
        assert(scr_page_obj && pages[npages-1]==scr_page_obj);
        shot("scrobble-page.ppm");
        assert(has_text(scr_page_obj,"Not set") && has_text(scr_page_obj,"Nothing waiting to send") && !lbf_requests);
        assert(lv_obj_has_flag(scr_forget_row,LV_OBJ_FLAG_HIDDEN));
        lv_obj_send_event(lv_obj_get_parent(find_label(scr_page_obj,"Send to ListenBrainz")),LV_EVENT_CLICKED,NULL);
        lv_obj_send_event(lv_obj_get_parent(find_label(scr_page_obj,"Keep .scrobbler.log")),LV_EVENT_CLICKED,NULL);
        frame(300);assert(global_settings.sloppy_listenbrainz==1 && global_settings.sloppy_scrobble_log==1);
        /* the token: typed on the Wi-Fi keyboard, shown as dots */
        lv_obj_send_event(scr_token_row,LV_EVENT_CLICKED,NULL);frame(500);
        assert(scr_tok_ta && pages[npages-1]!=scr_page_obj);
        const char *tok="good-6f1c2a9e-ui";
        kb_type(kb_now(),"good");
        lv_textarea_add_text(scr_tok_ta,tok+4);
        assert(!strcmp(lv_textarea_get_text(scr_tok_ta),tok));
        frame(2000);shot("scrobble-token.ppm");
        assert(!strstr(lv_label_get_text(lv_textarea_get_label(scr_tok_ta)),"good"));
        lv_textarea_set_text(scr_tok_ta,"no good");               /* a space: refused, stays */
        lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages-1],"Save token")),LV_EVENT_CLICKED,NULL);frame(300);
        assert(pages[npages-1]!=scr_page_obj);
        lv_textarea_set_text(scr_tok_ta,tok);
        lv_obj_send_event(lv_obj_get_parent(find_label(pages[npages-1],"Save token")),LV_EVENT_CLICKED,NULL);frame(500);
        assert(pages[npages-1]==scr_page_obj);
        /* saved with Wi-Fi up: checked at once, the name in a toast */
        scr_poll();frame(300);
        assert(scr_st.test==SL_SCROB_GOOD && !strcmp(scr_st.user,"tai") && scr_st.has_token && lbf_requests==1);
        assert(nh.n && strstr(nh.list[0].title,"Token works") && strstr(nh.list[0].body,"tai"));
        assert(has_text(scr_page_obj,"Signed in as tai") && has_text(scr_page_obj,"Saved"));
        assert(!lv_obj_has_flag(scr_forget_row,LV_OBJ_FLAG_HIDDEN));
        shot("scrobble-signed-in.ppm");
        lv_obj_scroll_to_y(lv_obj_get_child(scr_page_obj,1),2000,LV_ANIM_OFF);frame(100);shot("scrobble-page-end.ppm");
        assert(has_text(scr_page_obj,"Forget token"));
        lv_obj_scroll_to_y(lv_obj_get_child(scr_page_obj,1),0,LV_ANIM_OFF);frame(100);
        {   FILE *tf=fopen("/tmp/sloppy-ui-test-root/.rockbox/sloppy/listenbrainz.token","r");char tb[80]={0};
            assert(tf && fgets(tb,sizeof tb,tf));fclose(tf);assert(!strcmp(tb,"good-6f1c2a9e-ui\n")); }
        /* plays: 110 s of 200 is a listen; 40 s is history's play but no listen */
        long hist0=fake_size(HIST_FILE)<0?4:fake_size(HIST_FILE);   /* a new log starts with its magic */
        test_status=AUDIO_STATUS_PLAY;test_track.artist="Mira Sol";test_track.albumartist=NULL;test_track.album="Night Bus";
        test_track.tracknum=3;test_track.length=200000;test_track.mb_track_id=NULL;
        int r0=lbf_requests;
        lbf_play("/Music/scrobble-a.flac","Blue Hours",0);scr_poll();
        assert(lbf_requests==r0+1 && strstr(lbf_body,"playing_now") && strstr(lbf_body,"Blue Hours"));
        lbf_play("/Music/scrobble-a.flac","Blue Hours",110000);
        lbf_play("/Music/scrobble-b.flac","Short Skip",40000);scr_poll();
        assert(fake_size(HIST_FILE)==hist0+12);                    /* history: one play, as before */
        assert(lbf_requests==r0+3 && strstr(lbf_body,"\"single\"") && strstr(lbf_body,"Blue Hours") &&
               strstr(lbf_body,"\"tracknumber\":3") && strstr(lbf_body,"\"duration_ms\":200000"));
        {   FILE *lf=fopen("/tmp/sloppy-ui-test-root/.scrobbler.log","r");static char lb[4096];size_t ln=lf?fread(lb,1,sizeof lb-1,lf):0;
            if(lf)fclose(lf);lb[ln]=0;
            assert(!strncmp(lb,"#AUDIOSCROBBLER/1.1\n#TZ/UTC\n#CLIENT/GabagoolOS HiBy R1\n",54));
            assert(strstr(lb,"\nMira Sol\tNight Bus\tBlue Hours\t3\t200\tL\t17") && !strstr(lb,"Short Skip")); }
        /* offline: it waits on the card */
        wf_st.connected=false;sl_net_scrobble_online(false);
        r0=lbf_requests;
        lbf_play("/Music/scrobble-c.flac","Kept For Later",120000);
        lbf_play("/Music/scrobble-d.flac","Next One",0);scr_poll();frame(300);
        assert(lbf_requests==r0 && scr_st.queued==1);
        assert(has_text(scr_page_obj,"Waiting to send: 1") && has_text(scr_page_obj,"Waiting for Wi-Fi"));
        shot("scrobble-waiting.ppm");
        /* Wi-Fi back but the server down: five tries, one warning */
        lbf_answer=-1;wf_st.connected=true;
        unsigned fails0=scr_st.fail_seq;
        for(int i=0;i<5;i++){sl_net_scrobble_online(false);sl_net_scrobble_online(true);scr_poll();}
        frame(300);
        assert(scr_st.queued==1 && lbf_requests==r0+5 && scr_st.error[0]);
        assert(scr_st.fail_seq==fails0+1 && strstr(nh.list[0].title,"ListenBrainz is not answering"));   /* told once */
        assert(has_text(scr_page_obj,"ListenBrainz did not answer"));
        shot("scrobble-trouble.ppm");
        lbf_answer=200;sl_net_scrobble_online(false);sl_net_scrobble_online(true);scr_poll();frame(300);
        assert(scr_st.queued==0 && strstr(lbf_body,"Kept For Later") && has_text(scr_page_obj,"Last sent: now"));
        assert(!scr_st.error[0] && lv_obj_has_flag(scr_lines[2],LV_OBJ_FLAG_HIDDEN));
        shot("scrobble-sent.ppm");
        /* a token ListenBrainz does not know */
        sl_net_scrobble_token("nope-1234-5678");
        lv_obj_send_event(scr_test_row,LV_EVENT_CLICKED,NULL);scr_poll();frame(300);
        assert(scr_st.test==SL_SCROB_BAD && scr_st.token_bad && has_text(scr_page_obj,"Not accepted"));
        assert(strstr(nh.list[0].title,"ListenBrainz did not accept the token"));
        shot("scrobble-bad-token.ppm");
        /* forgotten: no file, nothing more is sent */
        lv_obj_send_event(scr_forget_row,LV_EVENT_CLICKED,NULL);scr_poll();frame(300);
        assert(!scr_st.has_token && has_text(scr_page_obj,"Not set") && access("/tmp/sloppy-ui-test-root/.rockbox/sloppy/listenbrainz.token",F_OK)!=0);
        /* the log says what happened, and never the token */
        assert(fake_find(ROCKBOX_DIR "/sloppy/sloppy.log")>=0);
        assert(!lbf_log_has("6f1c2a9e") && !lbf_log_has("1234-5678") && lbf_log_has("scrobble: batch sent"));
        lbf_log_print();
        /* off again, the harness as it was */
        test_status=0;hist_poll();test_track=tt0;test_status=st0;
        global_settings.sloppy_listenbrainz=global_settings.sloppy_scrobble_log=0;scr_apply();
        wf_st.connected=conn0;sl_net_scrobble_online(conn0);sl_net_scrobble_fake=NULL;
        while(npages>1){page_pop();frame(300);}
        assert(!scr_page_obj);
        printf("PASS: scrobbling: settings search, token typed and checked, listens sent, offline queue, backoff, bad token.\n");
    }
    /* ==== end scrobbling =================================================== */

    /* ==== Receive from phone (sloppy_bluetooth.inc) ==== */
    {
        int saved_status=test_status;test_status=AUDIO_STATUS_PLAY;   /* a song plays here first */
        sl_btrx_demo_auto=false;
        action(ACT_BLUETOOTH);frame(500);assert(sl_bt_page_obj==pages[npages-1]);
        lv_obj_t *rcv=find_label(sl_bt_body,"Receive from phone");assert(rcv);
        lv_obj_scroll_to_view_recursive(rcv,LV_ANIM_OFF);frame(100);shot("btrx-bluetooth-page.ppm");
        /* BlueALSA will not switch: said in a warning, nothing left on */
        sl_btrx_demo_fail=true;
        rcv=find_label(sl_bt_body,"Receive from phone");assert(rcv);   /* the page may have been redrawn */
        lv_obj_send_event(lv_obj_get_parent(rcv),LV_EVENT_CLICKED,NULL);frame(500);
        assert(sl_btrx_page_obj==pages[npages-1]);frame(200);
        assert(!sl_btrx_on && !sl_btrx_active && !test_voice_more);
        assert(nh.list[0].kind==NOTE_WARN && !strcmp(nh.list[0].title,"Bluetooth could not switch to receiving"));
        assert(!strcmp(nh.list[0].body,"Bluetooth is back as it was."));
        shot("btrx-failed.ppm");
        sl_btrx_demo_fail=false;
        /* started: visible, waiting; the Bluetooth page's own jobs wait */
        lv_obj_t *go=find_label(sl_btrx_body,"Start receiving");assert(go);
        lv_obj_send_event(lv_obj_get_parent(go),LV_EVENT_CLICKED,NULL);frame(200);
        assert(sl_btrx_on && sl_btrx.state==HIBY_BTRX_WAITING);
        assert(find_label(sl_btrx_body,"Visible as GabagoolOS R1") && find_label(sl_btrx_body,"Stop receiving"));
        frame(5000);frame(5000);shot("btrx-waiting.ppm");   /* the warning gone first */
        /* the phone connects: a notification, its name on the screen */
        sl_btrx_demo.state=HIBY_BTRX_CONNECTED;sl_btrx_demo.connects=1;
        strlcpy(sl_btrx_demo.mac,"AA:BB:CC:DD:EE:01",sizeof sl_btrx_demo.mac);
        strlcpy(sl_btrx_demo.name,"Tai's iPhone",sizeof sl_btrx_demo.name);
        strlcpy(sl_btrx_demo.codec,"AAC",sizeof sl_btrx_demo.codec);sl_btrx_demo.rate=48000;sl_btrx_demo.channels=2;
        frame(200);
        assert(!strcmp(nh.list[0].title,"Tai's iPhone connected") && nh.list[0].action==NA_BT);
        assert(find_label(sl_btrx_body,"Playing from Tai's iPhone") && find_label(sl_btrx_body,"AAC · 48.0 kHz"));
        assert(find_label(sl_btrx_body,"Connected. Play music on your phone."));   /* no audio yet: not "paused" */
        shot("btrx-connected.ppm");
        /* audio: 48 kHz in, the mixer at 44.1; the song here pauses */
        static int16_t pcm[4096*2];static uint32_t ph;
        #define BTRX_PUSH(n) do{for(int i_=0;i_<(n);i_++,ph++){pcm[i_*2]=(int16_t)(8000*sinf(ph*0.0654f));pcm[i_*2+1]=(int16_t)-pcm[i_*2];}sl_btrx_demo_push(pcm,(n));}while(0)
        BTRX_PUSH(4096);frame(100);
        assert(sl_btrx_active && test_voice_more && (test_status&AUDIO_STATUS_PAUSE));
        assert(test_voice_amp==MIX_AMP_UNITY);
        { const void *st;size_t sz;int peak=0;
          int before=sl_btrx_buffered();test_voice_more(&st,&sz);assert(sz==SL_BTRX_OUT*4);
          int used=before-sl_btrx_buffered();assert(used>=1110 && used<=1118);   /* 1024 * 48000 / 44100 */
          const int16_t *o=st;for(size_t i=0;i<sz/2;i++){if(abs(o[i])>peak)peak=abs(o[i]);}
          printf("receive from phone: 1024 out took %d in, peak %d\n",used,peak);
          assert(peak>7000 && peak<=8000 && abs(o[200]+o[201])<=1);   /* right is left upside down */
          /* ran dry, then the audio came back: one underrun */
          for(int k=0;k<4;k++)test_voice_more(&st,&sz);assert(sl_btrx_buffered()==0 && sl_btrx_dry);
          BTRX_PUSH(4096);test_voice_more(&st,&sz);assert(sl_btrx_underruns==1); }
        /* the phone's track and volume */
        strlcpy(sl_btrx_demo.title,"Gabagool Groove",sizeof sl_btrx_demo.title);
        strlcpy(sl_btrx_demo.artist,"The Sopranos",sizeof sl_btrx_demo.artist);
        strlcpy(sl_btrx_demo.player,"playing",sizeof sl_btrx_demo.player);sl_btrx_demo.volume=64;
        BTRX_PUSH(2048);frame(100);
        assert(test_voice_amp==sl_btrx_amp(64) && test_voice_amp>MIX_AMP_UNITY/4 && test_voice_amp<MIX_AMP_UNITY*27/100);   /* 64/127 squared */
        assert(find_label(sl_btrx_body,"Gabagool Groove") && find_label(sl_btrx_body,"The Sopranos"));
        assert(find_label(sl_btrx_body,"Playing through this player") && find_label(sl_btrx_body,"AAC · 48.0 kHz · phone volume 50%"));
        shot("btrx-playing.ppm");
        /* the phone pauses: three quiet seconds and the channel stops */
        strlcpy(sl_btrx_demo.player,"paused",sizeof sl_btrx_demo.player);
        sl_btrx_demo_last=sl_ms()-3500;frame(100);
        assert(!sl_btrx_active && !test_voice_more && find_label(sl_btrx_body,"Paused on your phone"));
        shot("btrx-paused.ppm");
        /* play from here asks the phone; it plays again when audio comes */
        lv_obj_t *pp=find_label(sl_btrx_body,LV_SYMBOL_PLAY);assert(pp);
        lv_obj_send_event(lv_obj_get_parent(pp),LV_EVENT_CLICKED,NULL);assert(!strcmp(sl_btrx_demo_ctl,"Play"));
        strlcpy(sl_btrx_demo.player,"playing",sizeof sl_btrx_demo.player);
        BTRX_PUSH(4096);frame(100);assert(sl_btrx_active && test_voice_more);
        lv_obj_t *nx=find_label(sl_btrx_body,LV_SYMBOL_NEXT);assert(nx);
        lv_obj_send_event(lv_obj_get_parent(nx),LV_EVENT_CLICKED,NULL);assert(!strcmp(sl_btrx_demo_ctl,"Next"));
        pp=find_label(sl_btrx_body,LV_SYMBOL_PAUSE);assert(pp);
        lv_obj_send_event(lv_obj_get_parent(pp),LV_EVENT_CLICKED,NULL);assert(!strcmp(sl_btrx_demo_ctl,"Pause"));
        /* play pressed here: the player's song wins, the phone waits */
        test_status=AUDIO_STATUS_PLAY;BTRX_PUSH(1024);frame(100);
        assert(!sl_btrx_active && sl_btrx_held_off && find_label(sl_btrx_body,"This player's music is playing"));
        BTRX_PUSH(4096);frame(100);assert(!sl_btrx_active);
        test_status|=AUDIO_STATUS_PAUSE;
        /* the Bluetooth page's own jobs wait while receiving */
        sl_bt_job_power=false;sl_bt_begin(SL_BT_JOB_POWER,"");assert(!sl_bt_busy && sl_bt_job==SL_BT_JOB_NONE);
        assert(!strcmp(nh.list[0].title,"Stop receiving from your phone first"));
        /* the phone goes: the session is over, Bluetooth back to normal */
        sl_btrx_demo.state=HIBY_BTRX_OFF;sl_btrx_demo_running=false;
        strlcpy(sl_btrx_demo.error,"Phone disconnected",sizeof sl_btrx_demo.error);
        frame(200);
        assert(!sl_btrx_on && !sl_btrx_active && !test_voice_more && !sl_btrx_held_off);
        assert(nh.list[0].kind==NOTE_INFO && !strcmp(nh.list[0].title,"Tai's iPhone disconnected"));
        assert(find_label(sl_btrx_body,"Start receiving"));
        shot("btrx-disconnected.ppm");
        /* Stop on the screen */
        lv_obj_send_event(lv_obj_get_parent(find_label(sl_btrx_body,"Start receiving")),LV_EVENT_CLICKED,NULL);frame(200);
        assert(sl_btrx_on);
        lv_obj_send_event(lv_obj_get_parent(find_label(sl_btrx_body,"Stop receiving")),LV_EVENT_CLICKED,NULL);frame(200);
        assert(!sl_btrx_on && !strcmp(nh.list[0].title,"Stopped receiving"));
        /* nobody came: said, not as a failure */
        lv_obj_send_event(lv_obj_get_parent(find_label(sl_btrx_body,"Start receiving")),LV_EVENT_CLICKED,NULL);frame(200);
        sl_btrx_demo.state=HIBY_BTRX_OFF;sl_btrx_demo_running=false;
        strlcpy(sl_btrx_demo.error,"No phone connected",sizeof sl_btrx_demo.error);frame(200);
        printf("receive from phone: timeout note \"%s\" / \"%s\" kind %d on %d\n",nh.list[0].title,nh.list[0].body,nh.list[0].kind,sl_btrx_on);
        assert(!sl_btrx_on && nh.list[0].kind==NOTE_INFO && !strcmp(nh.list[0].title,"No phone connected"));
        /* the control centre's Bluetooth tile and the notification lead here */
        lv_obj_send_event(lv_obj_get_parent(find_label(sl_btrx_body,"Start receiving")),LV_EVENT_CLICKED,NULL);frame(200);
        page_pop();frame(500);page_pop();frame(500);assert(!sl_btrx_page_obj);
        note_open(NA_BT);frame(500);assert(sl_btrx_page_obj==pages[npages-1]);
        page_pop();frame(500);
        sl_bt_page();frame(500);assert(find_label(sl_bt_body,"Receiving · tap to open") && find_label(sl_bt_body,"Receiving from your phone"));
        page_pop();frame(500);
        /* leaving for classic Rockbox puts Bluetooth back first */
        sl_btrx_shutdown();assert(!sl_btrx_on && !sl_btrx_running());
        #undef BTRX_PUSH
        sl_btrx_demo_auto=true;test_status=saved_status;frame(500);
        printf("receive from phone: failure, waiting, connect, audio 48k->44.1k, underrun, volume, pause, controls, disconnect, stop, timeout ok\n");
    }
    /* ==== end Receive from phone ==== */
    /* ======== Clicker (clicker) ========
     * taps, plus and minus, the volume keys (no volume change), counting
     * locked and dark, Reset's sheet, the save, open/close cycles, every
     * theme, the apps row and the live tile. */
    {
        int theme0=global_settings.sloppy_theme,style0=global_settings.sloppy_home_style,st0=test_status;
        int lang0=global_settings.sloppy_language,step0=sl_text_step;
        global_settings.sloppy_theme=0;global_settings.sloppy_home_style=0;appearance_reload(NULL);frame(700);
        while(npages>1){page_pop();frame(400);}
        remove(CK_FILE);clicker_loaded=false;clicker_count=0;
        global_settings.sloppy_sfx_volume=70;global_settings.sloppy_ui_sounds=1;
        test_status=AUDIO_STATUS_PLAY;
        int vol0=global_status.volume,setvol0=setvol_calls;
        action(ACT_CLICKER);frame(500);
        assert(clicker_on_top() && clicker_count==0 && !strcmp(lv_label_get_text(clicker_num),"0"));
        mini_update();assert(lv_obj_has_flag(mini,LV_OBJ_FLAG_HIDDEN));       /* plus and minus sit there */
#ifdef BUTTON_WAKE_SWALLOWS
        assert(test_wake_swallows==0);
#endif
        assert(lv_obj_get_style_opa(clicker_reset,0)<LV_OPA_COVER);         /* nothing to reset yet */
        shot("clicker-zero.ppm");
        /* a tap anywhere counts one, with the tap sound and a pop */
        SFX_RESET();touch_at(240,330,true);touch_at(240,330,false);
        assert(clicker_count==1 && sfx_last==SFX_TAP && !strcmp(lv_label_get_text(clicker_num),"1"));
        assert(lv_obj_get_style_transform_scale_x(clicker_box,0)>256);
        touch_at(60,560,true);touch_at(60,560,false);assert(clicker_count==2);
        frame(16);shot_now("clicker-pop.ppm");
        frame(600);assert(lv_obj_get_style_transform_scale_x(clicker_box,0)==256);
        assert(lv_obj_get_style_image_opa(clicker_glow,0)==LV_OPA_COVER && lv_obj_get_style_image_opa(clicker_bloom,0)==LV_OPA_TRANSP);
        /* what a count costs a frame, on the R1's scale: the pop and the bloom */
        pf_begin("clicker count");for(int k=0;k<3;k++){clicker_add(1,false);for(int i=0;i<12;i++)pf_frame(16);}pf_end();
        clicker_count=2;clicker_show();frame(1000);
        /* a scroll-like drag is not a tap */
        touch_at(240,520,true);touch_at(240,470,true);touch_at(240,400,true);touch_at(240,380,false);
        assert(clicker_count==2);
        /* plus and minus; never below zero */
        int py=CK_BTN_Y+CK_BTN/2;
        touch_at(W/2+78,py,true);touch_at(W/2+78,py,false);assert(clicker_count==3);
        SFX_RESET();touch_at(W/2-78,py,true);touch_at(W/2-78,py,false);assert(clicker_count==2 && sfx_last==SFX_TAP);
        for(int i=0;i<4;i++){SFX_RESET();touch_at(W/2-78,py,true);touch_at(W/2-78,py,false);}
        assert(clicker_count==0 && !strcmp(lv_label_get_text(clicker_num),"0"));
        frame(1000);assert(fake_equals(CK_FILE,"SLK1 0\n"));
        /* the volume keys count, a hold counts once, and the volume never moves */
        handle_button(BUTTON_UP);handle_button(BUTTON_UP|BUTTON_REL);assert(clicker_count==1);
        handle_button(BUTTON_DOWN);for(int i=0;i<6;i++)handle_button(BUTTON_DOWN|BUTTON_REPEAT);
        handle_button(BUTTON_DOWN|BUTTON_REL|BUTTON_REPEAT);assert(clicker_count==2);
        assert(global_status.volume==vol0 && setvol_calls==setvol0);
        assert(!hud||lv_obj_has_flag(hud,LV_OBJ_FLAG_HIDDEN));
        /* saved a moment after the last change, not on every one */
        assert(!fake_equals(CK_FILE,"SLK1 2\n"));
        frame(400);assert(!fake_equals(CK_FILE,"SLK1 2\n"));
        frame(600);assert(fake_equals(CK_FILE,"SLK1 2\n"));
        {   /* locked and dark: the keys count, the screen stays as it was */
            bool run0=sl_power_running;struct sl_power_state pw0=sl_power;
            sl_power_running=true;sl_power_suspended=false;sl_power_state_init(&sl_power,sl_ms());
            sl_power_applied_mode=-1;sl_power.mode=SL_POWER_LOCKED;sl_power_apply();frame(100);
            assert(sl_power_hides_ui());
            SFX_RESET();handle_button(BUTTON_UP);handle_button(BUTTON_UP|BUTTON_REL);
            assert(clicker_count==3 && sl_power.mode==SL_POWER_LOCKED && sfx_last==SFX_UI_TICK);
            assert(lv_obj_get_style_transform_scale_x(clicker_box,0)==256);     /* no motion behind the lock screen */
            sl_power.mode=SL_POWER_DARK;sl_power_apply();frame(100);assert(!test_lcd_on);
            for(int i=0;i<5;i++){handle_button(BUTTON_DOWN);frame(30);handle_button(BUTTON_DOWN|BUTTON_REL);frame(30);}
            assert(clicker_count==8 && sl_power.mode==SL_POWER_DARK && !test_lcd_on && wallpaper_screen_dark);
            assert(global_status.volume==vol0 && setvol_calls==setvol0);
            frame(1000);assert(fake_equals(CK_FILE,"SLK1 8\n"));               /* saved while dark */
            /* dim: a key lifts it, and counts */
            sl_power.mode=SL_POWER_ACTIVE;sl_power_apply();frame(100);
            sl_power.mode=SL_POWER_DIM;handle_button(BUTTON_UP);handle_button(BUTTON_UP|BUTTON_REL);
            assert(clicker_count==9 && sl_power.mode==SL_POWER_ACTIVE);
            /* other keys still go to the lock screen's filter */
            sl_power.mode=SL_POWER_DARK;sl_power_apply();frame(100);
            handle_button(BUTTON_POWER);handle_button(BUTTON_POWER|BUTTON_REL);assert(sl_power.mode==SL_POWER_LOCKED);
            handle_button(BUTTON_UP);handle_button(BUTTON_UP|BUTTON_REL);assert(clicker_count==10);
            /* unlocked: the number is there */
            sl_power_filter_touch(true,W/2,600);sl_power_filter_touch(true,W/2,400);sl_power_filter_touch(false,0,0);
            assert(!sl_power_is_locked());frame(600);
            assert(clicker_on_top() && !strcmp(lv_label_get_text(clicker_num),"10"));
            shot("clicker-after-unlock.ppm");
            sl_power_running=run0;sl_power=pw0;sl_power_applied_mode=-1;sl_power_overlay_show(false);
            if(!wallpaper_screen_dark)sl_power_set_drawing(true);
            wallpaper_screen_dark=false;sl_power_set_drawing(true);test_lcd_on=true;frame(300);
            printf("PASS: Clicker counts with the volume keys locked and dark, volume untouched.\n");
        }
        /* big numbers shrink to fit, seven digits at most */
        static const uint32_t bigs[]={1234,98765,654321,1234567};
        for(int k=0;k<4;k++){clicker_count=bigs[k];clicker_show();lv_obj_update_layout(clicker_num);
            const char *t=lv_label_get_text(clicker_num);
            const lv_font_t *f=lv_obj_get_style_text_font(clicker_num,0);
            assert(lv_text_get_width(t,strlen(t),f,0)<=W-32);
            assert(f==(k==0?&sl_count_184:k<3?&sl_count_136:&ss_regular_104));}
        clicker_count=1234;clicker_show();shot("clicker-1234.ppm");
        clicker_count=CK_MAX-1;clicker_add(1,false);clicker_add(1,false);assert(clicker_count==CK_MAX);
        frame(600);shot("clicker-big.ppm");
        clicker_count=42;clicker_show();frame(300);
        /* Reset asks first: Cancel and a tap outside keep the count */
        key_tap(clicker_reset);frame(400);assert(clicker_sheet_open() && clicker_count==42);
        shot("clicker-confirm.ppm");
        key_tap(lv_obj_get_parent(find_label(clicker_sheet,"Cancel")));frame(400);
        assert(!clicker_sheet_open() && clicker_count==42);
        key_tap(clicker_reset);frame(400);touch_at(240,200,true);touch_at(240,200,false);frame(400);
        assert(!clicker_sheet_open() && clicker_count==42);
        key_tap(clicker_reset);frame(400);
        SFX_RESET();key_tap(lv_obj_get_parent(find_label(clicker_sheet,"Reset")));frame(500);
        assert(!clicker_sheet_open() && clicker_count==0 && sfx_last==SFX_UI_REMOVE);
        assert(fake_equals(CK_FILE,"SLK1 0\n"));                              /* at once */
        key_tap(clicker_reset);frame(400);assert(!clicker_sheet_open());       /* nothing to reset */
        /* the count outlives the page and a restart */
        for(int i=0;i<7;i++)clicker_add(1,false);
        page_pop();frame(500);assert(!clicker_page_obj && !clicker_save_timer && !clicker_glow_px);
#ifdef BUTTON_WAKE_SWALLOWS
        assert(test_wake_swallows==!global_settings.sloppy_keys_dark);
#endif
        assert(fake_equals(CK_FILE,"SLK1 7\n"));
        clicker_loaded=false;clicker_count=0;clicker_load();assert(clicker_count==7);
        handle_button(BUTTON_UP);handle_button(BUTTON_UP|BUTTON_REL);          /* closed: the keys are volume again */
        assert(clicker_count==7 && global_status.volume!=vol0);
        global_status.volume=vol0;
        /* open and close: the same objects every time, nothing left behind */
        {   int o0=-1,t0=-1;
            for(int k=0;k<5;k++){clicker_page();frame(400);touch_at(240,330,true);touch_at(240,330,false);
                page_pop();frame(500);int o=objects(lv_screen_active()),t=objects(lv_layer_top());
                if(k==0){o0=o;t0=t;}else assert(o==o0 && t==t0);}
            assert(clicker_count==12 && !clicker_page_obj);
            printf("Clicker: 5 opens, %d objects after each\n",o0); }
        /* every theme, and RTL with the largest text */
        static const char *const names[]={"clicker-dark.ppm","clicker-light.ppm","clicker-true-dark.ppm","clicker-zule.ppm"};
        for(int t=0;t<4;t++){global_settings.sloppy_theme=t;appearance_reload(NULL);frame(700);
            while(npages>1){page_pop();frame(400);}
            clicker_page();frame(500);touch_at(240,330,true);touch_at(240,330,false);frame(16);frame(16);
            shot_now(t==1?"clicker-light-pop.ppm":"clicker-pop-theme.ppm");frame(700);shot(names[t]);
            if(t==3){key_tap(clicker_reset);frame(400);shot("clicker-confirm-zule.ppm");clicker_cancel_cb(NULL);frame(400);}
            page_pop();frame(500);}
        global_settings.sloppy_theme=0;global_settings.sloppy_language=5;sl_text_step=3;appearance_reload(NULL);frame(700);
        while(npages>1){page_pop();frame(400);}
        clicker_page();frame(500);shot("clicker-arabic-largest.ppm");
        key_tap(clicker_reset);frame(400);shot("clicker-confirm-arabic.ppm");clicker_cancel_cb(NULL);frame(400);
        page_pop();frame(500);
        global_settings.sloppy_language=lang0;sl_text_step=step0;appearance_reload(NULL);frame(700);
        while(npages>1){page_pop();frame(400);}
        /* the apps list row, and a live tile with the count */
        sl_games_page();frame(500);
        {   lv_obj_t *row=find_label(pages[npages-1],"Clicker");assert(row);
            lv_obj_scroll_to_view_recursive(row,LV_ANIM_OFF);frame(100);shot("clicker-apps-row.ppm");
            lv_obj_send_event(lv_obj_get_parent(row),LV_EVENT_CLICKED,NULL);frame(500);assert(clicker_on_top());
            page_pop();frame(500);page_pop();frame(500); }
        global_settings.sloppy_home_style=1;appearance_reload(NULL);frame(700);
        while(npages>1){page_pop();frame(400);}
        mt_pin(TL_CLICKER,SZ_MED);frame(600);
        {   struct mt_slot *ck=&mt.slot[mt_find(TL_CLICKER)];
            char want[12];snprintf(want,sizeof want,"%lu",(unsigned long)clicker_count);assert(clicker_count==16);
            assert(ck->faces && mt_back(ck) && !strcmp(lv_label_get_text(lv_obj_get_child(ck->back,0)),want));
            lv_obj_scroll_to_y(mt.body,lv_obj_get_y(ck->obj)-260,LV_ANIM_OFF);frame(100);shot("clicker-tile-front.ppm");
            mt_peek(ck->faces,mt_px(mt_span_h(ck->size)));ck->down_at=mt.ticks+4;frame(700);shot("clicker-tile.ppm");
            ck->size=SZ_SMALL;mt_fill(ck);mt_layout(false);frame(600);assert(mt_back(ck));
            lv_obj_scroll_to_y(mt.body,lv_obj_get_y(ck->obj)-260,LV_ANIM_OFF);frame(100);
            mt_peek(ck->faces,mt_px(mt_span_h(ck->size)));ck->down_at=mt.ticks+4;frame(700);shot("clicker-tile-small.ppm");
            g_dragged=false;lv_obj_send_event(ck->obj,LV_EVENT_CLICKED,NULL);frame(500);assert(clicker_on_top());
            page_pop();frame(500);
            mt_edit_enter(mt_find(TL_CLICKER));mt_unpin_cb(NULL);frame(300);mt_edit_exit();frame(200);
            assert(mt_find(TL_CLICKER)<0); }
        global_settings.sloppy_theme=theme0;global_settings.sloppy_home_style=style0;appearance_reload(NULL);frame(700);
        while(npages>1){page_pop();frame(400);}
        test_status=st0;
        printf("PASS: Clicker taps, keys, reset, save, themes, apps row and tile.\n");
    }
    /* ======== end Clicker ======== */
    /* ======== Spacey Lines ======== */
    {   /* from the Playroom: black until touched; a swirl draws into the tunnel and
         * stays on the page (no back from mid screen); each touch turns the tunnel;
         * a finger held in a corner fades it all; the clear button lifts the pen and
         * leaves black; swipe back from the edge and the back button both leave,
         * freeing the pictures, the timer and every object */
        while(npages>1){page_pop();frame(400);}
        int st0=test_status;test_status=AUDIO_STATUS_PLAY;   /* the mini player would sit over the stage */
        sl_games_page();frame(500);
        int objs0=objects(lv_screen_active()),depth=npages;malloc_statistics_t m0,m1;malloc_zone_statistics(NULL,&m0);
        lv_obj_t *row=find_label(pages[npages-1],"Spacey Lines");assert(row);
        while(row&&!lv_obj_has_flag(row,LV_OBJ_FLAG_CLICKABLE))row=lv_obj_get_parent(row);
        assert(row);g_dragged=false;lv_obj_send_event(row,LV_EVENT_CLICKED,NULL);frame(500);
        assert(spacey_on_top() && npages==depth+1 && spc.buf[0] && spc.buf[1] && spc.timer && spc.hint);
        mini_update();assert(lv_obj_has_flag(mini,LV_OBJ_FLAG_HIDDEN));
        #define SPC_LIT() ({long n_=0;for(int i_=0;i_<spc.bw*spc.bh;i_++)n_+=spc.buf[spc.cur][i_]!=0;n_;})
        #define SPC_LEVEL() ({uint64_t s_=0;for(int i_=0;i_<spc.bw*spc.bh;i_++){uint32_t v_=spc.buf[spc.cur][i_];s_+=(v_>>11)*2+(v_>>5&63)+(v_&31)*2;}s_;})
        for(int i=0;i<10;i++)frame(33);
        assert(SPC_LIT()==0 && !spc.frames);   /* nothing until a finger: no work either */
        lv_area_t sa;lv_obj_get_coords(spc.stage,&sa);int cx=(sa.x1+sa.x2)/2,cy=(sa.y1+sa.y2)/2;
        assert(sa.x1==14 && sa.x2==W-15 && sa.y2==H-15 && spc.bw*2==spc.sw && spc.bh*2==spc.sh);
        touch_at(cx+60,cy,true);assert(spc.clicks==1 && spc.down && spc.touched);
        for(int i=0;i<60;i++){float a=i*0.2f;touch_at(cx+(int)(150*cosf(a)),cy+(int)(200*sinf(a)),true);}
        touch_at(cx+150,cy,false);assert(!spc.down && spacey_on_top() && npages==depth+1);
        for(int i=0;i<20;i++)frame(33);
        long lit=SPC_LIT();printf("spacey: %ld of %d px lit after a swirl, %lu frames\n",lit,spc.bw*spc.bh,(unsigned long)spc.frames);
        assert(lit>spc.bw*spc.bh/20 && spc.frames>20);
        assert(lv_obj_has_flag(spc.hint,LV_OBJ_FLAG_HIDDEN));   /* the hint went with the first touch */
        /* the stage is painted: its middle is no longer the black card */
        {long on=0;for(int y=sa.y1+40;y<sa.y2-40;y+=4)for(int x=sa.x1+40;x<sa.x2-40;x+=4)on+=pixels[y*W+x]!=0;assert(on>200);}
        /* a sideways drag across the middle draws; it does not go back */
        touch_at(120,cy+100,true);for(int x=150;x<=420;x+=30)touch_at(x,cy+100,true);touch_at(420,cy+100,false);frame(400);
        assert(spacey_on_top() && npages==depth+1 && spc.clicks==2);
        touch_at(cx,cy,true);touch_at(cx,cy,false);assert(spc.clicks==3);   /* a tap: the other tunnel */
        for(int i=0;i<10;i++)frame(33);
        uint64_t level=SPC_LEVEL();
        touch_at(sa.x2+6,sa.y1-6,true);   /* the top right corner, from the gutter */
        assert(spc.down && spc.fx==spc.sw-1 && spc.fy==0);
        for(int i=0;i<40;i++)touch_at(sa.x2+6,sa.y1-6,true);
        uint64_t held=SPC_LEVEL();touch_at(sa.x2+6,sa.y1-6,false);
        printf("spacey: corner hold, level %llu -> %llu\n",(unsigned long long)level,(unsigned long long)held);
        assert(held<level/4 && spacey_on_top());
        spc_clear_cb(NULL);for(int i=0;i<SPC_CLEAR+5;i++)frame(33);
        assert(!spc.touched && !spc.clearing && SPC_LIT()==0);
        uint32_t idle=spc.frames;for(int i=0;i<10;i++)frame(33);assert(spc.frames==idle);   /* black and still: no work */
        /* covered by Now Playing: it waits */
        touch_at(cx,cy,true);touch_at(cx,cy,false);for(int i=0;i<5;i++)frame(33);
        np_show();frame(600);idle=spc.frames;for(int i=0;i<10;i++)frame(33);assert(spc.frames==idle);np_hide(0);frame(600);
        for(int i=0;i<5;i++)frame(33);assert(spc.frames>idle);
        /* back: a swipe in from the left edge */
        touch_at(6,cy,true);for(int x=36;x<=336;x+=30)touch_at(x,cy,true);touch_at(336,cy,false);frame(600);
        assert(!spc.page && !spc.buf[0] && !spc.timer && npages==depth && !spacey_on_top());
        /* again, through the action, leaving by the header's back button */
        action(ACT_SPACEY);frame(500);assert(spacey_on_top() && !spc.hint);   /* the hint once a session */
        touch_at(cx,cy,true);for(int i=0;i<20;i++)touch_at(cx+i*8,cy+i*5,true);touch_at(cx+160,cy+100,false);
        lv_obj_t *back=lv_obj_get_child(lv_obj_get_child(pages[npages-1],0),0);
        g_dragged=false;lv_obj_send_event(back,LV_EVENT_CLICKED,NULL);frame(600);
        assert(!spc.page && !spc.buf[0] && npages==depth);
        action(ACT_SPACEY);frame(100);action(ACT_SPACEY);frame(500);assert(npages==depth+1);   /* once on the stack */
        page_pop();frame(600);
        for(int i=0;i<10;i++)frame(100);
        malloc_zone_statistics(NULL,&m1);
        printf("spacey: objects %d -> %d, heap %+ld KiB\n",objs0,objects(lv_screen_active()),((long)m1.size_in_use-(long)m0.size_in_use)/1024);
        assert(objects(lv_screen_active())==objs0);
#if !__has_feature(address_sanitizer)
        assert((long)m1.size_in_use-(long)m0.size_in_use<64*1024);
#endif
        #undef SPC_LIT
        #undef SPC_LEVEL
        while(npages>1){page_pop();frame(400);}
        test_status=st0;mini_update();
        printf("PASS: Spacey Lines draws, turns, clears, waits when covered, leaves and frees.\n");
    }
    /* ======== end Spacey Lines ======== */
    b4_tests();
    c_tags_tests();
    c_scale_tests();
    c_unicode_tests();
    xf_script_tests();
    c_cue_tests();
    c_big_cover_tests();
    e_tests();
    f_tests();
    i_tests();
    h_tests();
    wave_tests();
    ep_tests();
    sb_tests();
    ab_tests();
    rm_tests();
    ot_tests();
    {   /* the screen audit's gate: nothing overlaps, is cut off, hides under the chrome, is too small to tap or too faint */
        au_gate();
        if (AU_GATE_STRICT && !getenv("SLOPPY_AUDIT_REPORT")) assert(au_total == 0 && au_glyph_missing == 0 && au_sym_foreign == 0);
        printf("%s: screen audit: %d findings (the gate is %s).\n", au_total ? "NOTE" : "PASS", au_total, AU_GATE_STRICT ? "strict" : "reporting");
    }
    return 0;
}
void __assert(const char *file, int line, const char *expr) { fprintf(stderr,"%s:%d: %s\n",file,line,expr);abort(); }
/* A tiny in-memory card for the app's save files under .rockbox/sloppy. */
#define FAKE_FILES 800
static struct { char path[MAX_PATH]; unsigned char *data; size_t size; bool used, dir; } fake_fs[FAKE_FILES];   /* as long as Rockbox's paths go */
static struct { int file; size_t pos; bool open, append; } fake_fds[8];
static int fake_find(const char *p) { for (int i=0;i<FAKE_FILES;i++) if (fake_fs[i].used && !strcmp(fake_fs[i].path,p)) return i; return -1; }
static long fake_size(const char *p) { int f=fake_find(p); return f<0 ? -1 : (long)fake_fs[f].size; }
static bool fake_equals(const char *p, const char *want) { int f=fake_find(p); return f>=0 && fake_fs[f].size==strlen(want) && !memcmp(fake_fs[f].data,want,fake_fs[f].size); }
static bool fake_is_dir(const char *p) {
    if (!strcmp(p,FAKE_CARD)) return true;
    int f=fake_find(p); return f>=0 && fake_fs[f].dir;
}
/* the folder a card path is in, made (or not) */
static bool fake_parent_ok(const char *p) {
    char d[MAX_PATH]; strlcpy(d,p,sizeof d); char *sl=strrchr(d,'/'); if (!sl) return false; *sl='\0';
    return fake_is_dir(d);
}
static int fake_new(const char *p) {
    int f; for (f=0;f<FAKE_FILES && fake_fs[f].used;f++); if (f==FAKE_FILES) return -1;
    fake_fs[f].used=true; fake_fs[f].dir=false; strlcpy(fake_fs[f].path,p,sizeof fake_fs[f].path); fake_fs[f].data=NULL; fake_fs[f].size=0; return f;
}
/* a file on the sandboxed card with these bytes */
static void fake_put(const char *p, const void *d, size_t n) {
    int f=fake_find(p); if (f<0) f=fake_new(p); assert(f>=0);
    fake_fs[f].data=realloc(fake_fs[f].data,n?n:1); memcpy(fake_fs[f].data,d,n); fake_fs[f].size=n;
}
static bool fake_has(const char *p, const char *needle) { int f=fake_find(p); return f>=0 && memmem(fake_fs[f].data,fake_fs[f].size,needle,strlen(needle))!=NULL; }
static bool fake_any(const char *in_path) { for (int i=0;i<FAKE_FILES;i++) if (fake_fs[i].used && strstr(fake_fs[i].path,in_path)) return true; return false; }
static void fake_card_reset(void) {
    for (int i=0;i<FAKE_FILES;i++) if (fake_fs[i].used && fake_card(fake_fs[i].path)) { free(fake_fs[i].data); fake_fs[i].data=NULL; fake_fs[i].used=false; fake_fs[i].dir=false; }
}
int sim_open(const char *p,int flags,...) {
    if (flags & (O_WRONLY|O_RDWR|O_CREAT)) SIM_CARD_GUARD(p);
    if (fake_card(p)) {
        int f=fake_find(p);
        if (f>=0 && fake_fs[f].dir) return -1;
        if (f>=0 && (flags & O_CREAT) && (flags & O_EXCL)) return -1;
        if (f<0) { if (!(flags & O_CREAT) || !fake_parent_ok(p) || (f=fake_new(p))<0) return -1; }
        if (flags & O_TRUNC) fake_fs[f].size=0;
        for (int k=0;k<8;k++) if (!fake_fds[k].open) { fake_fds[k].open=true; fake_fds[k].file=f; fake_fds[k].pos=0; fake_fds[k].append=(flags&O_APPEND)!=0; return 100+k; }
        return -1;
    }
    if (!strstr(p,"sloppy/") && !strstr(p,"Playlists/") && !strstr(p,".lrc") && !strstr(p,".LRC") && !strstr(p,"AutoEq/") && strncmp(p,"/peq/",5) && !strstr(p,".flac")
        && !strstr(p,".bmark")) return -1;   /* a book's bookmarks */
    int f=fake_find(p);
    if (flags & (O_WRONLY|O_RDWR)) {
        if (f<0) { for (f=0;f<FAKE_FILES && fake_fs[f].used;f++); if (f==FAKE_FILES) return -1;
                   fake_fs[f].used=true; strlcpy(fake_fs[f].path,p,sizeof fake_fs[f].path); fake_fs[f].data=NULL; fake_fs[f].size=0; }
        if (flags & O_TRUNC) fake_fs[f].size=0;
    } else if (f<0) return -1;
    for (int k=0;k<8;k++) if (!fake_fds[k].open) { fake_fds[k].open=true; fake_fds[k].file=f; fake_fds[k].pos=0; fake_fds[k].append=(flags&O_APPEND)!=0; return 100+k; }
    return -1;
}
static struct sim_dirent *fake_fs_readdir(void) {
    static struct sim_dirent entry;size_t n=strlen(fake_dir_listed);
    while (fake_dir_index<FAKE_FILES) {
        int i=fake_dir_index++;
        if (fake_fs[i].used && !strncmp(fake_fs[i].path,fake_dir_listed,n) && fake_fs[i].path[n]=='/' && !strchr(fake_fs[i].path+n+1,'/'))
        { strlcpy(entry.d_name,fake_fs[i].path+n+1,sizeof entry.d_name); return &entry; }
    }
    return NULL;
}
int fake_close(int fd) { if (fd>=100 && fd<108) fake_fds[fd-100].open=false; return 0; }
int sim_mkdir(const char *p) {
    SIM_CARD_GUARD(p);
    if (!fake_card(p)) { (void)p; return 0; }
    if (fake_find(p)>=0 || !strcmp(p,FAKE_CARD) || !fake_parent_ok(p)) return -1;
    int f=fake_new(p); if (f<0) return -1; fake_fs[f].dir=true; return 0;
}
int sim_rmdir(const char *p) {
    SIM_CARD_GUARD(p);
    int f=fake_find(p); if (!fake_card(p) || f<0 || !fake_fs[f].dir) return -1;
    size_t n=strlen(p); for (int i=0;i<FAKE_FILES;i++) if (fake_fs[i].used && !strncmp(fake_fs[i].path,p,n) && fake_fs[i].path[n]=='/') return -1;   /* not empty */
    fake_fs[f].used=fake_fs[f].dir=false; return 0;
}
int sim_remove(const char *p) {
    SIM_CARD_GUARD(p);
    int f=fake_find(p); if (f>=0 && fake_fs[f].dir) return -1;
    if (f>=0) { free(fake_fs[f].data); fake_fs[f].data=NULL; fake_fs[f].used=false; } return 0; }
/* POSIX: a file over a file goes silently; the app must look first */
int sim_rename(const char *a,const char *b) {
    SIM_CARD_GUARD(a); SIM_CARD_GUARD(b);
    int f=fake_find(a); if (f<0) return -1;
    if (fake_card(a)!=fake_card(b)) return -1;
    if (fake_card(b) && !fake_parent_ok(b)) return -1;
    int t=fake_find(b);
    if (t>=0 && t!=f && (fake_fs[t].dir || fake_fs[f].dir)) return -1;
    if (t!=f) sim_remove(b);
    if (fake_fs[f].dir) {   /* a folder takes what is in it along */
        size_t n=strlen(a); char np[400];
        for (int i=0;i<FAKE_FILES;i++) if (fake_fs[i].used && !strncmp(fake_fs[i].path,a,n) && fake_fs[i].path[n]=='/')
        { snprintf(np,sizeof np,"%s%s",b,fake_fs[i].path+n); strlcpy(fake_fs[i].path,np,sizeof fake_fs[i].path); }
    }
    strlcpy(fake_fs[f].path,b,sizeof fake_fs[f].path); return 0;
}
int sim_fsync(int fd) {(void)fd;return 0;}
ssize_t sim_write(int fd,const void *b,size_t n) {
    if (fd<100 || fd>=108 || !fake_fds[fd-100].open) return n;
    int f=fake_fds[fd-100].file; if (fake_fds[fd-100].append) fake_fds[fd-100].pos=fake_fs[f].size;
    size_t end=fake_fds[fd-100].pos+n;
    if (end>fake_fs[f].size) { fake_fs[f].data=realloc(fake_fs[f].data,end); fake_fs[f].size=end; }
    memcpy(fake_fs[f].data+fake_fds[fd-100].pos,b,n); fake_fds[fd-100].pos=end; return n;
}
off_t sim_lseek(int fd,off_t off,int whence) {
    if (fd<100 || fd>=108 || !fake_fds[fd-100].open) return -1;
    int f=fake_fds[fd-100].file; off_t base=whence==SEEK_END?(off_t)fake_fs[f].size:whence==SEEK_CUR?(off_t)fake_fds[fd-100].pos:0;
    if (base+off<0) return -1; fake_fds[fd-100].pos=(size_t)(base+off); return base+off;
}
int sim_ftruncate(int fd,off_t len) {
    if (fd<100 || fd>=108 || !fake_fds[fd-100].open || len<0) return -1;
    int f=fake_fds[fd-100].file; if ((size_t)len>fake_fs[f].size) { fake_fs[f].data=realloc(fake_fs[f].data,len); memset(fake_fs[f].data+fake_fs[f].size,0,len-fake_fs[f].size); }
    fake_fs[f].size=(size_t)len; return 0;
}
ssize_t sim_read(int fd,void *b,size_t n) {
    if (fd<100 || fd>=108 || !fake_fds[fd-100].open) return -1;
    int f=fake_fds[fd-100].file; size_t left=fake_fds[fd-100].pos<fake_fs[f].size?fake_fs[f].size-fake_fds[fd-100].pos:0; if (n>left) n=left;
    memcpy(b,fake_fs[f].data+fake_fds[fd-100].pos,n); fake_fds[fd-100].pos+=n; return n;
}

void probe_quiz_game(void){lib_state=SL_LIB_READY;quiz_page();frame(150);lv_obj_t *b=lv_obj_get_child(pages[npages-1],1);
    lv_obj_t *grid=lv_obj_get_child(b,lv_obj_get_child_count(b)-1);lv_obj_send_event(lv_obj_get_child(grid,1),LV_EVENT_CLICKED,NULL);frame(500);}

/* ---- scrobbling (scrobble): the log on the fake card ---- */
static bool lbf_log_has(const char *needle)
{
    int f=fake_find(ROCKBOX_DIR "/sloppy/sloppy.log");
    return f>=0 && memmem(fake_fs[f].data,fake_fs[f].size,needle,strlen(needle))!=NULL;
}
static void lbf_log_print(void)
{
    int f=fake_find(ROCKBOX_DIR "/sloppy/sloppy.log");
    const char *d=(const char *)fake_fs[f].data;size_t n=fake_fs[f].size;
    for(size_t i=0;i<n;){const char *e=memchr(d+i,'\n',n-i);size_t len=e?(size_t)(e-d-i):n-i;
        if(memmem(d+i,len,"scrobble:",9))printf("  %.*s\n",(int)len,d+i);i+=len+1;}
}
/* ---- end scrobbling ---- */
static void log_dump_atexit(void)
{
    FILE *o=fopen(getenv("SLOPPY_LOG_DUMP"),"w");if(!o)return;
    static const char *const logs[2]={ROCKBOX_DIR "/sloppy/sloppy.prev.log",ROCKBOX_DIR "/sloppy/sloppy.log"};
    for(int k=0;k<2;k++){int f=fake_find(logs[k]);if(f>=0)fwrite(fake_fs[f].data,1,fake_fs[f].size,o);}
    fclose(o);
}
bool ab_scan_silent;
void audiobooks_init(void) { ab_inits++; }
bool ab_db_load(void) { return true; }
int ab_db_count(void) { return fake_books_n; }
const struct ab_book *ab_db_book(int i) { return i >= 0 && i < fake_books_n ? &fake_books[i] : NULL; }
const char *ab_db_str(const struct ab_book *b, enum ab_str s) { const char *v = fake_book_str[b - fake_books][s]; return v ? v : ""; }
struct ab_progress *ab_progress_get(uint32_t id) { for (int i = 0; i < fake_books_n; i++) if (fake_books[i].id == id && fake_prog_on[i]) return &fake_prog[i]; return NULL; }
int ab_progress_last_book(void) { for (int i = 0; i < fake_books_n; i++) if (fake_prog_on[i] && !fake_prog[i].finished) return i; return -1; }
int ab_book_permille(const struct ab_book *b, const struct ab_progress *p) { if (!p) return 0; if (p->finished) return 1000; return b->length ? (int)((uint64_t)p->position * 1000 / b->length) : 0; }
bool audiobooks_play(int book, bool over) { ab_plays = book * 2 + over; return true; }
bool audiobooks_is_book(const char *path) { (void)path; return test_book_playing; }
bool audiobooks_play_at(int book, int file, unsigned long ms) { ab_play_ms = ms; ab_plays = book * 2; ab_play_file = file; return true; }
static const char *fake_files[3] = { "/Audiobooks/Dune/01 Prologue.mp3", "/Audiobooks/Dune/02 Arrakis.mp3", "/Audiobooks/Dune/03 The Desert.mp3" };
int ab_db_list_files(const char *path, bool single, const char ***names) { (void)path; *names = fake_files; return single ? 1 : 3; }
static const uint32_t fake_lengths[3] = { 600000, 1800000, 3600000 };
const uint32_t *ab_db_lengths(const struct ab_book *b) { (void)b; return fake_lengths; }
int ab_db_find_file(const char *path, int *f) { for (int i = 0; i < 3; i++) if (!strcmp(path, fake_files[i])) { *f = i; return test_book_playing ? 1 : -1; } return -1; }
struct ab_progress *ab_progress_touch(uint32_t id) { for (int i = 0; i < fake_books_n; i++) if (fake_books[i].id == id) { fake_prog_on[i] = true; fake_prog[i].id = id; return &fake_prog[i]; } return &fake_prog[0]; }
void ab_progress_dirty(void) {}
void ab_progress_save(void) { ab_saves++; }
uint32_t ab_now(void) { return 1761200000u; }
void ab_lock(void) {}
void ab_unlock(void) {}
bool ab_db_scan(bool full, bool quiet) { (void)full; (void)quiet; ab_scans++; assert(ab_scan_silent); return true; }
