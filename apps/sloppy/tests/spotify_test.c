/* Spotify playlist matching: cc -fsanitize=address,undefined spotify_test.c */
#include <assert.h>
#include <stdio.h>
#include "../sloppy_spotify_match.h"

static struct sp_local lib[] = {
    { "Night Tide", "Mira Sol", "Harbor Lights", 241000, 0 },
    { "Night Tide - Live at the Roxy", "Mira Sol", "Live", 268000, 0 },
    { "Blue Hours", "The Neon Harbor", "Blue", 200000, 0 },
    { "Blue Hours", "Neon Harbor", "Hits", 199000, 0 },
    { "Paper Kites", "Various Artists", "Summer Mix 2019", 180000, 0 },
    { "Paper Kites", "Various Artists", "Other Comp", 180000, 0 },
    { "Duet", "Mira Sol & Kite", "Together", 210000, 0 },
    { "Silent City", "Kite", "City", 300000, 0 },
    { "שולה", "חן", "אלבום", 150000, 0 },
    { "Long Song", "Kite", "City", 400000, 0 },
    { "Hey You (Radio Edit)", "Lumen", "Singles", 180000, 0 },
};
#define N ((int)(sizeof lib / sizeof lib[0]))

static int find(const char *title, const char *artists, const char *album, long ms)
{
    struct sp_song s = { "", "", "", ms };
    strncpy(s.title, title, SP_FIELD - 1); strncpy(s.artists, artists, SP_FIELD - 1); strncpy(s.album, album, SP_FIELD - 1);
    return sp_match(&s, lib, N);
}

int main(void)
{
    for (int i = 0; i < N; i++) lib[i].key = sp_title_key(lib[i].title);
    /* remaster notes ignored; the studio take, not the live one */
    assert(find("Night Tide - 2011 Remaster", "Mira Sol", "Harbor Lights (Deluxe)", 241500) == 0);
    assert(find("Night Tide (Remastered Version)", "Mira Sol", "x", 0) == 0);
    assert(find("Night Tide - Live at the Roxy", "Mira Sol", "Live", 268000) == 1);
    assert(find("Night Tide - Live", "Mira Sol", "", 0) == -1);          /* a live take we don't have */
    assert(find("Night Tide (Acoustic)", "Mira Sol", "", 0) == -1);
    /* feat. credits, a leading The, case and punctuation */
    assert(find("night tide (feat. Kite)", "Mira Sol;Kite", "", 241000) == 0);
    assert(find("Blue Hours", "Neon Harbor", "Blue", 200000) == 2);       /* album picks the copy */
    assert(find("Blue Hours", "Neon Harbor", "Hits", 199000) == 3);
    /* wrong artist, wrong length */
    assert(find("Silent City", "Mira Sol", "City", 300000) == -1);
    assert(find("Long Song", "Kite", "City", 200000) == -1);              /* 200 s off: another recording */
    assert(find("Long Song", "Kite", "City", 405000) == 9);
    /* compilations need their album */
    assert(find("Paper Kites", "Someone", "Summer Mix 2019", 180000) == 4);
    assert(find("Paper Kites", "Someone", "Their Own Album", 180000) == -1);
    /* several artists together; Hebrew as it is */
    assert(find("Duet", "Mira Sol;Kite", "", 0) == 6);
    assert(find("Duet", "Kite;Mira Sol", "", 0) == -1);
    assert(find("שולה", "חן", "", 150000) == 8);
    /* a version on the card's title has to be asked for */
    assert(find("Hey You", "Lumen", "", 180000) == -1);
    assert(find("Hey You - Radio Edit", "Lumen", "", 180000) == 10);
    /* lines */
    char line[] = "Night Tide\tMira Sol\tHarbor Lights\t241000\tUSXX1\r\n", hdr[] = "#name\tMix";
    struct sp_song s;
    assert(!sp_parse_line(hdr, &s));
    assert(sp_parse_line(line, &s) && !strcmp(s.album, "Harbor Lights") && s.ms == 241000);
    char shortl[] = "Only a title";
    assert(!sp_parse_line(shortl, &s));
    printf("PASS: Spotify matching: notes, versions, artists, compilations, lengths, lines.\n");
    return 0;
}
