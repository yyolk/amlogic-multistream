#!/usr/bin/env bash
# Cross-build the player against a CoreELEC sysroot and package it as a
# device AppImage.
#
#   scripts/build-coreelec-appimage.sh TARGET [--no-fetch]
#
#   coreelec21-ng-armv7hf  CoreELEC 21 Amlogic-ng, 32-bit ARM (armv7 hard-float)
#   coreelec22-no-aarch64  CoreELEC 22 Amlogic-no, 64-bit ARM
#
# Output: dist/TARGET/amlogic-multistream-<version>-TARGET.AppImage
#
# This is the device build. scripts/build-appimage.sh is the separate
# native host smoke build (x86_64 or Ubuntu aarch64); it bundles Ubuntu's
# FFmpeg and is not a CoreELEC binary.
#
# Build host: x86_64 Linux with Ubuntu's cross toolchain for the target
# (g++-arm-linux-gnueabihf or g++-aarch64-linux-gnu). The player links the
# shared libraries of the CoreELEC image itself: glibc, libstdc++, FFmpeg
# (libavformat, libavcodec, libavutil, libswscale), and libdrm. Those come
# from scripts/fetch-coreelec-sysroot.sh. The image has no headers, so
# headers come from pinned upstream releases that match it: FFmpeg at the
# version the image ships, and libdrm. CoreELEC has no libmosquitto, so it
# is cross-built from a pinned release (no TLS) and is the only library in
# the AppImage. Nothing from Ubuntu's X11, VAAPI, VDPAU, or OpenCL stack is
# linked or bundled.
#
# The AppImage is then checked by scripts/verify-coreelec-appimage.sh under
# qemu-user against the same CoreELEC libraries. That is not a GS-King X:
# DRM/KMS, Amlogic hardware decode, and the Kodi handover are not covered,
# and the default build does not open /dev/dri.
#
# --no-fetch fails instead of downloading the CoreELEC archive when the
# sysroot is not already in ${COREELEC_CACHE:-.cache/coreelec}.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/coreelec-targets.sh
source "$ROOT/scripts/coreelec-targets.sh"

die() {
  echo "error: $*" >&2
  exit 1
}

TARGET=""
NO_FETCH=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-fetch) NO_FETCH=1 ;;
    -h | --help)
      sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    -*) die "unknown argument: $1" ;;
    *)
      [[ -z "$TARGET" ]] || die "only one TARGET is accepted"
      TARGET=$1
      ;;
  esac
  shift
done
[[ -n "$TARGET" ]] || die "usage: $0 TARGET [--no-fetch] (TARGET: $COREELEC_TARGETS)"
coreelec_target "$TARGET"

[[ "$(uname -m)" == x86_64 ]] || die "the device build runs on an x86_64 host (found $(uname -m))"

for cmd in cmake make pkg-config curl tar xz sha256sum patchelf file readelf \
  "$CE_TRIPLE-gcc" "$CE_TRIPLE-g++" "$CE_TRIPLE-strip"; do
  command -v "$cmd" >/dev/null 2>&1 || die "required command '$cmd' is not installed"
done

CACHE="${COREELEC_CACHE:-$ROOT/.cache/coreelec}"
export COREELEC_CACHE="$CACHE"
SYSROOT="$CACHE/$TARGET/sysroot"
DEPS="$CACHE/$TARGET/deps"
DOWNLOADS="$CACHE/downloads"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-$TARGET}"
DIST_DIR="${DIST_DIR:-$ROOT/dist/$TARGET}"
APPDIR="$DIST_DIR/AppDir"

VERSION="${VERSION:-}"
if [[ -z "$VERSION" ]]; then
  VERSION="$(sed -n 's/^project(.*VERSION[[:space:]]\{1,\}\([0-9][^[:space:]]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
fi
VERSION="${VERSION:-0.1.0}"
OUTPUT="$DIST_DIR/amlogic-multistream-${VERSION}-${TARGET}.AppImage"

echo "Device AppImage for $CE_LINE, CoreELEC $CE_RELEASE ($TARGET)"

if [[ "$NO_FETCH" -eq 1 ]]; then
  "$ROOT/scripts/fetch-coreelec-sysroot.sh" "$TARGET" --check
else
  "$ROOT/scripts/fetch-coreelec-sysroot.sh" "$TARGET"
fi

JOBS=$(nproc 2>/dev/null || echo 2)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/coreelec-build.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The sysroot is only searched for the NEEDED entries of CoreELEC's own
# libraries (rpath-link). Its libc.so is a linker script that names a
# libc_nonshared.a the image does not ship, so it is never on -L. The
# cross toolchain's glibc is what the player links; verify-coreelec-appimage.sh
# then checks every symbol version against the sysroot's glibc.
#
# CoreELEC's libraries may need a newer glibc than the cross toolchain has
# (CoreELEC 22 ships glibc 2.44; Ubuntu 24.04's cross glibc is 2.39), so
# --allow-shlib-undefined stops ld from resolving their references against
# the toolchain's libc. Those references resolve against CoreELEC's glibc
# on the device, which verify-coreelec-appimage.sh exercises under qemu.
TOOLCHAIN="$CACHE/$TARGET/toolchain.cmake"
mkdir -p "$(dirname "$TOOLCHAIN")"
cat > "$TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $CE_CMAKE_PROCESSOR)
set(CMAKE_C_COMPILER $CE_TRIPLE-gcc)
set(CMAKE_CXX_COMPILER $CE_TRIPLE-g++)
set(CMAKE_CXX_FLAGS_INIT "-Wno-psabi")
set(CMAKE_FIND_ROOT_PATH "$DEPS")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-rpath-link,$SYSROOT/usr/lib -Wl,--allow-shlib-undefined")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,-rpath-link,$SYSROOT/usr/lib")
EOF

export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig"
export PKG_CONFIG_PATH=""
unset PKG_CONFIG_SYSROOT_DIR || true

DEPS_ID="ffmpeg-$CE_FFMPEG_VERSION libdrm-$CE_LIBDRM_VERSION mosquitto-$CE_MOSQUITTO_VERSION sysroot-$CE_TAR_SHA256 toolchain-$("$CE_TRIPLE-gcc" -dumpfullversion)"
DEPS_STAMP="$DEPS/.deps"

# Point a dev symlink in DEPS/lib at the CoreELEC library with that soname.
link_sysroot_lib() {
  local soname=$1 base=${1%%.so*}
  [[ -e "$SYSROOT/usr/lib/$soname" ]] || die "sysroot has no $soname"
  ln -sfn "$SYSROOT/usr/lib/$soname" "$DEPS/lib/$base.so"
}

build_deps() {
  rm -rf "$DEPS"
  mkdir -p "$DEPS/include" "$DEPS/lib/pkgconfig"

  # FFmpeg headers and .pc files at the version the image ships. Nothing is
  # compiled; the libraries are CoreELEC's.
  local ffmpeg_tar="$DOWNLOADS/ffmpeg-$CE_FFMPEG_VERSION.tar.xz"
  coreelec_download "$CE_FFMPEG_URL" "$ffmpeg_tar" "$CE_FFMPEG_SHA256"
  mkdir -p "$WORK/ffmpeg"
  tar -C "$WORK/ffmpeg" --strip-components=1 -xf "$ffmpeg_tar"
  echo "Installing FFmpeg $CE_FFMPEG_VERSION headers"
  (
    cd "$WORK/ffmpeg"
    ./configure --prefix="$DEPS" --enable-cross-compile --cross-prefix="$CE_TRIPLE-" \
      --arch="$CE_FFMPEG_ARCH" --target-os=linux --enable-shared --disable-static \
      --disable-programs --disable-doc --disable-everything --disable-asm \
      --disable-autodetect >"$WORK/ffmpeg-configure.log" 2>&1 ||
      { cat "$WORK/ffmpeg-configure.log" >&2; exit 1; }
    make install-headers >"$WORK/ffmpeg-install.log" 2>&1 ||
      { cat "$WORK/ffmpeg-install.log" >&2; exit 1; }
  )
  local soname
  for soname in $CE_FFMPEG_SONAMES; do
    link_sysroot_lib "$soname"
  done
  check_ffmpeg_versions

  # libdrm headers. Only -DAMLOGIC_DRM_KMS=ON includes them; the player
  # target needs libdrm.pc either way.
  local drm_tar="$DOWNLOADS/libdrm-$CE_LIBDRM_VERSION.tar.xz"
  coreelec_download "$CE_LIBDRM_URL" "$drm_tar" "$CE_LIBDRM_SHA256"
  mkdir -p "$WORK/libdrm" "$DEPS/include/libdrm"
  tar -C "$WORK/libdrm" --strip-components=1 -xf "$drm_tar"
  cp "$WORK/libdrm/xf86drm.h" "$WORK/libdrm/xf86drmMode.h" "$DEPS/include/"
  cp "$WORK/libdrm"/include/drm/*.h "$DEPS/include/libdrm/"
  link_sysroot_lib libdrm.so.2
  cat > "$DEPS/lib/pkgconfig/libdrm.pc" <<EOF
prefix=$DEPS
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libdrm
Description: CoreELEC $CE_RELEASE libdrm.so.2 with libdrm $CE_LIBDRM_VERSION headers
Version: $CE_LIBDRM_VERSION
Libs: -L\${libdir} -ldrm
Cflags: -I\${includedir} -I\${includedir}/libdrm
EOF

  # libmosquitto is not part of CoreELEC. Client library only, no TLS (the
  # player does not call mosquitto_tls_set), no cJSON.
  local mq_tar="$DOWNLOADS/mosquitto-$CE_MOSQUITTO_VERSION.tar.gz"
  coreelec_download "$CE_MOSQUITTO_URL" "$mq_tar" "$CE_MOSQUITTO_SHA256"
  mkdir -p "$WORK/mosquitto"
  tar -C "$WORK/mosquitto" --strip-components=1 -xzf "$mq_tar"
  echo "Cross-building libmosquitto $CE_MOSQUITTO_VERSION"
  # lib/CMakeLists.txt links OpenSSL::SSL even with WITH_TLS=OFF. An empty
  # target satisfies that without linking OpenSSL.
  echo 'add_library(OpenSSL::SSL INTERFACE IMPORTED)' > "$WORK/mosquitto-no-tls.cmake"
  cmake -S "$WORK/mosquitto" -B "$WORK/mosquitto-build" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PROJECT_INCLUDE="$WORK/mosquitto-no-tls.cmake" \
    -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_INSTALL_LIBDIR=lib \
    -DWITH_TLS=OFF -DWITH_TLS_PSK=OFF -DWITH_CJSON=OFF -DWITH_SRV=OFF \
    -DWITH_BROKER=OFF -DWITH_APPS=OFF -DWITH_PLUGINS=OFF -DWITH_CLIENTS=OFF \
    -DWITH_LIB_CPP=OFF -DDOCUMENTATION=OFF -DWITH_STATIC_LIBRARIES=OFF \
    >"$WORK/mosquitto-configure.log" 2>&1 ||
    { cat "$WORK/mosquitto-configure.log" >&2; exit 1; }
  cmake --build "$WORK/mosquitto-build" --parallel "$JOBS" >"$WORK/mosquitto-build.log" 2>&1 ||
    { cat "$WORK/mosquitto-build.log" >&2; exit 1; }
  cmake --install "$WORK/mosquitto-build" >/dev/null
  [[ -f "$DEPS/lib/pkgconfig/libmosquitto.pc" ]] || die "libmosquitto.pc was not installed"

  printf '%s\n' "$DEPS_ID" > "$DEPS_STAMP"
}

# The headers must describe the libraries the player will load on the device.
check_ffmpeg_versions() {
  local soname lib major header
  for soname in $CE_FFMPEG_SONAMES; do
    lib=${soname%%.so*}
    major=${soname##*.so.}
    header="$DEPS/include/$lib/version_major.h"
    [[ -f "$header" ]] || header="$DEPS/include/$lib/version.h"
    if ! grep -E -q "define[[:space:]]+${lib^^}_VERSION_MAJOR[[:space:]]+${major}\$" "$header"; then
      die "FFmpeg $CE_FFMPEG_VERSION headers do not match $soname from CoreELEC $CE_RELEASE"
    fi
  done
}

if [[ ! -f "$DEPS_STAMP" || "$(cat "$DEPS_STAMP")" != "$DEPS_ID" ]]; then
  build_deps
else
  echo "Using cached dependencies in $DEPS"
fi

CMAKE_ARGS=(
  -S "$ROOT"
  -B "$BUILD_DIR"
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
)
if command -v ccache >/dev/null 2>&1; then
  CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
# CMake reads the toolchain's *_INIT flags only on the first configure.
toolchain_sha=$(sha256sum "$TOOLCHAIN" | cut -d' ' -f1)
if [[ -f "$BUILD_DIR/CMakeCache.txt" && "$(cat "$BUILD_DIR/.toolchain-sha256" 2>/dev/null)" != "$toolchain_sha" ]]; then
  rm -rf "$BUILD_DIR"
fi
echo "Configuring player for $TARGET"
cmake "${CMAKE_ARGS[@]}"
printf '%s\n' "$toolchain_sha" > "$BUILD_DIR/.toolchain-sha256"
cmake --build "$BUILD_DIR" --target amlogic-multistream --parallel "$JOBS"
BIN="$BUILD_DIR/amlogic-multistream"
[[ -x "$BIN" ]] || die "player was not built at $BIN"
file "$BIN" | grep -E -q "$CE_FILE_PATTERN" || die "$(file "$BIN") is not the $TARGET architecture"

echo "Assembling $APPDIR"
rm -rf "$APPDIR"
rm -f "$DIST_DIR"/*.AppImage
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/amlogic-multistream"
cp "$BIN" "$APPDIR/usr/bin/amlogic-multistream"
cp -L "$DEPS/lib/libmosquitto.so.1" "$APPDIR/usr/lib/libmosquitto.so.1"
"$CE_TRIPLE-strip" --strip-unneeded "$APPDIR/usr/bin/amlogic-multistream" "$APPDIR/usr/lib/libmosquitto.so.1"
# $ORIGIN is a dynamic-linker token, not a shell variable.
# shellcheck disable=SC2016
patchelf --set-rpath '$ORIGIN/../lib' "$APPDIR/usr/bin/amlogic-multistream"
ln -s usr/bin/amlogic-multistream "$APPDIR/AppRun"
cp "$ROOT/packaging/amlogic-multistream.desktop" "$APPDIR/"
cp "$ROOT/packaging/amlogic-multistream.png" "$APPDIR/"
ln -s amlogic-multistream.png "$APPDIR/.DirIcon"
cat > "$APPDIR/usr/share/amlogic-multistream/coreelec-target" <<EOF
target=$TARGET
coreelec_release=$CE_RELEASE
coreelec_arch=$CE_DISTRO_ARCH
coreelec_archive_sha256=$CE_TAR_SHA256
ffmpeg_headers=$CE_FFMPEG_VERSION
libmosquitto=$CE_MOSQUITTO_VERSION
version=$VERSION
EOF

APPIMAGETOOL="$DOWNLOADS/appimagetool-$CE_APPIMAGETOOL_VERSION-x86_64.AppImage"
coreelec_download "$CE_APPIMAGETOOL_URL" "$APPIMAGETOOL" "$CE_APPIMAGETOOL_SHA256"
chmod +x "$APPIMAGETOOL"
RUNTIME="$DOWNLOADS/runtime-$CE_RUNTIME_VERSION-$CE_RUNTIME_ARCH"
coreelec_download "$CE_RUNTIME_URL" "$RUNTIME" "$CE_RUNTIME_SHA256"

echo "Packing $OUTPUT"
# Extracted instead of FUSE-mounted so containers and CI work.
APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$CE_RUNTIME_ARCH" "$APPIMAGETOOL" --no-appstream \
  --runtime-file "$RUNTIME" "$APPDIR" "$OUTPUT"
[[ -f "$OUTPUT" ]] || die "appimagetool did not write $OUTPUT"
chmod +x "$OUTPUT"

"$ROOT/scripts/verify-coreelec-appimage.sh" "$TARGET" "$OUTPUT"
ls -lh "$OUTPUT"
echo "Device AppImage ready: $OUTPUT"
