/* deflate_medium_tpl.h -- The deflate_medium deflate strategy
 *
 * Copyright (C) 2013 Intel Corporation. All rights reserved.
 * Authors:
 *  Arjan van de Ven    <arjan@linux.intel.com>
 *
 * Copyright (C) 2026 Hans Kristian Rosbach
 *
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* insert_match assumes:
 * - lookahead > match.match_length + WANT_MIN_MATCH
 * - match_len >= WANT_MIN_MATCH
 */
Z_FORCEINLINE static void SUFFIX(insert_match)(deflate_state *s, unsigned char *Z_RESTRICT window, struct match match, const uint32_t max_len) {
    uint32_t start;
    uint32_t match_len = match.match_length;
    uint32_t strstart = match.strstart + 1; // string at strstart already in table
    uint32_t end = strstart + match_len - 1;

    /* Insert new strings in the hash table only if the match length
     * is not too large. This saves time but degrades compression.
     */
    if (UNLIKELY(match_len > max_len)) {
        // For too long matches, insert only the tail position.
        start = end - 1;
    } else {
        start = strstart;
    }

#ifdef USE_FIZZLE
    if (UNLIKELY(start < match.orgstart))
        start = match.orgstart;
#endif

    insert_knuth_batch_static(s, window, start, end - start);
}

/* Look up the longest match for the string at strstart. The caller has already
 * stored strstart and lookahead into the state, which longest_match reads. */
Z_FORCEINLINE static struct match SUFFIX(find_best_match)(deflate_state *s, uint32_t strstart, uint32_t hash_head, uint32_t max_dist) {
    struct match m;
    uint32_t dist;

    m.strstart = strstart;
#ifdef USE_FIZZLE
    m.orgstart = strstart;
#else
    m.orgstart = 0; // For sanitizer
#endif

    dist = strstart - hash_head;
    if (dist - 1 < max_dist && hash_head != 0) {
        /* To simplify the code, we prevent matches with the string
         * of window index 0 (in particular we have to avoid a match
         * of the string with itself at the start of the input file).
         */
        m.match_length = FUNCTABLE_CALL(longest_match)(s, hash_head);
        m.match_start = s->match_start;
        if (UNLIKELY(m.match_length < WANT_MIN_MATCH))
            m.match_length = 1;
        if (UNLIKELY(m.match_start >= m.strstart)) {
            /* this can happen due to some restarts */
            m.match_length = 1;
        }
    } else {
        /* Set up the match to be a 1 byte literal */
        m.match_start = 0;
        m.match_length = 1;
    }

    return m;
}

/* fizzle_matches investigates whether next_match (which starts after current_match) can grow backwards
 * to absorb current_match entirely, or reduce it to a single literal.
 * This occurs because next_match points to a different historical dictionary position, allowing it to discover
 * a better matching alignment that current_match bypassed due to the medium-strategy skipping positions.
 *
 * fizzle_matches assumes:
 * - current_match.match_length > 1
 * - current_match.match_length - 1 <= next->match_start
 * - current_match.match_length - 1 <= next->strstart
 * - next_match.match_length >= WANT_MIN_MATCH
 *
 * fizzle_matches returns:
 * - If successful, current and next are returned modified.
 *   - current_match.match_length is then either 0, 1
 */
#ifdef USE_FIZZLE
static void fizzle_matches(unsigned char *Z_RESTRICT window, struct match *Z_RESTRICT current,
                           struct match *Z_RESTRICT next, int32_t max_dist) {
    unsigned char *match = window + next->match_start + 1 - current->match_length;
    unsigned char *orig  = window + next->strstart + 1 - current->match_length;

    /* quick exit check.. if this fails then don't bother with anything else */
    if (LIKELY(*match != *orig))
        return;

    int32_t limit = (int32_t)next->strstart > max_dist ? (int32_t)next->strstart - max_dist : 0;

    // Steps needed to successfully fizzle match
    uint32_t need = current->match_length - 1;

    // Protect next->strstart from moving past maximum distance
    int32_t max_steps_to_limit = (int32_t)next->strstart - limit;

    // Protect next->match_length from exceeding 256
    int32_t max_growth_allowed = 256 - (int32_t)next->match_length;

    // Protect next->match_start from going too far back
    int32_t max_steps_to_history = (int32_t)next->match_start - 1;

    // steps is the max number of backward steps allowed for each limitation
    int32_t steps1 = MIN((int32_t)current->match_length, max_steps_to_limit);
    int32_t steps2 = MIN(max_growth_allowed, max_steps_to_history);
    int32_t steps = MIN(steps1, steps2);

    // If we can't possibly fizzle the current match out, return early
    if (LIKELY(steps < (int32_t)need))
        return;

    // The quick exit above already checked the first byte of that range.
    // Compare the whole range here.
    if (LIKELY(memcmp(match, orig, (size_t)need) != 0))
        return;

    // Check whether the final extra backward byte is also possible.
    // This decides whether the current match becomes length 1 or 0.
    int extra_byte_ok = (steps == (int32_t)current->match_length);

    // Update variables, reduces current->match_length to 1.
    next->match_start  = next->match_start - need;
    next->strstart     = next->strstart - need;
    next->match_length = next->match_length + need;
    next->orgstart++;
    current->match_length = 1;

    // If every constraint allowed one more backward byte, test it.
    // If it matches, the current match is fully absorbed and becomes length 0.
    if (extra_byte_ok) {
        match = window + next->match_start - 1;
        orig  = window + next->strstart - 1;

        if (*match == *orig) {
            next->match_start--;
            next->strstart--;
            next->match_length++;
            current->match_length = 0;
        }
    }
}
#endif

/* ===========================================================================
 * The hot loop state (strstart, lookahead, sym_next, hash tables) lives in
 * locals, see deflate_fast.c for why. The state is written back before every
 * call that reads it (fill_window, longest_match, block flush).
 */
Z_INTERNAL block_state SUFFIX(deflate_medium)(deflate_state *s, int flush) {
    ALIGNED_(16) struct match current_match = {0};
#ifdef USE_FIZZLE
                 struct match next_match = {0};
    uint32_t window_end = s->window_size - MIN_LOOKAHEAD;
#endif
    uint32_t max_len = (16 * s->max_insert_length);
    unsigned char *window = s->window;
    Pos *head = s->head;
    Pos *prev = s->prev;
    const uint32_t w_mask = W_MASK(s);
    const uint32_t max_dist = MAX_DIST(s);
    uint32_t strstart = s->strstart;
    uint32_t lookahead = s->lookahead;
    unsigned int sym_next = s->sym_next;
    const unsigned int sym_end = s->sym_end;

#define SYNC_STATE() do { s->strstart = strstart; s->lookahead = lookahead; s->sym_next = sym_next; } while (0)
#define LOAD_STATE() do { strstart = s->strstart; lookahead = s->lookahead; sym_next = s->sym_next; } while (0)
/* Insert the string at pos and return the previous chain head */
#define INSERT_AT(pos, hash_head_out) do { \
        uint32_t val_ = Z_U32_FROM_LE(zng_memread_4(window + (pos))); \
        uint32_t h_; \
        UPDATE_HASH_KNUTH(h_, val_); \
        hash_head_out = head[h_]; \
        if (LIKELY(hash_head_out != (pos))) { \
            prev[(pos) & w_mask] = (Pos)hash_head_out; \
            head[h_] = (Pos)(pos); \
        } \
    } while (0)

    for (;;) {
        uint32_t curr_match_len;

        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need STD_MAX_MATCH bytes
         * for the next match, plus WANT_MIN_MATCH bytes to insert the
         * string following the next current_match.
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
#ifdef USE_FIZZLE
            next_match.match_length = 0;
#endif
        }

        /* Insert the string window[strstart .. strstart+2] in the
         * dictionary, and set hash_head to the head of the hash chain:
         */

#ifdef USE_FIZZLE
        /* If we already have a future match from a previous round, just use that */
        if (next_match.match_length > 0) {
            current_match = next_match;
            next_match.match_length = 0;
        } else {
            uint32_t hash_head = 0;   /* head of the hash chain */
            if (LIKELY(lookahead >= WANT_MIN_MATCH)) {
                INSERT_AT(strstart, hash_head);
            }

            s->strstart = strstart;
            s->lookahead = lookahead;
            current_match = SUFFIX(find_best_match)(s, strstart, hash_head, max_dist);
        }
        curr_match_len = current_match.match_length;

        if (curr_match_len >= WANT_MIN_MATCH && lookahead > (curr_match_len + WANT_MIN_MATCH)) {
            SUFFIX(insert_match)(s, window, current_match, max_len);
        }

        /* now, look ahead one */
        if (LIKELY(lookahead > MIN_LOOKAHEAD && (current_match.strstart + curr_match_len) < window_end)) {
            uint32_t next_start = current_match.strstart + curr_match_len;
            uint32_t hash_head;
            INSERT_AT(next_start, hash_head);

            s->strstart = next_start;
            s->lookahead = lookahead;
            next_match = SUFFIX(find_best_match)(s, next_start, hash_head, max_dist);

            uint32_t tmp_cmatch_len_sub = curr_match_len - 1;
            if (tmp_cmatch_len_sub
                     && next_match.match_length >= WANT_MIN_MATCH
                     && tmp_cmatch_len_sub <= next_match.match_start) {
                fizzle_matches(window, &current_match, &next_match, (int32_t)max_dist);
                curr_match_len = current_match.match_length;
            }

            strstart = current_match.strstart;
            if (curr_match_len == 0) {
                /* If current match fizzled out, jump to next loop iteration */
                continue;
            }
        } else {
            next_match.match_length = 0;
        }
#else
        // WITHOUT_FIZZLE: We don't look for future matches, so code is much simplified
        uint32_t hash_head = 0;   /* head of the hash chain */
        if (LIKELY(lookahead >= WANT_MIN_MATCH)) {
            INSERT_AT(strstart, hash_head);
        }

        s->strstart = strstart;
        s->lookahead = lookahead;
        current_match = SUFFIX(find_best_match)(s, strstart, hash_head, max_dist);
        curr_match_len = current_match.match_length;

        if (curr_match_len >= WANT_MIN_MATCH && lookahead > (curr_match_len + WANT_MIN_MATCH)) {
            SUFFIX(insert_match)(s, window, current_match, max_len);
        }
#endif

        /* now emit the current match */
        lookahead -= curr_match_len;
        if (LIKELY(curr_match_len == 1)) {
            /* matches shorter than WANT_MIN_MATCH are set to 1, we need to emit these as literals */
            sym_next = zng_tr_tally_lit_at(s, sym_next, window[current_match.strstart]);
        } else {
            check_match(s, current_match.strstart, current_match.match_start, curr_match_len);
            sym_next = zng_tr_tally_dist_at(s, sym_next, current_match.strstart - current_match.match_start, curr_match_len - STD_MIN_MATCH);
        }

        /* move the "cursor" forward */
        strstart += curr_match_len;

        if (UNLIKELY(sym_next == sym_end)) {
            SYNC_STATE();
            FLUSH_BLOCK(s, window, 0);
            sym_next = s->sym_next;
        }
    }

    SYNC_STATE();
    s->insert = strstart < (STD_MIN_MATCH - 1) ? strstart : (STD_MIN_MATCH - 1);
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, window, 1);
        return finish_done;
    }
    if (UNLIKELY(s->sym_next))
        FLUSH_BLOCK(s, window, 0);

    return block_done;
#undef SYNC_STATE
#undef LOAD_STATE
#undef INSERT_AT
}
