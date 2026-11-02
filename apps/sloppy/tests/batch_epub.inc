/* P51: the e-book reader (sloppy_epub.c, sloppy_reader.inc). Included from
 * ui_test.c after batch_h.inc, whose helpers it uses.
 *   ep_tests()            in the suite
 *   SLOPPY_EPUB_TESTS=1   the tests alone
 *   audit screens ebook-*, shot with
 *   SLOPPY_CENTER_SHOTS=<dir> SLOPPY_AUDIT_ONLY=ebook ./ui-test
 * The books are made at test time by epub_fixtures.py (Python's zipfile),
 * into the card the worker reads (H_ROOT). */
#define EP_BOOKS "/Books"
#define EP_ODD "/EpubTests"                   /* the broken ones: not on the shelf */
static bool ep_made;
static void ep_fixtures(void)
{
    if (ep_made) return;
    ep_made = true;
    char cmd[2000], here[600];
    snprintf(here, sizeof here, "%s", __FILE__);
    char *sl = strrchr(here, '/');
    if (sl) *sl = '\0';
    snprintf(cmd, sizeof cmd,
             "rm -rf '" H_ROOT EP_BOOKS "' '" H_ROOT EP_ODD "' '" H_ROOT SL_EPUB_STATE "' && mkdir -p '" H_ROOT EP_BOOKS "' '" H_ROOT EP_ODD "' && "
             "python3 -I '%s/epub_fixtures.py' '" H_ROOT EP_ODD "' && cd '" H_ROOT EP_ODD "' && "
             "mv latin.epub hebrew.epub deflate.epub big.epub cjk.epub '" H_ROOT EP_BOOKS "/'", here);
    assert(system(cmd) == 0);
}
/* everything the module keeps forgotten, and its files gone: as a first boot */
static void ep_clean_state(void)
{
    while (npages > 1) { page_pop(); frame(300); }
    for (int i = 0; i < 200 && sl_epub_busy(); i++) { frame(20); usleep(5000); }
    sl_epub_forget();
    assert(system("rm -rf '" H_ROOT SL_EPUB_STATE "'") == 0);
}
static bool ep_shown(void) { return rd.page && !rd.loading && (rd.at >= 0 || rd.state); }
static void ep_wait(bool (*done)(void), int ms)
{
    uint64_t until = h_ms() + (uint64_t)ms;
    while (h_ms() < until && !done()) { frame(20); usleep(4000); }
    frame(40);
}
static bool ep_laid(void) { return rd.page && rd.laid && rd.at >= 0; }
static bool ep_shelf_done(void) { return !sl_epub_shelf_scanning(); }
static void ep_open(const char *name, bool (*done)(void))
{
    char p[300];
    snprintf(p, sizeof p, "%s", name);
    rd_open(p);
    frame(300);
    ep_wait(done, 8000);
}
static void ep_tap(int x, int y) { touch_at(x, y, true); touch_at(x, y, false); frame(400); }
static void ep_swipe(int x0, int x1, int y)
{
    touch_at(x0, y, true);
    for (int i = 1; i <= 6; i++) touch_at(x0 + (x1 - x0) * i / 6, y, true);
    touch_at(x1, y, false);
    frame(400);
}
static int ep_mid_y(void) { return SB_H + SL_HDR_H + 200; }

static void ep_parse_tests(void)
{
    int err;
    /* ---- an EPUB 3, stored: metadata, the spine in order, the nav contents (not the landmarks), the cover */
    struct sl_epub *b = sl_epub_open(EP_BOOKS "/latin.epub", &err);
    assert(b && err == SL_EPUB_OK);
    assert(!strcmp(b->title, "The Quiet Harbour") && !strcmp(b->author, "Mira Sol") && !strcmp(b->lang, "en") && !b->rtl && !b->cut);
    assert(b->nspine == 3);
    assert(!strcmp(b->names + b->ent[b->spine[0]].name, "OEBPS/text/one.xhtml"));
    assert(!strcmp(b->names + b->ent[b->spine[1]].name, "OEBPS/text/two b.xhtml"));   /* %20 read */
    assert(!strcmp(b->names + b->ent[b->spine[2]].name, "OEBPS/text/three.xhtml"));
    assert(b->ntoc == 4);
    assert(!strcmp(sl_epub_toc_title(b, 0), "One: The Quiet Harbour") && !strcmp(sl_epub_toc_title(b, 1), "Two"));
    assert(!strcmp(sl_epub_toc_title(b, 2), "The Far Side") && b->toc[2].spine == 1 && b->toc[2].depth == 1 && b->toc[2].frag == sl_epub_hash("part-b"));
    assert(b->toc[3].spine == 2 && sl_epub_toc_at(b, 1) == 2);
    assert(b->cover >= 0 && !strcmp(b->names + b->ent[b->cover].name, "OEBPS/img/cover.png"));
    /* its second chapter: emphasis, entities, a break, a quote, lists, a picture, a rule, an anchor; no styles, no scripts */
    struct sl_epub_chap *c = sl_epub_chapter(b, 1);
    assert(c && !c->missing && !c->cut && c->np > 40);
    assert(c->p[0].kind == SL_EP_H2 && !strcmp(c->text + c->p[0].off, "Part Two"));
    assert((c->p[1].flags & SL_EPF_EM) && !strcmp(c->text + c->p[1].off, "A letter, kept in a drawer for forty years."));
    assert(!strcmp(c->text + c->p[2].off, "Caf\xc3\xa9 & bar \xe2\x80\x94 \xe2\x80\x9copen\xe2\x80\x9d, she said.\nThen nothing."));
    assert((c->p[3].flags & SL_EPF_QUOTE) && !(c->p[4].flags & SL_EPF_QUOTE));
    assert(!strcmp(c->text + c->p[4].off, "\xe2\x80\xa2 First thing") && !strcmp(c->text + c->p[7].off, "2. Two"));
    assert(c->p[8].kind == SL_EP_PIC && !strcmp(c->text + c->p[8].off, "A map of the bay") && c->p[9].kind == SL_EP_RULE);
    int fb = sl_epub_anchor(c, sl_epub_hash("part-b"));
    assert(fb > 9 && c->p[fb].kind == SL_EP_H3 && !strcmp(c->text + c->p[fb].off, "The Far Side"));
    for (int i = 0; i < c->np; i++) assert(!strstr(c->text + c->p[i].off, "not words") && !strstr(c->text + c->p[i].off, "nor these"));
    sl_epub_chap_free(c);
    sl_epub_free(b);
    /* ---- an EPUB 2, deflated: NCX contents, entities, an aside outside the order, a line in bold as a heading */
    b = sl_epub_open(EP_BOOKS "/deflate.epub", &err);
    assert(b && !strcmp(b->title, "Deflated & Done") && !strcmp(b->author, "A. N. Author") && b->nspine == 2 && b->ntoc == 2);
    assert(b->ent[b->spine[0]].method == 8 && b->cover < 0);
    c = sl_epub_chapter(b, 0);
    assert(c && c->p[0].kind == SL_EP_H1 && !strcmp(c->text + c->p[1].off, "Fish & chips <tonight> \xe2\x80\x94 and \xe2\x80\xa6 more."));
    sl_epub_chap_free(c);
    c = sl_epub_chapter(b, 1);
    assert(c && c->p[0].kind == SL_EP_H3 && !strcmp(c->text + c->p[0].off, "CHAPTER II"));
    sl_epub_chap_free(c);
    sl_epub_free(b);
    /* ---- Hebrew: the language turns its pages from the right */
    b = sl_epub_open(EP_BOOKS "/hebrew.epub", &err);
    assert(b && b->rtl && !strcmp(b->lang, "he") && b->ntoc == 2 && !strcmp(sl_epub_toc_title(b, 1), "\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa9\xd7\xa0\xd7\x99"));
    sl_epub_free(b);
    /* ---- a 2 MB chapter: cut at the cap, with the note; what it held in memory at most */
    sl_epub_mem_reset_peak();
    size_t m0 = sl_epub_mem_now();
    b = sl_epub_open(EP_BOOKS "/big.epub", &err);
    assert(b && b->nspine == 1 && b->ent[b->spine[0]].usize > 2000000);
    c = sl_epub_chapter(b, 0);
    assert(c && c->cut && !c->missing && c->n <= SL_EPUB_XHTML_CAP && c->np > 100);
    size_t peak_big = sl_epub_mem_peak() - m0, hold_big = c->bytes;
    sl_epub_chap_free(c);
    sl_epub_free(b);
    assert(sl_epub_mem_now() == m0);
    sl_epub_mem_reset_peak();
    b = sl_epub_open(EP_BOOKS "/latin.epub", &err);
    c = sl_epub_chapter(b, 0);
    size_t peak_typ = sl_epub_mem_peak() - m0, hold_typ = c->bytes + 0;
    sl_epub_chap_free(c);
    sl_epub_free(b);
    assert(sl_epub_mem_now() == m0);
    printf("e-books: memory: a typical chapter %zu KB at most while read, %zu KB held; a 2 MB chapter %zu KB at most, %zu KB held (cut at %d KB)\n",
           peak_typ / 1024, hold_typ / 1024, peak_big / 1024, hold_big / 1024, SL_EPUB_XHTML_CAP / 1024);
    assert(peak_big < 2u * 1024 * 1024);
    /* ---- broken ones: a clear reason each, nothing held */
    assert(!sl_epub_open(EP_ODD "/broken.epub", &err) && err == SL_EPUB_ERR_OPF);
    assert(!sl_epub_open(EP_ODD "/garbage.epub", &err) && err == SL_EPUB_ERR_ZIP);
    assert(!sl_epub_open(EP_ODD "/drm.epub", &err) && err == SL_EPUB_ERR_DRM);
    assert(!sl_epub_open(EP_ODD "/nothing-here.epub", &err) && err == SL_EPUB_ERR_OPEN);
    assert(sl_epub_mem_now() == m0);
    printf("PASS: e-books: EPUB 2 and 3 read: metadata, spine, contents, cover, chapters as paragraphs, a 2 MB chapter cut, broken ones refused\n");
}

static void ep_reader_tests(void)
{
    ep_clean_state();
    int st0 = test_status;
    test_status = 0; mini_update();           /* nothing playing: the page has the whole height */
    malloc_statistics_t m0, m1, m2;
    for (int i = 0; i < 10; i++) frame(100);
    malloc_zone_statistics(NULL, &m0);
    size_t e0 = sl_epub_mem_now();
    /* ---- opened: the first chapter's first page, on the paper of the theme */
    ep_open(EP_BOOKS "/latin.epub", ep_laid);
    assert(rd.book && rd.spine == 0 && rd.at == 0 && rd.npg > 2 && !rd.state);
    assert(find_label(rd.leaf[rd.cur], "The Quiet Harbour"));
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(rd.page, 0)) == (0xff000000 | rd_paper(rd.theme)) ||
           (lv_color_to_u32(lv_obj_get_style_bg_color(rd.page, 0)) & 0xffffff) == (rd_paper(rd.theme) & 0xf8fcf8));
    int npg0 = rd.npg;
    /* each page's words below its foot line, none cut by it */
    {
        lv_area_t f, l;
        lv_obj_get_coords(rd.foot, &f);
        lv_obj_t *leaf = rd.leaf[rd.cur];
        for (uint32_t i = 0; i < lv_obj_get_child_count(leaf); i++) { lv_obj_get_coords(lv_obj_get_child(leaf, i), &l); assert(l.y2 < f.y1); }
    }
    /* ---- turns: a tap at the right edge, at the left, a swipe, the side keys */
    ep_tap(W - 30, ep_mid_y()); assert(rd.at == 1);
    ep_tap(W / 2, ep_mid_y()); assert(rd.at == 2);    /* the middle turns on too */
    ep_tap(30 + 40, ep_mid_y()); assert(rd.at == 1);
    ep_swipe(W - 60, 100, ep_mid_y()); assert(rd.at == 2);
    ep_swipe(100, W - 60, ep_mid_y()); assert(rd.at == 1 && npages >= 2 && pages[npages - 1] == rd.page);   /* from mid screen: a turn, not back */
    handle_button(BUTTON_DOWN); handle_button(BUTTON_DOWN | BUTTON_REL); frame(400); assert(rd.at == 2);
    handle_button(BUTTON_UP); handle_button(BUTTON_UP | BUTTON_REL); frame(400); assert(rd.at == 1);
    /* every page the counting found is the page drawn */
    for (int k = 0; k + 1 < rd.npg; k++) { struct rd_pos e = rd_fill(rd.pg[k], NULL); assert(!rd_cmp(e, rd.pg[k + 1])); }
    /* ---- the last page, then on: the next chapter; back: the end of the first */
    rd_show(rd.npg - 1, 0); frame(100);
    ep_tap(W - 30, ep_mid_y()); ep_wait(ep_laid, 5000);
    assert(rd.spine == 1 && rd.at == 0);
    ep_tap(30 + 40, ep_mid_y()); ep_wait(ep_laid, 5000);
    assert(rd.spine == 0 && rd.at == npg0 - 1);
    /* ---- the contents: The Far Side is a heading inside the second chapter */
    rd_contents_page(); frame(400);
    assert(pages[npages - 1] == rd_toc_obj && h_text_ci(rd_toc_obj, "The Far Side"));
    h_click(h_row(rd_toc_obj, "The Far Side")); frame(400);
    ep_wait(ep_laid, 5000);
    assert(pages[npages - 1] == rd.page && rd.spine == 1);
    {
        int fb = sl_epub_anchor(rd.chap, sl_epub_hash("part-b"));
        assert(rd_cmp(rd.pg[rd.at], (struct rd_pos){ fb, 0 }) <= 0 && (rd.at + 1 == rd.npg || rd_cmp(rd.pg[rd.at + 1], (struct rd_pos){ fb, 0 }) > 0));
        assert(find_label(rd.leaf[rd.cur], "The Far Side"));
    }
    int at_toc = rd.at, sp_toc = rd.spine;
    /* ---- the sheet: a paper each way, the text size up then back */
    rd_sheet(); frame(400);
    assert(acts_open_now && rd.sw[RD_SEPIA]);
    h_click(rd.sw[RD_SEPIA]);
    assert(rd.theme == RD_SEPIA && (lv_color_to_u32(lv_obj_get_style_bg_color(rd.page, 0)) & 0xf8fcf8) == (rd_paper(RD_SEPIA) & 0xf8fcf8));
    assert(rd.at == at_toc);                  /* the same page, on the new paper */
    h_click(rd.sw[RD_DARK]); assert(rd.theme == RD_DARK);
    h_click(rd.sw[RD_LIGHT]); assert(rd.theme == RD_LIGHT);
    int size0 = rd.size, np0 = rd.npg;
    struct rd_pos first = rd.pg[rd.at];
    if (size0 < SL_TEXT_STEPS - 1)
    {
        h_click(rd.plus); ep_wait(ep_laid, 5000);
        assert(rd.size == size0 + 1 && rd.npg > np0);
        /* the page now holds where the words were */
        assert(rd_cmp(rd.pg[rd.at], first) <= 0 && (rd.at + 1 == rd.npg || rd_cmp(rd.pg[rd.at + 1], first) > 0));
        h_click(rd.minus); ep_wait(ep_laid, 5000);
        assert(rd.size == size0 && rd.npg == np0);
    }
    acts_close(); frame(400);
    /* ---- remembered: closed and opened again, then after a restart (the file read anew) */
    struct rd_pos keep = rd.pg[rd.at];
    page_pop(); frame(500);
    assert(!rd.page);
    ep_open(EP_BOOKS "/latin.epub", ep_laid);
    assert(rd.spine == sp_toc && !rd_cmp(rd.pg[rd.at], keep) && rd.theme == RD_LIGHT);
    page_pop(); frame(500);
    for (int i = 0; i < 200 && sl_epub_busy(); i++) { frame(20); usleep(5000); }
    assert(h_has(SL_EPUB_STATE "/reading.txt", "SLR1 0 ") && h_has(SL_EPUB_STATE "/reading.txt", "/Books/latin.epub"));
    sl_epub_forget();
    ep_open(EP_BOOKS "/latin.epub", ep_laid);
    assert(rd.spine == sp_toc && !rd_cmp(rd.pg[rd.at], keep) && rd.theme == RD_LIGHT);
    {
        struct sl_epub_pos p;
        assert(sl_epub_pos_get(EP_BOOKS "/latin.epub", &p) && p.permille > 300 && p.permille < 1000);
    }
    malloc_zone_statistics(NULL, &m1);
    page_pop(); frame(500);
    /* ---- Hebrew: from the right; a tap at the left edge is the next page, a swipe rightwards too */
    ep_open(EP_BOOKS "/hebrew.epub", ep_laid);
    assert(rd.rtl && rd.at == 0 && rd.npg > 1);
    ep_tap(30 + 40, ep_mid_y()); assert(rd.at == 1);
    ep_tap(W - 30, ep_mid_y()); assert(rd.at == 0);
    ep_swipe(100, W - 60, ep_mid_y()); assert(rd.at == 1);
    {
        /* its words as LVGL draws them: right to left, from the right edge */
        lv_obj_t *l = lv_obj_get_child(rd.leaf[rd.cur], 0);
        assert(l && lv_obj_check_type(l, &lv_label_class));
        assert(lv_bidi_detect_base_dir(lv_label_get_text(l)) == LV_BASE_DIR_RTL);
    }
    page_pop(); frame(500);
    /* ---- broken files: a clear message, not a crash */
    ep_open(EP_ODD "/broken.epub", ep_shown);
    assert(rd.state && find_label(rd.state, "This book can't be opened.") && find_label(rd.state, "Its list of contents is missing or damaged."));
    page_pop(); frame(500);
    ep_open(EP_ODD "/garbage.epub", ep_shown);
    assert(rd.state && h_text_ci(rd.state, "cut short"));
    page_pop(); frame(500);
    ep_open(EP_ODD "/drm.epub", ep_shown);
    assert(rd.state && h_text_ci(rd.state, "DRM"));
    page_pop(); frame(500);
    /* ---- a script the fonts lack: said so, and shown on request */
    ep_open(EP_ODD "/hindi.epub", ep_shown);
    assert(rd.state && h_text_ci(rd.state, "can't draw"));
    rd_anyway_cb(NULL); ep_wait(ep_laid, 5000);
    assert(rd.at == 0 && !rd.state);
    page_pop(); frame(500);
    /* ---- the 2 MB chapter: no frame waits long while it opens; its end says it was cut */
    {
        uint64_t worst = 0;
        rd_open(EP_BOOKS "/big.epub");
        uint64_t t0 = h_ms(), shown = 0;
        int frames = 0;
        for (int i = 0; i < 5000 && !ep_laid(); i++)
        {
            uint64_t t = h_ms(); frame(16); frames++;
            usleep(4000);                     /* the worker's time is real */
            uint64_t d = h_ms() - t - 4; if (d > worst) worst = d;
            if (!shown && rd.at >= 0) shown = h_ms() - t0;
        }
        assert(ep_laid());
        printf("e-books: a 2 MB chapter (cut at %d KB): its first page in %u ms, all %d pages counted in %u ms over %d frames, worst UI frame %u ms\n",
               SL_EPUB_XHTML_CAP / 1024, (unsigned)shown, rd.npg, (unsigned)(h_ms() - t0), frames, (unsigned)worst);
        assert(worst < 100);
        rd_show(rd.npg - 1, 0); frame(100);
        assert(find_label(rd.leaf[rd.cur], "The rest of this chapter is too long to show here."));
        page_pop(); frame(500);
    }
    /* ---- a paragraph of zero-width letters: one line that never wraps, longer than a page's words buffer */
    ep_open(EP_ODD "/zerowidth.epub", ep_shown);
    if (rd.state && h_text_ci(rd.state, "can't draw")) { rd_anyway_cb(NULL); ep_wait(ep_laid, 5000); }   /* letters no font has: shown anyway */
    if (!ep_laid()) printf("e-books: zero-width book: %s\n", rd.state ? "a message, not a page" : "not laid out");
    assert(ep_laid());
    page_pop(); frame(500);
    /* ---- memory: back where it was once the reader has closed */
    for (int i = 0; i < 200 && sl_epub_busy(); i++) { frame(20); usleep(5000); }
    for (int i = 0; i < 10; i++) frame(100);
    malloc_zone_statistics(NULL, &m2);
    long open_kb = ((long)m1.size_in_use - (long)m0.size_in_use) / 1024, after_kb = ((long)m2.size_in_use - (long)m0.size_in_use) / 1024;
    printf("e-books: memory: the reader open on a typical book %ld KB; %ld KB left after it closed; the module keeps %zu KB (where books were left)\n",
           open_kb, after_kb, (sl_epub_mem_now() - e0) / 1024);
    assert(sl_epub_mem_now() - e0 < 8 * 1024);
#if !__has_feature(address_sanitizer)
    assert(after_kb < 48);
#endif
    /* Chinese: the card's glyph packs draw it (across, not down) */
    ep_open(EP_BOOKS "/cjk.epub", ep_shown);
    assert(!rd.state && rd.at == 0 && rd.npg >= 1 && find_label(rd.leaf[rd.cur], "\xe7\xac\xac\xe4\xb8\x80\xe7\xab\xa0"));
    page_pop(); frame(500);   /* its pack stays loaded, as for a Chinese interface (sloppy_xfont.inc) */
    test_status = st0; mini_update();
    printf("PASS: e-books: the reader turns by tap, swipe and keys, both ways in Hebrew; contents jump; three papers, sizes; place kept across a restart; broken books say why\n");
}

static void ep_shelf_tests(void)
{
    ep_clean_state();
    /* ---- Music > Books: the shelf, found on the worker, covers made */
    action(ACT_EBOOKS); frame(300);
    assert(pages[npages - 1] == bs_page_obj);
    ep_wait(ep_shelf_done, 8000);
    frame(600);
    assert(bs_n == 5);
    int lat = -1;
    for (int k = 0; k < bs_n; k++) if (strstr(bs_books[k].path, "latin")) lat = k;
    assert(lat >= 0 && bs_books[lat].cover && !strcmp(bs_books[lat].title, "The Quiet Harbour"));
    char rb[96]; struct stat st;
    sl_epub_cover_name(rb, sizeof rb, bs_books[lat].hash);
    assert(h_file(rb, &st) && st.st_size == SL_EPUB_COVER_W * SL_EPUB_COVER_H * 2 + 12);
    assert(h_text_ci(bs_body, "New") && !h_text_ci(bs_body, "Continue reading"));
    /* ---- read a little, and the shelf has it to continue */
    h_click(lv_obj_get_parent(h_label_ci(bs_body, "The Quiet Harbour")));
    ep_wait(ep_laid, 5000);
    assert(rd.page && !strcmp(rd.path, EP_BOOKS "/latin.epub"));
    ep_tap(W - 30, ep_mid_y()); ep_tap(W - 30, ep_mid_y());
    page_pop(); frame(600);
    assert(pages[npages - 1] == bs_page_obj && h_text_ci(bs_body, "Continue reading"));
    /* a second look: nothing opened again (the books are known by size and time) */
    sl_epub_shelf_scan(); ep_wait(ep_shelf_done, 5000); frame(400);
    assert(bs_n == 5);
    /* ---- Files: an .epub is a book, and opens in the reader */
    assert(fm_kind("Story.EPUB", false) == FK_BOOK);
    page_pop(); frame(400);
    {
        /* the sandboxed card lists it; the worker reads the real file at the same place */
        sim_mkdir(FAKE_CARD "/EBooks");
        fake_put(FAKE_CARD "/EBooks/latin.epub", "PK", 2);
        assert(system("mkdir -p '" H_ROOT FAKE_CARD "/EBooks' && cp '" H_ROOT EP_BOOKS "/latin.epub' '" H_ROOT FAKE_CARD "/EBooks/'") == 0);
        sl_files_page(FAKE_CARD "/EBooks", 0); frame(400);
        struct fm_view *v = fm_find(FAKE_CARD "/EBooks");
        assert(v);
        int k = -1;
        for (int i = 0; i < v->n; i++) if (!strcmp(fm_name(v, i), "latin.epub")) k = i;
        assert(k >= 0 && v->e[k].kind == FK_BOOK);
        fm_open_entry(v, k); frame(300);
        ep_wait(ep_laid, 5000);
        assert(rd.page && !strcmp(rd.path, FAKE_CARD "/EBooks/latin.epub") && rd.book && !strcmp(rd.book->title, "The Quiet Harbour"));
        while (npages > 1) { page_pop(); frame(300); }
        sim_remove(FAKE_CARD "/EBooks/latin.epub");
        sim_rmdir(FAKE_CARD "/EBooks");
        assert(fake_find(FAKE_CARD "/EBooks") < 0);
        assert(system("rm -rf '" H_ROOT FAKE_CARD "/EBooks'") == 0);
    }
    printf("PASS: e-books: the shelf finds the books, makes their covers, offers the one being read; Files opens an .epub in the reader\n");
    ep_clean_state();
}
static void ep_tests(void)
{
    ep_fixtures();
    ep_parse_tests();
    ep_reader_tests();
    ep_shelf_tests();
}

/* ---- audit screens */
static void ep_au(int which)
{
    ep_fixtures();
    while (npages > 1) { page_pop(); frame(300); }
    switch (which)
    {
        case 0:   /* the shelf with books, one being read */
        {
            struct sl_epub_pos p = { 1, 3, 0, 420, (uint32_t)time(NULL) + 100000 };   /* read last */
            sl_epub_pos_set(EP_BOOKS "/latin.epub", &p);
            ebooks_page(); ep_wait(ep_shelf_done, 8000); frame(400);
            bs_refresh();
            break;
        }
        case 1:   /* no books */
            assert(system("mkdir -p '" H_ROOT "/EpubAside' && mv '" H_ROOT EP_BOOKS "'/*.epub '" H_ROOT "/EpubAside/'") == 0);
            sl_epub_forget();
            assert(system("rm -f '" H_ROOT SL_EPUB_STATE "/shelf.txt'") == 0);
            ebooks_page(); ep_wait(ep_shelf_done, 8000); frame(400);
            bs_refresh();
            assert(system("mv '" H_ROOT "/EpubAside/'*.epub '" H_ROOT EP_BOOKS "/' && rmdir '" H_ROOT "/EpubAside'") == 0);
            sl_epub_forget();
            break;
        case 2: case 3: case 4:   /* the reader on each paper */
        {
            int th, sz;
            sl_epub_prefs(&th, &sz);
            sl_epub_prefs_set(which - 2, -1);
            struct sl_epub_pos p = { 1, 0, 0, 380, (uint32_t)time(NULL) };
            sl_epub_pos_set(EP_BOOKS "/latin.epub", &p);
            ep_open(EP_BOOKS "/latin.epub", ep_laid);
            break;
        }
        case 5:   /* the contents */
            ep_open(EP_BOOKS "/latin.epub", ep_laid);
            rd_contents_page();
            break;
        case 6:   /* the sheet */
            ep_open(EP_BOOKS "/latin.epub", ep_laid);
            rd_sheet();
            break;
        case 7:   /* a broken book */
            ep_open(EP_ODD "/broken.epub", ep_shown);
            break;
        case 8:   /* a Hebrew book, from the right */
            sl_epub_prefs_set(-1, -1);
            ep_open(EP_BOOKS "/hebrew.epub", ep_laid);
            break;
        case 9:   /* a script the fonts lack */
            ep_open(EP_ODD "/hindi.epub", ep_shown);
            break;
        case 10:  /* the 2 MB chapter at its cut */
            ep_open(EP_BOOKS "/big.epub", ep_laid);
            rd_show(rd.npg - 1, 0);
            break;
    }
}
static void ep_au_reset(void)
{
    if (acts_open_now) { acts_close(); frame(300); }
    if (rd.page || bs_page_obj) { while (npages > 1) { page_pop(); frame(300); } }
}
