/**
 * @file lv_draw_sw_private.h
 *
 */

#ifndef LV_DRAW_SW_PRIVATE_H
#define LV_DRAW_SW_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_sw.h"
#include "../lv_draw_private.h"

#if LV_USE_DRAW_SW

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *      TYPEDEFS
 **********************/

struct lv_draw_sw_unit_t {
    lv_draw_unit_t base_unit;
    lv_draw_task_t * task_act;
#if LV_USE_OS
    lv_thread_sync_t sync;
    lv_thread_t thread;
    volatile bool inited;
    volatile bool exit_status;
#endif
    uint32_t idx;
};

#if LV_DRAW_SW_SHADOW_CACHE_SIZE
/*SloppyOS: a few corners, not one: the mini player's and a cover's shadow
 *alternate on every frame of a scroll. Keyed on the box's size too, which
 *the blur reads, so a hit is the same bytes the blur would make.*/
#define LV_DRAW_SW_SHADOW_CACHE_N 4
typedef struct {
    struct {
        uint8_t cache[LV_DRAW_SW_SHADOW_CACHE_SIZE * LV_DRAW_SW_SHADOW_CACHE_SIZE];
        int32_t cache_size;
        int32_t cache_r;
        int32_t cache_w, cache_h;
    } e[LV_DRAW_SW_SHADOW_CACHE_N];
    uint32_t next;
} lv_draw_sw_shadow_cache_t;
#endif

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/

#endif /* LV_USE_DRAW_SW */

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_DRAW_SW_PRIVATE_H*/
