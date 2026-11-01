#!/bin/sh
# requires GS-King X / CoreELEC
# RetroArch-style launch: stop Kodi, run the player, start Kodi.
#
# On-device only. CoreELEC Amlogic-ng for the GS-King X is a 32-bit ARM
# userspace (CoreELEC-Amlogic-ng.arm, g12b_s922x_beelink_gs_king_x). The CI
# aarch64 AppImage is not that binary. The base image has no fusermount, so
# APPIMAGE_EXTRACT_AND_RUN unpacks a type-2 AppImage instead of mounting it.
#
# Broker defaults: localhost:1883, topic prefix amlogic-multistream
# (MQTT_HOST, MQTT_PORT, MQTT_TOPIC_PREFIX). This binary does not program
# DRM/KMS planes or Amlogic hardware decode.

player_pid=
restored=0

# Start Kodi once. If the player is still running, SIGTERM it first (it
# sigwaits SIGTERM and exits). Do not call wait here: a wait inside a trap
# that interrupted wait deadlocks on bash and dash.
restore_kodi() {
    if [ "$restored" = 1 ]; then
        return 0
    fi
    restored=1
    if [ -n "$player_pid" ]; then
        kill "$player_pid" 2>/dev/null || true
        sleep 1
        player_pid=
    fi
    systemctl start kodi || true
}

# Bash runs EXIT when SIGHUP kills the shell. dash does not, but a HUP trap
# does run. TERM covers a kill of the shell alone. After HUP/TERM, exit so
# the script does not keep going and start the player again.
# Do not trap INT. Ctrl+C is the player's shutdown. Starting Kodi from an
# INT trap races the display. The shell ignores SIGINT only after the fork
# (below) so it can reach the normal start once the player has exited 0.
trap 'restore_kodi' EXIT
trap 'restore_kodi; exit 0' HUP TERM

systemctl stop kodi

# Give the display a moment
sleep 1

# Ignored by a plain ELF. Required for an AppImage on CoreELEC.
export APPIMAGE_EXTRACT_AND_RUN=1

# Background so HUP/TERM run now instead of after the player returns.
# Job control is off, so the child stays in this process group and still
# sees Ctrl+C. The shell sets SIGINT to ignore in an asynchronous child;
# the player blocks SIGINT and sigwaits it, which still receives the signal.
/storage/amlogic-multistream &
player_pid=$!
trap '' INT
wait "$player_pid" || true
player_pid=

trap - EXIT HUP TERM INT
restore_kodi
