#include <assert.h>
#include <stdio.h>
#include "../sloppy_pet_state.h"

int main(void)
{
    struct sl_pet_state s;
    const uint32_t start = 1760000000u;
    sl_pet_init(&s, start);
    assert(sl_pet_valid(&s));
    assert(s.species == SL_PET_UNCHOSEN);
    sl_pet_choose(&s, SL_PET_BUN, "Pip", start);
    assert(s.species == SL_PET_BUN && !strcmp(s.name, "Pip"));
    /* an egg stays an egg, and nothing decays, until it is hatched */
    assert(sl_pet_is_egg(&s) && sl_pet_valid(&s));
    assert(sl_pet_stage_at(&s, start + 10) == SL_PET_EGG);
    assert(sl_pet_stage_at(&s, start + 90 * 86400) == SL_PET_EGG);
    uint8_t egg_food = s.food;
    assert(!sl_pet_advance(&s, start + 5 * 86400) && s.food == egg_food && !s.dead);
    sl_pet_hatch(&s, start);
    assert(!sl_pet_is_egg(&s) && sl_pet_valid(&s) && s.born_epoch == start && s.last_epoch == start);
    assert(sl_pet_stage_at(&s, start + 1) == SL_PET_BABY);
    assert(sl_pet_stage_at(&s, start + 13 * 3600) == SL_PET_GROWN);
    assert(!sl_pet_advance(&s, start + 3599));
    assert(sl_pet_advance(&s, start + 24 * 3600));
    assert(s.food == 13 && s.clean == 61 && s.joy == 37 && s.energy == 61);
    assert(sl_pet_valid(&s));
    sl_pet_care(&s, SL_PET_FEED);
    sl_pet_care(&s, SL_PET_BATHE);
    sl_pet_care(&s, SL_PET_PLAY);
    sl_pet_care(&s, SL_PET_REST);
    assert(s.food == 41 && s.clean == 96 && s.joy == 37 && s.energy == 96);
    assert(s.meals == 1 && s.baths == 1 && s.games == 0 && s.rests == 1);
    sl_pet_game_result(&s, true);
    assert(s.joy == 61 && s.energy == 87 && s.games == 1);
    sl_pet_game_result(&s, false);
    assert(s.joy == 69 && s.energy == 82 && s.games == 2);
    assert(sl_pet_valid(&s));
    uint8_t cuddle_joy = s.joy; s.stimulation = 30;
    sl_pet_cuddle(&s, true);
    assert(s.joy == cuddle_joy + 2 && s.stimulation == 26 && sl_pet_valid(&s));
    sl_pet_cuddle(&s, false);
    assert(s.joy == cuddle_joy + 2 && s.stimulation == 22 && sl_pet_valid(&s));
    struct sl_pet_state saved = s;
    assert(!sl_pet_advance(&s, start)); /* clock rollback must not age the pet */
    assert(sl_pet_valid(&s));
    {   /* a clock set back a year starts counting again instead of freezing */
        struct sl_pet_state back = s;
        uint8_t food = back.food;
        assert(sl_pet_advance(&back, back.last_epoch - 365u * 86400u) && back.food == food);
        assert(sl_pet_advance(&back, back.last_epoch + 2 * 3600u) && back.food == food - 6);
        assert(sl_pet_valid(&back));
    }
    saved.food = 255;
    assert(!sl_pet_valid(&saved));
    struct sl_pet_legacy_state old = { .magic = SL_PET_OLD_MAGIC,
        .last_epoch = start, .meals = 7, .food = 48, .clean = 42,
        .joy = 61, .energy = 37 };
    old.checksum = sl_pet_bytes_checksum(&old, sizeof old - sizeof old.checksum);
    struct sl_pet_state converted;
    assert(sl_pet_migrate(&converted, &old));
    assert(sl_pet_valid(&converted) && converted.meals == 7 &&
           converted.food == 48 && converted.species == SL_PET_UNCHOSEN);
    old.checksum++;
    assert(!sl_pet_migrate(&converted, &old));
    struct sl_pet_v2_state v2 = { .magic = SL_PET_V2_MAGIC,
        .last_epoch = start, .born_epoch = start - 3600,
        .food = 71, .clean = 69, .joy = 88, .energy = 66,
        .species = SL_PET_BUN, .name = "Pip" };
    v2.checksum = sl_pet_bytes_checksum(&v2, sizeof v2 - sizeof v2.checksum);
    assert(sl_pet_migrate_v2(&converted, &v2));
    assert(sl_pet_valid(&converted) && !strcmp(converted.name,"Pip") &&
           converted.species == SL_PET_BUN);
    struct sl_pet_state excited;
    sl_pet_init(&excited, start);
    sl_pet_choose(&excited, SL_PET_SLOP, "Slop", start);
    sl_pet_hatch(&excited, start);
    for (int i = 0; i < 3; i++) sl_pet_game_result(&excited, true);
    assert(excited.manic && excited.stimulation >= 80 && sl_pet_valid(&excited));
    sl_pet_care(&excited, SL_PET_CALM);
    assert(excited.manic && excited.stimulation > 20);
    sl_pet_care(&excited, SL_PET_CALM);
    assert(!excited.manic && excited.stimulation <= 20 && sl_pet_valid(&excited));
    sl_pet_choose(&excited, SL_PET_SPAGHET, "Spaghet", start);
    sl_pet_hatch(&excited, start);
    assert(excited.species == SL_PET_SPAGHET && !strcmp(excited.name,"Spaghet"));
    struct sl_pet_v3_state v3 = { .magic = SL_PET_V3_MAGIC,
        .last_epoch = start, .born_epoch = start - 3600,
        .food = 76, .clean = 64, .joy = 91, .energy = 72,
        .species = SL_PET_SLOP, .name = "Slop", .stimulation = 83, .manic = 1 };
    v3.checksum = sl_pet_bytes_checksum(&v3, sizeof v3 - sizeof v3.checksum);
    assert(sl_pet_migrate_v3(&converted, &v3));
    assert(sl_pet_valid(&converted) && converted.manic && !converted.crisis);
    sl_pet_take_drugs(&converted);
    assert(converted.crisis && converted.stimulation == 100);
    sl_pet_relax(&converted, start);
    assert(!converted.crisis && !converted.manic && converted.hungover);
    assert(sl_pet_advance(&converted, start + 25 * 3600));
    assert(converted.hungover && sl_pet_valid(&converted));
    assert(sl_pet_advance(&converted, start + 49 * 3600));
    assert(!converted.hungover);
    /* an unset clock (1970) must not leave a save that loads as corrupt */
    struct sl_pet_state unset = converted;
    sl_pet_take_drugs(&unset);
    sl_pet_relax(&unset, 3600);
    assert(!unset.crisis && !unset.hungover && sl_pet_valid(&unset));
    /* an unterminated favourite artist is rejected even with a good checksum */
    memset(unset.taste[0].artist, 'A', SL_PET_ARTIST_SIZE);
    unset.checksum = sl_pet_checksum(&unset);
    assert(!sl_pet_valid(&unset));
    converted.joy = 85;
    sl_pet_stimulate(&converted, 90);
    sl_pet_take_drugs(&converted);
    sl_pet_hospital(&converted);
    assert(!converted.crisis && !converted.manic && converted.energy == 90);
    /* every substance, every outcome: what it does, and the overdose */
    for (int what = 0; what < SL_PET_SUBSTANCES; what++)
        for (unsigned roll = 0; roll < sl_pet_outcomes[what]; roll++)
        {
            struct sl_pet_state t = converted;
            t.manic = 1; t.stimulation = 85; t.crisis = what == SL_PET_BUMP;
            t.checksum = sl_pet_checksum(&t);
            int o = sl_pet_use(&t, what, roll);
            assert(o >= 0 && o < sl_pet_outcomes[what] && sl_pet_valid(&t));
            switch (what) {
            case SL_PET_SNORT: assert(t.crisis && t.manic && !t.dead); break;
            case SL_PET_ENERGY: assert(t.manic && t.crisis == (o == 2)); break;
            case SL_PET_JOINT: assert(!t.manic && !t.crisis && t.stimulation == 15); break;
            case SL_PET_ACID: assert(t.crisis == (o == 1) && t.manic == (o == 1 || o == 3)); break;
            case SL_PET_SYRUP: assert(!t.manic && t.stimulation == 5); break;
            case SL_PET_BUMP: assert(t.dead == (o < 3)); break;
            }
        }
    {   /* one more bump without a crisis is only more of the same */
        struct sl_pet_state t = converted;
        assert(sl_pet_use(&t, SL_PET_BUMP, 0) == 3 && !t.dead);
        /* the hospital, all five ways: always out of the crisis */
        for (unsigned roll = 0; roll < SL_PET_HOSPITAL_OUTCOMES; roll++)
        {
            struct sl_pet_state h = converted;
            h.manic = 1; h.crisis = 1; h.checksum = sl_pet_checksum(&h);
            assert(sl_pet_hospital_roll(&h, roll) == (int)roll && !h.crisis && !h.dead && sl_pet_valid(&h));
            assert(h.manic == (roll == 2) && (roll != 4 || h.joy == 100));
        }
        /* a worn-out pet's last bump is likelier its last; a rested one, less */
        int tired = 0, rested = 0;
        for (unsigned roll = 0; roll < 500; roll++)
        {
            struct sl_pet_state a = converted, b = converted;
            a.crisis = b.crisis = 1; a.energy = 10; b.energy = 90;
            sl_pet_use(&a, SL_PET_BUMP, roll); sl_pet_use(&b, SL_PET_BUMP, roll);
            tired += a.dead; rested += b.dead;
        }
        assert(tired > 300 && rested < 300 && tired > rested + 100);
        /* care: every outcome happens, and the nature leans them */
        int seen[5][SL_PET_CARE_OUTCOMES] = { { 0 } };
        for (unsigned roll = 0; roll < 2000; roll++)
            for (int act = SL_PET_FEED; act <= SL_PET_CALM; act++)
            {
                if (act == SL_PET_PLAY) continue;
                struct sl_pet_state c = converted;
                c.food = (uint8_t)(roll % 100); c.stimulation = (uint8_t)(roll * 7 % 100);
                c.born_epoch = roll; c.checksum = sl_pet_checksum(&c);
                uint8_t f0 = c.food, c0 = c.clean, e0 = c.energy;
                int o = sl_pet_care_roll(&c, (enum sl_pet_action)act, roll * 2654435761u);
                /* care never leaves it worse in what it was for */
                assert(act != SL_PET_FEED || c.food >= f0);
                assert(act != SL_PET_BATHE || c.clean >= c0);
                assert(act != SL_PET_REST || c.energy >= e0);
                assert(o >= 0 && o < SL_PET_CARE_OUTCOMES && sl_pet_valid(&c));
                seen[act][o]++;
            }
        for (int act = SL_PET_FEED; act <= SL_PET_REST; act++)
            if (act != SL_PET_PLAY) for (int o = 0; o < SL_PET_CARE_OUTCOMES; o++) assert(seen[act][o] > 0);
        assert(seen[SL_PET_FEED][0] > seen[SL_PET_FEED][1]);       /* the plain outcome is still the usual one */
        int natures[SL_PET_NATURES] = { 0 };
        for (unsigned b = 0; b < 1000; b++) { struct sl_pet_state c = converted; c.born_epoch = b; natures[sl_pet_nature(&c)]++; }
        for (int n = 0; n < SL_PET_NATURES; n++) assert(natures[n] > 120);
        /* away: something happens to a living pet, never to a dead one */
        { struct sl_pet_state c = converted; int e = sl_pet_away(&c, 12345); assert(e >= 0 && e < SL_PET_AWAY_EVENTS && sl_pet_valid(&c)); }
        struct sl_pet_state d = converted;
        sl_pet_end_life(&d);
        assert(sl_pet_away(&d, 1) == -1);
        assert(sl_pet_use(&d, SL_PET_SNORT, 0) == 0 && !d.crisis);   /* the dead take nothing */
    }
    uint8_t old_joy = converted.joy;
    sl_pet_club(&converted);
    assert(converted.joy >= old_joy && converted.games == 1);
    sl_pet_end_life(&converted);
    uint8_t dead_food = converted.food;
    sl_pet_care(&converted, SL_PET_FEED);
    assert(converted.dead && converted.food == dead_food && sl_pet_valid(&converted));
    struct sl_pet_state wired;
    sl_pet_init(&wired, start);
    sl_pet_choose(&wired, SL_PET_BOT, "Zap", start);
    sl_pet_hatch(&wired, start);
    wired.joy = 90; sl_pet_stimulate(&wired, 85);
    assert(wired.manic);
    uint8_t calm_energy = wired.energy;
    sl_pet_game_result(&wired, true);
    assert(wired.energy == calm_energy - 16 && !wired.dead); /* 9 + 7 manic cost */
    wired.crisis = 1;
    int rounds = 0;
    while (!wired.dead && rounds < 20) { sl_pet_club(&wired); rounds++; }
    assert(wired.dead && wired.energy == 0 && rounds <= 3 && sl_pet_valid(&wired));
    struct sl_pet_state tired;
    sl_pet_init(&tired, start);
    sl_pet_choose(&tired, SL_PET_BUN, "Nap", start);
    sl_pet_hatch(&tired, start);
    tired.energy = 3;
    sl_pet_club(&tired);
    assert(!tired.dead && tired.energy == 0); /* only a wired pet can overdo it */
    struct sl_pet_state listener;
    sl_pet_init(&listener, start);
    sl_pet_choose(&listener, SL_PET_SLOP, "Bop", start);
    sl_pet_hatch(&listener, start);
    listener.joy = 50; listener.energy = 50;
    int got_joy, got_energy;
    for (int m = 0; m < 14; m++) sl_pet_listen(&listener, "Mira Sol", false, false);
    for (int m = 0; m < 4; m++) sl_pet_listen(&listener, "Night Owl", true, false);
    assert(listener.music_minutes == 18 && listener.bank_day == 14 && listener.bank_night == 4);
    assert(sl_pet_favorite(&listener) && !strcmp(sl_pet_favorite(&listener), "Mira Sol"));
    assert(sl_pet_music_claim(&listener, &got_joy, &got_energy) == 10);
    assert(got_joy == 4 && got_energy == 0 && listener.joy == 54 && listener.bank_day == 4);
    assert(listener.bank_night == 4 && sl_pet_valid(&listener));
    for (int m = 0; m < 400; m++) sl_pet_listen(&listener, "Mira Sol", m & 1, true);
    assert(listener.bank_day == 404 && listener.bank_night == 404);
    sl_pet_music_claim(&listener, &got_joy, &got_energy);
    assert(got_joy == 12 && got_energy == 12 && !listener.manic && listener.bank_day == 4);
    /* A burst of new artists cannot evict the long-standing favorite. */
    const char *const passers[] = { "A", "B", "C", "D", "E", "F", "G", "H" };
    for (int m = 0; m < 8; m++) sl_pet_listen(&listener, passers[m], false, false);
    assert(!strcmp(sl_pet_favorite(&listener), "Mira Sol"));
    sl_pet_listen(&listener, "An artist name much longer than the slot", false, false);
    assert(sl_pet_valid(&listener));
    sl_pet_heard_song(&listener);
    assert(listener.songs_heard == 1);
    listener.dead = 1; listener.checksum = sl_pet_checksum(&listener);
    sl_pet_listen(&listener, "Mira Sol", false, false);
    assert(listener.music_minutes == 427);
    struct sl_pet_state fresh_ear;
    sl_pet_init(&fresh_ear, start);
    assert(!sl_pet_favorite(&fresh_ear));
    struct sl_pet_v4_state v4 = { .magic = SL_PET_V4_MAGIC, .meals = 3, .food = 40,
        .clean = 50, .joy = 60, .energy = 70, .species = SL_PET_BOT, .name = "Zed",
        .stimulation = 12, .hungover = 1, .hungover_until = start + 90000 };
    v4.checksum = sl_pet_bytes_checksum(&v4, sizeof v4 - sizeof v4.checksum);
    struct sl_pet_state v5;
    assert(sl_pet_migrate_v4(&v5, &v4) && sl_pet_valid(&v5));
    assert(v5.meals == 3 && v5.species == SL_PET_BOT && !strcmp(v5.name, "Zed") &&
           v5.hungover && v5.hungover_until == start + 90000 && !v5.music_minutes);
    v4.food = 101;
    assert(!sl_pet_migrate_v4(&v5, &v4));
    struct sl_pet_state neglected;
    sl_pet_init(&neglected, start);
    sl_pet_choose(&neglected, SL_PET_BUN, "Pip", start);
    sl_pet_hatch(&neglected, start);
    assert(sl_pet_advance(&neglected, start + 120 * 3600));
    assert(neglected.dead && sl_pet_valid(&neglected));
    assert(sl_pet_advance(&s, start + 365 * 24 * 3600u));
    assert(s.food == 0 && s.clean == 0 && s.joy == 0 && s.energy == 0);
    assert(sl_pet_valid(&s));
    puts("GabagoolOS pet clock, care, rollback, long absence and checksum tests passed");
    return 0;
}
