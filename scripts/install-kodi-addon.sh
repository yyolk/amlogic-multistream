#!/usr/bin/env bash
# Install the Kodi add-on (and optionally the player) onto CoreELEC.
#
# Runs on a workstation. With --host it copies over SSH to a GS-King X /
# CoreELEC box; that part requires the device and is not exercised by CI
# beyond --dry-run. Without --host it installs into a local --storage tree,
# which is what the host tests use.
#
#   <storage>/.kodi/addons/plugin.program.amlogic-multistream/
#   <storage>/amlogic-multistream          (only with --player)
#
# --target picks the CoreELEC line on the box:
#   coreelec21-ng-armv7hf  CoreELEC 21 Amlogic-ng, 32-bit ARM userspace (default)
#   coreelec22-no-aarch64  CoreELEC 22 Amlogic-no, 64-bit ARM userspace
# --player must be an ELF or type-2 AppImage of that architecture. An
# AppImage must also be the device build for that target
# (amlogic-multistream-*-<target>.AppImage from build-coreelec-appimage.sh);
# its embedded target marker is read with unsquashfs. The x86_64 and Ubuntu
# aarch64 CI AppImages are host smoke builds and are refused. With --host,
# the box's /etc/os-release architecture must match --target as well.
#
# Usage:
#   scripts/install-kodi-addon.sh --host root@coreelec [--target T] [--player FILE] [--dry-run]
#   scripts/install-kodi-addon.sh --storage DIR [--target T] [--player FILE]
#
# Options:
#   --host DEST                ssh destination (user@host)
#   --storage DIR              storage root (default /storage)
#   --target TARGET            coreelec21-ng-armv7hf (default) or coreelec22-no-aarch64
#   --player FILE              also install FILE as <storage>/amlogic-multistream
#   --allow-target-mismatch    install even if --player or the box does not match --target
#   --allow-non-arm32          older name for --allow-target-mismatch
#   --dry-run                  print the remote commands instead of running them

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/coreelec-targets.sh
source "$ROOT/scripts/coreelec-targets.sh"
ADDON_ID="plugin.program.amlogic-multistream"
MARKER="usr/share/amlogic-multistream/coreelec-target"

HOST=""
STORAGE="/storage"
TARGET="coreelec21-ng-armv7hf"
PLAYER=""
ALLOW_MISMATCH=0
DRY_RUN=0

die() {
  echo "error: $*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      shift
      [[ $# -gt 0 ]] || die "--host needs a destination"
      HOST=$1
      ;;
    --storage)
      shift
      [[ $# -gt 0 ]] || die "--storage needs a directory"
      STORAGE=$1
      ;;
    --target)
      shift
      [[ $# -gt 0 ]] || die "--target needs one of: $COREELEC_TARGETS"
      TARGET=$1
      ;;
    --player)
      shift
      [[ $# -gt 0 ]] || die "--player needs a file"
      PLAYER=$1
      ;;
    --allow-target-mismatch | --allow-non-arm32) ALLOW_MISMATCH=1 ;;
    --dry-run) DRY_RUN=1 ;;
    -h | --help)
      sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

coreelec_target "$TARGET" || exit 1
case "$CE_TRIPLE" in
  arm-*) EXPECTED_KIND="32-bit ARM" ;;
  aarch64-*) EXPECTED_KIND="64-bit ARM (aarch64)" ;;
  *) die "no ELF kind for $CE_TRIPLE" ;;
esac

if [[ "$DRY_RUN" -eq 1 && -z "$HOST" ]]; then
  die "--dry-run only applies with --host"
fi

# Refuse, or warn when --allow-target-mismatch was given.
mismatch() {
  if [[ "$ALLOW_MISMATCH" -eq 1 ]]; then
    echo "warning: $*; installing anyway" >&2
  else
    die "$*. Pass --allow-target-mismatch to install it anyway."
  fi
}

# Prints "32-bit ARM", "64-bit ARM (aarch64)", ..., or "not an ELF file".
elf_kind() {
  local -a b
  read -r -a b < <(od -An -v -t u1 -N 20 "$1" | tr -s ' \n' '  ')
  if [[ ${#b[@]} -lt 20 || "${b[0]}" != 127 || "${b[1]}" != 69 || "${b[2]}" != 76 || "${b[3]}" != 70 ]]; then
    echo "not an ELF file"
    return
  fi
  local bits=32
  [[ "${b[4]}" == 2 ]] && bits=64
  local machine
  if [[ "${b[5]}" == 2 ]]; then
    machine=$((b[18] * 256 + b[19]))
  else
    machine=$((b[19] * 256 + b[18]))
  fi
  case "$bits:$machine" in
    32:40) echo "32-bit ARM" ;;
    64:183) echo "64-bit ARM (aarch64)" ;;
    64:62) echo "64-bit x86-64" ;;
    32:3) echo "32-bit x86" ;;
    *) echo "$bits-bit ELF, e_machine $machine" ;;
  esac
}

# e_ident padding at offset 8 is "AI" + 0x02 on a type-2 AppImage.
is_type2_appimage() {
  [[ "$(od -An -c -j 8 -N 3 "$1" | tr -d ' ')" == 'AI002' ]]
}

# The squashfs payload of a type-2 AppImage starts where the runtime's ELF
# ends: e_shoff + e_shentsize * e_shnum (little-endian).
appimage_offset() {
  local f=$1 shoff ent num
  if [[ "$(od -An -t u1 -j 4 -N 1 "$f" | tr -d ' ')" == 2 ]]; then
    shoff=$(od -An -t u8 -j 40 -N 8 "$f" | tr -d ' ')
    ent=$(od -An -t u2 -j 58 -N 2 "$f" | tr -d ' ')
    num=$(od -An -t u2 -j 60 -N 2 "$f" | tr -d ' ')
  else
    shoff=$(od -An -t u4 -j 32 -N 4 "$f" | tr -d ' ')
    ent=$(od -An -t u2 -j 46 -N 2 "$f" | tr -d ' ')
    num=$(od -An -t u2 -j 48 -N 2 "$f" | tr -d ' ')
  fi
  echo $((shoff + ent * num))
}

if [[ -n "$PLAYER" ]]; then
  [[ -f "$PLAYER" ]] || die "player not found: $PLAYER"
  kind=$(elf_kind "$PLAYER")
  if [[ "$kind" != "$EXPECTED_KIND" ]]; then
    mismatch "$PLAYER is $kind. $CE_LINE needs a $EXPECTED_KIND binary, so this one will not run there"
  elif is_type2_appimage "$PLAYER"; then
    if command -v unsquashfs >/dev/null 2>&1; then
      marker=$(unsquashfs -o "$(appimage_offset "$PLAYER")" -cat "$PLAYER" "$MARKER" 2>/dev/null || true)
      built_for=$(sed -n 's/^target=//p' <<<"$marker")
      if [[ -z "$built_for" ]]; then
        mismatch "$PLAYER is not a CoreELEC device AppImage (no $MARKER). The x86_64 and Ubuntu aarch64 CI AppImages are host smoke builds; use amlogic-multistream-*-$TARGET.AppImage"
      elif [[ "$built_for" != "$TARGET" ]]; then
        mismatch "$PLAYER was built for $built_for, not $TARGET"
      else
        echo "player: $TARGET device AppImage (CoreELEC $(sed -n 's/^coreelec_release=//p' <<<"$marker"))"
      fi
    else
      echo "warning: unsquashfs (squashfs-tools) is not installed, so only the architecture of $PLAYER was checked" >&2
    fi
  fi
fi

work=$(mktemp -d /tmp/kodi-addon-install.XXXXXX)
trap 'rm -rf "$work"' EXIT
"$ROOT/scripts/package-kodi-addon.sh" --stage "$work" >/dev/null
[[ -f "$work/$ADDON_ID/addon.xml" ]] || die "staging failed"

addons_dir="$STORAGE/.kodi/addons"
player_dest="$STORAGE/amlogic-multistream"

if [[ -z "$HOST" ]]; then
  mkdir -p "$addons_dir"
  rm -rf "${addons_dir:?}/$ADDON_ID"
  cp -R "$work/$ADDON_ID" "$addons_dir/$ADDON_ID"
  echo "installed $addons_dir/$ADDON_ID"
  if [[ -n "$PLAYER" ]]; then
    cp "$PLAYER" "$player_dest.tmp"
    chmod 0755 "$player_dest.tmp"
    mv -f "$player_dest.tmp" "$player_dest"
    echo "installed $player_dest"
  fi
  exit 0
fi

q() { printf '%q' "$1"; }

# CoreELEC 21 sets COREELEC_ARCH, CoreELEC 22 sets DISTRO_ARCH. Expanded
# on the box.
# shellcheck disable=SC2016
remote_arch='. /etc/os-release && echo "${COREELEC_ARCH:-$DISTRO_ARCH}"'
# Tar over ssh: CoreELEC has tar and sh but the add-on tree must not depend
# on unzip or scp being present.
remote_addon="rm -rf $(q "$addons_dir/$ADDON_ID") && mkdir -p $(q "$addons_dir") && tar -C $(q "$addons_dir") -xf - && (kodi-send --action=UpdateLocalAddons >/dev/null 2>&1 || true)"
remote_player="cat > $(q "$player_dest.tmp") && chmod 0755 $(q "$player_dest.tmp") && mv -f $(q "$player_dest.tmp") $(q "$player_dest")"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "+ ssh $HOST '$remote_arch'   # expect $CE_DISTRO_ARCH ($CE_LINE)"
  echo "+ tar -C <stage> -cf - $ADDON_ID | ssh $HOST '$remote_addon'"
  if [[ -n "$PLAYER" ]]; then
    echo "+ ssh $HOST '$remote_player' < $PLAYER"
  fi
  exit 0
fi

box_arch=$(ssh "$HOST" "$remote_arch" | tr -d '\r')
if [[ "$box_arch" != "$CE_DISTRO_ARCH" ]]; then
  mismatch "$HOST runs CoreELEC ${box_arch:-<unknown>}, but --target $TARGET is for $CE_DISTRO_ARCH"
fi

tar -C "$work" -cf - "$ADDON_ID" | ssh "$HOST" "$remote_addon"
echo "installed $HOST:$addons_dir/$ADDON_ID"
if [[ -n "$PLAYER" ]]; then
  ssh "$HOST" "$remote_player" < "$PLAYER"
  echo "installed $HOST:$player_dest"
fi
echo "If Kodi lists the add-on as disabled, enable it under Add-ons > My add-ons."
