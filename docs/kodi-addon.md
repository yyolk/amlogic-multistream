# Kodi add-on: plugin.program.amlogic-multistream

`kodi-addon/plugin.program.amlogic-multistream/` is a Kodi launcher for the player. It does the same stop-Kodi, run-player, start-Kodi sequence as RetroArch launchers by running `scripts/launch-coreelec.sh`. The add-on does not contain the player and does not change how the launch script handles signals.

Running it **requires GS-King X / CoreELEC**. CI checks the add-on on a host only (see [What CI checks](#what-ci-checks)).

## The player binary is not included

The add-on runs whatever is at `PLAYER_BIN`, which defaults to `/storage/amlogic-multistream`. Put the device AppImage for the box's CoreELEC line there (see "Device AppImages" in the README):

| CoreELEC on the box | `/etc/os-release` | Device AppImage |
| --- | --- | --- |
| CoreELEC 21 Amlogic-ng (32-bit ARM userspace) | `COREELEC_ARCH="Amlogic-ng.arm"` | `amlogic-multistream-<version>-coreelec21-ng-armv7hf.AppImage` |
| CoreELEC 22 Amlogic-no (64-bit ARM userspace) | `DISTRO_ARCH="Amlogic-no.aarch64"` | `amlogic-multistream-<version>-coreelec22-no-aarch64.AppImage` |

They come from the `amlogic-multistream-appimage-coreelec21-ng-armv7hf` and `amlogic-multistream-appimage-coreelec22-no-aarch64` artifacts of the **CoreELEC device AppImages** workflow, or from `scripts/build-coreelec-appimage.sh`. The x86_64 and aarch64 artifacts of the CI workflow are host smoke builds against Ubuntu's FFmpeg and do not run on either CoreELEC line.

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
# CoreELEC 21 Amlogic-ng (the default --target)
./scripts/install-kodi-addon.sh --host root@coreelec \
  --player amlogic-multistream-0.1.0-coreelec21-ng-armv7hf.AppImage

# CoreELEC 22 Amlogic-no
./scripts/install-kodi-addon.sh --host root@coreelec --target coreelec22-no-aarch64 \
  --player amlogic-multistream-0.1.0-coreelec22-no-aarch64.AppImage
```

The helper first reads the box's `/etc/os-release` and stops if its architecture is not the one `--target` is for. It then copies the add-on tree to `/storage/.kodi/addons/plugin.program.amlogic-multistream/` with tar over SSH and runs `kodi-send --action=UpdateLocalAddons`. With `--player` it also installs the binary as `/storage/amlogic-multistream` with mode 0755.

`--player` is checked before anything is copied:

- `--target coreelec21-ng-armv7hf` (default) needs a 32-bit ARM ELF or AppImage. aarch64 and x86-64 are refused.
- `--target coreelec22-no-aarch64` needs an aarch64 ELF or AppImage. 32-bit ARM and x86-64 are refused.
- A type-2 AppImage must contain `usr/share/amlogic-multistream/coreelec-target` naming the same target. The host smoke AppImages have no such file, so the Ubuntu aarch64 one is refused for CoreELEC 22 even though it is aarch64. Reading the file needs `unsquashfs` (squashfs-tools) on the workstation; without it only the architecture is checked and a warning is printed.

`--allow-target-mismatch` (older name: `--allow-non-arm32`) installs anyway with a warning. `--dry-run` prints the remote commands without running them. `--storage DIR` with no `--host` installs into a local directory instead.

Kodi may list an add-on that was copied in by hand as disabled. If it does, enable it under **Add-ons → My add-ons → Program add-ons**, or restart Kodi with `systemctl restart kodi`.

### By hand

```sh
./scripts/package-kodi-addon.sh --stage /tmp/stage
scp -r /tmp/stage/plugin.program.amlogic-multistream root@coreelec:/storage/.kodi/addons/
scp amlogic-multistream-0.1.0-coreelec21-ng-armv7hf.AppImage root@coreelec:/storage/amlogic-multistream   # or the coreelec22-no-aarch64 AppImage on CoreELEC 22
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
- `install-kodi-addon.sh`: a local `--storage` install; for the default CoreELEC 21 target, rejection of aarch64 and x86-64 ELF headers; for `--target coreelec22-no-aarch64`, acceptance of aarch64 and rejection of 32-bit ARM and x86-64; synthetic type-2 AppImages with and without the device target marker (the unmarked aarch64 one stands in for the Ubuntu smoke AppImage and is refused); and `--host --dry-run`, including the `/etc/os-release` check.

The **CoreELEC device AppImages** workflow also installs each device AppImage with `--target` into a local `--storage` tree.

These items still need a GS-King X running CoreELEC: Kodi loading and listing the add-on, the real transient unit surviving `systemctl stop kodi`, the display handover, the zip install from the Kodi UI, the SSH install with its `/etc/os-release` check, and the device AppImage itself running under the CoreELEC kernel.
