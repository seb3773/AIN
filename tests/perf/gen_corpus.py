#!/usr/bin/env python3
"""Deterministic edge-case corpus for bit-exact encoder validation.

Usage: gen_corpus.py OUTDIR

Every file is generated from a fixed seed, so two runs produce identical
bytes.  The set deliberately targets the encoder's fragile spots:
  * sizes around 0/1/2/3/4, MAX_MATCH (256) and the zero-padding margin,
  * the M1/M2 window (0x7FFF / 0x8000 / 0x8100), the 16-bit bp wrap (0x10000),
    and the M3 lap / slide boundaries,
  * matches of length 255/256/257, distances 1, 2, 0x7FFF, 0x8000, 0xFFFF,
  * small alphabets (many hash collisions, long chains), runs, text, binary.
"""
import os, random, sys

out = sys.argv[1]
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
corpus = os.path.join(root, "corpus")
os.makedirs(out, exist_ok=True)

def w(name, data):
    with open(os.path.join(out, name), "wb") as f:
        f.write(bytes(data))

R = random.Random(0xA1B2C3)
def rnd(n, alpha=256, r=R):
    return bytes(r.randrange(alpha) for _ in range(n))

eng = open(os.path.join(corpus, "eng.txt"), "rb").read()
code = open(os.path.join(corpus, "code.c"), "rb").read()

# 1) tiny / boundary sizes, three content flavours each
for n in (1, 2, 3, 4, 5, 7, 8, 9, 255, 256, 257, 258, 259, 260, 261, 511, 512, 513):
    w(f"rnd_{n}.bin", rnd(n))
    w(f"run_{n}.bin", b"A" * n)
    w(f"txt_{n}.txt", eng[:n])

# 2) window / wrap boundary sizes (text-like + small-alphabet content)
for n in (0x7FFE, 0x7FFF, 0x8000, 0x8001, 0x80FF, 0x8100, 0x8101,
          0xFFFF, 0x10000, 0x10001, 0x100FF, 0x10100, 0x10101,
          0x17FFF, 0x18000, 0x20000, 0x20101):
    w(f"eng_{n:05x}.txt", (eng * (n // len(eng) + 1))[:n])
    w(f"a4_{n:05x}.bin", rnd(n, 4))

# 3) small alphabets: long chains, many collisions
for a in (2, 3, 16, 64):
    w(f"alpha{a}_200k.bin", rnd(200000, a))

# 4) runs with sparse noise, period patterns
r = random.Random(7)
b = bytearray(300000)
for i in range(0, len(b), r.randrange(50, 900) or 1):
    b[i] = r.randrange(256)
w("sparse_noise.bin", b)
w("period2.bin", b"\x12\x34" * 70000)
w("period3.bin", b"abc" * 50000)
w("period257.bin", (rnd(257) * 600))
w("period255.bin", (rnd(255) * 600))

# 5) exact match lengths / distances at window edges
for dist in (1, 2, 3, 0x7FFE, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0x10000):
    blk = rnd(300)
    filler = rnd(max(dist - 300, 0), 200)
    w(f"dist_{dist:05x}.bin", blk + filler[: max(dist - 300, 0)] + blk + rnd(64))
for L in (254, 255, 256, 257, 258, 512, 513):
    blk = rnd(L)
    w(f"mlen_{L}.bin", rnd(100) + blk + rnd(37) + blk + rnd(5) + blk)

# 6) mutated text: realistic matches with local edits
t = bytearray((eng * 20)[:400000])
r = random.Random(99)
for _ in range(6000):
    t[r.randrange(len(t))] = r.randrange(32, 127)
w("eng_mutated.txt", t)
w("code_x8.c", code * 8)

# 7) self binary / structured records
rec = bytearray()
r = random.Random(5)
for i in range(30000):
    rec += i.to_bytes(4, "little") + bytes([r.randrange(4)]) * 3 + b"REC" + bytes([i & 0xFF])
w("records2.bin", rec)

# 8) ends exactly on match / pad-sensitive tails
w("tail_match.bin", eng[:5000] + eng[1000:1256])
w("tail_zero.bin", eng[:5000] + b"\0" * 300)
w("tail_ff.bin", eng[:5000] + b"\xff" * 300)
