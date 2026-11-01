# MQTT command control

The player process connects to an MQTT broker, subscribes to command topics, and keeps an in-memory stream table. `start` / `stop` only flip a player-loop flag. Nothing in this build opens a decoder, a DRM/KMS device, or an Amlogic hardware plane.

CoreELEC display takeover is still `scripts/launch-coreelec.sh` (stop Kodi, run this process, start Kodi). The process now stays in the foreground until it receives SIGINT or SIGTERM, so Kodi stays stopped for that whole time.

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
| `start`, `play` | none | Set the player-loop flag. MQTT stays connected. |
| `stop` | none | Clear the player-loop flag. MQTT stays connected. |
| `add` | `id`, `url` (strings) | Insert a stream. `url` must start with `http://` or `https://` (lowercase scheme). A NUL byte in the URL is rejected, and so is any other scheme (`file`, `tcp`, `pipe`, `concat`, …). The rejected URL is not stored. Same id and url again is success (`already present`). Same id with a different url is rejected. |
| `remove` | `id` | Remove that stream. |
| `list` | none | Republish `<prefix>/status`. The response message is `ok`. |
| `layout` | `layout` optional | With no `layout` field, respond with the effective layout and do not change state. Otherwise store the token. `auto` picks a grid from the stream count (`1x1`, `1x2`, `2x2`, `2x3`, `3x3`). Any other non-empty string is stored and echoed back. |
| `mute` | `id`, `muted` | Set mute for one stream. `muted` is a JSON bool, or the string `"true"` / `"false"`. |
| `restart` | `id` optional | With `id`, ask that one stream to reconnect and keep its url and mute flag. Without `id`, do the same for every stream. No decoder is opened; the entry is the guidance a later player loop should clear and re-add. |

`stop` does not exit the process. Kill it with SIGINT or SIGTERM when Kodi should come back.

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

`layout` is the effective grid. `layout_setting` is what was stored (`auto` or an explicit token). `streams` is insertion order. `playing` is the player-loop flag, not a decoder state.

## Examples

No broker is required for the automated tests:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`layout_engine` checks grid selection. `stream_manager_test` checks add, remove, list, mute, restart, and layout state. `mqtt_smoke_test` checks JSON parsing, topic routing, and command results. None of these open a socket or link libmosquitto. The `amlogic-multistream` player binary is configured only when FFmpeg, libmosquitto, and libdrm are all present.

With a broker and the app running:

```bash
mosquitto_sub -h localhost -t 'amlogic-multistream/#' -v

mosquitto_pub -h localhost -t amlogic-multistream/cmd/add \
  -m '{"id":"cam1","url":"http://example/live.m3u8"}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/mute \
  -m '{"id":"cam1","muted":true}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/layout \
  -m '{"layout":"2x2"}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/list -m '{}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/restart \
  -m '{"id":"cam1"}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/start -m '{}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/stop -m '{}'
mosquitto_pub -h localhost -t amlogic-multistream/cmd/remove \
  -m '{"id":"cam1"}'

# Same add, with the command name in the payload:
mosquitto_pub -h localhost -t amlogic-multistream/cmd \
  -m '{"action":"add","id":"cam2","url":"http://example/other.m3u8"}'
```

## Not in this build

- Amlogic `multi_vdec` / hardware decode
- DRM/KMS modeset or a plane commit (the `DrmRenderer` hook **requires GS-King X / CoreELEC** and is not started on this path)
- HLS demux, audio, or a real mute that reaches a decoder
- TLS, HTTP control, and Home Assistant discovery payloads

`StreamManager` is the source of truth a later player loop can read. `--url` software decode and `SoftwareRenderer` are a separate path and do not start this controller.
