/* deflate_p.h -- Private inline functions and macros shared with more than
 *                one deflate method
 *
 * Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 */

#ifndef DEFLATE_P_H
#define DEFLATE_P_H

#include "functable.h"
#include "fallback_builtins.h"
#include "zmemory.h"

/* Forward declare common non-inlined functions declared in deflate.c */

#ifdef ZLIB_DEBUG
/* ===========================================================================
 * Check that the match at match_start is indeed a match.
 */
static inline void check_match(deflate_state *s, uint32_t start, uint32_t match, uint32_t length) {
    /* check that the match length is valid*/
    if (length < STD_MIN_MATCH || length > STD_MAX_MATCH) {
        fprintf(stderr, " start %u, match %u, length %u\n", start, match, length);
        z_error("invalid match length");
    }
    /* check that the match isn't at the same position as the start string */
    if (match == start) {
        fprintf(stderr, " start %u, match %u, length %u\n", start, match, length);
        z_error("invalid match position");
    }
    /* check that the match is indeed a match */
    if (memcmp(s->window + match, s->window + start, length) != 0) {
        int32_t i = 0;
        fprintf(stderr, " start %u, match %u, length %u\n", start, match, length);
        do {
            fprintf(stderr, "  %03d: match [%02x] start [%02x]\n", i++,
                s->window[match++], s->window[start++]);
        } while (--length != 0);
        z_error("invalid match");
    }
    if (z_verbose > 1) {
        fprintf(stderr, "\\[%u,%u]", start-match, length);
        do {
            putc(s->window[start++], stderr);
        } while (--length != 0);
    }
}
#else
#define check_match(s, start, match, length)
#endif

Z_INTERNAL void PREFIX(flush_pending)(PREFIX3(stream) *strm);

/* ===========================================================================
 * Literal-run acceleration. Data that does not compress (random bytes, already
 * compressed payloads, noisy images) still pays a hash insert, a chain-head
 * lookup and a longest_match call per byte, most of it cache misses, and then
 * emits every byte as a literal anyway. After skip_start consecutive positions
 * without a match the strategies emit extra literals without searching, the
 * count growing by one every 1 << skip_shift misses up to SKIP_MAX, and halved
 * whenever a match is found. Both parameters come from the level table. Matches that start inside a skipped stretch
 * are missed, which costs a little ratio only on data that was barely
 * compressing to begin with.
 */
#define SKIP_MAX   32
/* Literals are tallied lazily so that a later match can grow backwards over them;
 * at most this many are held back. */
#define PEND_MAX   64

/* Stored-block mode of deflate_quick: once this many consecutive positions
 * found no match, a stretch of the input is probed and, if it looks
 * incompressible, copied as a stored block instead of being coded literal by
 * literal. (The hashing strategies tried the same in journal step 004 and it
 * was rolled back: their block coder already chooses stored blocks where they
 * win, and deciding earlier lost ratio on mixed input.) */
#define STORED_MODE_MISSES 512
#define STORED_CHUNK_MIN   1024
#define STORED_CHUNK_MAX   8192
/* deflate_quick has no block coder to ask, so it probes: one lookup every
 * PROBE_STRIDE bytes, and the stretch is stored when fewer than one probe in
 * STORED_MAX_HITS finds a match */
#define PROBE_STRIDE       16
#define STORED_MAX_HITS    64

/* Number of positions to emit as literals without searching, given the miss count */
#define SKIP_STEP(s, miss_run, skip_start) \
    (((miss_run) > (skip_start)) ? MIN(((miss_run) - (skip_start)) >> (s)->skip_shift, (uint32_t)SKIP_MAX) : 0u)

/* A match ends the run */
#define SKIP_HIT(miss_run) ((miss_run) = 0)

/* Hash every stride-th position of a stretch that the strategy does not
 * search (skipped or stored), so that later matches can still point into it;
 * a match that starts between two hashed positions is recovered by the
 * backward growth over pending literals. stride 0 hashes nothing. */
void Z_INTERNAL zng_deflate_insert_stride(deflate_state *s, unsigned char *window, uint32_t start, uint32_t n, uint32_t stride);

#define SKIP_INSERT_KNUTH(s, window, pos, n, stride) do { \
        if ((stride) == 1) \
            insert_knuth_batch_static(s, window, pos, n); \
        else if (stride) \
            zng_deflate_insert_stride(s, window, pos, n, stride); \
    } while (0)

/* Number of positions to emit as literals without searching, given the miss count */
#define SKIP_STEP(s, miss_run, skip_start) \
    (((miss_run) > (skip_start)) ? MIN(((miss_run) - (skip_start)) >> (s)->skip_shift, (uint32_t)SKIP_MAX) : 0u)

/* A match ends the run */
#define SKIP_HIT(miss_run) ((miss_run) = 0)

/* Hash the positions skipped by the acceleration according to the level's
 * insert policy, so that later matches can still point into the skipped run.
 * The knuth variant is for levels below MIN_ROLL_LEVEL. */

/* Number of symbols that still fit in the symbol buffer, and the buffer bytes
 * that n symbols take */
#ifdef LIT_MEM
#  define SYM_ROOM(sym_next, sym_end) ((sym_end) - (sym_next))
#  define SYM_BYTES(n) (n)
#else
#  define SYM_ROOM(sym_next, sym_end) (((sym_end) - (sym_next)) / 3)
#  define SYM_BYTES(n) ((n) * 3)
#endif
/* True when fewer than n symbols fit in the symbol buffer */
#define SYM_SHORT(sym_next, sym_end, n) ((sym_end) - (sym_next) < SYM_BYTES(n))


/* ===========================================================================
 * Save the match info and tally the frequency counts. Return true if
 * the current block must be flushed.
 */

extern Z_INTERNAL const unsigned char zng_length_code[];
extern Z_INTERNAL const unsigned char zng_dist_code[];

static inline int zng_tr_tally_lit(deflate_state *s, unsigned char c) {
    /* c is the unmatched char */
    unsigned int sym_next = s->sym_next;
#ifdef LIT_MEM
    s->d_buf[sym_next] = 0;
    s->l_buf[sym_next] = c;
    s->sym_next = sym_next + 1;
#else
#  if OPTIMAL_CMP >= 32
    zng_memwrite_4(&s->sym_buf[sym_next], Z_U32_TO_LE((uint32_t)c << 16));
#  else
    s->sym_buf[sym_next] = 0;
    s->sym_buf[sym_next+1] = 0;
    s->sym_buf[sym_next+2] = c;
#  endif
    s->sym_next = sym_next + 3;
#endif
    s->dyn_ltree[c].Freq++;
    Tracevv((stderr, "%c", c));
#ifdef ZLIB_DEBUG
    uint32_t _c = c;
    Assert(_c <= (STD_MAX_MATCH-STD_MIN_MATCH), "zng_tr_tally: bad literal");
#endif
    return (s->sym_next == s->sym_end);
}

static inline int zng_tr_tally_dist(deflate_state* s, uint32_t dist, uint32_t len) {
    /* dist: distance of matched string */
    /* len: match length-STD_MIN_MATCH */
    unsigned int sym_next = s->sym_next;
#ifdef LIT_MEM
    Assert(dist <= UINT16_MAX, "dist should fit in uint16_t");
    Assert(len <= UINT8_MAX, "len should fit in uint8_t");
    s->d_buf[sym_next] = (uint16_t)dist;
    s->l_buf[sym_next] = (uint8_t)len;
    s->sym_next = sym_next + 1;
#else
#  if OPTIMAL_CMP >= 32
    zng_memwrite_4(&s->sym_buf[sym_next], Z_U32_TO_LE(dist | ((uint32_t)len << 16)));
#  else
    s->sym_buf[sym_next] = (uint8_t)(dist);
    s->sym_buf[sym_next+1] = (uint8_t)(dist >> 8);
    s->sym_buf[sym_next+2] = (uint8_t)len;
#  endif
    s->sym_next = sym_next + 3;
#endif
    dist--;
    Assert(dist < MAX_DIST(s) && (uint16_t)d_code(dist) < (uint16_t)D_CODES,
        "zng_tr_tally: bad match");

    s->dyn_ltree[zng_length_code[len] + LITERALS + 1].Freq++;
    s->dyn_dtree[d_code(dist)].Freq++;
    return (s->sym_next == s->sym_end);
}

/* ===========================================================================
 * Register-resident variants of the tally functions. The symbol buffer write
 * goes through memcpy-style byte access, which forces the compiler to assume
 * it may alias every field of the deflate state, so a loop that keeps sym_next
 * in the state reloads it (and everything else it holds) after every symbol.
 * These take sym_next by value and return the new value, so the caller keeps
 * it in a register and only the frequency counter touches memory. The caller
 * must store it back into s->sym_next before anything reads the state.
 */
Z_FORCEINLINE static unsigned int zng_tr_tally_lit_at(deflate_state *s, unsigned int sym_next, unsigned char c) {
#ifdef LIT_MEM
    s->d_buf[sym_next] = 0;
    s->l_buf[sym_next] = c;
    sym_next += 1;
#else
#  if OPTIMAL_CMP >= 32
    zng_memwrite_4(&s->sym_buf[sym_next], Z_U32_TO_LE((uint32_t)c << 16));
#  else
    s->sym_buf[sym_next] = 0;
    s->sym_buf[sym_next+1] = 0;
    s->sym_buf[sym_next+2] = c;
#  endif
    sym_next += 3;
#endif
    s->dyn_ltree[c].Freq++;
    Tracevv((stderr, "%c", c));
    return sym_next;
}

Z_FORCEINLINE static unsigned int zng_tr_tally_dist_at(deflate_state *s, unsigned int sym_next, uint32_t dist, uint32_t len) {
#ifdef LIT_MEM
    Assert(dist <= UINT16_MAX, "dist should fit in uint16_t");
    Assert(len <= UINT8_MAX, "len should fit in uint8_t");
    s->d_buf[sym_next] = (uint16_t)dist;
    s->l_buf[sym_next] = (uint8_t)len;
    sym_next += 1;
#else
#  if OPTIMAL_CMP >= 32
    zng_memwrite_4(&s->sym_buf[sym_next], Z_U32_TO_LE(dist | ((uint32_t)len << 16)));
#  else
    s->sym_buf[sym_next] = (uint8_t)(dist);
    s->sym_buf[sym_next+1] = (uint8_t)(dist >> 8);
    s->sym_buf[sym_next+2] = (uint8_t)len;
#  endif
    sym_next += 3;
#endif
    dist--;
    Assert(dist < MAX_DIST(s) && (uint16_t)d_code(dist) < (uint16_t)D_CODES,
        "zng_tr_tally: bad match");

    s->dyn_ltree[zng_length_code[len] + LITERALS + 1].Freq++;
    s->dyn_dtree[d_code(dist)].Freq++;
    return sym_next;
}

/* Tally n consecutive literals starting at p. The caller guarantees room for them. */
Z_FORCEINLINE static unsigned int zng_tr_tally_lits_at(deflate_state *s, unsigned int sym_next, const unsigned char *p, uint32_t n) {
    Assert(n <= SYM_ROOM(sym_next, s->sym_end), "pending literals overflow the symbol buffer");
    /* One or two literals between matches is the common case on compressible data */
    sym_next = zng_tr_tally_lit_at(s, sym_next, p[0]);
    if (n == 1)
        return sym_next;
    sym_next = zng_tr_tally_lit_at(s, sym_next, p[1]);
    for (uint32_t i = 2; i < n; i++)
        sym_next = zng_tr_tally_lit_at(s, sym_next, p[i]);
    return sym_next;
}

/* =========================================================================
 * Flush as much pending output as possible. All deflate() output, except for some
 * deflate_stored() output, goes through this function so some applications may wish to
 * modify it to avoid allocating a large strm->next_out buffer and copying into it.
 * See also read_buf().
 */
Z_FORCEINLINE static void flush_pending_inline(PREFIX3(stream) *strm) {
    uint32_t len;
    deflate_state *s = strm->state;

    zng_tr_flush_bits(s);
    len = MIN(s->pending, strm->avail_out);
    if (len == 0)
        return;

    Tracev((stderr, "[FLUSH]"));
    memcpy(strm->next_out, s->pending_out, len);
    strm->next_out  += len;
    s->pending_out  += len;
    strm->total_out += len;
    strm->avail_out -= len;
    s->pending      -= len;
    if (s->pending == 0)
        s->pending_out = s->pending_buf;
}

/* ===========================================================================
 * Reverse the first len bits of a code using bit manipulation
 */
Z_FORCEINLINE static uint16_t bi_reverse(uint16_t code, int len) {
    /* code: the value to invert */
    /* len: its bit length */
    Assert(len >= 1 && len <= 15, "code length must be 1-15");
    return zng_bitreverse16(code) >> (16 - len);
}

/* ===========================================================================
 * Read a new buffer from the current input stream, update the adler32 and total number of
 * bytes read.  All deflate() input goes through this function so some applications may wish
 * to modify it to avoid allocating a large strm->next_in buffer and copying from it.
 * See also flush_pending_inline().
 */
Z_FORCEINLINE static unsigned read_buf(PREFIX3(stream) *strm, unsigned char *buf, unsigned size) {
    deflate_state *s = strm->state;
    uint32_t len = MIN(strm->avail_in, size);

    if (len == 0)
        return 0;

    if (!DEFLATE_NEED_CHECKSUM(strm)) {
        memcpy(buf, strm->next_in, len);
#ifdef GZIP
    } else if (s->wrap == 2) {
        strm->adler = FUNCTABLE_CALL(crc32_copy)(strm->adler, buf, strm->next_in, len);
#endif
    } else if (s->wrap == 1) {
        strm->adler = FUNCTABLE_CALL(adler32_copy)(strm->adler, buf, strm->next_in, len);
    } else {
        memcpy(buf, strm->next_in, len);
    }

    strm->avail_in -= len;
    strm->next_in  += len;
    strm->total_in += len;
    return len;
}

/* ===========================================================================
 * Flush the current block, with given end-of-file flag.
 * IN assertion: strstart is set to the end of the current match.
 */
#define FLUSH_BLOCK_ONLY(s, window, last) { \
    int block_start = s->block_start; \
    zng_tr_flush_block(s, (block_start >= 0 ? \
                   &window[(unsigned)block_start] : \
                   NULL), \
                   (uint32_t)((int)s->strstart - block_start), \
                   (last)); \
    s->block_start = (int)s->strstart; \
    PREFIX(flush_pending)(s->strm); \
}

/* Same but force premature exit if necessary. */
#define FLUSH_BLOCK(s, window, last) { \
    FLUSH_BLOCK_ONLY(s, window, last); \
    if (s->strm->avail_out == 0) return (last) ? finish_started : need_more; \
}

/* Maximum stored block length in deflate format (not including header). */
#define MAX_STORED 65535

/* Compression function. Returns the block state after the call. */
typedef block_state (*compress_func) (deflate_state *s, int flush);

#endif
