/* Host regression tests: cc -std=c99 -Wall -Wextra -Werror test_power.c -o test_power */
#include <assert.h>
#include <stdio.h>
#include "../../apps/sloppy/sloppy_power_state.h"

static void test_sleep_setting(void)
{
    static const uint32_t sleeps[] = { 30000, 60000, 120000 }, dims[] = { 20000, 45000, 105000 };
    for (int i = 0; i < 3; i++)
    {
        struct sl_power_state s;
        sl_power_state_init(&s, 0);
        s.sleep_ms = sleeps[i];
        sl_power_state_tick(&s, dims[i] - 1);
        assert(s.mode == SL_POWER_ACTIVE);
        sl_power_state_tick(&s, dims[i]);
        assert(s.mode == SL_POWER_DIM);
        sl_power_state_tick(&s, sleeps[i] - 1);
        assert(s.mode == SL_POWER_DIM);
        sl_power_state_tick(&s, sleeps[i]);
        assert(s.mode == SL_POWER_DARK);
    }
}

static void test_idle_deadlines(uint32_t start)
{
    struct sl_power_state s;
    sl_power_state_init(&s, start);
    assert(!sl_power_state_tick(&s, start + 59999));
    assert(s.mode == SL_POWER_ACTIVE);
    assert(!sl_power_state_tick(&s, start + 60000));
    assert(s.mode == SL_POWER_DIM);
    assert(!sl_power_state_tick(&s, start + 74999));
    assert(s.mode == SL_POWER_DIM);
    assert(!sl_power_state_tick(&s, start + 75000));
    assert(s.mode == SL_POWER_DARK);
}

static void test_touch_activity(void)
{
    struct sl_power_state s;
    sl_power_state_init(&s, 0);
    assert(!sl_power_state_touch(&s, true, 120, 200, 59999));
    assert(!sl_power_state_touch(&s, false, 120, 200, 60010));
    sl_power_state_tick(&s, 75000);
    assert(s.mode == SL_POWER_ACTIVE);
    /* A stationary finger on a slider must never lock mid-gesture. */
    assert(!sl_power_state_touch(&s, true, 100, 100, 80000));
    sl_power_state_tick(&s, 200000);
    assert(s.mode == SL_POWER_ACTIVE);
    sl_power_state_touch(&s, true, 100, 100, 200001);
    sl_power_state_touch(&s, false, 100, 100, 200002);
    sl_power_state_tick(&s, 200003);
    assert(s.mode == SL_POWER_ACTIVE);
}

/* A dimmed screen is still readable, so a touch there brightens it and acts
 * at once (aa803a844); only a dark screen swallows the waking contact. */
static void test_dim_touch_acts(void)
{
    struct sl_power_state s;
    sl_power_state_init(&s, 0);
    sl_power_state_tick(&s, 60000);
    assert(s.mode == SL_POWER_DIM);
    assert(!sl_power_state_touch(&s, true, 50, 50, 60001));
    assert(s.mode == SL_POWER_ACTIVE);
    assert(!sl_power_state_touch(&s, false, 50, 50, 60002));
    sl_power_state_tick(&s, 60001 + 74999);
    assert(s.mode == SL_POWER_DIM);          /* the touch restarted the idle clock */
}

static void test_lock_wake_and_swipe(void)
{
    struct sl_power_state s;
    sl_power_state_init(&s, 0);
    sl_power_state_tick(&s, 75000);
    assert(sl_power_state_touch(&s, true, 200, 600, 75001));
    assert(s.mode == SL_POWER_LOCKED);
    /* Even a full upward swipe only wakes on the first contact. */
    assert(sl_power_state_touch(&s, true, 200, 200, 75002));
    assert(sl_power_state_touch(&s, false, 200, 200, 75003));
    assert(s.mode == SL_POWER_LOCKED);
    assert(sl_power_state_touch(&s, true, 200, 600, 75004));
    assert(sl_power_state_touch(&s, false, 450, 490, 75005));
    assert(s.mode == SL_POWER_LOCKED); /* mostly horizontal */
    assert(sl_power_state_touch(&s, true, 200, 600, 75006));
    assert(sl_power_state_touch(&s, false, 200, 504, 75007));
    assert(s.mode == SL_POWER_ACTIVE);
    assert(!sl_power_state_touch(&s, true, 200, 600, 75008));
    assert(!sl_power_state_touch(&s, false, 200, 600, 75009));
}

static void test_wake_timeout_and_local_keys(void)
{
    struct sl_power_state s;
    sl_power_state_init(&s, 0);
    sl_power_state_tick(&s, 75000);
    assert(sl_power_state_key(&s, 4, false, 80000));
    assert(s.mode == SL_POWER_LOCKED);
    assert(sl_power_state_key(&s, 4, false, 80001));
    assert(sl_power_state_key(&s, 4, true, 80002));
    sl_power_state_tick(&s, 95001);
    assert(s.mode == SL_POWER_LOCKED);
    sl_power_state_tick(&s, 95002);
    assert(s.mode == SL_POWER_DARK);
    sl_power_state_init(&s, 0);
    sl_power_state_tick(&s, 60000);
    assert(sl_power_state_key(&s, 4, false, 60001));
    assert(s.mode == SL_POWER_ACTIVE);
    assert(sl_power_state_key(&s, 4, false, 60002));
    assert(sl_power_state_key(&s, 4, true, 60003));
    assert(!sl_power_state_key(&s, 4, false, 60004));
    assert(!sl_power_state_key(&s, 4, true, 60005));
}

static void test_power_short_and_long_press(void)
{
    struct sl_power_state s;
    sl_power_state_init(&s, 0);
    assert(!sl_power_state_power(&s, true, false, 1));
    assert(s.mode == SL_POWER_ACTIVE); /* orphan release */
    assert(!sl_power_state_power(&s, false, false, 100));
    assert(!sl_power_state_power(&s, true, false, 250));
    assert(s.mode == SL_POWER_DARK);
    assert(!sl_power_state_power(&s, false, false, 1000));
    assert(!sl_power_state_power(&s, true, false, 1100));
    assert(s.mode == SL_POWER_LOCKED);
    assert(!sl_power_state_power(&s, false, false, 1200));
    assert(!sl_power_state_power(&s, true, false, 1300));
    assert(s.mode == SL_POWER_DARK);
    assert(!sl_power_state_power(&s, false, false, 2000));
    assert(!sl_power_state_tick(&s, 4999));
    assert(sl_power_state_tick(&s, 5000));
    assert(!sl_power_state_tick(&s, 5100));
    assert(!sl_power_state_power(&s, true, false, 5200));
    assert(s.mode == SL_POWER_DARK);
    /* Delayed polling must not turn a long hold into a short press. */
    assert(!sl_power_state_power(&s, false, false, 6000));
    assert(sl_power_state_power(&s, true, false, 10000));
}

int main(void)
{
    test_sleep_setting();
    test_idle_deadlines(0);
    test_idle_deadlines(UINT32_MAX - 50000); /* monotonic counter wrap */
    test_touch_activity();
    test_dim_touch_acts();
    test_lock_wake_and_swipe();
    test_wake_timeout_and_local_keys();
    test_power_short_and_long_press();
    puts("SloppyOS power state tests passed");
    return 0;
}
