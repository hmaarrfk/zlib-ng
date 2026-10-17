/*
 * The deflate_quick deflate strategy, designed to be used when cycles are
 * at a premium.
 *
 * Copyright (C) 2013 Intel Corporation. All rights reserved.
 * Authors:
 *  Wajdi Feghali   <wajdi.k.feghali@intel.com>
 *  Jim Guilford    <james.guilford@intel.com>
 *  Vinodh Gopal    <vinodh.gopal@intel.com>
 *     Erdinc Ozturk   <erdinc.ozturk@intel.com>
 *  Jim Kukunas     <james.t.kukunas@linux.intel.com>
 *
 * Portions are Copyright (C) 2016 12Sided Technology, LLC.
 * Author:
 *  Phil Vachon     <pvachon@12sidedtech.com>
 *
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "zmemory.h"
#include "deflate.h"
#include "deflate_p.h"
#include "functable.h"
#include "trees_emit.h"
#include "insert_string_p.h"

extern const ct_data static_ltree[L_CODES+2];
extern const ct_data static_dtree[D_CODES];

Z_FORCEINLINE static void quick_start_block(deflate_state *s, uint32_t strstart, int last) {
    zng_tr_emit_tree(s, STATIC_TREES, last);
    s->block_open = 1 + last;
    s->block_start = (int)strstart;
}

Z_FORCEINLINE static int quick_end_block(deflate_state *s, uint32_t strstart, int last) {
    if (s->block_open) {
        zng_tr_emit_end_block(s, static_ltree, last);
        s->block_open = 0;
        s->block_start = (int)strstart;
        PREFIX(flush_pending)(s->strm);
        return (s->strm->avail_out == 0);
    }
    return 0;
}

/* ===========================================================================
 * Probe a stretch of the window for compressibility: hash and check every
 * PROBE_STRIDE-th position like the main loop would. Returns the number of
 * positions that found a 4-byte match. The probed positions are inserted
 * into the hash so the stretch is not entirely absent from the dictionary.
 */
static uint32_t quick_probe(deflate_state *s, const unsigned char *window, uint32_t start, uint32_t len) {
    uint32_t hits = 0;
    uint32_t max_dist = MAX_DIST(s);
    for (uint32_t pos = start; pos + WANT_MIN_MATCH <= start + len; pos += PROBE_STRIDE) {
        uint32_t str_val = Z_U32_FROM_LE(zng_memread_4(window + pos));
        uint32_t hash_head = insert_knuth_val_head(s, pos, str_val);
        uint32_t dist = pos - hash_head;
        if (dist - 1 < max_dist && Z_U32_FROM_LE(zng_memread_4(window + hash_head)) == str_val)
            hits++;
    }
    return hits;
}


/* The block opened on a Z_FINISH call is marked final only when the remaining
 * input is too small for stored-block mode to ever want to close it again.
 * Otherwise the stream ends with an empty final static block (10 bits). */
#define QUICK_FINAL_OK(s, lookahead) ((s)->strm->avail_in + (lookahead) <= MIN_LOOKAHEAD + STORED_CHUNK_MIN)

Z_FORCEINLINE static block_state deflate_quick_impl(deflate_state *s, int flush,
                                                   uint32_t strstart, uint32_t lookahead) {
    unsigned char *window;
    unsigned last = (flush == Z_FINISH) ? 1 : 0;
    uint32_t miss_run = s->miss_run;
    const uint32_t skip_start = s->skip_start;

    if (UNLIKELY(last && s->block_open != 2 && QUICK_FINAL_OK(s, lookahead))) {
        /* Emit end of previous block */
        if (quick_end_block(s, strstart, 0))
            return need_more;
        /* Emit start of last block */
        quick_start_block(s, strstart, 1);
    } else if (UNLIKELY(s->block_open == 0 && lookahead > 0)) {
        /* Start new block only when we have lookahead data, so that if no
           input data is given an empty block will not be written */
        quick_start_block(s, strstart, 0);
    }

    window = s->window;

    for (;;) {
        if (UNLIKELY(s->pending + ((BIT_BUF_SIZE + 7) >> 3) >= s->pending_buf_size)) {
            PREFIX(flush_pending)(s->strm);
            if (s->strm->avail_out == 0) {
                s->lookahead = lookahead;
                s->strstart = strstart;
                s->miss_run = miss_run;
                return (last && s->strm->avail_in == 0 && s->bi_valid == 0 && s->block_open == 0) ? finish_started : need_more;
            }
        }

        if (UNLIKELY(lookahead < MIN_LOOKAHEAD)) {
            s->lookahead = lookahead;
            s->strstart = strstart;
            s->miss_run = miss_run;
            PREFIX(fill_window)(s);
            lookahead = s->lookahead;
            strstart = s->strstart;
            if (UNLIKELY(lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH))
                return need_more;
            if (UNLIKELY(lookahead == 0))
                break;

            if (UNLIKELY(s->block_open == 0)) {
                /* Start new block when we have lookahead data, so that if no
                   input data is given an empty block will not be written */
                quick_start_block(s, strstart, last && QUICK_FINAL_OK(s, lookahead));
            }
        }

        uint32_t str_val = Z_U32_FROM_LE(zng_memread_4(window + strstart));

        if (LIKELY(lookahead >= WANT_MIN_MATCH)) {
            uint32_t hash_head = insert_knuth_val_head(s, strstart, str_val);
            int64_t dist = (int64_t)strstart - hash_head;

            if (dist <= MAX_DIST(s) && dist > 0) {
                const uint8_t *match_start = window + hash_head;
                uint32_t match_val = Z_U32_FROM_LE(zng_memread_4(match_start));

                if (str_val == match_val) {
                    const uint8_t *scan_start = window + strstart;
                    uint32_t match_len = FUNCTABLE_CALL(compare256)(scan_start+2, match_start+2) + 2;

                    if (match_len >= WANT_MIN_MATCH) {
                        if (UNLIKELY(match_len > lookahead))
                            match_len = lookahead;

                        Assert(match_len <= STD_MAX_MATCH, "match too long");
                        Assert(strstart <= UINT16_MAX, "strstart should fit in uint16_t");
                        check_match(s, strstart, hash_head, match_len);

                        zng_tr_emit_dist(s, static_ltree, static_dtree, match_len - STD_MIN_MATCH, (uint32_t)dist);
                        lookahead -= match_len;
                        strstart += match_len;
                        miss_run = 0;
                        continue;
                    }
                }
            }
        }

        zng_tr_emit_lit(s, static_ltree, (uint8_t)str_val);
        strstart++;
        lookahead--;

        /* Literal-run acceleration, see deflate_p.h. Past skip_start consecutive
         * misses, extra positions are coded as literals without being hashed or
         * searched; past STORED_MODE_MISSES, a probed stretch that still looks
         * incompressible is copied as a stored block, which is both faster and
         * smaller than static-tree literals. */
        uint32_t skip = SKIP_STEP_QUICK(s, ++miss_run, skip_start);
        /* Skipping one or two positions at a time costs more than it saves here */
        if (UNLIKELY(skip >= 4)) {
            if (miss_run >= STORED_MODE_MISSES && lookahead > MIN_LOOKAHEAD + STORED_CHUNK_MIN) {
                /* Stored block size: what the pending buffer can take after a flush,
                 * leaving the lookahead the main loop needs at the end. */
                uint32_t n = MIN(lookahead - MIN_LOOKAHEAD, (uint32_t)STORED_CHUNK_MAX);
                uint32_t room;
                PREFIX(flush_pending)(s->strm);
                if (s->pending != 0) {
                    /* Output is full: pending_out has advanced, so nothing may be
                     * appended until the caller drained it. Decide again next call. */
                    s->lookahead = lookahead;
                    s->strstart = strstart;
                    s->miss_run = miss_run;
                    return need_more;
                }
                room = s->pending_buf_size - s->pending;
                if (n + 16 > room)
                    n = room > 16 ? room - 16 : 0;
                n &= ~(uint32_t)(PROBE_STRIDE - 1);
                if (n >= STORED_CHUNK_MIN && s->block_open != 2) {
                    if (quick_probe(s, window, strstart, n) * PROBE_STRIDE * STORED_MAX_HITS >= n) {
                        /* Compressible after all: the probes were hashed, restart the
                         * ramp so the next probe is not immediate */
                        miss_run = skip_start;
                        n = 0;
                    }
                }
                if (n >= STORED_CHUNK_MIN) {
                    /* Almost no probe found a match: store the stretch */
                    if (s->block_open) {
                        zng_tr_emit_end_block(s, static_ltree, 0);
                        s->block_open = 0;
                    }
                    zng_tr_stored_block(s, window + strstart, n, 0);
                    strstart += n;
                    lookahead -= n;
                    quick_start_block(s, strstart, 0);
                    PREFIX(flush_pending)(s->strm);
                    if (s->strm->avail_out == 0) {
                        s->lookahead = lookahead;
                        s->strstart = strstart;
                        s->miss_run = miss_run;
                        return need_more;
                    }
                    continue;
                }
            }
            /* Each literal takes at most 9 bits; keep the skip within the pending
             * buffer room and the lookahead the loop needs */
            uint32_t room = s->pending_buf_size - s->pending;
            skip = MIN(skip, room > 32 ? (room - 32) / 2 : 0);
            if (lookahead > MIN_LOOKAHEAD)
                skip = MIN(skip, lookahead - MIN_LOOKAHEAD);
            else
                skip = 0;
            if (s->skip_insert)
                insert_knuth_batch_head_static(s, window, strstart, skip);
            for (uint32_t i = 0; i < skip; i++)
                zng_tr_emit_lit(s, static_ltree, window[strstart + i]);
            strstart += skip;
            lookahead -= skip;
        }
    }

    s->lookahead = lookahead;
    s->strstart = strstart;
    s->miss_run = miss_run;
    s->insert = strstart < (STD_MIN_MATCH - 1) ? strstart : (STD_MIN_MATCH - 1);
    if (UNLIKELY(last)) {
        if (s->block_open != 2) {
            /* The open block, if any, was not marked final: close it and end the
             * stream with an empty final static block */
            if (s->block_open)
                zng_tr_emit_end_block(s, static_ltree, 0);
            quick_start_block(s, strstart, 1);
        }
        if (quick_end_block(s, strstart, 1))
            return finish_started;
        return finish_done;
    }

    if (quick_end_block(s, strstart, 0))
        return need_more;
    return block_done;
}

Z_INTERNAL block_state deflate_quick(deflate_state *s, int flush) {
    return deflate_quick_impl(s, flush, s->strstart, s->lookahead);
}
