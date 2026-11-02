/*
 *  Cover Flow meter: what a frame, a touch and a cover cost on the player.
 *  Counted while Cover Flow is open (a few clock reads a frame), written to
 *  the log as one summary when it closes. Histograms, so the summary can
 *  give p99 and not just an average.
 */
#define CFM_BINS 64
struct cfm_hist { uint32_t n, max, shift; uint64_t sum; uint32_t bin[CFM_BINS]; };

static void cfm_add(struct cfm_hist *h, uint32_t us)
{
    uint32_t b = us >> h->shift;
    h->bin[b < CFM_BINS ? b : CFM_BINS - 1]++;
    h->n++;
    h->sum += us;
    if (us > h->max) h->max = us;
}
/* upper edge of the bin holding the pct'th percentile, never above max */
static uint32_t cfm_pct(const struct cfm_hist *h, int pct)
{
    uint64_t want = ((uint64_t)h->n * pct + 99) / 100, seen = 0;
    for (int b = 0; b < CFM_BINS; b++)
        if ((seen += h->bin[b]) >= want && want)
        {
            uint32_t edge = (uint32_t)(b + 1) << h->shift;
            return b == CFM_BINS - 1 || edge > h->max ? h->max : edge;
        }
    return h->max;
}
static void cfm_log(const char *name, const struct cfm_hist *h)
{
    if (!h->n) { sl_log("cf %s: none", name); return; }
    sl_log("cf %s: n %lu avg %lu p50 %lu p95 %lu p99 %lu max %lu us", name, (unsigned long)h->n,
           (unsigned long)(h->sum / h->n), (unsigned long)cfm_pct(h, 50), (unsigned long)cfm_pct(h, 95),
           (unsigned long)cfm_pct(h, 99), (unsigned long)h->max);
}

static struct
{
    bool on;
    struct cfm_hist render;     /* cf_render */
    struct cfm_hist render_cpu; /* its CPU time: less than render when preempted */
    struct cfm_hist thumb_open, thumb_read;   /* a Cover Flow thumbnail: open (the folder search), read */
    struct cfm_hist refr;       /* LVGL refresh with a flush: draw, rotate, copy, present */
    struct cfm_hist rot, blit, present;   /* flush_cb parts, summed per refresh */
    struct cfm_hist frame;      /* render + refresh, for frames that drew the row */
    struct cfm_hist gap_drag, gap_coast;  /* present to present while the row moves */
    struct cfm_hist touch_gap;  /* touch reads while a finger is down: >16 ms is a wait */
    struct cfm_hist touch_lat;  /* touch read that moved the row, to that frame presented */
    struct cfm_hist art_step;   /* sl_art_step on the UI loop while open */
    struct cfm_hist loop_other; /* UI loop work outside LVGL and covers, per pass */
    struct cfm_hist settle;     /* row at rest to the covers around the middle real */
    uint64_t refr_t0, render_at, last_present, touch_us, touch_read_at, pending_touch, settle_at;
    uint32_t refr_rot, refr_blit, refr_present, refr_flushes, render_us;
    int32_t render_pos, prev_render_pos;
    bool motion, prev_motion, render_dragging;
    /* card I/O from Cover Flow: on the frame path (its timer) and at open */
    uint32_t tick_reads, tick_read_us, tick_read_max, tick_checks, tick_check_us, tick_check_max;
    uint32_t open_reads, open_read_us, open_checks, open_check_us;
    /* placeholders drawn while the album could still have a cover */
    uint64_t ph_since, ph_us, ph_centre_us, frames, ph_frames;
    int ph_state;               /* 0 none, 1 some on screen, 2 the middle one too */
    int near_ph;                /* of those, within 3 of the middle */
    bool was_moving;
    uint32_t settle_lost;       /* rests that moved on before the covers came */
    uint64_t open_at;
    uint32_t open_us, first_us, real_us;
    long avail_open;
} cfm;

static void cfm_refr_cb(lv_event_t *e)
{
    if (!cfm.on) return;
    uint64_t now = sl_perf_now_us();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START)
    {
        cfm.refr_t0 = now;
        cfm.refr_rot = cfm.refr_blit = cfm.refr_present = cfm.refr_flushes = 0;
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_RENDER_READY) { cfm.refr_flushes++; return; }
    if (lv_event_get_code(e) != LV_EVENT_REFR_READY || !cfm.refr_flushes) return;   /* nothing drawn */
    uint32_t took = (uint32_t)(now - cfm.refr_t0);
    cfm_add(&cfm.refr, took);
    cfm_add(&cfm.rot, cfm.refr_rot);
    cfm_add(&cfm.blit, cfm.refr_blit);
    cfm_add(&cfm.present, cfm.refr_present);
    if (cfm.render_at <= cfm.last_present) { cfm.last_present = now; return; }   /* labels only */
    /* this refresh showed a new render of the row */
    cfm_add(&cfm.frame, cfm.render_us + took);
    if (!cfm.first_us) cfm.first_us = (uint32_t)(now - cfm.open_at);
    if (cfm.motion && cfm.prev_motion && cfm.last_present && now - cfm.last_present < 250000)
        cfm_add(cfm.render_dragging ? &cfm.gap_drag : &cfm.gap_coast, (uint32_t)(now - cfm.last_present));
    if (cfm.pending_touch && cfm.render_at > cfm.pending_touch)
    {
        cfm_add(&cfm.touch_lat, (uint32_t)(now - cfm.pending_touch));
        cfm.pending_touch = 0;
    }
    cfm.last_present = now;
}

/* one render of the row, and whether the row had moved since the last */
static void cfm_rendered(uint64_t started, uint32_t us, int32_t pos, bool dragging)
{
    cfm_add(&cfm.render, us);
    cfm.render_us = us;
    cfm.render_at = started + us;
    cfm.prev_motion = cfm.motion;
    cfm.motion = pos != cfm.render_pos;
    cfm.prev_render_pos = cfm.render_pos;
    cfm.render_pos = pos;
    cfm.render_dragging = dragging;
}

/* placeholders on screen after a render: state 0, 1 or 2 (see ph_state) */
static void cfm_placeholders(int state, uint64_t now)
{
    cfm.frames++;
    if (state) cfm.ph_frames++;
    if (cfm.ph_state && cfm.ph_since)
    {
        cfm.ph_us += now - cfm.ph_since;
        if (cfm.ph_state == 2) cfm.ph_centre_us += now - cfm.ph_since;
    }
    cfm.ph_state = state;
    cfm.ph_since = now;
}

static void cfm_touch_read(bool down, int x, int y)
{
    static int lx = -1, ly = -1;
    if (!cfm.on) return;
    uint64_t now = sl_perf_now_us();
    if (!down) { cfm.touch_read_at = 0; lx = ly = -1; return; }
    if (cfm.touch_read_at) cfm_add(&cfm.touch_gap, (uint32_t)(now - cfm.touch_read_at));
    cfm.touch_read_at = now;
    if (x != lx || y != ly) cfm.touch_us = now;
    lx = x; ly = y;
}

static void cfm_begin(void)
{
    memset(&cfm, 0, sizeof cfm);
    cfm.render_cpu.shift = cfm.thumb_open.shift = cfm.thumb_read.shift = 9;
    cfm.render.shift = cfm.refr.shift = cfm.rot.shift = cfm.blit.shift = cfm.present.shift = 9;  /* 0.5 ms */
    cfm.frame.shift = cfm.gap_drag.shift = cfm.gap_coast.shift = cfm.touch_gap.shift = 9;
    cfm.art_step.shift = cfm.loop_other.shift = 9;
    cfm.touch_lat.shift = 10;                                                              /* 1 ms */
    cfm.settle.shift = 12;                                                                 /* 4 ms */
    cfm.render_pos = INT32_MIN;
    cfm.open_at = sl_perf_now_us();
    cfm.on = true;
#ifdef HAVE_LCD_PRESENT_STATS
    lcd_reset_present_stats();
#endif
}

static long sl_mem_available_kb(void);
static void cfm_end(void)
{
    if (!cfm.on) return;
    cfm_placeholders(0, sl_perf_now_us());
    cfm.on = false;
    unsigned long peak_kb, rss_kb = sl_perf_rss_kb(&peak_kb);
    sl_log("cf open: %lu us in cf_open (%lu reads %lu us, %lu checks %lu us), first frame %lu us, real covers %lu us",
           (unsigned long)cfm.open_us, (unsigned long)cfm.open_reads, (unsigned long)cfm.open_read_us,
           (unsigned long)cfm.open_checks, (unsigned long)cfm.open_check_us,
           (unsigned long)cfm.first_us, (unsigned long)cfm.real_us);
    cfm_log("render", &cfm.render);
    cfm_log("render cpu", &cfm.render_cpu);
    cfm_log("thumb open", &cfm.thumb_open);
    cfm_log("thumb read", &cfm.thumb_read);
    cfm_log("refresh", &cfm.refr);
    cfm_log("rotate", &cfm.rot);
    cfm_log("lcd_bitmap", &cfm.blit);
    cfm_log("present", &cfm.present);
    cfm_log("frame", &cfm.frame);
    cfm_log("gap drag", &cfm.gap_drag);
    cfm_log("gap coast", &cfm.gap_coast);
    uint32_t drops = 0;
    for (int b = 0; b < CFM_BINS; b++)
        if (((uint32_t)b << 9) >= 20000) drops += cfm.gap_drag.bin[b] + cfm.gap_coast.bin[b];
    sl_log("cf moving frames over 20 ms apart: %lu of %lu", (unsigned long)drops,
           (unsigned long)(cfm.gap_drag.n + cfm.gap_coast.n));
    cfm_log("touch gap", &cfm.touch_gap);
    cfm_log("touch to frame", &cfm.touch_lat);
    cfm_log("art step", &cfm.art_step);
    cfm_log("loop other", &cfm.loop_other);
    cfm_log("settle to real", &cfm.settle);
    sl_log("cf rests left before the covers came: %lu", (unsigned long)cfm.settle_lost);
    sl_log("cf card on frame path: %lu reads %lu us (worst %lu), %lu lookups %lu us (worst %lu)",
           (unsigned long)cfm.tick_reads, (unsigned long)cfm.tick_read_us, (unsigned long)cfm.tick_read_max,
           (unsigned long)cfm.tick_checks, (unsigned long)cfm.tick_check_us, (unsigned long)cfm.tick_check_max);
    sl_log("cf placeholders: %lu of %lu renders, %lu ms on screen, %lu ms in the middle",
           (unsigned long)cfm.ph_frames, (unsigned long)cfm.frames,
           (unsigned long)(cfm.ph_us / 1000), (unsigned long)(cfm.ph_centre_us / 1000));
    sl_log("cf memory: rss %lu KiB, peak %lu KiB, available %ld KiB at open, %ld KiB now",
           rss_kb, peak_kb, cfm.avail_open, sl_mem_available_kb());
#ifdef HAVE_LCD_PRESENT_STATS
    struct lcd_present_stats ps;
    lcd_get_present_stats(&ps);
    sl_log("cf lcd: %lu presents (%lu promoted), copy avg %lu max %lu us, flip avg %lu max %lu us, "
           "%lu vsync waits, doublebuf %d",
           ps.frames, ps.promoted_frames, ps.frames ? ps.copy_us_total / ps.frames : 0, ps.copy_us_max,
           ps.frames ? ps.present_us_total / ps.frames : 0, ps.present_us_max, ps.vsync_waits, ps.doublebuf);
#endif
}
