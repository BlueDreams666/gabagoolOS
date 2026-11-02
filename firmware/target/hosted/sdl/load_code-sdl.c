/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 Daniel Stenberg
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#define RB_FILESYSTEM_OS
#include <SDL_loadso.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "load_code.h"
#include "filesystem-sdl.h"
#include "debug.h"

void * os_lc_open(const char *ospath)
{
    void *handle = SDL_LoadObject(ospath);
#ifdef SIMULATOR
    /* Let a desktop simulator run freshly built codecs without installing
     * them into its simulated .rockbox directory (or an SD card). */
    if (handle == NULL && ospath != NULL)
    {
        const char *name = strrchr(ospath, '/');
        const char *build_dir = getenv("ROCKBOX_CODEC_BUILD_DIR");
        char build_codec[512];
        name = name ? name + 1 : ospath;
        snprintf(build_codec, sizeof build_codec, "%s/%s",
                 build_dir ? build_dir : "lib/rbcodec/codecs", name);
        handle = SDL_LoadObject(build_codec);
    }
#endif
    if (handle == NULL)
    {
        DEBUGF("%s(\"%s\") failed\n", __func__, ospath);
        DEBUGF("  SDL error '%s'\n", SDL_GetError());
    }

    return handle;
}

void * lc_get_header(void *handle)
{
    char *ret = SDL_LoadFunction(handle, "__header");
    if (ret == NULL)
        ret = SDL_LoadFunction(handle, "___header");

    return ret;
}

void lc_close(void *handle)
{
    SDL_UnloadObject(handle);
}
