/* The turntable scratch filter against a synthetic song: left is a 440 Hz
 * sine (a click shows as a jump), right counts the song's frames / 4 (where
 * the needle is). */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void (*pcm_play_callback_type)(const void **start, size_t *size);
#include "../sloppy_scratch.h"

#define FS 44100
static uint32_t song_frame, song_end = 0xffffffffu;
static int16_t chunk[2048 * 2];
static int16_t sine_at(uint32_t f) { return (int16_t)lrint(12000 * sin(f * 2 * M_PI * 440 / FS)); }
static int16_t code_at(uint32_t f) { return (int16_t)((f >> 2) & 0x7fff); }
static void pull(const void **start, size_t *size)
{
    if (song_frame >= song_end) { *start = NULL; *size = 0; return; }
    for (int i = 0; i < 2048; i++) { chunk[i * 2] = sine_at(song_frame + i); chunk[i * 2 + 1] = code_at(song_frame + i); }
    song_frame += 2048;
    *start = chunk; *size = sizeof chunk;
}

static int16_t last_l;
static int worst_jump;
static long heard;
/* Plays ms of output; returns the needle code of the last frame. */
static int play(int ms, bool check_jumps)
{
    int code = -1;
    long want = (long)FS * ms / 1000, got = 0;
    while (got < want)
    {
        const void *s; size_t n;
        scr_filter(&s, &n, pull);
        if (!s) break;
        const int16_t *o = s;
        for (size_t i = 0; i < n / 4; i++)
        {
            int j = abs(o[i * 2] - last_l);
            if (check_jumps && j > worst_jump) worst_jump = j;
            last_l = o[i * 2];
            code = o[i * 2 + 1];
        }
        got += n / 4;
        heard += n / 4;
    }
    return code;
}
/* the UI turning the record: frames over ms, in 30 Hz steps */
static int turn(int32_t frames, int ms)
{
    int steps = ms / 33, code = -1;
    for (int i = 0; i < steps; i++) { scr_turn(frames / steps, 33); code = play(33, true); }
    return code;
}

int main(void)
{
    scr_ring = malloc(SCR_RING * 4);
    scr_fs = FS;
    scr_reset();

    /* 1. Nobody touching: the song passes through exactly. */
    for (int k = 0; k < 400; k++)
    {
        const void *s; size_t n;
        uint32_t at = (uint32_t)heard;
        scr_filter(&s, &n, pull);
        const int16_t *o = s;
        for (size_t i = 0; i < n / 4; i++)
            assert(o[i * 2] == sine_at(at + i) && o[i * 2 + 1] == code_at(at + i));
        heard += n / 4;
        last_l = o[(n / 4 - 1) * 2];
    }
    uint32_t pos0 = (uint32_t)heard;

    /* 2. A hand on the record stops it, smoothly, into silence. */
    worst_jump = 0;
    scr_grab();
    play(150, true);
    assert(abs(last_l) < 60);
    assert(worst_jump < 900);                  /* no faster than the song itself */
    uint32_t held_at = scr_pos_pub;
    assert(held_at >= pos0 && held_at - pos0 < FS / 20);   /* stopped within 50 ms */

    /* 3. Pulling it back half a second plays the song backwards to there. */
    int prev = code_at(held_at);
    int code = turn(-FS / 2, 264);
    assert(code < prev);
    play(80, true);                            /* the hand catches up with the finger */
    int want = code_at(held_at - FS / 2);
    assert(abs((int32_t)(scr_pos_pub - (held_at - FS / 2))) < 64 && abs(code_at(scr_pos_pub) - want) <= 16);

    /* 4. Pushing it forward a whole second goes past where it was stopped:
     * more of the song comes out of pcmbuf. */
    turn(FS, 264);
    play(80, true);
    assert(abs((int32_t)(scr_pos_pub - (held_at + FS / 2))) < 64);
    assert(worst_jump < 7000);                 /* fast, but no clicks: 8x a 440 Hz sine */

    /* 5. Quick back-and-forth: the needle follows every change of direction. */
    for (int k = 0; k < 6; k++) { turn(-FS / 8, 99); turn(FS / 8, 99); }
    play(80, true);
    assert(abs((int32_t)(scr_pos_pub - (held_at + FS / 2))) < 64);

    /* 5b. A steady hand read at 30 Hz is a steady pitch, not a wobble at
     * the touch rate; and when the finger stops, so does the record. */
    {
        float lo = 99, hi = -99;
        for (int k = 0; k < 30; k++)
        {
            scr_turn(FS / 20, 33);
            play(33, true);
            if (k > 8) { if (scr_rate < lo) lo = scr_rate; if (scr_rate > hi) hi = scr_rate; }
        }
        assert(lo > 1.2f && hi - lo < 0.1f);
        for (int k = 0; k < 4; k++) { scr_turn(0, 33); play(33, true); }
        assert(scr_rate < 0.05f && scr_rate > -0.05f);
        turn(-FS * 30 / 20, 990);
        play(80, true);
    }

    /* 6. Let go: the motor brings it to speed and it plays on from the
     * needle, then hands back to plain playback on the very next frame. */
    uint32_t from = scr_pos_pub;
    scr_release();
    play(600, true);
    assert(scr_live && scr_rate == 1);
    assert(worst_jump < 7000);
    {
        const void *s; size_t n;
        scr_filter(&s, &n, pull);
        const int16_t *o = s;
        uint32_t at = scr_pos_pub - (uint32_t)(n / 4);
        for (size_t i = 0; i < n / 4; i++) assert(o[i * 2 + 1] == code_at(at + i) && o[i * 2] == sine_at(at + i));
        last_l = o[(n / 4 - 1) * 2];
        assert(at > from && at - from < FS);  /* about 0.6 s on, no jump */
    }

    /* 7. Paused: a nudge sounds, then the record is still and says so. */
    scr_motor = false;
    scr_grab(); turn(FS / 10, 132); scr_release();
    play(400, true);
    assert(scr_still && scr_rate == 0 && abs(last_l) < 60);
    scr_motor = true;
    play(600, true);
    assert(scr_live);

    /* 8. Back further than the ring holds: the record stops at the end of
     * the groove, and the UI learns how far the hand wanted to go. */
    scr_grab();
    turn(-(int32_t)SCR_RING - FS, 990);
    play(100, true);
    assert(scr_past_groove() > 0 && scr_pos_pub - (scr_head - scr_fill) <= SCR_EDGE + 1);
    scr_release();
    assert(worst_jump < 7000);                 /* even the sudden stop at the end */

    /* 9. A stop or seek throws the groove away. */
    const void *s = NULL; size_t n = 0;
    scr_filter(NULL, NULL, NULL);
    assert(scr_live && scr_fill == 0 && scr_lag == 0);
    scr_filter(&s, &n, pull);
    assert(s && n);

    /* 10. The song ends under the hand: after release the channel finishes. */
    scr_reset();
    song_end = song_frame + 4096;
    scr_grab(); turn(FS, 132); scr_release();
    int calls = 0;
    do { scr_filter(&s, &n, pull); calls++; } while (s && calls < 2000);
    assert(!s && calls < 2000);

    (void)scr_lag_ms;
    printf("scratch: exact play-through, hand stop, backwards, forwards past the stop, "
           "back and forth, release to live, paused nudge, groove end, reset, song end pass "
           "(worst jump %d)\n", worst_jump);
    return 0;
}
