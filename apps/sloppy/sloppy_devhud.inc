/* Developer overlay: frame rate and system numbers in a black strip under
 * the status bar, for checking the interface holds 60 fps. Its switch is in
 * Settings, shown after tapping the version line at the bottom.
 * Included from sloppy.c after the status bar.
 *
 * Row 1, the frames of the last second:
 *   FPS   frames drawn ("idle" when nothing moved)
 *   avg   mean time to draw and flush one frame
 *   max   the slowest of them
 *   drop  a frame came over 25 ms after the one before, while moving
 *   slow  a frame took over 16.7 ms to draw (a 60 fps budget)
 * Row 2, the system:
 *   UI    share of the second spent in lv_timer_handler
 *   CPU   whole-system load from /proc/stat
 *   free  memory the whole system has left (amber when it runs short)
 *   temp  the SoC's thermal zone, and the CPU clock */

#define HUD_H 40
#define HUD_CELLS 10

static struct {
    lv_obj_t *bar, *cell[HUD_CELLS];
    lv_timer_t *timer;
    uint64_t render_started, last_render, window_started, busy_us;
    uint32_t frames, slow, drops, worst_us, worst_gap_us;
    uint64_t frame_sum_us;
    unsigned long long cpu_total, cpu_idle;
    bool rendering, hooked;
} dev_hud;

static void devhud_display_cb(lv_event_t *e)
{
    if (!dev_hud.bar) return;
    uint64_t now = sl_perf_now_us();
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START)
    {
        /* frames close together are one motion: a long wait between
         * them is a dropped frame */
        if (dev_hud.last_render && now - dev_hud.last_render < 200000u)
        {
            uint32_t gap = (uint32_t)(now - dev_hud.last_render);
            if (gap > 25000u) dev_hud.drops++;
            if (gap > dev_hud.worst_gap_us) dev_hud.worst_gap_us = gap;
        }
        dev_hud.last_render = dev_hud.render_started = now;
        dev_hud.rendering = true;
        return;
    }
    if (!dev_hud.rendering) return;          /* LV_EVENT_REFR_READY with nothing drawn */
    dev_hud.rendering = false;
    uint32_t took = (uint32_t)(now - dev_hud.render_started);
    dev_hud.frames++;
    dev_hud.frame_sum_us += took;
    if (took > 16667u) dev_hud.slow++;
    if (took > dev_hud.worst_us) dev_hud.worst_us = took;
}

/* the main loop reports each lv_timer_handler call */
static void devhud_busy(uint32_t us)
{
    if (dev_hud.bar) dev_hud.busy_us += us;
}

static long devhud_read_long(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    long v = -1;
    if (fscanf(fp, "%ld", &v) != 1) v = -1;
    fclose(fp);
    return v;
}

/* whole-system CPU use since the last call, or -1 without /proc */
static int devhud_cpu_pct(void)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;
    unsigned long long u = 0, n = 0, s = 0, idle = 0, io = 0, irq = 0, sirq = 0, steal = 0;
    int got = fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &idle, &io, &irq, &sirq, &steal);
    fclose(fp);
    if (got < 4) return -1;
    unsigned long long total = u + n + s + idle + io + irq + sirq + steal, idl = idle + io;
    int pct = -1;
    if (dev_hud.cpu_total && total > dev_hud.cpu_total)
        pct = (int)(100 - (idl - dev_hud.cpu_idle) * 100 / (total - dev_hud.cpu_total));
    dev_hud.cpu_total = total;
    dev_hud.cpu_idle = idl;
    return pct;
}

static void devhud_cell(int i, uint32_t color, const char *fmt, ...)
{
    char b[24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    label_if_changed(dev_hud.cell[i], b);
    lv_obj_set_style_text_color(dev_hud.cell[i], lv_color_hex(color), 0);
}

#define HUD_GOOD 0x5ee07a
#define HUD_WARN 0xf2c94c
#define HUD_BAD  0xff5a5a
#define HUD_TEXT 0xe8e8e8
#define HUD_DIM  0x8a8a8a

static void devhud_tick(lv_timer_t *t)
{
    (void)t;
    if (!dev_hud.bar) return;
    uint64_t now = sl_perf_now_us();
    uint32_t span = (uint32_t)(now - dev_hud.window_started);
    if (span < 1000) span = 1000;
    /* this refresh draws the strip once a second: that frame is ours */
    bool idle = dev_hud.frames <= 2;
    int fps = (int)((dev_hud.frames * 1000000ull + span / 2) / span);
    if (idle) devhud_cell(0, HUD_DIM, "FPS idle");
    else devhud_cell(0, fps >= 57 ? HUD_GOOD : fps >= 45 ? HUD_WARN : HUD_BAD, "FPS %d", fps);
    uint32_t avg = dev_hud.frames ? (uint32_t)(dev_hud.frame_sum_us / dev_hud.frames) : 0;
    devhud_cell(1, avg > 16667u ? HUD_BAD : avg > 10000u ? HUD_WARN : HUD_TEXT,
             "avg %lu.%lums", (unsigned long)avg / 1000, (unsigned long)avg % 1000 / 100);
    devhud_cell(2, dev_hud.worst_us > 16667u ? HUD_BAD : dev_hud.worst_us > 12000u ? HUD_WARN : HUD_TEXT,
             "max %lu.%lums", (unsigned long)dev_hud.worst_us / 1000, (unsigned long)dev_hud.worst_us % 1000 / 100);
    devhud_cell(3, dev_hud.drops ? HUD_BAD : HUD_TEXT, "drop %lu", (unsigned long)dev_hud.drops);
    devhud_cell(4, dev_hud.slow ? HUD_WARN : HUD_TEXT, "slow %lu", (unsigned long)dev_hud.slow);

    int ui = (int)(dev_hud.busy_us * 100 / span);
    devhud_cell(5, ui > 80 ? HUD_BAD : ui > 50 ? HUD_WARN : HUD_TEXT, "UI %d%%", ui > 100 ? 100 : ui);
    int cpu = devhud_cpu_pct();
    if (cpu < 0) devhud_cell(6, HUD_DIM, "CPU -");
    else devhud_cell(6, cpu > 85 ? HUD_BAD : cpu > 60 ? HUD_WARN : HUD_TEXT, "CPU %d%%", cpu);
    /* what the whole system has left: under ~8 MB everything slows (the
     * music and the code get read back off the card), so it shows amber */
    long free_kb = sl_mem_available_kb();
    if (free_kb >= 0)
        devhud_cell(7, free_kb < 8 * 1024 ? HUD_WARN : HUD_TEXT, "free %ldM", (free_kb + 512) / 1024);
    else devhud_cell(7, HUD_DIM, "free -");
    long mc = devhud_read_long("/sys/class/thermal/thermal_zone0/temp");
    if (mc > 1000) mc /= 1000;           /* millidegrees on most kernels */
    if (mc >= 0) devhud_cell(8, mc > 75 ? HUD_BAD : mc > 60 ? HUD_WARN : HUD_TEXT, "%ld C", mc);
    else devhud_cell(8, HUD_DIM, "temp -");
    long khz = devhud_read_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    if (khz > 0) devhud_cell(9, HUD_TEXT, "%ldMHz", khz / 1000);
    else devhud_cell(9, HUD_DIM, "pages %d", npages);

    dev_hud.frames = dev_hud.slow = dev_hud.drops = 0;
    dev_hud.worst_us = dev_hud.worst_gap_us = 0;
    dev_hud.frame_sum_us = dev_hud.busy_us = 0;
    dev_hud.window_started = now;
    /* toasts, the mini player and the control centre join the top layer
     * later: stay above them. Only when one did: a move in the top layer
     * redraws the whole screen, which once a second was most of a frame. */
    if (lv_obj_get_index(dev_hud.bar) != (int32_t)lv_obj_get_child_count(lv_layer_top()) - 1)
        lv_obj_move_foreground(dev_hud.bar);
}

static void devhud_show(bool on)
{
    if (on == (dev_hud.bar != NULL)) return;
    if (!on)
    {
        lv_timer_delete(dev_hud.timer);
        lv_obj_delete(dev_hud.bar);
        dev_hud.bar = NULL;
        dev_hud.timer = NULL;
        return;
    }
    if (!dev_hud.hooked)
    {
        lv_display_add_event_cb(disp, devhud_display_cb, LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(disp, devhud_display_cb, LV_EVENT_REFR_READY, NULL);
        dev_hud.hooked = true;
    }
    dev_hud.bar = box(lv_layer_top());
    lv_obj_set_size(dev_hud.bar, lv_pct(100), HUD_H);
    lv_obj_set_y(dev_hud.bar, SB_H);
    lv_obj_set_style_bg_color(dev_hud.bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dev_hud.bar, LV_OPA_COVER, 0);
    for (int i = 0; i < HUD_CELLS; i++)
    {
        lv_obj_t *l = lv_label_create(dev_hud.bar);
        lv_obj_set_style_text_font(l, &sl_sym_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(HUD_DIM), 0);
        lv_label_set_text(l, "");
        /* fixed columns, so changing numbers do not shuffle the row */
        lv_obj_set_pos(l, 8 + (i % 5) * (W - 16) / 5, i < 5 ? 2 : 21);
        dev_hud.cell[i] = l;
    }
    dev_hud.rendering = false;
    dev_hud.last_render = 0;
    dev_hud.frames = dev_hud.slow = dev_hud.drops = 0;
    dev_hud.worst_us = dev_hud.worst_gap_us = 0;
    dev_hud.frame_sum_us = dev_hud.busy_us = 0;
    dev_hud.cpu_total = dev_hud.cpu_idle = 0;
    dev_hud.window_started = sl_perf_now_us();
    devhud_cpu_pct();                       /* the first reading is the baseline */
    dev_hud.timer = lv_timer_create(devhud_tick, 1000, NULL);
    devhud_tick(NULL);
}
