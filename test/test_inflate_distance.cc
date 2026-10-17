/* test_inflate_distance.cc - inflate() with match distances up to 32768
 *
 * zlib-ng's deflate never emits a distance above MAX_DIST (32506), so no
 * round-trip test reaches the last 262 bytes of the inflate window. Other
 * encoders (libdeflate, zopfli, 7-zip) use the full 32768. These tests
 * hand-build raw deflate streams with such distances and decode them with
 * output buffers from 64 bytes up, through both the fast loop and the
 * byte-at-a-time path, so that the window's far end is covered.
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
TEST(inflate_distance, far_distance_fast_path) {
    std::vector<uint8_t> stream, expected;
    build_stream(40000, {{5, 100}, {10, 32766}, {7, 32760}, {258, 32768}, {3, 32768}, {20, 32512}, {258, 32768}}, stream, expected);
    for (uint32_t chunk : {64u, 259u, 1000u, 4096u, 8192u, 16383u, 16384u, 65536u})
        decode_chunked(stream, expected, chunk, 0);
}

/* The same through the byte-at-a-time path (input fed 14 bytes per call keeps the fast loop out). */
TEST(inflate_distance, far_distance_slow_path) {
    std::vector<uint8_t> stream, expected;
    build_stream(40000, {{5, 100}, {10, 32766}, {7, 32760}, {258, 32768}, {3, 32768}, {20, 32512}, {258, 32768}}, stream, expected);
    for (uint32_t chunk : {64u, 1000u, 4096u, 16384u})
        decode_chunked(stream, expected, chunk, 14);
}

/* Matches spanning the window's end at every alignment, with a window that wraps several times. */
TEST(inflate_distance, wraparound) {
    std::vector<match_op> ops;
    for (int i = 0; i < 600; i++) ops.push_back({200 + (i % 59), 32768 - (i % 300)});
    std::vector<uint8_t> stream, expected;
    build_stream(33000, ops, stream, expected);
    for (uint32_t chunk : {64u, 333u, 4096u, 16383u, 20000u})
        decode_chunked(stream, expected, chunk, 0);
    decode_chunked(stream, expected, 777, 14);
}
