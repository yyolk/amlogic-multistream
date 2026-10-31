# amlogic-multistream

Multi-stream HLS viewer for Amlogic S922X devices (Beelink GS-King X, etc.) running CoreELEC.

## Goals
- Display 1–9 concurrent HLS live streams using Amlogic VPU hardware decoding
- Automatic optimal grid layouts based on stream count
- Control via MQTT (primary) and optional HTTP API for Home Assistant integration
- Fullscreen DRM/KMS output (no window manager required)
- Package as a portable AppImage
- Launch pattern identical to RetroArch on CoreELEC: stop Kodi → run app → restart Kodi

## Status
Early scaffolding. Single-stream hardware decode + MQTT control is the first milestone.

## Target Environment
- CoreELEC (Amlogic vendor kernel) on S922X / G12B
- Kodi stopped while the app is running
- Minimal userspace (no desktop)

## Architecture Notes
- Prefer C++ with FFmpeg (Amlogic hwaccel) or libmpv
- Isolate decoder and renderer behind interfaces so software/generic paths can be used for development
- MQTT commands (JSON): add, remove, layout, mute, focus, restart
- Document practical concurrent hardware decoder limit on S922X

## Launch Pattern (CoreELEC)
```bash
systemctl stop kodi
./amlogic-multistream ...
systemctl start kodi
```
Or via systemd-run wrapper (same pattern used by RetroArch addons).

## Related / Inspiration
- Quadstream (macOS/iOS, limited to 4)
- StreamGrid
- RetroArch launch pattern on CoreELEC
- AmlPlay (Amlogic hardware player reference)

## License
TBD
