#!/bin/sh
# Verify native ain against archives created by historical DOS AIN 2.2.
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
ARC="$ROOT/packed_corpus/ain22"
VOL="$ARC/volumes"
TMP="${TMPDIR:-/tmp}/ain22-verify-$$"
fail=0

if [ ! -x "$AIN" ]; then
    echo "error: ain binary not found at $AIN (run make first)" >&2
    exit 1
fi

if [ ! -d "$ARC" ]; then
    echo "AIN 2.2 interoperability: $ARC not found, skipping."
    echo "See tests/REPORT_DOS_INTEROP.md for documented verification results."
    exit 0
fi

mkdir -p "$TMP"

pass() { printf "PASS  %s\n" "$1"; }
failmsg() { printf "FAIL  %s\n" "$1"; fail=1; }

test_crc() {
    arch=$1
    name=$(basename "$arch")
    if "$AIN" t "$arch" >/dev/null; then
        pass "t  $name"
    else
        failmsg "t  $name"
    fi
}

extract_cmp() {
    arch=$1
    orig=$2
    want=$3
    name=$(basename "$arch")
    dest="$TMP/$name"
    mkdir -p "$dest"
    if ! "$AIN" x "$arch" -o "$dest/" >/dev/null; then
        failmsg "x  $name (extract failed)"
        return
    fi
    got=$(find "$dest" -type f -iname "$want" | head -n 1)
    if [ -z "$got" ]; then
        failmsg "x  $name (missing $want)"
        return
    fi
    if cmp -s "$orig" "$got"; then
        pass "x  $name -> $want"
    else
        failmsg "x  $name byte mismatch vs $orig"
    fi
}

extract_tree_cmp() {
    arch=$1
    srcdir=$2
    name=$(basename "$arch")
    dest="$TMP/$name"
    mkdir -p "$dest"
    if ! "$AIN" x "$arch" -o "$dest/" >/dev/null; then
        failmsg "x  $name (extract failed)"
        return
    fi
    ok=1
    for got in $(find "$dest" -type f | sort); do
        base=$(basename "$got")
        orig=$(find "$srcdir" -type f \( -iname "$base" \) | head -n 1)
        if [ -z "$orig" ] || ! cmp -s "$orig" "$got"; then
            ok=0
            break
        fi
    done
    if [ "$ok" -eq 1 ]; then
        pass "x  $name tree"
    else
        failmsg "x  $name tree"
    fi
}

echo "AIN 2.2 interoperability (DOSBox-created archives)"
echo "binary: $AIN"
echo

echo "== CRC test (single-volume) =="
for a in "$ARC"/*.AIN; do
    case "$(basename "$a")" in
        VOL*) continue ;;
    esac
    test_crc "$a"
done

echo
echo "== Extract bit-identity (single-file archives) =="
extract_cmp "$ARC/HELLO1.AIN" "$CORPUS/hello.txt" "HELLO.TXT"
extract_cmp "$ARC/HELLO2.AIN" "$CORPUS/hello.txt" "HELLO.TXT"
extract_cmp "$ARC/HELLO3.AIN" "$CORPUS/hello.txt" "HELLO.TXT"
extract_cmp "$ARC/HELLO4.AIN" "$CORPUS/hello.txt" "HELLO.TXT"
extract_cmp "$ARC/ENG1.AIN"   "$CORPUS/eng.txt"   "ENG.TXT"
extract_cmp "$ARC/ENG2.AIN"   "$CORPUS/eng.txt"   "ENG.TXT"
extract_cmp "$ARC/ENG3.AIN"   "$CORPUS/eng.txt"   "ENG.TXT"
extract_cmp "$ARC/ENG4.AIN"   "$CORPUS/eng.txt"   "ENG.TXT"
extract_cmp "$ARC/EMPTY1.AIN" "$CORPUS/empty.dat" "EMPTY.DAT"
extract_cmp "$ARC/EMPTY3.AIN" "$CORPUS/empty.dat" "EMPTY.DAT"
extract_cmp "$ARC/EMPTY4.AIN" "$CORPUS/empty.dat" "EMPTY.DAT"
extract_cmp "$ARC/ZEROS3.AIN" "$CORPUS/zeros.bin" "ZEROS.BIN"
extract_cmp "$ARC/ZEROS4.AIN" "$CORPUS/zeros.bin" "ZEROS.BIN"
extract_cmp "$ARC/MIXED1.AIN" "$CORPUS/mixed.bin" "MIXED.BIN"
extract_cmp "$ARC/MIXED2.AIN" "$CORPUS/mixed.bin" "MIXED.BIN"
extract_cmp "$ARC/MIXED3.AIN" "$CORPUS/mixed.bin" "MIXED.BIN"
extract_cmp "$ARC/IMAGE1.AIN" "$CORPUS/image.raw" "IMAGE.RAW"
extract_cmp "$ARC/IMAGE3.AIN" "$CORPUS/image.raw" "IMAGE.RAW"
extract_cmp "$ARC/CODE1.AIN"  "$CORPUS/code.c"    "CODE.C"

echo
echo "== Extract bit-identity (multi-file / tree) =="
extract_tree_cmp "$ARC/MULTI.AIN"  "$CORPUS"
extract_tree_cmp "$ARC/NESTED.AIN" "$CORPUS"

echo
echo "== CRC test (AIN 2.2 /F fragments, per-volume archives) =="
for a in "$VOL"/VOL20.AIN "$VOL"/VOL20.001 "$VOL"/VOL20.002; do
    test_crc "$a"
done

echo
if [ "$fail" -ne 0 ]; then
    echo "AIN 2.2 verification FAILED"
    exit 1
fi
echo "AIN 2.2 verification OK"
exit 0
