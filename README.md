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

## Launch on CoreELEC
See `scripts/launch-coreelec.sh`.
