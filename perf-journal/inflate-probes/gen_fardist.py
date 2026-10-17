import random, sys, zlib
random.seed(1234)
N = 40000
data = bytes(random.getrandbits(8) for _ in range(N))

class BW:
    def __init__(s): s.buf = bytearray(); s.acc = 0; s.n = 0
    def bits(s, v, n):           # LSB-first
        s.acc |= v << s.n; s.n += n
        while s.n >= 8:
            s.buf.append(s.acc & 0xff); s.acc >>= 8; s.n -= 8
    def huff(s, code, n):        # Huffman codes are MSB-first
        r = 0
        for i in range(n): r = (r << 1) | ((code >> i) & 1)
        s.bits(r, n)
    def align(s):
        if s.n: s.buf.append(s.acc & 0xff); s.acc = 0; s.n = 0
    def flush(s): s.align(); return bytes(s.buf)

def litlen(bw, sym):
    if sym <= 143: bw.huff(0x30 + sym, 8)
    elif sym <= 255: bw.huff(0x190 + sym - 144, 9)
    elif sym <= 279: bw.huff(sym - 256, 7)
    else: bw.huff(0xC0 + sym - 280, 8)

LBASE = [3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258]
LEXT  = [0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0]
DBASE = [1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577]
DEXT  = [0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13]

def match(bw, length, dist):
    for i in range(28, -1, -1):
        if length >= LBASE[i]:
            litlen(bw, 257 + i); bw.bits(length - LBASE[i], LEXT[i]); break
    for i in range(29, -1, -1):
        if dist >= DBASE[i]:
            bw.huff(i, 5); bw.bits(dist - DBASE[i], DEXT[i]); break

bw = BW()
# stored block, not final
bw.bits(0, 1); bw.bits(0, 2); bw.align()
bw.buf += N.to_bytes(2, 'little') + (N ^ 0xffff).to_bytes(2, 'little') + data
# fixed block, final
bw.bits(1, 1); bw.bits(1, 2)
out = bytearray(data)
tokens = [(5, 100), (10, 32766), (7, 32760), (258, 32768)]
for L, D in tokens:
    match(bw, L, D)
    for _ in range(L): out.append(out[-D])
litlen(bw, 256)
stream = bw.flush() + bytes(64)   # trailing padding so the fast loop has >= 15 bytes of input
assert zlib.decompress(stream, -15) == bytes(out)
open(sys.argv[1], 'wb').write(stream)
open(sys.argv[2], 'wb').write(bytes(out))
print(len(stream), len(out))
