/* Renders the R1 boot menu off the device:
 *   cc -std=gnu99 -O1 bootloader/boot-menu-preview.c -o /tmp/boot-menu-preview
 *   /tmp/boot-menu-preview [config.cfg]   -> boot-menu-<n>.ppm */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
typedef unsigned short fb_data;
#define FB_RGBPACK(r, g, b) ((fb_data)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HZ 100
#include "sloppy_boot_menu.h"

int main(int argc, char **argv)
{
    if (argc > 1) sb_load_accent(argv[1]);
    for (int sel = 0; sel < 3; sel++)
    {
        sb_draw(sel, 270 - sel * 100, 1000);
        char name[32];
        snprintf(name, sizeof name, "boot-menu-%d.ppm", sel);
        FILE *f = fopen(name, "wb");
        fprintf(f, "P6\n%d %d\n255\n", SB_W, SB_H);
        for (int i = 0; i < SB_W * SB_H; i++)
        {
            fb_data p = sb_frame[i];
            unsigned char rgb[3] = { (p >> 11) << 3 | (p >> 13), ((p >> 5) & 63) << 2 | ((p >> 9) & 3), (p & 31) << 3 | ((p >> 2) & 7) };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    return 0;
}
