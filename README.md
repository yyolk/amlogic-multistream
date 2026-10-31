# amlogic-multistream

Multi-stream HLS viewer for Amlogic S922X (GS-King X / CoreELEC).

## Goals
- Concurrent HLS streams with Amlogic hardware decoding (multi_vdec)
- Dynamic grid layouts
- MQTT (and optional HTTP) control for Home Assistant
- AppImage packaging
- RetroArch-style launch: stop Kodi → run app → restart Kodi

## Status
Grid layout selection is implemented and unit-tested. MQTT command control is implemented against an in-memory stream table. Single-stream DRM + Amlogic hardware decode is still a stub and is not started by this process.

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
cmake --build build
ctest --test-dir build --output-on-failure
```

`layout_engine_tests`, `stream_manager_test`, and `mqtt_smoke_test` do not need a broker. Layout and stream-table tests also do not need FFmpeg, libmosquitto, or libdrm. `mqtt_smoke_test` parses commands in-process and does not link libmosquitto.

The `amlogic-multistream` player target is configured only when libavformat, libavcodec, libavutil, libswscale, libmosquitto, and libdrm are all present.

## MQTT
The process connects to a broker and blocks until SIGINT or SIGTERM. Defaults are `localhost:1883` and topic prefix `amlogic-multistream` (`MQTT_HOST`, `MQTT_PORT`, `MQTT_TOPIC_PREFIX`). Command topics, JSON fields, and `mosquitto_pub` examples are in [docs/mqtt.md](docs/mqtt.md).

This build does not open DRM/KMS devices or Amlogic decoders. On CoreELEC, `scripts/launch-coreelec.sh` still stops Kodi for the lifetime of this process.

## Launch on CoreELEC
See `scripts/launch-coreelec.sh`.
