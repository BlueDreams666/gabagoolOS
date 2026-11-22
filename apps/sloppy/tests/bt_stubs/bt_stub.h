/* Just enough of Rockbox for apps/hiby_bluetooth.c and hibylinux_codec.c
 * to build on a host; bt_test.c supplies the functions. */
#ifndef BT_STUB_H
#define BT_STUB_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>

#define HIBY_LINUX 1
#define HAVE_HIBY_BLUETOOTH 1
#define HAVE_SLOPPY_UI 1
#define HZ 100
#define PIVOT_ROOT ""
#ifndef ROCKBOX_DIR
#define ROCKBOX_DIR "/tmp"
#endif
#define TIME_AFTER(a, b) ((long)(b) - (long)(a) < 0)
#define logf(...) do { } while (0)
#define mkdir(p) mkdir((p), 0777)

extern volatile long current_tick;
#define sleep rb_test_sleep             /* Rockbox's sleep is in ticks */
unsigned int rb_test_sleep(unsigned int ticks);

#define AUDIO_STATUS_PLAY 1
#define AUDIO_STATUS_PAUSE 2
int audio_status(void);
void audio_pause(void);
void audio_resume(void);

bool headphones_inserted(void);
bool button_add_input_device(int i);
void button_remove_input_device(int i);
bool pcm_is_initialized(void);

/* lang */
enum {
    LANG_BT_BLUETOOTH, LANG_BT_CODEC, LANG_BT_CODEC_CHANGE_FAILED, LANG_BT_CONNECTED,
    LANG_BT_CONNECTED_NO_ROUTE, LANG_BT_CONNECTING, LANG_BT_CONNECT_FAILED, LANG_BT_DEVICE,
    LANG_BT_DEVICES, LANG_BT_DISABLED, LANG_BT_DISCONNECT, LANG_BT_DISCONNECTED,
    LANG_BT_DISCONNECTING, LANG_BT_DONE, LANG_BT_ENABLED, LANG_BT_ENABLE_IT,
    LANG_BT_INITIALIZING, LANG_BT_IS_SUSPENDED, LANG_BT_LOCAL, LANG_BT_MAC,
    LANG_BT_NOT_ENABLED, LANG_BT_NO_CODECS, LANG_BT_NO_DEVICES, LANG_BT_NO_DEVICES_FOUND,
    LANG_BT_OUTPUT, LANG_BT_PAIR_FAILED, LANG_BT_SCANNING_PROGRESS, LANG_BT_SCAN_FOR_NEW,
    LANG_BT_SELECT_CODEC, LANG_BT_STATUS, LANG_BT_SUSPEND, LANG_BT_SUSPENDED,
    LANG_BT_SUSPENDED_RESUMING, LANG_BT_UNAVAILABLE, LANG_OFF, LANG_ON,
    LANG_SET_BOOL_NO, LANG_SET_BOOL_YES, LANG_UNKNOWN
};
#define str(id) ((char *)"text")
#define ID2P(id) ((char *)"text")

/* classic menus, never shown by the test */
enum { ACTION_NONE, ACTION_REDRAW, ACTION_STD_CANCEL, ACTION_STD_CONTEXT, ACTION_STD_OK };
enum { CONTEXT_STD };
enum { Icon_Submenu };
enum { YESNO_NO, YESNO_YES };
struct gui_synclist { void *data; int selected_item; };
struct simplelist_info {
    const char *(*get_name)(int, void *, char *, size_t);
    int (*action_callback)(int, struct gui_synclist *);
    int selection, title_icon, count;
    void *data;
};
struct text_message { const char **message_lines; int nb_lines; };
static inline void simplelist_info_init(struct simplelist_info *i, char *t, int c, void *d)
{ (void)t; i->count = c; i->data = d; i->selection = -1; }
static inline bool simplelist_show_list(struct simplelist_info *i) { i->selection = -1; return false; }
static inline void simplelist_reset_lines(void) {}
static inline void simplelist_addline(const char *f, ...) { (void)f; }
static inline int simplelist_get_line_count(void) { return 0; }
/* The R1's scheduler thread is the test's main thread: a splash, sleep or
 * pause asked for on any other is counted (on the R1 it froze the UI) */
#include <pthread.h>
extern pthread_t bt_test_main;
extern int bt_test_off_kernel;
static inline bool kernel_off_thread(void) { return !pthread_equal(pthread_self(), bt_test_main); }
static inline void bt_test_kernel_call(void) { if (kernel_off_thread()) __atomic_add_fetch(&bt_test_off_kernel, 1, __ATOMIC_RELAXED); }
static inline void splash(int t, const char *s) { (void)t; (void)s; bt_test_kernel_call(); }
static inline void splashf(int t, const char *s, ...) { (void)t; (void)s; bt_test_kernel_call(); }
static inline int get_action(int c, int t) { (void)c; (void)t; return ACTION_NONE; }
static inline int gui_syncyesno_run(const struct text_message *m, void *y, void *n)
{ (void)m; (void)y; (void)n; return YESNO_NO; }
static inline int confirm_delete_yesno(const char *n, void *x) { (void)n; (void)x; return YESNO_NO; }

/* codec side */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
void audiohw_set_volume(int vol_l, int vol_r);
void audiohw_preinit(void);
enum { SOUND_VOLUME };
static inline int sound_min(int s) { (void)s; return -700; }
static inline int sound_max(int s) { (void)s; return -100; }
struct test_settings { bool sloppy_gain, sloppy_digital_max; };
extern struct test_settings global_settings;
void alsa_controls_init(const char *name);
void alsa_controls_close(void);
void alsa_controls_set_bool(const char *name, bool value);
void alsa_controls_set_ints(const char *name, int count, long *values);
bool alsa_controls_get_int_range(const char *name, long *min, long *max);
bool alsa_has_control(const char *name);

/* defined by hibylinux_codec.c; target headers declare them */
void hiby_set_output(int ps);
int hiby_get_outputs(void);
#endif
