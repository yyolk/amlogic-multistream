# amlogic-multistream

Multi-stream HLS viewer for Amlogic S922X (GS-King X / CoreELEC).

## Goals
- Concurrent HLS streams with Amlogic hardware decoding (multi_vdec)
- Dynamic grid layouts
- MQTT (and optional HTTP) control for Home Assistant
- AppImage packaging
- RetroArch-style launch: stop Kodi → run app → restart Kodi

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

`stream_player_http` is added with the FFmpeg libraries. It serves one local MPEG-TS file on `127.0.0.1` (no broker) and checks that `StreamPlayer` plus `FfmpegDecoder` present at least one frame, that an unknown restart id does not reopen it, and that `stop` clears the viewport.

The `amlogic-multistream` player target is configured only when libavformat, libavcodec, libavutil, libswscale, libmosquitto, and libdrm are all present. `present_smoke_test` is registered with that target. It decodes a local fixture through `--url --render-dir` and checks that `--renderer drm` fails without a GS-King X. `-DAMLOGIC_DRM_KMS=ON` is off in CI.

## MQTT
Without `--url`, the process connects to a broker and software-decodes added streams until SIGINT or SIGTERM. Broker I/O runs on a background thread. The main thread ticks decode and present, so it is not blocked in `sigwait`. `start` / `play` opens an `FfmpegDecoder` per visible `http://` or `https://` stream and presents into `SoftwareRenderer` cells. `stop` closes those decoders and clears the viewport. `restart` with an id reopens that decoder; an unknown id fails and does not restart the others. `add` accepts at most 9 streams. A further add fails and does not publish status. `layout` accepts `auto` or `<rows>x<cols>` with each side in 1..3 and a lowercase `x`. `banana`, `4`, and `2X2` fail the command, leave the previous grid stored, and do not clear the wall. Defaults are `localhost:1883`, topic prefix `amlogic-multistream`, and a `1280x720` viewport (`MQTT_HOST`, `MQTT_PORT`, `MQTT_TOPIC_PREFIX`, `--viewport`, `--render-dir`). PPM output is off unless `--render-dir` is set. Command topics, JSON fields, and `mosquitto_pub` examples are in [docs/mqtt.md](docs/mqtt.md).

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

Network opens use a 15s socket read timeout (`rw_timeout`). CI that cannot reach that host should use the local fixture or `ffmpeg_decoder_tests` instead.

Pass `-DCMAKE_CXX_COMPILER=g++` when `c++` on the PATH is not g++.

## Tests

`layout_engine_tests` is the layout unit-test binary. CTest registers it as `layout_engine`. The same `ctest` run also executes `stream_manager_test`, `mqtt_smoke_test`, `software_renderer_tests`, `stream_player_test`, `scripts/test-ctest-require.sh` (the CI guard that rejects a run where `ffmpeg_decoder`, `present_smoke`, or `stream_player_http` was omitted or skipped), and `scripts/test-launch-coreelec.sh` (a stand-in for the CoreELEC launch script: bash, dash, and busybox ash when busybox is installed). When the FFmpeg libraries are present it also runs `ffmpeg_decoder_tests` and `stream_player_http`, and when the player dependencies are present it runs `present_smoke_test`.

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

`.github/workflows/ci.yml` runs on pull requests and on pushes to `main`. A push to any other branch is built when its pull request is opened.

The workflow is a two-runner matrix. Each runner installs the dev-container packages, runs `scripts/run-tests.sh`, then runs `scripts/build-appimage.sh`. linuxdeploy bundles the shared libraries it finds. The build is not statically linked.

| Artifact | Runner | Role |
| --- | --- | --- |
| `amlogic-multistream-appimage-x86_64` | `ubuntu-24.04` | Host and Codespaces smoke build |
| `amlogic-multistream-appimage-aarch64` | `ubuntu-24.04-arm` | Native aarch64 build. **Not** the GS-King X / CoreELEC binary |

Download both from the workflow run's Artifacts list. The file inside each artifact is `amlogic-multistream-<version>-<arch>.AppImage`. The `aarch64` in that name is the runner architecture. It is not a CoreELEC Amlogic-ng image.

On your own machine the same script follows `uname -m`:

```bash
./scripts/build-appimage.sh
```

That needs the build packages above plus `curl`, `patchelf`, `binutils`, and `file`. An x86_64 host produces the smoke image. An aarch64 host produces another aarch64 image. Packaging does not need a GS-King X. Neither file is the GS-King X binary.

The GS-King X runs CoreELEC **Amlogic-ng**, a 32-bit ARM userspace (`CoreELEC-Amlogic-ng.arm`, device tree `g12b_s922x_beelink_gs_king_x`). That kernel will not load an aarch64 userspace binary. CI does not cross-compile armv7. A full CoreELEC sysroot build is not part of this workflow.

linuxdeploy leaves glibc to the system and bundles the other shared libraries. Do not treat glibc 2.39 as the reason the device image fails. On Ubuntu 24.04 (the CI distro) the player's bundled libraries require `GLIBC_2.38` at most. `GLIBC_2.39` symbols are exports on the host `libc.so.6` and `libm.so.6`, which linuxdeploy does not bundle. CoreELEC 21.3-Omega pins glibc 2.38, so that floor matches. The mismatch is the 32-bit userspace, not a 2.39 requirement, and it is not a reason to retarget the build at Debian bullseye.

Ubuntu's FFmpeg is linked with libX11, VAAPI, VDPAU, and OpenCL. linuxdeploy's excludelist would omit `libX11.so.6` and the libraries behind it (`libxcb`, fontconfig, freetype, harfbuzz, and the rest of that closure), so `--help` fails on a machine that does not have them. `scripts/build-appimage.sh` copies those libraries into the AppDir. The script then runs `--help` in a chroot that only offers glibc, libstdc++, libgcc_s, zlib, and libdrm. A device AppImage should still link CoreELEC's FFmpeg, or one built without X11, VAAPI, VDPAU, and OpenCL, instead of shipping this Ubuntu stack.

**TODO:** Build a real GS-King X AppImage as 32-bit ARM (armv7, hard-float) against a CoreELEC Amlogic-ng sysroot (glibc 2.38 on CoreELEC 21.3-Omega). The `ubuntu-24.04-arm` aarch64 artifact cannot be that file. This repository does not produce the device AppImage yet.

## Launch on CoreELEC

`scripts/launch-coreelec.sh` **requires GS-King X / CoreELEC**. It stops Kodi, runs `/storage/amlogic-multistream`, and starts Kodi again. If the SSH session drops (SIGHUP) or the shell is killed (SIGTERM), a trap stops the player and starts Kodi. EXIT does the same. The script is `/bin/sh` (dash and busybox ash). Ctrl+C before the player has been started starts Kodi and exits 130, including while `systemctl stop kodi` or the following `sleep` is still running. After the player is running, the shell ignores SIGINT: the player handles Ctrl+C, exits 0, and the script starts Kodi on the normal path.

CoreELEC has no `fusermount`. A type-2 AppImage is extracted once to `/storage/amlogic-multistream.AppDir` (override with `PLAYER_EXTRACT_DIR`) and `AppRun` is executed directly. The type-2 runtime forks the player and does not forward SIGTERM, so the trap must not wait on the runtime. A bare ELF is executed as itself. `PLAYER_BIN` overrides `/storage/amlogic-multistream`. The script polls until that process has exited (up to 10 seconds, then SIGKILL) before it starts Kodi.

CI runs `scripts/test-launch-coreelec.sh` with a stand-in runtime, and again against the packaged AppImage. It does not boot a GS-King X.

Put a 32-bit ARM build at `/storage/amlogic-multistream` before running it. The aarch64 CI artifact is the wrong binary for that path.

## Kodi plugin install

`kodi-addon/plugin.program.amlogic-multistream/` is a Kodi launcher add-on. It **requires GS-King X / CoreELEC** to run. Its **Launch** entry starts the bundled copy of `scripts/launch-coreelec.sh` as a transient systemd unit (`systemd-run --unit=amlogic-multistream-launch`). The unit lets the script survive its own `systemctl stop kodi`, run the player, and start Kodi again. To return to Kodi early, run `systemctl stop amlogic-multistream-launch`.

```bash
./scripts/package-kodi-addon.sh                       # dist/plugin.program.amlogic-multistream-<version>.zip
./scripts/install-kodi-addon.sh --host root@coreelec  # or Kodi: Add-ons → Install from zip file
```

The add-on does not contain the player. Put a 32-bit ARM build at `/storage/amlogic-multistream`, or change **Player binary** (`PLAYER_BIN`) in the add-on settings. The x86_64 and aarch64 CI AppImages do not run on the GS-King X. `install-kodi-addon.sh --player FILE` refuses a binary that is not 32-bit ARM. CI uploads the zip as the `plugin.program.amlogic-multistream` artifact. Install steps, settings, and the list of host-only checks are in [docs/kodi-addon.md](docs/kodi-addon.md). Loading the add-on in Kodi and the display handover have not been verified on a device.
