"""Start scripts/launch-coreelec.sh outside of kodi.service.

No xbmc imports, so the command line can be checked on a host without Kodi.

The launch script runs `systemctl stop kodi`. A child of Kodi lives in the
kodi.service cgroup and would be killed by that stop, leaving Kodi down. The
script is therefore started as a transient systemd unit with systemd-run.
That unit has its own cgroup, so the script survives the stop, waits for the
player, and runs `systemctl start kodi`. KillMode=mixed sends SIGTERM to the
script alone on `systemctl stop UNIT`; the script's trap stops the player
and starts Kodi.
"""

import os
import subprocess

UNIT_NAME = "amlogic-multistream-launch"
DEFAULT_PLAYER_BIN = "/storage/amlogic-multistream"
BUNDLED_SCRIPT = os.path.join("resources", "bin", "launch-coreelec.sh")


class LaunchError(Exception):
    pass


def resolve_launch_script(addon_path, override=""):
    if override:
        return override
    return os.path.join(addon_path, BUNDLED_SCRIPT)


def build_command(script, player_bin, mqtt_host="", mqtt_port="",
                  mqtt_topic_prefix="", unit=UNIT_NAME):
    argv = [
        "systemd-run",
        "--unit=" + unit,
        "--collect",
        "--quiet",
        "--property=KillMode=mixed",
        "--setenv=HOME=/storage",
        "--setenv=PLAYER_BIN=" + player_bin,
    ]
    for name, value in (
        ("MQTT_HOST", mqtt_host),
        ("MQTT_PORT", mqtt_port),
        ("MQTT_TOPIC_PREFIX", mqtt_topic_prefix),
    ):
        if value not in ("", None):
            argv.append("--setenv=%s=%s" % (name, value))
    # Zip installs do not keep the executable bit, so run through the shell.
    argv += ["/bin/sh", script]
    return argv


def unit_active(unit=UNIT_NAME, run=subprocess.call):
    return run(["systemctl", "is-active", "--quiet", unit]) == 0


def launch(script, player_bin, mqtt_host="", mqtt_port="",
           mqtt_topic_prefix="", unit=UNIT_NAME, call=subprocess.call):
    """Start the launch script. Raises LaunchError if it cannot start.

    Returns once the transient unit exists. Kodi is stopped shortly after by
    the script itself, so the caller should not expect to keep running.
    """
    if not os.path.isfile(script):
        raise LaunchError("launch script not found: %s" % script)
    if not os.path.exists(player_bin):
        raise LaunchError("player not found: %s" % player_bin)
    if unit_active(unit, run=call):
        raise LaunchError("already running (%s)" % unit)
    # Without --collect support a failed earlier run keeps the unit name.
    call(["systemctl", "reset-failed", unit],
         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    argv = build_command(script, player_bin, mqtt_host, mqtt_port,
                         mqtt_topic_prefix, unit)
    rc = call(argv)
    if rc != 0:
        raise LaunchError("systemd-run exited %d" % rc)
    return argv
