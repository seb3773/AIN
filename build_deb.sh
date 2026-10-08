#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_DIR="${SCRIPT_DIR}/build/pkg_root"
VERSION="2.32-1"
ARCH="amd64"
DEB_NAME="ain_${VERSION}_${ARCH}.deb"

echo "=== Building AIN Archiver 2.32 from source ==="
cd "${SCRIPT_DIR}"
make clean
make -j"$(nproc)" all

echo "=== Running test suite before packaging ==="
make test

echo "=== Packaging DEB ==="
rm -rf "${PKG_DIR}"
mkdir -p "${PKG_DIR}/usr/bin"
mkdir -p "${PKG_DIR}/usr/share/doc/ain"
mkdir -p "${PKG_DIR}/DEBIAN"

# Install binary
if [ -f "${SCRIPT_DIR}/build/linux/ain" ]; then
    cp -f "${SCRIPT_DIR}/build/linux/ain" "${PKG_DIR}/usr/bin/ain"
else
    cp -L -f "${SCRIPT_DIR}/build/ain" "${PKG_DIR}/usr/bin/ain"
fi
chmod 755 "${PKG_DIR}/usr/bin/ain"

# Documentation
if [ -f "${SCRIPT_DIR}/README.md" ]; then
    cp "${SCRIPT_DIR}/README.md" "${PKG_DIR}/usr/share/doc/ain/"
fi
if [ -f "${SCRIPT_DIR}/FORMAT.md" ]; then
    cp "${SCRIPT_DIR}/FORMAT.md" "${PKG_DIR}/usr/share/doc/ain/"
fi

# DEBIAN/control
cat << 'CTRL_EOF' > "${PKG_DIR}/DEBIAN/control"
Package: ain
Version: 2.32-1
Section: utils
Priority: optional
Architecture: amd64
Maintainer: seb3773 <https://github.com/seb3773/AIN>
Depends: libc6 (>= 2.15)
Provides: ain
Description: Pure C reimplementation of historical Transas AIN 2.32 archiver
 Complete, standalone freestanding C tool combining both compressor and
 decompressor into a single binary (ain), achieving 100% bidirectional
 bit-exact compatibility with the original 16-bit DOS archiver (AIN.EXE).
 Features:
  - Compression modes M1 (Ultra), M2 (Normal), M3 (Fast), M4 (Store)
  - Multi-volume fragments (.ain, .a01, .a02...)
  - Triple SFX generation: Windows native PE 64-bit (.exe), Linux native ELF 64-bit (.sfx) & DOS 16-bit (.exe)
  - Selective extraction with wildcard pattern filtering
  - Path traversal & Zip-Slip protection
  - Historical digital preservation & software interoperability (EU 2009/24/EC)
CTRL_EOF

# Build DEB
dpkg-deb --build --root-owner-group "${PKG_DIR}" "${SCRIPT_DIR}/${DEB_NAME}"
echo "Package created: ${SCRIPT_DIR}/${DEB_NAME}"

# Clean temp packaging dir
rm -rf "${PKG_DIR}"

echo ""
echo "=== Package Information ==="
dpkg-deb -I "${SCRIPT_DIR}/${DEB_NAME}"
echo ""
echo "=== Package Contents ==="
dpkg-deb -c "${SCRIPT_DIR}/${DEB_NAME}"

echo ""
echo "=== AIN packaging finished successfully ==="
