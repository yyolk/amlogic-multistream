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
# systemd EnvironmentFile under the add-on profile. Mode 0600. The password
# is not a --setenv, so `systemctl show` does not list it.
MQTT_ENV_FILENAME = "mqtt.env"
# Inside double quotes, systemd's EnvironmentFile parser drops a backslash
# only before these characters (systemd src/basic/escape.h SHELL_NEED_ESCAPE).
_SYSTEMD_ENV_ESCAPES = set('"\\`$')


class LaunchError(Exception):
    pass


def resolve_launch_script(addon_path, override=""):
    if override:
        return override
    return os.path.join(addon_path, BUNDLED_SCRIPT)


def quote_systemd_env(value):
    """Quote a value for a systemd EnvironmentFile assignment."""
    out = ['"']
    for ch in value:
        if ch in _SYSTEMD_ENV_ESCAPES:
            out.append("\\")
        out.append(ch)
    out.append('"')
    return "".join(out)


def mqtt_env_path(profile_dir):
    return os.path.join(profile_dir, MQTT_ENV_FILENAME)


def clear_mqtt_env_file(profile_dir):
    if not profile_dir:
        return
    try:
        os.remove(mqtt_env_path(profile_dir))
    except OSError:
        pass


def write_mqtt_env_file(profile_dir, password):
    """Write MQTT_PASSWORD for EnvironmentFile=. Returns the absolute path.

    The file mode is 0600. A password in --setenv would be visible in
    `systemctl show`; this file is not.
    """
    if not profile_dir:
        raise LaunchError(
            "add-on profile directory is required to store the MQTT password")
    os.makedirs(profile_dir, exist_ok=True)
    path = mqtt_env_path(profile_dir)
    tmp = path + ".tmp"
    payload = ("MQTT_PASSWORD=%s\n" % quote_systemd_env(password)).encode("utf-8")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.fchmod(fd, 0o600)
        os.write(fd, payload)
    finally:
        os.close(fd)
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)
    os.chmod(path, 0o600)
    return path


def build_command(script, player_bin, mqtt_host="", mqtt_port="",
                  mqtt_topic_prefix="", mqtt_username="", mqtt_password="",
                  profile_dir="", unit=UNIT_NAME):
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
        ("MQTT_USERNAME", mqtt_username),
    ):
        if value not in ("", None):
            argv.append("--setenv=%s=%s" % (name, value))
    if mqtt_password not in ("", None):
        # Password stays in the EnvironmentFile, never in --setenv.
        argv.append("--property=EnvironmentFile=%s" %
                    write_mqtt_env_file(profile_dir, mqtt_password))
    else:
        clear_mqtt_env_file(profile_dir)
    # Zip installs do not keep the executable bit, so run through the shell.
    argv += ["/bin/sh", script]
    return argv


def unit_active(unit=UNIT_NAME, run=subprocess.call):
    return run(["systemctl", "is-active", "--quiet", unit]) == 0


def launch(script, player_bin, mqtt_host="", mqtt_port="",
           mqtt_topic_prefix="", mqtt_username="", mqtt_password="",
           profile_dir="", unit=UNIT_NAME, call=subprocess.call):
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
                         mqtt_topic_prefix, mqtt_username, mqtt_password,
                         profile_dir, unit)
    rc = call(argv)
    if rc != 0:
        raise LaunchError("systemd-run exited %d" % rc)
    return argv
