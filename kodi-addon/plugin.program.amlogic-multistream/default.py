import os
import sys
from urllib.parse import parse_qs

import xbmc
import xbmcaddon
import xbmcgui
import xbmcplugin

ADDON_PATH = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ADDON_PATH, "resources", "lib"))

import amlogic_launcher  # noqa: E402

ADDON = xbmcaddon.Addon()
ADDON_NAME = ADDON.getAddonInfo("name")


def tr(string_id):
    return ADDON.getLocalizedString(string_id)


def log(message, level=xbmc.LOGINFO):
    xbmc.log("[%s] %s" % (ADDON.getAddonInfo("id"), message), level)


def plugin_url(action):
    return "%s?action=%s" % (sys.argv[0], action)


def list_root(handle):
    for action, label_id in (("launch", 30001), ("settings", 30002)):
        item = xbmcgui.ListItem(label=tr(label_id))
        xbmcplugin.addDirectoryItem(handle, plugin_url(action), item, isFolder=False)
    xbmcplugin.endOfDirectory(handle)


def addon_profile_dir():
    raw = ADDON.getAddonInfo("profile") or ""
    if isinstance(raw, bytes):
        raw = raw.decode("utf-8")
    if raw.startswith("special://"):
        import xbmcvfs
        raw = xbmcvfs.translatePath(raw)
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8")
    return raw.rstrip("/\\")


def run_launch():
    script = amlogic_launcher.resolve_launch_script(
        ADDON_PATH, ADDON.getSettingString("launch_script").strip())
    player_bin = (ADDON.getSettingString("player_bin").strip()
                  or amlogic_launcher.DEFAULT_PLAYER_BIN)
    mqtt_port = ADDON.getSettingInt("mqtt_port")
    # Do not strip the password. Leading or trailing spaces can be part of it.
    mqtt_password = ADDON.getSettingString("mqtt_password")
    try:
        argv = amlogic_launcher.launch(
            script,
            player_bin,
            mqtt_host=ADDON.getSettingString("mqtt_host").strip(),
            mqtt_port=str(mqtt_port) if mqtt_port > 0 else "",
            mqtt_topic_prefix=ADDON.getSettingString("mqtt_topic_prefix").strip(),
            mqtt_username=ADDON.getSettingString("mqtt_username").strip(),
            mqtt_password=mqtt_password,
            profile_dir=addon_profile_dir(),
        )
    except (amlogic_launcher.LaunchError, OSError) as exc:
        log("launch failed: %s" % exc, xbmc.LOGERROR)
        xbmcgui.Dialog().ok(ADDON_NAME, "%s\n%s" % (tr(30010), exc))
        return False
    log("started: %s" % " ".join(argv))
    xbmcgui.Dialog().notification(ADDON_NAME, tr(30011), xbmcgui.NOTIFICATION_INFO, 3000)
    return True


def main(argv):
    handle = int(argv[1]) if len(argv) > 1 and argv[1].lstrip("-").isdigit() else -1
    query = argv[2].lstrip("?") if len(argv) > 2 else ""
    action = parse_qs(query).get("action", [""])[0]
    if action == "launch":
        run_launch()
    elif action == "settings":
        ADDON.openSettings()
    else:
        list_root(handle)


if __name__ == "__main__":
    main(sys.argv)
