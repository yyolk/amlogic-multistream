# amlogic-multistream

Multi-stream HLS viewer for Amlogic S922X (GS-King X / CoreELEC).

## Goals
- Concurrent HLS streams with Amlogic hardware decoding (multi_vdec)
- Dynamic grid layouts
- MQTT (and optional HTTP) control for Home Assistant
- AppImage packaging
- RetroArch-style launch: stop Kodi → run app → restart Kodi

### Near-term milestone (product floor)
- **Minimum product floor:** at least **2×2 (4 concurrent hardware-accelerated streams)** on GS-King X / CoreELEC 22. Prefer tile-sized HLS **480/720**; allow **1080p** when Mpx/s and `codec_mm` admit. The UI may still target up to **9** on CE22 (EGL compose); 9 is aspirational, not the floor. CE21 stays limited (instance cap 2) — do not promise a nine-tile CE21 wall; 2×2 is the CE22-oriented floor.
- Plan order ([milestones](https://github.com/yyolk/amlogic-multistream/milestones)): **1-stream → 2 instances → 2×2 floor → 3×3 goal**.
  - [Milestone #3](https://github.com/yyolk/amlogic-multistream/milestone/3) CE22 stage-1: 1-stream amcodec spike — [#65](https://github.com/yyolk/amlogic-multistream/issues/65)
  - [Milestone #4](https://github.com/yyolk/amlogic-multistream/milestone/4) CE22 stage-2: dual instance — [#66](https://github.com/yyolk/amlogic-multistream/issues/66)
  - [Milestone #1](https://github.com/yyolk/amlogic-multistream/milestone/1) 2×2 hardware wall (4 streams) product floor — [#67](https://github.com/yyolk/amlogic-multistream/issues/67)
  - [Milestone #2](https://github.com/yyolk/amlogic-multistream/milestone/2) 3×3 hardware wall (9 streams) aspirational goal — [#68](https://github.com/yyolk/amlogic-multistream/issues/68)
  - [Milestone #5](https://github.com/yyolk/amlogic-multistream/milestone/5) CE21: software wall + optional 1–2 amvideo — [#69](https://github.com/yyolk/amlogic-multistream/issues/69)

## Status
Grid layout selection is implemented and unit-tested. MQTT command control keeps an in-memory stream table and, while `start` / `play` is set, software-decodes each visible `http://` or `https://` stream and presents it. `FfmpegDecoder` opens HLS, HTTP, or (on the `--url` path only) a file URL with libavformat/libavcodec, decodes on the CPU, and pulls 8-bit YUV420P frames through `DecoderBackend::readFrame`. `isHardware()` is false for this backend. An HLS master keeps the video stream `av_find_best_stream` returns — the first variant in playlist order, not the highest bitrate — and sets `AVDISCARD_ALL` on the others so later segments of the remaining variants are not downloaded.

`--url` presents each decoded frame through the `Renderer` interface. The default implementation is `SoftwareRenderer`: it letterboxes one YUV420P frame into the `1x1` cell from `LayoutEngine`, keeps an RGB buffer, and can write PPM files with `--render-dir`. It does not open `/dev/dri`. The MQTT path uses the same renderer for every cell in the grid. Plane pointers stay owned by the decoder until the next `readFrame()` or `close()`; the renderer copies them during `present()`. PPM dumps on the MQTT path are off unless `--render-dir` is set.

`DrmRenderer` is the DRM/KMS hook and **requires GS-King X / CoreELEC**. The default build does not open `/dev/dri`. `-DAMLOGIC_DRM_KMS=ON` compiles an open of `/dev/dri/card0` plus a resource query; atomic plane commit is not implemented, and `present()` does not scan out. Amlogic hwaccel stays a no-op in `prepareAmlogicHwaccel` (`src/ffmpeg_decoder.cpp`), also marked **requires GS-King X / CoreELEC**. It must not grow inside `FfmpegDecoder`; a hardware decoder should be its own `DecoderBackend`.

## Layouts

`LayoutEngine::suggestLayout(streamCount)` picks an equal-cell grid:

- 1 stream → `1x1` (a count below 1 also suggests `1x1`, and `compute` places no tiles)
- 2 → `1x2` side by side
- 3–4 → `2x2` (3 streams leave the bottom-right cell empty)
- 5–6 → `2x3` (5 streams leave the last cell empty)
- 7–9 → `3x3` (unused cells stay empty)
- above 9 → `3x3`, clamped; at most 9 tiles are placed

`LayoutEngine::compute(width, height, streamCount, layoutOverride)` returns one integer rectangle per stream, row-major (left to right, top to bottom). Leave `layoutOverride` empty to use the suggestion, or pass a name such as `"2x3"` to force a grid. Any `"RxC"` with rows and columns in 1..3 is accepted, including rotations the suggestion does not emit (`"2x1"`, `"3x2"`, …).

Tiles meet edge to edge and do not overlap. The full grid covers the viewport exactly; when a side does not divide evenly, neighboring cells differ by at most one pixel. Streams that do not fill the grid keep that cell size — empty cells stay empty and are not stretched over. Fitting a frame's aspect ratio inside a cell is left to the renderer.

## Build dependencies

Debian/Ubuntu:

```bash
sudo apt-get install cmake g++ pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
  libmosquitto-dev libdrm-dev
```

The `ffmpeg` CLI is not linked into the player. It is used by `ffmpeg_decoder_tests`, `stream_player_http`, `present_smoke_test`, and by the manual `--url` check below to synthesize a local media fixture. The player target also needs libmosquitto and libdrm. `--url` does not start MQTT. It presents through `SoftwareRenderer` unless `--renderer drm` is set, and the default build does not open DRM.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`layout_engine_tests`, `stream_manager_test`, `mqtt_smoke_test`, and `stream_player_test` do not need a broker. Layout, stream-table, and player-loop tests also do not need FFmpeg, libmosquitto, or libdrm. `mqtt_smoke_test` parses commands in-process and does not link libmosquitto. `stream_player_test` presents fake YUV frames through `SoftwareRenderer`.

`software_renderer_tests` does not need FFmpeg, libmosquitto, libdrm, or a GPU. It checks BT.601 conversion, a fake `Renderer` fed by a one-stream `LayoutEngine` rect, RGB checksums, letterboxing into one cell, and a PPM dump.

`ffmpeg_decoder_tests` is added when libavformat, libavcodec, libavutil, and libswscale are present. It does not need libmosquitto, libdrm, or a network. The test runs the `ffmpeg` CLI to write a tiny local MPEG-4 MP4 and, when libx264 is available, an HLS playlist, then opens each with `FfmpegDecoder` and checks `isHardware() == false` plus at least one YUV420P frame. With libx264 it also serves a two-variant master on `127.0.0.1` and fails if the last segment of the variant that was not selected is still requested. If the CLI is missing, the test exits 77 and ctest marks it skipped. If the dev libraries are missing, CMake skips that target and still builds the other tests. `AMS_REQUIRE_DECODER_TESTS=1` turns a skipped HLS section (no libx264) into a failure.

`stream_player_http` is added with the FFmpeg libraries. It serves one local MPEG-TS file on `127.0.0.1` (no broker) and checks that `StreamPlayer` plus `FfmpegDecoder` present at least one frame, that an unknown restart id does not reopen it, and that `stop` clears the viewport. `stream_player_stall` checks that a blackhole or trickle URL, and a live HLS edge when libx264 is available, does not freeze a healthy neighbor and that closing those decoders returns within 2s.

The `amlogic-multistream` player target is configured only when libavformat, libavcodec, libavutil, libswscale, libmosquitto, and libdrm are all present. `present_smoke_test` is registered with that target. It decodes a local fixture through `--url --render-dir` and checks that `--renderer drm` fails without a GS-King X. `-DAMLOGIC_DRM_KMS=ON` is off in CI.

## MQTT
Without `--url`, the process connects to a broker and software-decodes added streams until SIGINT, SIGTERM, or SIGHUP. If the broker is unreachable it retries with backoff from 1s to 30s instead of exiting. An authentication refusal ("not authorised" or "bad user name or password") exits non-zero after three attempts so the launch script can start Kodi. Software mode does not drive HDMI: Launch stops Kodi and shows no player picture until a DRM/KMS backend exists. Broker I/O runs on a background thread. Each stream is decoded on its own thread, and the main thread presents already-decoded frames, so one stalled read does not freeze the wall or the signal wait. `remove`, `stop`, `restart`, and shutdown abort a decoder and do not join it while `open` or `read` is still running. On SIGINT, SIGTERM, or SIGHUP the process publishes offline status before that wait and stops applying commands, so a `list` during the wait cannot republish it as online. If a worker is still blocked after 2s it stops waiting. It sends DISCONNECT (capped at 500ms) only when that publish was queued on a live broker, so the broker does not replace the retained stream table with the empty startup will, and then it flushes and exits without joining the MQTT network thread. That keeps the exit inside the launch script's poll when the broker is down, drops during the wait, or silently drops packets. The first broker lookup also runs on a helper, so a hostname whose nameserver never answers does not hold SIGTERM either: if the signal arrives before the MQTT loop starts, the process flushes and exits without joining that helper. Authentication give-up uses the same 2s bound and still exits non-zero. `start` / `play` opens an `FfmpegDecoder` per visible `http://` or `https://` stream and presents into `SoftwareRenderer` cells. `stop` retires those decoders and clears the viewport. `restart` with an id reopens that decoder; an unknown id fails and does not restart the others. `add` accepts at most 9 streams. A further add fails and does not publish status. `layout` accepts `auto` or `<rows>x<cols>` with each side in 1..3 and a lowercase `x`. `banana`, `4`, and `2X2` fail the command, leave the previous grid stored, and do not clear the wall. Defaults are `localhost:1883`, topic prefix `amlogic-multistream`, and a `1280x720` viewport (`MQTT_HOST`, `MQTT_PORT`, `MQTT_TOPIC_PREFIX`, `--viewport`, `--render-dir`). PPM output is off unless `--render-dir` is set. Command topics, JSON fields, and `mosquitto_pub` examples are in [docs/mqtt.md](docs/mqtt.md).

This build does not open DRM/KMS devices or Amlogic decoders. `scripts/launch-coreelec.sh` **requires GS-King X / CoreELEC**. It stops Kodi, runs the player, and starts Kodi again. SIGHUP, SIGTERM, and EXIT stop the player and start Kodi if the SSH session drops or the script is killed. A type-2 AppImage is unpacked once and `AppRun` is what the trap signals, so the AppImage runtime cannot keep the player alive. Ctrl+C before the player starts also starts Kodi, including under dash and busybox ash. After the player is running, Ctrl+C belongs to the player.

## Manual player check

`--url` demuxes and software-decodes one input, presents each frame with `SoftwareRenderer`, prints a line per frame, and exits. It does not start MQTT and does not open `/dev/dri`. A generated local playlist needs no network. A live HLS URL does. `--render-dir DIR` also writes `frame-NNNNNN.ppm` (P6 RGB of the viewport).

```bash
mkdir -p /tmp/hls-poc
ffmpeg -y -f lavfi -i testsrc=size=320x240:rate=10:duration=2 \
  -c:v libx264 -pix_fmt yuv420p -hls_time 1 -hls_list_size 0 \
  /tmp/hls-poc/index.m3u8

./build/amlogic-multistream --url /tmp/hls-poc/index.m3u8 --frames 5 \
  --render-dir /tmp/hls-poc/frames
```

Expect exit status 0, `isHardware=0`, `Presenter software`, lines like `frame 0 320x240 fmt=yuv420p`, and `presented 0 ... rgbsum=`. With `--render-dir`, also expect `/tmp/hls-poc/frames/frame-000000.ppm`. Exit status 1 means open, decode, or present failed. Exit status 2 means bad arguments (`--help` with `--url` prints decode usage and exits 0; `--help` alone prints MQTT usage).

`--renderer drm` selects `DrmRenderer`. It **requires GS-King X / CoreELEC**. Without `-DAMLOGIC_DRM_KMS=ON` the process exits 1 and does not open `/dev/dri`.

Public HLS (network required), for example:

```bash
./build/amlogic-multistream --url https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8 --frames 5
```

Network opens use a 15s socket read timeout (`rw_timeout`). On the MQTT wall each stream decodes on its own thread, and `abort()` unblocks a stuck open or read on stop and shutdown. CI that cannot reach that host should use the local fixture or `ffmpeg_decoder_tests` instead.

Pass `-DCMAKE_CXX_COMPILER=g++` when `c++` on the PATH is not g++.

## Tests

`layout_engine_tests` is the layout unit-test binary. CTest registers it as `layout_engine`. The same `ctest` run also executes `stream_manager_test`, `mqtt_smoke_test`, `software_renderer_tests`, `stream_player_test`, `scripts/test-ctest-require.sh` (the CI guard that rejects a run where `ffmpeg_decoder`, `present_smoke`, or `stream_player_http` was omitted or skipped), `scripts/test-launch-coreelec.sh` (a stand-in for the CoreELEC launch script: bash, dash, and busybox ash when busybox is installed), and `tests/kodi_addon_test.py` (the Kodi add-on against stub Kodi modules, when `python3` is found). When the FFmpeg libraries are present it also runs `ffmpeg_decoder_tests`, `stream_player_http`, and `stream_player_stall`, and when the player dependencies are present it runs `present_smoke_test`, `mqtt_reconnect`, and `mqtt_exit_test`.

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
```

`./scripts/run-tests.sh` is those three steps. It fails if `layout_engine_tests` is missing from the CTest list, or if any test fails. GitHub Actions sets `AMS_REQUIRE_DECODER_TESTS=1` and runs the script on both the x86_64 and aarch64 runners before packaging. With that variable, the script also fails when `ffmpeg_decoder`, `present_smoke`, or `stream_player_http` is absent from the CTest list or finishes as skipped. Those runners install the dev packages and the `ffmpeg` CLI, then execute the tests natively. A machine without those packages can leave the variable unset and still run whatever CMake registered.

## Codespaces

`.devcontainer/` is an Ubuntu container with g++, CMake, pkg-config, the FFmpeg development libraries, the `ffmpeg` CLI, libmosquitto, and libdrm. A codespace is the host smoke environment (usually x86_64).

1. On GitHub, choose **Code → Codespaces → Create codespace** on `main` or on a pull request.
2. After the container is created, `postCreateCommand` runs `scripts/run-tests.sh`. The creation log includes the CTest result, and `build/` is ready.
3. Run the tests again with:

```bash
ctest --test-dir build --output-on-failure
```

Rebuild after source changes with `cmake --build build`. The CMake Tools extension uses this `build/` directory, so its test runner calls the same CTest list.

`./scripts/build-appimage.sh` in the codespace packages the codespace's own architecture. `scripts/launch-coreelec.sh` **requires GS-King X / CoreELEC** and is unused here.

## AppImage

There are two kinds of AppImage. The **host smoke** AppImages run on a normal x86_64 or aarch64 Linux machine and bundle Ubuntu's FFmpeg. The **device** AppImages are cross-built against a CoreELEC sysroot for the GS-King X. Only a device AppImage runs on the box.

| Artifact | Built by | Runs on |
| --- | --- | --- |
| `amlogic-multistream-appimage-x86_64` | `ci.yml`, `ubuntu-24.04` | x86_64 host and Codespaces (smoke build) |
| `amlogic-multistream-appimage-aarch64` | `ci.yml`, `ubuntu-24.04-arm` | aarch64 Ubuntu-like host (smoke build). **Not** a CoreELEC binary |
| `amlogic-multistream-appimage-coreelec21-ng-armv7hf` | `coreelec-appimage.yml` | GS-King X on **CoreELEC 21 Amlogic-ng** (32-bit ARM) |
| `amlogic-multistream-appimage-coreelec22-no-aarch64` | `coreelec-appimage.yml` | GS-King X on **CoreELEC 22 Amlogic-no** (64-bit ARM) |

Both workflows run on pull requests and on pushes to `main`. A push to any other branch is built when its pull request is opened. Download an artifact from the workflow run's Artifacts list. GitHub wraps it in a zip, so unzip it once to get the `.AppImage`.

### Host smoke AppImages

`ci.yml` is a two-runner matrix. Each runner installs the dev-container packages, runs `scripts/run-tests.sh`, then runs `scripts/build-appimage.sh`. linuxdeploy bundles the shared libraries it finds. The build is not statically linked. The file inside each artifact is `amlogic-multistream-<version>-<arch>.AppImage`, where `<arch>` is the runner architecture.

On your own machine the same script follows `uname -m`:

```bash
./scripts/build-appimage.sh
```

That needs the build packages above plus `curl`, `patchelf`, `binutils`, and `file`. An x86_64 host produces the smoke image. An aarch64 host produces another aarch64 smoke image. Neither is a GS-King X binary: the aarch64 one is linked against Ubuntu's FFmpeg (`libavcodec.so.60` plus the X11 stack below), which CoreELEC 22 does not ship.

Ubuntu's FFmpeg is linked with libX11, VAAPI, VDPAU, and OpenCL. linuxdeploy's excludelist would omit `libX11.so.6` and the libraries behind it (`libxcb`, fontconfig, freetype, harfbuzz, and the rest of that closure), so `--help` fails on a machine that does not have them. `scripts/build-appimage.sh` copies those libraries into the AppDir. The script then runs `--help` in a chroot that only offers glibc, libstdc++, libgcc_s, zlib, and libdrm.

### Device AppImages (GS-King X on CoreELEC)

The GS-King X (S922X, device tree `g12b_s922x_beelink_gs_king_x`) is supported by two CoreELEC lines with different userspaces. Each CoreELEC image only has the dynamic loader and libraries for its own userspace, so an aarch64 binary does not run on CoreELEC 21 and a 32-bit ARM binary does not run on CoreELEC 22. Pick the artifact for the line the box runs. `/etc/os-release` on the box says which: `COREELEC_ARCH="Amlogic-ng.arm"` on CoreELEC 21, `DISTRO_ARCH="Amlogic-no.aarch64"` on CoreELEC 22.

| Target | CoreELEC line | Userspace | Sysroot (official update archive) | Links CoreELEC's |
| --- | --- | --- | --- | --- |
| `coreelec21-ng-armv7hf` | CoreELEC 21 (Omega) Amlogic-ng | 32-bit ARM, armv7 hard-float | `CoreELEC-Amlogic-ng.arm-21.3-Omega.tar` | glibc 2.38, FFmpeg 6.0.1 (`libavcodec.so.60`), libdrm, libstdc++ |
| `coreelec22-no-aarch64` | CoreELEC 22 (Piers) Amlogic-no | 64-bit ARM | `CoreELEC-Amlogic-no.aarch64-22.0-Piers_rc1.tar` (pre-release) | glibc 2.44, FFmpeg 9.0.2 (`libavcodec.so.63`), libdrm, libstdc++ |

The file inside each artifact is `amlogic-multistream-<version>-<target>.AppImage`.

How a device AppImage is built (`scripts/build-coreelec-appimage.sh TARGET`, on an x86_64 host):

1. `scripts/fetch-coreelec-sysroot.sh TARGET` downloads the pinned CoreELEC update archive from [github.com/CoreELEC/CoreELEC/releases](https://github.com/CoreELEC/CoreELEC/releases), checks its sha256, and unpacks the shared libraries in `/usr/lib` plus `/etc/os-release` from its `target/SYSTEM` squashfs into `.cache/coreelec/TARGET/sysroot` (79 MB for CoreELEC 21, 120 MB for CoreELEC 22). No root is needed. The archive is deleted afterwards unless `--keep-archive` is given. `--check` only reports whether the sysroot is there and exits 1 with the fetch command when it is not.
2. The CoreELEC image has no headers, so FFmpeg's headers are generated from the upstream release that matches the image (`make install-headers`; nothing is compiled), and libdrm's headers come from libdrm 2.4.124. The script fails if the FFmpeg header major versions differ from the image's sonames.
3. CoreELEC does not ship libmosquitto. libmosquitto 2.0.22 is cross-built as a client library without TLS (the player does not use TLS).
4. The player is cross-compiled with Ubuntu's `g++-arm-linux-gnueabihf` or `g++-aarch64-linux-gnu` and linked against CoreELEC's own `libavformat`, `libavcodec`, `libavutil`, `libswscale`, and `libdrm`. glibc and libstdc++ symbols are linked through the cross toolchain (GCC 13, glibc 2.39) and then checked against CoreELEC's glibc and libstdc++, which are what load on the box. Only `libmosquitto.so.1` is bundled. Nothing from Ubuntu's FFmpeg, X11, VAAPI, VDPAU, or OpenCL stack is linked.
5. appimagetool packs the AppDir with the pinned armhf or aarch64 type-2 runtime. `usr/share/amlogic-multistream/coreelec-target` inside the AppImage records the target and CoreELEC release; `install-kodi-addon.sh` reads it.
6. `scripts/verify-coreelec-appimage.sh TARGET APPIMAGE` checks the result against the sysroot (below).

Every download (CoreELEC archive, FFmpeg, libdrm, libmosquitto, appimagetool, runtime) is pinned by version and sha256 in `scripts/coreelec-targets.sh`. Moving to another CoreELEC release means changing its archive digest there, and the FFmpeg version too if the image's `libavcodec` soname changed. CoreELEC 22 is pinned to `22.0-Piers_rc1` because no final 22.0 exists yet. A device AppImage relies on the box having the same FFmpeg sonames as the sysroot it was built against. A CoreELEC update that bumps FFmpeg's major version needs a rebuild.

Locally (x86_64 Ubuntu 24.04):

```bash
sudo apt-get install cmake make pkg-config curl xz-utils patchelf file squashfs-tools \
  qemu-user-static ffmpeg mosquitto \
  g++-arm-linux-gnueabihf g++-aarch64-linux-gnu
./scripts/build-coreelec-appimage.sh coreelec21-ng-armv7hf   # dist/coreelec21-ng-armv7hf/*.AppImage
./scripts/build-coreelec-appimage.sh coreelec22-no-aarch64   # dist/coreelec22-no-aarch64/*.AppImage
```

`--no-fetch` fails instead of downloading when the sysroot is missing. `COREELEC_CACHE` moves `.cache/coreelec`.

What CI checks for each device AppImage, under qemu-user with the CoreELEC sysroot as the library root, so the dynamic loader, glibc, libstdc++, and FFmpeg are CoreELEC's own files:

- The runtime and every ELF inside are the target architecture (`file` reports `ELF 32-bit LSB ... ARM, EABI5` or `ELF 64-bit LSB ... ARM aarch64`). The player's program interpreter is CoreELEC's (`/lib/ld-linux-armhf.so.3` or `/lib/ld-linux-aarch64.so.1`).
- Every `NEEDED` library is either bundled (`libmosquitto.so.1`) or present in the CoreELEC image. None is X11, VAAPI, VDPAU, OpenCL, or desktop GL.
- Every versioned symbol the player and libmosquitto use is defined by the CoreELEC library that provides it. Both builds need at most `GLIBC_2.38`.
- `--appimage-extract` through the bundled runtime, as `scripts/launch-coreelec.sh` does on the box.
- `AppRun --help`, `AppRun --url` software-decoding a local H.264 HLS fixture through CoreELEC's FFmpeg, and an MQTT connect, subscribe, and SIGTERM shutdown against a local mosquitto through the bundled libmosquitto.
- `install-kodi-addon.sh --target TARGET` accepts the artifact.

**Still requires a GS-King X / CoreELEC.** None of these has been run on a box yet:

- The AppImage runtime under the CoreELEC kernel, `launch-coreelec.sh` with that runtime, and the Kodi stop/start handover.
- Software decode speed on the S922X CPU, and MQTT against a real broker.
- DRM/KMS output and Amlogic hardware decode. These are not implemented: the device AppImages are built with the default `AMLOGIC_DRM_KMS=OFF`, do not open `/dev/dri`, and decode on the CPU (`isHardware=0`), exactly like the host build.

## Launch on CoreELEC

`scripts/launch-coreelec.sh` **requires GS-King X / CoreELEC**. It stops Kodi, runs `/storage/amlogic-multistream`, and starts Kodi again. Software MQTT mode does not open DRM/KMS, so this launch stops Kodi and does not put the player's video on screen. What HDMI shows has not been checked on a GS-King X. If the SSH session drops (SIGHUP) or the shell is killed (SIGTERM), a trap stops the player and starts Kodi. A second HUP or TERM during that restore is ignored so Kodi is still started. EXIT does the same. The script is `/bin/sh` (dash and busybox ash). Ctrl+C before the player has been started starts Kodi and exits 130, including while `systemctl stop kodi` or the following `sleep` is still running. After the player is running, the shell ignores SIGINT: the player handles Ctrl+C, exits 0, and the script starts Kodi on the normal path.

CoreELEC has no `fusermount`. A type-2 AppImage is extracted once to `/storage/amlogic-multistream.AppDir` (override with `PLAYER_EXTRACT_DIR`) and `AppRun` is executed directly. The script refuses an empty override, `/`, `/storage`, `$HOME`, and any directory that contains the script or the player. It deletes only `squashfs-root` and the source stamp inside the extract directory, never the directory itself. The type-2 runtime forks the player and does not forward SIGTERM, so the trap must not wait on the runtime. A bare ELF is executed as itself. `PLAYER_BIN` overrides `/storage/amlogic-multistream`. The script polls until that process has exited (up to 10 seconds, then SIGKILL) before it starts Kodi. A decode thread still blocked in the resolver does not hold that poll: after the offline status is published the player waits 2 seconds and then exits. A first broker lookup that never answers does not hold it either: the main thread stays in the signal wait, and SIGTERM exits without joining that helper. The player does not fork today; the trap still signals a forking `AppRun` and its children.

CI runs `scripts/test-launch-coreelec.sh` with a stand-in runtime, and again against the packaged host AppImage. It does not boot a GS-King X.

Put the device AppImage for the box's CoreELEC line at `/storage/amlogic-multistream` before running it: `coreelec21-ng-armv7hf` on CoreELEC 21 Amlogic-ng, `coreelec22-no-aarch64` on CoreELEC 22 Amlogic-no. The host smoke artifacts are the wrong binaries for that path.

## Kodi plugin install

`kodi-addon/plugin.program.amlogic-multistream/` is a Kodi launcher add-on. It **requires GS-King X / CoreELEC** to run. Its **Launch** entry starts the bundled copy of `scripts/launch-coreelec.sh` as a transient systemd unit (`systemd-run --unit=amlogic-multistream-launch`). That is one remote click. It stops Kodi, and software MQTT mode has no display until DRM/KMS, so the player puts no picture on screen. The unit lets the script survive its own `systemctl stop kodi`, run the player, and start Kodi again. To return to Kodi early, run `systemctl stop amlogic-multistream-launch`.

**The add-on does not bundle or download the player AppImage/binary; it is only a thin `systemd-run` launcher, so you must install `PLAYER_BIN` separately** (see [The player binary is not included](docs/kodi-addon.md#the-player-binary-is-not-included), tracking issue [#14](https://github.com/yyolk/amlogic-multistream/issues/14)).

```bash
./scripts/package-kodi-addon.sh   # dist/plugin.program.amlogic-multistream-<version>.zip

# CoreELEC 21 Amlogic-ng (default --target)
./scripts/install-kodi-addon.sh --host root@coreelec \
  --player amlogic-multistream-0.1.0-coreelec21-ng-armv7hf.AppImage

# CoreELEC 22 Amlogic-no
./scripts/install-kodi-addon.sh --host root@coreelec --target coreelec22-no-aarch64 \
  --player amlogic-multistream-0.1.0-coreelec22-no-aarch64.AppImage
```

Or install the zip from Kodi (Add-ons → Install from zip file) and copy the device AppImage to `/storage/amlogic-multistream` yourself. The add-on does not contain the player; **Player binary** (`PLAYER_BIN`) in the add-on settings points at it.

`install-kodi-addon.sh` checks the player against `--target` (default `coreelec21-ng-armv7hf`). For CoreELEC 21 it refuses anything that is not 32-bit ARM, which includes both host smoke AppImages. For CoreELEC 22 it refuses anything that is not aarch64. An AppImage must also carry the device marker for that target, so the Ubuntu aarch64 smoke AppImage is refused for CoreELEC 22 even though its architecture matches (reading the marker needs `unsquashfs`). With `--host`, the box's `/etc/os-release` architecture must match `--target` before anything is copied. `--allow-target-mismatch` (or the older `--allow-non-arm32`) overrides these checks. CI uploads the zip as the `plugin.program.amlogic-multistream` artifact. Install steps, settings, and the list of host-only checks are in [docs/kodi-addon.md](docs/kodi-addon.md). Loading the add-on in Kodi and the display handover have not been verified on a device.
