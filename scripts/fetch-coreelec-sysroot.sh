#!/usr/bin/env bash
# Fetch a CoreELEC sysroot from the official update archive.
#
#   scripts/fetch-coreelec-sysroot.sh TARGET [--check] [--keep-archive]
#
# TARGET is coreelec21-ng-armv7hf or coreelec22-no-aarch64 (see
# scripts/coreelec-targets.sh). The CoreELEC-<arch>-<release>.tar update
# archive from github.com/CoreELEC/CoreELEC/releases is downloaded, checked
# against the pinned sha256, and its target/SYSTEM squashfs is unpacked.
# Only the shared libraries in /usr/lib and /etc/os-release are kept: the
# player links CoreELEC's glibc, libstdc++, FFmpeg, and libdrm from there,
# and the AppImage check runs it under qemu-user against the same files.
# The Mali GPU blobs, kernel modules, Kodi, and Python are dropped.
#
# The sysroot has no headers and no static archives, so it is a link and
# run-time target only. Headers come from pinned upstream sources (see
# build-coreelec-appimage.sh).
#
# --check does not download. It exits 1 with the command to run when the
# sysroot is missing or was extracted from a different archive.
#
# Output: ${COREELEC_CACHE:-.cache/coreelec}/TARGET/sysroot
# Needs: curl, tar, sha256sum, unsquashfs (squashfs-tools). No root.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/coreelec-targets.sh
source "$ROOT/scripts/coreelec-targets.sh"

die() {
  echo "error: $*" >&2
  exit 1
}

TARGET=""
CHECK_ONLY=0
KEEP_ARCHIVE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --check) CHECK_ONLY=1 ;;
    --keep-archive) KEEP_ARCHIVE=1 ;;
    -h | --help)
      sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
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
[[ -n "$TARGET" ]] || die "usage: $0 TARGET [--check] (TARGET: $COREELEC_TARGETS)"
coreelec_target "$TARGET"

CACHE="${COREELEC_CACHE:-$ROOT/.cache/coreelec}"
SYSROOT="$CACHE/$TARGET/sysroot"
STAMP="$SYSROOT/.coreelec-archive-sha256"

# os-release KEY from the sysroot, without sourcing it.
os_release() {
  sed -n "s/^$1=\"\{0,1\}\([^\"]*\)\"\{0,1\}\$/\1/p" "$SYSROOT/etc/os-release" | head -n 1
}

verify_sysroot() {
  [[ -f "$STAMP" ]] || return 1
  [[ "$(cat "$STAMP")" == "$CE_TAR_SHA256" ]] || return 1
  local arch version
  arch=$(os_release COREELEC_ARCH)
  [[ -n "$arch" ]] || arch=$(os_release DISTRO_ARCH)
  version=$(os_release VERSION)
  if [[ "$arch" != "$CE_DISTRO_ARCH" || "$version" != "$CE_RELEASE" ]]; then
    echo "sysroot os-release is $arch $version, expected $CE_DISTRO_ARCH $CE_RELEASE" >&2
    return 1
  fi
  local lib
  for lib in ${CE_INTERP##*/} libc.so.6 libstdc++.so.6 libdrm.so.2 $CE_FFMPEG_SONAMES; do
    if [[ ! -e "$SYSROOT/usr/lib/$lib" ]]; then
      echo "sysroot is missing usr/lib/$lib" >&2
      return 1
    fi
  done
  [[ -e "$SYSROOT$CE_INTERP" ]] || return 1
}

if verify_sysroot; then
  echo "CoreELEC sysroot ready: $SYSROOT ($CE_LINE, $CE_RELEASE)"
  exit 0
fi

if [[ "$CHECK_ONLY" -eq 1 ]]; then
  echo "error: no $CE_LINE sysroot at $SYSROOT" >&2
  echo "Fetch it from $CE_TAR_URL with:" >&2
  echo "  scripts/fetch-coreelec-sysroot.sh $TARGET" >&2
  exit 1
fi

for cmd in curl tar sha256sum unsquashfs; do
  command -v "$cmd" >/dev/null 2>&1 || die "required command '$cmd' is not installed (unsquashfs is in squashfs-tools)"
done

ARCHIVE="$CACHE/downloads/$CE_TAR_NAME"
coreelec_download "$CE_TAR_URL" "$ARCHIVE" "$CE_TAR_SHA256"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/coreelec-sysroot.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "Extracting target/SYSTEM from $CE_TAR_NAME"
tar -C "$WORK" -xf "$ARCHIVE" --wildcards '*/target/SYSTEM'
SYSTEM=$(find "$WORK" -path '*/target/SYSTEM' -type f -print -quit)
[[ -n "$SYSTEM" ]] || die "$CE_TAR_NAME has no target/SYSTEM"

rm -rf "$SYSROOT"
mkdir -p "$(dirname "$SYSROOT")"
echo "Unpacking shared libraries into $SYSROOT"
# Device nodes and ownership are not needed; -no-xattrs keeps this rootless.
unsquashfs -q -no-xattrs -d "$SYSROOT" "$SYSTEM" \
  'usr/lib/*.so*' etc/os-release usr/lib/os-release >/dev/null
rm -f "$SYSROOT"/usr/lib/libMali*
# CoreELEC's /lib is an absolute symlink to /usr/lib. A relative one keeps
# the program interpreter path working under qemu-user -L.
ln -sfn usr/lib "$SYSROOT/lib"
printf '%s\n' "$CE_TAR_SHA256" > "$STAMP"

if [[ "$KEEP_ARCHIVE" -eq 0 ]]; then
  rm -f "$ARCHIVE"
fi

verify_sysroot || die "extracted sysroot failed verification"
echo "CoreELEC sysroot ready: $SYSROOT ($CE_LINE, $CE_RELEASE, $(du -sh "$SYSROOT" | cut -f1))"
