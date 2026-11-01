#!/bin/sh
# requires GS-King X / CoreELEC
# RetroArch-style launch: stop Kodi, run the player, start Kodi.
#
# On-device only. CoreELEC Amlogic-ng for the GS-King X is a 32-bit ARM
# userspace (CoreELEC-Amlogic-ng.arm, g12b_s922x_beelink_gs_king_x). The CI
# aarch64 AppImage is not that binary. The base image has no fusermount.
#
# A type-2 AppImage is unpacked once and AppRun is executed directly. The
# type-2 runtime forks the player and does not forward SIGTERM, so waiting
# on the runtime leaves the player running after this script starts Kodi.
# A bare ELF is executed as itself. PLAYER_BIN overrides the on-device path
# (default /storage/amlogic-multistream). PLAYER_EXTRACT_DIR overrides the
# unpack directory (default $PLAYER_BIN.AppDir).
#
# /bin/sh on CoreELEC is busybox ash. This file is dash/busybox sh: traps
# are installed before Kodi is stopped, so SIGINT during `systemctl stop`
# or `sleep` still starts Kodi. After the player is running the shell
# ignores SIGINT and Ctrl+C belongs to the player.
#
# Broker defaults: localhost:1883, topic prefix amlogic-multistream
# (MQTT_HOST, MQTT_PORT, MQTT_TOPIC_PREFIX). This binary does not program
# DRM/KMS planes or Amlogic hardware decode.

player_bin="${PLAYER_BIN:-/storage/amlogic-multistream}"
player_pid=
restored=0

abs_path() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s\n' "$(pwd)/$1" ;;
    esac
}

# e_ident padding at offset 8 is "AI" + 0x02 on a type-2 AppImage.
is_type2_appimage() {
    magic_dir=$(mktemp -d /tmp/amlogic-appimage-magic.XXXXXX) || return 1
    printf 'AI\002' > "$magic_dir/expect"
    dd if="$1" bs=1 skip=8 count=3 of="$magic_dir/got" 2>/dev/null || true
    cmp -s "$magic_dir/expect" "$magic_dir/got"
    rc=$?
    rm -rf "$magic_dir"
    return "$rc"
}

# Alive and not a zombie. wait inside a trap that interrupted wait
# deadlocks on bash and dash, so shutdown polls /proc instead.
proc_running() {
    pid="$1"
    status="/proc/$pid/status"
    if [ ! -r "$status" ]; then
        if [ -d /proc ]; then
            return 1
        fi
        kill -0 "$pid" 2>/dev/null
        return $?
    fi
    state=$(sed -n 's/^State:[[:space:]]*//p' "$status" 2>/dev/null | head -n 1)
    case "$state" in
        "" | Z*) return 1 ;;
        *) return 0 ;;
    esac
}

ppid_of() {
    sed -n 's/^PPid:[[:space:]]*//p' "/proc/$1/status" 2>/dev/null | head -n 1
}

# Children first. An AppRun that has not exec'd must not be able to leave
# the real player reparented to init.
signal_tree() {
    root=$1
    sig=$2
    local dir pid pp
    for dir in /proc/[0-9]*; do
        pid=${dir#/proc/}
        case "$pid" in
            *[!0-9]*) continue ;;
        esac
        pp=$(ppid_of "$pid")
        if [ "$pp" = "$root" ]; then
            signal_tree "$pid" "$sig"
        fi
    done
    kill -s "$sig" "$root" 2>/dev/null || true
}

stop_player() {
    if [ -z "$player_pid" ]; then
        return 0
    fi
    signal_tree "$player_pid" TERM
    i=0
    while [ "$i" -lt 10 ]; do
        if ! proc_running "$player_pid"; then
            player_pid=
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    signal_tree "$player_pid" KILL
    sleep 1
    player_pid=
}

# Start Kodi once. If the player is still running, stop it first and do
# not start Kodi while that pid is alive.
restore_kodi() {
    if [ "$restored" = 1 ]; then
        return 0
    fi
    restored=1
    stop_player
    systemctl start kodi || true
}

# EXIT covers a normal return. dash and busybox do not run EXIT when an
# untrapped HUP or INT kills the shell, so those signals are trapped too.
# INT before the player exists is "never mind" and must start Kodi.
# TERM is a kill of the shell alone. After the fork, INT is ignored so
# Ctrl+C reaches the player (it blocks SIGINT and sigwaits it) and this
# script starts Kodi on the normal path when wait returns.
trap 'restore_kodi' EXIT
trap 'restore_kodi; exit 0' HUP TERM
trap 'restore_kodi; exit 130' INT

if [ ! -e "$player_bin" ]; then
    echo "launch-coreelec: player not found: $player_bin" >&2
    trap - EXIT HUP TERM INT
    exit 1
fi
player_bin=$(abs_path "$player_bin")

run_bin=$player_bin
if is_type2_appimage "$player_bin"; then
    extract_root="${PLAYER_EXTRACT_DIR:-$player_bin.AppDir}"
    case "$extract_root" in
        /*) ;;
        *) extract_root="$(pwd)/$extract_root" ;;
    esac
    stamp="$extract_root/.source-stamp"
    if stat -c '%s %Y' "$player_bin" >/dev/null 2>&1; then
        current=$(stat -c '%s %Y' "$player_bin")
    else
        current=$(cksum "$player_bin")
    fi
    apprun="$extract_root/squashfs-root/AppRun"
    saved=$(cat "$stamp" 2>/dev/null || true)
    if [ ! -x "$apprun" ] || [ "$saved" != "$current" ]; then
        rm -rf "$extract_root"
        mkdir -p "$extract_root"
        # APPIMAGE_EXTRACT_AND_RUN would fork AppRun and wait in the runtime,
        # which is the orphan this path exists to avoid. --appimage-extract
        # writes squashfs-root and exits.
        if ! (
            cd "$extract_root" &&
                env -u APPIMAGE_EXTRACT_AND_RUN "$player_bin" --appimage-extract
        ); then
            echo "launch-coreelec: failed to extract $player_bin" >&2
            exit 1
        fi
        printf '%s\n' "$current" > "$stamp"
    fi
    if [ ! -x "$apprun" ]; then
        echo "launch-coreelec: extracted AppImage has no AppRun in $extract_root" >&2
        exit 1
    fi
    run_bin=$apprun
fi

# Extract before Kodi is stopped so a bad AppImage does not take the display.
systemctl stop kodi

# Give the display a moment. SIGINT here still starts Kodi.
sleep 1

# Background so HUP/TERM run now instead of after the player returns.
# Job control is off, so the child stays in this process group and still
# sees Ctrl+C. Do not move it to a new process group: a group kill would
# also kill this script, and Ctrl+C would no longer reach the player.
"$run_bin" &
player_pid=$!
trap '' INT
wait "$player_pid" || true
player_pid=

trap - EXIT HUP TERM INT
restore_kodi
