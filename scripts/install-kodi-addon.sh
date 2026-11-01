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
# --player must be a 32-bit ARM ELF (or a 32-bit ARM type-2 AppImage).
# CoreELEC Amlogic-ng on the GS-King X is a 32-bit ARM userspace; the
# x86_64 and aarch64 CI AppImages are refused unless --allow-non-arm32.
#
# Usage:
#   scripts/install-kodi-addon.sh --host root@coreelec [--player FILE] [--dry-run]
#   scripts/install-kodi-addon.sh --storage DIR [--player FILE]
#
# Options:
#   --host DEST          ssh destination (user@host)
#   --storage DIR        storage root (default /storage)
#   --player FILE        also install FILE as <storage>/amlogic-multistream
#   --allow-non-arm32    install --player even if it is not 32-bit ARM
#   --dry-run            print the remote commands instead of running them

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ADDON_ID="plugin.program.amlogic-multistream"

HOST=""
STORAGE="/storage"
PLAYER=""
ALLOW_NON_ARM32=0
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
    --player)
      shift
      [[ $# -gt 0 ]] || die "--player needs a file"
      PLAYER=$1
      ;;
    --allow-non-arm32) ALLOW_NON_ARM32=1 ;;
    --dry-run) DRY_RUN=1 ;;
    -h | --help)
      sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

if [[ "$DRY_RUN" -eq 1 && -z "$HOST" ]]; then
  die "--dry-run only applies with --host"
fi

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

if [[ -n "$PLAYER" ]]; then
  [[ -f "$PLAYER" ]] || die "player not found: $PLAYER"
  kind=$(elf_kind "$PLAYER")
  if [[ "$kind" != "32-bit ARM" ]]; then
    if [[ "$ALLOW_NON_ARM32" -eq 1 ]]; then
      echo "warning: $PLAYER is $kind, not 32-bit ARM; installing anyway" >&2
    else
      die "$PLAYER is $kind. CoreELEC Amlogic-ng on the GS-King X is a 32-bit ARM userspace, so this binary will not run there. Pass --allow-non-arm32 to install it anyway."
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

# Tar over ssh: CoreELEC has tar and sh but the add-on tree must not depend
# on unzip or scp being present.
remote_addon="rm -rf $(q "$addons_dir/$ADDON_ID") && mkdir -p $(q "$addons_dir") && tar -C $(q "$addons_dir") -xf - && (kodi-send --action=UpdateLocalAddons >/dev/null 2>&1 || true)"
remote_player="cat > $(q "$player_dest.tmp") && chmod 0755 $(q "$player_dest.tmp") && mv -f $(q "$player_dest.tmp") $(q "$player_dest")"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "+ tar -C <stage> -cf - $ADDON_ID | ssh $HOST '$remote_addon'"
  if [[ -n "$PLAYER" ]]; then
    echo "+ ssh $HOST '$remote_player' < $PLAYER"
  fi
  exit 0
fi

tar -C "$work" -cf - "$ADDON_ID" | ssh "$HOST" "$remote_addon"
echo "installed $HOST:$addons_dir/$ADDON_ID"
if [[ -n "$PLAYER" ]]; then
  ssh "$HOST" "$remote_player" < "$PLAYER"
  echo "installed $HOST:$player_dest"
fi
echo "If Kodi lists the add-on as disabled, enable it under Add-ons > My add-ons."
