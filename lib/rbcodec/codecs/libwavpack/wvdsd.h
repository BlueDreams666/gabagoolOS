////////////////////////////////////////////////////////////////////////////
//                           **** DSDPACK ****                            //
//         Lossless DSD (Direct Stream Digital) Audio Compressor          //
//                Copyright (c) 2013 - 2016 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// wvdsd.h

// Streaming reader for WavPack 5 DSD files (blocks with DSD_FLAG): raw,
// "fast" and "high" DSD blocks, from WavPack 5.6.0's unpack_dsd.c. Data is
// read through a read_stream callback a few KB at a time, so a whole block
// never has to sit in memory. Output is one byte (8 DSD bits, MSB first)
// per channel per "sample", left and right apart; mono and false-stereo
// blocks come out on both. Only the first stream (front L/R) of a
// multichannel file is decoded.

#ifndef WVDSD_H
#define WVDSD_H

#include <inttypes.h>

#define WVDSD_BUF           4096
#define WVDSD_HISTORY_BITS  5
#define WVDSD_BYTES_PER_BIN 1280

typedef struct {
    int32_t value, filter0, filter1, filter2, filter3, filter4, filter5, filter6, factor;
    unsigned int byte;
} WvdsdFilters;

typedef struct {
    int32_t (*infile)(void *, int32_t);

    // current block
    uint32_t flags, crc_want, crc, block_samples;
    int64_t block_index, total_samples, sample_index;
    uint32_t block_left;        // block bytes not yet pulled into buf
    uint32_t dsd_left;          // DSD sub-block bytes not yet consumed
    int mode, ready, error, mono;

    // stream facts (first audio block)
    uint32_t sample_rate;       // header rate (DSD byte rate / multiplier)
    uint32_t multiplier;        // DSD byte rate = sample_rate * multiplier
    uint32_t crc_errors;

    unsigned char buf [WVDSD_BUF];
    int pos, len;

    // "fast" mode
    int history_bins, p0, p1;
    unsigned char *value_lookup [1 << WVDSD_HISTORY_BITS];
    unsigned char lookup_buffer [(1 << WVDSD_HISTORY_BITS) * WVDSD_BYTES_PER_BIN];
    uint16_t summed_probabilities [1 << WVDSD_HISTORY_BITS] [256];
    unsigned char probabilities [1 << WVDSD_HISTORY_BITS] [256];
    uint32_t low, high, value;

    // "high" mode
    WvdsdFilters filters [2];
    int32_t ptable [256];
} WavpackDSD;

// Find the first DSD audio block from the current position (up to 1 MB of
// skipping) and get ready to decode it. Returns 1, or 0 if this is not a
// DSD WavPack stream.
int wvdsd_open (WavpackDSD *d, int32_t (*infile)(void *, int32_t));

// After the caller moved the stream somewhere else: find the next block
// that starts a sample group. Returns 1, or 0 at the end or on an error.
int wvdsd_resync (WavpackDSD *d);

// Skip whole blocks (reading, not decoding them) until the one holding
// sample. Returns 0 if the stream ends first.
int wvdsd_skip_to (WavpackDSD *d, int64_t sample);

// Decode up to count bytes per channel into left and right. Returns the
// number decoded, 0 at the end of the stream.
int wvdsd_unpack (WavpackDSD *d, unsigned char *left, unsigned char *right, int count);

// DSD bytes per second per channel (rate / 8 in bits)
#define wvdsd_byte_rate(d) ((d)->sample_rate * (d)->multiplier)

#endif
