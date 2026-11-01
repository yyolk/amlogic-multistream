#!/usr/bin/env bash
# Build amlogic-multistream and package it as an AppImage.
#
# Host: normal Linux. The AppImage matches this machine: x86_64 for a host
# smoke build, aarch64 for a native 64-bit ARM build. The aarch64 image is
# not the GS-King X binary. CoreELEC Amlogic-ng is 32-bit ARM userspace, and
# this script does not cross-compile it. CI runs the script once on each
# native runner. No GS-King X or CoreELEC image is required to package.
#
# linuxdeploy fills the AppDir (binary, desktop file, icon, shared libraries).
# Its excludelist omits libX11 and the libraries behind Ubuntu's FFmpeg
# (libxcb, fontconfig, freetype, harfbuzz, and the rest of that closure).
# This script copies those back in. A host — or a chroot — that only has
# glibc, libstdc++, libgcc_s, zlib, and libdrm can then run --help.
# appimagetool packs the AppDir after that copy. The smoke image is still
# not a GS-King X / CoreELEC build; a device AppImage should link that
# system's FFmpeg instead of shipping Ubuntu's X11 stack.
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

for cmd in cmake g++ pkg-config curl patchelf file strip timeout readelf sudo; do
  need "$cmd"
done
sudo -n true || die "passwordless sudo is required for the minimal-chroot --help smoke test"

LDCONFIG=$(command -v ldconfig || true)
if [[ -z "$LDCONFIG" && -x /sbin/ldconfig ]]; then
  LDCONFIG=/sbin/ldconfig
fi
[[ -n "$LDCONFIG" ]] || die "ldconfig is required to locate shared libraries"

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

# Sonames a minimal host is expected to provide. Everything else that the
# player needs is copied into the AppDir, including libraries linuxdeploy's
# excludelist would leave on the build machine (libX11 and its closure).
is_host_soname() {
  case "$1" in
    libc.so.6 | libm.so.6 | libdl.so.2 | librt.so.1 | libpthread.so.0 | \
      libresolv.so.2 | libutil.so.1 | libstdc++.so.6 | libgcc_s.so.1 | \
      libz.so.1 | libdrm.so.2 | ld-linux.so.* | ld-linux-*.so.*)
      return 0
      ;;
  esac
  return 1
}

needed_sonames() {
  readelf -d "$1" 2>/dev/null | awk -F'[][]' '/\(NEEDED\)/ { print $2 }'
}

find_system_soname() {
  local name=$1
  local path
  path=$("$LDCONFIG" -p | awk -v n="$name" '$1 == n { print $NF; exit }')
  [[ -n "$path" ]] || return 1
  readlink -f "$path"
}

# Copy NEEDED libraries that are not already in the AppDir and are not on
# the host allowlist. New files land next to libavutil so the existing
# $ORIGIN rpath finds them.
bundle_excluded_libraries() {
  local appdir=$1
  local avutil libdir
  avutil=$(find "$appdir" -name 'libavutil.so.*' -print -quit)
  [[ -n "$avutil" ]] || die "linuxdeploy did not bundle libavutil"
  libdir=$(dirname "$avutil")
  echo "Bundling libraries the minimal host does not provide into $libdir"

  local -A seen=()
  local -a queue=()
  enqueue() {
    local resolved
    resolved=$(readlink -f "$1")
    [[ -n "$resolved" && -f "$resolved" ]] || return 0
    if [[ -n "${seen[$resolved]:-}" ]]; then
      return 0
    fi
    seen[$resolved]=1
    queue+=("$resolved")
  }

  local file
  while IFS= read -r -d '' file; do
    if readelf -h "$file" >/dev/null 2>&1; then
      enqueue "$file"
    fi
  done < <(find "$appdir" -type f -print0)

  local index=0
  while [[ $index -lt ${#queue[@]} ]]; do
    file=${queue[$index]}
    index=$((index + 1))
    local soname
    while IFS= read -r soname; do
      [[ -n "$soname" ]] || continue
      if is_host_soname "$soname"; then
        continue
      fi
      local existing
      existing=$(find "$appdir" -name "$soname" -print -quit || true)
      if [[ -n "$existing" ]]; then
        enqueue "$existing"
        continue
      fi
      local src dest
      src=$(find_system_soname "$soname") || die "no system library for $soname (needed by $file)"
      dest="$libdir/$soname"
      echo "  bundle $soname"
      cp -a "$src" "$dest"
      # $ORIGIN is a dynamic-linker token, not a shell variable.
      # shellcheck disable=SC2016
      patchelf --set-rpath '$ORIGIN' "$dest"
      enqueue "$dest"
    done < <(needed_sonames "$file")
  done

  local missing=0
  while IFS= read -r -d '' file; do
    readelf -h "$file" >/dev/null 2>&1 || continue
    local soname
    while IFS= read -r soname; do
      [[ -n "$soname" ]] || continue
      if is_host_soname "$soname"; then
        continue
      fi
      if [[ ! -e "$libdir/$soname" ]]; then
        echo "missing $soname needed by $file" >&2
        missing=1
      fi
    done < <(needed_sonames "$file")
  done < <(find "$appdir" -type f -print0)
  [[ "$missing" -eq 0 ]] || die "AppDir is still missing shared libraries"
  LIBDIR=$libdir
}

CHROOT_DIR=""
cleanup_chroot() {
  if [[ -n "${CHROOT_DIR}" && -d "$CHROOT_DIR" ]]; then
    sudo umount "$CHROOT_DIR/proc" 2>/dev/null || true
    sudo umount "$CHROOT_DIR/app" 2>/dev/null || true
    rm -rf "$CHROOT_DIR"
    CHROOT_DIR=""
  fi
}
trap cleanup_chroot EXIT

# --help must start when the only libraries outside the AppDir are glibc,
# libstdc++, libgcc_s, zlib, and libdrm. That is the failure mode of an
# excludelist that drops libX11 while Ubuntu's libavutil still needs it.
smoke_minimal_chroot() {
  local appdir=$1
  local bin="$appdir/usr/bin/amlogic-multistream"
  [[ -e "$appdir/AppRun" ]] || die "AppDir has no AppRun"
  [[ -e "$bin" ]] || die "AppDir has no player binary"
  local interp interp_src
  interp=$(readelf -l "$bin" | sed -n 's/.*interpreter: \([^]]*\)\]/\1/p' | head -n 1)
  [[ -n "$interp" ]] || die "could not read the ELF interpreter from $bin"
  interp_src=$(readlink -f "$interp")
  [[ -f "$interp_src" ]] || die "interpreter not found: $interp"

  CHROOT_DIR=$(mktemp -d /tmp/amlogic-min-chroot.XXXXXX)
  mkdir -p "$CHROOT_DIR/opt/base" "$CHROOT_DIR/proc" "$CHROOT_DIR/app" \
    "$CHROOT_DIR/tmp" "$CHROOT_DIR/dev" "$CHROOT_DIR$(dirname "$interp")"
  cp -a "$interp_src" "$CHROOT_DIR$interp"
  sudo mknod -m 666 "$CHROOT_DIR/dev/null" c 1 3
  sudo mount --bind /proc "$CHROOT_DIR/proc"
  sudo mount --bind "$appdir" "$CHROOT_DIR/app"

  local -A wanted=()
  local file soname
  while IFS= read -r -d '' file; do
    readelf -h "$file" >/dev/null 2>&1 || continue
    while IFS= read -r soname; do
      [[ -n "$soname" ]] || continue
      if is_host_soname "$soname"; then
        wanted["$soname"]=1
      fi
    done < <(needed_sonames "$file")
  done < <(find "$appdir" -type f -print0)
  # glibc dlopens these. They are part of glibc, not X11.
  for soname in libnss_files.so.2 libnss_dns.so.2; do
    if "$LDCONFIG" -p | awk -v n="$soname" '$1 == n { found = 1 } END { exit !found }'; then
      wanted["$soname"]=1
    fi
  done

  local name src
  for name in "${!wanted[@]}"; do
    case "$name" in
      ld-linux*) continue ;;
    esac
    src=$(find_system_soname "$name") || die "host has no $name for the minimal chroot"
    cp -a "$src" "$CHROOT_DIR/opt/base/$name"
  done

  local lib_rel=${LIBDIR#"$appdir"}
  echo "Smoke-testing --help with only glibc, libstdc++, libgcc_s, zlib, and libdrm outside the AppDir"
  sudo env LANG=C LC_ALL=C chroot "$CHROOT_DIR" "$interp" \
    --library-path "/opt/base:/app${lib_rel}" \
    /app/AppRun --help
  cleanup_chroot
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
  --icon-file "$ROOT/packaging/amlogic-multistream.png"

bundle_excluded_libraries "$APPDIR"
smoke_minimal_chroot "$APPDIR"

echo "Packing $OUTPUT"
"$TOOLS_DIR/appimagetool-${ARCH}.AppImage" "$APPDIR" "$OUTPUT"

[[ -f "$OUTPUT" ]] || die "appimagetool did not write $OUTPUT"
chmod +x "$OUTPUT"

echo "Smoke-testing $OUTPUT --help"
timeout --foreground 60 "$OUTPUT" --appimage-extract-and-run --help
ls -lh "$OUTPUT"
echo "AppImage ready: $OUTPUT"
