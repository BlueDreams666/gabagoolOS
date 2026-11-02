/* GabagoolOS idle/input policy. GPLv2+. No hardware or LVGL dependencies. */
#ifndef SLOPPY_POWER_STATE_H
#define SLOPPY_POWER_STATE_H

#include <stdbool.h>
#include <stdint.h>

#define SL_POWER_LOCK_MS      75000u   /* default sleep; Settings may change it */
#define SL_POWER_DIM_LEAD_MS  15000u   /* dim this long before sleeping */
#define SL_POWER_WAKE_MS      15000u
#define SL_POWER_HOLD_MS       1200u   /* held this long: offer to power off */
#define SL_POWER_FORCE_MS      8000u   /* held this long: off, no questions */
#define SL_POWER_UNLOCK_PX       64
/* or a quick flick: this far, at this speed (px per second) */
#define SL_POWER_FLICK_PX        32
#define SL_POWER_FLICK_SPEED    250

enum sl_power_mode {
    SL_POWER_ACTIVE, SL_POWER_DIM, SL_POWER_LOCKED, SL_POWER_DARK
};

struct sl_power_state {
    enum sl_power_mode mode;
    uint32_t activity_at, power_at, ignored_keys;
    uint32_t sleep_ms;           /* idle time before the screen goes dark */
    bool touch_down, swallow_touch, power_down, power_fired, power_woke;
    bool saver;                  /* the screensaver over the lock screen: it stays lit while music plays */
    bool no_tap_wake;            /* Tap to wake off: a touch on a dark screen is swallowed, it stays dark */
    int touch_x, touch_y;
    uint32_t touch_at;
};

static inline void sl_power_state_init(struct sl_power_state *s, uint32_t now)
{
    *s = (struct sl_power_state){ .mode = SL_POWER_ACTIVE,
                                 .activity_at = now, .sleep_ms = SL_POWER_LOCK_MS };
}

static inline void sl_power_state_set(struct sl_power_state *s,
                                     enum sl_power_mode mode, uint32_t now)
{
    s->mode = mode;
    s->activity_at = now;
    s->saver = false;                    /* any change of mode ends the screensaver */
    /* A finger already on a control must not become an unlock gesture. */
    if (s->touch_down)
        s->swallow_touch = true;
}

/* True requests shutdown exactly once for a held power button. */
static inline bool sl_power_state_tick(struct sl_power_state *s, uint32_t now)
{
    if (s->power_down && !s->power_fired &&
        (uint32_t)(now - s->power_at) >= SL_POWER_HOLD_MS)
    {
        s->power_fired = true;
        return true;
    }
    if (s->touch_down || s->power_down)
        return false;
    uint32_t idle = now - s->activity_at;
    if (s->mode == SL_POWER_LOCKED && idle >= SL_POWER_WAKE_MS && !s->saver)
        sl_power_state_set(s, SL_POWER_DARK, now);
    else if (s->mode < SL_POWER_LOCKED)
    {
        uint32_t sleep = s->sleep_ms ? s->sleep_ms : SL_POWER_LOCK_MS;
        uint32_t dim = sleep > 2 * SL_POWER_DIM_LEAD_MS ? sleep - SL_POWER_DIM_LEAD_MS
                                                        : sleep * 2 / 3;
        if (idle >= sleep)
            sl_power_state_set(s, SL_POWER_DARK, now);
        else if (idle >= dim)
            s->mode = SL_POWER_DIM;
    }
    return false;
}

/* Consume the entire wake contact, including its release. A subsequent
 * upward swipe unlocks, and is likewise never delivered to the page below. */
static inline bool sl_power_state_touch(struct sl_power_state *s, bool down,
                                       int x, int y, uint32_t now)
{
    if (down)
    {
        if (!s->touch_down)
        {
            s->touch_down = true;
            s->touch_x = x;
            s->touch_y = y;
            s->touch_at = now;
            s->swallow_touch = false;
            if (s->mode == SL_POWER_DARK)
            {
                s->swallow_touch = true;
                if (s->no_tap_wake)
                    return true;         /* a pocket, not a wake: only a key lights it */
                s->mode = SL_POWER_LOCKED;
            }
            else if (s->mode == SL_POWER_DIM)
                s->mode = SL_POWER_ACTIVE; /* the dim screen is already readable */
        }
        s->activity_at = now;
        return s->swallow_touch || s->mode >= SL_POWER_LOCKED;
    }

    bool consume = s->swallow_touch || s->mode >= SL_POWER_LOCKED;
    if (s->touch_down && s->mode == SL_POWER_LOCKED && !s->swallow_touch)
    {
        int dy = s->touch_y - y;
        int dx = x - s->touch_x;
        uint32_t ms = now - s->touch_at;
        bool flick = dy >= SL_POWER_FLICK_PX &&
                     (uint32_t)dy * 1000u >= SL_POWER_FLICK_SPEED * (ms ? ms : 1);
        /* a thumb swipes on a slant: up to 1.5x as far sideways as up */
        if ((dy >= SL_POWER_UNLOCK_PX || flick) && 2 * dx <= 3 * dy && -2 * dx <= 3 * dy)
            sl_power_state_set(s, SL_POWER_ACTIVE, now);
    }
    s->touch_down = false;
    s->swallow_touch = false;
    return consume;
}

/* Local media/volume keys wake a sleeping display but cannot operate the
 * player until it is unlocked. Callers can exempt Bluetooth remote keys. */
static inline bool sl_power_state_key(struct sl_power_state *s, uint32_t key,
                                     bool released, uint32_t now)
{
    bool consume = (s->ignored_keys & key) != 0;
    if (s->mode >= SL_POWER_LOCKED)
    {
        consume = true;
        if (!released)
            sl_power_state_set(s, SL_POWER_LOCKED, now);
    }
    else if (s->mode == SL_POWER_DIM)
    {
        sl_power_state_set(s, SL_POWER_ACTIVE, now);
        consume = true;
    }
    s->activity_at = now;
    if (released)
        s->ignored_keys &= ~key;
    else if (consume)
        s->ignored_keys |= key;
    return consume;
}

/* A press on a dark screen wakes it to the lock screen at once, rather
 * than on release; a short press otherwise locks/sleeps; a hold shuts down.
 * A release with no preceding press is ignored (e.g. after a system dialog). */
static inline bool sl_power_state_power(struct sl_power_state *s,
                                       bool released, bool repeated,
                                       uint32_t now)
{
    if (!released)
    {
        if (!repeated && !s->power_down)
        {
            s->power_down = true;
            s->power_fired = false;
            s->power_at = now;
            s->power_woke = s->mode == SL_POWER_DARK;
            if (s->power_woke)
                sl_power_state_set(s, SL_POWER_LOCKED, now);
        }
        return false;
    }
    if (!s->power_down)
        return false;
    s->power_down = false;
    if (s->power_fired)
        return false;
    if ((uint32_t)(now - s->power_at) >= SL_POWER_HOLD_MS)
    {
        s->power_fired = true;
        return true;
    }
    if (s->power_woke)                   /* the press already woke it */
        return false;
    sl_power_state_set(s, s->mode == SL_POWER_DARK
                      ? SL_POWER_LOCKED : SL_POWER_DARK, now);
    return false;
}

#endif
