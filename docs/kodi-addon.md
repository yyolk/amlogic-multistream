# Kodi add-on: plugin.program.amlogic-multistream

`kodi-addon/plugin.program.amlogic-multistream/` is a Kodi launcher for the player. It does the same stop-Kodi, run-player, start-Kodi sequence as RetroArch launchers by running `scripts/launch-coreelec.sh`. The add-on does not contain the player and does not change how the launch script handles signals.

Running it **requires GS-King X / CoreELEC**. CI checks the add-on on a host only (see [What CI checks](#what-ci-checks)).

## The player binary is not included

The add-on runs whatever is at `PLAYER_BIN`, which defaults to `/storage/amlogic-multistream`. The GS-King X runs CoreELEC **Amlogic-ng**, a 32-bit ARM userspace. The CI AppImages are x86_64 and aarch64, and neither runs on that device. This repository does not build a 32-bit ARM player yet (see the TODO in the README's AppImage section). Until it does, put your own 32-bit ARM (armv7 hard-float) build, or a 32-bit ARM type-2 AppImage, at `PLAYER_BIN`.

## How Launch works

1. Launch checks that the launch script and `PLAYER_BIN` exist. If either is missing, it shows an error and Kodi keeps running.
2. It starts the launch script as a transient systemd unit:

   ```sh
   systemd-run --unit=amlogic-multistream-launch --collect --quiet \
     --property=KillMode=mixed \
     --setenv=HOME=/storage --setenv=PLAYER_BIN=/storage/amlogic-multistream \
     [--setenv=MQTT_HOST=… --setenv=MQTT_PORT=… --setenv=MQTT_TOPIC_PREFIX=…] \
     /bin/sh /storage/.kodi/addons/plugin.program.amlogic-multistream/resources/bin/launch-coreelec.sh
   ```

3. The launch script stops Kodi, runs the player, waits for it to exit, and starts Kodi again.

The add-on uses `systemd-run` rather than starting the script directly. A process started by Kodi belongs to `kodi.service`, so the script's own `systemctl stop kodi` would kill it and Kodi would never be started again. The transient unit has its own cgroup and keeps running after Kodi stops. `KillMode=mixed` makes `systemctl stop amlogic-multistream-launch` send SIGTERM to the script only. The script's trap then stops the player and starts Kodi. The script runs through `/bin/sh` because a zip install does not keep the executable bit.

If the unit is already active, Launch shows an error and does nothing.

### Returning to Kodi

The player has no MQTT quit command (`stop` only clears the play flag). To stop the player, use SSH or a Home Assistant shell command:

```sh
systemctl stop amlogic-multistream-launch
```

The player's output is in the journal:

```sh
journalctl -u amlogic-multistream-launch
```

## Settings

| Setting | Environment variable | Default |
| --- | --- | --- |
| Player binary or AppImage | `PLAYER_BIN` | `/storage/amlogic-multistream` |
| Launch script override (expert level) | — | empty: the bundled `resources/bin/launch-coreelec.sh` |
| Broker host | `MQTT_HOST` | `localhost` (empty: not passed) |
| Broker port | `MQTT_PORT` | `1883` (0: not passed) |
| Topic prefix | `MQTT_TOPIC_PREFIX` | `amlogic-multistream` (empty: not passed) |

`PLAYER_EXTRACT_DIR` is not exposed. The launch script unpacks an AppImage to `$PLAYER_BIN.AppDir`.

## Build the zip

```sh
./scripts/package-kodi-addon.sh            # dist/plugin.program.amlogic-multistream-<version>.zip
```

Two files are copied in when the zip is built: `scripts/launch-coreelec.sh` becomes `resources/bin/launch-coreelec.sh`, and `packaging/amlogic-multistream.png` becomes `resources/icon.png`. The zip is reproducible: entries are sorted and use a fixed timestamp. CI uploads the zip from the x86_64 job as the `plugin.program.amlogic-multistream` artifact. GitHub wraps artifacts in another zip, so unzip the download once to get the add-on zip.

## Install on CoreELEC

These steps require GS-King X / CoreELEC.

### From a zip in Kodi

1. Copy the zip to the box, for example over the Samba share or with `scp plugin.program.amlogic-multistream-*.zip root@coreelec:/storage/`.
2. In Kodi, open **Settings → System → Add-ons** and turn on **Unknown sources**.
3. Open **Add-ons**, select the add-on browser (the open box icon), choose **Install from zip file**, and pick the zip.
4. Put the player at `/storage/amlogic-multistream` (see below).

### With the install helper (SSH)

```sh
./scripts/install-kodi-addon.sh --host root@coreelec --player /path/to/armv7/amlogic-multistream
```

The helper copies the add-on tree to `/storage/.kodi/addons/plugin.program.amlogic-multistream/` with tar over SSH, then runs `kodi-send --action=UpdateLocalAddons`. With `--player` it also installs the binary as `/storage/amlogic-multistream` with mode 0755. A player that is not a 32-bit ARM ELF is refused, which includes both CI AppImages, unless you pass `--allow-non-arm32`. `--dry-run` prints the remote commands without running them. `--storage DIR` with no `--host` installs into a local directory instead.

Kodi may list an add-on that was copied in by hand as disabled. If it does, enable it under **Add-ons → My add-ons → Program add-ons**, or restart Kodi with `systemctl restart kodi`.

### By hand

```sh
./scripts/package-kodi-addon.sh --stage /tmp/stage
scp -r /tmp/stage/plugin.program.amlogic-multistream root@coreelec:/storage/.kodi/addons/
scp /path/to/armv7/amlogic-multistream root@coreelec:/storage/amlogic-multistream
ssh root@coreelec 'chmod 0755 /storage/amlogic-multistream && kodi-send --action=UpdateLocalAddons'
```

Do not copy `kodi-addon/plugin.program.amlogic-multistream/` from the repository directly. That tree does not contain the launch script or the icon, which are added when the add-on is packaged or staged.

### Run it

Open **Add-ons → Program add-ons** (or **Game add-ons**), choose **amlogic-multistream**, then **Launch amlogic-multistream (stops Kodi)**. **Settings** opens the add-on settings. A favourite or keymap that runs `RunPlugin(plugin://plugin.program.amlogic-multistream/?action=launch)` launches the player in one step.

## What CI checks

`tests/kodi_addon_test.py` is registered with CTest as `kodi_addon` and runs in `scripts/run-tests.sh` on both CI runners. It does not need Kodi or a device. It checks:

- `addon.xml`: the id matches the directory, the `xbmc.python` 3.x import, the `pluginsource` entry for `default.py` that provides `executable`, and the metadata.
- `settings.xml` and `strings.po`: every label resolves to a string, and every setting and string id that `default.py` uses exists.
- The packaged zip: one top-level `plugin.program.amlogic-multistream/` directory, no bytecode, and a byte-identical executable copy of `scripts/launch-coreelec.sh`.
- `default.py` from the extracted zip, with mode bits dropped as a zip install may do, run against stub `xbmc*` modules and stand-ins for `systemd-run` and `systemctl`. **Launch** runs the bundled launch script through `/bin/sh` with `PLAYER_BIN` and the MQTT settings. The stand-in log reads `stop kodi` and then `start kodi`, and a stub player sees that environment. A missing player and an already-active unit both leave Kodi alone.
- `install-kodi-addon.sh`: a local `--storage` install, rejection of aarch64 and x86-64 ELF headers, and `--host --dry-run`.

These items still need a GS-King X running CoreELEC: Kodi loading and listing the add-on, the real transient unit surviving `systemctl stop kodi`, the display handover, the zip install from the Kodi UI, and the SSH install.
