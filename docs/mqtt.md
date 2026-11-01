# MQTT command control

The player process connects to an MQTT broker, subscribes to command topics, and keeps an in-memory stream table. Broker I/O runs on the mosquitto thread. The main thread presents until SIGINT, SIGTERM, or SIGHUP. Each visible stream is opened and decoded on its own thread.

`start` / `play` sets the player-loop flag. While it is set, each visible stream is opened with `FfmpegDecoder` (CPU, `isHardware()` false) and at most one already-decoded YUV420P frame per tick is presented with `SoftwareRenderer` into the `LayoutEngine` cell for that stream. `stop` clears the flag, aborts and joins those decoders, and clears the RGB viewport. The stream table is kept, and MQTT stays connected. DRM/KMS and Amlogic hardware decode are not started. Software mode does not drive HDMI: the RGB buffer is not scanned out, so Launch from the Kodi add-on stops Kodi and puts no player picture on screen until a DRM/KMS backend exists. What the HDMI port itself shows (black, a console, or a frozen Kodi frame) has not been checked on a GS-King X. Stop the process (`systemctl stop amlogic-multistream-launch` on the add-on path) to return to Kodi.

CoreELEC display takeover is still `scripts/launch-coreelec.sh` (stop Kodi, run this process, start Kodi). The process stays in the foreground until it receives SIGINT, SIGTERM, or SIGHUP, so Kodi stays stopped for that whole time.

## Broker

Environment variables, overridden by the matching flag:

| Variable | Flag | Default |
| --- | --- | --- |
| `MQTT_HOST` | `--mqtt-host` | `localhost` |
| `MQTT_PORT` | `--mqtt-port` | `1883` |
| `MQTT_TOPIC_PREFIX` | `--mqtt-prefix` | `amlogic-multistream` |
| `MQTT_USERNAME` | `--mqtt-user` | empty (anonymous) |
| `MQTT_PASSWORD` | `--mqtt-pass` | empty |
| `MQTT_CLIENT_ID` | `--mqtt-client-id` | `amlogic-multistream` |

TLS is not implemented. The client uses a clean session, QoS 1, and reconnects with backoff from 1s to 30s. That backoff includes the first connection: a refused port or a name that does not resolve keeps the process up and retries instead of exiting. A CONNACK of "not authorised" or "bad user name or password" does not. After three of those refusals the process exits non-zero, and `scripts/launch-coreelec.sh` starts Kodi. Outside a lab LAN, broker authentication (a username and password, or a broker ACL) is expected. The Kodi add-on passes `MQTT_USERNAME` and, when a password is set, `MQTT_PASSWORD` through a mode 0600 `EnvironmentFile` (see [kodi-addon.md](kodi-addon.md)). This does not add TLS verification and does not change that CoreELEC runs the player as root.

```bash
amlogic-multistream --mqtt-host 192.168.1.10 --mqtt-prefix living-room
```

`--help` prints the same list and does not connect.

Software present, only on this MQTT path (`--url` still sizes its viewport from the frame):

| Flag | Default | Effect |
| --- | --- | --- |
| `--viewport WxH` | `1280x720` | RGB viewport. Each side must be 1..8192. |
| `--render-dir DIR` | off | Write `frame-NNNNNN.ppm` (P6, the whole viewport) on each successful present. No directory means no files. |

The presenter is always `SoftwareRenderer`. It does not open `/dev/dri`. `--renderer drm` is only valid with `--url`.

The main thread presents at most one frame that each visible stream has already decoded, then waits about 20ms for SIGINT, SIGTERM, or SIGHUP. It does not wait on socket I/O. A stream that is stalled, trickling, or waiting at a live HLS edge blocks only its own decode thread. `stop`, `remove`, `restart`, and shutdown call `DecoderBackend::abort()`, which latches for that decoder and makes the in-flight open or read return, so shutdown is not held by a stuck socket. The per-socket `rw_timeout` is still 15s on that stream's thread. Streams past the last layout cell are not opened. A failed open or a read that ends retries after one second; `restart` retries immediately.

`FfmpegDecoder` keeps the protocol allowlist (`http`, `https`, `tls`, `tcp`, `crypto`, `hls`, and `file` only on the `--url` local-file path) and also sets a demuxer allowlist (`hls`, `mpegts`, `mov`, `mp4`, `mpeg`) and a codec allowlist (`h264`, `hevc`, `mpeg4`, `mpeg2video`, `mpeg1video`, `mjpeg`, plus `aac`, `mp3`, `ac3`, and `eac3` so a container probe can open the audio stream). HLS copies those whitelists onto its segment demuxers. An HTTP GIF is rejected. This is not a fix for TLS verification or for the player running as root.

## Topics

`<prefix>` below is the topic prefix with trailing slashes removed.

| Topic | Direction | Retain | Purpose |
| --- | --- | --- | --- |
| `<prefix>/cmd/#` | subscribe | commands must be published with retain **false** | One topic per command |
| `<prefix>/cmd` | subscribe | false | Same commands, with the name in the payload |
| `<prefix>/status` | publish | true | Current player and stream table |
| `<prefix>/response` | publish | false | Result of the last command |

The broker will is a retained offline status. It starts as `"online": false` with an empty stream list. After each status publish the client stores a new will built from the current table (`"online": false` and the real streams). libmosquitto sends that will on the next CONNECT, including a reconnect after the broker drops. A SIGKILL before that next CONNECT still publishes the will from the previous CONNECT. SIGINT, SIGTERM, and SIGHUP are a clean shutdown: the process publishes a final retained status from the live table with `"online": false`, then disconnects, so the broker discards the will. Each retained status payload is also written to stdout as `MQTT status …`. Stdout is line-buffered, so a SIGKILL leaves that last line in the log (the add-on journal) even though the process cannot publish again.

Retained messages on command topics are ignored. A command is applied only when it is delivered live.

## Commands

JSON objects. An empty payload is treated as `{}`. Command names are case-insensitive (`Add` and `add` are the same). Field names, stream ids, and URLs are case-sensitive. Unknown object fields are ignored. Nested objects and arrays are ignored.

`play` is an alias of `start`.

Publish either to `<prefix>/cmd/<command>` or to `<prefix>/cmd` with `"action"` (or `"command"`) set. If the topic already names a command, that name wins over the payload.

| Command | Payload | Effect |
| --- | --- | --- |
| `start`, `play` | none | Set the player-loop flag. On the next ticks, open a software decoder for each visible stream and present frames. MQTT stays connected. |
| `stop` | none | Clear the player-loop flag, close decoders, and clear the viewport. The stream table stays. MQTT stays connected. Does not exit. |
| `add` | `id`, `url` (strings) | Insert a stream. `url` must start with `http://` or `https://` (lowercase scheme). A NUL byte in the URL is rejected, and so is any other scheme (`file`, `tcp`, `pipe`, `concat`, …). The rejected URL is not stored, and the player will not open it. Same id and url again is success (`already present`) and does not reopen the decoder. Same id with a different url is rejected. The table holds at most 9 streams (`LayoutEngine::kMaxStreams`, the largest grid). A new id past that fails with `stream limit reached`, does not publish status, and does not open a decoder. Removing a stream frees a slot. While playing, the next tick opens the new stream. While stopped, it is stored until `start`. |
| `remove` | `id` | Remove that stream and close its decoder. |
| `list` | none | Republish `<prefix>/status`. The response message is `ok`. |
| `layout` | `layout` optional | With no `layout` field, respond with the effective layout and do not change state. `auto` picks a grid from the stream count (`1x1`, `1x2`, `2x2`, `2x3`, `3x3`). An explicit token must be `<rows>x<cols>` with a lowercase `x` and each side a digit from 1 to 3 (`2x2`, and rotations such as `2x1` and `3x2`). Anything else fails the command and does not publish status: `banana`, a number such as `4` (stored by the parser as the string `"4"`), `2X2`, `AUTO`, and an empty string. The previous `layout` and `layout_setting` stay, and the wall is not cleared. Cells that lose a stream on a successful change are cleared. |
| `mute` | `id`, `muted` | Set mute for one stream. `muted` is a JSON bool, or the string `"true"` / `"false"`. There is no audio decoder, so mute does not change the picture. |
| `restart` | `id` optional | With a non-empty string `id`, bump that stream's generation so the player closes and reopens its decoder. The url and mute flag stay. An unknown id fails with `stream not found` and does not restart any other stream. An empty id, or an id that is not a string (for example `true`), fails with `id is required` and also restarts nothing. Omitting `id` restarts every stream. A JSON `null` is not stored, so `"id": null` is the same as omitting `id`. |

`stop` does not exit the process. Kill it with SIGINT, SIGTERM, or SIGHUP when Kodi should come back. Send `start` (or `play`) after `add` before frames are presented. The examples below do that. Software mode still does not put those frames on a display.

### Response

Published to `<prefix>/response`:

```json
{"ok":true,"command":"add","id":"cam1","message":"added"}
```

`id` is `""` when the command has no stream id. `ok` is `false` for unknown commands, malformed JSON, a missing stream, a duplicate id with a different url, a URL that is not `http://` or `https://`, a 10th stream, or a layout token the grid engine cannot parse. A failed `layout` or `add` does not publish `<prefix>/status`, so the retained layout and stream list stay as they were. Messages used today include `playing`, `stopped`, `added`, `already present`, `stream already exists`, `stream limit reached`, `removed`, `stream not found`, `ok`, `muted`, `unmuted`, `restart requested`, `restarted N`, `command is required`, `unknown command`, `id and url are required`, `url must be http:// or https://`, `id is required`, `muted is required`, `layout must be a non-empty string`, `invalid layout`, `invalid json`, and `payload too large`.

A successful `layout` command uses the effective layout string as `message` (for example `2x2` or `1x2`).

### Status

Published to `<prefix>/status` after a successful command that changes or reports state, and again on each broker connect:

```json
{
  "online": true,
  "playing": false,
  "layout": "1x2",
  "layout_setting": "auto",
  "streams": [
    {"id": "cam1", "url": "http://example/live.m3u8", "muted": false}
  ]
}
```

`layout` is the effective grid. `layout_setting` is what was stored (`auto` or an explicit token). `streams` is insertion order. `playing` is the player-loop flag: true means each visible stream has a decode thread and the main thread is presenting, false means those threads have been joined and the decoders are closed.

## Examples

No broker is required for the automated tests:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`layout_engine` checks grid selection. `stream_manager_test` checks add, remove, list, mute, restart generation, layout state, rejection of tokens such as `banana` and `2X2`, and the 9-stream cap. `mqtt_smoke_test` checks JSON parsing, topic routing, and command results, including that an unknown or non-string restart id does not bump other streams, that `banana`, `4`, and `2X2` fail without changing the stored layout or requesting a status publish, and that a 10th `add` fails with `stream limit reached`. `stream_player_test` drives `StreamPlayer` with a fake decoder and `SoftwareRenderer`: add, play, present into layout cells, stop, a one-id restart, and a rejected layout that leaves the previous grid on screen. None of these open a socket or link libmosquitto. `stream_player_http` is added when the FFmpeg libraries are present. It serves one MPEG-TS file on `127.0.0.1` and checks decode and present through the real `FfmpegDecoder`. It does not open a broker. `stream_player_stall` is added with those libraries too. It places a healthy MPEG-TS stream next to a connection that never answers and a connection that trickles one byte every 5s, and a live HLS stream next to VOD when libx264 is available. Other streams must keep presenting, and closing the player must return within 2s. The `amlogic-multistream` player binary is configured only when FFmpeg, libmosquitto, and libdrm are all present. GitHub Actions sets `AMS_REQUIRE_DECODER_TESTS=1`, so `scripts/run-tests.sh` fails if `ffmpeg_decoder`, `present_smoke`, or `stream_player_http` is missing from CTest or is skipped.

With a broker and the app running:

```bash
mosquitto_sub -h localhost -t 'amlogic-multistream/#' -v

mosquitto_pub -h localhost -t amlogic-multistream/cmd/add \
  -m '{"id":"cam1","url":"http://example/live.m3u8"}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/start -m '{}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/mute \
  -m '{"id":"cam1","muted":true}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/layout \
  -m '{"layout":"2x2"}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/list -m '{}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/restart \
  -m '{"id":"cam1"}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/stop -m '{}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/remove \
  -m '{"id":"cam1"}'

# Same add, with the command name in the payload:
mosquitto_pub -h localhost -t amlogic-multistream/cmd \
  -m '{"action":"add","id":"cam2","url":"http://example/other.m3u8"}'
```

## Not in this build

- Amlogic `multi_vdec` / hardware decode (`FfmpegDecoder::isHardware()` stays false; a backend that reports hardware is closed)
- DRM/KMS modeset or a plane commit (the `DrmRenderer` hook **requires GS-King X / CoreELEC** and is not started on this path)
- Audio, or a mute flag that reaches a decoder
- TLS to the broker, HTTP control, and Home Assistant discovery payloads

`StreamManager` is the table `StreamPlayer` reads. `--url` is still a separate one-shot decode and does not start this controller. The player does not set `FfmpegDecoder`'s local-file allowance; only `http://` and `https://` are opened from MQTT.
