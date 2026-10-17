/* deflate_fast.c -- compress data using the fast strategy of deflation algorithm
 *
 * Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "zmemory.h"
#include "deflate.h"
#include "deflate_p.h"
#include "functable.h"
#include "insert_string_p.h"

/* ===========================================================================
 * Compress as much as possible from the input stream, return the current
 * block state.
 * This function does not perform lazy evaluation of matches and inserts
 * new strings in the dictionary only for unmatched strings or for short
 * matches. It is used only for the fast compression options.
 *
 * The hot loop state (strstart, lookahead, sym_next, the hash tables) lives
 * in locals. The symbol buffer is written with byte-typed stores that the
 * compiler must assume alias the state struct, so keeping these in the struct
 * costs a reload of every one of them per symbol. The locals are written back
 * to the state before every call that reads it.
 */
Z_INTERNAL block_state deflate_fast(deflate_state *s, int flush) {
    unsigned char *window = s->window;
    Pos *head = s->head;
    Pos *prev = s->prev;
    const uint32_t w_mask = W_MASK(s);
    const uint32_t max_dist = MAX_DIST(s);
    const uint32_t max_insert = s->max_insert_length;
    uint32_t strstart = s->strstart;
    uint32_t lookahead = s->lookahead;
    unsigned int sym_next = s->sym_next;
    const unsigned int sym_end = s->sym_end;
    uint32_t match_len = 0;

#define SYNC_STATE() do { s->strstart = strstart; s->lookahead = lookahead; s->sym_next = sym_next; } while (0)
#define LOAD_STATE() do { strstart = s->strstart; lookahead = s->lookahead; sym_next = s->sym_next; } while (0)

    for (;;) {
        uint8_t lc;

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
        if (LIKELY(lookahead >= WANT_MIN_MATCH)) {
            uint32_t str_val = Z_U32_FROM_LE(zng_memread_4(window + strstart));
            uint32_t h, hash_head;

            UPDATE_HASH_KNUTH(h, str_val);
            hash_head = head[h];
            if (LIKELY(hash_head != strstart)) {
                prev[strstart & w_mask] = (Pos)hash_head;
                head[h] = (Pos)strstart;
            }
            lc = (uint8_t)str_val;

            /* Find the longest match.
             * At this point we have always match length < WANT_MIN_MATCH
             */
            uint32_t dist = strstart - hash_head;
            if (dist - 1 < max_dist && hash_head != 0) {
                /* To simplify the code, we prevent matches with the string
                 * of window index 0 (in particular we have to avoid a match
                 * of the string with itself at the start of the input file).
                 */
                s->strstart = strstart;
                s->lookahead = lookahead;
                match_len = FUNCTABLE_CALL(longest_match)(s, hash_head);
                /* longest_match() sets match_start */
            }
        } else {
            lc = window[strstart];
        }

        if (match_len >= WANT_MIN_MATCH) {
            Assert(strstart <= UINT16_MAX, "strstart should fit in uint16_t");
            Assert(s->match_start <= UINT16_MAX, "match_start should fit in uint16_t");
            check_match(s, strstart, s->match_start, match_len);

            sym_next = zng_tr_tally_dist_at(s, sym_next, strstart - s->match_start, match_len - STD_MIN_MATCH);

            lookahead -= match_len;

            /* Insert new strings in the hash table only if the match length
             * is not too large. This saves time but degrades compression.
             */
            if (match_len <= max_insert && lookahead >= WANT_MIN_MATCH) {
                match_len--; /* string at strstart already in table */
                strstart++;

                insert_knuth_batch_static(s, window, strstart, match_len);
                strstart += match_len;
            } else {
                strstart += match_len;
                insert_knuth(s, window, strstart + 2 - STD_MIN_MATCH);

                /* If lookahead < STD_MIN_MATCH, ins_h is garbage, but it does not
                 * matter since it will be recomputed at next deflate call.
                 */
            }
            match_len = 0;
        } else {
            /* No match, output a literal byte */
            sym_next = zng_tr_tally_lit_at(s, sym_next, lc);
            lookahead--;
            strstart++;
        }
        if (UNLIKELY(sym_next == sym_end)) {
            SYNC_STATE();
            FLUSH_BLOCK(s, window, 0);
            sym_next = s->sym_next;
        }
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
