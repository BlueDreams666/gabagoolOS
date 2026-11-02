/***************************************************************************
 * Card guard: while the card is handed to a computer (USB mode, from the
 * handover until it is mounted again) nothing writes to it. A write then
 * would keep it busy (umount: EBUSY), or land under the empty mount point
 * and hide there once the card is back. Shared by filesystem-app.c and the
 * UI harness's fake card, so the harness tests this very check.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 or later.
 ****************************************************************************/
#ifndef CARD_GUARD_H
#define CARD_GUARD_H
#include <stdbool.h>
#include <string.h>

/* fpath is on the card at root ("/data/mnt/sd_0"; the kernel also says
 * "/usr/data/mnt/sd_0", /data being a link to usr/data) and the card is away */
static inline bool card_guard_refuses(const char *fpath, bool away, const char *root)
{
    if (!away || !fpath)
        return false;
    char usr[64];
    const char *roots[2] = { root, usr };
    if (strlen(root) + 5 > sizeof usr)
        return true;                     /* an odd root: refuse rather than guess */
    strcpy(usr, "/usr");
    strcat(usr, root);
    for (int i = 0; i < 2; i++)
    {
        size_t n = strlen(roots[i]);
        if (!strncmp(fpath, roots[i], n) && (fpath[n] == '/' || fpath[n] == '\0' || roots[i][n ? n - 1 : 0] == '/'))
            return true;
    }
    return false;
}
#endif
