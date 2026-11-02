/***************************************************************************
 * Standard MIDI files (.mid, .midi, .rmi, .kar), played by a small synth
 * of GabagoolOS's own: a General MIDI family of instruments each drawn from
 * sines (libmidi/midi_tables.h, made by sloppy-dev/gen-midi-tables.py), and
 * a drum kit of swept tones and noise. No patch set is needed. Rockbox's
 * MIDI plugin (apps/plugins/midi) showed the way; this keeps no state
 * outside the codec, reads the file from the playback buffer, checks every
 * byte it reads against the file's end, and seeks.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#include "codeclib.h"
#include "libmidi/midi_tables.h"

CODEC_HEADER

#define MAX_TRACKS  64
#define MAX_VOICES  48
#define FILE_MAX    (768 * 1024)
#define OUT_FRAMES  512                  /* a multiple of MIDI_BLOCK */
#define TAIL_MAX    (MIDI_RATE * 3)      /* the sound after the last event */

/* ------------------------------------------------------------- the file */
struct track
{
    const uint8_t *start, *p, *end;
    uint32_t next;                       /* the tick of its next event */
    uint8_t status;                      /* running status */
    bool done;
};

static uint8_t *file_buf;
static size_t file_len;
static struct track tracks[MAX_TRACKS];
static int ntracks;
static unsigned tpq;                     /* ticks a quarter note; 0 with SMPTE time */
static unsigned smpte_tps;               /* ticks a second, SMPTE time */

/* ------------------------------------------------------------ the synth */
struct chan
{
    uint8_t prog, vol, expr, pan, sustain, range, rpn_msb, rpn_lsb;
    int bend;                            /* -8192 .. 8191 */
};

struct voice
{
    bool used, drum, held, releasing;
    uint8_t ch, note, stage;             /* stage: 0 attack, 1 decay, 2 release */
    int vel;
    uint32_t phase, step, age;
    const int16_t *wave;
    const struct midi_family *fam;
    int attack_left;
    int32_t env;                         /* Q30 */
    int32_t gl, gr;                      /* the gains the last block ended on, Q15 */
    /* drums */
    const struct midi_drum *dr;
    uint32_t tone_step;
    int32_t tone_lev, noise_lev;         /* Q30 */
    uint32_t lfsr;
    int32_t noise_prev;
};

static int16_t waves[16][257];
static int16_t sine_wave[257];
static struct chan chans[16];
static struct voice voices[MAX_VOICES];
static uint32_t voice_age;
static int32_t mix[OUT_FRAMES * 2];
static int32_t pcm[OUT_FRAMES * 2];

/* ---------------------------------------------------------- the timing */
static uint32_t tempo_us;                /* a quarter note */
static uint64_t spt_q16;                 /* samples a tick, Q16 */
static uint32_t cur_tick;
static uint64_t tick_frac;               /* Q16 samples into the current tick */
static uint64_t samples_done;
static uint32_t tail_done;               /* frames rendered after the last event */
static bool skipping;                    /* a seek running the events silently */

static int sine_at(unsigned i)
{
    i &= 255;
    if (i < 64) return sine_q[i];
    if (i < 128) return sine_q[128 - i];
    if (i < 192) return -sine_q[i - 128];
    return -sine_q[256 - i];
}

static void make_waves(void)
{
    for (int f = 0; f < 16; f++)
    {
        int32_t raw[256], peak = 1;
        for (int k = 0; k < 256; k++)
        {
            int32_t s = 0;
            for (int h = 0; h < 8; h++)
                s += fam[f].harm[h] * sine_at((unsigned)(h + 1) * k);
            raw[k] = s;
            if (s > peak) peak = s;
            if (-s > peak) peak = -s;
        }
        for (int k = 0; k < 256; k++)
            waves[f][k] = (int16_t)((int64_t)raw[k] * 30000 / peak);
        waves[f][256] = waves[f][0];
    }
    for (int k = 0; k < 256; k++)
        sine_wave[k] = (int16_t)(sine_at(k) * 30000 / 32767);
    sine_wave[256] = sine_wave[0];
}

static void set_tempo(uint32_t us)
{
    tempo_us = us ? us : 500000;
    if (tpq)
        spt_q16 = ((uint64_t)tempo_us * MIDI_RATE << 16) / ((uint64_t)tpq * 1000000);
    else
        spt_q16 = ((uint64_t)MIDI_RATE << 16) / smpte_tps;
    if (!spt_q16)
        spt_q16 = 1;
}

static void reset_channels(void)
{
    for (int c = 0; c < 16; c++)
    {
        struct chan *ch = &chans[c];
        ci->memset(ch, 0, sizeof *ch);
        ch->vol = 100;
        ch->expr = 127;
        ch->pan = 64;
        ch->range = 2;
        ch->rpn_msb = ch->rpn_lsb = 127;
    }
}

/* a pitch in 1/64 semitones to a phase step */
static uint32_t pitch_step(int p64)
{
    if (p64 < 0) p64 = 0;
    int oct = p64 / 768;
    if (oct > 10) return step_top[767];
    return step_top[p64 % 768] >> (10 - oct);
}

static uint32_t note_step(const struct chan *ch, int note)
{
    return pitch_step(note * 64 + ch->bend * ch->range * 64 / 8192);
}

/* ------------------------------------------------------------- notes */
static struct voice *voice_for(int ch, int note)
{
    struct voice *best = NULL;
    for (int i = 0; i < MAX_VOICES; i++)
    {
        struct voice *v = &voices[i];
        if (v->used && v->ch == ch && v->note == note && !v->drum)
            return v;                        /* the same key again */
        if (!v->used)
            return v;
        /* none free: the quietest, those letting go first */
        if (!best || (v->releasing && !best->releasing) ||
            (v->releasing == best->releasing && v->env + v->tone_lev + v->noise_lev <
                                                best->env + best->tone_lev + best->noise_lev))
            best = v;
    }
    return best;
}

static void note_on(int c, int note, int vel)
{
    if (skipping)
        return;
    struct chan *ch = &chans[c];
    struct voice *v;
    if (c == 9)
    {
        const struct midi_drum *d = &drum[note & 127];
        if (!d->tone && !d->noise)
            return;
        v = voice_for(c, -1);
        ci->memset(v, 0, sizeof *v);
        v->used = v->drum = true;
        v->ch = c;
        v->note = note;
        v->vel = vel;
        v->dr = d;
        v->tone_step = d->f0;
        v->tone_lev = (int32_t)d->tone << 15;
        v->noise_lev = (int32_t)d->noise << 15;
        v->lfsr = 0x1234567u ^ ((uint32_t)note << 8) ^ voice_age;
        v->age = ++voice_age;
        return;
    }
    v = voice_for(c, note);
    bool again = v->used && v->ch == c && v->note == note;
    int32_t env = again ? v->env : 0;    /* a key struck again starts from where it is: no click */
    int32_t gl = again ? v->gl : 0, gr = again ? v->gr : 0;
    uint32_t phase = again ? v->phase : 0;
    ci->memset(v, 0, sizeof *v);
    v->used = true;
    v->ch = c;
    v->note = note;
    v->vel = vel;
    v->fam = &fam[(ch->prog >> 3) & 15];
    v->wave = waves[(ch->prog >> 3) & 15];
    v->step = note_step(ch, note);
    v->phase = phase;
    v->env = env;
    v->gl = gl;
    v->gr = gr;
    v->attack_left = v->fam->attack;
    v->age = ++voice_age;
}

static void voice_release(struct voice *v)
{
    if (!v->drum)
    {
        v->releasing = true;
        v->stage = 2;
    }
}

static void note_off(int c, int note)
{
    if (c == 9 || skipping)
        return;
    for (int i = 0; i < MAX_VOICES; i++)
    {
        struct voice *v = &voices[i];
        if (v->used && !v->drum && v->ch == c && v->note == note && !v->releasing)
        {
            if (chans[c].sustain)
                v->held = true;
            else
                voice_release(v);
        }
    }
}

static void controller(int c, int num, int val)
{
    struct chan *ch = &chans[c];
    switch (num)
    {
        case 6:                          /* data entry: the bend range (RPN 0) */
            if (ch->rpn_msb == 0 && ch->rpn_lsb == 0)
                ch->range = val > 24 ? 24 : val;
            break;
        case 7:   ch->vol = val; break;
        case 10:  ch->pan = val; break;
        case 11:  ch->expr = val; break;
        case 64:
            ch->sustain = val >= 64;
            if (!ch->sustain)
                for (int i = 0; i < MAX_VOICES; i++)
                    if (voices[i].used && voices[i].ch == c && voices[i].held)
                    {
                        voices[i].held = false;
                        voice_release(&voices[i]);
                    }
            break;
        case 98: case 99: ch->rpn_msb = ch->rpn_lsb = 127; break;   /* NRPN: not ours */
        case 100: ch->rpn_lsb = val; break;
        case 101: ch->rpn_msb = val; break;
        case 120:                        /* all sound off */
            for (int i = 0; i < MAX_VOICES; i++)
                if (voices[i].ch == c)
                    voices[i].used = false;
            break;
        case 121:                        /* reset controllers */
            ch->expr = 127;
            ch->sustain = 0;
            ch->bend = 0;
            ch->rpn_msb = ch->rpn_lsb = 127;
            break;
        case 123:                        /* all notes off */
            for (int i = 0; i < MAX_VOICES; i++)
                if (voices[i].used && voices[i].ch == c)
                    voice_release(&voices[i]);
            break;
    }
}

static void pitch_bend(int c, int value)
{
    struct chan *ch = &chans[c];
    ch->bend = value - 8192;
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used && !voices[i].drum && voices[i].ch == c)
            voices[i].step = note_step(ch, voices[i].note);
}

/* ------------------------------------------------------------- events */
static uint32_t varlen(struct track *t)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
    {
        if (t->p >= t->end) { t->done = true; return 0; }
        uint8_t c = *t->p++;
        v = (v << 7) | (c & 0x7f);
        if (!(c & 0x80)) return v;
    }
    return v;
}

static void track_next(struct track *t)
{
    if (t->p >= t->end) { t->done = true; return; }
    uint32_t d = varlen(t);
    if (t->done) return;
    if (t->next + d < t->next) { t->done = true; return; }   /* past four billion ticks */
    t->next += d;
}

static void track_event(struct track *t)
{
    if (t->p >= t->end) { t->done = true; return; }
    uint8_t status = *t->p;
    if (status & 0x80)
    {
        t->p++;
        t->status = status < 0xf0 ? status : 0;   /* system messages end running status */
    }
    else if (!(status = t->status))
    {
        t->done = true;                  /* data with no status: not a MIDI file here */
        return;
    }
    if (status == 0xff)
    {
        if (t->p >= t->end) { t->done = true; return; }
        uint8_t type = *t->p++;
        uint32_t len = varlen(t);
        if (t->done || len > (uint32_t)(t->end - t->p)) { t->done = true; return; }
        if (type == 0x2f) { t->done = true; return; }
        if (type == 0x51 && len >= 3)
            set_tempo((uint32_t)t->p[0] << 16 | t->p[1] << 8 | t->p[2]);
        t->p += len;
    }
    else if (status == 0xf0 || status == 0xf7)
    {
        uint32_t len = varlen(t);
        if (t->done || len > (uint32_t)(t->end - t->p)) { t->done = true; return; }
        t->p += len;
    }
    else if (status >= 0xf0)
    {
        t->done = true;                  /* real-time and common messages do not belong in a file */
        return;
    }
    else
    {
        int n = (status & 0xe0) == 0xc0 ? 1 : 2;   /* program and pressure: one byte */
        if (t->end - t->p < n) { t->done = true; return; }
        int d1 = t->p[0] & 0x7f, d2 = n > 1 ? t->p[1] & 0x7f : 0;
        t->p += n;
        int c = status & 15;
        switch (status & 0xf0)
        {
            case 0x80: note_off(c, d1); break;
            case 0x90: d2 ? note_on(c, d1, d2) : note_off(c, d1); break;
            case 0xb0: controller(c, d1, d2); break;
            case 0xc0: chans[c].prog = d1; break;
            case 0xe0: pitch_bend(c, d2 << 7 | d1); break;
        }
    }
    track_next(t);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* the header and the tracks; false when it is not a MIDI file */
static bool smf_open(void)
{
    const uint8_t *b = file_buf, *end = file_buf + file_len;
    /* RIFF RMID: the file is in its "data" chunk */
    if (file_len >= 20 && !ci->memcmp(b, "RIFF", 4) && !ci->memcmp(b + 8, "RMID", 4))
    {
        const uint8_t *p = b + 12;
        b = NULL;
        while (end - p >= 8)
        {
            uint32_t n = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
            if (!ci->memcmp(p, "data", 4)) { b = p + 8; if (n < (uint32_t)(end - b)) end = b + n; break; }
            if (n > (uint32_t)(end - p - 8)) break;
            p += 8 + n + (n & 1);
        }
        if (!b) return false;
    }
    if (end - b < 14 || ci->memcmp(b, "MThd", 4))
        return false;
    uint32_t hlen = be32(b + 4);
    unsigned div = b[12] << 8 | b[13];
    if (hlen < 6 || hlen > (uint32_t)(end - b - 8))
        return false;
    if (div & 0x8000)
    {
        int fps = -(int8_t)(div >> 8);
        smpte_tps = (unsigned)(fps == 29 ? 30 : fps) * (div & 0xff);
        tpq = 0;
        if (!smpte_tps) return false;
    }
    else
        tpq = div ? div : 96;
    const uint8_t *p = b + 8 + hlen;
    ntracks = 0;
    while (end - p >= 8 && ntracks < MAX_TRACKS)
    {
        uint32_t n = be32(p + 4);
        const uint8_t *body = p + 8;
        if (n > (uint32_t)(end - body)) n = (uint32_t)(end - body);   /* cut short: what is there */
        if (!ci->memcmp(p, "MTrk", 4))
        {
            tracks[ntracks].start = body;
            tracks[ntracks].end = body + n;
            ntracks++;
        }
        p = body + n;
    }
    return ntracks > 0;
}

static void restart(void)
{
    for (int i = 0; i < ntracks; i++)
    {
        struct track *t = &tracks[i];
        t->p = t->start;
        t->next = 0;
        t->status = 0;
        t->done = false;
        track_next(t);
    }
    reset_channels();
    ci->memset(voices, 0, sizeof voices);
    set_tempo(500000);
    cur_tick = 0;
    tick_frac = 0;
    samples_done = 0;
    tail_done = 0;
}

/* the earliest next event; UINT32_MAX when every track has ended */
static uint32_t next_event(void)
{
    uint32_t n = UINT32_MAX;
    for (int i = 0; i < ntracks; i++)
        if (!tracks[i].done && tracks[i].next < n)
            n = tracks[i].next;
    return n;
}

static void dispatch(uint32_t tick)
{
    for (int i = 0; i < ntracks; i++)
    {
        struct track *t = &tracks[i];
        int guard = 100000;              /* a track of nothing but zero deltas still ends */
        while (!t->done && t->next == tick && guard--)
            track_event(t);
        if (guard < 0) t->done = true;
    }
}

/* frames until the next event, at most max; 0 when an event is due now */
static int frames_to_event(int max, uint32_t ev)
{
    if (ev == UINT32_MAX)
        return max;
    uint64_t need = (uint64_t)(ev - cur_tick) * spt_q16;
    need = need > tick_frac ? need - tick_frac : 0;
    uint64_t frames = (need + 0xffff) >> 16;
    return frames < (uint64_t)max ? (int)frames : max;
}

static void advance(int frames, uint32_t ev)
{
    uint64_t total = tick_frac + ((uint64_t)frames << 16);
    uint64_t ticks = total / spt_q16;
    if (ev != UINT32_MAX && ticks > ev - cur_tick)
        ticks = ev - cur_tick;
    cur_tick += (uint32_t)ticks;
    tick_frac = total - ticks * spt_q16;
    samples_done += frames;
}

/* ------------------------------------------------------------- render */
static void voice_block(struct voice *v, int32_t *out, int n)
{
    const struct chan *ch = &chans[v->ch];
    int32_t chg = ch->vol * ch->expr * 2;                    /* Q15 */
    int32_t velg = v->vel * v->vel * 2;                      /* Q15 */
    int32_t pl = pan_q[ch->pan], pr = pan_q[127 - ch->pan];
    if (v->drum)
    {
        const struct midi_drum *d = v->dr;
        int32_t g = (int32_t)((int64_t)chg * velg >> 15);
        int32_t tl = (int32_t)((int64_t)(v->tone_lev >> 15) * g >> 15);
        int32_t nl = (int32_t)((int64_t)(v->noise_lev >> 15) * g >> 15);
        for (int i = 0; i < n; i++)
        {
            int32_t s = 0;
            if (tl)
            {
                unsigned k = v->phase >> 24;
                s = sine_wave[k] * tl >> 15;
                v->phase += v->tone_step;
            }
            if (nl)
            {
                v->lfsr = v->lfsr * 1664525u + 1013904223u;
                int32_t r = (int32_t)(v->lfsr >> 17) - 16384;   /* -16384 .. 16383 */
                int32_t x = d->bright ? r - v->noise_prev : r;  /* bright: high-passed */
                v->noise_prev = r;
                s += x * nl >> 15;
            }
            out[2 * i] += s * pl >> 15;
            out[2 * i + 1] += s * pr >> 15;
        }
        /* a block's decay and sweep */
        v->tone_lev = (int32_t)((int64_t)v->tone_lev * d->tone_decay >> 16);
        v->noise_lev = (int32_t)((int64_t)v->noise_lev * d->noise_decay >> 16);
        if (d->sweep && v->tone_step > d->f1)
            v->tone_step = d->f1 + (uint32_t)((uint64_t)(v->tone_step - d->f1) * d->sweep >> 16);
        if (v->tone_lev < (1 << 16) && v->noise_lev < (1 << 16))
            v->used = false;
        return;
    }
    /* the envelope, once a block */
    const struct midi_family *f = v->fam;
    if (v->stage == 0)
    {
        if (v->attack_left > 0)
        {
            v->env += ((1 << 30) - v->env) / v->attack_left;
            v->attack_left--;
        }
        if (v->attack_left <= 0) { v->env = 1 << 30; v->stage = 1; }
    }
    else if (v->stage == 1)
    {
        int32_t sus = (int32_t)f->sustain << 15;
        v->env = sus + (int32_t)((int64_t)(v->env - sus) * f->decay >> 16);
        if (!f->sustain && v->env < (1 << 18))
            v->used = false;
    }
    else
    {
        v->env = (int32_t)((int64_t)v->env * f->release >> 16);
        if (v->env < (1 << 18))
            v->used = false;
    }
    int32_t amp = (int32_t)((int64_t)(v->env >> 15) * velg >> 15);
    amp = (int32_t)((int64_t)amp * chg >> 15);
    int32_t gl1 = amp * pl >> 15, gr1 = amp * pr >> 15;
    if (!v->used) gl1 = gr1 = 0;         /* the last block fades to nothing */
    int32_t gl = v->gl, gr = v->gr, dl = (gl1 - gl) / n, dr = (gr1 - gr) / n;
    const int16_t *w = v->wave;
    uint32_t ph = v->phase, st = v->step;
    for (int i = 0; i < n; i++)
    {
        unsigned k = ph >> 24;
        int32_t a = w[k], b = w[k + 1];
        int32_t s = a + ((b - a) * (int32_t)((ph >> 8) & 0xffff) >> 16);
        ph += st;
        gl += dl;
        gr += dr;
        out[2 * i] += s * gl >> 15;
        out[2 * i + 1] += s * gr >> 15;
    }
    v->phase = ph;
    v->gl = gl1;
    v->gr = gr1;
}

static bool voices_sounding(void)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].used)
            return true;
    return false;
}

static void render(int32_t *out, int frames)
{
    ci->memset(out, 0, frames * 2 * sizeof *out);
    for (int at = 0; at < frames; at += MIDI_BLOCK)
    {
        int n = frames - at < MIDI_BLOCK ? frames - at : MIDI_BLOCK;
        for (int i = 0; i < MAX_VOICES; i++)
            if (voices[i].used)
                voice_block(&voices[i], out + 2 * at, n);
    }
}

/* up to OUT_FRAMES frames into pcm; 0 at the end */
static int produce(void)
{
    int done = 0;
    while (done < OUT_FRAMES)
    {
        uint32_t ev = next_event();
        if (ev == UINT32_MAX)
        {
            /* the last notes ring out, three seconds at most */
            if (!voices_sounding() || tail_done >= TAIL_MAX) break;
            int n = OUT_FRAMES - done;
            render(mix + 2 * done, n);
            done += n;
            tail_done += n;
            samples_done += n;
            continue;
        }
        int n = frames_to_event(OUT_FRAMES - done, ev);
        if (!n)
        {
            dispatch(ev);
            cur_tick = ev;
            continue;
        }
        render(mix + 2 * done, n);
        advance(n, ev);
        done += n;
    }
    /* a quarter of full scale a voice at its loudest: room for chords */
    for (int i = 0; i < done * 2; i++)
        pcm[i] = mix[i] * 1024;
    return done;
}

/* the events up to ms, silently */
static void seek_to(unsigned long ms)
{
    uint64_t target = (uint64_t)ms * MIDI_RATE / 1000;
    restart();
    skipping = true;
    while (samples_done < target)
    {
        uint32_t ev = next_event();
        if (ev == UINT32_MAX) break;
        uint64_t need = (uint64_t)(ev - cur_tick) * spt_q16;
        need = need > tick_frac ? need - tick_frac : 0;
        uint64_t frames = (need + 0xffff) >> 16;
        if (frames > target - samples_done) frames = target - samples_done;
        if (!frames) { dispatch(ev); cur_tick = ev; continue; }
        /* advance() takes an int a time */
        while (frames)
        {
            int n = frames > 1 << 20 ? 1 << 20 : (int)frames;
            advance(n, ev);
            frames -= n;
        }
    }
    skipping = false;
}

enum codec_status codec_main(enum codec_entry_call_reason reason)
{
    if (reason == CODEC_LOAD)
        ci->configure(DSP_SET_SAMPLE_DEPTH, 28);
    return CODEC_OK;
}

enum codec_status codec_run(void)
{
    if (codec_init())
        return CODEC_ERROR;
    codec_set_replaygain(ci->id3);
    ci->configure(DSP_SET_FREQUENCY, MIDI_RATE);
    ci->configure(DSP_SET_STEREO_MODE, STEREO_INTERLEAVED);

    if (!ci->seek_buffer(0))
        return CODEC_ERROR;
    file_len = ci->filesize > FILE_MAX ? 0 : (size_t)ci->filesize;
    file_buf = file_len ? malloc(file_len) : NULL;
    if (!file_buf)
        return CODEC_ERROR;
    size_t got = 0;
    while (got < file_len)
    {
        size_t n = ci->read_filebuf(file_buf + got, file_len - got);
        if (!n) break;
        got += n;
    }
    file_len = got;
    if (!smf_open())
        return CODEC_ERROR;
    make_waves();
    restart();

    intptr_t param = ci->id3->elapsed;
    if (param)
    {
        seek_to(param);
        ci->set_elapsed(samples_done * 1000 / MIDI_RATE);
    }
    while (1)
    {
        long action = ci->get_command(&param);
        if (action == CODEC_ACTION_HALT)
            break;
        if (action == CODEC_ACTION_SEEK_TIME)
        {
            seek_to(param);
            ci->set_elapsed(samples_done * 1000 / MIDI_RATE);
            ci->seek_complete();
        }
        int n = produce();
        if (!n)
            break;
        /* never past the length the library was told, and its ring-out */
        if (ci->id3->length && samples_done > ((uint64_t)ci->id3->length + 3000) * MIDI_RATE / 1000)
            break;
        ci->pcmbuf_insert(pcm, NULL, n);
        ci->set_elapsed(samples_done * 1000 / MIDI_RATE);
    }
    return CODEC_OK;
}
