# AUDIT9-B: host player on `main` @ e53336a after #54

Independent second look of the host player. Audited commit:
`e53336aebe049fcd1557a185c38872c0a5f99feb` (`main`, squash of
[#54](https://github.com/yyolk/amlogic-multistream/pull/54), AUDIT8 P1).

No production code is changed in this PR. Nothing was fixed. Probes ran
against this tree's sources and the `e53336a` `amlogic-multistream` binary
from `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh`.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path. **should-fix** = real, contained, worth doing before hardware
work. **nit** = polish or a narrow footgun. **drop** = checked, not a finding.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3.0, FFmpeg 6.1.1-3ubuntu5
(libavcodec/libavformat/libswscale 6.1.1), libmosquitto / mosquitto 2.0.18.

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh`: **14/14 pass**,
none skipped. CTest real time **119.26 s**. Required decoder tests
(`ffmpeg_decoder`, `present_smoke`, `stream_player_http`) ran and passed.
`stream_player_stall` 9.34 s, `mqtt_reconnect` 17.57 s, `mqtt_exit_test` 42.22 s,
`launch_coreelec` 38.30 s.

`docs/research/**` was not in scope. On-device DRM/KMS and Amlogic decode
were not exercised.

---

## Verdict

**No must-fix.** #54 still holds: an unpresentable frame is rejected at copy
and on `--url`, and the suite's P1 cases pass. #51 (B2) still holds, including
a `data:` segment whose URL ends in `.ts`.

Two should-fix items are new. **D1:** the 8192 cap is applied only after
decode and `sws_scale`, so a frame the player will refuse is still allocated,
and the MQTT path then retries that decode every second. **J1:** `add` stores
JSON-decoded newlines in ids and URLs, and the add log prints them raw, which
writes a line that is exactly `MQTT status …`.

| ID | Severity | Finding |
| --- | --- | --- |
| **D1** | **should-fix** | Frames larger than 8192 are fully decoded and converted before the cap. A 12000×12000 MJPEG moved RSS by about 425 MB and `readFrame` returned it. `--url` then exits 1. MQTT copy rejects it and retries the open every 1 s. |
| **J1** | **should-fix** | `add` accepts `\n` / `\r` / other non-NUL controls in `id` and `url`. The add log then emits a forged `MQTT status` line. NUL in the URL is still rejected. Retained status JSON stays escaped. |
| E1 | nit | A decode-thread exception sets `done` and leaves `failed` clear, so that id never retries until something else retires the worker. |
| H1 | nit | An invalid `--mqtt-host` or topic prefix is replaced with `localhost` / `amlogic-multistream` and the process still runs. |
| U1 | nit | `scripts/build-appimage.sh` downloads linuxdeploy and appimagetool from GitHub `continuous` with no checksum, then executes them. CI runs that script. The CoreELEC sysroot path already pins sha256. |

---

## D1 — should-fix: the extent cap runs after the allocation `[repro]` `[code]`

`SoftwareRenderer::kMaxExtent` is 8192. `frameAcceptable()` enforces it, and
both copy and `--url` call that helper before treating a frame as presentable
(`src/stream_player.cpp` `copyOwned`, `src/app.cpp` `frameLooksComplete`).
That is the #54 fix, and it holds for the thin frames the P1 tests use.

`FfmpegDecoder::publishYuv420` does not. It allocates the YUV420P destination
and runs `sws_scale` at the decoded width and height, then returns success:

```307:316:src/ffmpeg_decoder.cpp
    if (yuv_->format != AV_PIX_FMT_YUV420P || yuv_->width != src->width || yuv_->height != src->height) {
        av_frame_unref(yuv_);
        yuv_->format = AV_PIX_FMT_YUV420P;
        yuv_->width = src->width;
        yuv_->height = src->height;
        const int buf_rc = av_frame_get_buffer(yuv_, 32);
```

`open()` never lowers `AVCodecContext.max_pixels`. libavcodec 6.1.1's option
default is `INT_MAX` (`libavcodec/options_table.h`), so the decoder accepts a
frame whose area is far above 8192×8192.

A throwaway probe linked `src/ffmpeg_decoder.cpp` against this machine's
FFmpeg 6.1.1 and read one frame from lavfi MJPEG in MP4 (local-file allowance
on, same path `--url` uses):

| File | `readFrame` | RSS delta | Within 8192? |
| --- | --- | --- | --- |
| 8192×8192 | true | +198 MB (RSS 229 MB) | yes |
| 12000×12000 | true | +425 MB (RSS 456 MB) | no |
| 16000×64 | true | +5 MB | no |
| 8200×32 | true | +2 MB | no |

The 8192 square is inside the cap. `./build/amlogic-multistream --url
/tmp/sq8192.mp4 --frames 1` exited 0 and printed `presented 0`.

The 12000 square is not. The same binary printed the conversion line and then
rejected the frame:

```
FfmpegDecoder: video 12000x12000 yuvj420p -> yuv420p
Decoded frame 0 12000x12000 cannot be presented
```

Exit status 1. The allocation had already happened.

On the MQTT path the same frame comes back from `readFrame`, `copyOwned`
returns false, and the worker is a read failure:

```221:224:src/stream_player.cpp
            if (!copied) {
                worker->failed = WorkerFail::Read;
                worker->cv.notify_all();
                return;
            }
```

`tick()` retires that worker and waits `kRetryDelay` (1 s) before opening
again. The P1 suite case (8224×32) now takes this path and stays cheap. A
12000×12000 stream takes the same path and repeats the ~425 MB decode once a
second for as long as the id stays in the table. Nine streams is the table
cap, so nine such URLs stack. A thin frame such as 16000×64 does not, which
is why the existing P1 fixture does not show it.

Worth doing before hardware work because a hardware backend that copies this
"reject after decode" order will reserve the same buffers on the device.
The bound belongs in the decoder: set `max_pixels` to `kMaxExtent * kMaxExtent`
before `avcodec_open2`, and return failure from `publishYuv420` before
`av_frame_get_buffer` / `sws_scale` when either side exceeds `kMaxExtent`.
The 1 s retry can stay; it should not reallocate a frame the presenter will
refuse.

---

## J1 — should-fix: control characters in `id` and `url` forge a status log line `[repro]` `[code]`

`mqttUrlAllowed` rejects a NUL and requires an `http://` or `https://` prefix.
It allows every other byte. The JSON parser turns `\n`, `\r`, `\t`, and
`\u00XX` into those bytes (`src/mqtt_json.cpp` string escapes). `\u0000` in a
URL is still rejected, because that becomes a NUL.

`add` of this payload succeeds:

```json
{"id":"cam","url":"http://127.0.0.1/a\nMQTT status {\"online\":true}"}
```

`StreamManager::addStream` logs the stored strings with no escaping
(`src/stream_manager.cpp`, the "Adding stream" line). The process writes:

```
Adding stream: cam -> http://127.0.0.1/a
MQTT status {"online":true}
```

The second line is exactly the prefix `publishRaw` uses for a retained status
(`src/mqtt_controller.cpp`). An id with an embedded newline does the same
split. A successful `add` then publishes the real status, and that JSON
escapes the newline, so the broker payload and the later stdout line stay
intact. The forged line is still in the log. A reader that accepts any
`MQTT status` line, or a SIGKILL in the gap before the real publish, records
a status the player did not publish.

Reject bytes below 0x20 in both `id` and `url` at `add`, next to the existing
NUL check. The decoder should not see them either.

---

## Nits

### E1 — exception path leaves the id stuck `[code]`

The decode thread catches exceptions from `sessionLoop`, logs them, and sets
`done` without setting `failed` (`src/stream_player.cpp`, the worker lambda).
`sessionLoop` itself sets `WorkerFail` on a normal open/read/copy failure.
`tick()` only retires a live worker when `failed != None` or the generation
or URL changes. After an exception the thread has exited, `frameReady` is
false, and the session still owns the worker, so later ticks do nothing for
that id.

`FfmpegDecoder` does not throw on the paths exercised here (`copyOwned`
already catches `bad_alloc`). The hole is the catch block that was added so a
throw would not `std::terminate` the process. Set `failed` to `Read` in that
catch, under the same mutex as `done`, and the existing retry applies.

### H1 — invalid broker host and topic prefix fall back `[code]`

`loadConfig` replaces a host that contains whitespace with `localhost`, and a
prefix that is empty, starts with `/`, or contains `#`, `+`, or whitespace
with `amlogic-multistream` (`src/mqtt_controller.cpp`). It logs the
replacement and keeps going. A prefix typo therefore publishes the stream
table, URLs included, on the default retained topic. Failing the process
(exit 1, the same as a bad `--viewport`) matches the other flag checks.

### U1 — host AppImage tools are unpinned `[code]`

`scripts/build-appimage.sh` `download()` curls linuxdeploy, the appimage
plugin, and appimagetool from `…/releases/download/continuous/…`, marks them
executable, and caches them forever if the destination is already executable.
The script then runs those binaries. `.github/workflows/ci.yml` runs
`./scripts/build-appimage.sh` on both runners. The script also requires
passwordless `sudo` for the minimal-chroot `--help` check.
`scripts/coreelec-targets.sh` already checks sha256 before unpacking a
sysroot. Pin these three tools the same way (tag or commit, plus sha256).

---

## Rechecked, still closed

| Item | Result |
| --- | --- |
| #54 P1 | Suite cases pass (`stream_player_test` wide-frame retry, `present_smoke`, `stream_player_http`). 8192×8192 presents. 8200×32 and 12000×12000 are refused at present / `--url`. The remaining gap is D1 (cost of the decode), not the old tight present-fail loop. |
| #51 B2 | Suite cases pass. A separate probe: HTTP playlist segment `file:///etc/passwd` fails; `data:…mpegts…` ending in `.ts` fails with `Protocol 'data' not on whitelist 'http,https,tls,tcp,crypto,hls'`. |
| B1 | Left as a nit. #54 does not touch the pre-`mosquitto_loop_start` signal window. No new timing evidence. |
| L1 | Left as a nit. `normalize_abs` still pathname-expands via `set -- $1`. No new must/should impact beyond the glob already reported. |

---

## Dropped

- **CRLF in an `http://` URL.** FFmpeg 6.1.1 percent-encodes CR/LF in the path (`http.c` `bprint_escaped_path`). A URL `http://127.0.0.1:PORT\r\nX-Injected: pwned/crlf` produced this request and nothing else: `GET /crlf HTTP/1.1` with `Host: 127.0.0.1:PORT`. `atoi` on the port stops at the carriage return, so the injected text never reaches the wire.
- **Redirect to `file://`.** An HTTP 302 from a segment to `file:///etc/passwd` did not open that file (`tcp://:80` failed, `open` returned false) with the local-file allowance left off.
- **In-cap 8192×8192 cost.** About 200 MB for one frame, and `--url` presents it. That matches the documented cap. D1 is only the work above the cap.
- **TLS verification.** Still off, and `docs/mqtt.md` already says the allowlists are not a TLS boundary.
- **Full-range MJPEG (`yuvj420p`).** swscale converts it to `yuv420p` and the BT.601 helper is studio-range. Pre-existing picture-quality behavior, not a cap or allowlist break.

---

## How this was checked

1. Read of host C++ (`src/*.cpp`, `src/*.h`), `scripts/launch-coreelec.sh`,
   `scripts/run-tests.sh`, `scripts/build-appimage.sh`,
   `scripts/ctest-require.sh`, and `.github/workflows/ci.yml`, plus the MQTT
   and launcher docs those paths implement. `docs/research/**` skipped.
2. `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `e53336a`.
   14/14 passed, 119.26 s CTest, exit 0.
3. Throwaway probe (not in the tree) compiled `src/ffmpeg_decoder.cpp` with
   the system FFmpeg and printed RSS around `open` / `readFrame`. Fixtures
   were lavfi MJPEG or H.264 in MP4/MPEG-TS. `--url` checks used
   `./build/amlogic-multistream` from the suite build.
4. FFmpeg 6.1.1 `http.c`, `hls.c`, `avio.c`, `url.c`, and `options_table.h`
   for whitelist, redirect, and `max_pixels` behavior, then the probes above
   for `data:`, `file://` segments, and the CRLF request bytes.
5. Throwaway driver of `executeCommand` / `parseFlatJsonObject` for the
   newline URL. The forged line is the `addStream` log, quoted in J1.
