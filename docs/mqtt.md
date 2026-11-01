# MQTT command control

The player process connects to an MQTT broker, subscribes to command topics, and keeps an in-memory stream table. Broker I/O runs on the mosquitto thread. The main thread ticks software decode and present until SIGINT or SIGTERM, so the process is not blocked in `sigwait` with no decode loop.

`start` / `play` sets the player-loop flag. While it is set, each visible stream is opened with `FfmpegDecoder` (CPU, `isHardware()` false) and one YUV420P frame per tick is presented with `SoftwareRenderer` into the `LayoutEngine` cell for that stream. `stop` clears the flag, closes those decoders, and clears the RGB viewport. The stream table is kept, and MQTT stays connected. DRM/KMS and Amlogic hardware decode are not started.

CoreELEC display takeover is still `scripts/launch-coreelec.sh` (stop Kodi, run this process, start Kodi). The process stays in the foreground until it receives SIGINT or SIGTERM, so Kodi stays stopped for that whole time.

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

TLS is not implemented. The client uses a clean session, QoS 1, and reconnects with backoff from 1s to 30s.

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

The main thread pulls at most one frame from each visible stream, presents it, then waits about 20ms for SIGINT or SIGTERM. A read that blocks inside FFmpeg (the socket `rw_timeout` is 15s) delays the next tick and the signal until that read returns. Streams past the last layout cell are not opened. A failed open or a read that ends retries after one second; `restart` retries immediately.

## Topics

`<prefix>` below is the topic prefix with trailing slashes removed.

| Topic | Direction | Retain | Purpose |
| --- | --- | --- | --- |
| `<prefix>/cmd/#` | subscribe | commands must be published with retain **false** | One topic per command |
| `<prefix>/cmd` | subscribe | false | Same commands, with the name in the payload |
| `<prefix>/status` | publish | true | Current player and stream table |
| `<prefix>/response` | publish | false | Result of the last command |

The broker will is a retained offline status (`"online": false` and an empty stream list). That payload is fixed at connect time. On a clean shutdown the process publishes a final retained status built from the real table with `"online": false` before it disconnects.

Retained messages on command topics are ignored. A command is applied only when it is delivered live.

## Commands

JSON objects. An empty payload is treated as `{}`. Command names are case-insensitive (`Add` and `add` are the same). Field names, stream ids, and URLs are case-sensitive. Unknown object fields are ignored. Nested objects and arrays are ignored.

`play` is an alias of `start`.

Publish either to `<prefix>/cmd/<command>` or to `<prefix>/cmd` with `"action"` (or `"command"`) set. If the topic already names a command, that name wins over the payload.

| Command | Payload | Effect |
| --- | --- | --- |
| `start`, `play` | none | Set the player-loop flag. On the next ticks, open a software decoder for each visible stream and present frames. MQTT stays connected. |
| `stop` | none | Clear the player-loop flag, close decoders, and clear the viewport. The stream table stays. MQTT stays connected. Does not exit. |
| `add` | `id`, `url` (strings) | Insert a stream. `url` must start with `http://` or `https://` (lowercase scheme). A NUL byte in the URL is rejected, and so is any other scheme (`file`, `tcp`, `pipe`, `concat`, …). The rejected URL is not stored, and the player will not open it. Same id and url again is success (`already present`) and does not reopen the decoder. Same id with a different url is rejected. While playing, the next tick opens the new stream. While stopped, it is stored until `start`. |
| `remove` | `id` | Remove that stream and close its decoder. |
| `list` | none | Republish `<prefix>/status`. The response message is `ok`. |
| `layout` | `layout` optional | With no `layout` field, respond with the effective layout and do not change state. Otherwise store the token. `auto` picks a grid from the stream count (`1x1`, `1x2`, `2x2`, `2x3`, `3x3`). Any other non-empty string is stored and echoed back. A token `LayoutEngine` cannot parse stops present until it is replaced. Cells that lose a stream are cleared. |
| `mute` | `id`, `muted` | Set mute for one stream. `muted` is a JSON bool, or the string `"true"` / `"false"`. There is no audio decoder, so mute does not change the picture. |
| `restart` | `id` optional | With a non-empty string `id`, bump that stream's generation so the player closes and reopens its decoder. The url and mute flag stay. An unknown id fails with `stream not found` and does not restart any other stream. An empty id, or an id that is not a string (for example `true`), fails with `id is required` and also restarts nothing. Omitting `id` restarts every stream. A JSON `null` is not stored, so `"id": null` is the same as omitting `id`. |

`stop` does not exit the process. Kill it with SIGINT or SIGTERM when Kodi should come back. Send `start` (or `play`) after `add` before frames are presented. The examples below do that.

### Response

Published to `<prefix>/response`:

```json
{"ok":true,"command":"add","id":"cam1","message":"added"}
```

`id` is `""` when the command has no stream id. `ok` is `false` for unknown commands, malformed JSON, a missing stream, a duplicate id with a different url, or a URL that is not `http://` or `https://`. Messages used today include `playing`, `stopped`, `added`, `already present`, `stream already exists`, `removed`, `stream not found`, `ok`, `muted`, `unmuted`, `restart requested`, `restarted N`, `command is required`, `unknown command`, `id and url are required`, `url must be http:// or https://`, `id is required`, `muted is required`, `layout must be a non-empty string`, `invalid json`, and `payload too large`.

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

`layout` is the effective grid. `layout_setting` is what was stored (`auto` or an explicit token). `streams` is insertion order. `playing` is the player-loop flag: true means the main thread is decoding and presenting, false means decoders are closed.

## Examples

No broker is required for the automated tests:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`layout_engine` checks grid selection. `stream_manager_test` checks add, remove, list, mute, restart generation, and layout state. `mqtt_smoke_test` checks JSON parsing, topic routing, and command results, including that an unknown or non-string restart id does not bump other streams. `stream_player_test` drives `StreamPlayer` with a fake decoder and `SoftwareRenderer`: add, play, present into layout cells, stop, and a one-id restart. None of these open a socket or link libmosquitto. `stream_player_http` is added when the FFmpeg libraries are present. It serves one MPEG-TS file on `127.0.0.1` and checks decode and present through the real `FfmpegDecoder`. It does not open a broker. The `amlogic-multistream` player binary is configured only when FFmpeg, libmosquitto, and libdrm are all present.

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
