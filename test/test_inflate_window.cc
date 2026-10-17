/* test_inflate_window.cc - inflate() with small output buffers (in-window decoding)
 *
 * zlib-ng's deflate never emits distances above 32506, so round-trip tests
 * cannot reach the far end of the 32K window. These tests hand-build raw
 * deflate streams with distances up to 32768 and decode them with small
 * output buffers, through both the fast loop and the byte-at-a-time path.
 */

#include "zbuild.h"
#ifdef ZLIB_COMPAT
#  include "zlib.h"
#else
#  include "zlib-ng.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include <gtest/gtest.h>

namespace {

/* LSB-first bit writer with MSB-first Huffman code emission (RFC 1951). */
struct bit_writer {
    std::vector<uint8_t> buf;
    uint32_t acc = 0;
    int n = 0;
    void bits(uint32_t v, int cnt) {
        acc |= v << n; n += cnt;
        while (n >= 8) { buf.push_back((uint8_t)acc); acc >>= 8; n -= 8; }
    }
    void huff(uint32_t code, int cnt) {
        uint32_t r = 0;
        for (int i = 0; i < cnt; i++) r = (r << 1) | ((code >> i) & 1);
        bits(r, cnt);
    }
    void align() { if (n) { buf.push_back((uint8_t)acc); acc = 0; n = 0; } }
};

static const int LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const int LEXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const int DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const int DEXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void fixed_litlen(bit_writer &bw, int sym) {
    if (sym <= 143) bw.huff(0x30 + sym, 8);
    else if (sym <= 255) bw.huff(0x190 + sym - 144, 9);
    else if (sym <= 279) bw.huff(sym - 256, 7);
    else bw.huff(0xC0 + sym - 280, 8);
}

static void fixed_match(bit_writer &bw, int length, int dist, std::vector<uint8_t> &expected) {
    int i;
    for (i = 28; i >= 0; i--) if (length >= LBASE[i]) break;
    fixed_litlen(bw, 257 + i); bw.bits(length - LBASE[i], LEXT[i]);
    for (i = 29; i >= 0; i--) if (dist >= DBASE[i]) break;
    bw.huff(i, 5); bw.bits(dist - DBASE[i], DEXT[i]);
    size_t from = expected.size() - dist;
    for (int k = 0; k < length; k++) expected.push_back(expected[from + k]);
}

struct match_op { int length, dist; };

/* A stored block of n pseudo-random bytes followed by one fixed-Huffman block of matches. */
static void build_stream(size_t n, const std::vector<match_op> &ops, std::vector<uint8_t> &stream,
                         std::vector<uint8_t> &expected) {
    uint32_t rng = 0x12345678u;
    expected.clear();
    for (size_t i = 0; i < n; i++) { rng = rng * 1103515245u + 12345u; expected.push_back((uint8_t)(rng >> 16)); }
    bit_writer bw;
    bw.bits(0, 1); bw.bits(0, 2); bw.align();      /* stored, not final */
    bw.bits((uint32_t)n, 16); bw.bits((uint32_t)(~n & 0xffff), 16);
    bw.buf.insert(bw.buf.end(), expected.begin(), expected.end());
    bw.bits(1, 1); bw.bits(1, 2);                   /* fixed, final */
    for (const match_op &op : ops) fixed_match(bw, op.length, op.dist, expected);
    fixed_litlen(bw, 256);
    bw.align();
    stream = bw.buf;
}

/* Decode with avail_out-sized output chunks; instep > 0 feeds the input that many bytes at a time. */
static void decode_chunked(const std::vector<uint8_t> &stream, const std::vector<uint8_t> &expected,
                           uint32_t chunk, uint32_t instep) {
    std::vector<uint8_t> out(expected.size() + 1024, 0);
    PREFIX3(stream) s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(PREFIX(inflateInit2)(&s, -15), Z_OK);
    s.next_in = (z_const unsigned char *)stream.data();
    s.avail_in = instep ? 0 : (uint32_t)stream.size();
    size_t fed = instep ? 0 : stream.size();
    s.next_out = out.data();
    s.avail_out = chunk;
    int ret;
    for (;;) {
        if (instep && s.avail_in == 0 && fed < stream.size()) {
            uint32_t take = (uint32_t)MIN((size_t)instep, stream.size() - fed);
            s.avail_in = take; fed += take;
        }
        ret = PREFIX(inflate)(&s, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        ASSERT_TRUE(ret == Z_OK || ret == Z_BUF_ERROR) << "ret " << ret << " at total_out " << s.total_out;
        ASSERT_FALSE(ret == Z_OK && s.avail_out != 0 && s.avail_in != 0) << "returned with input and output space left";
        if (s.avail_out == 0) s.avail_out = chunk;
        ASSERT_LE(s.total_out, expected.size());
    }
    ASSERT_EQ(PREFIX(inflateEnd)(&s), Z_OK);
    EXPECT_EQ(s.total_out, expected.size());
    EXPECT_EQ(memcmp(out.data(), expected.data(), expected.size()), 0);
}

} // namespace

/* Far distances with the fast loop: short copies whose chunk over-write would clobber
   the oldest history in a circular window, then matches that read exactly that history. */
TEST(inflate_window, far_distance_fast_path) {
    std::vector<uint8_t> stream, expected;
    build_stream(40000, {{5, 100}, {10, 32766}, {7, 32760}, {258, 32768}, {3, 32768}, {20, 32512}, {258, 32768}}, stream, expected);
    for (uint32_t chunk : {64u, 259u, 1000u, 4096u, 8192u, 16383u, 16384u, 65536u})
        decode_chunked(stream, expected, chunk, 0);
}

/* The same through the byte-at-a-time path (input fed 14 bytes per call keeps the fast loop out). */
TEST(inflate_window, far_distance_slow_path) {
    std::vector<uint8_t> stream, expected;
    build_stream(40000, {{5, 100}, {10, 32766}, {7, 32760}, {258, 32768}, {3, 32768}, {20, 32512}, {258, 32768}}, stream, expected);
    for (uint32_t chunk : {64u, 1000u, 4096u, 16384u})
        decode_chunked(stream, expected, chunk, 14);
}

/* Matches spanning the window's end at every alignment, with a window that wraps several times. */
TEST(inflate_window, wraparound) {
    std::vector<match_op> ops;
    for (int i = 0; i < 600; i++) ops.push_back({200 + (i % 59), 32768 - (i % 300)});
    std::vector<uint8_t> stream, expected;
    build_stream(33000, ops, stream, expected);
    for (uint32_t chunk : {64u, 333u, 4096u, 16383u, 20000u})
        decode_chunked(stream, expected, chunk, 0);
    decode_chunked(stream, expected, 777, 14);
}

/* With bytes pending in the window and no output space, inflate must report Z_BUF_ERROR, not Z_OK. */
TEST(inflate_window, no_progress_is_buf_error) {
    std::vector<match_op> ops;
    for (int i = 0; i < 300; i++) ops.push_back({258, 1000 + i});
    std::vector<uint8_t> stream, expected;
    build_stream(5000, ops, stream, expected);
    std::vector<uint8_t> out(expected.size(), 0);
    PREFIX3(stream) s;
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(PREFIX(inflateInit2)(&s, -15), Z_OK);
    s.next_in = (z_const unsigned char *)stream.data();
    s.avail_in = (uint32_t)stream.size();
    s.next_out = out.data();
    s.avail_out = 64;
    int ret;
    for (;;) {
        ret = PREFIX(inflate)(&s, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        ASSERT_EQ(ret, Z_OK);
        if (s.avail_out == 0) {
            ASSERT_EQ(PREFIX(inflate)(&s, Z_NO_FLUSH), Z_BUF_ERROR);
            /* past the stored block, pending output must not be reported as a stored block in progress */
            long mark = PREFIX(inflateMark)(&s);
            if (s.total_out > 5000)
                ASSERT_FALSE((mark >> 16) == -1 && (mark & 0xffff) != 0);
            s.avail_out = (uint32_t)MIN((size_t)64, expected.size() - s.total_out);
        }
    }
    ASSERT_EQ(PREFIX(inflateEnd)(&s), Z_OK);
    EXPECT_EQ(s.total_out, expected.size());
    EXPECT_EQ(memcmp(out.data(), expected.data(), expected.size()), 0);
}

/* A preset dictionary of nearly the window size, then a one-shot Z_FINISH with the exact output size. */
TEST(inflate_window, dictionary_then_finish) {
    std::vector<uint8_t> dict(32000), data(10000);
    uint32_t rng = 7;
    for (auto &b : dict) { rng = rng * 1103515245u + 12345u; b = (uint8_t)("acgt"[(rng >> 16) & 3]); }
    for (size_t i = 0; i < data.size(); i++) data[i] = dict[(i * 7) % dict.size()];
    for (int wbits : {15, -15}) {
        std::vector<uint8_t> compr(PREFIX(compressBound)((unsigned long)data.size()));
        PREFIX3(stream) c;
        memset(&c, 0, sizeof(c));
        ASSERT_EQ(PREFIX(deflateInit2)(&c, 6, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY), Z_OK);
        ASSERT_EQ(PREFIX(deflateSetDictionary)(&c, dict.data(), (uint32_t)dict.size()), Z_OK);
        c.next_in = data.data(); c.avail_in = (uint32_t)data.size();
        c.next_out = compr.data(); c.avail_out = (uint32_t)compr.size();
        ASSERT_EQ(PREFIX(deflate)(&c, Z_FINISH), Z_STREAM_END);
        size_t clen = c.total_out;
        ASSERT_EQ(PREFIX(deflateEnd)(&c), Z_OK);

        for (int flush : {Z_FINISH, Z_NO_FLUSH, Z_SYNC_FLUSH}) {
            std::vector<uint8_t> out(data.size(), 0);
            PREFIX3(stream) s;
            memset(&s, 0, sizeof(s));
            ASSERT_EQ(PREFIX(inflateInit2)(&s, wbits), Z_OK);
            s.next_in = compr.data(); s.avail_in = (uint32_t)clen;
            s.next_out = out.data(); s.avail_out = (uint32_t)out.size();
            if (wbits < 0)
                ASSERT_EQ(PREFIX(inflateSetDictionary)(&s, dict.data(), (uint32_t)dict.size()), Z_OK);
            int ret = PREFIX(inflate)(&s, flush);
            if (ret == Z_NEED_DICT) {
                ASSERT_EQ(PREFIX(inflateSetDictionary)(&s, dict.data(), (uint32_t)dict.size()), Z_OK);
                ret = PREFIX(inflate)(&s, flush);
            }
            ASSERT_EQ(ret, Z_STREAM_END) << "wbits " << wbits << " flush " << flush;
            EXPECT_EQ(s.total_out, data.size());
            EXPECT_EQ(memcmp(out.data(), data.data(), data.size()), 0);
            uint8_t got[32768];
            uint32_t gotlen = 0;
            ASSERT_EQ(PREFIX(inflateGetDictionary)(&s, got, &gotlen), Z_OK);
            ASSERT_LE(gotlen, 32768u);
            if (wbits < 0)      /* the raw window holds the dictionary plus the output, in order */
                EXPECT_EQ(memcmp(got + gotlen - 100, out.data() + out.size() - 100, 100), 0);
            ASSERT_EQ(PREFIX(inflateEnd)(&s), Z_OK);
        }
    }
}
