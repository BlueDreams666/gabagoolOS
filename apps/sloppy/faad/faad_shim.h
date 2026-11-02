/* GabagoolOS's copy of Rockbox's libfaad (AAC-LC), for internet radio
 * (sloppy_net.c): the same sources, built into the interface with the C
 * library's own malloc and memcpy instead of a codec's (codeclib.h is not
 * used). The IRAM attributes mean nothing on a hosted player. */
#ifndef FAAD_SHIM_H
#define FAAD_SHIM_H
#include <limits.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifndef IBSS_ATTR
#define IBSS_ATTR
#endif
#ifndef ICODE_ATTR
#define ICODE_ATTR
#endif
#ifndef ICONST_ATTR
#define ICONST_ATTR
#endif
#ifndef IDATA_ATTR
#define IDATA_ATTR
#endif
#ifndef MEM_ALIGN_ATTR
#define MEM_ALIGN_ATTR __attribute__((aligned(16)))
#endif
/* the codec library's own endian words, from the compiler's */
#if !defined(ROCKBOX_LITTLE_ENDIAN) && !defined(ROCKBOX_BIG_ENDIAN)
# if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define ROCKBOX_BIG_ENDIAN 1
# else
#  define ROCKBOX_LITTLE_ENDIAN 1
# endif
#endif
#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
/* codeclib's bit scan, as the players' compilers give it */
#define BS_LOG2  0
#define BS_CLZ   1
#define BS_SHORT 2
#define BS_0_0   4
static inline unsigned int bs_generic(unsigned int v, int mode)
{
    if (!v) return (mode & BS_CLZ) ? ((mode & BS_0_0) ? 0 : 32) : 0;
    return (mode & BS_CLZ) ? (unsigned)__builtin_clz(v) : 31u - (unsigned)__builtin_clz(v);
}
#define av_log2(v) bs_generic(v, BS_0_0)
#ifndef swap32
#define swap32(x) __builtin_bswap32((uint32_t)(x))
#endif
#endif
