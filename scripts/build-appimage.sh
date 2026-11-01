#!/usr/bin/env bash
# Build amlogic-multistream and package it as an AppImage.
#
# Host: normal Linux. The AppImage matches this machine: x86_64 for a host
# smoke build, aarch64 for an Amlogic S922X device build. CI runs the script
# once on each runner. No GS-King X or CoreELEC image is required to package.
#
# linuxdeploy fills the AppDir (binary, desktop file, icon, shared libraries).
# linuxdeploy-plugin-appimage then runs appimagetool. A copy of appimagetool
# is also placed on PATH because the plugin looks there if its bundled copy
# is not found.
#
# scripts/launch-coreelec.sh requires GS-King X / CoreELEC and is not used here.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
APPDIR="${APPDIR:-$DIST_DIR/AppDir}"
TOOLS_DIR="${APPIMAGE_TOOLS_DIR:-$ROOT/.cache/appimage-tools}"

LINUXDEPLOY_URL_BASE="${LINUXDEPLOY_URL_BASE:-https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous}"
PLUGIN_URL_BASE="${PLUGIN_URL_BASE:-https://github.com/linuxdeploy/linuxdeploy-plugin-appimage/releases/download/continuous}"
APPIMAGETOOL_URL_BASE="${APPIMAGETOOL_URL_BASE:-https://github.com/AppImage/appimagetool/releases/download/continuous}"

die() {
  echo "error: $*" >&2
  exit 1
}

need() {
  command -v "$1" >/dev/null 2>&1 || die "required command '$1' is not installed"
}

for cmd in cmake g++ pkg-config curl patchelf file strip timeout; do
  need "$cmd"
done

case "$(uname -m)" in
  x86_64) ARCH=x86_64 ;;
  aarch64 | arm64) ARCH=aarch64 ;;
  *) die "unsupported architecture: $(uname -m) (need x86_64 or aarch64)" ;;
esac
export ARCH
echo "Packaging for ${ARCH} ($(uname -m))"

VERSION="${VERSION:-}"
if [[ -z "$VERSION" ]]; then
  VERSION="$(sed -n 's/^project(.*VERSION[[:space:]]\{1,\}\([0-9][^[:space:]]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
fi
VERSION="${VERSION:-0.1.0}"

BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
CMAKE_ARGS=(
  -S "$ROOT"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  -DCMAKE_CXX_COMPILER=g++
)
if command -v ccache >/dev/null 2>&1; then
  CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

echo "Configuring player ($BUILD_TYPE, g++)"
cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --parallel

BIN="$BUILD_DIR/amlogic-multistream"
if [[ ! -x "$BIN" ]]; then
  die "player binary not built at $BIN. Install cmake, g++, pkg-config, libavformat-dev, libavcodec-dev, libavutil-dev, libswscale-dev, libmosquitto-dev, and libdrm-dev, then re-run."
fi

# --help exits 0 without a broker. The default path connects to MQTT and
# either blocks or exits 1 when nothing is listening on localhost:1883.
echo "Smoke-testing unpackaged player --help"
timeout --foreground 60 "$BIN" --help

mkdir -p "$TOOLS_DIR" "$DIST_DIR"
rm -rf "$APPDIR"
rm -f "$DIST_DIR"/*.AppImage

download() {
  local name="$1"
  local url="$2"
  local dest="$TOOLS_DIR/$name"
  if [[ -x "$dest" ]]; then
    echo "Using cached $dest"
    return 0
  fi
  echo "Downloading $url"
  curl -fsSL --retry 3 --retry-delay 2 -o "$dest.partial" "$url"
  chmod +x "$dest.partial"
  mv "$dest.partial" "$dest"
}

# AppImages are extracted instead of FUSE-mounted so CI and containers work.
export APPIMAGE_EXTRACT_AND_RUN=1

download "linuxdeploy-${ARCH}.AppImage" "${LINUXDEPLOY_URL_BASE}/linuxdeploy-${ARCH}.AppImage"
download "linuxdeploy-plugin-appimage-${ARCH}.AppImage" "${PLUGIN_URL_BASE}/linuxdeploy-plugin-appimage-${ARCH}.AppImage"
download "appimagetool-${ARCH}.AppImage" "${APPIMAGETOOL_URL_BASE}/appimagetool-${ARCH}.AppImage"
export PATH="$TOOLS_DIR:$PATH"

OUTPUT="$DIST_DIR/amlogic-multistream-${VERSION}-${ARCH}.AppImage"
export LDAI_OUTPUT="$OUTPUT"
export LDAI_NO_APPSTREAM=1
unset LDAI_SIGN || true

"$TOOLS_DIR/linuxdeploy-${ARCH}.AppImage" \
  --appdir "$APPDIR" \
  --executable "$BIN" \
  --desktop-file "$ROOT/packaging/amlogic-multistream.desktop" \
  --icon-file "$ROOT/packaging/amlogic-multistream.png" \
  --output appimage

[[ -f "$OUTPUT" ]] || die "linuxdeploy did not write $OUTPUT"
chmod +x "$OUTPUT"

echo "Smoke-testing $OUTPUT --help"
timeout --foreground 60 "$OUTPUT" --appimage-extract-and-run --help
ls -lh "$OUTPUT"
echo "AppImage ready: $OUTPUT"
