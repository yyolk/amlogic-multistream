#!/usr/bin/env bash
# Package kodi-addon/plugin.program.amlogic-multistream as an installable zip.
#
# Host-only. The zip is architecture independent and does not contain the
# player. Two files are added at package time so the repository keeps one
# copy of each:
#   resources/bin/launch-coreelec.sh  <- scripts/launch-coreelec.sh
#   resources/icon.png                <- packaging/amlogic-multistream.png
#
# The add-on runs the launch script on the device, which requires
# GS-King X / CoreELEC. Packaging does not.
#
# Usage:
#   scripts/package-kodi-addon.sh [--out DIR]     write DIR/<id>-<version>.zip
#   scripts/package-kodi-addon.sh --stage DIR     write the tree to DIR/<id>
#
# The zip path (or staged directory) is printed on the last line of stdout.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ADDON_ID="plugin.program.amlogic-multistream"
SRC="$ROOT/kodi-addon/$ADDON_ID"
OUT_DIR="${DIST_DIR:-$ROOT/dist}"
STAGE_ONLY=""

die() {
  echo "error: $*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      shift
      [[ $# -gt 0 ]] || die "--out needs a directory"
      OUT_DIR=$1
      ;;
    --stage)
      shift
      [[ $# -gt 0 ]] || die "--stage needs a directory"
      STAGE_ONLY=$1
      ;;
    -h | --help)
      sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

command -v python3 >/dev/null 2>&1 || die "required command 'python3' is not installed"
[[ -f "$SRC/addon.xml" ]] || die "missing $SRC/addon.xml"
[[ -f "$ROOT/scripts/launch-coreelec.sh" ]] || die "missing scripts/launch-coreelec.sh"
[[ -f "$ROOT/packaging/amlogic-multistream.png" ]] || die "missing packaging/amlogic-multistream.png"

version=$(python3 - "$SRC/addon.xml" "$ADDON_ID" <<'EOF'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
if root.get("id") != sys.argv[2]:
    sys.exit("addon.xml id %r does not match %r" % (root.get("id"), sys.argv[2]))
print(root.get("version"))
EOF
)
[[ -n "$version" ]] || die "addon.xml has no version"

stage_tree() {
  local dest=$1/$ADDON_ID
  rm -rf "$dest"
  mkdir -p "$dest"
  (cd "$SRC" && tar --exclude='__pycache__' --exclude='*.pyc' -cf - .) | (cd "$dest" && tar -xf -)
  mkdir -p "$dest/resources/bin"
  cp "$ROOT/scripts/launch-coreelec.sh" "$dest/resources/bin/launch-coreelec.sh"
  chmod 0755 "$dest/resources/bin/launch-coreelec.sh"
  cp "$ROOT/packaging/amlogic-multistream.png" "$dest/resources/icon.png"
  chmod 0644 "$dest/resources/icon.png"
}

if [[ -n "$STAGE_ONLY" ]]; then
  mkdir -p "$STAGE_ONLY"
  STAGE_ONLY=$(cd "$STAGE_ONLY" && pwd)
  stage_tree "$STAGE_ONLY"
  echo "$STAGE_ONLY/$ADDON_ID"
  exit 0
fi

mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
work=$(mktemp -d /tmp/kodi-addon-stage.XXXXXX)
trap 'rm -rf "$work"' EXIT
stage_tree "$work"

zip_path="$OUT_DIR/$ADDON_ID-$version.zip"
rm -f "$zip_path"
# Kodi expects one top-level directory named after the add-on id. Entries
# are sorted with a fixed timestamp so the zip is reproducible.
python3 - "$work" "$ADDON_ID" "$zip_path" <<'EOF'
import os
import stat
import sys
import zipfile

base, addon_id, out = sys.argv[1:4]
entries = []
for dirpath, dirnames, filenames in os.walk(os.path.join(base, addon_id)):
    dirnames.sort()
    entries.append(dirpath)
    for name in sorted(filenames):
        entries.append(os.path.join(dirpath, name))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
    for path in entries:
        arcname = os.path.relpath(path, base).replace(os.sep, "/")
        if os.path.isdir(path):
            info = zipfile.ZipInfo(arcname + "/", date_time=(1980, 1, 1, 0, 0, 0))
            info.external_attr = (stat.S_IFDIR | 0o755) << 16 | 0x10
            zf.writestr(info, b"")
            continue
        info = zipfile.ZipInfo(arcname, date_time=(1980, 1, 1, 0, 0, 0))
        mode = 0o755 if os.stat(path).st_mode & stat.S_IXUSR else 0o644
        info.external_attr = (stat.S_IFREG | mode) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        with open(path, "rb") as fh:
            zf.writestr(info, fh.read())
EOF

echo "$zip_path"
