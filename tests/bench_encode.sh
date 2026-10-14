#!/bin/sh
# Bit-exact encode vs packed_corpus streams, plus a long encode timing bench.
#
# Timing builds a synthetic payload from corpus/ (copies + tiny separators)
# large enough that M1 takes at least 2.5s, so CPU noise does not dominate.
# Timing is informational (does not fail the suite). If
# tests/bench_encode.baseline exists, a speedup vs that snapshot is printed.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if [ -z "${AIN:-}" ]; then
    if [ -x "$ROOT/build/linux/ain" ]; then
        AIN="$ROOT/build/linux/ain"
    else
        AIN="$ROOT/build/ain"
    fi
fi
CORPUS="$ROOT/corpus"
PACKED="$ROOT/packed_corpus"
BASELINE="$ROOT/tests/bench_encode.baseline"
TMP="${TMPDIR:-/tmp}/ain-bench-$$"

if [ ! -x "$AIN" ]; then
    echo "error: ain binary not found at $AIN (run make first)" >&2
    exit 1
fi

mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

python3 - "$AIN" "$CORPUS" "$PACKED" "$BASELINE" "$TMP" << 'PY'
import os, struct, subprocess, sys, time

ain, corpus, packed, baseline_path, tmp = sys.argv[1:6]
fail = 0

def stream(path):
    d = open(path, "rb").read()
    idx = struct.unpack_from("<I", d, 14)[0]
    if idx == 0:
        idx = len(d)
    return d[24:idx]

pairs = [
    ("eng.txt",     "ENG-M1.AIN",     1),
    ("eng.txt",     "ENG-M2.AIN",     2),
    ("eng.txt",     "ENG-M3.AIN",     3),
    ("eng.txt",     "ENG-M4.AIN",     4),
    ("hello.txt",   "HELLO-M1.AIN",   1),
    ("hello.txt",   "HELLO-M2.AIN",   2),
    ("hello.txt",   "HELLO-M3.AIN",   3),
    ("hello.txt",   "HELLO-M4.AIN",   4),
    ("image.raw",   "IMAGE-M1.AIN",   1),
    ("image.raw",   "IMAGE-M3.AIN",   3),
    ("mixed.bin",   "MIXED-M2.AIN",   2),
    ("mixed.bin",   "MIXED-M3.AIN",   3),
    ("zeros.bin",   "ZEROS.AIN",      3),
    ("zeros.bin",   "ZEROS-M3.AIN",   3),
    ("zeros.bin",   "ZEROS-M4.AIN",   4),
    ("empty.dat",   "EMPTY.AIN",      1),
    ("empty.dat",   "EMPTY-M3.AIN",   3),
    ("empty.dat",   "EMPTY-M4.AIN",   4),
    ("code.c",      "CODE.AIN",       3),
]

print("=== Bit-exact encode vs packed_corpus (compressed stream) ===")
for src, arc, m in pairs:
    srcp = os.path.join(corpus, src)
    refp = os.path.join(packed, arc)
    if not os.path.isfile(refp):
        print(f"SKIP  {src} M{m} vs {arc} (missing reference)")
        continue
    dst = os.path.join(tmp, arc)
    r = subprocess.run(
        [ain, "a", f"-m{m}", dst, srcp],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        print(f"FAIL  encode {src} M{m}: {r.stderr.decode(errors='replace')[:200]}")
        fail += 1
        continue
    got = stream(dst)
    ref = stream(refp)
    if got == ref:
        print(f"PASS  {src:12} M{m} vs {arc:14}  {len(got)} bytes")
    else:
        print(f"FAIL  {src:12} M{m} vs {arc:14}  got {len(got)} ref {len(ref)}")
        fail += 1

print("=== Encode round-trip (extract == original) ===")
bundle = [
    os.path.join(corpus, f)
    for f in ("eng.txt", "image.raw", "mixed.bin", "zeros.bin", "hello.txt", "empty.dat")
]
arc = os.path.join(tmp, "roundtrip.ain")
r = subprocess.run([ain, "a", "-m1", arc] + bundle,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
if r.returncode != 0:
    print("FAIL  roundtrip encode")
    fail += 1
else:
    dest = os.path.join(tmp, "x")
    os.makedirs(dest, exist_ok=True)
    r = subprocess.run([ain, "x", arc, "-o", dest + "/"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0:
        print("FAIL  roundtrip extract")
        fail += 1
    else:
        rt_ok = True
        for f in bundle:
            name = os.path.basename(f)
            got = None
            for dirpath, _, files in os.walk(dest):
                for fn in files:
                    if fn.lower() == name.lower():
                        got = os.path.join(dirpath, fn)
                        break
            if got is None:
                print(f"FAIL  roundtrip missing {name}")
                fail += 1
                rt_ok = False
                continue
            if open(got, "rb").read() != open(f, "rb").read():
                print(f"FAIL  roundtrip mismatch {name}")
                fail += 1
                rt_ok = False
        if rt_ok:
            print("PASS  roundtrip M1 6 files")

UNIT_NAMES = (
    "eng.txt", "image.raw", "mixed.bin", "zeros.bin",
    "records.bin", "pattern.bin", "code.c", "random.bin",
    "biased.bin", "image24.raw",
)
TARGET_M1_S = 2.5
MAX_COPIES = 64
NREP = 3

blobs = []
for name in UNIT_NAMES:
    p = os.path.join(corpus, name)
    blobs.append(open(p, "rb").read())
unit = b"".join(blobs)
payload_path = os.path.join(tmp, "bench.bin")


def write_payload(copies):
    with open(payload_path, "wb") as out:
        for i in range(copies):
            out.write(unit)
            out.write(bytes((
                i & 0xFF, (i >> 8) & 0xFF, 0xA5, 0x5A,
                0x3C, 0xC3, 0x96, 0x69,
            )))
    return os.path.getsize(payload_path)


def run_encode(method, outpath):
    t0 = time.perf_counter()
    subprocess.run(
        [ain, "a", f"-m{method}", outpath, payload_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return time.perf_counter() - t0


print("=== Calibrate payload (target slowest mode M1 >= %.1fs) ===" % TARGET_M1_S)
copies = 8
cal_out = os.path.join(tmp, "cal.ain")
m1_cal = 0.0
size = 0
while copies <= MAX_COPIES:
    size = write_payload(copies)
    try:
        os.remove(cal_out)
    except FileNotFoundError:
        pass
    m1_cal = run_encode(1, cal_out)
    print("  copies=%d  size=%.2f MB  M1=%.3fs" % (copies, size / 1e6, m1_cal))
    if m1_cal >= TARGET_M1_S:
        break
    copies *= 2
else:
    print("  warning: M1 still below target at copies=%d" % MAX_COPIES)

print("=== Encode timing (payload %.2f MB, %d copies) ===" % (size / 1e6, copies))
times = {}
for method in (1, 2, 3, 4):
    out = os.path.join(tmp, "bench-m%d.ain" % method)
    run_encode(method, out)
    samples = []
    for _ in range(NREP):
        try:
            os.remove(out)
        except FileNotFoundError:
            pass
        samples.append(run_encode(method, out))
    samples.sort()
    med = samples[len(samples) // 2]
    times[method] = med
    spread = samples[-1] - samples[0]
    print("M%d: median %7.3fs   min %7.3f  max %7.3f  spread %6.3fs  n=%d" % (
        method, med, samples[0], samples[-1], spread, len(samples)))
    if method == 1 and med < 2.0:
        print("  note: M1 median < 2s (noisy); payload may need more copies on this CPU")

if os.environ.get("BENCH_WRITE_BASELINE"):
    with open(baseline_path, "w") as f:
        f.write("# Median encode times (seconds) for a given synthetic payload.\n")
        f.write("# SIZE is the payload byte count; compare runs via throughput\n")
        f.write("# (bytes/second) since the payload auto-scales per CPU speed.\n")
        f.write("SIZE %d\n" % size)
        for m in (1, 2, 3, 4):
            f.write("M%d %.4f\n" % (m, times[m]))
    print("=== wrote baseline -> %s (SIZE %d) ===" % (baseline_path, size))

if os.path.isfile(baseline_path):
    print("=== vs tests/bench_encode.baseline (throughput-normalized) ===")
    base = {}
    base_size = 0
    for line in open(baseline_path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        k, v = parts[0], parts[1]
        if k == "SIZE":
            base_size = int(v)
        elif k[0] == "M" and k[1:].isdigit():
            base[int(k[1:])] = float(v)
    for m in (1, 2, 3, 4):
        if m not in base or times[m] <= 0:
            continue
        b = base[m]
        if base_size > 0 and size > 0:
            base_bps = base_size / b
            cur_bps = size / times[m]
            speedup = cur_bps / base_bps if base_bps > 0 else 0.0
            print("M%d: %.4fs/%.1fMB -> %.4fs/%.1fMB   %.2fx (throughput %.1f -> %.1f MB/s)"
                  % (m, b, base_size / 1e6, times[m], size / 1e6, speedup,
                     base_bps / 1e6, cur_bps / 1e6))
        else:
            speedup = b / times[m]
            print("M%d: %.3fs -> %.3fs   %.2fx (legacy raw; payload sizes may differ)"
                  % (m, b, times[m], speedup))

sys.exit(1 if fail else 0)
PY
