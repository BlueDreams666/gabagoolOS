/* What a kernel uevent means for the cached power readings (netlink-monitor.c).
 * GPLv2+.
 *
 * The readings in power-linux.c (is USB power in, is it charging, the level)
 * are read again only after a "battery event". That was a change of the
 * battery's own supply: with the battery full, plugging the cable in changes
 * the USB supply and not the battery, no event came, and the player went on
 * believing nothing was plugged in: no USB mode, no charging mark, until it
 * was restarted (a test R1 at 94 and 100 %, Nov 12 and 13; another at 81 %
 * charged, so its battery changed and it was noticed). Any power supply's
 * change counts, and without one the readings are still read again every
 * few seconds, so a lost event cannot hide a cable either. */
#ifndef NETLINK_UEVENT_H
#define NETLINK_UEVENT_H
#include <stdbool.h>
#include <string.h>

/* a "change@/devices/.../power_supply/<battery, usb, ...>" message */
static inline bool uevent_is_power(const char *msg)
{
    return msg[0] == 'c' && strstr(msg, "power_supply") != NULL;
}
/* no event for this long: the readings are due all the same */
#define UEVENT_POWER_STALE_TICKS(hz) (5 * (hz))
static inline bool uevent_power_stale(long now, long last_event, long hz)
{
    return now - last_event >= UEVENT_POWER_STALE_TICKS(hz);
}
#endif
