#!/usr/bin/env bash
# Check a CoreELEC device AppImage against the CoreELEC sysroot it targets.
#
#   scripts/verify-coreelec-appimage.sh TARGET APPIMAGE
#
# Runs on an x86_64 host with qemu-user-static. No GS-King X is involved.
#
# Static checks:
#   - The type-2 runtime and every ELF inside are TARGET's architecture, and
#     executables use CoreELEC's program interpreter.
#   - Every NEEDED library is either in the AppImage or in the CoreELEC
#     sysroot, and none of them is X11, VAAPI, VDPAU, OpenCL, or desktop GL.
#   - Every versioned symbol reference (GLIBC_*, GLIBCXX_*, ...) is defined
#     by the library that will provide it on CoreELEC.
#
# Run-time checks under qemu-user, with the CoreELEC sysroot as the
# library root (qemu -L), so the dynamic loader, glibc, libstdc++, and
# FFmpeg are CoreELEC's own files:
#   - The AppImage runtime unpacks itself with --appimage-extract, which is
#     what scripts/launch-coreelec.sh does on the device.
#   - AppRun --help.
#   - AppRun --url decodes a local fixture through CoreELEC's FFmpeg
#     (needs the host ffmpeg CLI to write it).
#   - AppRun connects to a local mosquitto broker and exits 0 on SIGTERM
#     (needs the host mosquitto broker).
# AMS_REQUIRE_DEVICE_CHECKS=1 turns a skipped run-time check into a failure.
#
# Not checked here (requires GS-King X / CoreELEC): the CoreELEC kernel,
# /dev/dri, Amlogic hardware decode, the Kodi stop/start handover, and
# performance. This build does not open /dev/dri or the Amlogic decoders.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/coreelec-targets.sh
source "$ROOT/scripts/coreelec-targets.sh"

die() {
  echo "FAIL $*" >&2
  exit 1
}

[[ $# -eq 2 ]] || die "usage: $0 TARGET APPIMAGE (TARGET: $COREELEC_TARGETS)"
coreelec_target "$1"
APPIMAGE=$(readlink -f "$2")
[[ -f "$APPIMAGE" ]] || die "no such AppImage: $2"

for cmd in file readelf od timeout "$CE_QEMU"; do
  command -v "$cmd" >/dev/null 2>&1 || die "required command '$cmd' is not installed (qemu is in qemu-user-static)"
done

"$ROOT/scripts/fetch-coreelec-sysroot.sh" "$CE_TARGET" --check >/dev/null
SYSROOT="${COREELEC_CACHE:-$ROOT/.cache/coreelec}/$CE_TARGET/sysroot"
SYSLIB="$SYSROOT/usr/lib"
REQUIRE="${AMS_REQUIRE_DEVICE_CHECKS:-0}"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/coreelec-verify.XXXXXX")
BROKER_PID=""
PLAYER_PID=""
cleanup() {
  [[ -z "$PLAYER_PID" ]] || kill -KILL "$PLAYER_PID" 2>/dev/null || true
  [[ -z "$BROKER_PID" ]] || kill "$BROKER_PID" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

pass() { echo "ok   $*"; }
skip() {
  if [[ "$REQUIRE" == 1 ]]; then
    die "$* (AMS_REQUIRE_DEVICE_CHECKS=1)"
  fi
  echo "skip $*"
}

echo "Verifying $APPIMAGE"
echo "Target: $CE_LINE, CoreELEC $CE_RELEASE ($CE_TARGET)"
echo "Sysroot: $SYSROOT"

# --- AppImage runtime --------------------------------------------------------
desc=$(file -b "$APPIMAGE")
echo "$desc" | grep -E -q "$CE_FILE_PATTERN" || die "AppImage runtime is not $CE_TARGET: $desc"
magic=$(od -An -c -j 8 -N 3 "$APPIMAGE" | tr -d ' ')
[[ "$magic" == 'AI002' ]] || die "not a type-2 AppImage (magic '$magic')"
pass "runtime: $desc"

(cd "$WORK" && "$CE_QEMU" "$APPIMAGE" --appimage-extract >/dev/null) ||
  die "--appimage-extract failed under $CE_QEMU"
APPDIR="$WORK/squashfs-root"
[[ -x "$APPDIR/AppRun" ]] || die "extracted AppImage has no AppRun"
pass "--appimage-extract under $CE_QEMU"

# --- ELF closure ---------------------------------------------------------------
mapfile -t elves < <(find "$APPDIR" -type f -exec sh -c 'readelf -h "$1" >/dev/null 2>&1 && echo "$1"' _ {} \;)
[[ ${#elves[@]} -gt 0 ]] || die "no ELF files in the AppImage"

needed_of() {
  readelf -d "$1" 2>/dev/null | awk -F'[][]' '/\(NEEDED\)/ { print $2 }'
}

# Provider of SONAME on the device: the AppImage's usr/lib, else CoreELEC.
provider_of() {
  if [[ -e "$APPDIR/usr/lib/$1" ]]; then
    echo "$APPDIR/usr/lib/$1"
  elif [[ -e "$SYSLIB/$1" ]]; then
    readlink -f "$SYSLIB/$1"
  else
    return 1
  fi
}

declare -A from_coreelec=() bundled=()
for elf in "${elves[@]}"; do
  rel=${elf#"$APPDIR"/}
  desc=$(file -b "$elf")
  echo "$desc" | grep -E -q "$CE_FILE_PATTERN" || die "$rel is not $CE_TARGET: $desc"
  if readelf -l "$elf" 2>/dev/null | grep -q 'Requesting program interpreter'; then
    interp=$(readelf -l "$elf" | sed -n 's/.*Requesting program interpreter: \([^]]*\)\]/\1/p')
    [[ "$interp" == "$CE_INTERP" ]] || die "$rel uses interpreter $interp, CoreELEC has $CE_INTERP"
  fi
  case "${elf##*/}" in
    libX11* | libxcb* | libva* | libvdpau* | libOpenCL* | libGL.* | libGLX*)
      die "$rel is part of a desktop graphics stack and must not be bundled" ;;
  esac
  [[ "$rel" == usr/lib/* ]] && bundled[${elf##*/}]=1
  while IFS= read -r soname; do
    [[ -n "$soname" ]] || continue
    case "$soname" in
      libX11* | libxcb* | libva* | libvdpau* | libOpenCL* | libGL.* | libGLX*)
        die "$rel needs $soname (desktop graphics stack)" ;;
    esac
    provider_of "$soname" >/dev/null || die "$rel needs $soname, which is neither in the AppImage nor in CoreELEC $CE_RELEASE"
    [[ -e "$APPDIR/usr/lib/$soname" ]] || from_coreelec[$soname]=1
  done < <(needed_of "$elf")
done
pass "architecture and interpreter ($CE_INTERP) of ${#elves[@]} ELF file(s)"
echo "     bundled:       $(printf '%s ' "${!bundled[@]}" | tr ' ' '\n' | sort | tr '\n' ' ')"
echo "     from CoreELEC: $(printf '%s ' "${!from_coreelec[@]}" | tr ' ' '\n' | sort | tr '\n' ' ')"

# Version names a library defines (.gnu.version_d).
defined_versions() {
  readelf -V --wide "$1" 2>/dev/null | awk '
    /^Version definition section/ { d = 1; next }
    /^Version (needs|symbols) section/ { d = 0 }
    d && /Rev:/ { for (i = 1; i <= NF; i++) if ($i == "Name:") print $(i + 1) }'
}

# "soname version" pairs an ELF requires (.gnu.version_r).
needed_versions() {
  readelf -V --wide "$1" 2>/dev/null | awk '
    /^Version needs section/ { n = 1; next }
    /^Version (definition|symbols) section/ { n = 0 }
    n && /File:/ { for (i = 1; i <= NF; i++) if ($i == "File:") file = $(i + 1) }
    n && /Name:/ && !/File:/ { for (i = 1; i <= NF; i++) if ($i == "Name:") print file, $(i + 1) }'
}

declare -A defs=()
max_glibc=""
for elf in "${elves[@]}"; do
  rel=${elf#"$APPDIR"/}
  while read -r soname version; do
    [[ -n "$soname" ]] || continue
    lib=$(provider_of "$soname") || die "$rel: no provider for $soname"
    if [[ -z "${defs[$lib]:-}" ]]; then
      defs[$lib]=" $(defined_versions "$lib" | tr '\n' ' ') "
    fi
    [[ "${defs[$lib]}" == *" $version "* ]] ||
      die "$rel needs $version from $soname, which CoreELEC $CE_RELEASE does not provide"
    if [[ "$version" == GLIBC_[0-9]* ]]; then
      max_glibc=$(printf '%s\n%s\n' "$max_glibc" "${version#GLIBC_}" | sort -V | tail -n 1)
    fi
  done < <(needed_versions "$elf")
done
ce_glibc=$(defined_versions "$(readlink -f "$SYSLIB/libc.so.6")" | sed -n 's/^GLIBC_\([0-9][0-9.]*\)$/\1/p' | sort -V | tail -n 1)
pass "symbol versions resolve against CoreELEC (needs GLIBC_${max_glibc:-none}, CoreELEC has GLIBC_${ce_glibc})"

# --- Run under qemu with CoreELEC's libraries -----------------------------------
PLAYER=("$CE_QEMU" -L "$SYSROOT" "$APPDIR/AppRun")

if ! timeout 120 "$CE_QEMU" -L "$SYSROOT" "$SYSROOT$CE_INTERP" --list "$APPDIR/usr/bin/amlogic-multistream" >"$WORK/ldd.txt" 2>&1; then
  cat "$WORK/ldd.txt" >&2
  die "CoreELEC's dynamic loader could not resolve the player"
fi
if grep -q 'not found' "$WORK/ldd.txt"; then
  cat "$WORK/ldd.txt" >&2
  die "CoreELEC's dynamic loader could not find a library"
fi
pass "CoreELEC $CE_INTERP resolves every library"
sed 's/^/     /' "$WORK/ldd.txt"

timeout 120 "${PLAYER[@]}" --help >"$WORK/help.txt" 2>&1 || { cat "$WORK/help.txt" >&2; die "AppRun --help failed"; }
grep -q 'MQTT' "$WORK/help.txt" || { cat "$WORK/help.txt" >&2; die "AppRun --help printed no usage"; }
pass "AppRun --help under $CE_QEMU"

if command -v ffmpeg >/dev/null 2>&1; then
  # HLS with H.264 when the host can encode it, else an MPEG-2 transport stream.
  encoders=$(ffmpeg -hide_banner -encoders 2>/dev/null || true)
  if grep -q ' libx264 ' <<<"$encoders"; then
    fixture="$WORK/hls/index.m3u8"
    mkdir -p "$WORK/hls"
    ffmpeg -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=10:duration=2 \
      -c:v libx264 -pix_fmt yuv420p -hls_time 1 -hls_list_size 0 "$fixture"
  else
    fixture="$WORK/fixture.ts"
    ffmpeg -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=10:duration=1 \
      -c:v mpeg2video -pix_fmt yuv420p -f mpegts "$fixture"
  fi
  if ! timeout 300 "${PLAYER[@]}" --url "$fixture" --frames 3 >"$WORK/decode.txt" 2>&1; then
    cat "$WORK/decode.txt" >&2
    die "AppRun --url failed to decode a local fixture with CoreELEC's FFmpeg"
  fi
  if ! grep -q 'isHardware=0' "$WORK/decode.txt" || ! grep -q '^presented 2 ' "$WORK/decode.txt"; then
    cat "$WORK/decode.txt" >&2
    die "AppRun --url did not present 3 frames"
  fi
  pass "AppRun --url software-decoded 3 frames with CoreELEC FFmpeg ($(sed -n 's/.* codec=\([^ ]*\).*/\1/p' "$WORK/decode.txt" | head -n 1))"
else
  skip "AppRun --url decode (no host ffmpeg CLI to write a fixture)"
fi

mosquitto_bin=$(command -v mosquitto || true)
[[ -n "$mosquitto_bin" || ! -x /usr/sbin/mosquitto ]] || mosquitto_bin=/usr/sbin/mosquitto
if [[ -n "$mosquitto_bin" ]]; then
  port=$((20000 + RANDOM % 20000))
  printf 'listener %s 127.0.0.1\nallow_anonymous true\npersistence false\nlog_dest stderr\nlog_type all\n' \
    "$port" > "$WORK/mosquitto.conf"
  "$mosquitto_bin" -c "$WORK/mosquitto.conf" >"$WORK/broker.log" 2>&1 &
  BROKER_PID=$!
  sleep 0.5
  "${PLAYER[@]}" --mqtt-host 127.0.0.1 --mqtt-port "$port" --mqtt-client-id ams-verify \
    >"$WORK/mqtt.txt" 2>&1 &
  PLAYER_PID=$!
  connected=0
  for _ in $(seq 1 300); do
    if grep -q 'as ams-verify' "$WORK/broker.log" && grep -q 'SUBSCRIBE' "$WORK/broker.log"; then
      connected=1
      break
    fi
    kill -0 "$PLAYER_PID" 2>/dev/null || break
    sleep 0.1
  done
  if [[ "$connected" -ne 1 ]]; then
    cat "$WORK/mqtt.txt" "$WORK/broker.log" >&2
    die "player did not connect and subscribe through the bundled libmosquitto"
  fi
  kill -TERM "$PLAYER_PID"
  status=0
  wait "$PLAYER_PID" || status=$?
  PLAYER_PID=""
  if [[ "$status" -ne 0 ]] || ! grep -q 'MQTT shutting down on signal 15' "$WORK/mqtt.txt"; then
    cat "$WORK/mqtt.txt" >&2
    die "player did not exit 0 on SIGTERM (status $status)"
  fi
  pass "MQTT connect, subscribe, and SIGTERM shutdown through the bundled libmosquitto"
else
  skip "MQTT connect (no host mosquitto broker)"
fi

echo "Not checked (requires GS-King X / CoreELEC): kernel, /dev/dri, Amlogic hardware decode, Kodi handover."
echo "$CE_TARGET AppImage checks ok"
