/* GabagoolOS e-books: EPUB 2 and 3 read straight from the zip, a chapter
 * at a time. The zip's directory, container.xml, the OPF (title, author,
 * language, spine, cover) and the table of contents (EPUB 3 nav, else
 * EPUB 2 NCX) are read on a worker thread, and each chapter's XHTML is
 * inflated (lodepng, through sl_inflate) and reduced to paragraphs there
 * too: headings, emphasis when a whole paragraph is, quotes, list items,
 * line breaks, a quiet line for each picture, nothing of scripts and
 * styles. The interface lays the paragraphs out in pages itself
 * (sloppy_reader.inc). The shelf (the books on the card, their covers)
 * and where each book was left are kept under SL_EPUB_STATE.
 * Plain POSIX, like sloppy_net.c: paths here are as Rockbox names them. */
#ifndef SLOPPY_EPUB_H
#define SLOPPY_EPUB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SL_EPUB_STATE "/.rockbox/sloppy/books"
#define SL_EPUB_DIR "/Books"
#define SL_EPUB_XHTML_CAP (512 * 1024)       /* one chapter's XHTML at most: the rest of a longer one is cut, with a note */
#define SL_EPUB_PARAS_MAX 16000              /* paragraphs in a chapter */
#define SL_EPUB_ENTRIES_MAX 4096             /* files in a book's zip */
#define SL_EPUB_SPINE_MAX 2000
#define SL_EPUB_TOC_MAX 400
#define SL_EPUB_SHELF_MAX 300
#define SL_EPUB_COVER_W 141                  /* a cover on the shelf, 2:3 */
#define SL_EPUB_COVER_H 212

enum { SL_EPUB_OK, SL_EPUB_ERR_OPEN, SL_EPUB_ERR_ZIP, SL_EPUB_ERR_OPF, SL_EPUB_ERR_EMPTY, SL_EPUB_ERR_MEM,
       SL_EPUB_ERR_DRM, SL_EPUB_ERR_HELD };

struct sl_epub_ent { uint32_t name, hash, lho, csize, usize; uint16_t method; };
struct sl_epub_toc { uint32_t title; int16_t spine; uint8_t depth; uint32_t frag; };   /* frag: the #id's hash, 0 none */
struct sl_epub {
    char path[300];                           /* as Rockbox names it */
    char title[192], author[128], lang[16];
    bool rtl;                                 /* pages turn from the right: Hebrew, Arabic, or the book says so */
    int nspine;
    int *spine;                               /* each part's entry */
    uint32_t *spine_at;                       /* its XHTML bytes before it, for how far through the book is */
    uint64_t total;
    int ntoc;
    struct sl_epub_toc *toc;
    char *pool;                               /* the contents' titles */
    size_t pool_n, names_n;                   /* bytes in pool; in names */
    int nent;
    struct sl_epub_ent *ent;
    char *names;
    int cover;                                /* the cover picture's entry, -1 none */
    bool cut;                                 /* the zip's directory was missing (a download cut short): read from its headers */
};
/* a paragraph of a chapter: its words at text + off, len bytes, a NUL after */
enum { SL_EP_P, SL_EP_H1, SL_EP_H2, SL_EP_H3, SL_EP_PIC, SL_EP_RULE };
enum { SL_EPF_EM = 1, SL_EPF_QUOTE = 2, SL_EPF_PRE = 4, SL_EPF_LI = 8 };
struct sl_epub_para { uint32_t off, len; uint8_t kind, flags; };
struct sl_epub_chap {
    int spine;
    char *text;
    size_t n;
    struct sl_epub_para *p;
    int np;
    uint32_t *anchor;                         /* an id's hash ... */
    int *anchor_para;                         /* ... and the paragraph it starts */
    int nanchor;
    bool cut;                                 /* longer than SL_EPUB_XHTML_CAP: the rest is not here */
    bool missing;                             /* the zip has no such part, or it could not be read */
    size_t bytes;                             /* what it holds in memory */
};

/* synchronous: the worker's own, and the tests' */
struct sl_epub *sl_epub_open(const char *path, int *err);
struct sl_epub_chap *sl_epub_chapter(const struct sl_epub *b, int spine);
void sl_epub_free(struct sl_epub *b);
void sl_epub_chap_free(struct sl_epub_chap *c);
const char *sl_epub_toc_title(const struct sl_epub *b, int k);
int sl_epub_toc_at(const struct sl_epub *b, int spine);     /* the contents line a part is under, -1 none */
int sl_epub_anchor(const struct sl_epub_chap *c, uint32_t frag);   /* the paragraph an id starts, 0 when unknown */
uint32_t sl_epub_hash(const char *s);

/* the worker: one book opened, or one of its chapters read, at a time; the
 * newest asked for wins (an older one's result is dropped). Polled. */
enum { SL_EPUB_J_NONE, SL_EPUB_J_OPEN, SL_EPUB_J_CHAP };
unsigned sl_epub_open_start(const char *path);
unsigned sl_epub_chapter_start(struct sl_epub *b, int spine);
/* what finished: its kind, and the ticket the start returned */
int sl_epub_poll(unsigned *ticket, struct sl_epub **book, struct sl_epub_chap **chap, int *err);
void sl_epub_drop(void);                      /* the reader closed: anything still coming is freed when it comes */
void sl_epub_release(struct sl_epub *b);      /* freed now, or when the worker is done with it */
bool sl_epub_busy(void);

/* the shelf: every .epub under /Books (and a few other usual places) */
struct sl_epub_book {
    char path[256];
    char title[128], author[96];
    uint32_t size, mtime, hash;
    uint8_t cover, bad, rtl;
};
void sl_epub_shelf_scan(void);                /* on the worker; cheap for books seen before */
bool sl_epub_shelf_scanning(void);
unsigned sl_epub_shelf_seq(void);             /* moves each time the shelf changes */
int sl_epub_shelf(struct sl_epub_book **out); /* a copy (the caller frees it), sorted by title */
void sl_epub_cover_name(char *out, size_t n, uint32_t hash);   /* the cover's LVGL picture, as Rockbox names it */

/* where each book was left, and the reader's own look */
struct sl_epub_pos { int spine, para; uint32_t off; uint16_t permille; uint32_t when; };
bool sl_epub_pos_get(const char *path, struct sl_epub_pos *p);
void sl_epub_pos_set(const char *path, const struct sl_epub_pos *p);
bool sl_epub_pos_last(char *path, size_t n);  /* the book read last */
void sl_epub_prefs(int *theme, int *size);    /* -1 until chosen */
void sl_epub_prefs_set(int theme, int size);
void sl_epub_save(bool now);                  /* now: on this thread (USB mode, power off); else on the worker */
void sl_epub_hold(bool hold);                 /* USB mode: nothing read or written */
void sl_epub_forget(void);                    /* the tests: everything in memory dropped */

/* the tests: what this module holds now, and the most it has */
size_t sl_epub_mem_now(void);
size_t sl_epub_mem_peak(void);
void sl_epub_mem_reset_peak(void);
#endif
