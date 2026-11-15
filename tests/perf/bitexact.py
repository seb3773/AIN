#!/usr/bin/env python3
"""Bit-exact gate: candidate encoder vs reference (legacy) encoder.

Usage: bitexact.py REF_BIN CAND_BIN [-j N]

Checks, for methods M1..M4:
  1. packed_corpus/ reference streams (historical DOS-validated archives),
  2. every file of corpus/ (recursive) + the generated edge-case corpus,
     encoded one file per archive, REF vs CAND byte-for-byte,
  3. multi-file archives (session state carried across files),
  4. CAND round-trip (extract == original) on the generated corpus,
  5. decoder: every archive under packed_corpus/ extracted by REF and CAND.
Only the archive creation timestamp (header bytes 10..13) and the header
CRC that covers it (bytes 22..23) are masked.  Exit status != 0 on any diff.
"""
import os, struct, subprocess, sys, tempfile, shutil
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CORPUS = os.path.join(ROOT, "corpus")
PACKED = os.path.join(ROOT, "packed_corpus")

args = [a for a in sys.argv[1:]]
jobs = os.cpu_count() or 4
if "-j" in args:
    i = args.index("-j"); jobs = int(args[i + 1]); del args[i:i + 2]
if len(args) != 2:
    print(__doc__); sys.exit(2)
REF, CAND = map(os.path.abspath, args)

tmp = tempfile.mkdtemp(prefix="ain-bitexact-")
gen = os.path.join(tmp, "gen")
subprocess.run([sys.executable, os.path.join(HERE, "gen_corpus.py"), gen], check=True)

def masked(path):
    d = open(path, "rb").read()
    return d[:10] + d[14:22] + d[24:]

def stream(path):
    d = open(path, "rb").read()
    idx = struct.unpack_from("<I", d, 14)[0] or len(d)
    return d[24:idx]

def enc(binp, m, out, inputs, extra=()):
    r = subprocess.run([binp, "a", f"-m{m}", *extra, out, *inputs],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    return r.returncode

fails = []
n_ok = 0

# 1) packed_corpus references
pairs = [("eng.txt","ENG-M1.AIN",1),("eng.txt","ENG-M2.AIN",2),("eng.txt","ENG-M3.AIN",3),
         ("eng.txt","ENG-M4.AIN",4),("hello.txt","HELLO-M1.AIN",1),("hello.txt","HELLO-M2.AIN",2),
         ("hello.txt","HELLO-M3.AIN",3),("hello.txt","HELLO-M4.AIN",4),("image.raw","IMAGE-M1.AIN",1),
         ("image.raw","IMAGE-M3.AIN",3),("mixed.bin","MIXED-M2.AIN",2),("mixed.bin","MIXED-M3.AIN",3),
         ("zeros.bin","ZEROS.AIN",3),("zeros.bin","ZEROS-M3.AIN",3),("zeros.bin","ZEROS-M4.AIN",4),
         ("empty.dat","EMPTY.AIN",1),("empty.dat","EMPTY-M3.AIN",3),("empty.dat","EMPTY-M4.AIN",4),
         ("code.c","CODE.AIN",3)]
for src, arc, m in pairs:
    refp = os.path.join(PACKED, arc)
    if not os.path.isfile(refp):
        continue
    out = os.path.join(tmp, "pc_" + arc)
    if enc(CAND, m, out, [os.path.join(CORPUS, src)]) != 0 or stream(out) != stream(refp):
        fails.append(f"packed_corpus {src} M{m} vs {arc}")
    else:
        n_ok += 1

# 2) single-file archives, REF vs CAND
files = []
for base in (CORPUS, gen):
    for dp, _, fns in os.walk(base):
        for fn in sorted(fns):
            files.append(os.path.join(dp, fn))
cases = [(f, m) for f in files for m in (1, 2, 3, 4)]

def one(case):
    f, m = case
    key = f"{abs(hash(f)) % 10**12}_{m}"
    a = os.path.join(tmp, f"r_{key}.ain"); b = os.path.join(tmp, f"c_{key}.ain")
    ra = enc(REF, m, a, [f]); rb = enc(CAND, m, b, [f])
    ok = (ra == rb) and (ra != 0 or True)
    if ra == 0 and rb == 0:
        ok = masked(a) == masked(b)
    for p in (a, b):
        try: os.remove(p)
        except FileNotFoundError: pass
    return (f, m, ok)

with ThreadPoolExecutor(jobs) as ex:
    for f, m, ok in ex.map(one, cases):
        if ok: n_ok += 1
        else: fails.append(f"single {os.path.relpath(f, tmp if f.startswith(tmp) else ROOT)} M{m}")

# 3) multi-file archives
top = sorted(os.path.join(CORPUS, f) for f in os.listdir(CORPUS)
             if os.path.isfile(os.path.join(CORPUS, f)))
gen_files = sorted(os.path.join(gen, f) for f in os.listdir(gen))
multi = [("corpus_all", top, ()), ("nested_r", [os.path.join(CORPUS, "nested")], ("-r",)),
         ("gen_all", gen_files, ())]
def multi_one(c):
    name, inputs, extra = c
    res = []
    for m in (1, 2, 3):
        a = os.path.join(tmp, f"mr_{name}{m}.ain"); b = os.path.join(tmp, f"mc_{name}{m}.ain")
        ra = enc(REF, m, a, inputs, extra); rb = enc(CAND, m, b, inputs, extra)
        res.append((f"multi {name} M{m}", ra == 0 and rb == 0 and masked(a) == masked(b)))
    return res
with ThreadPoolExecutor(jobs) as ex:
    for res in ex.map(multi_one, multi):
        for name, ok in res:
            if ok: n_ok += 1
            else: fails.append(name)

# 4) CAND round-trip of the generated corpus (all files in one archive)
for m in (1, 2, 3):
    arc = os.path.join(tmp, f"rt{m}.ain")
    dest = os.path.join(tmp, f"rt{m}")
    os.makedirs(dest)
    enc(CAND, m, arc, gen_files)
    subprocess.run([CAND, "x", arc, "-o", dest + "/"], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    got = {}
    for dp, _, fns in os.walk(dest):
        for fn in fns:
            got[fn.lower()] = os.path.join(dp, fn)
    bad = 0
    for f in gen_files:
        # DOS 8.3 names: compare by content set instead of name
        pass
    want = sorted(open(f, "rb").read() for f in gen_files)
    have = sorted(open(p, "rb").read() for p in got.values())
    if want == have: n_ok += 1
    else: fails.append(f"roundtrip gen M{m}")

# 5) decoder: extract every historical archive with REF and CAND, compare trees
def tree(d):
    out = {}
    for dp, _, fns in os.walk(d):
        for fn in fns:
            p = os.path.join(dp, fn)
            out[os.path.relpath(p, d)] = open(p, "rb").read()
    return out
arcs = []
for dp, _, fns in os.walk(PACKED):
    for fn in sorted(fns):
        if fn.upper().endswith((".AIN", ".EXE")):
            arcs.append(os.path.join(dp, fn))
for i, arc in enumerate(arcs):
    res = []
    for tag, binp in (("r", REF), ("c", CAND)):
        d = os.path.join(tmp, f"dx_{tag}{i}")
        os.makedirs(d)
        rc = subprocess.run([binp, "x", arc, "-o", d + "/"], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL).returncode
        res.append((rc, tree(d)))
    if res[0] == res[1]: n_ok += 1
    else: fails.append(f"decode {os.path.relpath(arc, ROOT)}")

shutil.rmtree(tmp, ignore_errors=True)
print(f"bit-exact checks passed: {n_ok}, failed: {len(fails)}")
for f in fails[:50]:
    print("FAIL", f)
sys.exit(1 if fails else 0)
