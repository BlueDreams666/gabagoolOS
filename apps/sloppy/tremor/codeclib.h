/* What Tremor and the MDCT took from Rockbox's codec interface
 * (codeclib.h, codecs.h, platform.h), for the copy in GabagoolOS's
 * internet radio: plain malloc, no IRAM, the transforms below. */
#ifndef __CODECLIB_H__
#define __CODECLIB_H__
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#ifndef ROCKBOX_BIG_ENDIAN
#define ROCKBOX_BIG_ENDIAN 1
#endif
#elif !defined(ROCKBOX_LITTLE_ENDIAN)
#define ROCKBOX_LITTLE_ENDIAN 1
#endif
#include "fft.h"
#include "mdct.h"
#ifndef ICODE_ATTR
#define ICODE_ATTR
#endif
#ifndef IBSS_ATTR
#define IBSS_ATTR
#endif
#ifndef ICONST_ATTR
#define ICONST_ATTR
#endif
#ifndef MEM_ALIGN_ATTR
#define MEM_ALIGN_ATTR __attribute__((aligned(16)))
#endif
#ifndef ICODE_ATTR_TREMOR_MDCT
#define ICODE_ATTR_TREMOR_MDCT
#endif
#ifndef ICODE_ATTR_TREMOR_NOT_MDCT
#define ICODE_ATTR_TREMOR_NOT_MDCT
#endif
#ifndef ICONST_ATTR_TREMOR_WINDOW
#define ICONST_ATTR_TREMOR_WINDOW
#endif
#ifndef IBSS_ATTR_TREMOR_LARGE
#define IBSS_ATTR_TREMOR_LARGE
#endif
#ifndef letoh32
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define letoh32(x) __builtin_bswap32(x)
#define betoh32(x) (x)
#else
#define letoh32(x) (x)
#define betoh32(x) __builtin_bswap32(x)
#endif
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
extern void ff_imdct_half(unsigned int nbits, int32_t *output, const int32_t *input);
extern void ff_imdct_calc(unsigned int nbits, int32_t *output, const int32_t *input);
extern void ff_fft_calc_c(int nbits, FFTComplex *z);
#endif
