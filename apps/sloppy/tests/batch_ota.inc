/* P69 (#36), option A: software updates from the card and from the phone
 * remote (sloppy_ota.c, sloppy_update.inc, sloppy_net.c's POST /update,
 * sloppy-dev/ota/make-update.py). Included from ui_test.c after
 * batch_remote.inc, whose rmt_get it uses.
 *   ot_tests()             in the suite
 *   SLOPPY_OTA_TESTS=1     the tests alone
 *   audit screens update-*, shot with
 *   SLOPPY_CENTER_SHOTS=<dir> SLOPPY_AUDIT_ONLY=update ./ui-test
 *
 * The signatures are real Ed25519 through OpenSSL 1.1.1 loaded with dlopen,
 * as the player loads /usr/lib/libcrypto.so.1.1: the copy DaVinci Resolve
 * keeps on this Mac (SLOPPY_OTA_CRYPTO=<path> for another). The key pair
 * is made here, at random, for this run only; no key is kept anywhere. */
#include <dlfcn.h>
#define OT_LIB_MAC "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/libcrypto.1.1.dylib"
#define OT_W SIM_TMP "/sloppy-ota-work"
#define OT_TOOL "sloppyos/sloppy-dev/ota/make-update.py"
static struct {
    void *(*priv)(int, void *, const unsigned char *, size_t);
    int (*getpub)(const void *, unsigned char *, size_t *);
    void *(*ctx_new)(void);
    void (*ctx_free)(void *);
    int (*ds_init)(void *, void **, const void *, void *, void *);
    int (*ds)(void *, unsigned char *, size_t *, const unsigned char *, size_t);
    void *pkey;
    const char *lib;
} ots;
static unsigned char ot_pub[32];
static const char *ot_lib(void) { return getenv("SLOPPY_OTA_CRYPTO") ? getenv("SLOPPY_OTA_CRYPTO") : OT_LIB_MAC; }
/* a fresh key pair, the player told its public half */
static void ot_keys(void)
{
    if (!ots.pkey)
    {
        void *c = dlopen(ot_lib(), RTLD_NOW);
        if (!c) { printf("ota: no OpenSSL 1.1.1 at %s (%s): set SLOPPY_OTA_CRYPTO\n", ot_lib(), dlerror()); assert(c); }
        *(void **)&ots.priv = dlsym(c, "EVP_PKEY_new_raw_private_key");
        *(void **)&ots.getpub = dlsym(c, "EVP_PKEY_get_raw_public_key");
        *(void **)&ots.ctx_new = dlsym(c, "EVP_MD_CTX_new");
        *(void **)&ots.ctx_free = dlsym(c, "EVP_MD_CTX_free");
        *(void **)&ots.ds_init = dlsym(c, "EVP_DigestSignInit");
        *(void **)&ots.ds = dlsym(c, "EVP_DigestSign");
        assert(ots.priv && ots.getpub && ots.ctx_new && ots.ctx_free && ots.ds_init && ots.ds);
        unsigned char seed[32];
        arc4random_buf(seed, sizeof seed);
        ots.pkey = ots.priv(1087, NULL, seed, 32);
        size_t n = 32;
        assert(ots.pkey && ots.getpub(ots.pkey, ot_pub, &n) == 1 && n == 32);
        const char *(*ver)(int) = (const char *(*)(int))dlsym(c, "OpenSSL_version");
        printf("ota: signing and checking with %s (%s), a key pair made for this run\n", ver ? ver(0) : "?", ot_lib());
    }
    memcpy(sl_ota_test_key, ot_pub, 32);
    sl_ota_test_key_set = true;
    sl_ota_crypto_lib = ot_lib();
}
static void ot_sign(const void *m, size_t n, unsigned char sig[64])
{
    void *ctx = ots.ctx_new();
    size_t sn = 64;
    assert(ctx && ots.ds_init(ctx, NULL, NULL, NULL, ots.pkey) == 1 && ots.ds(ctx, sig, &sn, m, n) == 1 && sn == 64);
    ots.ctx_free(ctx);
}

/* ---- files */
static void ot_path(char *b, size_t n, const char *rel) { snprintf(b, n, "%s/%s", sim_root_dir, rel); }
static void ot_sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ot_sh(const char *fmt, ...)
{
    char c[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c, sizeof c, fmt, ap);
    va_end(ap);
    int rc = system(c);
    if (rc) printf("ota: \"%s\" said %d\n", c, rc);
    assert(rc == 0);
}
static void ot_put(const char *full, const void *b, size_t n)
{
    char d[700];
    snprintf(d, sizeof d, "%s", full);
    char *slash = strrchr(d, '/');
    *slash = '\0';
    struct stat st;
    if (stat(d, &st)) ot_sh("mkdir -p '%s'", d);
    FILE *f = fopen(full, "wb");
    assert(f && fwrite(b, 1, n, f) == n && !fclose(f));
}
static char *ot_slurp(const char *full, size_t *n)
{
    FILE *f = fopen(full, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)l + 1);
    assert(b && fread(b, 1, (size_t)l, f) == (size_t)l);
    fclose(f);
    b[l] = '\0';
    if (n) *n = (size_t)l;
    return b;
}
static bool ot_is(const char *rel, const void *want, size_t n)
{
    char p[600];
    size_t got;
    ot_path(p, sizeof p, rel);
    char *b = ot_slurp(p, &got);
    bool same = b && got == n && !memcmp(b, want, n);
    free(b);
    return same;
}
static bool ot_there(const char *rel) { char p[600]; struct stat st; ot_path(p, sizeof p, rel); return !stat(p, &st); }

/* the two releases: 1.0 on the card, 1.1 coming. The program is 300 KB of
 * words that repeat at a distance, so inflate's window is used. */
#define OT_R1_OLD 260000
#define OT_R1_NEW 300000
static char *ot_r1_old, *ot_r1_new;
static const char ot_a_old[] = "codec a, 1.0", ot_a_new[] = "codec a, 1.1, a little longer", ot_gone[] = "a codec 1.1 drops",
                  ot_same[] = "the same in both", ot_rock[] = "a plugin 1.1 brings";
static const char *const ot_user[] = { ".rockbox/sloppy/ota_test_save.dat", "Playlists/ota_test.m3u", ".rockbox/ota_test_playlist_control" };
static char *ot_cfg0;                    /* config.cfg as it was, if there was one */
static size_t ot_cfg0_n;
static void ot_fill(char *b, size_t n, unsigned seed)
{
    /* 1 KB pieces: noise, or a piece from up to 24 KB back (inflate's window
     * at work), so the zip is a few hundred KB, read in many pieces */
    uint32_t x = seed * 2654435761u + 1;
    for (size_t at = 0; at < n; at += 1024)
    {
        size_t l = n - at < 1024 ? n - at : 1024;
        x = x * 1664525u + 1013904223u;
        if (at >= 24 * 1024 && (x >> 28) < 6) memcpy(b + at, b + at - 1024 * (1 + (x >> 8) % 24), l);
        else for (size_t i = 0; i < l; i++) { x = x * 1664525u + 1013904223u; b[at + i] = (char)(x >> 24); }
    }
}
/* the card as 1.0 left it, and the owner's files */
static void ot_card_old(void)
{
    char p[600];
    ot_path(p, sizeof p, ".rockbox/rockbox.r1"); ot_put(p, ot_r1_old, OT_R1_OLD);
    ot_path(p, sizeof p, ".rockbox/codecs/ota_a.codec"); ot_put(p, ot_a_old, sizeof ot_a_old);
    ot_path(p, sizeof p, ".rockbox/codecs/ota_gone.codec"); ot_put(p, ot_gone, sizeof ot_gone);
    ot_path(p, sizeof p, ".rockbox/langs/ota_same.lng"); ot_put(p, ot_same, sizeof ot_same);
    ot_path(p, sizeof p, ".rockbox/rocks/ota_new.rock"); unlink(p);
    for (unsigned i = 0; i < ARRAYLEN(ot_user); i++) { ot_path(p, sizeof p, ot_user[i]); ot_put(p, ot_user[i], strlen(ot_user[i])); }
    ot_sh("rm -rf '%s/.rockbox/update' '%s/" SL_OTA_UNDO "'", sim_root_dir, sim_root_dir);
    snprintf(sl_ota_running, sizeof sl_ota_running, "1.0");   /* the old build is the one running, whatever this build is */
}
static bool ot_set_old(void)
{
    return ot_is(".rockbox/rockbox.r1", ot_r1_old, OT_R1_OLD) && ot_is(".rockbox/codecs/ota_a.codec", ot_a_old, sizeof ot_a_old) &&
           ot_is(".rockbox/codecs/ota_gone.codec", ot_gone, sizeof ot_gone) && ot_is(".rockbox/langs/ota_same.lng", ot_same, sizeof ot_same) &&
           !ot_there(".rockbox/rocks/ota_new.rock");
}
static bool ot_set_new(void)
{
    return ot_is(".rockbox/rockbox.r1", ot_r1_new, OT_R1_NEW) && ot_is(".rockbox/codecs/ota_a.codec", ot_a_new, sizeof ot_a_new) &&
           !ot_there(".rockbox/codecs/ota_gone.codec") && ot_is(".rockbox/langs/ota_same.lng", ot_same, sizeof ot_same) &&
           ot_is(".rockbox/rocks/ota_new.rock", ot_rock, sizeof ot_rock);
}
static void ot_user_untouched(void)
{
    for (unsigned i = 0; i < ARRAYLEN(ot_user); i++) assert(ot_is(ot_user[i], ot_user[i], strlen(ot_user[i])));
    char p[600];
    size_t n;
    ot_path(p, sizeof p, ".rockbox/config.cfg");
    char *now = ot_slurp(p, &n);
    assert(!ot_cfg0 == !now && (!now || (n == ot_cfg0_n && !memcmp(now, ot_cfg0, n))));
    free(now);
}

/* the releases as zips, the update as a release makes it */
static char *ot_man, *ot_zip;             /* the 1.1 update's manifest and zip */
static size_t ot_man_n, ot_zip_n;
static void ot_release(const char *dir, bool new)
{
    char p[600];
    ot_sh("rm -rf '%s' && mkdir -p '%s'", dir, dir);
    snprintf(p, sizeof p, "%s/.rockbox/rockbox.r1", dir); ot_put(p, new ? ot_r1_new : ot_r1_old, new ? OT_R1_NEW : OT_R1_OLD);
    snprintf(p, sizeof p, "%s/.rockbox/codecs/ota_a.codec", dir); ot_put(p, new ? ot_a_new : ot_a_old, new ? sizeof ot_a_new : sizeof ot_a_old);
    snprintf(p, sizeof p, "%s/.rockbox/langs/ota_same.lng", dir); ot_put(p, ot_same, sizeof ot_same);
    if (new) { snprintf(p, sizeof p, "%s/.rockbox/rocks/ota_new.rock", dir); ot_put(p, ot_rock, sizeof ot_rock); }
    else { snprintf(p, sizeof p, "%s/.rockbox/codecs/ota_gone.codec", dir); ot_put(p, ot_gone, sizeof ot_gone); }
    /* what a release also carries and an update must leave */
    snprintf(p, sizeof p, "%s/.rockbox/sloppy/gabagool.wav", dir); ot_put(p, "RIFF", 4);
    snprintf(p, sizeof p, "%s/Games/Put mario.gbc here.txt", dir); ot_put(p, "x", 1);
    ot_sh("cd '%s' && rm -f ../%s.zip && COPYFILE_DISABLE=1 zip -qrX ../%s.zip .rockbox Games", dir, new ? "new" : "old", new ? "new" : "old");
}
static void ot_make(void)
{
    if (ot_man) return;
    ot_r1_old = malloc(OT_R1_OLD); ot_r1_new = malloc(OT_R1_NEW);
    ot_fill(ot_r1_old, OT_R1_OLD, 1); ot_fill(ot_r1_new, OT_R1_NEW, 2);
    ot_release(OT_W "/old", false);
    ot_release(OT_W "/new", true);
    FILE *f = fopen(OT_W "/notes.json", "w");
    fputs("{\"en\":\"Game on then: Sudoku hints and a faster All list.\",\"he\":\"\\u05de\\u05e9\\u05d7\\u05e7\\u05d9\\u05dd \\u05d7\\u05d3\\u05e9\\u05d9\\u05dd.\"}", f);
    fclose(f);
    ot_sh("python3 -I %s build --zip " OT_W "/new.zip --from " OT_W "/old.zip --version 1.1 --min-from 1.0 --build-date 2026-10-29 "
          "--notes " OT_W "/notes.json -o " OT_W "/u11 > /dev/null", OT_TOOL);
    ot_man = ot_slurp(OT_W "/u11.manifest.json", &ot_man_n);
    ot_zip = ot_slurp(OT_W "/u11.payload.zip", &ot_zip_n);
    assert(ot_man && ot_zip);
    assert(!strstr(ot_man, "sloppy/") && !strstr(ot_man, "Games/") && !strstr(ot_man, "ota_same"));
    assert(strstr(ot_man, "\"remove\":[\".rockbox/codecs/ota_gone.codec\"]"));
    /* signed here, packed by the tool */
    unsigned char sig[64];
    ot_sign(ot_man, ot_man_n, sig);
    ot_put(OT_W "/u11.manifest.sig", sig, 64);
    ot_sh("python3 -I %s pack " OT_W "/u11 > /dev/null", OT_TOOL);
}
static char *ot_sub(const char *s, const char *a, const char *b)   /* s with the first a made b */
{
    const char *at = strstr(s, a);
    assert(at);
    size_t n = strlen(s) - strlen(a) + strlen(b);
    char *o = malloc(n + 1);
    memcpy(o, s, (size_t)(at - s));
    strcpy(o + (at - s), b);
    strcat(o, at + strlen(a));
    return o;
}
/* an update file at the card's top, from a manifest; flags: 1 a bad
 * signature, 2 cut short, 4 a byte of the zip changed */
static void ot_drop(const char *name, const char *man, int flags)
{
    char p[600];
    ot_sh("rm -f '%s'/*.gbupdate", sim_root_dir);
    size_t mn = strlen(man), n = 20 + mn + 64 + ot_zip_n;
    unsigned char *b = malloc(n);
    memcpy(b, "GBUPDATE\x01\0\0\0", 12);
    for (int i = 0; i < 4; i++) { b[12 + i] = (unsigned char)(mn >> 8 * i); b[16 + i] = (unsigned char)(64 >> 8 * i); }
    memcpy(b + 20, man, mn);
    ot_sign(man, mn, b + 20 + mn);
    if (flags & 1) b[20 + mn + 5] ^= 0x40;
    memcpy(b + 20 + mn + 64, ot_zip, ot_zip_n);
    if (flags & 4) b[20 + mn + 64 + 40] ^= 0x01;
    ot_path(p, sizeof p, name);
    ot_put(p, b, flags & 2 ? n - 300 : n);
    free(b);
}
static void ot_wait(void)
{
    for (uint64_t t0 = h_ms(); h_ms() - t0 < 20000; )
    {
        struct sl_ota_status s;
        sl_ota_status(&s);
        if (!sl_ota_busy() && s.state != SL_OTA_LOOKING && s.state != SL_OTA_WORKING) return;
        frame(20);
        usleep(2000);
    }
    assert(!"the update's worker never settled");
}
static struct sl_ota_status ot_scan(void)
{
    struct sl_ota_status s;
    sl_ota_scan();
    usleep(1000);
    ot_wait();
    sl_ota_status(&s);
    return s;
}
static struct sl_ota_status ot_install(void)
{
    struct sl_ota_status s;
    assert(sl_ota_install());
    ot_wait();
    sl_ota_status(&s);
    return s;
}
/* a start: the build on the card's rockbox.r1 is the one the boot menu runs */
static struct sl_ota_status ot_restart(void)
{
    struct sl_ota_status s;
    sl_ota_test_reset();
    ot_keys();
    snprintf(sl_ota_running, sizeof sl_ota_running, "%s", ot_is(".rockbox/rockbox.r1", ot_r1_new, OT_R1_NEW) ? "1.1" : "1.0");
    sl_ota_cut_after = 0;
    sl_ota_boot();
    sl_ota_status(&s);
    return s;
}
static char *ot_journal(void) { char p[600]; ot_path(p, sizeof p, ".rockbox/update/journal"); return ot_slurp(p, NULL); }
static void ot_stage_good(void)
{
    ot_card_old();
    snprintf(sl_ota_running, sizeof sl_ota_running, "1.0");
    sl_ota_test_reset();
    ot_keys();
    ot_drop("GabagoolOS 1.1.gbupdate", ot_man, 0);
    struct sl_ota_status s = ot_scan();
    assert(s.state == SL_OTA_FOUND);
    s = ot_install();
    assert(s.state == SL_OTA_READY);
}

/* ---- the phone remote's upload */
static int ot_post(const void *b, size_t n, size_t send_n, char *answer, size_t an)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(SL_REMOTE_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);   /* a refusal closes early: the rest of the body is not a crash */
    assert(s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof a));
    char h[256];
    int hn = snprintf(h, sizeof h, "POST /update HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\n%s\r\n", n, rm_cookie);
    send(s, h, (size_t)hn, 0);
    size_t at = 0;
    int got = 0;
    while (at < send_n)
    {
        ssize_t w = send(s, (const char *)b + at, send_n - at > 8192 ? 8192 : send_n - at, MSG_DONTWAIT);
        if (w > 0) at += (size_t)w;
        else if (errno != EAGAIN) break;
        frame(5);
        usleep(500);
    }
    if (send_n < n) { close(s); return 0; }   /* the phone went away part way */
    for (uint64_t until = h_ms() + 8000; h_ms() < until && got < (int)an - 1; )
    {
        ssize_t r = recv(s, answer + got, an - 1 - (size_t)got, MSG_DONTWAIT);
        if (r > 0) { got += (int)r; continue; }
        if (r == 0) break;
        frame(5);
        usleep(1000);
    }
    answer[got] = '\0';
    close(s);
    return atoi(answer + 9);
}

static void ot_tests(void)
{
    struct sl_ota_status s;
    char p[600], ans[512];
    ot_sh("rm -rf " OT_W " && mkdir -p " OT_W " '%s/.rockbox' '%s/Playlists'", sim_root_dir, sim_root_dir);
    ot_path(p, sizeof p, ".rockbox/config.cfg");
    ot_cfg0 = ot_slurp(p, &ot_cfg0_n);
    ot_keys();
    ot_make();
    printf("ota: the update: manifest %zu bytes, zip %zu bytes\n", ot_man_n, ot_zip_n);

    /* ---- nothing at the top: nothing found */
    ot_card_old();
    ot_sh("rm -f '%s'/*.gbupdate", sim_root_dir);
    s = ot_scan();
    assert(s.state == SL_OTA_NONE && s.key && s.crypto);

    /* ---- the build with no key refuses everything, and says so */
    sl_ota_test_key_set = false;
    ot_drop("GabagoolOS 1.1.gbupdate", ot_man, 0);
    s = ot_scan();
    assert(!s.key && s.state == SL_OTA_REJECTED && s.why == SL_OTA_E_NOKEY);
    assert(!sl_ota_install());
    update_page(); frame(300);
    assert(has_text(lv_screen_active(), sl_tr("This build can't check updates")));
    page_pop(); frame(300);
    ot_keys();
    printf("PASS: ota: a build with no release key checks nothing and installs nothing.\n");

    /* ---- the player's OpenSSL without Ed25519 (or none): nothing installed */
    sl_ota_test_reset();
    sl_ota_crypto_lib = "/nonexistent/libcrypto.so.1.1";
    s = ot_scan();
    assert(s.state == SL_OTA_REJECTED && s.why == SL_OTA_E_NOCRYPTO && !s.crypto);
    sl_ota_test_reset();
    ot_keys();
    printf("PASS: ota: no OpenSSL with Ed25519: refused, the page says so.\n");

    /* ---- the broken ones: each rejected, nothing on the card changed */
    {
        struct { const char *what; char *man; int flags, why; bool at_install; } bad[] = {
            { "a bad signature", ot_man, 1, SL_OTA_E_NOTOURS, false },
            { "an older version", ot_sub(ot_man, "\"version\":\"1.1\"", "\"version\":\"0.9\""), 0, SL_OTA_E_OLDER, false },
            { "min_from too high", ot_sub(ot_man, "\"min_from\":\"1.0\"", "\"min_from\":\"1.5\""), 0, SL_OTA_E_MINFROM, false },
            { "a file's hash wrong", NULL, 0, SL_OTA_E_DAMAGED, true },
            { "the file cut short", ot_man, 2, SL_OTA_E_DAMAGED, false },
            { "a byte of the zip changed", ot_man, 4, SL_OTA_E_DAMAGED, true },
            { "a file outside .rockbox", ot_sub(ot_man, "\".rockbox/rocks/ota_new.rock\"", "\"Music/ota_new.rock\""), 0, SL_OTA_E_PATH, false },
            { "a climb out with ..", ot_sub(ot_man, "\".rockbox/rocks/ota_new.rock\"", "\".rockbox/../ota_new.rock\""), 0, SL_OTA_E_PATH, false },
            { "the owner's saves", ot_sub(ot_man, "\".rockbox/rocks/ota_new.rock\"", "\".rockbox/sloppy/pet.dat\""), 0, SL_OTA_E_PATH, false },
            { "config.cfg", ot_sub(ot_man, "\".rockbox/rocks/ota_new.rock\"", "\".rockbox/config.cfg\""), 0, SL_OTA_E_PATH, false },
            { "a remove outside", ot_sub(ot_man, "\"remove\":[\".rockbox/codecs/ota_gone.codec\"]", "\"remove\":[\"Music/song.flac\"]"), 0, SL_OTA_E_PATH, false },
            { "not a manifest", ot_sub(ot_man, "\"product\":\"GabagoolOS\"", "\"product\":\"SonixOS\""), 0, SL_OTA_E_NOTOURS, false },
        };
        /* a file's hash wrong: the first file's first hex digit */
        {
            char *at = strstr(strstr(ot_man, "\"files\""), "\"sha256\":\"") + 10;
            bad[3].man = strdup(ot_man);
            bad[3].man[at - ot_man] = *at == '0' ? '1' : '0';
        }
        for (unsigned i = 0; i < ARRAYLEN(bad); i++)
        {
            ot_card_old();
            ot_drop("GabagoolOS 1.1.gbupdate", bad[i].man, bad[i].flags);
            s = ot_scan();
            if (bad[i].at_install) { assert(s.state == SL_OTA_FOUND); s = ot_install(); }
            if (s.state != SL_OTA_REJECTED || s.why != bad[i].why) printf("ota: %s: state %d why %d\n", bad[i].what, s.state, s.why);
            assert(s.state == SL_OTA_REJECTED && s.why == bad[i].why);
            assert(ot_set_old() && !ot_there(".rockbox/update/stage/plan") && !ot_there(".rockbox/update/journal"));
            ot_user_untouched();
            if (bad[i].man != ot_man) free(bad[i].man);
        }
        /* the words for each, in the page */
        update_page(); frame(300);
        assert(has_text(lv_screen_active(), sl_tr("This file isn't a GabagoolOS update")));
        page_pop(); frame(300);
    }
    printf("PASS: ota: a bad signature, an older version, min_from too high, a wrong hash, a cut file, a changed zip and "
           "files outside .rockbox are each refused; the card is as it was.\n");

    /* ---- found: what it is, its notes in the player's language */
    ot_card_old();
    ot_drop("GabagoolOS 1.1.gbupdate", ot_man, 0);
    sl_ota_lang("en");
    s = ot_scan();
    assert(s.state == SL_OTA_FOUND && !strcmp(s.version, "1.1") && !strcmp(s.min_from, "1.0") && !strcmp(s.file, "GabagoolOS 1.1.gbupdate"));
    assert(strstr(s.notes, "Sudoku hints") && s.files == 3 && s.size == 20 + ot_man_n + 64 + ot_zip_n);
    sl_ota_lang("he");
    s = ot_scan();
    assert(!strncmp(s.notes, "\xd7\x9e\xd7\xa9\xd7\x97\xd7\xa7", 8));
    sl_ota_lang("fr");                    /* none in French: English */
    s = ot_scan();
    assert(strstr(s.notes, "Sudoku hints"));
    sl_ota_lang("en");
    /* the same version as running is the one installed: quiet */
    snprintf(sl_ota_running, sizeof sl_ota_running, "1.1");
    s = ot_scan();
    assert(s.state == SL_OTA_NONE);
    snprintf(sl_ota_running, sizeof sl_ota_running, "1.0");
    printf("PASS: ota: an update at the card's top is found, with its version, size and notes in the player's language.\n");

    /* ---- not enough space: the MB needed, nothing written */
    s = ot_scan();
    sl_ota_free_fake = 100 * 1024;
    s = ot_install();
    assert(s.state == SL_OTA_REJECTED && s.why == SL_OTA_E_SPACE && s.need > (2u << 20));
    assert(!ot_there(".rockbox/update/stage") && ot_set_old());
    sl_ota_free_fake = -1;
    printf("PASS: ota: not enough space: refused before anything is written, with how much more.\n");

    /* ---- USB mode part way through: stopped, nothing half-staged, back after */
    s = ot_scan();
    assert(s.state == SL_OTA_FOUND);
    sl_ota_slow_ms = 40;
    assert(sl_ota_install());
    for (uint64_t t0 = h_ms(); h_ms() - t0 < 5000; usleep(2000)) { sl_ota_status(&s); if (s.state == SL_OTA_WORKING && s.done) break; }
    assert(s.state == SL_OTA_WORKING);
    sl_ota_hold(true);
    ot_wait();
    sl_ota_status(&s);
    assert(s.state == SL_OTA_REJECTED && s.why == SL_OTA_E_USB && !ot_there(".rockbox/update/stage/plan") && ot_set_old());
    assert(!sl_ota_install());
    sl_ota_slow_ms = 0;
    sl_ota_hold(false);
    ot_wait();
    sl_ota_status(&s);
    assert(s.state == SL_OTA_FOUND);
    printf("PASS: ota: USB mode stops a check part way; nothing half-staged stays, and the update is found again after.\n");

    /* ---- installed from the card: checked, staged, and the memory given back */
    {
        malloc_statistics_t m0, m1;
        malloc_zone_statistics(NULL, &m0);
        s = ot_install();
        assert(s.state == SL_OTA_READY && s.changed == 3);
        malloc_zone_statistics(NULL, &m1);
        long after = ((long)m1.size_in_use - (long)m0.size_in_use) / 1024;
        printf("ota: memory: the engine's state %u KB always; %ld KB left after a check and stage of %zu KB\n",
               (unsigned)(sl_ota_test_static_bytes() / 1024), after, ot_zip_n / 1024);
#if !__has_feature(address_sanitizer)
        assert(after < 16);
#endif
    }
    assert(ot_set_old());                 /* staged, not swapped: nothing changed yet */
    assert(ot_is(".rockbox/update/stage/.rockbox/rockbox.r1", ot_r1_new, OT_R1_NEW) && !ot_there(".rockbox/update/stage/.rockbox/langs/ota_same.lng"));
    s = ot_restart();                     /* a power off before Restart: still ready */
    s = ot_scan();
    assert(s.state == SL_OTA_READY && !strcmp(s.version, "1.1"));
    update_page(); frame(300);
    assert(has_text(lv_screen_active(), sl_tr("Verified")) && has_text(lv_screen_active(), sl_tr("Restart to update")));
    page_pop(); frame(300);
    /* the no-key build will not swap a staged one either */
    sl_ota_test_key_set = false;
    assert(sl_ota_swap() == SL_OTA_E_NOKEY && ot_set_old());
    ot_keys();
    printf("PASS: ota: checked and staged: only the files that differ, nothing in place yet, ready after a restart too.\n");

    /* ---- the swap, and the start after it */
    assert(sl_ota_swap() == 0);
    assert(ot_set_new() && ot_there(SL_OTA_UNDO) && ot_is(".rockbox/update/prev/.rockbox/rockbox.r1", ot_r1_old, OT_R1_OLD));
    assert(ot_is(".rockbox/update/prev/.rockbox/codecs/ota_gone.codec", ot_gone, sizeof ot_gone));
    ot_user_untouched();
    s = ot_restart();
    assert(!strcmp(sl_ota_running, "1.1") && s.trial && !strcmp(s.updated_to, "1.1") && s.prev && !strcmp(s.prev_version, "1.0") && !s.restart);
    up_since_ms = 0;
    {
        int on0 = global_settings.sloppy_notifications;
        global_settings.sloppy_notifications = 1;
        notes_load();
        up_poll();
        assert(e_note_has("Updated to GabagoolOS 1.1"));
        global_settings.sloppy_notifications = on0;
        pop_hide_now();
    }
    sl_ota_status(&s);
    assert(!s.updated_to[0]);
    sl_ota_healthy();
    { char *j = ot_journal(); assert(j && strstr(j, "S committed\n")); free(j); }
    assert(!ot_there(SL_OTA_UNDO) && !ot_there("GabagoolOS 1.1.gbupdate"));
    sl_ota_status(&s);
    assert(!s.trial && s.prev);
    s = ot_scan();
    assert(s.state == SL_OTA_NONE && strstr(s.notes, "Sudoku"));   /* What's new, of what is installed */
    printf("PASS: ota: Restart to update swaps it in, the program last; the next start is on 1.1, says so once, keeps it after it is healthy.\n");

    /* ---- Previous version: 1.0 back, the owner's files as they were */
    assert(sl_ota_revert() == 0);
    assert(ot_set_old() && !ot_there(".rockbox/update/journal") && !ot_there(".rockbox/update/prev/.rockbox/rockbox.r1"));
    ot_user_untouched();
    s = ot_restart();
    assert(!strcmp(sl_ota_running, "1.0") && !s.prev && !s.trial && !s.restart);
    printf("PASS: ota: Previous version puts 1.0 back whole.\n");

    /* ---- a power cut at every step of the swap: the next start finishes it */
    int steps = 0;
    for (int k = 1; k < 60; k++)
    {
        ot_stage_good();
        sl_ota_cut_after = k;
        int r = sl_ota_swap();
        sl_ota_cut_after = 0;
        if (r == 0) { steps = k - 1; break; }
        assert(r == SL_OTA_E_STOPPED);
        bool began = ot_there(".rockbox/update/journal");
        s = ot_restart();
        char *j = ot_journal();
        bool trial = j && strstr(j, "S trial\n");
        free(j);
        /* either the old set whole and no journal, or the new set whole on trial */
        if (!(trial ? ot_set_new() : ot_set_old() && !ot_there(".rockbox/update/journal")))
            printf("ota: cut after %d steps: began %d trial %d\n", k, began, trial);
        assert(trial ? ot_set_new() : ot_set_old() && !ot_there(".rockbox/update/journal"));
        assert(trial == ot_there(SL_OTA_UNDO));
        if (trial && strcmp(sl_ota_running, "1.1"))
        {
            assert(s.restart);            /* 1.0 finished the swap: once, the restart that starts 1.1 */
            s = ot_restart();
            assert(!s.restart && s.trial && !strcmp(sl_ota_running, "1.1"));
        }
        ot_user_untouched();
    }
    assert(steps > 10);
    printf("PASS: ota: a power cut after each of the swap's %d steps: the next start has the old set or the new one, whole.\n", steps);

    /* ---- a staged file gone before a cut swap is finished: rolled back */
    ot_stage_good();
    sl_ota_cut_after = 9;                 /* past "S swapping", before the program */
    assert(sl_ota_swap() == SL_OTA_E_STOPPED);
    ot_path(p, sizeof p, ".rockbox/update/stage/.rockbox/rockbox.r1");
    assert(!unlink(p));
    s = ot_restart();
    assert(ot_set_old() && !ot_there(".rockbox/update/journal") && !ot_there(SL_OTA_UNDO) && !s.restart);
    ot_user_untouched();
    printf("PASS: ota: a swap that can't be finished is rolled back: 1.0 whole.\n");

    /* ---- a cut at every step of Previous version */
    int rsteps = 0;
    for (int k = 1; k < 60; k++)
    {
        ot_stage_good();
        assert(sl_ota_swap() == 0);
        s = ot_restart();
        assert(s.prev);
        sl_ota_cut_after = k;
        int r = sl_ota_revert();
        sl_ota_cut_after = 0;
        if (r == 0) { rsteps = k - 1; break; }
        s = ot_restart();
        bool back = ot_set_old();
        assert(back || ot_set_new());
        if (back && strcmp(sl_ota_running, "1.0")) { assert(s.restart); s = ot_restart(); assert(!s.restart); }
        if (!back) assert(s.prev || ot_there(SL_OTA_UNDO));   /* never began: still undoable */
        ot_user_untouched();
    }
    assert(rsteps > 5);
    printf("PASS: ota: a power cut after each of Previous version's %d steps leaves 1.1 or 1.0 whole.\n", rsteps);

    /* ---- the boot menu's way back: the undo script, run by sh, at any step */
    for (int k = 7; k < 30; k += 4)
    {
        ot_stage_good();
        sl_ota_cut_after = k;
        sl_ota_swap();
        sl_ota_cut_after = 0;
        if (!ot_there(SL_OTA_UNDO)) continue;
        ot_sh("cd / && /bin/sh '%s/" SL_OTA_UNDO "' > /dev/null", sim_root_dir);
        assert(ot_set_old() && !ot_there(SL_OTA_UNDO));
        s = ot_restart();
        assert(!ot_there(".rockbox/update/journal") && !s.prev && !s.restart && ot_set_old());
        ot_user_untouched();
    }
    ot_stage_good();
    assert(sl_ota_swap() == 0);
    ot_sh("cd / && /bin/sh '%s/" SL_OTA_UNDO "' > /dev/null", sim_root_dir);
    assert(ot_set_old());
    s = ot_restart();
    assert(!ot_there(".rockbox/update/journal") && !s.prev);
    printf("PASS: ota: the undo script puts 1.0 back from any step, and the next start tidies up.\n");

    /* ---- install from the phone remote: the upload, its progress, the same checks */
    ot_card_old();
    ot_sh("rm -f '%s'/*.gbupdate", sim_root_dir);
    sl_ota_test_reset();
    ot_keys();
    snprintf(sl_ota_running, sizeof sl_ota_running, "1.0");
    rm_set(1);
    pop_hide_now();
    rm_test_pair();
    {
        size_t n;
        char *u = ot_slurp(OT_W "/u11.gbupdate", &n);
        assert(u);
        char *b;
        rmt_get("/api/update", &b);
        assert(rmt_json(b) && strstr(b, "\"state\":\"none\"") && strstr(b, "\"key\":true"));
        /* no key: refused before a byte lands */
        sl_ota_test_key_set = false;
        assert(ot_post(u, n, n, ans, sizeof ans) == 403 && !ot_there(".rockbox/update/incoming.part"));
        ot_keys();
        /* USB mode: refused */
        sl_ota_hold(true);
        assert(ot_post(u, n, n, ans, sizeof ans) == 503);
        sl_ota_hold(false);
        ot_wait();
        /* the phone gone part way: nothing kept */
        ot_post(u, n, n / 2, ans, sizeof ans);
        for (int i = 0; i < 400 && ot_there(".rockbox/update/incoming.part"); i++) { frame(20); usleep(5000); }
        assert(!ot_there(".rockbox/update/incoming.part") && !ot_there(".rockbox/update/incoming.gbupdate"));
        /* the whole file: checked like one at the top, from the remote */
        update_page(); frame(200);
        assert(ot_post(u, n, n, ans, sizeof ans) == 200 && strstr(ans, "{\"ok\":true}"));
        ot_wait();
        sl_ota_status(&s);
        assert(s.state == SL_OTA_FOUND && !s.file[0] && !strcmp(s.version, "1.1"));
        for (int i = 0; i < 4; i++) { up_poll(); frame(100); }
        assert(has_text(lv_screen_active(), "SENT FROM THE PHONE REMOTE"));   /* the header, in capitals */
        rmt_get("/api/update", &b);
        assert(rmt_json(b) && strstr(b, "\"state\":\"found\"") && strstr(b, "1.1"));
        s = ot_install();
        assert(s.state == SL_OTA_READY && !ot_there(".rockbox/update/incoming.gbupdate"));
        assert(sl_ota_swap() == 0 && ot_set_new());
        s = ot_restart();
        assert(s.trial && !strcmp(sl_ota_running, "1.1"));
        /* a damaged upload: rejected in plain words */
        assert(sl_ota_revert() == 0);
        s = ot_restart();
        u[n - 10] ^= 1;
        assert(ot_post(u, n, n, ans, sizeof ans) == 200);
        ot_wait();
        s = ot_install();
        assert(s.state == SL_OTA_REJECTED && s.why == SL_OTA_E_DAMAGED && ot_set_old());
        rmt_get("/api/update", &b);
        assert(rmt_json(b) && strstr(b, "\"state\":\"rejected\""));
        assert(!ot_there(".rockbox/update/incoming.gbupdate"));   /* not left on the card */
        /* one that is not ours at all: refused at once, and gone */
        assert(ot_post("not an update at all", 20, 20, ans, sizeof ans) == 200);
        ot_wait();
        sl_ota_status(&s);
        assert(s.state == SL_OTA_REJECTED && !ot_there(".rockbox/update/incoming.gbupdate"));
        free(u);
        while (npages > 1) { page_pop(); frame(300); }
    }
    rm_set(0);
    pop_hide_now();
    ot_user_untouched();
    printf("PASS: ota: from the phone remote: the upload lands, is checked and installs; no key, USB mode and a dropped phone keep nothing.\n");

    /* ---- tidy: the card as it was */
    ot_sh("rm -rf '%s/.rockbox/update' '%s/" SL_OTA_UNDO "' '%s/.rockbox/rockbox.r1' '%s/.rockbox/codecs/ota_a.codec' '%s/.rockbox/codecs/ota_gone.codec' "
          "'%s/.rockbox/langs/ota_same.lng' '%s/.rockbox/rocks/ota_new.rock' '%s/.rockbox/sloppy/ota_test_save.dat' '%s/Playlists/ota_test.m3u' "
          "'%s/.rockbox/ota_test_playlist_control' " OT_W, sim_root_dir, sim_root_dir, sim_root_dir, sim_root_dir, sim_root_dir, sim_root_dir,
          sim_root_dir, sim_root_dir, sim_root_dir, sim_root_dir);
    ot_sh("rm -f '%s'/*.gbupdate", sim_root_dir);
    sl_ota_test_reset();
    sl_ota_test_key_set = false;
    snprintf(sl_ota_running, sizeof sl_ota_running, "%s", SL_VERSION);
    free(ot_cfg0); ot_cfg0 = NULL;
    sl_ota_boot();
}

/* ---- the screens, for the audit and the shots */
static bool ot_au_dirty;
static void ot_au_reset(void)
{
    if (!ot_au_dirty) return;
    ot_au_dirty = false;
    sl_ota_test_frozen = false;
    sl_ota_test_reset();
    sl_ota_test_key_set = false;
    sl_ota_boot();
    sl_ota_status(&up_st);
}
static void ot_au(int which)
{
    ot_au_dirty = true;
    struct sl_ota_status s;
    memset(&s, 0, sizeof s);
    s.key = s.crypto = true;
    snprintf(s.version, sizeof s.version, "1.1");
    snprintf(s.min_from, sizeof s.min_from, "1.0");
    snprintf(s.file, sizeof s.file, "GabagoolOS-1.1.gbupdate");
    s.size = 24117248;
    s.files = 212;
    snprintf(s.notes, sizeof s.notes, "%s", "Sudoku hints, a faster All list and a Year tile. Bluetooth pairs with a PIN. "
             "The volume remembers where it was after USB mode.");
    switch (which)
    {
        case 0: s.state = SL_OTA_NONE; s.notes[0] = '\0'; break;
        case 1: s.key = false; s.state = SL_OTA_NONE; break;
        case 2: s.state = SL_OTA_FOUND; break;
        case 3: s.state = SL_OTA_WORKING; s.phase = SL_OTA_PH_UNPACK; s.done = 9u << 20; s.total = 21u << 20; s.changed = 37; break;
        case 4: s.state = SL_OTA_READY; s.changed = 37; break;
        case 5: s.state = SL_OTA_REJECTED; s.why = SL_OTA_E_NOTOURS; break;
        case 6: s.state = SL_OTA_REJECTED; s.why = SL_OTA_E_OLDER; snprintf(s.version, sizeof s.version, "0.9"); break;
        case 7: s.state = SL_OTA_REJECTED; s.why = SL_OTA_E_MINFROM; snprintf(s.version, sizeof s.version, "2.0"); snprintf(s.min_from, sizeof s.min_from, "1.5"); break;
        case 8: s.state = SL_OTA_REJECTED; s.why = SL_OTA_E_DAMAGED; break;
        case 9: s.state = SL_OTA_REJECTED; s.why = SL_OTA_E_SPACE; s.need = 37u << 20; break;
        case 10: s.state = SL_OTA_REJECTED; s.why = SL_OTA_E_USB; break;
        case 11: s.state = SL_OTA_RECEIVING; s.rx = 14u << 20; s.rx_total = 23u << 20; s.file[0] = '\0'; break;
        case 12: s.state = SL_OTA_NONE; s.trial = true; s.prev = true; snprintf(s.prev_version, sizeof s.prev_version, "0.9"); break;
        case 13: s.state = SL_OTA_REJECTED; s.why = SL_OTA_E_NOCRYPTO; break;
        case 14: s.state = SL_OTA_FOUND; s.file[0] = '\0'; s.boot_menu = true; break;
        case 15: s.state = SL_OTA_NONE; s.notes[0] = '\0'; s.prev = true; snprintf(s.prev_version, sizeof s.prev_version, "0.9"); break;
    }
    sl_ota_test_frozen = true;
    sl_ota_test_force(&s);
    sl_ota_status(&up_st);
    if (which == 15) { up_st = s; up_revert_page(); return; }
    update_page();
    frame(300);
}
