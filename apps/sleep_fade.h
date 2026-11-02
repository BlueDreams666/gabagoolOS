/***************************************************************************
 * The sleep timer's fade to silence (GabagoolOS). GPLv2+.
 *
 * The level 'done' ticks into a fade 'ticks' long, on the mixer's Q16
 * amplitude scale (65536 = unity, 0 = mute): a cubic ease, (left/ticks)^3.
 * It starts gently (the first second of a 30 s fade drops 0.9 dB), is
 * -18 dB halfway, and lands on silence with no slope left, so the pause
 * that follows has nothing to cut. No dependencies: pcmbuf.c steps it
 * once a tick and tests/sleep_fade_test.c runs it through the real mixer.
 ****************************************************************************/
#ifndef SLEEP_FADE_H
#define SLEEP_FADE_H

#include <stdint.h>

#define SLEEP_FADE_UNITY 65536u

static inline unsigned int sleep_fade_level(long done, long ticks)
{
    if (ticks <= 0 || done >= ticks)
        return 0;
    if (done <= 0)
        return SLEEP_FADE_UNITY;
    uint64_t left = (uint64_t)(ticks - done), n = (uint64_t)ticks;
    /* ticks up to an hour at 100 Hz: 360000^3 * 65536 still fits in 64 bits
     * when divided in two steps */
    return (unsigned int)(((left * left * SLEEP_FADE_UNITY) / (n * n)) * left / n);
}

#endif /* SLEEP_FADE_H */
