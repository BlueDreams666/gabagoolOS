/* GabagoolOS e-books, the plain-POSIX half: see sloppy_epub.h. */
#define _GNU_SOURCE 1                   /* memmem */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#include "lvgl.h"
#include "src/misc/lv_text_ap.h"
#include "sloppy_epub.h"
#include "sloppy_png.h"

const char *sl_net_card_root(void);          /* sloppy_net.c */
unsigned char *sl_art_rgb_load(const unsigned char *bytes, int len, int *w, int *h);   /* sloppy_art.c */
void sl_art_rgb_free(unsigned char *rgb);

/* ---------------------------------------------------------------- memory, counted */
static pthread_mutex_t ep_mem_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t ep_mem, ep_mem_top;
struct ep_hd { size_t n; size_t pad; };
static void *ep_alloc(size_t n)
{
    struct ep_hd *h = malloc(sizeof *h + n);
    if (!h) return NULL;
    h->n = n;
    pthread_mutex_lock(&ep_mem_lock);
    ep_mem += n;
    if (ep_mem > ep_mem_top) ep_mem_top = ep_mem;
    pthread_mutex_unlock(&ep_mem_lock);
    return h + 1;
}
static void *ep_zalloc(size_t n) { void *p = ep_alloc(n); if (p) memset(p, 0, n); return p; }
static void ep_free(void *p)
{
    if (!p) return;
    struct ep_hd *h = (struct ep_hd *)p - 1;
    pthread_mutex_lock(&ep_mem_lock);
    ep_mem -= h->n;
    pthread_mutex_unlock(&ep_mem_lock);
    free(h);
}
static void *ep_realloc(void *p, size_t n)
{
    if (!p) return ep_alloc(n);
    struct ep_hd *h = (struct ep_hd *)p - 1;
    size_t was = h->n;
    struct ep_hd *g = realloc(h, sizeof *g + n);
    if (!g) return NULL;
    g->n = n;
    pthread_mutex_lock(&ep_mem_lock);
    ep_mem = ep_mem - was + n;
    if (ep_mem > ep_mem_top) ep_mem_top = ep_mem;
    pthread_mutex_unlock(&ep_mem_lock);
    return g + 1;
}
/* lodepng's own buffer, counted as it was at its largest */
static void ep_count_foreign(size_t n, int sign)
{
    pthread_mutex_lock(&ep_mem_lock);
    if (sign > 0) { ep_mem += n; if (ep_mem > ep_mem_top) ep_mem_top = ep_mem; }
    else ep_mem -= n;
    pthread_mutex_unlock(&ep_mem_lock);
}
size_t sl_epub_mem_now(void) { pthread_mutex_lock(&ep_mem_lock); size_t n = ep_mem; pthread_mutex_unlock(&ep_mem_lock); return n; }
size_t sl_epub_mem_peak(void) { pthread_mutex_lock(&ep_mem_lock); size_t n = ep_mem_top; pthread_mutex_unlock(&ep_mem_lock); return n; }
void sl_epub_mem_reset_peak(void) { pthread_mutex_lock(&ep_mem_lock); ep_mem_top = ep_mem; pthread_mutex_unlock(&ep_mem_lock); }

/* ---------------------------------------------------------------- small helpers */
static volatile bool ep_held;                /* USB mode */
uint32_t sl_epub_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h ? h : 1;
}
static bool ep_real(char *out, size_t n, const char *rb)
{
    const char *root = sl_net_card_root();
    return root && snprintf(out, n, "%s%s", root, rb) < (int)n;
}
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static bool ep_pread(int fd, void *b, size_t n, off_t at)
{
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = pread(fd, (char *)b + got, n - got, at + (off_t)got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}
static size_t ep_utf8(char *o, uint32_t c)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xc0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xe0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    if (c < 0x110000) { o[0] = (char)(0xf0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); return 4; }
    return 0;
}
/* &...; at s (s[0] == '&', up to end): its character into o, how many bytes it took in *used; 0 not one */
static size_t ep_entity(const char *s, const char *end, char *o, size_t *used)
{
    static const struct { const char *n; uint32_t c; } named[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
        { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
        { "ldquo", 0x201c }, { "rdquo", 0x201d }, { "laquo", 0xab }, { "raquo", 0xbb }, { "copy", 0xa9 },
        { "reg", 0xae }, { "trade", 0x2122 }, { "deg", 0xb0 }, { "middot", 0xb7 }, { "bull", 0x2022 },
        { "shy", 0xad }, { "eacute", 0xe9 }, { "egrave", 0xe8 }, { "aacute", 0xe1 }, { "agrave", 0xe0 },
        { "ouml", 0xf6 }, { "uuml", 0xfc }, { "auml", 0xe4 }, { "szlig", 0xdf }, { "ccedil", 0xe7 },
        { "iacute", 0xed }, { "oacute", 0xf3 }, { "uacute", 0xfa }, { "ntilde", 0xf1 }, { "thinsp", ' ' },
        { "ensp", ' ' }, { "emsp", ' ' }, { "zwnj", 0x200c }, { "zwj", 0x200d }, { "lrm", 0x200e }, { "rlm", 0x200f },
    };
    const char *semi = s + 1;
    while (semi < end && semi - s < 12 && *semi != ';' && *semi != '&' && *semi != '<') semi++;
    if (semi >= end || *semi != ';') return 0;
    uint32_t c = 0;
    if (s[1] == '#')
    {
        const char *q = s + 2;
        int base = 10;
        if (q < semi && (*q == 'x' || *q == 'X')) { base = 16; q++; }
        if (q == semi) return 0;
        for (; q < semi; q++)
        {
            int d = *q >= '0' && *q <= '9' ? *q - '0' : base == 16 && *q >= 'a' && *q <= 'f' ? *q - 'a' + 10 :
                    base == 16 && *q >= 'A' && *q <= 'F' ? *q - 'A' + 10 : -1;
            if (d < 0) return 0;
            c = c * (uint32_t)base + (uint32_t)d;
            if (c > 0x10ffff) return 0;
        }
        if (c == 0xa0) c = ' ';
    }
    else
    {
        size_t len = (size_t)(semi - s - 1);
        unsigned i;
        for (i = 0; i < sizeof named / sizeof named[0]; i++)
            if (strlen(named[i].n) == len && !memcmp(named[i].n, s + 1, len)) break;
        if (i == sizeof named / sizeof named[0]) return 0;
        c = named[i].c;
    }
    *used = (size_t)(semi - s + 1);
    if (!c) return 0;
    return ep_utf8(o, c);
}
/* s..end with entities read and white space run together, into out */
static void ep_text(char *out, size_t n, const char *s, const char *end)
{
    size_t o = 0;
    bool sp = false;
    while (s < end && o + 5 < n)
    {
        char c = *s;
        if (c == '<') { while (s < end && *s != '>') s++; s++; continue; }   /* a tag inside a title: its words only */
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = o > 0; s++; continue; }
        if (sp) { out[o++] = ' '; sp = false; }
        size_t used;
        size_t k = c == '&' ? ep_entity(s, end, out + o, &used) : 0;
        if (k) { o += k; s += used; }
        else out[o++] = *s++;
    }
    while (o && out[o - 1] == ' ') o--;
    if (n) out[o] = '\0';
}

/* ---------------------------------------------------------------- XML, roughly */
struct xt { const char *name; int nlen; const char *at; int alen; bool close, empty; };
/* The next tag from *pp: the words before it end at the return; *pp moves past it.
 * Comments, <? ?> and <! > are passed over. NULL at the end. */
static const char *x_next(const char **pp, const char *end, struct xt *t)
{
    const char *s = *pp;
    for (;;)
    {
        const char *lt = memchr(s, '<', (size_t)(end - s));
        if (!lt) { *pp = end; return NULL; }
        if (end - lt >= 4 && !memcmp(lt, "<!--", 4))
        {
            const char *q = lt + 4;
            while (q + 2 < end && memcmp(q, "-->", 3)) q++;
            if (q + 2 >= end) { *pp = end; return NULL; }
            if (lt != s) { /* the words before a comment count */ }
            s = q + 3;
            continue;
        }
        if (end - lt >= 9 && !memcmp(lt, "<![CDATA[", 9))
        {
            const char *q = lt + 9;
            while (q + 2 < end && memcmp(q, "]]>", 3)) q++;
            if (q + 2 >= end) { *pp = end; return NULL; }
            s = q + 3;
            continue;
        }
        const char *gt = lt + 1;
        char quote = 0;
        while (gt < end && (quote || *gt != '>'))
        {
            if (quote) { if (*gt == quote) quote = 0; }
            else if (*gt == '"' || *gt == '\'') quote = *gt;
            gt++;
        }
        if (gt >= end) { *pp = end; return NULL; }   /* cut off inside a tag */
        if (lt[1] == '?' || lt[1] == '!') { s = gt + 1; if (s >= end) { *pp = end; return NULL; } continue; }
        const char *q = lt + 1;
        t->close = *q == '/';
        if (t->close) q++;
        const char *nm = q;
        while (q < gt && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r' && *q != '/') q++;
        const char *colon = memchr(nm, ':', (size_t)(q - nm));
        if (colon) nm = colon + 1;
        t->name = nm;
        t->nlen = (int)(q - nm);
        t->at = q;
        t->alen = (int)(gt - q);
        t->empty = gt[-1] == '/';
        *pp = gt + 1;
        return lt;
    }
}
static bool x_is(const struct xt *t, const char *name)
{
    int n = (int)strlen(name);
    return t->nlen == n && !strncasecmp(t->name, name, (size_t)n);
}
/* an attribute's value (its prefix ignored), entities read */
static bool x_attr(const struct xt *t, const char *name, char *out, size_t n)
{
    const char *s = t->at, *end = t->at + t->alen;
    size_t want = strlen(name);
    while (s < end)
    {
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '/')) s++;
        const char *an = s;
        while (s < end && *s != '=' && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != '/') s++;
        const char *ae = s;
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
        if (s >= end || *s != '=') continue;
        s++;
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
        if (s >= end) break;
        char q = *s;
        const char *v, *ve;
        if (q == '"' || q == '\'') { v = ++s; while (s < end && *s != q) s++; ve = s; if (s < end) s++; }
        else { v = s; while (s < end && *s != ' ' && *s != '\t' && *s != '\n') s++; ve = s; }
        const char *colon = memchr(an, ':', (size_t)(ae - an));
        const char *ln = colon ? colon + 1 : an;
        if ((size_t)(ae - ln) == want && !strncasecmp(ln, name, want))
        {
            ep_text(out, n, v, ve);
            return true;
        }
    }
    if (n) out[0] = '\0';
    return false;
}
/* the words up to </name>, from *pp */
static void x_inner(const char **pp, const char *end, const char *name, char *out, size_t n)
{
    const char *s = *pp, *from = s;
    struct xt t;
    const char *lt;
    int depth = 0;
    while ((lt = x_next(&s, end, &t)))
    {
        if (x_is(&t, name) && !t.empty) { if (!t.close) depth++; else if (!depth--) break; }
    }
    ep_text(out, n, from, lt ? lt : end);
    *pp = s;
}

/* ---------------------------------------------------------------- the zip */
static int ep_find(const struct sl_epub *b, const char *name)
{
    uint32_t h = sl_epub_hash(name);
    for (int i = 0; i < b->nent; i++)
        if (b->ent[i].hash == h && !strcmp(b->names + b->ent[i].name, name)) return i;
    for (int i = 0; i < b->nent; i++)          /* a book that named a file in another case */
        if (!strcasecmp(b->names + b->ent[i].name, name)) return i;
    return -1;
}
static bool ep_add_ent(struct sl_epub *b, size_t *names_cap, const char *name, size_t nlen,
                       uint32_t lho, uint32_t csize, uint32_t usize, uint16_t method)
{
    if (b->nent >= SL_EPUB_ENTRIES_MAX || nlen >= 512) return false;
    if (b->names_n + nlen + 1 > *names_cap)
    {
        size_t cap = *names_cap ? *names_cap * 2 : 8192;
        while (cap < b->names_n + nlen + 1) cap *= 2;
        char *nn = ep_realloc(b->names, cap);
        if (!nn) return false;
        b->names = nn;
        *names_cap = cap;
    }
    if (!(b->nent & 63))
    {
        struct sl_epub_ent *ne = ep_realloc(b->ent, (size_t)(b->nent + 64) * sizeof *ne);
        if (!ne) return false;
        b->ent = ne;
    }
    struct sl_epub_ent *e = &b->ent[b->nent++];
    e->name = (uint32_t)b->names_n;
    memcpy(b->names + b->names_n, name, nlen);
    b->names[b->names_n + nlen] = '\0';
    b->names_n += nlen + 1;
    e->hash = sl_epub_hash(b->names + e->name);
    e->lho = lho; e->csize = csize; e->usize = usize; e->method = method;
    return true;
}
/* the zip's directory; or, when it is missing (a file cut short), its files from their own headers */
static int ep_zip_read(struct sl_epub *b, int fd, off_t size)
{
    size_t names_cap = 0;
    b->names_n = 0;
    unsigned char tail[22 + 1024];
    size_t tn = size < (off_t)sizeof tail ? (size_t)size : sizeof tail;
    if (size >= 22 && ep_pread(fd, tail, tn, size - (off_t)tn))
    {
        for (size_t i = tn - 22 + 1; i-- > 0; )
            if (rd32(tail + i) == 0x06054b50)
            {
                uint32_t cdn = rd16(tail + i + 10), cds = rd32(tail + i + 12), cdo = rd32(tail + i + 16);
                if (cdo == 0xffffffffu || (off_t)cdo + cds > size || cds > 1u << 20) break;   /* 4096 entries fit; more is no book (the R1 has ~4 MB free) */
                unsigned char *cd = ep_alloc(cds ? cds : 1);
                if (!cd) return SL_EPUB_ERR_MEM;
                if (!ep_pread(fd, cd, cds, cdo)) { ep_free(cd); break; }
                size_t k = 0;
                for (uint32_t j = 0; j < cdn && k + 46 <= cds; j++)
                {
                    if (rd32(cd + k) != 0x02014b50) break;
                    uint16_t nl = rd16(cd + k + 28), xl = rd16(cd + k + 30), cl = rd16(cd + k + 32);
                    if (k + 46 + nl > cds) break;
                    if (!ep_add_ent(b, &names_cap, (const char *)cd + k + 46, nl, rd32(cd + k + 42), rd32(cd + k + 20),
                                    rd32(cd + k + 24), rd16(cd + k + 10))) break;
                    k += 46u + nl + xl + cl;
                }
                ep_free(cd);
                return b->nent ? SL_EPUB_OK : SL_EPUB_ERR_ZIP;
            }
    }
    /* no directory: walk the files' own headers from the start */
    off_t at = 0;
    unsigned char h[30];
    char name[512];
    b->cut = true;
    while (at + 30 <= size && ep_pread(fd, h, 30, at) && rd32(h) == 0x04034b50)
    {
        uint16_t flags = rd16(h + 6), method = rd16(h + 8), nl = rd16(h + 26), xl = rd16(h + 28);
        uint32_t cs = rd32(h + 18), us = rd32(h + 22);
        if ((flags & 8) || nl >= sizeof name || !ep_pread(fd, name, nl, at + 30)) break;   /* sizes after the data: no way past it */
        off_t data = at + 30 + nl + xl;
        if (data + (off_t)cs > size) break;   /* this one is cut off: the last there is */
        if (!ep_add_ent(b, &names_cap, name, nl, (uint32_t)at, cs, us, method)) break;
        at = data + (off_t)cs;
    }
    return b->nent ? SL_EPUB_OK : SL_EPUB_ERR_ZIP;
}
/* a file of the book, at most cap bytes of it (*cut when there was more) */
static char *ep_member(const struct sl_epub *b, int fd, int k, size_t cap, size_t *outn, bool *cut)
{
    *outn = 0;
    if (cut) *cut = false;
    if (k < 0 || k >= b->nent || ep_held) return NULL;
    const struct sl_epub_ent *e = &b->ent[k];
    unsigned char h[30];
    if (!ep_pread(fd, h, 30, e->lho) || rd32(h) != 0x04034b50) return NULL;
    off_t data = (off_t)e->lho + 30 + rd16(h + 26) + rd16(h + 28);
    if (e->method == 0)
    {
        size_t n = e->usize < cap ? e->usize : cap;
        char *o = ep_alloc(n + 1);
        if (!o) return NULL;
        if (!ep_pread(fd, o, n, data)) { ep_free(o); return NULL; }
        o[n] = '\0';
        *outn = n;
        if (cut) *cut = e->usize > cap;
        return o;
    }
    if (e->method != 8) return NULL;
    size_t cn = e->csize < cap ? e->csize : cap;
    unsigned char *in = ep_alloc(cn ? cn : 1);
    if (!in) return NULL;
    if (!ep_pread(fd, in, cn, data)) { ep_free(in); return NULL; }
    unsigned char *raw = NULL;
    size_t rn = 0;
    int rc = sl_inflate(in, cn, cap, &raw, &rn);
    /* lodepng grows its buffer by halves: count it at the most it was */
    size_t grown = rn + rn / 2;
    ep_count_foreign(grown, 1);
    ep_free(in);
    ep_count_foreign(grown, -1);
    if (rc < 0 || !raw) { free(raw); return NULL; }
    char *o = ep_alloc(rn + 1);
    if (!o) { free(raw); return NULL; }
    memcpy(o, raw, rn);
    free(raw);
    o[rn] = '\0';
    *outn = rn;
    if (cut) *cut = rc == 1 && (rn >= cap || cn < e->csize);
    return o;
}

/* ---------------------------------------------------------------- paths inside the book */
static void ep_dir_of(char *out, size_t n, const char *path)
{
    const char *sl = strrchr(path, '/');
    size_t k = sl ? (size_t)(sl - path + 1) : 0;
    if (k >= n) k = n - 1;
    memcpy(out, path, k);
    out[k] = '\0';
}
/* href from a file in dir: decoded, its #fragment cut (its hash in *frag), ./ and ../ gone */
static void ep_resolve(char *out, size_t n, const char *dir, const char *href, uint32_t *frag)
{
    char t[600];
    size_t o = 0;
    if (frag) *frag = 0;
    if (href[0] != '/') for (const char *q = dir; *q && o + 1 < sizeof t; q++) t[o++] = *q;
    else href++;
    for (const char *q = href; *q && o + 1 < sizeof t; q++)
    {
        if (*q == '#') { if (frag && q[1]) *frag = sl_epub_hash(q + 1); break; }
        if (*q == '%' && q[1] && q[2])
        {
            char hx[3] = { q[1], q[2], 0 };
            char *e;
            long v = strtol(hx, &e, 16);
            if (!*e) { t[o++] = (char)v; q += 2; continue; }
        }
        t[o++] = *q;
    }
    t[o] = '\0';
    /* fold the parts */
    size_t w = 0;
    char *s = t;
    out[0] = '\0';
    while (*s)
    {
        char *e = strchr(s, '/');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        if (len == 1 && s[0] == '.') {}
        else if (len == 2 && s[0] == '.' && s[1] == '.')
        {
            if (w) { w--; while (w && out[w - 1] != '/') w--; }
        }
        else if (len && w + len + 2 < n)
        {
            memcpy(out + w, s, len);
            w += len;
            if (e) out[w++] = '/';
        }
        out[w] = '\0';
        if (!e) break;
        s = e + 1;
    }
}

/* ---------------------------------------------------------------- the book */
struct ep_item { uint32_t id; int ent; uint8_t flags; };
enum { MI_XHTML = 1, MI_IMAGE = 2, MI_NAV = 4, MI_NCX = 8, MI_COVER = 16 };
static void ep_toc_add(struct sl_epub *b, size_t *cap, const char *title, const char *dir, const char *href, int depth)
{
    if (b->ntoc >= SL_EPUB_TOC_MAX || !title[0]) return;
    char path[512];
    uint32_t frag;
    ep_resolve(path, sizeof path, dir, href, &frag);
    int k = ep_find(b, path), sp = -1;
    for (int i = 0; k >= 0 && i < b->nspine; i++) if (b->spine[i] == k) { sp = i; break; }
    if (sp < 0) return;                       /* a line to nothing we show */
    size_t tl = strlen(title);
    if (tl > 160) tl = 160;
    if (b->pool_n + tl + 1 > *cap)
    {
        size_t nc = *cap ? *cap * 2 : 4096;
        while (nc < b->pool_n + tl + 1) nc *= 2;
        char *np = ep_realloc(b->pool, nc);
        if (!np) return;
        b->pool = np;
        *cap = nc;
    }
    if (!(b->ntoc & 31))
    {
        struct sl_epub_toc *nt = ep_realloc(b->toc, (size_t)(b->ntoc + 32) * sizeof *nt);
        if (!nt) return;
        b->toc = nt;
    }
    struct sl_epub_toc *t = &b->toc[b->ntoc++];
    t->title = (uint32_t)b->pool_n;
    memcpy(b->pool + b->pool_n, title, tl);
    /* never half a character: a cut one goes whole */
    if (tl == 160)
    {
        size_t st = tl;
        while (st && ((unsigned char)b->pool[b->pool_n + st - 1] & 0xc0) == 0x80) st--;   /* back over its continuation bytes */
        if (st && ((unsigned char)b->pool[b->pool_n + st - 1] & 0xc0) == 0xc0)            /* to its lead byte */
        {
            unsigned char lead = (unsigned char)b->pool[b->pool_n + st - 1];
            size_t want = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
            if (tl - (st - 1) < want) tl = st - 1;                                       /* not all there: left out */
        }
    }
    b->pool[b->pool_n + tl] = '\0';
    b->pool_n += tl + 1;
    t->spine = (int16_t)sp;
    t->depth = (uint8_t)(depth < 0 ? 0 : depth > 3 ? 3 : depth);
    t->frag = frag;
}
static void ep_toc_nav(struct sl_epub *b, const char *x, size_t n, const char *dir)
{
    const char *s = x, *end = x + n;
    struct xt t;
    char ty[64], href[512], title[200];
    size_t cap = 0;
    bool in = false;
    int depth = -1, navs = 0;
    while (x_next(&s, end, &t))
    {
        if (x_is(&t, "nav"))
        {
            if (t.close) { if (in) break; continue; }
            navs++;
            x_attr(&t, "type", ty, sizeof ty);
            in = strstr(ty, "toc") != NULL;
            continue;
        }
        if (!in) continue;
        if (x_is(&t, "ol")) { depth += t.close ? -1 : 1; continue; }
        if (x_is(&t, "a") && !t.close && x_attr(&t, "href", href, sizeof href))
        {
            x_inner(&s, end, "a", title, sizeof title);
            ep_toc_add(b, &cap, title, dir, href, depth);
        }
    }
    if (!b->ntoc && navs && !in)
    {
        /* a nav not marked toc: the first one */
        s = x;
        while (x_next(&s, end, &t))
        {
            if (x_is(&t, "nav") && t.close) break;
            if (x_is(&t, "ol")) { depth += t.close ? -1 : 1; continue; }
            if (x_is(&t, "a") && !t.close && x_attr(&t, "href", href, sizeof href))
            {
                x_inner(&s, end, "a", title, sizeof title);
                ep_toc_add(b, &cap, title, dir, href, depth);
            }
        }
    }
}
static void ep_toc_ncx(struct sl_epub *b, const char *x, size_t n, const char *dir)
{
    const char *s = x, *end = x + n;
    struct xt t;
    char href[512], title[200] = "";
    size_t cap = b->pool_n ? b->pool_n : 0;
    int depth = -1;
    bool in_label = false;
    while (x_next(&s, end, &t))
    {
        if (x_is(&t, "navPoint")) { if (!t.empty) depth += t.close ? -1 : 1; if (!t.close) title[0] = '\0'; continue; }
        if (x_is(&t, "navMap") && t.close) break;
        if (x_is(&t, "navLabel")) { in_label = !t.close; continue; }
        if (in_label && x_is(&t, "text") && !t.close && !t.empty) { x_inner(&s, end, "text", title, sizeof title); continue; }
        if (x_is(&t, "content") && !t.close && depth >= 0 && x_attr(&t, "src", href, sizeof href))
            ep_toc_add(b, &cap, title, dir, href, depth);
    }
}
static bool ep_rtl_lang(const char *l)
{
    return !strncasecmp(l, "he", 2) || !strncasecmp(l, "iw", 2) || !strncasecmp(l, "ar", 2) || !strncasecmp(l, "fa", 2) ||
           !strncasecmp(l, "ur", 2) || !strncasecmp(l, "yi", 2);
}
/* chapters locked by DRM (fonts obfuscated are fine: we draw our own) */
static bool ep_drm(const struct sl_epub *b, int fd)
{
    int k = ep_find(b, "META-INF/encryption.xml");
    if (k < 0) return false;
    size_t n;
    char *x = ep_member(b, fd, k, 256 * 1024, &n, NULL);
    if (!x) return false;
    bool locked = false;
    const char *s = x, *end = x + n;
    struct xt t;
    char uri[512];
    while (!locked && x_next(&s, end, &t))
        if (x_is(&t, "CipherReference") && x_attr(&t, "URI", uri, sizeof uri))
        {
            const char *dot = strrchr(uri, '.');
            locked = dot && (!strcasecmp(dot, ".xhtml") || !strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm"));
        }
    ep_free(x);
    return locked;
}
static bool ep_media(const char *mt, const char *href, int kind)
{
    const char *dot = strrchr(href, '.');
    if (kind == MI_XHTML) return strstr(mt, "html") || (dot && (!strcasecmp(dot, ".xhtml") || !strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")));
    if (kind == MI_IMAGE) return !strncmp(mt, "image/", 6) || (dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png")));
    return strstr(mt, "dtbncx") || (dot && !strcasecmp(dot, ".ncx"));
}
void sl_epub_free(struct sl_epub *b)
{
    if (!b) return;
    ep_free(b->spine); ep_free(b->spine_at); ep_free(b->toc); ep_free(b->pool); ep_free(b->ent); ep_free(b->names);
    ep_free(b);
}
struct sl_epub *sl_epub_open(const char *path, int *err)
{
    int e = SL_EPUB_OK;
    char real[600];
    struct sl_epub *b = NULL;
    char *opf = NULL;
    struct ep_item *items = NULL;
    int fd = -1;
    if (ep_held) { e = SL_EPUB_ERR_HELD; goto out; }
    if (!ep_real(real, sizeof real, path) || (fd = open(real, O_RDONLY)) < 0) { e = SL_EPUB_ERR_OPEN; goto out; }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 22) { e = SL_EPUB_ERR_ZIP; goto out; }
    b = ep_zalloc(sizeof *b);
    if (!b) { e = SL_EPUB_ERR_MEM; goto out; }
    snprintf(b->path, sizeof b->path, "%s", path);
    b->cover = -1;
    if ((e = ep_zip_read(b, fd, size)) != SL_EPUB_OK) goto out;
    size_t n;
    /* container.xml names the OPF */
    char opf_path[512] = "", dir[512];
    int k = ep_find(b, "META-INF/container.xml");
    char *c = k >= 0 ? ep_member(b, fd, k, 64 * 1024, &n, NULL) : NULL;
    if (c)
    {
        const char *s = c;
        struct xt t;
        while (x_next(&s, c + n, &t))
            if (x_is(&t, "rootfile") && x_attr(&t, "full-path", opf_path, sizeof opf_path)) break;
        ep_free(c);
    }
    if (!opf_path[0])
        for (int i = 0; i < b->nent; i++)   /* no container: any .opf */
        {
            const char *nm = b->names + b->ent[i].name, *dot = strrchr(nm, '.');
            if (dot && !strcasecmp(dot, ".opf")) { snprintf(opf_path, sizeof opf_path, "%s", nm); break; }
        }
    k = opf_path[0] ? ep_find(b, opf_path) : -1;
    opf = k >= 0 ? ep_member(b, fd, k, 512 * 1024, &n, NULL) : NULL;   /* read at ~2.5x its size: kept small */
    if (!opf) { e = SL_EPUB_ERR_OPF; goto out; }
    if (ep_drm(b, fd)) { e = SL_EPUB_ERR_DRM; goto out; }
    ep_dir_of(dir, sizeof dir, opf_path);
    /* metadata, the manifest and the spine */
    const char *s = opf, *end = opf + n;
    struct xt t;
    int nitems = 0, cap_items = 0;
    uint32_t cover_id = 0, ncx_id = 0;
    char v[512], href[512], mt[96], props[128], path2[512];
    int nspine_cap = 0;
    while (x_next(&s, end, &t))
    {
        if (t.close) continue;
        if (x_is(&t, "title") && !b->title[0] && !t.empty) x_inner(&s, end, "title", b->title, sizeof b->title);
        else if (x_is(&t, "creator") && !b->author[0] && !t.empty) x_inner(&s, end, "creator", b->author, sizeof b->author);
        else if (x_is(&t, "language") && !b->lang[0] && !t.empty) x_inner(&s, end, "language", b->lang, sizeof b->lang);
        else if (x_is(&t, "meta") && x_attr(&t, "name", v, sizeof v) && !strcmp(v, "cover") && x_attr(&t, "content", v, sizeof v))
            cover_id = sl_epub_hash(v);
        else if (x_is(&t, "item") && x_attr(&t, "href", href, sizeof href))
        {
            if (nitems >= SL_EPUB_ENTRIES_MAX) continue;
            if (nitems >= cap_items)
            {
                cap_items = cap_items ? cap_items * 2 : 64;
                struct ep_item *ni = ep_realloc(items, (size_t)cap_items * sizeof *ni);
                if (!ni) { e = SL_EPUB_ERR_MEM; goto out; }
                items = ni;
            }
            struct ep_item *it = &items[nitems++];
            x_attr(&t, "id", v, sizeof v);
            it->id = sl_epub_hash(v);
            x_attr(&t, "media-type", mt, sizeof mt);
            x_attr(&t, "properties", props, sizeof props);
            ep_resolve(path2, sizeof path2, dir, href, NULL);
            it->ent = ep_find(b, path2);
            it->flags = (ep_media(mt, href, MI_XHTML) ? MI_XHTML : 0) | (ep_media(mt, href, MI_IMAGE) ? MI_IMAGE : 0) |
                        (strstr(props, "nav") ? MI_NAV : 0) | (ep_media(mt, href, MI_NCX) ? MI_NCX : 0) |
                        (strstr(props, "cover-image") ? MI_COVER : 0);
            if ((it->flags & MI_IMAGE) && !(it->flags & MI_COVER) && b->cover < 0 && it->ent >= 0)
            {
                /* a picture named for the cover, when the book points at none */
                char low[512];
                size_t i;
                for (i = 0; href[i] && i + 1 < sizeof low; i++) low[i] = (char)(href[i] >= 'A' && href[i] <= 'Z' ? href[i] + 32 : href[i]);
                low[i] = '\0';
                if (strstr(low, "cover")) it->flags |= 0x80;
            }
        }
        else if (x_is(&t, "spine"))
        {
            if (x_attr(&t, "toc", v, sizeof v)) ncx_id = sl_epub_hash(v);
            if (x_attr(&t, "page-progression-direction", v, sizeof v) && !strcmp(v, "rtl")) b->rtl = true;
        }
        else if (x_is(&t, "itemref") && x_attr(&t, "idref", v, sizeof v))
        {
            uint32_t id = sl_epub_hash(v);
            if (x_attr(&t, "linear", v, sizeof v) && !strcmp(v, "no") && b->nspine) continue;   /* asides, beyond the first */
            for (int i = 0; i < nitems; i++)
                if (items[i].id == id && items[i].ent >= 0 && (items[i].flags & MI_XHTML) && b->nspine < SL_EPUB_SPINE_MAX)
                {
                    if (b->nspine >= nspine_cap)
                    {
                        nspine_cap = nspine_cap ? nspine_cap * 2 : 32;
                        int *ns = ep_realloc(b->spine, (size_t)nspine_cap * sizeof *ns);
                        if (!ns) { e = SL_EPUB_ERR_MEM; goto out; }
                        b->spine = ns;
                    }
                    b->spine[b->nspine++] = items[i].ent;
                    break;
                }
        }
    }
    if (!b->nspine) { e = SL_EPUB_ERR_EMPTY; goto out; }
    if (ep_rtl_lang(b->lang)) b->rtl = true;
    b->spine_at = ep_alloc((size_t)b->nspine * sizeof *b->spine_at);
    if (!b->spine_at) { e = SL_EPUB_ERR_MEM; goto out; }
    for (int i = 0; i < b->nspine; i++)
    {
        b->spine_at[i] = (uint32_t)b->total;
        uint32_t u = b->ent[b->spine[i]].usize;
        b->total += u > SL_EPUB_XHTML_CAP ? SL_EPUB_XHTML_CAP : u ? u : 1;
    }
    /* the cover: EPUB 3's, EPUB 2's, or a picture named so */
    for (int i = 0; i < nitems && b->cover < 0; i++) if ((items[i].flags & MI_COVER) && items[i].ent >= 0) b->cover = items[i].ent;
    for (int i = 0; i < nitems && b->cover < 0; i++) if (cover_id && items[i].id == cover_id && (items[i].flags & MI_IMAGE)) b->cover = items[i].ent;
    for (int i = 0; i < nitems && b->cover < 0; i++) if (items[i].flags & 0x80) b->cover = items[i].ent;
    /* the contents: EPUB 3's nav, else the NCX */
    for (int pass = 0; pass < 2 && !b->ntoc; pass++)
        for (int i = 0; i < nitems; i++)
        {
            bool want = pass == 0 ? (items[i].flags & MI_NAV) != 0 : ((items[i].flags & MI_NCX) || (ncx_id && items[i].id == ncx_id));
            if (!want || items[i].ent < 0) continue;
            size_t xn;
            char *x = ep_member(b, fd, items[i].ent, 512 * 1024, &xn, NULL);
            if (!x) continue;
            char tdir[512];
            ep_dir_of(tdir, sizeof tdir, b->names + b->ent[items[i].ent].name);
            if (pass == 0) ep_toc_nav(b, x, xn, tdir); else ep_toc_ncx(b, x, xn, tdir);
            ep_free(x);
            break;
        }
    if (!b->title[0])
    {
        /* the file's name, less .epub */
        const char *base = strrchr(path, '/');
        snprintf(b->title, sizeof b->title, "%s", base ? base + 1 : path);
        char *dot = strrchr(b->title, '.');
        if (dot && dot != b->title) *dot = '\0';
    }
out:
    if (fd >= 0) close(fd);
    ep_free(opf);
    ep_free(items);
    if (e != SL_EPUB_OK) { sl_epub_free(b); b = NULL; }
    if (err) *err = e;
    return b;
}
const char *sl_epub_toc_title(const struct sl_epub *b, int k) { return b && k >= 0 && k < b->ntoc ? b->pool + b->toc[k].title : ""; }
int sl_epub_toc_at(const struct sl_epub *b, int spine)
{
    int best = -1;
    for (int i = 0; b && i < b->ntoc; i++)
        if (b->toc[i].spine <= spine && (best < 0 || b->toc[i].spine >= b->toc[best].spine)) best = i;
    return best;
}

/* ---------------------------------------------------------------- a chapter, as paragraphs */
struct cp {
    struct sl_epub_chap *c;
    size_t cap;
    int pcap, acap;
    size_t start;
    int kind, flags;
    int em, bold, skip, pre, quote, li_n[4], lists;
    size_t em_n, bold_n, all_n;
    bool space, full;
};
static bool cp_room(struct cp *p, size_t more)
{
    if (p->c->n + more + 1 <= p->cap) return true;
    p->full = true;
    return false;
}
static void cp_byte(struct cp *p, char ch)
{
    if (cp_room(p, 1)) p->c->text[p->c->n++] = ch;
}
static void cp_push(struct cp *p, int kind, int flags)
{
    struct sl_epub_chap *c = p->c;
    if (c->np >= SL_EPUB_PARAS_MAX) { p->full = true; return; }
    if (c->np >= p->pcap)
    {
        int nc = p->pcap ? p->pcap * 2 : 256;
        if (nc > SL_EPUB_PARAS_MAX) nc = SL_EPUB_PARAS_MAX;
        struct sl_epub_para *np = ep_realloc(c->p, (size_t)nc * sizeof *np);
        if (!np) { p->full = true; return; }
        c->p = np;
        p->pcap = nc;
    }
    c->text[c->n++] = '\0';
    struct sl_epub_para *q = &c->p[c->np++];
    q->off = (uint32_t)p->start;
    q->len = (uint32_t)(c->n - 1 - p->start);
    q->kind = (uint8_t)kind;
    q->flags = (uint8_t)flags;
    p->start = c->n;
}
static void cp_flush(struct cp *p)
{
    struct sl_epub_chap *c = p->c;
    while (c->n > p->start && (c->text[c->n - 1] == ' ' || c->text[c->n - 1] == '\n')) c->n--;
    p->space = false;
    if (c->n > p->start)
    {
        int kind = p->kind, flags = p->flags | (p->quote ? SL_EPF_QUOTE : 0) | (p->pre ? SL_EPF_PRE : 0);
        if (kind == SL_EP_P && p->all_n && p->em_n == p->all_n) flags |= SL_EPF_EM;
        /* a short line all in bold (older books' chapter titles): a small heading */
        if (kind == SL_EP_P && p->all_n && p->bold_n == p->all_n && c->n - p->start < 120 && !(flags & SL_EPF_LI)) kind = SL_EP_H3;
        if (cp_room(p, 1)) cp_push(p, kind, flags);
    }
    else c->n = p->start;
    p->em_n = p->bold_n = p->all_n = 0;
    p->flags &= ~SL_EPF_LI;
}
static void cp_anchor(struct cp *p, const struct xt *t)
{
    char id[160];
    if (!memmem(t->at, (size_t)t->alen, "id=", 3) || !x_attr(t, "id", id, sizeof id) || !id[0]) return;
    struct sl_epub_chap *c = p->c;
    if (c->nanchor >= 1024) return;
    if (c->nanchor >= p->acap)
    {
        int nc = p->acap ? p->acap * 2 : 32;
        uint32_t *na = ep_realloc(c->anchor, (size_t)nc * sizeof *na);
        if (!na) return;
        c->anchor = na;
        int *np = ep_realloc(c->anchor_para, (size_t)nc * sizeof *np);
        if (!np) return;
        c->anchor_para = np;
        p->acap = nc;
    }
    c->anchor[c->nanchor] = sl_epub_hash(id);
    c->anchor_para[c->nanchor++] = c->np + (c->n > p->start ? 1 : 0);   /* the paragraph after the one open */
}
static void cp_words(struct cp *p, const char *s, const char *end)
{
    struct sl_epub_chap *c = p->c;
    while (s < end && !p->full)
    {
        unsigned char ch = (unsigned char)*s;
        if (!p->pre && (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')) { p->space = c->n > p->start; s++; continue; }
        if (ch == '\r') { s++; continue; }
        if (p->space) { cp_byte(p, ' '); p->space = false; }
        if (ch == '&')
        {
            char o[4];
            size_t used, k = ep_entity(s, end, o, &used);
            if (k)
            {
                s += used;
                if (k == 2 && (unsigned char)o[0] == 0xc2 && (unsigned char)o[1] == 0xad) continue;   /* a soft hyphen: not drawn */
                if (cp_room(p, k)) { memcpy(c->text + c->n, o, k); c->n += k; }
                p->all_n++; if (p->em) p->em_n++; if (p->bold) p->bold_n++;
                continue;
            }
        }
        /* soft hyphens, zero-width spaces and byte-order marks: not drawn */
        if (ch == 0xc2 && s + 1 < end && (unsigned char)s[1] == 0xad) { s += 2; continue; }
        if (ch == 0xe2 && s + 2 < end && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0x8b) { s += 3; continue; }
        if (ch == 0xef && s + 2 < end && (unsigned char)s[1] == 0xbb && (unsigned char)s[2] == 0xbf) { s += 3; continue; }
        if (ch == '\t') ch = ' ';
        if (ch < 0x20 && ch != '\n') { s++; continue; }
        cp_byte(p, (char)ch);
        if ((ch & 0xc0) != 0x80 && ch != ' ' && ch != '\n') { p->all_n++; if (p->em) p->em_n++; if (p->bold) p->bold_n++; }
        s++;
    }
}
static void cp_tag(struct cp *p, const struct xt *t)
{
    static const char *const skips[] = { "head", "script", "style", "svg", "math", "noscript", "template", "rt", "rp", "title" };
    static const char *const blocks[] = { "p", "div", "li", "blockquote", "pre", "ul", "ol", "dl", "dt", "dd", "table", "tr",
                                          "section", "article", "aside", "header", "footer", "figure", "figcaption", "nav",
                                          "main", "address", "center", "hgroup", "body", "caption", "h1", "h2", "h3", "h4", "h5", "h6" };
    if (!t->close) cp_anchor(p, t);
    for (unsigned i = 0; i < sizeof skips / sizeof skips[0]; i++)
        if (x_is(t, skips[i]))
        {
            if (t->empty) return;
            if (!t->close)
            {
                if (!p->skip && (i == 3 || i == 4))
                {
                    /* a drawing or a formula: a quiet line where it was */
                    cp_flush(p);
                    if (cp_room(p, 1)) cp_push(p, SL_EP_PIC, 0);
                }
                p->skip++;
            }
            else if (p->skip) p->skip--;
            return;
        }
    if (p->skip) return;
    if (x_is(t, "br")) { if (p->c->n > p->start) { cp_byte(p, '\n'); p->space = false; } return; }
    if (x_is(t, "hr")) { cp_flush(p); if (cp_room(p, 1)) cp_push(p, SL_EP_RULE, 0); return; }
    if (x_is(t, "img") || x_is(t, "image"))
    {
        char alt[200];
        cp_flush(p);
        x_attr(t, "alt", alt, sizeof alt);
        size_t k = strlen(alt);
        if (cp_room(p, k + 1)) { memcpy(p->c->text + p->c->n, alt, k); p->c->n += k; cp_push(p, SL_EP_PIC, 0); }
        return;
    }
    if (x_is(t, "em") || x_is(t, "i") || x_is(t, "cite") || x_is(t, "dfn") || x_is(t, "var"))
    {
        if (!t->empty) { if (!t->close) p->em++; else if (p->em) p->em--; }
        return;
    }
    if (x_is(t, "strong") || x_is(t, "b"))
    {
        if (!t->empty) { if (!t->close) p->bold++; else if (p->bold) p->bold--; }
        return;
    }
    if (x_is(t, "td") || x_is(t, "th")) { if (!t->close) p->space = p->c->n > p->start; return; }
    for (unsigned i = 0; i < sizeof blocks / sizeof blocks[0]; i++)
        if (x_is(t, blocks[i]))
        {
            cp_flush(p);
            if (t->empty) return;
            const char *nm = blocks[i];
            if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6')
                p->kind = t->close ? SL_EP_P : nm[1] == '1' ? SL_EP_H1 : nm[1] == '2' ? SL_EP_H2 : SL_EP_H3;
            else if (!strcmp(nm, "blockquote")) { if (!t->close) p->quote++; else if (p->quote) p->quote--; }
            else if (!strcmp(nm, "pre")) { if (!t->close) p->pre++; else if (p->pre) p->pre--; }
            else if (!strcmp(nm, "ol") || !strcmp(nm, "ul"))
            {
                if (!t->close) { if (p->lists < 4) p->li_n[p->lists] = nm[0] == 'o' ? 1 : 0; p->lists++; }
                else if (p->lists) p->lists--;
            }
            else if (!strcmp(nm, "li") && !t->close)
            {
                /* a list's mark: a number in an ordered list, else a bullet */
                char mark[16];
                int d = p->lists ? (p->lists > 4 ? 3 : p->lists - 1) : 0;
                if (p->lists && p->li_n[d]) snprintf(mark, sizeof mark, "%d. ", p->li_n[d]++);
                else strcpy(mark, "\xe2\x80\xa2 ");
                size_t k = strlen(mark);
                if (cp_room(p, k)) { memcpy(p->c->text + p->c->n, mark, k); p->c->n += k; }
                p->flags |= SL_EPF_LI;
            }
            return;
        }
}
static bool ep_has_arabic(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] >= 0xd8 && (unsigned char)s[i] <= 0xdb) return true;
    return false;
}
void sl_epub_chap_free(struct sl_epub_chap *c)
{
    if (!c) return;
    ep_free(c->text); ep_free(c->p); ep_free(c->anchor); ep_free(c->anchor_para);
    ep_free(c);
}
static void ep_parse(struct sl_epub_chap *c, const char *x, size_t n)
{
    struct cp p;
    memset(&p, 0, sizeof p);
    p.c = c;
    p.cap = n + 64;
    c->text = ep_alloc(p.cap);
    if (!c->text) { c->missing = true; return; }
    const char *s = x, *end = x + n;
    /* the words start at <body> (a file with none: everything but its head) */
    const char *body = NULL;
    {
        const char *q = s;
        struct xt t;
        while (x_next(&q, end, &t)) if (x_is(&t, "body") && !t.close) { body = q; break; }
    }
    if (body) s = body;
    struct xt t;
    for (;;)
    {
        const char *from = s;
        const char *lt = x_next(&s, end, &t);
        if (!p.skip) cp_words(&p, from, lt ? lt : end);
        if (!lt || p.full) break;
        cp_tag(&p, &t);
        if (p.full) break;
    }
    if (p.full) c->cut = true;
    cp_flush(&p);
    /* Arabic letters joined as they are drawn (LVGL's own forms; a label
     * given them again leaves them), so the lines are measured as drawn */
    bool ar = false;
    for (int i = 0; i < c->np && !ar; i++) ar = ep_has_arabic(c->text + c->p[i].off, c->p[i].len);
    if (ar)
    {
        size_t need = 0;
        for (int i = 0; i < c->np; i++)
            need += ep_has_arabic(c->text + c->p[i].off, c->p[i].len) ? lv_text_ap_calc_bytes_count(c->text + c->p[i].off) : c->p[i].len + 1;
        char *nt = ep_alloc(need + 1);
        if (nt)
        {
            size_t o = 0;
            for (int i = 0; i < c->np; i++)
            {
                const char *src = c->text + c->p[i].off;
                if (ep_has_arabic(src, c->p[i].len)) lv_text_ap_proc(src, nt + o);
                else memcpy(nt + o, src, c->p[i].len + 1);
                c->p[i].off = (uint32_t)o;
                c->p[i].len = (uint32_t)strlen(nt + o);
                o += c->p[i].len + 1;
            }
            ep_free(c->text);
            c->text = nt;
            c->n = o;
            p.cap = need + 1;
        }
    }
    else if (c->n + 1 < p.cap)
    {
        char *nt = ep_realloc(c->text, c->n + 1);   /* only what it holds */
        if (nt) { c->text = nt; p.cap = c->n + 1; }
    }
    if (c->np < p.pcap && c->np)
    {
        struct sl_epub_para *np = ep_realloc(c->p, (size_t)c->np * sizeof *np);
        if (np) c->p = np;
    }
    c->bytes = p.cap + (size_t)c->np * sizeof *c->p + (size_t)c->nanchor * 8;
}
struct sl_epub_chap *sl_epub_chapter(const struct sl_epub *b, int spine)
{
    struct sl_epub_chap *c = ep_zalloc(sizeof *c);
    if (!c) return NULL;
    c->spine = spine;
    char real[600];
    int fd = b && spine >= 0 && spine < b->nspine && !ep_held && ep_real(real, sizeof real, b->path) ? open(real, O_RDONLY) : -1;
    size_t n = 0;
    bool cut = false;
    char *x = fd >= 0 ? ep_member(b, fd, b->spine[spine], SL_EPUB_XHTML_CAP, &n, &cut) : NULL;
    if (fd >= 0) close(fd);
    if (!x) { c->missing = true; return c; }
    if (n >= 2 && (((unsigned char)x[0] == 0xff && (unsigned char)x[1] == 0xfe) || ((unsigned char)x[0] == 0xfe && (unsigned char)x[1] == 0xff)))
    {
        /* UTF-16: as UTF-8 */
        bool le = (unsigned char)x[0] == 0xff;
        char *u = ep_alloc(n / 2 * 3 + 4);
        size_t o = 0;
        if (u)
        {
            for (size_t i = 2; i + 1 < n; i += 2)
            {
                uint32_t ch = le ? (unsigned char)x[i] | (unsigned char)x[i + 1] << 8 : (unsigned char)x[i] << 8 | (unsigned char)x[i + 1];
                if (ch >= 0xd800 && ch < 0xdc00 && i + 3 < n)   /* a pair: one letter past U+FFFF (an emoji, CJK Extension B) */
                {
                    uint32_t lo = le ? (unsigned char)x[i + 2] | (unsigned char)x[i + 3] << 8 : (unsigned char)x[i + 2] << 8 | (unsigned char)x[i + 3];
                    if (lo < 0xdc00 || lo >= 0xe000) continue;
                    ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00);
                    i += 2;
                }
                else if (ch >= 0xd800 && ch < 0xe000) continue;   /* half a pair alone */
                o += ep_utf8(u + o, ch);
            }
            u[o] = '\0';
        }
        ep_free(x);
        x = u;
        n = o;
        if (!x) { c->missing = true; return c; }
    }
    ep_parse(c, x, n);
    ep_free(x);
    if (cut) c->cut = true;
    return c;
}
int sl_epub_anchor(const struct sl_epub_chap *c, uint32_t frag)
{
    for (int i = 0; c && frag && i < c->nanchor; i++)
        if (c->anchor[i] == frag) return c->anchor_para[i] < c->np ? c->anchor_para[i] : c->np ? c->np - 1 : 0;
    return 0;
}

/* ---------------------------------------------------------------- files we keep */
static bool ep_write(const char *rb, const void *d, size_t n)
{
    char real[600], tmp[620];
    if (ep_held || !ep_real(real, sizeof real, rb)) return false;
    snprintf(tmp, sizeof tmp, "%s.tmp", real);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    bool ok = write(fd, d, n) == (ssize_t)n;
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) < 0) ok = false;
    if (!ok || rename(tmp, real) < 0) { unlink(tmp); return false; }
    return true;
}
static void ep_mkdirs(void)
{
    char real[600];
    if (ep_held) return;   /* USB mode: the card is the computer's */
    if (ep_real(real, sizeof real, "/.rockbox/sloppy")) mkdir(real, 0777);
    if (ep_real(real, sizeof real, SL_EPUB_STATE)) mkdir(real, 0777);
}
static char *ep_slurp(const char *rb, size_t cap, size_t *outn)
{
    char real[600];
    *outn = 0;
    if (ep_held || !ep_real(real, sizeof real, rb)) return NULL;
    int fd = open(real, O_RDONLY);
    if (fd < 0) return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || (size_t)sz > cap) { close(fd); return NULL; }
    char *b = ep_alloc((size_t)sz + 1);
    if (b && !ep_pread(fd, b, (size_t)sz, 0)) { ep_free(b); b = NULL; }
    close(fd);
    if (b) { b[sz] = '\0'; *outn = (size_t)sz; }
    return b;
}

/* ---------------------------------------------------------------- where each book was left */
#define POS_FILE SL_EPUB_STATE "/reading.txt"
#define POS_MAX 256
struct ep_posrec { uint32_t hash; struct sl_epub_pos p; char path[256]; };
static pthread_mutex_t pos_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ep_posrec *pos;
static int npos, pos_theme = -1, pos_size = -1;
static bool pos_read, pos_dirty;
static void pos_load(void)
{
    if (pos_read) return;
    pos_read = true;
    size_t n;
    char *b = ep_slurp(POS_FILE, 256 * 1024, &n);
    if (!b) return;
    char *line = b, *nl;
    if (!strncmp(line, "SLR1 ", 5)) sscanf(line + 5, "%d %d", &pos_theme, &pos_size);
    else { ep_free(b); return; }
    if (pos_theme < -1 || pos_theme > 2) pos_theme = -1;
    if (pos_size < -1 || pos_size > 3) pos_size = -1;
    for (line = strchr(line, '\n'); line && (nl = strchr(++line, '\n')); line = nl)
    {
        *nl = '\0';
        struct ep_posrec r;
        unsigned h, permille, when, off;
        int at = 0;
        memset(&r, 0, sizeof r);
        if (sscanf(line, "%x %d %d %u %u %u %n", &h, &r.p.spine, &r.p.para, &off, &permille, &when, &at) < 6 || !at) continue;
        if (npos >= POS_MAX) break;
        if (!(npos & 15))
        {
            struct ep_posrec *np = ep_realloc(pos, (size_t)(npos + 16) * sizeof *np);
            if (!np) break;
            pos = np;
        }
        r.hash = h; r.p.off = off; r.p.permille = (uint16_t)(permille > 1000 ? 1000 : permille); r.p.when = when;
        snprintf(r.path, sizeof r.path, "%s", line + at);
        pos[npos++] = r;
        *nl = '\n';
    }
    ep_free(b);
}
/* the file's words made under the lock, written after it: the interface
 * reading a place never waits for the card */
/* one writer at a time (the worker's save and the UI's at USB mode): each
 * takes its copy and writes it whole before the next, so the newest is last */
static pthread_mutex_t pos_write_lock = PTHREAD_MUTEX_INITIALIZER;
static void pos_write(void)
{
    pthread_mutex_lock(&pos_write_lock);
    pthread_mutex_lock(&pos_lock);
    if (!pos_dirty) { pthread_mutex_unlock(&pos_lock); pthread_mutex_unlock(&pos_write_lock); return; }
    size_t cap = 64 + (size_t)npos * 320;
    char *b = ep_alloc(cap);
    if (!b) { pthread_mutex_unlock(&pos_lock); pthread_mutex_unlock(&pos_write_lock); return; }
    size_t o = (size_t)snprintf(b, cap, "SLR1 %d %d\n", pos_theme, pos_size);
    for (int i = 0; i < npos && o + 320 < cap; i++)
        o += (size_t)snprintf(b + o, cap - o, "%08x %d %d %u %u %u %s\n", (unsigned)pos[i].hash, pos[i].p.spine, pos[i].p.para,
                              (unsigned)pos[i].p.off, (unsigned)pos[i].p.permille, (unsigned)pos[i].p.when, pos[i].path);
    pos_dirty = false;
    pthread_mutex_unlock(&pos_lock);
    ep_mkdirs();
    bool ok = ep_write(POS_FILE, b, o);
    ep_free(b);
    if (!ok) { pthread_mutex_lock(&pos_lock); pos_dirty = true; pthread_mutex_unlock(&pos_lock); }
    pthread_mutex_unlock(&pos_write_lock);
}
bool sl_epub_pos_get(const char *path, struct sl_epub_pos *p)
{
    pthread_mutex_lock(&pos_lock);
    pos_load();
    uint32_t h = sl_epub_hash(path);
    bool ok = false;
    for (int i = 0; i < npos && !ok; i++) if (pos[i].hash == h) { *p = pos[i].p; ok = true; }
    pthread_mutex_unlock(&pos_lock);
    return ok;
}
void sl_epub_pos_set(const char *path, const struct sl_epub_pos *p)
{
    pthread_mutex_lock(&pos_lock);
    pos_load();
    uint32_t h = sl_epub_hash(path);
    int i = 0;
    while (i < npos && pos[i].hash != h) i++;
    if (i == npos)
    {
        if (npos >= POS_MAX)
        {
            /* the one read longest ago makes room */
            int old = 0;
            for (int k = 1; k < npos; k++) if (pos[k].p.when < pos[old].p.when) old = k;
            i = old;
        }
        else
        {
            if (!(npos & 15))
            {
                struct ep_posrec *np = ep_realloc(pos, (size_t)(npos + 16) * sizeof *np);
                if (!np) { pthread_mutex_unlock(&pos_lock); return; }
                pos = np;
            }
            npos++;
        }
    }
    pos[i].hash = h;
    pos[i].p = *p;
    snprintf(pos[i].path, sizeof pos[i].path, "%s", path);
    pos_dirty = true;
    pthread_mutex_unlock(&pos_lock);
}
bool sl_epub_pos_last(char *path, size_t n)
{
    pthread_mutex_lock(&pos_lock);
    pos_load();
    int best = -1;
    for (int i = 0; i < npos; i++) if (best < 0 || pos[i].p.when >= pos[best].p.when) best = i;
    if (best >= 0) snprintf(path, n, "%s", pos[best].path);
    pthread_mutex_unlock(&pos_lock);
    return best >= 0;
}
void sl_epub_prefs(int *theme, int *size)
{
    pthread_mutex_lock(&pos_lock);
    pos_load();
    *theme = pos_theme;
    *size = pos_size;
    pthread_mutex_unlock(&pos_lock);
}
void sl_epub_prefs_set(int theme, int size)
{
    pthread_mutex_lock(&pos_lock);
    pos_load();
    if (pos_theme != theme || pos_size != size) pos_dirty = true;
    pos_theme = theme;
    pos_size = size;
    pthread_mutex_unlock(&pos_lock);
}

/* ---------------------------------------------------------------- the shelf */
#define SHELF_FILE SL_EPUB_STATE "/shelf.txt"
static pthread_mutex_t shelf_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sl_epub_book *shelf;
static int nshelf;
static bool shelf_read;
static unsigned shelf_seq_n;
static void shelf_load(void)
{
    /* under shelf_lock */
    if (shelf_read) return;
    shelf_read = true;
    size_t n;
    char *b = ep_slurp(SHELF_FILE, 512 * 1024, &n);
    if (!b || strncmp(b, "SLS1\n", 5)) { ep_free(b); return; }
    for (char *line = b + 5, *nl; *line && (nl = strchr(line, '\n')); line = nl + 1)
    {
        *nl = '\0';
        struct sl_epub_book k;
        memset(&k, 0, sizeof k);
        char *f[6];
        int nf = 0;
        for (char *q = line; nf < 6; )
        {
            f[nf++] = q;
            char *tab = strchr(q, '\t');
            if (!tab) break;
            *tab = '\0';
            q = tab + 1;
        }
        if (nf < 6 || nshelf >= SL_EPUB_SHELF_MAX) continue;
        unsigned fl = 0;
        k.size = (uint32_t)strtoul(f[0], NULL, 10);
        k.mtime = (uint32_t)strtoul(f[1], NULL, 10);
        fl = (unsigned)strtoul(f[2], NULL, 10);
        k.cover = fl & 1; k.bad = (fl >> 1) & 1; k.rtl = (fl >> 2) & 1;
        snprintf(k.path, sizeof k.path, "%s", f[3]);
        snprintf(k.title, sizeof k.title, "%s", f[4]);
        snprintf(k.author, sizeof k.author, "%s", f[5]);
        k.hash = sl_epub_hash(k.path);
        if (!(nshelf & 15))
        {
            struct sl_epub_book *ns = ep_realloc(shelf, (size_t)(nshelf + 16) * sizeof *ns);
            if (!ns) break;
            shelf = ns;
        }
        shelf[nshelf++] = k;
    }
    ep_free(b);
}
static void ep_clean(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }
static void shelf_write(const struct sl_epub_book *bk, int n)
{
    size_t cap = 16 + (size_t)n * 512;
    char *b = ep_alloc(cap);
    if (!b) return;
    size_t o = (size_t)snprintf(b, cap, "SLS1\n");
    for (int i = 0; i < n && o + 512 < cap; i++)
        o += (size_t)snprintf(b + o, cap - o, "%u\t%u\t%u\t%s\t%s\t%s\n", (unsigned)bk[i].size, (unsigned)bk[i].mtime,
                              (unsigned)(bk[i].cover | bk[i].bad << 1 | bk[i].rtl << 2), bk[i].path, bk[i].title, bk[i].author);
    ep_mkdirs();
    ep_write(SHELF_FILE, b, o);
    ep_free(b);
}
void sl_epub_cover_name(char *out, size_t n, uint32_t hash) { snprintf(out, n, SL_EPUB_STATE "/c%08x.bin", (unsigned)hash); }
/* the cover, fitted to the shelf's 2:3 and made an LVGL RGB565 picture */
static bool ep_cover_write(uint32_t hash, const unsigned char *rgb, int w, int h)
{
    int ow = SL_EPUB_COVER_W, oh = SL_EPUB_COVER_H;
    /* the middle 2:3 of it */
    int sw = w, sh = h;
    if ((long)w * oh > (long)h * ow) sw = (int)((long)h * ow / oh); else sh = (int)((long)w * oh / ow);
    int ox = (w - sw) / 2, oy = (h - sh) / 2;
    if (sw <= 0 || sh <= 0) return false;
    size_t bytes = (size_t)ow * oh * 2 + 12;
    uint8_t *dst = ep_alloc(bytes);
    if (!dst) return false;
    uint16_t *px = (uint16_t *)(dst + 12);
    for (int y = 0; y < oh; y++)
    {
        int y0 = oy + y * sh / oh, y1 = oy + (y + 1) * sh / oh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < ow; x++)
        {
            int x0 = ox + x * sw / ow, x1 = ox + (x + 1) * sw / ow;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, bl = 0, k = 0;
            for (int yy = y0; yy < y1; yy++)
            {
                const unsigned char *q = rgb + ((size_t)yy * w + x0) * 3;
                for (int xx = x0; xx < x1; xx++, q += 3) { r += q[0]; g += q[1]; bl += q[2]; k++; }
            }
            r /= k; g /= k; bl /= k;
            px[y * ow + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (bl >> 3));
        }
    }
    memset(dst, 0, 12);
    dst[0] = 0x19; dst[1] = 0x12;               /* LVGL 9's image head: RGB565 */
    dst[4] = (uint8_t)ow; dst[5] = (uint8_t)(ow >> 8);
    dst[6] = (uint8_t)oh; dst[7] = (uint8_t)(oh >> 8);
    dst[8] = (uint8_t)(ow * 2); dst[9] = (uint8_t)((ow * 2) >> 8);
    char rb[96];
    sl_epub_cover_name(rb, sizeof rb, hash);
    bool ok = ep_write(rb, dst, bytes);
    ep_free(dst);
    return ok;
}
static void ep_cover(struct sl_epub *b, struct sl_epub_book *k)
{
    if (b->cover < 0) return;
    char real[600];
    int fd = ep_real(real, sizeof real, b->path) ? open(real, O_RDONLY) : -1;
    if (fd < 0) return;
    size_t n;
    bool cut;
    char *img = ep_member(b, fd, b->cover, 1u << 20, &n, &cut);   /* a bigger cover is left out, not the player's memory */
    close(fd);
    if (!img) return;
    int w = 0, h = 0;
    unsigned char *rgb = cut ? NULL : sl_art_rgb_load((const unsigned char *)img, (int)n, &w, &h);
    ep_free(img);
    if (!rgb) return;
    ep_count_foreign((size_t)w * h * 3, 1);
    ep_mkdirs();
    k->cover = ep_cover_write(k->hash, rgb, w, h);
    ep_count_foreign((size_t)w * h * 3, -1);
    sl_art_rgb_free(rgb);
}
static int shelf_cmp(const void *a, const void *b)
{
    const struct sl_epub_book *x = a, *y = b;
    int c = strcasecmp(x->title, y->title);
    return c ? c : strcmp(x->path, y->path);
}
struct ep_walk { struct sl_epub_book *found; int n, cap; };
static volatile bool ep_reader_waiting;      /* a book to open: the shelf stops and looks again later */
static void ep_walk_dir(struct ep_walk *w, const char *rb, int depth)
{
    char real[600];
    if (ep_held || ep_reader_waiting || !ep_real(real, sizeof real, rb)) return;
    DIR *d = opendir(real);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && w->n < SL_EPUB_SHELF_MAX && !ep_held)
    {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        char child[600];
        if (snprintf(child, sizeof child, "%s%s%s", rb, rb[1] ? "/" : "", nm) >= 250) continue;
        char cr[700];
        struct stat st;
        if (!ep_real(cr, sizeof cr, child) || stat(cr, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth > 0) ep_walk_dir(w, child, depth - 1); continue; }
        const char *dot = strrchr(nm, '.');
        if (!dot || strcasecmp(dot, ".epub")) continue;
        if (w->n >= w->cap)
        {
            int nc = w->cap ? w->cap * 2 : 16;
            struct sl_epub_book *nf = ep_realloc(w->found, (size_t)nc * sizeof *nf);
            if (!nf) break;
            w->found = nf;
            w->cap = nc;
        }
        struct sl_epub_book *k = &w->found[w->n++];
        memset(k, 0, sizeof *k);
        snprintf(k->path, sizeof k->path, "%s", child);
        k->size = (uint32_t)st.st_size;
        k->mtime = (uint32_t)st.st_mtime;
        k->hash = sl_epub_hash(k->path);
    }
    closedir(d);
}
/* the worker's: every book's place, the new ones opened for their names and covers */
static bool ep_shelf_run(void)
{
    struct ep_walk w = { NULL, 0, 0 };
    static const struct { const char *dir; int depth; } where[] = {
        { SL_EPUB_DIR, 4 }, { "/", 0 }, { "/Documents", 2 }, { "/Download", 1 }, { "/Downloads", 1 }, { "/eBooks", 3 },
    };
    for (unsigned i = 0; i < sizeof where / sizeof where[0]; i++) ep_walk_dir(&w, where[i].dir, where[i].depth);
    if (ep_reader_waiting || ep_held) { ep_free(w.found); return false; }
    pthread_mutex_lock(&shelf_lock);
    shelf_load();
    /* a book seen before and not changed since: what was read of it is kept (its title set says so) */
    for (int i = 0; i < w.n; i++)
        for (int j = 0; j < nshelf; j++)
            if (shelf[j].hash == w.found[i].hash && shelf[j].size == w.found[i].size && shelf[j].mtime == w.found[i].mtime &&
                !strcmp(shelf[j].path, w.found[i].path)) { w.found[i] = shelf[j]; break; }
    pthread_mutex_unlock(&shelf_lock);
    bool changed = false;
    for (int i = 0; i < w.n && !ep_reader_waiting && !ep_held; i++)
    {
        struct sl_epub_book *k = &w.found[i];
        if (k->title[0]) continue;             /* seen before, unchanged */
        int err;
        struct sl_epub *b = sl_epub_open(k->path, &err);
        if (b)
        {
            snprintf(k->title, sizeof k->title, "%s", b->title);
            snprintf(k->author, sizeof k->author, "%s", b->author);
            k->rtl = b->rtl;
            ep_cover(b, k);
            sl_epub_free(b);
        }
        else
        {
            const char *base = strrchr(k->path, '/');
            snprintf(k->title, sizeof k->title, "%s", base ? base + 1 : k->path);
            k->bad = 1;
        }
        ep_clean(k->title);
        ep_clean(k->author);
        changed = true;
    }
    if (ep_reader_waiting || ep_held)
    {
        /* stopped: what was learnt is kept for the next look */
        pthread_mutex_lock(&shelf_lock);
        for (int i = 0; i < w.n; i++)
        {
            if (!w.found[i].title[0]) continue;
            int j = 0;
            while (j < nshelf && strcmp(shelf[j].path, w.found[i].path)) j++;
            if (j < nshelf) shelf[j] = w.found[i];
        }
        pthread_mutex_unlock(&shelf_lock);
        ep_free(w.found);
        return false;
    }
    if (w.n) qsort(w.found, (size_t)w.n, sizeof *w.found, shelf_cmp);
    pthread_mutex_lock(&shelf_lock);
    if (!changed && w.n != nshelf) changed = true;
    for (int i = 0; !changed && i < w.n; i++) changed = strcmp(w.found[i].path, shelf[i].path) != 0;
    /* covers of books gone from the card go with them */
    for (int j = 0; j < nshelf; j++)
    {
        bool still = false;
        for (int i = 0; i < w.n && !still; i++) still = w.found[i].hash == shelf[j].hash;
        char rb[96], real[600];
        sl_epub_cover_name(rb, sizeof rb, shelf[j].hash);
        if (!still && shelf[j].cover && ep_real(real, sizeof real, rb)) unlink(real);
    }
    ep_free(shelf);
    shelf = w.found;
    nshelf = w.n;
    if (changed) shelf_seq_n++;
    pthread_mutex_unlock(&shelf_lock);
    if (changed) shelf_write(shelf, nshelf);   /* only this thread changes the shelf: read without the lock */
    return true;
}
unsigned sl_epub_shelf_seq(void) { pthread_mutex_lock(&shelf_lock); unsigned s = shelf_seq_n; pthread_mutex_unlock(&shelf_lock); return s; }
int sl_epub_shelf(struct sl_epub_book **out)
{
    pthread_mutex_lock(&shelf_lock);
    shelf_load();
    int n = nshelf;
    *out = n ? malloc((size_t)n * sizeof **out) : NULL;
    if (*out) memcpy(*out, shelf, (size_t)n * sizeof **out); else n = 0;
    pthread_mutex_unlock(&shelf_lock);
    return n;
}

/* ---------------------------------------------------------------- the worker */
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t job_thread;
static bool job_running, job_joinable;
static struct {
    int kind;                                 /* SL_EPUB_J_OPEN / _CHAP, NONE none waiting */
    char path[300];
    struct sl_epub *book;
    int spine;
    unsigned ticket;
} job_want;
static bool job_shelf, job_save;
static unsigned job_tickets;
static struct sl_epub *job_busy_book;         /* the book a chapter is being read from */
static bool job_busy_free;                    /* released meanwhile: freed when done */
static struct { int kind, err; unsigned ticket; struct sl_epub *book; struct sl_epub_chap *chap; } job_done;
static bool job_drop;                         /* the reader closed: results freed */
static bool shelf_running;

static void *ep_worker(void *unused)
{
    (void)unused;
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   /* music and touch come first */
#endif
    for (;;)
    {
        pthread_mutex_lock(&job_lock);
        if (job_want.kind != SL_EPUB_J_NONE)
        {
            int kind = job_want.kind, spine = job_want.spine;
            unsigned ticket = job_want.ticket;
            char path[300];
            snprintf(path, sizeof path, "%s", job_want.path);
            struct sl_epub *book = job_want.book;
            job_want.kind = SL_EPUB_J_NONE;
            ep_reader_waiting = false;
            job_busy_book = book;
            job_busy_free = false;
            pthread_mutex_unlock(&job_lock);
            int err = SL_EPUB_OK;
            struct sl_epub *b = NULL;
            struct sl_epub_chap *c = NULL;
            if (kind == SL_EPUB_J_OPEN) b = sl_epub_open(path, &err);
            else c = sl_epub_chapter(book, spine);
            pthread_mutex_lock(&job_lock);
            job_busy_book = NULL;
            if (job_busy_free) { sl_epub_free(book); job_busy_free = false; }
            bool stale = job_drop || ticket != job_tickets || job_want.kind != SL_EPUB_J_NONE;
            if (stale) { sl_epub_free(b); sl_epub_chap_free(c); }
            else
            {
                sl_epub_free(job_done.book);
                sl_epub_chap_free(job_done.chap);
                job_done.kind = kind; job_done.err = err; job_done.ticket = ticket; job_done.book = b; job_done.chap = c;
            }
            pthread_mutex_unlock(&job_lock);
            continue;
        }
        if (job_save)
        {
            job_save = false;
            pthread_mutex_unlock(&job_lock);
            pos_write();
            continue;
        }
        if (job_shelf)
        {
            shelf_running = true;
            pthread_mutex_unlock(&job_lock);
            bool done = ep_shelf_run();
            pthread_mutex_lock(&job_lock);
            shelf_running = false;
            if (done || ep_held) job_shelf = false;   /* else: a book came first, the look goes on after */
            pthread_mutex_unlock(&job_lock);
            continue;
        }
        job_running = false;
        pthread_mutex_unlock(&job_lock);
        return NULL;
    }
}
static void ep_kick(void)
{
    /* under job_lock */
    if (job_running) return;
    if (job_joinable) { pthread_join(job_thread, NULL); job_joinable = false; }
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    job_running = pthread_create(&job_thread, &a, ep_worker, NULL) == 0;
    pthread_attr_destroy(&a);
    job_joinable = job_running;
}
static unsigned ep_want(int kind, const char *path, struct sl_epub *b, int spine)
{
    pthread_mutex_lock(&job_lock);
    job_drop = false;
    job_want.kind = kind;
    snprintf(job_want.path, sizeof job_want.path, "%s", path ? path : "");
    job_want.book = b;
    job_want.spine = spine;
    job_want.ticket = ++job_tickets;
    ep_reader_waiting = true;
    sl_epub_free(job_done.book); sl_epub_chap_free(job_done.chap);
    memset(&job_done, 0, sizeof job_done);
    unsigned t = job_tickets;
    ep_kick();
    pthread_mutex_unlock(&job_lock);
    return t;
}
unsigned sl_epub_open_start(const char *path) { return ep_want(SL_EPUB_J_OPEN, path, NULL, 0); }
unsigned sl_epub_chapter_start(struct sl_epub *b, int spine) { return ep_want(SL_EPUB_J_CHAP, NULL, b, spine); }
int sl_epub_poll(unsigned *ticket, struct sl_epub **book, struct sl_epub_chap **chap, int *err)
{
    pthread_mutex_lock(&job_lock);
    int k = job_done.kind;
    if (k)
    {
        *ticket = job_done.ticket; *book = job_done.book; *chap = job_done.chap; *err = job_done.err;
        memset(&job_done, 0, sizeof job_done);
    }
    pthread_mutex_unlock(&job_lock);
    return k;
}
void sl_epub_drop(void)
{
    pthread_mutex_lock(&job_lock);
    job_drop = true;
    job_tickets++;
    if (job_want.kind) { job_want.kind = SL_EPUB_J_NONE; ep_reader_waiting = false; }
    sl_epub_free(job_done.book); sl_epub_chap_free(job_done.chap);
    memset(&job_done, 0, sizeof job_done);
    pthread_mutex_unlock(&job_lock);
}
void sl_epub_release(struct sl_epub *b)
{
    if (!b) return;
    pthread_mutex_lock(&job_lock);
    if (job_want.book == b) { job_want.kind = SL_EPUB_J_NONE; ep_reader_waiting = false; }
    if (job_busy_book == b) job_busy_free = true;
    else sl_epub_free(b);
    pthread_mutex_unlock(&job_lock);
}
bool sl_epub_busy(void)
{
    pthread_mutex_lock(&job_lock);
    bool b = job_running;
    pthread_mutex_unlock(&job_lock);
    return b;
}
void sl_epub_shelf_scan(void)
{
    pthread_mutex_lock(&job_lock);
    if (!ep_held) { job_shelf = true; ep_kick(); }
    pthread_mutex_unlock(&job_lock);
}
bool sl_epub_shelf_scanning(void)
{
    pthread_mutex_lock(&job_lock);
    bool s = job_shelf || shelf_running;
    pthread_mutex_unlock(&job_lock);
    return s;
}
void sl_epub_save(bool now)
{
    if (now) { pos_write(); return; }
    pthread_mutex_lock(&pos_lock);
    bool dirty = pos_dirty;
    pthread_mutex_unlock(&pos_lock);
    if (!dirty) return;                       /* nothing new: no thread woken for it */
    pthread_mutex_lock(&job_lock);
    job_save = true;
    ep_kick();
    pthread_mutex_unlock(&job_lock);
}
void sl_epub_hold(bool hold)
{
    if (hold) sl_epub_save(true);             /* where the book was, first */
    ep_held = hold;
    if (hold)
    {
        /* what is reading the card stops at its next file */
        for (int i = 0; i < 400 && sl_epub_busy(); i++) usleep(5000);
    }
}
void sl_epub_forget(void)
{
    for (int i = 0; i < 2000 && sl_epub_busy(); i++) usleep(2000);
    pthread_mutex_lock(&pos_lock);
    ep_free(pos); pos = NULL; npos = 0; pos_read = false; pos_dirty = false; pos_theme = pos_size = -1;
    pthread_mutex_unlock(&pos_lock);
    pthread_mutex_lock(&shelf_lock);
    ep_free(shelf); shelf = NULL; nshelf = 0; shelf_read = false;
    pthread_mutex_unlock(&shelf_lock);
}
