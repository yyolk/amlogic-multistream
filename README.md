# amlogic-multistream

Multi-stream HLS viewer for Amlogic S922X (GS-King X / CoreELEC).

## Goals
- Concurrent HLS streams with Amlogic hardware decoding (multi_vdec)
- Dynamic grid layouts
- MQTT (and optional HTTP) control for Home Assistant
- AppImage packaging
- RetroArch-style launch: stop Kodi → run app → restart Kodi

## Status
Grid layout selection is implemented and unit-tested. MQTT command control is implemented against an in-memory stream table. Single-stream software decode is in place: `FfmpegDecoder` opens one HLS, HTTP, or file URL with libavformat/libavcodec, decodes on the CPU, and pulls 8-bit YUV420P frames through `DecoderBackend::readFrame`. `isHardware()` is false for this backend.

DRM presentation and Amlogic hwaccel are still separate. The hwaccel insertion point in `src/ffmpeg_decoder.cpp` (`prepareAmlogicHwaccel`) is a no-op marked **requires GS-King X / CoreELEC**. It must not grow inside this software class; a future hardware backend should be its own `DecoderBackend`.

`DecodedFrame` plane pointers are owned by the decoder and stay valid only until the next `readFrame()` or `close()`. A renderer should copy them. `Renderer` itself is unchanged.

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

The `ffmpeg` CLI is not linked into the player. It is used by `ffmpeg_decoder_tests` and by the manual `--url` check below to synthesize a local media fixture. The player target also needs libmosquitto and libdrm. `--url` does not start MQTT or DRM.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`layout_engine_tests`, `stream_manager_test`, and `mqtt_smoke_test` do not need a broker. Layout and stream-table tests also do not need FFmpeg, libmosquitto, or libdrm. `mqtt_smoke_test` parses commands in-process and does not link libmosquitto.

`ffmpeg_decoder_tests` is added when libavformat, libavcodec, libavutil, and libswscale are present. It does not need libmosquitto, libdrm, or a network. The test runs the `ffmpeg` CLI to write a tiny local MPEG-4 MP4 and, when libx264 is available, an HLS playlist, then opens each with `FfmpegDecoder` and checks `isHardware() == false` plus at least one YUV420P frame. If the CLI is missing, the test exits 77 and ctest marks it skipped. If the dev libraries are missing, CMake skips that target and still builds the other tests.

The `amlogic-multistream` player target is configured only when libavformat, libavcodec, libavutil, libswscale, libmosquitto, and libdrm are all present.

## MQTT
Without `--url`, the process connects to a broker and blocks until SIGINT or SIGTERM. Defaults are `localhost:1883` and topic prefix `amlogic-multistream` (`MQTT_HOST`, `MQTT_PORT`, `MQTT_TOPIC_PREFIX`). Command topics, JSON fields, and `mosquitto_pub` examples are in [docs/mqtt.md](docs/mqtt.md).

This build does not open DRM/KMS devices or Amlogic decoders. `scripts/launch-coreelec.sh` requires GS-King X / CoreELEC. It stops Kodi for the lifetime of this process.

## Manual player check

`--url` demuxes and software-decodes one input, prints each frame, and exits. It does not start MQTT and does not call DRM present. A generated local playlist needs no network. A live HLS URL does.

```bash
mkdir -p /tmp/hls-poc
ffmpeg -y -f lavfi -i testsrc=size=320x240:rate=10:duration=2 \
  -c:v libx264 -pix_fmt yuv420p -hls_time 1 -hls_list_size 0 \
  /tmp/hls-poc/index.m3u8

./build/amlogic-multistream --url /tmp/hls-poc/index.m3u8 --frames 5
```

Expect exit status 0, `isHardware=0`, and lines like `frame 0 320x240 fmt=yuv420p`. Exit status 1 means open or decode failed. Exit status 2 means bad arguments (`--help` with `--url` prints decode usage and exits 0; `--help` alone prints MQTT usage).

Public HLS (network required), for example:

```bash
./build/amlogic-multistream --url https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8 --frames 5
```

Network opens use a 15s socket read timeout (`rw_timeout`). CI that cannot reach that host should use the local fixture or `ffmpeg_decoder_tests` instead.

Pass `-DCMAKE_CXX_COMPILER=g++` when `c++` on the PATH is not g++.

## Tests

`layout_engine_tests` is the layout unit-test binary. CTest registers it as `layout_engine`. The same `ctest` run also executes `stream_manager_test`, `mqtt_smoke_test`, and, when the FFmpeg libraries are present, `ffmpeg_decoder_tests`.

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
```

`./scripts/run-tests.sh` is those three steps. It fails if `layout_engine_tests` is missing from the CTest list, or if any test fails. GitHub Actions runs it on both the x86_64 and aarch64 runners before packaging. Those runners execute the tests natively.

## Codespaces

`.devcontainer/` is an Ubuntu container with g++, CMake, pkg-config, the FFmpeg development libraries, the `ffmpeg` CLI, libmosquitto, and libdrm. A codespace is the host smoke environment (usually x86_64).

1. On GitHub, choose **Code → Codespaces → Create codespace** on `main` or on a pull request.
2. After the container is created, `postCreateCommand` runs `scripts/run-tests.sh`. The creation log includes the CTest result, and `build/` is ready.
3. Run the tests again with:

```bash
ctest --test-dir build --output-on-failure
```

Rebuild after source changes with `cmake --build build`. The CMake Tools extension uses this `build/` directory, so its test runner calls the same CTest list.

`./scripts/build-appimage.sh` in the codespace packages the codespace's own architecture. `scripts/launch-coreelec.sh` requires GS-King X / CoreELEC and is unused here.

## AppImage

`.github/workflows/ci.yml` runs on pull requests and on pushes to `main`. A push to any other branch is built when its pull request is opened.

The workflow is a two-runner matrix. Each runner installs the dev-container packages, runs `scripts/run-tests.sh`, then runs `scripts/build-appimage.sh`. linuxdeploy bundles the shared libraries it finds. The build is not statically linked.

| Artifact | Runner | Role |
| --- | --- | --- |
| `amlogic-multistream-appimage-x86_64` | `ubuntu-24.04` | Host and Codespaces smoke build |
| `amlogic-multistream-appimage-aarch64` | `ubuntu-24.04-arm` | Amlogic S922X device build |

Download both from the workflow run's Artifacts list. The file inside each artifact is `amlogic-multistream-<version>-<arch>.AppImage`.

On your own machine the same script follows `uname -m`:

```bash
./scripts/build-appimage.sh
```

That needs the build packages above plus `curl`, `patchelf`, `binutils`, and `file`. An x86_64 host produces the smoke image. An aarch64 host produces the device image. Packaging does not need a GS-King X. Copy the aarch64 AppImage to the device when you are ready to launch it there.

linuxdeploy leaves glibc to the system and bundles the other shared libraries. A CoreELEC image with a 32-bit userspace will not run the aarch64 file; the device userspace needs to be aarch64.

## Launch on CoreELEC

`scripts/launch-coreelec.sh` requires GS-King X / CoreELEC. It stops Kodi, runs `/storage/amlogic-multistream`, and starts Kodi again. CI and Codespaces do not call it.
