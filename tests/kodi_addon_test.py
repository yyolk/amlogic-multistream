#!/usr/bin/env python3
"""Host checks for kodi-addon/plugin.program.amlogic-multistream.

Runs without Kodi or a device. xbmc, xbmcaddon, xbmcgui, and xbmcplugin are
stub modules; systemd-run and systemctl are stand-ins on PATH; the player is
a shell script. default.py runs from the packaged zip and drives the real
bundled launch-coreelec.sh through the stand-in systemd-run.

Device-only, not covered here (requires GS-King X / CoreELEC): Kodi loading
the add-on, the real systemd transient unit surviving `systemctl stop kodi`,
the display handover, and running a device player. The install helper's
--target checks run against synthetic ELF headers and AppImages.
"""

import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDON_ID = "plugin.program.amlogic-multistream"
ADDON_DIR = os.path.join(ROOT, "kodi-addon", ADDON_ID)
LAUNCH_SCRIPT = os.path.join(ROOT, "scripts", "launch-coreelec.sh")
PACKAGE = os.path.join(ROOT, "scripts", "package-kodi-addon.sh")
INSTALL = os.path.join(ROOT, "scripts", "install-kodi-addon.sh")
UNIT = "amlogic-multistream-launch"

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ADDON_DIR, "resources", "lib"))
import amlogic_launcher  # noqa: E402


def read(path, mode="r"):
    with open(path, mode) as fh:
        return fh.read()


def settings_defaults():
    root = ET.parse(os.path.join(ADDON_DIR, "resources", "settings.xml")).getroot()
    out = {}
    for setting in root.iter("setting"):
        default = setting.find("default")
        value = default.text if default is not None and default.text else ""
        if setting.get("type") == "integer":
            value = int(value or 0)
        out[setting.get("id")] = value
    return out


def string_ids():
    po = read(os.path.join(ADDON_DIR, "resources", "language",
                           "resource.language.en_gb", "strings.po"))
    return {int(m) for m in re.findall(r'^msgctxt "#(\d+)"$', po, re.M)}


def elf_header(ei_class, machine):
    head = bytearray(64)
    head[0:4] = b"\x7fELF"
    head[4] = ei_class
    head[5] = 1
    head[6] = 1
    head[16] = 2
    head[18:20] = machine.to_bytes(2, "little")
    return bytes(head)


def appimage_bytes(ei_class, machine, squashfs):
    """A type-2 AppImage: ELF header with the AI\\x02 magic, then squashfs.

    The runtime's ELF ends at e_shoff + e_shentsize * e_shnum; with no
    section headers that is e_shoff, which points just past the header.
    """
    head = bytearray(elf_header(ei_class, machine))
    head[8:11] = b"AI\x02"
    if ei_class == 2:
        head[40:48] = (64).to_bytes(8, "little")
        head[58:60] = (64).to_bytes(2, "little")
    else:
        head[32:36] = (64).to_bytes(4, "little")
        head[46:48] = (40).to_bytes(2, "little")
    return bytes(head) + squashfs


STUBS = {
    "xbmc.py": '''
import json, os
LOGDEBUG, LOGINFO, LOGWARNING, LOGERROR = 0, 1, 2, 3
def _event(kind, **kw):
    kw["kind"] = kind
    with open(os.environ["KODI_STUB_LOG"], "a") as fh:
        fh.write(json.dumps(kw) + "\\n")
def log(msg, level=LOGDEBUG):
    _event("log", msg=msg, level=level)
''',
    "xbmcaddon.py": '''
import json, os
from xbmc import _event
class Addon:
    def __init__(self, id=None):
        self._settings = json.loads(os.environ["KODI_STUB_SETTINGS"])
    def getAddonInfo(self, key):
        return {
            "id": "plugin.program.amlogic-multistream",
            "name": "amlogic-multistream",
            "profile": os.environ["KODI_STUB_PROFILE"],
        }[key]
    def getLocalizedString(self, string_id):
        _event("string", id=string_id)
        return "#%d" % string_id
    def getSettingString(self, key):
        return str(self._settings[key])
    def getSettingInt(self, key):
        return int(self._settings[key])
    def openSettings(self):
        _event("openSettings")
''',
    "xbmcgui.py": '''
from xbmc import _event
NOTIFICATION_INFO, NOTIFICATION_WARNING, NOTIFICATION_ERROR = "info", "warning", "error"
class ListItem:
    def __init__(self, label=""):
        self.label = label
class Dialog:
    def ok(self, heading, message):
        _event("dialog_ok", heading=heading, message=message)
        return True
    def notification(self, heading, message, icon="", time=0):
        _event("notification", heading=heading, message=message)
''',
    "xbmcplugin.py": '''
from xbmc import _event
def addDirectoryItem(handle, url, listitem, isFolder=False):
    _event("item", handle=handle, url=url, label=listitem.label, folder=isFolder)
    return True
def endOfDirectory(handle, succeeded=True):
    _event("endOfDirectory", handle=handle, succeeded=succeeded)
''',
}

# Executes the unit command in the foreground with only the --setenv
# variables and an EnvironmentFile=, like a transient unit that does not
# inherit the caller's env. The password file uses systemd's double-quoted
# EnvironmentFile syntax, which this shell can source.
FAKE_SYSTEMD_RUN = '''#!/bin/sh
for a in "$@"; do printf '%s\\n' "$a"; done > "{work}/systemd-run.argv"
envfile="{work}/unit.env"
: > "$envfile"
while [ $# -gt 0 ]; do
    case "$1" in
        --setenv=*)
            printf '%s\\n' "${{1#--setenv=}}" >> "$envfile"
            ;;
        --property=EnvironmentFile=*)
            file="${{1#--property=EnvironmentFile=}}"
            file="${{file#-}}"
            if [ -f "$file" ]; then
                cat "$file" >> "$envfile"
            fi
            ;;
        --*) ;;
        *) break ;;
    esac
    shift
done
exec env -i PATH="$PATH" /bin/sh -c 'set -a; . "$1"; shift; exec "$@"' sh "$envfile" "$@"
'''

FAKE_SYSTEMCTL = '''#!/bin/sh
printf '%s\\n' "$*" >> "{work}/systemctl.log"
if [ "$1" = "is-active" ]; then
    [ -e "{work}/unit-active" ] && exit 0
    exit 3
fi
exit 0
'''

FAKE_PLAYER = '''#!/bin/sh
{{
    echo "PLAYER_BIN=$PLAYER_BIN"
    echo "MQTT_HOST=$MQTT_HOST"
    echo "MQTT_PORT=$MQTT_PORT"
    echo "MQTT_TOPIC_PREFIX=$MQTT_TOPIC_PREFIX"
    echo "MQTT_USERNAME=$MQTT_USERNAME"
    echo "MQTT_PASSWORD=$MQTT_PASSWORD"
    echo "HOME=$HOME"
}} > "{work}/player.env"
'''


def write_exec(path, text):
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, 0o755)


def parse_systemd_env_file(text):
    """Parse a systemd EnvironmentFile the way load_env_file does.

    Mirrors the v255 env-file.c state machine, including double-quote
    escapes for \" \\\\ ` and $. No variable expansion: EnvironmentFile=
    does not expand $VAR.
    """
    whitespace = set(" \t\n\r")
    newline = set("\n\r")
    comments = set("#;")
    shell_need_escape = set('"\\`$')
    state = "PRE_KEY"
    key = []
    value = []
    have_value = False
    last_value_ws = None
    last_key_ws = None
    out = {}

    def push(chomp_value):
        parsed_key = "".join(key)
        if last_key_ws is not None:
            parsed_key = parsed_key[:last_key_ws]
        if not have_value:
            parsed_value = ""
        else:
            parsed_value = "".join(value)
            if chomp_value and last_value_ws is not None:
                parsed_value = parsed_value[:last_value_ws]
        if parsed_key:
            out[parsed_key] = parsed_value

    for ch in text:
        if state == "PRE_KEY":
            if ch in comments:
                state = "COMMENT"
            elif ch not in whitespace:
                state = "KEY"
                last_key_ws = None
                key.append(ch)
        elif state == "KEY":
            if ch in newline:
                state = "PRE_KEY"
                key = []
                last_key_ws = None
            elif ch == "=":
                state = "PRE_VALUE"
                last_value_ws = None
            else:
                if ch not in whitespace:
                    last_key_ws = None
                elif last_key_ws is None:
                    last_key_ws = len(key)
                key.append(ch)
        elif state == "PRE_VALUE":
            if ch in newline:
                state = "PRE_KEY"
                push(False)
                key = []
                value = []
                have_value = False
                last_key_ws = None
                last_value_ws = None
            elif ch == "'":
                state = "SINGLE"
                have_value = True
            elif ch == '"':
                state = "DOUBLE"
                have_value = True
            elif ch == "\\":
                state = "VALUE_ESCAPE"
                have_value = True
            elif ch not in whitespace:
                state = "VALUE"
                have_value = True
                value.append(ch)
        elif state == "VALUE":
            if ch in newline:
                state = "PRE_KEY"
                push(True)
                key = []
                value = []
                have_value = False
                last_key_ws = None
                last_value_ws = None
            elif ch == "\\":
                state = "VALUE_ESCAPE"
                last_value_ws = None
            else:
                if ch not in whitespace:
                    last_value_ws = None
                elif last_value_ws is None:
                    last_value_ws = len(value)
                value.append(ch)
        elif state == "VALUE_ESCAPE":
            state = "VALUE"
            if ch not in newline:
                value.append(ch)
                have_value = True
        elif state == "SINGLE":
            if ch == "'":
                state = "PRE_VALUE"
            else:
                value.append(ch)
                have_value = True
        elif state == "DOUBLE":
            if ch == '"':
                state = "PRE_VALUE"
            elif ch == "\\":
                state = "DOUBLE_ESCAPE"
            else:
                value.append(ch)
                have_value = True
        elif state == "DOUBLE_ESCAPE":
            state = "DOUBLE"
            if ch in shell_need_escape:
                value.append(ch)
                have_value = True
            elif ch not in newline:
                value.append("\\")
                value.append(ch)
                have_value = True
        elif state == "COMMENT":
            if ch == "\\":
                state = "COMMENT_ESCAPE"
            elif ch in newline:
                state = "PRE_KEY"
        elif state == "COMMENT_ESCAPE":
            state = "PRE_KEY" if ch in newline else "COMMENT"

    if state in ("PRE_VALUE", "VALUE", "VALUE_ESCAPE", "SINGLE", "DOUBLE", "DOUBLE_ESCAPE"):
        push(state == "VALUE")
    return out


def package(out_dir):
    res = subprocess.run([PACKAGE, "--out", out_dir], check=True,
                         stdout=subprocess.PIPE, universal_newlines=True)
    return res.stdout.strip().splitlines()[-1]


class AddonMetadataTest(unittest.TestCase):
    def setUp(self):
        self.root = ET.parse(os.path.join(ADDON_DIR, "addon.xml")).getroot()

    def test_addon_xml(self):
        self.assertEqual(self.root.tag, "addon")
        self.assertEqual(self.root.get("id"), ADDON_ID)
        self.assertEqual(os.path.basename(ADDON_DIR), ADDON_ID)
        self.assertRegex(self.root.get("version"), r"^\d+\.\d+\.\d+$")
        self.assertTrue(self.root.get("name"))
        self.assertTrue(self.root.get("provider-name"))

        imports = {i.get("addon"): i.get("version") for i in self.root.iter("import")}
        self.assertIn("xbmc.python", imports)
        self.assertTrue(imports["xbmc.python"].startswith("3."))

        points = {e.get("point"): e for e in self.root.iter("extension")}
        plugin = points["xbmc.python.pluginsource"]
        self.assertEqual(plugin.get("library"), "default.py")
        self.assertTrue(os.path.isfile(os.path.join(ADDON_DIR, "default.py")))
        provides = plugin.findtext("provides").split()
        self.assertIn("executable", provides)

        meta = points["xbmc.addon.metadata"]
        self.assertEqual(meta.findtext("platform"), "linux")
        for tag in ("summary", "description"):
            self.assertTrue(meta.find(tag).text.strip())
        # icon.png is copied in by package-kodi-addon.sh.
        self.assertEqual(meta.findtext("assets/icon"), "resources/icon.png")

    def test_settings_and_strings(self):
        sroot = ET.parse(os.path.join(ADDON_DIR, "resources", "settings.xml")).getroot()
        self.assertEqual(sroot.get("version"), "1")
        self.assertEqual(sroot.find("section").get("id"), ADDON_ID)
        ids = string_ids()
        for elem in sroot.iter():
            for attr in ("label", "help"):
                value = elem.get(attr)
                if value:
                    self.assertIn(int(value), ids, "%s %s=%s" % (elem.tag, attr, value))
            if elem.tag == "heading":
                self.assertIn(int(elem.text), ids)

        defaults = settings_defaults()
        self.assertEqual(defaults["player_bin"], amlogic_launcher.DEFAULT_PLAYER_BIN)
        self.assertEqual(defaults["launch_script"], "")

        source = read(os.path.join(ADDON_DIR, "default.py"))
        used = set(re.findall(r'getSetting\w*\("(\w+)"\)', source))
        self.assertTrue(used)
        self.assertLessEqual(used, set(defaults))
        used_ids = re.findall(r"\b(30\d{3})\b", source)
        self.assertTrue(used_ids)
        for sid in used_ids:
            self.assertIn(int(sid), ids)


class LauncherModuleTest(unittest.TestCase):
    def test_build_command(self):
        argv = amlogic_launcher.build_command("/a/launch.sh", "/storage/p")
        self.assertEqual(argv[0], "systemd-run")
        self.assertIn("--unit=" + UNIT, argv)
        self.assertIn("--collect", argv)
        self.assertIn("--property=KillMode=mixed", argv)
        self.assertIn("--setenv=PLAYER_BIN=/storage/p", argv)
        self.assertEqual(argv[-2:], ["/bin/sh", "/a/launch.sh"])
        self.assertFalse([a for a in argv if a.startswith("--setenv=MQTT")])

        argv = amlogic_launcher.build_command("/a/launch.sh", "/p", "broker", "1884", "pfx")
        for env in ("MQTT_HOST=broker", "MQTT_PORT=1884", "MQTT_TOPIC_PREFIX=pfx"):
            self.assertIn("--setenv=" + env, argv)
        self.assertEqual(argv[-2:], ["/bin/sh", "/a/launch.sh"])
        self.assertFalse([a for a in argv if "EnvironmentFile" in a or "MQTT_PASSWORD" in a])

    def test_build_command_credentials(self):
        password = 'pa$ss "q" `t`\\x'
        with tempfile.TemporaryDirectory() as profile:
            argv = amlogic_launcher.build_command(
                "/a/launch.sh", "/p", "broker", "1884", "pfx",
                "ha-user", password, profile)
            self.assertIn("--setenv=MQTT_USERNAME=ha-user", argv)
            env_path = os.path.join(profile, "mqtt.env")
            self.assertIn("--property=EnvironmentFile=" + env_path, argv)
            self.assertEqual(argv[-2:], ["/bin/sh", "/a/launch.sh"])
            joined = "\n".join(argv)
            self.assertNotIn("MQTT_PASSWORD", joined)
            self.assertNotIn(password, joined)
            self.assertEqual(os.stat(env_path).st_mode & 0o777, 0o600)
            self.assertEqual(
                read(env_path),
                'MQTT_PASSWORD="pa\\$ss \\"q\\" \\`t\\`\\\\x"\n')
            self.assertEqual(parse_systemd_env_file(read(env_path))["MQTT_PASSWORD"],
                             password)

            newline = "line1\nline2"
            argv = amlogic_launcher.build_command(
                "/a/launch.sh", "/p", mqtt_password=newline, profile_dir=profile)
            self.assertEqual(read(env_path), 'MQTT_PASSWORD="line1\nline2"\n')
            self.assertEqual(parse_systemd_env_file(read(env_path))["MQTT_PASSWORD"],
                             newline)
            self.assertNotIn(newline, "\n".join(argv))

            argv = amlogic_launcher.build_command(
                "/a/launch.sh", "/p", mqtt_username="only-user", profile_dir=profile)
            self.assertIn("--setenv=MQTT_USERNAME=only-user", argv)
            self.assertFalse(os.path.exists(env_path))
            self.assertFalse([a for a in argv if "EnvironmentFile" in a])

        with self.assertRaises(amlogic_launcher.LaunchError):
            amlogic_launcher.build_command(
                "/a/launch.sh", "/p", mqtt_password="secret", profile_dir="")

    def test_resolve_launch_script(self):
        self.assertEqual(amlogic_launcher.resolve_launch_script("/x"),
                         "/x/resources/bin/launch-coreelec.sh")
        self.assertEqual(amlogic_launcher.resolve_launch_script("/x", "/storage/l.sh"),
                         "/storage/l.sh")


class PackageTest(unittest.TestCase):
    def test_zip_contents(self):
        with tempfile.TemporaryDirectory() as out:
            zip_path = package(out)
            version = ET.parse(os.path.join(ADDON_DIR, "addon.xml")).getroot().get("version")
            self.assertEqual(os.path.basename(zip_path), "%s-%s.zip" % (ADDON_ID, version))
            with zipfile.ZipFile(zip_path) as zf:
                names = zf.namelist()
                for name in names:
                    self.assertTrue(name.startswith(ADDON_ID + "/"), name)
                    self.assertNotIn("__pycache__", name)
                    self.assertFalse(name.endswith(".pyc"), name)
                for rel in ("addon.xml", "default.py", "resources/settings.xml",
                            "resources/lib/amlogic_launcher.py", "resources/icon.png",
                            "resources/bin/launch-coreelec.sh",
                            "resources/language/resource.language.en_gb/strings.po"):
                    self.assertIn("%s/%s" % (ADDON_ID, rel), names)
                bundled = "%s/resources/bin/launch-coreelec.sh" % ADDON_ID
                self.assertEqual(zf.read(bundled), read(LAUNCH_SCRIPT, "rb"))
                mode = zf.getinfo(bundled).external_attr >> 16
                self.assertTrue(mode & stat.S_IXUSR)
                self.assertEqual(zf.read("%s/resources/icon.png" % ADDON_ID),
                                 read(os.path.join(ROOT, "packaging", "amlogic-multistream.png"), "rb"))
                self.assertEqual(zf.read("%s/addon.xml" % ADDON_ID),
                                 read(os.path.join(ADDON_DIR, "addon.xml"), "rb"))


class DefaultPyTest(unittest.TestCase):
    """default.py from the packaged zip against stub Kodi modules."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="kodi-addon-test.")
        zip_path = package(os.path.join(cls.tmp, "dist"))
        with zipfile.ZipFile(zip_path) as zf:
            zf.extractall(os.path.join(cls.tmp, "addons"))
        cls.addon = os.path.join(cls.tmp, "addons", ADDON_ID)
        # zipfile.extractall drops the mode bits, as a Kodi zip install may.
        cls.bundled = os.path.join(cls.addon, "resources", "bin", "launch-coreelec.sh")
        os.chmod(cls.bundled, 0o644)
        cls.stubs = os.path.join(cls.tmp, "stubs")
        os.mkdir(cls.stubs)
        for name, text in STUBS.items():
            with open(os.path.join(cls.stubs, name), "w") as fh:
                fh.write(text)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = tempfile.mkdtemp(prefix="case.", dir=self.tmp)
        self.bin = os.path.join(self.work, "bin")
        os.mkdir(self.bin)
        write_exec(os.path.join(self.bin, "systemd-run"), FAKE_SYSTEMD_RUN.format(work=self.work))
        write_exec(os.path.join(self.bin, "systemctl"), FAKE_SYSTEMCTL.format(work=self.work))
        self.player = os.path.join(self.work, "amlogic-multistream")
        write_exec(self.player, FAKE_PLAYER.format(work=self.work))
        self.settings = settings_defaults()
        self.settings["player_bin"] = self.player
        self.profile = os.path.join(self.work, "profile")

    def run_default(self, query):
        env = dict(os.environ)
        env.update({
            "PATH": self.bin + os.pathsep + env.get("PATH", ""),
            "PYTHONPATH": self.stubs,
            "PYTHONDONTWRITEBYTECODE": "1",
            "KODI_STUB_LOG": os.path.join(self.work, "events.jsonl"),
            "KODI_STUB_SETTINGS": json.dumps(self.settings),
            "KODI_STUB_PROFILE": self.profile,
        })
        for key in ("PLAYER_BIN", "MQTT_HOST", "MQTT_PORT", "MQTT_TOPIC_PREFIX",
                    "MQTT_USERNAME", "MQTT_PASSWORD"):
            env.pop(key, None)
        # Kodi runs the library as __main__ with argv [url, handle, query].
        # runpy would overwrite argv[0] with the file path.
        runner = ("import sys; path = sys.argv[1]; sys.argv = sys.argv[2:]; "
                  "src = open(path).read(); "
                  "exec(compile(src, path, 'exec'), {'__name__': '__main__', '__file__': path})")
        argv = [sys.executable, "-c", runner, os.path.join(self.addon, "default.py"),
                "plugin://%s/" % ADDON_ID, "7", query]
        res = subprocess.run(argv, env=env, cwd=self.work, timeout=60,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             universal_newlines=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        return self.events()

    def events(self):
        path = os.path.join(self.work, "events.jsonl")
        if not os.path.exists(path):
            return []
        with open(path) as fh:
            return [json.loads(line) for line in fh]

    def systemctl_log(self):
        path = os.path.join(self.work, "systemctl.log")
        return read(path).splitlines() if os.path.exists(path) else []

    def systemd_run_argv(self):
        path = os.path.join(self.work, "systemd-run.argv")
        return read(path).splitlines() if os.path.exists(path) else None

    def test_root_lists_launch_and_settings(self):
        events = self.run_default("")
        items = [e for e in events if e["kind"] == "item"]
        self.assertEqual([i["url"] for i in items],
                         ["plugin://%s/?action=launch" % ADDON_ID,
                          "plugin://%s/?action=settings" % ADDON_ID])
        self.assertTrue(all(i["handle"] == 7 and not i["folder"] for i in items))
        self.assertEqual(events[-1]["kind"], "endOfDirectory")
        self.assertIsNone(self.systemd_run_argv())

    def test_settings_action(self):
        events = self.run_default("?action=settings")
        self.assertIn("openSettings", [e["kind"] for e in events])
        self.assertIsNone(self.systemd_run_argv())

    def test_launch_runs_bundled_script(self):
        self.settings.update(mqtt_host="broker.lan", mqtt_port=1884,
                             mqtt_topic_prefix="living-room")
        events = self.run_default("?action=launch")

        argv = self.systemd_run_argv()
        self.assertIsNotNone(argv, "systemd-run was not called")
        self.assertEqual(argv[-2:], ["/bin/sh", self.bundled])
        self.assertIn("--unit=" + UNIT, argv)
        self.assertIn("--setenv=PLAYER_BIN=" + self.player, argv)

        self.assertEqual(self.systemctl_log(), [
            "is-active --quiet " + UNIT,
            "reset-failed " + UNIT,
            "stop kodi",
            "start kodi",
        ])
        player_env = read(os.path.join(self.work, "player.env")).splitlines()
        self.assertEqual(player_env, [
            "PLAYER_BIN=" + self.player,
            "MQTT_HOST=broker.lan",
            "MQTT_PORT=1884",
            "MQTT_TOPIC_PREFIX=living-room",
            "MQTT_USERNAME=",
            "MQTT_PASSWORD=",
            "HOME=/storage",
        ])
        self.assertFalse(os.path.exists(os.path.join(self.profile, "mqtt.env")))
        self.assertIn("notification", [e["kind"] for e in events])
        self.assertNotIn("dialog_ok", [e["kind"] for e in events])

    def test_launch_passes_mqtt_credentials(self):
        password = 's3cret $x "q"'
        self.settings.update(mqtt_host="broker.lan", mqtt_port=1884,
                             mqtt_topic_prefix="living-room",
                             mqtt_username="ha-user", mqtt_password=password)
        events = self.run_default("?action=launch")

        argv = self.systemd_run_argv()
        self.assertIsNotNone(argv)
        env_path = os.path.join(self.profile, "mqtt.env")
        self.assertIn("--setenv=MQTT_USERNAME=ha-user", argv)
        self.assertIn("--setenv=MQTT_HOST=broker.lan", argv)
        self.assertIn("--property=EnvironmentFile=" + env_path, argv)
        joined = "\n".join(argv)
        self.assertNotIn("MQTT_PASSWORD", joined)
        self.assertNotIn(password, joined)
        self.assertEqual(os.stat(env_path).st_mode & 0o777, 0o600)
        self.assertEqual(parse_systemd_env_file(read(env_path))["MQTT_PASSWORD"], password)
        player_env = read(os.path.join(self.work, "player.env")).splitlines()
        self.assertEqual(player_env, [
            "PLAYER_BIN=" + self.player,
            "MQTT_HOST=broker.lan",
            "MQTT_PORT=1884",
            "MQTT_TOPIC_PREFIX=living-room",
            "MQTT_USERNAME=ha-user",
            "MQTT_PASSWORD=" + password,
            "HOME=/storage",
        ])
        logged = json.dumps(events)
        self.assertNotIn(password, logged)
        self.assertNotIn("dialog_ok", [e["kind"] for e in events])

    def test_launch_script_override(self):
        override = os.path.join(self.work, "launch-coreelec.sh")
        shutil.copy(LAUNCH_SCRIPT, override)
        self.settings["launch_script"] = override
        self.run_default("?action=launch")
        self.assertEqual(self.systemd_run_argv()[-2:], ["/bin/sh", override])
        self.assertEqual(self.systemctl_log()[-2:], ["stop kodi", "start kodi"])

    def test_missing_player_keeps_kodi(self):
        self.settings["player_bin"] = os.path.join(self.work, "missing")
        events = self.run_default("?action=launch")
        self.assertIsNone(self.systemd_run_argv())
        self.assertEqual(self.systemctl_log(), [])
        dialogs = [e for e in events if e["kind"] == "dialog_ok"]
        self.assertEqual(len(dialogs), 1)
        self.assertIn("player not found", dialogs[0]["message"])

    def test_already_running(self):
        open(os.path.join(self.work, "unit-active"), "w").close()
        events = self.run_default("?action=launch")
        self.assertIsNone(self.systemd_run_argv())
        self.assertEqual(self.systemctl_log(), ["is-active --quiet " + UNIT])
        self.assertIn("dialog_ok", [e["kind"] for e in events])


class InstallHelperTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kodi-install-test.")
        self.storage = os.path.join(self.tmp, "storage")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def player(self, name, header):
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as fh:
            fh.write(header + b"\0" * 64)
        return path

    def install(self, *args):
        return subprocess.run([INSTALL] + list(args), stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, universal_newlines=True)

    def test_local_install_with_arm32_player(self):
        arm32 = self.player("arm32", elf_header(1, 40))
        res = self.install("--storage", self.storage, "--player", arm32)
        self.assertEqual(res.returncode, 0, res.stderr)
        addon = os.path.join(self.storage, ".kodi", "addons", ADDON_ID)
        self.assertTrue(os.path.isfile(os.path.join(addon, "addon.xml")))
        self.assertEqual(read(os.path.join(addon, "resources", "bin", "launch-coreelec.sh"), "rb"),
                         read(LAUNCH_SCRIPT, "rb"))
        dest = os.path.join(self.storage, "amlogic-multistream")
        self.assertEqual(read(dest, "rb"), read(arm32, "rb"))
        self.assertTrue(os.stat(dest).st_mode & stat.S_IXUSR)

    def test_refuses_64bit_player(self):
        for name, header in (("aarch64", elf_header(2, 183)), ("x86_64", elf_header(2, 62))):
            path = self.player(name, header)
            res = self.install("--storage", self.storage, "--player", path)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("32-bit ARM", res.stderr)
            self.assertFalse(os.path.exists(os.path.join(self.storage, "amlogic-multistream")))
        res = self.install("--storage", self.storage, "--player", path, "--allow-non-arm32")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("warning", res.stderr)

    def test_host_dry_run(self):
        arm32 = self.player("arm32", elf_header(1, 40))
        res = self.install("--host", "root@coreelec", "--player", arm32, "--dry-run")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("ssh root@coreelec", res.stdout)
        self.assertIn("/etc/os-release", res.stdout)
        self.assertIn("expect Amlogic-ng.arm", res.stdout)
        self.assertIn("tar -C /storage/.kodi/addons -xf -", res.stdout)
        self.assertIn("/storage/amlogic-multistream", res.stdout)

    def test_coreelec22_target_wants_aarch64(self):
        aarch64 = self.player("aarch64", elf_header(2, 183))
        res = self.install("--storage", self.storage, "--target", "coreelec22-no-aarch64",
                           "--player", aarch64)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(read(os.path.join(self.storage, "amlogic-multistream"), "rb"),
                         read(aarch64, "rb"))
        os.remove(os.path.join(self.storage, "amlogic-multistream"))
        for name, header in (("arm32", elf_header(1, 40)), ("x86_64", elf_header(2, 62))):
            path = self.player(name, header)
            res = self.install("--storage", self.storage, "--target", "coreelec22-no-aarch64",
                               "--player", path)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("64-bit ARM (aarch64)", res.stderr)
            self.assertFalse(os.path.exists(os.path.join(self.storage, "amlogic-multistream")))

        res = self.install("--host", "root@coreelec", "--target", "coreelec22-no-aarch64",
                           "--player", aarch64, "--dry-run")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("expect Amlogic-no.aarch64", res.stdout)

    def test_unknown_target(self):
        res = self.install("--storage", self.storage, "--target", "aarch64")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("coreelec22-no-aarch64", res.stderr)

    def appimage(self, name, ei_class, machine, target=None):
        tree = os.path.join(self.tmp, name + ".AppDir")
        os.makedirs(os.path.join(tree, "usr", "share", "amlogic-multistream"))
        write_exec(os.path.join(tree, "AppRun"), "#!/bin/sh\n")
        if target:
            with open(os.path.join(tree, "usr", "share", "amlogic-multistream",
                                   "coreelec-target"), "w") as fh:
                fh.write("target=%s\ncoreelec_release=test\n" % target)
        sqfs = os.path.join(self.tmp, name + ".squashfs")
        subprocess.run(["mksquashfs", tree, sqfs, "-noappend", "-quiet", "-all-root"],
                       check=True, stdout=subprocess.DEVNULL)
        path = os.path.join(self.tmp, name + ".AppImage")
        with open(path, "wb") as fh:
            fh.write(appimage_bytes(ei_class, machine, read(sqfs, "rb")))
        return path

    def test_device_appimage_marker(self):
        if not (shutil.which("mksquashfs") and shutil.which("unsquashfs")):
            if os.environ.get("CI") == "true":
                self.fail("CI needs squashfs-tools for the device AppImage marker check")
            self.skipTest("squashfs-tools is not installed")
        ce22 = ("--target", "coreelec22-no-aarch64")

        device = self.appimage("ce22", 2, 183, "coreelec22-no-aarch64")
        res = self.install("--storage", self.storage, *ce22, "--player", device)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("coreelec22-no-aarch64 device AppImage", res.stdout)

        # The ubuntu-24.04-arm CI AppImage is aarch64 too, but has no marker.
        smoke = self.appimage("smoke", 2, 183)
        os.remove(os.path.join(self.storage, "amlogic-multistream"))
        res = self.install("--storage", self.storage, *ce22, "--player", smoke)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("host smoke", res.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.storage, "amlogic-multistream")))
        res = self.install("--storage", self.storage, *ce22, "--player", smoke,
                           "--allow-target-mismatch")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("warning", res.stderr)

        ce21 = self.appimage("ce21", 1, 40, "coreelec21-ng-armv7hf")
        res = self.install("--storage", self.storage, "--player", ce21)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("coreelec21-ng-armv7hf device AppImage", res.stdout)

        wrong = self.appimage("wrong", 1, 40, "coreelec99-test")
        res = self.install("--storage", self.storage, "--player", wrong)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("built for coreelec99-test", res.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
