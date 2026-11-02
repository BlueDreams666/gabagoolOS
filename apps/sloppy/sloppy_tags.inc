/* ------------------------------------------ artists and genres, one by one */
/* Included by sloppy.c. A tag that names more than one artist ("A feat. B",
 * "A & B", "A, B", "A; B", "A / B", "A x B", "A vs. B") counts for each of
 * them, and a genre tag naming more than one ("Rock; Pop") for each genre.
 * Nothing about it is stored: the split is made from the library in memory
 * the first time a list needs it, and again when the library or a setting
 * changes, so turning a setting on or off needs no rescan. Band names that
 * only look like two artists stay whole: a list of the well known ones,
 * any "... & the ..." (Tom Petty & the Heartbreakers), a comma before "the"
 * (Tyler, the Creator), a slash between short halves (AC/DC), and the names
 * picked on Keep together (keep_together.txt on the card, a name a line). */

/* the ways of naming several artists; a bit each in sloppy_split_off */
enum { TG_FEAT = 1, TG_AMP = 2, TG_COMMA = 4, TG_SEMI = 8, TG_SLASH = 16, TG_X = 32, TG_VS = 64 };
#define TG_SEPS 7
static const struct { int bit; const char *name, *example, *note; } tg_seps[TG_SEPS] = {
    { TG_FEAT, "feat.", "Artist feat. Guest", "And ft., featuring, (with Guest)" },
    { TG_AMP, "&", "Artist & Artist", "Not before \"the\": Tom Petty & the Heartbreakers stays whole" },
    { TG_COMMA, ",", "Artist, Artist", "Not before \"the\": Tyler, the Creator stays whole" },
    { TG_SEMI, ";", "Artist; Artist", NULL },
    { TG_SLASH, "/", "Artist / Artist", "Not between short names: AC/DC stays whole" },
    { TG_X, "x", "Artist x Artist", NULL },
    { TG_VS, "vs.", "Artist vs. Artist", NULL },
};
#define TG_KEEP_FILE ROCKBOX_DIR "/sloppy/keep_together.txt"
#define TG_KEEP_MAX 256
#define TG_PIECES 12
/* names that are one act, whatever they look like */
static const char *const tg_keep_builtin[] = {
    "Simon & Garfunkel", "Earth, Wind & Fire", "Crosby, Stills, Nash & Young", "Crosby, Stills & Nash",
    "Emerson, Lake & Palmer", "Peter, Paul and Mary", "Hall & Oates", "Daryl Hall & John Oates",
    "Mumford & Sons", "Brooks & Dunn", "Sam & Dave", "Ike & Tina Turner", "Belle & Sebastian",
    "Iron & Wine", "Above & Beyond", "Chase & Status", "Captain & Tennille", "Seals & Crofts",
    "Loggins & Messina", "Ashford & Simpson", "Peaches & Herb", "Sonny & Cher", "Big & Rich",
    "Matt & Kim", "Nico & Vinz", "Maddie & Tae", "Love/Hate", "Kool & the Gang", "Hootie & the Blowfish",
    /* genres, Discogs' and iTunes' among them, that are one genre */
    "Singer/Songwriter", "Hip-Hop/Rap", "Hip Hop/Rap", "R&B/Soul", "Funk / Soul", "Folk, World, & Country",
    "Stage & Screen", "Brass & Military",
};
#define TG_KEEP_BUILTIN ((int)(sizeof tg_keep_builtin / sizeof tg_keep_builtin[0]))
static char *tg_keep_buf;               /* the card's list, read once: names separated by NULs */
static const char *tg_keep_user[TG_KEEP_MAX];
static int tg_keep_n = -1;              /* -1: not read yet */
static unsigned tg_keep_gen;            /* changes with the list, so the index is made again */

static void tg_keep_load(void)
{
    if (tg_keep_n >= 0) return;
    tg_keep_n = 0;
    free(tg_keep_buf);
    tg_keep_buf = NULL;
    int fd = open(TG_KEEP_FILE, O_RDONLY);
    if (fd < 0) return;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size > 0 && size < 64 * 1024 && lseek(fd, 0, SEEK_SET) == 0 && (tg_keep_buf = malloc((size_t)size + 1)))
    {
        ssize_t got = read(fd, tg_keep_buf, (size_t)size);
        tg_keep_buf[got > 0 ? got : 0] = '\0';
        for (char *p = tg_keep_buf; *p && tg_keep_n < TG_KEEP_MAX; )
        {
            char *e = p + strcspn(p, "\r\n");
            bool last = !*e;
            *e = '\0';
            while (*p == ' ') p++;
            if (*p) tg_keep_user[tg_keep_n++] = p;
            if (last) break;
            p = e + 1;
        }
    }
    close(fd);
}
static bool tg_keep_save(void)
{
    mkdir(ROCKBOX_DIR "/sloppy");
    int fd = open(TG_KEEP_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    bool ok = true;
    for (int i = 0; i < tg_keep_n && ok; i++)
        ok = write(fd, tg_keep_user[i], strlen(tg_keep_user[i])) == (ssize_t)strlen(tg_keep_user[i]) && write(fd, "\n", 1) == 1;
    if (ok) ok = fsync(fd) == 0;   /* on the card before the rename */
    if (close(fd) < 0) ok = false;
    if (!ok || rename(TG_KEEP_FILE ".tmp", TG_KEEP_FILE) < 0) { remove(TG_KEEP_FILE ".tmp"); return false; }
    return true;
}
static bool tg_kept(const char *s)
{
    tg_keep_load();
    for (int i = 0; i < tg_keep_n; i++) if (!strcasecmp(tg_keep_user[i], s)) return true;
    return false;
}
/* Keep together: a name on the card's list, or off it again */
static void tg_keep_set(const char *s, bool keep)
{
    tg_keep_load();
    if (keep == tg_kept(s)) return;
    /* the list is read back from the file, so a name added is in it */
    size_t total = 0;
    for (int i = 0; i < tg_keep_n; i++) if (keep || strcasecmp(tg_keep_user[i], s)) total += strlen(tg_keep_user[i]) + 1;
    if (keep) total += strlen(s) + 1;
    char *nb = malloc(total + 1), *at = nb;
    if (!nb) return;
    int n = 0;
    for (int i = 0; i < tg_keep_n; i++)
    {
        if (!keep && !strcasecmp(tg_keep_user[i], s)) continue;
        strcpy(at, tg_keep_user[i]);
        tg_keep_user[n++] = at;
        at += strlen(at) + 1;
    }
    if (keep && n < TG_KEEP_MAX) { strcpy(at, s); tg_keep_user[n++] = at; }
    free(tg_keep_buf);
    tg_keep_buf = nb;
    tg_keep_n = n;
    tg_keep_gen++;
    if (!tg_keep_save()) toast_warn("Could not save that. Check the card.");
}

static int tg_fold(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* the keyword w (lower case) at s, as a whole word */
static int tg_word(const char *s, const char *w)
{
    int k = 0;
    while (w[k] && tg_fold((unsigned char)s[k]) == (unsigned char)w[k]) k++;
    return w[k] ? 0 : k;
}
/* A known name starting at s and ending where a piece can end: its length */
static int tg_keep_at(const char *s)
{
    for (int pass = 0; pass < 2; pass++)
    {
        int n = pass ? tg_keep_n : TG_KEEP_BUILTIN;
        for (int i = 0; i < n; i++)
        {
            const char *k = pass ? tg_keep_user[i] : tg_keep_builtin[i];
            if (tg_fold((unsigned char)*k) != tg_fold((unsigned char)*s)) continue;
            size_t len = strlen(k);
            if (strncasecmp(s, k, len)) continue;
            char c = s[len];
            if (!c || c == ' ' || c == ',' || c == ';' || c == '/' || c == ')' || c == ']') return (int)len;
        }
    }
    return 0;
}
/* A separator at s (start is where this piece began): its length, or 0.
 * *kind says which (TG_*, or 0 for a genre's); *open is set when it opens
 * a bracket, "(feat. B)", closed at the end of the tag. */
static int tg_sep_at(const char *s, const char *start, unsigned seps, bool genre, int *kind, bool *open)
{
    unsigned char c = (unsigned char)*s;
    *kind = 0;
    if (genre)
    {
        if (c == ';' || c == ',' || c == '|' || c == '\\' || c == '/') return 1;
        if (c == ' ' && (s[1] == '/' || s[1] == '|' || s[1] == ';')) return s[2] == ' ' ? 3 : 2;
        return 0;
    }
    if ((seps & TG_SEMI) && c == ';') { *kind = TG_SEMI; return 1; }
    if ((seps & TG_FEAT) && (c == ' ' || c == '(' || c == '['))
    {
        const char *p = s + 1;
        if (c == ' ' && (*p == '(' || *p == '[')) { c = (unsigned char)*p; p++; }
        else if (c != ' ') p = s + 1;
        static const char *const kw[] = { "featuring ", "feat. ", "feat ", "ft. ", "ft ", "with " };
        for (unsigned i = 0; i < sizeof kw / sizeof kw[0]; i++)
        {
            if (i == 5 && c != '(' && c != '[') continue;     /* "with" only in brackets: "(with B)" */
            int k = tg_word(p, kw[i]);
            if (!k) continue;
            *open = c == '(' || c == '[';
            *kind = TG_FEAT;
            return (int)(p - s) + k;
        }
    }
    if ((seps & TG_AMP) && c == ' ' && s[1] == '&' && s[2] == ' ')
    {
        if (tg_word(s + 3, "the ")) return 0;             /* "Tom Petty & the Heartbreakers" is a band */
        *kind = TG_AMP;
        return 3;
    }
    if ((seps & TG_COMMA) && c == ',')
    {
        const char *p = s + 1 + (s[1] == ' ');
        if (tg_word(p, "the ") || tg_word(p, "jr") || tg_word(p, "sr") || tg_word(p, "inc") || tg_word(p, "ltd")) return 0;
        if (*p == '&') return 0;                         /* "A, B, & C": the & splits */
        *kind = TG_COMMA;
        return (int)(p - s);
    }
    if ((seps & TG_SLASH) && c == ' ' && s[1] == '/' && s[2] == ' ') { *kind = TG_SLASH; return 3; }
    if ((seps & TG_SLASH) && c == '/')
    {
        /* bare: only between halves longer than two letters (AC/DC stays) */
        int left = (int)(s - start), right = (int)strcspn(s + 1, " /,;&");
        if (left <= 2 || right <= 2) return 0;
        *kind = TG_SLASH;
        return 1;
    }
    if ((seps & TG_X) && c == ' ' && (s[1] == 'x' || s[1] == 'X') && s[2] == ' ') { *kind = TG_X; return 3; }
    if ((seps & TG_VS) && c == ' ')
    {
        int k = tg_word(s + 1, "vs. ");
        if (!k) k = tg_word(s + 1, "vs ");
        if (k) { *kind = TG_VS; return 1 + k; }
    }
    return 0;
}
struct tg_piece { const char *p; int len; };
/* The artists (or genres) one tag names, in order, each once. A tag that
 * names one is one piece, the whole of it. *ambiguous: it was split where
 * a band's own name could have looked the same (&, a comma, a slash, x or
 * vs.), so Keep together offers it. */
static int tg_split(const char *s, bool genre, struct tg_piece *out, int max, bool *ambiguous)
{
    unsigned seps = genre ? 0 : (unsigned)~global_settings.sloppy_split_off & 127;
    bool whole = genre ? global_settings.sloppy_genres_whole : global_settings.sloppy_artists_whole;
    if (ambiguous) *ambiguous = false;
    if (!s || !*s || max < 1) return 0;
    int n = 0, kind;
    bool open = false;
    if (!whole && !tg_kept(s))
    {
        /* a quick look first: most tags name one, and need nothing more */
        bool any = false;
        for (const char *p = s; *p && !any; p++) any = tg_sep_at(p, s, seps, genre, &kind, &open) > 0;
        if (any)
        {
            const char *p = s, *start;
            bool closing = false;
            while (*p == ' ') p++;
            start = p;
            for (;;)
            {
                if (p == start) { int k = tg_keep_at(p); if (k) p += k; }
                open = false;
                int sep = *p ? tg_sep_at(p, start, seps, genre, &kind, &open) : 0;
                if (*p && !sep) { p++; continue; }
                /* a piece ends: trimmed, a bracket it closes dropped */
                const char *e = p;
                while (e > start && (e[-1] == ' ' || (e[-1] == ',' && *p))) e--;   /* "A, B, & C": the comma before the & */
                if (closing && e > start && (e[-1] == ')' || e[-1] == ']')) { e--; while (e > start && e[-1] == ' ') e--; }
                if (e > start && n < max)
                {
                    bool seen = false;
                    for (int i = 0; i < n && !seen; i++)
                        seen = out[i].len == e - start && !strncasecmp(out[i].p, start, (size_t)(e - start));
                    if (!seen) out[n++] = (struct tg_piece){ start, (int)(e - start) };
                }
                if (!*p) break;
                if (ambiguous && (kind & (TG_AMP | TG_COMMA | TG_SLASH | TG_X | TG_VS))) *ambiguous = true;
                if (open) closing = true;
                p += sep;
                while (*p == ' ') p++;
                start = p;
            }
        }
    }
    if (!n) out[n++] = (struct tg_piece){ s, (int)strlen(s) };
    return n;
}

/* ------------------------------------------------------------ the index */
/* Artists by id, each with the albums they're on (theirs first) and how
 * many songs; listed A to Z through order[]. Genres renumbered A to Z,
 * each with its albums by title. Every album's own credit is kept, so a
 * song without an artist tag of its own counts for its album's artists. */
struct tg_index {
    bool built, failed;
    const void *tracks, *albums;          /* what it was made from */
    int nt, na;
    unsigned key, epoch;
    char *arena; size_t arena_len, arena_cap;
    int *hash; int hash_cap;              /* name -> id; a genre's with bit 30 */
    int n, cap, listed;                   /* artists named, room for them, listed (with an album) */
    int32_t *name;                        /* arena offsets */
    int *songs, *first, *mine, *order;    /* first: n + 1 offsets into alb; mine: how many of theirs lead it */
    int *alb;
    int *acred_first, *acred;             /* each album's credit: acred[acred_first[a]..acred_first[a + 1]) */
    int ng, gcap;
    int32_t *gname;
    int *gfirst, *galb;
};
static struct tg_index tgx;
static unsigned lib_epoch;               /* moves on whenever sl_lib is read again */
#define TG_NAME(i) (tgx.arena + tgx.name[i])
#define TG_GNAME(i) (tgx.arena + tgx.gname[i])
static void tg_free(void)
{
    free(tgx.arena); free(tgx.hash); free(tgx.name); free(tgx.songs); free(tgx.first); free(tgx.mine);
    free(tgx.order); free(tgx.alb); free(tgx.acred_first); free(tgx.acred); free(tgx.gname); free(tgx.gfirst); free(tgx.galb);
    memset(&tgx, 0, sizeof tgx);
}
static unsigned tg_key(void)
{
    return (unsigned)global_settings.sloppy_artists_whole | (unsigned)global_settings.sloppy_genres_whole << 1
         | ((unsigned)global_settings.sloppy_split_off & 127) << 2 | (unsigned)sl_sort_as_written << 9 | tg_keep_gen << 10;
}
static int32_t tg_arena_add(const char *p, int len)
{
    if (tgx.arena_len + (size_t)len + 1 > tgx.arena_cap)
    {
        size_t nc = tgx.arena_cap ? tgx.arena_cap * 2 : 16 * 1024;
        while (nc < tgx.arena_len + (size_t)len + 1) nc *= 2;
        char *na = realloc(tgx.arena, nc);
        if (!na) return -1;
        tgx.arena = na;
        tgx.arena_cap = nc;
    }
    memcpy(tgx.arena + tgx.arena_len, p, (size_t)len);
    tgx.arena[tgx.arena_len + (size_t)len] = '\0';
    tgx.arena_len += (size_t)len + 1;
    return (int32_t)(tgx.arena_len - (size_t)len - 1);
}
static unsigned tg_hash(const char *p, int len, bool genre)
{
    unsigned h = genre ? 0x9e3779b9u : 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ (unsigned)tg_fold((unsigned char)p[i])) * 16777619u;
    return h;
}
static bool tg_rehash(int cap)
{
    int *nh = malloc((size_t)cap * sizeof *nh);
    if (!nh) return false;
    for (int i = 0; i < cap; i++) nh[i] = -1;
    for (int g = 0; g < 2; g++)
        for (int i = 0; i < (g ? tgx.ng : tgx.n); i++)
        {
            const char *s = tgx.arena + (g ? tgx.gname : tgx.name)[i];
            unsigned slot = tg_hash(s, (int)strlen(s), g) & (unsigned)(cap - 1);
            while (nh[slot] >= 0) slot = (slot + 1) & (unsigned)(cap - 1);
            nh[slot] = g << 30 | i;
        }
    free(tgx.hash);
    tgx.hash = nh;
    tgx.hash_cap = cap;
    return true;
}
/* the id of a name (case aside), or -1; *slot_out: where a new one goes */
static int tg_lookup(const char *p, int len, bool genre, int *slot_out)
{
    if (!tgx.hash_cap) return -1;
    unsigned slot = tg_hash(p, len, genre) & (unsigned)(tgx.hash_cap - 1);
    for (;;)
    {
        int v = tgx.hash[slot];
        if (v < 0) break;
        if ((v >> 30) == (int)genre)
        {
            const char *s = tgx.arena + (genre ? tgx.gname : tgx.name)[v & 0x3fffffff];
            if (!strncasecmp(s, p, (size_t)len) && !s[len]) return v & 0x3fffffff;
        }
        slot = (slot + 1) & (unsigned)(tgx.hash_cap - 1);
    }
    if (slot_out) *slot_out = (int)slot;
    return -1;
}
/* the id of a name, made if new; -1 out of memory */
static int tg_intern(const char *p, int len, bool genre)
{
    if ((tgx.n + tgx.ng + 1) * 2 > tgx.hash_cap && !tg_rehash(tgx.hash_cap ? tgx.hash_cap * 2 : 4096)) return -1;
    int slot, id = tg_lookup(p, len, genre, &slot);
    if (id >= 0) return id;
    int *n = genre ? &tgx.ng : &tgx.n, *cap = genre ? &tgx.gcap : &tgx.cap;
    int32_t **names = genre ? &tgx.gname : &tgx.name;
    if (*n == *cap)
    {
        int nc = *cap ? *cap * 2 : 1024;
        int32_t *nn = realloc(*names, (size_t)nc * sizeof *nn);
        if (!nn) return -1;
        *names = nn;
        *cap = nc;
    }
    int32_t off = tg_arena_add(p, len);
    if (off < 0) return -1;
    (*names)[*n] = off;
    tgx.hash[slot] = (genre ? 1 << 30 : 0) | *n;
    return (*n)++;
}
/* the ids of the artists (or genres) a tag names; made if new while the
 * index is being made, else only those it has */
static int tg_ids(const char *s, bool genre, int *ids, int max, bool make)
{
    struct tg_piece pc[TG_PIECES];
    int n = tg_split(s, genre, pc, LV_MIN(max, TG_PIECES), NULL), m = 0;
    for (int i = 0; i < n; i++)
    {
        int id = make ? tg_intern(pc[i].p, pc[i].len, genre) : tg_lookup(pc[i].p, pc[i].len, genre, NULL);
        if (id < 0 && make) return -1;
        if (id >= 0) ids[m++] = id;
    }
    return m;
}
static bool tg_album_by(int a, int id)
{
    for (int q = tgx.acred_first[a]; q < tgx.acred_first[a + 1]; q++) if (tgx.acred[q] == id) return true;
    return false;
}
static int tg_cmp_order(const void *a, const void *b)
{
    const char *x = TG_NAME(*(const int *)a), *y = TG_NAME(*(const int *)b);
    int c = sl_sort_cmp(x, y);
    return c ? c : strcasecmp(x, y);
}
/* theirs first (marked with bit 30 while sorting), then the ones they're
 * on; each oldest first, as the Album artists pages have them */
static int tg_cmp_albums(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    bool mx = x >> 30, my = y >> 30;
    if (mx != my) return my - mx;
    x &= 0x3fffffff; y &= 0x3fffffff;
    const struct sl_album *ax = &sl_lib.albums[x], *ay = &sl_lib.albums[y];
    if (ax->year != ay->year) return (ax->year > ay->year) - (ax->year < ay->year);
    int c = sl_sort_cmp(ax->title, ay->title);
    return c ? c : (x > y) - (x < y);
}
static int tg_cmp_gname(const void *a, const void *b)
{
    const char *x = TG_GNAME(*(const int *)a), *y = TG_GNAME(*(const int *)b);
    int c = sl_sort_cmp(x, y);
    return c ? c : strcasecmp(x, y);
}
static bool tg_pair(int **id, int **alb, int *n, int *cap, int i, int a)
{
    if (*n == *cap)
    {
        int nc = *cap ? *cap * 2 : 4096;
        int *g1 = realloc(*id, (size_t)nc * sizeof *g1);
        if (!g1) return false;
        *id = g1;
        int *g2 = realloc(*alb, (size_t)nc * sizeof *g2);
        if (!g2) return false;
        *alb = g2;
        *cap = nc;
    }
    (*id)[*n] = i; (*alb)[*n] = a; (*n)++;
    return true;
}
static bool tg_make(void)
{
    int na = sl_lib.n_albums;
    int *pair_id = NULL, *pair_alb = NULL, pairs = 0, pair_cap = 0, *stamp = NULL, stamp_cap = 0, acred_cap = 0, acred_n = 0;
    bool ok = (tgx.acred_first = malloc(((size_t)na + 1) * sizeof(int))) != NULL;
    /* the albums' own credits: who each album artist tag names */
    for (int a = 0; ok && a < na; a++)
    {
        if ((a & 511) == 511) yield();     /* a big library takes a while: the decoder runs meanwhile */
        int ids[TG_PIECES], n = tg_ids(sl_lib.albums[a].artist, false, ids, TG_PIECES, true);
        if (n < 0) { ok = false; break; }
        if (acred_n + n > acred_cap)
        {
            int nc = acred_cap ? acred_cap * 2 : na + 64;
            int *g = realloc(tgx.acred, (size_t)nc * sizeof *g);
            if (!g) { ok = false; break; }
            tgx.acred = g; acred_cap = nc;
        }
        tgx.acred_first[a] = acred_n;
        memcpy(tgx.acred + acred_n, ids, (size_t)n * sizeof *ids);
        acred_n += n;
    }
    if (ok) tgx.acred_first[na] = acred_n;
    /* each song counts for who it names (or its album's credit); each album
     * goes under each of them once */
    for (int a = 0; ok && a < na; a++)
    {
        const struct sl_album *al = &sl_lib.albums[a];
        if ((a & 255) == 255) yield();
        for (int j = al->n_tracks ? 0 : -1; ok && j < al->n_tracks; j++)
        {
            int ids[TG_PIECES], n;
            const int *use = ids;
            const char *own = NULL;
            if (j >= 0) { int t = al->tracks[j]; own = t >= 0 && t < sl_lib.n_tracks ? sl_lib.tracks[t].artist : NULL; }
            if (own) n = tg_ids(own, false, ids, TG_PIECES, true);
            else { use = tgx.acred + tgx.acred_first[a]; n = tgx.acred_first[a + 1] - tgx.acred_first[a]; }   /* an album with no songs yet too */
            if (n < 0) { ok = false; break; }
            if (tgx.n > stamp_cap)
            {
                int nc = tgx.cap > tgx.n ? tgx.cap : tgx.n + 1024;
                int *g = realloc(stamp, (size_t)nc * sizeof *g);
                if (g) stamp = g;
                int *s2 = g ? realloc(tgx.songs, (size_t)nc * sizeof *s2) : NULL;
                if (!s2) { ok = false; break; }
                tgx.songs = s2;
                for (int i = stamp_cap; i < nc; i++) { stamp[i] = -1; tgx.songs[i] = 0; }
                stamp_cap = nc;
            }
            for (int i = 0; ok && i < n; i++)
            {
                int id = use[i];
                if (j >= 0) tgx.songs[id]++;
                if (stamp[id] == a) continue;
                stamp[id] = a;
                ok = tg_pair(&pair_id, &pair_alb, &pairs, &pair_cap, id, a);
            }
        }
    }
    /* each artist's albums in their order; the ones with any, A to Z (Various
     * Artists, credited only with songs all someone else's, has none) */
    if (ok)
    {
        tgx.first = calloc((size_t)tgx.n + 1, sizeof(int));
        tgx.mine = calloc((size_t)tgx.n + 1, sizeof(int));
        tgx.alb = malloc(((size_t)pairs + 1) * sizeof(int));
        tgx.order = malloc(((size_t)tgx.n + 1) * sizeof(int));
        if (!tgx.songs) tgx.songs = calloc(1, sizeof(int));
        if (stamp_cap < tgx.n + 1) { int *g = realloc(stamp, ((size_t)tgx.n + 1) * sizeof *g); if (g) { stamp = g; stamp_cap = tgx.n + 1; } }
        ok = tgx.first && tgx.mine && tgx.alb && tgx.order && tgx.songs && stamp_cap >= tgx.n + 1;
    }
    if (ok)
    {
        for (int i = 0; i < pairs; i++) tgx.first[pair_id[i] + 1]++;
        for (int i = 0; i < tgx.n; i++) tgx.first[i + 1] += tgx.first[i];
        int *fill = stamp;                   /* reused: where each artist's next album goes */
        for (int i = 0; i < tgx.n; i++) fill[i] = tgx.first[i];
        for (int i = 0; i < pairs; i++)
        {
            int id = pair_id[i], a = pair_alb[i];
            tgx.alb[fill[id]++] = a | (tg_album_by(a, id) ? 1 << 30 : 0);
        }
        for (int id = 0; id < tgx.n; id++)
        {
            int f = tgx.first[id], c = tgx.first[id + 1] - f, m = 0;
            if (c > 1) qsort(tgx.alb + f, (size_t)c, sizeof(int), tg_cmp_albums);
            for (int k = f; k < f + c; k++) { if (tgx.alb[k] >> 30) m++; tgx.alb[k] &= 0x3fffffff; }
            tgx.mine[id] = m;
            if (c) tgx.order[tgx.listed++] = id;
        }
        qsort(tgx.order, (size_t)tgx.listed, sizeof(int), tg_cmp_order);
    }
    /* genres: each album under each genre its tag names, by title */
    pairs = 0;
    for (int k = 0; ok && k < na; k++)
    {
        int a = sl_lib.albums_by_title ? sl_lib.albums_by_title[k] : k;
        const char *g = a >= 0 && a < na ? sl_lib.albums[a].genre : NULL;
        if (!g || !g[0]) continue;
        int ids[TG_PIECES], n = tg_ids(g, true, ids, TG_PIECES, true);
        if (n < 0) { ok = false; break; }
        for (int i = 0; ok && i < n; i++) ok = tg_pair(&pair_id, &pair_alb, &pairs, &pair_cap, ids[i], a);
    }
    if (ok)
    {
        /* renumbered A to Z, so a genre's id is its place in the list */
        int *gorder = malloc(((size_t)tgx.ng + 1) * sizeof(int)), *rank = malloc(((size_t)tgx.ng + 1) * sizeof(int));
        int32_t *gn = malloc(((size_t)tgx.ng + 1) * sizeof *gn);
        tgx.gfirst = calloc((size_t)tgx.ng + 1, sizeof(int));
        tgx.galb = malloc(((size_t)pairs + 1) * sizeof(int));
        ok = gorder && rank && gn && tgx.gfirst && tgx.galb;
        if (ok)
        {
            for (int i = 0; i < tgx.ng; i++) gorder[i] = i;
            qsort(gorder, (size_t)tgx.ng, sizeof(int), tg_cmp_gname);
            for (int i = 0; i < tgx.ng; i++) { rank[gorder[i]] = i; gn[i] = tgx.gname[gorder[i]]; }
            for (int i = 0; i < pairs; i++) tgx.gfirst[rank[pair_id[i]] + 1]++;
            for (int i = 0; i < tgx.ng; i++) tgx.gfirst[i + 1] += tgx.gfirst[i];
            int *fill = gorder;
            for (int i = 0; i < tgx.ng; i++) fill[i] = tgx.gfirst[i];
            for (int i = 0; i < pairs; i++) tgx.galb[fill[rank[pair_id[i]]]++] = pair_alb[i];
            free(tgx.gname);
            tgx.gname = gn;
            tgx.gcap = tgx.ng;
            gn = NULL;
            ok = tg_rehash(tgx.hash_cap ? tgx.hash_cap : 4096);   /* the genres' new numbers */
        }
        free(gorder); free(rank); free(gn);
    }
    free(pair_id); free(pair_alb); free(stamp);
    return ok;
}
/* The index for the library as it is now, made again when anything it was
 * made from has changed. false: not enough memory (the lists say so). */
static bool tg_current(void)
{
    return tgx.tracks == sl_lib.tracks && tgx.albums == sl_lib.albums && tgx.nt == sl_lib.n_tracks
        && tgx.na == sl_lib.n_albums && tgx.key == tg_key() && tgx.epoch == lib_epoch && (tgx.built || tgx.failed);
}
static bool tg_ready(void)
{
    unsigned key = tg_key();
    if (tg_current()) return tgx.built;
    uint64_t t0 = sl_perf_now_us();
    tg_free();
    bool ok = tg_make();
    if (!ok) { tg_free(); sl_log("library: not enough memory to list artists and genres one by one"); }
    else sl_log("library: %d artists, %d genres from %d albums in %u ms", tgx.listed, tgx.ng, sl_lib.n_albums,
                (unsigned)((sl_perf_now_us() - t0) / 1000));
    tgx.built = ok; tgx.failed = !ok;
    tgx.tracks = sl_lib.tracks; tgx.albums = sl_lib.albums; tgx.nt = sl_lib.n_tracks; tgx.na = sl_lib.n_albums;
    tgx.key = key; tgx.epoch = lib_epoch;
    return ok;
}
static int tg_n_artists(void) { return tg_ready() ? tgx.listed : 0; }
static int tg_albums_of(int id) { return tgx.first[id + 1] - tgx.first[id]; }
/* Does song t count for artist id? Its own artist tag, or its album's */
static bool tg_track_by(int t, int id)
{
    const char *own = sl_lib.tracks[t].artist;
    if (!own) return tg_album_by(sl_lib.tracks[t].album, id);
    int ids[TG_PIECES], n = tg_ids(own, false, ids, TG_PIECES, false);
    for (int i = 0; i < n; i++) if (ids[i] == id) return true;
    return false;
}
/* the songs artist id is on, album by album as the page shows them; the
 * caller frees the list */
static int tg_artist_tracks(int id, int **out)
{
    *out = NULL;
    if (!tg_ready() || id < 0 || id >= tgx.n) return 0;
    int total = 0;
    for (int k = tgx.first[id]; k < tgx.first[id + 1]; k++) total += sl_lib.albums[tgx.alb[k]].n_tracks;
    int *l = total ? malloc((size_t)total * sizeof *l) : NULL, n = 0;
    if (!l) return 0;
    for (int k = tgx.first[id]; k < tgx.first[id + 1]; k++)
    {
        const struct sl_album *al = &sl_lib.albums[tgx.alb[k]];
        for (int j = 0; j < al->n_tracks; j++) if (tg_track_by(al->tracks[j], id)) l[n++] = al->tracks[j];
    }
    *out = l;
    return n;
}
/* the artist a name is, or -1 */
static int tg_find(const char *name)
{
    return name && tg_ready() ? tg_lookup(name, (int)strlen(name), false, NULL) : -1;
}
/* the first artist song t names: for Go to artist */
static int tg_first_artist_of(int t)
{
    struct tg_piece pc[TG_PIECES];
    if (!tg_ready() || tg_split(sl_track_artist(t), false, pc, TG_PIECES, NULL) < 1) return -1;
    return tg_lookup(pc[0].p, pc[0].len, false, NULL);
}
