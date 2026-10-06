#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

MODE="release"
MAKE_ARGS=()

for arg in "$@"; do
    case "$arg" in
        debug|-DEBUG|--debug|DEBUG=1)
            MODE="debug"
            MAKE_ARGS+=(debug)
            ;;
        clean)
            make clean
            exit 0
            ;;
        test)
            make test
            exit 0
            ;;
        deb)
            make deb
            exit 0
            ;;
        *)
            MAKE_ARGS+=("$arg")
            ;;
    esac
done

echo "=========================================================="
if [ "$MODE" = "debug" ]; then
    echo "  Building AIN Archiver 2.32 (DEBUG mode)"
else
    echo "  Building AIN Archiver 2.32 (PRODUCTION - Release)"
fi
echo "=========================================================="

if [ "$MODE" = "debug" ]; then
    make debug
else
    make clean >/dev/null 2>&1 || true
    make -j"$(nproc)" "${MAKE_ARGS[@]}"
fi

echo ""
echo "=== Summary of Generated Binary ==="
ls -lh "$SCRIPT_DIR/build/ain"
echo "Done."
