/* GabagoolOS lyrics parser tests: cc -fsanitize=address,undefined lyrics_test.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../sloppy_lyrics_state.h"
#include "../sloppy_taglyrics.h"
#include <fcntl.h>

static struct sl_lrc lrc;
static char buf[SL_LRC_MAX_BYTES + 8];

static void parse(const char *raw, size_t len)
{
    sl_lrc_to_utf8((const unsigned char *)raw, len, (unsigned char *)buf, sizeof buf);
    sl_lrc_parse(buf, &lrc);
}
#define PARSE(s) parse(s, sizeof(s) - 1)

/* a song file of these bytes; its tag lyrics through the converter, in buf */
static size_t tag_file(const unsigned char *bytes, size_t len)
{
    const char *path = "/tmp/sloppy-taglyrics-test.bin";
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(bytes, 1, len, f) == len);
    fclose(f);
    int fd = open(path, O_RDONLY);
    static unsigned char raw[4096];
    size_t n = sl_tag_lyrics(fd, raw, sizeof raw);
    close(fd);
    remove(path);
    buf[0] = 0;
    if (n) sl_lrc_to_utf8(raw, n, (unsigned char *)buf, sizeof buf);
    return n;
}
static size_t put_be(unsigned char *p, uint32_t v, int n, int bits)
{
    for (int i = n - 1; i >= 0; i--) { p[i] = (unsigned char)(v & ((1u << bits) - 1)); v >>= bits; }
    return (size_t)n;
}
static void tag_tests(void)
{
    unsigned char f[2048];
    size_t n = 0;
    /* ID3v2.3: a picture frame first, then Latin-1 lyrics with a description */
    memcpy(f, "ID3\x03\x00\x00", 6); n = 10;
    memcpy(f + n, "APIC", 4); put_be(f + n + 4, 300, 4, 8); f[n + 8] = f[n + 9] = 0; n += 10;
    memset(f + n, 0xAB, 300); n += 300;
    const char uslt[] = "\x00" "eng" "desc\0" "[00:01.00]Caf\xe9 au lait";
    memcpy(f + n, "USLT", 4); put_be(f + n + 4, sizeof uslt - 1, 4, 8); f[n + 8] = f[n + 9] = 0; n += 10;
    memcpy(f + n, uslt, sizeof uslt - 1); n += sizeof uslt - 1;
    memset(f + n, 0, 64); n += 64;                               /* padding */
    put_be(f + 6, (uint32_t)(n - 10), 4, 7);
    memcpy(f + n, "\xff\xfb audio", 8);
    assert(tag_file(f, n + 8) && !strcmp(buf, "[00:01.00]Caf\xc3\xa9 au lait"));
    parse(buf, strlen(buf));
    assert(lrc.synced && lrc.n == 1 && lrc.lines[0].ms == 1000);
    /* ID3v2.4, syncsafe sizes: UTF-16 with a mark, an empty description */
    n = 0; memcpy(f, "ID3\x04\x00\x00", 6); n = 10;
    const unsigned char w[] = { 1, 'e', 'n', 'g', 0xff, 0xfe, 0, 0, 0xff, 0xfe, 'H', 0, 'i', 0, 0xd0, 0x05 };
    memcpy(f + n, "USLT", 4); put_be(f + n + 4, sizeof w, 4, 7); f[n + 8] = f[n + 9] = 0; n += 10;
    memcpy(f + n, w, sizeof w); n += sizeof w;
    put_be(f + 6, (uint32_t)(n - 10), 4, 7);
    assert(tag_file(f, n) && !strcmp(buf, "Hi\xd7\x90"));
    /* UTF-16 big-endian with no mark (encoding 2) */
    n = 0; memcpy(f, "ID3\x04\x00\x00", 6); n = 10;
    const unsigned char be[] = { 2, 'e', 'n', 'g', 0, 0, 0, 'O', 0, 'k' };
    memcpy(f + n, "USLT", 4); put_be(f + n + 4, sizeof be, 4, 7); f[n + 8] = f[n + 9] = 0; n += 10;
    memcpy(f + n, be, sizeof be); n += sizeof be;
    put_be(f + 6, (uint32_t)(n - 10), 4, 7);
    assert(tag_file(f, n) && !strcmp(buf, "Ok"));
    /* no lyrics frame, a frame size past the tag, and not a tag at all */
    n = 0; memcpy(f, "ID3\x03\x00\x00", 6); n = 10;
    memcpy(f + n, "TIT2", 4); put_be(f + n + 4, 3, 4, 8); f[n + 8] = f[n + 9] = 0; n += 10;
    memcpy(f + n, "\0Hi", 3); n += 3;
    put_be(f + 6, (uint32_t)(n - 10), 4, 7);
    assert(!tag_file(f, n));
    put_be(f + 14, 9999, 4, 8);
    assert(!tag_file(f, n));
    assert(!tag_file((const unsigned char *)"RIFF....WAVEfmt ", 16));
    /* FLAC: stream info, a picture, then the comments with lyrics in lower case */
    n = 0; memcpy(f, "fLaC", 4); n = 4;
    f[n] = 0; put_be(f + n + 1, 34, 3, 8); n += 4; memset(f + n, 0, 34); n += 34;
    f[n] = 6; put_be(f + n + 1, 200, 3, 8); n += 4; memset(f + n, 0x55, 200); n += 200;
    unsigned char vc[256]; size_t v = 0;
    #define LE(x) do { uint32_t _x = (x); vc[v++] = _x & 255; vc[v++] = _x >> 8 & 255; vc[v++] = _x >> 16 & 255; vc[v++] = _x >> 24; } while (0)
    LE(3); memcpy(vc + v, "lib", 3); v += 3;
    LE(2);
    LE(10); memcpy(vc + v, "TITLE=Song", 10); v += 10;
    LE(22); memcpy(vc + v, "lyrics=[00:02]Hello\nX", 22); v += 22;
    #undef LE
    f[n] = 0x84; put_be(f + n + 1, (uint32_t)v, 3, 8); n += 4; memcpy(f + n, vc, v); n += v;
    assert(tag_file(f, n) && !strcmp(buf, "[00:02]Hello\nX"));
    /* FLAC with comments but no lyrics */
    memcpy(f + n - 22, "LYRICX=[00:02]Hello\nX", 22);
    assert(!tag_file(f, n));
}

int main(void)
{
    /* timed lines, metadata, CRLF, several stamps per line, sorting */
    PARSE("\xef\xbb\xbf[ar:Someone]\r\n[ti:Song]\r\n[00:12.50]First line\r\n"
          "[00:05]Intro\r\n[00:20.1][01:00.123]Chorus  \r\n[00:30.00]\r\n[00:40:25]Last");
    assert(lrc.synced && lrc.n == 6);
    assert(lrc.lines[0].ms == 5000 && !strcmp(lrc.lines[0].text, "Intro"));
    assert(lrc.lines[1].ms == 12500 && !strcmp(lrc.lines[1].text, "First line"));
    assert(lrc.lines[2].ms == 20100 && !strcmp(lrc.lines[2].text, "Chorus"));
    assert(lrc.lines[3].ms == 30000 && !lrc.lines[3].text[0]);        /* a timed gap */
    assert(lrc.lines[4].ms == 40250 && !strcmp(lrc.lines[4].text, "Last"));
    assert(lrc.lines[5].ms == 60123 && !strcmp(lrc.lines[5].text, "Chorus"));
    assert(sl_lrc_find(&lrc, 4999) == -1 && sl_lrc_find(&lrc, 5000) == 0);
    assert(sl_lrc_find(&lrc, 20099) == 1 && sl_lrc_find(&lrc, 59000) == 4);
    assert(sl_lrc_find(&lrc, 999999) == 5);

    /* offset: positive shows lyrics sooner; word times are removed */
    PARSE("[offset:+500]\n[00:01.00]<00:01.00>Hel<00:01.40>lo\n[00:00.20]Zero");
    assert(lrc.synced && lrc.n == 2 && lrc.lines[0].ms == 0 && lrc.lines[1].ms == 500);
    assert(!strcmp(lrc.lines[1].text, "Hello"));
    PARSE("[offset:-250]\n[00:01.00]Later");
    assert(lrc.lines[0].ms == 1250);

    /* plain lyrics are unsynced, keep one blank line between verses */
    PARSE("Verse one\nline two\n\n\n\nVerse two\n\n");
    assert(!lrc.synced && lrc.n == 4 && !lrc.lines[2].text[0] && !strcmp(lrc.lines[3].text, "Verse two"));
    assert(sl_lrc_find(&lrc, 1000) == -1);

    /* Hebrew: UTF-8 kept, Windows-1255 and UTF-16 converted */
    PARSE("[00:01.00]\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d");
    assert(!strcmp(lrc.lines[0].text, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"));
    PARSE("[00:01.00]\xf9\xec\xe5\xed \x96 ok");
    assert(!strcmp(lrc.lines[0].text, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xe2\x80\x93 ok"));
    PARSE("[00:02]Caf\xe9");                       /* Latin-1 */
    assert(!strcmp(lrc.lines[0].text, "Caf\xc3\xa9"));
    PARSE("[00:02]D\xe9j\xe0 vu, \xe0 la caf\xe9 tr\xe8s bien");   /* French, not Hebrew */
    assert(!strcmp(lrc.lines[0].text, "D\xc3\xa9j\xc3\xa0 vu, \xc3\xa0 la caf\xc3\xa9 tr\xc3\xa8s bien"));
    const char u16[] = "\xff\xfe[\0" "0\0" "0\0" ":\0" "0\0" "3\0" "]\0" "\xe9\x05" "\n\0";
    parse(u16, sizeof u16 - 1);
    assert(lrc.synced && lrc.n == 1 && lrc.lines[0].ms == 3000 && !strcmp(lrc.lines[0].text, "\xd7\xa9"));

    /* malformed stamps are text, not times; nothing reads out of bounds */
    PARSE("[0a:10]x\n[00:99]y\n[00:10\n[:]\n[");
    assert(!lrc.synced && lrc.n == 5);
    PARSE("");
    assert(lrc.n == 0);

    /* too many lines are capped; truncation keeps whole UTF-8 characters */
    static char big[SL_LRC_MAX_LINES * 20 + 32];
    size_t at = 0;
    for (int i = 0; i < SL_LRC_MAX_LINES + 50; i++)
        at += (size_t)snprintf(big + at, sizeof big - at, "[%02d:%02d]L%d\n", i / 60, i % 60, i);
    parse(big, at);
    assert(lrc.n == SL_LRC_MAX_LINES);
    unsigned char small[6];
    size_t n = sl_lrc_to_utf8((const unsigned char *)"ab\xd7\xa9\xd7\xa9", 6, small, sizeof small);
    assert(n == 4 && sl_lrc_utf8_valid(small, n));

    /* fuzz: random bytes never crash and always give valid UTF-8 lines */
    srand(7);
    for (int round = 0; round < 3000; round++)
    {
        char raw[300];
        size_t len = (size_t)(rand() % (int)sizeof raw);
        for (size_t i = 0; i < len; i++)
        {
            int r = rand() % 12;
            raw[i] = r < 3 ? "[]:.<>\n0123456789"[rand() % 17] : (char)(rand() & 0xff);
        }
        parse(raw, len);
        for (int i = 0; i < lrc.n; i++)
            assert(sl_lrc_utf8_valid((const unsigned char *)lrc.lines[i].text, strlen(lrc.lines[i].text)));
        for (int i = 1; lrc.synced && i < lrc.n; i++) assert(lrc.lines[i - 1].ms <= lrc.lines[i].ms);
    }

    char path[64];
    assert(sl_lrc_path("/Music/a.b/song.mp3", path, sizeof path, false) && !strcmp(path, "/Music/a.b/song.lrc"));
    assert(sl_lrc_path("/Music/a.b/noext", path, sizeof path, true) && !strcmp(path, "/Music/a.b/noext.LRC"));
    assert(!sl_lrc_path("/x.mp3", path, 6, false));
    tag_tests();
    printf("PASS: lyrics timing, offsets, metadata, plain text, Hebrew encodings, limits, fuzz, tags\n");
    return 0;
}
