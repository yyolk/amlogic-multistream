# amlogic-multistream

Multi-stream HLS viewer for Amlogic S922X (GS-King X / CoreELEC).

## Goals
- Concurrent HLS streams with Amlogic hardware decoding (multi_vdec)
- Dynamic grid layouts
- MQTT (and optional HTTP) control for Home Assistant
- AppImage packaging
- RetroArch-style launch: stop Kodi → run app → restart Kodi

## Status
Initial skeleton. Grid layout selection is implemented and unit-tested. Single-stream DRM + hardware decode proof of concept is the next milestone.

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

## Build (on a normal Linux or cross)
```bash
cmake -S . -B build
cmake --build build --target layout_engine_tests
ctest --test-dir build --output-on-failure
```

The `amlogic-multistream` player target is configured only when libavformat, libavcodec, libavutil, libswscale, libmosquitto, and libdrm are all present. Layout tests do not need them.

On Debian/Ubuntu those player packages are:

```bash
sudo apt-get install cmake g++ pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
  libmosquitto-dev libdrm-dev
```

Pass `-DCMAKE_CXX_COMPILER=g++` when `c++` on the PATH is not g++.

## Tests

`layout_engine_tests` is the layout unit-test binary. CTest registers it as the `layout_engine` test. `ctest` runs that binary and every other target registered with `add_test`.

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
```

`./scripts/run-tests.sh` is the same three steps. It fails if `layout_engine_tests` is missing from the CTest list, or if any test fails. GitHub Actions runs this script on pull requests and on pushes to `main` before it builds the AppImage.

## Codespaces

`.devcontainer/` is a Ubuntu dev container with g++, CMake, pkg-config, the FFmpeg development libraries, libmosquitto, and libdrm.

1. On GitHub, choose **Code → Codespaces → Create codespace** on `main` or on a pull request.
2. After the container is created, `postCreateCommand` runs `scripts/run-tests.sh`. The creation log includes the `layout_engine_tests` result, and `build/` is ready for CTest.
3. Run the tests again with:

```bash
ctest --test-dir build --output-on-failure
```

Rebuild after source changes with `cmake --build build`. The CMake Tools extension uses this `build/` directory, so its test runner calls the same CTest list.

`scripts/build-appimage.sh` works in the codespace too. `scripts/launch-coreelec.sh` is the on-device launch path: it stops Kodi and runs `/storage/amlogic-multistream` on CoreELEC. CI and Codespaces leave that script unused.

## AppImage

`.github/workflows/ci.yml` runs on pull requests and on pushes to `main`. A push to any other branch is built when its pull request is opened, so that commit is not packaged twice.

The job installs the dev-container packages, runs `scripts/run-tests.sh` (`layout_engine_tests` and any other CTest targets), then runs `scripts/build-appimage.sh`. The script builds the player and packs it with linuxdeploy and appimagetool. Download `amlogic-multistream-appimage` from the workflow run's Artifacts list.

Locally, with the build packages above plus `curl`, `patchelf`, `binutils`, and `file`:

```bash
./scripts/build-appimage.sh
```

The image is `dist/amlogic-multistream-<version>-<arch>.AppImage`. Packaging runs on a normal Linux host and does not use a GS-King X or a DRM device.

## Launch on CoreELEC
See `scripts/launch-coreelec.sh`. Run it on a CoreELEC device. CI and Codespaces skip it.
