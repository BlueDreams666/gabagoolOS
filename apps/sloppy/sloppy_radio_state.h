/* Sloppy FM economy: buildings, upgrade trees, plaques, Request Line
 * callers and Syndication prestige. No LVGL here so tests/radio_test.c can
 * drive the whole game. GPLv2+.
 *
 * Table indices are saved as bits. Never reorder or remove an entry;
 * only append. */
#ifndef SLOPPY_RADIO_STATE_H
#define SLOPPY_RADIO_STATE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define SL_FM_MAGIC    0x464D5632u   /* "FMV2" */
#define SL_FM_VERSION  3   /* 3: 20 buildings, ~700 upgrades; older saves start fresh */
#define FM_B_MAX       24
#define FM_UPG_WORDS   32
#define FM_PERK_WORDS  4
#define FM_ACH_WORDS   16
#define FM_MAX_OWNED   1000
#define FM_TOP         1e300

struct sl_fm_state {
    /* doubles first so the checksummed bytes have no padding holes */
    double listeners;        /* spendable */
    double run_total;        /* earned this broadcast (since last syndication) */
    double all_total;        /* ever earned, drives Signal Rights */
    double tap_total;
    uint32_t magic, version, size, last_time, run_started, taps;
    uint32_t rights, rights_spent, syndications, callers;
    uint32_t upgrades[FM_UPG_WORDS];
    uint32_t perks[FM_PERK_WORDS];
    uint32_t plaques[FM_ACH_WORDS];
    uint16_t owned[FM_B_MAX];
    uint32_t format, format_since;   /* Station Format and when it was picked */
    uint32_t reserved[14];
    uint32_t checksum;
};

struct sl_fm_rates {
    double per_sec, per_tap, global;
    double each[FM_B_MAX];   /* listeners/s from one building of each kind */
};

/* ------------------------------------------------------------ tables */
enum {
    FM_INTERN, FM_JINGLE, FM_CALLIN, FM_STREET, FM_TOWER,
    FM_CHOPPER, FM_SAT, FM_PODCAST, FM_STREAM, FM_ALGO,
    FM_MOON, FM_PIRATE, FM_HOLO, FM_PRISM, FM_PREDICT,
    FM_ECHO, FM_KERNEL, FM_MULTI, FM_BRAIN, FM_CLONE,
    FM_B_COUNT };

struct fm_building { const char *name, *plural, *short_name, *blurb; double base_cost, base_lps; };
static const struct fm_building fm_buildings[FM_B_COUNT] = {
    { "Intern DJ", "Intern DJs", "Intern", "Works for exposure", 15, 0.1 },
    { "Jingle Machine", "Jingle Machines", "Jingle", "Earworms on demand", 100, 1 },
    { "Call-in Line", "Call-in Lines", "Call-in", "Caller nine, you're on air", 1.1e3, 8 },
    { "Street Team", "Street Teams", "Street", "Stickers on every lamppost", 1.2e4, 47 },
    { "Transmitter Tower", "Transmitter Towers", "Tower", "More watts, more fans", 1.3e5, 260 },
    { "Traffic Chopper", "Traffic Choppers", "Chopper", "Live from above", 1.4e6, 1.4e3 },
    { "Satellite Uplink", "Satellite Uplinks", "Satellite", "Coast to coast", 2.0e7, 7.8e3 },
    { "Podcast Network", "Podcast Networks", "Podcast", "Same show, on demand", 3.3e8, 4.4e4 },
    { "Streaming App", "Streaming Apps", "Streaming", "Now with a skip button", 5.1e9, 2.6e5 },
    { "The Algorithm", "The Algorithm", "Algorithm", "It knows what you like", 7.5e10, 1.6e6 },
    { "Moon Relay", "Moon Relays", "Moon", "Broadcasting to the tides", 1.0e12, 1.0e7 },
    { "Pirate Dimension", "Pirate Dimensions", "Pirate", "Unlicensed in 11 realities", 1.4e13, 6.5e7 },
    { "Holographic Studio", "Holographic Studios", "Hologram", "The DJ is not really there", 1.7e14, 4.3e8 },
    { "Frequency Prism", "Frequency Prisms", "Prism", "Splits one song into seven", 2.1e15, 2.9e9 },
    { "Hit Predictor", "Hit Predictors", "Predictor", "Plays tomorrow's hits today", 2.6e16, 2.1e10 },
    { "Echo Chamber", "Echo Chambers", "Echo", "Everyone agrees this slaps", 3.1e17, 1.5e11 },
    { "Radio Kernel", "Radio Kernels", "Kernel", "The station, compiled", 7.1e19, 1.1e12 },
    { "Multiverse Simulcast", "Multiverse Simulcasts", "Multiverse", "Every you is listening", 1.2e22, 8.3e12 },
    { "Brainwave Broadcaster", "Brainwave Broadcasters", "Brainwave", "No radio required", 1.9e24, 6.4e13 },
    { "Clone DJ", "Clone DJs", "Clone", "It's you. It's all you", 5.4e26, 5.1e14 },
};

enum fm_upg_kind { FU_BUILDING, FU_TAP_FLAT, FU_TAP_PCT, FU_GLOBAL_PCT,
                   FU_SYNERGY, FU_CALLER, FU_FANCLUB, FU_COMBO, FU_INTERN, FU_TAP_BUILD };
enum fm_unlock { UL_OWN, UL_TOTAL, UL_TAPS, UL_PLAQUES, UL_CALLERS, UL_PAIR };

struct fm_upg_def {
    const char *name, *desc;
    double cost, need;
    uint8_t kind, unlock, target, source;
    float value;
};
struct fm_upgrade {
    const char *name, *desc;    /* static strings: filling one is cheap */
    double cost, need;
    uint8_t kind, unlock, target, source;
    float value;
};

/* Every building gets a x2 at each of these counts. Upgrade id of tier t
 * of building b is FM_TIER_ID(b, t); ids up to FM_B_MAX * 16 are reserved. */
#define FM_TIERS 15
#define FM_TIER_STRIDE 16
#define FM_TIER_ID(b, t) ((b) * FM_TIER_STRIDE + (t))
static const uint16_t fm_tier_need[FM_TIERS] = { 1, 5, 25, 50, 100, 150, 200, 250, 300, 350, 400, 450, 500, 550, 600 };
/* Prices are a multiple of the building's base cost. Kept steep on purpose:
 * an upgrade should be a decision, not something you always have cash for. */
static const double fm_tier_cost[FM_TIERS] = { 25, 150, 2500, 2.5e5, 2.5e7, 2.5e9, 2.5e11, 2.5e14, 2.5e17, 2.5e20,
                                               2.5e23, 2.5e26, 2.5e29, 2.5e32, 2.5e35 };
static const char *const fm_tier_name[FM_TIERS] = {
    "Fresh Batteries", "Better Cables", "Soundproofing", "Pro Headphones", "Gold Records",
    "Platinum Records", "Diamond Records", "Hall of Fame", "Legend Status", "Eternal Rotation",
    "Cosmic Airplay", "Quantum Reverb", "Infinite Loop", "Galactic Charts", "The Final Mix",
};
static const char *const fm_tier_desc[FM_B_COUNT] = {
    "Intern DJs twice as efficient",
    "Jingle Machines twice as efficient",
    "Call-in Lines twice as efficient",
    "Street Teams twice as efficient",
    "Transmitter Towers twice as efficient",
    "Traffic Choppers twice as efficient",
    "Satellite Uplinks twice as efficient",
    "Podcast Networks twice as efficient",
    "Streaming Apps twice as efficient",
    "The Algorithm works twice as hard",
    "Moon Relays twice as efficient",
    "Pirate Dimensions twice as efficient",
    "Holographic Studios twice as efficient",
    "Frequency Prisms twice as efficient",
    "Hit Predictors twice as efficient",
    "Echo Chambers twice as efficient",
    "Radio Kernels twice as efficient",
    "Multiverse Simulcasts twice as efficient",
    "Brainwave Broadcasters twice as efficient",
    "Clone DJs twice as efficient",
};

/* Intern specialisations (id FM_INTERN_ID + b, b >= 1): Intern DJs x2, and
 * building b gains 1% per b Intern DJs. Need 15 of both. */
#define FM_INTERN_ID 384
static const char *const fm_intern_name[FM_B_COUNT] = {
    NULL,
    "Jingle Intern",
    "Call-in Intern",
    "Street Intern",
    "Tower Intern",
    "Chopper Intern",
    "Satellite Intern",
    "Podcast Intern",
    "Streaming Intern",
    "Algorithm Intern",
    "Moon Intern",
    "Pirate Intern",
    "Hologram Intern",
    "Prism Intern",
    "Predictor Intern",
    "Echo Intern",
    "Kernel Intern",
    "Multiverse Intern",
    "Brainwave Intern",
    "Clone Intern",
};
static const char *const fm_intern_desc[FM_B_COUNT] = {
    NULL,
    "Interns x2, Jingle Machines +1% for each Intern DJ",
    "Interns x2, Call-in Lines +1% for every 2 Intern DJs",
    "Interns x2, Street Teams +1% for every 3 Intern DJs",
    "Interns x2, Transmitter Towers +1% for every 4 Intern DJs",
    "Interns x2, Traffic Choppers +1% for every 5 Intern DJs",
    "Interns x2, Satellite Uplinks +1% for every 6 Intern DJs",
    "Interns x2, Podcast Networks +1% for every 7 Intern DJs",
    "Interns x2, Streaming Apps +1% for every 8 Intern DJs",
    "Interns x2, The Algorithm +1% for every 9 Intern DJs",
    "Interns x2, Moon Relays +1% for every 10 Intern DJs",
    "Interns x2, Pirate Dimensions +1% for every 11 Intern DJs",
    "Interns x2, Holographic Studios +1% for every 12 Intern DJs",
    "Interns x2, Frequency Prisms +1% for every 13 Intern DJs",
    "Interns x2, Hit Predictors +1% for every 14 Intern DJs",
    "Interns x2, Echo Chambers +1% for every 15 Intern DJs",
    "Interns x2, Radio Kernels +1% for every 16 Intern DJs",
    "Interns x2, Multiverse Simulcasts +1% for every 17 Intern DJs",
    "Interns x2, Brainwave Broadcasters +1% for every 18 Intern DJs",
    "Interns x2, Clone DJs +1% for every 19 Intern DJs",
};

/* Crossovers (id FM_PAIR_ID + k): target +5% per source building owned.
 * Need 15 of both. */
#define FM_PAIR_ID 408
struct fm_pair { uint8_t target, source; const char *name, *desc; };
static const struct fm_pair fm_pairs[] = {
    { FM_INTERN, FM_STREET, "Intern-Street Crossover", "Intern DJs +5% per Street Team" },
    { FM_JINGLE, FM_TOWER, "Jingle-Tower Simulcast", "Jingle Machines +5% per Transmitter Tower" },
    { FM_CALLIN, FM_CHOPPER, "Call-in-Chopper Collab", "Call-in Lines +5% per Traffic Chopper" },
    { FM_STREET, FM_SAT, "Street-Satellite Remix", "Street Teams +5% per Satellite Uplink" },
    { FM_TOWER, FM_PODCAST, "Tower-Podcast Mashup", "Transmitter Towers +5% per Podcast Network" },
    { FM_CHOPPER, FM_STREAM, "Chopper-Streaming Duet", "Traffic Choppers +5% per Streaming App" },
    { FM_SAT, FM_ALGO, "Satellite-Algorithm Crossover", "Satellite Uplinks +5% per The Algorithm" },
    { FM_PODCAST, FM_MOON, "Podcast-Moon Simulcast", "Podcast Networks +5% per Moon Relay" },
    { FM_STREAM, FM_PIRATE, "Streaming-Pirate Collab", "Streaming Apps +5% per Pirate Dimension" },
    { FM_ALGO, FM_HOLO, "Algorithm-Hologram Remix", "The Algorithm +5% per Holographic Studio" },
    { FM_MOON, FM_PRISM, "Moon-Prism Mashup", "Moon Relays +5% per Frequency Prism" },
    { FM_PIRATE, FM_PREDICT, "Pirate-Predictor Duet", "Pirate Dimensions +5% per Hit Predictor" },
    { FM_HOLO, FM_ECHO, "Hologram-Echo Crossover", "Holographic Studios +5% per Echo Chamber" },
    { FM_PRISM, FM_KERNEL, "Prism-Kernel Simulcast", "Frequency Prisms +5% per Radio Kernel" },
    { FM_PREDICT, FM_MULTI, "Predictor-Multiverse Collab", "Hit Predictors +5% per Multiverse Simulcast" },
    { FM_ECHO, FM_BRAIN, "Echo-Brainwave Remix", "Echo Chambers +5% per Brainwave Broadcaster" },
    { FM_KERNEL, FM_CLONE, "Kernel-Clone Mashup", "Radio Kernels +5% per Clone DJ" },
    { FM_INTERN, FM_PODCAST, "Intern-Podcast Duet", "Intern DJs +5% per Podcast Network" },
    { FM_JINGLE, FM_STREAM, "Jingle-Streaming Crossover", "Jingle Machines +5% per Streaming App" },
    { FM_CALLIN, FM_ALGO, "Call-in-Algorithm Simulcast", "Call-in Lines +5% per The Algorithm" },
    { FM_STREET, FM_MOON, "Street-Moon Collab", "Street Teams +5% per Moon Relay" },
    { FM_TOWER, FM_PIRATE, "Tower-Pirate Remix", "Transmitter Towers +5% per Pirate Dimension" },
    { FM_CHOPPER, FM_HOLO, "Chopper-Hologram Mashup", "Traffic Choppers +5% per Holographic Studio" },
    { FM_SAT, FM_PRISM, "Satellite-Prism Duet", "Satellite Uplinks +5% per Frequency Prism" },
    { FM_PODCAST, FM_PREDICT, "Podcast-Predictor Crossover", "Podcast Networks +5% per Hit Predictor" },
    { FM_STREAM, FM_ECHO, "Streaming-Echo Simulcast", "Streaming Apps +5% per Echo Chamber" },
    { FM_ALGO, FM_KERNEL, "Algorithm-Kernel Collab", "The Algorithm +5% per Radio Kernel" },
    { FM_MOON, FM_MULTI, "Moon-Multiverse Remix", "Moon Relays +5% per Multiverse Simulcast" },
    { FM_PIRATE, FM_BRAIN, "Pirate-Brainwave Mashup", "Pirate Dimensions +5% per Brainwave Broadcaster" },
    { FM_HOLO, FM_CLONE, "Hologram-Clone Duet", "Holographic Studios +5% per Clone DJ" },
};
#define FM_PAIR_COUNT ((int)(sizeof fm_pairs / sizeof fm_pairs[0]))
#define FM_UPG_EXTRA 480
#define FM_EXTRA_PRICE 4   /* the fm_extra prices below are before this */

static const struct fm_upg_def fm_extra[] = {
    /* tap line */
    { "Better Antenna",    "+1 listener per tap",  100,    25,   FU_TAP_FLAT, UL_TAPS, 0, 0, 1 },
    { "Boom Mic",          "+5 listeners per tap", 1500,   150,  FU_TAP_FLAT, UL_TAPS, 0, 0, 5 },
    { "Studio Monitors",   "+50 listeners per tap", 5e4,   600,  FU_TAP_FLAT, UL_TAPS, 0, 0, 50 },
    { "Golden Microphone", "+500 listeners per tap", 5e6,  2000, FU_TAP_FLAT, UL_TAPS, 0, 0, 500 },
    { "Signal Boost I",    "Taps also earn 1% of your listeners/s", 5e4,  1e4,  FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost II",   "Taps also earn 1% of your listeners/s", 5e6,  1e6,  FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost III",  "Taps also earn 1% of your listeners/s", 5e8,  1e8,  FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost IV",   "Taps also earn 1% of your listeners/s", 5e10, 1e10, FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost V",    "Taps also earn 1% of your listeners/s", 5e13, 1e13, FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost VI",   "Taps also earn 1% of your listeners/s", 5e16, 1e16, FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost VII",  "Taps also earn 1% of your listeners/s", 5e19, 1e19, FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost VIII", "Taps also earn 1% of your listeners/s", 5e22, 1e22, FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    /* ratings: everything */
    { "Morning Show",      "+5% all listeners",  1e5,  2e4,  FU_GLOBAL_PCT, UL_TOTAL, 0, 0, 5 },
    { "Drive Time",        "+5% all listeners",  1e7,  2e6,  FU_GLOBAL_PCT, UL_TOTAL, 0, 0, 5 },
    { "Top 40 Countdown",  "+10% all listeners", 1e9,  2e8,  FU_GLOBAL_PCT, UL_TOTAL, 0, 0, 10 },
    { "Sweeps Week",       "+10% all listeners", 1e11, 2e10, FU_GLOBAL_PCT, UL_TOTAL, 0, 0, 10 },
    { "Award Season",      "+15% all listeners", 1e13, 2e12, FU_GLOBAL_PCT, UL_TOTAL, 0, 0, 15 },
    { "Cult Following",    "+15% all listeners", 1e15, 2e14, FU_GLOBAL_PCT, UL_TOTAL, 0, 0, 15 },
    { "Household Name",    "+20% all listeners", 1e17, 2e16, FU_GLOBAL_PCT, UL_TOTAL, 0, 0, 20 },
    { "Frequency of the Century", "+25% all listeners", 1e19, 2e18, FU_GLOBAL_PCT, UL_TOTAL, 0, 0, 25 },
    /* synergies: target +value% per source building; both need 15 */
    { "Intern Call Screeners",   "Intern DJs +5% per Call-in Line",     2.7e4,  15, FU_SYNERGY, UL_PAIR, FM_INTERN,  FM_CALLIN,  5 },
    { "Traffic Report Jingles",  "Jingle Machines +5% per Chopper",     3.5e7,  15, FU_SYNERGY, UL_PAIR, FM_JINGLE,  FM_CHOPPER, 5 },
    { "Listener Mailbag",        "Call-in Lines +5% per Podcast",       8.2e9,  15, FU_SYNERGY, UL_PAIR, FM_CALLIN,  FM_PODCAST, 5 },
    { "QR Code Stickers",        "Street Teams +5% per Streaming App",  1.3e11, 15, FU_SYNERGY, UL_PAIR, FM_STREET,  FM_STREAM,  5 },
    { "Relay Handshake",         "Towers +5% per Satellite",            5e8,    15, FU_SYNERGY, UL_PAIR, FM_TOWER,   FM_SAT,     5 },
    { "Algorithmic Traffic",     "Choppers +5% per Algorithm",          1.9e12, 15, FU_SYNERGY, UL_PAIR, FM_CHOPPER, FM_ALGO,    5 },
    { "Lunar Listening Parties", "Podcasts +5% per Moon Relay",         2.5e13, 15, FU_SYNERGY, UL_PAIR, FM_PODCAST, FM_MOON,    5 },
    { "Interdimensional Uplink", "Satellites +5% per Pirate Dimension", 3.5e14, 15, FU_SYNERGY, UL_PAIR, FM_SAT,     FM_PIRATE,  5 },
    /* Request Line: target 0 = rings more often, 1 = boosts last longer */
    { "Hold Music",        "Callers ring 25% more often",   7.7e4, 1,  FU_CALLER, UL_CALLERS, 0, 0, 1.25f },
    { "Call Waiting",      "Callers ring 25% more often",   7.7e7, 7,  FU_CALLER, UL_CALLERS, 0, 0, 1.25f },
    { "Extended Segment",  "Caller boosts last 50% longer", 7.7e9, 27, FU_CALLER, UL_CALLERS, 1, 0, 1.5f },
    /* fan club: scales with plaques */
    { "Fan Club Newsletter", "+0.2% listeners per plaque", 9e6,  10, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.002f },
    { "Fan Club Merch",      "+0.3% listeners per plaque", 9e8,  25, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.003f },
    { "Fan Convention",      "+0.4% listeners per plaque", 9e10, 50, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.004f },
    { "Fan Army",            "+0.5% listeners per plaque", 9e13, 75, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.005f },
    { "Fan Dimension",       "+0.6% listeners per plaque", 9e16, 100, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.006f },
    /* Signal Lock: raises the tap combo ceiling */
    { "Fine Tuning",         "Signal Lock tops out at x4 taps", 2e4, 500,  FU_COMBO, UL_TAPS, 0, 0, 1 },
    { "Phase Lock",          "Signal Lock tops out at x5 taps", 2e8, 5000, FU_COMBO, UL_TAPS, 0, 0, 1 },
    /* added in v3: always append */
    { "Signal Boost IX",     "Taps also earn 1% of your listeners/s", 5e25, 1e25, FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost X",      "Taps also earn 1% of your listeners/s", 5e28, 1e28, FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost XI",     "Taps also earn 1% of your listeners/s", 5e31, 1e31, FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Signal Boost XII",    "Taps also earn 1% of your listeners/s", 5e34, 1e34, FU_TAP_PCT, UL_TOTAL, 0, 0, 1 },
    { "Fan Mail Sorting",    "+0.1 per tap for each building you own", 1e5,  1000,   FU_TAP_BUILD, UL_TAPS, 0, 0, 0.1f },
    { "Request Hotkeys",     "+0.5 per tap for each building you own", 1e7,  3000,   FU_TAP_BUILD, UL_TAPS, 0, 0, 0.5f },
    { "Crowd Surfing",       "+5 per tap for each building you own",   1e9,  8000,   FU_TAP_BUILD, UL_TAPS, 0, 0, 5 },
    { "Stage Dive",          "+50 per tap for each building you own",  1e11, 15000,  FU_TAP_BUILD, UL_TAPS, 0, 0, 50 },
    { "Mosh Pit",            "+500 per tap for each building you own", 1e14, 30000,  FU_TAP_BUILD, UL_TAPS, 0, 0, 500 },
    { "Wall of Sound",       "+5,000 per tap for each building you own", 1e17, 60000, FU_TAP_BUILD, UL_TAPS, 0, 0, 5000 },
    { "Standing Ovation",    "+50,000 per tap for each building you own", 1e20, 100000, FU_TAP_BUILD, UL_TAPS, 0, 0, 5e4 },
    { "Encore of Encores",   "+500,000 per tap for each building you own", 1e23, 200000, FU_TAP_BUILD, UL_TAPS, 0, 0, 5e5 },
    { "Switchboard",         "Callers ring 25% more often",   7.7e11, 77,  FU_CALLER, UL_CALLERS, 0, 0, 1.25f },
    { "Call Queue",          "Caller boosts last 50% longer", 7.7e13, 150, FU_CALLER, UL_CALLERS, 1, 0, 1.5f },
    { "Hotline Hall of Fame","Callers ring 25% more often",   7.7e16, 300, FU_CALLER, UL_CALLERS, 0, 0, 1.25f },
    { "Perfect Pitch",       "Signal Lock tops out at x6 taps", 2e12, 20000,  FU_COMBO, UL_TAPS, 0, 0, 1 },
    { "Resonance",           "Signal Lock tops out at x7 taps", 2e16, 100000, FU_COMBO, UL_TAPS, 0, 0, 1 },
    { "Fan Galaxy", "+0.7% listeners per plaque", 9e19, 125, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.007f },
    { "Fan Cult", "+0.8% listeners per plaque", 9e22, 150, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.008f },
    { "Fan Nation", "+0.9% listeners per plaque", 9e25, 175, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.009f },
    { "Fan Planet", "+1% listeners per plaque", 9e28, 200, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.01f },
    { "Fan Empire", "+1.1% listeners per plaque", 9e31, 225, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.011f },
    { "Fan Hive Mind", "+1.2% listeners per plaque", 9e34, 250, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.012f },
    { "Fan Singularity", "+1.3% listeners per plaque", 9e37, 275, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.013f },
    { "Fan Religion", "+1.4% listeners per plaque", 9e40, 300, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.014f },
    { "Fan Universe", "+1.5% listeners per plaque", 9e43, 330, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.015f },
    { "Fan Everything", "+1.6% listeners per plaque", 9e46, 360, FU_FANCLUB, UL_PLAQUES, 0, 0, 0.016f },
};
#define FM_EXTRA_COUNT ((int)(sizeof fm_extra / sizeof fm_extra[0]))

/* Playlists (id FM_PLAYLIST_ID + k): +2..5% all listeners each, priced from
 * 1 million up past 1e65, each on sale once a quarter of its price has been
 * earned this broadcast. The long tail of the shop. */
#define FM_PLAYLIST_ID 600
#define FM_PLAYLIST_COUNT 240
static const char *const fm_playlist_name[FM_PLAYLIST_COUNT] = {
    "Late-Night Jazz Hour", "Late-Night Disco Set", "Late-Night Rock Block", "Late-Night Soul Session",
    "Late-Night Synthwave Mix", "Late-Night Country Roads", "Late-Night Hip-Hop Hour", "Late-Night Indie Hour",
    "Late-Night Metal Marathon", "Late-Night Pop Parade", "Sunrise Disco Set", "Sunrise Rock Block",
    "Sunrise Soul Session", "Sunrise Synthwave Mix", "Sunrise Country Roads", "Sunrise Hip-Hop Hour",
    "Sunrise Indie Hour", "Sunrise Metal Marathon", "Sunrise Pop Parade", "Sunrise Jazz Hour",
    "Rush Hour Rock Block", "Rush Hour Soul Session", "Rush Hour Synthwave Mix", "Rush Hour Country Roads",
    "Rush Hour Hip-Hop Hour", "Rush Hour Indie Hour", "Rush Hour Metal Marathon", "Rush Hour Pop Parade",
    "Rush Hour Jazz Hour", "Rush Hour Disco Set", "Lo-Fi Soul Session", "Lo-Fi Synthwave Mix",
    "Lo-Fi Country Roads", "Lo-Fi Hip-Hop Hour", "Lo-Fi Indie Hour", "Lo-Fi Metal Marathon",
    "Lo-Fi Pop Parade", "Lo-Fi Jazz Hour", "Lo-Fi Disco Set", "Lo-Fi Rock Block",
    "Deep Cut Synthwave Mix", "Deep Cut Country Roads", "Deep Cut Hip-Hop Hour", "Deep Cut Indie Hour",
    "Deep Cut Metal Marathon", "Deep Cut Pop Parade", "Deep Cut Jazz Hour", "Deep Cut Disco Set",
    "Deep Cut Rock Block", "Deep Cut Soul Session", "Throwback Country Roads", "Throwback Hip-Hop Hour",
    "Throwback Indie Hour", "Throwback Metal Marathon", "Throwback Pop Parade", "Throwback Jazz Hour",
    "Throwback Disco Set", "Throwback Rock Block", "Throwback Soul Session", "Throwback Synthwave Mix",
    "Summer Hip-Hop Hour", "Summer Indie Hour", "Summer Metal Marathon", "Summer Pop Parade",
    "Summer Jazz Hour", "Summer Disco Set", "Summer Rock Block", "Summer Soul Session",
    "Summer Synthwave Mix", "Summer Country Roads", "Rainy Day Indie Hour", "Rainy Day Metal Marathon",
    "Rainy Day Pop Parade", "Rainy Day Jazz Hour", "Rainy Day Disco Set", "Rainy Day Rock Block",
    "Rainy Day Soul Session", "Rainy Day Synthwave Mix", "Rainy Day Country Roads", "Rainy Day Hip-Hop Hour",
    "Road Trip Metal Marathon", "Road Trip Pop Parade", "Road Trip Jazz Hour", "Road Trip Disco Set",
    "Road Trip Rock Block", "Road Trip Soul Session", "Road Trip Synthwave Mix", "Road Trip Country Roads",
    "Road Trip Hip-Hop Hour", "Road Trip Indie Hour", "Midnight Pop Parade", "Midnight Jazz Hour",
    "Midnight Disco Set", "Midnight Rock Block", "Midnight Soul Session", "Midnight Synthwave Mix",
    "Midnight Country Roads", "Midnight Hip-Hop Hour", "Midnight Indie Hour", "Midnight Metal Marathon",
    "Acoustic Jazz Hour", "Acoustic Disco Set", "Acoustic Rock Block", "Acoustic Soul Session",
    "Acoustic Synthwave Mix", "Acoustic Country Roads", "Acoustic Hip-Hop Hour", "Acoustic Indie Hour",
    "Acoustic Metal Marathon", "Acoustic Pop Parade", "Unplugged Disco Set", "Unplugged Rock Block",
    "Unplugged Soul Session", "Unplugged Synthwave Mix", "Unplugged Country Roads", "Unplugged Hip-Hop Hour",
    "Unplugged Indie Hour", "Unplugged Metal Marathon", "Unplugged Pop Parade", "Unplugged Jazz Hour",
    "Extended Rock Block", "Extended Soul Session", "Extended Synthwave Mix", "Extended Country Roads",
    "Extended Hip-Hop Hour", "Extended Indie Hour", "Extended Metal Marathon", "Extended Pop Parade",
    "Extended Jazz Hour", "Extended Disco Set", "Live Soul Session", "Live Synthwave Mix",
    "Live Country Roads", "Live Hip-Hop Hour", "Live Indie Hour", "Live Metal Marathon",
    "Live Pop Parade", "Live Jazz Hour", "Live Disco Set", "Live Rock Block",
    "Bootleg Synthwave Mix", "Bootleg Country Roads", "Bootleg Hip-Hop Hour", "Bootleg Indie Hour",
    "Bootleg Metal Marathon", "Bootleg Pop Parade", "Bootleg Jazz Hour", "Bootleg Disco Set",
    "Bootleg Rock Block", "Bootleg Soul Session", "Remastered Country Roads", "Remastered Hip-Hop Hour",
    "Remastered Indie Hour", "Remastered Metal Marathon", "Remastered Pop Parade", "Remastered Jazz Hour",
    "Remastered Disco Set", "Remastered Rock Block", "Remastered Soul Session", "Remastered Synthwave Mix",
    "Golden Hip-Hop Hour", "Golden Indie Hour", "Golden Metal Marathon", "Golden Pop Parade",
    "Golden Jazz Hour", "Golden Disco Set", "Golden Rock Block", "Golden Soul Session",
    "Golden Synthwave Mix", "Golden Country Roads", "Cosmic Indie Hour", "Cosmic Metal Marathon",
    "Cosmic Pop Parade", "Cosmic Jazz Hour", "Cosmic Disco Set", "Cosmic Rock Block",
    "Cosmic Soul Session", "Cosmic Synthwave Mix", "Cosmic Country Roads", "Cosmic Hip-Hop Hour",
    "Underground Metal Marathon", "Underground Pop Parade", "Underground Jazz Hour", "Underground Disco Set",
    "Underground Rock Block", "Underground Soul Session", "Underground Synthwave Mix", "Underground Country Roads",
    "Underground Hip-Hop Hour", "Underground Indie Hour", "Chart-Topping Pop Parade", "Chart-Topping Jazz Hour",
    "Chart-Topping Disco Set", "Chart-Topping Rock Block", "Chart-Topping Soul Session", "Chart-Topping Synthwave Mix",
    "Chart-Topping Country Roads", "Chart-Topping Hip-Hop Hour", "Chart-Topping Indie Hour", "Chart-Topping Metal Marathon",
    "Slow Jam Jazz Hour", "Slow Jam Disco Set", "Slow Jam Rock Block", "Slow Jam Soul Session",
    "Slow Jam Synthwave Mix", "Slow Jam Country Roads", "Slow Jam Hip-Hop Hour", "Slow Jam Indie Hour",
    "Slow Jam Metal Marathon", "Slow Jam Pop Parade", "Stadium Disco Set", "Stadium Rock Block",
    "Stadium Soul Session", "Stadium Synthwave Mix", "Stadium Country Roads", "Stadium Hip-Hop Hour",
    "Stadium Indie Hour", "Stadium Metal Marathon", "Stadium Pop Parade", "Stadium Jazz Hour",
    "Basement Rock Block", "Basement Soul Session", "Basement Synthwave Mix", "Basement Country Roads",
    "Basement Hip-Hop Hour", "Basement Indie Hour", "Basement Metal Marathon", "Basement Pop Parade",
    "Basement Jazz Hour", "Basement Disco Set", "Holiday Soul Session", "Holiday Synthwave Mix",
    "Holiday Country Roads", "Holiday Hip-Hop Hour", "Holiday Indie Hour", "Holiday Metal Marathon",
    "Holiday Pop Parade", "Holiday Jazz Hour", "Holiday Disco Set", "Holiday Rock Block",
};
#define FM_UPG_COUNT (FM_PLAYLIST_ID + FM_PLAYLIST_COUNT)

/* Syndication perks: cost in Signal Rights, up to two parents */
enum { PK_CONTRACT, PK_SEED1, PK_SEED2, PK_SEED3, PK_BRAND1, PK_BRAND2,
       PK_EVER1, PK_EVER2, PK_HOT1, PK_HOT2, PK_STAFF, PK_ARCHIVE,
       PK_SISTER, PK_TAPLEGEND, PK_CHARTS, PK_GOLDEN, PK_PRIME,
       /* v3 */
       PK_SEED4, PK_SEED5, PK_BRAND3, PK_BRAND4, PK_EVER3, PK_HOT3, PK_STAFF2, PK_ARCHIVE2,
       PK_CHARTS2, PK_CHARTS3, PK_CHARTS4, PK_CHARTS5, PK_CHARTS6, PK_GOLDEN2, PK_TROPHY,
       PK_TAPLEGEND2, PK_PRIME2, PK_FORMAT, PK_COUNT };
struct fm_perk { const char *name, *desc; uint32_t cost; int8_t p1, p2; };
static const struct fm_perk fm_perks[PK_COUNT] = {
    { "Affiliate Contract",   "+5% all listeners. Opens the deals",       1,   -1, -1 },
    { "Seed Money I",         "Start each broadcast with 1,000",          3,   PK_CONTRACT, -1 },
    { "Seed Money II",        "Start each broadcast with 1 million",      30,  PK_SEED1, -1 },
    { "Seed Money III",       "Start each broadcast with 1 billion",      300, PK_SEED2, -1 },
    { "Brand Recognition I",  "Buildings cost 5% less",                   5,   PK_CONTRACT, -1 },
    { "Brand Recognition II", "Buildings cost another 5% less",           50,  PK_BRAND1, -1 },
    { "Evergreen Library I",  "Offline earnings for up to 3 days",        3,   PK_CONTRACT, -1 },
    { "Evergreen Library II", "Offline earnings for up to 7 days",        30,  PK_EVER1, -1 },
    { "Hotline Legends I",    "Callers ring 15% more often",              7,   PK_CONTRACT, -1 },
    { "Hotline Legends II",   "Callers wait twice as long, boosts +20%",  77,  PK_HOT1, -1 },
    { "Retained Staff",       "Start with 10 Intern DJs, 5 Jingles",      25,  PK_SEED1, PK_BRAND1 },
    { "Archive Tapes",        "Start with every Fresh Batteries upgrade", 100, PK_STAFF, -1 },
    { "Sister Stations",      "+2% listeners per syndication (max 50)",   15,  PK_CONTRACT, -1 },
    { "Tap Legend",           "Taps also earn 2% of your listeners/s",    10,  PK_CONTRACT, -1 },
    { "Syndicated Charts",    "+15% all listeners",                       50,  PK_SISTER, PK_TAPLEGEND },
    { "Golden Mic Award",     "+25% listeners and taps",                  200, PK_CHARTS, PK_HOT1 },
    { "Network Prime Time",   "Each Signal Right gives +2%, not +1%",     500, PK_GOLDEN, -1 },
    { "Seed Money IV",        "Start each broadcast with 1 trillion",     3000,  PK_SEED3, -1 },
    { "Seed Money V",         "Start each broadcast with 1 quadrillion",  30000, PK_SEED4, -1 },
    { "Brand Recognition III","Buildings cost another 5% less",           500,   PK_BRAND2, -1 },
    { "Brand Recognition IV", "Buildings cost another 5% less",           5000,  PK_BRAND3, -1 },
    { "Evergreen Library III","Offline earnings for up to 30 days",       300,   PK_EVER2, -1 },
    { "Hotline Legends III",  "Callers ring another 30% more often",      777,   PK_HOT2, -1 },
    { "Retained Staff II",    "Start with 25 of the first five buildings", 250,  PK_STAFF, -1 },
    { "Archive Tapes II",     "Also start with every Better Cables upgrade", 1000, PK_ARCHIVE, -1 },
    { "Syndicated Charts II", "+25% all listeners",                       500,   PK_CHARTS, -1 },
    { "Syndicated Charts III","+25% all listeners",                       5000,  PK_CHARTS2, -1 },
    { "Syndicated Charts IV", "+50% all listeners",                       50000, PK_CHARTS3, -1 },
    { "Syndicated Charts V",  "+50% all listeners",                       500000, PK_CHARTS4, -1 },
    { "Syndicated Charts VI", "Listeners x2",                             5000000, PK_CHARTS5, -1 },
    { "Golden Mic Award II",  "+50% listeners per tap",                   2000,  PK_GOLDEN, -1 },
    { "Trophy Cabinet",       "+1% all listeners per 5 plaques",          150,   PK_SISTER, -1 },
    { "Tap Legend II",        "Taps also earn another 3% of listeners/s", 100,   PK_TAPLEGEND, -1 },
    { "Network Prime Time II","Each Signal Right gives +3%",              50000, PK_PRIME, -1 },
    { "Format Flexibility",   "Switch formats every 2 minutes",           40,    PK_CONTRACT, -1 },
};

/* Station Formats: one at a time, a trade-off each. Unlocked by the first
 * Street Team or any syndication; switching again waits FM_FORMAT_WAIT. */
enum { FMT_NONE, FMT_TOP40, FMT_TALK, FMT_LATE, FMT_HOTLINE, FMT_COUNT };
#define FM_FORMAT_WAIT 600
struct fm_format { const char *name, *desc; };
static const struct fm_format fm_formats[FMT_COUNT] = {
    { "No format",  "Pick a format for your station" },
    { "Top 40",     "+20% listeners/s" },
    { "Talk",       "Taps x3, but listeners/s -10%" },
    { "Late Night", "Offline earnings last twice as long" },
    { "Hotline",    "Callers ring twice as often, listeners/s -10%" },
};

/* Plaques (achievements) are generated from ranges */
#define FM_PL_TOTAL   0    /* 60: 1e2 .. 1e61 listeners ever */
#define FM_PL_BUILD   60   /* 260: each building x 13 counts */
#define FM_PL_TAPS    320  /* 8 */
#define FM_PL_SYND    328  /* 8 */
#define FM_PL_CALL    336  /* 8 */
#define FM_PL_UPG     344  /* 16: upgrades owned at once */
#define FM_PL_RIGHTS  360  /* 12 */
#define FM_PLAQUE_COUNT 372
#define FM_PL_BUILD_N 13
static const uint16_t fm_pl_build[FM_PL_BUILD_N] = { 1, 50, 100, 150, 200, 250, 300, 350, 400, 450, 500, 550, 600 };
static const uint32_t fm_pl_taps[8] = { 100, 1000, 5000, 10000, 50000, 100000, 500000, 1000000 };
static const uint32_t fm_pl_synd[8] = { 1, 3, 5, 10, 25, 50, 100, 250 };
static const uint32_t fm_pl_call[8] = { 1, 7, 27, 77, 150, 300, 777, 1500 };
static const uint32_t fm_pl_upg[16] = { 10, 25, 50, 75, 100, 150, 200, 250, 300, 350, 400, 450, 500, 550, 600, 700 };
static const uint32_t fm_pl_rights[12] = { 1, 10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000, 1000000, 10000000 };

/* ----------------------------------------------------------- helpers */
static bool fm_bit(const uint32_t *w, int i) { return (w[i >> 5] >> (i & 31)) & 1; }
static void fm_set(uint32_t *w, int i) { w[i >> 5] |= 1u << (i & 31); }
static bool sl_fm_has(const struct sl_fm_state *s, int u) { return fm_bit(s->upgrades, u); }
static bool sl_fm_perk(const struct sl_fm_state *s, int p) { return fm_bit(s->perks, p); }

static int fm_popcount(const uint32_t *w, int words)
{
    int n = 0;
    for (int i = 0; i < words; i++)
        for (uint32_t v = w[i]; v; v &= v - 1) n++;
    return n;
}
static int sl_fm_plaque_count(const struct sl_fm_state *s) { return fm_popcount(s->plaques, FM_ACH_WORDS); }

static void sl_fm_fmt(double v, char *out, size_t n)
{
    static const char *const names[] = { "million", "billion", "trillion", "quadrillion",
        "quintillion", "sextillion", "septillion", "octillion", "nonillion", "decillion",
        "undecillion", "duodecillion", "tredecillion", "quattuordecillion", "quindecillion" };
    if (!(v >= 0)) v = 0;
    if (v < 10 && floor(v * 10) != floor(v) * 10) { snprintf(out, n, "%.1f", floor(v * 10) / 10); return; }
    if (v < 1e6)
    {
        unsigned long x = (unsigned long)v;
        if (x >= 1000) snprintf(out, n, "%lu,%03lu", x / 1000, x % 1000);
        else snprintf(out, n, "%lu", x);
        return;
    }
    double m = v / 1e6;
    int e = 0;
    while (m >= 1000 && e < 14) { m /= 1000; e++; }
    if (m >= 1000) snprintf(out, n, "%.3e", v);
    else snprintf(out, n, "%.3f %s", floor(m * 1000) / 1000, names[e]);
}

/* Fills `u` with upgrade `i`; false for an unused id. Ids:
 *   FM_TIER_ID(b, t)     building tiers          0..383
 *   FM_INTERN_ID + b     intern specialisations  385..407
 *   FM_PAIR_ID + k       crossovers              408..479
 *   FM_UPG_EXTRA + k     the fm_extra table      480..599
 *   FM_PLAYLIST_ID + k   playlists               600..839 */
static bool sl_fm_upgrade(int i, struct fm_upgrade *u)
{
    static const char *const pct_desc[4] = { "+2% all listeners", "+3% all listeners",
                                             "+4% all listeners", "+5% all listeners" };
    if (i < 0 || i >= FM_UPG_COUNT) return false;
    u->source = 0;
    if (i < FM_INTERN_ID)
    {
        int b = i / FM_TIER_STRIDE, t = i % FM_TIER_STRIDE;
        if (b >= FM_B_COUNT || t >= FM_TIERS) return false;
        u->name = fm_tier_name[t];
        u->desc = fm_tier_desc[b];
        u->cost = fm_buildings[b].base_cost * fm_tier_cost[t];
        u->need = fm_tier_need[t];
        u->kind = FU_BUILDING; u->unlock = UL_OWN; u->target = (uint8_t)b;
        u->value = 2;
        return true;
    }
    if (i < FM_PAIR_ID)
    {
        int b = i - FM_INTERN_ID;
        if (b < 1 || b >= FM_B_COUNT) return false;
        u->name = fm_intern_name[b];
        u->desc = fm_intern_desc[b];
        u->cost = fm_buildings[b].base_cost * 100;
        u->need = 15;
        u->kind = FU_INTERN; u->unlock = UL_PAIR; u->target = (uint8_t)b; u->source = FM_INTERN;
        u->value = (float)b;   /* Intern DJs per 1% */
        return true;
    }
    if (i < FM_UPG_EXTRA)
    {
        int k = i - FM_PAIR_ID;
        if (k >= FM_PAIR_COUNT) return false;
        const struct fm_pair *pr = &fm_pairs[k];
        u->name = pr->name;
        u->desc = pr->desc;
        u->cost = fm_buildings[pr->source].base_cost * 40;
        u->need = 15;
        u->kind = FU_SYNERGY; u->unlock = UL_PAIR; u->target = pr->target; u->source = pr->source;
        u->value = 5;
        return true;
    }
    if (i < FM_PLAYLIST_ID)
    {
        int k = i - FM_UPG_EXTRA;
        if (k >= FM_EXTRA_COUNT) return false;
        const struct fm_upg_def *d = &fm_extra[k];
        u->name = d->name;
        u->desc = d->desc;
        u->cost = d->cost * FM_EXTRA_PRICE; u->need = d->need;
        u->kind = d->kind; u->unlock = d->unlock; u->target = d->target; u->source = d->source;
        u->value = d->value;
        return true;
    }
    int k = i - FM_PLAYLIST_ID, tier = k / 60;
    u->name = fm_playlist_name[k];
    u->desc = pct_desc[tier];
    u->cost = 1e6 * pow(10, k * 0.26);
    u->need = u->cost / 4;
    u->kind = FU_GLOBAL_PCT; u->unlock = UL_TOTAL; u->target = 0;
    u->value = (float)(2 + tier);
    return true;
}
/* How many upgrades exist, for "x of y" */
static int sl_fm_upgrade_total(void)
{
    static int total;
    struct fm_upgrade u;
    if (!total)
        for (int i = 0; i < FM_UPG_COUNT; i++) total += sl_fm_upgrade(i, &u);
    return total;
}

static bool sl_fm_unlocked(const struct sl_fm_state *s, const struct fm_upgrade *u)
{
    switch (u->unlock)
    {
    case UL_OWN:     return s->owned[u->target] >= u->need;
    case UL_TOTAL:   return s->run_total >= u->need;
    case UL_TAPS:    return s->taps >= u->need;
    case UL_PLAQUES: return sl_fm_plaque_count(s) >= u->need;
    case UL_CALLERS: return s->callers >= u->need;
    case UL_PAIR:    return s->owned[u->target] >= u->need && s->owned[u->source] >= u->need;
    }
    return false;
}

/* 0..1: how close a locked upgrade is to going on sale */
static double sl_fm_unlock_progress(const struct sl_fm_state *s, const struct fm_upgrade *u)
{
    double have, need = u->need > 0 ? u->need : 1;
    switch (u->unlock)
    {
    case UL_OWN:     have = s->owned[u->target]; break;
    case UL_TOTAL:   have = s->run_total; break;
    case UL_TAPS:    have = s->taps; break;
    case UL_PLAQUES: have = sl_fm_plaque_count(s); break;
    case UL_CALLERS: have = s->callers; break;
    default:
    {
        double a = s->owned[u->target], b = s->owned[u->source];
        have = a < b ? a : b;
    }
    }
    double f = have / need;
    return f > 1 ? 1 : f < 0 ? 0 : f;
}
/* What unlocks it, e.g. "At 50 Intern DJs" */
static void sl_fm_unlock_text(const struct fm_upgrade *u, char *out, size_t n)
{
    char v[40];
    switch (u->unlock)
    {
    case UL_OWN:
        snprintf(out, n, "Unlocks at %.0f %s", u->need, u->need > 1 ? fm_buildings[u->target].plural : fm_buildings[u->target].name);
        break;
    case UL_TOTAL:
        sl_fm_fmt(u->need, v, sizeof v);
        snprintf(out, n, "Unlocks after %s listeners this broadcast", v);
        break;
    case UL_TAPS:
        sl_fm_fmt(u->need, v, sizeof v);
        snprintf(out, n, "Unlocks at %s taps", v);
        break;
    case UL_PLAQUES:  snprintf(out, n, "Unlocks at %.0f plaques", u->need); break;
    case UL_CALLERS:  snprintf(out, n, "Unlocks after %.0f callers answered", u->need); break;
    default:
        snprintf(out, n, "Unlocks at %.0f %s and %.0f %s", u->need, fm_buildings[u->target].plural,
                 u->need, fm_buildings[u->source].plural);
    }
}

/* Rights earned for a lifetime total: first at FM_RIGHT_BASE (a trillion, as
 * in Cookie Clicker), then cubic. tests/radio_test.c's bot reports the time. */
#define FM_RIGHT_BASE 1e12
static uint32_t sl_fm_rights_for(double all_total)
{
    double r = floor(cbrt(all_total / FM_RIGHT_BASE));
    return r >= 4e9 ? 4000000000u : (uint32_t)r;
}
/* 0..1 progress from the last whole Right to the next */
static double sl_fm_right_progress(double all_total)
{
    double r = cbrt(all_total / FM_RIGHT_BASE);
    return r - floor(r);
}
static uint32_t sl_fm_pending(const struct sl_fm_state *s)
{
    uint32_t r = sl_fm_rights_for(s->all_total);
    return r > s->rights ? r - s->rights : 0;
}

static double sl_fm_building_cost(const struct sl_fm_state *s, int b, unsigned k)
{
    double disc = 1;
    if (sl_fm_perk(s, PK_BRAND1)) disc -= 0.05;
    if (sl_fm_perk(s, PK_BRAND2)) disc -= 0.05;
    if (sl_fm_perk(s, PK_BRAND3)) disc -= 0.05;
    if (sl_fm_perk(s, PK_BRAND4)) disc -= 0.05;
    double first = disc * fm_buildings[b].base_cost * pow(1.15, s->owned[b]);
    return k == 1 ? first : first * (pow(1.15, k) - 1) / 0.15;
}
static unsigned sl_fm_max_afford(const struct sl_fm_state *s, int b)
{
    double first = sl_fm_building_cost(s, b, 1);
    if (s->listeners < first) return 0;
    double k = floor(log(s->listeners * 0.15 / first + 1) / log(1.15));
    unsigned room = FM_MAX_OWNED - s->owned[b];
    unsigned n = k > room ? room : (unsigned)k;
    while (n && sl_fm_building_cost(s, b, n) > s->listeners * (1 + 1e-12)) n--;   /* float fuzz */
    return n;
}

static bool sl_fm_formats_open(const struct sl_fm_state *s) { return s->owned[FM_STREET] || s->syndications; }
/* Seconds until the format can change again (0 = now) */
static uint32_t sl_fm_format_wait(const struct sl_fm_state *s, uint32_t now)
{
    uint32_t wait = sl_fm_perk(s, PK_FORMAT) ? 120 : FM_FORMAT_WAIT;
    if (!s->format || now < s->format_since || now - s->format_since >= wait) return 0;
    return wait - (now - s->format_since);
}
static bool sl_fm_set_format(struct sl_fm_state *s, uint32_t f, uint32_t now)
{
    if (f == FMT_NONE || f >= FMT_COUNT || f == s->format || !sl_fm_formats_open(s) || sl_fm_format_wait(s, now))
        return false;
    s->format = f;
    s->format_since = now;
    return true;
}
static uint32_t sl_fm_offline_cap(const struct sl_fm_state *s)
{
    uint32_t cap = sl_fm_perk(s, PK_EVER3) ? 30 * 86400 : sl_fm_perk(s, PK_EVER2) ? 7 * 86400
                 : sl_fm_perk(s, PK_EVER1) ? 3 * 86400 : 86400;
    return s->format == FMT_LATE ? cap * 2 : cap;
}
/* Seconds between callers is the base range divided by this */
static double sl_fm_caller_rate(const struct sl_fm_state *s)
{
    double r = sl_fm_perk(s, PK_HOT1) ? 1.15 : 1;
    if (sl_fm_perk(s, PK_HOT3)) r *= 1.3;
    if (s->format == FMT_HOTLINE) r *= 2;
    struct fm_upgrade u;
    for (int i = FM_UPG_EXTRA; i < FM_UPG_COUNT; i++)
        if (sl_fm_has(s, i) && sl_fm_upgrade(i, &u) && u.kind == FU_CALLER && u.target == 0) r *= u.value;
    return r;
}
static double sl_fm_boost_len(const struct sl_fm_state *s)
{
    double r = sl_fm_perk(s, PK_HOT2) ? 1.2 : 1;
    struct fm_upgrade u;
    for (int i = FM_UPG_EXTRA; i < FM_UPG_COUNT; i++)
        if (sl_fm_has(s, i) && sl_fm_upgrade(i, &u) && u.kind == FU_CALLER && u.target == 1) r *= u.value;
    return r;
}

/* Signal Lock: quick taps fill a 0..1 combo meter that multiplies taps
 * up to this ceiling; it drains when the player stops. */
#define FM_COMBO_STEP  0.06
#define FM_COMBO_DRAIN 0.5    /* per second, after FM_COMBO_IDLE ms */
#define FM_COMBO_IDLE  600
static double sl_fm_combo_cap(const struct sl_fm_state *s)
{
    double cap = 3;
    struct fm_upgrade u;
    for (int i = FM_UPG_EXTRA; i < FM_UPG_COUNT; i++)
        if (sl_fm_has(s, i) && sl_fm_upgrade(i, &u) && u.kind == FU_COMBO) cap += u.value;
    return cap;
}
static double sl_fm_combo_mult(const struct sl_fm_state *s, double combo)
{
    if (combo < 0) combo = 0;
    if (combo > 1) combo = 1;
    return 1 + combo * (sl_fm_combo_cap(s) - 1);
}
/* Next building count that unlocks a x2, or 0 when all are reached */
static unsigned sl_fm_next_tier(const struct sl_fm_state *s, int b)
{
    for (int t = 0; t < FM_TIERS; t++)
        if (s->owned[b] < fm_tier_need[t]) return fm_tier_need[t];
    return 0;
}

/* What the upgrades bought this broadcast add up to, for the Owned view */
struct fm_summary {
    double tap_flat, tap_pct, tap_build, global_pct, fan, combo_cap, caller_rate, boost_len;
    double bmul[FM_B_MAX];
    int tiers[FM_B_MAX], owned_count;
};
static void sl_fm_summary(const struct sl_fm_state *s, struct fm_summary *m)
{
    memset(m, 0, sizeof *m);
    m->fan = m->caller_rate = m->boost_len = 1;
    m->combo_cap = sl_fm_combo_cap(s);
    for (int b = 0; b < FM_B_MAX; b++) m->bmul[b] = 1;
    int plaques = sl_fm_plaque_count(s);
    struct fm_upgrade u;
    for (int i = 0; i < FM_UPG_COUNT; i++)
    {
        if (!sl_fm_has(s, i) || !sl_fm_upgrade(i, &u)) continue;
        m->owned_count++;
        switch (u.kind)
        {
        case FU_BUILDING:   m->bmul[u.target] *= u.value; m->tiers[u.target]++; break;
        case FU_TAP_FLAT:   m->tap_flat += u.value; break;
        case FU_TAP_PCT:    m->tap_pct += u.value; break;
        case FU_GLOBAL_PCT: m->global_pct += u.value; break;
        case FU_SYNERGY:    m->bmul[u.target] *= 1 + u.value / 100 * s->owned[u.source]; break;
        case FU_INTERN:     m->bmul[FM_INTERN] *= 2; m->bmul[u.target] *= 1 + 0.01 * s->owned[FM_INTERN] / u.value; break;
        case FU_TAP_BUILD:  m->tap_build += u.value; break;
        case FU_FANCLUB:    m->fan *= 1 + plaques * u.value; break;
        case FU_CALLER:     if (u.target) m->boost_len *= u.value; else m->caller_rate *= u.value; break;
        }
    }
}

/* Rebuild the cached rates. Call after load, purchases, plaques. */
static void sl_fm_recalc(const struct sl_fm_state *s, struct sl_fm_rates *r)
{
    double bmul[FM_B_MAX], tap_flat = 1, tap_pct = 0, gpct = 0, fan = 1, tap_build = 0;
    unsigned buildings = 0;
    for (int b = 0; b < FM_B_COUNT; b++) buildings += s->owned[b];
    int plaques = sl_fm_plaque_count(s);
    for (int b = 0; b < FM_B_MAX; b++) bmul[b] = 1;
    for (int i = 0; i < FM_UPG_COUNT; i++)
    {
        struct fm_upgrade u;
        if (!sl_fm_has(s, i) || !sl_fm_upgrade(i, &u)) continue;
        switch (u.kind)
        {
        case FU_BUILDING:   bmul[u.target] *= u.value; break;
        case FU_TAP_FLAT:   tap_flat += u.value; break;
        case FU_TAP_PCT:    tap_pct += u.value / 100; break;
        case FU_GLOBAL_PCT: gpct += u.value / 100; break;
        case FU_SYNERGY:    bmul[u.target] *= 1 + u.value / 100 * s->owned[u.source]; break;
        case FU_INTERN:     bmul[FM_INTERN] *= 2; bmul[u.target] *= 1 + 0.01 * s->owned[FM_INTERN] / u.value; break;
        case FU_TAP_BUILD:  tap_build += u.value; break;
        case FU_FANCLUB:    fan *= 1 + plaques * u.value; break;
        }
    }
    double level = s->rights * (sl_fm_perk(s, PK_PRIME2) ? 0.03 : sl_fm_perk(s, PK_PRIME) ? 0.02 : 0.01);
    double perk = 1;
    if (sl_fm_perk(s, PK_CONTRACT)) perk *= 1.05;
    if (sl_fm_perk(s, PK_SISTER)) perk *= 1 + 0.02 * (s->syndications > 50 ? 50 : s->syndications);
    if (sl_fm_perk(s, PK_CHARTS)) perk *= 1.15;
    if (sl_fm_perk(s, PK_GOLDEN)) perk *= 1.25;
    if (sl_fm_perk(s, PK_TAPLEGEND)) tap_pct += 0.02;
    if (sl_fm_perk(s, PK_TAPLEGEND2)) tap_pct += 0.03;
    if (sl_fm_perk(s, PK_CHARTS2)) perk *= 1.25;
    if (sl_fm_perk(s, PK_CHARTS3)) perk *= 1.25;
    if (sl_fm_perk(s, PK_CHARTS4)) perk *= 1.5;
    if (sl_fm_perk(s, PK_CHARTS5)) perk *= 1.5;
    if (sl_fm_perk(s, PK_CHARTS6)) perk *= 2;
    if (sl_fm_perk(s, PK_TROPHY)) perk *= 1 + 0.01 * (plaques / 5);
    r->global = (1 + level) * (1 + gpct) * fan * perk;
    double fmt = s->format == FMT_TOP40 ? 1.2 : s->format == FMT_TALK || s->format == FMT_HOTLINE ? 0.9 : 1;
    r->per_sec = 0;
    for (int b = 0; b < FM_B_MAX; b++)
    {
        r->each[b] = b < FM_B_COUNT ? fm_buildings[b].base_lps * bmul[b] * r->global * fmt : 0;
        r->per_sec += s->owned[b] * r->each[b];
    }
    r->per_tap = (tap_flat + tap_build * buildings) * r->global * (sl_fm_perk(s, PK_GOLDEN) ? 1.25 : 1)
               * (sl_fm_perk(s, PK_GOLDEN2) ? 1.5 : 1) + r->per_sec * tap_pct;
    if (s->format == FMT_TALK) r->per_tap *= 3;
}

/* ------------------------------------------------------------ actions */
static void sl_fm_earn(struct sl_fm_state *s, double add)
{
    if (!(add > 0)) return;
    s->listeners = s->listeners + add < FM_TOP ? s->listeners + add : FM_TOP;
    s->run_total = s->run_total + add < FM_TOP ? s->run_total + add : FM_TOP;
    s->all_total = s->all_total + add < FM_TOP ? s->all_total + add : FM_TOP;
}
static double sl_fm_tap(struct sl_fm_state *s, const struct sl_fm_rates *r, double mult)
{
    double add = r->per_tap * mult;
    sl_fm_earn(s, add);
    s->tap_total = s->tap_total + add < FM_TOP ? s->tap_total + add : FM_TOP;
    if (s->taps < UINT32_MAX) s->taps++;
    return add;
}
/* k = 0 buys as many as affordable. Returns how many were bought. */
static unsigned sl_fm_buy_building(struct sl_fm_state *s, int b, unsigned k)
{
    if (b < 0 || b >= FM_B_COUNT) return 0;
    unsigned room = FM_MAX_OWNED - s->owned[b];
    if (!k) k = sl_fm_max_afford(s, b);
    if (k > room) k = room;
    if (!k) return 0;
    double cost = sl_fm_building_cost(s, b, k);
    if (cost > s->listeners * (1 + 1e-12)) return 0;
    s->listeners = cost < s->listeners ? s->listeners - cost : 0;
    s->owned[b] += (uint16_t)k;
    return k;
}
static bool sl_fm_buy_upgrade(struct sl_fm_state *s, int i)
{
    struct fm_upgrade u;
    if (!sl_fm_upgrade(i, &u) || sl_fm_has(s, i) || !sl_fm_unlocked(s, &u) || s->listeners < u.cost)
        return false;
    s->listeners -= u.cost;
    fm_set(s->upgrades, i);
    return true;
}
static bool sl_fm_perk_ready(const struct sl_fm_state *s, int p)
{
    const struct fm_perk *k = &fm_perks[p];
    return !sl_fm_perk(s, p) && (k->p1 < 0 || sl_fm_perk(s, k->p1)) && (k->p2 < 0 || sl_fm_perk(s, k->p2));
}
static bool sl_fm_buy_perk(struct sl_fm_state *s, int p)
{
    if (p < 0 || p >= PK_COUNT || !sl_fm_perk_ready(s, p) || s->rights - s->rights_spent < fm_perks[p].cost)
        return false;
    s->rights_spent += fm_perks[p].cost;
    fm_set(s->perks, p);
    return true;
}
static void sl_fm_start_run(struct sl_fm_state *s)
{
    if (sl_fm_perk(s, PK_SEED5)) s->listeners = 1e15;
    else if (sl_fm_perk(s, PK_SEED4)) s->listeners = 1e12;
    else if (sl_fm_perk(s, PK_SEED3)) s->listeners = 1e9;
    else if (sl_fm_perk(s, PK_SEED2)) s->listeners = 1e6;
    else if (sl_fm_perk(s, PK_SEED1)) s->listeners = 1e3;
    if (sl_fm_perk(s, PK_STAFF)) { s->owned[FM_INTERN] = 10; s->owned[FM_JINGLE] = 5; }
    if (sl_fm_perk(s, PK_STAFF2))
        for (int b = 0; b < 5; b++) if (s->owned[b] < 25) s->owned[b] = 25;
    for (int b = 0; b < FM_B_COUNT; b++)
    {
        if (sl_fm_perk(s, PK_ARCHIVE)) fm_set(s->upgrades, FM_TIER_ID(b, 0));
        if (sl_fm_perk(s, PK_ARCHIVE2)) fm_set(s->upgrades, FM_TIER_ID(b, 1));
    }
}
/* Prestige: trade this broadcast for Signal Rights */
static bool sl_fm_syndicate(struct sl_fm_state *s)
{
    uint32_t gain = sl_fm_pending(s);
    if (!gain) return false;
    s->rights += gain;
    s->listeners = s->run_total = 0;
    memset(s->owned, 0, sizeof s->owned);
    memset(s->upgrades, 0, sizeof s->upgrades);
    if (s->syndications < UINT32_MAX) s->syndications++;
    s->run_started = s->last_time;
    sl_fm_start_run(s);
    return true;
}
/* Wall clock catch-up, capped; a clock that went backwards pays nothing */
static double sl_fm_offline(struct sl_fm_state *s, const struct sl_fm_rates *r, uint32_t now)
{
    if (!now || !s->last_time || now <= s->last_time)
    { if (now) s->last_time = now; return 0; }
    uint32_t elapsed = now - s->last_time;
    if (elapsed > sl_fm_offline_cap(s)) elapsed = sl_fm_offline_cap(s);
    double add = r->per_sec * elapsed;
    sl_fm_earn(s, add);
    s->last_time = now;
    return add;
}

/* Sets newly earned plaques; returns how many, *first = index of one */
static int sl_fm_check_plaques(struct sl_fm_state *s, int *first)
{
    int got = 0, upgrades = fm_popcount(s->upgrades, FM_UPG_WORDS);
    double total = 100;
    for (int i = 0; i < FM_PLAQUE_COUNT; i++, total *= i <= FM_PL_BUILD ? 10 : 1)
    {
        if (fm_bit(s->plaques, i)) continue;
        bool ok;
        if (i < FM_PL_BUILD) ok = s->all_total >= total;
        else if (i < FM_PL_TAPS)
            ok = s->owned[(i - FM_PL_BUILD) / FM_PL_BUILD_N] >= fm_pl_build[(i - FM_PL_BUILD) % FM_PL_BUILD_N];
        else if (i < FM_PL_SYND) ok = s->taps >= fm_pl_taps[i - FM_PL_TAPS];
        else if (i < FM_PL_CALL) ok = s->syndications >= fm_pl_synd[i - FM_PL_SYND];
        else if (i < FM_PL_UPG) ok = s->callers >= fm_pl_call[i - FM_PL_CALL];
        else if (i < FM_PL_RIGHTS) ok = (uint32_t)upgrades >= fm_pl_upg[i - FM_PL_UPG];
        else ok = s->rights >= fm_pl_rights[i - FM_PL_RIGHTS];
        if (!ok) continue;
        fm_set(s->plaques, i);
        if (!got++ && first) *first = i;
    }
    return got;
}
static void sl_fm_plaque_name(int i, char *out, size_t n)
{
    char v[40];
    if (i < FM_PL_BUILD) { sl_fm_fmt(pow(10, 2 + i), v, sizeof v); snprintf(out, n, "%s listeners", v); }
    else if (i < FM_PL_TAPS)
    {
        int b = (i - FM_PL_BUILD) / FM_PL_BUILD_N, k = fm_pl_build[(i - FM_PL_BUILD) % FM_PL_BUILD_N];
        snprintf(out, n, "%d %s", k, k > 1 ? fm_buildings[b].plural : fm_buildings[b].name);
    }
    else if (i < FM_PL_SYND) { sl_fm_fmt(fm_pl_taps[i - FM_PL_TAPS], v, sizeof v); snprintf(out, n, "%s taps", v); }
    else if (i < FM_PL_CALL) snprintf(out, n, "Syndicated %u time%s", (unsigned)fm_pl_synd[i - FM_PL_SYND],
                                      fm_pl_synd[i - FM_PL_SYND] > 1 ? "s" : "");
    else if (i < FM_PL_UPG) snprintf(out, n, "%u caller%s answered", (unsigned)fm_pl_call[i - FM_PL_CALL],
                                     fm_pl_call[i - FM_PL_CALL] > 1 ? "s" : "");
    else if (i < FM_PL_RIGHTS) snprintf(out, n, "%u upgrades at once", (unsigned)fm_pl_upg[i - FM_PL_UPG]);
    else
    {
        sl_fm_fmt(fm_pl_rights[i - FM_PL_RIGHTS], v, sizeof v);
        snprintf(out, n, "%s Signal Right%s", v, fm_pl_rights[i - FM_PL_RIGHTS] > 1 ? "s" : "");
    }
}

/* Request Line outcomes */
enum { FM_CALL_HIT, FM_CALL_VIRAL, FM_CALL_PAYOLA };
static uint32_t sl_fm_rand(uint32_t *seed)
{
    uint32_t x = *seed ? *seed : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *seed = x;
}
static int sl_fm_call_pick(uint32_t *seed)
{
    uint32_t r = sl_fm_rand(seed) % 100;
    return r < 45 ? FM_CALL_HIT : r < 65 ? FM_CALL_VIRAL : FM_CALL_PAYOLA;
}
static double sl_fm_payola(const struct sl_fm_state *s, const struct sl_fm_rates *r)
{
    double a = s->listeners * 0.15, b = r->per_sec * 900;
    return (a < b ? a : b) + 13;
}

/* --------------------------------------------------------------- save */
static uint32_t sl_fm_checksum(const struct sl_fm_state *s)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(struct sl_fm_state, checksum); i++)
        hash = (hash ^ p[i]) * 16777619u;
    return hash;
}
static bool fm_num_ok(double v) { return isfinite(v) && v >= 0 && v <= FM_TOP; }
static bool sl_fm_valid(const struct sl_fm_state *s)
{
    if (s->magic != SL_FM_MAGIC || s->version != SL_FM_VERSION || s->size != sizeof *s ||
        s->checksum != sl_fm_checksum(s))
        return false;
    if (!fm_num_ok(s->listeners) || !fm_num_ok(s->run_total) || !fm_num_ok(s->all_total) ||
        !fm_num_ok(s->tap_total) || s->rights_spent > s->rights || s->format >= FMT_COUNT)
        return false;
    for (int b = 0; b < FM_B_MAX; b++)
        if (s->owned[b] > (b < FM_B_COUNT ? FM_MAX_OWNED : 0)) return false;
    struct fm_upgrade u;
    for (int i = 0; i < FM_UPG_WORDS * 32; i++) if (fm_bit(s->upgrades, i) && !sl_fm_upgrade(i, &u)) return false;
    for (int i = PK_COUNT; i < FM_PERK_WORDS * 32; i++) if (fm_bit(s->perks, i)) return false;
    for (int i = FM_PLAQUE_COUNT; i < FM_ACH_WORDS * 32; i++) if (fm_bit(s->plaques, i)) return false;
    return true;
}
static void sl_fm_seal(struct sl_fm_state *s) { s->checksum = sl_fm_checksum(s); }
static void sl_fm_init(struct sl_fm_state *s, uint32_t now)
{
    memset(s, 0, sizeof *s);   /* padding must be zero: it is checksummed */
    s->magic = SL_FM_MAGIC;
    s->version = SL_FM_VERSION;
    s->size = sizeof *s;
    s->last_time = s->run_started = now;
    sl_fm_seal(s);
}
#endif
