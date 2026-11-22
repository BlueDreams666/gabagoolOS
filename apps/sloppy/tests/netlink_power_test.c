/* cc -I firmware/target/hosted apps/sloppy/tests/netlink_power_test.c -o /tmp/netlink_power_test
 * Which kernel uevents make the player read its power supplies again
 * (firmware/target/hosted/netlink-uevent.h). */
#include <assert.h>
#include <stdio.h>
#include "netlink-uevent.h"

int main(void)
{
    /* a cable into a full player: only the USB supply changes. Taken for
     * nothing (the rule was the word "battery"), the player never saw the
     * cable: no USB mode until it was restarted */
    assert(uevent_is_power("change@/devices/i2c-0/0-0062/power_supply/usb"));
    assert(uevent_is_power("change@/devices/i2c-0/0-0062/power_supply/battery"));
    assert(uevent_is_power("change@/devices/platform/charger/power_supply/mp2731-charger"));
    /* not the headphone switch, a key device coming or going, or a supply's add */
    assert(!uevent_is_power("change@/devices/virtual/switch/headset"));
    assert(!uevent_is_power("add@/devices/virtual/input/input7"));
    assert(!uevent_is_power("remove@/devices/i2c-0/0-0062/power_supply/usb"));
    /* and with no event at all, the supplies are read again within 5 s */
    const long hz = 100;
    assert(!uevent_power_stale(1000, 1000, hz) && !uevent_power_stale(1000 + 499, 1000, hz));
    assert(uevent_power_stale(1000 + 500, 1000, hz) && uevent_power_stale(999999, 1000, hz));
    printf("PASS: a change of any power supply has the player read them again, and 5 s without one does too.\n");
    return 0;
}
