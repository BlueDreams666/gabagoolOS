#include <stdbool.h>
#define TAGCACHE_BUFSZ (MAX_PATH * 2 + 32)
enum { tag_filename, tag_title, tag_albumartist, tag_artist, tag_album,
       tag_tracknumber, tag_discnumber, tag_length, tag_year, tag_mtime, tag_genre };
struct tagcache_search { int idx_id, entry_count; };
struct tagcache_stat { const char *db_path; bool ready; };
bool tagcache_is_usable(void);
struct tagcache_stat *tagcache_get_stat(void);
bool tagcache_search(struct tagcache_search *, int);
bool tagcache_get_next(struct tagcache_search *, char *, long);
bool tagcache_retrieve(struct tagcache_search *, int, int, char *, long);
long tagcache_get_numeric(const struct tagcache_search *, int);
void tagcache_search_finish(struct tagcache_search *);
