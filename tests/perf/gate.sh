#!/bin/sh
# Full acceptance gate for encoder optimisations.
#
#   tests/perf/gate.sh [REF_REV] [--timing]
#
# REF_REV (default: $AIN_REF_REV or 3ab90bc) is the git revision of the
# legacy/reference encoder.  The script builds, in a temp dir:
#   ref        legacy src/ain.c at REF_REV       (same CFLAGS as candidate)
#   cand       working-tree src/ain.c            (production build)
#   cand_f     cand with the M1/M2 accelerator forced on every file size
#   cand_s     cand with the scalar (non-SSE2) code paths
#   verify     cand_f + AIN_VERIFY_FASTPATH: every match search is run by the
#              legacy walk AND the accelerated one, abort on any difference
#   verify_t   verify with a 2-way/4-set index (forces the fallback paths)
# then runs bitexact.py for cand, cand_f, cand_s and the verify builds on
# large non-repetitive payloads.  --timing additionally runs timing.py.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
REV=${AIN_REF_REV:-3ab90bc}
TIMING=0
for a in "$@"; do
    case "$a" in
        --timing) TIMING=1 ;;
        *) REV=$a ;;
    esac
done
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O3 -flto -fomit-frame-pointer -Wall -Wextra}
T=$(mktemp -d "${TMPDIR:-/tmp}/ain-gate-XXXXXX")
trap 'rm -rf "$T"' EXIT

CAND_CFLAGS="$CFLAGS"

echo "== build (ref = $REV)"
mkdir -p "$T/ref/src"
for f in ain.c ain_sfx_stub.h ain_sfx_linux_stub.h ain_sfx_win_stub.h; do
    git -C "$ROOT" show "$REV:src/$f" > "$T/ref/src/$f"
done
$CC $CFLAGS -I"$T/ref/src" -o "$T/ref.bin" "$T/ref/src/ain.c"
$CC $CAND_CFLAGS -I"$ROOT/src" -o "$T/cand"   "$ROOT/src/ain.c"
$CC $CAND_CFLAGS -I"$ROOT/src" -DM12F_MIN_SIZE=0 -o "$T/cand_f" "$ROOT/src/ain.c"
$CC $CAND_CFLAGS -I"$ROOT/src" -U__SSE2__ -o "$T/cand_s" "$ROOT/src/ain.c"
$CC -O2 -I"$ROOT/src" $CAND_CFLAGS -DAIN_VERIFY_FASTPATH -DM12F_MIN_SIZE=0 -o "$T/verify" "$ROOT/src/ain.c"
$CC -O2 -I"$ROOT/src" $CAND_CFLAGS -DAIN_VERIFY_FASTPATH -DM12F_MIN_SIZE=0 \
    -DM12F_SETS_LOG=2 -DM12F_WAYS=2 -o "$T/verify_t" "$ROOT/src/ain.c"

echo "== bit-exact: cand";   python3 "$ROOT/tests/perf/bitexact.py" "$T/ref.bin" "$T/cand"
echo "== bit-exact: cand_f"; python3 "$ROOT/tests/perf/bitexact.py" "$T/ref.bin" "$T/cand_f"
echo "== bit-exact: cand_s"; python3 "$ROOT/tests/perf/bitexact.py" "$T/ref.bin" "$T/cand_s"
echo "== bit-exact: verify (per-search self-check on the whole corpus)"
python3 "$ROOT/tests/perf/bitexact.py" "$T/ref.bin" "$T/verify"

echo "== per-search self-check on large payloads"
python3 - "$T" "$ROOT" << 'PY'
import os, subprocess, sys
T, ROOT = sys.argv[1:3]
src = open(os.path.join(ROOT, "tests/perf/timing.py")).read()
code = src[src.index("UNIT = ("):src.index("QUICK = [")]
code = code.replace('tmp = tempfile.mkdtemp(prefix="ain-timing-")\n', "").replace("target = int(mb * 1e6)\n", "")
import random
g = {"os": os, "random": random, "CORPUS": os.path.join(ROOT, "corpus"), "target": 2_500_000}
exec(code, g)
fail = 0
for name in ("mk_text", "mk_code", "mut_corpus", "corpus", "rep_biased", "rep_records", "rep_random"):
    p = os.path.join(T, name + ".bin"); open(p, "wb").write(g["gens"][name]())
    for vb in ("verify", "verify_t"):
        for m in (1, 2):
            out = os.path.join(T, "v.ain")
            if os.path.exists(out): os.remove(out)
            r = subprocess.run([os.path.join(T, vb), "a", f"-m{m}", out, p],
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            ok = r.returncode == 0
            fail += not ok
            print(f"{'PASS' if ok else 'FAIL'}  {vb:8} M{m} {name}" + ("" if ok else "  " + r.stderr[:200]))
sys.exit(1 if fail else 0)
PY

if [ "$TIMING" = 1 ]; then
    echo "== timing (ref vs cand)"
    python3 "$ROOT/tests/perf/timing.py" "$T/ref.bin" "$T/cand"
fi
echo "== gate PASSED"
