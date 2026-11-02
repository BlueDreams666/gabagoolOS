////////////////////////////////////////////////////////////////////////////
//                           **** DSDPACK ****                            //
//         Lossless DSD (Direct Stream Digital) Audio Compressor          //
//                Copyright (c) 2013 - 2016 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// wvdsd.c

// WavPack 5 DSD decoding for Rockbox: the three block modes of WavPack
// 5.6.0's unpack_dsd.c (raw, "fast" range-coded bytes with a history
// model, and "high" adaptive bit prediction), reading the block through a
// small buffer instead of holding all of it, and the block and header
// handling of open_utils.c and unpack_utils.c that DSD needs.

#include <string.h>
#include "wvdsd.h"

#define DSD_FLAG        0x80000000
#define FALSE_STEREO    0x40000000
#define MONO_FLAG       4
#define MONO_DATA       (MONO_FLAG | FALSE_STEREO)
#define INITIAL_BLOCK   0x800
#define SRATE_LSB       23
#define SRATE_MASK      (0xfL << SRATE_LSB)

#define ID_UNIQUE       0x3f
#define ID_ODD_SIZE     0x40
#define ID_LARGE        0x80
#define ID_DSD_BLOCK    0xe
#define ID_SAMPLE_RATE  0x27

#define MIN_STREAM_VERS 0x402
#define MAX_STREAM_VERS 0x410

static const uint32_t sample_rates [] = { 6000, 8000, 9600, 11025, 12000, 16000, 22050,
    24000, 32000, 44100, 48000, 64000, 88200, 96000, 192000 };

static uint32_t le32 (const unsigned char *p)
{
    return p [0] | (p [1] << 8) | (p [2] << 16) | ((uint32_t) p [3] << 24);
}

/////////////////////////////// block reading ////////////////////////////////

// next byte of the block: 0xff past its end (a bad size, which the CRC
// then catches), and also with error set when the stream itself ends

static int block_byte (WavpackDSD *d)
{
    if (d->pos == d->len) {
        uint32_t n = d->block_left < WVDSD_BUF ? d->block_left : WVDSD_BUF;
        int32_t got;

        if (!n)
            return 0xff;

        got = d->infile (d->buf, n);

        if (got <= 0) {
            d->error = 1;
            return 0xff;
        }

        d->block_left -= got;
        d->pos = 0;
        d->len = got;
    }

    return d->buf [d->pos++];
}

// next byte of the DSD sub-block; callers check dsd_left first, as the
// original checks byteptr < endptr

static int dsd_byte (WavpackDSD *d)
{
    if (!d->dsd_left)
        return 0xff;

    d->dsd_left--;
    return block_byte (d);
}

static void skip_block (WavpackDSD *d)
{
    d->pos = d->len = 0;

    while (d->block_left && !d->error) {
        uint32_t n = d->block_left < WVDSD_BUF ? d->block_left : WVDSD_BUF;
        int32_t got = d->infile (d->buf, n);

        if (got <= 0)
            d->error = 1;
        else
            d->block_left -= got;
    }

    d->dsd_left = 0;
    d->ready = 0;
}

// Read from the current position until a valid 32-byte WavPack header is
// found (no more than 1 MB on), as wputils.c's read_next_header does.

static int next_header (WavpackDSD *d, unsigned char *h)
{
    int32_t skipped = 0;

    if (d->infile (h, 32) != 32)
        return 0;

    while (1) {
        if (!memcmp (h, "wvpk", 4) && !(h [4] & 1) && le32 (h + 4) >= 24 &&
            le32 (h + 4) < 0x1000000 && h [9] == 4 &&
            h [8] >= (MIN_STREAM_VERS & 0xff) && h [8] <= (MAX_STREAM_VERS & 0xff))
                return 1;

        if (++skipped > 1048576)
            return 0;

        memmove (h, h + 1, 31);

        if (d->infile (h + 31, 1) != 1)
            return 0;
    }
}

//////////////////////////////// mode set-up /////////////////////////////////

static void init_ptable (int32_t *table, int rate_i, int rate_s);

#define DSD_BYTE_READY(low,high) (!(((low) ^ (high)) & 0xff000000))

static int init_dsd_block_fast (WavpackDSD *d)
{
    unsigned char history_bits, max_probability, *lb_ptr = d->lookup_buffer;
    int total_summed_probabilities = 0, bi, i;

    if (!d->dsd_left)
        return 0;

    history_bits = dsd_byte (d);

    if (!d->dsd_left || history_bits > WVDSD_HISTORY_BITS)
        return 0;

    d->history_bins = 1 << history_bits;
    memset (d->value_lookup, 0, sizeof (d->value_lookup));

    max_probability = dsd_byte (d);

    if (max_probability < 0xff) {
        unsigned char *outptr = (unsigned char *) d->probabilities;
        unsigned char *outend = outptr + sizeof (*d->probabilities) * d->history_bins;

        while (outptr < outend && d->dsd_left) {
            int code = dsd_byte (d);

            if (code > max_probability) {
                int zcount = code - max_probability;

                while (outptr < outend && zcount--)
                    *outptr++ = 0;
            }
            else if (code)
                *outptr++ = code;
            else
                break;
        }

        if (outptr < outend || (d->dsd_left && dsd_byte (d)))
            return 0;
    }
    else if (d->dsd_left > sizeof (*d->probabilities) * d->history_bins) {
        unsigned char *outptr = (unsigned char *) d->probabilities;
        int n = sizeof (*d->probabilities) * d->history_bins;

        while (n--)
            *outptr++ = dsd_byte (d);
    }
    else
        return 0;

    for (bi = 0; bi < d->history_bins; ++bi) {
        int32_t sum_values;

        for (sum_values = i = 0; i < 256; ++i)
            d->summed_probabilities [bi] [i] = sum_values += d->probabilities [bi] [i];

        if (sum_values) {
            if ((total_summed_probabilities += sum_values) > d->history_bins * WVDSD_BYTES_PER_BIN)
                return 0;

            d->value_lookup [bi] = lb_ptr;

            for (i = 0; i < 256; i++) {
                int c = d->probabilities [bi] [i];

                while (c--)
                    *lb_ptr++ = i;
            }
        }
    }

    if (d->dsd_left < 4 || total_summed_probabilities > d->history_bins * WVDSD_BYTES_PER_BIN)
        return 0;

    for (i = 4; i--;)
        d->value = (d->value << 8) | dsd_byte (d);

    d->p0 = d->p1 = 0;
    d->low = 0; d->high = 0xffffffff;
    return 1;
}

#define PTABLE_BITS 8
#define PTABLE_BINS (1<<PTABLE_BITS)
#define PTABLE_MASK (PTABLE_BINS-1)

#define UP   0x010000fe
#define DOWN 0x00010000
#define DECAY 8

#define PRECISION 20
#define VALUE_ONE (1 << PRECISION)
#define PRECISION_USE 12

#define RATE_S 20

static void init_ptable (int32_t *table, int rate_i, int rate_s)
{
    int value = 0x808000, rate = rate_i << 8, c, i;

    for (c = (rate + 128) >> 8; c--;)
        value += (DOWN - value) >> DECAY;

    for (i = 0; i < PTABLE_BINS/2; ++i) {
        table [i] = value;
        table [PTABLE_BINS-1-i] = 0x100ffff - value;

        if (value > 0x010000) {
            rate += (rate * rate_s + 128) >> 8;

            for (c = (rate + 64) >> 7; c--;)
                value += (DOWN - value) >> DECAY;
        }
    }
}

static int init_dsd_block_high (WavpackDSD *d)
{
    int channel, rate_i, rate_s, i;

    if (d->dsd_left < (uint32_t) (d->mono ? 13 : 20))
        return 0;

    rate_i = dsd_byte (d);
    rate_s = dsd_byte (d);

    if (rate_s != RATE_S)
        return 0;

    init_ptable (d->ptable, rate_i, rate_s);

    for (channel = 0; channel < (d->mono ? 1 : 2); ++channel) {
        WvdsdFilters *sp = d->filters + channel;

        sp->filter1 = dsd_byte (d) << (PRECISION - 8);
        sp->filter2 = dsd_byte (d) << (PRECISION - 8);
        sp->filter3 = dsd_byte (d) << (PRECISION - 8);
        sp->filter4 = dsd_byte (d) << (PRECISION - 8);
        sp->filter5 = dsd_byte (d) << (PRECISION - 8);
        sp->filter6 = 0;
        sp->factor = dsd_byte (d) & 0xff;
        sp->factor |= (dsd_byte (d) << 8) & 0xff00;
        sp->factor = (int32_t)((uint32_t)sp->factor << 16) >> 16;
    }

    d->high = 0xffffffff;
    d->low = 0x0;

    for (i = 4; i--;)
        d->value = (d->value << 8) | dsd_byte (d);

    return 1;
}

// The DSD sub-block's first two bytes: the rate multiplier as a power of
// two, and the mode.

static int init_dsd_block (WavpackDSD *d)
{
    int power;

    if (d->dsd_left < 2)
        return 0;

    power = dsd_byte (d);

    if (power > 31)
        return 0;

    d->multiplier = 1U << power;
    d->mode = dsd_byte (d);

    if (!d->mode)
        return d->dsd_left == d->block_samples * (d->mono ? 1 : 2);

    if (d->mode == 1)
        return init_dsd_block_fast (d);
    else if (d->mode == 3)
        return init_dsd_block_high (d);
    else
        return 0;
}

// Read the header just found and the block's metadata up to its DSD data.
// Returns 1 for a block to decode, -1 for one to skip, 0 for an error.

static int start_block (WavpackDSD *d, const unsigned char *h)
{
    d->flags = le32 (h + 24);
    d->block_left = le32 (h + 4) - 24;
    d->block_samples = le32 (h + 20);
    d->block_index = le32 (h + 16) + ((int64_t) h [10] << 32);
    d->crc_want = le32 (h + 28);
    d->pos = d->len = 0;
    d->dsd_left = 0;
    d->ready = 0;

    if (le32 (h + 12) != (uint32_t) -1)
        d->total_samples = le32 (h + 12) + ((int64_t) h [11] << 32) - h [11];

    if (!(d->flags & DSD_FLAG))
        return 0;

    if (!(d->flags & INITIAL_BLOCK) || !d->block_samples)
        return -1;

    d->mono = (d->flags & MONO_DATA) != 0;

    if ((d->flags & SRATE_MASK) != SRATE_MASK)
        d->sample_rate = sample_rates [(d->flags & SRATE_MASK) >> SRATE_LSB];

    while (d->block_left || d->pos < d->len) {
        uint32_t size, i;
        int id = block_byte (d);

        size = block_byte (d) << 1;

        if (id & ID_LARGE) {
            size += block_byte (d) << 9;
            size += (uint32_t) block_byte (d) << 17;
        }

        if (d->error)
            return 0;

        if ((id & ID_UNIQUE) == ID_DSD_BLOCK) {
            d->dsd_left = (id & ID_ODD_SIZE) ? size - 1 : size;
            d->crc = 0xffffffff;
            d->sample_index = d->block_index;
            d->ready = init_dsd_block (d) && !d->error;
            return 1;
        }

        if ((id & ID_UNIQUE) == ID_SAMPLE_RATE && (size == 4)) {
            unsigned char r [4];
            int length = (id & ID_ODD_SIZE) ? 3 : 4;

            for (i = 0; i < 4; ++i)
                r [i] = block_byte (d);

            d->sample_rate = r [0] | (r [1] << 8) | ((uint32_t) r [2] << 16);

            if (length == 4)
                d->sample_rate |= (uint32_t) (r [3] & 0x7f) << 24;

            continue;
        }

        for (i = 0; i < size; ++i)
            block_byte (d);
    }

    // an audio block without DSD data decodes as silence

    d->sample_index = d->block_index;
    d->mode = -1;
    return 1;
}

// Move to the next block with DSD audio for the first stream.

static int next_audio_block (WavpackDSD *d)
{
    unsigned char h [32];

    while (!d->error) {
        int res;

        if (!next_header (d, h))
            return 0;

        res = start_block (d, h);

        if (res > 0)
            return 1;

        if (!res)
            return 0;

        skip_block (d);
    }

    return 0;
}

int wvdsd_open (WavpackDSD *d, int32_t (*infile)(void *, int32_t))
{
    memset (d, 0, sizeof (*d));
    d->infile = infile;
    d->total_samples = -1;
    d->sample_rate = 44100;
    return next_audio_block (d) && d->multiplier;
}

int wvdsd_resync (WavpackDSD *d)
{
    d->pos = d->len = 0;
    d->block_left = d->dsd_left = 0;
    d->ready = d->error = 0;
    return next_audio_block (d);
}

int wvdsd_skip_to (WavpackDSD *d, int64_t sample)
{
    while (d->block_index + d->block_samples <= sample) {
        skip_block (d);

        if (!next_audio_block (d))
            return 0;
    }

    return 1;
}

///////////////////////////////// decoding ///////////////////////////////////

static int decode_fast (WavpackDSD *d, unsigned char *output, int sample_count)
{
    int total_samples = sample_count;

    if (!d->mono)
        total_samples *= 2;

    while (total_samples--) {
        unsigned int mult, index, code, i;

        if (!d->summed_probabilities [d->p0] [255])
            return 0;

        mult = (d->high - d->low) / d->summed_probabilities [d->p0] [255];

        if (!mult) {
            if (d->dsd_left >= 4)
                for (i = 4; i--;)
                    d->value = (d->value << 8) | dsd_byte (d);

            d->low = 0;
            d->high = 0xffffffff;
            mult = d->high / d->summed_probabilities [d->p0] [255];

            if (!mult)
                return 0;
        }

        index = (d->value - d->low) / mult;

        if (index >= d->summed_probabilities [d->p0] [255])
            return 0;

        if ((*output++ = code = d->value_lookup [d->p0] [index]))
            d->low += d->summed_probabilities [d->p0] [code-1] * mult;

        d->high = d->low + d->probabilities [d->p0] [code] * mult - 1;
        d->crc += (d->crc << 1) + code;

        if (d->mono)
            d->p0 = code & (d->history_bins-1);
        else {
            d->p0 = d->p1;
            d->p1 = code & (d->history_bins-1);
        }

        while (DSD_BYTE_READY (d->high, d->low) && d->dsd_left) {
            d->value = (d->value << 8) | dsd_byte (d);
            d->high = (d->high << 8) | 0xff;
            d->low <<= 8;
        }
    }

    return sample_count;
}

// one bit of one channel of "high" mode; the coder's state is passed in
// locals so it stays in registers (the R1's codecs run from -O1 C)

typedef struct { uint32_t low, high, value; } WvdsdCoder;

static inline __attribute__((always_inline))
void decode_high_bit (WavpackDSD *d, WvdsdCoder *rc, WvdsdFilters *sp)
{
    int32_t *pp = d->ptable + ((sp->value >> (PRECISION - PRECISION_USE)) & PTABLE_MASK);
    uint32_t split = rc->low + ((rc->high - rc->low) >> 8) * (*pp >> 16);

    if (rc->value <= split) {
        rc->high = split;
        *pp += (UP - *pp) >> DECAY;
        sp->filter0 = -1;
    }
    else {
        rc->low = split + 1;
        *pp += (DOWN - *pp) >> DECAY;
        sp->filter0 = 0;
    }

    while (DSD_BYTE_READY (rc->high, rc->low) && d->dsd_left) {
        rc->value = (rc->value << 8) | dsd_byte (d);
        rc->high = (rc->high << 8) | 0xff;
        rc->low <<= 8;
    }

    sp->value += sp->filter6 * 8;
    sp->byte = (sp->byte << 1) | (sp->filter0 & 1);
    sp->factor += (((sp->value ^ sp->filter0) >> 31) | 1) & ((sp->value ^ (sp->value - (sp->filter6 * 16))) >> 31);
    sp->filter1 += ((sp->filter0 & VALUE_ONE) - sp->filter1) >> 6;
    sp->filter2 += ((sp->filter0 & VALUE_ONE) - sp->filter2) >> 4;
    sp->filter3 += (sp->filter2 - sp->filter3) >> 4;
    sp->filter4 += (sp->filter3 - sp->filter4) >> 4;
    sp->value = (sp->filter4 - sp->filter5) >> 4;
    sp->filter5 += sp->value;
    sp->filter6 += (sp->value - sp->filter6) >> 3;
    sp->value = sp->filter1 - sp->filter5 + ((sp->filter6 * sp->factor) >> 2);
}

static int decode_high (WavpackDSD *d, unsigned char *output, int sample_count)
{
    int total_samples = sample_count, stereo = !d->mono;
    WvdsdCoder rc = { d->low, d->high, d->value };
    WvdsdFilters f0 = d->filters [0], f1 = d->filters [1];
    uint32_t crc = d->crc;

    while (total_samples--) {
        int bitcount = 8;

        f0.value = f0.filter1 - f0.filter5 + ((f0.filter6 * f0.factor) >> 2);

        if (stereo)
            f1.value = f1.filter1 - f1.filter5 + ((f1.filter6 * f1.factor) >> 2);

        if (stereo)
            while (bitcount--) {
                decode_high_bit (d, &rc, &f0);
                decode_high_bit (d, &rc, &f1);
            }
        else
            while (bitcount--)
                decode_high_bit (d, &rc, &f0);

        crc += (crc << 1) + (*output++ = f0.byte & 0xff);
        f0.factor -= (f0.factor + 512) >> 10;

        if (stereo) {
            crc += (crc << 1) + (*output++ = f1.byte & 0xff);
            f1.factor -= (f1.factor + 512) >> 10;
        }
    }

    d->low = rc.low; d->high = rc.high; d->value = rc.value;
    d->filters [0] = f0; d->filters [1] = f1;
    d->crc = crc;
    return sample_count;
}

int wvdsd_unpack (WavpackDSD *d, unsigned char *left, unsigned char *right, int count)
{
    unsigned char tmp [1024];
    int done = 0;

    while (done < count) {
        int64_t block_end = d->block_index + d->block_samples;
        int n, i;

        if (d->total_samples >= 0 && d->sample_index >= d->total_samples)
            break;

        if (d->sample_index >= block_end) {
            if (d->mode >= 0 && (!d->ready || d->crc != d->crc_want))
                d->crc_errors++;

            skip_block (d);

            if (!next_audio_block (d))
                break;

            continue;
        }

        n = count - done;

        if (n > (int) sizeof (tmp) / 2)
            n = sizeof (tmp) / 2;

        if (n > block_end - d->sample_index)
            n = (int) (block_end - d->sample_index);

        if (d->ready && d->mode == 0) {
            for (i = 0; i < n * (d->mono ? 1 : 2); ++i)
                d->crc += (d->crc << 1) + (tmp [i] = dsd_byte (d));
        }
        else if (d->ready && d->mode == 1) {
            if (!decode_fast (d, tmp, n))
                d->ready = 0;
        }
        else if (d->ready && d->mode == 3)
            decode_high (d, tmp, n);

        if (d->error)
            d->ready = 0;

        if (!d->ready)
            memset (tmp, 0x55, n * 2);

        if (d->mono) {
            memcpy (left + done, tmp, n);
            memcpy (right + done, tmp, n);
        }
        else
            for (i = 0; i < n; ++i) {
                left [done + i] = tmp [i * 2];
                right [done + i] = tmp [i * 2 + 1];
            }

        d->sample_index += n;
        done += n;
    }

    return done;
}
