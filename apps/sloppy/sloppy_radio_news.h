/* Sloppy FM news ticker. Each headline airs once its condition holds:
 * a building index means "own at least one", the NW_ codes cover the
 * rest. Original writing; add freely. GPLv2+. */
#ifndef SLOPPY_RADIO_NEWS_H
#define SLOPPY_RADIO_NEWS_H
#include "sloppy_radio_state.h"

enum { NW_ALWAYS = -1, NW_SYND = -2, NW_TOP40 = -3, NW_TALK = -4, NW_LATE = -5, NW_HOTLINE = -6,
       NW_CALLER = -7, NW_TAPPER = -8, NW_BIG = -9, NW_PLAQUES = -10, NW_PRIME = -11, NW_EARLY = -12 };
struct fm_news { int8_t need; const char *text; };
static const struct fm_news fm_news[] = {
    { NW_ALWAYS, "Sloppy FM: the station nobody asked for, now on air." },
    { NW_ALWAYS, "Local man tunes in by accident, stays for the jingles." },
    { NW_ALWAYS, "Weather: cloudy with a strong chance of bangers." },
    { NW_ALWAYS, "Rival station calls Sloppy FM \"too sloppy\". Sloppy FM agrees." },
    { NW_ALWAYS, "Listener requests silence. Request denied." },
    { NW_ALWAYS, "Traffic: everyone is stuck listening to Sloppy FM. Great news." },
    { NW_ALWAYS, "Studio cat walks across the mixing desk. New genre invented." },
    { NW_ALWAYS, "Sloppy FM apologises for four hours of dead air, calls it \"ambient\"." },
    { NW_ALWAYS, "Horoscope: Sagittarius, turn it up. Everyone else, also turn it up." },
    { NW_ALWAYS, "Studio coffee machine now gets more airtime than the morning host." },
    { NW_ALWAYS, "Guess-the-song contest enters week six. The song was the jingle." },
    { NW_ALWAYS, "Scientists discover a new frequency. It is already playing Sloppy FM." },
    { NW_ALWAYS, "Grandma calls in to say the music is too loud. Turns it up anyway." },
    { NW_ALWAYS, "DJ forgets which button is the mic. Listeners hear 40 minutes of sandwich." },
    { NW_ALWAYS, "Sloppy FM wins award for Most Adequate Station, fourth year running." },
    { NW_ALWAYS, "Coming up after the break: the song you like. (It is not.)" },
    { NW_ALWAYS, "Station slogan changed from \"Pretty good radio\" to just \"Radio.\"" },
    { NW_ALWAYS, "Weather: the sun is out. Sloppy FM takes full credit." },
    { NW_ALWAYS, "Nine out of ten dentists have heard of Sloppy FM. The tenth is lying." },
    { NW_ALWAYS, "Morning host's alarm clock is, awkwardly, a different radio station." },
    { NW_ALWAYS, "Sloppy FM airs its own blooper reel by mistake. Ratings up 4%." },
    { NW_ALWAYS, "Lost and found: one (1) sense of rhythm, last seen near the studio." },
    { NW_ALWAYS, "Station clock is five minutes fast. News at 9 is now news at 8:55." },
    { NW_ALWAYS, "Survey: most listeners do not know what FM stands for. Neither do we." },
    { NW_ALWAYS, "Sloppy FM's hold music tops its own charts. Hold music asks for an agent." },
    { NW_ALWAYS, "Area man turns radio off and on again. Still Sloppy FM." },
    { NW_ALWAYS, "Breaking: nothing happened. More on this story as it develops." },
    { NW_ALWAYS, "Health tip: dancing in the car counts as cardio if the song is good enough." },
    { NW_EARLY,  "Sloppy FM currently has more microphones than listeners. Working on it." },
    { NW_EARLY,  "Station's first fan identified. It is the host's mum." },
    { NW_EARLY,  "Signal reaches the end of the street. Neighbours cautiously optimistic." },

    { FM_INTERN, "Interns ask to be paid in something other than exposure." },
    { FM_INTERN, "Intern plays the same song four times in a row. Ratings go up." },
    { FM_INTERN, "Intern discovers the fade-out button. Nothing will end abruptly again." },
    { FM_INTERN, "Intern promoted to Senior Intern. Pay stays at zero, title gets longer." },
    { FM_INTERN, "Intern asks where the vinyl goes. Is told: \"the past\"." },
    { FM_JINGLE, "Jingle stuck in the city's head for a third straight week." },
    { FM_JINGLE, "Scientists confirm the jingle is scientifically catchy." },
    { FM_JINGLE, "Jingle machine gains sentience, writes a jingle about itself." },
    { FM_JINGLE, "Local baby's first word is the station jingle. Parents concerned, proud." },
    { FM_JINGLE, "Petition to ban the jingle hits 10,000 names. Petition is sung to the jingle." },
    { FM_CALLIN, "Caller nine wins a toaster. Caller ten is furious." },
    { FM_CALLIN, "Call-in line jammed by people asking what the jingle is." },
    { FM_CALLIN, "Caller requests a song that \"goes hmm hmm hmmmm\". DJ nails it." },
    { FM_CALLIN, "Man calls to request a song, talks about his divorce for an hour." },
    { FM_CALLIN, "Caller on hold since Tuesday says the hold music is \"honestly great\"." },
    { FM_STREET, "Sloppy FM stickers found on the moon. Street team denies it." },
    { FM_STREET, "City bans stickers. Street team switches to chalk." },
    { FM_STREET, "Street team hands out 40,000 keychains. Town is now 60% keychain." },
    { FM_STREET, "Street team spotted putting up flyers on top of their own flyers." },
    { FM_TOWER,  "New tower visible from space, mostly to birds." },
    { FM_TOWER,  "Neighbours report hearing Sloppy FM through their fillings." },
    { FM_TOWER,  "Tower now so tall it has its own weather. The weather is also sloppy." },
    { FM_TOWER,  "Pigeons unionise on the transmitter tower, demand better reception." },
    { FM_CHOPPER, "Traffic chopper reports the traffic is mostly other choppers." },
    { FM_CHOPPER, "Heavy congestion near the traffic chopper's landing pad. Irony noted." },
    { FM_CHOPPER, "Chopper pilot admits he has no idea about traffic, just likes flying." },
    { FM_SAT,    "Uplink reaches three new continents and one confused penguin." },
    { FM_SAT,    "Satellite beams Sloppy FM to the space station. Astronauts want more jingle." },
    { FM_SAT,    "Satellite loses signal for a minute. Nobody noticed. Everybody noticed." },
    { FM_PODCAST, "Podcast network launches a podcast about its other podcasts." },
    { FM_PODCAST, "New podcast is two hosts saying \"no, totally\" for 90 minutes." },
    { FM_PODCAST, "Episode 400 opens with \"quick housekeeping\". Runs three hours." },
    { FM_PODCAST, "Podcast about podcasts gets its own podcast. Experts warn of a loop." },
    { FM_STREAM, "Streaming app adds a skip button. Nobody skips." },
    { FM_STREAM, "App adds dark mode, darker mode and \"midnight in a cave\" mode." },
    { FM_STREAM, "App asks \"Are you still listening?\". Entire country says yes." },
    { FM_ALGO,   "The Algorithm now recommends Sloppy FM to people who hate radio." },
    { FM_ALGO,   "The Algorithm asks for a raise. The Algorithm gets a raise." },
    { FM_ALGO,   "The Algorithm predicted this headline. It was right." },
    { FM_ALGO,   "The Algorithm now picks the songs, the hosts and, allegedly, lunch." },
    { FM_MOON,   "Tides now keep time with the Sloppy FM beat." },
    { FM_MOON,   "Moon Relay: the far side really likes the jingles." },
    { FM_MOON,   "Werewolves complain the moon \"has too many ads now\"." },
    { FM_MOON,   "Moon Relay broadcasts to a single crater. Crater loyalty at 100%." },
    { FM_PIRATE, "Sloppy FM airs in 11 realities. A 12th files a complaint." },
    { FM_PIRATE, "Alternate-reality Sloppy FM is slightly less sloppy. Nobody likes it." },
    { FM_PIRATE, "Listener calls in from yesterday to request tomorrow's hit." },
    { FM_PIRATE, "Reality 7 adopts the jingle as its national anthem." },
    { FM_HOLO,   "Holographic host wins Best Personality. Nobody checks if it has one." },
    { FM_HOLO,   "Fans line up to meet the hologram DJ. Hologram waves at the wrong line." },
    { FM_HOLO,   "Studio glitch shows seven DJs at once. Ratings up sevenfold." },
    { FM_PRISM,  "Frequency Prism splits the jingle into seven colours. All of them slap." },
    { FM_PRISM,  "Physicists confused why the prism only outputs bangers." },
    { FM_PREDICT, "Hit Predictor calls next summer's anthem. Songwriter hasn't been born yet." },
    { FM_PREDICT, "Hit Predictor predicts it will be switched off. It is wrong. Again." },
    { FM_PREDICT, "Record labels ask the Hit Predictor for a raise on their behalf." },
    { FM_ECHO,   "Echo Chamber reports 100% of listeners agree. Listeners agree." },
    { FM_ECHO,   "Echo chamber echo chamber echo chamber. Engineers investigating." },
    { FM_KERNEL, "Radio Kernel updated to v2.0. Now 40% more jingle per cycle." },
    { FM_KERNEL, "Kernel panic! It was just the drop. Everything is fine." },
    { FM_KERNEL, "Station now runs on 11 lines of code. Ten of them are the jingle." },
    { FM_MULTI,  "Multiverse Simulcast reaches the universe where cats run radio. Ratings: purr." },
    { FM_MULTI,  "Alternate you calls in to request the song you were about to request." },
    { FM_BRAIN,  "Brainwave Broadcaster skips the radio entirely. Radios feel left out." },
    { FM_BRAIN,  "Dreams now come with a jingle at the start. Nobody minds." },
    { FM_CLONE,  "Clone DJ asks the original DJ to take the weekend off. Forever." },
    { FM_CLONE,  "Seventeen Clone DJs host a panel on originality." },
    { FM_CLONE,  "It's you. On every frequency. Congratulations, you are the station." },

    { NW_SYND,   "Sloppy FM signs another affiliate. Nobody reads the contract." },
    { NW_SYND,   "Affiliates fight over the jingle rights. The jingle wins." },
    { NW_SYND,   "Network executives discover Sloppy FM, pretend they always knew." },
    { NW_SYND,   "Sloppy FM reruns now outrate new episodes of everything." },
    { NW_TOP40,  "Top 40 countdown is now 40 minutes of the same banger. No complaints." },
    { NW_TOP40,  "Chart analysts baffled: Sloppy FM's number one is the jingle, again." },
    { NW_TALK,   "Talk show host argues with himself for an hour, loses." },
    { NW_TALK,   "Three-hour debate on whether soup is a drink ends in a tie." },
    { NW_LATE,   "Late Night host whispers so softly ratings rise just to hear him." },
    { NW_LATE,   "Insomniacs vote Sloppy FM Best Station Between 2 and 4 AM." },
    { NW_HOTLINE, "Hotline so hot the phones are on fire. Please stop calling. Keep calling." },
    { NW_HOTLINE, "Studio installs a second phone. Then a third. Then a wall of phones." },
    { NW_CALLER, "Caller wins \"a song of their choice\". Chooses the jingle." },
    { NW_CALLER, "Payola scandal rocks Sloppy FM. Listeners unsure what payola is, approve." },
    { NW_TAPPER, "Knob technician warns DJs about \"excessive tuning\". Nobody listens." },
    { NW_TAPPER, "Studio replaces the tuning knob for the twelfth time this month." },
    { NW_BIG,    "Sloppy FM now has more listeners than some countries have people." },
    { NW_BIG,    "Economists add \"listening to Sloppy FM\" to GDP. GDP up 3%." },
    { NW_PLAQUES, "Studio runs out of wall for plaques. Plaques move to the ceiling." },
    { NW_PRIME,  "Network Prime Time: every hour is prime time now. Sleep is cancelled." },
};
#define FM_NEWS_COUNT ((int)(sizeof fm_news / sizeof fm_news[0]))

static bool sl_fm_news_ok(const struct sl_fm_state *s, int need, double per_sec)
{
    if (need >= 0) return need < FM_B_COUNT && s->owned[need];
    switch (need)
    {
    case NW_ALWAYS:  return true;
    case NW_SYND:    return s->syndications > 0;
    case NW_TOP40:   return s->format == FMT_TOP40;
    case NW_TALK:    return s->format == FMT_TALK;
    case NW_LATE:    return s->format == FMT_LATE;
    case NW_HOTLINE: return s->format == FMT_HOTLINE;
    case NW_CALLER:  return s->callers > 0;
    case NW_TAPPER:  return s->taps >= 500;
    case NW_BIG:     return per_sec >= 1e6;
    case NW_PLAQUES: return sl_fm_plaque_count(s) >= 20;
    case NW_PRIME:   return sl_fm_perk(s, PK_PRIME);
    case NW_EARLY:   return s->all_total < 1e4;
    }
    return false;
}
/* Picks a headline into `out`; stories about the station's own numbers
 * turn up now and then. */
static void sl_fm_news(const struct sl_fm_state *s, double per_sec, uint32_t *seed, char *out, size_t n)
{
    char v[40];
    uint32_t roll = sl_fm_rand(seed) % 12;
    if (roll == 0 && s->all_total >= 1e4)
    {
        sl_fm_fmt(s->all_total, v, sizeof v);
        snprintf(out, n, "UPDATE: Sloppy FM has now reached %s listeners, all told.", v);
        return;
    }
    if (roll == 1 && s->syndications)
    {
        snprintf(out, n, "BREAKING: Sloppy FM now syndicated in %u markets.", (unsigned)s->syndications * 12);
        return;
    }
    if (roll == 2 && s->taps >= 100)
    {
        sl_fm_fmt(s->taps, v, sizeof v);
        snprintf(out, n, "STATS: the tuning knob has been turned %s times. The knob is fine.", v);
        return;
    }
    int pick = 0, seen = 0;
    for (int i = 0; i < FM_NEWS_COUNT; i++)
        if (sl_fm_news_ok(s, fm_news[i].need, per_sec) && sl_fm_rand(seed) % ++seen == 0)
            pick = i;   /* reservoir pick */
    snprintf(out, n, "NEWS: %s", fm_news[pick].text);
}
#endif
