/* GabagoolOS's Tremor allocates from the C library: the player is hosted,
 * and the radio's decoder is not a codec with a buffer of its own. */
#include "os_types.h"
void *ogg_malloc(size_t size) { return malloc(size); }
void *ogg_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void *ogg_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void ogg_free(void *ptr) { free(ptr); }
void ogg_malloc_init(void) {}
void ogg_malloc_destroy(void) {}
