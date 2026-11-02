#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../sloppy_radio_news.h"

/* id of the upgrade with this name (first match) */
static int upg(const char *name)
{
    struct fm_upgrade u;
    for (int i = 0; i < FM_UPG_COUNT; i++)
        if (sl_fm_upgrade(i, &u) && !strcmp(u.name, name)) return i;
    assert(!"no such upgrade");
    return -1;
}

/* Greedy bot: always buys the item with the best payback, taps 4 times a
 * second. Returns simulated seconds until all_total reaches `goal`. */
static double bot_run(struct sl_fm_state *s, double goal, int *buys)
{
    const double tps = 4;
    struct sl_fm_rates r;
    double t = 0;
    *buys = 0;
    sl_fm_recalc(s, &r);
    while (s->all_total < goal && t < 30 * 86400)
    {
        double base = r.per_sec + r.per_tap * tps, best = 1e308, cost = 0;
        int kind = -1, idx = 0;
        for (int b = 0; b < FM_B_COUNT; b++)
        {
            if (b > 0 && !s->owned[b - 1]) break;
            struct sl_fm_state t2 = *s;
            t2.owned[b]++;
            struct sl_fm_rates r2;
            sl_fm_recalc(&t2, &r2);
            double c = sl_fm_building_cost(s, b, 1), gain = r2.per_sec + r2.per_tap * tps - base;
            double score = c / gain + (c > s->listeners ? (c - s->listeners) / base : 0);
            if (gain > 0 && score < best) { best = score; kind = 0; idx = b; cost = c; }
        }
        for (int i = 0; i < FM_UPG_COUNT; i++)
        {
            struct fm_upgrade u;
            if (!sl_fm_upgrade(i, &u) || sl_fm_has(s, i) || !sl_fm_unlocked(s, &u)) continue;
            struct sl_fm_state t2 = *s;
            fm_set(t2.upgrades, i);
            struct sl_fm_rates r2;
            sl_fm_recalc(&t2, &r2);
            double gain = r2.per_sec + r2.per_tap * tps - base;
            double score = u.cost / gain + (u.cost > s->listeners ? (u.cost - s->listeners) / base : 0);
            if (gain > 0 && score < best) { best = score; kind = 1; idx = i; cost = u.cost; }
        }
        assert(kind >= 0 && base > 0);
        if (cost > s->listeners)
        {
            double wait = ceil((cost - s->listeners) / base);
            if (s->all_total + wait * base >= goal) { t += (goal - s->all_total) / base; sl_fm_earn(s, goal - s->all_total); break; }
            t += wait;
            sl_fm_earn(s, wait * base);
        }
        assert(kind ? sl_fm_buy_upgrade(s, idx) : sl_fm_buy_building(s, idx, 1) == 1);
        (*buys)++;
        sl_fm_check_plaques(s, NULL);
        sl_fm_recalc(s, &r);
    }
    return t;
}

int main(void)
{
    struct sl_fm_state s;
    struct sl_fm_rates r;
    char buf[48];

    /* formatting */
    sl_fm_fmt(0, buf, sizeof buf); assert(!strcmp(buf, "0"));
    sl_fm_fmt(0.1, buf, sizeof buf); assert(!strcmp(buf, "0.1"));
    sl_fm_fmt(1.02, buf, sizeof buf); assert(!strcmp(buf, "1"));
    sl_fm_fmt(123456, buf, sizeof buf); assert(!strcmp(buf, "123,456"));
    sl_fm_fmt(1234567, buf, sizeof buf); assert(!strcmp(buf, "1.234 million"));
    sl_fm_fmt(999999999999.0, buf, sizeof buf); assert(!strcmp(buf, "999.999 billion"));
    sl_fm_fmt(1e40, buf, sizeof buf); assert(!strcmp(buf, "10.000 duodecillion"));
    sl_fm_fmt(1e60, buf, sizeof buf); assert(strstr(buf, "e+60"));
    sl_fm_fmt(-5, buf, sizeof buf); assert(!strcmp(buf, "0"));

    /* fresh state, tapping, first building */
    sl_fm_init(&s, 1000);
    assert(sl_fm_valid(&s) && s.listeners == 0 && !sl_fm_buy_building(&s, FM_INTERN, 1));
    sl_fm_recalc(&s, &r);
    assert(r.per_tap == 1 && r.per_sec == 0);
    for (int i = 0; i < 15; i++) sl_fm_tap(&s, &r, 1);
    assert(s.taps == 15 && s.listeners == 15 && s.tap_total == 15);
    assert(sl_fm_buy_building(&s, FM_INTERN, 1) == 1 && s.listeners == 0 && s.owned[FM_INTERN] == 1);
    sl_fm_recalc(&s, &r);
    assert(fabs(r.per_sec - 0.1) < 1e-9);

    /* bulk cost matches a loop; Max buys exactly what is affordable */
    {
        struct sl_fm_state t = s;
        double loop = 0;
        for (int i = 0; i < 10; i++) { loop += sl_fm_building_cost(&t, FM_JINGLE, 1); t.owned[FM_JINGLE]++; }
        assert(fabs(loop - sl_fm_building_cost(&s, FM_JINGLE, 10)) < 1e-6 * loop);
        s.listeners = 5000;
        unsigned n = sl_fm_max_afford(&s, FM_JINGLE);
        assert(n > 0 && sl_fm_building_cost(&s, FM_JINGLE, n) <= 5000 && sl_fm_building_cost(&s, FM_JINGLE, n + 1) > 5000);
        assert(sl_fm_buy_building(&s, FM_JINGLE, 0) == n && s.owned[FM_JINGLE] == n && s.listeners >= 0);
        s.listeners = 1e300;
        assert(sl_fm_buy_building(&s, FM_STREET, 0) == FM_MAX_OWNED && !sl_fm_buy_building(&s, FM_STREET, 1));
        s.owned[FM_STREET] = 995; assert(sl_fm_max_afford(&s, FM_STREET) == 5);
        s.owned[FM_STREET] = FM_MAX_OWNED; assert(sl_fm_max_afford(&s, FM_STREET) == 0);
        s.owned[FM_STREET] = 0; s.listeners = 0;
    }

    /* upgrades: locked until unlocked, x2, synergy, order independent */
    s.listeners = 1e6;
    assert(!sl_fm_buy_upgrade(&s, FM_TIER_ID(FM_TOWER, 0)));               /* needs a tower */
    sl_fm_recalc(&s, &r);
    double before = r.each[FM_INTERN];
    assert(sl_fm_buy_upgrade(&s, FM_TIER_ID(FM_INTERN, 0)) && !sl_fm_buy_upgrade(&s, FM_TIER_ID(FM_INTERN, 0)));
    sl_fm_recalc(&s, &r);
    assert(fabs(r.each[FM_INTERN] - 2 * before) < 1e-9);
    assert(!sl_fm_buy_upgrade(&s, FM_TIER_ID(0, 15)) && !sl_fm_buy_upgrade(&s, FM_INTERN_ID) && !sl_fm_buy_upgrade(&s, FM_UPG_COUNT) && !sl_fm_buy_upgrade(&s, -1));
    {
        struct sl_fm_state a, b;
        sl_fm_init(&a, 1); a.owned[FM_INTERN] = 20; a.owned[FM_CALLIN] = 20; a.listeners = 1e9; a.run_total = 1e9;
        b = a;
        int list[] = { 0, 1, upg("Intern Call Screeners"), upg("Morning Show"), upg("Drive Time"), FM_TIER_ID(FM_CALLIN, 0) };
        for (int i = 0; i < 6; i++) assert(sl_fm_buy_upgrade(&a, list[i]));
        for (int i = 5; i >= 0; i--) assert(sl_fm_buy_upgrade(&b, list[i]));
        struct sl_fm_rates ra, rb;
        sl_fm_recalc(&a, &ra); sl_fm_recalc(&b, &rb);
        assert(fabs(ra.per_sec - rb.per_sec) < 1e-9 * ra.per_sec);
        /* Intern x2 x2, then synergy +5% x 20 Call-in lines = x2 */
        assert(fabs(ra.each[FM_INTERN] / ra.global - 0.1 * 4 * 2) < 1e-9);
    }

    /* plaques and fan club */
    {
        struct sl_fm_state p;
        int first = -1;
        sl_fm_init(&p, 1);
        p.all_total = 1e6; p.owned[FM_INTERN] = 50; p.taps = 100;
        assert(sl_fm_check_plaques(&p, &first) == 5 + 2 + 1 && first == 0);
        assert(sl_fm_check_plaques(&p, NULL) == 0 && sl_fm_plaque_count(&p) == 8);
        sl_fm_plaque_name(FM_PL_BUILD + 1, buf, sizeof buf); assert(!strcmp(buf, "50 Intern DJs"));
        sl_fm_plaque_name(3, buf, sizeof buf); assert(!strcmp(buf, "100,000 listeners"));
        sl_fm_plaque_name(FM_PL_SYND, buf, sizeof buf); assert(!strcmp(buf, "Syndicated 1 time"));
    }

    /* Request Line */
    {
        uint32_t seed = 42;
        int seen[3] = {0};
        for (int i = 0; i < 1000; i++) seen[sl_fm_call_pick(&seed)]++;
        assert(seen[0] > 350 && seen[1] > 120 && seen[2] > 250);
        struct sl_fm_state p; sl_fm_init(&p, 1); p.listeners = 1e9;
        struct sl_fm_rates pr; sl_fm_recalc(&p, &pr); pr.per_sec = 10;
        assert(sl_fm_payola(&p, &pr) == 9013);
        assert(sl_fm_caller_rate(&p) == 1 && sl_fm_boost_len(&p) == 1);
        p.callers = 7; p.listeners = 1e9;
        assert(sl_fm_buy_upgrade(&p, upg("Hold Music")) && sl_fm_buy_upgrade(&p, upg("Call Waiting")));
        assert(fabs(sl_fm_caller_rate(&p) - 1.5625) < 1e-6);
    }

    /* Signal Lock combo, tier hints, Right progress */
    {
        struct sl_fm_state p; sl_fm_init(&p, 1);
        assert(sl_fm_combo_mult(&p, 0) == 1 && sl_fm_combo_mult(&p, 1) == 3 && sl_fm_combo_mult(&p, 7) == 3);
        p.taps = 5000; p.listeners = 1e9;
        assert(sl_fm_buy_upgrade(&p, upg("Fine Tuning")) && sl_fm_buy_upgrade(&p, upg("Phase Lock")));
        assert(sl_fm_combo_mult(&p, 1) == 5 && fabs(sl_fm_combo_mult(&p, 0.5) - 3) < 1e-9);
        assert(sl_fm_next_tier(&p, FM_INTERN) == 1);
        p.owned[FM_INTERN] = 30; assert(sl_fm_next_tier(&p, FM_INTERN) == 50);
        p.owned[FM_INTERN] = 350; assert(sl_fm_next_tier(&p, FM_INTERN) == 400);
        p.owned[FM_INTERN] = 600; assert(sl_fm_next_tier(&p, FM_INTERN) == 0);
        assert(sl_fm_right_progress(FM_RIGHT_BASE) == 0 && fabs(sl_fm_right_progress(3.375 * FM_RIGHT_BASE) - 0.5) < 1e-9);
    }

    /* every upgrade has a name and description, and the tier text names its building */
    for (int i = 0; i < FM_UPG_COUNT; i++)
    {
        struct fm_upgrade u;
        if (!sl_fm_upgrade(i, &u)) { assert(i < FM_PLAYLIST_ID); continue; }
        assert(u.name && *u.name && u.desc && *u.desc && strlen(u.name) < 30);
        if (u.kind == FU_BUILDING) assert(!strncmp(u.desc, fm_buildings[u.target].name, strlen(fm_buildings[u.target].name) - 1));
    }

    /* the shop is big, and says what unlocks the next thing */
    {
        struct fm_upgrade u;
        struct sl_fm_state p; sl_fm_init(&p, 1);
        assert(sl_fm_upgrade_total() > 600 && FM_PLAQUE_COUNT > 300 && PK_COUNT > 30);
        sl_fm_upgrade(FM_TIER_ID(FM_INTERN, 3), &u);
        sl_fm_unlock_text(&u, buf, sizeof buf); assert(!strcmp(buf, "Unlocks at 50 Intern DJs"));
        p.owned[FM_INTERN] = 25; assert(sl_fm_unlock_progress(&p, &u) == 0.5);
        sl_fm_upgrade(FM_INTERN_ID + FM_TOWER, &u);
        assert(u.kind == FU_INTERN && !strcmp(u.name, "Tower Intern"));
        p.owned[FM_TOWER] = 5; assert(fabs(sl_fm_unlock_progress(&p, &u) - 5.0 / 15) < 1e-9);
        sl_fm_upgrade(FM_PLAYLIST_ID + 239, &u);
        assert(u.cost > 1e65 && u.value == 5 && u.unlock == UL_TOTAL);
        /* an intern specialisation doubles interns and feeds its building */
        struct sl_fm_rates r1, r2;
        p.owned[FM_TOWER] = 15; p.owned[FM_INTERN] = 40; p.listeners = 1e12;
        sl_fm_recalc(&p, &r1);
        assert(sl_fm_buy_upgrade(&p, FM_INTERN_ID + FM_TOWER));
        sl_fm_recalc(&p, &r2);
        assert(fabs(r2.each[FM_INTERN] - 2 * r1.each[FM_INTERN]) < 1e-9);
        assert(fabs(r2.each[FM_TOWER] - r1.each[FM_TOWER] * 1.1) < 1e-6);   /* 40 interns / 4 = +10% */
    }

    /* Owned summary matches what was bought */
    {
        struct sl_fm_state p; struct fm_summary m;
        sl_fm_init(&p, 1);
        sl_fm_summary(&p, &m); assert(!m.owned_count && m.bmul[FM_INTERN] == 1 && m.combo_cap == 3);
        p.owned[FM_INTERN] = 20; p.owned[FM_CALLIN] = 20; p.taps = 600; p.callers = 30; p.run_total = 1e9; p.listeners = 1e12;
        int list[] = { 0, 1, upg("Better Antenna"), upg("Studio Monitors"), upg("Signal Boost I"), upg("Morning Show"),
                       upg("Intern Call Screeners"), upg("Hold Music"), upg("Extended Segment"), upg("Fine Tuning") };
        for (int i = 0; i < 10; i++) assert(sl_fm_buy_upgrade(&p, list[i]));
        sl_fm_summary(&p, &m);
        assert(m.owned_count == 10 && m.tiers[FM_INTERN] == 2 && m.bmul[FM_INTERN] == 8 && m.tap_flat == 51);
        assert(m.tap_pct == 1 && m.global_pct == 5 && m.caller_rate == 1.25 && m.boost_len == 1.5 && m.combo_cap == 4);
    }

    /* Station Formats */
    {
        struct sl_fm_state p; struct sl_fm_rates pr, base;
        sl_fm_init(&p, 1); p.owned[FM_JINGLE] = 10;
        sl_fm_recalc(&p, &base);
        assert(!sl_fm_set_format(&p, FMT_TOP40, 100));                     /* locked */
        p.owned[FM_STREET] = 1; sl_fm_recalc(&p, &base);
        assert(sl_fm_set_format(&p, FMT_TOP40, 100) && p.format == FMT_TOP40);
        sl_fm_recalc(&p, &pr); assert(fabs(pr.per_sec - base.per_sec * 1.2) < 1e-9 && pr.per_tap == base.per_tap);
        assert(sl_fm_format_wait(&p, 400) == 300 && !sl_fm_set_format(&p, FMT_TALK, 400));
        assert(sl_fm_set_format(&p, FMT_TALK, 700) && !sl_fm_set_format(&p, FMT_COUNT, 5000));
        sl_fm_recalc(&p, &pr); assert(fabs(pr.per_sec - base.per_sec * 0.9) < 1e-9 && fabs(pr.per_tap - 3 * base.per_tap) < 1e-9);
        assert(sl_fm_set_format(&p, FMT_LATE, 1400) && sl_fm_offline_cap(&p) == 2 * 86400);
        assert(sl_fm_set_format(&p, FMT_HOTLINE, 2100) && sl_fm_caller_rate(&p) == 2);
        sl_fm_seal(&p); assert(sl_fm_valid(&p));
        p.format = FMT_COUNT; sl_fm_seal(&p); assert(!sl_fm_valid(&p));
        p.format = FMT_HOTLINE; p.all_total = 2 * FM_RIGHT_BASE; assert(sl_fm_syndicate(&p) && p.format == FMT_HOTLINE && sl_fm_formats_open(&p));
    }

    /* news: every headline fits, no repeats, every condition can come true */
    {
        for (int i = 0; i < FM_NEWS_COUNT; i++)
        {
            assert(strlen(fm_news[i].text) + 6 < 160 && strlen(fm_news[i].text) > 20);
            for (int j = 0; j < i; j++) assert(strcmp(fm_news[i].text, fm_news[j].text));
        }
        struct sl_fm_state p; sl_fm_init(&p, 1);
        for (int b = 0; b < FM_B_COUNT; b++) p.owned[b] = 1;
        p.syndications = 1; p.callers = 1; p.taps = 500; fm_set(p.perks, PK_PRIME);
        memset(p.plaques, 0xff, sizeof p.plaques);
        for (int f = FMT_TOP40; f < FMT_COUNT; f++)
        {
            p.format = (uint32_t)f;
            for (int i = 0; i < FM_NEWS_COUNT; i++)
                if (fm_news[i].need >= NW_HOTLINE && fm_news[i].need <= NW_TOP40 && fm_news[i].need != -2 - f) continue;
                else if (fm_news[i].need != NW_EARLY) assert(sl_fm_news_ok(&p, fm_news[i].need, 2e6));
        }
        sl_fm_init(&p, 1);
        assert(sl_fm_news_ok(&p, NW_EARLY, 0) && !sl_fm_news_ok(&p, NW_SYND, 0) && !sl_fm_news_ok(&p, FM_JINGLE, 0));
        char line[160]; uint32_t seed = 7; int news = 0;
        for (int i = 0; i < 200; i++)
        {
            sl_fm_news(&p, 0, &seed, line, sizeof line);
            news += !strncmp(line, "NEWS: ", 6);
            assert(!strstr(line, "Jingle machine") && !strstr(line, "BREAKING"));
        }
        assert(news == 200);   /* nothing to brag about yet */
        printf("news: %d headlines\n", FM_NEWS_COUNT);
    }

    /* offline catch-up, rollback, perk caps */
    sl_fm_init(&s, 1000);
    s.owned[FM_JINGLE] = 2;
    sl_fm_recalc(&s, &r);
    assert(sl_fm_offline(&s, &r, 1010) == 20 && s.listeners == 20);
    assert(sl_fm_offline(&s, &r, 1005) == 0 && s.listeners == 20);      /* clock rollback */
    assert(sl_fm_offline(&s, &r, 1015) == 20);
    assert(sl_fm_offline(&s, &r, 1015 + 1000000) == 2.0 * 86400);       /* 1 day cap */
    fm_set(s.perks, PK_EVER1);
    assert(sl_fm_offline(&s, &r, s.last_time + 1000000) == 2.0 * 3 * 86400);

    /* save integrity */
    sl_fm_init(&s, 1000);
    s.listeners = 55; sl_fm_seal(&s); assert(sl_fm_valid(&s));
    s.listeners++; assert(!sl_fm_valid(&s));
    s.listeners = NAN; sl_fm_seal(&s); assert(!sl_fm_valid(&s));
    s.listeners = -1; sl_fm_seal(&s); assert(!sl_fm_valid(&s));
    s.listeners = 1; s.owned[FM_B_COUNT] = 1; sl_fm_seal(&s); assert(!sl_fm_valid(&s));
    s.owned[FM_B_COUNT] = 0; fm_set(s.upgrades, FM_TIER_ID(0, 15)); sl_fm_seal(&s); assert(!sl_fm_valid(&s));
    s.upgrades[0] = 0; s.rights_spent = 1; sl_fm_seal(&s); assert(!sl_fm_valid(&s));
    s.rights_spent = 0; s.version = 1; sl_fm_seal(&s); assert(!sl_fm_valid(&s));
    s.version = SL_FM_VERSION; sl_fm_seal(&s); assert(sl_fm_valid(&s));

    /* Syndication */
    assert(sl_fm_rights_for(0.999 * FM_RIGHT_BASE) == 0 && sl_fm_rights_for(FM_RIGHT_BASE) == 1 && sl_fm_rights_for(8 * FM_RIGHT_BASE) == 2);
    assert(sl_fm_rights_for(1000 * FM_RIGHT_BASE) == 10 && sl_fm_rights_for(1e300) == 4000000000u);
    sl_fm_init(&s, 1000);
    s.all_total = 0.5 * FM_RIGHT_BASE; assert(!sl_fm_syndicate(&s));
    s.all_total = s.run_total = 28 * FM_RIGHT_BASE; s.listeners = 7; s.owned[FM_TOWER] = 9; s.taps = 3;
    fm_set(s.upgrades, 0); fm_set(s.plaques, 0);
    assert(sl_fm_pending(&s) == 3 && sl_fm_syndicate(&s));
    assert(s.rights == 3 && s.syndications == 1 && s.listeners == 0 && s.run_total == 0 && !s.owned[FM_TOWER]);
    assert(!s.upgrades[0] && s.plaques[0] == 1 && s.taps == 3 && s.all_total == 28 * FM_RIGHT_BASE);
    assert(!sl_fm_syndicate(&s));                                     /* nothing new */
    sl_fm_recalc(&s, &r); assert(fabs(r.global - 1.03) < 1e-9);
    assert(!sl_fm_buy_perk(&s, PK_SEED1));                            /* needs parent */
    assert(sl_fm_buy_perk(&s, PK_CONTRACT) && !sl_fm_buy_perk(&s, PK_CONTRACT));
    sl_fm_recalc(&s, &r); assert(fabs(r.global - 1.03 * 1.05) < 1e-9); /* spending keeps the bonus */
    assert(!sl_fm_buy_perk(&s, PK_SEED1) && s.rights_spent == 1);       /* 2 left, costs 3 */
    s.rights = 200;
    assert(sl_fm_buy_perk(&s, PK_BRAND1) && sl_fm_buy_perk(&s, PK_SEED1));
    assert(sl_fm_buy_perk(&s, PK_STAFF) && sl_fm_buy_perk(&s, PK_ARCHIVE));
    s.all_total = FM_RIGHT_BASE * 210.5 * 210.5 * 210.5;
    assert(sl_fm_syndicate(&s) && s.rights == 210);
    assert(s.listeners == 1000 && s.owned[FM_INTERN] == 10 && s.owned[FM_JINGLE] == 5 && sl_fm_has(&s, FM_TIER_ID(FM_CLONE, 0)));
    assert(sl_fm_building_cost(&s, FM_INTERN, 1) < 0.951 * 15 * pow(1.15, 10));
    sl_fm_seal(&s); assert(sl_fm_valid(&s));
    /* fuzz: any save that passes the checks must play without NaN, overflow or crash */
    {
        uint32_t seed = 99;
        int accepted = 0;
        for (int i = 0; i < 20000; i++)
        {
            struct sl_fm_state f;
            uint8_t *p = (uint8_t *)&f;
            for (size_t k = 0; k < sizeof f; k++) p[k] = (uint8_t)sl_fm_rand(&seed);
            if (i & 1)   /* half the time, plausible values in the right ranges */
            {
                sl_fm_init(&f, 1000);
                f.listeners = pow(10, sl_fm_rand(&seed) % 300);
                f.all_total = f.run_total = pow(10, sl_fm_rand(&seed) % 300);
                for (int b = 0; b < FM_B_COUNT; b++) f.owned[b] = (uint16_t)(sl_fm_rand(&seed) % (FM_MAX_OWNED + 1));
                for (int k = 0; k < 200; k++)
                {
                    int id = (int)(sl_fm_rand(&seed) % FM_UPG_COUNT);
                    struct fm_upgrade u;
                    if (sl_fm_upgrade(id, &u)) fm_set(f.upgrades, id);
                }
                for (int k = 0; k < PK_COUNT; k++) if (sl_fm_rand(&seed) & 1) fm_set(f.perks, k);
                f.rights = sl_fm_rand(&seed); f.rights_spent = f.rights / 2;
                f.format = sl_fm_rand(&seed) % FMT_COUNT;
                f.syndications = sl_fm_rand(&seed); f.callers = sl_fm_rand(&seed); f.taps = sl_fm_rand(&seed);
            }
            else { f.magic = SL_FM_MAGIC; f.version = SL_FM_VERSION; f.size = sizeof f; }
            sl_fm_seal(&f);
            if (!sl_fm_valid(&f)) continue;
            accepted++;
            struct sl_fm_rates fr;
            sl_fm_recalc(&f, &fr);
            assert(isfinite(fr.per_sec) && isfinite(fr.per_tap) && fr.per_sec >= 0 && fr.per_tap > 0);
            sl_fm_offline(&f, &fr, f.last_time + 86400);
            sl_fm_tap(&f, &fr, 777 * 5);
            for (int b = 0; b < FM_B_COUNT; b++) { sl_fm_max_afford(&f, b); sl_fm_buy_building(&f, b, 0); }
            for (int u = 0; u < FM_UPG_COUNT; u++) sl_fm_buy_upgrade(&f, u);
            for (int p2 = 0; p2 < PK_COUNT; p2++) sl_fm_buy_perk(&f, p2);
            sl_fm_check_plaques(&f, NULL);
            sl_fm_syndicate(&f);
            sl_fm_recalc(&f, &fr);
            char line[160];
            sl_fm_news(&f, fr.per_sec, &seed, line, sizeof line);
            sl_fm_fmt(f.listeners, line, sizeof line);
            sl_fm_seal(&f);
            assert(sl_fm_valid(&f) && isfinite(f.listeners) && isfinite(fr.per_sec));
        }
        assert(accepted > 5000);
        printf("fuzz: %d random saves accepted and played safely\n", accepted);
    }
    puts("PASS: Sloppy FM economy, upgrades, plaques, callers, offline, save checks, syndication");

    /* balance: time to the first Signal Right, then a faster second run */
    {
        int buys;
        struct sl_fm_state b;
        sl_fm_init(&b, 1);
        double t1 = bot_run(&b, FM_RIGHT_BASE, &buys);
        if (getenv("FM_WRITE_MIDGAME"))   /* a realistic save for the UI harness */
        {
            FILE *f = fopen(getenv("FM_WRITE_MIDGAME"), "wb");
            sl_fm_seal(&b);
            assert(f && fwrite(&b, sizeof b, 1, f) == 1);
            fclose(f);
        }
        printf("balance: first Signal Right after %.1f h (%d purchases)\n", t1 / 3600, buys);
        assert(t1 > 10 * 3600 && t1 < 30 * 3600);   /* Cookie Clicker pace: most of a day */
        double goal = FM_RIGHT_BASE * pow(sl_fm_rights_for(b.all_total) + 5, 3);
        assert(sl_fm_syndicate(&b));
        b.rights_spent = 0;
        for (int p = 0; p < PK_COUNT; p++) sl_fm_buy_perk(&b, p);
        sl_fm_start_run(&b);
        double t2 = bot_run(&b, goal, &buys);
        printf("balance: +5 more Rights after %.1f h (%d purchases, %u perks spent)\n", t2 / 3600, buys, (unsigned)b.rights_spent);
    }
    return 0;
}
