# Survivors: `main` @ f1ea360

Filtered from [REVIEW2.md](REVIEW2.md). That file is the unfiltered pass.
This file is the list to act on. No production code was changed, and no
GitHub issues were filed.

`origin/main` is `f1ea360` (#6 and #7 are in it). Re-checked on Ubuntu 24.04
against that tree. FFmpeg 6.1.1 (libavformat 60.16). The launch tests used
the real `scripts/launch-coreelec.sh` with the binary path substituted, a
logging `systemctl` stub, and bash, dash, and busybox ash 1.36.1. Nothing
here ran on a GS-King X.

Severity: **major** = wrong behavior on a path this tree already ships.
Device checks that would change a conclusion are in "Needs a device", and
they are not blockers.

---

## Survivors

### 1. major — SIGTERM to the launch script kills the AppImage runtime and leaves the player running

- **Was:** T1, ranked a blocker there. A dropped SSH session does not hit
  this. SIGHUP to the process group (what a terminal hangup delivers) kills
  the payload and the script starts Kodi, under bash, dash, and busybox, for
  both the AppImage and a bare ELF.
- **Where:** `scripts/launch-coreelec.sh:25-27` (`kill "$player_pid"`),
  `:54-55` (`player_pid=$!`). README `:152` says a SIGTERM trap stops the
  player.
- **Evidence:** With `APPIMAGE_EXTRACT_AND_RUN=1`, `$!` is the type-2
  runtime. Current type2-runtime forks, `execv`s `AppRun` in the child, and
  `waitpid`s in the parent. It installs no signal handler and no
  `PDEATHSIG`. Reproduced with appimagetool's runtime from 2026-10-31 and a
  stand-in that blocks SIGINT/SIGTERM and `sigwait`s, same as
  `MqttController::start`:
  - SIGTERM to the shell, AppImage, bash dash and busybox: the payload's
    ppid becomes 1, and the stub has already logged `start kodi`.
  - The same SIGTERM to a bare ELF: the payload logs signal 15 and exits,
    then `start kodi` runs. No orphan.
  - Ctrl+C after the player is running delivers SIGINT to that `sigwait`
    even though the background child starts with SIGINT ignored, and Kodi
    is started. That path is fine. Do not "fix" it by killing the process
    group: the player is in the shell's group so Ctrl+C can reach it.
- **Fix:** Make the waited-for pid the player. Run the extracted `AppRun`
  directly so `$!` is that process and not the runtime. Signaling the
  runtime's children before the runtime is the other workable patch. A
  group kill is not: the player stays in the shell's group so Ctrl+C can
  reach it, and a group kill takes the script down too. After the signal,
  poll `kill -0` for a few seconds and only then `systemctl start kodi`.
  A killed runtime also leaves `/tmp/appimage_extracted_<hash>` behind
  (observed); that exit path removes it. One CI stand-in of this script
  is enough to lock the fix.

### 2. major — Ctrl+C before the player starts does not start Kodi on dash or busybox

- **Was:** T2. The post-fork half of that writeup is right and is not this
  bug (see survivor 1).
- **Where:** `scripts/launch-coreelec.sh:39-45`. INT is untrapped across
  `systemctl stop kodi` and `sleep 1`. `trap '' INT` is set only after the
  fork (`:56`).
- **Evidence:** SIGINT to the process group 0.4 s after start, during
  `sleep 1`, with a stub `systemctl` that returns immediately. Disposition
  was the default, as in a foreground terminal.
  - bash: the shell dies with status −2 and the EXIT trap logs `start kodi`.
  - dash and busybox ash 1.36.1: the shell dies with status −2, `stop kodi`
    has run, and `start kodi` does not.
  - The same split happens for the AppImage script and the bare-ELF script.
    The binary has not been forked yet.
  This is the stranded-Kodi outcome, on the `#!/bin/sh` path, in the window
  the script itself opens with `sleep 1`. A real `systemctl stop kodi` only
  makes that window longer. It was not timed here.
- **Fix:** `trap 'restore_kodi; exit 130' INT` before `systemctl stop kodi`.
  Keep `trap '' INT` after the fork. Survivor 1's post-fork Ctrl+C check
  still has to pass after that change.

### 3. major — An HLS master keeps downloading every variant for the whole playback

- **Was:** O1 / S3. The "always decodes 320x240" sentence is playlist order.
- **Where:** `src/ffmpeg_decoder.cpp:107-112`. Nothing sets
  `AVStream.discard`.
- **Evidence:** Two libx264 variants, 320x240 then 640x480, one-second
  segments, four segments each, served by `http.server`.
  - `--url` of the low-first master, `--frames 40`: decodes stream 0 at
    320x240, and GETs `lo/index{0,1,2,3}.ts` and `hi/index{0,1,2,3}.ts`.
  - High variant listed first: stream 0 is 640x480. Both variants are still
    fetched. `av_find_best_stream` returned the first entry both times.
  - Separate harness, same library: after `avformat_find_stream_info`, set
    `AVDISCARD_ALL` on every stream except 0. The next read logs
    `No longer receiving playlist 1` and does not GET `hi/index3.ts`.
    `lo/index0`–`2` and `hi/index0`–`2` are already fetched inside
    `open` / `avformat_find_stream_info`. Discard does not rewind that.
    FFmpeg 6.1 `hls.c` `playlist_needed` returns 0 once every stream in
    that playlist is discarded, which is what stops the later GETs.
- **Fix:** In `FfmpegDecoder::open`, after `avformat_find_stream_info`,
  keep the stream `av_find_best_stream` already returns and set
  `AVDISCARD_ALL` on the others. That is the whole fix. A bitrate picker
  waits until a tile size exists; stream 0 is the first variant, and that
  is an acceptable picture for this path. Add the two-variant fixture next
  to the existing HLS test and assert the later segment of the other
  variant is not requested.

### 4. major — Distro FFmpeg needs `libX11.so.6`, and the AppImage excludelist will not ship it

- **Was:** T3, ranked a blocker there. Whether CoreELEC has the library was
  not checked. The x86_64 smoke image runs on Ubuntu, which has libX11, so
  CI `--help` does not see this.
- **Where:** `scripts/build-appimage.sh:112-117` (linuxdeploy with its
  default excludelist). `README.md:146` says the remaining mismatch is the
  32-bit userspace.
- **Evidence:** `readelf -d` on Ubuntu 24.04 `libavutil.so.58` lists
  `NEEDED libX11.so.6` (also `libva-x11`, `libvdpau`, `libOpenCL`).
  linuxdeploy's excludelist
  (`probonopd/AppImages` `excludelist`, the file
  `generate-excludelist.sh` downloads) contains `libX11.so.6`, `libxcb`,
  `libfontconfig`, `libfreetype`, `libharfbuzz`, `libfribidi`, `libgmp`,
  `libz`, `libdrm`, `libstdc++`, and `libgcc_s`. A chroot of this player's
  ldd closure that bundled every library not on that list, and offered only
  glibc, libstdc++, libgcc_s, zlib, libdrm, and the loader, failed at
  process start:

  `error while loading shared libraries: libX11.so.6`

  The same closure still references other excluded sonames
  (`libxcb.so.1`, `libfontconfig.so.1`, `libfreetype.so.6`,
  `libharfbuzz.so.0`, `libfribidi.so.0`, `libgmp.so.10`, and others).
  The loader stops at libX11, so the later faults were not walked.
- **Fix:** Do this in the device build, not by rebuilding the Ubuntu smoke
  image. Link an FFmpeg configured without x11, vaapi, vdpau, and OpenCL,
  or link the CoreELEC sysroot's FFmpeg. Putting libX11 on the box is the
  wrong repair: the next excluded library is still missing. The armv7
  image is already the README TODO (`README.md:148`); this is a constraint
  on that build.

---

## Needs a device

Not blockers. Each one is a fact the tree states or implies that was not
measured on a GS-King X. Do them when the box is on the bench. Do not open
a second packaging track from the guesses.

- **`/bin/sh`.** `readlink -f /bin/sh`. Survivor 2 already fails on dash
  and on busybox ash 1.36.1. This only says whether this box hits it.
- **X11 and the other excluded libraries.** `ls` of `libX11.so.6`,
  `libxcb.so.1`, `libfontconfig.so.1` on the image. Survivor 4 is the
  loader result for a host that lacks them.
- **Kernel versus userspace.** `uname -m` next to `file` on `/bin/busybox`
  or the Kodi binary. `README.md:144` says that kernel will not load an
  aarch64 userspace binary. That was not checked. It decides an armv7
  sysroot against a self-contained aarch64 image. The armv7 TODO is
  already written.
- **`/tmp`.** Size and whether it is tmpfs, only if extract-and-run stays.
  The 158 MB figure was not remeasured. A SIGTERM'd runtime leaves
  `appimage_extracted_<hash>` behind; that much was observed on this host.

---

## Dropped

- **T1 blocker rank** — SIGHUP to the group and post-fork Ctrl+C both start Kodi; the player does not own the display yet. The leak is survivor 1, at major.
- **T2 `[HW]` rank** — reproduced on dash and busybox here. The on-box shell is a confirm, not the bug.
- **T3 blocker rank** — loader failure is real; "CoreELEC has no X11" was not checked (Needs a device).
- **O1 "always 320x240"** — high-variant-first decodes 640x480. Both variants are still downloaded, which is survivor 3.
- **O2 no device binary / arm64-kernel theory** — the armv7 image is already `README.md:148`. The kernel sentence is unverified (Needs a device).
- **L1 `start()` blocks** — `mqtt_controller.cpp:131` `sigwait` is the documented MQTT mode. `--url` returns at `app.cpp:294` and never enters it. Nothing in this tree decodes on that thread.
- **L2 `playing_` private** — same. No reader exists outside the controller.
- **L3 `restart` is log-only** — `stream_manager.cpp:21-24` says the decoder is a later milestone. The log line is the implementation.
- **L4 decoder interrupt / threads** — nothing calls `open` from another thread. `rw_timeout` is already 15 s.
- **L5 nine-tile present cost** — `--url` presents one tile. The 28.9 ms wall figure was not remeasured, and this tree does not draw nine tiles.
- **L6 viewport from the first frame** — correct for the one stream `--url` opens.
- **L7 bad layout tokens** — `setLayout` stores any non-empty token, and no caller passes that token to `compute` yet.
- **L8 frame lifetime across threads** — there is no present thread.
- **L9 stream cap** — the table holds strings. Cap it when a decoder is created per entry.
- **L10 pacing / live reconnect** — `--url` plays a finite fixture. No live loop exists.
- **T4 `--url` allows `file:` inside a remote playlist** — reproduced (an HTTP playlist whose segment is `file://…` decoded a frame). `setAllowLocalFile(true)` is the CLI path on purpose (`app.cpp:179-181`). MQTT `add` still accepts only `http://` and `https://`.
- **T5 TLS verify and demuxer whitelist** — FFmpeg defaults. No device CA bundle, and plain `http` is the POC transport.
- **N1 whitelist comment** — the `hls` token is unused by the demuxer and harmless.
- **N2 script exits 0** — `wait || true` hides the player status. Cosmetic next to survivors 1 and 2.
- **N3 uppercase schemes** — fail-closed, and documented.
- **N4 BT.601 PPM** — test output.
- **N5 unchecked `systemctl stop`** — no failing stop in hand.
- **N6 158 MB tmpfs** — size and tmpfs not remeasured (Needs a device). The leftover extract dir is part of survivor 1.
- **N7 one-second sleep in `restore_kodi`** — folded into the survivor 1 wait.
- **N8 URL logged in full** — operator's own `--url`.
- **N9 `docs/mqtt.md` still says "HLS demux" is absent** — doc drift. Edit it in the PR that touches the file.
- **C1 silent CI skip** — reproduced: with pkg-config hidden, `scripts/run-tests.sh` exits 0 and ctest reports 4/4. The workflow installs the dev packages before that script, and `build-appimage.sh` fails if the player does not link. The 4/4 result is the script's local fallback, which its header already describes.
- **C2 launch-script CI** — the stand-in is the regression check for survivors 1 and 2, not a separate defect.
- **C3 chroot smoke in CI** — that chroot is how survivor 4 was checked. Add it when the device build exists.
- **C4 `AMLOGIC_DRM_KMS` never compiled in CI** — the hook fails closed, and ctest does not need it.
- **C5 whitelist test only checks open failure** — the `file:` segment behavior is survivor 3's fixture, not its own task.
- **C6 no `MqttController` test** — there is no loop refactor to protect.

---

## Still closed

Re-checked only far enough to avoid reopening them.

- **S4 MQTT scheme allowlist.** `mqtt_commands.cpp:27-32` still accepts only lowercase `http://` and `https://` and rejects NUL. The `file:` segment result above is the `--url` allowance, survivor drop T4.
- **S5 software present.** `--url` calls `present` and prints a tile. Seen on the HLS runs (`presented 0`, software presenter, no `/dev/dri`).
- **S1 for a bare ELF, for SIGHUP, and for Ctrl+C after the fork.** Reproduced. The two holes that remain are survivors 1 and 2.
