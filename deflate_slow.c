/* deflate_slow.c -- compress data using the slow strategy of deflation algorithm
 *
 * Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "deflate.h"
#include "deflate_p.h"
#include "insert_string_p.h"

/* ===========================================================================
 * Same as deflate_medium, but achieves better compression. We use a lazy
 * evaluation for matches: a match is finally adopted only if there is
 * no better match at the next window position.
 *
 * The hot loop state (strstart, lookahead, sym_next, hash tables) lives in
 * locals, see deflate_fast.c for why. The state is written back before every
 * call that reads it (fill_window, longest_match, block flush).
 */
Z_INTERNAL block_state deflate_slow(deflate_state *s, int flush) {
    longest_match_func longest_match = s->longest_match;
    insert_batch_func insert_batch = s->insert_batch;
    unsigned char *window = s->window;
    Pos *head = s->head;
    Pos *prev = s->prev;
    const uint32_t w_mask = W_MASK(s);
    const uint32_t max_dist = MAX_DIST(s);
    const uint32_t max_lazy = s->max_lazy_match;
    int level = s->level;
    uint32_t strstart = s->strstart;
    uint32_t lookahead = s->lookahead;
    unsigned int sym_next = s->sym_next;
    const unsigned int sym_end = s->sym_end;

    /* Matches this long or shorter are discarded. Z_FILTERED ignores lengths up to 5, other
     * strategies discard nothing. */
    const uint32_t match_discard = (s->strategy == Z_FILTERED) ? 5 : 0;
    /* Same floor for longest_match, kept below good_match. */
    const uint32_t match_floor = MIN(match_discard, s->good_match - 1);

#define SYNC_STATE() do { s->strstart = strstart; s->lookahead = lookahead; s->sym_next = sym_next; } while (0)
#define LOAD_STATE() do { strstart = s->strstart; lookahead = s->lookahead; sym_next = s->sym_next; } while (0)

    /* Process the input block. */
    for (;;) {
        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need STD_MAX_MATCH bytes
         * for the next match, plus WANT_MIN_MATCH bytes to insert the
         * string following the next match.
         */
        if (UNLIKELY(lookahead < MIN_LOOKAHEAD)) {
            SYNC_STATE();
            PREFIX(fill_window)(s);
            LOAD_STATE();
            if (UNLIKELY(lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH)) {
                return need_more;
            }
            if (UNLIKELY(lookahead == 0))
                break; /* flush the current block */
        }

        /* Insert the string window[strstart .. strstart+2] in the
         * dictionary, and set hash_head to the head of the hash chain:
         */
        uint32_t hash_head = 0;
        if (LIKELY(lookahead >= WANT_MIN_MATCH)) {
            uint32_t h;
            if (level >= MIN_ROLL_LEVEL) {
                h = s->ins_h;
                UPDATE_HASH_ROLL(h, window[strstart + (STD_MIN_MATCH-1)]);
                s->ins_h = h;
            } else {
                uint32_t val = Z_U32_FROM_LE(zng_memread_4(window + strstart));
                UPDATE_HASH_KNUTH(h, val);
            }
            hash_head = head[h];
            if (LIKELY(hash_head != strstart)) {
                prev[strstart & w_mask] = (Pos)hash_head;
                head[h] = (Pos)strstart;
            }
        }

        /* Find the longest match, discarding those <= prev_length.
         */
        s->prev_match = s->match_start;
        uint32_t prev_length = s->prev_length;
        uint32_t match_len = STD_MIN_MATCH - 1;
        uint32_t dist = strstart - hash_head;

        if (dist - 1 < max_dist && prev_length < max_lazy && hash_head != 0) {
            /* To simplify the code, we prevent matches with the string
             * of window index 0 (in particular we have to avoid a match
             * of the string with itself at the start of the input file).
             */

            /* longest_match only looks for matches longer than s->prev_length. */
            s->prev_length = MAX(prev_length, match_floor);
            s->strstart = strstart;
            s->lookahead = lookahead;
            match_len = longest_match(s, hash_head);
            /* Restore the real previous length, the lazy evaluation below relies on it. */
            s->prev_length = prev_length;
            /* longest_match() sets match_start */

            if (match_len <= match_discard) {
                /* Match not long enough, treat it as no match found, which makes a garbage
                 * match_start that is harmless. */
                match_len = STD_MIN_MATCH - 1;
            }
        }
        /* If there was a match at the previous step and the current
         * match is not better, output the previous match:
         */
        if (prev_length >= STD_MIN_MATCH && match_len <= prev_length) {
            uint32_t max_insert = strstart + lookahead - STD_MIN_MATCH;
            /* Do not insert strings in hash table beyond this. */

            Assert((strstart-1) <= UINT16_MAX, "strstart-1 should fit in uint16_t");
            check_match(s, strstart - 1, s->prev_match, prev_length);

            sym_next = zng_tr_tally_dist_at(s, sym_next, strstart - 1 - s->prev_match, prev_length - STD_MIN_MATCH);

            /* Insert in hash table all strings up to the end of the match.
             * strstart-1 and strstart are already inserted. If there is not
             * enough lookahead, the last two strings are not inserted in
             * the hash table.
             */
            prev_length -= 1;
            lookahead -= prev_length;

            uint32_t mov_fwd = prev_length - 1;
            if (max_insert > strstart) {
                uint32_t insert_cnt = mov_fwd;
                if (UNLIKELY(insert_cnt > max_insert - strstart))
                    insert_cnt = max_insert - strstart;
                insert_batch(s, window, strstart + 1, insert_cnt);
            }
            s->prev_length = 0;
            s->match_available = 0;
            strstart += mov_fwd + 1;

            if (UNLIKELY(sym_next == sym_end)) {
                SYNC_STATE();
                FLUSH_BLOCK(s, window, 0);
                sym_next = s->sym_next;
            }

        } else if (s->match_available) {
            /* If there was no match at the previous position, output a
             * single literal. If there was a match but the current match
             * is longer, truncate the previous match to a single literal.
             */
            sym_next = zng_tr_tally_lit_at(s, sym_next, window[strstart-1]);
            if (UNLIKELY(sym_next == sym_end)) {
                /* The block ends before this position is advanced past. avail_out can
                 * only reach zero inside the flush, so it is checked just there. */
                SYNC_STATE();
                FLUSH_BLOCK_ONLY(s, window, 0);
                sym_next = s->sym_next;
                s->prev_length = match_len;
                strstart++;
                lookahead--;
                if (UNLIKELY(s->strm->avail_out == 0)) {
                    SYNC_STATE();
                    return need_more;
                }
            } else {
                s->prev_length = match_len;
                strstart++;
                lookahead--;
            }
        } else {
            /* There is no previous match to compare with, wait for
             * the next step to decide.
             */
            s->prev_length = match_len;
            s->match_available = 1;
            strstart++;
            lookahead--;
        }
    }
    Assert(flush != Z_NO_FLUSH, "no flush?");
    if (UNLIKELY(s->match_available)) {
        sym_next = zng_tr_tally_lit_at(s, sym_next, window[strstart-1]);
        s->match_available = 0;
    }
    SYNC_STATE();
    s->insert = strstart < (STD_MIN_MATCH - 1) ? strstart : (STD_MIN_MATCH - 1);
    if (UNLIKELY(flush == Z_FINISH)) {
        FLUSH_BLOCK(s, window, 1);
        return finish_done;
    }
    if (UNLIKELY(s->sym_next))
        FLUSH_BLOCK(s, window, 0);
    return block_done;
#undef SYNC_STATE
#undef LOAD_STATE
}
