#!/usr/bin/env python3
"""Deterministic A/B: instruction and cache-miss counts under cachegrind.

Usage: icount.py REF_BIN CAND_BIN [--methods 1,2,3] [--mb 1.5] [--extra FILE]...

Wall-clock timing on a shared/loaded machine has a +-5..10% noise floor;
this tool is noise-free.  For each payload and method it runs
`valgrind --tool=cachegrind --cache-sim=yes` and reports
  Ir     instructions executed
  D1m    L1 data misses (read+write)
  LLm    last-level misses (data)
  estC   rough cycle estimate = Ir + 12*D1m + 150*LLm
Use it to accept/reject micro-optimisations, then confirm with timing.py.
Payloads are the same generators as timing.py (mk_text, mk_code,
mut_corpus, corpus), truncated to --mb MB.  Needs valgrind in PATH or
$VALGRIND.
"""
import os, re, subprocess, sys, tempfile, shutil, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
a = sys.argv[1:]
def opt(name, default):
    if name in a:
        i = a.index(name); v = a[i + 1]; del a[i:i + 2]; return v
    return default
methods = [int(x) for x in opt("--methods", "1,2,3").split(",")]
mb = opt("--mb", "1.5")
extra = []
while "--extra" in a:
    i = a.index("--extra"); extra.append(os.path.abspath(a[i + 1])); del a[i:i + 2]
if len(a) != 2:
    print(__doc__); sys.exit(2)
REF, CAND = map(os.path.abspath, a)
VG = os.environ.get("VALGRIND") or shutil.which("valgrind")
if not VG:
    sys.exit("valgrind not found (set $VALGRIND or install it, e.g. `nix build nixpkgs#valgrind`)")

tmp = tempfile.mkdtemp(prefix="ain-icount-")
# Reuse timing.py's payload generators (import as a module without running it).
src = open(os.path.join(HERE, "timing.py")).read()
gen_src = src[src.index("UNIT = ("):src.index("QUICK = [")]
g = {"os": os, "random": __import__("random"), "CORPUS": os.path.join(os.path.dirname(os.path.dirname(HERE)), "corpus"),
     "tempfile": tempfile, "target": int(float(mb) * 1e6)}
gen_src = gen_src.replace('tmp = tempfile.mkdtemp(prefix="ain-timing-")\n', "").replace("target = int(mb * 1e6)\n", "")
exec(gen_src, g)
payloads = {}
for name in ("mk_text", "mk_code", "mut_corpus", "corpus"):
    p = os.path.join(tmp, name + ".bin")
    open(p, "wb").write(g["gens"][name]())
    payloads[name] = p
for e in extra:
    payloads[os.path.basename(e)[:10]] = e

def cg(binp, m, srcp):
    out = os.path.join(tmp, "o.ain"); cgo = os.path.join(tmp, "cg.out")
    for f in (out, cgo):
        if os.path.exists(f): os.remove(f)
    r = subprocess.run([VG, "--tool=cachegrind", "--cache-sim=yes", f"--cachegrind-out-file={cgo}",
                        binp, "a", f"-m{m}", out, srcp],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    txt = r.stderr
    def num(label):
        mm = re.search(label + r"\s*:\s*([\d,]+)", txt)
        return int(mm.group(1).replace(",", "")) if mm else 0
    Ir = num(r"I\s+refs"); D1 = num(r"D1\s+misses"); LL = num(r"LLd\s+misses")
    return Ir, D1, LL, Ir + 12 * D1 + 150 * LL

print(f"{'payload':11} {'M':>2} {'Ir ref':>13} {'Ir cand':>13} {'Ir x':>6} {'D1m x':>6} {'LLm x':>6} {'estC x':>7}")
tot = {}
for name, p in payloads.items():
    for m in methods:
        r = cg(REF, m, p); c = cg(CAND, m, p)
        rr = [r[i] / c[i] if c[i] else 0 for i in range(4)]
        t = tot.setdefault(m, [0, 0]); t[0] += r[3]; t[1] += c[3]
        print(f"{name:11} {m:>2} {r[0]:13,} {c[0]:13,} {rr[0]:6.3f} {rr[1]:6.3f} {rr[2]:6.3f} {rr[3]:7.3f}")
for m, (r, c) in tot.items():
    print(f"M{m}: estimated-cycles speedup {r / c:.3f}x")
shutil.rmtree(tmp, ignore_errors=True)
