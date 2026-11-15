#!/usr/bin/env python3
"""Encoder timing A/B bench: REF vs CAND, interleaved.

Usage: timing.py REF_BIN CAND_BIN [--reps N] [--mb SIZE] [--methods 1,2,3]
                 [--payloads all|quick|none|<name,...>] [--extra FILE]...

Payloads (~SIZE MB each, deterministic):
  rep_<file>  one corpus file replicated (tiny unique separators) — this is
              what tests/bench_encode.sh measures; very repetitive.
  corpus      the whole corpus concatenated and replicated.
  mk_text / mk_code  order-4 Markov text from eng.txt / code.c: realistic
              statistics without long exact repeats.
  mut_corpus  corpus copies with byte edits + small insert/deletes.
  many_small  ~300 files of 1..60 KB in one archive (per-file overheads).
  --extra F   any real file of yours (repeatable).

Every (payload, method) is run REPS times per
binary, alternating REF/CAND to cancel drift.  Time = wall time of the child
(single-threaded; rusage granularity is too coarse on some kernels).

Statistic (--stat): q25 (default: lower quartile, robust on a loaded
machine), median or min.  Verdict uses a bootstrap 95% CI of S(REF)/S(CAND):
  GAIN  if CI low  > 1 + THRESH   (real speedup)
  LOSS  if CI high < 1 - THRESH   (real slowdown)
  noise otherwise.
"""
import os, random, subprocess, sys, tempfile, shutil, statistics, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CORPUS = os.path.join(ROOT, "corpus")
THRESH = 0.01

a = sys.argv[1:]
def opt(name, default):
    if name in a:
        i = a.index(name); v = a[i + 1]; del a[i:i + 2]; return v
    return default
reps = int(opt("--reps", "9"))
stat = opt("--stat", "q25")
mb = float(opt("--mb", "4"))
methods = [int(x) for x in opt("--methods", "1,2,3").split(",")]
sel = opt("--payloads", "quick")
extra = []
while "--extra" in a:
    i = a.index("--extra"); extra.append(os.path.abspath(a[i + 1])); del a[i:i + 2]
if len(a) != 2:
    print(__doc__); sys.exit(2)
REF, CAND = map(os.path.abspath, a)

UNIT = ("eng.txt", "image.raw", "mixed.bin", "zeros.bin", "records.bin",
        "pattern.bin", "code.c", "random.bin", "biased.bin", "image24.raw")
tmp = tempfile.mkdtemp(prefix="ain-timing-")
target = int(mb * 1e6)
corp = {n.split(".")[0]: open(os.path.join(CORPUS, n), "rb").read() for n in UNIT}

def rep(blob):
    out = bytearray(); i = 0
    while len(out) < target:
        out += blob; out += bytes((i & 255, (i >> 8) & 255, 0xA5, 0x5A)); i += 1
    return bytes(out[:target])

def markov(text, order=4, seed=1):
    """Order-k character model: realistic statistics, no long exact repeats."""
    model = {}
    for i in range(len(text) - order):
        model.setdefault(text[i:i + order], []).append(text[i + order])
    r = random.Random(seed); keys = list(model)
    st = text[:order]; out = bytearray(st)
    while len(out) < target:
        nxt = model.get(bytes(st))
        if not nxt:
            st = r.choice(keys); out += st; continue
        c = nxt[r.randrange(len(nxt))]; out.append(c); st = bytes(out[-order:])
    return bytes(out[:target])

def mutated(blob, seed=2):
    """Corpus copies with ~0.5% byte edits and small insert/deletes per copy."""
    r = random.Random(seed); out = bytearray()
    while len(out) < target:
        b = bytearray(blob)
        for _ in range(len(b) // 200):
            b[r.randrange(len(b))] = r.randrange(256)
        for _ in range(20):
            k = r.randrange(len(b))
            if r.random() < 0.5: del b[k:k + r.randrange(1, 9)]
            else: b[k:k] = bytes(r.randrange(256) for _ in range(r.randrange(1, 9)))
        out += b
    return bytes(out[:target])

gens = {}
for n in corp:
    gens["rep_" + n] = (lambda n=n: rep(corp[n]))
allc = b"".join(corp.values())
gens["corpus"]   = lambda: rep(allc)
gens["mk_text"]  = lambda: markov(corp["eng"], 4, 1)
gens["mk_code"]  = lambda: markov(corp["code"], 4, 3)
gens["mut_corpus"] = lambda: mutated(allc)
def many_small():
    """~300 files of 1..60 KB cut from the realistic generators."""
    r = random.Random(4); out = []
    pool = [markov(corp["eng"], 4, 11), markov(corp["code"], 4, 12), mutated(allc, 13)]
    total = 0; k = 0
    while total < target:
        src = pool[k % 3]; n = r.randrange(1000, 60000); o = r.randrange(0, len(src) - n)
        out.append(src[o:o + n]); total += n; k += 1
    return out
gens["many_small"] = many_small
QUICK = ["mk_text", "mk_code", "mut_corpus", "corpus", "rep_biased",
         "rep_image24", "rep_random", "rep_records"]
if sel == "all": chosen = list(gens)
elif sel == "quick": chosen = QUICK
elif sel == "none": chosen = []
else: chosen = sel.split(",")
payloads = {}
for name in chosen:
    data = gens[name]()
    if isinstance(data, list):                    # multi-file payload
        d = os.path.join(tmp, name); os.makedirs(d)
        files = []
        for i, blob in enumerate(data):
            fp = os.path.join(d, f"F{i:05d}.DAT"); open(fp, "wb").write(blob); files.append(fp)
        payloads[name] = files
    else:
        p = os.path.join(tmp, name + ".bin")
        with open(p, "wb") as f: f.write(data)
        payloads[name] = p
for e in extra:
    nm = os.path.basename(e)[:10]; payloads[nm] = e; chosen.append(nm)

def run(binp, m, src):
    out = os.path.join(tmp, "o.ain")
    try: os.remove(out)
    except FileNotFoundError: pass
    t0 = time.perf_counter()
    pid = os.fork()
    if pid == 0:
        fd = os.open(os.devnull, os.O_WRONLY); os.dup2(fd, 1); os.dup2(fd, 2)
        os.execv(binp, [binp, "a", f"-m{m}", out] + (src if isinstance(src, list) else [src]))
    _, st, ru = os.wait4(pid, 0)
    t1 = time.perf_counter()
    if st != 0: raise SystemExit(f"encode failed: {binp} M{m} {src}")
    return t1 - t0  # wall time of a single-threaded child: rusage ticks are 10 ms here

def S(v):
    v = sorted(v)
    if stat == "min": return v[0]
    if stat == "median": return statistics.median(v)
    return v[(len(v) - 1) // 4]          # lower quartile: robust to load spikes

def ci(r, c, n=2000):
    rnd = random.Random(1)
    vals = []
    for _ in range(n):
        rr = [rnd.choice(r) for _ in r]; cc = [rnd.choice(c) for _ in c]
        vals.append(S(rr) / S(cc))
    vals.sort()
    return vals[int(0.025 * n)], vals[int(0.975 * n)]

print(f"payload ~{mb} MB, reps={reps}, stat={stat}, REF={REF}\n                          CAND={CAND}")
print(f"{'payload':11} {'M':>2} {'ref ms':>9} {'cand ms':>9} {'speedup':>8} {'95% CI':>15}  verdict")
tot_r = {m: 0.0 for m in methods}; tot_c = {m: 0.0 for m in methods}
for name in chosen:
    src = payloads[name]
    for m in methods:
        run(REF, m, src); run(CAND, m, src)          # warm-up (page cache)
        r, c = [], []
        for k in range(reps):
            if k % 2: c.append(run(CAND, m, src)); r.append(run(REF, m, src))
            else:     r.append(run(REF, m, src)); c.append(run(CAND, m, src))
        mr, mc = S(r), S(c)
        lo, hi = ci(r, c)
        v = "GAIN" if lo > 1 + THRESH else ("LOSS" if hi < 1 - THRESH else "noise")
        tot_r[m] += mr; tot_c[m] += mc
        print(f"{name:11} {m:>2} {mr*1e3:9.1f} {mc*1e3:9.1f} {mr/mc:7.3f}x  [{lo:.3f},{hi:.3f}]  {v}")
print(f"--- totals (sum of {stat} over payloads) ---")
for m in methods:
    print(f"M{m}: ref {tot_r[m]*1e3:8.1f} ms  cand {tot_c[m]*1e3:8.1f} ms  speedup {tot_r[m]/tot_c[m]:.3f}x")
shutil.rmtree(tmp, ignore_errors=True)
