#ifndef SLOPPY_LV_ASSERT_H
#define SLOPPY_LV_ASSERT_H
/* LVGL's asserts (a NULL argument, a failed allocation) halt the main loop.
 * This writes where to sloppy.log first: a silent 'lvgl' hang names itself. */
void sl_lv_assert(const char *file, int line);
#endif
