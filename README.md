# amlogic-multistream

Multi-stream HLS viewer for Amlogic S922X (GS-King X / CoreELEC).

## Goals
- Concurrent HLS streams with Amlogic hardware decoding (multi_vdec)
- Dynamic grid layouts
- MQTT (and optional HTTP) control for Home Assistant
- AppImage packaging
- RetroArch-style launch: stop Kodi → run app → restart Kodi

## Status
Initial skeleton. Single-stream DRM + hardware decode proof of concept is the next milestone.

## Build (on a normal Linux or cross)
```bash
mkdir build && cd build
cmake ..
make
```

## Launch on CoreELEC
See `scripts/launch-coreelec.sh`.
