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
 *
 * Literals skipped by the literal-run acceleration are not tallied immediately:
 * they form a pending run just before the lazy position, and a match that is
 * emitted later can grow backwards over them. Together with the lazy literal
 * at strstart-1 that match_available tracks, every position below strstart is
 * accounted for whenever a block is flushed.
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
    uint32_t miss_run = s->miss_run;
    const uint32_t skip_start = s->skip_start;
    uint32_t pend = 0;  /* literals just before the lazy position that are not tallied yet */

    /* Matches this long or shorter are discarded. Z_FILTERED ignores lengths up to 5, other
     * strategies discard nothing. */
    const uint32_t match_discard = (s->strategy == Z_FILTERED) ? 5 : 0;
    /* Same floor for longest_match, kept below good_match. */
    const uint32_t match_floor = MIN(match_discard, s->good_match - 1);

#define SYNC_STATE() do { s->strstart = strstart; s->lookahead = lookahead; s->sym_next = sym_next; s->miss_run = miss_run; } while (0)
#define LOAD_STATE() do { strstart = s->strstart; lookahead = s->lookahead; sym_next = s->sym_next; } while (0)
/* Tally the pending literals, which end just before window position end_pos */
#define TALLY_PENDING(end_pos) do { \
        if (pend) { \
            sym_next = zng_tr_tally_lits_at(s, sym_next, window + (end_pos) - pend, pend); \
            pend = 0; \
        } \
    } while (0)

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
                /* The pending run ends before the lazy literal, if there is one */
                TALLY_PENDING(strstart - s->match_available);
                SYNC_STATE();
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
            uint32_t match_pos = strstart - 1;
            uint32_t match_start = s->prev_match;
            uint32_t emit_len = prev_length;

            /* Grow the match backwards over the pending literals: positions that
             * were skipped by the literal-run acceleration were never searched, and
             * a searched position only saw its own hash chain. The end of the match
             * stays where it was, so the bookkeeping below uses prev_length. */
            while (pend && emit_len < STD_MAX_MATCH && match_start > 0 &&
                   window[match_pos - 1] == window[match_start - 1]) {
                match_pos--;
                match_start--;
                emit_len++;
                pend--;
            }
            TALLY_PENDING(match_pos);

            Assert(match_pos <= UINT16_MAX, "match_pos should fit in uint16_t");
            check_match(s, match_pos, match_start, emit_len);

            sym_next = zng_tr_tally_dist_at(s, sym_next, match_pos - match_start, emit_len - STD_MIN_MATCH);

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
            SKIP_HIT(miss_run);

            if (UNLIKELY(sym_next == sym_end)) {
                SYNC_STATE();
                FLUSH_BLOCK(s, window, 0);
                sym_next = s->sym_next;
            }

        } else if (s->match_available) {
            /* If there was no match at the previous position, output a
             * single literal. If there was a match but the current match
             * is longer, truncate the previous match to a single literal.
             * Pending skipped literals precede it and go first.
             * The block boundary, if a flush follows, is strstart: everything below
             * it has been emitted, the position at strstart is still being decided. */
            if (UNLIKELY(pend))
                TALLY_PENDING(strstart - 1);
            sym_next = zng_tr_tally_lit_at(s, sym_next, window[strstart-1]);
            if (UNLIKELY(sym_next == sym_end)) {
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
                goto accelerate;
            }
            s->prev_length = match_len;
            strstart++;
            lookahead--;

accelerate:
            /* Literal-run acceleration: when neither the previous nor the current
             * position matched, skip positions without searching. The current
             * position (now strstart-1) is a lazy literal; it becomes the pending run
             * together with the skipped positions, which then leaves nothing lazy.
             * The symbol buffer has room for at least one symbol here, and must keep
             * room for the pending run plus one more. */
            if (match_len < STD_MIN_MATCH) {
                uint32_t skip = SKIP_STEP(s, ++miss_run, skip_start);
                if (UNLIKELY(skip)) {
                    uint32_t room = SYM_ROOM(sym_next, sym_end);
                    skip = room > 2 ? MIN(skip, room - 2) : 0;
                    if (lookahead > MIN_LOOKAHEAD)
                        skip = MIN(skip, lookahead - MIN_LOOKAHEAD);
                    else
                        skip = 0;
                    if (skip) {
                        if (s->skip_insert == 1) {
                            /* Continues the hash from strstart-1, which keeps the
                             * rolling hash consistent as well */
                            insert_batch(s, window, strstart, skip);
                        } else {
                            zng_deflate_insert_stride(s, window, strstart, skip, s->skip_insert);
                            if (level >= MIN_ROLL_LEVEL) {
                                /* Re-seed the rolling hash for the next position */
                                s->ins_h = update_hash_roll(window[strstart + skip], window[strstart + skip + 1]);
                            }
                        }
                        pend = 1 + skip;
                        lookahead -= skip;
                        strstart += skip;
                        s->prev_length = 0;
                        s->match_available = 0;
                        if (UNLIKELY(pend >= PEND_MAX || SYM_SHORT(sym_next, sym_end, pend + 2))) {
                            TALLY_PENDING(strstart);
                            if (UNLIKELY(sym_next == sym_end)) {
                                SYNC_STATE();
                                FLUSH_BLOCK(s, window, 0);
                                sym_next = s->sym_next;
                            }
                        }
                    }
                }
            } else {
                SKIP_HIT(miss_run);
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
        pend++;
        s->match_available = 0;
    }
    TALLY_PENDING(strstart);
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
#undef TALLY_PENDING
}
