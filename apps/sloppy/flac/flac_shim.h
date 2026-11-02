/* What the FLAC decoder took from Rockbox's codec interface (codeclib.h,
 * platform.h), for the copy in GabagoolOS's internet radio. */
#ifndef FLAC_SHIM_H
#define FLAC_SHIM_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef ICODE_ATTR
#define ICODE_ATTR
#endif
#ifndef IBSS_ATTR
#define IBSS_ATTR
#endif
#ifndef ICONST_ATTR
#define ICONST_ATTR
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef av_log2
static inline int av_log2(unsigned v) { return v ? 31 - __builtin_clz(v) : 0; }   /* floor(log2), and 0 for 0 */
#endif
#ifndef swap16
#define swap16(v) ((uint16_t)(((uint16_t)(v)) >> 8 | ((uint16_t)(v)) << 8))
#endif
#ifndef swap32
#define swap32(v) ((uint32_t)(((uint32_t)(v)) >> 24 | (((uint32_t)(v)) >> 8 & 0xff00) | (((uint32_t)(v)) << 8 & 0xff0000) | ((uint32_t)(v)) << 24))
#endif
#endif
